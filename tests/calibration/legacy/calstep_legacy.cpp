// See calstep_legacy.h. Only the lines between BEGIN COPY and END COPY are the
// copy; check_twin.sh diffs them against src/calstep_dialog.cpp.

#include "calstep_legacy.h"

#include <algorithm>
#include <cmath>

namespace calstep_legacy
{
namespace
{
    // myframe.h MyFrame::GetPixelScale
    struct MyFrame
    {
        static double GetPixelScale(double pixelSizeMicrons, int focalLengthMm, int binning)
        {
            return 206.265 * pixelSizeMicrons * (double) binning / (double) focalLengthMm;
        }
    };

    // wx/defs.h
    template<typename T1, typename T2>
    inline auto wxMin(T1 a, T2 b)
    {
        return (a < b) ? a : b;
    }

    // image_math.h
    inline double radians(double degrees)
    {
        return degrees * M_PI / 180.;
    }
} // namespace

// BEGIN COPY
#define MIN_STEPS 6.0

// the recommended calibration distance is 20 arc-seconds or 25 pixels, whichever is greater
// (25 pixels when the image scale is unknown)
int CalstepDialog::GetCalibrationDistance(int focalLength, double pixelSize, int binning)
{
    double pixelScale = MyFrame::GetPixelScale(pixelSize, focalLength, binning);
    double nominalDistanceArcsecs = 20.0;
    double MinDistancePx = CalstepDialog::DEFAULT_DISTANCE;
    return (int) ceil(std::max(MinDistancePx, nominalDistanceArcsecs / pixelScale));
}

// Based on computed image scale, compute an RA calibration pulse direction that will result in DesiredSteps
//  for a "travel" distance of <distance> pixels in each direction, adjusted for declination.
//  Result will be rounded up to the nearest 50 ms and is constrained to insure Dec calibration is at least
//  MIN_STEPs
//
//  FocalLength = focal length in millimeters
//  PixelSize = pixel size in microns (un-binned)
//  Binning = pixel binning factor (combined hardware and software binning)
//  GuideSpeed = guide rate as fraction of sidereal rate
//  DesiredSteps = desired number of calibration steps
//  Declination = declination in degrees
//  pImageScale = address of computed image scale (arc-sec/pixel)
//  pStepSize = address of computed calibration step size, milliseconds
//
void CalstepDialog::GetCalibrationStepSize(int FocalLength, double PixelSize, int binning, double GuideSpeed, int DesiredSteps,
                                           double Declination, int distance, double *pImageScale, int *pStepSize)
{
    double ImageScale = MyFrame::GetPixelScale(PixelSize, FocalLength, binning); // arc-sec per pixel
    double totalDuration = (double) distance * ImageScale / (15.0 * GuideSpeed); // 15 arc-sec/sec is sidereal rate
    double Pulse = totalDuration / DesiredSteps * 1000.0; // milliseconds at DEC=0
    double MaxPulse = totalDuration / MIN_STEPS * 1000.0; // max pulse size to still get MIN steps
    Pulse = wxMin(MaxPulse, Pulse / cos(radians(Declination))); // UI forces abs(Dec) <= 60 degrees
    if (pImageScale)
        *pImageScale = ImageScale;
    *pStepSize = (int) ceil(Pulse / 50.0) * 50; // round up to nearest 50 ms, too-small pulses can lead to calibration problems
}
// END COPY
} // namespace calstep_legacy
