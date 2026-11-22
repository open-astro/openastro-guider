// The multi-star refine step (#142): multistar_refine::Refine, which GuiderMultiStar::RefineOffset
// delegates to, on plain star records and a scripted star finder. It moved unchanged from
// guider_multistar.cpp into src/multistar_refine.h; test_multistar_parity.cpp compares it with
// the frozen copy of the old code. These pin what it does, and that the star after a zero-count
// drop is counted once (#262, ZeroCountDropCountsTheNextStarOnce).

#include <gtest/gtest.h>

#include "multistar_refine.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace
{
struct Point
{
    double X = 0.;
    double Y = 0.;
};

// The GuideStar fields Refine reads and writes
struct TestStar
{
    int id = 0;
    double X = 0.;
    double Y = 0.;
    double SNR = 10.;
    Point referencePoint;
    unsigned int missCount = 0;
    unsigned int zeroCount = 0;
    Point offsetFromPrimary;
    bool wasLost = false;
};

struct FindCall
{
    int id;
    double x;
    double y;
    bool verbose;
};

// Where each star is found on this frame (by id), or not found when absent
struct Sky
{
    std::map<int, Point> found;
    std::vector<FindCall> calls;

    bool operator()(TestStar& s, double x, double y, bool verbose)
    {
        calls.push_back({ s.id, x, y, verbose });
        auto it = found.find(s.id);
        if (it == found.end())
            return false;
        s.X = it->second.X;
        s.Y = it->second.Y;
        return true;
    }

    int CallsFor(int id) const
    {
        int n = 0;
        for (const FindCall& c : calls)
            n += c.id == id;
        return n;
    }
};

struct Fixture
{
    std::vector<TestStar> stars;
    multistar_refine::Frame frame;
    multistar_refine::State state;
    DescriptiveStats stats;
    Sky sky;
    std::vector<std::string> log;
    bool validPositions = true;

    Fixture()
    {
        TestStar primary;
        primary.X = 500;
        primary.Y = 400;
        stars.push_back(primary);
        frame.primaryX = 500;
        frame.primaryY = 400;
        frame.primarySNR = 10;
        frame.maxStars = 9;
        frame.stabilitySigmaX = 5;
        // Five primary distances (sigma 1.10) so the sixth, this frame's, ends the start-up
        // stabilisation; a primary distance of 1-2 px keeps sigma about 1
        for (double d : { 0., 2., 0., 2., 0. })
            stats.AddValue(d);
    }

    // A secondary at (x, y) whose reference point is there too
    TestStar& AddStar(int id, double x, double y, double snr = 10.)
    {
        TestStar s;
        s.id = id;
        s.X = s.referencePoint.X = x;
        s.Y = s.referencePoint.Y = y;
        s.SNR = snr;
        s.offsetFromPrimary = { x - frame.primaryX, y - frame.primaryY };
        stars.push_back(s);
        return stars.back();
    }

    // The star with this id is found moved by (dx, dy) from its reference point
    void Move(int id, double dx, double dy)
    {
        for (const TestStar& s : stars)
            if (s.id == id)
                sky.found[id] = { s.referencePoint.X + dx, s.referencePoint.Y + dy };
    }

    multistar_refine::Result Run(double offsetX, double offsetY)
    {
        frame.offsetX = offsetX;
        frame.offsetY = offsetY;
        bool valid = validPositions;
        return multistar_refine::Refine(
            stars, frame, state, stats, [this](TestStar& s, double x, double y, bool v) { return sky(s, x, y, v); },
            [valid](double, double) { return valid; }, [this](const std::string& s) { log.push_back(s); });
    }

    const TestStar *Find(int id) const
    {
        for (const TestStar& s : stars)
            if (s.id == id)
                return &s;
        return nullptr;
    }
};
} // namespace

TEST(MultistarRefine, SnrWeightedMeanAppliedWhenSmaller)
{
    Fixture f;
    f.AddStar(1, 100, 100, 10); // weight 1
    f.AddStar(2, 200, 100, 20); // weight 2
    f.AddStar(3, 300, 100, 5); // weight 0.5
    for (int id : { 1, 2, 3 })
        f.Move(id, 1.0, 0.);
    multistar_refine::Result r = f.Run(1.4, 0.);
    ASSERT_TRUE(r.refined);
    EXPECT_DOUBLE_EQ(r.offsetX, (1.4 + 1 * 1.0 + 2 * 1.0 + 0.5 * 1.0) / (1 + 1 + 2 + 0.5));
    EXPECT_DOUBLE_EQ(r.offsetY, 0.);
    EXPECT_EQ(r.starsUsed, 4u);
    ASSERT_EQ(f.log.size(), 2u);
    EXPECT_EQ(f.log[0], "MultiStar: [#1 1.00,0.00,1.00,U] [#2 1.00,0.00,2.00,U] [#3 1.00,0.00,0.50,U] \n");
    EXPECT_EQ(f.log[1], "refined, 3 included, MultiStar: {1.09, 0.00}, one-star: {1.40, 0.00}\n");
    // looked for where last found, minimal logging
    ASSERT_EQ(f.sky.calls.size(), 3u);
    EXPECT_EQ(f.sky.calls[0].x, 100);
    EXPECT_FALSE(f.sky.calls[0].verbose);
}

TEST(MultistarRefine, MeanNotAppliedWhenNotSmaller)
{
    Fixture f;
    f.AddStar(1, 100, 100);
    f.Move(1, 1.5, 0.);
    multistar_refine::Result r = f.Run(1.0, 0.);
    EXPECT_FALSE(r.refined);
    EXPECT_EQ(r.starsUsed, 2u);
    EXPECT_EQ(f.log.back(), "single-star, 1 included, MultiStar: {1.25, 0.00}, one-star: {1.00, 0.00}\n");
}

TEST(MultistarRefine, NoPrimaryOffsetNoRefinement)
{
    Fixture f;
    f.AddStar(1, 100, 100);
    f.Move(1, 1.0, 0.);
    multistar_refine::Result r = f.Run(0., 0.);
    EXPECT_FALSE(r.refined);
    EXPECT_TRUE(f.sky.calls.empty());
    EXPECT_TRUE(f.log.empty());
}

TEST(MultistarRefine, StabilisesUntilSixPrimaryDistances)
{
    Fixture f;
    f.stats.ClearAll();
    f.AddStar(1, 100, 100);
    f.Move(1, 1.0, 0.);
    for (int i = 0; i < 5; i++)
    {
        EXPECT_FALSE(f.Run(1.0, 0.).refined);
        EXPECT_TRUE(f.state.stabilizing);
    }
    EXPECT_TRUE(f.sky.calls.empty());
}

TEST(MultistarRefine, LargePrimaryJumpStabilisesUntilBackWithinTwoSigma)
{
    Fixture f;
    f.AddStar(1, 100, 100);
    f.Move(1, 0.5, 0.);
    // The frame's own distance is in the sigma, so a steady history makes a jump stand out
    f.stats.ClearAll();
    for (int i = 0; i < 40; i++)
        f.stats.AddValue(i % 2 ? 1.2 : 0.8);
    multistar_refine::Result r = f.Run(3.0, 0.); // sigma 0.37 with it: over 5 sigma
    EXPECT_FALSE(r.refined);
    EXPECT_TRUE(f.state.stabilizing);
    EXPECT_EQ(f.log.front(), "MultiStar: large primary error, entering stabilization period\n");
    EXPECT_TRUE(f.sky.calls.empty());
    // sigma 0.37: 1.0 is over 2 sigma, still stabilising
    EXPECT_FALSE(f.Run(1.0, 0.).refined);
    EXPECT_TRUE(f.state.stabilizing);
    // 0.6 is within 2 sigma: the stabilisation ends and the same frame is refined
    r = f.Run(0.6, 0.);
    EXPECT_FALSE(f.state.stabilizing);
    EXPECT_TRUE(r.refined);
    EXPECT_DOUBLE_EQ(r.offsetX, (0.6 + 0.5) / 2);
}

TEST(MultistarRefine, ZeroMovementOnBothAxesDropsTheStar)
{
    Fixture f;
    f.AddStar(1, 100, 100);
    f.AddStar(2, 200, 100);
    f.Move(1, 0., 0.);
    f.Move(2, 1.0, 0.);
    multistar_refine::Result r = f.Run(1.4, 0.);
    EXPECT_EQ(f.Find(1), nullptr);
    EXPECT_EQ(f.sky.CallsFor(2), 1);
    EXPECT_EQ(r.starsUsed, 3u); // the dropped star is counted
    EXPECT_DOUBLE_EQ(r.offsetX, (1.4 + 1.0) / 2);
    EXPECT_EQ(f.log[0], "MultiStar: [#1 0.00,0.00,0.00,DZ] [#1 1.00,0.00,1.00,U] \n");
}

TEST(MultistarRefine, ZeroCountDropCountsTheNextStarOnce)
{
    // #262: a star whose movement is exactly zero on one axis five frames running is dropped, and
    // the next star is found and averaged once. The drop's continue used to leave the erasures
    // flag set, so the next star's pass did not advance past it and it was found and averaged a
    // second time: twice its weight, and starsUsed counted it twice. Upstream PHD2 has the bug.
    Fixture f;
    TestStar& a = f.AddStar(1, 100, 100);
    a.zeroCount = 4;
    f.AddStar(2, 200, 100);
    f.AddStar(3, 300, 100);
    f.Move(1, 0., 0.5); // zero on X: zeroCount 5
    f.Move(2, 1.0, 0.);
    f.Move(3, 1.0, 0.);
    multistar_refine::Result r = f.Run(1.4, 0.);
    EXPECT_EQ(f.Find(1), nullptr);
    EXPECT_EQ(f.sky.CallsFor(2), 1);
    EXPECT_EQ(f.sky.CallsFor(3), 1);
    EXPECT_EQ(r.starsUsed, 4u); // 1 + the dropped star 1 + star 2 + star 3
    EXPECT_DOUBLE_EQ(r.offsetX, (1.4 + 2 * 1.0) / 3);
    EXPECT_EQ(f.log[0], "MultiStar: [#1 0.00,0.00,0.00,DZ] [#1 1.00,0.00,1.00,U] [#2 1.00,0.00,1.00,U] \n");
    EXPECT_EQ(f.log[1], "refined, 2 included, MultiStar: {1.13, 0.00}, one-star: {1.40, 0.00}\n");
}

TEST(MultistarRefine, ZeroCountDropBeforeAMissCountsEveryStarOnce)
{
    // #262: when the star after the drop is a miss, the star after it used to be the one counted
    // twice, the miss's own continue passing the flag on
    Fixture f;
    TestStar& a = f.AddStar(1, 100, 100);
    a.zeroCount = 4;
    f.AddStar(2, 200, 100);
    f.AddStar(3, 300, 100);
    f.Move(1, 0., 0.5);
    f.Move(2, 20., 0.); // over 2.5 sigma: a miss
    f.Move(3, 1.0, 0.);
    multistar_refine::Result r = f.Run(1.4, 0.);
    EXPECT_EQ(f.sky.CallsFor(2), 1);
    EXPECT_EQ(f.sky.CallsFor(3), 1);
    EXPECT_EQ(r.starsUsed, 4u);
    EXPECT_DOUBLE_EQ(r.offsetX, (1.4 + 1.0) / 2);
}

TEST(MultistarRefine, ZeroCountDropOfTheLastStar)
{
    Fixture f;
    f.AddStar(1, 100, 100);
    TestStar& b = f.AddStar(2, 200, 100);
    b.zeroCount = 4;
    f.Move(1, 1.0, 0.5);
    f.Move(2, 0.5, 0.);
    multistar_refine::Result r = f.Run(1.4, 0.5);
    EXPECT_EQ(f.stars.size(), 2u);
    EXPECT_EQ(f.Find(2), nullptr);
    EXPECT_EQ(r.starsUsed, 3u);
    EXPECT_DOUBLE_EQ(r.offsetX, (1.4 + 1.0) / 2);
}

TEST(MultistarRefine, OneAxisZeroCountsUpAndDecaysOnMovement)
{
    Fixture f;
    f.AddStar(1, 100, 100);
    f.Move(1, 0., 0.5);
    f.Run(1.4, 0.);
    EXPECT_EQ(f.Find(1)->zeroCount, 1u);
    f.Move(1, 0.5, 0.5);
    f.Run(1.4, 0.);
    EXPECT_EQ(f.Find(1)->zeroCount, 0u);
}

TEST(MultistarRefine, MissesAndTheReferenceResetAfterTen)
{
    Fixture f;
    f.AddStar(1, 100, 100);
    f.Move(1, 20., 0.5); // over 2.5 sigma, and not zero on an axis
    for (unsigned int i = 1; i <= 10; i++)
    {
        f.log.clear();
        multistar_refine::Result r = f.Run(1.4, 0.);
        EXPECT_FALSE(r.refined); // nothing averaged
        EXPECT_EQ(f.Find(1)->missCount, i);
        EXPECT_EQ(f.log.size(), 1u);
        EXPECT_EQ(f.log[0], "MultiStar: [#1 20.00,0.50,0.00,M" + std::to_string(i) + "] \n");
    }
    f.log.clear();
    f.Run(1.4, 0.);
    EXPECT_EQ(f.log[0], "MultiStar: [#1 20.00,0.50,0.00,R] \n");
    EXPECT_EQ(f.Find(1)->missCount, 0u);
    EXPECT_DOUBLE_EQ(f.Find(1)->referencePoint.X, 120.);
    // a usable frame takes a miss off
    f.stars[1].missCount = 3;
    f.Move(1, 0.5, 0.5);
    f.Run(1.4, 0.);
    EXPECT_EQ(f.Find(1)->missCount, 2u);
}

TEST(MultistarRefine, MaxStarsCapsTheStarsUsed)
{
    Fixture f;
    f.frame.maxStars = 3;
    for (int id = 1; id <= 5; id++)
    {
        f.AddStar(id, 100. * id, 100);
        f.Move(id, 1.0, 0.);
    }
    multistar_refine::Result r = f.Run(1.4, 0.);
    EXPECT_EQ(r.starsUsed, 3u);
    EXPECT_EQ(f.sky.calls.size(), 2u);
}

TEST(MultistarRefine, LostStarIsLookedForAtItsOffsetFromThePrimary)
{
    Fixture f;
    f.AddStar(1, 120, 130);
    f.Run(1.4, 0.); // not found
    EXPECT_TRUE(f.Find(1)->wasLost);
    EXPECT_EQ(f.log[0], "MultiStar: [#1 0.00,0.00,0.00,L] \n");
    f.frame.primaryX = 510;
    f.frame.primaryY = 405;
    f.sky.calls.clear();
    f.Move(1, 1.0, 0.);
    f.Run(1.4, 0.);
    ASSERT_EQ(f.sky.calls.size(), 1u);
    EXPECT_DOUBLE_EQ(f.sky.calls[0].x, 510 + (120 - 500));
    EXPECT_DOUBLE_EQ(f.sky.calls[0].y, 405 + (130 - 400));
    EXPECT_FALSE(f.Find(1)->wasLost);
}

TEST(MultistarRefine, LockPositionMoveReReferencesAfterTheStabilisation)
{
    // GuiderMultiStar::SetLockPosition sets lockPositionMoved and stabilizing
    for (bool valid : { true, false })
    {
        Fixture f;
        f.validPositions = valid;
        f.AddStar(1, 100, 100);
        f.AddStar(2, 200, 100);
        f.Move(1, 3.0, 1.0);
        f.state.stabilizing = true;
        f.state.lockPositionMoved = true;
        f.frame.primaryX = 503;
        multistar_refine::Result r = f.Run(0.5, 0.); // within 2 sigma
        EXPECT_FALSE(r.refined);
        EXPECT_FALSE(f.state.stabilizing);
        EXPECT_FALSE(f.state.lockPositionMoved);
        ASSERT_EQ(f.sky.calls.size(), 2u);
        EXPECT_TRUE(f.sky.calls[0].verbose);
        // at the expected position when it is valid, else where last found
        EXPECT_DOUBLE_EQ(f.sky.calls[0].x, valid ? 503 + (100 - 500) : 100);
        EXPECT_DOUBLE_EQ(f.Find(1)->referencePoint.X, 103.);
        EXPECT_DOUBLE_EQ(f.Find(1)->referencePoint.Y, 101.);
        EXPECT_FALSE(f.Find(1)->wasLost);
        EXPECT_TRUE(f.Find(2)->wasLost); // not found: keeps its reference
        EXPECT_DOUBLE_EQ(f.Find(2)->referencePoint.X, 200.);
        EXPECT_EQ(f.log[1], "MultiStar: updating star positions after lock position change\n");
    }
}
