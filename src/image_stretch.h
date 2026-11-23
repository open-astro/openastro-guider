/*
 *  image_stretch.h
 *  PHD Guiding
 *
 *  The display stretch of a 16-bit guide frame to 8-bit gray (#115, slice S7.1
 *  of steps 4-9): black level, white level and gamma through a 64K-entry
 *  lookup table, the same table and arithmetic as usImage::CopyToImage, so the
 *  output is byte-identical to that wxImage's red, green and blue channels.
 *  GET /api/frame.jpg stretches with the frame's FiltMin and FiltMax (from
 *  usImage::CalcStats) and MyFrame::Stretch_gamma.
 *  Kept free of wx so it can be unit tested (tests/image/test_jpeg_encode.cpp).
 */

#ifndef IMAGE_STRETCH_H_INCLUDED
#define IMAGE_STRETCH_H_INCLUDED

#include <cstddef>

namespace image_stretch
{

// Fills lut[0..0xffff]: 0 up to and including blevel, 255 from wlevel up, and
// pow((v - blevel) / (wlevel - blevel), power) * 255 truncated in between.
// Levels are clamped to 0..0xffff first.
void BuildGammaLut(int blevel, int wlevel, double power, unsigned char *lut);

// Stretches count pixels of src into count bytes of dst through BuildGammaLut.
void StretchGray8(const unsigned short *src, size_t count, int blevel, int wlevel, double power, unsigned char *dst);

} // namespace image_stretch

#endif // IMAGE_STRETCH_H_INCLUDED
