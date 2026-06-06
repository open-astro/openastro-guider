/*
 *  star_find_core.h
 *  PHD Guiding
 *
 *  Pure pixel math for star detection, factored out of Star::Find so it can be
 *  unit tested without wxWidgets, a usImage, or the rest of the app. Operates
 *  on a raw 16-bit pixel buffer and plain numbers: background annulus
 *  estimation, thresholding, centroid, mass, SNR, HFD, and the saturation
 *  heuristic. Star::Find builds a StarImage view over its usImage, calls
 *  FindStar(), and keeps the PHD_Point/member updates and Debug logging.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef STAR_FIND_CORE_H_INCLUDED
#define STAR_FIND_CORE_H_INCLUDED

namespace star_find
{

enum class Mode
{
    Centroid, // mirrors Star::FIND_CENTROID
    Peak, // mirrors Star::FIND_PEAK
};

// Mirrors Star::FindResult (subset reachable from the pixel math; the higher-
// level STAR_TOO_NEAR_EDGE / STAR_MASSCHANGE are decided by callers).
enum class Result
{
    Ok,
    Saturated,
    LowSNR,
    LowMass,
    LowHFD,
    HiHFD,
    Error,
};

// A read-only view over a 16-bit image. subframe is [left,top]..[right,bottom]
// inclusive; set subframeEmpty for the whole image.
struct StarImage
{
    const unsigned short *data = nullptr;
    int width = 0;
    int height = 0;
    bool subframeEmpty = true;
    int subLeft = 0;
    int subTop = 0;
    int subRight = 0;
    int subBottom = 0;
    unsigned short pedestal = 0;
    int bitsPerPixel = 16;
};

struct Output
{
    double x = 0.; // refined centroid (or base position on early-out)
    double y = 0.;
    double mass = 0.;
    double snr = 0.;
    double hfd = 0.;
    unsigned short peakVal = 0; // raw peak value
    Result result = Result::Error;
    bool found = false; // result == Ok || Saturated
};

// Locate/measure a star near (baseX, baseY). Verbatim lift of the pixel math
// in Star::Find(); no logging, no exceptions for control flow (invalid search
// bounds return Result::Error). minHFD/maxHFD bound the accepted HFD in
// Centroid mode; maxADU (>0) is the known saturation ADU.
Output FindStar(const StarImage& img, int baseX, int baseY, int searchRegion, Mode mode, double minHFD, double maxHFD,
                unsigned short maxADU);

} // namespace star_find

#endif // STAR_FIND_CORE_H_INCLUDED
