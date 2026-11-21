// Unit tests for src/guide_history.cpp.
//
// GuideHistory is the data store behind the JSON-RPC get_guide_history
// method, moved out of the History graph window (#115 step 2). These tests
// link the REAL guide_history.cpp and guiding_stats.cpp and pin the values
// get_guide_history reports:
//   - the ring of the last /graph/maxLength steps (oldest first);
//   - n = min(steps, /graph/length);
//   - osc index and the RA/Dec limited counts over the raw last n steps;
//   - rms and peak over the last /graph/length steps that are neither the
//     first step after a dither nor a step taken while settling;
//   - star_lost, counted until the next reset;
// against an independent brute-force model, plus the quirks that come with
// the original code (no clamp of the length to the capacity).

#include <gtest/gtest.h>

#include "guide_history.h"

#include <cmath>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

// guide_history.h only forward-declares this (the real one is in guidinglog.h
// and carries a wxString). GuideHistory never looks inside it.
struct FrameDroppedInfo
{
};

namespace
{

constexpr std::int64_t kTimeBase = 1795123456789;

S_HISTORY MakeStep(double ra, double dec, bool raLimited = false, bool decLimited = false)
{
    static std::int64_t s_t = kTimeBase;
    S_HISTORY h {};
    h.timestamp = (s_t += 1000);
    h.dx = ra * 0.5;
    h.dy = dec * 0.5;
    h.ra = ra;
    h.dec = dec;
    h.starSNR = 30.0;
    h.starMass = 5000.0;
    h.raDur = (int) (ra * 100);
    h.decDur = (int) (dec * 100);
    h.raDir = 2;
    h.decDir = 0;
    h.raLimited = raLimited;
    h.decLimited = decLimited;
    return h;
}

double PopulationSigma(const std::vector<double>& v)
{
    if (v.size() <= 1)
        return 0.0;
    double mean = 0.0;
    for (double x : v)
        mean += x;
    mean /= v.size();
    double ss = 0.0;
    for (double x : v)
        ss += (x - mean) * (x - mean);
    return std::sqrt(ss / v.size());
}

double Peak(const std::vector<double>& v)
{
    double peak = 0.0;
    for (double x : v)
        peak = std::max(peak, std::fabs(x));
    return peak;
}

// Brute-force model of what get_guide_history reports, computed from the full
// list of steps since the last reset.
struct Reference
{
    struct Entry
    {
        S_HISTORY step;
        bool excluded; // left out of rms and peak
    };

    unsigned int capacity;
    unsigned int length;
    std::deque<Entry> all; // every step since the last reset, oldest first
    bool ditherPending = false;
    unsigned int starLost = 0;

    Reference(unsigned int capacity_, unsigned int length_) : capacity(capacity_), length(length_) { }

    void Step(const S_HISTORY& s, bool settling)
    {
        bool excluded;
        if (ditherPending)
        {
            ditherPending = false;
            excluded = true;
        }
        else
            excluded = settling;
        all.push_back({ s, excluded });
    }
    void Dither() { ditherPending = true; }
    void StarLost() { ++starLost; }
    void Reset()
    {
        all.clear();
        ditherPending = false;
        starLost = 0;
    }

    unsigned int HistorySize() const { return std::min<unsigned int>(all.size(), capacity); }
    unsigned int Nr() const { return std::min(HistorySize(), length); }

    // the last Nr() steps, oldest first
    std::vector<S_HISTORY> Window() const
    {
        std::vector<S_HISTORY> w;
        for (size_t i = all.size() - Nr(); i < all.size(); i++)
            w.push_back(all[i].step);
        return w;
    }

    double OscIndex() const
    {
        unsigned int nr = Nr();
        if (nr < 2)
            return 0.0;
        std::vector<S_HISTORY> w = Window();
        int same = 0;
        for (size_t i = 0; i + 1 < w.size(); i++)
            if (w[i].ra * w[i + 1].ra > 0.0)
                ++same;
        return 1.0 - (double) same / (double) (nr - 1);
    }

    unsigned int RaLimitCnt() const
    {
        unsigned int n = 0;
        for (const S_HISTORY& h : Window())
            n += h.raLimited ? 1 : 0;
        return n;
    }
    unsigned int DecLimitCnt() const
    {
        unsigned int n = 0;
        for (const S_HISTORY& h : Window())
            n += h.decLimited ? 1 : 0;
        return n;
    }

    // the last `length` RA (or Dec) values that are not excluded
    std::vector<double> Included(bool ra) const
    {
        std::vector<double> v;
        for (auto it = all.rbegin(); it != all.rend() && v.size() < length; ++it)
            if (!it->excluded)
                v.insert(v.begin(), ra ? it->step.ra : it->step.dec);
        return v;
    }
};

void ExpectSameStep(const S_HISTORY& a, const S_HISTORY& b)
{
    EXPECT_EQ(a.timestamp, b.timestamp);
    EXPECT_EQ(a.dx, b.dx);
    EXPECT_EQ(a.dy, b.dy);
    EXPECT_EQ(a.ra, b.ra);
    EXPECT_EQ(a.dec, b.dec);
    EXPECT_EQ(a.starSNR, b.starSNR);
    EXPECT_EQ(a.starMass, b.starMass);
    EXPECT_EQ(a.raDur, b.raDur);
    EXPECT_EQ(a.decDur, b.decDur);
    EXPECT_EQ(a.raDir, b.raDir);
    EXPECT_EQ(a.decDir, b.decDir);
    EXPECT_EQ(a.raLimited, b.raLimited);
    EXPECT_EQ(a.decLimited, b.decLimited);
}

void ExpectMatches(const GuideHistory& gh, const Reference& ref)
{
    const SummaryStats& s = gh.Stats();
    ASSERT_EQ(gh.History().size(), ref.HistorySize());
    EXPECT_EQ(s.nr, ref.Nr());

    double osc = ref.OscIndex();
    EXPECT_NEAR(s.osc_index, osc, 1e-12);
    if (ref.Nr() >= 2)
        EXPECT_EQ(s.osc_alert, s.osc_index > 0.6 || s.osc_index < 0.15);
    else
        EXPECT_FALSE(s.osc_alert);

    EXPECT_EQ(s.ra_limit_cnt, ref.RaLimitCnt());
    EXPECT_EQ(s.dec_limit_cnt, ref.DecLimitCnt());
    EXPECT_EQ(s.star_lost_cnt, ref.starLost);

    std::vector<double> ra = ref.Included(true);
    std::vector<double> dec = ref.Included(false);
    // Compare variances: WindowedAxisStats keeps running sums, so a window whose
    // true variance is 0 can come out as ~1e-15, i.e. a sigma of ~1e-7.
    double sigmaRa = PopulationSigma(ra);
    double sigmaDec = PopulationSigma(dec);
    EXPECT_NEAR(s.rms_ra * s.rms_ra, sigmaRa * sigmaRa, 1e-9);
    EXPECT_NEAR(s.rms_dec * s.rms_dec, sigmaDec * sigmaDec, 1e-9);
    EXPECT_NEAR(s.rms_tot * s.rms_tot, sigmaRa * sigmaRa + sigmaDec * sigmaDec, 1e-9);
    EXPECT_DOUBLE_EQ(s.rms_tot, std::hypot(s.rms_ra, s.rms_dec));
    // NEAR, not DOUBLE_EQ: when every value in the window is 0, WindowedAxisStats
    // reports its initial maxDisplacement (DBL_MIN) as the peak
    EXPECT_NEAR(s.ra_peak, Peak(ra), 1e-12);
    EXPECT_NEAR(s.dec_peak, Peak(dec), 1e-12);

    if (!ref.all.empty())
    {
        ExpectSameStep(s.cur, ref.all.back().step);
        ExpectSameStep(gh.History()[gh.History().size() - 1], ref.all.back().step);
    }
}

void ExpectZeroStats(const GuideHistory& gh)
{
    const SummaryStats& s = gh.Stats();
    EXPECT_EQ(gh.History().size(), 0u);
    EXPECT_EQ(s.nr, 0u);
    EXPECT_EQ(s.rms_ra, 0.0);
    EXPECT_EQ(s.rms_dec, 0.0);
    EXPECT_EQ(s.rms_tot, 0.0);
    EXPECT_EQ(s.osc_index, 0.0);
    EXPECT_FALSE(s.osc_alert);
    EXPECT_EQ(s.ra_peak, 0.0);
    EXPECT_EQ(s.dec_peak, 0.0);
    EXPECT_EQ(s.star_lost_cnt, 0u);
    EXPECT_EQ(s.ra_limit_cnt, 0u);
    EXPECT_EQ(s.dec_limit_cnt, 0u);
    ExpectSameStep(s.cur, S_HISTORY {});
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and validation (graph.cpp SetMinLength / SetMaxLength)
// ---------------------------------------------------------------------------

TEST(GuideHistory, ValidMinLength)
{
    EXPECT_EQ(GuideHistory::ValidMinLength(0), 50u);
    EXPECT_EQ(GuideHistory::ValidMinLength(1), 1u);
    EXPECT_EQ(GuideHistory::ValidMinLength(75), 75u);
}

TEST(GuideHistory, DefaultsMatchTheGraph)
{
    EXPECT_EQ((int) GuideHistory::DefaultMinLength, 50);
    EXPECT_EQ((int) GuideHistory::DefaultMaxLength, 400);
}

TEST(GuideHistory, ConstructorValidatesLengths)
{
    GuideHistory a(50, 400, 100, kTimeBase);
    EXPECT_EQ(a.MinLength(), 50u);
    EXPECT_EQ(a.Capacity(), 400u);
    EXPECT_EQ(a.Length(), 100u);

    // minLength < 1 -> default; maxLength below it -> minLength
    GuideHistory b(0, 10, 7, kTimeBase);
    EXPECT_EQ(b.MinLength(), 50u);
    EXPECT_EQ(b.Capacity(), 50u);
    EXPECT_EQ(b.Length(), 7u);

    GuideHistory c(20, 5, 40, kTimeBase);
    EXPECT_EQ(c.MinLength(), 20u);
    EXPECT_EQ(c.Capacity(), 20u);
    EXPECT_EQ(c.Length(), 40u); // the length is not clamped to the capacity
}

TEST(GuideHistory, StartsEmpty)
{
    GuideHistory gh(50, 400, 100, kTimeBase);
    ExpectZeroStats(gh);
}

// ---------------------------------------------------------------------------
// History ring
// ---------------------------------------------------------------------------

TEST(GuideHistory, HistoryIsOldestFirstAndWraps)
{
    const unsigned int capacity = 10;
    GuideHistory gh(5, capacity, 10, kTimeBase);
    std::vector<S_HISTORY> steps;
    for (int i = 0; i < 13; i++)
    {
        S_HISTORY s = MakeStep(i + 1, -(i + 1), i % 2 == 0, i % 3 == 0);
        s.raDir = (char) (i % 4);
        s.decDir = (char) (3 - i % 4);
        steps.push_back(s);
        gh.AppendData(s, false);
    }

    const circular_buffer<S_HISTORY>& h = gh.History();
    ASSERT_EQ(h.size(), capacity);
    for (unsigned int i = 0; i < capacity; i++)
        ExpectSameStep(h[i], steps[3 + i]);
    ExpectSameStep(h[0], steps[3]);
    ExpectSameStep(h[h.size() - 1], steps.back());
    ExpectSameStep(gh.Stats().cur, steps.back());
}

// ---------------------------------------------------------------------------
// Dither and settling exclusion
// ---------------------------------------------------------------------------

TEST(GuideHistory, StepAfterDitherIsLeftOutOfRmsAndPeak)
{
    GuideHistory gh(5, 50, 20, kTimeBase);
    gh.AppendData(MakeStep(1.0, 1.0), false);
    gh.AppendData(MakeStep(-1.0, -1.0), false);
    gh.AppendData(DitherInfo { kTimeBase, 3.0, 3.0 });
    gh.AppendData(MakeStep(9.0, -9.0), false); // excluded

    const SummaryStats& s = gh.Stats();
    EXPECT_EQ(s.nr, 3u);
    EXPECT_DOUBLE_EQ(s.rms_ra, 1.0);
    EXPECT_DOUBLE_EQ(s.rms_dec, 1.0);
    EXPECT_DOUBLE_EQ(s.ra_peak, 1.0);
    EXPECT_DOUBLE_EQ(s.dec_peak, 1.0);
    // the excluded step still counts for the osc index: signs +, -, + -> no same-side pairs
    EXPECT_DOUBLE_EQ(s.osc_index, 1.0);

    gh.AppendData(MakeStep(5.0, 5.0), false); // included again
    EXPECT_EQ(gh.Stats().nr, 4u);
    EXPECT_DOUBLE_EQ(gh.Stats().ra_peak, 5.0);
    EXPECT_DOUBLE_EQ(gh.Stats().rms_ra, PopulationSigma({ 1.0, -1.0, 5.0 }));
}

TEST(GuideHistory, OnlyTheFirstStepAfterTwoDithersIsLeftOut)
{
    GuideHistory gh(5, 50, 20, kTimeBase);
    gh.AppendData(DitherInfo { kTimeBase, 1.0, 1.0 });
    gh.AppendData(DitherInfo { kTimeBase, 1.0, 1.0 });
    gh.AppendData(MakeStep(9.0, 9.0), false); // excluded
    gh.AppendData(MakeStep(2.0, 2.0), false);
    EXPECT_DOUBLE_EQ(gh.Stats().ra_peak, 2.0);
}

TEST(GuideHistory, SettlingStepsAreLeftOutOfRmsAndPeakOnly)
{
    GuideHistory gh(5, 50, 20, kTimeBase);
    gh.AppendData(MakeStep(1.0, 2.0), false);
    gh.AppendData(MakeStep(8.0, 8.0, true, true), true);
    gh.AppendData(MakeStep(-8.0, -8.0, true, false), true);
    gh.AppendData(MakeStep(-1.0, -2.0), false);

    const SummaryStats& s = gh.Stats();
    EXPECT_EQ(s.nr, 4u);
    EXPECT_DOUBLE_EQ(s.ra_peak, 1.0);
    EXPECT_DOUBLE_EQ(s.dec_peak, 2.0);
    EXPECT_DOUBLE_EQ(s.rms_ra, 1.0);
    EXPECT_DOUBLE_EQ(s.rms_dec, 2.0);
    EXPECT_EQ(s.ra_limit_cnt, 2u);
    EXPECT_EQ(s.dec_limit_cnt, 1u);
    // pairs (1,8) same, (8,-8) not, (-8,-1) same -> 1 - 2/3
    EXPECT_DOUBLE_EQ(s.osc_index, 1.0 - 2.0 / 3.0);
}

TEST(GuideHistory, DitherFlagIsClearedEvenBySettlingStep)
{
    // m_ditherStarted is checked before settling, so a settling step after a
    // dither uses up the dither and the next step is included.
    GuideHistory gh(5, 50, 20, kTimeBase);
    gh.AppendData(DitherInfo { kTimeBase, 1.0, 1.0 });
    gh.AppendData(MakeStep(9.0, 9.0), true);
    gh.AppendData(MakeStep(3.0, 3.0), false);
    EXPECT_DOUBLE_EQ(gh.Stats().ra_peak, 3.0);
}

// ---------------------------------------------------------------------------
// Osc index and limit counts
// ---------------------------------------------------------------------------

TEST(GuideHistory, OscIndex)
{
    GuideHistory alt(5, 100, 20, kTimeBase);
    alt.AppendData(MakeStep(1.0, 0.0), false);
    EXPECT_EQ(alt.Stats().osc_index, 0.0); // nr < 2
    EXPECT_FALSE(alt.Stats().osc_alert);
    for (int i = 1; i < 50; i++)
        alt.AppendData(MakeStep(i % 2 ? -1.0 : 1.0, 0.0), false);
    EXPECT_DOUBLE_EQ(alt.Stats().osc_index, 1.0);
    EXPECT_TRUE(alt.Stats().osc_alert); // > 0.6

    GuideHistory same(5, 100, 20, kTimeBase);
    for (int i = 0; i < 50; i++)
        same.AppendData(MakeStep(0.5, 0.0), false);
    EXPECT_DOUBLE_EQ(same.Stats().osc_index, 0.0);
    EXPECT_TRUE(same.Stats().osc_alert); // < 0.15

    // a zero offset is on neither side
    GuideHistory zero(5, 100, 20, kTimeBase);
    zero.AppendData(MakeStep(0.0, 0.0), false);
    zero.AppendData(MakeStep(0.0, 0.0), false);
    EXPECT_DOUBLE_EQ(zero.Stats().osc_index, 1.0);
}

TEST(GuideHistory, OscIndexAndLimitCountsSlideWithTheWindow)
{
    const unsigned int length = 8;
    GuideHistory gh(4, 30, length, kTimeBase);
    Reference ref(30, length);
    // runs of same-sign RA and limited flags, so pairs leave the window
    const double ras[] = { 1, 2, 3, -1, -2, 4, -5, 6, 7, 8, 9, -1, -1, -1, 2, 2, 2, 2, -3, 4, -5, 6, 1, 1, 1 };
    int i = 0;
    for (double ra : ras)
    {
        S_HISTORY s = MakeStep(ra, -ra, i % 3 == 0, i % 5 < 2);
        gh.AppendData(s, false);
        ref.Step(s, false);
        ExpectMatches(gh, ref);
        ++i;
    }
}

// ---------------------------------------------------------------------------
// Star lost
// ---------------------------------------------------------------------------

TEST(GuideHistory, StarLostIsCumulativeUntilReset)
{
    GuideHistory gh(2, 4, 3, kTimeBase);
    FrameDroppedInfo lost;
    gh.AppendData(lost);
    for (int i = 0; i < 10; i++) // wraps the 4-step ring twice
    {
        gh.AppendData(MakeStep(1.0, 1.0), false);
        gh.AppendData(lost);
    }
    EXPECT_EQ(gh.Stats().star_lost_cnt, 11u);
    EXPECT_EQ(gh.History().size(), 4u);
    EXPECT_EQ(gh.Stats().nr, 3u);

    gh.ResetData();
    EXPECT_EQ(gh.Stats().star_lost_cnt, 0u);
}

// ---------------------------------------------------------------------------
// ResetData
// ---------------------------------------------------------------------------

TEST(GuideHistory, ResetDataClearsEverythingButTheSizes)
{
    GuideHistory gh(5, 20, 10, kTimeBase);
    FrameDroppedInfo lost;
    for (int i = 0; i < 30; i++)
        gh.AppendData(MakeStep(i % 2 ? 3.0 : -2.0, 1.5, true, true), false);
    gh.AppendData(lost);
    gh.AppendData(DitherInfo { kTimeBase, 1.0, 1.0 });
    ASSERT_GT(gh.Stats().ra_peak, 0.0);

    gh.ResetData();
    ExpectZeroStats(gh);
    EXPECT_EQ(gh.Capacity(), 20u);
    EXPECT_EQ(gh.Length(), 10u);
    EXPECT_EQ(gh.MinLength(), 5u);

    // the dither before the reset is forgotten: the next step counts
    gh.AppendData(MakeStep(4.0, -6.0), false);
    EXPECT_DOUBLE_EQ(gh.Stats().ra_peak, 4.0);
    EXPECT_DOUBLE_EQ(gh.Stats().dec_peak, 6.0);
    EXPECT_EQ(gh.Stats().nr, 1u);
    EXPECT_EQ(gh.Stats().ra_limit_cnt, 0u);
}

// ---------------------------------------------------------------------------
// Randomized comparison with the brute-force reference
// ---------------------------------------------------------------------------

class GuideHistoryRandom : public ::testing::TestWithParam<std::pair<unsigned int, unsigned int>>
{
};

TEST_P(GuideHistoryRandom, MatchesReference)
{
    const unsigned int capacity = GetParam().first;
    const unsigned int length = GetParam().second;
    GuideHistory gh(2, capacity, length, kTimeBase);
    Reference ref(capacity, length);

    std::mt19937 rng(12345 + capacity * 7 + length);
    std::uniform_real_distribution<double> offset(-3.0, 3.0);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    FrameDroppedInfo lost;

    for (int i = 0; i < 2000; i++)
    {
        double p = u(rng);
        if (p < 0.05)
        {
            gh.AppendData(DitherInfo { kTimeBase + i, 1.0, -1.0 });
            ref.Dither();
        }
        else if (p < 0.08)
        {
            gh.AppendData(lost);
            ref.StarLost();
        }
        else if (p < 0.083)
        {
            gh.ResetData();
            ref.Reset();
        }
        else
        {
            double ra = u(rng) < 0.03 ? 0.0 : offset(rng);
            S_HISTORY s = MakeStep(ra, offset(rng), u(rng) < 0.2, u(rng) < 0.2);
            bool settling = u(rng) < 0.1;
            gh.AppendData(s, settling);
            ref.Step(s, settling);
        }
        ExpectMatches(gh, ref);
        if (::testing::Test::HasFailure())
        {
            ADD_FAILURE() << "first mismatch after event " << i;
            return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(CapacityAndLength, GuideHistoryRandom,
                         ::testing::Values(std::make_pair(40u, 25u), std::make_pair(30u, 30u), std::make_pair(60u, 2u),
                                           std::make_pair(400u, 100u)));

// ---------------------------------------------------------------------------
// Parity pin: /graph/length larger than /graph/maxLength
// ---------------------------------------------------------------------------
// graph.cpp never clamped /graph/length to the capacity, and get_guide_history
// has always reported what follows from that. This test documents the current
// behaviour so a future clamp is a deliberate API change, not an accident:
// with length > capacity the window never fills, so the osc counter and the
// limit counts never drop the oldest step, while rms and peak still use the
// last `length` steps.

TEST(GuideHistory, ParityPinLengthAboveCapacityIsNotClamped)
{
    const unsigned int capacity = 10;
    const unsigned int length = 20;
    GuideHistory gh(5, capacity, length, kTimeBase);

    std::vector<double> ras;
    for (int i = 1; i <= 30; i++)
    {
        gh.AppendData(MakeStep(i, -i, true, true), false);
        ras.push_back(i);
    }

    const SummaryStats& s = gh.Stats();
    EXPECT_EQ(gh.History().size(), capacity);
    EXPECT_EQ(s.nr, capacity);

    // limit counts keep growing past the ring size
    EXPECT_EQ(s.ra_limit_cnt, 30u);
    EXPECT_EQ(s.dec_limit_cnt, 30u);

    // 29 same-side pairs over nr - 1 = 9 -> the osc index goes below 0
    EXPECT_DOUBLE_EQ(s.osc_index, 1.0 - 29.0 / 9.0);
    EXPECT_LT(s.osc_index, 0.0);
    EXPECT_TRUE(s.osc_alert);

    // rms and peak use the last 20 steps (11..30), not the 10 in the ring
    std::vector<double> last20(ras.end() - 20, ras.end());
    EXPECT_NEAR(s.rms_ra, PopulationSigma(last20), 1e-9);
    EXPECT_DOUBLE_EQ(s.ra_peak, 30.0);
    EXPECT_NE(PopulationSigma(last20), PopulationSigma(std::vector<double>(ras.end() - 10, ras.end())));
}
