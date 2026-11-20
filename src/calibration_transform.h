/*
 *  calibration_transform.h
 *  PHD Guiding
 *
 *  Pure calibration coordinate-transform math, factored out of Mount so it can
 *  be unit tested without wxWidgets, a Mount instance, or a live scope.
 *
 *  These convert between camera pixel vectors and mount (RA/Dec) vectors using
 *  the calibration's RA-axis angle (xAngle) and the measured axis-orthogonality
 *  error (yAngleError). Everything here is a free function over plain numbers:
 *  no wxWidgets, no PHD_Point (point.h also declares ShiftPoint, which pulls in
 *  wxLongLong), no member state, no logging. Mount::Transform* read their
 *  members, call these, and keep the validity checks + Debug logging.
 *
 *  DecideDecComp and CompensatedRaRate are the Dec compensation of the RA rate
 *  that Mount::AdjustCalibrationForScopePointing applies on every entry to
 *  Guiding (#157); Mount keeps the alert, the Dec comp setting and the logging.
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

#ifndef CALIBRATION_TRANSFORM_H_INCLUDED
#define CALIBRATION_TRANSFORM_H_INCLUDED

#include <optional>

namespace calibration_transform
{

struct XY
{
    double x = 0.;
    double y = 0.;
};

// Result of CameraToMount, including the intermediates Mount::Transform* logs.
struct CamToMount
{
    XY mount; // the transformed mount vector
    double hyp = 0.; // vector length (preserved by the rotation)
    double cameraTheta = 0.; // input vector angle (radians)
    double xAngle = 0.; // cameraTheta - calXAngle
    double yAngle = 0.; // cameraTheta - (calXAngle + yAngleError)
};

// Result of MountToCamera, including the intermediates Mount::Transform* logs.
struct MountToCam
{
    XY camera; // the transformed camera vector
    double hyp = 0.;
    double mountTheta = 0.; // input vector angle (radians), possibly sign-flipped
    double xAngle = 0.; // mountTheta + calXAngle
};

// Camera pixel vector -> mount vector. calXAngle is the calibration RA-axis
// angle; yAngleError is the axis-orthogonality error. Mirrors
// Mount::TransformCameraCoordinatesToMountCoordinates.
CamToMount CameraToMount(double camX, double camY, double calXAngle, double yAngleError);

// Mount vector -> camera pixel vector. Mirrors
// Mount::TransformMountCoordinatesToCameraCoordinates (including the
// mountTheta sign flip when |yAngleError| > pi/2).
MountToCam MountToCamera(double mountX, double mountY, double calXAngle, double yAngleError);

// The axis-orthogonality error of a calibration, norm_angle(xAngle - yAngle + pi/2): about 0
// when a North pulse moves the star at the angle of a West pulse's motion minus pi/2, about pi
// when the Dec axis is mirrored. Mount::SetCalibration stores it as m_yAngleError.
double YAngleError(double calXAngle, double calYAngle);

// What Mount::AdjustCalibrationForScopePointing does with the RA rate when guiding starts (#157).
enum class DecComp
{
    None, // guide with the calibration's RA rate as measured
    Compensate, // scale the RA rate by cos(Dec now) / cos(calibration Dec), when Dec comp is enabled
    Alert, // calibrated above the Dec comp limit and the RA rate has changed: recalibration is needed
};

// A Dec reading within this of the calibration Dec has not moved: 72", above one 1' step of a
// low-precision LX200 Dec and the arc-second rounding of ASCOM and Alpaca drivers. Ignoring a
// smaller change below the 60 deg limit changes the RA rate by tan(Dec) * 0.02 deg, at most 0.06%.
constexpr double DEC_COMP_MIN_MOVE_DEG = 0.02;

// Above the Dec comp limit the calibration's RA rate is used as measured, and the alert asks for
// a new calibration only when the move changed the RA rate by more than this fraction,
// |cos(Dec now) / cos(calibration Dec) - 1|: the 5% the same function allows a mount guide
// speed to drift from the calibration's before it alerts. At Dec 80 that is a move of about 0.5 deg.
constexpr double DEC_COMP_RATE_TOLERANCE = 0.05;

// Decides the Dec compensation for a calibration made at calDecRad, guiding at newDecRad
// (radians; nullopt or non-finite when unknown) with the Dec comp limit compLimitRad
// (Scope::DEC_COMP_LIMIT). An unknown Dec, or one that moved no more than DEC_COMP_MIN_MOVE_DEG,
// is None. Otherwise a calibration within the limit is Compensate; one beyond it is Alert when
// the RA rate changed by more than DEC_COMP_RATE_TOLERANCE, else None.
DecComp DecideDecComp(std::optional<double> calDecRad, std::optional<double> newDecRad, double compLimitRad);

// The RA rate for guiding at newDecRad from a calibration of calXRate at calDecRad:
// calXRate / cos(calDecRad) * cos(newDecRad), with newDecRad held within +-89 deg so the rate
// does not grow huge near the pole.
double CompensatedRaRate(double calXRate, double calDecRad, double newDecRad);

} // namespace calibration_transform

#endif // CALIBRATION_TRANSFORM_H_INCLUDED
