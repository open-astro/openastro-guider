/*
 *  jpeg_encode.h
 *  PHD Guiding
 *
 *  The JPEG encoder behind GET /api/frame.jpg (#115, slice S7.1 of steps 4-9),
 *  on libjpeg directly instead of wxImage and wxJPEGHandler. It makes the
 *  same JPEG wxJPEGHandler::SaveFile (wx 3.2) made from the stretched frame:
 *  RGB input, 3 components, jpeg_set_defaults (baseline, standard Huffman
 *  tables, not progressive, 2x2 chroma subsampling, JFIF with no density),
 *  then jpeg_set_quality(quality, force_baseline TRUE). With the same libjpeg
 *  the bytes are the same.
 *  Kept free of wx so it can be unit tested (tests/image/test_jpeg_encode.cpp).
 */

#ifndef JPEG_ENCODE_H_INCLUDED
#define JPEG_ENCODE_H_INCLUDED

#include <string>

namespace jpeg_encode
{

// The quality /api/frame.jpg has always used (wxIMAGE_OPTION_QUALITY 85)
constexpr int kFrameQuality = 85;

// Encodes width x height 8-bit gray pixels (row-major, no padding) into *out,
// which it replaces, as a 3-component JPEG of R = G = B = the gray value.
// Returns false, with libjpeg's message in *error if given, on any libjpeg
// error (an empty image, say); *out is then empty.
bool EncodeGray8(const unsigned char *gray, int width, int height, int quality, std::string *out, std::string *error = nullptr);

} // namespace jpeg_encode

#endif // JPEG_ENCODE_H_INCLUDED
