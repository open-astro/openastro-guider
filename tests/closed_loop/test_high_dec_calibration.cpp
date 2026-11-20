// High-declination calibration (#141), closed loop.
//
// A star's RA motion per ms of guide pulse shrinks with cos(Dec). With one
// step for both axes, the West leg needs 1/cos(Dec) as many pulses as at the
// equator and fails once it needs more than MAX_CALIBRATION_STEPS + 1 (61):
// above about 79 deg on the audit profile. CalibrationRun sizes the West and
// East pulses for the Dec Scope reads when the calibration begins
// (RaCalibrationStepMs, up to RA_STEP_MAX_SCALE times the step); the Dec leg,
// backlash clearing and the South nudge keep the step. Backlash clearing
// looks for the Dec leg's move estimated for that Dec (#156), where the RA
// rate made it shrink with cos(Dec).
//
// These run the extracted CalibrationRun as Scope drives it (extracted_run.h)
// on a SimMount whose RA image rate follows cos(true Dec), and compare it with
// the frozen legacy twin where the Dec is not known. Every test starts a
// fresh calibration with Begin: nothing here restores, reuses or skips one.
//
// The profile is the audit's: FL 400 mm, 3.75 um pixels, bin 1, 0.5x sidereal,
// so 0.003878 px/ms at Dec 0, a 550 ms step and a 25 px distance.

#include <gtest/gtest.h>

#include "calibration_run.h"
#include "extracted_run.h"
#include "harness.h"
#include "legacy/legacy_twin.h"
#include "sim_mount.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace closed_loop;

namespace
{

namespace cr = calibration_run;

constexpr double kRate = 0.003878; // px/ms at Dec 0, the audit profile
constexpr int kStepMs = 550;
constexpr int kDistancePx = 25;
constexpr int kRaStepBudget = 40; // West steps a calibration may take at any Dec up to 85

SimMountConfig ProfileMount(double trueDecDeg)
{
    SimMountConfig c;
    c.raPxPerMsAtEquator = kRate;
    c.decPxPerMs = kRate;
    c.trueDecDeg = trueDecDeg;
    return c;
}

Settings ProfileSettings()
{
    Settings s;
    s.stepMs = kStepMs;
    s.distancePx = kDistancePx;
    return s;
}

template<class Run>
CalOutcome Calibrate(const SimMountConfig& cfg, const Settings& s = ProfileSettings())
{
    SimMount sim(cfg);
    return RunCalibration<Run>(sim, s);
}

// The star's displacement from the lock along the West direction, px
double RaOffset(const CalOutcome& o, const SimMountConfig& c)
{
    double w = Radians(c.westAngleDeg);
    return (o.finalTrue.x - o.lock.x) * std::cos(w) + (o.finalTrue.y - o.lock.y) * std::sin(w);
}

// The two runs made the same calls on the same frames, leaving out the Dec read at Begin
::testing::AssertionResult SameCalls(const CalOutcome& legacy, const CalOutcome& extracted)
{
    std::vector<Event> b;
    for (const Event& e : extracted.events)
        if (!IsBeginDecRead(e))
            b.push_back(e);
    const std::vector<Event>& a = legacy.events;
    if (a.size() != b.size())
        return ::testing::AssertionFailure() << "event count " << a.size() << "/" << b.size();
    for (size_t i = 0; i < a.size(); i++)
    {
        const Event& x = a[i];
        const Event& y = b[i];
        if (x.kind != y.kind || x.frame != y.frame || x.text != y.text || x.dir != y.dir || x.ms != y.ms || x.step != y.step ||
            x.dx != y.dx || x.dy != y.dy || x.dist != y.dist || x.angle != y.angle || x.rate != y.rate ||
            x.yAngle != y.yAngle || x.yRate != y.yRate)
            return ::testing::AssertionFailure() << "event " << i << " differs: kind " << x.kind << "/" << y.kind << " text '"
                                                 << x.text << "'/'" << y.text << "' ms " << x.ms << "/" << y.ms;
    }
    if (legacy.outcome != extracted.outcome || legacy.fail != extracted.fail || legacy.frames != extracted.frames)
        return ::testing::AssertionFailure() << "outcome differs";
    return ::testing::AssertionSuccess();
}

// The South pulses the nudge sent, in order
std::vector<int> NudgePulses(const CalOutcome& o)
{
    std::vector<int> v;
    bool nudging = false;
    for (const Event& e : o.events)
    {
        if (e.kind == Event::Status)
            nudging = e.text.rfind("Nudge South", 0) == 0;
        else if (e.kind == Event::Move && nudging)
        {
            v.push_back(e.ms);
            nudging = false;
        }
    }
    return v;
}

const Event *BeginDebug(const CalOutcome& o)
{
    for (const Event& e : o.events)
        if (e.frame == -1 && e.kind == Event::Debug && e.text.rfind("RA calibration step ", 0) == 0)
            return &e;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// RaCalibrationStepMs
// ---------------------------------------------------------------------------

TEST(RaCalibrationStepMs, ScalesWithOneOverCosDecUpToFourTimesTheStep)
{
    auto at = [](int stepMs, double decDeg) { return cr::RaCalibrationStepMs(stepMs, Radians(decDeg)); };
    // no Dec: the step, as before #141
    EXPECT_EQ(cr::RaCalibrationStepMs(550, std::nullopt), 550);
    // Dec 0 is exactly the step, and the step grows smoothly (rounded up) with 1/cos(Dec)
    EXPECT_EQ(at(550, 0.), 550);
    EXPECT_EQ(at(550, 30.), 636); // 635.09
    EXPECT_EQ(at(550, -30.), 636);
    EXPECT_EQ(at(550, 45.), 778); // 777.82
    EXPECT_EQ(at(550, 60.), 1100); // 1099.9999999999998: cos(60 deg) is a hair over 0.5
    EXPECT_EQ(at(550, 70.), 1609); // 1608.09
    EXPECT_EQ(at(550, 75.), 2126); // 2125.04
    // capped at RA_STEP_MAX_SCALE times the step from acos(1/4) = 75.52 deg up
    EXPECT_EQ(at(550, 76.), 2200);
    EXPECT_EQ(at(550, 80.), 2200);
    EXPECT_EQ(at(550, -80.), 2200);
    EXPECT_EQ(at(550, 85.), 2200);
    EXPECT_EQ(at(750, 85.), 3000);
    EXPECT_EQ(at(750, 60.), 1500);
    EXPECT_EQ(at(100, 89.9), 400);
    EXPECT_DOUBLE_EQ(cr::RA_STEP_MAX_SCALE, 4.0);
}

TEST(RaCalibrationStepMs, PoleIsCappedAndFinite)
{
    // cos(+-90 deg) is about 6e-17, not 0: the cap, not the division, gives the step
    EXPECT_EQ(cr::RaCalibrationStepMs(550, M_PI / 2), 4 * 550);
    EXPECT_EQ(cr::RaCalibrationStepMs(550, -M_PI / 2), 4 * 550);
    EXPECT_EQ(cr::RaCalibrationStepMs(550, M_PI), 550); // cos(180 deg) = -1: magnitude 1
}

TEST(RaCalibrationStepMs, NeverShrinksTheStep)
{
    for (int deg = -180; deg <= 180; deg++)
        for (int stepMs : { 1, 100, 550, 750, 2000, 5000 })
        {
            int ms = cr::RaCalibrationStepMs(stepMs, Radians(deg));
            EXPECT_GE(ms, stepMs) << deg << " " << stepMs;
            EXPECT_LE(ms, 4 * stepMs) << deg << " " << stepMs;
        }
}

TEST(RaCalibrationStepMs, StaysUnderTheAlpacaPulseGuideTimeout)
{
    auto at = [](int stepMs, double decDeg) { return cr::RaCalibrationStepMs(stepMs, Radians(decDeg)); };
    EXPECT_EQ(cr::RA_STEP_MAX_MS, 25000);
    // AlpacaClient times a blocking PulseGuide out after 30 s: a grown pulse stops at 25 s
    EXPECT_EQ(at(6250, 85.), 25000); // exactly 4x
    EXPECT_EQ(at(7500, 85.), 25000); // 4x would be 30000
    EXPECT_EQ(at(20000, 60.), 25000); // 2x would be 40000
    EXPECT_EQ(at(20000, 30.), 23095); // 23094.01, under the ceiling
    // a step already over the ceiling is the user's choice and is not grown or cut
    EXPECT_EQ(at(25000, 85.), 25000);
    EXPECT_EQ(at(60000, 85.), 60000);
    EXPECT_EQ(at(60000, 0.), 60000);
    for (int deg = -90; deg <= 90; deg++)
        for (int stepMs : { 5000, 10000, 24999, 25001, 60000 })
        {
            int ms = cr::RaCalibrationStepMs(stepMs, Radians(deg));
            EXPECT_GE(ms, stepMs) << deg << " " << stepMs;
            EXPECT_LE(ms, std::max(stepMs, cr::RA_STEP_MAX_MS)) << deg << " " << stepMs;
        }
}

TEST(RaCalibrationStepMs, ANonFiniteDecKeepsTheStep)
{
    EXPECT_EQ(cr::RaCalibrationStepMs(550, std::nan("")), 550);
    EXPECT_EQ(cr::RaCalibrationStepMs(550, INFINITY), 550);
}

// ---------------------------------------------------------------------------
// The RA step budget over Dec
// ---------------------------------------------------------------------------

class HighDecStepBudget : public ::testing::TestWithParam<int>
{
};

TEST_P(HighDecStepBudget, CompletesWithinFortyRaSteps)
{
    // the West leg completes in at most 40 pulses at every Dec to 85; before
    // #141 it took 46 at 75 and failed RaDidNotMove at 79 and above
    const int dec = GetParam();
    SimMountConfig c = ProfileMount(dec);
    SimMount sim(c);
    CalOutcome o = RunCalibration<ExtractedRun>(sim, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_LE(o.measured.raStepCount, kRaStepBudget);
    EXPECT_GE(o.measured.raStepCount, 1);
    // every West pulse is the Dec's RA step
    int raStep = cr::RaCalibrationStepMs(kStepMs, Radians(dec));
    EXPECT_EQ(o.Moves(Dir::West), std::vector<int>((size_t) o.measured.raStepCount, raStep));
    EXPECT_NEAR(o.measured.xRate, sim.RaPxPerMs(), 0.02 * sim.RaPxPerMs());
    EXPECT_NEAR(o.measured.yRate, kRate, 0.02 * kRate);
}

INSTANTIATE_TEST_SUITE_P(HighDec, HighDecStepBudget, ::testing::Values(0, 45, 60, 70, 75, 80, 85, -80),
                         [](const ::testing::TestParamInfo<int>& info) {
                             return info.param < 0 ? "Dec_minus" + std::to_string(-info.param)
                                                   : "Dec" + std::to_string(info.param);
                         });

TEST(HighDec, StepCountsOverDec)
{
    // the pulses the West leg needs: about distance / (rate * cos(Dec) * raStep)
    const int cases[][3] = {
        { 0, 550, 12 },   { 45, 778, 12 },  { 60, 1100, 12 }, { 70, 1609, 12 },
        { 75, 2126, 12 }, { 80, 2200, 17 }, { 85, 2200, 34 }, { -85, 2200, 34 },
    };
    for (const auto& tc : cases)
    {
        CalOutcome o = Calibrate<ExtractedRun>(ProfileMount(tc[0]));
        SCOPED_TRACE("dec " + std::to_string(tc[0]));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_EQ(o.measured.raStepCount, tc[2]);
        EXPECT_EQ(o.Moves(Dir::West), std::vector<int>((size_t) tc[2], tc[1]));
        // Scope logs the RA step it chose, with the Dec and the step
        const Event *d = BeginDebug(o);
        ASSERT_NE(d, nullptr);
        char expected[128];
        std::snprintf(expected, sizeof(expected), "RA calibration step %d ms for Dec %.1f (Dec step %d ms)\n", tc[1],
                      (double) tc[0], kStepMs);
        EXPECT_EQ(d->text, expected);
    }
}

// ---------------------------------------------------------------------------
// What the RA leg measures and sends back
// ---------------------------------------------------------------------------

TEST(HighDec, XRateMatchesTruth)
{
    // xRate is the distance over the time pulsed: an xRate divided by the step
    // instead of the RA step would be 4x too high at 80 deg, and the guider
    // would then send a quarter of the RA correction it needs
    for (int dec : { 60, 75, 80, 85, -80 })
    {
        SimMountConfig c = ProfileMount(dec);
        SimMount sim(c);
        CalOutcome o = RunCalibration<ExtractedRun>(sim, ProfileSettings());
        SCOPED_TRACE("dec " + std::to_string(dec));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        double truth = kRate * std::cos(Radians(dec));
        EXPECT_NEAR(o.measured.xRate, truth, 0.02 * truth);
        EXPECT_NEAR(o.measured.xRate, sim.RaPxPerMs(), 0.02 * sim.RaPxPerMs());
        // the West row of the guide log carries the same rate
        ASSERT_FALSE(o.Of(Event::DirectComplete).empty());
        EXPECT_EQ(o.Of(Event::DirectComplete)[0].text, "West");
        EXPECT_EQ(o.Of(Event::DirectComplete)[0].rate, o.measured.xRate);
    }
}

TEST(HighDec, EastReturnsWest)
{
    // the East leg sends back all the West leg sent: had it counted the West
    // pulses at the step instead of the RA step, it would leave the star about
    // 75% of the way out at 80 deg
    for (int fast = 0; fast < 2; fast++)
        for (int dec : { 70, 80, 85, -80 })
        {
            SimMountConfig c = ProfileMount(dec);
            Settings s = ProfileSettings();
            s.fastRecenter = fast != 0;
            CalOutcome o = Calibrate<ExtractedRun>(c, s);
            SCOPED_TRACE("dec " + std::to_string(dec) + " fast " + std::to_string(fast));
            ASSERT_EQ(o.outcome, Outcome::Complete);
            EXPECT_EQ(o.MoveTotal(Dir::East), o.MoveTotal(Dir::West));
            EXPECT_LT(std::fabs(RaOffset(o, c)), 0.5);
            int raStep = cr::RaCalibrationStepMs(kStepMs, Radians(dec));
            std::vector<int> east = o.Moves(Dir::East);
            ASSERT_FALSE(east.empty());
            // without fast recenter East pulses the RA step; with it, never less than the RA step
            // (2500 ms, the max RA duration, here)
            for (size_t i = 0; i + 1 < east.size(); i++)
                EXPECT_EQ(east[i], fast ? std::max(raStep, 2500) : raStep);
        }
}

// ---------------------------------------------------------------------------
// What does not change
// ---------------------------------------------------------------------------

TEST(HighDec, DecLegUnchanged)
{
    // backlash clearing, the North leg and the nudge pulse the step at every Dec;
    // the South recenter returns the North leg in pulses of at least the step
    for (int dec : { 0, 60, 80, 85 })
    {
        SimMountConfig c = ProfileMount(dec);
        c.decBacklashMs = 1500.;
        c.noiseSigmaPx = 0.02;
        c.seed = 3;
        SimMount sim(c);
        CalOutcome o = RunCalibration<ExtractedRun>(sim, ProfileSettings());
        SCOPED_TRACE("dec " + std::to_string(dec));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        std::vector<int> north = o.Moves(Dir::North);
        ASSERT_FALSE(north.empty());
        EXPECT_EQ(north, std::vector<int>(north.size(), kStepMs));
        EXPECT_NEAR(o.measured.yRate, kRate, 0.02 * kRate);
        // South returns every North step, and each nudge after it is at most a step
        std::vector<int> nudges = NudgePulses(o);
        int nudgeTotal = 0;
        for (int ms : nudges)
        {
            EXPECT_LE(ms, kStepMs);
            nudgeTotal += ms;
        }
        EXPECT_EQ(o.MoveTotal(Dir::South) - nudgeTotal, o.measured.decStepCount * kStepMs);
        EXPECT_TRUE(o.HasDebug("Backlash: Starting north clearing using pulse width of 550\n"));
    }
}

TEST(HighDec, UnknownDecIsLegacy)
{
    // a mount that reports no Dec (or fails the read) keeps the step for every
    // leg: the extracted run makes exactly the calls the code before #141 made,
    // including its failure at 80 deg. Noiseless, except at Dec 0: there a
    // noiseless run ends its North leg exactly antiparallel to the nudge, and
    // on this build the cosine rounds past -1, where the twin's unclamped acos
    // gives NaN and the run's clamped one pi (#154; the parity test covers that
    // difference). A little noise keeps the Dec 0 comparison exact.
    for (int dec : { 0, 45, 75, 80, -80 })
    {
        SimMountConfig c = ProfileMount(dec);
        c.reportDec = false;
        if (dec == 0)
        {
            c.noiseSigmaPx = 0.02;
            c.seed = 3;
        }
        CalOutcome legacy = Calibrate<LegacyTwinRun>(c);
        CalOutcome extracted = Calibrate<ExtractedRun>(c);
        SCOPED_TRACE("dec " + std::to_string(dec));
        EXPECT_TRUE(SameCalls(legacy, extracted));
        for (int ms : extracted.Moves(Dir::West))
            EXPECT_EQ(ms, kStepMs);
        const Event *d = BeginDebug(extracted);
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(d->text, "RA calibration step 550 ms for Dec Unknown (Dec step 550 ms)\n");
    }
    // and so does a mount at Dec 0 (with the same noise, for the same reason)
    SimMountConfig c = ProfileMount(0.);
    c.noiseSigmaPx = 0.02;
    c.seed = 3;
    EXPECT_TRUE(SameCalls(Calibrate<LegacyTwinRun>(c), Calibrate<ExtractedRun>(c)));
}

TEST(HighDec, WrongDecReading)
{
    // a mount that reports 85 deg while it points at the equator: the RA step is
    // 4x too long, so each West pulse moves the star 8.5 px, which stays inside
    // the 15 px search region, and the calibration completes in 3 steps (Scope's
    // sanity check then raises its too-few-steps advisory)
    SimMountConfig c = ProfileMount(0.);
    c.reportedDecDeg = 85.;
    SimMount sim(c);
    CalOutcome o = RunCalibration<ExtractedRun>(sim, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_GE(o.measured.raStepCount, 3);
    for (int ms : o.Moves(Dir::West))
    {
        EXPECT_EQ(ms, 4 * kStepMs);
        EXPECT_LE(ms * sim.RaPxPerMs(), 9.0);
    }
    EXPECT_NEAR(o.measured.xRate, kRate, 0.02 * kRate);
    EXPECT_EQ(o.MoveTotal(Dir::East), o.MoveTotal(Dir::West));

    // the other way round, 0 reported at 75 deg, is today's calibration
    SimMountConfig low = ProfileMount(75.);
    low.reportedDecDeg = 0.;
    CalOutcome l = Calibrate<ExtractedRun>(low);
    ASSERT_EQ(l.outcome, Outcome::Complete);
    EXPECT_EQ(l.measured.raStepCount, 46);
    EXPECT_EQ(l.Moves(Dir::West), std::vector<int>(46, kStepMs));
}

TEST(HighDec, Pole)
{
    // at +-90 the RA step is 4x the step, finite; the star cannot move in RA, so
    // the West leg still ends in RaDidNotMove after 61 pulses
    for (int dec : { 90, -90 })
    {
        CalOutcome o = Calibrate<ExtractedRun>(ProfileMount(dec));
        SCOPED_TRACE("dec " + std::to_string(dec));
        ASSERT_EQ(o.outcome, Outcome::Failed);
        EXPECT_EQ(o.fail, Fail::RaDidNotMove);
        EXPECT_EQ(o.Moves(Dir::West), std::vector<int>(61, 4 * kStepMs));
    }
}

// ---------------------------------------------------------------------------
// Backlash clearing (#156)
// ---------------------------------------------------------------------------

namespace
{

// The Debug line backlash clearing starts with: the step it looks for
std::string LookingFor(const CalOutcome& o)
{
    for (const Event& e : o.events)
        if (e.kind == Event::Debug && e.text.rfind("Backlash: Looking for ", 0) == 0)
            return e.text;
    return std::string();
}

// The clearing moves accepted for a North pulse the Dec backlash took up
// whole: what such a move measured was centroid noise. The sim's Dec gear
// starts engaged for South, so the first backlashMs of North pulses move nothing.
int AcceptedWhileAbsorbed(const CalOutcome& o, double backlashMs)
{
    int northMs = 0; // North pulse time sent on earlier frames
    int lastPulseMs = 0;
    int accepted = 0;
    int frame = -2;
    for (const Event& e : o.events)
    {
        if (e.frame != frame)
        {
            frame = e.frame;
            northMs += lastPulseMs;
            lastPulseMs = 0;
        }
        if (e.kind == Event::Debug && e.text.rfind("Backlash: Accepted clearing move", 0) == 0 && northMs > 0 &&
            northMs <= backlashMs)
            accepted++;
        if (e.kind == Event::Move && e.dir == Dir::North)
            lastPulseMs = e.ms;
    }
    return accepted;
}

// A calibration run that also records how far the star's true position got
// from the lock on any frame
struct TrackedRun
{
    CalOutcome o;
    double furthestPx = 0.;

    double FinalPx() const { return std::hypot(o.finalTrue.x - o.lock.x, o.finalTrue.y - o.lock.y); }
};

TrackedRun CalibrateTracked(const SimMountConfig& c)
{
    SimMount sim(c);
    const Pt lock = sim.TruePosition();
    TrackedRun t;
    FrameHook<ExtractedRun> hook = [&t, lock](int, Settings&, SimMount& m, ExtractedRun&)
    {
        Pt p = m.TruePosition();
        t.furthestPx = std::max(t.furthestPx, std::hypot(p.x - lock.x, p.y - lock.y));
    };
    t.o = RunCalibration<ExtractedRun>(sim, ProfileSettings(), 2000, hook);
    t.furthestPx = std::max(t.furthestPx, t.FinalPx());
    return t;
}

} // namespace

TEST(BacklashClearingStepPx, IsTheDecLegsMoveForAKnownDec)
{
    // xRate is the RA image rate at the Dec, cos(Dec) times the equator's; a
    // Dec pulse moves the star as far at every Dec. A known Dec divides the
    // cos(Dec) back out: 0.6 of the Dec leg's move a step, 1.28 px here
    const double atEquator = kRate * kStepMs * 0.6;
    for (int dec : { 30, 45, 60, 75, 80, 85, 87, -80 })
    {
        double xRate = kRate * std::cos(Radians(dec));
        EXPECT_NEAR(cr::BacklashClearingStepPx(xRate, kStepMs, Radians(dec)), atEquator, 1e-12) << dec;
    }
    // Dec 0 is the RA-rate step exactly
    EXPECT_EQ(cr::BacklashClearingStepPx(kRate, kStepMs, 0.), atEquator);
}

TEST(BacklashClearingStepPx, AnUnknownOrNonFiniteDecKeepsTheRaRateStep)
{
    // the step before #156, bit for bit: what the twin computes
    const double xRate = kRate * std::cos(Radians(60.));
    const double before = xRate * kStepMs * 0.6;
    EXPECT_EQ(cr::BacklashClearingStepPx(xRate, kStepMs, std::nullopt), before);
    EXPECT_EQ(cr::BacklashClearingStepPx(xRate, kStepMs, std::nan("")), before);
    EXPECT_EQ(cr::BacklashClearingStepPx(xRate, kStepMs, INFINITY), before);
}

TEST(BacklashClearingStepPx, PoleIsFinite)
{
    // cos(+-90 deg) is about 6e-17: the floor, not the division, gives the step
    EXPECT_DOUBLE_EQ(cr::BACKLASH_MIN_COS_DEC, 0.01);
    const double xRate = 0.0001;
    for (double dec : { M_PI / 2, -M_PI / 2, Radians(89.9) })
    {
        double step = cr::BacklashClearingStepPx(xRate, kStepMs, dec);
        EXPECT_TRUE(std::isfinite(step));
        EXPECT_DOUBLE_EQ(step, xRate / cr::BACKLASH_MIN_COS_DEC * kStepMs * 0.6);
    }
    // cos(180 deg) = -1: magnitude 1
    EXPECT_DOUBLE_EQ(cr::BacklashClearingStepPx(xRate, kStepMs, M_PI), xRate * kStepMs * 0.6);
}

TEST(HighDecBacklash, LooksForTheDecLegsMoveAtEveryDec)
{
    // #156: before, clearing looked for 0.6 of the RA leg's move, 1.3 px at
    // the equator but 0.2 px at 80 deg. With the mount's Dec it looks for 0.6
    // of the Dec leg's move at every Dec; max(8, 60000 / 550) attempts
    for (int dec : { 0, 60, 80, 85, -80 })
    {
        CalOutcome o = Calibrate<ExtractedRun>(ProfileMount(dec));
        SCOPED_TRACE("dec " + std::to_string(dec));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_EQ(LookingFor(o), "Backlash: Looking for 3 moves of 1.3 px, max attempts = 109\n");
    }

    // the Dec guide rate half the RA rate: half the Dec move, as before
    SimMountConfig half = ProfileMount(80.);
    half.decGuideDegPerSec = half.raGuideDegPerSec / 2;
    half.decPxPerMs = kRate / 2;
    CalOutcome h = Calibrate<ExtractedRun>(half);
    ASSERT_EQ(h.outcome, Outcome::Complete);
    EXPECT_EQ(LookingFor(h), "Backlash: Looking for 3 moves of 0.6 px, max attempts = 109\n");

    // no Dec from the mount: the RA-rate step, as the twin looks for it
    SimMountConfig unknown = ProfileMount(60.);
    unknown.reportDec = false;
    CalOutcome legacy = Calibrate<LegacyTwinRun>(unknown);
    CalOutcome extracted = Calibrate<ExtractedRun>(unknown);
    ASSERT_EQ(extracted.outcome, Outcome::Complete);
    EXPECT_EQ(LookingFor(extracted), "Backlash: Looking for 3 moves of 0.6 px, max attempts = 109\n");
    EXPECT_EQ(LookingFor(extracted), LookingFor(legacy));
}

TEST(HighDecBacklash, ClearingAtDec80KeepsTheBacklashOutOfTheDecRate)
{
    // #156: Dec 80 with 2500 ms of Dec backlash, so the first four 550 ms
    // clearing pulses move nothing, and 0.2 px of centroid noise. Looking
    // for 0.2 px moves, clearing accepted noise between two frames as a move
    // and could end with backlash left, which the North leg then measured:
    // yRate up to 17 % low. Looking for the Dec leg's 1.3 px, it waits the
    // backlash out at every seed.
    for (int seed = 1; seed <= 20; seed++)
    {
        SimMountConfig c = ProfileMount(80.);
        c.decBacklashMs = 2500.;
        c.noiseSigmaPx = 0.2;
        c.seed = (uint64_t) seed;
        CalOutcome o = Calibrate<ExtractedRun>(c);
        SCOPED_TRACE("seed " + std::to_string(seed));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_TRUE(o.HasDebug("Backlash: Got 3 acceptable moves"));
        EXPECT_EQ(AcceptedWhileAbsorbed(o, c.decBacklashMs), 0);
        // backlash measured into the North leg makes yRate low; the noise alone moves it up to about 3.5 %
        EXPECT_GT(o.measured.yRate, 0.97 * kRate);
        EXPECT_LT(o.measured.yRate, 1.05 * kRate);

        // at Dec 0 the RA-rate step is the Dec leg's, and the twin waited it out too
        c.trueDecDeg = 0.;
        CalOutcome equator = Calibrate<LegacyTwinRun>(c);
        ASSERT_EQ(equator.outcome, Outcome::Complete);
        EXPECT_EQ(AcceptedWhileAbsorbed(equator, c.decBacklashMs), 0);
        EXPECT_GT(equator.measured.yRate, 0.97 * kRate);
        EXPECT_LT(equator.measured.yRate, 1.05 * kRate);
    }
}

TEST(HighDecBacklash, WrongDecReadingLooksForTooBigAMoveAndGoesOn)
{
    // a mount that reports 85 deg while it points at the equator: clearing
    // looks for 11.5x the Dec leg's move (14.7 px), accepts none, and goes on
    // once the star has moved the calibration distance, as for a mount whose
    // moves are all too small; the calibration completes with the true Dec rate
    SimMountConfig c = ProfileMount(0.);
    c.reportedDecDeg = 85.;
    c.decBacklashMs = 1500.;
    TrackedRun t = CalibrateTracked(c);
    const CalOutcome& o = t.o;
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_EQ(LookingFor(o), "Backlash: Looking for 3 moves of 14.7 px, max attempts = 109\n");
    EXPECT_FALSE(o.HasDebug("Backlash: Accepted clearing move"));
    EXPECT_TRUE(o.HasDebug("Backlash: Reached clearing limit but total displacement > 3px - proceeding"));
    EXPECT_NEAR(o.measured.yRate, kRate, 0.02 * kRate);

    // What that costs. With the right reading clearing goes on after 3 moves
    // of the 2.1 px a pulse makes, the North leg goes the calibration distance,
    // the South leg sends back the North leg's pulses and the nudge brings the
    // star to about 2 px from the lock. With a reading whose cos(Dec) is under
    // 0.6 of the true one (55 deg at the equator already), clearing uses up the
    // calibration distance before the North leg starts: the star goes about
    // twice the calibration distance North, more than #140's edge margin
    // (calibration distance plus search region), and since the South leg only
    // sends back the North leg and the nudge is at most 4 pulses of a step, it
    // ends about one calibration distance from the lock. The calibration still
    // completes with the true rates; the guider then guides the star back.
    SimMountConfig right = c;
    right.reportedDecDeg = 0.;
    TrackedRun r = CalibrateTracked(right);
    ASSERT_EQ(r.o.outcome, Outcome::Complete);
    EXPECT_EQ(r.o.MoveTotal(Dir::North), 9350);
    EXPECT_EQ(r.o.MoveTotal(Dir::South), 8800);
    EXPECT_LT(r.FinalPx(), 2.5);
    EXPECT_NEAR(r.furthestPx, 30.4, 0.5);
    EXPECT_LT(r.furthestPx, 1.5 * kDistancePx);

    for (int reported : { 55, 60, 85, 90 })
    {
        SimMountConfig w = c;
        w.reportedDecDeg = reported;
        TrackedRun wt = CalibrateTracked(w);
        SCOPED_TRACE("reported dec " + std::to_string(reported));
        ASSERT_EQ(wt.o.outcome, Outcome::Complete);
        EXPECT_NEAR(wt.o.measured.yRate, kRate, 0.02 * kRate);
        EXPECT_FALSE(wt.o.HasDebug("Backlash: Accepted clearing move"));
        EXPECT_EQ(wt.o.MoveTotal(Dir::North), 14850);
        EXPECT_EQ(wt.o.MoveTotal(Dir::South), 8800);
        EXPECT_NEAR(wt.FinalPx(), 23.5, 0.5);
        EXPECT_GT(wt.FinalPx(), 0.9 * kDistancePx);
        EXPECT_NEAR(wt.furthestPx, 51.8, 0.5);
        EXPECT_GT(wt.furthestPx, 2 * kDistancePx);
    }
}
