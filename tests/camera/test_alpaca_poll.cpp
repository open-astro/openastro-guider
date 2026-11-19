// Alpaca ImageReady poll pacing tests (open-astro/openastro-guider#120).
//
// CameraAlpaca::Capture sleeps until 100 ms before the exposure is due, then
// polls GET camera/N/imageready until the frame is ready. It used to poll
// every 20 ms for the whole readout (about 50 HTTP requests a second); it now
// waits AlpacaPoll::ImageReadyIntervalMs(time past the expected end) between
// polls, which these tests pin.

#include <gtest/gtest.h>

#include "alpaca_poll.h"

#include <algorithm>

using AlpacaPoll::ImageReadyIntervalMs;

TEST(AlpacaPoll, TwentyMsUntilTheExposureIsDue)
{
    EXPECT_EQ(ImageReadyIntervalMs(-100000), 20);
    EXPECT_EQ(ImageReadyIntervalMs(-100), 20);
    EXPECT_EQ(ImageReadyIntervalMs(-1), 20);
    EXPECT_EQ(ImageReadyIntervalMs(0), 20);
}

TEST(AlpacaPoll, FastReadoutKeepsTwentyMs)
{
    // A guide camera whose frame is ready within ~80 ms of the exposure end
    // keeps today's cadence.
    for (long e = 1; e <= 83; ++e)
        EXPECT_EQ(ImageReadyIntervalMs(e), 20) << "e=" << e;
    EXPECT_GT(ImageReadyIntervalMs(84), 20);
}

TEST(AlpacaPoll, QuarterOfTheTimeWaitedThenCapped)
{
    EXPECT_EQ(ImageReadyIntervalMs(400), 100);
    EXPECT_EQ(ImageReadyIntervalMs(999), 249);
    EXPECT_EQ(ImageReadyIntervalMs(1000), 250);
    EXPECT_EQ(ImageReadyIntervalMs(1001), 250);
    EXPECT_EQ(ImageReadyIntervalMs(60000), 250);
    EXPECT_EQ(ImageReadyIntervalMs(100000000L), 250);
}

TEST(AlpacaPoll, MonotonicNonDecreasing)
{
    int prev = ImageReadyIntervalMs(-1000);
    for (long e = -999; e <= 10000; ++e)
    {
        int cur = ImageReadyIntervalMs(e);
        EXPECT_GE(cur, prev) << "e=" << e;
        prev = cur;
    }
}

TEST(AlpacaPoll, AddedLatencyIsBounded)
{
    for (long e = 1; e <= 20000; ++e)
    {
        int iv = ImageReadyIntervalMs(e);
        EXPECT_GE(iv, 20) << "e=" << e;
        EXPECT_LE(iv, 250) << "e=" << e;
        EXPECT_LE(iv, std::max(20L, e / 4)) << "e=" << e;
    }
}

namespace
{
struct Readout
{
    int polls;
    long overshootMs; // how long after the frame was ready it was seen
};

// Simulate the wait loop: the first poll's sleep starts 80 ms before the
// exposure is due (the loop starts 100 ms early and the first poll takes
// a 20 ms sleep), and the frame is ready readoutMs after the exposure end.
// Request time is ignored.
Readout Simulate(long readoutMs)
{
    long e = -80;
    Readout r { 0, 0 };
    while (true)
    {
        e += ImageReadyIntervalMs(e);
        ++r.polls;
        if (e >= readoutMs)
        {
            r.overshootMs = e - readoutMs;
            return r;
        }
    }
}
}

TEST(AlpacaPoll, SimulatedReadouts)
{
    const long kReadouts[] = { 0, 50, 80, 200, 1000, 5000, 30000 };
    for (long R : kReadouts)
    {
        Readout r = Simulate(R);
        EXPECT_LE(r.overshootMs, std::max(20L, R / 4 + 20)) << "R=" << R;
        EXPECT_LE(r.overshootMs, 270) << "R=" << R;
        // the old fixed 20 ms loop's count
        long oldPolls = (R + 80 + 19) / 20;
        EXPECT_LE(r.polls, oldPolls) << "R=" << R;
    }

    // A fast readout keeps the old cadence exactly.
    EXPECT_EQ(Simulate(50).polls, (50 + 80 + 19) / 20);
    EXPECT_EQ(Simulate(50).overshootMs, 10);

    // A 1 s readout: about 20 polls instead of 54; 5 s: about 36 instead of 254.
    EXPECT_LE(Simulate(1000).polls, 22);
    EXPECT_LE(Simulate(5000).polls, 40);
    EXPECT_LE(Simulate(30000).polls, 140);
}
