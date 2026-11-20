// Differential test: calibration_run::CalibrationRun against the frozen legacy
// twin of Scope's calibration (#142).
//
// Each scenario runs twice on identical SimMounts, once through the twin (the
// scope.cpp code before the extraction, legacy/) and once through the
// extracted run as Scope now drives it (extracted_run.h), with the same frame
// hooks. The two must produce the same thing, exactly:
//   - every recorded call, in order and on the same frame: guide-log rows,
//     status messages, Calibrating events with all their fields, scheduled
//     moves, DirectComplete rows, Debug lines, alerts, failure and completion
//     reports, and the calibration stored (doubles compared bit for bit);
//   - the mount reads (coordinates, guide rates, can-pulse-guide) in the same
//     order and number, and the pulses the mount received;
//   - the outcome, the failure and what was measured.
// The scenarios are a fixed matrix over geometry, backlash, Dec guide mode,
// assumeOrthogonal, fast recenter, coordinates and pulse guiding, sweeps that
// clear the calibration or change a setting before every frame of a run, and
// 5000 seeded random scenarios. Coverage counters make sure every path of the
// state machine was taken.
//
// It also unit-tests CalibrationRun's own contract: Begin resets, Clear fails
// the next frame, a run in Complete reports Complete again if Scope enters it
// again, and a DEC_NONE run that reaches Complete with the mode switched to
// one that guides Dec fails with DecModeChanged (#148).
//
// That last one is where the run departs from the twin on purpose (#148). The
// twin stored the RA-only calibration on that frame and, IsCalibrated() being
// false, stored and reported it again on every frame after. A scenario that
// reaches that point (the twin's first SetCalibration has no Dec rate while
// the mode is Auto, North or South) must instead have the run fail with
// DecModeChanged on that frame, report only the failure (alert,
// CalibrationFailed log row and event) there, and match the twin exactly in
// everything before it: every call, the mount reads, the pulses and what was
// measured. Every other scenario must still match the twin exactly.
//
// Since #141 the extracted run sizes the RA step for the Dec it reads at Begin,
// which the twin never did. The differential therefore runs it with that read
// failing (UNKNOWN_DECLINATION), where it must still be the twin exactly, and
// leaves out the two records of that read (the Declination read and its Debug
// line at Begin), which the twin has no counterpart for. The mount's Dec is
// still random and still stored with the result. test_high_dec_calibration.cpp
// covers a known Dec.

#include <gtest/gtest.h>

#include "calibration_run.h"
#include "extracted_run.h"
#include "harness.h"
#include "legacy/legacy_twin.h"
#include "nudge_cosine.h"
#include "sim_mount.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace closed_loop;

namespace
{

// The frame hooks of a scenario, applied the same way to both implementations
struct Plan
{
    int clearAt = -1; // Clear() before this frame
    int modeAt = -1; // change the Dec guide mode before this frame (#148)
    DecMode modeTo = DecMode::Auto;
    int stepAt = -1; // change the calibration step before this frame (#117)
    int stepTo = 0;
    int distanceAt = -1; // change the calibration distance before this frame
    int distanceTo = 0;
    int bumpAt = -1; // move the star without a pulse before this frame
    double bumpX = 0., bumpY = 0.;
    int homeAt = -1; // put the star back on the lock position before this frame
    // move the star to an exact position before a frame (#154's nudge geometry, nudge_cosine.h)
    struct Place
    {
        int at = -1;
        double x = 0., y = 0.;
    };
    Place place[2];
};

struct Scenario
{
    SimMountConfig cfg;
    Settings settings;
    Plan plan;
    int maxFrames = 600;
    std::string name;
};

template<class Run>
FrameHook<Run> HookFor(const Plan& p)
{
    return [p](int frame, Settings& s, SimMount& sim, Run& run)
    {
        if (frame == p.modeAt)
            s.decMode = p.modeTo;
        if (frame == p.stepAt)
            s.stepMs = p.stepTo;
        if (frame == p.distanceAt)
            s.distancePx = p.distanceTo;
        if (frame == p.bumpAt)
            sim.Displace(p.bumpX, p.bumpY);
        if (frame == p.homeAt)
            sim.MoveTo(sim.Config().startX, sim.Config().startY);
        for (const Plan::Place& at : p.place)
            if (frame == at.at)
                sim.MoveTo(at.x, at.y);
        if (frame == p.clearAt)
            run.Clear();
    };
}

uint64_t Bits(double d)
{
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

bool Same(double a, double b)
{
    return Bits(a) == Bits(b);
}

std::string Describe(const Event& e)
{
    std::ostringstream os;
    os.precision(17);
    os << "kind " << e.kind << " frame " << e.frame << " text '" << e.text << "' direction '" << e.direction << "' step "
       << e.step << " dx " << e.dx << " dy " << e.dy << " x " << e.x << " y " << e.y << " dist " << e.dist << " dir "
       << (int) e.dir << " ms " << e.ms << " opts " << e.moveOptions << " angle " << e.angle << " rate " << e.rate << " parity "
       << e.parity << " yAngle " << e.yAngle << " yRate " << e.yRate << " decParity " << e.decParity << " declination "
       << e.declination;
    return os.str();
}

bool SameEvent(const Event& a, const Event& b)
{
    return a.kind == b.kind && a.frame == b.frame && a.text == b.text && a.direction == b.direction && a.step == b.step &&
        Same(a.dx, b.dx) && Same(a.dy, b.dy) && Same(a.x, b.x) && Same(a.y, b.y) && Same(a.dist, b.dist) && a.dir == b.dir &&
        a.ms == b.ms && a.moveOptions == b.moveOptions && Same(a.angle, b.angle) && Same(a.rate, b.rate) &&
        a.parity == b.parity && Same(a.yAngle, b.yAngle) && Same(a.yRate, b.yRate) && a.decParity == b.decParity &&
        Same(a.declination, b.declination);
}

bool SameSteps(const std::vector<std::pair<double, double>>& a, const std::vector<std::pair<double, double>>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!Same(a[i].first, b[i].first) || !Same(a[i].second, b[i].second))
            return false;
    return true;
}

struct Pair
{
    CalOutcome legacy;
    CalOutcome extracted;
    std::vector<ProbeKind> legacyProbes, extractedProbes;
    std::vector<std::pair<Dir, int>> legacyPulses, extractedPulses;
};

// The extracted run as Scope drives it when the mount's Dec cannot be read at Begin
class ExtractedRunDecUnknown : public ExtractedRun
{
public:
    explicit ExtractedRunDecUnknown(SimMount& sim) : ExtractedRun(sim, false) { }
};

Pair RunBoth(const Scenario& sc)
{
    Pair p;
    SimMount a(sc.cfg);
    p.legacy = RunCalibration<LegacyTwinRun>(a, sc.settings, sc.maxFrames, HookFor<LegacyTwinRun>(sc.plan));
    p.legacyProbes = a.ProbeLog();
    p.legacyPulses = a.Pulses();
    SimMount b(sc.cfg);
    p.extracted =
        RunCalibration<ExtractedRunDecUnknown>(b, sc.settings, sc.maxFrames, HookFor<ExtractedRunDecUnknown>(sc.plan));
    std::vector<Event>& ev = p.extracted.events;
    size_t beginReads = (size_t) std::count_if(ev.begin(), ev.end(), IsBeginDecRead);
    EXPECT_EQ(beginReads, 2u) << sc.name;
    ev.erase(std::remove_if(ev.begin(), ev.end(), IsBeginDecRead), ev.end());
    p.extractedProbes = b.ProbeLog();
    p.extractedPulses = b.Pulses();
    return p;
}

::testing::AssertionResult Identical(const Pair& p)
{
    const CalOutcome& a = p.legacy;
    const CalOutcome& b = p.extracted;
    if (a.outcome != b.outcome || a.fail != b.fail || a.frames != b.frames)
        return ::testing::AssertionFailure()
            << "outcome " << (int) a.outcome << "/" << (int) b.outcome << " fail " << (int) a.fail << "/" << (int) b.fail
            << " frames " << a.frames << "/" << b.frames;
    size_t n = std::min(a.events.size(), b.events.size());
    for (size_t i = 0; i < n; i++)
        if (!SameEvent(a.events[i], b.events[i]))
            return ::testing::AssertionFailure()
                << "event " << i << "\n  legacy:    " << Describe(a.events[i]) << "\n  extracted: " << Describe(b.events[i]);
    if (a.events.size() != b.events.size())
        return ::testing::AssertionFailure() << "event count " << a.events.size() << "/" << b.events.size();
    if (p.legacyProbes != p.extractedProbes)
        return ::testing::AssertionFailure()
            << "mount reads differ: " << p.legacyProbes.size() << "/" << p.extractedProbes.size();
    if (p.legacyPulses != p.extractedPulses)
        return ::testing::AssertionFailure() << "pulses differ";
    const Measured& ma = a.measured;
    const Measured& mb = b.measured;
    if (!Same(ma.xAngle, mb.xAngle) || !Same(ma.xRate, mb.xRate) || !Same(ma.yAngle, mb.yAngle) || !Same(ma.yRate, mb.yRate) ||
        ma.raParity != mb.raParity || ma.decParity != mb.decParity || ma.raStepCount != mb.raStepCount ||
        ma.decStepCount != mb.decStepCount)
        return ::testing::AssertionFailure() << "measured calibration differs";
    if (!SameSteps(ma.raSteps, mb.raSteps) || !SameSteps(ma.decSteps, mb.decSteps))
        return ::testing::AssertionFailure() << "measured steps differ";
    if (!Same(a.finalTrue.x, b.finalTrue.x) || !Same(a.finalTrue.y, b.finalTrue.y))
        return ::testing::AssertionFailure() << "final star position differs";
    return ::testing::AssertionSuccess();
}

// The Dec guide mode a scenario's run has on a frame
DecMode ModeOn(const Scenario& sc, int frame)
{
    return sc.plan.modeAt >= 0 && frame >= sc.plan.modeAt ? sc.plan.modeTo : sc.settings.decMode;
}

// #148: the frame on which the twin first stored a calibration with no Dec
// rate while the Dec guide mode needed one, or -1 if it never did
int DecModeChangedFrame(const Scenario& sc, const CalOutcome& legacy)
{
    for (const Event& e : legacy.events)
        if (e.kind == Event::SetCalibration)
            return e.yRate == RATE_UNCALIBRATED && ModeOn(sc, e.frame) != DecMode::None ? e.frame : -1;
    return -1;
}

// A #148 scenario: the run fails with DecModeChanged on frame f, reporting only
// that, and everything recorded before frame f is the twin's exactly
::testing::AssertionResult IdenticalUntilDecModeChanged(const Scenario& sc, const Pair& p, int f)
{
    const CalOutcome& a = p.legacy;
    const CalOutcome& b = p.extracted;
    if (b.outcome != Outcome::Failed || b.fail != Fail::DecModeChanged || b.frames != f + 1)
        return ::testing::AssertionFailure()
            << "#148 at frame " << f << ": outcome " << (int) b.outcome << " fail " << (int) b.fail << " frames " << b.frames;
    auto before = [f](const std::vector<Event>& ev)
    {
        size_t n = 0;
        while (n < ev.size() && ev[n].frame < f)
            n++;
        return n;
    };
    size_t na = before(a.events);
    size_t nb = before(b.events);
    if (na != nb)
        return ::testing::AssertionFailure() << "#148: events before frame " << f << " " << na << "/" << nb;
    for (size_t i = 0; i < nb; i++)
        if (!SameEvent(a.events[i], b.events[i]))
            return ::testing::AssertionFailure() << "#148: event " << i << "\n  legacy:    " << Describe(a.events[i])
                                                 << "\n  extracted: " << Describe(b.events[i]);
    const std::string msg = "Calibration failed: the Dec guide mode was changed from Off during calibration";
    const Event::Kind failure[] = { Event::Alert, Event::LogFailed, Event::NotifyFailed };
    if (b.events.size() != nb + 3)
        return ::testing::AssertionFailure() << "#148: " << b.events.size() - nb << " events on frame " << f;
    for (size_t i = 0; i < 3; i++)
    {
        const Event& e = b.events[nb + i];
        if (e.kind != failure[i] || e.frame != f || e.text != msg)
            return ::testing::AssertionFailure() << "#148: failure event " << i << ": " << Describe(e);
    }
    // the twin's later frames in Complete read and pulse nothing
    if (p.legacyProbes != p.extractedProbes)
        return ::testing::AssertionFailure()
            << "#148: mount reads differ: " << p.legacyProbes.size() << "/" << p.extractedProbes.size();
    if (p.legacyPulses != p.extractedPulses)
        return ::testing::AssertionFailure() << "#148: pulses differ";
    const Measured& ma = a.measured;
    const Measured& mb = b.measured;
    if (!Same(ma.xAngle, mb.xAngle) || !Same(ma.xRate, mb.xRate) || !Same(ma.yAngle, mb.yAngle) || !Same(ma.yRate, mb.yRate) ||
        ma.raParity != mb.raParity || ma.decParity != mb.decParity || ma.raStepCount != mb.raStepCount ||
        ma.decStepCount != mb.decStepCount)
        return ::testing::AssertionFailure() << "#148: measured calibration differs";
    if (!SameSteps(ma.raSteps, mb.raSteps) || !SameSteps(ma.decSteps, mb.decSteps))
        return ::testing::AssertionFailure() << "#148: measured steps differ";
    // unless the scenario moves the star by hand on a frame the run never reached
    if (sc.plan.bumpAt <= f && sc.plan.homeAt <= f &&
        (!Same(a.finalTrue.x, b.finalTrue.x) || !Same(a.finalTrue.y, b.finalTrue.y)))
        return ::testing::AssertionFailure() << "#148: final star position differs";
    return ::testing::AssertionSuccess();
}

// Whether a Debug line prints a NaN right after prefix ("nan" on arm64, "-nan" on x86-64 for 0/0)
bool NanAfter(const Event& e, const std::string& prefix)
{
    if (e.kind != Event::Debug || e.text.rfind(prefix, 0) != 0)
        return false;
    std::string rest = e.text.substr(prefix.size());
    return rest.rfind("nan", 0) == 0 || rest.rfind("-nan", 0) == 0;
}

// #154: the index of the event where the twin's South nudge took the acos of a
// cosine rounded past -1 or +1 and the run's took it of the clamped cosine: the
// first event that differs is the nudge's angle, NaN in the twin and pi or 0
// in the run. -1 for any other scenario.
int NudgeClampedEvent(const Pair& p)
{
    const std::vector<Event>& a = p.legacy.events;
    const std::vector<Event>& b = p.extracted.events;
    size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && SameEvent(a[i], b[i]))
        i++;
    if (i == n || b[i].kind != Event::Debug || b[i].frame != a[i].frame)
        return -1;
    const std::string prefix = "Nudge: theta = ";
    if (!NanAfter(a[i], prefix) || (b[i].text != prefix + "3.14\n" && b[i].text != prefix + "0.00\n"))
        return -1;
    return (int) i;
}

// Whether a frame hook of the scenario acts after frame f
bool PlanActsAfter(const Plan& plan, int f)
{
    int last = std::max({ plan.clearAt, plan.modeAt, plan.stepAt, plan.distanceAt, plan.bumpAt, plan.homeAt, plan.place[0].at,
                          plan.place[1].at });
    return last > f;
}

// A #154 scenario, at event i (NudgeClampedEvent):
//   - the cosine rounded past +1 (the star went past the lock along the North
//     leg): the run's angle is 0 where the twin's was NaN; both stop the nudge
//     as "wrong direction", so everything else, the angle in that line
//     included, must be the twin's exactly;
//   - past -1 (the star still north of the lock, straight back down the North
//     leg): the run's angle is pi, and it goes on to nudge where the twin
//     stopped. Everything before must be the twin's exactly, and the run may
//     only add the nudge's South pulses: no other pulse, no mount read, the
//     same calibration measured and the same outcome (unless a frame hook
//     acts after that frame, which the twin's run never reached).
::testing::AssertionResult IdenticalUntilNudgeClamped(const Scenario& sc, const Pair& p, size_t i)
{
    const CalOutcome& a = p.legacy;
    const CalOutcome& b = p.extracted;
    const std::string discontinued = "Nudging discontinued, wrong direction: ";
    if (i + 1 >= a.events.size() || !NanAfter(a.events[i + 1], discontinued))
        return ::testing::AssertionFailure() << "#154: the twin did not stop the nudge on its NaN angle";
    if (b.events[i].text == "Nudge: theta = 0.00\n")
    {
        if (i + 1 >= b.events.size() || b.events[i + 1].text != discontinued + "0.00\n")
            return ::testing::AssertionFailure() << "#154: the run did not stop the nudge on angle 0";
        Pair q = p;
        q.extracted.events[i].text = a.events[i].text;
        q.extracted.events[i + 1].text = a.events[i + 1].text;
        return Identical(q);
    }

    const int f = a.events[i].frame;
    for (const Event& e : b.events)
        if (e.kind == Event::Debug && e.frame == f && e.text.rfind(discontinued, 0) == 0)
            return ::testing::AssertionFailure() << "#154: the run stopped the nudge at angle pi";
    const std::vector<std::pair<Dir, int>>& pa = p.legacyPulses;
    const std::vector<std::pair<Dir, int>>& pb = p.extractedPulses;
    if (pb.size() < pa.size() || !std::equal(pa.begin(), pa.end(), pb.begin()))
        return ::testing::AssertionFailure() << "#154: the twin's pulses are not the run's first ones";
    const int stepMs = sc.plan.stepAt >= 0 && sc.plan.stepAt <= f ? sc.plan.stepTo : sc.settings.stepMs;
    for (size_t k = pa.size(); k < pb.size(); k++)
        if (pb[k].first != Dir::South || pb[k].second < 0 || pb[k].second > stepMs)
            return ::testing::AssertionFailure() << "#154: pulse " << k << " is not a nudge";
    const Measured& ma = a.measured;
    const Measured& mb = b.measured;
    if (!Same(ma.xAngle, mb.xAngle) || !Same(ma.xRate, mb.xRate) || !Same(ma.yAngle, mb.yAngle) || !Same(ma.yRate, mb.yRate) ||
        ma.raParity != mb.raParity || ma.decParity != mb.decParity || ma.raStepCount != mb.raStepCount ||
        ma.decStepCount != mb.decStepCount || !SameSteps(ma.raSteps, mb.raSteps) || !SameSteps(ma.decSteps, mb.decSteps))
        return ::testing::AssertionFailure() << "#154: measured calibration differs";
    if (PlanActsAfter(sc.plan, f))
        return ::testing::AssertionSuccess();
    if (a.outcome != b.outcome || a.fail != b.fail || b.frames < a.frames)
        return ::testing::AssertionFailure()
            << "#154: outcome " << (int) a.outcome << "/" << (int) b.outcome << " fail " << (int) a.fail << "/" << (int) b.fail;
    if (p.legacyProbes != p.extractedProbes)
        return ::testing::AssertionFailure()
            << "#154: mount reads differ: " << p.legacyProbes.size() << "/" << p.extractedProbes.size();
    return ::testing::AssertionSuccess();
}

// The comparison a scenario gets: the twin exactly, except on the #148 and #154 paths
::testing::AssertionResult Matches(const Scenario& sc, const Pair& p)
{
    int f = DecModeChangedFrame(sc, p.legacy);
    if (f >= 0)
        return IdenticalUntilDecModeChanged(sc, p, f);
    int i = NudgeClampedEvent(p);
    return i < 0 ? Identical(p) : IdenticalUntilNudgeClamped(sc, p, (size_t) i);
}

// What the scenarios exercised, so a passing run cannot be a vacuous one
struct Coverage
{
    int complete = 0, completeDecNone = 0, reentered = 0, decModeChanged = 0, raFail = 0, decFail = 0, backlashFail = 0,
        cleared = 0, backlashProceed = 0, backlashAccepted = 0, nudges = 0, nudgeWrongWay = 0, eastAdvisory = 0,
        southAdvisory = 0, assumeOrtho = 0, parityOdd = 0, noCoords = 0, ranOut = 0, nudgeClampedPi = 0, nudgeClampedZero = 0;

    void Add(const Pair& p)
    {
        const CalOutcome& o = p.legacy;
        // the twin's nudge angle was NaN for a cosine rounded past -1 or +1; the run's is pi or 0 (#154)
        int clamped = NudgeClampedEvent(p);
        if (clamped >= 0)
        {
            bool pi = p.extracted.events[(size_t) clamped].text == "Nudge: theta = 3.14\n";
            nudgeClampedPi += pi;
            nudgeClampedZero += !pi;
        }
        // the twin re-entered Complete (#148); the run failed on that frame instead
        decModeChanged += p.extracted.fail == Fail::DecModeChanged;
        if (o.outcome == Outcome::Complete)
            complete++;
        if (o.outcome == Outcome::Continue)
            ranOut++;
        if (o.HasDebug("Skipping Dec calibration"))
            completeDecNone++;
        if (o.Count(Event::LogComplete) > 1)
            reentered++;
        raFail += o.fail == Fail::RaDidNotMove;
        decFail += o.fail == Fail::DecDidNotMove;
        backlashFail += o.fail == Fail::BacklashClearingFailed;
        cleared += o.fail == Fail::Cleared;
        backlashProceed += o.HasDebug("Reached clearing limit");
        backlashAccepted += o.HasDebug("Got 3 acceptable moves");
        nudges += o.HasDebug("Sending NudgeSouth");
        nudgeWrongWay += o.HasDebug("Nudging discontinued");
        eastAdvisory += o.HasDebug("Calibration alert:");
        southAdvisory += o.HasDebug("Omitted calibration alert");
        assumeOrtho += o.HasDebug("Assuming orthogonal axes");
        parityOdd += o.measured.raParity == Parity::Odd || o.measured.decParity == Parity::Odd;
        noCoords += o.HasDebug("coords = N/A");
    }
};

void Check(const Scenario& sc, Coverage *cov = nullptr)
{
    Pair p = RunBoth(sc);
    EXPECT_TRUE(Matches(sc, p)) << sc.name;
    if (cov)
        cov->Add(p);
}

SimMountConfig BaseMount()
{
    SimMountConfig c;
    c.raPxPerMsAtEquator = 0.003878;
    c.decPxPerMs = 0.003878;
    return c;
}

Settings BaseSettings()
{
    Settings s;
    s.stepMs = 550;
    s.distancePx = 25;
    return s;
}

// A seeded random source with its own mapping, so a seed means the same scenario everywhere
class Rng
{
    std::mt19937_64 m_rng;

public:
    explicit Rng(uint64_t seed) : m_rng(seed) { }
    double U(double lo, double hi) { return lo + (hi - lo) * ((double) (m_rng() >> 11) * (1.0 / 9007199254740992.0)); }
    int I(int lo, int hi) { return lo + (int) (m_rng() % (uint64_t) (hi - lo + 1)); }
    bool B(double p) { return U(0., 1.) < p; }
    uint64_t Raw() { return m_rng(); }
};

Scenario RandomScenario(uint64_t seed)
{
    Rng r(seed);
    Scenario sc;
    SimMountConfig& c = sc.cfg;
    c.westAngleDeg = r.U(-180., 180.);
    c.decMirrored = r.B(0.5);
    c.nonOrthoDeg = r.B(0.3) ? r.U(-20., 20.) : 0.;
    auto rate = [&r]()
    {
        if (r.B(0.04))
            return 0.;
        if (r.B(0.06))
            return r.U(0., 0.0003);
        return r.U(0.001, 0.03);
    };
    c.raPxPerMsAtEquator = rate();
    c.decPxPerMs = rate();
    c.trueDecDeg = r.B(0.2) ? r.U(-89.5, 89.5) : r.U(-60., 60.);
    c.reportDec = r.B(0.9);
    if (r.B(0.1))
        c.reportedDecDeg = r.U(-90., 90.);
    c.decBacklashMs = r.B(0.4) ? r.U(0., 5000.) : 0.;
    c.decEngagedSouth = r.B(0.7);
    c.noiseSigmaPx = r.B(0.3) ? 0. : r.U(0., 0.6);
    c.seed = r.Raw();
    c.coordinates = r.B(0.8);
    c.raParityOdd = r.B(0.2);
    c.decParityOdd = r.B(0.2);
    c.guideRates = r.B(0.8);
    c.raGuideDegPerSec = r.U(0.1, 1.) * 15. / 3600.;
    c.decGuideDegPerSec = r.B(0.5) ? c.raGuideDegPerSec : r.U(0.1, 1.) * 15. / 3600.;
    c.canPulseGuide = r.B(0.7);
    c.startX = r.U(100., 900.);
    c.startY = r.U(100., 900.);

    Settings& s = sc.settings;
    s.stepMs = r.I(100, 2000);
    s.distancePx = r.I(8, 45);
    s.maxRaMs = r.I(300, 5000);
    s.maxDecMs = r.I(300, 5000);
    s.decMode = (DecMode) r.I(0, 3);
    s.assumeOrthogonal = r.B(0.3);
    s.fastRecenter = r.B(0.7);
    s.maxMovePx = r.I(5, 50);

    Plan& p = sc.plan;
    if (r.B(0.1))
        p.clearAt = r.I(0, 150);
    if (r.B(0.15))
    {
        p.modeAt = r.I(0, 150);
        p.modeTo = (DecMode) r.I(0, 3);
    }
    if (r.B(0.1))
    {
        p.stepAt = r.I(0, 150);
        p.stepTo = r.I(100, 2000);
    }
    if (r.B(0.05))
    {
        p.distanceAt = r.I(0, 150);
        p.distanceTo = r.I(8, 45);
    }
    if (r.B(0.15))
    {
        p.bumpAt = r.I(0, 150);
        p.bumpX = r.U(-20., 20.);
        p.bumpY = r.U(-20., 20.);
    }
    if (r.B(0.05))
        p.homeAt = r.I(0, 150);

    sc.name = "random seed " + std::to_string(seed);
    return sc;
}

// The frames of a scenario's run, for sweeps that act before each one
int FramesOf(const Scenario& sc)
{
    SimMount sim(sc.cfg);
    return RunCalibration<LegacyTwinRun>(sim, sc.settings, sc.maxFrames).frames;
}

} // namespace

// ---------------------------------------------------------------------------
// Differential: the extracted run against the legacy twin
// ---------------------------------------------------------------------------

TEST(CalibrationRunParity, ScenarioMatrix)
{
    Coverage cov;
    const double angles[] = { 0., 30., 90., 200., -135. };
    for (double angle : angles)
        for (int mirrored = 0; mirrored < 2; mirrored++)
            for (int backlash = 0; backlash < 2; backlash++)
                for (int mode = 0; mode < 4; mode++)
                    for (int ortho = 0; ortho < 2; ortho++)
                        for (int fast = 0; fast < 2; fast++)
                            for (int coords = 0; coords < 2; coords++)
                                for (int pulse = 0; pulse < 2; pulse++)
                                    for (int noise = 0; noise < 2; noise++)
                                    {
                                        Scenario sc;
                                        sc.cfg = BaseMount();
                                        sc.cfg.westAngleDeg = angle;
                                        sc.cfg.decMirrored = mirrored != 0;
                                        sc.cfg.nonOrthoDeg = ortho ? 8. : 0.;
                                        sc.cfg.decBacklashMs = backlash ? 1500. : 0.;
                                        sc.cfg.coordinates = coords != 0;
                                        sc.cfg.canPulseGuide = pulse != 0;
                                        sc.cfg.noiseSigmaPx = noise ? 0.2 : 0.;
                                        sc.cfg.seed = 7;
                                        sc.settings = BaseSettings();
                                        sc.settings.decMode = (DecMode) mode;
                                        sc.settings.assumeOrthogonal = ortho != 0;
                                        sc.settings.fastRecenter = fast != 0;
                                        std::ostringstream name;
                                        name << "angle " << angle << " mirrored " << mirrored << " backlash " << backlash
                                             << " mode " << mode << " ortho " << ortho << " fast " << fast << " coords "
                                             << coords << " pulse " << pulse << " noise " << noise;
                                        sc.name = name.str();
                                        Check(sc, &cov);
                                    }
    EXPECT_GT(cov.complete, 0);
    EXPECT_GT(cov.completeDecNone, 0);
    EXPECT_GT(cov.backlashAccepted, 0);
    EXPECT_GT(cov.nudges, 0);
    EXPECT_GT(cov.assumeOrtho, 0);
    EXPECT_GT(cov.noCoords, 0);
}

TEST(CalibrationRunParity, FailuresAndEdges)
{
    Coverage cov;
    for (int noise = 0; noise < 2; noise++)
    {
        const double sigma = noise ? 0.3 : 0.;
        Scenario sc;
        sc.cfg = BaseMount();
        sc.cfg.noiseSigmaPx = sigma;
        sc.settings = BaseSettings();

        sc.cfg.raPxPerMsAtEquator = 0.; // RA never moves
        sc.name = "RA never moves";
        Check(sc, &cov);

        sc.cfg = BaseMount();
        sc.cfg.noiseSigmaPx = sigma;
        sc.cfg.decPxPerMs = 0.; // Dec never moves: backlash clearing fails
        sc.name = "Dec never moves";
        Check(sc, &cov);

        const double decRates[] = { 0.0002, 0.0005, 0.0008, 0.0012 }; // proceeds after clearing, then may fail
        for (double r : decRates)
        {
            sc.cfg.decPxPerMs = r;
            sc.name = "Dec barely moves " + std::to_string(r);
            Check(sc, &cov);
        }

        // every declination, with the mount's Dec reported, missing and wrong
        for (int dec = -89; dec <= 89; dec += 2)
            for (int report = 0; report < 3; report++)
            {
                sc.cfg = BaseMount();
                sc.cfg.noiseSigmaPx = sigma;
                sc.cfg.trueDecDeg = dec;
                sc.cfg.reportDec = report != 1;
                if (report == 2)
                    sc.cfg.reportedDecDeg = 0.;
                sc.name = "Dec " + std::to_string(dec) + " report " + std::to_string(report);
                Check(sc, &cov);
            }

        // guide rates that differ between the axes, or are unavailable
        for (int rates = 0; rates < 3; rates++)
        {
            sc.cfg = BaseMount();
            sc.cfg.noiseSigmaPx = sigma;
            sc.cfg.guideRates = rates != 2;
            sc.cfg.decGuideDegPerSec = rates == 1 ? sc.cfg.raGuideDegPerSec * 0.5 : sc.cfg.raGuideDegPerSec;
            sc.cfg.decBacklashMs = 800.;
            sc.name = "guide rates " + std::to_string(rates);
            Check(sc, &cov);
        }

        // parity from the coordinates
        for (int odd = 0; odd < 4; odd++)
        {
            sc.cfg = BaseMount();
            sc.cfg.noiseSigmaPx = sigma;
            sc.cfg.raParityOdd = (odd & 1) != 0;
            sc.cfg.decParityOdd = (odd & 2) != 0;
            sc.name = "parity " + std::to_string(odd);
            Check(sc, &cov);
        }

        // East and South legs that do not come back (advisories), and nudges that cannot help
        for (int f = 0; f < 120; f += 3)
        {
            sc.cfg = BaseMount();
            sc.cfg.noiseSigmaPx = sigma;
            sc.cfg.canPulseGuide = (f / 3) % 2 == 0;
            sc.settings = BaseSettings();
            sc.plan = Plan();
            sc.plan.homeAt = f;
            sc.name = "star home before frame " + std::to_string(f);
            Check(sc, &cov);
            sc.plan = Plan();
            sc.plan.bumpAt = f;
            sc.plan.bumpX = 15.;
            sc.plan.bumpY = -12.;
            sc.name = "bump before frame " + std::to_string(f);
            Check(sc, &cov);
        }
        sc.plan = Plan();
    }
    EXPECT_GT(cov.raFail, 0);
    EXPECT_GT(cov.backlashFail, 0);
    EXPECT_GT(cov.backlashProceed, 0);
    EXPECT_GT(cov.decFail, 0);
    EXPECT_GT(cov.eastAdvisory, 0);
    EXPECT_GT(cov.southAdvisory, 0);
    EXPECT_GT(cov.parityOdd, 0);
    EXPECT_GT(cov.nudgeWrongWay, 0);
}

TEST(CalibrationRunParity, ClearOrChangeASettingBeforeEveryFrame)
{
    Coverage cov;
    for (int variant = 0; variant < 3; variant++)
    {
        Scenario base;
        base.cfg = BaseMount();
        base.cfg.noiseSigmaPx = 0.1;
        base.cfg.decBacklashMs = variant == 1 ? 1500. : 0.;
        base.settings = BaseSettings();
        base.settings.decMode = variant == 2 ? DecMode::None : DecMode::Auto;
        const int frames = FramesOf(base) + 2;
        for (int f = 0; f < frames; f++)
        {
            Scenario sc = base;
            sc.plan.clearAt = f;
            sc.name = "variant " + std::to_string(variant) + " clear before frame " + std::to_string(f);
            Check(sc, &cov);

            for (int mode = 0; mode < 4; mode++)
            {
                sc = base;
                sc.maxFrames = frames + 20;
                sc.plan.modeAt = f;
                sc.plan.modeTo = (DecMode) mode;
                sc.name = "variant " + std::to_string(variant) + " mode " + std::to_string(mode) + " before frame " +
                    std::to_string(f);
                Check(sc, &cov);
            }

            sc = base;
            sc.plan.stepAt = f;
            sc.plan.stepTo = 900;
            sc.name = "variant " + std::to_string(variant) + " step 900 before frame " + std::to_string(f);
            Check(sc, &cov);

            sc = base;
            sc.plan.distanceAt = f;
            sc.plan.distanceTo = 15;
            sc.name = "variant " + std::to_string(variant) + " distance 15 before frame " + std::to_string(f);
            Check(sc, &cov);
        }
    }
    EXPECT_GT(cov.cleared, 0);
    EXPECT_GT(cov.reentered, 0);
    EXPECT_GT(cov.decModeChanged, 0);
    EXPECT_GT(cov.ranOut, 0);
}

TEST(CalibrationRunParity, RandomScenarios)
{
    Coverage cov;
    for (uint64_t seed = 1; seed <= 5000; seed++)
    {
        Scenario sc = RandomScenario(seed);
        Pair p = RunBoth(sc);
        ::testing::AssertionResult same = Matches(sc, p);
        EXPECT_TRUE(same) << sc.name;
        if (!same)
            break; // one detailed report is enough
        cov.Add(p);
    }
    EXPECT_GT(cov.complete, 1000);
    EXPECT_GT(cov.completeDecNone, 0);
    EXPECT_GT(cov.raFail, 0);
    EXPECT_GT(cov.decFail, 0);
    EXPECT_GT(cov.backlashFail, 0);
    EXPECT_GT(cov.backlashProceed, 0);
    EXPECT_GT(cov.cleared, 0);
    EXPECT_GT(cov.reentered, 0);
    EXPECT_GT(cov.decModeChanged, 0);
    EXPECT_GT(cov.nudges, 0);
    EXPECT_GT(cov.eastAdvisory, 0);
    EXPECT_GT(cov.southAdvisory, 0);
    EXPECT_GT(cov.assumeOrtho, 0);
    EXPECT_GT(cov.parityOdd, 0);
    EXPECT_GT(cov.noCoords, 0);
}

TEST(CalibrationRunParity, NudgeCosinePastOne)
{
    // #154: the star put where the nudge's cosine rounds past -1 (still north
    // of the lock) and past +1 (south of it), whatever the compiler fuses
    // (nudge_cosine.h). The twin's angle is NaN in both; the run's is pi, and
    // it nudges, or 0, and it stops as the twin did.
    Coverage cov;
    for (int pastLock = 0; pastLock < 2; pastLock++)
    {
        Scenario sc;
        sc.cfg = nudge_cosine::Mount();
        sc.settings = BaseSettings();
        std::pair<int, int> f = nudge_cosine::Frames<LegacyTwinRun>(sc.cfg, sc.settings);
        ASSERT_GE(f.first, 0);
        ASSERT_GT(f.second, f.first);
        Pt south = nudge_cosine::SouthStart(sc.cfg);
        Pt nudge = nudge_cosine::NudgeStar(sc.cfg, pastLock != 0);
        sc.plan.place[0] = { f.first, south.x, south.y };
        sc.plan.place[1] = { f.second, nudge.x, nudge.y };
        sc.name = pastLock ? "nudge cosine past +1" : "nudge cosine past -1";
        Check(sc, &cov);
    }
    EXPECT_EQ(cov.nudgeClampedPi, 1);
    EXPECT_EQ(cov.nudgeClampedZero, 1);
}

// ---------------------------------------------------------------------------
// CalibrationRun's own contract
// ---------------------------------------------------------------------------

namespace
{

namespace cr = calibration_run;

class RecordingSink : public cr::Sink
{
public:
    std::vector<cr::Effect> effects;
    void Emit(const cr::Effect& e) override { effects.push_back(e); }
};

class SimProbe : public cr::Probe
{
    SimMount& m_sim;

public:
    explicit SimProbe(SimMount& sim) : m_sim(sim) { }
    bool Coordinates(double *ra, double *dec) override { return m_sim.Coordinates(ra, dec); }
    bool GuideRates(double *ra, double *dec) override { return m_sim.GuideRates(ra, dec); }
    bool CanPulseGuide() override { return m_sim.CanPulseGuide(); }
};

cr::Pt ToRun(const Pt& p)
{
    return cr::Pt { p.x, p.y, p.valid };
}

cr::Settings RunSettings()
{
    cr::Settings s;
    s.stepMs = 550;
    s.distancePx = 25;
    return s;
}

// Drives a run on the sim, applying its moves, until it leaves Continue or maxFrames pass
cr::FrameResult Drive(cr::CalibrationRun& run, SimMount& sim, const cr::Settings& s, int maxFrames,
                      RecordingSink *out = nullptr)
{
    SimProbe probe(sim);
    cr::FrameResult r;
    for (int i = 0; i < maxFrames; i++)
    {
        RecordingSink sink;
        r = run.OnFrame(ToRun(sim.Observe()), s, probe, sink);
        for (const cr::Effect& e : sink.effects)
            if (e.kind == cr::Effect::Move)
                sim.Pulse(static_cast<Dir>(e.dir), e.ms);
        if (out)
            out->effects.insert(out->effects.end(), sink.effects.begin(), sink.effects.end());
        if (r.outcome != cr::Outcome::Continue)
            break;
    }
    return r;
}

} // namespace

TEST(CalibrationRun, StartsClearedAndFailsWithoutBegin)
{
    SimMount sim(BaseMount());
    SimProbe probe(sim);
    RecordingSink sink;
    cr::CalibrationRun run;
    EXPECT_EQ(run.state(), cr::State::Cleared);
    cr::FrameResult r = run.OnFrame(ToRun(sim.Observe()), RunSettings(), probe, sink);
    EXPECT_EQ(r.outcome, cr::Outcome::Failed);
    EXPECT_EQ(r.fail, cr::Fail::Cleared);
}

TEST(CalibrationRun, BeginResetsARunThatWasUnderWay)
{
    SimMount sim(BaseMount());
    cr::CalibrationRun run;
    run.Begin(ToRun(sim.Observe()), std::nullopt);
    // into the North leg, with measurements made
    for (int i = 0; i < 40 && run.state() != cr::State::GoNorth; i++)
        Drive(run, sim, RunSettings(), 1);
    ASSERT_EQ(run.state(), cr::State::GoNorth);
    ASSERT_FALSE(run.measured().raSteps.empty());

    run.Begin(ToRun(sim.Observe()), std::nullopt);
    EXPECT_EQ(run.state(), cr::State::GoWest);
    EXPECT_TRUE(run.measured().raSteps.empty());
    EXPECT_TRUE(run.measured().decSteps.empty());
    EXPECT_EQ(run.measured().raStepCount, 0);
    EXPECT_EQ(run.measured().decStepCount, 0);

    // the first frame after Begin takes the starting location and reads the coordinates again
    int coordReads = sim.probeCalls[(int) ProbeKind::Coordinates];
    RecordingSink sink;
    Drive(run, sim, RunSettings(), 1, &sink);
    EXPECT_EQ(sim.probeCalls[(int) ProbeKind::Coordinates], coordReads + 1);
    ASSERT_FALSE(sink.effects.empty());
    EXPECT_EQ(sink.effects[0].kind, cr::Effect::Debug);
    EXPECT_EQ(sink.effects[0].text.rfind("Scope::UpdateCalibrationState: starting location", 0), 0u);
    EXPECT_EQ(sink.effects[1].kind, cr::Effect::LogStep);
    EXPECT_EQ(sink.effects[1].rec.leg, cr::Leg::West);
    EXPECT_EQ(sink.effects[1].rec.step, 0);
}

TEST(CalibrationRun, ClearFromAnyStateFailsTheNextFrameWithNothingElse)
{
    // the states a run passes through, each cleared once it is reached
    const cr::State states[] = { cr::State::GoWest,  cr::State::GoEast,     cr::State::ClearBacklash, cr::State::GoNorth,
                                 cr::State::GoSouth, cr::State::NudgeSouth, cr::State::Complete };
    for (cr::State target : states)
    {
        SimMountConfig c = BaseMount();
        c.decBacklashMs = 1500.;
        c.noiseSigmaPx = 0.05;
        SimMount sim(c);
        cr::CalibrationRun run;
        run.Begin(ToRun(sim.Observe()), std::nullopt);
        Drive(run, sim, RunSettings(), 1); // past the first frame, which takes the starting location
        for (int i = 0; i < 400 && run.state() != target; i++)
            Drive(run, sim, RunSettings(), 1);
        ASSERT_EQ(run.state(), target) << (int) target;

        run.Clear();
        EXPECT_EQ(run.state(), cr::State::Cleared);
        std::vector<ProbeKind> reads = sim.ProbeLog();
        SimProbe probe(sim);
        RecordingSink sink;
        cr::FrameResult r = run.OnFrame(ToRun(sim.Observe()), RunSettings(), probe, sink);
        EXPECT_EQ(r.outcome, cr::Outcome::Failed) << (int) target;
        EXPECT_EQ(r.fail, cr::Fail::Cleared);
        EXPECT_TRUE(sink.effects.empty()) << (int) target;
        EXPECT_EQ(sim.ProbeLog(), reads) << (int) target;
    }
}

TEST(CalibrationRun, CompleteIsReportedAgainOnEveryLaterFrame)
{
    SimMount sim(BaseMount());
    cr::CalibrationRun run;
    run.Begin(ToRun(sim.Observe()), std::nullopt);
    cr::FrameResult r = Drive(run, sim, RunSettings(), 400);
    ASSERT_EQ(r.outcome, cr::Outcome::Complete);
    EXPECT_TRUE(r.firstComplete);
    const cr::Measured first = run.measured();

    for (int i = 0; i < 3; i++)
    {
        std::vector<ProbeKind> reads = sim.ProbeLog();
        SimProbe probe(sim);
        RecordingSink sink;
        r = run.OnFrame(ToRun(sim.Observe()), RunSettings(), probe, sink);
        EXPECT_EQ(r.outcome, cr::Outcome::Complete);
        EXPECT_FALSE(r.firstComplete);
        EXPECT_TRUE(sink.effects.empty());
        EXPECT_EQ(sim.ProbeLog(), reads);
        EXPECT_EQ(run.measured().xAngle, first.xAngle);
        EXPECT_EQ(run.measured().yRate, first.yRate);
    }

    // a new Begin starts over: the next completion is a first one again
    run.Begin(ToRun(sim.Observe()), std::nullopt);
    r = Drive(run, sim, RunSettings(), 400);
    ASSERT_EQ(r.outcome, cr::Outcome::Complete);
    EXPECT_TRUE(r.firstComplete);
}

TEST(CalibrationRun, DecNoneCompletesOnTheFrameAfterTheEastLeg)
{
    SimMount sim(BaseMount());
    cr::CalibrationRun run;
    cr::Settings s = RunSettings();
    s.decMode = cr::DecMode::None;
    run.Begin(ToRun(sim.Observe()), std::nullopt);
    RecordingSink all;
    cr::FrameResult r;
    int frames = 0;
    while (frames < 400 && run.state() != cr::State::Complete)
    {
        r = Drive(run, sim, s, 1, &all);
        frames++;
    }
    ASSERT_EQ(run.state(), cr::State::Complete);
    // the frame that skipped the Dec leg did not complete the run
    EXPECT_EQ(r.outcome, cr::Outcome::Continue);
    EXPECT_EQ(run.measured().yRate, cr::RATE_UNCALIBRATED);
    for (const cr::Effect& e : all.effects)
    {
        if (e.kind == cr::Effect::Move)
        {
            EXPECT_TRUE(e.dir == cr::Dir::West || e.dir == cr::Dir::East);
        }
    }
    r = Drive(run, sim, s, 1);
    EXPECT_EQ(r.outcome, cr::Outcome::Complete);
    EXPECT_TRUE(r.firstComplete);
}

TEST(CalibrationRun, DecModeChangedFromNoneFailsWithNoEffectsOrReads)
{
    // #148: the run skipped the Dec leg for DEC_NONE; the mode is Auto, North
    // or South on the frame it reaches Complete
    for (int to = (int) cr::DecMode::Auto; to <= (int) cr::DecMode::South; to++)
    {
        SimMount sim(BaseMount());
        cr::CalibrationRun run;
        cr::Settings s = RunSettings();
        s.decMode = cr::DecMode::None;
        run.Begin(ToRun(sim.Observe()), std::nullopt);
        for (int i = 0; i < 400 && run.state() != cr::State::Complete; i++)
            Drive(run, sim, s, 1);
        ASSERT_EQ(run.state(), cr::State::Complete);
        const cr::Measured skipped = run.measured();
        ASSERT_EQ(skipped.yRate, cr::RATE_UNCALIBRATED);

        s.decMode = static_cast<cr::DecMode>(to);
        std::vector<ProbeKind> reads = sim.ProbeLog();
        SimProbe probe(sim);
        RecordingSink sink;
        cr::FrameResult r = run.OnFrame(ToRun(sim.Observe()), s, probe, sink);
        SCOPED_TRACE("mode " + std::to_string(to));
        EXPECT_EQ(r.outcome, cr::Outcome::Failed);
        EXPECT_EQ(r.fail, cr::Fail::DecModeChanged);
        EXPECT_FALSE(r.firstComplete);
        EXPECT_TRUE(sink.effects.empty());
        EXPECT_EQ(sim.ProbeLog(), reads);
        EXPECT_EQ(run.measured().xRate, skipped.xRate);
        EXPECT_EQ(run.measured().yRate, cr::RATE_UNCALIBRATED);

        // it does not report Complete again, whatever the mode is next
        EXPECT_NE(run.state(), cr::State::Complete);
        for (int next = (int) cr::DecMode::None; next <= (int) cr::DecMode::South; next++)
        {
            s.decMode = static_cast<cr::DecMode>(next);
            RecordingSink later;
            r = run.OnFrame(ToRun(sim.Observe()), s, probe, later);
            EXPECT_NE(r.outcome, cr::Outcome::Complete) << next;
            EXPECT_TRUE(later.effects.empty()) << next;
        }

        // the next calibration starts from Begin as always, and completes
        s.decMode = cr::DecMode::None;
        run.Begin(ToRun(sim.Observe()), std::nullopt);
        r = Drive(run, sim, s, 400);
        EXPECT_EQ(r.outcome, cr::Outcome::Complete);
        EXPECT_TRUE(r.firstComplete);
    }
}

TEST(CalibrationRun, DecNoneCompletesWhenTheModeIsStillNone)
{
    // #148's check needs both: a calibration with no Dec rate and a mode that
    // guides Dec. A DEC_NONE run still completes when the mode is None, and an
    // Auto run that measured the Dec rate completes when the mode is then None.
    SimMount sim(BaseMount());
    cr::CalibrationRun run;
    cr::Settings s = RunSettings();
    s.decMode = cr::DecMode::None;
    run.Begin(ToRun(sim.Observe()), std::nullopt);
    cr::FrameResult r = Drive(run, sim, s, 400);
    EXPECT_EQ(r.outcome, cr::Outcome::Complete);
    EXPECT_EQ(run.measured().yRate, cr::RATE_UNCALIBRATED);

    SimMountConfig c = BaseMount();
    c.noiseSigmaPx = 0.05;
    SimMount sim2(c);
    s.decMode = cr::DecMode::Auto;
    run.Begin(ToRun(sim2.Observe()), std::nullopt);
    for (int i = 0; i < 400 && run.state() != cr::State::NudgeSouth; i++)
        Drive(run, sim2, s, 1);
    ASSERT_EQ(run.state(), cr::State::NudgeSouth);
    s.decMode = cr::DecMode::None;
    r = Drive(run, sim2, s, 400);
    EXPECT_EQ(r.outcome, cr::Outcome::Complete);
    EXPECT_NE(run.measured().yRate, cr::RATE_UNCALIBRATED);
}

TEST(NudgeAngle, IsTheAngleBetweenTheNudgeAndTheNorthLeg)
{
    EXPECT_DOUBLE_EQ(cr::NudgeAngle(-1., 0., 1., 0.), M_PI);
    EXPECT_DOUBLE_EQ(cr::NudgeAngle(1., 0., 1., 0.), 0.);
    EXPECT_DOUBLE_EQ(cr::NudgeAngle(0., 1., 1., 0.), M_PI / 2);
    EXPECT_DOUBLE_EQ(cr::NudgeAngle(-0.6, -0.8, 0.8, -0.6), M_PI / 2);
    const double c = std::cos(Radians(150.));
    const double s = std::sin(Radians(150.));
    EXPECT_NEAR(cr::NudgeAngle(c, s, 1., 0.), Radians(150.), 1e-12);
}

TEST(NudgeAngle, ACosineRoundedPastMinusOneIsPi)
{
    // #154: exactly antiparallel directions along (115, 252), hypotenuse 277.
    // Their rounded direction cosines give a cosine past -1 however the dot
    // product is rounded: two rounded products and a sum, or either product
    // fused into the add (the premise, checked here). acos of it was NaN.
    const double a = 115. / 277.;
    const double b = 252. / 277.;
    volatile double aa = a * a;
    volatile double bb = b * b;
    EXPECT_GT(aa + bb, 1.);
    EXPECT_GT(std::fma(a, a, bb), 1.);
    EXPECT_GT(std::fma(b, b, aa), 1.);

    EXPECT_EQ(cr::NudgeAngle(a, b, -a, -b), std::acos(-1.));
    EXPECT_EQ(cr::NudgeAngle(-a, -b, a, b), std::acos(-1.));
    EXPECT_EQ(cr::NudgeAngle(b, a, -b, -a), std::acos(-1.));
}

TEST(NudgeAngle, ACosineRoundedPastPlusOneIsZero)
{
    const double a = 115. / 277.;
    const double b = 252. / 277.;
    EXPECT_EQ(cr::NudgeAngle(a, b, a, b), 0.);
    EXPECT_EQ(cr::NudgeAngle(-a, -b, -a, -b), 0.);
}

TEST(NudgeAngle, NanDirectionCosinesStayNan)
{
    // the star exactly on the lock: 0/0 direction cosines. The angle stays
    // NaN, which fails the nudge's 40 deg test and ends the nudge, where it is
    // not needed (pinned, not changed by #154)
    const double nan = std::nan("");
    EXPECT_TRUE(std::isnan(cr::NudgeAngle(nan, nan, 0.6, 0.8)));
    EXPECT_TRUE(std::isnan(cr::NudgeAngle(nan, nan, -0.6, -0.8)));
}
