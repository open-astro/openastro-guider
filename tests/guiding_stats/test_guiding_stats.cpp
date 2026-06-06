// Unit tests for src/guiding_stats.cpp.
//
// Unlike the guide-algorithm "math twin" tests, this file links the REAL
// production guiding_stats.cpp. That module has zero wxWidgets/Mount/MyFrame
// coupling — it only needs <deque>, <math.h>, <algorithm> and the std headers
// our shadow phd.h already provides — so the actual code under test compiles
// and runs unmodified. These are true unit tests, not formula replicas.
//
// Covered:
//   DescriptiveStats     — on-the-fly count/mean/sum/min/max/variance/sigma/
//                          population-sigma/maxDelta/lastValue + ClearAll reset
//   HighPassFilter /
//   LowPassFilter        — first-sample seeding + recurrence
//   AxisStats            — count/sum/mean/variance/sigma/median(odd,even)/
//                          min-max displacement/maxDelta/move+reversal counts/
//                          linear fit (slope, intercept, R^2, drift-removed sigma)
//   WindowedAxisStats    — auto-window trimming, RemoveOldestEntry bookkeeping,
//                          ChangeWindowSize, min/max recompute on age-out, and a
//                          regression for the maxDeltaInx reset in ClearAll.

#include <gtest/gtest.h>

#include "guiding_stats.h"

// ---------------------------------------------------------------------------
// DescriptiveStats
// ---------------------------------------------------------------------------
// Classic worked example: {2,4,4,4,5,5,7,9}. n=8, mean=5, sum=40.
// Sum of squared deviations from the mean (the "S" the Knuth recurrence
// accumulates) = 32, so population sigma = sqrt(32/8) = 2, sample sigma =
// sqrt(32/7), and sample variance = 32/7 (GetVariance() == GetSigma()^2).
TEST(DescriptiveStats, WorkedExample)
{
    DescriptiveStats ds;
    for (double v : { 2., 4., 4., 4., 5., 5., 7., 9. })
        ds.AddValue(v);

    EXPECT_EQ(ds.GetCount(), 8u);
    EXPECT_DOUBLE_EQ(ds.GetSum(), 40.);
    EXPECT_DOUBLE_EQ(ds.GetMean(), 5.);
    EXPECT_DOUBLE_EQ(ds.GetMinimum(), 2.);
    EXPECT_DOUBLE_EQ(ds.GetMaximum(), 9.);
    EXPECT_DOUBLE_EQ(ds.GetLastValue(), 9.);
    EXPECT_DOUBLE_EQ(ds.GetVariance(), 32. / 7.); // sample variance S/(n-1)
    EXPECT_DOUBLE_EQ(ds.GetPopulationSigma(), 2.);
    EXPECT_DOUBLE_EQ(ds.GetSigma(), std::sqrt(32. / 7.));
    EXPECT_DOUBLE_EQ(ds.GetSigma(), std::sqrt(ds.GetVariance())); // sigma == sqrt(variance)
    EXPECT_DOUBLE_EQ(ds.GetMaxDelta(), 2.); // largest |x_i - x_{i-1}|
}

TEST(DescriptiveStats, SingleValueHasNoSpread)
{
    DescriptiveStats ds;
    ds.AddValue(42.);

    EXPECT_EQ(ds.GetCount(), 1u);
    EXPECT_DOUBLE_EQ(ds.GetMean(), 42.);
    EXPECT_DOUBLE_EQ(ds.GetSum(), 42.);
    EXPECT_DOUBLE_EQ(ds.GetMinimum(), 42.);
    EXPECT_DOUBLE_EQ(ds.GetMaximum(), 42.);
    EXPECT_DOUBLE_EQ(ds.GetVariance(), 0.); // count <= 1
    EXPECT_DOUBLE_EQ(ds.GetSigma(), 0.);
    EXPECT_DOUBLE_EQ(ds.GetMaxDelta(), 0.); // count <= 1
}

TEST(DescriptiveStats, ClearAllResets)
{
    DescriptiveStats ds;
    for (double v : { 10., -3., 7. })
        ds.AddValue(v);
    ds.ClearAll();

    EXPECT_EQ(ds.GetCount(), 0u);
    EXPECT_DOUBLE_EQ(ds.GetMean(), 0.);
    EXPECT_DOUBLE_EQ(ds.GetSum(), 0.);
    EXPECT_DOUBLE_EQ(ds.GetMaxDelta(), 0.);

    // Reusable after reset: minimum should track the new data, not stale state.
    ds.AddValue(100.);
    ds.AddValue(50.);
    EXPECT_EQ(ds.GetCount(), 2u);
    EXPECT_DOUBLE_EQ(ds.GetMinimum(), 50.);
    EXPECT_DOUBLE_EQ(ds.GetMaximum(), 100.);
}

// ---------------------------------------------------------------------------
// HighPassFilter / LowPassFilter
// ---------------------------------------------------------------------------
// CutoffPeriod=10, SamplePeriod=1.
//   LPF alpha = 1 - 10/11 = 1/11
//   HPF alpha =     10/11
// Both seed on the first sample (return it verbatim), then run their
// recurrence.
TEST(LowPassFilter, SeedsThenSmooths)
{
    LowPassFilter lpf(10., 1.);
    const double alpha = 1. / 11.;

    EXPECT_DOUBLE_EQ(lpf.AddValue(0.), 0.); // first point seeds
    double s1 = 0. + alpha * (11. - 0.); // == 1.0
    EXPECT_DOUBLE_EQ(lpf.AddValue(11.), s1);
    double s2 = s1 + alpha * (11. - s1); // recurrence runs again
    EXPECT_DOUBLE_EQ(lpf.AddValue(11.), s2);
    EXPECT_DOUBLE_EQ(lpf.GetCurrentLPF(), s2);

    lpf.Reset();
    EXPECT_DOUBLE_EQ(lpf.AddValue(5.), 5.); // seeds again after reset
}

TEST(HighPassFilter, SeedsThenDifferences)
{
    HighPassFilter hpf(10., 1.);
    const double alpha = 10. / 11.;

    EXPECT_DOUBLE_EQ(hpf.AddValue(4.), 4.); // first point seeds
    // hpf = alpha * (prevHpf + new - prev) = alpha * (4 + 7 - 4) = alpha*7
    EXPECT_DOUBLE_EQ(hpf.AddValue(7.), alpha * (4. + 7. - 4.));
    EXPECT_DOUBLE_EQ(hpf.GetCurrentHPF(), alpha * 7.);
}

// ---------------------------------------------------------------------------
// AxisStats — perfectly linear dataset pos = 2*t + 1, t = 0..4
// ---------------------------------------------------------------------------
TEST(AxisStats, LinearDatasetStats)
{
    AxisStats ax;
    for (int t = 0; t <= 4; ++t)
        ax.AddGuideInfo(t, 2. * t + 1., 0.);

    EXPECT_EQ(ax.GetCount(), 5u);
    EXPECT_DOUBLE_EQ(ax.GetSum(), 25.); // 1+3+5+7+9
    EXPECT_DOUBLE_EQ(ax.GetMean(), 5.);
    EXPECT_DOUBLE_EQ(ax.GetMinDisplacement(), 1.);
    EXPECT_DOUBLE_EQ(ax.GetMaxDisplacement(), 9.);
    EXPECT_DOUBLE_EQ(ax.GetMedian(), 5.); // odd count -> middle element
    EXPECT_DOUBLE_EQ(ax.GetMaxDelta(), 2.); // every step is +2
    EXPECT_DOUBLE_EQ(ax.GetVariance(), 10.); // (n*SYY - SY^2)/(n(n-1))
    EXPECT_DOUBLE_EQ(ax.GetSigma(), std::sqrt(10.));
    EXPECT_DOUBLE_EQ(ax.GetPopulationSigma(), std::sqrt(8.));
}

TEST(AxisStats, LinearFitIsExactOnLinearData)
{
    AxisStats ax;
    for (int t = 0; t <= 4; ++t)
        ax.AddGuideInfo(t, 2. * t + 1., 0.);

    double slope = 0., intercept = 0., sigma = -1.;
    double rsq = ax.GetLinearFitResults(&slope, &intercept, &sigma);

    EXPECT_DOUBLE_EQ(slope, 2.);
    EXPECT_DOUBLE_EQ(intercept, 1.);
    EXPECT_DOUBLE_EQ(rsq, 1.); // perfect correlation
    EXPECT_NEAR(sigma, 0., 1e-9); // no residual after removing the fit
}

TEST(AxisStats, LinearFitDegenerateOnTooFewPoints)
{
    AxisStats ax;
    ax.AddGuideInfo(0., 5., 0.); // single point

    double slope = 9., intercept = 9., sigma = 9.;
    double rsq = ax.GetLinearFitResults(&slope, &intercept, &sigma);

    EXPECT_DOUBLE_EQ(slope, 0.);
    EXPECT_DOUBLE_EQ(intercept, 0.);
    EXPECT_DOUBLE_EQ(sigma, 0.);
    EXPECT_DOUBLE_EQ(rsq, 0.);
}

TEST(AxisStats, MedianEvenCountAveragesMiddlePair)
{
    AxisStats ax;
    for (double p : { 1., 2., 3., 4. })
        ax.AddGuideInfo(0., p, 0.);
    EXPECT_DOUBLE_EQ(ax.GetMedian(), 2.5); // (2+3)/2
}

TEST(AxisStats, MoveAndReversalCounts)
{
    AxisStats ax;
    const double amts[] = { 0., 1., 1., -1., 0., -2. };
    for (double a : amts)
        ax.AddGuideInfo(0., 0., a);

    EXPECT_EQ(ax.GetMoveCount(), 4u); // four non-zero pulses
    EXPECT_EQ(ax.GetReversalCount(), 1u); // only +1 -> -1 flips sign
}

TEST(AxisStats, ClearAllResets)
{
    AxisStats ax;
    for (int t = 0; t < 5; ++t)
        ax.AddGuideInfo(t, t * 3., 1.);
    ax.ClearAll();

    EXPECT_EQ(ax.GetCount(), 0u);
    EXPECT_EQ(ax.GetMoveCount(), 0u);
    EXPECT_EQ(ax.GetReversalCount(), 0u);
    EXPECT_DOUBLE_EQ(ax.GetSum(), 0.);
    EXPECT_DOUBLE_EQ(ax.GetMaxDelta(), 0.);
}

// ---------------------------------------------------------------------------
// WindowedAxisStats
// ---------------------------------------------------------------------------
TEST(WindowedAxisStats, AutoTrimsToWindowSize)
{
    WindowedAxisStats w(3);
    const double positions[] = { 10., 20., 30., 40., 50. };
    for (int t = 0; t < 5; ++t)
        w.AddGuideInfo(t, positions[t], 0.);

    // Window keeps only the most recent 3 entries: 30, 40, 50.
    EXPECT_EQ(w.GetCount(), 3u);
    EXPECT_DOUBLE_EQ(w.GetSum(), 120.);
    EXPECT_DOUBLE_EQ(w.GetMean(), 40.);
    EXPECT_DOUBLE_EQ(w.GetMinDisplacement(), 30.);
    EXPECT_DOUBLE_EQ(w.GetMaxDisplacement(), 50.);
}

TEST(WindowedAxisStats, RemoveOldestUpdatesBookkeeping)
{
    WindowedAxisStats w; // no auto-windowing
    w.AddGuideInfo(0., 10., 1.); // move
    w.AddGuideInfo(1., 20., -1.); // move + reversal
    w.AddGuideInfo(2., 30., 0.); // no move

    EXPECT_EQ(w.GetMoveCount(), 2u);
    EXPECT_EQ(w.GetReversalCount(), 1u);

    w.RemoveOldestEntry(); // drops the (10, move) entry
    EXPECT_EQ(w.GetCount(), 2u);
    EXPECT_EQ(w.GetMoveCount(), 1u); // one move aged out
    EXPECT_DOUBLE_EQ(w.GetSum(), 50.); // 20 + 30
    EXPECT_DOUBLE_EQ(w.GetMinDisplacement(), 20.); // recomputed after age-out
}

TEST(WindowedAxisStats, ChangeWindowSizeTrims)
{
    WindowedAxisStats w;
    for (int t = 0; t < 5; ++t)
        w.AddGuideInfo(t, (t + 1) * 10., 0.); // 10,20,30,40,50

    EXPECT_EQ(w.GetCount(), 5u);
    EXPECT_TRUE(w.ChangeWindowSize(2)); // keep most recent 2: 40, 50
    EXPECT_EQ(w.GetCount(), 2u);
    EXPECT_DOUBLE_EQ(w.GetMinDisplacement(), 40.);
    EXPECT_DOUBLE_EQ(w.GetMaxDisplacement(), 50.);

    EXPECT_TRUE(w.ChangeWindowSize(0)); // 0 disables windowing, keeps data
    EXPECT_EQ(w.GetCount(), 2u);
}

// Regression: ClearAll() must reset maxDeltaInx alongside maxDelta. If it
// doesn't, a reused windowed instance carries a stale index into
// AdjustMinMaxValues(), which keys its full min/max/maxDelta recompute off
// maxDeltaInx == 0. A reused instance must produce the same min/max as a
// fresh one for the identical data stream.
TEST(WindowedAxisStats, ClearAllResetsMaxDeltaInxForReuse)
{
    auto run = [](WindowedAxisStats& w)
    {
        for (int t = 0; t < 6; ++t)
            w.AddGuideInfo(t, (t % 2 == 0) ? 100. : 5., 0.);
    };

    WindowedAxisStats fresh(3);
    run(fresh);

    WindowedAxisStats reused(3);
    for (int t = 0; t < 4; ++t)
        reused.AddGuideInfo(t, 999., 0.); // dirty it with different data
    reused.ClearAll();
    run(reused);

    EXPECT_EQ(reused.GetCount(), fresh.GetCount());
    EXPECT_DOUBLE_EQ(reused.GetMinDisplacement(), fresh.GetMinDisplacement());
    EXPECT_DOUBLE_EQ(reused.GetMaxDisplacement(), fresh.GetMaxDisplacement());
    EXPECT_DOUBLE_EQ(reused.GetSum(), fresh.GetSum());
}
