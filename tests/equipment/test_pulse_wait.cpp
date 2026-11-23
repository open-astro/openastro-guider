// pulse_wait (#119).
//
// ScopeAlpaca::Guide times an Alpaca PulseGuide from just before the request
// is sent, so a request that blocks while the pulse runs (AlpacaBridge's
// iOptron driver blocks about 300 ms) is not followed by a second wait for
// the whole pulse. The rules live in pulse_wait.cpp.

#include <gtest/gtest.h>

#include "pulse_wait.h"

using PulseWait::CompletionTimedOut;
using PulseWait::SleepBeforeFirstPollMs;

TEST(PulseWait, BlockingCallLongerThanThePulseNeedsNoSleep)
{
    // RC51: a 117 ms pulse whose PulseGuide request took 302 ms
    EXPECT_EQ(SleepBeforeFirstPollMs(117, 302), 0);
}

TEST(PulseWait, FastCallSleepsForWhatIsLeftOfThePulse)
{
    EXPECT_EQ(SleepBeforeFirstPollMs(117, 5), 122);
}

TEST(PulseWait, CallEndingWithThePulseNeedsNoSleep)
{
    EXPECT_EQ(SleepBeforeFirstPollMs(117, 117), 0);
    EXPECT_EQ(SleepBeforeFirstPollMs(117, 116), 11);
}

TEST(PulseWait, InstantCallSleepsTheWholePulsePlusMargin)
{
    EXPECT_EQ(SleepBeforeFirstPollMs(117, 0), 127);
    EXPECT_EQ(SleepBeforeFirstPollMs(0, 0), 0);
}

TEST(PulseWait, LongPulseStillWaitsForItsRemainder)
{
    EXPECT_EQ(SleepBeforeFirstPollMs(3000, 302), 2708);
}

TEST(PulseWait, CompletionTimesOutTwoSecondsAfterThePulse)
{
    EXPECT_FALSE(CompletionTimedOut(117, 117));
    EXPECT_FALSE(CompletionTimedOut(117, 2117));
    EXPECT_TRUE(CompletionTimedOut(117, 2118));
    // AlpacaBridge's iOptron driver reports IsPulseGuiding for pulse + 500 ms and its
    // SynScan/Sky-Watcher drivers for pulse + 1000 ms, both well inside the window
    EXPECT_FALSE(CompletionTimedOut(117, 117 + 500 + 40));
    EXPECT_FALSE(CompletionTimedOut(117, 117 + 1000 + 40));
}

// ScopeAlpaca::Guide times the pulse with wxStopWatch, which reads the wall clock (#142). RC51
// boots with a stale clock and then syncs it, so a step back during a pulse gives a negative
// elapsed time: the sleep was the pulse plus the size of the step (an hour's step slept the
// worker thread for an hour inside one guide pulse), and the completion wait never timed out.
TEST(PulseWait, NegativeElapsedSleepsNoLongerThanTheWholePulse)
{
    EXPECT_EQ(SleepBeforeFirstPollMs(117, -1), 127);
    EXPECT_EQ(SleepBeforeFirstPollMs(117, -3600 * 1000L), 127);
    EXPECT_EQ(SleepBeforeFirstPollMs(0, -3600 * 1000L), 0);
}

TEST(PulseWait, NegativeElapsedStillTimesOut)
{
    // A negative elapsed time says the clock stepped back, not how long the mount has been
    // waited on, so a mount still reporting IsPulseGuiding is given up on rather than waited
    // on for the size of the step.
    EXPECT_TRUE(CompletionTimedOut(117, -1));
    EXPECT_TRUE(CompletionTimedOut(117, -3600 * 1000L));
}

// #294: the PulseGuide PUT's own timeout. A held request (a 35 s stall on the e2e fake) fails the
// move after 15 s, not the client's 30 s, and a mount that blocks for the whole pulse is waited for.
TEST(PulseWait, ShortPulseRequestTimesOutAfter15s)
{
    EXPECT_EQ(PulseWait::RequestTimeoutMs(117), 15000);
    EXPECT_EQ(PulseWait::RequestTimeoutMs(0), 15000);
    EXPECT_EQ(PulseWait::RequestTimeoutMs(5000), 15000);
}

TEST(PulseWait, LongPulseRequestWaitsThePulsePlus10s)
{
    EXPECT_EQ(PulseWait::RequestTimeoutMs(5001), 15001);
    EXPECT_EQ(PulseWait::RequestTimeoutMs(25000), 35000);
}

TEST(PulseWait, NegativePulseRequestTimesOutAfter15s)
{
    EXPECT_EQ(PulseWait::RequestTimeoutMs(-1), 15000);
}
