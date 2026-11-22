// The frame rejection GuiderMultiStar::UpdateCurrentPosition runs after the primary star is found
// (#142): MassChecker (the star's mass against the median of the last frames) and DistanceChecker
// (the distance from the lock position against the smoothed guide error, with its 5 s wait and
// recovery), on an injected clock. Both moved unchanged from guider_multistar.cpp into
// src/frame_reject.cpp; test_multistar_parity.cpp compares them with the frozen copy of the old
// code. These pin what they do, including one known bug as it is (see
// RecoveringToGuidingKeepsTheForcedTolerance).

#include <gtest/gtest.h>

#include "frame_reject.h"

#include <random>
#include <string>
#include <vector>

using frame_reject::DistanceChecker;
using frame_reject::GuiderStatus;
using frame_reject::MassChecker;

namespace
{
constexpr int64_t kT0 = 100000; // an arbitrary monotonic reading
constexpr int64_t kFrameMs = 2000; // 2 s frames

// Appends n frames of mass at kFrameMs intervals from *t
void Feed(MassChecker& mc, double mass, int n, int64_t *t)
{
    for (int i = 0; i < n; i++)
    {
        mc.AppendData(mass, *t);
        *t += kFrameMs;
    }
}

GuiderStatus Guiding(double avgDist = 1.0, unsigned int frames = 20)
{
    GuiderStatus g;
    g.guiding = true;
    g.frameCount = frames;
    g.avgDist = avgDist;
    return g;
}

constexpr double kNoJumpTolerance = 9e99; // UpdateCurrentPosition's tolerance with tolerate-jumps off
} // namespace

// --- MassChecker ---

TEST(MassChecker, NeverRejectsWithFewerThanFiveEntries)
{
    MassChecker mc;
    int64_t t = kT0;
    Feed(mc, 1000, 4, &t);
    double limits[4] = {};
    EXPECT_FALSE(mc.CheckMass(1e9, 0.5, limits));
    EXPECT_FALSE(mc.CheckMass(0, 0.5, limits));
    Feed(mc, 1000, 1, &t);
    EXPECT_TRUE(mc.CheckMass(1e9, 0.5, limits));
}

TEST(MassChecker, SteadyMassLimits)
{
    // Steady 1000 at threshold 0.5: low 1000 * 0.5, median, high 1000 * 1.5, median * (1 + 2 * 0.5)
    MassChecker mc;
    int64_t t = kT0;
    Feed(mc, 1000, 20, &t);
    double limits[4] = {};
    EXPECT_FALSE(mc.CheckMass(1400, 0.5, limits));
    EXPECT_DOUBLE_EQ(limits[0], 500);
    EXPECT_DOUBLE_EQ(limits[1], 1000);
    EXPECT_DOUBLE_EQ(limits[2], 1500);
    EXPECT_DOUBLE_EQ(limits[3], 2000);
    EXPECT_TRUE(mc.CheckMass(1600, 0.5, limits)); // over limits[2]
    EXPECT_TRUE(mc.CheckMass(499, 0.5, limits)); // under limits[0]
    EXPECT_FALSE(mc.CheckMass(501, 0.5, limits));
    EXPECT_FALSE(mc.CheckMass(1500, 0.5, limits)); // the limits are exclusive
    EXPECT_FALSE(mc.CheckMass(500, 0.5, limits));
}

TEST(MassChecker, HighWaterMarkTracksTheMedianNotOneSample)
{
    // A single bright frame does not move the median, so it does not raise the high limit
    MassChecker mc;
    int64_t t = kT0;
    Feed(mc, 1000, 10, &t);
    Feed(mc, 3000, 1, &t);
    Feed(mc, 1000, 10, &t);
    double limits[4] = {};
    EXPECT_TRUE(mc.CheckMass(1600, 0.5, limits));
    EXPECT_DOUBLE_EQ(limits[2], 1500);
    EXPECT_DOUBLE_EQ(mc.HighMass(), 1000);
}

TEST(MassChecker, MedianSpikeLimitWhenDimmed)
{
    // After the median rose to 2000 and fell back to 1000 (clouds), the high-water limit is
    // 3000, but a spike over the median * 2 is still rejected
    MassChecker mc;
    int64_t t = kT0;
    double limits[4] = {};
    Feed(mc, 2000, 20, &t);
    mc.CheckMass(2000, 0.5, limits);
    t += 2 * MassChecker::DefaultTimeWindowMs; // the 2000s age out
    Feed(mc, 1000, 20, &t);
    EXPECT_TRUE(mc.CheckMass(2100, 0.5, limits));
    EXPECT_DOUBLE_EQ(limits[2], 3000);
    EXPECT_DOUBLE_EQ(limits[3], 2000);
    EXPECT_FALSE(mc.CheckMass(1900, 0.5, limits));
}

TEST(MassChecker, LowWaterMarkDriftsBackUpFivePercentPerCheck)
{
    MassChecker mc;
    int64_t t = kT0;
    double limits[4] = {};
    Feed(mc, 400, 20, &t);
    mc.CheckMass(400, 0.5, limits);
    EXPECT_DOUBLE_EQ(mc.LowMass(), 400);
    t += 2 * MassChecker::DefaultTimeWindowMs;
    Feed(mc, 1000, 20, &t);
    mc.CheckMass(1000, 0.5, limits);
    EXPECT_DOUBLE_EQ(mc.LowMass(), 400 + 0.05 * 600);
    mc.CheckMass(1000, 0.5, limits);
    EXPECT_DOUBLE_EQ(mc.LowMass(), 430 + 0.05 * 570);
}

TEST(MassChecker, SustainedCloudFadeRejectionRun)
{
    // A drop to 40 % at 2 s frames, every frame appended as UpdateCurrentPosition does (a
    // rejected mass is appended too). The frames are rejected until the median follows the drop:
    // with a 45 s window of 22 frames, after 12 frames.
    MassChecker mc;
    int64_t t = kT0;
    double limits[4] = {};
    Feed(mc, 1000, 30, &t);
    int rejected = 0;
    for (int i = 0; i < 40; i++)
    {
        bool reject = mc.CheckMass(400, 0.5, limits);
        mc.AppendData(400, t);
        t += kFrameMs;
        if (!reject)
            break;
        ++rejected;
    }
    EXPECT_EQ(rejected, 12);
}

TEST(MassChecker, SlowFadeUnderTheThresholdIsAccepted)
{
    // A 50 % fade over 100 frames never trips threshold 0.5
    MassChecker mc;
    int64_t t = kT0;
    Feed(mc, 1000, 20, &t);
    double limits[4] = {};
    for (int i = 0; i <= 100; i++)
    {
        double mass = 1000 - 5 * i;
        EXPECT_FALSE(mc.CheckMass(mass, 0.5, limits)) << i;
        mc.AppendData(mass, t);
        t += kFrameMs;
    }
}

TEST(MassChecker, ManualExposureChangeResets)
{
    MassChecker mc;
    mc.SetExposure(1000, false);
    int64_t t = kT0;
    Feed(mc, 1000, 10, &t);
    mc.SetExposure(1000, false);
    EXPECT_EQ(mc.Count(), 10u);
    mc.SetExposure(2000, false);
    EXPECT_EQ(mc.Count(), 0u);
}

TEST(MassChecker, AutoExposureNormalisesAndDoesNotReset)
{
    MassChecker mc;
    mc.SetExposure(1000, true); // auto on: a reset
    int64_t t = kT0;
    Feed(mc, 1000, 10, &t); // 1 per ms
    mc.SetExposure(2000, true);
    EXPECT_EQ(mc.Count(), 10u);
    EXPECT_DOUBLE_EQ(mc.AdjustedMass(2000), 1.0);
    double limits[4] = {};
    EXPECT_FALSE(mc.CheckMass(2000, 0.5, limits)); // the same flux at twice the exposure
    EXPECT_DOUBLE_EQ(limits[1], 1.0); // not rejected: limits stay per ms
    EXPECT_TRUE(mc.CheckMass(3200, 0.5, limits)); // 1.6 per ms
    // rejected: multiplied back by the exposure for the log
    EXPECT_DOUBLE_EQ(limits[0], 1000);
    EXPECT_DOUBLE_EQ(limits[1], 2000);
    EXPECT_DOUBLE_EQ(limits[2], 3000);
    EXPECT_DOUBLE_EQ(limits[3], 4000);
    mc.SetExposure(2000, false); // auto off: a reset
    EXPECT_EQ(mc.Count(), 0u);
}

TEST(MassChecker, EntriesOlderThanTwiceTheWindowAreDropped)
{
    MassChecker mc;
    const int64_t kept = 2 * MassChecker::DefaultTimeWindowMs; // 45 s
    mc.AppendData(1000, kT0);
    mc.AppendData(1000, kT0 + kept);
    EXPECT_EQ(mc.Count(), 2u); // exactly the window old: kept
    mc.AppendData(1000, kT0 + kept + 1);
    EXPECT_EQ(mc.Count(), 2u); // the first one dropped
    mc.SetTimeWindow(1000);
    mc.AppendData(1000, kT0 + kept + 2002);
    EXPECT_EQ(mc.Count(), 1u);
}

TEST(MassChecker, ZeroThresholdRejectsNearlyEveryFrameOfANoisySteadySeries)
{
    // Threshold 0 makes the spike limit the median itself, so every mass over the median is
    // rejected, and the low limit, the lowest median seen drifting back up, rejects most of the
    // rest: with 3 % noise, over 90 % of the frames of a steady star
    MassChecker mc;
    std::mt19937 rng(1);
    std::normal_distribution<double> noise(1000, 30);
    int64_t t = kT0;
    double limits[4] = {};
    int rejected = 0;
    const int n = 400;
    for (int i = 0; i < n + 10; i++)
    {
        double mass = noise(rng);
        if (i >= 10 && mc.CheckMass(mass, 0., limits))
            ++rejected;
        mc.AppendData(mass, t);
        t += kFrameMs;
    }
    EXPECT_GT(rejected, n * 9 / 10);
}

TEST(MassChecker, ResetForgetsTheMarks)
{
    MassChecker mc;
    int64_t t = kT0;
    Feed(mc, 1000, 10, &t);
    double limits[4] = {};
    mc.CheckMass(1000, 0.5, limits);
    mc.Reset();
    EXPECT_EQ(mc.Count(), 0u);
    EXPECT_DOUBLE_EQ(mc.HighMass(), 0.);
    EXPECT_DOUBLE_EQ(mc.LowMass(), 9e99);
}

// --- DistanceChecker ---

TEST(DistanceChecker, AcceptsWhileNotGuidingPausedSettlingOrWithoutStats)
{
    DistanceChecker dc;
    GuiderStatus g = Guiding();
    g.guiding = false;
    EXPECT_TRUE(dc.CheckDistance(1e6, 4, g, kT0));
    g = Guiding();
    g.paused = true;
    EXPECT_TRUE(dc.CheckDistance(1e6, 4, g, kT0));
    g = Guiding();
    g.settling = true;
    EXPECT_TRUE(dc.CheckDistance(1e6, 4, g, kT0));
    g = Guiding(1.0, DistanceChecker::MIN_FRAMES_FOR_STATS - 1);
    EXPECT_TRUE(dc.CheckDistance(1e6, 4, g, kT0));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_GUIDING);
    g = Guiding(1.0, DistanceChecker::MIN_FRAMES_FOR_STATS);
    EXPECT_FALSE(dc.CheckDistance(1e6, 4, g, kT0));
}

TEST(DistanceChecker, JumpWaitRecoverCycle)
{
    std::vector<std::string> log;
    DistanceChecker dc([&log](const std::string& s) { log.push_back(s); });
    GuiderStatus g = Guiding(1.0);
    EXPECT_TRUE(dc.CheckDistance(4.0, 4, g, kT0)); // not over tolerance * avgDist
    EXPECT_FALSE(dc.CheckDistance(4.1, 4, g, kT0));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_WAITING);
    EXPECT_DOUBLE_EQ(dc.ForceTolerance(), 0.); // a jump does not force the tolerance
    EXPECT_FALSE(dc.CheckDistance(10, 4, g, kT0 + 4999));
    EXPECT_TRUE(dc.CheckDistance(10, 4, g, kT0 + DistanceChecker::WAIT_INTERVAL_MS));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_RECOVERING);
    EXPECT_TRUE(dc.CheckDistance(10, 4, g, kT0 + 6000)); // recovering accepts the large ones
    EXPECT_TRUE(dc.CheckDistance(1, 4, g, kT0 + 8000));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_GUIDING);
    std::vector<std::string> want = {
        "DistanceChecker: reject for large offset (4.10 > 4.00) avgDist = 1.00 count = 20\n",
        "DistanceChecker: activated\n",
        "DistanceChecker: reject for large offset (10.00 > 4.00) avgDist = 1.00 count = 20\n",
        "DistanceChecker: reject for large offset (10.00 > 4.00) avgDist = 1.00 count = 20\n",
        "DistanceChecker: begin recovering\n",
        "DistanceChecker: reject for large offset (10.00 > 4.00) avgDist = 1.00 count = 20\n",
        "DistanceChecker: deactivated\n",
    };
    EXPECT_EQ(log, want);
}

TEST(DistanceChecker, SmallOffsetWhileWaitingEndsTheWait)
{
    DistanceChecker dc;
    GuiderStatus g = Guiding(1.0);
    EXPECT_FALSE(dc.CheckDistance(5, 4, g, kT0));
    EXPECT_TRUE(dc.CheckDistance(1, 4, g, kT0 + 1000));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_GUIDING);
    EXPECT_FALSE(dc.CheckDistance(5, 4, g, kT0 + 2000)); // a new jump waits again
}

TEST(DistanceChecker, ActivateForcesToleranceTwoUntilASmallOffset)
{
    // A lost star or a mass change: the next frames over 2x the average are rejected even with
    // tolerate-jumps off, for up to 5 s
    DistanceChecker dc;
    GuiderStatus g = Guiding(1.0);
    dc.Activate(kT0);
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_WAITING);
    EXPECT_DOUBLE_EQ(dc.ForceTolerance(), 2.0);
    EXPECT_FALSE(dc.CheckDistance(2.5, kNoJumpTolerance, g, kT0 + 2000));
    EXPECT_TRUE(dc.CheckDistance(1.5, kNoJumpTolerance, g, kT0 + 4000));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_GUIDING);
    EXPECT_DOUBLE_EQ(dc.ForceTolerance(), 0.);
    EXPECT_TRUE(dc.CheckDistance(2.5, kNoJumpTolerance, g, kT0 + 6000));
}

TEST(DistanceChecker, ActivateOnlyFromGuiding)
{
    // A second Activate while waiting does not restart the 5 s
    DistanceChecker dc;
    GuiderStatus g = Guiding(1.0);
    dc.Activate(kT0);
    dc.Activate(kT0 + 4000);
    EXPECT_TRUE(dc.CheckDistance(10, kNoJumpTolerance, g, kT0 + DistanceChecker::WAIT_INTERVAL_MS));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_RECOVERING);
}

TEST(DistanceChecker, RecoveringToGuidingKeepsTheForcedTolerance)
{
    // Known bug, pinned as it is (#142): Activate (a lost star) forces the tolerance to 2.0;
    // ST_WAITING -> ST_GUIDING clears it, but ST_WAITING -> ST_RECOVERING -> ST_GUIDING does not.
    // Back to guiding with tolerate-jumps off (9e99), the next frame over 2x the smoothed error is
    // still rejected as a jump ("Recovering", a StarLost to the client and a dead-reckoned frame),
    // and so are the frames after it for up to 5 s. Upstream PHD2 has the same code.
    DistanceChecker dc;
    GuiderStatus g = Guiding(1.0);
    dc.Activate(kT0);
    EXPECT_FALSE(dc.CheckDistance(3, kNoJumpTolerance, g, kT0 + 2000));
    EXPECT_TRUE(dc.CheckDistance(3, kNoJumpTolerance, g, kT0 + DistanceChecker::WAIT_INTERVAL_MS));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_RECOVERING);
    EXPECT_TRUE(dc.CheckDistance(1, kNoJumpTolerance, g, kT0 + 7000));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_GUIDING);
    EXPECT_DOUBLE_EQ(dc.ForceTolerance(), 2.0);
    EXPECT_FALSE(dc.CheckDistance(2.5, kNoJumpTolerance, g, kT0 + 9000));
    EXPECT_EQ(dc.GetState(), DistanceChecker::ST_WAITING);
}
