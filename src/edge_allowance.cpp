/*
 *  edge_allowance.cpp
 *  PHD Guiding
 *
 *  See edge_allowance.h.
 */

#include "edge_allowance.h"

int EdgeAllowance(bool mountCalibrated, bool willForceRecal, int calTotDist)
{
    return !mountCalibrated || willForceRecal ? calTotDist : 0;
}
