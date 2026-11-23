/*
 *  calibration_adjust.cpp
 *  PHD Guiding
 *
 *  The calibration adjustment on entry to Guiding. See calibration_adjust.h.
 *  A lift of the arithmetic and decisions of Mount::AdjustCalibrationFor-
 *  ScopePointing and Mount::FlipCalibration, in the same order and with the
 *  same expressions.
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

#include "calibration_adjust.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace calibration_adjust
{

using namespace calibration_codec;

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

    double Radians(double degrees)
    {
        return degrees * M_PI / 180.;
    }

    int OppositeSide(int p)
    {
        switch (p)
        {
        case PIER_SIDE_EAST:
            return PIER_SIDE_WEST;
        case PIER_SIDE_WEST:
            return PIER_SIDE_EAST;
        default:
            return PIER_SIDE_UNKNOWN;
        }
    }

    int OppositeParity(int p)
    {
        switch (p)
        {
        case GUIDE_PARITY_EVEN:
            return GUIDE_PARITY_ODD;
        case GUIDE_PARITY_ODD:
            return GUIDE_PARITY_EVEN;
        default:
            return p;
        }
    }

    // Mount::SetCalibration's effect on m_cal and m_xRate (the parities are never GUIDE_PARITY_UNCHANGED here)
    void Store(CalibrationRecord *mcal, double *xRate, const CalibrationRecord& cal)
    {
        std::string timestamp = mcal->timestamp;
        *mcal = cal;
        mcal->timestamp = timestamp;
        mcal->isValid = true;
        *xRate = cal.xRate;
    }

} // namespace

bool GuideSpeedsChanged(double calRa, double calDec, double curRa, double curDec)
{
    return std::fabs(1.0 - curRa / calRa) > GUIDE_SPEED_TOLERANCE || std::fabs(1.0 - curDec / calDec) > GUIDE_SPEED_TOLERANCE;
}

bool CalibrationLacksPierSide(int newPierSide, int calPierSide)
{
    return newPierSide != PIER_SIDE_UNKNOWN && calPierSide == PIER_SIDE_UNKNOWN;
}

bool IsOppositeSide(int a, int b)
{
    return (a == PIER_SIDE_EAST && b == PIER_SIDE_WEST) || (a == PIER_SIDE_WEST && b == PIER_SIDE_EAST);
}

BinningChange AdjustForBinning(const CalibrationRecord& cal, unsigned short binning)
{
    BinningChange bc;
    bc.xRate = cal.xRate;
    bc.yRate = cal.yRate;
    if (binning != cal.binning && cal.isValid)
    {
        bc.apply = true;
        // KNOWN BUG, kept as it is: unsigned short (int) division, so 2 -> 3 gives 1, not 1.5, and the
        // image scale change goes unseen (fixed on fix/142-adjust-for-pointing-binning)
        bc.scaleFactor = binning / cal.binning;
        double adj = (double) cal.binning / (double) binning;
        bc.xRate *= adj;
        bc.yRate *= adj;
    }
    return bc;
}

FlippedCalibration FlipCalibrationData(double origX, double origY, int pierSide, int decGuideParity, bool decFlipRequired)
{
    double newX = origX + M_PI;
    double newY = origY;

    if (decFlipRequired)
    {
        newY += M_PI;
    }

    FlippedCalibration f;
    f.xAngle = NormAngle(newX);
    f.yAngle = NormAngle(newY);
    f.pierSide = OppositeSide(pierSide);
    // Dec polarity changes when pier side changes, i.e. if Guide(NORTH) moves the star north on one side,
    // then Guide(NORTH) will move the star south on the other side of the pier.
    // For mounts with CalibrationFlipRequiresDecFlip, the parity does not change after the flip.
    f.decGuideParity = decFlipRequired ? decGuideParity : OppositeParity(decGuideParity);
    return f;
}

PointingPlan PlanPointing(const PointingInput& in)
{
    PointingPlan plan;

    if (in.compareGuideSpeeds)
        plan.guideSpeedsChanged =
            GuideSpeedsChanged(in.calRaGuideSpeed, in.calDecGuideSpeed, in.raGuideSpeed, in.decGuideSpeed);

    plan.lacksPierSide = CalibrationLacksPierSide(in.newPierSide, in.cal.pierSide);

    // Compensate for binning or pixel size changes
    double scaleAdjustment = 1.0;
    if (std::fabs(in.cameraPixelSize - in.profilePixelSize) >= PIXEL_SIZE_TOLERANCE)
    {
        plan.pixelSizeChanged = true;
        scaleAdjustment = in.cameraPixelSize / in.profilePixelSize;
    }

    plan.binning = AdjustForBinning(in.cal, in.binning);
    if (plan.binning.apply)
        scaleAdjustment *= plan.binning.scaleFactor;

    plan.scaleAdjustment = scaleAdjustment;
    plan.imageScaleChanged = std::fabs(scaleAdjustment - 1.0) >= IMAGE_SCALE_TOLERANCE;

    // the binning change keeps the pier side, so this is the side the flip sees
    plan.flip = IsOppositeSide(in.newPierSide, in.cal.pierSide);

    return plan;
}

PointingFinish FinishPointing(const CalibrationRecord& cal, double newDeclination, double newRotatorAngle, bool decCompEnabled,
                              double decCompLimit)
{
    PointingFinish f;

    if (newRotatorAngle != ROTATOR_POSITION_UNKNOWN)
    {
        if (cal.rotatorAngle == ROTATOR_POSITION_UNKNOWN)
        {
            // we do not know the rotator position at calibration time so cannot automatically adjust calibration
            f.rotator = RotatorAction::UnknownAtCalibration;
        }
        else
        {
            double da = newRotatorAngle - cal.rotatorAngle;

            if (std::fabs(da) > ROTATOR_TOLERANCE_DEG)
            {
                f.rotator = RotatorAction::Rotate;
                f.rotatorDeltaDeg = da;

                da = Radians(da);

                f.xAngle = NormAngle(cal.xAngle - da);
                f.yAngle = NormAngle(cal.yAngle - da);
            }
        }
    }

    // compensate RA guide rate for declination if the declination changed and we know both the
    // calibration declination and the current declination (#157)
    std::optional<double> calDec;
    std::optional<double> nowDec;
    if (cal.declination != UNKNOWN_DEC)
        calDec = cal.declination;
    if (newDeclination != UNKNOWN_DEC)
        nowDec = newDeclination;

    f.decComp = calibration_transform::DecideDecComp(calDec, nowDec, decCompLimit);
    f.clampedDeclination = newDeclination;
    f.xRate = cal.xRate;
    if (f.decComp == calibration_transform::DecComp::Compensate && decCompEnabled)
    {
        // Don't do a full dec comp too close to pole - xRate will become a huge number and will cause problems downstream
        f.clampedDeclination = std::max(Radians(-89.0), std::min(Radians(89.0), newDeclination));
        f.xRate = calibration_transform::CompensatedRaRate(cal.xRate, cal.declination, f.clampedDeclination);
        f.decCompensated = true;
    }

    return f;
}

PointingResult AdjustForPointing(const PointingInput& in, bool calibratedAtFlip)
{
    PointingResult r;
    r.cal = in.cal;
    r.xRate = in.xRate;

    r.plan = PlanPointing(in);

    if (r.plan.binning.apply)
    {
        CalibrationRecord cal(r.cal);
        cal.xRate = r.plan.binning.xRate;
        cal.yRate = r.plan.binning.yRate;
        cal.binning = in.binning;
        Store(&r.cal, &r.xRate, cal);
    }

    if (r.plan.flip && calibratedAtFlip)
    {
        // Mount::xAngle() and yAngle(): yAngle() is rebuilt from the stored yAngleError
        double origX = r.cal.xAngle;
        double origY = NormAngle(r.cal.xAngle - calibration_transform::YAngleError(r.cal.xAngle, r.cal.yAngle) + M_PI / 2.);
        FlippedCalibration f = FlipCalibrationData(origX, origY, r.cal.pierSide, r.cal.decGuideParity, in.decFlipRequired);
        CalibrationRecord cal(r.cal);
        cal.xAngle = f.xAngle;
        cal.yAngle = f.yAngle;
        cal.pierSide = f.pierSide;
        cal.decGuideParity = f.decGuideParity;
        Store(&r.cal, &r.xRate, cal);
        r.flipped = true;
    }

    r.finish = FinishPointing(r.cal, in.newDeclination, in.newRotatorAngle, in.decCompEnabled, in.decCompLimit);

    switch (r.finish.rotator)
    {
    case RotatorAction::None:
        break;
    case RotatorAction::UnknownAtCalibration:
        r.cal.rotatorAngle = in.newRotatorAngle;
        break;
    case RotatorAction::Rotate:
    {
        CalibrationRecord cal(r.cal);
        cal.xAngle = r.finish.xAngle;
        cal.yAngle = r.finish.yAngle;
        cal.rotatorAngle = in.newRotatorAngle;
        Store(&r.cal, &r.xRate, cal);
        break;
    }
    }

    r.xRate = r.finish.xRate;

    return r;
}

} // namespace calibration_adjust
