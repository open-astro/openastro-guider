/*
 *  image_stretch.cpp
 *  PHD Guiding
 *
 *  See image_stretch.h. BuildGammaLut is a verbatim lift of usImage.cpp's
 *  buildGammaLookupTable: the same float arithmetic in the same order, so the
 *  table is the same byte for byte.
 */

#include "image_stretch.h"

#include <cmath>
#include <vector>

namespace image_stretch
{

void BuildGammaLut(int blevel, int wlevel, double power, unsigned char *lut)
{
    if (blevel < 0)
        blevel = 0;
    if (wlevel < 0)
        wlevel = 0;
    if (blevel > 0xffff)
        blevel = 0xffff;
    if (wlevel > 0xffff)
        wlevel = 0xffff;

    for (int i = 0; i <= blevel; ++i)
        lut[i] = 0;

    float range = wlevel - blevel;
    for (int i = blevel + 1; i < wlevel; ++i)
    {
        float d = (i - blevel) / range;
        lut[i] = std::pow(d, (float) power) * 255.0;
    }

    for (int i = wlevel; i < 0x10000; ++i)
        lut[i] = 255;
}

void StretchGray8(const unsigned short *src, size_t count, int blevel, int wlevel, double power, unsigned char *dst)
{
    std::vector<unsigned char> lut(0x10000);
    BuildGammaLut(blevel, wlevel, power, lut.data());

    for (size_t i = 0; i < count; i++)
        dst[i] = lut[src[i]];
}

} // namespace image_stretch
