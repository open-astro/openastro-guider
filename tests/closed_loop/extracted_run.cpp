#include "extracted_run.h"

#include <cstdio>
#include <optional>

namespace closed_loop
{

namespace
{
    namespace cr = calibration_run;

    // Scope's ScopeCalibrationHost as a Probe: the reads go to the SimMount, and each is recorded
    class SimProbe : public cr::Probe
    {
        SimMount& m_sim;
        std::function<void(const char *)> m_record;

    public:
        SimProbe(SimMount& sim, std::function<void(const char *)> record) : m_sim(sim), m_record(std::move(record)) { }
        bool Coordinates(double *raHours, double *decDeg) override
        {
            m_record("Coordinates");
            return m_sim.Coordinates(raHours, decDeg);
        }
        bool GuideRates(double *raDegPerSec, double *decDegPerSec) override
        {
            m_record("GuideRates");
            return m_sim.GuideRates(raDegPerSec, decDegPerSec);
        }
        bool CanPulseGuide() override
        {
            m_record("CanPulseGuide");
            return m_sim.CanPulseGuide();
        }
    };

    class Replay : public cr::Sink
    {
        std::function<void(const cr::Effect&)> m_fn;

    public:
        explicit Replay(std::function<void(const cr::Effect&)> fn) : m_fn(std::move(fn)) { }
        void Emit(const cr::Effect& e) override { m_fn(e); }
    };

    template<class... A>
    std::string Format(const char *fmt, A... args)
    {
        int n = std::snprintf(nullptr, 0, fmt, args...);
        if (n <= 0)
            return std::string();
        std::string buf((size_t) n + 1, '\0');
        std::snprintf(buf.data(), buf.size(), fmt, args...);
        buf.resize((size_t) n);
        return buf;
    }

    const char *LegName(cr::Leg leg)
    {
        static const char *const names[] = { "West", "East", "Backlash", "North", "South", "NudgeSouth" };
        return names[static_cast<int>(leg)];
    }

    // scope.cpp CalibrationStepMessage, untranslated
    std::string StepMessage(const cr::Effect& e)
    {
        switch (e.message)
        {
        case cr::Message::WestStep:
            return Format("West step %3d, dist=%4.1f", e.number, e.rec.dist);
        case cr::Message::EastStep:
            return Format("East step %3d, dist=%4.1f", e.number, e.rec.dist);
        case cr::Message::BacklashFirst:
            return "Clearing backlash step 1";
        case cr::Message::BacklashStep:
            return Format("Clearing backlash step %3d", e.number);
        case cr::Message::NorthStep:
            return Format("North step %3d, dist=%4.1f", e.number, e.rec.dist);
        case cr::Message::SouthStep:
            return Format("South step %3d, dist=%4.1f", e.number, e.rec.dist);
        case cr::Message::NudgeSouth:
            return Format("Nudge South %3d", e.number);
        }
        return std::string();
    }

    const char *SouthAdviceText(cr::SouthAdvice a)
    {
        switch (a)
        {
        case cr::SouthAdvice::NoSouthCannotPulseGuide:
            return "Advisory: Calibration successful but little or no south movement was measured, so "
                   "guiding will probably be impaired.\n "
                   "This is usually caused by a faulty guide cable or very large Dec backlash. \n"
                   "Check the guide cable and read the online Help for how to identify these types of "
                   "problems (Manual Guide, Declination backlash).";
        case cr::SouthAdvice::LittleSouthCannotPulseGuide:
            return "Advisory: Calibration successful but little south movement was measured, so guiding will probably "
                   "be impaired. \n"
                   "This is usually caused by very large Dec backlash or other problems with the mount mechanics. \n"
                   "Read the online Help for how to identify these types of problems (Manual Guide, Declination "
                   "backlash).";
        case cr::SouthAdvice::LittleSouth:
            return "Advisory: Calibration successful but little south movement was measured, so guiding may be "
                   "impaired.\n "
                   "This is usually caused by very large Dec backlash or other problems with the mount mechanics. \n"
                   "Read the online help for how to deal with this type of problem (Declination backlash).";
        }
        return "";
    }

    cr::Settings ToRunSettings(const Settings& s)
    {
        cr::Settings r;
        r.stepMs = s.stepMs;
        r.distancePx = s.distancePx;
        r.maxRaMs = s.maxRaMs;
        r.maxDecMs = s.maxDecMs;
        r.decMode = static_cast<cr::DecMode>(s.decMode);
        r.assumeOrthogonal = s.assumeOrthogonal;
        r.fastRecenter = s.fastRecenter;
        r.maxMovePx = s.maxMovePx;
        return r;
    }
} // namespace

Event& ExtractedRun::Add(Event::Kind kind, const std::string& text)
{
    Event e;
    e.kind = kind;
    e.frame = m_frame;
    e.text = text;
    m_events.push_back(e);
    return m_events.back();
}

void ExtractedRun::Report(const cr::Effect& e)
{
    switch (e.kind)
    {
    case cr::Effect::LogStep:
    {
        Event& ev = Add(Event::LogStep, LegName(e.rec.leg));
        ev.step = e.rec.step;
        ev.dx = e.rec.dx;
        ev.dy = e.rec.dy;
        ev.x = e.rec.pos.x;
        ev.y = e.rec.pos.y;
        ev.dist = e.rec.dist;
        break;
    }
    case cr::Effect::Status:
    {
        // CalibrationStatus: StatusMsg, then NotifyCalibrationStep with the message
        m_lastStatus = StepMessage(e);
        Add(Event::Status, m_lastStatus);
        Event& ev = Add(Event::NotifyStep, m_lastStatus);
        ev.direction = LegName(e.rec.leg);
        ev.step = e.rec.step;
        ev.dx = e.rec.dx;
        ev.dy = e.rec.dy;
        ev.x = e.rec.pos.x;
        ev.y = e.rec.pos.y;
        ev.dist = e.rec.dist;
        break;
    }
    case cr::Effect::Move:
    {
        Event& ev = Add(Event::Move);
        ev.dir = static_cast<Dir>(e.dir);
        ev.ms = e.ms;
        ev.moveOptions = 0; // MOVEOPTS_CALIBRATION_MOVE
        break;
    }
    case cr::Effect::DirectComplete:
    {
        Event& ev = Add(Event::DirectComplete, e.ra ? "West" : "North");
        ev.angle = e.angle;
        ev.rate = e.rate;
        ev.parity = static_cast<int>(e.parity);
        break;
    }
    case cr::Effect::EastAdvisory:
    {
        const std::string msg = "Advisory: Little or no east movement was measured, so guiding will probably be impaired. "
                                "Check the guide cable and use the Manual Guide tool to confirm basic operation of the mount.";
        Add(Event::Alert, msg);
        Add(Event::Debug, "Calibration alert: " + msg + "\n");
        break;
    }
    case cr::Effect::SouthAdvisory:
        Add(Event::Debug, std::string("Omitted calibration alert: ") + SouthAdviceText(e.southAdvice) + "\n");
        break;
    case cr::Effect::BacklashDebug:
        Add(Event::Debug,
            Format("Backlash: %s, Last Delta = %0.2f px, CumDistance = %0.2f px\n", m_lastStatus.c_str(), e.a, e.b));
        break;
    case cr::Effect::Debug:
        Add(Event::Debug, e.text);
        break;
    }
}

void ExtractedRun::ClearCalibration()
{
    m_calibrated = false; // Mount::ClearCalibration
    m_run.Clear();
}

bool ExtractedRun::IsCalibrated() const
{
    if (!m_calibrated)
        return false;
    if (m_decMode == DecMode::None)
        return true;
    return m_calibration.yRate != RATE_UNCALIBRATED;
}

void ExtractedRun::Begin(const Pt& lock, const Settings& settings)
{
    m_frame = -1;
    m_decMode = settings.decMode;
    ClearCalibration();

    // Scope reads the mount's Dec once and sizes the RA step for it (#141)
    Add(Event::Probe, "Declination");
    std::optional<double> dec = m_readDeclination ? m_sim.ReportedDecRad() : std::nullopt;
    std::string decStr = dec ? Format("%.1f", Degrees(*dec)) : std::string("Unknown");
    Add(Event::Debug,
        Format("RA calibration step %d ms for Dec %s (Dec step %d ms)\n", cr::RaCalibrationStepMs(settings.stepMs, dec),
               decStr.c_str(), settings.stepMs));
    m_run.Begin(cr::Pt { lock.x, lock.y, lock.valid }, dec);
}

void ExtractedRun::Clear()
{
    m_frame = -1;
    ClearCalibration();
}

FrameReport ExtractedRun::OnFrame(const Pt& star, const Settings& settings)
{
    m_frame = m_frames++;
    m_decMode = settings.decMode;

    SimProbe probe(m_sim, [this](const char *what) { Add(Event::Probe, what); });
    Replay sink([this](const cr::Effect& e) { Report(e); });
    cr::FrameResult r = m_run.OnFrame(cr::Pt { star.x, star.y, star.valid }, ToRunSettings(settings), probe, sink);

    FrameReport report;
    switch (r.outcome)
    {
    case cr::Outcome::Continue:
        break;

    case cr::Outcome::Failed:
    {
        std::string msg;
        bool alert = true;
        switch (r.fail)
        {
        case cr::Fail::Cleared:
            msg = "Calibration failed: the calibration was cleared while calibrating";
            alert = false;
            break;
        case cr::Fail::RaDidNotMove:
            msg = "RA Calibration Failed: star did not move enough";
            break;
        case cr::Fail::BacklashClearingFailed:
            msg = "Backlash Clearing Failed: star did not move enough";
            break;
        case cr::Fail::DecDidNotMove:
            msg = "DEC Calibration Failed: star did not move enough";
            break;
        case cr::Fail::DecModeChanged:
            msg = "Calibration failed: the Dec guide mode was changed from Off during calibration";
            break;
        }
        if (alert)
            Add(Event::Alert, msg);
        Add(Event::LogFailed, msg);
        Add(Event::NotifyFailed, msg);
        ClearCalibration();
        report.error = true;
        break;
    }

    case cr::Outcome::Complete:
    {
        // Scope copies what the run measured into m_calibration the first time;
        // on a later frame in Complete (Mount::IsCalibrated() false, say the
        // mount disconnected) it stores what it holds already
        const cr::Measured& m = m_run.measured();
        if (r.firstComplete)
        {
            m_calibration.xAngle = m.xAngle;
            m_calibration.xRate = m.xRate;
            m_calibration.yAngle = m.yAngle;
            m_calibration.yRate = m.yRate;
            m_calibration.raParity = static_cast<int>(m.raParity);
            m_calibration.decParity = static_cast<int>(m.decParity);
        }
        Cal cal(m_calibration);
        Add(Event::Probe, "Declination");
        std::optional<double> dec = m_sim.ReportedDecRad();
        cal.declination = dec ? *dec : 997.0;
        Add(Event::Probe, "SideOfPier");
        m_calibration = cal; // Scope::SetCalibration
        m_calibrated = true; // Mount::SetCalibration
        Event& set = Add(Event::SetCalibration);
        set.angle = cal.xAngle;
        set.rate = cal.xRate;
        set.parity = cal.raParity;
        set.yAngle = cal.yAngle;
        set.yRate = cal.yRate;
        set.decParity = cal.decParity;
        set.declination = cal.declination;
        Add(Event::SanityCheck);
        Add(Event::Status, "Calibration complete");
        Add(Event::LogComplete);
        Add(Event::NotifyComplete);
        Add(Event::Debug, "Calibration Complete\n");
        break;
    }
    }

    report.calibrated = !report.error && IsCalibrated();
    return report;
}

Measured ExtractedRun::Result() const
{
    const cr::Measured& r = m_run.measured();
    Measured m;
    m.xAngle = r.xAngle;
    m.xRate = r.xRate;
    m.yAngle = r.yAngle;
    m.yRate = r.yRate;
    m.raParity = static_cast<Parity>(r.raParity);
    m.decParity = static_cast<Parity>(r.decParity);
    m.raSteps = r.raSteps;
    m.decSteps = r.decSteps;
    m.raStepCount = r.raStepCount;
    m.decStepCount = r.decStepCount;
    return m;
}

} // namespace closed_loop
