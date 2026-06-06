// Unit tests for src/zfilterfactory.cpp.
//
// ZFilterFactory is pure DSP math (poles/zeros -> bilinear transform ->
// recurrence coefficients) with zero wx/Mount coupling, so the real
// production translation unit links and runs here directly.
//
// Two layers of coverage:
//   1. Structural invariants of the produced filter (order, corner, name,
//      coefficient count, the normalized ycoeffs[0] == -1, finiteness).
//   2. An end-to-end recurrence: feed a constant input through the exact
//      difference equation GuideAlgorithmZFilter uses (src/
//      guide_algorithm_zfilter.cpp lines 79-92) and confirm the output
//      converges to the input — i.e. the factory really produced a
//      unity-DC-gain low-pass filter. This validates the gain normalization
//      and all coefficients together, not just their shape.
//
// BESSEL and BUTTERWORTH are the only designs the factory supports
// (guide_algorithm_zfilter.cpp uses BESSEL by default and BUTTERWORTH when
// corner < 6); both are exercised here.

#include <gtest/gtest.h>

#include "zfilterfactory.h"

#include <cmath>
#include <deque>

namespace
{

// One step of the exact difference equation GuideAlgorithmZFilter::result()
// uses (with m_sumCorr == 0): shift in `input`, return the new output. xv/yv
// are the filter's input/output histories, carried across calls.
double StepFilter(const std::vector<double>& xc, const std::vector<double>& yc, double gain, std::deque<double>& xv,
                  std::deque<double>& yv, double input)
{
    xv.push_front(input / gain);
    xv.pop_back();
    yv.push_front(0.0);
    yv.pop_back();

    double acc = 0.0;
    for (size_t i = 0; i < xc.size(); ++i)
        acc += xv[i] * xc[i];
    for (size_t i = 1; i < yc.size(); ++i)
        acc += yv[i] * yc[i];
    yv[0] = acc;
    return acc;
}

// Runs a constant input through the filter for enough steps to settle and
// returns the final output. For a unity-gain low-pass this converges to `input`.
double SettledResponse(ZFilterFactory& f, double input, int steps = 5000)
{
    std::deque<double> xv(f.xcoeffs.size(), 0.0);
    std::deque<double> yv(f.ycoeffs.size(), 0.0);
    double out = 0.0;
    for (int n = 0; n < steps; ++n)
        out = StepFilter(f.xcoeffs, f.ycoeffs, f.gain(), xv, yv, input);
    return out;
}

void ExpectAllFinite(const std::vector<double>& v)
{
    for (double c : v)
        EXPECT_TRUE(std::isfinite(c)) << "coefficient not finite: " << c;
}

} // namespace

TEST(ZFilterFactory, ReportsRequestedDesignParams)
{
    ZFilterFactory bw(BUTTERWORTH, 3, 10.0);
    EXPECT_EQ(bw.order(), 3);
    EXPECT_EQ(bw.design(), BUTTERWORTH);
    EXPECT_EQ(bw.getname(), "Butterworth");
    EXPECT_NEAR(bw.corner(), 10.0, 1e-9); // corner == 1/raw_alpha1 == p

    ZFilterFactory be(BESSEL, 2, 7.5);
    EXPECT_EQ(be.order(), 2);
    EXPECT_EQ(be.design(), BESSEL);
    EXPECT_EQ(be.getname(), "Bessel");
    EXPECT_NEAR(be.corner(), 7.5, 1e-9);
}

TEST(ZFilterFactory, CoefficientShapeAndNormalization)
{
    for (int order = 1; order <= 5; ++order)
    {
        ZFilterFactory f(BUTTERWORTH, order, 10.0);
        // expand() produces order+1 coefficients on each side.
        EXPECT_EQ(f.xcoeffs.size(), static_cast<size_t>(order + 1));
        EXPECT_EQ(f.ycoeffs.size(), static_cast<size_t>(order + 1));
        // ycoeffs are normalized by the leading denominator term, so the
        // first one is always -1 (the y[n] term moved to the RHS).
        EXPECT_DOUBLE_EQ(f.ycoeffs[0], -1.0);
        ExpectAllFinite(f.xcoeffs);
        ExpectAllFinite(f.ycoeffs);
        EXPECT_GT(f.gain(), 0.0);
        EXPECT_TRUE(std::isfinite(f.gain()));
    }
}

TEST(ZFilterFactory, ButterworthIsUnityGainLowpass)
{
    for (int order = 1; order <= 4; ++order)
    {
        ZFilterFactory f(BUTTERWORTH, order, 10.0);
        // A constant DC input must pass through a low-pass filter unchanged.
        EXPECT_NEAR(SettledResponse(f, 3.7), 3.7, 1e-6) << "Butterworth order " << order;
        EXPECT_NEAR(SettledResponse(f, -12.0), -12.0, 1e-6) << "Butterworth order " << order;
    }
}

TEST(ZFilterFactory, BesselIsUnityGainLowpass)
{
    for (int order = 1; order <= 4; ++order)
    {
        ZFilterFactory f(BESSEL, order, 20.0);
        EXPECT_NEAR(SettledResponse(f, 5.0), 5.0, 1e-6) << "Bessel order " << order;
    }
}

TEST(ZFilterFactory, AttenuatesAlternatingSignal)
{
    // A Nyquist-rate alternating sequence (+a, -a, +a, ...) is the highest
    // frequency representable; a low-pass should knock it down hard. Compare
    // the settled output magnitude against the input amplitude.
    ZFilterFactory f(BUTTERWORTH, 3, 10.0);
    const double amp = 10.0;

    std::deque<double> xv(f.xcoeffs.size(), 0.0);
    std::deque<double> yv(f.ycoeffs.size(), 0.0);

    double maxOut = 0.0;
    for (int n = 0; n < 4000; ++n)
    {
        double input = (n & 1) ? amp : -amp;
        double acc = StepFilter(f.xcoeffs, f.ycoeffs, f.gain(), xv, yv, input);
        if (n > 2000) // after settling
            maxOut = std::max(maxOut, std::fabs(acc));
    }
    EXPECT_LT(maxOut, amp * 0.5) << "high frequency not attenuated";
}

TEST(ZFilterFactory, RejectsInvalidParameters)
{
    EXPECT_ANY_THROW(ZFilterFactory(BUTTERWORTH, 0, 10.0)); // order must be > 0
    EXPECT_ANY_THROW(ZFilterFactory(BUTTERWORTH, -2, 10.0));
    EXPECT_ANY_THROW(ZFilterFactory(BUTTERWORTH, 2, 1.0)); // corner period must be >= 2
}
