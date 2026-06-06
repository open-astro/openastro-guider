/*
 *  star_find_core.cpp
 *  PHD Guiding
 *
 *  Pure pixel math for star detection. See star_find_core.h. This is a
 *  verbatim lift of the body of Star::Find(): the same smoothed-peak search,
 *  iterative background-annulus (inner A=7, outer B=12) mean/sigma estimation,
 *  3-sigma threshold, in-aperture centroid + mass, Simonetti SNR, half-flux
 *  radius, and saturation heuristic. wxMax/wxMin become std::max/std::min,
 *  wxPoint becomes a local pair, and Debug logging is dropped (the caller
 *  keeps it). The newX/newY/PeakVal/Mass/SNR/HFD update semantics on each
 *  early-out are preserved exactly.
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

#include "star_find_core.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{

// helper struct for HFR calculation (mirrors the R2M in star.cpp)
struct R2M
{
    double r2 = 0.;
    int px = 0;
    int py = 0;
    double m = 0.;
    R2M() { }
    R2M(int x, int y, double m_) : px(x), py(y), m(m_) { }
    bool operator<(const R2M& rhs) const { return r2 < rhs.r2; }
};

double hfr(std::vector<R2M>& vec, double cx, double cy, double mass)
{
    if (vec.size() == 1) // hot pixel?
        return 0.25;

    // compute Half Flux Radius (HFR)
    for (auto it = vec.begin(); it != vec.end(); ++it)
    {
        double dx = (double) it->px - cx;
        double dy = (double) it->py - cy;
        it->r2 = dx * dx + dy * dy;
    }
    std::sort(vec.begin(), vec.end()); // sort by ascending radius^2

    // find radius of half-mass
    double r20, r21, m0, m1;
    r20 = r21 = m0 = m1 = 0.0;
    double halfm = 0.5 * mass;
    for (auto it = vec.begin(); it != vec.end(); ++it)
    {
        const R2M& rm = *it;
        r20 = r21;
        m0 = m1;
        r21 = rm.r2;
        m1 += rm.m;
        if (m1 > halfm)
            break;
    }

    // interpolate
    double hfr;
    if (m1 > m0)
    {
        double r0 = sqrt(r20), r1 = sqrt(r21);
        double s = (r1 - r0) / (m1 - m0);
        hfr = r0 + s * (halfm - m0);
    }
    else
        hfr = 0.25;

    return hfr;
}

} // namespace

namespace star_find
{

Output FindStar(const StarImage& img, int base_x, int base_y, int searchRegion, Mode mode, double minHFD, double maxHFD,
                unsigned short maxADU)
{
    Output out;
    out.x = base_x;
    out.y = base_y;
    out.result = Result::Ok;

    int minx, miny, maxx, maxy;

    if (img.subframeEmpty)
    {
        minx = miny = 0;
        maxx = img.width - 1;
        maxy = img.height - 1;
    }
    else
    {
        minx = img.subLeft;
        maxx = img.subRight;
        miny = img.subTop;
        maxy = img.subBottom;
    }

    // search region bounds
    int start_x = std::max(base_x - searchRegion, minx);
    int end_x = std::min(base_x + searchRegion, maxx);
    int start_y = std::max(base_y - searchRegion, miny);
    int end_y = std::min(base_y + searchRegion, maxy);

    if (end_x <= start_x || end_y <= start_y)
    {
        out.result = Result::Error;
        out.found = false;
        return out;
    }

    const unsigned short *imgdata = img.data;
    int rowsize = img.width;

    int peak_x = 0, peak_y = 0;
    unsigned int peak_val = 0;
    unsigned short max3[3] = { 0, 0, 0 };

    if (mode == Mode::Peak)
    {
        for (int y = start_y; y <= end_y; y++)
        {
            for (int x = start_x; x <= end_x; x++)
            {
                unsigned short val = imgdata[y * rowsize + x];

                if (val > peak_val)
                {
                    peak_val = val;
                    peak_x = x;
                    peak_y = y;
                }
            }
        }

        out.peakVal = peak_val;
    }
    else
    {
        // find the peak value within the search region using a smoothing function
        // also check for saturation

        for (int y = start_y + 1; y <= end_y - 1; y++)
        {
            for (int x = start_x + 1; x <= end_x - 1; x++)
            {
                unsigned short p = imgdata[y * rowsize + x];
                unsigned int val = 4 * (unsigned int) p + imgdata[(y - 1) * rowsize + (x - 1)] +
                    imgdata[(y - 1) * rowsize + (x + 1)] + imgdata[(y + 1) * rowsize + (x - 1)] +
                    imgdata[(y + 1) * rowsize + (x + 1)] + 2 * imgdata[(y - 1) * rowsize + (x + 0)] +
                    2 * imgdata[(y + 0) * rowsize + (x - 1)] + 2 * imgdata[(y + 0) * rowsize + (x + 1)] +
                    2 * imgdata[(y + 1) * rowsize + (x + 0)];

                if (val > peak_val)
                {
                    peak_val = val;
                    peak_x = x;
                    peak_y = y;
                }

                if (p > max3[0])
                    std::swap(p, max3[0]);
                if (p > max3[1])
                    std::swap(p, max3[1]);
                if (p > max3[2])
                    std::swap(p, max3[2]);
            }
        }

        out.peakVal = max3[0]; // raw peak val
        peak_val /= 16; // smoothed peak value
    }

    // measure noise in the annulus with inner radius A and outer radius B
    int const A = 7; // inner radius
    int const B = 12; // outer radius
    int const A2 = A * A;
    int const B2 = B * B;

    // center window around peak value
    start_x = std::max(peak_x - B, minx);
    end_x = std::min(peak_x + B, maxx);
    start_y = std::max(peak_y - B, miny);
    end_y = std::min(peak_y + B, maxy);

    // find the mean and stdev of the background

    unsigned int nbg = 0;
    double mean_bg = 0., prev_mean_bg;
    double sigma2_bg = 0.;
    double sigma_bg = 0.;

    for (int iter = 0; iter < 9; iter++)
    {
        double sum = 0.0;
        double a = 0.0;
        double q = 0.0;
        nbg = 0;

        const unsigned short *row = imgdata + rowsize * start_y;
        for (int y = start_y; y <= end_y; y++, row += rowsize)
        {
            int dy = y - peak_y;
            int dy2 = dy * dy;
            for (int x = start_x; x <= end_x; x++)
            {
                int dx = x - peak_x;
                int r2 = dx * dx + dy2;

                // exclude points not in annulus
                if (r2 <= A2 || r2 > B2)
                    continue;

                double const val = (double) row[x];

                if (iter > 0 && (val < mean_bg - 2.0 * sigma_bg || val > mean_bg + 2.0 * sigma_bg))
                    continue;

                sum += val;
                ++nbg;
                double const k = (double) nbg;
                double const a0 = a;
                a += (val - a) / k;
                q += (val - a0) * (val - a);
            }
        }

        if (nbg < 10) // only possible after the first iteration
            break;

        prev_mean_bg = mean_bg;
        mean_bg = sum / (double) nbg;
        sigma2_bg = q / (double) (nbg - 1);
        sigma_bg = sqrt(sigma2_bg);

        if (iter > 0 && fabs(mean_bg - prev_mean_bg) < 0.5)
            break;
    }

    unsigned short thresh;

    double cx = 0.0;
    double cy = 0.0;
    double mass = 0.0;
    unsigned int n;

    std::vector<R2M> hfrvec;

    if (mode == Mode::Peak)
    {
        mass = peak_val;
        n = 1;
        thresh = 0;
    }
    else
    {
        thresh = (unsigned short) (mean_bg + 3.0 * sigma_bg + 0.5);

        // find pixels over threshold within aperture; compute mass and centroid

        start_x = std::max(peak_x - A, minx);
        end_x = std::min(peak_x + A, maxx);
        start_y = std::max(peak_y - A, miny);
        end_y = std::min(peak_y + A, maxy);

        n = 0;

        const unsigned short *row = imgdata + rowsize * start_y;
        for (int y = start_y; y <= end_y; y++, row += rowsize)
        {
            int dy = y - peak_y;
            int dy2 = dy * dy;
            if (dy2 > A2)
                continue;

            for (int x = start_x; x <= end_x; x++)
            {
                int dx = x - peak_x;

                // exclude points outside aperture
                if (dx * dx + dy2 > A2)
                    continue;

                // exclude points below threshold
                unsigned short val = row[x];
                if (val < thresh)
                    continue;

                double const d = (double) val - mean_bg;

                cx += dx * d;
                cy += dy * d;
                mass += d;
                ++n;

                hfrvec.push_back(R2M(x, y, d));
            }
        }
    }

    out.mass = mass;

    // SNR estimate (Simonetti 2004)
    double const gain = .5; // electrons per ADU, nominal
    out.snr = n > 0 ? mass / sqrt(mass / gain + sigma2_bg * (double) n * (1.0 + 1.0 / (double) nbg)) : 0.0;

    double const LOW_SNR = 3.0;

    // a few scattered pixels over threshold can give a false positive: require
    // the smoothed peak value to be above the threshold
    if (peak_val <= thresh && out.snr >= LOW_SNR)
        out.snr = LOW_SNR - 0.1;

    if (mass < 10.0)
    {
        out.hfd = 0.;
        out.result = Result::LowMass;
        out.found = false;
        return out;
    }

    if (out.snr < LOW_SNR)
    {
        out.hfd = 0.;
        out.result = Result::LowSNR;
        out.found = false;
        return out;
    }

    out.x = peak_x + cx / mass;
    out.y = peak_y + cy / mass;

    out.hfd = 2.0 * hfr(hfrvec, out.x, out.y, mass);
    // Check for constraints on HFD value
    if (mode != Mode::Peak)
    {
        if (out.hfd < minHFD)
        {
            out.result = Result::LowHFD;
            out.found = false;
            return out;
        }
        if (out.hfd > maxHFD)
        {
            out.result = Result::HiHFD;
            out.found = false;
            return out;
        }
    }

    // check for saturation
    unsigned int mx = (unsigned int) max3[0];

    // remove pedestal
    if (mx >= img.pedestal)
        mx -= img.pedestal;
    else
        mx = 0; // unlikely

    if (maxADU > 0)
    {
        // maxADU is known
        if (mx >= maxADU)
            out.result = Result::Saturated;
        out.found = (out.result == Result::Ok || out.result == Result::Saturated);
        return out;
    }

    // maxADU not known, use the "flat-top" heuristic
    unsigned int d = (unsigned int) (max3[0] - max3[2]);

    if (img.bitsPerPixel < 12)
    {
        if (d * 191U < 1U * mx)
            out.result = Result::Saturated;
    }
    else
    {
        if (d * 65535U < 32U * mx)
            out.result = Result::Saturated;
    }

    out.found = (out.result == Result::Ok || out.result == Result::Saturated);
    return out;
}

} // namespace star_find
