/*
 *  calstep_dialog.cpp
 *  PHD Guiding
 *
 *  Created by Bruce Waddington
 *  Copyright (c) 2013 Bruce Waddington
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Bret McKee, Dad Dog Development,
 *     Craig Stark, Stark Labs nor the names of its
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

#include "phd.h"
#include "calstep_dialog.h"
#include <cmath>

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
