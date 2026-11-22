// star_find::FindStar on synthetic guide frames (#142).
//
// test_star_find pins one star from one FITS fixture, a 0.6 px HFD star that the 1.5 px default
// MinStarHFD would reject. These tests put pixel-integrated Gaussian stars with seeded noise on
// known backgrounds (synthetic_star.h) and check what the centroid, the guide signal, does with
// them: its accuracy at sub-pixel offsets, its precision against SNR, mirror symmetry and a
// background gradient; the HFD of undersampled stars against the default MinStarHFD; SNR; the
// saturation checks (a known saturation ADU with a pedestal, the flat-top heuristic for 8-, 12-
// and 16-bit data, Peak mode's documented gap); and the rejection branches (LowMass, LowSNR and
// the smoothed-peak clamp, LowHFD, HiHFD, a single hot pixel, the subframe-bounded search, a star
// cut by the frame edge).

#include <gtest/gtest.h>

#include "star_find_core.h"
#include "synthetic_star.h"

#include <cmath>
#include <vector>

using star_find::FindStar;
using star_find::Mode;
using star_find::Output;
using star_find::Result;
using star_find::StarImage;

namespace
{
constexpr int W = 200;
constexpr int H = 160;
constexpr double BG = 800.0;

StarImage View(const std::vector<unsigned short>& px, int w, int h, int bpp = 16, unsigned short pedestal = 0)
{
    StarImage im;
    im.data = px.data();
    im.width = w;
    im.height = h;
    im.bitsPerPixel = bpp;
    im.pedestal = pedestal;
    return im;
}

// One star of sigma with a point-sampled peak of peak ADU at (cx, cy) on BG, with noise of
// noiseSigma ADU (none for 0)
std::vector<unsigned short> StarFrame(double cx, double cy, double sigma, double peak, double noiseSigma = 0.0,
                                      uint32_t seed = 1, unsigned clip = 65535)
{
    synth::Frame f(W, H, BG);
    synth::AddStar(f, cx, cy, sigma, synth::FluxForPeak(peak, sigma));
    if (noiseSigma > 0.0)
        synth::AddNoise(f, noiseSigma, seed);
    return synth::Quantize(f, clip);
}

Output Find(const std::vector<unsigned short>& px, int bx, int by, double minHFD = 0.0, double maxHFD = 99.0,
            unsigned short maxADU = 0, Mode mode = Mode::Centroid, int searchRegion = 15)
{
    return FindStar(View(px, W, H), bx, by, searchRegion, mode, minHFD, maxHFD, maxADU);
}

// RMS centroid error per axis over n noisy realisations, and the mean SNR
struct Precision
{
    double rms = 0.0;
    double snr = 0.0;
    int lost = 0;
};

Precision Measure(double peak, double noiseSigma, int n = 200)
{
    Precision p;
    double se = 0.0;
    int found = 0;
    for (int i = 0; i < n; ++i)
    {
        double const cx = 100.3, cy = 80.6;
        std::vector<unsigned short> px = StarFrame(cx, cy, 1.5, peak, noiseSigma, 1000 + i);
        Output o = Find(px, 100, 81);
        if (!o.found)
        {
            ++p.lost;
            continue;
        }
        se += (o.x - cx) * (o.x - cx) + (o.y - cy) * (o.y - cy);
        p.snr += o.snr;
        ++found;
    }
    p.rms = std::sqrt(se / (2.0 * found));
    p.snr /= found;
    return p;
}

#define EXPECT_RESULT(o, r) EXPECT_EQ(static_cast<int>((o).result), static_cast<int>(r))
} // namespace

// --- accuracy ---

TEST(StarFindSynthetic, NoiselessCentroidIsWithinFiveHundredthsOfAPixel)
{
    for (double sigma : { 1.0, 1.5, 2.5 })
        for (double fx : { 0.0, 0.25, 0.5, 0.75 })
            for (double fy : { 0.0, 0.25, 0.5, 0.75 })
            {
                double const cx = 100 + fx, cy = 80 + fy;
                Output o = Find(StarFrame(cx, cy, sigma, 5000), 100, 80);
                // found, and Saturated only where the flat-top heuristic takes a corner-centred
                // star's equal top pixels for a clipped one (CornerCentredNoiselessStarLooksFlatTopped)
                ASSERT_TRUE(o.found) << sigma << " " << fx << " " << fy;
                if (fx != 0.5 || fy != 0.5)
                {
                    EXPECT_RESULT(o, Result::Ok) << sigma << " " << fx << " " << fy;
                }
                EXPECT_NEAR(o.x, cx, 0.05) << sigma << " " << fx << " " << fy;
                EXPECT_NEAR(o.y, cy, 0.05) << sigma << " " << fx << " " << fy;
            }
}

TEST(StarFindSynthetic, TightStarsHaveNoMeasurablePixelLocking)
{
    // sigma 1-1.5 px: the aperture (7 px) and the annulus (7-12 px) hold the whole star
    for (double sigma : { 1.0, 1.5 })
        for (double f : { 0.25, 0.5, 0.75 })
        {
            Output o = Find(StarFrame(100 + f, 80, sigma, 5000), 100, 80);
            EXPECT_NEAR(o.x, 100 + f, 0.002) << sigma << " " << f;
        }
}

TEST(StarFindSynthetic, PrecisionImprovesWithSnr)
{
    Precision const faint = Measure(150, 20); // SNR about 17
    Precision const mid = Measure(300, 20); // about 30
    Precision const bright = Measure(1200, 20); // about 75
    EXPECT_EQ(faint.lost + mid.lost + bright.lost, 0);
    EXPECT_NEAR(faint.snr, 17, 3);
    EXPECT_NEAR(bright.snr, 75, 5);
    EXPECT_LT(faint.rms, 0.25);
    EXPECT_LT(mid.rms, 0.12);
    EXPECT_LT(bright.rms, 0.05);
    EXPECT_GT(faint.rms, mid.rms);
    EXPECT_GT(mid.rms, bright.rms);
}

TEST(StarFindSynthetic, MirroredFrameGivesTheMirroredCentroid)
{
    std::vector<unsigned short> px = StarFrame(100.3, 80.6, 1.5, 2000, 15, 7);
    std::vector<unsigned short> mirrored(px.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            mirrored[(size_t) y * W + (W - 1 - x)] = px[(size_t) y * W + x];

    Output o = Find(px, 100, 81);
    Output m = Find(mirrored, W - 1 - 100, 81);
    ASSERT_TRUE(o.found);
    ASSERT_TRUE(m.found);
    EXPECT_NEAR(m.x, W - 1 - o.x, 1e-6);
    EXPECT_NEAR(m.y, o.y, 1e-6);
    EXPECT_NEAR(m.mass, o.mass, 1e-6);
    EXPECT_NEAR(m.snr, o.snr, 1e-6);
}

TEST(StarFindSynthetic, BackgroundGradientMovesTheCentroidLittle)
{
    synth::Frame f(W, H, BG);
    synth::AddStar(f, 100.3, 80.6, 1.5, synth::FluxForPeak(2000, 1.5));
    synth::AddGradientX(f, 0.5);
    synth::AddNoise(f, 10, 3);
    Output o = Find(synth::Quantize(f), 100, 81);
    ASSERT_TRUE(o.found);
    EXPECT_NEAR(o.x, 100.3, 0.05);
    EXPECT_NEAR(o.y, 80.6, 0.05);
}

TEST(StarFindSynthetic, CornerCentredNoiselessStarLooksFlatTopped)
{
    // Pinned limitation of the flat-top heuristic (maxADU unknown): a noiseless star centred on
    // a pixel corner has four equal top pixels, so max3[0] - max3[2] is 0 and it is reported
    // Saturated although nothing is clipped. Noise makes the top pixels differ in practice.
    Output o = Find(StarFrame(100.5, 80.5, 1.0, 5000), 100, 80);
    EXPECT_TRUE(o.found);
    EXPECT_RESULT(o, Result::Saturated);
    EXPECT_LT(o.peakVal, 65535);
}

// --- HFD against the default MinStarHFD (1.5 px) ---

TEST(StarFindSynthetic, HfdOfUndersampledStars)
{
    // A Gaussian's HFD is 2.355 sigma. RC51's guide scope samples at ~7.7"/px, where a star is
    // sigma 0.5-0.7 px: the default MinStarHFD of 1.5 px rejects sigma 0.5 and keeps 0.7.
    struct Case
    {
        double sigma;
        double hfd;
    } cases[] = { { 0.5, 1.099 }, { 0.7, 1.589 }, { 1.0, 2.381 }, { 2.0, 4.625 } };
    for (const Case& c : cases)
    {
        std::vector<unsigned short> px = StarFrame(100.3, 80.2, c.sigma, 5000 * 2.25 / (c.sigma * c.sigma), 10, 1);
        Output o = Find(px, 100, 80);
        ASSERT_TRUE(o.found) << c.sigma;
        EXPECT_NEAR(o.hfd, c.hfd, 0.01) << c.sigma;
        EXPECT_NEAR(o.hfd, 2.355 * c.sigma, 0.15 * 2.355 * c.sigma) << c.sigma;

        Output withDefault = Find(px, 100, 80, 1.5);
        EXPECT_EQ(withDefault.found, c.sigma >= 0.7) << c.sigma;
        if (!withDefault.found)
        {
            EXPECT_RESULT(withDefault, Result::LowHFD);
        }
    }
}

// --- SNR ---

TEST(StarFindSynthetic, SnrGrowsWithFluxAndFallsWithNoise)
{
    double last = 0.0;
    for (double peak : { 100.0, 200.0, 400.0, 800.0, 1600.0 })
    {
        Output o = Find(StarFrame(100.3, 80.6, 1.5, peak, 20, 11), 100, 81);
        ASSERT_TRUE(o.found) << peak;
        EXPECT_GT(o.snr, last) << peak;
        last = o.snr;
    }
    last = 1e9;
    for (double noise : { 5.0, 10.0, 20.0, 40.0 })
    {
        Output o = Find(StarFrame(100.3, 80.6, 1.5, 800, noise, 11), 100, 81);
        ASSERT_TRUE(o.found) << noise;
        EXPECT_LT(o.snr, last) << noise;
        last = o.snr;
    }
}

TEST(StarFindSynthetic, GoldenSnr)
{
    Output o = Find(StarFrame(100.3, 80.6, 1.5, 800, 20, 11), 100, 81);
    ASSERT_TRUE(o.found);
    EXPECT_NEAR(o.snr, 57.78, 0.01);
    EXPECT_NEAR(o.mass, 10700.84, 0.01);
}

// --- saturation ---

TEST(StarFindSynthetic, ClippedSixteenBitStarIsSaturated)
{
    Output o = Find(StarFrame(100.3, 80.6, 1.5, 200000, 10, 1), 100, 81);
    EXPECT_TRUE(o.found);
    EXPECT_RESULT(o, Result::Saturated);
    EXPECT_EQ(o.peakVal, 65535);
}

TEST(StarFindSynthetic, UnclippedSixteenBitStarIsNotSaturated)
{
    Output o = Find(StarFrame(100.3, 80.6, 1.5, 30000, 10, 1), 100, 81);
    EXPECT_TRUE(o.found);
    EXPECT_RESULT(o, Result::Ok);
}

TEST(StarFindSynthetic, KnownSaturationAduIsComparedWithoutThePedestal)
{
    // a 12-bit camera (maxADU 4095) delivering a pedestal of 200: the raw peak 4295 is saturated,
    // 4294 is not, whatever the flat-top heuristic would say
    for (unsigned clip : { 4295u, 4294u })
    {
        std::vector<unsigned short> px = StarFrame(100.3, 80.6, 1.5, 20000, 10, 1, clip);
        Output o = FindStar(View(px, W, H, 16, 200), 100, 81, 15, Mode::Centroid, 0.0, 99.0, 4095);
        EXPECT_TRUE(o.found) << clip;
        EXPECT_RESULT(o, clip == 4295u ? Result::Saturated : Result::Ok);
    }
}

TEST(StarFindSynthetic, EightBitFlatTopUsesItsOwnThreshold)
{
    // top three pixels 255, 254, 254: d = 1, so d * 191 < 255 flags an 8-bit star, while the
    // >= 12-bit rule (d * 65535 < 32 * 255) does not
    synth::Frame f(W, H, 20);
    synth::AddStar(f, 100, 80, 1.5, synth::FluxForPeak(200, 1.5));
    std::vector<unsigned short> px = synth::Quantize(f, 255);
    px[(size_t) 80 * W + 100] = 255;
    px[(size_t) 80 * W + 101] = 254;
    px[(size_t) 81 * W + 100] = 254;

    Output eight = FindStar(View(px, W, H, 8), 100, 80, 15, Mode::Centroid, 0.0, 99.0, 0);
    EXPECT_TRUE(eight.found);
    EXPECT_RESULT(eight, Result::Saturated);
    Output sixteen = FindStar(View(px, W, H, 16), 100, 80, 15, Mode::Centroid, 0.0, 99.0, 0);
    EXPECT_TRUE(sixteen.found);
    EXPECT_RESULT(sixteen, Result::Ok);
}

TEST(StarFindSynthetic, TwelveBitDataScaledToSixteenBitsIsSaturatedWhenClipped)
{
    // 12-bit data shifted up 4 bits clips at 65520, not 65535
    synth::Frame f(W, H, BG);
    synth::AddStar(f, 100.3, 80.6, 1.5, synth::FluxForPeak(200000, 1.5));
    synth::AddNoise(f, 10, 1);
    std::vector<unsigned short> px = synth::Quantize(f, 65520);
    for (unsigned short& v : px)
        v = (unsigned short) (v & ~15u);
    Output o = Find(px, 100, 81);
    EXPECT_TRUE(o.found);
    EXPECT_RESULT(o, Result::Saturated);
}

TEST(StarFindSynthetic, PeakModeNeverReportsSaturation)
{
    // the documented limitation in star_find_core.h: a change here is a deliberate one
    Output o = Find(StarFrame(100.3, 80.6, 1.5, 200000, 10, 1), 100, 81, 0.0, 99.0, 0, Mode::Peak);
    EXPECT_TRUE(o.found);
    EXPECT_RESULT(o, Result::Ok);
}

// --- rejection branches ---

TEST(StarFindSynthetic, HotPixelIsRejectedByTheDefaultMinHfd)
{
    synth::Frame f(W, H, BG);
    synth::AddNoise(f, 10, 4);
    std::vector<unsigned short> px = synth::Quantize(f);
    px[(size_t) 80 * W + 100] = 3000;
    px[(size_t) 60 * W + 40] = 3000;
    px[(size_t) 120 * W + 150] = 3000;

    Output o = Find(px, 100, 80, 1.5);
    EXPECT_FALSE(o.found);
    EXPECT_RESULT(o, Result::LowHFD);

    // with no HFD floor the lone pixel is a "star" of HFD 0.5 (hfr 0.25 for a single pixel)
    Output any = Find(px, 100, 80, 0.0);
    EXPECT_TRUE(any.found);
    EXPECT_DOUBLE_EQ(any.hfd, 0.5);
    EXPECT_DOUBLE_EQ(any.x, 100.0);
    EXPECT_DOUBLE_EQ(any.y, 80.0);
}

TEST(StarFindSynthetic, ScatteredNoisePixelsOverThresholdAreLowSnr)
{
    // pixels over threshold whose smoothed peak is not: the SNR is clamped under LOW_SNR
    synth::Frame f(W, H, BG);
    synth::AddNoise(f, 10, 4);
    std::vector<unsigned short> px = synth::Quantize(f);
    px[(size_t) 78 * W + 98] = 870;
    px[(size_t) 82 * W + 103] = 870;
    px[(size_t) 80 * W + 100] = 870;
    Output o = Find(px, 100, 80);
    EXPECT_FALSE(o.found);
    EXPECT_RESULT(o, Result::LowSNR);
}

TEST(StarFindSynthetic, VeryFaintStarIsLowSnr)
{
    // a peak of 3 noise sigma
    Output o = Find(StarFrame(100.3, 80.6, 1.5, 30, 10, 5), 100, 81);
    EXPECT_FALSE(o.found);
    EXPECT_RESULT(o, Result::LowSNR);
}

TEST(StarFindSynthetic, TinyMassIsLowMass)
{
    // noiseless: the threshold is the background, and 5 ADU over it is all the mass there is
    std::vector<unsigned short> px((size_t) W * H, 800);
    px[(size_t) 80 * W + 100] = 805;
    Output o = Find(px, 100, 80);
    EXPECT_FALSE(o.found);
    EXPECT_RESULT(o, Result::LowMass);
}

TEST(StarFindSynthetic, BloatedStarIsHiHfd)
{
    Output o = Find(StarFrame(100.3, 80.6, 6.0, 3000, 10, 1), 100, 81, 0.0, 5.0);
    EXPECT_FALSE(o.found);
    EXPECT_RESULT(o, Result::HiHFD);
}

TEST(StarFindSynthetic, SearchStaysInsideTheSubframe)
{
    // a brighter star 5 px outside the subframe's right edge, inside the search region, is
    // never picked
    synth::Frame f(W, H, BG);
    synth::AddStar(f, 100, 80, 1.5, synth::FluxForPeak(2000, 1.5));
    synth::AddStar(f, 114, 80, 1.5, synth::FluxForPeak(20000, 1.5));
    synth::AddNoise(f, 10, 6);
    std::vector<unsigned short> px = synth::Quantize(f);

    StarImage im = View(px, W, H);
    im.subframeEmpty = false;
    im.subLeft = 85;
    im.subRight = 109;
    im.subTop = 65;
    im.subBottom = 95;
    Output o = FindStar(im, 100, 80, 15, Mode::Centroid, 0.0, 99.0, 0);
    ASSERT_TRUE(o.found);
    EXPECT_NEAR(o.x, 100.0, 0.1);
    EXPECT_NEAR(o.y, 80.0, 0.1);

    // without the subframe the brighter star wins
    Output whole = Find(px, 100, 80);
    ASSERT_TRUE(whole.found);
    EXPECT_NEAR(whole.x, 114.0, 0.1);
}

TEST(StarFindSynthetic, StarCutByTheFrameEdgeIsFoundWithABiasedCentroid)
{
    // STAR_TOO_NEAR_EDGE is never produced: a star centred on the first column is found, its
    // centroid pulled into the frame by the missing half. Pinned so a change is deliberate.
    Output o = Find(StarFrame(0.0, 80.0, 1.5, 5000, 10, 1), 2, 80);
    ASSERT_TRUE(o.found);
    EXPECT_RESULT(o, Result::Ok);
    EXPECT_NEAR(o.x, 0.925, 0.005);
    EXPECT_NEAR(o.y, 80.0, 0.02);
}

// --- a background annulus of 1-9 pixels ---

namespace
{
Output FindInSmallFrame(int size, double cx, double cy)
{
    synth::Frame f(size, size, 100);
    synth::AddStar(f, cx, cy, 1.0, synth::FluxForPeak(3000, 1.0));
    synth::AddNoise(f, 3, 5);
    std::vector<unsigned short> px = synth::Quantize(f);
    return FindStar(View(px, size, size), (int) cx, (int) cy, 7, Mode::Centroid, 0.0, 99.0, 0);
}
} // namespace

TEST(StarFindSynthetic, AnnulusOfFewerThanTenPixelsGivesNoBackground)
{
    // In a 10 or 11 px frame the 7-12 px annulus around a centred star has 1-9 in-bounds pixels.
    // The background loop stopped on its first pass with mean and sigma still 0, so the
    // threshold was 0: every aperture pixel, sky included, counted as star (mass 28706 for a star
    // of 18850), the SNR was inflated and the centroid pulled toward the frame centre, and the
    // star came back Ok. Like an empty annulus (EmptyBackgroundAnnulusIsNotFoundNotNaN in
    // test_star_find), there is no background estimate: not found, LowSNR.
    for (int size : { 10, 11 })
    {
        Output o = FindInSmallFrame(size, size / 2, size / 2);
        EXPECT_FALSE(o.found) << size;
        EXPECT_RESULT(o, Result::LowSNR);
    }
}

TEST(StarFindSynthetic, AnnulusOfTenPixelsOrMoreIsABackground)
{
    // from 12 px the annulus has enough pixels: the star is found with its own mass
    for (int size : { 12, 15, 20 })
    {
        Output o = FindInSmallFrame(size, size / 2, size / 2);
        ASSERT_TRUE(o.found) << size;
        EXPECT_NEAR(o.x, size / 2, 0.02) << size;
        EXPECT_NEAR(o.mass, synth::FluxForPeak(3000, 1.0), 300) << size;
    }
}
