/*
 *  image_scale.h
 *  PHD Guiding
 *
 *  Helpers for MyFrame::MakeImageScaleAdjustments and the settings that flag
 *  an image-scale change (focal length, binning, pixel size). Moved out of
 *  AdvancedDialog (#115 step 4, S4.1). Kept free of wx so it can be unit
 *  tested (tests/calibration/test_image_scale.cpp).
 */

#ifndef IMAGE_SCALE_H_INCLUDED
#define IMAGE_SCALE_H_INCLUDED

#include <cmath>

namespace image_scale
{
// Floating point equality comparisons can be a problem due to rounding and trivial inequalities.
// PercentChange can be used to see if a change is worth reacting to
static inline double PercentChange(double oldVal, double newVal)
{
    if (std::fabs(oldVal) < 0.0001)
    {
        return 100. * newVal; // not meaningful, but avoids divide by zero
    }
    else
        return 100. * std::fabs(1. - newVal / oldVal);
}

// A mount guide speed in degrees/sec as a multiple of the sidereal rate
static inline double SiderealRateFromGuideSpeed(double guideSpeed)
{
    double const siderealSecsPerSec = 0.9973;
    return guideSpeed * 3600.0 / (15.0 * siderealSecsPerSec);
}
} // namespace image_scale

#endif // IMAGE_SCALE_H_INCLUDED
