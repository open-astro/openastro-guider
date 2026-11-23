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
#include "calstep_math.h"

static_assert(CalstepDialog::DEFAULT_STEPS == calstep_math::DEFAULT_STEPS, "calstep_math default steps");
static_assert(CalstepDialog::DEFAULT_DISTANCE == calstep_math::DEFAULT_DISTANCE, "calstep_math default distance");

// the recommended calibration distance is 20 arc-seconds or 25 pixels, whichever is greater
// (25 pixels when the image scale is unknown). The math is in calstep_math.cpp (#142).
int CalstepDialog::GetCalibrationDistance(int focalLength, double pixelSize, int binning)
{
    return calstep_math::CalibrationDistance(focalLength, pixelSize, binning);
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
//  Returns false, leaving *pImageScale and *pStepSize unchanged, when no step can be computed: the
//  image scale is unknown (focal length 0, the profile default), or the guide speed is not above 0.
//  The math is in calstep_math.cpp (#142).
//
bool CalstepDialog::GetCalibrationStepSize(int FocalLength, double PixelSize, int binning, double GuideSpeed, int DesiredSteps,
                                           double Declination, int distance, double *pImageScale, int *pStepSize)
{
    return calstep_math::CalibrationStepSize(FocalLength, PixelSize, binning, GuideSpeed, DesiredSteps, Declination, distance,
                                             pImageScale, pStepSize);
}
