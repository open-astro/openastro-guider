// Frozen twin of the stretch GET /api/frame.jpg used through #115 step 6: usImage.cpp's
// buildGammaLookupTable and usImage::CopyToImage at d949b831 (slice S6.9), with the wxImage
// replaced by a plain RGB buffer of the same layout (R, G, B per pixel, rows without padding).
// Slice S7.3 deletes the originals; this copy stays as the reference that
// image_stretch::StretchGray8 must match byte for byte. Do not edit it to follow the source.

#ifndef TESTS_IMAGE_LEGACY_COPY_TO_IMAGE_LEGACY_H
#define TESTS_IMAGE_LEGACY_COPY_TO_IMAGE_LEGACY_H

#include <math.h>
#include <vector>

namespace legacy
{

static unsigned char *buildGammaLookupTable(int blevel, int wlevel, double power)
{
    unsigned char *result = new unsigned char[0x10000];

    if (blevel < 0)
        blevel = 0;
    if (wlevel < 0)
        wlevel = 0;
    if (blevel > 0xffff)
        blevel = 0xffff;
    if (wlevel > 0xffff)
        wlevel = 0xffff;

    for (int i = 0; i <= blevel; ++i)
        result[i] = 0;

    float range = wlevel - blevel;
    for (int i = blevel + 1; i < wlevel; ++i)
    {
        float d = (i - blevel) / range;
        result[i] = pow(d, (float) power) * 255.0;
    }

    for (int i = wlevel; i < 0x10000; ++i)
        result[i] = 255;

    return result;
}

// usImage::CopyToImage into a width x height x 3 RGB buffer
inline std::vector<unsigned char> CopyToImage(const unsigned short *ImageData, unsigned int NPixels, int blevel, int wlevel,
                                              double power)
{
    std::vector<unsigned char> img(NPixels * 3);

    unsigned char *ImgPtr = img.data();
    const unsigned short *RawPtr = ImageData;

    unsigned char *lutTable = buildGammaLookupTable(blevel, wlevel, power);

    for (unsigned int i = 0; i < NPixels; i++, RawPtr++)
    {
        unsigned short v = *RawPtr;
        unsigned char d = lutTable[v];
        *ImgPtr++ = d;
        *ImgPtr++ = d;
        *ImgPtr++ = d;
    }

    delete[] lutTable;

    return img;
}

} // namespace legacy

#endif // TESTS_IMAGE_LEGACY_COPY_TO_IMAGE_LEGACY_H
