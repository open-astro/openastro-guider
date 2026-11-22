// The frozen twin of CalstepDialog's calibration distance and step math (#142):
// calstep_legacy.cpp holds a verbatim LF copy of src/calstep_dialog.cpp lines
// 40-77 at 7755af25, before the math moved to calstep_math.cpp
// (check_twin.sh diffs it), compiled against stand-ins for the wx and app names
// it uses (MyFrame::GetPixelScale copied from myframe.h, wxMin, radians from
// image_math.h). Everything is in namespace calstep_legacy.

#ifndef CALSTEP_LEGACY_H
#define CALSTEP_LEGACY_H

namespace calstep_legacy
{
class CalstepDialog
{
public:
    enum
    {
        DEFAULT_STEPS = 12,
        DEFAULT_DISTANCE = 25
    };

    static int GetCalibrationDistance(int focalLength, double pixelSize, int binning);
    static void GetCalibrationStepSize(int focalLength, double pixelSize, int binning, double guideSpeed, int desiredSteps,
                                       double declination, int distance, double *imageScale, int *stepSize);
};
} // namespace calstep_legacy

#endif
