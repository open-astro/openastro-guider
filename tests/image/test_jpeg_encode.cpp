// The wx-free frame.jpg path (#115, slice S7.1 of steps 4-9): image_stretch::StretchGray8 against
// a frozen twin of usImage::CopyToImage (byte for byte), and jpeg_encode::EncodeGray8 decoded
// back with libjpeg: the size, 3 components and the baseline 4:2:0 quality-85 layout
// wxJPEGHandler wrote, and the pixels close to the stretched frame. No golden JPEG: the bytes
// may differ between libjpeg versions.
//
// Quality 85 is lossy: a star's edge holds frequencies its quantization tables step by 10 to 30,
// so a few pixels around each star decode up to about 8 levels off, noise-free or not, and the
// YCbCr round trip alone leaves +-3 here and there on smooth content. So the star frame is held
// to a worst case, a share within 2 levels and a mean, and a smooth frame to 3 levels everywhere.

#include "image_stretch.h"
#include "jpeg_encode.h"
#include "legacy/copy_to_image_legacy.h"
#include "../star/synthetic_star.h"

#include <gtest/gtest.h>

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <jpeglib.h>

namespace
{

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// A 640 x 480 guide frame like the fake camera's: 12 stars, two of them clipped, on a sloped
// background with read noise
std::vector<unsigned short> FixtureFrame()
{
    synth::Frame f(kWidth, kHeight, 1200.0);
    synth::AddGradientX(f, 0.5);
    const double stars[][4] = {
        { 50.3, 40.7, 1.6, 3000 },    { 600.1, 30.2, 2.0, 12000 }, { 320.5, 240.5, 1.8, 40000 }, { 100.0, 400.4, 2.5, 8000 },
        { 500.7, 420.9, 1.4, 70000 }, { 210.2, 120.6, 2.2, 5000 }, { 420.4, 150.1, 1.7, 2500 },  { 30.8, 250.3, 3.0, 20000 },
        { 610.6, 260.8, 1.5, 15000 }, { 260.0, 460.0, 2.1, 6000 }, { 380.9, 330.3, 1.9, 90000 }, { 150.5, 300.5, 2.4, 10000 },
    };
    for (const auto& s : stars)
        synth::AddStar(f, s[0], s[1], s[2], synth::FluxForPeak(s[3], s[2]));
    synth::AddNoise(f, 25.0, 20261123);
    return synth::Quantize(f, 65535);
}

void MinMax(const std::vector<unsigned short>& px, int *lo, int *hi)
{
    *lo = 65535;
    *hi = 0;
    for (unsigned short v : px)
    {
        *lo = std::min(*lo, (int) v);
        *hi = std::max(*hi, (int) v);
    }
}

// The stretch's output must be the wxImage's channels, each byte equal
void ExpectMatchesLegacy(const std::vector<unsigned short>& px, int blevel, int wlevel, double power)
{
    std::vector<unsigned char> gray(px.size());
    image_stretch::StretchGray8(px.data(), px.size(), blevel, wlevel, power, gray.data());
    std::vector<unsigned char> rgb = legacy::CopyToImage(px.data(), (unsigned int) px.size(), blevel, wlevel, power);

    size_t mismatches = 0;
    for (size_t i = 0; i < px.size(); i++)
    {
        if (gray[i] != rgb[3 * i] || gray[i] != rgb[3 * i + 1] || gray[i] != rgb[3 * i + 2])
        {
            if (++mismatches <= 3)
                ADD_FAILURE() << "pixel " << i << " value " << px[i] << ": " << (int) gray[i] << " vs " << (int) rgb[3 * i]
                              << " (levels " << blevel << ".." << wlevel << ", gamma " << power << ")";
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

struct DecodeError
{
    jpeg_error_mgr pub;
    std::jmp_buf jump;
};

void DecodeErrorExit(j_common_ptr cinfo)
{
    std::longjmp(reinterpret_cast<DecodeError *>(cinfo->err)->jump, 1);
}

struct Decoded
{
    bool ok = false;
    int width = 0;
    int height = 0;
    int components = 0;
    bool progressive = false;
    J_COLOR_SPACE colorSpace = JCS_UNKNOWN;
    int sampling[3][2] = {};
    unsigned short quant[2][DCTSIZE2] = {};
    std::vector<unsigned char> rgb;
};

Decoded Decode(const std::string& jpeg)
{
    Decoded d;
    jpeg_decompress_struct cinfo;
    DecodeError jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = DecodeErrorExit;
    if (setjmp(jerr.jump))
    {
        jpeg_destroy_decompress(&cinfo);
        d.ok = false;
        return d;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char *>(jpeg.data()), jpeg.size());
    jpeg_read_header(&cinfo, TRUE);

    d.width = cinfo.image_width;
    d.height = cinfo.image_height;
    d.components = cinfo.num_components;
    d.progressive = cinfo.progressive_mode;
    d.colorSpace = cinfo.jpeg_color_space;
    for (int c = 0; c < 3 && c < cinfo.num_components; c++)
    {
        d.sampling[c][0] = cinfo.comp_info[c].h_samp_factor;
        d.sampling[c][1] = cinfo.comp_info[c].v_samp_factor;
    }
    for (int t = 0; t < 2; t++)
        if (cinfo.quant_tbl_ptrs[t])
            for (int k = 0; k < DCTSIZE2; k++)
                d.quant[t][k] = cinfo.quant_tbl_ptrs[t]->quantval[k];

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    d.rgb.resize((size_t) cinfo.output_width * cinfo.output_height * 3);
    while (cinfo.output_scanline < cinfo.output_height)
    {
        JSAMPROW row = &d.rgb[(size_t) cinfo.output_scanline * cinfo.output_width * 3];
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    d.ok = true;
    return d;
}

// The quantization tables jpeg_set_defaults + jpeg_set_quality(quality, TRUE) make
void ExpectedQuant(int quality, unsigned short out[2][DCTSIZE2])
{
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    cinfo.in_color_space = JCS_RGB;
    cinfo.input_components = 3;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < DCTSIZE2; k++)
            out[t][k] = cinfo.quant_tbl_ptrs[t]->quantval[k];
    jpeg_destroy_compress(&cinfo);
}

struct ErrorStats
{
    int worst = 0;
    size_t over2 = 0;
    double mean = 0.0;
};

// Every decoded channel against the gray value it was encoded from
ErrorStats Compare(const std::vector<unsigned char>& gray, const std::vector<unsigned char>& rgb)
{
    ErrorStats e;
    double sum = 0.0;
    for (size_t i = 0; i < gray.size(); i++)
        for (int c = 0; c < 3; c++)
        {
            int diff = std::abs((int) rgb[3 * i + c] - (int) gray[i]);
            e.worst = std::max(e.worst, diff);
            if (diff > 2)
                ++e.over2;
            sum += diff;
        }
    e.mean = sum / (gray.size() * 3);
    return e;
}

std::vector<unsigned char> StretchedFixture()
{
    std::vector<unsigned short> px = FixtureFrame();
    int lo, hi;
    MinMax(px, &lo, &hi);
    std::vector<unsigned char> gray(px.size());
    image_stretch::StretchGray8(px.data(), px.size(), lo, hi, 0.5, gray.data());
    return gray;
}

} // namespace

TEST(StretchGray8, MatchesCopyToImageOnEveryValue)
{
    std::vector<unsigned short> every(0x10000);
    for (size_t i = 0; i < every.size(); i++)
        every[i] = (unsigned short) i;

    const int levels[][2] = {
        { 0, 65535 },   { 1000, 5000 },   { 1180, 65535 }, { 0, 0 },      { 500, 500 }, { 5000, 1000 },
        { -10, 70000 }, { 65535, 65535 }, { 123, 124 },    { 70000, -5 }, { 0, 1 },     { 65534, 65535 },
    };
    const double gammas[] = { 0.1, 0.37, 0.5, 0.75, 1.0, 1.3, 2.0, 3.0 };
    for (const auto& l : levels)
        for (double g : gammas)
            ExpectMatchesLegacy(every, l[0], l[1], g);
}

TEST(StretchGray8, MatchesCopyToImageOnFixtureFrame)
{
    std::vector<unsigned short> px = FixtureFrame();
    int lo, hi;
    MinMax(px, &lo, &hi);
    for (double g : { 0.4, 1.0, 1.6 })
        ExpectMatchesLegacy(px, lo, hi, g);
    ExpectMatchesLegacy(px, lo + 200, lo + 3000, 1.0);
}

TEST(JpegEncode, DecodesToTheStretchedFrame)
{
    std::vector<unsigned char> gray = StretchedFixture();

    std::string jpeg;
    std::string error;
    ASSERT_TRUE(jpeg_encode::EncodeGray8(gray.data(), kWidth, kHeight, jpeg_encode::kFrameQuality, &jpeg, &error)) << error;
    ASSERT_GT(jpeg.size(), 4u);
    EXPECT_EQ((unsigned char) jpeg[0], 0xFF);
    EXPECT_EQ((unsigned char) jpeg[1], 0xD8);
    EXPECT_EQ((unsigned char) jpeg[jpeg.size() - 2], 0xFF);
    EXPECT_EQ((unsigned char) jpeg[jpeg.size() - 1], 0xD9);
    EXPECT_NE(jpeg.find("\xFF\xC0", 0, 2), std::string::npos) << "no baseline SOF0";
    EXPECT_EQ(jpeg.find("\xFF\xC2", 0, 2), std::string::npos) << "progressive SOF2";

    Decoded d = Decode(jpeg);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, kWidth);
    EXPECT_EQ(d.height, kHeight);
    EXPECT_EQ(d.components, 3);
    EXPECT_EQ(d.colorSpace, JCS_YCbCr);
    EXPECT_FALSE(d.progressive);

    // jpeg_set_defaults' 4:2:0, which wxJPEGHandler kept
    EXPECT_EQ(d.sampling[0][0], 2);
    EXPECT_EQ(d.sampling[0][1], 2);
    for (int c = 1; c < 3; c++)
    {
        EXPECT_EQ(d.sampling[c][0], 1);
        EXPECT_EQ(d.sampling[c][1], 1);
    }

    // quality 85's tables, as wx's wxIMAGE_OPTION_QUALITY 85 gave
    unsigned short want[2][DCTSIZE2];
    ExpectedQuant(85, want);
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < DCTSIZE2; k++)
            EXPECT_EQ(d.quant[t][k], want[t][k]) << "table " << t << " entry " << k;

    ErrorStats e = Compare(gray, d.rgb);
    EXPECT_LE(e.worst, 10);
    EXPECT_LE(e.over2, gray.size() * 3 / 50) << "more than 2 % of the values over 2 levels off";
    EXPECT_LE(e.mean, 1.0);
}

// Broad blobs on a gradient, no noise: nothing for quality 85 to throw away
TEST(JpegEncode, SmoothFrameWithinThreeLevels)
{
    synth::Frame f(kWidth, kHeight, 1200.0);
    synth::AddGradientX(f, 2.0);
    synth::AddStar(f, 200.5, 150.5, 25.0, synth::FluxForPeak(30000, 25.0));
    synth::AddStar(f, 450.5, 330.5, 25.0, synth::FluxForPeak(50000, 25.0));
    std::vector<unsigned short> px = synth::Quantize(f, 65535);
    int lo, hi;
    MinMax(px, &lo, &hi);
    std::vector<unsigned char> gray(px.size());
    image_stretch::StretchGray8(px.data(), px.size(), lo, hi, 1.0, gray.data());

    std::string jpeg;
    ASSERT_TRUE(jpeg_encode::EncodeGray8(gray.data(), kWidth, kHeight, jpeg_encode::kFrameQuality, &jpeg));
    Decoded d = Decode(jpeg);
    ASSERT_TRUE(d.ok);
    ASSERT_EQ(d.width, kWidth);
    ASSERT_EQ(d.height, kHeight);
    EXPECT_LE(Compare(gray, d.rgb).worst, 3);
}

TEST(JpegEncode, SameInputSameBytes)
{
    std::vector<unsigned char> gray = StretchedFixture();
    std::string a, b;
    ASSERT_TRUE(jpeg_encode::EncodeGray8(gray.data(), kWidth, kHeight, jpeg_encode::kFrameQuality, &a));
    ASSERT_TRUE(jpeg_encode::EncodeGray8(gray.data(), kWidth, kHeight, jpeg_encode::kFrameQuality, &b));
    EXPECT_EQ(a, b);
}

// Noise compresses badly: well over the destination's 64 KiB block, so it is emptied mid-encode
TEST(JpegEncode, OutputLargerThanOneBlock)
{
    const int w = 1024, h = 768;
    std::vector<unsigned char> gray((size_t) w * h);
    unsigned int x = 12345;
    for (auto& g : gray)
    {
        x = x * 1103515245u + 12345u;
        g = (unsigned char) (x >> 24);
    }
    std::string jpeg;
    ASSERT_TRUE(jpeg_encode::EncodeGray8(gray.data(), w, h, jpeg_encode::kFrameQuality, &jpeg));
    EXPECT_GT(jpeg.size(), 3u * 65536u);
    Decoded d = Decode(jpeg);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.width, w);
    EXPECT_EQ(d.height, h);
}

TEST(JpegEncode, EmptyImageFailsWithoutExiting)
{
    unsigned char px = 0;
    std::string jpeg = "stale";
    std::string error;
    EXPECT_FALSE(jpeg_encode::EncodeGray8(&px, 0, 0, jpeg_encode::kFrameQuality, &jpeg, &error));
    EXPECT_TRUE(jpeg.empty());
    EXPECT_FALSE(error.empty());

    // and the encoder still works afterwards
    std::vector<unsigned char> gray = StretchedFixture();
    EXPECT_TRUE(jpeg_encode::EncodeGray8(gray.data(), kWidth, kHeight, jpeg_encode::kFrameQuality, &jpeg));
}
