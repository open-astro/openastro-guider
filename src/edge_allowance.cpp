/*
 *  edge_allowance.cpp
 *  PHD Guiding
 *
 *  See edge_allowance.h.
 */

#include "edge_allowance.h"

#include <algorithm>

int EdgeAllowance(bool mountCalibrated, bool willForceRecal, int calTotDist)
{
    return !mountCalibrated || willForceRecal ? calTotDist : 0;
}

static int GearAllowance(const CalibrationGear& gear, bool willForceRecal)
{
    return gear.connected ? EdgeAllowance(gear.calibrated, willForceRecal, gear.calTotDist) : 0;
}

int EdgeAllowance(const CalibrationGear& mount, const CalibrationGear& ao, bool willForceRecal)
{
    return std::max(GearAllowance(mount, willForceRecal), GearAllowance(ao, willForceRecal));
}

bool ClearOfEdge(double x, double y, int width, int height, int edgeDist)
{
    return x > edgeDist && x < width - edgeDist && y > edgeDist && y < height - edgeDist;
}

bool StarClearForRecalibration(double x, double y, int width, int height, int searchRegion, const CalibrationGear& mount,
                               const CalibrationGear& ao)
{
    return ClearOfEdge(x, y, width, height, searchRegion + EdgeAllowance(mount, ao, true));
}
