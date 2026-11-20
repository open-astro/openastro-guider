/*
 *  edge_allowance.h
 *  PHD Guiding
 *
 *  How far beyond the search region auto-select keeps a star from the frame
 *  edge (#140): far enough for the calibration walk when the mount or AO will
 *  calibrate after the selection. Kept free of wx so it can be unit tested
 *  (tests/star/test_edge_allowance.cpp).
 */

#ifndef EDGE_ALLOWANCE_H_INCLUDED
#define EDGE_ALLOWANCE_H_INCLUDED

// What the edge allowance needs to know about the mount or the AO; an absent one is not connected
struct CalibrationGear
{
    bool connected = false;
    bool calibrated = false;
    int calTotDist = 0; // Mount::CalibrationTotDistance
};

// calTotDist (Mount::CalibrationTotDistance) when the mount will calibrate after the selection, else 0.
// It will when it is not calibrated, or when the guide request that selects the star will clear its
// calibration first (guide with recalibrate: true, whose STATE_CALIBRATE runs after STATE_SELECT_STAR).
int EdgeAllowance(bool mountCalibrated, bool willForceRecal, int calTotDist);

// GuiderMultiStar::AutoSelect's allowance: the largest EdgeAllowance over the connected mount and AO.
int EdgeAllowance(const CalibrationGear& mount, const CalibrationGear& ao, bool willForceRecal);

// GuideStar::AutoFind's edge rule: whether a star at (x, y) is more than edgeDist (the search region plus
// the allowance) inside a width x height frame.
bool ClearOfEdge(double x, double y, int width, int height, int edgeDist);

// Whether a star already selected (find_star while looping, which on a calibrated mount keeps only the search
// region) can stay selected for guide with recalibrate: true: AutoFind's edge rule with the allowance of a
// forced recalibration. If not, the guide request selects again (Guider::SelectedStarClearForRecalibration).
bool StarClearForRecalibration(double x, double y, int width, int height, int searchRegion, const CalibrationGear& mount,
                               const CalibrationGear& ao);

#endif
