// Unit tests for src/calibration_sanity.cpp (#142): the advisory checks
// Scope::SanityCheckCalibration runs on a calibration that has just completed. Only the
// first issue found is reported, in the order steps, axis angles, axis rates, then a
// difference from the previous calibration. The advisory only alerts; it never rejects
// the calibration.

#include <gtest/gtest.h>

#include "calibration_sanity.h"

#include <cmath>

using namespace calibration_sanity;

namespace
{

const double DEC_COMP_LIMIT = M_PI / 2.0 * 2.0 / 3.0; // Scope::DEC_COMP_LIMIT

double Rad(double deg)
{
    return deg * M_PI / 180.;
}

// A clean calibration at Dec 0: enough steps, orthogonal axes, equal rates, equal guide speeds, no previous one
Input Clean()
{
    Input in;
    in.raStepCount = 12;
    in.decStepCount = 10;
    in.xAngle = Rad(30.);
    in.yAngle = Rad(30. - 90.);
    in.xRate = 0.004;
    in.yRate = 0.004;
    in.declination = 0.;
    in.raGuideSpeed = 0.002;
    in.decGuideSpeed = 0.002;
    in.imageScale = 1.93;
    in.decCompEnabled = true;
    in.decCompLimit = DEC_COMP_LIMIT;
    return in;
}

} // namespace

TEST(CalibrationSanity, CleanCalibrationPasses)
{
    EXPECT_EQ(Classify(Clean()).issue, Issue::None);
}

TEST(CalibrationSanity, TooFewSteps)
{
    Input in = Clean();
    in.raStepCount = 3;
    Result r = Classify(in);
    EXPECT_EQ(r.issue, Issue::Steps);
    EXPECT_EQ(r.raSteps, 3);
    EXPECT_EQ(r.decSteps, 10);

    in = Clean();
    in.decStepCount = 3;
    EXPECT_EQ(Classify(in).issue, Issue::Steps);

    // no Dec steps is Dec guiding off, not too few
    in.decStepCount = 0;
    EXPECT_EQ(Classify(in).issue, Issue::None);

    in.raStepCount = 4;
    in.decStepCount = 4;
    EXPECT_EQ(Classify(in).issue, Issue::None);
}

TEST(CalibrationSanity, StepsComeBeforeAngles)
{
    Input in = Clean();
    in.raStepCount = 2;
    in.yAngle = in.xAngle; // as bad as it gets
    EXPECT_EQ(Classify(in).issue, Issue::Steps);
}

TEST(CalibrationSanity, NonOrthogonalAxes)
{
    Input in = Clean();
    in.yAngle = in.xAngle - Rad(70.); // 20 deg off
    Result r = Classify(in);
    EXPECT_EQ(r.issue, Issue::Angle);
    EXPECT_NEAR(r.nonOrthoDeg, 20., 1e-9);

    in.yAngle = in.xAngle - Rad(78.); // 12 deg off: within 12.5
    EXPECT_EQ(Classify(in).issue, Issue::None);

    // a mirrored Dec axis (+90 instead of -90) is orthogonal
    in.yAngle = in.xAngle + Rad(90.);
    EXPECT_EQ(Classify(in).issue, Issue::None);
}

TEST(CalibrationSanity, RatesThatDoNotFollowCosDec)
{
    Input in = Clean();
    in.yRate = 0.002; // ratio 2, expected 1
    Result r = Classify(in);
    EXPECT_EQ(r.issue, Issue::Rates);
    EXPECT_NEAR(r.expectedRatio, 1., 1e-12);
    EXPECT_NEAR(r.actualRatio, 2., 1e-12);
    EXPECT_NEAR(r.declinationDeg, 0., 1e-12);

    // at Dec 60 the expected ratio is 0.5
    in = Clean();
    in.declination = Rad(60.);
    in.xRate = 0.002;
    EXPECT_EQ(Classify(in).issue, Issue::None);

    // Dec guide speed twice the RA's: the Dec rate doubles
    in = Clean();
    in.decGuideSpeed = 0.004;
    in.yRate = 0.008;
    EXPECT_EQ(Classify(in).issue, Issue::None);

    // no RA guide speed: speeds taken as equal
    in = Clean();
    in.raGuideSpeed = -1.;
    in.yRate = 0.002;
    EXPECT_EQ(Classify(in).issue, Issue::Rates);
}

TEST(CalibrationSanity, RatesAreNotCheckedWhenTheyCannotBeCompared)
{
    Input in = Clean();
    in.yRate = 0.002;

    Input unknownDec = in;
    unknownDec.declination = UNKNOWN_DEC;
    EXPECT_EQ(Classify(unknownDec).issue, Issue::None);

    Input high = in;
    high.declination = Rad(61.);
    EXPECT_EQ(Classify(high).issue, Issue::None);

    Input noDec = in;
    noDec.yRate = RATE_UNCALIBRATED;
    EXPECT_EQ(Classify(noDec).issue, Issue::None);

    Input noComp = in;
    noComp.decCompEnabled = false;
    EXPECT_EQ(Classify(noComp).issue, Issue::None);
}

TEST(CalibrationSanity, DecRateDifferentFromThePreviousCalibration)
{
    Input in = Clean();
    in.oldValid = true;
    in.oldXAngle = in.xAngle + Rad(2.);
    in.oldImageScale = in.imageScale + 0.05;
    in.oldYRate = in.yRate * 1.3;
    Result r = Classify(in);
    EXPECT_EQ(r.issue, Issue::Different);
    EXPECT_NEAR(r.decRateRatio, 1.3, 1e-12);

    // within 20%
    in.oldYRate = in.yRate * 1.15;
    EXPECT_EQ(Classify(in).issue, Issue::None);
}

TEST(CalibrationSanity, ADifferentSetupIsNotComparedWithThePreviousCalibration)
{
    Input in = Clean();
    in.oldValid = true;
    in.oldXAngle = in.xAngle;
    in.oldImageScale = in.imageScale;
    in.oldYRate = in.yRate * 2.;
    ASSERT_EQ(Classify(in).issue, Issue::Different);

    Input scale = in;
    scale.oldImageScale = in.imageScale + 0.2;
    EXPECT_EQ(Classify(scale).issue, Issue::None);

    Input angle = in;
    angle.oldXAngle = in.xAngle + Rad(6.);
    EXPECT_EQ(Classify(angle).issue, Issue::None);

    Input invalid = in;
    invalid.oldValid = false;
    EXPECT_EQ(Classify(invalid).issue, Issue::None);

    Input oldNoDec = in;
    oldNoDec.oldYRate = RATE_UNCALIBRATED;
    EXPECT_EQ(Classify(oldNoDec).issue, Issue::None);
}

TEST(CalibrationSanity, AnEarlierIssueWinsOverADifference)
{
    Input in = Clean();
    in.yRate = 0.002; // Rates
    in.oldValid = true;
    in.oldXAngle = in.xAngle;
    in.oldImageScale = in.imageScale;
    in.oldYRate = 0.004; // also Different
    EXPECT_EQ(Classify(in).issue, Issue::Rates);
}
