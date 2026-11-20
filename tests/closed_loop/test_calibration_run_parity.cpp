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
// the next frame, and a run in Complete reports Complete again (#148).
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

// What the scenarios exercised, so a passing run cannot be a vacuous one
struct Coverage
{
    int complete = 0, completeDecNone = 0, reentered = 0, raFail = 0, decFail = 0, backlashFail = 0, cleared = 0,
        backlashProceed = 0, backlashAccepted = 0, nudges = 0, nudgeWrongWay = 0, eastAdvisory = 0, southAdvisory = 0,
        assumeOrtho = 0, parityOdd = 0, noCoords = 0, ranOut = 0;

    void Add(const CalOutcome& o)
    {
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
    EXPECT_TRUE(Identical(p)) << sc.name;
    if (cov)
        cov->Add(p.legacy);
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
    EXPECT_GT(cov.ranOut, 0);
}

TEST(CalibrationRunParity, RandomScenarios)
{
    Coverage cov;
    for (uint64_t seed = 1; seed <= 5000; seed++)
    {
        Scenario sc = RandomScenario(seed);
        Pair p = RunBoth(sc);
        ::testing::AssertionResult same = Identical(p);
        EXPECT_TRUE(same) << sc.name;
        if (!same)
            break; // one detailed report is enough
        cov.Add(p.legacy);
    }
    EXPECT_GT(cov.complete, 1000);
    EXPECT_GT(cov.completeDecNone, 0);
    EXPECT_GT(cov.raFail, 0);
    EXPECT_GT(cov.decFail, 0);
    EXPECT_GT(cov.backlashFail, 0);
    EXPECT_GT(cov.backlashProceed, 0);
    EXPECT_GT(cov.cleared, 0);
    EXPECT_GT(cov.reentered, 0);
    EXPECT_GT(cov.nudges, 0);
    EXPECT_GT(cov.eastAdvisory, 0);
    EXPECT_GT(cov.southAdvisory, 0);
    EXPECT_GT(cov.assumeOrtho, 0);
    EXPECT_GT(cov.parityOdd, 0);
    EXPECT_GT(cov.noCoords, 0);
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
