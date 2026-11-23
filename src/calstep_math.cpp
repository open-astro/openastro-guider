/*
 *  calstep_math.cpp
 *  PHD Guiding
 *
 *  See calstep_math.h. The arithmetic is CalstepDialog's, in the same order.
 */

#include "calstep_math.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace calstep_math
{
namespace
{
    constexpr double PI = 3.14159265358979323846;
}

double PixelScale(double pixelSizeMicrons, int focalLengthMm, int binning)
{
    if (focalLengthMm <= 0 || !(pixelSizeMicrons > 0.0) || binning <= 0)
        return 0.0;
    return 206.265 * pixelSizeMicrons * (double) binning / (double) focalLengthMm;
}

int CalibrationDistance(int focalLengthMm, double pixelSizeMicrons, int binning)
{
    double pixelScale = PixelScale(pixelSizeMicrons, focalLengthMm, binning);
    if (!(pixelScale > 0.0) || !std::isfinite(pixelScale))
        return DEFAULT_DISTANCE;
    double nominalDistanceArcsecs = 20.0;
    double MinDistancePx = DEFAULT_DISTANCE;
    return (int) ceil(std::max(MinDistancePx, nominalDistanceArcsecs / pixelScale));
}

bool CalibrationStepSize(int focalLengthMm, double pixelSizeMicrons, int binning, double guideSpeed, int desiredSteps,
                         double declination, int distance, double *imageScale, int *stepMs)
{
    double ImageScale = PixelScale(pixelSizeMicrons, focalLengthMm, binning); // arc-sec per pixel
    if (!(ImageScale > 0.0) || !std::isfinite(ImageScale) || !(guideSpeed > 0.0) || desiredSteps <= 0 || distance <= 0)
        return false;
    double totalDuration = (double) distance * ImageScale / (15.0 * guideSpeed); // 15 arc-sec/sec is sidereal rate
    double Pulse = totalDuration / desiredSteps * 1000.0; // milliseconds at DEC=0
    double MaxPulse = totalDuration / MIN_STEPS * 1000.0; // max pulse size to still get MIN steps
    double decPulse = Pulse / cos(declination * PI / 180.0);
    Pulse = MaxPulse < decPulse ? MaxPulse : decPulse; // wxMin; UI forces abs(Dec) <= 60 degrees
    double units = ceil(Pulse / 50.0); // round up to nearest 50 ms, too-small pulses can lead to calibration problems
    if (!(units > 0.0) || !(units <= INT_MAX / 50))
        return false;
    if (imageScale)
        *imageScale = ImageScale;
    *stepMs = (int) units * 50;
    return true;
}
} // namespace calstep_math
