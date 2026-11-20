/*
 *  calibration_transform.cpp
 *  PHD Guiding
 *
 *  Pure calibration coordinate-transform math. See calibration_transform.h.
 *  Verbatim lift of the bodies of Mount::TransformCameraCoordinatesToMount-
 *  Coordinates / TransformMountCoordinatesToCameraCoordinates, operating on
 *  plain doubles instead of PHD_Point. The (0,0) -> angle 0 guard matches
 *  PHD_Point::Angle().
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

#include "calibration_transform.h"

#include <algorithm>
#include <cmath>

namespace
{

// Matches PHD_Point::Angle(): atan2(y, x), but returns 0 for the (0,0) vector
// since atan2(0,0) is implementation-defined.
double vectorAngle(double x, double y)
{
    if (x != 0. || y != 0.)
        return std::atan2(y, x);
    return 0.;
}

} // namespace

namespace calibration_transform
{

CamToMount CameraToMount(double camX, double camY, double calXAngle, double yAngleError)
{
    CamToMount out;
    out.hyp = std::hypot(camX, camY);
    out.cameraTheta = vectorAngle(camX, camY);

    // xAngle measures RA axis rotation vs camera X axis, positive is CW from x axis
    out.xAngle = out.cameraTheta - calXAngle;
    // yAngleError is the orthogonality error
    out.yAngle = out.cameraTheta - (calXAngle + yAngleError);

    out.mount.x = std::cos(out.xAngle) * out.hyp;
    out.mount.y = std::sin(out.yAngle) * out.hyp;
    return out;
}

MountToCam MountToCamera(double mountX, double mountY, double calXAngle, double yAngleError)
{
    MountToCam out;
    out.hyp = std::hypot(mountX, mountY);
    out.mountTheta = vectorAngle(mountX, mountY);

    if (std::fabs(yAngleError) > M_PI / 2.)
        out.mountTheta = -out.mountTheta;

    out.xAngle = out.mountTheta + calXAngle;

    out.camera.x = std::cos(out.xAngle) * out.hyp;
    out.camera.y = std::sin(out.xAngle) * out.hyp;
    return out;
}

double YAngleError(double calXAngle, double calYAngle)
{
    // norm_angle() from image_math.h: norm(val, -pi, pi)
    double const val = calXAngle - calYAngle + M_PI / 2.;
    double const start = -M_PI;
    double const range = M_PI - start;
    double const ofs = val - start;
    return val - std::floor(ofs / range) * range;
}

DecComp DecideDecComp(std::optional<double> calDecRad, std::optional<double> newDecRad, double compLimitRad)
{
    if (!calDecRad || !newDecRad || !std::isfinite(*calDecRad) || !std::isfinite(*newDecRad))
        return DecComp::None;

    // A reading that moved by a driver's rounding or the calibration's residual has not moved (#157).
    // Before, any difference in the doubles acted.
    double const cal = *calDecRad;
    double const now = *newDecRad;
    if (std::fabs(now - cal) <= DEC_COMP_MIN_MOVE_DEG * M_PI / 180.)
        return DecComp::None;

    if (std::fabs(cal) <= compLimitRad)
        return DecComp::Compensate;

    // |cos(now) / cos(cal) - 1| > tolerance, without dividing by a cos(cal) near 0
    double const cosCal = std::fabs(std::cos(cal));
    if (std::fabs(std::fabs(std::cos(now)) - cosCal) > DEC_COMP_RATE_TOLERANCE * cosCal)
        return DecComp::Alert;
    return DecComp::None;
}

double CompensatedRaRate(double calXRate, double calDecRad, double newDecRad)
{
    double const limit = 89. * M_PI / 180.;
    newDecRad = std::max(-limit, std::min(limit, newDecRad));
    return calXRate / std::cos(calDecRad) * std::cos(newDecRad);
}

} // namespace calibration_transform
