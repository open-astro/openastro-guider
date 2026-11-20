// Characterization of the Hysteresis (RA default) and ResistSwitch (Dec default) guide
// algorithms (#142).
//
// Every test is a typed test that runs on each implementation in algorithm_impls.h: the frozen
// copy of the production code before the extraction (the twin), and production as it is now
// over the real guide_algorithm_kernels.cpp. The tests pin what the guider does
// today, quirks included, so the move of the math into a wx-free kernel can be held to it:
// the defaults, the minMove gate (exclusive for the gate, and ResistSwitch counts only entries
// strictly above it), the hysteresis blend and the setters' ranges and fallbacks; ResistSwitch's
// direction switch after three net entries, its "not getting worse" check, the fast switch on a
// large reversal and the overshoot veto (which the old math twin in test_guide_algorithm_math
// was missing); and the Debug lines each call writes, in order.

#include "algorithm_impls.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace algo_test;

namespace
{
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Inf = std::numeric_limits<double>::infinity();

template<class A>
std::vector<double> Run(A& algo, const std::vector<double>& inputs)
{
    std::vector<double> out;
    for (double in : inputs)
        out.push_back(algo.result(in));
    return out;
}

std::vector<double> Repeat(double v, int n)
{
    return std::vector<double>((size_t) n, v);
}

// The "Throw->" lines of a log, in order: why each vetoed ResistSwitch call returned 0
std::vector<std::string> Throws(const Log& log)
{
    std::vector<std::string> t;
    for (const std::string& line : log)
        if (line.rfind("Throw->", 0) == 0)
            t.push_back(line.substr(7));
    return t;
}

const std::string BELOW = "input < m_minMove";
const std::string WEAK = "not compelling enough";
const std::string NOT_WORSE = "Not getting worse";
const std::string OVERSHOT = "must have overshot -- vetoing move";
} // namespace

// ---------------------------------------------------------------------------
// Hysteresis
// ---------------------------------------------------------------------------

template<class T>
class HysteresisCharacterization : public testing::Test
{
};

using HysteresisImpls = testing::Types<TwinHysteresis, ExtractedHysteresis>;
TYPED_TEST_SUITE(HysteresisCharacterization, HysteresisImpls);

TYPED_TEST(HysteresisCharacterization, ConstructorDefaults)
{
    TypeParam h;
    EXPECT_EQ(h.MinMove(), 0.2);
    EXPECT_EQ(h.Hysteresis(), 0.1);
    EXPECT_EQ(h.Aggression(), 0.7);
    EXPECT_EQ(h.LastMove(), 0.);
    EXPECT_TRUE(h.DebugLog().empty());
}

TYPED_TEST(HysteresisCharacterization, FirstMoveBlendsAgainstZero)
{
    TypeParam h;
    // (1 - 0.1) * 1.0 + 0.1 * 0, times 0.7
    EXPECT_DOUBLE_EQ(h.result(1.0), 0.63);
    EXPECT_DOUBLE_EQ(h.LastMove(), 0.63);
    EXPECT_DOUBLE_EQ(h.result(-1.0), (0.9 * -1.0 + 0.1 * 0.63) * 0.7);
}

TYPED_TEST(HysteresisCharacterization, LogsEachResult)
{
    TypeParam h;
    h.result(1.0);
    h.result(0.05);
    h.result(-2.345);
    EXPECT_EQ(h.DebugLog(),
              (Log { "GuideAlgorithmHysteresis::Result() returns 0.63 from input 1.00\n",
                     "GuideAlgorithmHysteresis::Result() returns 0.00 from input 0.05\n",
                     "GuideAlgorithmHysteresis::Result() returns -1.48 from input -2.35\n" }));
}

TYPED_TEST(HysteresisCharacterization, BelowMinMoveReturnsZeroAndClearsLastMove)
{
    TypeParam h;
    h.result(1.0);
    EXPECT_EQ(h.result(0.1999), 0.);
    EXPECT_EQ(h.LastMove(), 0.);
    EXPECT_EQ(h.result(-0.19), 0.);
    EXPECT_EQ(h.result(0.), 0.);
    // the next move blends against the zero, not against the move before the gate
    EXPECT_DOUBLE_EQ(h.result(1.0), 0.63);
}

TYPED_TEST(HysteresisCharacterization, MinMoveGateIsExclusive)
{
    TypeParam h;
    EXPECT_DOUBLE_EQ(h.result(0.2), 0.9 * 0.2 * 0.7);
    EXPECT_DOUBLE_EQ(h.result(-0.2), (0.9 * -0.2 + 0.1 * (0.9 * 0.2 * 0.7)) * 0.7);
}

TYPED_TEST(HysteresisCharacterization, MinMoveZeroBlendsEvenAZeroInput)
{
    TypeParam h;
    EXPECT_FALSE(h.SetMinMove(0.));
    EXPECT_EQ(h.MinMove(), 0.);
    h.result(1.0);
    // nothing gates a zero input, so the last move's share comes out again
    EXPECT_DOUBLE_EQ(h.result(0.), 0.1 * 0.63 * 0.7);
    EXPECT_DOUBLE_EQ(h.result(0.), 0.1 * (0.1 * 0.63 * 0.7) * 0.7);
}

TYPED_TEST(HysteresisCharacterization, SetMinMoveRejectsNegativeWithTheDefault)
{
    TypeParam h;
    h.SetMinMove(0.5);
    h.result(1.0);
    EXPECT_TRUE(h.SetMinMove(-0.01));
    EXPECT_EQ(h.MinMove(), 0.2);
    EXPECT_DOUBLE_EQ(h.LastMove(), 0.63); // untouched
    EXPECT_EQ(h.DebugLog().back(), "Error->invalid minMove");
}

TYPED_TEST(HysteresisCharacterization, HysteresisZeroIsProportional)
{
    TypeParam h;
    EXPECT_FALSE(h.SetHysteresis(0.));
    for (double in : { 1.0, -3.0, 0.5, 2.25 })
        EXPECT_DOUBLE_EQ(h.result(in), in * 0.7);
}

TYPED_TEST(HysteresisCharacterization, HysteresisIsClippedTo0To099)
{
    TypeParam h;
    EXPECT_FALSE(h.SetHysteresis(0.99));
    EXPECT_EQ(h.Hysteresis(), 0.99);
    EXPECT_TRUE(h.SetHysteresis(1.0));
    EXPECT_EQ(h.Hysteresis(), 0.99);
    EXPECT_EQ(h.DebugLog().back(), "Error->invalid hysteresis");
    EXPECT_TRUE(h.SetHysteresis(-0.5));
    EXPECT_EQ(h.Hysteresis(), 0.);

    // at 0.99 the output is nearly all last move
    h.SetHysteresis(0.99);
    h.SetAggression(1.0);
    h.result(5.0);
    EXPECT_DOUBLE_EQ(h.result(1.0), (1.0 - 0.99) * 1.0 + 0.99 * ((1.0 - 0.99) * 5.0));
}

TYPED_TEST(HysteresisCharacterization, AggressionRangeAndFallback)
{
    TypeParam h;
    EXPECT_FALSE(h.SetAggression(0.));
    EXPECT_EQ(h.result(5.0), 0.);
    EXPECT_EQ(h.LastMove(), 0.);
    EXPECT_FALSE(h.SetAggression(2.0));
    EXPECT_DOUBLE_EQ(h.result(1.0), 1.8);
    EXPECT_TRUE(h.SetAggression(2.01));
    EXPECT_EQ(h.Aggression(), 0.7); // the default, not a clip
    EXPECT_TRUE(h.SetAggression(-0.1));
    EXPECT_EQ(h.Aggression(), 0.7);
}

TYPED_TEST(HysteresisCharacterization, SetAggressionClearsLastMoveEvenWhenRejected)
{
    TypeParam h;
    h.result(1.0);
    h.SetAggression(0.7);
    EXPECT_EQ(h.LastMove(), 0.);
    h.result(1.0);
    h.SetAggression(5.0);
    EXPECT_EQ(h.LastMove(), 0.);
}

TYPED_TEST(HysteresisCharacterization, SetHysteresisAndMinMoveKeepLastMove)
{
    TypeParam h;
    h.result(1.0);
    h.SetHysteresis(0.5);
    h.SetMinMove(0.1);
    EXPECT_DOUBLE_EQ(h.LastMove(), 0.63);
}

TYPED_TEST(HysteresisCharacterization, ResetClearsLastMove)
{
    TypeParam h;
    h.result(1.0);
    h.reset();
    EXPECT_EQ(h.LastMove(), 0.);
    EXPECT_DOUBLE_EQ(h.result(1.0), 0.63);
}

TYPED_TEST(HysteresisCharacterization, NaNInputPoisonsUntilGatedOrReset)
{
    TypeParam h;
    EXPECT_TRUE(std::isnan(h.result(NaN))); // fabs(NaN) < minMove is false
    EXPECT_TRUE(std::isnan(h.LastMove()));
    EXPECT_TRUE(std::isnan(h.result(1.0)));
    EXPECT_EQ(h.result(0.1), 0.); // the gate clears it
    EXPECT_DOUBLE_EQ(h.result(1.0), 0.63);
    h.result(NaN);
    h.reset();
    EXPECT_DOUBLE_EQ(h.result(1.0), 0.63);
}

TYPED_TEST(HysteresisCharacterization, NaNHysteresisIsAccepted)
{
    // the range check is two comparisons, both false for NaN
    TypeParam h;
    EXPECT_FALSE(h.SetHysteresis(NaN));
    EXPECT_TRUE(std::isnan(h.result(1.0)));
    EXPECT_EQ(h.result(0.1), 0.);
}

TYPED_TEST(HysteresisCharacterization, HugeInputs)
{
    TypeParam h;
    EXPECT_EQ(h.result(Inf), Inf);
    EXPECT_TRUE(std::isnan(h.result(-Inf))); // 0.9 * -inf + 0.1 * inf
    h.reset();
    EXPECT_DOUBLE_EQ(h.result(1e300), 0.9 * 1e300 * 0.7);
}

// ---------------------------------------------------------------------------
// ResistSwitch
// ---------------------------------------------------------------------------

template<class T>
class ResistSwitchCharacterization : public testing::Test
{
};

using ResistSwitchImpls = testing::Types<TwinResistSwitch, ExtractedResistSwitch>;
TYPED_TEST_SUITE(ResistSwitchCharacterization, ResistSwitchImpls);

TYPED_TEST(ResistSwitchCharacterization, ConstructorDefaults)
{
    TypeParam r;
    EXPECT_EQ(r.MinMove(), 0.2);
    EXPECT_EQ(r.Aggression(), 1.0);
    EXPECT_TRUE(r.FastSwitchEnabled());
    EXPECT_EQ(r.CurrentSide(), 0);
    EXPECT_EQ(r.History(), Repeat(0., 10));
    EXPECT_EQ(r.DebugLog(),
              (Log { "GuideAlgorithmResistSwitch::SetMinMove() returns 0, m_minMove=0.20\n",
                     "GuideAlgorithmResistSwitch::SetAggression() returns 0, m_aggression=1.00\n",
                     "GuideAlgorithmResistSwitch::SetFastSwitchEnabled(1)\n" }));
}

TYPED_TEST(ResistSwitchCharacterization, BelowMinMoveReturnsZeroButIsRecorded)
{
    TypeParam r;
    r.ClearLog();
    EXPECT_EQ(r.result(0.1), 0.);
    EXPECT_EQ(r.History().back(), 0.1);
    EXPECT_EQ(r.DebugLog(), (Log { "Throw->" + BELOW, "GuideAlgorithmResistSwitch::result() returns 0.00 from input 0.10\n" }));
}

TYPED_TEST(ResistSwitchCharacterization, HistoryKeepsTheLastTenInputs)
{
    TypeParam r;
    std::vector<double> in;
    for (int i = 1; i <= 13; i++)
        in.push_back(0.01 * i);
    Run(r, in);
    EXPECT_EQ(r.History(), std::vector<double>(in.begin() + 3, in.end()));
}

TYPED_TEST(ResistSwitchCharacterization, ThreeNetEntriesSwitchDirection)
{
    TypeParam r;
    r.SetFastSwitchEnabled(false);
    r.ClearLog();
    EXPECT_EQ(Run(r, Repeat(0.5, 3)), (std::vector<double> { 0., 0., 0.5 }));
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(Throws(r.DebugLog()), (std::vector<std::string> { WEAK, WEAK }));
    ASSERT_EQ(r.DebugLog().size(), 6u);
    EXPECT_EQ(r.DebugLog()[4], "switching direction from 0 to 1 - decHistory=3 oldest=0.00 newest=1.50\n");
    // then every move that way passes
    EXPECT_EQ(Run(r, { 0.3, 0.25, 0.5 }), (std::vector<double> { 0.3, 0.25, 0.5 }));
}

TYPED_TEST(ResistSwitchCharacterization, AnEntryAtMinMoveIsNotCounted)
{
    TypeParam r;
    EXPECT_EQ(Run(r, Repeat(0.2, 12)), Repeat(0., 12));
    EXPECT_EQ(r.CurrentSide(), 0);
    EXPECT_EQ(Throws(r.DebugLog()), std::vector<std::string>(12, WEAK));
}

TYPED_TEST(ResistSwitchCharacterization, OvershootVeto)
{
    // On the committed side, a smaller reversal (under the fast-switch threshold) is taken as
    // an overshoot and not corrected, until the history turns
    TypeParam r;
    Run(r, Repeat(0.5, 3));
    ASSERT_EQ(r.CurrentSide(), 1);
    r.ClearLog();
    EXPECT_EQ(r.result(-0.4), 0.);
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(r.DebugLog(),
              (Log { "Throw->" + OVERSHOT, "GuideAlgorithmResistSwitch::result() returns 0.00 from input -0.40\n" }));

    // decHistory goes 1, 0 (overshoot), -1, -2 (not compelling), then -3 with the newest three
    // (-1.2) worse than the oldest three (0 + .5 + .5): switch
    r.ClearLog();
    std::vector<double> out = Run(r, Repeat(-0.4, 6));
    EXPECT_EQ(out, (std::vector<double> { 0., 0., 0., 0., -0.4, -0.4 }));
    EXPECT_EQ(Throws(r.DebugLog()), (std::vector<std::string> { OVERSHOT, OVERSHOT, WEAK, WEAK }));
    EXPECT_EQ(r.CurrentSide(), -1);
}

TYPED_TEST(ResistSwitchCharacterization, OvershootVetoAppliesWithFastSwitchOff)
{
    TypeParam r;
    r.SetFastSwitchEnabled(false);
    Run(r, Repeat(0.5, 3));
    EXPECT_EQ(r.result(-0.7), 0.);
    EXPECT_EQ(Throws(r.DebugLog()).back(), OVERSHOT);
    EXPECT_EQ(r.CurrentSide(), 1);
}

TYPED_TEST(ResistSwitchCharacterization, FastSwitchOnALargeReversal)
{
    TypeParam r;
    Run(r, Repeat(0.5, 3));
    r.ClearLog();
    EXPECT_EQ(r.result(-0.7), -0.7);
    EXPECT_EQ(r.CurrentSide(), -1);
    EXPECT_EQ(r.History(), (std::vector<double> { 0., 0., 0., 0., 0., 0., 0., -0.7, -0.7, -0.7 }));
    ASSERT_EQ(r.DebugLog().size(), 3u);
    EXPECT_EQ(r.DebugLog()[0], "resist switch: large excursion: input -0.70 thresh 0.60 direction from 1 to -1\n");
    EXPECT_EQ(r.DebugLog()[1], "switching direction from 0 to -1 - decHistory=-3 oldest=0.00 newest=-2.10\n");
    EXPECT_EQ(r.DebugLog()[2], "GuideAlgorithmResistSwitch::result() returns -0.70 from input -0.70\n");
}

TYPED_TEST(ResistSwitchCharacterization, FastSwitchFromNoSideOnTheFirstLargeInput)
{
    TypeParam r;
    EXPECT_EQ(r.result(0.61), 0.61);
    EXPECT_EQ(r.CurrentSide(), 1);
    // and not on the side it already has: a large move that way just passes
    r.ClearLog();
    EXPECT_EQ(r.result(5.0), 5.0);
    EXPECT_EQ(r.DebugLog().size(), 1u);
}

TYPED_TEST(ResistSwitchCharacterization, FastSwitchThresholdIsExclusive)
{
    TypeParam r;
    Run(r, Repeat(0.5, 3));
    double thresh = 3.0 * 0.2;
    EXPECT_EQ(r.result(-thresh), 0.);
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(Throws(r.DebugLog()).back(), OVERSHOT);
}

TYPED_TEST(ResistSwitchCharacterization, NotGettingWorseHoldsTheSide)
{
    // side +1 from three 0.9s, then a steady -0.5: after the vetoes and the weak tallies,
    // decHistory reaches -3 while the oldest three still outweigh the newest three, so the
    // switch waits until 1.3 < 1.5
    TypeParam r;
    r.SetFastSwitchEnabled(false);
    Run(r, Repeat(0.9, 3));
    r.ClearLog();
    std::vector<double> out = Run(r, Repeat(-0.5, 8));
    EXPECT_EQ(out, (std::vector<double> { 0., 0., 0., 0., 0., 0., 0., -0.5 }));
    EXPECT_EQ(Throws(r.DebugLog()),
              (std::vector<std::string> { OVERSHOT, OVERSHOT, OVERSHOT, WEAK, WEAK, NOT_WORSE, NOT_WORSE }));
    EXPECT_EQ(r.CurrentSide(), -1);
}

TYPED_TEST(ResistSwitchCharacterization, ASteadyErrorAcrossIsNeverCorrected)
{
    // With a full window of the same small error on the other side, the newest three equal
    // the oldest three, so "not getting worse" vetoes it on every frame
    TypeParam r;
    Run(r, Repeat(-1.5, 3));
    ASSERT_EQ(r.CurrentSide(), -1);
    EXPECT_EQ(Run(r, Repeat(0.3, 30)), Repeat(0., 30));
    EXPECT_EQ(r.CurrentSide(), -1);
    EXPECT_EQ(Throws(r.DebugLog()).back(), NOT_WORSE);
    // once it grows, it switches
    EXPECT_EQ(r.result(0.4), 0.4);
    EXPECT_EQ(r.CurrentSide(), 1);
}

TYPED_TEST(ResistSwitchCharacterization, SwitchAndOvershootVetoInOneFrame)
{
    // The history switches the side on a frame whose own input points the other way: the
    // switch is logged and kept, and the input is vetoed as an overshoot
    TypeParam r;
    r.SetFastSwitchEnabled(false);
    // weak (decHistory 1, 2, 1, 2), then decHistory 3 with the newest three (-1 + .5 + .5)
    // no worse than the oldest three (0)
    EXPECT_EQ(Run(r, { 1.0, 2.0, -1.0, 0.5, 0.5 }), Repeat(0., 5));
    EXPECT_EQ(Throws(r.DebugLog()), (std::vector<std::string> { WEAK, WEAK, WEAK, WEAK, NOT_WORSE }));
    ASSERT_EQ(r.CurrentSide(), 0);
    // -0.2 is at minMove, so it passes the gate but is not counted: decHistory stays 3 and the
    // newest three are now .5 + .5 - .2 = 0.8
    r.ClearLog();
    EXPECT_EQ(r.result(-0.2), 0.);
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(r.DebugLog(),
              (Log { "switching direction from 0 to 1 - decHistory=3 oldest=0.00 newest=0.80\n", "Throw->" + OVERSHOT,
                     "GuideAlgorithmResistSwitch::result() returns 0.00 from input -0.20\n" }));
    EXPECT_EQ(r.result(0.5), 0.5);
}

TYPED_TEST(ResistSwitchCharacterization, AggressionScalesThePassedMove)
{
    TypeParam r;
    EXPECT_FALSE(r.SetAggression(0.5));
    r.SetFastSwitchEnabled(false);
    EXPECT_EQ(Run(r, Repeat(0.5, 3)), (std::vector<double> { 0., 0., 0.25 }));
    EXPECT_FALSE(r.SetAggression(0.));
    EXPECT_EQ(r.result(0.5), 0.);
    EXPECT_EQ(r.CurrentSide(), 1);
}

TYPED_TEST(ResistSwitchCharacterization, AggressionRangeAndFallback)
{
    TypeParam r;
    r.ClearLog();
    EXPECT_TRUE(r.SetAggression(1.01));
    EXPECT_EQ(r.Aggression(), 1.0);
    EXPECT_FALSE(r.SetAggression(0.5));
    EXPECT_TRUE(r.SetAggression(-0.1));
    EXPECT_EQ(r.Aggression(), 1.0); // the default, not the last value
    EXPECT_EQ(r.DebugLog(),
              (Log { "Error->invalid aggression", "GuideAlgorithmResistSwitch::SetAggression() returns 1, m_aggression=1.00\n",
                     "GuideAlgorithmResistSwitch::SetAggression() returns 0, m_aggression=0.50\n", "Error->invalid aggression",
                     "GuideAlgorithmResistSwitch::SetAggression() returns 1, m_aggression=1.00\n" }));
}

TYPED_TEST(ResistSwitchCharacterization, SetMinMoveResetsTheSideOnlyWhenAccepted)
{
    TypeParam r;
    Run(r, Repeat(0.5, 3));
    ASSERT_EQ(r.CurrentSide(), 1);
    EXPECT_FALSE(r.SetMinMove(0.3));
    EXPECT_EQ(r.CurrentSide(), 0);
    EXPECT_EQ(r.History().back(), 0.5); // the history stays

    Run(r, Repeat(0.5, 3));
    ASSERT_EQ(r.CurrentSide(), 1);
    r.ClearLog();
    EXPECT_TRUE(r.SetMinMove(0.)); // must be > 0
    EXPECT_EQ(r.MinMove(), 0.2);
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(r.DebugLog(),
              (Log { "Error->invalid minMove", "GuideAlgorithmResistSwitch::SetMinMove() returns 1, m_minMove=0.20\n" }));
}

TYPED_TEST(ResistSwitchCharacterization, SetFastSwitchKeepsState)
{
    TypeParam r;
    Run(r, Repeat(0.5, 3));
    r.ClearLog();
    r.SetFastSwitchEnabled(false);
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(r.DebugLog(), (Log { "GuideAlgorithmResistSwitch::SetFastSwitchEnabled(0)\n" }));
}

TYPED_TEST(ResistSwitchCharacterization, ResetClearsHistoryAndSide)
{
    TypeParam r;
    Run(r, Repeat(0.5, 3));
    r.reset();
    EXPECT_EQ(r.CurrentSide(), 0);
    EXPECT_EQ(r.History(), Repeat(0., 10));
}

TYPED_TEST(ResistSwitchCharacterization, NaNInputIsVetoedAndRecorded)
{
    TypeParam r;
    Run(r, Repeat(0.5, 3));
    r.ClearLog();
    EXPECT_EQ(r.result(NaN), 0.);
    EXPECT_TRUE(std::isnan(r.History().back()));
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(r.DebugLog(),
              (Log { "Throw->" + OVERSHOT, "GuideAlgorithmResistSwitch::result() returns 0.00 from input nan\n" }));
    TypeParam s;
    EXPECT_EQ(s.result(NaN), 0.); // side 0, decHistory 0
    EXPECT_EQ(Throws(s.DebugLog()), (std::vector<std::string> { WEAK }));
}

TYPED_TEST(ResistSwitchCharacterization, InfiniteInputFastSwitchesThrough)
{
    TypeParam r;
    EXPECT_EQ(r.result(Inf), Inf);
    EXPECT_EQ(r.CurrentSide(), 1);
    EXPECT_EQ(r.result(1e300), 1e300);
}
