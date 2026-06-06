/*
 *  guide_algorithm_lowpass_math.h
 *  PHD Guiding
 *
 *  Pure decision math for the Lowpass and Lowpass2 guide algorithms, factored
 *  out of GuideAlgorithmLowpass / GuideAlgorithmLowpass2 so it can be unit
 *  tested. Those translation units each also define a wxWidgets
 *  ConfigDialogPane, so the algorithm .cpp cannot be linked into a test
 *  without the full GUI; this header holds only the math, over the (already
 *  wx-free) WindowedAxisStats and plain numbers.
 *
 *  The Compute() functions mutate the supplied filter state exactly as the
 *  production result() does and return the result plus the few "log events"
 *  the production code emits, so the wxFrame-side wrappers keep their
 *  Debug.Write traces verbatim.
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

#ifndef GUIDE_ALGORITHM_LOWPASS_MATH_H_INCLUDED
#define GUIDE_ALGORITHM_LOWPASS_MATH_H_INCLUDED

#include "guiding_stats.h"

namespace guide_lowpass
{

// Mirrors GuideAlgorithmLowpass::reset(): clear, then zero-fill HISTORY_SIZE
// points so the median/linear-fit have a full window from the first result().
void Reset(WindowedAxisStats& stats, int& timeBase, int historySize);

struct Result
{
    double value = 0.; // the value to return as the guide correction
    double computed = 0.; // median + slopeWeight*slope, before the input clamp
    bool clampedToInput = false; // |computed| > |input| -> returned input instead
};

// Mirrors GuideAlgorithmLowpass::result(). Mutates stats/timeBase.
Result Compute(WindowedAxisStats& stats, int& timeBase, double slopeWeight, double minMove, double input);

} // namespace guide_lowpass

namespace guide_lowpass2
{

// Mirrors GuideAlgorithmLowpass2::reset(): clear, reset counters. (No
// zero-fill — Lowpass2's stats are auto-windowed.)
void Reset(WindowedAxisStats& stats, int& timeBase, int& rejects);

struct Result
{
    double value = 0.;
    double computed = 0.; // value before the input clamp
    double slope = 0.; // linear-fit slope (0 until >= 4 points)
    bool clampedToInput = false; // |computed| > |input|
    bool outlierDumped = false; // |input| > 4*minMove cleared the history
    bool threeRejectReset = false; // 3 successive rejects cleared the history
};

// Mirrors GuideAlgorithmLowpass2::result(). Mutates stats/timeBase/rejects.
Result Compute(WindowedAxisStats& stats, int& timeBase, int& rejects, double aggressiveness, double minMove, double input);

} // namespace guide_lowpass2

#endif // GUIDE_ALGORITHM_LOWPASS_MATH_H_INCLUDED
