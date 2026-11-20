// Tests of the simple guide algorithms' math.
//
// Hysteresis and ResistSwitch (the RA and Dec defaults) run on the REAL
// guide_algorithm_kernels.cpp, the per-frame math GuideAlgorithmHysteresis::result() and
// GuideAlgorithmResistSwitch::result() delegate to (#142). These replaced copies of the
// formulas that had drifted from production: the ResistSwitch copy had neither the overshoot
// veto nor the "not getting worse" check. test_guide_algorithm_kernels holds the wider
// characterization and the differential test against the code before the extraction.
//
// Identity has nothing to extract (result() returns its input), so its one-line formula is
// still mirrored here: src/guide_algorithm_identity.cpp, GuideAlgorithmIdentity::result().

#include <gtest/gtest.h>

#include "guide_algorithm_kernels.h"

#include <cmath>
#include <vector>

using namespace guide_algorithm_kernels;

namespace identity_twin
{

// Mirrors GuideAlgorithmIdentity::result()
inline double result(double input)
{
    return input;
}

} // namespace identity_twin

TEST(IdentityAlgo, ReturnsInputUnchanged)
{
    EXPECT_DOUBLE_EQ(identity_twin::result(0.0), 0.0);
    EXPECT_DOUBLE_EQ(identity_twin::result(1.5), 1.5);
    EXPECT_DOUBLE_EQ(identity_twin::result(-3.7), -3.7);
    EXPECT_DOUBLE_EQ(identity_twin::result(1e6), 1e6);
}

// ---------------------------------------------------------------------------
// Hysteresis
// ---------------------------------------------------------------------------

TEST(HysteresisAlgo, DefaultsAreTheConstructors)
{
    HysteresisParams p;
    EXPECT_EQ(p.minMove, 0.2);
    EXPECT_EQ(p.hysteresis, 0.1);
    EXPECT_EQ(p.aggression, 0.7);
    EXPECT_EQ(HysteresisState().lastMove, 0.);
}

TEST(HysteresisAlgo, ZeroBelowMinMove)
{
    HysteresisState s;
    HysteresisParams p; // minMove 0.2
    EXPECT_EQ(HysteresisResult(s, p, 0.0), 0.0);
    EXPECT_EQ(HysteresisResult(s, p, 0.1), 0.0);
    EXPECT_EQ(HysteresisResult(s, p, 0.19), 0.0);
    EXPECT_EQ(HysteresisResult(s, p, -0.19), 0.0);
}

TEST(HysteresisAlgo, FirstStepBlendsAgainstZeroLastMove)
{
    HysteresisState s;
    HysteresisParams p; // h 0.1, aggression 0.7
    // (1 - 0.1) * 1.0 + 0.1 * 0 = 0.9, times 0.7
    EXPECT_DOUBLE_EQ(HysteresisResult(s, p, 1.0), 0.63);
    EXPECT_DOUBLE_EQ(s.lastMove, 0.63);
}

TEST(HysteresisAlgo, BlendsLastMoveOnSubsequentSteps)
{
    HysteresisState s;
    HysteresisParams p;
    HysteresisResult(s, p, 1.0); // lastMove 0.63
    EXPECT_DOUBLE_EQ(HysteresisResult(s, p, 1.0), (0.9 + 0.1 * 0.63) * 0.7);
}

TEST(HysteresisAlgo, GatedStepClearsLastMove)
{
    HysteresisState s;
    HysteresisParams p;
    HysteresisResult(s, p, 1.0);
    EXPECT_EQ(HysteresisResult(s, p, 0.0), 0.0);
    EXPECT_EQ(s.lastMove, 0.0);
}

TEST(HysteresisAlgo, NegativeInputProducesNegativeOutput)
{
    HysteresisState s;
    HysteresisParams p;
    EXPECT_DOUBLE_EQ(HysteresisResult(s, p, -1.0), -0.63);
}

TEST(HysteresisAlgo, AggressionScalesProportionally)
{
    HysteresisParams p;
    p.aggression = 1.0;
    HysteresisState s1;
    EXPECT_DOUBLE_EQ(HysteresisResult(s1, p, 1.0), 0.9);
    p.aggression = 0.5;
    HysteresisState s2;
    EXPECT_DOUBLE_EQ(HysteresisResult(s2, p, 1.0), 0.45);
    p.aggression = 0.;
    HysteresisState s3;
    EXPECT_EQ(HysteresisResult(s3, p, 1.0), 0.);
}

TEST(HysteresisAlgo, MaxHysteresisIsMostlyLastMove)
{
    HysteresisState s;
    s.lastMove = 5.0;
    HysteresisParams p;
    p.hysteresis = 0.99;
    p.aggression = 1.0;
    EXPECT_DOUBLE_EQ(HysteresisResult(s, p, 1.0), 4.96);
}

TEST(HysteresisAlgo, MinMoveZeroPassesAZeroInputsShareOfTheLastMove)
{
    HysteresisState s;
    HysteresisParams p;
    p.minMove = 0.;
    HysteresisResult(s, p, 1.0);
    EXPECT_DOUBLE_EQ(HysteresisResult(s, p, 0.), 0.1 * 0.63 * 0.7);
}

// ---------------------------------------------------------------------------
// Resist Switch
// ---------------------------------------------------------------------------

namespace
{
ResistSwitchParams NoFastSwitch()
{
    ResistSwitchParams p;
    p.fastSwitchEnabled = false;
    return p;
}

std::vector<double> Feed(ResistSwitchState& s, const ResistSwitchParams& p, const std::vector<double>& inputs)
{
    std::vector<double> out;
    for (double in : inputs)
        out.push_back(ResistSwitchResult(s, p, in).result);
    return out;
}
} // namespace

TEST(ResistSwitchAlgo, DefaultsAreTheConstructors)
{
    ResistSwitchParams p;
    EXPECT_EQ(p.minMove, 0.2);
    EXPECT_EQ(p.aggression, 1.0);
    EXPECT_TRUE(p.fastSwitchEnabled);
    ResistSwitchState s;
    s.currentSide = 1;
    s.history[3] = 2.;
    ResistSwitchReset(s);
    EXPECT_EQ(s.currentSide, 0);
    for (double h : s.history)
        EXPECT_EQ(h, 0.);
    EXPECT_EQ(RESIST_SWITCH_HISTORY_SIZE, 10u);
}

TEST(ResistSwitchAlgo, BelowMinMoveReturnsZero)
{
    ResistSwitchState s;
    ResistSwitchParams p;
    for (double in : { 0.0, 0.1, -0.19 })
    {
        ResistSwitchStep step = ResistSwitchResult(s, p, in);
        EXPECT_EQ(step.result, 0.0);
        EXPECT_EQ(step.veto, ResistSwitchVeto::BelowMinMove);
    }
    EXPECT_EQ(s.history[RESIST_SWITCH_HISTORY_SIZE - 1], -0.19); // still recorded
}

TEST(ResistSwitchAlgo, IsolatedSpikeDoesNotSwitch)
{
    ResistSwitchState s;
    ResistSwitchStep step = ResistSwitchResult(s, NoFastSwitch(), 0.5);
    EXPECT_EQ(step.result, 0.0);
    EXPECT_EQ(step.veto, ResistSwitchVeto::NotCompelling);
    EXPECT_EQ(s.currentSide, 0);
}

TEST(ResistSwitchAlgo, SustainedDirectionSwitches)
{
    ResistSwitchState s;
    ResistSwitchParams p = NoFastSwitch();
    ResistSwitchResult(s, p, 0.5);
    ResistSwitchResult(s, p, 0.5);
    ResistSwitchStep step = ResistSwitchResult(s, p, 0.5);
    EXPECT_EQ(step.result, 0.5);
    EXPECT_EQ(step.veto, ResistSwitchVeto::None);
    EXPECT_TRUE(step.switched);
    EXPECT_EQ(step.switchFrom, 0);
    EXPECT_EQ(step.switchTo, 1);
    EXPECT_EQ(step.decHistory, 3);
    EXPECT_EQ(step.oldest, 0.);
    EXPECT_DOUBLE_EQ(step.newest, 1.5);
    EXPECT_EQ(s.currentSide, 1);
}

TEST(ResistSwitchAlgo, OvershootVetoBlocksASmallReversal)
{
    // On the committed side, a reversal under the fast-switch threshold (3 x minMove = 0.6)
    // is taken for an overshoot and not corrected while the history still points the other way.
    // The old copy of the formula here had no veto and passed -0.4 straight through.
    ResistSwitchState s;
    ResistSwitchParams p; // fast switch on
    Feed(s, p, { 0.5, 0.5, 0.5 });
    ASSERT_EQ(s.currentSide, 1);
    ResistSwitchStep step = ResistSwitchResult(s, p, -0.4);
    EXPECT_EQ(step.result, 0.0);
    EXPECT_EQ(step.veto, ResistSwitchVeto::Overshoot);
    EXPECT_FALSE(step.fastSwitched);
    EXPECT_FALSE(step.switched);
    EXPECT_EQ(s.currentSide, 1);
    // the veto holds until the history turns: decHistory 1, 0 still veto, then -1, -2 are
    // not compelling, then -3 with the newest three worse than the oldest switches
    EXPECT_EQ(Feed(s, p, { -0.4, -0.4, -0.4, -0.4, -0.4 }), (std::vector<double> { 0., 0., 0., 0., -0.4 }));
    EXPECT_EQ(s.currentSide, -1);
}

TEST(ResistSwitchAlgo, OvershootVetoEvenWithAHugeReversalWhenFastSwitchIsOff)
{
    ResistSwitchState s;
    ResistSwitchParams p = NoFastSwitch();
    Feed(s, p, { 0.5, 0.5, 0.5 });
    ResistSwitchStep step = ResistSwitchResult(s, p, -5.0);
    EXPECT_EQ(step.result, 0.0);
    EXPECT_EQ(step.veto, ResistSwitchVeto::Overshoot);
}

TEST(ResistSwitchAlgo, OvershootVetoOnTheFrameTheHistorySwitches)
{
    // decHistory reaches 3 on a frame whose own input is negative: the side switches to +1 and
    // the input is vetoed in the same step
    ResistSwitchState s;
    ResistSwitchParams p = NoFastSwitch();
    Feed(s, p, { 1.0, 2.0, -1.0, 0.5, 0.5 });
    ResistSwitchStep step = ResistSwitchResult(s, p, -0.2);
    EXPECT_TRUE(step.switched);
    EXPECT_EQ(step.switchTo, 1);
    EXPECT_EQ(step.veto, ResistSwitchVeto::Overshoot);
    EXPECT_EQ(step.result, 0.);
    EXPECT_EQ(s.currentSide, 1);
}

TEST(ResistSwitchAlgo, NotGettingWorseHoldsTheSide)
{
    ResistSwitchState s;
    ResistSwitchParams p = NoFastSwitch();
    Feed(s, p, { 0.9, 0.9, 0.9 });
    std::vector<ResistSwitchVeto> vetoes;
    for (int i = 0; i < 8; i++)
        vetoes.push_back(ResistSwitchResult(s, p, -0.5).veto);
    EXPECT_EQ(vetoes,
              (std::vector<ResistSwitchVeto> { ResistSwitchVeto::Overshoot, ResistSwitchVeto::Overshoot,
                                               ResistSwitchVeto::Overshoot, ResistSwitchVeto::NotCompelling,
                                               ResistSwitchVeto::NotCompelling, ResistSwitchVeto::NotGettingWorse,
                                               ResistSwitchVeto::NotGettingWorse, ResistSwitchVeto::None }));
    EXPECT_EQ(s.currentSide, -1);
}

TEST(ResistSwitchAlgo, FastSwitchOnLargeReverseImmediate)
{
    ResistSwitchState s;
    ResistSwitchParams p; // minMove 0.2, thresh 0.6
    s.currentSide = 1;
    ResistSwitchStep step = ResistSwitchResult(s, p, -0.7);
    EXPECT_TRUE(step.fastSwitched);
    EXPECT_EQ(step.thresh, 3.0 * 0.2);
    EXPECT_EQ(step.fastSwitchFrom, 1);
    EXPECT_EQ(step.fastSwitchTo, -1);
    EXPECT_TRUE(step.switched);
    EXPECT_EQ(step.switchFrom, 0);
    EXPECT_EQ(step.result, -0.7);
    EXPECT_EQ(s.currentSide, -1);
    for (unsigned i = 0; i < RESIST_SWITCH_HISTORY_SIZE; i++)
        EXPECT_EQ(s.history[i], i < RESIST_SWITCH_HISTORY_SIZE - 3 ? 0. : -0.7);
}

TEST(ResistSwitchAlgo, AggressionScalesResult)
{
    ResistSwitchState s;
    ResistSwitchParams p = NoFastSwitch();
    p.aggression = 0.5;
    EXPECT_EQ(Feed(s, p, { 0.5, 0.5, 0.5 }), (std::vector<double> { 0., 0., 0.25 }));
}

TEST(ResistSwitchAlgo, SignOfNaNIsZero)
{
    EXPECT_EQ(ResistSwitchSign(2.), 1);
    EXPECT_EQ(ResistSwitchSign(-1e-300), -1);
    EXPECT_EQ(ResistSwitchSign(0.), 0);
    EXPECT_EQ(ResistSwitchSign(-0.), 0);
    EXPECT_EQ(ResistSwitchSign(std::nan("")), 0);
}
