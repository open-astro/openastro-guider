/*
 *  guide_algorithm_gp_params.h
 *  PHD Guiding
 *
 *  The Gaussian Process hyperparameters that set_algo_param "periodLength"
 *  writes (#138). GuideAlgorithmGaussianProcess::SetParam used to build a
 *  zero-filled vector with only the period set, so the three signal
 *  variances went to 0 (accepted, and saved to the profile, which disables
 *  the PPEC prediction) and the three length scales back to their defaults,
 *  and the RPC still answered "could not set param". Kept free of wx so it
 *  can be unit tested (tests/algorithms/test_guide_algorithm_gp_params.cpp).
 */

#ifndef GUIDE_ALGORITHM_GP_PARAMS_H_INCLUDED
#define GUIDE_ALGORITHM_GP_PARAMS_H_INCLUDED

#include <vector>

// current is GetGPHyperparameters(), indexed by the Hyperparameters enum of
// gaussian_process_guider.h. Returns current with only PKPeriodLength
// replaced by period_length; a vector too short to hold the period is
// returned unchanged (SetGPHyperparameters rejects a vector of the wrong size).
std::vector<double> GPHyperparametersWithPeriodLength(std::vector<double> current, double period_length);

#endif
