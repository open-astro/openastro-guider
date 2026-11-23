// guide_move_plan (#142).
//
// How Mount::MoveOffset turns a mount-axis offset into a pulse per axis (PlanOffset), and what
// Scope::MoveAxis does to each pulse before it is sent (PlanAxis): the guiding-disabled refusal,
// the Dec guide mode filter, the max-duration clamp and the limit-reached count; and when
// Scope::AlertLimitReached raises the limit-reached alert (LimitAlertThrottle) and the clock it
// runs on (AlertClockS); when ScopeAlpaca::Guide raises the PulseGuide-failed alert
// (PulseFailedAlertThrottle); what a dither's fast-recenter step moved (MovedPx,
// AfterRecenterStep); the result Mount::MoveOffset reports for its two pulses (SendDecAfter,
// OffsetResult); and the Dec backlash compensation BacklashComp::ApplyBacklashComp adds
// (PlanBacklashComp). The rules live in src/guide_move_plan.cpp; Scope, Mount and BacklashComp
// keep the Debug lines, the alert, the BLC history and the pulse.

#include <gtest/gtest.h>

#include "guide_move_plan.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <random>
#include <vector>

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

TEST(PlanAxis, RecoveryMoveIsNotClampedToTheMaxDuration)
{
    // A dither's fast recenter sizes each step to 70% of the search region and an AO bump to the
    // bump step, so neither is held to the max duration a guide step is (#152)
    AxisPlan plan = Plan(Dir::West, 4000, RECOVERY_MOVE, Policy(DecMode::Auto, 2500));
    EXPECT_EQ(plan.ms, 4000);
    EXPECT_FALSE(plan.limited);
    EXPECT_FALSE(plan.capped);
    EXPECT_EQ(Plan(Dir::North, 4000, AO_BUMP, Policy(DecMode::Auto, 2500, 1000)).ms, 4000);
}

// --- PlanAxis: the MAX_PULSE_MS ceiling (#152) ---

TEST(PlanAxis, MaxPulseIsUnderTheAlpacaHttpTimeout)
{
    // AlpacaClient's 30 s timeout for a PulseGuide that blocks; #141's RA_STEP_MAX_MS is the same
    EXPECT_EQ(MAX_PULSE_MS, 25000);
}

TEST(PlanAxis, RecoveryMoveOverTheMaxPulseIsCapped)
{
    AxisPlan plan = Plan(Dir::West, 30000, RECOVERY_MOVE, Policy(DecMode::Auto, 2500));
    EXPECT_EQ(plan.ms, MAX_PULSE_MS);
    EXPECT_TRUE(plan.limited);
    EXPECT_TRUE(plan.capped);
    EXPECT_FALSE(plan.alert);

    AxisPlan atTheMax = Plan(Dir::East, MAX_PULSE_MS, RECOVERY_MOVE, Policy(DecMode::Auto, 2500));
    EXPECT_EQ(atTheMax.ms, MAX_PULSE_MS);
    EXPECT_FALSE(atTheMax.limited);
    EXPECT_FALSE(atTheMax.capped);
}

TEST(PlanAxis, AoBumpOverTheMaxPulseIsCapped)
{
    AxisPlan plan = Plan(Dir::South, INT_MAX, AO_BUMP, Policy(DecMode::Auto, 2500, 1000));
    EXPECT_EQ(plan.ms, MAX_PULSE_MS);
    EXPECT_TRUE(plan.limited);
    EXPECT_TRUE(plan.capped);
    EXPECT_FALSE(plan.alert);
}

TEST(PlanAxis, GuideStepIsCappedWhateverTheMaxDuration)
{
    // set_guide_limits accepts up to 1e6 ms
    AxisPlan plan = Plan(Dir::West, INT_MAX, GUIDE_STEP, Policy(DecMode::Auto, 1000000, 1000000));
    EXPECT_EQ(plan.ms, MAX_PULSE_MS);
    EXPECT_TRUE(plan.limited);
    EXPECT_TRUE(plan.capped);

    AxisPlan deduced = Plan(Dir::North, 40000, DEDUCED_MOVE, Policy(DecMode::Auto, 2500, 30000));
    EXPECT_EQ(deduced.ms, MAX_PULSE_MS);
    EXPECT_TRUE(deduced.capped);

    // under the ceiling the max duration clamps, as before
    AxisPlan clamped = Plan(Dir::West, 40000, GUIDE_STEP, Policy(DecMode::Auto, 2500));
    EXPECT_EQ(clamped.ms, 2500);
    EXPECT_TRUE(clamped.limited);
    EXPECT_FALSE(clamped.capped);
    AxisPlan atTheMax = Plan(Dir::West, 40000, GUIDE_STEP, Policy(DecMode::Auto, MAX_PULSE_MS));
    EXPECT_EQ(atTheMax.ms, MAX_PULSE_MS);
    EXPECT_FALSE(atTheMax.capped);
}

TEST(PlanAxis, CappedGuideStepsAreCountedLikeClampedOnes)
{
    LimitTracker ra, dec;
    PlanAxis(Dir::West, INT_MAX, GUIDE_STEP, Policy(DecMode::Auto, 1000000), ra, dec);
    PlanAxis(Dir::West, INT_MAX, GUIDE_STEP, Policy(DecMode::Auto, 1000000), ra, dec);
    EXPECT_EQ(ra.lastLimited, Dir::West);
    EXPECT_EQ(ra.count, 2);
}

TEST(PlanAxis, CappedRecoveryMoveLeavesTheTrackersAlone)
{
    LimitTracker ra, dec;
    PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec);
    PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec);
    ASSERT_EQ(ra.count, 2);

    EXPECT_TRUE(PlanAxis(Dir::West, INT_MAX, RECOVERY_MOVE, Policy(), ra, dec).capped);
    EXPECT_TRUE(PlanAxis(Dir::North, INT_MAX, AO_BUMP, Policy(), ra, dec).capped);
    EXPECT_EQ(ra.lastLimited, Dir::West);
    EXPECT_EQ(ra.count, 2);
    EXPECT_EQ(dec.lastLimited, Dir::None);
    EXPECT_EQ(dec.count, 0);
}

TEST(PlanAxis, CalibrationAndManualMovesAreNotCapped)
{
    // Their length is the caller's: calibration_duration (#141 does not shrink a step over 25 s)
    // and guide_pulse, which refuses more than 60000 ms itself (#137)
    AxisPlan calibration = Plan(Dir::West, 30000, CALIBRATION_MOVE, Policy());
    EXPECT_EQ(calibration.ms, 30000);
    EXPECT_FALSE(calibration.capped);
    AxisPlan manual = Plan(Dir::South, 60000, MANUAL, Policy(DecMode::Auto, 2500, 2500, false));
    EXPECT_EQ(manual.ms, 60000);
    EXPECT_FALSE(manual.capped);
}

TEST(PlanAxis, NoMoveSizedFromAnOffsetExceedsTheMaxPulse)
{
    // Every combination of move bits: one with ALGO_RESULT, ALGO_DEDUCE or USE_BLC (a guide step,
    // deduced move, recovery move or AO bump) is at most MAX_PULSE_MS; the rest pass as asked
    const Dir dirs[] = { Dir::North, Dir::South, Dir::East, Dir::West };
    const int requests[] = { 0, 1, 2500, 24999, 25000, 25001, 60000, 1000000, INT_MAX };
    const int maxima[] = { 0, 2500, 25000, 30000, 1000000 };
    for (unsigned opts = 0; opts < 32; opts++)
    {
        bool fromAnOffset = (opts & (ALGO_RESULT | ALGO_DEDUCE | USE_BLC)) != 0;
        for (Dir dir : dirs)
            for (int ms : requests)
                for (int maxMs : maxima)
                {
                    AxisPlan plan = Plan(dir, ms, opts, Policy(DecMode::Auto, maxMs, maxMs));
                    if (fromAnOffset)
                        EXPECT_LE(plan.ms, MAX_PULSE_MS) << "opts " << opts << " ms " << ms << " max " << maxMs;
                    else
                        EXPECT_EQ(plan.ms, ms) << "opts " << opts << " ms " << ms;
                    EXPECT_GE(plan.ms, 0);
                    // a guide step's max duration clamps first when it is the lower
                    bool guideStep = (opts & (ALGO_RESULT | ALGO_DEDUCE)) != 0;
                    EXPECT_EQ(plan.capped, fromAnOffset && ms > MAX_PULSE_MS && (!guideStep || maxMs > MAX_PULSE_MS))
                        << "opts " << opts << " ms " << ms << " max " << maxMs;
                }
    }
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

TEST(PlanAxis, RecoveryAndBumpMovesIgnoreTheDecGuideMode)
{
    // On purpose, as upstream (#152). The only recovery move is a dither's fast recenter, and PHD2
    // dithers in Dec with a North or South guide mode: PhdController switches the mode to Auto for
    // the settle (or dithers RA only when there is no settle, or the mode is Off), so the filter
    // would only ever zero a recenter step that outlives the settle, stranding the star on the far
    // side of the new lock position where one-way guide steps cannot bring it back. An AO bump
    // unloads the AO, which corrects both ways whatever the mount's Dec mode; a zeroed bump lets
    // the AO run into its limit.
    const DecMode modes[] = { DecMode::None, DecMode::North, DecMode::South };
    for (DecMode mode : modes)
    {
        AxisPlan south = Plan(Dir::South, 500, RECOVERY_MOVE, Policy(mode));
        EXPECT_EQ(south.ms, 500);
        EXPECT_FALSE(south.zeroedByDecMode);
        EXPECT_EQ(Plan(Dir::North, 500, AO_BUMP, Policy(mode)).ms, 500);
    }
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
    ASSERT_EQ(ra.count, 2);

    PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(DecMode::Auto, 2500, 2500, false), ra, dec);
    EXPECT_EQ(ra.lastLimited, Dir::West);
    EXPECT_EQ(ra.count, 2);
}

// --- PlanAxis: limit-reached count ---

TEST(PlanAxis, FifthClampInARowInOneDirectionRaisesTheAlert)
{
    // The count is the clamped guide steps in a row in one direction, the first included, and
    // it reaching LIMIT_REACHED_WARN_COUNT (5) alerts, so the 5th clamp in a row is the first to
    // alert (#153; it was the 6th). Scope::AlertLimitReached shows it at most every 30 s.
    LimitTracker ra, dec;
    for (int i = 1; i <= 4; i++)
    {
        AxisPlan plan = PlanAxis(Dir::North, 4000, GUIDE_STEP, Policy(), ra, dec);
        EXPECT_TRUE(plan.limited);
        EXPECT_FALSE(plan.alert) << "clamp " << i;
        EXPECT_EQ(dec.count, i);
        EXPECT_EQ(dec.lastLimited, Dir::North);
    }
    EXPECT_EQ(LIMIT_REACHED_WARN_COUNT, 5);
    EXPECT_TRUE(PlanAxis(Dir::North, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
    EXPECT_TRUE(PlanAxis(Dir::North, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
    EXPECT_EQ(ra.lastLimited, Dir::None);
    EXPECT_EQ(ra.count, 0);
}

TEST(PlanAxis, ClampInTheOtherDirectionRestartsTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 4; i++)
        PlanAxis(Dir::West, 4000, GUIDE_STEP, Policy(), ra, dec);
    ASSERT_EQ(ra.count, 4);

    AxisPlan east = PlanAxis(Dir::East, 4000, GUIDE_STEP, Policy(), ra, dec);
    EXPECT_TRUE(east.limited);
    EXPECT_FALSE(east.alert);
    EXPECT_EQ(ra.count, 1);
    EXPECT_EQ(ra.lastLimited, Dir::East);

    // the East clamp is the first of a new run, so the 5th East clamp alerts
    for (int i = 2; i <= 4; i++)
        EXPECT_FALSE(PlanAxis(Dir::East, 4000, GUIDE_STEP, Policy(), ra, dec).alert) << "clamp " << i;
    EXPECT_TRUE(PlanAxis(Dir::East, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
}

TEST(PlanAxis, UnclampedGuideStepRestartsTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 4; i++)
        PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec);
    ASSERT_EQ(dec.count, 4);

    AxisPlan small = PlanAxis(Dir::South, 100, GUIDE_STEP, Policy(), ra, dec);
    EXPECT_FALSE(small.limited);
    EXPECT_EQ(dec.count, 0);
    EXPECT_EQ(dec.lastLimited, Dir::None);

    // so the next four clamps do not alert, and the fifth does
    for (int i = 0; i < 4; i++)
        EXPECT_FALSE(PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
    EXPECT_TRUE(PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec).alert);
}

TEST(PlanAxis, StepZeroedByDecModeRestartsTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 4; i++)
        PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(), ra, dec);
    AxisPlan zeroed = PlanAxis(Dir::South, 4000, GUIDE_STEP, Policy(DecMode::North), ra, dec);
    EXPECT_FALSE(zeroed.alert);
    EXPECT_EQ(dec.count, 0);
    EXPECT_EQ(dec.lastLimited, Dir::None);
}

TEST(PlanAxis, MovesThatAreNotGuideStepsDoNotTouchTheCount)
{
    LimitTracker ra, dec;
    for (int i = 0; i < 4; i++)
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
        EXPECT_EQ(r.alert, i >= 5);
        EXPECT_EQ(d.alert, i >= 5);
    }
    EXPECT_EQ(ra.lastLimited, Dir::East);
    EXPECT_EQ(dec.lastLimited, Dir::South);
}

// --- PlanOffset into PlanAxis ---

TEST(PlanOffsetAndAxis, ZeroRateGuideStepIsClampedAndAlerts)
{
    // A zero RA rate asks for an INT_MAX pulse: a guide step is clamped to maxRa, counted, and
    // alerts from the 5th step, like any runaway
    LimitTracker ra, dec;
    for (int i = 1; i <= 6; i++)
    {
        OffsetPlan offset = PlanOffset(1.0, 0.0, 0.0, 0.005);
        ASSERT_EQ(offset.raMs, INT_MAX);
        AxisPlan plan = PlanAxis(offset.raDir, offset.raMs, GUIDE_STEP, Policy(DecMode::Auto, 2500), ra, dec);
        EXPECT_EQ(plan.ms, 2500);
        EXPECT_TRUE(plan.limited);
        EXPECT_EQ(plan.alert, i >= 5);
    }
}

TEST(PlanOffsetAndAxis, UncalibratedDecRateGivesNoDecPulse)
{
    // An RA-only calibration stores yRate CALIBRATION_RATE_UNCALIBRATED (123e4 px/ms)
    OffsetPlan offset = PlanOffset(0.0, 25.0, 0.005, 123e4);
    EXPECT_EQ(offset.decMs, 0);
}

TEST(PlanOffsetAndAxis, ZeroRateRecoveryMoveIsCappedNotSentAsIntMax)
{
    // A dither recenter or an AO bump at a zero rate asked for INT_MAX ms (24 days) and sent it (#152)
    OffsetPlan offset = PlanOffset(-1.0, 1.0, 0.0, 0.0);
    ASSERT_EQ(offset.raMs, INT_MAX);
    ASSERT_EQ(offset.decMs, INT_MAX);
    LimitTracker ra, dec;
    AxisPlan raPlan = PlanAxis(offset.raDir, offset.raMs, RECOVERY_MOVE, Policy(), ra, dec);
    AxisPlan decPlan = PlanAxis(offset.decDir, offset.decMs, AO_BUMP, Policy(), ra, dec);
    EXPECT_EQ(raPlan.ms, MAX_PULSE_MS);
    EXPECT_TRUE(raPlan.capped);
    EXPECT_EQ(decPlan.ms, MAX_PULSE_MS);
    EXPECT_TRUE(decPlan.capped);
}

// --- LimitAlertThrottle: when Scope::AlertLimitReached raises the alert (#164) ---

namespace
{
// A grace period that ended long before the calls of a test
constexpr int64_t NO_GRACE = 0;
} // namespace

TEST(LimitAlertThrottle, FirstAlertOfAnAxisIsRaised)
{
    LimitAlertThrottle throttle;
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Ra, 1000, NO_GRACE));
}

TEST(LimitAlertThrottle, SameAxisIsHeldForThirtySeconds)
{
    LimitAlertThrottle throttle;
    ASSERT_TRUE(throttle.ShouldRaise(Axis::Dec, 1000, NO_GRACE));
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, 1000, NO_GRACE));
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, 1029, NO_GRACE));
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Dec, 1000 + LIMIT_ALERT_THROTTLE_S, NO_GRACE));
}

TEST(LimitAlertThrottle, HeldCallsDoNotExtendTheThrottle)
{
    // Guide steps keep asking every couple of seconds; the throttle counts from the raised alert
    LimitAlertThrottle throttle;
    ASSERT_TRUE(throttle.ShouldRaise(Axis::Ra, 1000, NO_GRACE));
    for (int64_t t = 1002; t < 1030; t += 2)
        EXPECT_FALSE(throttle.ShouldRaise(Axis::Ra, t, NO_GRACE)) << "t=" << t;
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Ra, 1030, NO_GRACE));
}

TEST(LimitAlertThrottle, RaAlertDoesNotSuppressDecAlert)
{
    // One function-static time used to hold a Dec alert for 30 s after an RA one
    LimitAlertThrottle throttle;
    ASSERT_TRUE(throttle.ShouldRaise(Axis::Ra, 1000, NO_GRACE));
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Dec, 1002, NO_GRACE));
    // and each axis keeps its own 30 s
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Ra, 1004, NO_GRACE));
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, 1004, NO_GRACE));
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Ra, 1030, NO_GRACE));
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, 1031, NO_GRACE));
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Dec, 1032, NO_GRACE));
}

TEST(LimitAlertThrottle, DecAlertDoesNotSuppressRaAlert)
{
    LimitAlertThrottle throttle;
    ASSERT_TRUE(throttle.ShouldRaise(Axis::Dec, 1000, NO_GRACE));
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Ra, 1010, NO_GRACE));
}

TEST(LimitAlertThrottle, NothingIsRaisedDuringTheGracePeriod)
{
    // DeferPulseLimitAlertCheck: 120 s after a calibration or a guide start
    LimitAlertThrottle throttle;
    const int64_t graceEnd = 1120;
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Ra, 1000, graceEnd));
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, 1119, graceEnd));
}

TEST(LimitAlertThrottle, CallSuppressedByTheGracePeriodDoesNotRestartTheThrottle)
{
    // The time used to be restarted before the grace period was checked, so the guide step that
    // asked just before the grace period ended held the first alert after it for 30 s more
    LimitAlertThrottle throttle;
    const int64_t graceEnd = 1120;
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Ra, 1000, graceEnd));
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Ra, 1110, graceEnd));
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Ra, graceEnd, graceEnd));
}

TEST(LimitAlertThrottle, GuideStepsThroughTheGracePeriodDoNotHoldTheFirstAlertAfterIt)
{
    // Clamped guide steps every 2 s from 1001 ask all through the grace period, and the step at
    // its end raises the alert
    LimitAlertThrottle throttle;
    const int64_t graceEnd = 1120;
    for (int64_t t = 1001; t < graceEnd; t += 2)
        EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, t, graceEnd)) << "t=" << t;
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Dec, graceEnd, graceEnd));
}

TEST(LimitAlertThrottle, GracePeriodAfterARaisedAlertKeepsItsThrottle)
{
    // A recalibration after an alert defers the next one; once both have passed it is raised
    LimitAlertThrottle throttle;
    ASSERT_TRUE(throttle.ShouldRaise(Axis::Dec, 1000, NO_GRACE));
    const int64_t graceEnd = 1010;
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, 1005, graceEnd));
    EXPECT_FALSE(throttle.ShouldRaise(Axis::Dec, 1015, graceEnd));
    EXPECT_TRUE(throttle.ShouldRaise(Axis::Dec, 1030, graceEnd));
}

// --- PulseFailedAlertThrottle: when ScopeAlpaca::Guide raises the PulseGuide-failed alert (#188) ---

TEST(PulseFailedAlertThrottle, FirstFailedPulseRaisesTheAlert)
{
    PulseFailedAlertThrottle throttle;
    EXPECT_TRUE(throttle.ShouldRaise(1000));
}

TEST(PulseFailedAlertThrottle, BothAxesFailingEveryFrameRaiseOneAlertEveryThirtySeconds)
{
    // A mount that stops taking pulses: an RA and a Dec pulse fail every 2 s frame. Each failed
    // pulse used to raise its own alert, 12 for 6 frames
    PulseFailedAlertThrottle throttle;
    int raised = 0;
    for (int64_t t = 1000; t < 1000 + 3 * PULSE_FAILED_ALERT_THROTTLE_S; t += 2)
    {
        raised += throttle.ShouldRaise(t) ? 1 : 0; // RA
        raised += throttle.ShouldRaise(t) ? 1 : 0; // Dec
    }
    EXPECT_EQ(raised, 3);
}

TEST(PulseFailedAlertThrottle, HeldForThirtySecondsFromTheRaisedAlert)
{
    PulseFailedAlertThrottle throttle;
    ASSERT_TRUE(throttle.ShouldRaise(1000));
    EXPECT_FALSE(throttle.ShouldRaise(1000));
    for (int64_t t = 1001; t < 1000 + PULSE_FAILED_ALERT_THROTTLE_S; ++t)
        EXPECT_FALSE(throttle.ShouldRaise(t)) << "t=" << t;
    EXPECT_TRUE(throttle.ShouldRaise(1000 + PULSE_FAILED_ALERT_THROTTLE_S));
    EXPECT_FALSE(throttle.ShouldRaise(1001 + PULSE_FAILED_ALERT_THROTTLE_S));
}

TEST(PulseFailedAlertThrottle, FailureAfterAQuietSpellIsRaisedAtOnce)
{
    // Pulses failed, then went through for a while; the next failure is past the 30 s
    PulseFailedAlertThrottle throttle;
    ASSERT_TRUE(throttle.ShouldRaise(1000));
    EXPECT_TRUE(throttle.ShouldRaise(1300));
}

// --- AlertClockS: the clock both alerts' throttles and the grace period run on (#212) ---

namespace
{
int64_t SteadyS()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

TEST(AlertClockS, IsTheSteadyClock)
{
    // The wall clock held the alert back for as long as a step back of the system time (a Pi
    // that boots with a stale clock and then syncs it) and cut the grace period short by a step
    // forward; the monotonic clock is moved by neither
    int64_t before = SteadyS();
    int64_t now = AlertClockS();
    int64_t after = SteadyS();
    EXPECT_GE(now, before);
    EXPECT_LE(now, after);
}

// --- MovedPx: what one axis of a fast-recenter step moved (#165) ---

TEST(MovedPx, WholePulseSentIsTheWholeDistance)
{
    EXPECT_DOUBLE_EQ(MovedPx(7.3, PulseMs(7.3, 0.005), PulseMs(7.3, 0.005), 0.005), 7.3);
    EXPECT_DOUBLE_EQ(MovedPx(-7.3, 1460, 1460, 0.005), 7.3);
}

TEST(MovedPx, PulseCutByTheCeilingCountsOnlyWhatWasSent)
{
    // 150 px at 0.005 px/ms asks for 30 000 ms; PlanAxis sends MAX_PULSE_MS (#152)
    EXPECT_DOUBLE_EQ(MovedPx(150.0, 30000, MAX_PULSE_MS, 0.005), 125.0);
}

TEST(MovedPx, FailedOrRefusedMoveMovedNothing)
{
    // MoveAxis reports amountMoved 0 when the pulse fails, guiding is disabled, or the RA move
    // failed slewing and the Dec move was never asked
    EXPECT_DOUBLE_EQ(MovedPx(4.0, 800, 0, 0.005), 0.0);
    EXPECT_DOUBLE_EQ(MovedPx(-4.0, 800, 0, 0.005), 0.0);
}

TEST(MovedPx, DecBacklashCompensationIsTakenUpBeforeTheStep)
{
    // ApplyBacklashComp adds 1000 ms to the 30 000 ms pulse; the cut 25 000 ms moved 24 000 ms of it
    EXPECT_DOUBLE_EQ(MovedPx(150.0, 31000, MAX_PULSE_MS, 0.005), 120.0);
    // backlash that the sent part did not take up moved nothing
    EXPECT_DOUBLE_EQ(MovedPx(1.0, 200 + 1000, 25, 0.005), 0.0);
}

TEST(MovedPx, AoCutAtItsTravelCountsTheStepsTaken)
{
    // an AO at 0.5 px a step asked for 20 steps stepped 12 before its travel limit
    EXPECT_DOUBLE_EQ(MovedPx(10.0, 20, 12, 0.5), 6.0);
}

TEST(MovedPx, ZeroLengthPulseIsTheWholeDistance)
{
    // a step under half a ms of pulse rounds to 0 ms and is sent as asked
    EXPECT_DOUBLE_EQ(MovedPx(0.002, 0, 0, 0.005), 0.002);
}

// --- AfterRecenterStep: the fast recenter after one of its steps (#165) ---

TEST(AfterRecenterStep, StepThatMovedTheRestEndsTheRecenter)
{
    RecenterProgress p = AfterRecenterStep(3.0, 1.0, 3.0, 0.7, 0);
    EXPECT_NEAR(p.remainingX, 0.0, 1e-12);
    EXPECT_NEAR(p.remainingY, 0.3, 1e-12);
    EXPECT_TRUE(p.done);
}

TEST(AfterRecenterStep, HalfAPixelLeftOnAnAxisKeepsItGoing)
{
    RecenterProgress p = AfterRecenterStep(3.0, 1.0, 2.5, 1.0, 0);
    EXPECT_NEAR(p.remainingX, 0.5, 1e-12);
    EXPECT_FALSE(p.done);
}

TEST(AfterRecenterStep, StepThatMovedNothingIsTriedAgain)
{
    // a one-off failed step (an Alpaca pulse error) is made up by the next step instead of
    // handing the whole dither to the guide algorithm
    RecenterProgress p = AfterRecenterStep(8.0, 2.0, 0.0, 0.0, 0);
    EXPECT_DOUBLE_EQ(p.remainingX, 8.0);
    EXPECT_DOUBLE_EQ(p.remainingY, 2.0);
    EXPECT_EQ(p.stalledSteps, 1);
    EXPECT_FALSE(p.done);
}

TEST(AfterRecenterStep, StepsInARowThatMovedNothingEndTheRecenter)
{
    // a failed or refused step every time: the guide algorithm corrects the dither instead of the
    // recenter sending the same step on every frame
    RecenterProgress p = AfterRecenterStep(8.0, 2.0, 0.0, 0.0, RECENTER_MAX_STALLED_STEPS - 1);
    EXPECT_DOUBLE_EQ(p.remainingX, 8.0);
    EXPECT_DOUBLE_EQ(p.remainingY, 2.0);
    EXPECT_EQ(p.stalledSteps, RECENTER_MAX_STALLED_STEPS);
    EXPECT_TRUE(p.done);
}

TEST(AfterRecenterStep, StepThatMovedRestartsTheStalledCount)
{
    RecenterProgress p = AfterRecenterStep(8.0, 2.0, 3.0, 0.0, RECENTER_MAX_STALLED_STEPS - 1);
    EXPECT_DOUBLE_EQ(p.remainingX, 5.0);
    EXPECT_EQ(p.stalledSteps, 0);
    EXPECT_FALSE(p.done);
}

TEST(AfterRecenterStep, StepThatMovedOneAxisKeepsItGoing)
{
    RecenterProgress p = AfterRecenterStep(8.0, 2.0, 0.0, 2.0, 0);
    EXPECT_DOUBLE_EQ(p.remainingX, 8.0);
    EXPECT_NEAR(p.remainingY, 0.0, 1e-12);
    EXPECT_EQ(p.stalledSteps, 0);
    EXPECT_FALSE(p.done);
}

namespace
{
struct RecenterRun
{
    int steps = 0;
    double movedX = 0.0; // px the mount moved: what each step sent, at the rate
    double movedY = 0.0;
};

// Guider's fast recenter of a dither of (ditherX, ditherY) mount px in steps of at most
// (stepX, stepY), through Mount::MoveOffset (PlanOffset) and Scope::MoveAxis (PlanAxis) as a
// recovery move, until AfterRecenterStep says it is done. Step number failedStep (from 1, 0 for
// none) fails in MoveAxis and sends nothing, as a one-off pulse error does
RecenterRun Recenter(double ditherX, double ditherY, double stepX, double stepY, double xRate, double yRate,
                     const AxisPolicy& policy, int failedStep = 0)
{
    RecenterRun run;
    LimitTracker ra, dec;
    double remainingX = ditherX, remainingY = ditherY;
    int stalledSteps = 0;
    while (run.steps < 100)
    {
        double sx = std::min(remainingX, stepX), sy = std::min(remainingY, stepY);
        ++run.steps;
        OffsetPlan offset = PlanOffset(-sx, -sy, xRate, yRate);
        AxisPlan raPlan = PlanAxis(offset.raDir, offset.raMs, RECOVERY_MOVE, policy, ra, dec);
        AxisPlan decPlan = PlanAxis(offset.decDir, offset.decMs, RECOVERY_MOVE, policy, ra, dec);
        bool failed = run.steps == failedStep;
        int raSent = raPlan.refused || failed ? 0 : raPlan.ms;
        int decSent = decPlan.refused || failed ? 0 : decPlan.ms;
        run.movedX += raSent * xRate;
        run.movedY += decSent * yRate;
        double mx = MovedPx(sx, offset.raMs, raSent, xRate);
        double my = MovedPx(sy, offset.decMs, decSent, yRate);
        RecenterProgress p = AfterRecenterStep(remainingX, remainingY, mx, my, stalledSteps);
        remainingX = p.remainingX;
        remainingY = p.remainingY;
        stalledSteps = p.stalledSteps;
        if (p.done)
            break;
    }
    return run;
}
} // namespace

TEST(Recenter, StepsTheMountSendsWholeMoveTheDither)
{
    // 21 px of RA and 6 px of Dec in steps of (10.5, 3) at 0.005 px/ms: no pulse is cut
    RecenterRun run = Recenter(21.0, 6.0, 10.5, 3.0, 0.005, 0.005, Policy());
    EXPECT_EQ(run.steps, 2);
    EXPECT_NEAR(run.movedX, 21.0, 0.01);
    EXPECT_NEAR(run.movedY, 6.0, 0.01);
}

TEST(Recenter, StepCutByTheCeilingIsMadeUpByAnotherStep)
{
    // At 0.4 px/s a 10.5 px step asks for 26 250 ms and MAX_PULSE_MS moves 10 px of it, so two
    // steps leave 1 px of a 21 px dither; the recenter used to end there as if they had moved 21
    RecenterRun run = Recenter(21.0, 0.0, 10.5, 0.0, 0.0004, 0.005, Policy());
    EXPECT_EQ(run.steps, 3);
    EXPECT_NEAR(run.movedX, 21.0, 0.01);
}

TEST(Recenter, RefusedStepsEndTheRecenterAfterThreeInARow)
{
    // Guiding disabled: MoveAxis refuses every step, and the recenter used to count each as moved
    // until it had counted the whole dither, 10 steps for this one
    RecenterRun run = Recenter(105.0, 30.0, 10.5, 3.0, 0.005, 0.005, Policy(DecMode::Auto, 2500, 2500, false));
    EXPECT_EQ(run.steps, RECENTER_MAX_STALLED_STEPS);
    EXPECT_DOUBLE_EQ(run.movedX, 0.0);
    EXPECT_DOUBLE_EQ(run.movedY, 0.0);
}

TEST(Recenter, OneFailedStepIsMadeUpByTheNextSteps)
{
    // the first of the dither's two steps fails once; the recenter still moves the whole dither
    RecenterRun run = Recenter(21.0, 6.0, 10.5, 3.0, 0.005, 0.005, Policy(), 1);
    EXPECT_EQ(run.steps, 3);
    EXPECT_NEAR(run.movedX, 21.0, 0.01);
    EXPECT_NEAR(run.movedY, 6.0, 0.01);
    // and a one-step dither whose step fails once takes one more step
    run = Recenter(5.0, 2.0, 10.5, 3.0, 0.005, 0.005, Policy(), 1);
    EXPECT_EQ(run.steps, 2);
    EXPECT_NEAR(run.movedX, 5.0, 0.01);
    EXPECT_NEAR(run.movedY, 2.0, 0.01);
}

TEST(Recenter, RecoveryStepIgnoresTheDecGuideModeAsBefore)
{
    // Not changed by #165: a recovery move is not filtered by the Dec guide mode, Off included
    // (pinned in RecoveryAndBumpMovesIgnoreTheDecGuideMode, #162)
    RecenterRun run = Recenter(0.0, 6.0, 0.0, 3.0, 0.005, 0.005, Policy(DecMode::None));
    EXPECT_EQ(run.steps, 2);
    EXPECT_NEAR(run.movedY, 6.0, 0.01);
}

// Mount::MoveOffset's result for its RA and Dec pulses (#181)
TEST(OffsetResult, DecIsSentAfterAnRaThatMoved)
{
    EXPECT_TRUE(SendDecAfter(MoveResult::Ok));
}

TEST(OffsetResult, DecIsNotSentAfterAFailedRa)
{
    // #294: an RA PulseGuide that failed, timed out or was given up on a stop leaves the frame
    // old (a 30 s timeout made it 30 s old), so its Dec pulse is not sent
    EXPECT_FALSE(SendDecAfter(MoveResult::Error));
}

TEST(OffsetResult, FailedRaWithNoDecSentFailsTheMoveOnce)
{
    // The Dec move is not sent, so it reads Ok: the move is the RA's failure, counted once by
    // MyFrame::OnMoveComplete (#213)
    EXPECT_EQ(OffsetResult(MoveResult::Error, MoveResult::Ok), MoveResult::Error);
    EXPECT_EQ(AfterGuideMove(OffsetResult(MoveResult::Error, MoveResult::Ok), 0).inARow, 1);
}

TEST(OffsetResult, DecIsNotSentWhileSlewingOrAtTheAoLimit)
{
    EXPECT_FALSE(SendDecAfter(MoveResult::ErrorSlewing));
    EXPECT_FALSE(SendDecAfter(MoveResult::ErrorAoLimitReached));
}

TEST(OffsetResult, BothAxesMovedIsOk)
{
    EXPECT_EQ(OffsetResult(MoveResult::Ok, MoveResult::Ok), MoveResult::Ok);
}

TEST(OffsetResult, FailedRaIsNotHiddenByAGoodDec)
{
    // An RA pulse error followed by a Dec pulse that went through: the move failed, so
    // MyFrame::OnMoveComplete counts it
    EXPECT_EQ(OffsetResult(MoveResult::Error, MoveResult::Ok), MoveResult::Error);
}

TEST(OffsetResult, FailedDecFailsTheMove)
{
    EXPECT_EQ(OffsetResult(MoveResult::Ok, MoveResult::Error), MoveResult::Error);
    EXPECT_EQ(OffsetResult(MoveResult::Ok, MoveResult::ErrorSlewing), MoveResult::ErrorSlewing);
    EXPECT_EQ(OffsetResult(MoveResult::Ok, MoveResult::ErrorAoLimitReached), MoveResult::ErrorAoLimitReached);
}

TEST(OffsetResult, DecFailureThatStopsGuidingWinsOverAnRaError)
{
    // OnMoveComplete stops guiding on a slew and recenters the AO at its limit: a plain RA
    // error must not hide either
    EXPECT_EQ(OffsetResult(MoveResult::Error, MoveResult::Error), MoveResult::Error);
    EXPECT_EQ(OffsetResult(MoveResult::Error, MoveResult::ErrorSlewing), MoveResult::ErrorSlewing);
    EXPECT_EQ(OffsetResult(MoveResult::Error, MoveResult::ErrorAoLimitReached), MoveResult::ErrorAoLimitReached);
}

TEST(OffsetResult, RaSlewOrAoLimitIsTheResultWhenDecIsNotSent)
{
    EXPECT_EQ(OffsetResult(MoveResult::ErrorSlewing, MoveResult::Ok), MoveResult::ErrorSlewing);
    EXPECT_EQ(OffsetResult(MoveResult::ErrorAoLimitReached, MoveResult::Ok), MoveResult::ErrorAoLimitReached);
}

// Runs results through AfterGuideMove from a count of 0 and returns the last one
static FailedGuideMoves RunGuideMoves(std::initializer_list<MoveResult> results)
{
    FailedGuideMoves failed;
    for (MoveResult r : results)
        failed = AfterGuideMove(r, failed.inARow);
    return failed;
}

TEST(AfterGuideMove, GoodMoveKeepsTheCountAtZero)
{
    FailedGuideMoves failed = AfterGuideMove(MoveResult::Ok, 0);
    EXPECT_EQ(failed.inARow, 0);
    EXPECT_FALSE(failed.stopGuiding);
}

TEST(AfterGuideMove, FailedMoveAddsOne)
{
    FailedGuideMoves failed = AfterGuideMove(MoveResult::Error, 3);
    EXPECT_EQ(failed.inARow, 4);
    EXPECT_FALSE(failed.stopGuiding);
}

TEST(AfterGuideMove, GoodMoveStartsTheCountAgain)
{
    // A single good move resets the count (#213): failures now and then never add up to a stop
    FailedGuideMoves failed = AfterGuideMove(MoveResult::Ok, MAX_FAILED_GUIDE_MOVES - 1);
    EXPECT_EQ(failed.inARow, 0);
    EXPECT_FALSE(failed.stopGuiding);
}

TEST(AfterGuideMove, TenthFailureInARowStopsGuiding)
{
    EXPECT_EQ(MAX_FAILED_GUIDE_MOVES, 10);
    FailedGuideMoves failed;
    for (int i = 1; i < MAX_FAILED_GUIDE_MOVES; ++i)
    {
        failed = AfterGuideMove(MoveResult::Error, failed.inARow);
        EXPECT_EQ(failed.inARow, i);
        EXPECT_FALSE(failed.stopGuiding) << "stopped at failure " << i;
    }
    failed = AfterGuideMove(MoveResult::Error, failed.inARow);
    EXPECT_EQ(failed.inARow, MAX_FAILED_GUIDE_MOVES);
    EXPECT_TRUE(failed.stopGuiding);
}

TEST(AfterGuideMove, FailuresBrokenByAGoodMoveDoNotStop)
{
    // 9 failures, a good move, 9 failures: 18 failed moves, never 10 in a row
    FailedGuideMoves failed;
    for (int run = 0; run < 2; ++run)
    {
        failed = AfterGuideMove(MoveResult::Ok, failed.inARow);
        for (int i = 0; i < MAX_FAILED_GUIDE_MOVES - 1; ++i)
        {
            failed = AfterGuideMove(MoveResult::Error, failed.inARow);
            EXPECT_FALSE(failed.stopGuiding);
        }
    }
    EXPECT_EQ(failed.inARow, MAX_FAILED_GUIDE_MOVES - 1);
}

TEST(AfterGuideMove, EveryErrorKindCounts)
{
    // An RA error the Dec pulse followed is a failed move (#181); OnMoveComplete stops guiding on a
    // slew and restarts it at an AO limit, a guide start resetting the count
    EXPECT_EQ(AfterGuideMove(MoveResult::Error, 0).inARow, 1);
    EXPECT_EQ(AfterGuideMove(OffsetResult(MoveResult::Error, MoveResult::Ok), 0).inARow, 1);
    EXPECT_EQ(AfterGuideMove(MoveResult::ErrorSlewing, 0).inARow, 1);
    EXPECT_EQ(AfterGuideMove(MoveResult::ErrorAoLimitReached, 0).inARow, 1);
}

TEST(AfterGuideMove, MoreThanTenFailuresKeepStopping)
{
    // Past the 10th failure in a row every failure still asks to stop
    FailedGuideMoves failed = RunGuideMoves({ MoveResult::Error, MoveResult::Error, MoveResult::Error, MoveResult::Error,
                                              MoveResult::Error, MoveResult::Error, MoveResult::Error, MoveResult::Error,
                                              MoveResult::Error, MoveResult::Error, MoveResult::Error });
    EXPECT_EQ(failed.inARow, 11);
    EXPECT_TRUE(failed.stopGuiding);
}

// --- PlanBacklashComp: Dec backlash compensation (#142) ---

namespace
{
struct DecPulse
{
    Dir dir;
    int ms;
};

// The Dec half of Mount::MoveOffset for each y offset in turn: PlanOffset's pulse, the backlash
// compensation BacklashComp::ApplyBacklashComp adds, then what Scope::MoveAxis sends of it.
// yRate 0.01 px/ms, so 1 px asks for 100 ms.
std::vector<DecPulse> SendDec(std::initializer_list<double> ys, DecMode mode, unsigned opts = GUIDE_STEP, bool enabled = true,
                              int pulseWidth = 800)
{
    std::vector<DecPulse> sent;
    Dir last = Dir::None;
    LimitTracker ra, dec;
    AxisPolicy policy = Policy(mode, 2500, 2500);
    for (double y : ys)
    {
        OffsetPlan offset = PlanOffset(0.0, y, 0.01, 0.01);
        int requested = offset.decMs;
        BacklashPlan blc = PlanBacklashComp(opts, enabled, pulseWidth, mode, y, last);
        requested += blc.addMs;
        AxisPlan axis = PlanAxis(offset.decDir, requested, opts, policy, ra, dec);
        sent.push_back({ offset.decDir, axis.ms });
    }
    return sent;
}

int SentMs(const std::vector<DecPulse>& sent, Dir dir)
{
    int total = 0;
    for (const DecPulse& p : sent)
        if (p.dir == dir)
            total += p.ms;
    return total;
}
} // namespace

TEST(PlanBacklashComp, FirstMoveAddsNothingAndSetsTheDirection)
{
    Dir last = Dir::None;
    BacklashPlan p = PlanBacklashComp(GUIDE_STEP, true, 800, DecMode::Auto, 1.0, last);
    EXPECT_FALSE(p.skipped);
    EXPECT_FALSE(p.reversal);
    EXPECT_EQ(p.addMs, 0);
    EXPECT_EQ(last, Dir::South);
}

TEST(PlanBacklashComp, ReversalInAutoAddsThePulse)
{
    Dir last = Dir::South;
    BacklashPlan p = PlanBacklashComp(GUIDE_STEP, true, 800, DecMode::Auto, -1.0, last);
    EXPECT_TRUE(p.reversal);
    EXPECT_EQ(p.addMs, 800);
    EXPECT_EQ(last, Dir::North);

    // the same direction again adds nothing
    p = PlanBacklashComp(GUIDE_STEP, true, 800, DecMode::Auto, -0.5, last);
    EXPECT_FALSE(p.reversal);
    EXPECT_EQ(p.addMs, 0);
    EXPECT_EQ(last, Dir::North);
}

TEST(PlanBacklashComp, RecoveryAndDeducedMovesAreCompensatedToo)
{
    for (unsigned opts : { RECOVERY_MOVE, DEDUCED_MOVE })
    {
        Dir last = Dir::North;
        BacklashPlan p = PlanBacklashComp(opts, true, 800, DecMode::Auto, 2.0, last);
        EXPECT_TRUE(p.reversal) << opts;
        EXPECT_EQ(p.addMs, 800) << opts;
        EXPECT_EQ(last, Dir::South) << opts;
    }
}

TEST(PlanBacklashComp, SkippedMovesLeaveTheDirectionAlone)
{
    struct Case
    {
        unsigned opts;
        bool enabled;
        int pulse;
        double y;
    } cases[] = {
        { CALIBRATION_MOVE, true, 800, 1.0 }, // no USE_BLC
        { MANUAL, true, 800, 1.0 },           { GUIDE_STEP, false, 800, 1.0 }, // disabled
        { GUIDE_STEP, true, 0, 1.0 }, // no pulse
        { GUIDE_STEP, true, -5, 1.0 },        { GUIDE_STEP, true, 800, 0.0 }, // no Dec distance
    };
    for (const Case& c : cases)
    {
        Dir last = Dir::North;
        BacklashPlan p = PlanBacklashComp(c.opts, c.enabled, c.pulse, DecMode::Auto, c.y, last);
        EXPECT_TRUE(p.skipped);
        EXPECT_FALSE(p.reversal);
        EXPECT_EQ(p.addMs, 0);
        EXPECT_EQ(last, Dir::North);
    }
}

TEST(PlanBacklashComp, AutoModeCompensatesEachReversalOnce)
{
    // S, N, N, S: the first North and the second South are reversals
    std::vector<DecPulse> sent = SendDec({ 1.0, -1.0, -1.0, 1.0 }, DecMode::Auto);
    ASSERT_EQ(sent.size(), 4u);
    EXPECT_EQ(sent[0].ms, 100);
    EXPECT_EQ(sent[1].ms, 900);
    EXPECT_EQ(sent[2].ms, 100);
    EXPECT_EQ(sent[3].ms, 900);
}

// Scope::SetDecGuideMode disables the compensator for North and South ("no recovery from
// over-shoots"), but set_backlash_comp {Enabled: true}, a Guiding Assistant apply or the profile
// can enable it in those modes. The guide steps the mode filters out are zeroed by MoveAxis after
// the compensator has seen them, so each allowed pulse after one counted as a reversal and got a
// whole compensation pulse more in the direction the mount was already moving.
TEST(PlanBacklashComp, NorthOnlyModeAddsNoPulseToAlternatingRequests)
{
    std::vector<DecPulse> sent = SendDec({ -1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0 }, DecMode::North);
    EXPECT_EQ(SentMs(sent, Dir::South), 0); // filtered by the mode
    EXPECT_EQ(SentMs(sent, Dir::North), 4 * 100); // only what the guide steps asked for
    for (const DecPulse& p : sent)
        EXPECT_EQ(p.ms, p.dir == Dir::North ? 100 : 0);
}

TEST(PlanBacklashComp, SouthOnlyModeAddsNoPulseToAlternatingRequests)
{
    std::vector<DecPulse> sent = SendDec({ 1.0, -1.0, 1.0, -1.0, 1.0 }, DecMode::South);
    EXPECT_EQ(SentMs(sent, Dir::North), 0);
    EXPECT_EQ(SentMs(sent, Dir::South), 3 * 100);
}

TEST(PlanBacklashComp, InertInEveryModeButAuto)
{
    for (DecMode mode : { DecMode::None, DecMode::North, DecMode::South })
        for (unsigned opts : { GUIDE_STEP, DEDUCED_MOVE, RECOVERY_MOVE })
        {
            Dir last = Dir::South;
            BacklashPlan p = PlanBacklashComp(opts, true, 800, mode, -1.0, last);
            EXPECT_TRUE(p.skipped) << static_cast<int>(mode) << " " << opts;
            EXPECT_EQ(p.addMs, 0) << static_cast<int>(mode) << " " << opts;
            EXPECT_EQ(last, Dir::South) << static_cast<int>(mode) << " " << opts;
        }
}

TEST(PlanBacklashComp, RecoveryMoveInNorthModeGetsNoCompensation)
{
    // a dither's fast recenter is not filtered by the Dec guide mode, so a South recovery move is
    // sent; it is not compensated, and does not make the next North guide step a reversal
    Dir last = Dir::North;
    EXPECT_EQ(PlanBacklashComp(RECOVERY_MOVE, true, 800, DecMode::North, 2.0, last).addMs, 0);
    EXPECT_EQ(PlanBacklashComp(GUIDE_STEP, true, 800, DecMode::North, -1.0, last).addMs, 0);
}
