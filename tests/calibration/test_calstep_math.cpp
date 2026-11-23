// calstep_math (#142).
//
// The recommended calibration distance and step that Scope::CheckCalibrationDuration (on a
// binning or guide-speed change) and MyFrame::MakeImageScaleAdjustments (on an image-scale
// change) set from the focal length, pixel size, binning and mount guide speed. The math lived in
// CalstepDialog and moved to src/calstep_math.cpp; the differential tests run it against the
// frozen copy of the CalstepDialog lines (legacy/calstep_legacy.cpp) and require the same
// results, doubles bit for bit, wherever the copy is defined.
//
// The copy is not defined for an unknown image scale or a guide speed of 0: a focal length of 0
// (the profile default, and accepted by set_profile_setup) gave an infinite image scale, and
// (int) ceil(inf / 50) * 50 is undefined behaviour; a pixel size of 0 did the same to the
// distance. calstep_math answers DEFAULT_DISTANCE and "no step" for them instead.

#include <gtest/gtest.h>

#include "calstep_math.h"
#include "legacy/calstep_legacy.h"

#include <cmath>
#include <cstring>
#include <limits>

using calstep_math::CalibrationDistance;
using calstep_math::CalibrationStepSize;
using calstep_math::PixelScale;

namespace
{
// The step, or -1 when there is none
int Step(int fl, double px, int bin, double speed, double dec = 0.0, int distance = -1, int steps = calstep_math::DEFAULT_STEPS)
{
    if (distance < 0)
        distance = CalibrationDistance(fl, px, bin);
    int ms = -1;
    if (!CalibrationStepSize(fl, px, bin, speed, steps, dec, distance, nullptr, &ms))
        return -1;
    return ms;
}

bool SameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}
} // namespace

// --- golden values ---

TEST(CalstepMath, PixelScaleOfA400mmGuideScopeAt375Microns)
{
    EXPECT_NEAR(PixelScale(3.75, 400, 1), 1.9337, 1e-4);
    EXPECT_NEAR(PixelScale(3.75, 400, 2), 2 * 1.9337, 2e-4);
}

TEST(CalstepMath, DistanceIs20ArcsecondsOr25PixelsWhicheverIsGreater)
{
    EXPECT_EQ(CalibrationDistance(400, 3.75, 1), 25); // 20 / 1.934 = 10.3 px
    EXPECT_EQ(CalibrationDistance(400, 3.75, 2), 25); // binning only shrinks it further
    EXPECT_EQ(CalibrationDistance(2000, 3.75, 1), 52); // 20 / 0.387 = 51.7 px, rounded up
    EXPECT_EQ(CalibrationDistance(1000, 3.75, 1), 26); // 20 / 0.774 = 25.9 px
}

TEST(CalstepMath, StepAtHalfSiderealOnTheEquator)
{
    // 25 px x 1.934"/px / (15"/s x 0.5) = 6.446 s, over 12 steps = 537 ms, rounded up to 550
    double scale = 0.0;
    int ms = 0;
    ASSERT_TRUE(CalibrationStepSize(400, 3.75, 1, 0.5, 12, 0.0, 25, &scale, &ms));
    EXPECT_EQ(ms, 550);
    EXPECT_NEAR(scale, 1.9337, 1e-4);
}

TEST(CalstepMath, StepGrowsWithDeclinationUpToTheSixStepCap)
{
    EXPECT_EQ(Step(400, 3.75, 1, 0.5, 30.0), 650); // 537 / cos 30 = 620
    // 537 / cos 60 = 1074, which is also the cap (6.446 s / 6 steps), rounded up to 1100
    EXPECT_EQ(Step(400, 3.75, 1, 0.5, 60.0), 1100);
    EXPECT_EQ(Step(400, 3.75, 1, 0.5, 80.0), 1100);
    EXPECT_EQ(Step(400, 3.75, 1, 0.5, -60.0), 1100);
}

TEST(CalstepMath, StepScalesWithBinningAndGuideSpeed)
{
    // binning 2 doubles the image scale at the same 25 px distance: 1074 ms
    EXPECT_EQ(Step(400, 3.75, 2, 0.5), 1100);
    // a 1x guide speed halves the step: 269 ms
    EXPECT_EQ(Step(400, 3.75, 1, 1.0), 300);
    // a long focal length: 52 px x 0.387"/px / 7.5"/s / 12 = 223 ms
    EXPECT_EQ(Step(2000, 3.75, 1, 0.5), 250);
}

TEST(CalstepMath, StepIsAlwaysAPositiveMultipleOf50)
{
    for (int fl : { 50, 135, 240, 400, 910, 2000, 4000 })
        for (double px : { 2.4, 2.9, 3.75, 5.2, 9.0 })
            for (int bin : { 1, 2, 3, 4 })
                for (double speed : { 0.1, 0.25, 0.5, 0.9, 1.0 })
                    for (double dec : { -60.0, 0.0, 45.0, 60.0 })
                    {
                        int ms = Step(fl, px, bin, speed, dec);
                        EXPECT_GT(ms, 0) << fl << " " << px << " " << bin << " " << speed << " " << dec;
                        EXPECT_EQ(ms % 50, 0) << fl << " " << px << " " << bin << " " << speed << " " << dec;
                    }
}

// --- the same results as the frozen CalstepDialog copy ---

TEST(CalstepMath, MatchesTheFrozenCopyWhereverItIsDefined)
{
    int compared = 0;
    for (int fl : { 1, 50, 135, 240, 400, 910, 2000, 4000, 50000 })
        for (double px : { 0.5, 2.4, 2.9, 3.75, 5.2, 9.0, 24.0 })
            for (int bin : { 1, 2, 3, 4 })
            {
                int legacyDistance = calstep_legacy::CalstepDialog::GetCalibrationDistance(fl, px, bin);
                ASSERT_EQ(CalibrationDistance(fl, px, bin), legacyDistance) << fl << " " << px << " " << bin;
                for (double speed : { 0.05, 0.1, 0.25, 0.5, 0.9, 1.0, 2.0 })
                    for (double dec : { -89.0, -60.0, -1.0, 0.0, 0.5, 30.0, 45.0, 60.0, 75.0, 89.0, 90.0 })
                        for (int steps : { 1, 6, 12, 20 })
                            for (int distance : { 1, 25, legacyDistance, 80 })
                            {
                                double legacyScale = -1.0, scale = -2.0;
                                int legacyMs = -1, ms = -2;
                                calstep_legacy::CalstepDialog::GetCalibrationStepSize(fl, px, bin, speed, steps, dec, distance,
                                                                                      &legacyScale, &legacyMs);
                                ASSERT_TRUE(CalibrationStepSize(fl, px, bin, speed, steps, dec, distance, &scale, &ms));
                                ASSERT_EQ(ms, legacyMs) << fl << " " << px << " " << bin << " " << speed << " " << dec << " "
                                                        << steps << " " << distance;
                                ASSERT_TRUE(SameBits(scale, legacyScale));
                                ++compared;
                            }
            }
    EXPECT_EQ(compared, 9 * 7 * 4 * 7 * 11 * 4 * 4);
}

TEST(CalstepMath, ANullImageScaleIsNotWritten)
{
    int ms = 0;
    EXPECT_TRUE(CalibrationStepSize(400, 3.75, 1, 0.5, 12, 0.0, 25, nullptr, &ms));
    EXPECT_EQ(ms, 550);
}

// --- no undefined step for an unknown image scale or guide speed ---

TEST(CalstepMath, FocalLengthZeroGivesTheDefaultDistanceAndNoStep)
{
    // the profile default: the scale is unknown, so the step is left as it is
    EXPECT_EQ(CalibrationDistance(0, 3.75, 1), calstep_math::DEFAULT_DISTANCE);
    double scale = 7.0;
    int ms = 750;
    EXPECT_FALSE(CalibrationStepSize(0, 3.75, 1, 0.5, 12, 0.0, 25, &scale, &ms));
    EXPECT_EQ(ms, 750);
    EXPECT_EQ(scale, 7.0);
    EXPECT_EQ(PixelScale(3.75, 0, 1), 0.0);
    EXPECT_EQ(Step(-400, 3.75, 1, 0.5), -1);
}

TEST(CalstepMath, PixelSizeOrBinningZeroGivesTheDefaultDistanceAndNoStep)
{
    EXPECT_EQ(CalibrationDistance(400, 0.0, 1), calstep_math::DEFAULT_DISTANCE);
    EXPECT_EQ(CalibrationDistance(400, 3.75, 0), calstep_math::DEFAULT_DISTANCE);
    EXPECT_EQ(CalibrationDistance(400, std::numeric_limits<double>::quiet_NaN(), 1), calstep_math::DEFAULT_DISTANCE);
    EXPECT_EQ(Step(400, 0.0, 1, 0.5, 0.0, 25), -1);
    EXPECT_EQ(Step(400, 3.75, 0, 0.5, 0.0, 25), -1);
    EXPECT_EQ(Step(400, std::numeric_limits<double>::quiet_NaN(), 1, 0.5, 0.0, 25), -1);
}

TEST(CalstepMath, GuideSpeedZeroNegativeOrNaNGivesNoStep)
{
    EXPECT_EQ(Step(400, 3.75, 1, 0.0), -1);
    EXPECT_EQ(Step(400, 3.75, 1, -0.5), -1);
    EXPECT_EQ(Step(400, 3.75, 1, std::numeric_limits<double>::quiet_NaN()), -1);
}

TEST(CalstepMath, NoStepForNoStepsOrNoDistance)
{
    EXPECT_EQ(Step(400, 3.75, 1, 0.5, 0.0, 25, 0), -1);
    EXPECT_EQ(Step(400, 3.75, 1, 0.5, 0.0, 0), -1);
}

TEST(CalstepMath, NoStepAnIntCannotHold)
{
    // a guide speed reported as 1e-9 x sidereal: about 5e11 ms
    EXPECT_EQ(Step(400, 3.75, 1, 1e-9), -1);
    // a declination past the pole turns the pulse negative
    EXPECT_EQ(Step(400, 3.75, 1, 0.5, 120.0), -1);
}
