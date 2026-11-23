// Replays a PHD2 guide log through the wx-free guide algorithm kernels (#142 Tier 4). See
// guide_log_replay.h.

#include "guide_log_replay.h"

#include "guide_move_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

using namespace guide_move_plan;
using guide_log::Event;
using guide_log::Session;
using guide_sim::Algo;
using guide_sim::AxisAlgorithm;

namespace guide_log_replay
{

namespace
{
    constexpr unsigned GUIDE_STEP = ALGO_RESULT | USE_BLC | GRAPH; // MOVEOPTS_GUIDE_STEP
    constexpr unsigned RECOVERY_MOVE = USE_BLC; // MOVEOPTS_RECOVERY_MOVE

    // Half the last printed digit of the log's %.3f offsets
    constexpr double LOG_ROUNDING = 0.0005;

    DecMode DecModeFromLog(const std::string& s)
    {
        if (s == "North")
            return DecMode::North;
        if (s == "South")
            return DecMode::South;
        if (s == "Off" || s == "None")
            return DecMode::None;
        return DecMode::Auto;
    }

    char DirChar(Dir d)
    {
        switch (d)
        {
        case Dir::North:
            return 'N';
        case Dir::South:
            return 'S';
        case Dir::East:
            return 'E';
        case Dir::West:
            return 'W';
        case Dir::None:
            break;
        }
        return 0;
    }

    // The signed px a pulse moves the star's offset back by: West and South pulses reduce a positive
    // RA and Dec offset (PlanOffset sends West for x > 0 and South for y > 0)
    double Correction(char dir, int ms, double ratePxPerMs)
    {
        double px = ms * ratePxPerMs;
        return dir == 'W' || dir == 'S' ? px : dir == 'E' || dir == 'N' ? -px : 0.;
    }

    struct Axis
    {
        bool ra = true;
        double ratePxPerMs = 0.; // the header's rate
        AxisPolicy policy;
        LimitTracker raLimit;
        LimitTracker decLimit;

        // The pulse for a distance on this axis: PlanOffset, then PlanAxis for a guide step or, for
        // the dither's fast recenter, a recovery move
        AxisPlan Plan(double distance, Dir *dir, bool recovery)
        {
            OffsetPlan plan = ra ? PlanOffset(distance, 0., ratePxPerMs, 1.) : PlanOffset(0., distance, 1., ratePxPerMs);
            *dir = ra ? plan.raDir : plan.decDir;
            return PlanAxis(*dir, ra ? plan.raMs : plan.decMs, recovery ? RECOVERY_MOVE : GUIDE_STEP, policy, raLimit,
                            decLimit);
        }
    };

    Axis MakeAxis(const Session& s, bool ra)
    {
        Axis a;
        a.ra = ra;
        a.ratePxPerMs = (ra ? s.xRate : s.yRate) / 1000.;
        a.policy.guidingEnabled = s.guidingEnabled;
        a.policy.decMode = DecModeFromLog(s.decGuideMode);
        a.policy.maxRaMs = s.maxRaMs;
        a.policy.maxDecMs = s.maxDecMs;
        return a;
    }

    std::string Describe(const guide_log::Algorithm& a)
    {
        char buf[160];
        if (a.name == "Hysteresis")
            std::snprintf(buf, sizeof buf, "Hysteresis (hyst %.2f, aggr %.2f, minMove %.3f)", a.hysteresis, a.aggression,
                          a.minMove);
        else if (a.name == "Resist Switch")
            std::snprintf(buf, sizeof buf, "Resist Switch (aggr %.0f%%, minMove %.3f%s)", a.aggression * 100., a.minMove,
                          a.fastSwitch ? "" : ", no fast switch");
        else if (a.name == "Lowpass")
            std::snprintf(buf, sizeof buf, "Lowpass (slope %.1f, minMove %.3f)", a.slopeWeight, a.minMove);
        else if (a.name == "Lowpass2")
            std::snprintf(buf, sizeof buf, "Lowpass2 (aggr %.0f, minMove %.3f)", a.aggressiveness, a.minMove);
        else
            std::snprintf(buf, sizeof buf, "%s", a.name.c_str());
        return buf;
    }

    // A header's algorithm with the production constructor's defaults
    guide_log::Algorithm Defaults(Algo kind)
    {
        AxisAlgorithm d(kind);
        guide_log::Algorithm a;
        switch (kind)
        {
        case Algo::Hysteresis:
            a.name = "Hysteresis";
            a.minMove = d.hysteresis.minMove;
            a.hysteresis = d.hysteresis.hysteresis;
            a.aggression = d.hysteresis.aggression;
            break;
        case Algo::ResistSwitch:
            a.name = "Resist Switch";
            a.minMove = d.resistSwitch.minMove;
            a.aggression = d.resistSwitch.aggression;
            a.fastSwitch = d.resistSwitch.fastSwitchEnabled;
            break;
        case Algo::Lowpass:
            a.name = "Lowpass";
            a.minMove = d.lowpassMinMove;
            a.slopeWeight = d.lowpassSlopeWeight;
            break;
        case Algo::Lowpass2:
            a.name = "Lowpass2";
            a.minMove = d.lowpass2MinMove;
            a.aggressiveness = d.lowpass2Aggressiveness;
            break;
        case Algo::None:
            a.name = "None";
            break;
        }
        return a;
    }

    // Population standard deviation
    class Stat
    {
    public:
        void Add(double x)
        {
            m_n++;
            m_sum += x;
            m_sum2 += x * x;
        }
        int Count() const { return m_n; }
        double Sigma() const
        {
            if (m_n == 0)
                return 0.;
            double mean = m_sum / m_n;
            return std::sqrt(std::max(0., m_sum2 / m_n - mean * mean));
        }

    private:
        int m_n = 0;
        double m_sum = 0.;
        double m_sum2 = 0.;
    };

    // What production did, in the order it did it. The log does not always say: a dither resets the
    // algorithms (GuidingDithered) on the main thread while the worker may already be computing the
    // frame in flight, which the log then shows after the DITHER line, computed before the reset. The
    // fast recenter that follows is a recovery move (MOVEOPTS_RECOVERY_MOVE: no algorithm, no max
    // duration), logged as a frame row whose offset is the size of the dither on each axis and whose
    // guide distances equal it. (The sign is the dither turned around on RA; on Dec it depends on the
    // side of pier, so only the size is matched.) Guiding enabled or disabled is the same kind of
    // main-thread change: the frame in flight may have been computed before it or after it.
    struct Step
    {
        enum Kind
        {
            Row, // a frame row
            DitherReset, // the algorithms reset and the lock moved: a new stretch for the closed loop
            Enable, // guiding enabled: the algorithms reset
            Disable,
            SettleBegin,
            SettleEnd,
        };
        Kind kind = Row;
        const guide_log::Frame *frame = nullptr;
        bool recenter = false; // Row: the dither's fast recenter
        bool afterSwitch = false; // Row: the first after guiding was enabled or disabled, in flight then
        int line = 0;
    };

    // Within this of the dither's size, px: the dither and the offsets are both printed %.3f
    constexpr double RECENTER_MATCH = 0.0015;

    bool IsRecenter(const guide_log::Frame& f, double ditherRa, double ditherDec)
    {
        return std::fabs(std::fabs(f.raRaw) - std::fabs(ditherRa)) <= RECENTER_MATCH &&
            std::fabs(std::fabs(f.decRaw) - std::fabs(ditherDec)) <= RECENTER_MATCH && f.raGuide == f.raRaw &&
            f.decGuide == f.decRaw;
    }

    std::vector<Step> Steps(const Session& s)
    {
        std::vector<Step> steps;
        bool pending = false; // a dither whose reset has not been placed yet
        double ditherRa = 0.;
        double ditherDec = 0.;
        int framesSince = 0;
        bool switched = false; // guiding was enabled or disabled since the last frame
        for (const Event& ev : s.events)
        {
            Step st;
            st.line = ev.line;
            switch (ev.kind)
            {
            case Event::Dither:
                pending = true;
                ditherRa = ev.ditherRa;
                ditherDec = ev.ditherDec;
                framesSince = 0;
                continue;
            case Event::GuidingEnabled:
                st.kind = Step::Enable;
                switched = true;
                break;
            case Event::GuidingDisabled:
                st.kind = Step::Disable;
                switched = true;
                break;
            case Event::SettleBegin:
                st.kind = Step::SettleBegin;
                break;
            case Event::SettleEnd:
                st.kind = Step::SettleEnd;
                break;
            case Event::FrameRow:
                if (ev.frame.dropped)
                    continue;
                st.kind = Step::Row;
                st.frame = &ev.frame;
                st.afterSwitch = switched;
                switched = false;
                if (pending)
                {
                    framesSince++;
                    bool recenter = IsRecenter(ev.frame, ditherRa, ditherDec);
                    // with no recenter row, the second frame after the dither is the first computed after it
                    if (recenter || framesSince == 2)
                    {
                        Step reset;
                        reset.kind = Step::DitherReset;
                        reset.line = ev.line;
                        steps.push_back(reset);
                        st.recenter = recenter;
                        pending = false;
                    }
                }
                break;
            }
            steps.push_back(st);
        }
        return steps;
    }

    enum class Match
    {
        Exact, // within the tolerance of the logged guide distance
        Ambiguous, // only with the input moved within its rounding: a threshold inside it
        Mismatch,
    };

    // One step of algo on input, against the logged guide distance. The log rounds the input to
    // 0.001 px: if the input either side of that rounding gives the logged answer (a minMove or
    // switch threshold inside the rounding), the step is ambiguous and algo follows the branch the
    // log shows. *out is what algo returned.
    Match StepAgainstLog(AxisAlgorithm& algo, double input, double loggedOut, double tolerance, double *out)
    {
        AxisAlgorithm before = algo;
        *out = algo.Result(input);
        if (std::fabs(*out - loggedOut) <= tolerance)
            return Match::Exact;
        for (double nudge : { -LOG_ROUNDING, LOG_ROUNDING })
        {
            AxisAlgorithm trial = before;
            double o = trial.Result(input + nudge);
            if (std::fabs(o - loggedOut) <= tolerance + LOG_ROUNDING)
            {
                algo = trial;
                *out = o;
                return Match::Ambiguous;
            }
        }
        return Match::Mismatch;
    }

    // Whether the logged pulse for a guide distance is the plan's, within the header rate's rounding
    bool PulseMatches(const AxisPlan& plan, Dir dir, int loggedMs, char loggedDir, double rateSlack)
    {
        int slack = 1 + (int) std::ceil(rateSlack * plan.ms);
        bool sameDir = plan.ms == 0 ? loggedMs == 0 : DirChar(dir) == loggedDir;
        return sameDir && std::abs(plan.ms - loggedMs) <= slack;
    }

    // The frame in flight when guiding was enabled or disabled went out under the policy its logged
    // pulse shows: the axis's policy for the frame (and the plan for the logged distance under it)
    AxisPolicy FramePolicy(Axis& axis, const Step& st, double loggedOut, int loggedMs, char loggedDir, double rateSlack)
    {
        if (!st.afterSwitch)
            return axis.policy;
        Dir dir;
        if (PulseMatches(axis.Plan(loggedOut, &dir, st.recenter), dir, loggedMs, loggedDir, rateSlack))
            return axis.policy;
        Axis other = axis;
        other.policy.guidingEnabled = !other.policy.guidingEnabled;
        if (PulseMatches(other.Plan(loggedOut, &dir, st.recenter), dir, loggedMs, loggedDir, rateSlack))
            return other.policy;
        return axis.policy;
    }

    // The header prints the rate to 0.001 px/s: a pulse is |d| / rate ms, so the rate's relative
    // rounding moves a long pulse by a ms or two
    double RateSlack(const Axis& axis, const Options& opt)
    {
        return opt.rateRounding / (axis.ratePxPerMs * 1000.);
    }

    // Open-loop reproduction of one axis
    void Reproduce(const Session& s, bool ra, const Options& opt, AxisReproduction *rep)
    {
        const guide_log::Algorithm& logged = ra ? s.x : s.y;
        rep->algorithm = Describe(logged);
        std::unique_ptr<AxisAlgorithm> algo = MakeAlgorithm(logged);
        rep->supported = algo != nullptr;
        if (!algo)
            return;
        Axis axis = MakeAxis(s, ra);
        double rateSlack = RateSlack(axis, opt);

        for (const Step& st : Steps(s))
        {
            switch (st.kind)
            {
            case Step::DitherReset:
                algo->Reset();
                continue;
            case Step::Enable:
                algo->Reset();
                axis.policy.guidingEnabled = true;
                continue;
            case Step::Disable:
                axis.policy.guidingEnabled = false;
                continue;
            case Step::SettleBegin:
            case Step::SettleEnd:
                continue;
            case Step::Row:
                break;
            }
            const guide_log::Frame& f = *st.frame;
            double input = ra ? f.raRaw : f.decRaw;
            double loggedOut = ra ? f.raGuide : f.decGuide;
            int loggedMs = ra ? f.raMs : f.decMs;
            char loggedDir = ra ? f.raDir : f.decDir;

            double out = loggedOut;
            if (!st.recenter)
            {
                rep->steps++;
                Match m = StepAgainstLog(*algo, input, loggedOut, opt.distanceTolerance, &out);
                double err = std::fabs(out - loggedOut);
                if (m == Match::Exact)
                {
                    rep->distanceMatches++;
                    rep->maxDistanceError = std::max(rep->maxDistanceError, err);
                }
                else if (m == Match::Ambiguous)
                    rep->thresholdAmbiguous++;
                else
                {
                    rep->maxDistanceError = std::max(rep->maxDistanceError, err);
                    if ((int) rep->mismatches.size() < opt.maxMismatchesKept)
                        rep->mismatches.push_back({ st.line, f.frame, ra ? 'R' : 'D', input, loggedOut, out, loggedMs, -1 });
                }
            }

            // the pulse, from the logged guide distance so a distance mismatch is not counted twice;
            // with Dec backlash compensation on, the Dec pulse has the compensation added, which the
            // log does not show apart
            if (!ra && s.backlashComp)
                continue;
            Axis frameAxis = axis;
            frameAxis.policy = FramePolicy(axis, st, loggedOut, loggedMs, loggedDir, rateSlack);
            Dir dir;
            AxisPlan plan = frameAxis.Plan(loggedOut, &dir, st.recenter);
            if (PulseMatches(plan, dir, loggedMs, loggedDir, rateSlack))
            {
                rep->durationsChecked++;
                rep->durationMatches++;
            }
            else if (loggedMs == 0 && plan.ms > 0)
                rep->unsent++; // a pulse the mount did not take: it failed, or guiding stopped as it was sent
            else
            {
                rep->durationsChecked++;
                if ((int) rep->mismatches.size() < opt.maxMismatchesKept)
                    rep->mismatches.push_back({ st.line, f.frame, ra ? 'R' : 'D', input, loggedOut, out, loggedMs, plan.ms });
            }
        }
    }

    // Closed loop on one axis: the uncorrected motion rebuilt from the log, guided by algo. Returns the
    // residual's sigma over the counted frames. With followLog (algo is the logged algorithm), a step
    // still on the logged track takes the logged branch at a threshold the log's rounding hides, as
    // the reproduction does; *maxSelfError gets the largest |residual - logged offset|, *uncorrected
    // the motion's own sigma and *loggedSigma the logged offsets' over the same frames.
    double ClosedLoop(const Session& s, bool ra, AxisAlgorithm algo, const Options& opt, bool followLog, double *maxSelfError,
                      double *uncorrected, double *loggedSigma, int *frames)
    {
        Axis axis = MakeAxis(s, ra);
        double rateSlack = RateSlack(axis, opt);
        double logCorrection = 0.; // px the logged pulses moved the star back, this stretch
        double correction = 0.; // px the replayed pulses did
        bool settling = false;
        Stat residual;
        Stat motion;
        Stat loggedStat;
        double maxErr = 0.;
        for (const Step& st : Steps(s))
        {
            switch (st.kind)
            {
            case Step::DitherReset:
                // the lock moved: a new stretch, from the star where the log has it
                algo.Reset();
                logCorrection = 0.;
                correction = 0.;
                continue;
            case Step::Enable:
                algo.Reset();
                axis.policy.guidingEnabled = true;
                continue;
            case Step::Disable:
                axis.policy.guidingEnabled = false;
                continue;
            case Step::SettleBegin:
                settling = true;
                continue;
            case Step::SettleEnd:
                settling = false;
                continue;
            case Step::Row:
                break;
            }
            const guide_log::Frame& f = *st.frame;
            double raw = ra ? f.raRaw : f.decRaw;
            double loggedOut = ra ? f.raGuide : f.decGuide;
            int loggedMs = ra ? f.raMs : f.decMs;
            char loggedDir = ra ? f.raDir : f.decDir;
            double sky = raw + logCorrection; // where the star would be without this stretch's pulses
            double r = sky - correction;
            maxErr = std::max(maxErr, std::fabs(r - raw));
            Axis frameAxis = axis;
            frameAxis.policy = FramePolicy(axis, st, loggedOut, loggedMs, loggedDir, rateSlack);
            if (!settling && frameAxis.policy.guidingEnabled)
            {
                residual.Add(r);
                motion.Add(sky);
                loggedStat.Add(raw);
            }
            double logged = Correction(loggedDir, loggedMs, axis.ratePxPerMs);
            if (st.recenter)
                correction += logged; // the same recovery move whatever the algorithm: r is raw here
            else
            {
                double out;
                if (followLog && std::fabs(r - raw) <= 1e-9)
                    StepAgainstLog(algo, r, loggedOut, opt.distanceTolerance, &out);
                else
                    out = algo.Result(r);
                Dir dir;
                AxisPlan plan = frameAxis.Plan(out, &dir, false);
                // a pulse the log planned but the mount did not take (it failed, or guiding stopped as
                // it went out) is not taken from the replayed algorithm either
                Dir unused;
                bool unsent = loggedMs == 0 && frameAxis.Plan(loggedOut, &unused, false).ms > 0;
                if (!unsent)
                {
                    // on the logged track, the logged pulse: the header's rate is rounded, which can move
                    // a planned pulse by a ms the log does not have
                    if (followLog && std::fabs(r - raw) <= 1e-9 && PulseMatches(plan, dir, loggedMs, loggedDir, rateSlack))
                        correction += logged;
                    else
                        correction += Correction(DirChar(dir), plan.ms, axis.ratePxPerMs);
                }
            }
            logCorrection += logged;
            axis.raLimit = frameAxis.raLimit;
            axis.decLimit = frameAxis.decLimit;
        }
        if (maxSelfError)
            *maxSelfError = std::max(*maxSelfError, maxErr);
        if (uncorrected)
            *uncorrected = motion.Sigma();
        if (loggedSigma)
            *loggedSigma = loggedStat.Sigma();
        if (frames)
            *frames = residual.Count();
        return residual.Sigma();
    }

    void Predict(const Session& s, bool ra, SessionReport *rep)
    {
        const guide_log::Algorithm& logged = ra ? s.x : s.y;
        std::vector<Prediction>& out = ra ? rep->raPredictions : rep->decPredictions;
        std::vector<std::pair<guide_log::Algorithm, const char *>> candidates;
        if (MakeAlgorithm(logged))
            candidates.emplace_back(logged, "as logged");
        for (Algo k : { Algo::Hysteresis, Algo::ResistSwitch, Algo::Lowpass, Algo::Lowpass2 })
            candidates.emplace_back(Defaults(k), "defaults");
        for (const auto& [c, label] : candidates)
        {
            Prediction p;
            p.algorithm = Describe(c) + ", " + label;
            p.rms = ClosedLoop(s, ra, *MakeAlgorithm(c), Options(), false, nullptr, nullptr, nullptr, nullptr);
            out.push_back(p);
        }
    }

} // namespace

std::unique_ptr<AxisAlgorithm> MakeAlgorithm(const guide_log::Algorithm& logged)
{
    std::unique_ptr<AxisAlgorithm> a;
    if (logged.name == "Hysteresis")
    {
        a = std::make_unique<AxisAlgorithm>(Algo::Hysteresis);
        a->hysteresis.minMove = logged.minMove;
        a->hysteresis.hysteresis = logged.hysteresis;
        a->hysteresis.aggression = logged.aggression;
    }
    else if (logged.name == "Resist Switch")
    {
        a = std::make_unique<AxisAlgorithm>(Algo::ResistSwitch);
        a->resistSwitch.minMove = logged.minMove;
        a->resistSwitch.aggression = logged.aggression;
        a->resistSwitch.fastSwitchEnabled = logged.fastSwitch;
    }
    else if (logged.name == "Lowpass")
    {
        a = std::make_unique<AxisAlgorithm>(Algo::Lowpass);
        a->lowpassMinMove = logged.minMove;
        a->lowpassSlopeWeight = logged.slopeWeight;
    }
    else if (logged.name == "Lowpass2")
    {
        a = std::make_unique<AxisAlgorithm>(Algo::Lowpass2);
        a->lowpass2MinMove = logged.minMove;
        a->lowpass2Aggressiveness = logged.aggressiveness;
    }
    return a;
}

SessionReport ReplaySession(const Session& s, const Options& opt)
{
    SessionReport rep;
    rep.line = s.line;
    rep.begins = s.begins;
    rep.pixelScale = s.pixelScale;
    rep.frames = s.FrameCount();
    if (!s.mount)
        return rep;

    Reproduce(s, true, opt, &rep.ra);
    Reproduce(s, false, opt, &rep.dec);

    // the logged offsets over the frames the prediction counts, and the logged algorithms closed loop
    // on the motion rebuilt from them, which gives them back
    // (an algorithm the kernels do not have still gets the logged offsets, and no self-replay)
    std::unique_ptr<AxisAlgorithm> raAlgo = MakeAlgorithm(s.x);
    std::unique_ptr<AxisAlgorithm> decAlgo = MakeAlgorithm(s.y);
    AxisAlgorithm none(Algo::None);
    double raSelf = ClosedLoop(s, true, raAlgo ? *raAlgo : none, opt, true, raAlgo ? &rep.selfReplayMaxError : nullptr,
                               &rep.uncorrected.ra, &rep.logged.ra, &rep.logged.frames);
    double decSelf = ClosedLoop(s, false, decAlgo ? *decAlgo : none, opt, true, decAlgo ? &rep.selfReplayMaxError : nullptr,
                                &rep.uncorrected.dec, &rep.logged.dec, nullptr);
    rep.selfReplay.ra = raAlgo ? raSelf : 0.;
    rep.selfReplay.dec = decAlgo ? decSelf : 0.;
    rep.selfReplay.frames = rep.logged.frames;
    rep.uncorrected.frames = rep.logged.frames;

    Predict(s, true, &rep);
    Predict(s, false, &rep);
    return rep;
}

std::string Markdown(const std::string& title, const std::vector<SessionReport>& reports)
{
    std::ostringstream md;
    char buf[512];
    md << "### Guide log replay: " << title << "\n\n";
    md << "| Session (line) | Frames | RA algorithm | RA distances | RA pulses | Dec algorithm | Dec distances | Dec pulses | "
          "Logged RMS RA / Dec | Self-replay RMS RA / Dec (max offset error) |\n";
    md << "|---|---|---|---|---|---|---|---|---|---|\n";
    for (const SessionReport& r : reports)
    {
        if (r.frames == 0)
            continue;
        auto repro = [](const AxisReproduction& a, bool pulses)
        {
            char b[64];
            if (!a.supported)
                return std::string("n/a");
            if (pulses && a.unsent)
                std::snprintf(b, sizeof b, "%d/%d (+%d not sent)", a.durationMatches, a.durationsChecked, a.unsent);
            else if (pulses)
                std::snprintf(b, sizeof b, "%d/%d", a.durationMatches, a.durationsChecked);
            else if (a.thresholdAmbiguous)
                std::snprintf(b, sizeof b, "%d/%d (%d at a threshold)", a.distanceMatches + a.thresholdAmbiguous, a.steps,
                              a.thresholdAmbiguous);
            else
                std::snprintf(b, sizeof b, "%d/%d", a.distanceMatches, a.steps);
            return std::string(b);
        };
        double k = r.pixelScale;
        std::snprintf(
            buf, sizeof buf, "| %s (%d) | %d | %s | %s | %s | %s | %s | %s | %.2f\" / %.2f\" | %.2f\" / %.2f\" (%.3f px) |\n",
            r.begins.c_str(), r.line, r.frames, r.ra.algorithm.c_str(), repro(r.ra, false).c_str(), repro(r.ra, true).c_str(),
            r.dec.algorithm.c_str(), repro(r.dec, false).c_str(), repro(r.dec, true).c_str(), r.logged.ra * k, r.logged.dec * k,
            r.selfReplay.ra * k, r.selfReplay.dec * k, r.selfReplayMaxError);
        md << buf;
    }
    md << "\nPredicted RMS (sigma, arcsec) of each algorithm on the night's reconstructed motion, outside settling; "
          "the mount is taken to move exactly rate x duration (no backlash):\n\n";
    md << "| Session (line) | Uncorrected RA / Dec | RA predictions | Dec predictions |\n|---|---|---|---|\n";
    for (const SessionReport& r : reports)
    {
        if (r.frames == 0)
            continue;
        double k = r.pixelScale;
        auto list = [k](const std::vector<Prediction>& ps)
        {
            std::string s;
            for (const Prediction& p : ps)
            {
                char b[200];
                std::snprintf(b, sizeof b, "%s%s: %.2f\"", s.empty() ? "" : "<br>", p.algorithm.c_str(), p.rms * k);
                s += b;
            }
            return s;
        };
        std::snprintf(buf, sizeof buf, "| %s (%d) | %.2f\" / %.2f\" | ", r.begins.c_str(), r.line, r.uncorrected.ra * k,
                      r.uncorrected.dec * k);
        md << buf << list(r.raPredictions) << " | " << list(r.decPredictions) << " |\n";
    }
    return md.str();
}

} // namespace guide_log_replay
