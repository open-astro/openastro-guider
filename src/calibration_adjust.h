/*
 *  calibration_adjust.h
 *  PHD Guiding
 *
 *  What Mount::AdjustCalibrationForScopePointing does to the calibration on
 *  every entry to Guiding, and the data change of Mount::FlipCalibration,
 *  factored out of mount.cpp so they can be tested without wxWidgets, a
 *  Mount, a camera or a mount (#142). Kept free of wx; tested by
 *  tests/calibration/test_calibration_adjust.cpp.
 *
 *  The adjustment runs in two halves around the meridian flip, because
 *  between them Mount makes calls this code cannot see into: Mount::
 *  SetCalibration for a binning change, MyFrame::HandleImageScaleChange
 *  (which can clear the calibration) and Mount::FlipCalibration (which does
 *  nothing when the mount is not calibrated then). PlanPointing decides the
 *  first half from the calibration as guiding starts; Mount does it, flips,
 *  and gives FinishPointing the calibration it then has, for the rotator and
 *  the Dec compensation of the RA rate. AdjustForPointing runs both halves
 *  on a value copy of the mount's calibration state, the way Mount does them
 *  when the flip finds the mount calibrated. Alert texts, Debug lines and
 *  the calls stay in Mount.
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

#ifndef CALIBRATION_ADJUST_H_INCLUDED
#define CALIBRATION_ADJUST_H_INCLUDED

#include "calibration_codec.h"
#include "calibration_transform.h"

namespace calibration_adjust
{

using calibration_codec::CalibrationRecord;

// Mount guide speeds more than this fraction away from the calibration's raise the
// "Mount guide speeds are different" alert
constexpr double GUIDE_SPEED_TOLERANCE = 0.05;

// Profile and camera pixel sizes this far apart (um) or more are a pixel size change
constexpr double PIXEL_SIZE_TOLERANCE = 1.0;

// An image scale ratio this far from 1 or more is an image scale change (MyFrame::HandleImageScaleChange)
constexpr double IMAGE_SCALE_TOLERANCE = 0.01;

// A rotator move (degrees) larger than this rotates the calibration
constexpr double ROTATOR_TOLERANCE_DEG = 0.05;

// Whether mount guide speeds now (curRa, curDec) differ from the calibration's (calRa, calDec,
// both > 0) by more than GUIDE_SPEED_TOLERANCE on either axis
bool GuideSpeedsChanged(double calRa, double calDec, double curRa, double curDec);

// True when the scope now reports a pier side and the calibration has none: no automatic flip
bool CalibrationLacksPierSide(int newPierSide, int calPierSide);

// True when a and b are East and West, either way round (IsOppositeSide in mount.cpp)
bool IsOppositeSide(int a, int b);

// The binning change of a valid calibration taken at calBinning, now guiding at binning
struct BinningChange
{
    bool apply = false; // binning differs and the calibration is valid: Mount stores the rescaled rates
    double scaleFactor = 1.; // what the image scale ratio is multiplied by
    double xRate = 0.; // the rates rescaled to the new binning
    double yRate = 0.;
};
BinningChange AdjustForBinning(const CalibrationRecord& cal, unsigned short binning);

// The flip of a calibration's data to the other side of the pier (Mount::FlipCalibration). origX and
// origY are the calibration's xAngle() and yAngle() as Mount reports them.
struct FlippedCalibration
{
    double xAngle = 0.; // normalized origX + pi
    double yAngle = 0.; // normalized origY, + pi when decFlipRequired
    int pierSide = calibration_codec::PIER_SIDE_UNKNOWN; // the opposite side; UNKNOWN stays UNKNOWN
    int decGuideParity = calibration_codec::GUIDE_PARITY_UNKNOWN; // inverted unless decFlipRequired
};
FlippedCalibration FlipCalibrationData(double origX, double origY, int pierSide, int decGuideParity, bool decFlipRequired);

// The first half: decided from the calibration and its state as guiding starts
struct PointingInput
{
    CalibrationRecord cal; // Mount's m_cal
    double xRate = 0.; // Mount's m_xRate, the RA rate in use
    double newDeclination = calibration_codec::UNKNOWN_DEC; // radians
    int newPierSide = calibration_codec::PIER_SIDE_UNKNOWN;
    double newRotatorAngle = calibration_codec::ROTATOR_POSITION_UNKNOWN; // degrees
    unsigned short binning = 1; // the camera's binning now
    double cameraPixelSize = 0.; // the camera's pixel size now (um)
    double profilePixelSize = 0.; // the profile's pixel size (um)
    // The calibration's guide speeds (CalibrationDetails) and the mount's now, when the pointing
    // source can report its position, both calibration speeds are > 0 and the mount reported its rates
    bool compareGuideSpeeds = false;
    double calRaGuideSpeed = 0.;
    double calDecGuideSpeed = 0.;
    double raGuideSpeed = 0.;
    double decGuideSpeed = 0.;
    bool decCompEnabled = true; // Mount::DecCompensationEnabled()
    bool decFlipRequired = false; // CalibrationFlipRequiresDecFlip()
    double decCompLimit = 0.; // Scope::DEC_COMP_LIMIT
};

struct PointingPlan
{
    bool guideSpeedsChanged = false; // alert
    bool lacksPierSide = false; // alert
    bool pixelSizeChanged = false; // alert, and the camera's pixel size is written to the profile
    BinningChange binning;
    double scaleAdjustment = 1.; // the image scale ratio
    bool imageScaleChanged = false; // MyFrame::HandleImageScaleChange
    bool flip = false; // Mount::FlipCalibration
};
PointingPlan PlanPointing(const PointingInput& in);

// The second half: decided from the calibration Mount has after the flip
enum class RotatorAction
{
    None, // no rotator, or it has not moved
    UnknownAtCalibration, // alert and record the rotator angle, with no change to the angles
    Rotate, // Mount stores the calibration with both angles turned back by the rotator's move
};

struct PointingFinish
{
    RotatorAction rotator = RotatorAction::None;
    double rotatorDeltaDeg = 0.; // the move, for Rotate
    double xAngle = 0.; // the rotated angles, for Rotate
    double yAngle = 0.;
    calibration_transform::DecComp decComp = calibration_transform::DecComp::None;
    bool decCompensated = false; // Compensate with Dec comp enabled: xRate is the compensated rate
    double clampedDeclination = 0.; // the Dec the compensation used, held within +-89 deg
    double xRate = 0.; // the RA rate to guide with: compensated, or the calibration's
};
PointingFinish FinishPointing(const CalibrationRecord& cal, double newDeclination, double newRotatorAngle, bool decCompEnabled,
                              double decCompLimit);

// Both halves on a value copy of the state, as Mount runs them when the mount is calibrated at the flip
// (calibratedAtFlip): each of Mount's SetCalibration calls stores the record it is given (isValid true)
// and resets xRate to its xRate.
struct PointingResult
{
    PointingPlan plan;
    PointingFinish finish;
    bool flipped = false; // the flip ran
    CalibrationRecord cal; // m_cal afterwards
    double xRate = 0.; // m_xRate afterwards
};
PointingResult AdjustForPointing(const PointingInput& in, bool calibratedAtFlip = true);

} // namespace calibration_adjust

#endif // CALIBRATION_ADJUST_H_INCLUDED
