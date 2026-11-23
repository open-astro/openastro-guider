/*
 *  calibration_sanity.h
 *  PHD Guiding
 *
 *  The checks Scope::SanityCheckCalibration runs on a calibration that has
 *  just completed (too few steps, non-orthogonal axes, RA/Dec rates that do
 *  not follow cos(Dec), a Dec rate unlike the previous calibration's),
 *  factored out of scope.cpp so they can be tested without wxWidgets or a
 *  Scope (#142). Kept free of wx; tested by
 *  tests/calibration/test_calibration_sanity.cpp. Only one issue is reported,
 *  the first in that order. The advisory texts, the CalWarning_* opt-out,
 *  FlagCalibrationIssue and the Debug lines stay in Scope.
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

#ifndef CALIBRATION_SANITY_H_INCLUDED
#define CALIBRATION_SANITY_H_INCLUDED

namespace calibration_sanity
{

// Same values as CalibrationIssueType (mount.h); scope.cpp asserts it
enum class Issue
{
    None = 0,
    Steps,
    Angle,
    Rates,
    Different
};

constexpr int CAL_ALERT_MINSTEPS = 4;
constexpr double CAL_ALERT_ORTHOGONALITY_TOLERANCE = 12.5; // Degrees
constexpr double CAL_ALERT_DECRATE_DIFFERENCE = 0.20; // Ratio tolerance
constexpr double CAL_ALERT_AXISRATES_TOLERANCE = 0.20; // Ratio tolerance

// CALIBRATION_RATE_UNCALIBRATED (scope.h) and UNKNOWN_DECLINATION (mount.h)
constexpr double RATE_UNCALIBRATED = 123e4;
constexpr double UNKNOWN_DEC = 997.0;

struct Input
{
    // the new calibration and its details, as read back from the profile
    int raStepCount = 0;
    int decStepCount = 0;
    double xAngle = 0.;
    double yAngle = 0.;
    double xRate = 0.;
    double yRate = 0.;
    double declination = UNKNOWN_DEC;
    double raGuideSpeed = 0.;
    double decGuideSpeed = 0.;
    double imageScale = 0.;
    // the calibration before it
    bool oldValid = false;
    double oldXAngle = 0.;
    double oldYRate = 0.;
    double oldImageScale = 0.;

    bool decCompEnabled = true; // Mount::DecCompensationEnabled()
    double decCompLimit = 0.; // Scope::DEC_COMP_LIMIT
};

struct Result
{
    Issue issue = Issue::None;
    // the numbers the Debug detail line prints for the issue
    int raSteps = 0; // Steps
    int decSteps = 0;
    double nonOrthoDeg = 0.; // Angle
    double declinationDeg = 0.; // Rates
    double expectedRatio = 0.;
    double actualRatio = 0.;
    double decRateRatio = 0.; // Different: previous / current Dec rate
};

Result Classify(const Input& in);

} // namespace calibration_sanity

#endif // CALIBRATION_SANITY_H_INCLUDED
