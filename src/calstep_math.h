/*
 *  calstep_math.h
 *  PHD Guiding
 *
 *  The recommended calibration distance and calibration step (#142), from
 *  the focal length, pixel size, binning and mount guide speed. Used by
 *  CalstepDialog::GetCalibrationDistance / GetCalibrationStepSize, which
 *  Scope::CheckCalibrationDuration and AdvancedDialog::MakeImageScaleAdjustments
 *  call. Kept free of wx so it can be unit tested
 *  (tests/calibration/test_calstep_math.cpp).
 */

#ifndef CALSTEP_MATH_H_INCLUDED
#define CALSTEP_MATH_H_INCLUDED

namespace calstep_math
{
constexpr int DEFAULT_STEPS = 12; // calibration steps wanted on each leg
constexpr int DEFAULT_DISTANCE = 25; // px, the least recommended calibration distance
constexpr double MIN_STEPS = 6.0; // the step is capped so a leg still takes at least this many

// arc-seconds per pixel, as MyFrame::GetPixelScale; 0 when the scale is unknown
// (focal length, pixel size or binning not above 0)
double PixelScale(double pixelSizeMicrons, int focalLengthMm, int binning);

// 20 arc-seconds or DEFAULT_DISTANCE pixels, whichever is greater, in pixels,
// rounded up; DEFAULT_DISTANCE when the image scale is unknown
int CalibrationDistance(int focalLengthMm, double pixelSizeMicrons, int binning);

// The RA calibration pulse, in ms, that moves the star <distance> px in
// <desiredSteps> steps at <guideSpeed> x sidereal, longer by 1/cos(declination)
// but never so long that a leg takes fewer than MIN_STEPS steps, rounded up to
// a multiple of 50 ms. Returns false and leaves *stepMs and *imageScale unchanged
// when no step can be computed: the image scale is unknown (a focal length of 0
// is the profile default), the guide speed, step count or distance is not above
// 0, or the pulse is not a positive number of ms an int holds.
bool CalibrationStepSize(int focalLengthMm, double pixelSizeMicrons, int binning, double guideSpeed, int desiredSteps,
                         double declination, int distance, double *imageScale, int *stepMs);
} // namespace calstep_math

#endif
