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

} // namespace calibration_transform

#endif // CALIBRATION_TRANSFORM_H_INCLUDED
