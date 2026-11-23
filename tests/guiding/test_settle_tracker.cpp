// A guide or dither settle on an injected clock (open-astro/openastro-guider#142, Tier 2).
//
// SettleTracker is PhdController's settle as STATE_SETTLE_BEGIN and STATE_SETTLE_WAIT run it:
// the settle timeout timed from Begin, the run of in-range frames timed from its first frame,
// and the TotalFrames / DroppedFrames counts SettleDone reports. Ara opens each sub on the one
// SettleDone a settle sends, so an early settle trails a sub and a missed one fails the dither.
// What a single frame does is SettleFrame::OnFrame's (test_settle_frame.cpp); these tests are
// about the times the tracker gives it, read from the clock.
//
// The settle timers were wxStopWatch, which reads the wall clock. RC51 boots with a stale clock
// and then syncs it, so the system time can step by hours mid-session (#257): a step forward
// settled a run or timed the settle out on the next frame, a step back held both for the size
// of the step. The tracker now reads the steady clock, and the ClockStep tests below check that
// a step of +-1 h leaves the settle to real time.

#include <gtest/gtest.h>

#include "settle_tracker.h"

#include <chrono>

using SettleFrame::Outcome;
using SettleFrame::Result;

namespace
{
// The fake time the tests set: a wall clock, which a step of the system time moves, and a
// steady clock, which it does not. Both advance with real time.
struct FakeTime
{
    static int64_t wallMs;
    static int64_t steadyMs;

    static void Set(int64_t ms)
    {
        wallMs = 1795000000000 + ms; // epoch milliseconds, November 2026
        steadyMs = 5000 + ms; // an arbitrary monotonic reading
    }
    static void Advance(int64_t ms)
    {
        wallMs += ms;
        steadyMs += ms;
    }
    static void StepSystemTime(int64_t ms) { wallMs += ms; }
};
int64_t FakeTime::wallMs = 0;
int64_t FakeTime::steadyMs = 0;

int64_t FakeSteadyMs()
{
    return FakeTime::steadyMs;
}

// The clock PhdController gives its SettleTracker (SettleTracker::ProductionClockMs), on the fake:
// the steady clock (ProductionClock.IsTheSteadyClock, #257)
constexpr SettleTracker::ClockMs kProductionClock = &FakeSteadyMs;

constexpr int64_t kHourMs = 3600 * 1000;

// 1.5 px for settleTimeSec s within timeoutSec s, no frame limit to speak of
SettleParams Params(int settleTimeSec, int timeoutSec, int frames = 9999)
{
    SettleParams p;
    p.tolerancePx = 1.5;
    p.settleTimeSec = settleTimeSec;
    p.timeoutSec = timeoutSec;
    p.frames = frames;
    return p;
}

constexpr double kIn = 0.5; // in range of 1.5 px
constexpr double kOut = 3.0; // out of range

class SettleTrackerTest : public ::testing::Test
{
protected:
    void SetUp() override { FakeTime::Set(0); }

    // A frame at fake time tMs since the test began, locked on the star
    Result FrameAt(int64_t tMs, const SettleParams& params, double error, bool locked = true, bool aoBump = false)
    {
        AdvanceTo(tMs);
        return tracker.OnFrame(params, false, locked, error, aoBump);
    }

    void AdvanceTo(int64_t tMs)
    {
        FakeTime::Advance(tMs - nowMs);
        nowMs = tMs;
    }

    SettleTracker tracker { kProductionClock };
    int64_t nowMs = 0;
};
} // namespace

// --- the clock ---

TEST(ProductionClock, IsTheSteadyClock)
{
    // The wall clock jumps when the system time is set (a Pi that boots with a stale clock and
    // then syncs it); the monotonic clock does not (#257)
    auto steady = []()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    int64_t before = steady();
    int64_t now = SettleTracker::ProductionClockMs();
    int64_t after = steady();
    EXPECT_GE(now, before);
    EXPECT_LE(now, after);
}

// --- settle reached ---

TEST_F(SettleTrackerTest, NoSettleTimeSettlesOnTheFirstInRangeFrame)
{
    SettleParams p = Params(0, 60);
    tracker.Begin();
    EXPECT_EQ(FrameAt(2000, p, kIn).outcome, Outcome::Settled);
    EXPECT_EQ(tracker.FrameCount(), 1);
    EXPECT_EQ(tracker.DroppedFrameCount(), 0);
}

TEST_F(SettleTrackerTest, SettlesExactlyAtTheSettleTimeInRange)
{
    // time=5: in range from t=1 s; the run is timed from its first frame and the settle time is
    // compared in whole seconds (ms / 1000), so 4.999 s is not enough and 5.000 s is
    SettleParams p = Params(5, 60);
    tracker.Begin();
    EXPECT_EQ(FrameAt(0, p, kOut).outcome, Outcome::Settling);

    Result r = FrameAt(1000, p, kIn);
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_TRUE(r.startInRangeRun);
    EXPECT_EQ(r.timeInRangeMs, 0);

    r = FrameAt(3000, p, kIn);
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_EQ(r.timeInRangeMs, 2000);

    r = FrameAt(5999, p, kIn);
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_EQ(r.timeInRangeMs, 4999);

    EXPECT_EQ(FrameAt(6000, p, kIn).outcome, Outcome::Settled);
    EXPECT_EQ(tracker.FrameCount(), 5);
    EXPECT_EQ(tracker.DroppedFrameCount(), 0);
}

TEST_F(SettleTrackerTest, ErrorEqualToTheToleranceIsInRange)
{
    SettleParams p = Params(0, 60);
    tracker.Begin();
    EXPECT_EQ(FrameAt(1000, p, p.tolerancePx).outcome, Outcome::Settled);
}

TEST_F(SettleTrackerTest, LeavingTheRangeRestartsTheTimer)
{
    // in range from 1 s, out at 3 s, back at 4 s: success only 5 s after the re-entry
    SettleParams p = Params(5, 60);
    tracker.Begin();
    FrameAt(1000, p, kIn);
    FrameAt(2000, p, kIn);
    EXPECT_EQ(FrameAt(3000, p, kOut).outcome, Outcome::Settling);

    Result r = FrameAt(4000, p, kIn);
    EXPECT_TRUE(r.startInRangeRun);
    EXPECT_EQ(r.timeInRangeMs, 0);
    for (int64_t t = 5000; t < 9000; t += 1000)
        EXPECT_EQ(FrameAt(t, p, kIn).outcome, Outcome::Settling) << "t = " << t;
    EXPECT_EQ(FrameAt(9000, p, kIn).outcome, Outcome::Settled);
}

// --- the min-time rule ---

TEST_F(SettleTrackerTest, AnAoBumpHoldsTheTimedSuccessOnly)
{
    // the asymmetry: with a settle time, an AO bump in progress holds the success; with none, the
    // first in-range frame settles whatever the bump
    SettleParams p = Params(2, 60);
    tracker.Begin();
    FrameAt(0, p, kIn);
    EXPECT_EQ(FrameAt(2000, p, kIn, true, true).outcome, Outcome::Settling);
    EXPECT_EQ(FrameAt(3000, p, kIn, true, false).outcome, Outcome::Settled);

    SettleParams p0 = Params(0, 60);
    tracker.Begin();
    EXPECT_EQ(FrameAt(4000, p0, kIn, true, true).outcome, Outcome::Settled);
}

TEST_F(SettleTrackerTest, TheFrameLimitSettlesWhateverTheDistance)
{
    // dither with settle frames (DitherCompat): time and timeout disabled, every frame out of range
    SettleParams p = Params(9999, 9999, 10);
    tracker.Begin();
    for (int i = 1; i < 10; ++i)
        EXPECT_EQ(FrameAt(i * 1000, p, 150.).outcome, Outcome::Settling) << "frame " << i;
    EXPECT_EQ(FrameAt(10000, p, 150.).outcome, Outcome::Settled);
    EXPECT_EQ(tracker.FrameCount(), 10);
}

// --- settle timeout ---

TEST_F(SettleTrackerTest, NeverInRangeTimesOut)
{
    // timed from Begin in whole seconds: 9.999 s goes on, 10.000 s times out
    SettleParams p = Params(5, 10);
    AdvanceTo(500);
    tracker.Begin();
    EXPECT_EQ(FrameAt(5500, p, kOut).outcome, Outcome::Settling);
    EXPECT_EQ(FrameAt(10499, p, kOut).outcome, Outcome::Settling);
    EXPECT_EQ(FrameAt(10500, p, kOut).outcome, Outcome::TimedOut);
    EXPECT_EQ(tracker.FrameCount(), 3);
    EXPECT_EQ(tracker.DroppedFrameCount(), 0);
}

TEST_F(SettleTrackerTest, SuccessIsCheckedBeforeTheTimeoutOnTheSameFrame)
{
    // time=10, timeout=10, in range from Begin: at 10 s both are reached and the settle succeeds
    SettleParams p = Params(10, 10);
    tracker.Begin();
    FrameAt(0, p, kIn);
    EXPECT_EQ(FrameAt(10000, p, kIn).outcome, Outcome::Settled);
}

TEST_F(SettleTrackerTest, TimeEqualToTimeoutFailsWhenTheRunStartsLate)
{
    // time=10, timeout=10, in range only from 1 s: the timeout comes first
    SettleParams p = Params(10, 10);
    tracker.Begin();
    FrameAt(1000, p, kIn);
    Result r = FrameAt(10000, p, kIn);
    EXPECT_EQ(r.outcome, Outcome::TimedOut);
    EXPECT_EQ(r.timeInRangeMs, 9000);
}

// --- star lost ---

TEST_F(SettleTrackerTest, LostStarFramesAreDroppedAndRestartTheRun)
{
    // a frame not locked on the star is a dropped frame, whatever its error, and ends the run:
    // the next in-range frame starts a new one
    SettleParams p = Params(5, 60);
    tracker.Begin();
    FrameAt(0, p, kIn);
    FrameAt(4000, p, kIn);

    Result r = FrameAt(5000, p, kIn, false);
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_FALSE(r.lockedOnStar);
    EXPECT_EQ(tracker.DroppedFrameCount(), 1);

    r = FrameAt(6000, p, kIn);
    EXPECT_TRUE(r.startInRangeRun);
    EXPECT_EQ(FrameAt(10999, p, kIn).outcome, Outcome::Settling);
    EXPECT_EQ(FrameAt(11000, p, kIn).outcome, Outcome::Settled);
    EXPECT_EQ(tracker.FrameCount(), 6);
    EXPECT_EQ(tracker.DroppedFrameCount(), 1);
}

TEST_F(SettleTrackerTest, StarLostThroughoutTimesOut)
{
    SettleParams p = Params(5, 10);
    tracker.Begin();
    for (int64_t t = 1000; t < 10000; t += 1000)
        EXPECT_EQ(FrameAt(t, p, kIn, false).outcome, Outcome::Settling) << "t = " << t;
    EXPECT_EQ(FrameAt(10000, p, kIn, false).outcome, Outcome::TimedOut);
    EXPECT_EQ(tracker.FrameCount(), 10);
    EXPECT_EQ(tracker.DroppedFrameCount(), 10);
}

TEST_F(SettleTrackerTest, ATransientDroppedFrameKeepsTheRunTimed)
{
    // #136: a dropped frame mid-run neither ends the run nor restarts its timer
    SettleParams p = Params(5, 60);
    tracker.Begin();
    FrameAt(0, p, kIn);
    AdvanceTo(3000);
    Result r = tracker.OnFrame(p, true, true, kIn, false);
    EXPECT_EQ(r.timeInRangeMs, 3000);
    EXPECT_EQ(FrameAt(5000, p, kIn).outcome, Outcome::Settled);
    EXPECT_EQ(tracker.DroppedFrameCount(), 1);
}

// --- a new settle, a new request ---

TEST_F(SettleTrackerTest, BeginStartsAFreshSettle)
{
    // a dither after a settle: no run and no counts carried over, the timeout from the new Begin
    SettleParams p = Params(5, 10);
    tracker.Begin();
    FrameAt(0, p, kIn);
    FrameAt(1000, p, kIn, false);
    EXPECT_EQ(FrameAt(6000, p, kIn).outcome, Outcome::Settling);

    AdvanceTo(20000);
    tracker.Begin();
    EXPECT_EQ(tracker.FrameCount(), 0);
    EXPECT_EQ(tracker.DroppedFrameCount(), 0);
    Result r = FrameAt(21000, p, kIn);
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_TRUE(r.startInRangeRun);
    EXPECT_EQ(FrameAt(29999, p, kOut).outcome, Outcome::Settling);
    EXPECT_EQ(FrameAt(30000, p, kOut).outcome, Outcome::TimedOut);
}

TEST_F(SettleTrackerTest, ResetCountsZeroesTheCountsOnly)
{
    SettleParams p = Params(5, 60);
    tracker.Begin();
    FrameAt(1000, p, kIn, false);
    FrameAt(2000, p, kIn);
    tracker.ResetCounts();
    EXPECT_EQ(tracker.FrameCount(), 0);
    EXPECT_EQ(tracker.DroppedFrameCount(), 0);
}

// --- a step of the system clock, +-1 h ---
//
// On the wall clock a step forward settled a run or timed the settle out on the next frame, and a
// step back held both for the size of the step (#257). On the steady clock the settle takes its
// settle time in range and its timeout in real time, whichever way the system time was set.

TEST_F(SettleTrackerTest, ClockStepForwardDoesNotSettleARunEarly)
{
    // 2 s into a 10 s in-range run, the clock steps 1 h forward: the run still needs its 10 s
    SettleParams p = Params(10, 60);
    tracker.Begin();
    FrameAt(0, p, kIn);
    FrameAt(2000, p, kIn);
    FakeTime::StepSystemTime(kHourMs);
    Result r = FrameAt(3000, p, kIn);
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_EQ(r.timeInRangeMs, 3000);
    EXPECT_EQ(FrameAt(9999, p, kIn).outcome, Outcome::Settling);
    EXPECT_EQ(FrameAt(10000, p, kIn).outcome, Outcome::Settled);
}

TEST_F(SettleTrackerTest, ClockStepForwardDoesNotTimeOutEarly)
{
    // 3 s into a 60 s timeout, out of range, the clock steps 1 h forward: the timeout still
    // comes 60 s after Begin
    SettleParams p = Params(10, 60);
    tracker.Begin();
    FrameAt(1000, p, kOut);
    FrameAt(3000, p, kOut);
    FakeTime::StepSystemTime(kHourMs);
    for (int64_t t = 4000; t < 60000; t += 1000)
        ASSERT_EQ(FrameAt(t, p, kOut).outcome, Outcome::Settling) << "t = " << t;
    EXPECT_EQ(FrameAt(60000, p, kOut).outcome, Outcome::TimedOut);
}

TEST_F(SettleTrackerTest, ClockStepBackDoesNotHoldTheSettle)
{
    // 2 s into a 10 s in-range run, the clock steps 1 h back: the run completes 10 s after its
    // first frame, and Settling.SettleTime counts up from 0 meanwhile
    SettleParams p = Params(10, 60);
    tracker.Begin();
    FrameAt(0, p, kIn);
    FrameAt(2000, p, kIn);
    FakeTime::StepSystemTime(-kHourMs);
    for (int64_t t = 3000; t < 10000; t += 1000)
    {
        Result r = FrameAt(t, p, kIn);
        ASSERT_EQ(r.outcome, Outcome::Settling) << "t = " << t;
        ASSERT_EQ(r.timeInRangeMs, t) << "t = " << t;
    }
    EXPECT_EQ(FrameAt(10000, p, kIn).outcome, Outcome::Settled);
}

TEST_F(SettleTrackerTest, ClockStepBackDoesNotHoldTheTimeout)
{
    // never in range, the clock steps 1 h back 3 s in: the settle still times out 60 s after Begin
    SettleParams p = Params(10, 60);
    tracker.Begin();
    FrameAt(3000, p, kOut);
    FakeTime::StepSystemTime(-kHourMs);
    for (int64_t t = 4000; t < 60000; t += 1000)
        ASSERT_EQ(FrameAt(t, p, kOut).outcome, Outcome::Settling) << "t = " << t;
    EXPECT_EQ(FrameAt(60000, p, kOut).outcome, Outcome::TimedOut);
}
