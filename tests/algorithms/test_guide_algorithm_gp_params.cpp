// Gaussian Process set_algo_param "periodLength" (#138).
//
// GuideAlgorithmGaussianProcess::SetParam("periodLength") built a
// zero-filled hyperparameter vector with only the period set: the three
// signal variances were accepted as 0 and saved to the profile, which turns
// the PPEC prediction off for good, and the three length scales went back to
// their defaults. It now writes back the current hyperparameters with only
// the period changed (GPHyperparametersWithPeriodLength); these tests run
// that through the real GaussianProcessGuider the algorithm keeps them in.

#include <gtest/gtest.h>

#include <vector>

#include "gaussian_process_guider.h"
#include "guide_algorithm_gp_params.h"

namespace
{

// distinct values, none a default, so a reset to a default shows too
const double kSE0KLengthScale = 900.0;
const double kSE0KSignalVariance = 7.5;
const double kPKLengthScale = 12.0;
const double kPKSignalVariance = 3.25;
const double kSE1KLengthScale = 40.0;
const double kSE1KSignalVariance = 2.5;
const double kPKPeriodLength = 480.0;

GaussianProcessGuider::guide_parameters Parameters()
{
    GaussianProcessGuider::guide_parameters p;
    p.control_gain_ = 0.6;
    p.min_move_ = 0.2;
    p.prediction_gain_ = 0.5;
    p.min_periods_for_inference_ = 2.0;
    p.min_periods_for_period_estimation_ = 2.0;
    p.points_for_approximation_ = 100;
    p.compute_period_ = true;
    p.SE0KLengthScale_ = kSE0KLengthScale;
    p.SE0KSignalVariance_ = kSE0KSignalVariance;
    p.PKLengthScale_ = kPKLengthScale;
    p.PKSignalVariance_ = kPKSignalVariance;
    p.SE1KLengthScale_ = kSE1KLengthScale;
    p.SE1KSignalVariance_ = kSE1KSignalVariance;
    p.PKPeriodLength_ = kPKPeriodLength;
    return p;
}

} // namespace

TEST(GPParams, OnlyThePeriodChanges)
{
    const std::vector<double> current = { 1., 2., 3., 4., 5., 6., 7. };
    ASSERT_EQ(current.size(), (size_t) NumParameters);

    const std::vector<double> next = GPHyperparametersWithPeriodLength(current, 600.);

    ASSERT_EQ(next.size(), current.size());
    for (int i = 0; i < NumParameters; i++)
    {
        if (i == PKPeriodLength)
            EXPECT_EQ(next[i], 600.);
        else
            EXPECT_EQ(next[i], current[i]) << "hyperparameter " << i;
    }
}

TEST(GPParams, AVectorWithoutAPeriodIsReturnedUnchanged)
{
    EXPECT_TRUE(GPHyperparametersWithPeriodLength({}, 600.).empty());
    const std::vector<double> shortVec = { 1., 2. };
    EXPECT_EQ(GPHyperparametersWithPeriodLength(shortVec, 600.), shortVec);
}

TEST(GPParams, IssueReproPeriodLengthKeepsTheOtherHyperparametersOfTheGuider)
{
    // SetParam: SetGPHyperparameters(GPHyperparametersWithPeriodLength(GetGPHyperparameters(), val))
    GaussianProcessGuider gpg(Parameters());
    const std::vector<double> before = gpg.GetGPHyperparameters();
    ASSERT_EQ(before.size(), (size_t) NumParameters);
    EXPECT_NEAR(before[PKPeriodLength], kPKPeriodLength, 1e-6);

    gpg.SetGPHyperparameters(GPHyperparametersWithPeriodLength(gpg.GetGPHyperparameters(), 600.));

    const std::vector<double> after = gpg.GetGPHyperparameters();
    ASSERT_EQ(after.size(), (size_t) NumParameters);
    EXPECT_NEAR(after[PKPeriodLength], 600., 1e-6);
    EXPECT_NEAR(after[SE0KLengthScale], kSE0KLengthScale, 1e-6);
    EXPECT_NEAR(after[SE0KSignalVariance], kSE0KSignalVariance, 1e-9);
    EXPECT_NEAR(after[PKLengthScale], kPKLengthScale, 1e-6);
    EXPECT_NEAR(after[PKSignalVariance], kPKSignalVariance, 1e-9);
    EXPECT_NEAR(after[SE1KLengthScale], kSE1KLengthScale, 1e-6);
    EXPECT_NEAR(after[SE1KSignalVariance], kSE1KSignalVariance, 1e-9);
}

TEST(GPParams, RepeatedPeriodChangesDoNotDrift)
{
    GaussianProcessGuider gpg(Parameters());
    const double periods[] = { 600., 120., 1200., kPKPeriodLength };
    for (double period : periods)
        gpg.SetGPHyperparameters(GPHyperparametersWithPeriodLength(gpg.GetGPHyperparameters(), period));

    const std::vector<double> after = gpg.GetGPHyperparameters();
    EXPECT_NEAR(after[PKPeriodLength], kPKPeriodLength, 1e-6);
    EXPECT_NEAR(after[SE0KSignalVariance], kSE0KSignalVariance, 1e-9);
    EXPECT_NEAR(after[PKSignalVariance], kPKSignalVariance, 1e-9);
    EXPECT_NEAR(after[SE1KSignalVariance], kSE1KSignalVariance, 1e-9);
    EXPECT_NEAR(after[PKLengthScale], kPKLengthScale, 1e-6);
}
