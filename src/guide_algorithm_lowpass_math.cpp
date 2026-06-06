/*
 *  guide_algorithm_lowpass_math.cpp
 *  PHD Guiding
 *
 *  Pure decision math for the Lowpass / Lowpass2 guide algorithms. See
 *  guide_algorithm_lowpass_math.h. Each function is a verbatim lift of the
 *  math previously inline in the algorithm result()/reset() methods, with the
 *  logic preserved exactly; only the Debug.Write() calls are hoisted back to
 *  the caller via the returned log-event flags.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of openphdguiding.org nor the names of its
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

#include "guide_algorithm_lowpass_math.h"

#include <cmath>

namespace guide_lowpass
{

void Reset(WindowedAxisStats& stats, int& timeBase, int historySize)
{
    stats.ClearAll();
    timeBase = 0;

    // Needs to be zero-filled to start
    while ((int) stats.GetCount() < historySize)
    {
        stats.AddGuideInfo(timeBase++, 0, 0);
    }
}

Result Compute(WindowedAxisStats& stats, int& timeBase, double slopeWeight, double minMove, double input)
{
    // Manual trimming of window (instead of auto-size) is done for full backward compatibility with original algo
    stats.AddGuideInfo(timeBase++, input, 0);
    double median = stats.GetMedian();
    stats.RemoveOldestEntry();
    double slope;
    double intcpt;
    stats.GetLinearFitResults(&slope, &intcpt);
    double dReturn = median + slopeWeight * slope;

    Result r;
    r.computed = dReturn;

    if (std::fabs(dReturn) > std::fabs(input))
    {
        r.clampedToInput = true;
        dReturn = input;
    }

    if (std::fabs(input) < minMove)
    {
        dReturn = 0.0;
    }

    r.value = dReturn;
    return r;
}

} // namespace guide_lowpass

namespace guide_lowpass2
{

void Reset(WindowedAxisStats& stats, int& timeBase, int& rejects)
{
    stats.ClearAll();
    timeBase = 0;
    rejects = 0;
}

Result Compute(WindowedAxisStats& stats, int& timeBase, int& rejects, double aggressiveness, double minMove, double input)
{
    stats.AddGuideInfo(timeBase++, input, 0); // AxisStats instance is auto-windowed
    unsigned int numpts = stats.GetCount();
    double dReturn;
    double attenuation = aggressiveness / 100.;
    double newSlope = 0;

    Result r;

    if (numpts < 4)
        dReturn = input * attenuation; // Don't fall behind while we're figuring things out
    else
    {
        if (std::fabs(input) > 4.0 * minMove) // Outlier deflection - dump the history
        {
            dReturn = input * attenuation;
            Reset(stats, timeBase, rejects);
            numpts = 0;
            r.outlierDumped = true;
        }
        else
        {
            double intcpt;
            stats.GetLinearFitResults(&newSlope, &intcpt);
            dReturn = newSlope * (double) numpts * attenuation;
            // Don't return a result that will push the star further in the wrong direction
            if (input * dReturn < 0)
                dReturn = 0;
        }
    }

    r.computed = dReturn;

    if (std::fabs(dReturn) > std::fabs(input)) // Keep guide pulses below magnitude of last deflection
    {
        r.clampedToInput = true;
        dReturn = input * attenuation;
        rejects++;
        if (rejects > 3) // 3-in-a-row, our slope is not useful
        {
            Reset(stats, timeBase, rejects);
            r.threeRejectReset = true;
        }
    }
    else
        rejects = 0;

    if (std::fabs(input) < minMove)
        dReturn = 0.0;

    r.value = dReturn;
    r.slope = newSlope;
    return r;
}

} // namespace guide_lowpass2
