/*
 *  guide_algorithm_kernels.h
 *  PHD Guiding
 *
 *  The per-frame math of the Hysteresis and ResistSwitch guide algorithms (#142), the RA and
 *  Dec defaults for a mount. GuideAlgorithmHysteresis::result() and
 *  GuideAlgorithmResistSwitch::result() delegate to these with the same arithmetic in the same
 *  order; the classes keep the profile, the setters' checks, the dialogs and the Debug lines,
 *  which they write from what a step returns.
 *  Kept free of wx so it can be unit tested (tests/algorithms).
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Bret McKee, Dad Dog Development,
 *     Craig Stark, Stark Labs nor the names of its
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

#ifndef GUIDE_ALGORITHM_KERNELS_H_INCLUDED
#define GUIDE_ALGORITHM_KERNELS_H_INCLUDED

#include <array>

namespace guide_algorithm_kernels
{

// GuideAlgorithmHysteresis's m_minMove, m_hysteresis and m_aggression; the defaults are the
// constructor's with nothing in the profile
struct HysteresisParams
{
    double minMove = 0.2;
    double hysteresis = 0.1; // 0..0.99
    double aggression = 0.7; // 0..2
};

// GuideAlgorithmHysteresis's m_lastMove; reset() and SetAggression() zero it
struct HysteresisState
{
    double lastMove = 0.;
};

// GuideAlgorithmHysteresis::result(): blends the input with the last move, scales it by the
// aggression, and returns 0 for an input under minMove. The return is the new last move.
double HysteresisResult(HysteresisState& state, const HysteresisParams& params, double input);

constexpr unsigned int RESIST_SWITCH_HISTORY_SIZE = 10;

// GuideAlgorithmResistSwitch's m_minMove, m_aggression and m_fastSwitchEnabled; the defaults
// are the constructor's with nothing in the profile
struct ResistSwitchParams
{
    double minMove = 0.2; // > 0
    double aggression = 1.0; // 0..1
    bool fastSwitchEnabled = true;
};

// GuideAlgorithmResistSwitch's m_history (oldest first) and m_currentSide (-1, 0 or 1).
// ResistSwitchReset() is reset(); SetMinMove() zeroes currentSide when it accepts the value.
struct ResistSwitchState
{
    std::array<double, RESIST_SWITCH_HISTORY_SIZE> history {};
    int currentSide = 0;
};

void ResistSwitchReset(ResistSwitchState& state);

// guide_algorithm_resistswitch.cpp's sign(): 1, -1, or 0 for 0 and NaN
int ResistSwitchSign(double x);

// Why a ResistSwitch step returned 0; each is a THROW_INFO line in the Debug log
enum class ResistSwitchVeto
{
    None,
    BelowMinMove, // "input < m_minMove"
    NotCompelling, // "not compelling enough"
    NotGettingWorse, // "Not getting worse"
    Overshoot, // "must have overshot -- vetoing move"
};

// One ResistSwitch step and what it logs, in this order: the fast switch, the switch, the veto
struct ResistSwitchStep
{
    double result = 0.;

    // a large excursion forced a switch: "resist switch: large excursion: ..."
    bool fastSwitched = false;
    double thresh = 0.;
    int fastSwitchFrom = 0;
    int fastSwitchTo = 0;

    // the history changed the side: "switching direction from ..."
    bool switched = false;
    int switchFrom = 0;
    int switchTo = 0;
    int decHistory = 0;
    double oldest = 0.;
    double newest = 0.;

    ResistSwitchVeto veto = ResistSwitchVeto::None;
};

// GuideAlgorithmResistSwitch::result(): records the input in the history, then passes it only
// on the side the history has settled on, scaled by the aggression.
ResistSwitchStep ResistSwitchResult(ResistSwitchState& state, const ResistSwitchParams& params, double input);

} // namespace guide_algorithm_kernels

#endif // GUIDE_ALGORITHM_KERNELS_H_INCLUDED
