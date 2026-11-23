// The guider's averaged distance on an injected clock (open-astro/openastro-guider#142, Tier 2).
//
// GuideDistanceTracker is what Guider::UpdateCurrentDistance, CurrentError and
// CurrentErrorSmoothed kept: the settle's "pixels" test and the variable delay compare the short
// average (alpha 0.3), not the raw star offset, and the multi-star DistanceChecker the long one.
// A dither bumps both at once, so the first frame after a dither is not read as settled; a resume
// from a full pause or the end of a fast recenter re-seeds them; and once no star has been found
// for 20 s they read 100 px, so a lost star does not settle. The 20 s run on
// monotonic_timers::ClockS, the steady clock, since #226.

#include <gtest/gtest.h>

#include "guide_distance_tracker.h"

using monotonic_timers::StarFoundTimer;

namespace
{
// Fake time in whole seconds: a wall clock, which a step of the system time moves, and a steady
// clock, which it does not
int64_t g_wallS = 0;
int64_t g_steadyS = 0;

int64_t FakeSteadyS()
{
    return g_steadyS;
}

void Advance(int64_t s)
{
    g_wallS += s;
    g_steadyS += s;
}

class GuideDistanceTrackerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_wallS = 1795000000; // epoch seconds, November 2026
        g_steadyS = 5000; // an arbitrary monotonic reading
    }

    // production gives the tracker monotonic_timers::ClockS, the steady clock
    GuideDistanceTracker tracker { &FakeSteadyS };
};
} // namespace

TEST_F(GuideDistanceTrackerTest, NotGuidingReadsTheLastSample)
{
    tracker.OnStarFound(false, 2.0, 1.0);
    EXPECT_EQ(tracker.CurrentError(false), 2.0);
    EXPECT_EQ(tracker.CurrentError(true), 1.0);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(false), 2.0);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(true), 1.0);
    EXPECT_EQ(tracker.FrameCount(), 1u);

    tracker.OnStarFound(false, 3.0, 0.5);
    EXPECT_EQ(tracker.CurrentError(false), 3.0);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(true), 0.5);
    EXPECT_EQ(tracker.FrameCount(), 1u);
}

TEST_F(GuideDistanceTrackerTest, GuidingTheShortAverageHasAlphaPointThree)
{
    tracker.OnStarFound(false, 1.0, 1.0);
    tracker.OnStarFound(true, 0.0, 0.0);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(false), 0.7);
    tracker.OnStarFound(true, 0.0, 0.0);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(false), 0.49);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(true), 0.49);
    EXPECT_EQ(tracker.FrameCount(), 3u);
}

TEST_F(GuideDistanceTrackerTest, TheLongAverageIsARunningMeanThenAlphaPointZeroFourFive)
{
    // the running mean while the count is under 10 (samples 1 to 9), then alpha 0.045 from the
    // 10th sample on
    tracker.OnStarFound(false, 1.0, 0.5);
    for (int d = 2; d <= 9; ++d)
        tracker.OnStarFound(true, d, d / 2.);
    EXPECT_EQ(tracker.FrameCount(), 9u);
    EXPECT_DOUBLE_EQ(tracker.CurrentErrorSmoothed(false), 5.0);
    EXPECT_DOUBLE_EQ(tracker.CurrentErrorSmoothed(true), 2.5);

    tracker.OnStarFound(true, 10.0, 5.0);
    EXPECT_EQ(tracker.FrameCount(), 10u);
    EXPECT_DOUBLE_EQ(tracker.CurrentErrorSmoothed(false), 5.0 + 0.045 * 5.0);
    EXPECT_DOUBLE_EQ(tracker.CurrentErrorSmoothed(true), 2.5 + 0.045 * 2.5);
}

TEST_F(GuideDistanceTrackerTest, ADitherBumpsTheAveragesAtOnce)
{
    // converged at 0.2 px, a 4 px dither (3 px in RA) reads 4.2 straight away, so the first frame
    // after it is not taken as settled; the next sample of 4.0 gives 0.7 * 4.2 + 0.3 * 4.0
    tracker.OnStarFound(false, 0.2, 0.2);
    tracker.OnDither(4.0, 3.0);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(false), 4.2);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(true), 3.2);
    EXPECT_DOUBLE_EQ(tracker.CurrentErrorSmoothed(false), 4.2);
    EXPECT_DOUBLE_EQ(tracker.CurrentErrorSmoothed(true), 3.2);

    tracker.OnStarFound(true, 4.0, 3.0);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(false), 4.14);
}

TEST_F(GuideDistanceTrackerTest, RequestResetReseedsFromTheNextSample)
{
    // a resume from a full pause, or a fast recenter done: the next sample, guiding or not,
    // restarts every average from itself
    tracker.OnStarFound(false, 0.2, 0.1);
    for (int i = 0; i < 12; ++i)
        tracker.OnStarFound(true, 0.3, 0.2);
    tracker.RequestReset();
    EXPECT_EQ(tracker.FrameCount(), 13u); // nothing changes until that sample
    tracker.OnStarFound(true, 2.5, 1.5);
    EXPECT_EQ(tracker.CurrentError(false), 2.5);
    EXPECT_EQ(tracker.CurrentError(true), 1.5);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(false), 2.5);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(true), 1.5);
    EXPECT_EQ(tracker.FrameCount(), 1u);

    // once only
    tracker.OnStarFound(true, 0.5, 0.5);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(false), 2.5 + 0.3 * (0.5 - 2.5));
}

TEST_F(GuideDistanceTrackerTest, NoStarFoundReadsTheLargeDistance)
{
    EXPECT_EQ(tracker.CurrentError(false), StarFoundTimer::LARGE_DISTANCE);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(true), StarFoundTimer::LARGE_DISTANCE);
    EXPECT_EQ(StarFoundTimer::LARGE_DISTANCE, 100.0);
}

TEST_F(GuideDistanceTrackerTest, AStarOlderThanTwentySecondsReadsTheLargeDistance)
{
    tracker.OnStarFound(false, 0.8, 0.4);
    Advance(20);
    EXPECT_EQ(tracker.CurrentError(false), 0.8);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(true), 0.4);
    Advance(1);
    EXPECT_EQ(tracker.CurrentError(false), StarFoundTimer::LARGE_DISTANCE);
    EXPECT_EQ(tracker.CurrentError(true), StarFoundTimer::LARGE_DISTANCE);
    EXPECT_EQ(tracker.CurrentErrorSmoothed(false), StarFoundTimer::LARGE_DISTANCE);

    // the next star found restarts the 20 s
    tracker.OnStarFound(true, 0.8, 0.4);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(false), 0.8);
}

TEST_F(GuideDistanceTrackerTest, RaOnlyIgnoresTheDecError)
{
    // Dec guide mode None: the settle compares the RA-only average, so a Dec error alone settles
    tracker.OnStarFound(false, 5.0, 0.3);
    EXPECT_EQ(tracker.CurrentError(true), 0.3);
    tracker.OnStarFound(true, 5.0, 0.3);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(true), 0.3);
    EXPECT_DOUBLE_EQ(tracker.CurrentError(false), 5.0);
}

TEST_F(GuideDistanceTrackerTest, AClockStepDoesNotReadTheLargeDistance)
{
    // a step of the system time of +-1 h leaves the 20 s to real time: the tracker reads the
    // steady clock (#226)
    for (int64_t step : { int64_t { 3600 }, int64_t { -3600 } })
    {
        tracker.OnStarFound(false, 0.8, 0.4);
        g_wallS += step;
        Advance(1);
        EXPECT_EQ(tracker.CurrentError(false), 0.8) << "step " << step;
        Advance(19);
        EXPECT_EQ(tracker.CurrentError(false), 0.8) << "step " << step;
        Advance(1);
        EXPECT_EQ(tracker.CurrentError(false), StarFoundTimer::LARGE_DISTANCE) << "step " << step;
    }
}
