// Unit tests for src/guide_algorithm_lowpass_math.cpp — the decision math of
// the Lowpass and Lowpass2 guide algorithms, extracted from the (GUI-coupled)
// GuideAlgorithmLowpass / GuideAlgorithmLowpass2 translation units so it can
// be tested directly. The helper runs against the REAL production
// WindowedAxisStats (guiding_stats.cpp), so this exercises the actual
// median / linear-fit / windowing path the algorithm uses, not a twin.
//
// These cover the wrapper logic that the stats tests don't: the median +
// slope*weight combination and input clamp (Lowpass); the warm-up
// attenuation, outlier-dump, drift slope, sign guard and min-move gate
// (Lowpass2).

#include <gtest/gtest.h>

#include "guide_algorithm_lowpass_math.h"

#include <cmath>

namespace
{
constexpr int HISTORY_SIZE = 10; // matches both algorithms' HISTORY_SIZE
}

// --- Lowpass ----------------------------------------------------------------

TEST(GuideLowpass, ResetZeroFillsWindow)
{
    WindowedAxisStats stats;
    int tb = 99;
    guide_lowpass::Reset(stats, tb, HISTORY_SIZE);
    EXPECT_EQ(stats.GetCount(), static_cast<unsigned>(HISTORY_SIZE));
    EXPECT_EQ(tb, HISTORY_SIZE); // timeBase advanced once per fill point
}

TEST(GuideLowpass, BelowMinMoveReturnsZero)
{
    WindowedAxisStats stats;
    int tb = 0;
    guide_lowpass::Reset(stats, tb, HISTORY_SIZE);
    guide_lowpass::Result r = guide_lowpass::Compute(stats, tb, /*slopeWeight*/ 5.0, /*minMove*/ 0.5, /*input*/ 0.3);
    EXPECT_DOUBLE_EQ(r.value, 0.0); // |input| < minMove
}

TEST(GuideLowpass, ConstantInputConvergesToInput)
{
    WindowedAxisStats stats;
    int tb = 0;
    guide_lowpass::Reset(stats, tb, HISTORY_SIZE);
    const double input = 2.0; // above minMove
    guide_lowpass::Result r;
    for (int i = 0; i < 30; ++i)
        r = guide_lowpass::Compute(stats, tb, 5.0, 0.2, input);
    // Once the whole window holds the constant, median == input and slope == 0.
    EXPECT_NEAR(r.value, input, 1e-9);
    EXPECT_NEAR(r.computed, input, 1e-9);
    EXPECT_FALSE(r.clampedToInput);
}

TEST(GuideLowpass, ClampsWhenComputedExceedsInput)
{
    // A steep upward ramp with a heavy slope weight drives median+slope*weight
    // past the latest input; the algorithm must then clamp to input.
    WindowedAxisStats stats;
    int tb = 0;
    guide_lowpass::Reset(stats, tb, HISTORY_SIZE);
    bool everClamped = false;
    for (int i = 1; i <= 20; ++i)
    {
        double input = i * 1.0; // ramp 1,2,3,...
        guide_lowpass::Result r = guide_lowpass::Compute(stats, tb, /*slopeWeight*/ 10.0, /*minMove*/ 0.2, input);
        if (r.clampedToInput)
        {
            everClamped = true;
            EXPECT_DOUBLE_EQ(r.value, input); // clamp returns input exactly
            EXPECT_GT(std::fabs(r.computed), std::fabs(input));
        }
    }
    EXPECT_TRUE(everClamped) << "steep ramp should trip the clamp at least once";
}

// --- Lowpass2 ---------------------------------------------------------------

TEST(GuideLowpass2, ResetClearsState)
{
    WindowedAxisStats stats(HISTORY_SIZE);
    int tb = 7, rej = 3;
    guide_lowpass2::Reset(stats, tb, rej);
    EXPECT_EQ(stats.GetCount(), 0u);
    EXPECT_EQ(tb, 0);
    EXPECT_EQ(rej, 0);
}

TEST(GuideLowpass2, WarmupAppliesAttenuation)
{
    WindowedAxisStats stats(HISTORY_SIZE);
    int tb = 0, rej = 0;
    guide_lowpass2::Reset(stats, tb, rej);
    const double aggr = 80.0; // attenuation 0.8
    const double input = 1.0; // above minMove, below 4*minMove (=2.0)
    // First 3 points: numpts < 4 -> input * attenuation.
    for (int i = 0; i < 3; ++i)
    {
        guide_lowpass2::Result r = guide_lowpass2::Compute(stats, tb, rej, aggr, /*minMove*/ 0.5, input);
        EXPECT_NEAR(r.value, input * 0.8, 1e-9) << "warmup point " << i;
    }
}

TEST(GuideLowpass2, BelowMinMoveReturnsZero)
{
    WindowedAxisStats stats(HISTORY_SIZE);
    int tb = 0, rej = 0;
    guide_lowpass2::Reset(stats, tb, rej);
    guide_lowpass2::Result r = guide_lowpass2::Compute(stats, tb, rej, 80.0, /*minMove*/ 0.5, /*input*/ 0.2);
    EXPECT_DOUBLE_EQ(r.value, 0.0);
}

TEST(GuideLowpass2, OutlierDumpsHistory)
{
    WindowedAxisStats stats(HISTORY_SIZE);
    int tb = 0, rej = 0;
    guide_lowpass2::Reset(stats, tb, rej);
    const double aggr = 80.0, minMove = 0.2;
    // Three small warm-up points so the next call sees numpts >= 4.
    for (int i = 0; i < 3; ++i)
        guide_lowpass2::Compute(stats, tb, rej, aggr, minMove, 0.1);
    // 4th point is a large outlier: |input| > 4*minMove (=0.8) -> dump.
    guide_lowpass2::Result r = guide_lowpass2::Compute(stats, tb, rej, aggr, minMove, /*input*/ 1.0);
    EXPECT_TRUE(r.outlierDumped);
    EXPECT_NEAR(r.value, 1.0 * 0.8, 1e-9); // input * attenuation
    EXPECT_EQ(stats.GetCount(), 0u); // history cleared
    EXPECT_EQ(tb, 0);
}

TEST(GuideLowpass2, ConstantInputHasNoDriftCorrection)
{
    // Lowpass2 is a drift (slope) filter: a constant signal has zero slope, so
    // once past warm-up it emits no correction.
    WindowedAxisStats stats(HISTORY_SIZE);
    int tb = 0, rej = 0;
    guide_lowpass2::Reset(stats, tb, rej);
    const double input = 1.0, minMove = 0.5; // |input| < 4*minMove avoids outlier path
    guide_lowpass2::Result r;
    for (int i = 0; i < 8; ++i)
        r = guide_lowpass2::Compute(stats, tb, rej, 80.0, minMove, input);
    EXPECT_NEAR(r.slope, 0.0, 1e-9);
    EXPECT_NEAR(r.value, 0.0, 1e-9);
}

TEST(GuideLowpass2, SteadyDriftProducesSameSignCorrection)
{
    // A steady upward drift should yield a positive slope and a positive,
    // attenuated correction (never opposing the input). minMove is kept large
    // enough that the gentle ramp stays below 4*minMove and so takes the
    // linear-fit branch rather than the outlier-dump path.
    WindowedAxisStats stats(HISTORY_SIZE);
    int tb = 0, rej = 0;
    guide_lowpass2::Reset(stats, tb, rej);
    const double minMove = 0.5;
    guide_lowpass2::Result r;
    for (int i = 1; i <= 8; ++i)
        r = guide_lowpass2::Compute(stats, tb, rej, /*aggr*/ 80.0, minMove, /*input*/ i * 0.1);
    EXPECT_GT(r.slope, 0.0);
    EXPECT_GE(r.value, 0.0); // same sign as the (positive) drift, or gated to 0
}
