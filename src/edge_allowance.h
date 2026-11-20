/*
 *  edge_allowance.h
 *  PHD Guiding
 *
 *  How far beyond the search region auto-select keeps a star from the frame
 *  edge for one mount or AO (#140): far enough for the calibration walk when
 *  the mount will calibrate after the selection. Kept free of wx so it can be
 *  unit tested (tests/star/test_edge_allowance.cpp).
 */

#ifndef EDGE_ALLOWANCE_H_INCLUDED
#define EDGE_ALLOWANCE_H_INCLUDED

// calTotDist (Mount::CalibrationTotDistance) when the mount will calibrate after the selection, else 0.
// It will when it is not calibrated, or when the guide request that selects the star will clear its
// calibration first (guide with recalibrate: true, whose STATE_CALIBRATE runs after STATE_SELECT_STAR).
// AutoSelect takes the largest over the mount and the AO.
int EdgeAllowance(bool mountCalibrated, bool willForceRecal, int calTotDist);

#endif
