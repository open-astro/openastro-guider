// Differential test of the guide algorithm kernel extraction (#142).
//
// The frozen twin (the production code before the extraction) and production as it is now
// (the real guide_algorithm_kernels.cpp behind the production result(), reset() and setters)
// are driven with the same calls: guide inputs from seeded random sequences built to reach
// every branch (noise around minMove, inputs exactly at minMove and at 3 x minMove and one ulp
// either side, sustained drifts and sign changes, zeros of both signs, huge values, infinities
// and NaN), with resets and setter calls (minMove 0, aggression 0 and the maximum, hysteresis 0
// and 1, the fast switch on and off, out-of-range and NaN values) mixed in. After every call
// the two must agree exactly: the return value and every double of the state bit for bit, the
// side, the parameters, the setters' error flag, and the Debug lines written. The one
// allowance is the sign and payload of a NaN, which the source does not determine (see Same).
//
// Coverage counters check that the sweeps reach every veto, the fast switch, the history
// switch, a switch and a veto on one frame, and the non-finite inputs.

#include "algorithm_impls.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>

using namespace algo_test;

namespace
{

uint64_t Bits(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

// a and b are the same double, bit for bit (the sign of zero included). Two NaNs are the
// same whatever their sign and payload: when both operands of an operation are NaN, IEEE 754
// leaves open which one the result carries, and GCC orders the operands of an fadd differently
// in different translation units (at -O0 the twin and the kernel get -nan and nan from the
// same NaN inputs).
::testing::AssertionResult Same(const char *what, double a, double b)
{
    if (Bits(a) == Bits(b) || (std::isnan(a) && std::isnan(b)))
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << what << ": twin " << a << " (0x" << std::hex << Bits(a) << ") extracted " << b
                                         << " (0x" << Bits(b) << ")";
}

// The log lines written since the last call, with a NaN's sign dropped ("-nan" is "nan"),
// for the reason Same() gives
Log Tail(const Log& log, size_t from)
{
    Log tail(log.begin() + (std::ptrdiff_t) from, log.end());
    for (std::string& line : tail)
    {
        for (size_t pos; (pos = line.find("-nan")) != std::string::npos;)
            line.erase(pos, 1);
    }
    return tail;
}

struct Coverage
{
    int below = 0;
    int weak = 0;
    int notWorse = 0;
    int overshoot = 0;
    int passed = 0;
    int fastSwitch = 0;
    int switched = 0;
    int switchAndVeto = 0;
    int nonFinite = 0;
    int setterErrors = 0;
    int hysteresisGated = 0;
    int hysteresisPassed = 0;

    void CountResistSwitch(const Log& lines)
    {
        bool sw = false;
        bool veto = false;
        for (const std::string& l : lines)
        {
            if (l == "Throw->input < m_minMove")
                below++, veto = true;
            else if (l == "Throw->not compelling enough")
                weak++, veto = true;
            else if (l == "Throw->Not getting worse")
                notWorse++, veto = true;
            else if (l == "Throw->must have overshot -- vetoing move")
                overshoot++, veto = true;
            else if (l.rfind("resist switch: large excursion", 0) == 0)
                fastSwitch++;
            else if (l.rfind("switching direction", 0) == 0)
                switched++, sw = true;
        }
        if (!veto)
            passed++;
        if (sw && veto)
            switchAndVeto++;
    }
};

// Guide inputs that reach every branch, for an algorithm whose minMove is about mm
class InputSource
{
public:
    explicit InputSource(uint64_t seed) : m_rng(seed) { }

    double Next(double mm)
    {
        double scale = mm > 0. ? mm : 0.2;
        int kind = Pick(100);
        if (kind < 30)
        {
            // noise around the gate
            return Uniform(-3. * scale, 3. * scale);
        }
        if (kind < 55)
        {
            // a drift that persists, with sign changes when it wanders across zero
            m_walk += Uniform(-0.6, 0.6) * scale;
            if (Pick(40) == 0)
                m_walk = -m_walk;
            return m_walk + Uniform(-0.5, 0.5) * scale;
        }
        if (kind < 70)
        {
            // the gate and the fast-switch threshold, and one ulp either side
            double edges[] = { mm, 3.0 * mm };
            double e = edges[Pick(2)];
            int ulp = Pick(3);
            if (ulp == 1)
                e = std::nextafter(e, 0.);
            else if (ulp == 2)
                e = std::nextafter(e, 1e300);
            return Pick(2) ? e : -e;
        }
        if (kind < 78)
            return Pick(2) ? 0. : -0.;
        if (kind < 91)
            return Uniform(-50., 50.);
        if (kind < 93)
        {
            // huge: rarely 1e300, as each %.2f of it in a Debug line is 300 digits
            if (Pick(8) == 0)
                return Pick(2) ? 1e300 : -1e300;
            return Pick(2) ? 1e15 : -1e15;
        }
        if (kind < 95)
            return Pick(2) ? 4.9e-324 : -1e-310;
        if (kind < 97)
            return Pick(2) ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
        return std::numeric_limits<double>::quiet_NaN();
    }

    int Pick(int n) { return (int) (m_rng() % (uint64_t) n); }
    double Uniform(double lo, double hi) { return lo + (hi - lo) * ((double) (m_rng() >> 11) * 0x1.0p-53); }

private:
    std::mt19937_64 m_rng;
    double m_walk = 0.;
};

// A setter value: the range ends, just outside them, the defaults, zero, NaN, or random
double ParamValue(InputSource& in, std::initializer_list<double> edges)
{
    if (in.Pick(4) == 0)
        return in.Uniform(-0.5, 2.5);
    if (in.Pick(20) == 0)
        return std::numeric_limits<double>::quiet_NaN();
    size_t i = (size_t) in.Pick((int) edges.size());
    return edges.begin()[i];
}

void CompareHysteresis(const TwinHysteresis& t, const ExtractedHysteresis& e, size_t tLog, size_t eLog, int step)
{
    ASSERT_TRUE(Same("lastMove", t.LastMove(), e.LastMove())) << "step " << step;
    ASSERT_TRUE(Same("minMove", t.MinMove(), e.MinMove())) << "step " << step;
    ASSERT_TRUE(Same("hysteresis", t.Hysteresis(), e.Hysteresis())) << "step " << step;
    ASSERT_TRUE(Same("aggression", t.Aggression(), e.Aggression())) << "step " << step;
    ASSERT_EQ(Tail(t.DebugLog(), tLog), Tail(e.DebugLog(), eLog)) << "step " << step;
}

void CompareResistSwitch(const TwinResistSwitch& t, const ExtractedResistSwitch& e, size_t tLog, size_t eLog, int step)
{
    ASSERT_EQ(t.CurrentSide(), e.CurrentSide()) << "step " << step;
    std::vector<double> th = t.History();
    std::vector<double> eh = e.History();
    ASSERT_EQ(th.size(), eh.size()) << "step " << step;
    for (size_t i = 0; i < th.size(); i++)
        ASSERT_TRUE(Same("history", th[i], eh[i])) << "entry " << i << " step " << step;
    ASSERT_TRUE(Same("minMove", t.MinMove(), e.MinMove())) << "step " << step;
    ASSERT_TRUE(Same("aggression", t.Aggression(), e.Aggression())) << "step " << step;
    ASSERT_EQ(t.FastSwitchEnabled(), e.FastSwitchEnabled()) << "step " << step;
    ASSERT_EQ(Tail(t.DebugLog(), tLog), Tail(e.DebugLog(), eLog)) << "step " << step;
}

// One seeded run of calls on both Hysteresis implementations
void RunHysteresis(uint64_t seed, int steps, Coverage& cov)
{
    SCOPED_TRACE(testing::Message() << "hysteresis seed " << seed);
    InputSource in(seed);
    TwinHysteresis t;
    ExtractedHysteresis e;
    CompareHysteresis(t, e, 0, 0, -1);
    for (int step = 0; step < steps && !testing::Test::HasFatalFailure(); step++)
    {
        size_t tLog = t.DebugLog().size();
        size_t eLog = e.DebugLog().size();
        int op = in.Pick(100);
        if (op < 2)
        {
            t.reset();
            e.reset();
        }
        else if (op < 4)
        {
            double v = ParamValue(in, { 0., 0.05, 0.2, 1.0, -0.01, 1e300 });
            bool a = t.SetMinMove(v);
            ASSERT_EQ(a, e.SetMinMove(v));
            cov.setterErrors += a;
        }
        else if (op < 6)
        {
            double v = ParamValue(in, { 0., 0.1, 0.5, 0.99, 1.0, -0.01 });
            bool a = t.SetHysteresis(v);
            ASSERT_EQ(a, e.SetHysteresis(v));
            cov.setterErrors += a;
        }
        else if (op < 8)
        {
            double v = ParamValue(in, { 0., 0.7, 1.0, 2.0, 2.0000001, -0.0 });
            bool a = t.SetAggression(v);
            ASSERT_EQ(a, e.SetAggression(v));
            cov.setterErrors += a;
        }
        else
        {
            double x = in.Next(t.MinMove());
            cov.nonFinite += !std::isfinite(x);
            double a = t.result(x);
            double b = e.result(x);
            ASSERT_TRUE(Same("result", a, b)) << "input " << x << " step " << step;
            if (a == 0.)
                cov.hysteresisGated++;
            else
                cov.hysteresisPassed++;
        }
        CompareHysteresis(t, e, tLog, eLog, step);
    }
}

// One seeded run of calls on both ResistSwitch implementations
void RunResistSwitch(uint64_t seed, int steps, Coverage& cov)
{
    SCOPED_TRACE(testing::Message() << "resistswitch seed " << seed);
    InputSource in(seed);
    TwinResistSwitch t;
    ExtractedResistSwitch e;
    CompareResistSwitch(t, e, 0, 0, -1);
    for (int step = 0; step < steps && !testing::Test::HasFatalFailure(); step++)
    {
        size_t tLog = t.DebugLog().size();
        size_t eLog = e.DebugLog().size();
        int op = in.Pick(100);
        if (op < 2)
        {
            t.reset();
            e.reset();
        }
        else if (op < 4)
        {
            double v = ParamValue(in, { 0., 0.05, 0.2, 1.0, -0.01, 1e-300 });
            bool a = t.SetMinMove(v);
            ASSERT_EQ(a, e.SetMinMove(v));
            cov.setterErrors += a;
        }
        else if (op < 6)
        {
            double v = ParamValue(in, { 0., 0.5, 1.0, 1.0000001, -0.0 });
            bool a = t.SetAggression(v);
            ASSERT_EQ(a, e.SetAggression(v));
            cov.setterErrors += a;
        }
        else if (op < 8)
        {
            bool on = in.Pick(2) != 0;
            t.SetFastSwitchEnabled(on);
            e.SetFastSwitchEnabled(on);
        }
        else
        {
            double x = in.Next(t.MinMove());
            cov.nonFinite += !std::isfinite(x);
            double a = t.result(x);
            double b = e.result(x);
            ASSERT_TRUE(Same("result", a, b)) << "input " << x << " step " << step;
            cov.CountResistSwitch(Tail(t.DebugLog(), tLog));
        }
        CompareResistSwitch(t, e, tLog, eLog, step);
    }
}

} // namespace

TEST(GuideAlgorithmParity, HysteresisSeededSequences)
{
    Coverage cov;
    // fewer than ResistSwitch: with hysteresis 0.99 and aggression 2 the output grows 1.98x a
    // frame, and formatting its Debug lines dominates the run time
    for (uint64_t seed = 1; seed <= 1000 && !HasFatalFailure(); seed++)
        RunHysteresis(seed, 300, cov);
    EXPECT_GT(cov.hysteresisGated, 1000);
    EXPECT_GT(cov.hysteresisPassed, 1000);
    EXPECT_GT(cov.nonFinite, 1000);
    EXPECT_GT(cov.setterErrors, 1000);
}

TEST(GuideAlgorithmParity, ResistSwitchSeededSequences)
{
    Coverage cov;
    for (uint64_t seed = 1; seed <= 2000 && !HasFatalFailure(); seed++)
        RunResistSwitch(seed, 300, cov);
    EXPECT_GT(cov.below, 1000);
    EXPECT_GT(cov.weak, 1000);
    EXPECT_GT(cov.notWorse, 1000);
    EXPECT_GT(cov.overshoot, 1000);
    EXPECT_GT(cov.passed, 1000);
    EXPECT_GT(cov.fastSwitch, 1000);
    EXPECT_GT(cov.switched, 1000);
    EXPECT_GT(cov.switchAndVeto, 100);
    EXPECT_GT(cov.nonFinite, 1000);
    EXPECT_GT(cov.setterErrors, 1000);
}

TEST(GuideAlgorithmParity, HysteresisParameterGrid)
{
    // every corner of the parameters, each over its own seeded sequence
    const double minMoves[] = { 0., 0.05, 0.2, 1.0 };
    const double hystereses[] = { 0., 0.1, 0.5, 0.99, 1.0 };
    const double aggressions[] = { 0., 0.7, 1.0, 2.0 };
    uint64_t seed = 100000;
    for (double mm : minMoves)
        for (double h : hystereses)
            for (double a : aggressions)
            {
                SCOPED_TRACE(testing::Message() << "minMove " << mm << " hysteresis " << h << " aggression " << a);
                InputSource in(++seed);
                TwinHysteresis t;
                ExtractedHysteresis e;
                t.SetMinMove(mm);
                e.SetMinMove(mm);
                t.SetHysteresis(h);
                e.SetHysteresis(h);
                t.SetAggression(a);
                e.SetAggression(a);
                for (int step = 0; step < 500; step++)
                {
                    size_t tLog = t.DebugLog().size();
                    size_t eLog = e.DebugLog().size();
                    double x = in.Next(mm);
                    ASSERT_TRUE(Same("result", t.result(x), e.result(x))) << "input " << x << " step " << step;
                    CompareHysteresis(t, e, tLog, eLog, step);
                    ASSERT_FALSE(HasFatalFailure());
                }
            }
}

TEST(GuideAlgorithmParity, ResistSwitchParameterGrid)
{
    // minMove, aggression 0 to 100 % and the fast switch, each corner over its own sequence
    const double minMoves[] = { 1e-9, 0.05, 0.2, 1.0 };
    const double aggressions[] = { 0., 0.01, 0.5, 1.0 };
    uint64_t seed = 200000;
    Coverage cov;
    for (double mm : minMoves)
        for (double a : aggressions)
            for (int fast = 0; fast <= 1; fast++)
            {
                SCOPED_TRACE(testing::Message() << "minMove " << mm << " aggression " << a << " fastSwitch " << fast);
                InputSource in(++seed);
                TwinResistSwitch t;
                ExtractedResistSwitch e;
                t.SetMinMove(mm);
                e.SetMinMove(mm);
                t.SetAggression(a);
                e.SetAggression(a);
                t.SetFastSwitchEnabled(fast != 0);
                e.SetFastSwitchEnabled(fast != 0);
                for (int step = 0; step < 500; step++)
                {
                    size_t tLog = t.DebugLog().size();
                    size_t eLog = e.DebugLog().size();
                    double x = in.Next(mm);
                    ASSERT_TRUE(Same("result", t.result(x), e.result(x))) << "input " << x << " step " << step;
                    cov.CountResistSwitch(Tail(t.DebugLog(), tLog));
                    CompareResistSwitch(t, e, tLog, eLog, step);
                    ASSERT_FALSE(HasFatalFailure());
                }
            }
    EXPECT_GT(cov.fastSwitch, 50);
    EXPECT_GT(cov.overshoot, 50);
    EXPECT_GT(cov.notWorse, 50);
}
