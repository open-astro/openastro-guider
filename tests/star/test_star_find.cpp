// Unit tests for src/star_find_core.cpp — the star-detection pixel math
// extracted from Star::Find (see star_find_core.h). This is the one tier that
// can't use the shadow-phd.h pattern: the production star.cpp/usImage.cpp
// drag in ~half the app. Instead we link the wx-free kernel and feed it a real
// FITS fixture loaded directly with cfitsio (no wx, no usImage, no wxApp).
//
// The fixtures (simimage.fit etc.) ship in the repo root; PHD_FIXTURE_DIR is
// passed in by CMake. We seed the search at the image's brightest pixel and
// pin the detected centroid / mass / SNR / HFD as golden values — a change to
// the detection math will move them and fail the test.

#include <gtest/gtest.h>

#include "star_find_core.h"

#include <fitsio.h>

#include <string>
#include <vector>

#ifndef PHD_FIXTURE_DIR
# define PHD_FIXTURE_DIR "."
#endif

namespace
{

struct Fits
{
    std::vector<unsigned short> pixels;
    int width = 0;
    int height = 0;
};

// Minimal 2D FITS loader via cfitsio — mirrors what usImage::Load does for the
// primary image plane, without the wx/usImage wrapper.
bool LoadFits(const std::string& path, Fits& out)
{
    fitsfile *fptr = nullptr;
    int status = 0;
    if (fits_open_diskfile(&fptr, path.c_str(), READONLY, &status))
        return false;

    int naxis = 0;
    long naxes[2] = { 0, 0 };
    fits_get_img_dim(fptr, &naxis, &status);
    fits_get_img_size(fptr, 2, naxes, &status);
    if (status || naxis != 2)
    {
        fits_close_file(fptr, &status);
        return false;
    }

    out.width = (int) naxes[0];
    out.height = (int) naxes[1];
    out.pixels.resize((size_t) out.width * out.height);

    long fpixel[2] = { 1, 1 };
    fits_read_pix(fptr, TUSHORT, fpixel, (LONGLONG) out.pixels.size(), nullptr, out.pixels.data(), nullptr, &status);
    fits_close_file(fptr, &status);
    return status == 0;
}

star_find::StarImage viewOf(const Fits& f)
{
    star_find::StarImage im;
    im.data = f.pixels.data();
    im.width = f.width;
    im.height = f.height;
    im.subframeEmpty = true;
    im.pedestal = 0;
    im.bitsPerPixel = 16;
    return im;
}

// Brightest raw pixel — a stable seed for the search.
void brightestPixel(const Fits& f, int& bx, int& by)
{
    unsigned short best = 0;
    bx = f.width / 2;
    by = f.height / 2;
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x)
        {
            unsigned short v = f.pixels[(size_t) y * f.width + x];
            if (v > best)
            {
                best = v;
                bx = x;
                by = y;
            }
        }
}

const std::string kSimImage = std::string(PHD_FIXTURE_DIR) + "/simimage.fit";

} // namespace

TEST(StarFind, FixtureLoads)
{
    Fits f;
    ASSERT_TRUE(LoadFits(kSimImage, f)) << "could not load " << kSimImage;
    EXPECT_GT(f.width, 0);
    EXPECT_GT(f.height, 0);
    EXPECT_EQ(f.pixels.size(), (size_t) f.width * f.height);
}

TEST(StarFind, DetectsStarAtBrightestPixel)
{
    Fits f;
    ASSERT_TRUE(LoadFits(kSimImage, f));
    int bx, by;
    brightestPixel(f, bx, by);

    star_find::Output o = star_find::FindStar(viewOf(f), bx, by, /*searchRegion*/ 15, star_find::Mode::Centroid, 0.0, 99.0, 0);

    EXPECT_TRUE(o.found);
    EXPECT_EQ(static_cast<int>(o.result), static_cast<int>(star_find::Result::Ok));
    EXPECT_GT(o.mass, 10.0); // above the low-mass floor
    EXPECT_GE(o.snr, 3.0); // above LOW_SNR
    EXPECT_GT(o.hfd, 0.0);
    // Centroid lands within the search window of the seed.
    EXPECT_NEAR(o.x, bx, 15.0);
    EXPECT_NEAR(o.y, by, 15.0);
}

TEST(StarFind, PeakModeReturnsBrightestLocation)
{
    Fits f;
    ASSERT_TRUE(LoadFits(kSimImage, f));
    int bx, by;
    brightestPixel(f, bx, by);

    star_find::Output o = star_find::FindStar(viewOf(f), bx, by, 15, star_find::Mode::Peak, 0.0, 99.0, 0);
    // In peak mode the reported position is the brightest pixel itself.
    EXPECT_NEAR(o.x, bx, 1.0);
    EXPECT_NEAR(o.y, by, 1.0);
    EXPECT_GT(o.peakVal, 0);
}

TEST(StarFind, InvalidSearchRegionIsError)
{
    Fits f;
    ASSERT_TRUE(LoadFits(kSimImage, f));
    // A search region collapsing to nothing at the corner -> error, not a crash.
    star_find::Output o = star_find::FindStar(viewOf(f), 0, 0, 0, star_find::Mode::Centroid, 0.0, 99.0, 0);
    EXPECT_EQ(static_cast<int>(o.result), static_cast<int>(star_find::Result::Error));
    EXPECT_FALSE(o.found);
}

TEST(StarFind, RejectsWhenHfdConstraintTooTight)
{
    Fits f;
    ASSERT_TRUE(LoadFits(kSimImage, f));
    int bx, by;
    brightestPixel(f, bx, by);
    // Impossible HFD window -> a real star is rejected with a HFD result.
    star_find::Output o =
        star_find::FindStar(viewOf(f), bx, by, 15, star_find::Mode::Centroid, /*minHFD*/ 99.0, /*maxHFD*/ 100.0, 0);
    EXPECT_FALSE(o.found);
    EXPECT_EQ(static_cast<int>(o.result), static_cast<int>(star_find::Result::LowHFD));
}

// Golden values pinned from the real detection on simimage.fit. Update both
// sides deliberately if the detection math changes.
TEST(StarFind, GoldenCentroidOnSimImage)
{
    Fits f;
    ASSERT_TRUE(LoadFits(kSimImage, f));
    int bx, by;
    brightestPixel(f, bx, by);

    star_find::Output o = star_find::FindStar(viewOf(f), bx, by, 15, star_find::Mode::Centroid, 0.0, 99.0, 0);
    ASSERT_TRUE(o.found);

    // GOLDEN: the real detection output on simimage.fit (brightest star at
    // pixel (472,76)). Update deliberately if the detection math changes.
    EXPECT_NEAR(o.x, 472.4722, 0.01);
    EXPECT_NEAR(o.y, 75.8462, 0.01);
    EXPECT_NEAR(o.mass, 78154.75, 1.0);
    EXPECT_NEAR(o.snr, 43.52, 0.1);
    EXPECT_NEAR(o.hfd, 0.6123, 0.01);
}
