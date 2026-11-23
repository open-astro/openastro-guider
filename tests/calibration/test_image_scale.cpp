// image_scale (#115 step 4, S4.1).
//
// The helpers MyFrame::MakeImageScaleAdjustments and the focal length / pixel size settings use,
// moved out of AdvancedDialog. PercentChange decides whether a focal length or pixel size change
// is over the 5% that flags an image-scale change; SiderealRateFromGuideSpeed turns a mount guide
// rate (degrees/sec) into a multiple of sidereal for DetermineGuideSpeed.

#include <gtest/gtest.h>

#include "image_scale.h"

using image_scale::PercentChange;
using image_scale::SiderealRateFromGuideSpeed;

TEST(PercentChange, IsTheAbsoluteRelativeChange)
{
    EXPECT_DOUBLE_EQ(PercentChange(400., 400.), 0.);
    EXPECT_DOUBLE_EQ(PercentChange(400., 500.), 25.);
    EXPECT_DOUBLE_EQ(PercentChange(400., 300.), 25.);
    EXPECT_DOUBLE_EQ(PercentChange(-2., -3.), 50.);
}

TEST(PercentChange, FivePercentThreshold)
{
    // The callers flag an image-scale change only above 5%
    EXPECT_FALSE(PercentChange(400., 419.) > 5.0);
    EXPECT_TRUE(PercentChange(400., 421.) > 5.0);
    EXPECT_FALSE(PercentChange(3.75, 3.8) > 5.0);
    EXPECT_TRUE(PercentChange(3.75, 2.9) > 5.0);
}

TEST(PercentChange, OldValueNearZeroIsOneHundredTimesTheNewValue)
{
    // Not meaningful, but avoids dividing by zero: a focal length set from 0 always flags
    EXPECT_DOUBLE_EQ(PercentChange(0., 400.), 40000.);
    EXPECT_DOUBLE_EQ(PercentChange(0.00009, 0.), 0.);
    EXPECT_DOUBLE_EQ(PercentChange(-0.00009, -1.), -100.);
    // At the 0.0001 boundary the relative change is used
    EXPECT_DOUBLE_EQ(PercentChange(0.0001, 0.0002), 100.);
}

TEST(SiderealRateFromGuideSpeed, MultipleOfSidereal)
{
    // 15 arc-sec per sidereal second, 0.9973 sidereal seconds per second
    double const sidereal = 15.0 * 0.9973 / 3600.0;
    EXPECT_DOUBLE_EQ(SiderealRateFromGuideSpeed(sidereal), 1.0);
    EXPECT_DOUBLE_EQ(SiderealRateFromGuideSpeed(sidereal / 2), 0.5);
    EXPECT_DOUBLE_EQ(SiderealRateFromGuideSpeed(0.), 0.);
}
