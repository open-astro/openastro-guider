/*
 *  guide_algorithm_gp_params.cpp
 *  PHD Guiding
 *
 *  See guide_algorithm_gp_params.h.
 */

#include "guide_algorithm_gp_params.h"

#include "gaussian_process_guider.h"

std::vector<double> GPHyperparametersWithPeriodLength(std::vector<double> current, double period_length)
{
    if (current.size() > PKPeriodLength)
        current[PKPeriodLength] = period_length;
    return current;
}
