/*
 *  calibration_sanity.cpp
 *  PHD Guiding
 *
 *  The calibration sanity classifier. See calibration_sanity.h. A lift of
 *  the checks in Scope::SanityCheckCalibration, in the same order and with
 *  the same expressions.
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

#include "calibration_sanity.h"

#include <cmath>

namespace calibration_sanity
{

namespace
{

    // norm_angle() from image_math.h: norm(val, -pi, pi)
    double NormAngle(double val)
    {
        double const start = -M_PI;
        double const range = M_PI - start;
        double const ofs = val - start;
        return val - std::floor(ofs / range) * range;
    }

    double Degrees(double radians)
    {
        return radians * 180. / M_PI;
    }

} // namespace

Result Classify(const Input& in)
{
    Result r;
    int xSteps = in.raStepCount;
    int ySteps = in.decStepCount;

    // Too few steps
    if (xSteps < CAL_ALERT_MINSTEPS || (ySteps < CAL_ALERT_MINSTEPS && ySteps > 0)) // Dec guiding might be disabled
    {
        r.issue = Issue::Steps;
        r.raSteps = xSteps;
        r.decSteps = ySteps;
        return r;
    }

    // Non-orthogonal RA/Dec axes
    double nonOrtho = Degrees(
        std::fabs(std::fabs(NormAngle(in.xAngle - in.yAngle)) - M_PI / 2.)); // Delta from the nearest multiple of 90 degrees
    if (nonOrtho > CAL_ALERT_ORTHOGONALITY_TOLERANCE)
    {
        r.issue = Issue::Angle;
        r.nonOrthoDeg = nonOrtho;
    }
    else
    {
        // RA/Dec rates should be related by cos(dec) but don't check if Dec is too high or Dec guiding is disabled.  Also
        // don't check if DecComp is disabled because a Sitech controller might be monkeying around with the apparent rates
        if (in.declination != UNKNOWN_DEC && in.yRate != RATE_UNCALIBRATED && std::fabs(in.declination) <= in.decCompLimit &&
            in.decCompEnabled)
        {
            double expectedRatio = std::cos(in.declination);
            double speedRatio;
            if (in.raGuideSpeed > 0.) // for mounts that may have different guide speeds on RA and Dec axes
                speedRatio = in.decGuideSpeed / in.raGuideSpeed;
            else
                speedRatio = 1.0;
            double actualRatio = in.xRate * speedRatio / in.yRate;
            if (std::fabs(expectedRatio - actualRatio) > CAL_ALERT_AXISRATES_TOLERANCE)
            {
                r.issue = Issue::Rates;
                r.declinationDeg = Degrees(in.declination);
                r.expectedRatio = expectedRatio;
                r.actualRatio = actualRatio;
            }
        }
    }

    // Finally check for a significantly different result but don't be stupid - ignore differences if the configuration
    // looks quite different Can't do straight equality checks because of rounding - the "old" values have passed through
    // the registry get/set routines
    if (r.issue == Issue::None && in.oldValid && std::fabs(in.oldImageScale - in.imageScale) < 0.1 &&
        (std::fabs(Degrees(in.oldXAngle - in.xAngle)) < 5.0))
    {
        if (in.yRate != RATE_UNCALIBRATED && in.oldYRate != RATE_UNCALIBRATED)
        {
            double newDecRate = in.yRate;
            if (newDecRate != 0.)
            {
                if (std::fabs(1.0 - (in.oldYRate / newDecRate)) > CAL_ALERT_DECRATE_DIFFERENCE)
                {
                    r.issue = Issue::Different;
                    r.decRateRatio = in.oldYRate / newDecRate;
                }
            }
        }
    }

    return r;
}

} // namespace calibration_sanity
