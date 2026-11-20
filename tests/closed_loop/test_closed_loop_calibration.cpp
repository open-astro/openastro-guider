// Closed-loop characterization of Scope's calibration state machine (#142).
//
// A SimMount (sim_mount.h) stands in for the mount and the guide camera, and
// the harness (harness.h) drives the calibration frame by frame, the way the
// guider does. Every Calibration test is a typed test that runs twice: on the
// frozen legacy twin of Scope::BeginCalibration / UpdateCalibrationState
// (legacy/), and on the extracted calibration_run::CalibrationRun driven as
// Scope drives it (extracted_run.h). They pin what the calibration does,
// quirks included, and hold for both, so the extraction changed none of it;
// test_calibration_run_parity.cpp compares the two effect by effect. The
// behaviours the run has changed since are the RA step sized for the Dec
// (#141): at Dec 0 it is the step, the high-Dec tests here leave the Dec
// unknown, and test_high_dec_calibration.cpp covers a known Dec; and a
// DEC_NONE calibration that fails when the mode was switched to one that
// guides Dec before it completed (#148), where the twin kept completing on
// every frame. The LegacyTwin tests pin the twin's old behaviour of each.
//
// Every test starts a fresh calibration with Begin: nothing here restores,
// reuses or skips a calibration.
//
// The "today" profile is the audit's: FL 400 mm, 3.75 um pixels, bin 1, 0.5x
// sidereal guide rate, so 1.934"/px and 0.003878 px/ms on both axes at Dec 0.
// The calibration step calculator gives a 550 ms step and a 25 px distance
// (12 RA steps at Dec 0).

#include <gtest/gtest.h>

#include "extracted_run.h"
#include "harness.h"
#include "legacy/legacy_twin.h"
#include "nudge_cosine.h"
#include "sim_mount.h"

#include <cmath>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace closed_loop;

namespace
{

constexpr double kRate = 0.003878; // px/ms at Dec 0, the audit profile
constexpr int kStepMs = 550;
constexpr int kDistancePx = 25;

SimMountConfig ProfileMount()
{
    SimMountConfig c;
    c.raPxPerMsAtEquator = kRate;
    c.decPxPerMs = kRate;
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
CalOutcome Calibrate(const SimMountConfig& cfg, const Settings& s, int maxFrames = 2000, const FrameHook<Run>& hook = nullptr)
{
    SimMount sim(cfg);
    return RunCalibration<Run>(sim, s, maxFrames, hook);
}

double AngleErrDeg(double a, double b)
{
    return std::fabs(Degrees(NormAngle(a - b)));
}

double Dist(const Pt& a, const Pt& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

int LastMoveFrame(const CalOutcome& o)
{
    int f = -1;
    for (const Event& e : o.events)
        if (e.kind == Event::Move)
            f = e.frame;
    return f;
}

int FirstDebugFrame(const CalOutcome& o, const std::string& fragment)
{
    for (const Event& e : o.events)
        if (e.kind == Event::Debug && e.text.find(fragment) != std::string::npos)
            return e.frame;
    return -1;
}

// Whether the first Debug line holding fragment prints a NaN right after it.
// printf shows the NaN's sign, and 0/0 gives -nan on x86-64 but nan on arm64.
bool DebugShowsNanAfter(const CalOutcome& o, const std::string& fragment)
{
    for (const Event& e : o.events)
    {
        if (e.kind != Event::Debug)
            continue;
        std::string::size_type at = e.text.find(fragment);
        if (at == std::string::npos)
            continue;
        std::string rest = e.text.substr(at + fragment.size());
        return rest.rfind("nan", 0) == 0 || rest.rfind("-nan", 0) == 0;
    }
    return false;
}

// The South nudge compares the nudge with the North leg through acos of their
// direction cosines. In a noiseless run the two are exactly antiparallel, so
// the cosine sits on -1 and rounding (FMA contraction included) decides
// whether acos returns pi or NaN. Tests of the nudge add a little noise.
SimMountConfig WithLightNoise(SimMountConfig c)
{
    c.noiseSigmaPx = 0.02;
    c.seed = 3;
    return c;
}

bool StartsWith(const std::string& s, const std::string& prefix)
{
    return s.rfind(prefix, 0) == 0;
}

// The South pulses the nudge sent, in order
std::vector<int> NudgePulses(const CalOutcome& o)
{
    std::vector<int> v;
    bool nudging = false;
    for (const Event& e : o.events)
    {
        if (e.kind == Event::Status)
            nudging = StartsWith(e.text, "Nudge South");
        else if (e.kind == Event::Move && nudging)
        {
            EXPECT_EQ(e.dir, Dir::South);
            v.push_back(e.ms);
            nudging = false;
        }
    }
    return v;
}

// The two implementations every Calibration test runs on
template<class Run>
class Calibration : public ::testing::Test
{
};

struct ImplementationName
{
    template<class T>
    static std::string GetName(int)
    {
        return std::is_same_v<T, LegacyTwinRun> ? "LegacyTwin" : "CalibrationRun";
    }
};

using Implementations = ::testing::Types<LegacyTwinRun, ExtractedRun>;
TYPED_TEST_SUITE(Calibration, Implementations, ImplementationName);

} // namespace

// ---------------------------------------------------------------------------
// SimMount self-tests
// ---------------------------------------------------------------------------

TEST(SimMount, WestMovesAlongWestAngleScaledByCosDec)
{
    SimMountConfig c;
    c.westAngleDeg = 30.;
    c.raPxPerMsAtEquator = 0.01;
    c.trueDecDeg = 60.;
    SimMount sim(c);
    Pt a = sim.Observe();
    sim.Pulse(Dir::West, 1000);
    Pt b = sim.Observe();
    EXPECT_NEAR(Dist(a, b), 5.0, 1e-9); // 0.01 px/ms * cos 60 * 1000 ms
    EXPECT_NEAR(Degrees(std::atan2(b.y - a.y, b.x - a.x)), 30., 1e-9);
    EXPECT_NEAR(sim.RaPxPerMs(), 0.005, 1e-12);
    sim.Pulse(Dir::East, 1000);
    EXPECT_NEAR(Dist(a, sim.Observe()), 0., 1e-9);
    // PHD's xAngle is start.Angle(cur), the reverse of the motion
    EXPECT_NEAR(Degrees(sim.ExpectedXAngle()), -150., 1e-9);
}

TEST(SimMount, NorthIsWestMinus90OrMirroredPlus90WithNonOrthogonality)
{
    for (int mirrored = 0; mirrored < 2; mirrored++)
    {
        SimMountConfig c;
        c.westAngleDeg = 30.;
        c.decMirrored = mirrored != 0;
        c.nonOrthoDeg = 5.;
        c.decPxPerMs = 0.01;
        SimMount sim(c);
        Pt a = sim.Observe();
        sim.Pulse(Dir::North, 1000);
        Pt b = sim.Observe();
        double expected = mirrored ? 30. + 90. + 5. : 30. - 90. + 5.;
        EXPECT_NEAR(Dist(a, b), 10., 1e-9);
        EXPECT_NEAR(Degrees(std::atan2(b.y - a.y, b.x - a.x)), expected, 1e-9) << "mirrored " << mirrored;
        EXPECT_NEAR(Degrees(sim.ExpectedYAngle()), expected, 1e-9);
    }
}

TEST(SimMount, DecBacklashIsDeadTimeOnlyOnReversal)
{
    SimMountConfig c;
    c.westAngleDeg = 0.; // North moves the star toward -y
    c.decPxPerMs = 0.01;
    c.decBacklashMs = 300.;
    c.decEngagedSouth = true;
    SimMount sim(c);
    double y0 = sim.Observe().y;
    sim.Pulse(Dir::North, 500); // reversal: 300 ms lost
    EXPECT_NEAR(y0 - sim.Observe().y, 2.0, 1e-9);
    sim.Pulse(Dir::North, 500); // same direction: all of it
    EXPECT_NEAR(y0 - sim.Observe().y, 7.0, 1e-9);
    sim.Pulse(Dir::South, 200); // reversal: all 200 ms lost, 100 ms of slack left
    EXPECT_NEAR(y0 - sim.Observe().y, 7.0, 1e-9);
    sim.Pulse(Dir::South, 500);
    EXPECT_NEAR(y0 - sim.Observe().y, 3.0, 1e-9);
    // RA has no backlash
    sim.Pulse(Dir::West, 100);
    sim.Pulse(Dir::East, 100);
    EXPECT_NEAR(y0 - sim.Observe().y, 3.0, 1e-9);
}

TEST(SimMount, SameSeedGivesTheSameNoise)
{
    SimMountConfig c;
    c.noiseSigmaPx = 0.3;
    c.seed = 42;
    SimMount a(c), b(c);
    c.seed = 43;
    SimMount d(c);
    bool differs = false;
    double sum2 = 0.;
    for (int i = 0; i < 2000; i++)
    {
        Pt pa = a.Observe(), pb = b.Observe(), pd = d.Observe();
        EXPECT_EQ(pa.x, pb.x);
        EXPECT_EQ(pa.y, pb.y);
        differs = differs || pa.x != pd.x;
        sum2 += (pa.x - c.startX) * (pa.x - c.startX);
    }
    EXPECT_TRUE(differs);
    EXPECT_NEAR(std::sqrt(sum2 / 2000.), 0.3, 0.03);
}

TEST(SimMount, ProbesReportCoordinatesWithParityAndAreCounted)
{
    SimMountConfig c;
    c.trueDecDeg = 40.;
    for (int odd = 0; odd < 2; odd++)
    {
        c.raParityOdd = odd != 0;
        c.decParityOdd = odd != 0;
        SimMount sim(c);
        double ra0, dec0, ra1, dec1;
        ASSERT_TRUE(sim.Coordinates(&ra0, &dec0));
        EXPECT_DOUBLE_EQ(ra0, 6.);
        EXPECT_DOUBLE_EQ(dec0, 40.);
        sim.Pulse(Dir::West, 4000);
        sim.Pulse(Dir::North, 4000);
        ASSERT_TRUE(sim.Coordinates(&ra1, &dec1));
        // 0.5x sidereal for 4 s: 30" of RA axis = 2 s of time; 30" of Dec
        EXPECT_NEAR((ra1 - ra0) * 3600., odd ? 2.0 : -2.0, 1e-9);
        EXPECT_NEAR((dec1 - dec0) * 3600., odd ? -30.0 : 30.0, 1e-9);
        EXPECT_EQ(sim.probeCalls[(int) ProbeKind::Coordinates], 2);
    }

    c.coordinates = false;
    c.guideRates = false;
    c.reportDec = false;
    c.canPulseGuide = false;
    SimMount sim(c);
    double a, b;
    EXPECT_FALSE(sim.Coordinates(&a, &b));
    EXPECT_FALSE(sim.GuideRates(&a, &b));
    EXPECT_FALSE(sim.CanPulseGuide());
    EXPECT_FALSE(sim.ReportedDecRad().has_value());
    EXPECT_EQ(sim.ProbeLog(),
              (std::vector<ProbeKind> { ProbeKind::Coordinates, ProbeKind::GuideRates, ProbeKind::CanPulseGuide }));
}

TEST(SimMount, PierFlipTurnsWestAndMirrorsDec)
{
    SimMountConfig c;
    c.westAngleDeg = 30.;
    SimMount sim(c);
    sim.FlipPier();
    EXPECT_NEAR(Degrees(sim.ExpectedXAngle()), 30., 1e-9);
    EXPECT_TRUE(sim.Config().decMirrored);
    EXPECT_NEAR(Degrees(sim.ExpectedYAngle()), -60., 1e-9); // 210 + 90
}

TEST(SimMount, PierFlipReversesDecParityOnlyWhenItMirrorsDec)
{
    // a flip that keeps North's image direction leaves North moving the scope
    // south on the sky (Mount::FlipCalibration without decFlipRequired)
    for (int mirrors = 0; mirrors < 2; ++mirrors)
    {
        SimMountConfig c;
        c.flipMirrorsDec = mirrors != 0;
        SimMount sim(c);
        sim.FlipPier();
        double ra0, dec0, ra1, dec1;
        ASSERT_TRUE(sim.Coordinates(&ra0, &dec0));
        sim.Pulse(Dir::North, 1000);
        sim.Pulse(Dir::West, 1000);
        ASSERT_TRUE(sim.Coordinates(&ra1, &dec1));
        SCOPED_TRACE("flipMirrorsDec " + std::to_string(mirrors));
        EXPECT_EQ(dec1 < dec0, mirrors != 0);
        EXPECT_LT(ra1, ra0); // RA parity never changes
        EXPECT_EQ(sim.Config().decParityOdd, mirrors != 0);
    }
}

// ---------------------------------------------------------------------------
// Calibration geometry (audit cal-state-machine a, b)
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, GeometryOverCameraAnglesNormalAndMirrored)
{
    const double angles[] = { 0., 30., 90., 200., -135. };
    for (double angle : angles)
    {
        for (int mirrored = 0; mirrored < 2; mirrored++)
        {
            SimMountConfig c = ProfileMount();
            c.westAngleDeg = angle;
            c.decMirrored = mirrored != 0;
            SimMount sim(c);
            CalOutcome o = RunCalibration<TypeParam>(sim, ProfileSettings());
            SCOPED_TRACE("west " + std::to_string(angle) + (mirrored ? " mirrored" : ""));
            ASSERT_EQ(o.outcome, Outcome::Complete);
            const Measured& m = o.measured;
            EXPECT_LT(AngleErrDeg(m.xAngle, sim.ExpectedXAngle()), 0.5);
            EXPECT_LT(AngleErrDeg(m.yAngle, sim.ExpectedYAngle()), 0.5);
            EXPECT_NEAR(m.xRate, kRate, 0.02 * kRate);
            EXPECT_NEAR(m.yRate, kRate, 0.02 * kRate);
            // Mount::SetCalibration's yAngleError: 0 normal, 180 mirrored
            double yAngleError = Degrees(NormAngle(m.xAngle - m.yAngle + M_PI / 2));
            if (mirrored)
            {
                EXPECT_NEAR(std::fabs(yAngleError), 180., 0.5);
            }
            else
            {
                EXPECT_NEAR(yAngleError, 0., 0.5);
            }
            EXPECT_EQ(m.raStepCount, 12);
            EXPECT_EQ(m.decStepCount, 12);
            EXPECT_EQ(m.raParity, Parity::Even);
            EXPECT_EQ(m.decParity, Parity::Even);
        }
    }
}

TYPED_TEST(Calibration, StepCountIsDistanceOverStep)
{
    // audit case (a): theta 30, both rates 0.02 px/ms, 100 ms steps: 2 px a step
    SimMountConfig c;
    c.westAngleDeg = 30.;
    c.raPxPerMsAtEquator = 0.02;
    c.decPxPerMs = 0.02;
    Settings s;
    s.stepMs = 100;
    s.distancePx = 25;
    CalOutcome o = Calibrate<TypeParam>(c, s);
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_EQ(o.measured.raStepCount, 13); // ceil(25 / 2)
    EXPECT_EQ(o.Moves(Dir::West).size(), 13u);
    // the last of the 3 backlash-clearing pulses is North step 1
    EXPECT_EQ(o.measured.decStepCount, 13);
    EXPECT_EQ(o.Moves(Dir::North).size(), 3u + 12u);
    EXPECT_NEAR(o.measured.xRate, 0.02, 1e-12);
    EXPECT_NEAR(o.measured.yRate, 0.02, 1e-12);
    EXPECT_NEAR(Degrees(o.measured.xAngle), -150., 1e-9);
    EXPECT_NEAR(Degrees(o.measured.yAngle), -60., 1e-9);
    // one West and one North complete row, one completion, no alert
    EXPECT_EQ(o.Of(Event::DirectComplete).size(), 2u);
    EXPECT_EQ(o.Of(Event::DirectComplete)[0].text, "West");
    EXPECT_EQ(o.Of(Event::DirectComplete)[1].text, "North");
    EXPECT_EQ(o.Count(Event::LogComplete), 1);
    EXPECT_EQ(o.Count(Event::NotifyComplete), 1);
    EXPECT_EQ(o.Count(Event::Alert), 0);
}

TYPED_TEST(Calibration, NoisyCentroidsStayWithinTolerance)
{
    // 0.1 px of noise on each end of a 25 px leg: about 0.3 deg of angle error at 1 sigma
    const double angles[] = { 0., 30., 90., 200., -135. };
    for (int seed = 1; seed <= 5; seed++)
    {
        SimMountConfig c = ProfileMount();
        c.westAngleDeg = angles[seed - 1];
        c.decMirrored = seed % 2 == 0;
        c.noiseSigmaPx = 0.1;
        c.seed = (uint64_t) seed;
        SimMount sim(c);
        CalOutcome o = RunCalibration<TypeParam>(sim, ProfileSettings());
        SCOPED_TRACE("seed " + std::to_string(seed));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_LT(AngleErrDeg(o.measured.xAngle, sim.ExpectedXAngle()), 1.0);
        EXPECT_LT(AngleErrDeg(o.measured.yAngle, sim.ExpectedYAngle()), 1.0);
        EXPECT_NEAR(o.measured.xRate, kRate, 0.02 * kRate);
        EXPECT_NEAR(o.measured.yRate, kRate, 0.02 * kRate);
    }
}

// ---------------------------------------------------------------------------
// The 61-pulse limits (audit c)
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, RaThatNeverMovesFailsAfter61WestPulses)
{
    SimMountConfig c = ProfileMount();
    c.raPxPerMsAtEquator = 0.;
    CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Failed);
    EXPECT_EQ(o.fail, Fail::RaDidNotMove);
    // `m_calibrationSteps++ > MAX_CALIBRATION_STEPS`: 61 pulses, the 62nd frame fails
    EXPECT_EQ(o.Moves(Dir::West).size(), 61u);
    EXPECT_EQ(o.frames, 62);
    EXPECT_EQ(o.Count(Event::Move), 61);
    ASSERT_EQ(o.Count(Event::Alert), 1);
    EXPECT_EQ(o.Of(Event::Alert)[0].text, "RA Calibration Failed: star did not move enough");
    ASSERT_EQ(o.Count(Event::LogFailed), 1);
    EXPECT_EQ(o.Of(Event::LogFailed)[0].text, "RA Calibration Failed: star did not move enough");
    ASSERT_EQ(o.Count(Event::NotifyFailed), 1);
    EXPECT_EQ(o.Of(Event::NotifyFailed)[0].text, "RA Calibration Failed: star did not move enough");
    EXPECT_EQ(o.Of(Event::LogStep).back().step, 61);
}

TYPED_TEST(Calibration, DecThatBarelyMovesFailsAfter61NorthSteps)
{
    // 0.4 px a North pulse: the backlash clearing accepts it (Dec guide rate a
    // tenth of RA's shrinks the expected step to 0.12 px), but 61 steps give 24.4 px
    SimMountConfig c;
    c.raPxPerMsAtEquator = 0.02;
    c.decPxPerMs = 0.004;
    c.decGuideDegPerSec = c.raGuideDegPerSec / 10.;
    Settings s;
    s.stepMs = 100;
    s.distancePx = 25;
    CalOutcome o = Calibrate<TypeParam>(c, s);
    ASSERT_EQ(o.outcome, Outcome::Failed);
    EXPECT_EQ(o.fail, Fail::DecDidNotMove);
    // 3 clearing pulses (the last is North step 1), then steps 2..61
    EXPECT_EQ(o.Moves(Dir::North).size(), 3u + 60u);
    EXPECT_EQ(o.measured.decStepCount, 0); // set only when the North leg completes
    ASSERT_EQ(o.Count(Event::Alert), 1);
    EXPECT_EQ(o.Of(Event::Alert)[0].text, "DEC Calibration Failed: star did not move enough");
    EXPECT_EQ(o.Of(Event::LogFailed)[0].text, "DEC Calibration Failed: star did not move enough");
    EXPECT_EQ(o.Of(Event::NotifyFailed)[0].text, "DEC Calibration Failed: star did not move enough");
    const Event last = o.Of(Event::LogStep).back();
    EXPECT_EQ(last.text, "North");
    EXPECT_EQ(last.step, 61);
    EXPECT_NEAR(last.dist, 24.4, 1e-9);
}

// ---------------------------------------------------------------------------
// Dec backlash (audit d, e)
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, BacklashIsClearedBeforeTheDecRateIsMeasured)
{
    for (double backlashMs : { 0., 600., 2000., 3500. })
    {
        SimMountConfig c = ProfileMount();
        c.decBacklashMs = backlashMs;
        CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
        SCOPED_TRACE("backlash " + std::to_string(backlashMs));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_NEAR(o.measured.xRate, kRate, 0.02 * kRate);
        EXPECT_NEAR(o.measured.yRate, kRate, 0.05 * kRate);

        // the last accepted clearing move is logged as North step 1: the step-0
        // row at the clearing marker, then step 1 one pulse further, same frame
        std::vector<Event> rows = o.Of(Event::LogStep);
        size_t i = 0;
        while (i < rows.size() && !(rows[i].text == "North" && rows[i].step == 0))
            i++;
        ASSERT_LT(i + 1, rows.size());
        EXPECT_EQ(rows[i].dist, 0.);
        EXPECT_EQ(rows[i + 1].text, "North");
        EXPECT_EQ(rows[i + 1].step, 1);
        EXPECT_EQ(rows[i + 1].frame, rows[i].frame);
        EXPECT_NEAR(rows[i + 1].dist, kRate * kStepMs, 1e-9);
        EXPECT_EQ(rows[i - 1].text, "Backlash");
    }
}

TYPED_TEST(Calibration, BacklashClearingFailsAfterMaxClearingPulses)
{
    // max(8, 60000 / step) North pulses without the star moving
    const int cases[][2] = { { 550, 109 }, { 750, 80 }, { 10000, 8 } };
    for (const auto& tc : cases)
    {
        SimMountConfig c = ProfileMount();
        c.decPxPerMs = 0.;
        Settings s = ProfileSettings();
        s.stepMs = tc[0];
        CalOutcome o = Calibrate<TypeParam>(c, s);
        SCOPED_TRACE("step " + std::to_string(tc[0]));
        ASSERT_EQ(o.outcome, Outcome::Failed);
        EXPECT_EQ(o.fail, Fail::BacklashClearingFailed);
        EXPECT_EQ((int) o.Moves(Dir::North).size(), tc[1]);
        ASSERT_EQ(o.Count(Event::Alert), 1);
        EXPECT_EQ(o.Of(Event::Alert)[0].text, "Backlash Clearing Failed: star did not move enough");
        EXPECT_EQ(o.Of(Event::NotifyFailed)[0].text, "Backlash Clearing Failed: star did not move enough");
    }
}

TYPED_TEST(Calibration, BacklashClearingProceedsWhenTheStarMovedThreePixels)
{
    // 1.1 px a pulse is under the 1.28 px expected step, so no move is accepted;
    // the clearing stops at 25 px and, having moved 3 px or more, the North leg
    // starts over from there
    SimMountConfig c = ProfileMount();
    c.decPxPerMs = 0.002;
    CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_TRUE(o.HasDebug("Backlash: Reached clearing limit but total displacement > 3px"));
    EXPECT_FALSE(o.HasDebug("Backlash: Accepted clearing move"));
    EXPECT_EQ(o.measured.decStepCount, 23);
    EXPECT_EQ(o.Moves(Dir::North).size(), 23u + 23u);
    EXPECT_NEAR(o.measured.yRate, 0.002, 1e-9);
}

TYPED_TEST(Calibration, BacklashClearingProceedsThenDecFailsWhenItMovedTooLittle)
{
    // 0.055 px a pulse: 6 px after 109 clearing pulses, enough to go on, too little to calibrate
    SimMountConfig c = ProfileMount();
    c.decPxPerMs = 0.0001;
    CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Failed);
    EXPECT_EQ(o.fail, Fail::DecDidNotMove);
    EXPECT_TRUE(o.HasDebug("Backlash: Reached clearing limit but total displacement > 3px"));
    EXPECT_EQ(o.Moves(Dir::North).size(), 109u + 61u);
}

// ---------------------------------------------------------------------------
// DEC_NONE (audit f) and the recentering legs (audit g)
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, DecNoneSkipsTheDecLegAndCompletesOnTheNextFrame)
{
    Settings s = ProfileSettings();
    s.decMode = DecMode::None;
    CalOutcome o = Calibrate<TypeParam>(ProfileMount(), s);
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_TRUE(o.Moves(Dir::North).empty());
    EXPECT_TRUE(o.Moves(Dir::South).empty());
    EXPECT_EQ(o.measured.yRate, RATE_UNCALIBRATED);
    EXPECT_EQ(o.measured.yAngle, NormAngle(o.measured.xAngle + M_PI / 2.));
    EXPECT_EQ(o.measured.decParity, Parity::Unknown);
    EXPECT_EQ(o.measured.decStepCount, 0);
    EXPECT_TRUE(o.HasDebug("Skipping Dec calibration as DecGuideMode == NONE"));

    // the frame after the last East pulse sets COMPLETE and moves nothing; the
    // calibration completes on the frame after that
    int lastEast = LastMoveFrame(o);
    EXPECT_EQ(o.frames, lastEast + 3);
    EXPECT_EQ(FirstDebugFrame(o, "Skipping Dec calibration"), lastEast + 1);
    ASSERT_EQ(o.Count(Event::LogComplete), 1);
    EXPECT_EQ(o.Of(Event::LogComplete)[0].frame, lastEast + 2);
    // DEC_NONE throughout stores the RA-only calibration once and does not fail (#148)
    EXPECT_EQ(o.Count(Event::SetCalibration), 1);
    EXPECT_EQ(o.Count(Event::NotifyComplete), 1);
    EXPECT_EQ(o.Count(Event::LogFailed), 0);
}

TYPED_TEST(Calibration, DecModeNoneAfterTheDecLegStillCompletes)
{
    // switching Auto to None once the Dec leg has run, during the North leg or
    // on the frame the run completes, keeps the measured Dec rate and completes
    // once; DEC_NONE does not need the Dec rate, and it is there anyway
    CalOutcome base = Calibrate<TypeParam>(WithLightNoise(ProfileMount()), ProfileSettings());
    ASSERT_EQ(base.outcome, Outcome::Complete);
    ASSERT_NE(base.measured.yRate, RATE_UNCALIBRATED);
    int firstNorth = -1;
    for (const Event& e : base.events)
        if (firstNorth < 0 && e.kind == Event::LogStep && e.text == "North")
            firstNorth = e.frame;
    ASSERT_GE(firstNorth, 0);

    for (int at : { firstNorth + 1, base.frames - 1 })
    {
        FrameHook<TypeParam> toNone = [at](int frame, Settings& st, SimMount&, TypeParam&)
        {
            if (frame == at)
                st.decMode = DecMode::None;
        };
        CalOutcome o = Calibrate<TypeParam>(WithLightNoise(ProfileMount()), ProfileSettings(), 2000, toNone);
        SCOPED_TRACE("None before frame " + std::to_string(at));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_EQ(o.frames, base.frames);
        EXPECT_EQ(o.measured.yRate, base.measured.yRate);
        EXPECT_EQ(o.Count(Event::SetCalibration), 1);
        EXPECT_EQ(o.Count(Event::LogComplete), 1);
        EXPECT_EQ(o.Count(Event::NotifyComplete), 1);
        EXPECT_EQ(o.Count(Event::LogFailed), 0);
        EXPECT_EQ(o.Count(Event::NotifyFailed), 0);
        EXPECT_FALSE(o.HasDebug("Skipping Dec calibration"));
    }
}

TEST(LegacyTwin, DecModeAutoAfterDecNoneSkipReenteredCompleteEveryFrame)
{
    // #148, before the fix: switching DEC_NONE to Auto after the Dec leg was
    // skipped left yRate uncalibrated, so IsCalibrated() stayed false and
    // COMPLETE ran again on every frame, storing and reporting the RA-only
    // calibration each time. The extracted run now fails instead
    // (CalibrationRunDecMode below).
    Settings s = ProfileSettings();
    s.decMode = DecMode::None;
    CalOutcome base = Calibrate<LegacyTwinRun>(ProfileMount(), s);
    ASSERT_EQ(base.outcome, Outcome::Complete);
    const int completeFrame = base.frames - 1;

    FrameHook<LegacyTwinRun> toAuto = [&](int frame, Settings& st, SimMount&, LegacyTwinRun&)
    {
        if (frame == completeFrame)
            st.decMode = DecMode::Auto;
    };
    CalOutcome o = Calibrate<LegacyTwinRun>(ProfileMount(), s, completeFrame + 10, toAuto);
    EXPECT_EQ(o.outcome, Outcome::Continue);
    EXPECT_EQ(o.Count(Event::LogComplete), 10);
    EXPECT_EQ(o.Count(Event::NotifyComplete), 10);
    EXPECT_EQ(o.Count(Event::SetCalibration), 10);
    EXPECT_EQ(LastMoveFrame(o), LastMoveFrame(base));
}

TEST(CalibrationRunDecMode, NoneToAutoNorthOrSouthAtTheCompleteFrameFailsOnce)
{
    // #148: a calibration that skipped the Dec leg for DEC_NONE, with the mode
    // switched to one that guides Dec before the frame it completes on, has
    // no Dec rate for that mode. It fails once on that frame, with an alert
    // and one CalibrationFailed, and stores and reports nothing.
    Settings s = ProfileSettings();
    s.decMode = DecMode::None;
    CalOutcome base = Calibrate<ExtractedRun>(ProfileMount(), s);
    ASSERT_EQ(base.outcome, Outcome::Complete);
    const int completeFrame = base.frames - 1;
    ASSERT_EQ(FirstDebugFrame(base, "Skipping Dec calibration"), completeFrame - 1);

    const std::string msg = "Calibration failed: the Dec guide mode was changed from Off during calibration";
    for (DecMode to : { DecMode::Auto, DecMode::North, DecMode::South })
    {
        FrameHook<ExtractedRun> change = [&](int frame, Settings& st, SimMount&, ExtractedRun&)
        {
            if (frame == completeFrame)
                st.decMode = to;
        };
        CalOutcome o = Calibrate<ExtractedRun>(ProfileMount(), s, completeFrame + 10, change);
        SCOPED_TRACE("mode " + std::to_string((int) to));
        ASSERT_EQ(o.outcome, Outcome::Failed);
        EXPECT_EQ(o.fail, Fail::DecModeChanged);
        EXPECT_EQ(o.frames, completeFrame + 1);

        ASSERT_EQ(o.Count(Event::Alert), 1);
        EXPECT_EQ(o.Of(Event::Alert)[0].text, msg);
        EXPECT_EQ(o.Of(Event::Alert)[0].frame, completeFrame);
        ASSERT_EQ(o.Count(Event::LogFailed), 1);
        EXPECT_EQ(o.Of(Event::LogFailed)[0].text, msg);
        EXPECT_EQ(o.Of(Event::LogFailed)[0].frame, completeFrame);
        ASSERT_EQ(o.Count(Event::NotifyFailed), 1);
        EXPECT_EQ(o.Of(Event::NotifyFailed)[0].text, msg);
        EXPECT_EQ(o.Count(Event::SetCalibration), 0);
        EXPECT_EQ(o.Count(Event::SanityCheck), 0);
        EXPECT_EQ(o.Count(Event::LogComplete), 0);
        EXPECT_EQ(o.Count(Event::NotifyComplete), 0);

        // the failing frame does nothing but report the failure: no step, no move, no mount read
        for (const Event& e : o.events)
        {
            if (e.frame == completeFrame)
            {
                EXPECT_TRUE(e.kind == Event::Alert || e.kind == Event::LogFailed || e.kind == Event::NotifyFailed)
                    << "kind " << e.kind;
            }
        }
        // everything before it is the DEC_NONE calibration's
        EXPECT_EQ(LastMoveFrame(o), LastMoveFrame(base));
        EXPECT_TRUE(o.Moves(Dir::North).empty());
        EXPECT_TRUE(o.Moves(Dir::South).empty());
    }
}

TYPED_TEST(Calibration, EastAndSouthRecenterSendBackWhatWestAndNorthSent)
{
    struct Case
    {
        bool fast;
        int maxMovePx;
        int maxRaMs;
        int maxDecMs;
    };
    const Case cases[] = {
        { true, 15, 2500, 2500 }, // floor(15 / xRate) = 3867, clamped to 2500
        { true, 15, 10000, 10000 }, // 3867 East, floor(0.8 * 15 / yRate) = 3094 South
        { true, 1, 2500, 2500 }, // 257 ms is under the step: one step a pulse
        { false, 15, 2500, 2500 },
    };
    for (const Case& tc : cases)
    {
        Settings s = ProfileSettings();
        s.fastRecenter = tc.fast;
        s.maxMovePx = tc.maxMovePx;
        s.maxRaMs = tc.maxRaMs;
        s.maxDecMs = tc.maxDecMs;
        CalOutcome o = Calibrate<TypeParam>(WithLightNoise(ProfileMount()), s);
        SCOPED_TRACE("fast " + std::to_string(tc.fast) + " maxMove " + std::to_string(tc.maxMovePx) + " maxRa " +
                     std::to_string(tc.maxRaMs));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        const Measured& m = o.measured;

        EXPECT_EQ(o.MoveTotal(Dir::East), o.MoveTotal(Dir::West));
        EXPECT_EQ(o.MoveTotal(Dir::West), m.raStepCount * kStepMs);
        int eastPulse = kStepMs;
        int southPulse = kStepMs;
        if (tc.fast)
        {
            eastPulse = std::max(std::min((int) floor((double) tc.maxMovePx / m.xRate), tc.maxRaMs), kStepMs);
            southPulse = std::max(std::min((int) floor(0.8 * (double) tc.maxMovePx / m.yRate), tc.maxDecMs), kStepMs);
        }
        std::vector<int> east = o.Moves(Dir::East);
        int westTotal = m.raStepCount * kStepMs;
        ASSERT_EQ((int) east.size(), DIV_ROUND_UP(westTotal, eastPulse));
        for (size_t i = 0; i + 1 < east.size(); i++)
            EXPECT_EQ(east[i], eastPulse);
        EXPECT_EQ(east.back(), westTotal - eastPulse * ((int) east.size() - 1));

        // South returns the North steps, the clearing pulse that became step 1
        // included. The two clearing pulses before it are not returned; the
        // South nudge that follows takes the star back.
        std::vector<int> nudges = NudgePulses(o);
        std::vector<int> south = o.Moves(Dir::South);
        ASSERT_GE(south.size(), nudges.size());
        south.resize(south.size() - nudges.size());
        int northTotal = m.decStepCount * kStepMs;
        int southTotal = 0;
        for (int ms : south)
            southTotal += ms;
        EXPECT_EQ(southTotal, northTotal);
        ASSERT_EQ((int) south.size(), DIV_ROUND_UP(northTotal, southPulse));
        for (size_t i = 0; i + 1 < south.size(); i++)
            EXPECT_EQ(south[i], southPulse);
        EXPECT_FALSE(nudges.empty());
        EXPECT_LT(Dist(o.finalTrue, o.lock), 2.0);
    }
}

TYPED_TEST(Calibration, EastAdvisoryOnlyWhenTheMountCannotPulseGuide)
{
    // the star is put back where the East leg started, as if no East pulse had
    // moved it; only a mount that cannot pulse guide (an ST-4 cable) is alerted
    SimMountConfig c = ProfileMount();
    CalOutcome base = Calibrate<TypeParam>(c, ProfileSettings());
    int lastEast = -1;
    for (const Event& e : base.events)
        if (e.kind == Event::Move && e.dir == Dir::East)
            lastEast = e.frame;
    ASSERT_GE(lastEast, 0);

    for (int canPulse = 0; canPulse < 2; canPulse++)
    {
        c.canPulseGuide = canPulse != 0;
        FrameHook<TypeParam> noEast = [&](int frame, Settings&, SimMount& sim, TypeParam&)
        {
            if (frame == lastEast + 1)
            {
                double d = base.measured.raStepCount * kStepMs * kRate;
                double a = Radians(c.westAngleDeg);
                sim.MoveTo(c.startX + d * std::cos(a), c.startY + d * std::sin(a));
            }
        };
        CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings(), 2000, noEast);
        SCOPED_TRACE(canPulse ? "pulse guide" : "ST-4");
        ASSERT_EQ(o.outcome, Outcome::Complete);
        if (canPulse)
        {
            EXPECT_EQ(o.Count(Event::Alert), 0);
        }
        else
        {
            ASSERT_EQ(o.Count(Event::Alert), 1);
            EXPECT_TRUE(StartsWith(o.Of(Event::Alert)[0].text, "Advisory: Little or no east movement was measured"));
            EXPECT_EQ(o.Of(Event::Alert)[0].frame, lastEast + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// The South nudge (audit h)
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, NudgeSouthBringsTheStarBackWhenItCan)
{
    SimMountConfig c = WithLightNoise(ProfileMount());
    c.decBacklashMs = 600.;
    CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Complete);
    std::vector<int> nudges = NudgePulses(o);
    ASSERT_FALSE(nudges.empty());
    EXPECT_LE(nudges.size(), 4u);
    for (int ms : nudges)
    {
        EXPECT_GT(ms, 0);
        EXPECT_LE(ms, kStepMs);
    }
    EXPECT_LT(Dist(o.finalTrue, o.lock), 2.0);
}

TYPED_TEST(Calibration, NudgeSouthStopsAfterFourPulses)
{
    // a lot of backlash leaves the star further north than 4 step-sized nudges reach
    SimMountConfig c = WithLightNoise(ProfileMount());
    c.decBacklashMs = 3500.;
    CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Complete);
    std::vector<int> nudges = NudgePulses(o);
    EXPECT_EQ(nudges, std::vector<int>(4, kStepMs));
    EXPECT_GT(Dist(o.finalTrue, o.lock), 2.0);
}

TYPED_TEST(Calibration, NudgeSouthIsSkippedWhenTheStarIsOffTheNorthLine)
{
    SimMountConfig c = ProfileMount();
    c.decBacklashMs = 600.;
    CalOutcome base = Calibrate<TypeParam>(c, ProfileSettings());
    const int nudgeFrame = FirstDebugFrame(base, "Nudge: theta");
    ASSERT_GE(nudgeFrame, 0);

    // push the star 30 px along RA just before the frame the nudge starts on
    FrameHook<TypeParam> bump = [&](int frame, Settings&, SimMount& sim, TypeParam&)
    {
        if (frame == nudgeFrame)
        {
            double a = Radians(c.westAngleDeg);
            sim.Displace(30. * std::cos(a), 30. * std::sin(a));
        }
    };
    CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings(), 2000, bump);
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_TRUE(NudgePulses(o).empty());
    EXPECT_TRUE(o.HasDebug("Nudging discontinued, wrong direction"));
}

TYPED_TEST(Calibration, NudgeSouthEndsOnANanAngleWhenTheStarIsOnTheLock)
{
    // nudge_amt == 0 gives 0/0 direction cosines and a NaN theta, which fails
    // the 40 deg test and ends the nudge as "wrong direction" (pinned, not fixed)
    SimMountConfig c = ProfileMount();
    c.decBacklashMs = 600.;
    CalOutcome base = Calibrate<TypeParam>(c, ProfileSettings());
    const int nudgeFrame = FirstDebugFrame(base, "Nudge: theta");
    ASSERT_GE(nudgeFrame, 0);

    Pt lock;
    FrameHook<TypeParam> home = [&](int frame, Settings&, SimMount& sim, TypeParam&)
    {
        if (frame == 0)
            lock = sim.TruePosition();
        if (frame == nudgeFrame)
            sim.MoveTo(lock.x, lock.y);
    };
    CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings(), 2000, home);
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_TRUE(NudgePulses(o).empty());
    EXPECT_TRUE(DebugShowsNanAfter(o, "Nudge: theta = "));
    EXPECT_TRUE(DebugShowsNanAfter(o, "Nudging discontinued, wrong direction: "));
}

namespace
{

// #154: a noiseless calibration whose nudge cosine rounds past -1 (or, with
// pastLock, past +1) on the frame the nudge starts (nudge_cosine.h)
template<class Run>
CalOutcome CalibrateWithNudgeCosinePastOne(bool pastLock, int *nudgeFrame)
{
    const SimMountConfig c = nudge_cosine::Mount();
    // the geometry's premise: hypot of the placed vectors is exact
    EXPECT_EQ(std::hypot(nudge_cosine::kDirX / 8., nudge_cosine::kDirY / 8.), nudge_cosine::kLength / 8.);
    EXPECT_EQ(std::hypot(nudge_cosine::kDirX / 64., nudge_cosine::kDirY / 64.), nudge_cosine::kLength / 64.);
    std::pair<int, int> f = nudge_cosine::Frames<Run>(c, ProfileSettings());
    EXPECT_GE(f.first, 0);
    EXPECT_GT(f.second, f.first);
    *nudgeFrame = f.second;
    return Calibrate<Run>(c, ProfileSettings(), 2000, nudge_cosine::Placing<Run>(c, f.first, f.second, pastLock));
}

} // namespace

TEST(LegacyTwin, NudgeCosinePastMinusOneGaveNanAndStoppedTheNudge)
{
    // #154, before the fix: the star 4.3 px north of the lock, exactly back
    // down the North leg, gave a cosine of -1.0000000000000002, acos gave
    // NaN, and the nudge ended as "wrong direction" with the star left there.
    // The extracted run clamps the cosine (CalibrationRunNudge below).
    int nudgeFrame = -1;
    CalOutcome o = CalibrateWithNudgeCosinePastOne<LegacyTwinRun>(false, &nudgeFrame);
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_EQ(nudge_cosine::FirstDebugFrame(o, "Nudge: theta"), nudgeFrame);
    EXPECT_TRUE(DebugShowsNanAfter(o, "Nudge: theta = "));
    EXPECT_TRUE(DebugShowsNanAfter(o, "Nudging discontinued, wrong direction: "));
    EXPECT_TRUE(NudgePulses(o).empty());
    EXPECT_EQ(Dist(o.finalTrue, o.lock), nudge_cosine::kLength / 64.);
}

TEST(CalibrationRunNudge, CosinePastMinusOneStillNudgesBackToTheLock)
{
    // #154: the same frame on the extracted run. The cosine is clamped to -1,
    // the angle is pi, and the nudge takes the star back to the lock.
    int nudgeFrame = -1;
    CalOutcome o = CalibrateWithNudgeCosinePastOne<ExtractedRun>(false, &nudgeFrame);
    ASSERT_EQ(o.outcome, Outcome::Complete);
    bool sawTheta = false;
    for (const Event& e : o.events)
        if (e.kind == Event::Debug && e.frame == nudgeFrame && StartsWith(e.text, "Nudge: theta = "))
        {
            EXPECT_EQ(e.text, "Nudge: theta = 3.14\n");
            sawTheta = true;
        }
    EXPECT_TRUE(sawTheta);
    EXPECT_FALSE(o.HasDebug("Nudging discontinued"));
    std::vector<int> nudges = NudgePulses(o);
    ASSERT_FALSE(nudges.empty());
    EXPECT_LE(nudges.size(), 4u);
    EXPECT_EQ(nudges[0], kStepMs); // 4.3 px is more than a step's 2.1 px
    EXPECT_LT(Dist(o.finalTrue, o.lock), 2.0);
}

TEST(CalibrationRunNudge, CosinePastPlusOneStillStopsAsWrongDirection)
{
    // #154: the star past the lock, exactly down the North leg's direction
    // beyond it: the cosine is +1.0000000000000002, clamped to 1, so the angle
    // is 0 instead of NaN. The nudge stops as "wrong direction" as before, and
    // the Debug line shows the angle.
    int nudgeFrame = -1;
    CalOutcome o = CalibrateWithNudgeCosinePastOne<ExtractedRun>(true, &nudgeFrame);
    ASSERT_EQ(o.outcome, Outcome::Complete);
    EXPECT_TRUE(NudgePulses(o).empty());
    EXPECT_TRUE(o.HasDebug("Nudge: theta = 0.00\n"));
    EXPECT_TRUE(o.HasDebug("Nudging discontinued, wrong direction: 0.00\n"));
    EXPECT_EQ(Dist(o.finalTrue, o.lock), nudge_cosine::kLength / 64.);
}

// ---------------------------------------------------------------------------
// assumeOrthogonal (audit i) and parity (audit j)
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, AssumeOrthogonalSnapsYAngleToTheNearerRightAngle)
{
    for (int assume = 0; assume < 2; assume++)
    {
        SimMountConfig c = ProfileMount();
        c.westAngleDeg = 30.;
        c.nonOrthoDeg = 10.;
        Settings s = ProfileSettings();
        s.assumeOrthogonal = assume != 0;
        SimMount sim(c);
        CalOutcome o = RunCalibration<TypeParam>(sim, s);
        ASSERT_EQ(o.outcome, Outcome::Complete);
        const Measured& m = o.measured;
        if (assume)
        {
            // true North is at -50 deg; xAngle + 90 = -60 is nearer than xAngle - 90 = 120
            EXPECT_EQ(m.yAngle, NormAngle(m.xAngle + M_PI / 2.));
            EXPECT_NEAR(Degrees(m.yAngle), -60., 1e-9);
            EXPECT_NEAR(m.yRate, kRate * std::cos(Radians(10.)), 1e-12);
            EXPECT_TRUE(o.HasDebug("Assuming orthogonal axes"));
        }
        else
        {
            EXPECT_NEAR(Degrees(m.yAngle), -50., 1e-9);
            EXPECT_NEAR(m.yRate, kRate, 1e-12);
        }
    }
}

TYPED_TEST(Calibration, GuideParityFromTheMountCoordinates)
{
    struct Case
    {
        bool coordinates;
        bool raOdd;
        bool decOdd;
        double rateDegPerSec;
        Parity ra;
        Parity dec;
    };
    const double half = 0.5 * 15. / 3600.;
    const Case cases[] = {
        { true, false, false, half, Parity::Even, Parity::Even },
        { true, true, false, half, Parity::Odd, Parity::Even },
        { true, false, true, half, Parity::Even, Parity::Odd },
        { false, false, false, half, Parity::Unknown, Parity::Unknown },
        { true, false, false, 1e-6, Parity::Unknown, Parity::Unknown }, // under 1" of motion
    };
    for (const Case& tc : cases)
    {
        SimMountConfig c = ProfileMount();
        c.coordinates = tc.coordinates;
        c.raParityOdd = tc.raOdd;
        c.decParityOdd = tc.decOdd;
        c.raGuideDegPerSec = tc.rateDegPerSec;
        c.decGuideDegPerSec = tc.rateDegPerSec;
        CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_EQ(o.measured.raParity, tc.ra);
        EXPECT_EQ(o.measured.decParity, tc.dec);
        // the parity is in the West/North complete rows too
        std::vector<Event> dc = o.Of(Event::DirectComplete);
        ASSERT_EQ(dc.size(), 2u);
        EXPECT_EQ(dc[0].parity, (int) tc.ra);
        EXPECT_EQ(dc[1].parity, (int) tc.dec);
    }
}

TYPED_TEST(Calibration, MountReadsHappenAtTodaysCallSites)
{
    // start coords, West-end coords, East cable check, East-end coords and
    // guide rates, coords before each clearing pulse after the first, North-end coords
    SimMountConfig c = ProfileMount();
    SimMount sim(c);
    CalOutcome o = RunCalibration<TypeParam>(sim, ProfileSettings());
    ASSERT_EQ(o.outcome, Outcome::Complete);
    using P = ProbeKind;
    EXPECT_EQ(sim.ProbeLog(),
              (std::vector<P> { P::Coordinates, P::Coordinates, P::CanPulseGuide, P::Coordinates, P::GuideRates, P::Coordinates,
                                P::Coordinates, P::Coordinates }));

    // DEC_NONE stops after the East cable check
    SimMount sim2(c);
    Settings s = ProfileSettings();
    s.decMode = DecMode::None;
    ASSERT_EQ(RunCalibration<TypeParam>(sim2, s).outcome, Outcome::Complete);
    EXPECT_EQ(sim2.ProbeLog(), (std::vector<P> { P::Coordinates, P::Coordinates, P::CanPulseGuide }));

    // no coordinates: the West and North ends do not read again
    c.coordinates = false;
    SimMount sim3(c);
    ASSERT_EQ(RunCalibration<TypeParam>(sim3, ProfileSettings()).outcome, Outcome::Complete);
    EXPECT_EQ(
        sim3.ProbeLog(),
        (std::vector<P> { P::Coordinates, P::CanPulseGuide, P::Coordinates, P::GuideRates, P::Coordinates, P::Coordinates }));
}

// ---------------------------------------------------------------------------
// Cleared mid-run (#137, #145)
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, ClearedWhileCalibratingFailsWithoutAnAlert)
{
    FrameHook<TypeParam> clear = [](int frame, Settings&, SimMount&, TypeParam& run)
    {
        if (frame == 5)
            run.Clear();
    };
    CalOutcome o = Calibrate<TypeParam>(ProfileMount(), ProfileSettings(), 2000, clear);
    ASSERT_EQ(o.outcome, Outcome::Failed);
    EXPECT_EQ(o.fail, Fail::Cleared);
    EXPECT_EQ(o.frames, 6);
    EXPECT_EQ(o.Count(Event::Alert), 0);
    EXPECT_EQ(o.Moves(Dir::West).size(), 5u);
    const std::string msg = "Calibration failed: the calibration was cleared while calibrating";
    ASSERT_EQ(o.Count(Event::LogFailed), 1);
    EXPECT_EQ(o.Of(Event::LogFailed)[0].text, msg);
    ASSERT_EQ(o.Count(Event::NotifyFailed), 1);
    EXPECT_EQ(o.Of(Event::NotifyFailed)[0].text, msg);
    EXPECT_EQ(o.Of(Event::LogFailed)[0].frame, 5);
}

// ---------------------------------------------------------------------------
// High declination with the Dec unknown (#141). The run pulses the step on the
// West leg when it has no Dec, as the code before #141 did at every Dec: the
// leg needs more steps as the star's RA motion shrinks, and fails once it
// needs more than 61. With a known Dec the RA step grows with 1/cos(Dec)
// instead; test_high_dec_calibration.cpp covers that.
// ---------------------------------------------------------------------------

TYPED_TEST(Calibration, HighDecRaStepsGrowWithOneOverCosDecWhenTheDecIsUnknown)
{
    const int cases[][2] = { { 0, 12 }, { 45, 17 }, { 60, 24 }, { 70, 35 }, { 75, 46 }, { 78, 57 }, { -78, 57 } };
    for (const auto& tc : cases)
    {
        SimMountConfig c = ProfileMount();
        c.trueDecDeg = tc[0];
        c.reportDec = false;
        SimMount sim(c);
        CalOutcome o = RunCalibration<TypeParam>(sim, ProfileSettings());
        SCOPED_TRACE("dec " + std::to_string(tc[0]));
        ASSERT_EQ(o.outcome, Outcome::Complete);
        EXPECT_EQ(o.measured.raStepCount, tc[1]);
        EXPECT_EQ(o.Moves(Dir::West), std::vector<int>(tc[1], kStepMs));
        EXPECT_NEAR(o.measured.xRate, sim.RaPxPerMs(), 0.02 * sim.RaPxPerMs());
        EXPECT_NEAR(o.measured.yRate, kRate, 0.02 * kRate);
        EXPECT_EQ(o.MoveTotal(Dir::East), o.MoveTotal(Dir::West));
    }
}

TYPED_TEST(Calibration, HighDecFailsAt79DegreesAndAboveWhenTheDecIsUnknown)
{
    for (int dec : { 79, 80, 85, -80 })
    {
        SimMountConfig c = ProfileMount();
        c.trueDecDeg = dec;
        c.reportDec = false;
        CalOutcome o = Calibrate<TypeParam>(c, ProfileSettings());
        SCOPED_TRACE("dec " + std::to_string(dec));
        ASSERT_EQ(o.outcome, Outcome::Failed);
        EXPECT_EQ(o.fail, Fail::RaDidNotMove);
        EXPECT_EQ(o.Moves(Dir::West), std::vector<int>(61, kStepMs));
    }
}

TEST(LegacyTwin, HighDecIgnoredTheReportedDec)
{
    // before #141 the calibration never read the Dec before or during the legs;
    // a missing or wrong reading changed nothing but the Dec stored with the
    // result. The extracted run now reads it at Begin (test_high_dec_calibration.cpp).
    SimMountConfig c = ProfileMount();
    c.trueDecDeg = 75.;
    CalOutcome reported = Calibrate<LegacyTwinRun>(c, ProfileSettings());
    c.reportDec = false;
    CalOutcome unknown = Calibrate<LegacyTwinRun>(c, ProfileSettings());
    c.reportDec = true;
    c.reportedDecDeg = 0.;
    CalOutcome wrong = Calibrate<LegacyTwinRun>(c, ProfileSettings());
    for (const CalOutcome *o : { &unknown, &wrong })
    {
        ASSERT_EQ(o->events.size(), reported.events.size());
        for (size_t i = 0; i < o->events.size(); i++)
        {
            const Event& a = o->events[i];
            const Event& b = reported.events[i];
            EXPECT_EQ(a.kind, b.kind);
            EXPECT_EQ(a.ms, b.ms);
            EXPECT_EQ(a.step, b.step);
            EXPECT_EQ(a.dist, b.dist);
            if (a.kind != Event::Debug) // the starting-location line prints the coordinates
            {
                EXPECT_EQ(a.text, b.text);
            }
        }
    }
    EXPECT_EQ(reported.measured.raStepCount, 46);
}
