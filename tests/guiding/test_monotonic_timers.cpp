// The timers behind Guider::CurrentError's 100 px "large distance" (StarFoundTimer) and
// MyFrame's camera reconnect throttle (ReconnectThrottle), on an injected clock, and the clock
// their callers give them the time from (ClockS) (open-astro/openastro-guider#226).
//
// Both read wxDateTime::GetTimeNow(), the wall clock. RC51 boots with a stale clock and then
// syncs it: a step forward mid-guide made the guider read a 100 px error on the next frame, and a
// step back made old reconnect attempts look recent, so the throttle refused a reconnect it
// should have allowed (a step forward did the opposite). The rules live in
// src/monotonic_timers.cpp; Guider and MyFrame keep the averaging, the events and the reconnect.
//
// The set_pa_session lease (PaSessionLease) on an injected clock, and ClockMs, the same clock in
// milliseconds that the lease, the multi-star DistanceChecker's wait and the star mass-change
// window are given the time from (#231). They read wxGetUTCTimeMillis(), the wall clock: a step
// back held the lease for the size of the step beyond its timeout, and a step forward ended it in
// the middle of Ara's polar alignment.

#include <gtest/gtest.h>

#include "monotonic_timers.h"

#include <chrono>

using monotonic_timers::ClockMs;
using monotonic_timers::ClockS;
using monotonic_timers::PaSessionLease;
using monotonic_timers::ReconnectThrottle;
using monotonic_timers::StarFoundTimer;

namespace
{
int64_t SteadyS()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// An arbitrary monotonic reading, as a steady clock's zero is arbitrary
constexpr int64_t kT0 = 5000;
} // namespace

// --- ClockS ---

TEST(ClockS, IsTheSteadyClock)
{
    // The wall clock jumps when the system time is set (a Pi that boots with a stale clock and
    // then syncs it); the monotonic clock does not
    int64_t before = SteadyS();
    int64_t now = ClockS();
    int64_t after = SteadyS();
    EXPECT_GE(now, before);
    EXPECT_LE(now, after);
}

// --- StarFoundTimer: Guider::CurrentError and CurrentErrorSmoothed ---

TEST(StarFoundTimer, LargeDistanceBeforeAnyStarIsFound)
{
    StarFoundTimer timer;
    EXPECT_EQ(timer.CurrentError(kT0, 1.5), StarFoundTimer::LARGE_DISTANCE);
    EXPECT_EQ(StarFoundTimer::LARGE_DISTANCE, 100.0);
}

TEST(StarFoundTimer, AveragedDistanceUpToTwentySecondsAfterTheStar)
{
    StarFoundTimer timer;
    timer.Found(kT0);
    EXPECT_EQ(timer.CurrentError(kT0, 1.5), 1.5);
    EXPECT_EQ(timer.CurrentError(kT0 + 20, 1.5), 1.5);
}

TEST(StarFoundTimer, LargeDistanceMoreThanTwentySecondsAfterTheStar)
{
    StarFoundTimer timer;
    timer.Found(kT0);
    EXPECT_EQ(timer.CurrentError(kT0 + 21, 1.5), StarFoundTimer::LARGE_DISTANCE);
}

TEST(StarFoundTimer, FindingTheStarAgainRestartsTheTwentySeconds)
{
    StarFoundTimer timer;
    timer.Found(kT0);
    timer.Found(kT0 + 15);
    EXPECT_EQ(timer.CurrentError(kT0 + 35, 0.7), 0.7);
    EXPECT_EQ(timer.CurrentError(kT0 + 36, 0.7), StarFoundTimer::LARGE_DISTANCE);
}

TEST(StarFoundTimer, AStarFoundAtClockZeroCounts)
{
    // The wall-clock timestamp used 0 for "never found"; a monotonic clock may read 0
    StarFoundTimer timer;
    timer.Found(0);
    EXPECT_EQ(timer.CurrentError(5, 2.0), 2.0);
}

// --- ReconnectThrottle: MyFrame::OnTryReconnect, 3 camera reconnects in 60 s ---

TEST(ReconnectThrottle, ThreeAttemptsInAMinuteThenRefused)
{
    ReconnectThrottle throttle;
    EXPECT_TRUE(throttle.TryAttempt(kT0));
    EXPECT_TRUE(throttle.TryAttempt(kT0 + 10));
    EXPECT_TRUE(throttle.TryAttempt(kT0 + 20));
    EXPECT_EQ(throttle.Attempts(), 3);
    EXPECT_FALSE(throttle.TryAttempt(kT0 + 30));
    EXPECT_EQ(throttle.Attempts(), 3); // a refused attempt is not recorded
}

TEST(ReconnectThrottle, AnAttemptOlderThanTheWindowIsForgotten)
{
    ReconnectThrottle throttle;
    throttle.TryAttempt(kT0);
    throttle.TryAttempt(kT0 + 10);
    throttle.TryAttempt(kT0 + 20);
    EXPECT_FALSE(throttle.TryAttempt(kT0 + 60)); // the first is exactly 60 s old: still counted
    EXPECT_TRUE(throttle.TryAttempt(kT0 + 61));
    EXPECT_EQ(throttle.Attempts(), 3);
    EXPECT_FALSE(throttle.TryAttempt(kT0 + 70));
    EXPECT_TRUE(throttle.TryAttempt(kT0 + 71));
    EXPECT_EQ(throttle.Attempts(), 3);
}

TEST(ReconnectThrottle, AllowedAgainAMinuteAfterTheLastAttempt)
{
    ReconnectThrottle throttle;
    throttle.TryAttempt(kT0);
    throttle.TryAttempt(kT0 + 1);
    throttle.TryAttempt(kT0 + 2);
    EXPECT_FALSE(throttle.TryAttempt(kT0 + 50));
    EXPECT_TRUE(throttle.TryAttempt(kT0 + 63));
    EXPECT_EQ(throttle.Attempts(), 1);
}

// --- ClockMs ---

// The lease, the DistanceChecker wait and the mass-change window are given the time from ClockMs.
// It must be the steady clock: a reading of the wall clock moves with every step of the system
// time (RC51 boots with a stale clock and then syncs it), which lengthened or cut short a running
// set_pa_session lease by the size of the step.
TEST(ClockMs, IsTheSteadyClock)
{
    int64_t before = SteadyMs();
    int64_t now = ClockMs();
    int64_t after = SteadyMs();
    EXPECT_GE(now, before);
    EXPECT_LE(now, after);
}

TEST(ClockMs, IsClockSInMilliseconds)
{
    // one clock in two units, so a timer in seconds and one in milliseconds agree
    int64_t beforeS = ClockMs() / 1000;
    int64_t s = ClockS();
    int64_t afterS = ClockMs() / 1000;
    EXPECT_GE(s, beforeS);
    EXPECT_LE(s, afterS);
}

// --- PaSessionLease: set_pa_session / get_pa_session ---

TEST(PaSessionLease, InactiveUntilStarted)
{
    PaSessionLease lease;
    EXPECT_FALSE(lease.Active(0));
    EXPECT_FALSE(lease.Active(1'000'000));
    PaSessionLease::Status s = lease.StatusAt(0);
    EXPECT_FALSE(s.active);
    EXPECT_EQ(s.expiresInS, 0);
}

TEST(PaSessionLease, HeldForTheTimeoutThenExpires)
{
    PaSessionLease lease;
    lease.Start(5'000, 600);
    EXPECT_TRUE(lease.Active(5'000));
    EXPECT_TRUE(lease.Active(5'000 + 599'999));
    EXPECT_FALSE(lease.Active(5'000 + 600'000));
    EXPECT_FALSE(lease.Active(5'000 + 3'600'000));
}

TEST(PaSessionLease, StartsAtClockZero)
{
    // a monotonic clock can read 0, so 0 must not mean "no lease"
    PaSessionLease lease;
    lease.Start(0, 10);
    EXPECT_TRUE(lease.Active(0));
    EXPECT_TRUE(lease.Active(9'999));
    EXPECT_FALSE(lease.Active(10'000));
}

TEST(PaSessionLease, ExpiresInSRoundsDownAndAgreesWithActive)
{
    PaSessionLease lease;
    lease.Start(1'000, 600);
    PaSessionLease::Status s = lease.StatusAt(1'000);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.expiresInS, 600);
    s = lease.StatusAt(1'001);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.expiresInS, 599);
    s = lease.StatusAt(1'000 + 599'999);
    EXPECT_TRUE(s.active);
    EXPECT_EQ(s.expiresInS, 0);
    s = lease.StatusAt(1'000 + 600'000);
    EXPECT_FALSE(s.active);
    EXPECT_EQ(s.expiresInS, 0);
}

TEST(PaSessionLease, RenewStartsAgainFromNow)
{
    PaSessionLease lease;
    lease.Start(0, 600);
    lease.Start(300'000, 10); // a renewal may shorten the lease as well as lengthen it
    EXPECT_TRUE(lease.Active(309'999));
    EXPECT_FALSE(lease.Active(310'000));
    EXPECT_EQ(lease.StatusAt(300'000).expiresInS, 10);
}

TEST(PaSessionLease, EndDropsIt)
{
    PaSessionLease lease;
    lease.Start(0, 600);
    lease.End();
    EXPECT_FALSE(lease.Active(1));
    EXPECT_FALSE(lease.StatusAt(1).active);
    lease.Start(2, 10);
    EXPECT_TRUE(lease.Active(3));
}

TEST(PaSessionLease, MaxTimeoutDoesNotOverflow)
{
    PaSessionLease lease;
    int64_t now = SteadyMs();
    lease.Start(now, 3600);
    EXPECT_EQ(lease.StatusAt(now).expiresInS, 3600);
    EXPECT_FALSE(lease.Active(now + 3'600'000));
}
