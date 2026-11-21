// The offsets of a random dither (open-astro/openastro-guider#173).
//
// MyFrame::Dither drew DITHER_RANDOM offsets from rand(), and only the simulator camera ever
// called srand, so with any other camera every daemon start began from the C library's
// default state and repeated the same dithers (the first one RA +2.04, Dec -0.63 px for a
// 3 px dither on glibc). DitherRandom has its own std::mt19937, seeded from
// std::random_device, and keeps the old distribution: RA, then Dec unless the dither is RA
// only, each uniform in [-amount, +amount] with both ends included.

#include <gtest/gtest.h>

#include "dither_random.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

namespace
{
constexpr double AMOUNT = 3.0;

std::pair<double, double> Next(DitherRandom& gen, double amount = AMOUNT, bool raOnly = false)
{
    double dRa = 99., dDec = 99.;
    gen.GetDither(amount, raOnly, &dRa, &dDec);
    return { dRa, dDec };
}

// The old mapping of one generator output onto [-1, +1], as rand() / RAND_MAX * 2 - 1
double Unit(std::uint32_t v)
{
    return (v / (double) std::mt19937::max()) * 2.0 - 1.0;
}
} // namespace

// Each daemon start begins with rand() in the same state; two generators made from that state
// must not give the same dither (the bug: they did, every start)
TEST(DitherRandom, DefaultGeneratorsDoNotRepeatAcrossStarts)
{
    srand(1);
    DitherRandom first;
    auto a = Next(first);

    srand(1);
    DitherRandom second;
    auto b = Next(second);

    EXPECT_FALSE(a.first == b.first && a.second == b.second)
        << "two starts gave the same first dither RA " << a.first << " Dec " << a.second;
}

// An injected seed gives the std::mt19937 sequence, mapped as before: RA from the first output,
// Dec from the second. 3499211612 and 581869302 are std::mt19937's first two outputs for its
// default seed 5489, fixed by the C++ standard on every platform.
TEST(DitherRandom, InjectedSeedGivesTheMt19937Sequence)
{
    DitherRandom gen(5489);
    auto d = Next(gen);
    EXPECT_DOUBLE_EQ(d.first, AMOUNT * Unit(3499211612u));
    EXPECT_DOUBLE_EQ(d.second, AMOUNT * Unit(581869302u));

    std::mt19937 ref(173);
    DitherRandom seeded(173);
    for (int i = 0; i < 50; i++)
    {
        auto e = Next(seeded, 2.5);
        double wantRa = 2.5 * Unit(ref());
        double wantDec = 2.5 * Unit(ref());
        ASSERT_DOUBLE_EQ(e.first, wantRa) << "dither " << i;
        ASSERT_DOUBLE_EQ(e.second, wantDec) << "dither " << i;
    }
}

TEST(DitherRandom, SameSeedSameSequence)
{
    DitherRandom a(42), b(42);
    for (int i = 0; i < 20; i++)
    {
        auto x = Next(a);
        auto y = Next(b);
        ASSERT_EQ(x, y) << "dither " << i;
    }
}

// The simulator camera calls srand and rand on its own; they must not move the dither sequence
TEST(DitherRandom, SequenceIsIndependentOfTheCLibraryRand)
{
    DitherRandom alone(7);
    std::vector<std::pair<double, double>> want;
    for (int i = 0; i < 10; i++)
        want.push_back(Next(alone));

    DitherRandom mixed(7);
    for (int i = 0; i < 10; i++)
    {
        srand(2);
        (void) rand();
        ASSERT_EQ(Next(mixed), want[i]) << "dither " << i;
    }
}

// As before: an RA-only dither has no Dec and draws nothing for it, so the next draw goes to RA
TEST(DitherRandom, RaOnlyHasNoDecAndDrawsNoDecValue)
{
    DitherRandom raOnly(11), both(11);
    auto r1 = Next(raOnly, AMOUNT, true);
    auto r2 = Next(raOnly, AMOUNT, true);
    auto b = Next(both);
    EXPECT_EQ(r1.second, 0.0);
    EXPECT_EQ(r2.second, 0.0);
    EXPECT_EQ(r1.first, b.first);
    EXPECT_EQ(r2.first, b.second);
}

// Each axis uniform over [-amount, +amount], both signs, out to the ends; RA and Dec unrelated
TEST(DitherRandom, OffsetsAreUniformWithinTheAmount)
{
    DitherRandom gen(2026);
    constexpr int N = 20000;
    constexpr int BINS = 10;
    int raBins[BINS] = {}, decBins[BINS] = {};
    double raMin = 0., raMax = 0., decMin = 0., decMax = 0., raSum = 0., decSum = 0., cross = 0.;
    for (int i = 0; i < N; i++)
    {
        auto d = Next(gen);
        ASSERT_LE(std::fabs(d.first), AMOUNT);
        ASSERT_LE(std::fabs(d.second), AMOUNT);
        raMin = std::min(raMin, d.first);
        raMax = std::max(raMax, d.first);
        decMin = std::min(decMin, d.second);
        decMax = std::max(decMax, d.second);
        raSum += d.first;
        decSum += d.second;
        cross += d.first * d.second;
        raBins[std::min(BINS - 1, (int) ((d.first + AMOUNT) / (2 * AMOUNT) * BINS))]++;
        decBins[std::min(BINS - 1, (int) ((d.second + AMOUNT) / (2 * AMOUNT) * BINS))]++;
    }
    EXPECT_LT(raMin, -0.999 * AMOUNT);
    EXPECT_GT(raMax, 0.999 * AMOUNT);
    EXPECT_LT(decMin, -0.999 * AMOUNT);
    EXPECT_GT(decMax, 0.999 * AMOUNT);
    // the mean of N uniform draws on [-3, 3] has a standard deviation of 3 / sqrt(3 N) = 0.012
    EXPECT_NEAR(raSum / N, 0.0, 0.05);
    EXPECT_NEAR(decSum / N, 0.0, 0.05);
    // E[RA Dec] = 0 with a standard deviation of 3 / N^0.5 = 0.021
    EXPECT_NEAR(cross / N, 0.0, 0.1);
    for (int b = 0; b < BINS; b++)
    {
        // 2000 expected per bin, a standard deviation of about 42
        EXPECT_NEAR(raBins[b], N / BINS, 200) << "RA bin " << b;
        EXPECT_NEAR(decBins[b], N / BINS, 200) << "Dec bin " << b;
    }
}

TEST(DitherRandom, ZeroAmountGivesNoOffset)
{
    DitherRandom gen;
    auto d = Next(gen, 0.0);
    EXPECT_EQ(d.first, 0.0);
    EXPECT_EQ(d.second, 0.0);
}
