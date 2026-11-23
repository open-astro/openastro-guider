// Unit tests for src/calibration_adjust.cpp (#142): what entering Guiding does to the
// calibration, extracted from Mount::AdjustCalibrationForScopePointing, and the data
// change of Mount::FlipCalibration.
//
// The guider recalibrates on every new target, so most of this runs on a calibration
// that has just been measured where the scope points. It matters after
// restore_calibration, guide{recalibrate:false} and a meridian flip: a wrong flip makes
// Dec run away, and a wrong RA rate or binning rescale makes every RA pulse the wrong
// length. These pin what the code does, including the binning fix of #252 below.

#include <gtest/gtest.h>

#include "calibration_adjust.h"

#include <cmath>

using namespace calibration_adjust;
using calibration_codec::GUIDE_PARITY_EVEN;
using calibration_codec::GUIDE_PARITY_ODD;
using calibration_codec::GUIDE_PARITY_UNKNOWN;
using calibration_codec::PIER_SIDE_EAST;
using calibration_codec::PIER_SIDE_UNKNOWN;
using calibration_codec::PIER_SIDE_WEST;
using calibration_codec::ROTATOR_POSITION_UNKNOWN;
using calibration_codec::UNKNOWN_DEC;
using calibration_transform::DecComp;

namespace
{

// Scope::DEC_COMP_LIMIT, spelled as scope.cpp spells it
const double DEC_COMP_LIMIT = M_PI / 2.0 * 2.0 / 3.0;

double Rad(double deg)
{
    return deg * M_PI / 180.;
}

double Deg(double rad)
{
    return rad * 180. / M_PI;
}

// A valid calibration at Dec 0 on the East side, binning 1, no rotator
CalibrationRecord Cal()
{
    CalibrationRecord c;
    c.xRate = 0.004; // px/ms
    c.yRate = 0.003;
    c.xAngle = Rad(30.);
    c.yAngle = Rad(30. - 90.);
    c.declination = 0.;
    c.rotatorAngle = ROTATOR_POSITION_UNKNOWN;
    c.binning = 1;
    c.pierSide = PIER_SIDE_EAST;
    c.raGuideParity = GUIDE_PARITY_EVEN;
    c.decGuideParity = GUIDE_PARITY_EVEN;
    c.isValid = true;
    return c;
}

// Guiding starts where the calibration was made, with nothing changed
PointingInput Same(const CalibrationRecord& cal = Cal())
{
    PointingInput in;
    in.cal = cal;
    in.xRate = cal.xRate;
    in.newDeclination = cal.declination;
    in.newPierSide = cal.pierSide;
    in.newRotatorAngle = ROTATOR_POSITION_UNKNOWN;
    in.binning = cal.binning;
    in.cameraPixelSize = 3.75;
    in.profilePixelSize = 3.75;
    in.decCompEnabled = true;
    in.decFlipRequired = false;
    in.decCompLimit = DEC_COMP_LIMIT;
    return in;
}

// The angle between a and b, -pi..pi
double AngleDiff(double a, double b)
{
    return std::remainder(a - b, 2. * M_PI);
}

} // namespace

TEST(AdjustForPointing, NothingChangedLeavesTheCalibrationAlone)
{
    PointingResult r = AdjustForPointing(Same());
    EXPECT_FALSE(r.plan.guideSpeedsChanged);
    EXPECT_FALSE(r.plan.lacksPierSide);
    EXPECT_FALSE(r.plan.pixelSizeChanged);
    EXPECT_FALSE(r.plan.binning.apply);
    EXPECT_FALSE(r.plan.imageScaleChanged);
    EXPECT_FALSE(r.plan.flip);
    EXPECT_FALSE(r.flipped);
    EXPECT_EQ(r.finish.rotator, RotatorAction::None);
    EXPECT_EQ(r.finish.decComp, DecComp::None);
    EXPECT_EQ(r.xRate, Cal().xRate);
    EXPECT_EQ(r.cal.xAngle, Cal().xAngle);
    EXPECT_EQ(r.cal.yAngle, Cal().yAngle);
}

// ---- Dec change ----------------------------------------------------------------------------

TEST(AdjustForPointing, DecZeroToSixtyHalvesTheRaRate)
{
    PointingInput in = Same();
    in.newDeclination = Rad(60.);
    PointingResult r = AdjustForPointing(in);
    EXPECT_EQ(r.finish.decComp, DecComp::Compensate);
    EXPECT_TRUE(r.finish.decCompensated);
    EXPECT_NEAR(r.xRate, 0.5 * Cal().xRate, 1e-12);
    // the compensated rate is never stored in the calibration
    EXPECT_EQ(r.cal.xRate, Cal().xRate);
}

TEST(AdjustForPointing, DecCompDisabledKeepsTheCalibrationRate)
{
    PointingInput in = Same();
    in.newDeclination = Rad(60.);
    in.decCompEnabled = false;
    PointingResult r = AdjustForPointing(in);
    EXPECT_EQ(r.finish.decComp, DecComp::Compensate);
    EXPECT_FALSE(r.finish.decCompensated);
    EXPECT_EQ(r.xRate, Cal().xRate);
}

TEST(AdjustForPointing, DecNearThePoleIsClampedTo89)
{
    PointingInput in = Same();
    in.newDeclination = Rad(89.9);
    PointingResult r = AdjustForPointing(in);
    EXPECT_TRUE(r.finish.decCompensated);
    EXPECT_NEAR(Deg(r.finish.clampedDeclination), 89., 1e-9);
    EXPECT_TRUE(std::isfinite(r.xRate));
    EXPECT_NEAR(r.xRate, Cal().xRate * std::cos(Rad(89.)), 1e-12);

    in.newDeclination = Rad(-89.9);
    r = AdjustForPointing(in);
    EXPECT_NEAR(Deg(r.finish.clampedDeclination), -89., 1e-9);
}

TEST(AdjustForPointing, CalibrationFarFromTheEquatorAlertsAndKeepsTheRate)
{
    CalibrationRecord cal = Cal();
    cal.declination = Rad(65.);
    PointingInput in = Same(cal);
    in.newDeclination = Rad(70.);
    PointingResult r = AdjustForPointing(in);
    EXPECT_EQ(r.finish.decComp, DecComp::Alert);
    EXPECT_FALSE(r.finish.decCompensated);
    EXPECT_EQ(r.xRate, cal.xRate);
}

TEST(AdjustForPointing, ACompensatedRateFromTheLastGuideIsReset)
{
    // m_xRate still holds the last guide start's compensated rate; an unknown Dec now resets it
    PointingInput in = Same();
    in.xRate = 2. * Cal().xRate;
    in.newDeclination = UNKNOWN_DEC;
    PointingResult r = AdjustForPointing(in);
    EXPECT_EQ(r.finish.decComp, DecComp::None);
    EXPECT_EQ(r.xRate, Cal().xRate);

    CalibrationRecord cal = Cal();
    cal.declination = UNKNOWN_DEC;
    in = Same(cal);
    in.xRate = 2. * cal.xRate;
    in.newDeclination = Rad(40.);
    r = AdjustForPointing(in);
    EXPECT_EQ(r.xRate, cal.xRate);
}

// ---- Pier side flip ------------------------------------------------------------------------

TEST(AdjustForPointing, OppositeSideFlipsTheRaAngleAndTheDecParity)
{
    PointingInput in = Same();
    in.newPierSide = PIER_SIDE_WEST;
    PointingResult r = AdjustForPointing(in);
    EXPECT_TRUE(r.plan.flip);
    EXPECT_TRUE(r.flipped);
    EXPECT_NEAR(AngleDiff(r.cal.xAngle, Cal().xAngle + M_PI), 0., 1e-12);
    EXPECT_NEAR(AngleDiff(r.cal.yAngle, Cal().yAngle), 0., 1e-12);
    EXPECT_EQ(r.cal.pierSide, PIER_SIDE_WEST);
    EXPECT_EQ(r.cal.decGuideParity, GUIDE_PARITY_ODD);
    EXPECT_EQ(r.cal.raGuideParity, GUIDE_PARITY_EVEN);
    EXPECT_EQ(r.cal.xRate, Cal().xRate);
    EXPECT_EQ(r.cal.yRate, Cal().yRate);
    EXPECT_EQ(r.cal.declination, Cal().declination);
    EXPECT_EQ(r.cal.binning, Cal().binning);
    // the angles stay normalized
    EXPECT_LE(std::fabs(r.cal.xAngle), M_PI);
}

TEST(AdjustForPointing, DecFlipRequiredFlipsBothAnglesAndKeepsTheParity)
{
    PointingInput in = Same();
    in.newPierSide = PIER_SIDE_WEST;
    in.decFlipRequired = true;
    PointingResult r = AdjustForPointing(in);
    EXPECT_NEAR(AngleDiff(r.cal.xAngle, Cal().xAngle + M_PI), 0., 1e-12);
    EXPECT_NEAR(AngleDiff(r.cal.yAngle, Cal().yAngle + M_PI), 0., 1e-12);
    EXPECT_EQ(r.cal.decGuideParity, GUIDE_PARITY_EVEN);
}

TEST(AdjustForPointing, UnknownPierSideDoesNotFlip)
{
    PointingInput in = Same();
    in.newPierSide = PIER_SIDE_UNKNOWN;
    EXPECT_FALSE(AdjustForPointing(in).flipped);

    // a calibration with no pier side alerts once the scope reports one, and does not flip
    CalibrationRecord cal = Cal();
    cal.pierSide = PIER_SIDE_UNKNOWN;
    in = Same(cal);
    in.newPierSide = PIER_SIDE_WEST;
    PointingResult r = AdjustForPointing(in);
    EXPECT_TRUE(r.plan.lacksPierSide);
    EXPECT_FALSE(r.plan.flip);
    EXPECT_EQ(r.cal.xAngle, cal.xAngle);
}

TEST(AdjustForPointing, NoFlipWhenTheMountIsNotCalibratedAtTheFlip)
{
    PointingInput in = Same();
    in.newPierSide = PIER_SIDE_WEST;
    PointingResult r = AdjustForPointing(in, false);
    EXPECT_TRUE(r.plan.flip);
    EXPECT_FALSE(r.flipped);
    EXPECT_EQ(r.cal.pierSide, PIER_SIDE_EAST);
}

TEST(FlipCalibrationData, FlippingTwiceIsTheIdentity)
{
    for (bool decFlip : { false, true })
    {
        FlippedCalibration a = FlipCalibrationData(Rad(170.), Rad(80.), PIER_SIDE_EAST, GUIDE_PARITY_ODD, decFlip);
        FlippedCalibration b = FlipCalibrationData(a.xAngle, a.yAngle, a.pierSide, a.decGuideParity, decFlip);
        EXPECT_NEAR(AngleDiff(b.xAngle, Rad(170.)), 0., 1e-12);
        EXPECT_NEAR(AngleDiff(b.yAngle, Rad(80.)), 0., 1e-12);
        EXPECT_EQ(b.pierSide, PIER_SIDE_EAST);
        EXPECT_EQ(b.decGuideParity, GUIDE_PARITY_ODD);
    }
}

TEST(FlipCalibrationData, UnknownStaysUnknown)
{
    FlippedCalibration f = FlipCalibrationData(0., 0., PIER_SIDE_UNKNOWN, GUIDE_PARITY_UNKNOWN, false);
    EXPECT_EQ(f.pierSide, PIER_SIDE_UNKNOWN);
    EXPECT_EQ(f.decGuideParity, GUIDE_PARITY_UNKNOWN);
}

// ---- Rotator -------------------------------------------------------------------------------

TEST(AdjustForPointing, RotatorMoveTurnsBothAnglesBack)
{
    CalibrationRecord cal = Cal();
    cal.rotatorAngle = 100.;
    PointingInput in = Same(cal);
    in.newRotatorAngle = 110.;
    PointingResult r = AdjustForPointing(in);
    EXPECT_EQ(r.finish.rotator, RotatorAction::Rotate);
    EXPECT_NEAR(r.finish.rotatorDeltaDeg, 10., 1e-12);
    EXPECT_NEAR(AngleDiff(r.cal.xAngle, cal.xAngle - Rad(10.)), 0., 1e-12);
    EXPECT_NEAR(AngleDiff(r.cal.yAngle, cal.yAngle - Rad(10.)), 0., 1e-12);
    EXPECT_EQ(r.cal.rotatorAngle, 110.);

    // within 0.05 deg is no move
    in.newRotatorAngle = 100.04;
    EXPECT_EQ(AdjustForPointing(in).finish.rotator, RotatorAction::None);
}

TEST(AdjustForPointing, RotatorUnknownAtCalibrationAlertsAndRecordsTheAngle)
{
    PointingInput in = Same();
    in.newRotatorAngle = 45.;
    PointingResult r = AdjustForPointing(in);
    EXPECT_EQ(r.finish.rotator, RotatorAction::UnknownAtCalibration);
    EXPECT_EQ(r.cal.rotatorAngle, 45.);
    EXPECT_EQ(r.cal.xAngle, Cal().xAngle);
}

// ---- Guide speeds and pixel size -----------------------------------------------------------

TEST(AdjustForPointing, GuideSpeedsMoreThanFivePercentOffAlert)
{
    PointingInput in = Same();
    in.compareGuideSpeeds = true;
    in.calRaGuideSpeed = in.calDecGuideSpeed = 0.002;
    in.raGuideSpeed = in.decGuideSpeed = 0.002 * 1.04;
    EXPECT_FALSE(AdjustForPointing(in).plan.guideSpeedsChanged);
    in.decGuideSpeed = 0.002 * 0.94;
    EXPECT_TRUE(AdjustForPointing(in).plan.guideSpeedsChanged);

    // not compared (no position, a calibration speed <= 0, or the rates read failed)
    in.compareGuideSpeeds = false;
    EXPECT_FALSE(AdjustForPointing(in).plan.guideSpeedsChanged);
}

TEST(AdjustForPointing, PixelSizeChangeIsAnImageScaleChange)
{
    PointingInput in = Same();
    in.cameraPixelSize = 7.5;
    PointingResult r = AdjustForPointing(in);
    EXPECT_TRUE(r.plan.pixelSizeChanged);
    EXPECT_DOUBLE_EQ(r.plan.scaleAdjustment, 2.0);
    EXPECT_TRUE(r.plan.imageScaleChanged);

    // under 1 um apart is the same camera
    in.cameraPixelSize = 4.7;
    r = AdjustForPointing(in);
    EXPECT_FALSE(r.plan.pixelSizeChanged);
    EXPECT_FALSE(r.plan.imageScaleChanged);
}

// ---- Binning -------------------------------------------------------------------------------

TEST(AdjustForPointing, BinningOneToTwoHalvesBothRates)
{
    PointingInput in = Same();
    in.binning = 2;
    PointingResult r = AdjustForPointing(in);
    EXPECT_TRUE(r.plan.binning.apply);
    EXPECT_DOUBLE_EQ(r.cal.xRate, 0.5 * Cal().xRate);
    EXPECT_DOUBLE_EQ(r.cal.yRate, 0.5 * Cal().yRate);
    EXPECT_EQ(r.cal.binning, 2);
    EXPECT_DOUBLE_EQ(r.plan.scaleAdjustment, 2.0);
    EXPECT_TRUE(r.plan.imageScaleChanged);
    EXPECT_DOUBLE_EQ(r.xRate, 0.5 * Cal().xRate);
}

TEST(AdjustForPointing, BinningRescaleIsDoneBeforeTheDecCompensation)
{
    PointingInput in = Same();
    in.binning = 2;
    in.newDeclination = Rad(60.);
    PointingResult r = AdjustForPointing(in);
    EXPECT_NEAR(r.xRate, 0.25 * Cal().xRate, 1e-12);
}

TEST(AdjustForPointing, AnInvalidCalibrationIsNotRescaled)
{
    CalibrationRecord cal = Cal();
    cal.isValid = false;
    PointingInput in = Same(cal);
    in.binning = 2;
    PointingResult r = AdjustForPointing(in);
    EXPECT_FALSE(r.plan.binning.apply);
    EXPECT_FALSE(r.plan.imageScaleChanged);
    EXPECT_EQ(r.cal.xRate, cal.xRate);
}

// The image scale factor of a binning change is the exact ratio new / old (#252). It was an
// integer division of two unsigned shorts, so 2 -> 3 gave 1 instead of 1.5 and the image scale
// change (HandleImageScaleChange: a new calibration step, min-move reset, calibration cleared)
// was skipped; 2 -> 1 gave 0 instead of 0.5 and 3 -> 2 gave 0 instead of 2/3. Every binning
// change is now an image scale change.
TEST(AdjustForPointing, BinningImageScaleFactorIsTheExactRatio)
{
    struct Case
    {
        unsigned short from, to;
        double factor;
    };
    const Case cases[] = {
        { 1, 2, 2. },  { 1, 3, 3. },  { 2, 4, 2. },      { 2, 3, 1.5 },  { 3, 2, 2. / 3. },
        { 2, 1, 0.5 }, { 4, 2, 0.5 }, { 3, 4, 4. / 3. }, { 4, 3, 0.75 }, { 1, 4, 4. },
    };
    for (const Case& c : cases)
    {
        CalibrationRecord cal = Cal();
        cal.binning = c.from;
        PointingInput in = Same(cal);
        in.binning = c.to;
        PointingResult r = AdjustForPointing(in);
        SCOPED_TRACE(testing::Message() << c.from << " -> " << c.to);
        EXPECT_TRUE(r.plan.binning.apply);
        EXPECT_DOUBLE_EQ(r.plan.binning.scaleFactor, c.factor);
        EXPECT_DOUBLE_EQ(r.plan.scaleAdjustment, c.factor);
        EXPECT_TRUE(r.plan.imageScaleChanged);
        // the rates use the exact ratio
        EXPECT_DOUBLE_EQ(r.cal.xRate, Cal().xRate * c.from / c.to);
        EXPECT_DOUBLE_EQ(r.cal.yRate, Cal().yRate * c.from / c.to);
    }
}

TEST(AdjustForPointing, PixelSizeAndBinningFactorsMultiply)
{
    CalibrationRecord cal = Cal();
    cal.binning = 2;
    PointingInput in = Same(cal);
    in.binning = 3;
    in.cameraPixelSize = 7.5;
    PointingResult r = AdjustForPointing(in);
    EXPECT_DOUBLE_EQ(r.plan.scaleAdjustment, 2.0 * 1.5);
    EXPECT_TRUE(r.plan.imageScaleChanged);

    // a smaller pixel and a larger binning that cancel are no image scale change
    in.cameraPixelSize = 2.5;
    in.profilePixelSize = 3.75;
    r = AdjustForPointing(in);
    EXPECT_TRUE(r.plan.pixelSizeChanged);
    EXPECT_NEAR(r.plan.scaleAdjustment, 1.0, 1e-12);
    EXPECT_FALSE(r.plan.imageScaleChanged);
}
