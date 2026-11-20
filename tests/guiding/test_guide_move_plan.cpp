// guide_move_plan (#142).
//
// How Mount::MoveOffset turns a mount-axis offset into a pulse per axis (PlanOffset), and what
// Scope::MoveAxis does to each pulse before it is sent (PlanAxis): the guiding-disabled refusal,
// the Dec guide mode filter, the max-duration clamp and the limit-reached count. The rules live
// in src/guide_move_plan.cpp; Scope and Mount keep the Debug lines, the alert and the pulse.

#include <gtest/gtest.h>

#include "guide_move_plan.h"

#include <climits>
#include <cmath>
#include <limits>
#include <random>

using namespace guide_move_plan;

namespace
{
// == MOVEOPTS_* in mount.h
constexpr unsigned CALIBRATION_MOVE = 0;
constexpr unsigned GUIDE_STEP = ALGO_RESULT | USE_BLC | GRAPH;
constexpr unsigned DEDUCED_MOVE = ALGO_DEDUCE | USE_BLC | GRAPH;
constexpr unsigned RECOVERY_MOVE = USE_BLC;
constexpr unsigned AO_BUMP = USE_BLC;

AxisPolicy Policy(DecMode mode = DecMode::Auto, int maxRa = 2500, int maxDec = 2500, bool enabled = true)
{
    AxisPolicy p;
    p.guidingEnabled = enabled;
    p.decMode = mode;
    p.maxRaMs = maxRa;
    p.maxDecMs = maxDec;
    return p;
}

// PlanAxis with fresh trackers
AxisPlan Plan(Dir dir, int ms, unsigned opts, const AxisPolicy& p)
{
    LimitTracker ra, dec;
    return PlanAxis(dir, ms, opts, p, ra, dec);
}
} // namespace

// --- PlanOffset: directions ---

TEST(PlanOffset, PositiveXIsWestNegativeXIsEast)
{
    EXPECT_EQ(PlanOffset(1.0, 0.0, 0.005, 0.005).raDir, Dir::West);
    EXPECT_EQ(PlanOffset(-1.0, 0.0, 0.005, 0.005).raDir, Dir::East);
}

TEST(PlanOffset, PositiveYIsSouthNegativeYIsNorth)
{
    EXPECT_EQ(PlanOffset(0.0, 1.0, 0.005, 0.005).decDir, Dir::South);
    EXPECT_EQ(PlanOffset(0.0, -1.0, 0.005, 0.005).decDir, Dir::North);
}

TEST(PlanOffset, ZeroOffsetIsEastAndNorthWithNoPulse)
{
    // MoveOffset sets a direction even for a zero pulse; MoveAxis then sends nothing
    OffsetPlan plan = PlanOffset(0.0, 0.0, 0.005, 0.005);
    EXPECT_EQ(plan.raDir, Dir::East);
    EXPECT_EQ(plan.raMs, 0);
    EXPECT_EQ(plan.decDir, Dir::North);
    EXPECT_EQ(plan.decMs, 0);
}

TEST(PlanOffset, EachAxisUsesItsOwnRate)
{
    OffsetPlan plan = PlanOffset(-1.0, 2.0, 0.01, 0.004);
    EXPECT_EQ(plan.raDir, Dir::East);
    EXPECT_EQ(plan.raMs, 100);
    EXPECT_EQ(plan.decDir, Dir::South);
    EXPECT_EQ(plan.decMs, 500);
}

// --- PulseMs: ROUND(fabs(d / rate)) ---

TEST(PulseMs, RoundsToNearestNotDown)
{
    EXPECT_EQ(PulseMs(0.5, 0.005), 100);
    EXPECT_EQ(PulseMs(0.0025, 0.005), 1); // 0.5 ms rounds up
    EXPECT_EQ(PulseMs(0.00249, 0.005), 0);
    EXPECT_EQ(PulseMs(0.00751, 0.005), 2);
}

TEST(PulseMs, UsesTheMagnitudeOfDistanceAndRate)
{
    EXPECT_EQ(PulseMs(-0.5, 0.005), 100);
    EXPECT_EQ(PulseMs(0.5, -0.005), 100);
    EXPECT_EQ(PulseMs(-0.5, -0.005), 100);
}

TEST(PulseMs, MatchesTheRoundExpressionItReplaced)
{
    std::mt19937_64 rng(142);
    std::uniform_real_distribution<double> dist(-50.0, 50.0);
    std::uniform_real_distribution<double> rate(0.0005, 0.05);
    for (int i = 0; i < 100000; i++)
    {
        double d = dist(rng);
        double r = rate(rng);
        ASSERT_EQ(PulseMs(d, r), ROUND(fabs(d / r))) << "d=" << d << " r=" << r;
    }
}

TEST(PulseMs, ZeroRateGivesIntMaxInsteadOfUndefinedBehaviour)
{
    // A zero xRate (or one Dec compensation made tiny) used to cast inf to int, which is
    // undefined; arm64 saturates to INT_MAX, and that is now the defined result everywhere
    EXPECT_EQ(PulseMs(1.0, 0.0), INT_MAX);
    EXPECT_EQ(PulseMs(-1.0, 0.0), INT_MAX);
    EXPECT_EQ(PulseMs(1.0, 1e-300), INT_MAX);
    EXPECT_EQ(PulseMs(std::numeric_limits<double>::infinity(), 0.005), INT_MAX);
    EXPECT_EQ(PulseMs(3e9, 1.0), INT_MAX);
}

TEST(PulseMs, NoOffsetAtZeroRateGivesNoPulse)
{
    // 0 / 0 is NaN; arm64 casts it to 0
    EXPECT_EQ(PulseMs(0.0, 0.0), 0);
    EXPECT_EQ(PulseMs(std::numeric_limits<double>::quiet_NaN(), 0.005), 0);
}

TEST(PulseMs, LargestPulseThatFitsIsExact)
{
    EXPECT_EQ(PulseMs(2147483646.0, 1.0), 2147483646);
    EXPECT_EQ(PulseMs(2147483646.6, 1.0), INT_MAX);
    EXPECT_EQ(PulseMs(2147483647.0, 1.0), INT_MAX);
}

// --- PlanAxis: which moves are filtered and clamped ---

TEST(PlanAxis, GuideStepIsClampedToMaxRa)
{
    AxisPlan plan = Plan(Dir::West, 4000, GUIDE_STEP, Policy(DecMode::Auto, 2500));
    EXPECT_FALSE(plan.refused);
    EXPECT_EQ(plan.ms, 2500);
    EXPECT_TRUE(plan.limited);
    EXPECT_FALSE(plan.alert);
}

TEST(PlanAxis, DeducedMoveIsClampedLikeAGuideStep)
{
    AxisPlan plan = Plan(Dir::East, 4000, DEDUCED_MOVE, Policy(DecMode::Auto, 2500));
    EXPECT_EQ(plan.ms, 2500);
    EXPECT_TRUE(plan.limited);
}

TEST(PlanAxis, GuideStepAtTheMaxIsNotLimited)
{
    AxisPlan plan = Plan(Dir::West, 2500, GUIDE_STEP, Policy(DecMode::Auto, 2500));
    EXPECT_EQ(plan.ms, 2500);
    EXPECT_FALSE(plan.limited);
}

TEST(PlanAxis, DecGuideStepIsClampedToMaxDec)
{
    // MoveOffset adds the backlash pulse before MoveAxis, so 2000 ms + 800 ms BLC is a 2800 ms
    // request and is clamped as a whole
    AxisPlan plan = Plan(Dir::North, 2000 + 800, GUIDE_STEP, Policy(DecMode::Auto, 2500, 2500));
    EXPECT_EQ(plan.ms, 2500);
    EXPECT_TRUE(plan.limited);
}

TEST(PlanAxis, RecoveryMoveBypassesTheClamp)
{
    // pinned: a recovery move (and an AO bump) is sent as asked
    AxisPlan plan = Plan(Dir::West, 4000, RECOVERY_MOVE, Policy(DecMode::Auto, 2500));
    EXPECT_EQ(plan.ms, 4000);
    EXPECT_FALSE(plan.limited);
    EXPECT_EQ(Plan(Dir::North, 4000, AO_BUMP, Policy(DecMode::Auto, 2500, 1000)).ms, 4000);
}

TEST(PlanAxis, CalibrationMoveBypassesTheClampAndTheDecMode)
{
    // MOVEOPTS_CALIBRATION_MOVE has no bits, so no calibration pulse is clamped or filtered
    AxisPlan plan = Plan(Dir::South, 2000, CALIBRATION_MOVE, Policy(DecMode::North, 2500, 1000));
    EXPECT_EQ(plan.ms, 2000);
    EXPECT_FALSE(plan.limited);
    EXPECT_FALSE(plan.zeroedByDecMode);
    EXPECT_EQ(Plan(Dir::North, 2000, CALIBRATION_MOVE, Policy(DecMode::None, 2500, 1000)).ms, 2000);
    EXPECT_EQ(Plan(Dir::West, 5000, CALIBRATION_MOVE, Policy(DecMode::Auto, 1000)).ms, 5000);
}

TEST(PlanAxis, NoDirectionPassesThrough)
{
    AxisPlan plan = Plan(Dir::None, 4000, GUIDE_STEP, Policy(DecMode::None, 1000, 1000));
    EXPECT_EQ(plan.ms, 4000);
    EXPECT_FALSE(plan.limited);
    EXPECT_FALSE(plan.zeroedByDecMode);
}

// --- PlanAxis: Dec guide mode ---

TEST(PlanAxis, DecNorthZeroesSouthGuideStepsOnly)
{
    AxisPlan south = Plan(Dir::South, 500, GUIDE_STEP, Policy(DecMode::North));
    EXPECT_EQ(south.ms, 0);
    EXPECT_TRUE(south.zeroedByDecMode);
    EXPECT_FALSE(south.limited);

    AxisPlan north = Plan(Dir::North, 500, GUIDE_STEP, Policy(DecMode::North));
    EXPECT_EQ(north.ms, 500);
    EXPECT_FALSE(north.zeroedByDecMode);
}

TEST(PlanAxis, DecSouthZeroesNorthGuideStepsOnly)
{
    EXPECT_EQ(Plan(Dir::North, 500, GUIDE_STEP, Policy(DecMode::South)).ms, 0);
    EXPECT_EQ(Plan(Dir::South, 500, GUIDE_STEP, Policy(DecMode::South)).ms, 500);
}

TEST(PlanAxis, DecNoneZeroesBothDirectionsButNotRa)
{
    EXPECT_EQ(Plan(Dir::North, 500, GUIDE_STEP, Policy(DecMode::None)).ms, 0);
    EXPECT_EQ(Plan(Dir::South, 500, DEDUCED_MOVE, Policy(DecMode::None)).ms, 0);
    EXPECT_EQ(Plan(Dir::West, 500, GUIDE_STEP, Policy(DecMode::None)).ms, 500);
    EXPECT_EQ(Plan(Dir::East, 500, GUIDE_STEP, Policy(DecMode::None)).ms, 500);
}

TEST(PlanAxis, DecAutoZeroesNothing)
{
    EXPECT_EQ(Plan(Dir::North, 500, GUIDE_STEP, Policy(DecMode::Auto)).ms, 500);
    EXPECT_EQ(Plan(Dir::South, 500, GUIDE_STEP, Policy(DecMode::Auto)).ms, 500);
}

TEST(PlanAxis, SouthRecoveryMoveIsNotFilteredInDecNorth)
{
    // pinned: only guide steps and deduced moves honour the Dec guide mode
    AxisPlan plan = Plan(Dir::South, 500, RECOVERY_MOVE, Policy(DecMode::North));
    EXPECT_EQ(plan.ms, 500);
    EXPECT_FALSE(plan.zeroedByDecMode);
}

TEST(PlanAxis, ZeroedStepIsNotLimitedEvenWithMaxDecZero)
{
    AxisPlan plan = Plan(Dir::South, 3000, GUIDE_STEP, Policy(DecMode::North, 2500, 0));
    EXPECT_EQ(plan.ms, 0);
    EXPECT_TRUE(plan.zeroedByDecMode);
    EXPECT_FALSE(plan.limited);
}

// --- PlanAxis: guiding disabled ---

TEST(PlanAxis, GuidingDisabledRefusesGuideSteps)
{
    AxisPlan plan = Plan(Dir::West, 500, GUIDE_STEP, Policy(DecMode::Auto, 2500, 2500, false));
    EXPECT_TRUE(plan.refused);
    EXPECT_EQ(plan.ms, 0);
    EXPECT_FALSE(plan.limited);
}

TEST(PlanAxis, GuidingDisabledRefusesCalibrationAndRecoveryMoves)
{
    // pinned: MOVEOPTS_CALIBRATION_MOVE lacks MOVEOPT_MANUAL
    EXPECT_TRUE(Plan(Dir::West, 500, CALIBRATION_MOVE, Policy(DecMode::Auto, 2500, 2500, false)).refused);
    EXPECT_TRUE(Plan(Dir::North, 500, RECOVERY_MOVE, Policy(DecMode::Auto, 2500, 2500, false)).refused);
}

TEST(PlanAxis, GuidingDisabledLetsManualMovesThrough)
{
    AxisPlan plan = Plan(Dir::West, 4000, MANUAL, Policy(DecMode::Auto, 2500, 2500, false));
    EXPECT_FALSE(plan.refused);
    EXPECT_EQ(plan.ms, 4000);
    EXPECT_FALSE(plan.limited);

    // a manual move flagged as a guide step is still clamped
    AxisPlan clamped = Plan(Dir::West, 4000, MANUAL | ALGO_RESULT, Policy(DecMode::Auto, 2500, 2500, false));
    EXPECT_FALSE(clamped.refused);
    EXPECT_EQ(clamped.ms, 2500);
    EXPECT_TRUE(clamped.limited);
}

TEST(PlanAxis, RefusedMoveLeavesTheTrackersAlone)
{
    LimitTracker ra, dec;
    PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec);
    PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec);
    ASSERT_EQ(ra.lastLimited, Dir::West);
    ASSERT_EQ(ra.count, 1);

    PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(DecMode::Auto, 2500, 2500, false), ra, dec);
    EXPECT_EQ(ra.lastLimited, Dir::West);
    EXPECT_EQ(ra.count, 1);
}

// --- PlanAxis: limit-reached count ---

TEST(PlanAxis, SixthClampInARowInOneDirectionRaisesTheAlert)
{
    // The first clamp only records the direction; each later one in the same direction counts,
    // and the count reaching LIMIT_REACHED_WARN_COUNT (5) alerts, so the 6th clamp in a row is
    // the first to alert. Scope::AlertLimitReached shows it at most every 30 s.
    LimitTracker ra, dec;
    for (int i = 1; i <= 5; i++)
    {
        AxisPlan plan = PlanAxis(Dir::North, 4000, GUIDE_STEP, Policy(), ra, dec);
        EXPECT_TRUE(plan.limited);
        EXPECT_FALSE(plan.alert) << "clamp " << i;
        EXPECT_EQ(dec.count, i - 1);
        EXPECT_EQ(dec.lastLimited, Dir::North);
    }
    EXPECT_TRUE(PlanAxis(Dir::North, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
    EXPECT_TRUE(PlanAxis(Dir::North, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
    EXPECT_EQ(ra.lastLimited, Dir::None);
    EXPECT_EQ(ra.count, 0);
}

TEST(PlanAxis, ClampInTheOtherDirectionRestartsTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 5; i++)
        PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec);
    ASSERT_EQ(ra.count, 4);

    AxisPlan east = PlanAxis(Dir::East, 4000, GUIDE_STEP, Policy(), ra, dec);
    EXPECT_TRUE(east.limited);
    EXPECT_FALSE(east.alert);
    EXPECT_EQ(ra.count, 0);
    EXPECT_EQ(ra.lastLimited, Dir::East);
}

TEST(PlanAxis, UnclampedGuideStepRestartsTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 5; i++)
        PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec);
    ASSERT_EQ(dec.count, 4);

    AxisPlan small = PlanAxis(Dir::South, 100, GUIDE_STEP, Policy(), ra, dec);
    EXPECT_FALSE(small.limited);
    EXPECT_EQ(dec.count, 0);
    EXPECT_EQ(dec.lastLimited, Dir::None);

    // so the next five clamps do not alert
    for (int i = 0; i < 5; i++)
        EXPECT_FALSE(PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
}

TEST(PlanAxis, StepZeroedByDecModeRestartsTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 5; i++)
        PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec);
    PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(DecMode::North), ra, dec);
    EXPECT_EQ(dec.count, 0);
    EXPECT_EQ(dec.lastLimited, Dir::None);
}

TEST(PlanAxis, MovesThatAreNotGuideStepsDoNotTouchTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 5; i++)
    {
        PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec);
        PlanAxis(Dir::West, 100, RECOVERY_MOVE, Policy(), ra, dec);
        PlanAxis(Dir::East, 100, CALIBRATION_MOVE, Policy(), ra, dec);
    }
    EXPECT_EQ(ra.count, 4);
    EXPECT_TRUE(PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
}

TEST(PlanAxis, RaAndDecCountSeparately)
{
    // MoveOffset plans RA then Dec on every guide step
    LimitTracker ra, dec;
    for (int i = 1; i <= 6; i++)
    {
        AxisPlan r = PlanAxis(Dir::East, 4000, GUIDE_STEP, Policy(), ra, dec);
        AxisPlan d = PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec);
        EXPECT_EQ(r.alert, i == 6);
        EXPECT_EQ(d.alert, i == 6);
    }
    EXPECT_EQ(ra.lastLimited, Dir::East);
    EXPECT_EQ(dec.lastLimited, Dir::South);
}

// --- PlanOffset into PlanAxis ---

TEST(PlanOffsetAndAxis, ZeroRateGuideStepIsClampedAndAlerts)
{
    // A zero RA rate asks for an INT_MAX pulse: a guide step is clamped to maxRa, counted, and
    // alerts on the 6th step, like any runaway
    LimitTracker ra, dec;
    for (int i = 1; i <= 6; i++)
    {
        OffsetPlan offset = PlanOffset(1.0, 0.0, 0.0, 0.005);
        ASSERT_EQ(offset.raMs, INT_MAX);
        AxisPlan plan = PlanAxis(offset.raDir, offset.raMs, GUIDE_STEP, Policy(DecMode::Auto, 2500), ra, dec);
        EXPECT_EQ(plan.ms, 2500);
        EXPECT_TRUE(plan.limited);
        EXPECT_EQ(plan.alert, i == 6);
    }
}

TEST(PlanOffsetAndAxis, UncalibratedDecRateGivesNoDecPulse)
{
    // An RA-only calibration stores yRate CALIBRATION_RATE_UNCALIBRATED (123e4 px/ms)
    OffsetPlan offset = PlanOffset(0.0, 25.0, 0.005, 123e4);
    EXPECT_EQ(offset.decMs, 0);
}
