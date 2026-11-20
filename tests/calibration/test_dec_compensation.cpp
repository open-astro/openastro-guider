// Unit tests for the Dec compensation decision in src/calibration_transform.cpp,
// extracted from Mount::AdjustCalibrationForScopePointing (#157). Every entry to
// Guiding asks it what to do with the RA rate for the Dec the scope now points at:
// nothing, scale the RA rate by cos(Dec now) / cos(calibration Dec), or, for a
// calibration made above the 60 deg Dec comp limit, raise the "Calibration was too
// far from equator" alert.
//
// The guider recalibrates on every new target, so the usual case is a calibration
// that has just completed and a Dec reading that has moved by a driver's rounding
// or the calibration's residual. That must not alert, and below the limit it
// changes the RA rate by so little that it is left alone. A real move must still
// compensate below the limit and alert above it.

#include <gtest/gtest.h>

#include "calibration_transform.h"

#include <cmath>
#include <limits>
#include <optional>

using namespace calibration_transform;

namespace
{

// Scope::DEC_COMP_LIMIT, spelled as scope.cpp spells it
const double DEC_COMP_LIMIT = M_PI / 2.0 * 2.0 / 3.0;

double Rad(double deg)
{
    return deg * M_PI / 180.;
}

double Deg(double rad)
{
    return rad * 180. / M_PI;
}

DecComp Decide(double calDeg, double newDeg)
{
    return DecideDecComp(Rad(calDeg), Rad(newDeg), DEC_COMP_LIMIT);
}

// The Dec, on the same side of the pole and equator as calDeg, at which the RA rate is
// ratio times the rate at calDeg.
double DecForRateRatio(double calDeg, double ratio)
{
    double d = Deg(std::acos(std::cos(Rad(calDeg)) * ratio));
    return calDeg < 0 ? -d : d;
}

const char *Name(DecComp d)
{
    switch (d)
    {
    case DecComp::None:
        return "None";
    case DecComp::Compensate:
        return "Compensate";
    case DecComp::Alert:
        return "Alert";
    }
    return "?";
}

} // namespace

TEST(DecComp, FreshHighDecCalibrationIgnoresDecJitter)
{
    // The issue's case: a calibration at Dec 80 that has just completed, and a
    // reported Dec that has wobbled by 0.001 deg (3.6") by the time guiding starts.
    EXPECT_STREQ(Name(Decide(80., 80.001)), "None");
    EXPECT_STREQ(Name(Decide(80., 79.999)), "None");

    // Anywhere above the limit, north or south, a change under the 0.02 deg floor
    for (double cal : { 61., 70., 80., 85., 87., -70., -80. })
        for (double d : { 0.001, -0.001, 0.019, -0.019 })
            EXPECT_STREQ(Name(Decide(cal, cal + d)), "None") << "cal " << cal << " moved " << d;
}

TEST(DecComp, RealMoveFromHighDecCalibrationAlerts)
{
    // The issue's other case: a calibration at 70 used at 75 has the wrong RA rate.
    EXPECT_STREQ(Name(Decide(70., 75.)), "Alert");
    EXPECT_STREQ(Name(Decide(75., 70.)), "Alert");
    EXPECT_STREQ(Name(Decide(-70., -75.)), "Alert");
    EXPECT_STREQ(Name(Decide(80., 79.)), "Alert"); // RA rate +10%
    EXPECT_STREQ(Name(Decide(80., 40.)), "Alert");
    EXPECT_STREQ(Name(Decide(70., 0.)), "Alert");
}

TEST(DecComp, HighDecAlertsOnlyWhenTheRaRateChangesOverFivePercent)
{
    // Above the limit the calibration's RA rate is used as measured; a move that
    // changes it by 4% keeps it, one that changes it by 6% asks for a calibration.
    // Toward the pole (rate falls) and toward the equator (rate rises).
    for (double cal : { 65., 70., 80., 85., -80. })
    {
        EXPECT_STREQ(Name(Decide(cal, DecForRateRatio(cal, 0.96))), "None") << "cal " << cal;
        EXPECT_STREQ(Name(Decide(cal, DecForRateRatio(cal, 0.94))), "Alert") << "cal " << cal;
        EXPECT_STREQ(Name(Decide(cal, DecForRateRatio(cal, 1.04))), "None") << "cal " << cal;
        EXPECT_STREQ(Name(Decide(cal, DecForRateRatio(cal, 1.06))), "Alert") << "cal " << cal;
    }
    // At Dec 80 that is about half a degree.
    EXPECT_STREQ(Name(Decide(80., 80.25)), "None");
    EXPECT_STREQ(Name(Decide(80., 80.6)), "Alert");
}

TEST(DecComp, NearThePoleTheFloorStillHoldsJitter)
{
    // At 89.9 a 0.019 deg change moves cos(Dec) by 19%; it is still a reading that
    // has not moved. 0.03 deg is past the floor and alerts.
    EXPECT_STREQ(Name(Decide(89.9, 89.881)), "None");
    EXPECT_STREQ(Name(Decide(89.9, 89.87)), "Alert");
}

TEST(DecComp, BelowLimitCompensatesRealMoves)
{
    // Unchanged below the limit: a Dec that moved scales the RA rate, however little
    // the scale, and also when the scope is now above the limit.
    EXPECT_STREQ(Name(Decide(0., 0.5)), "Compensate");
    EXPECT_STREQ(Name(Decide(0., -0.03)), "Compensate");
    EXPECT_STREQ(Name(Decide(0., 15.)), "Compensate");
    EXPECT_STREQ(Name(Decide(30., 45.)), "Compensate");
    EXPECT_STREQ(Name(Decide(45., 30.)), "Compensate");
    EXPECT_STREQ(Name(Decide(-30., -45.)), "Compensate");
    EXPECT_STREQ(Name(Decide(59., 59.1)), "Compensate");
    EXPECT_STREQ(Name(Decide(40., 75.)), "Compensate");
}

TEST(DecComp, BelowLimitJitterKeepsTheCalibrationRate)
{
    // Before #157 these compensated too. Leaving them alone keeps the calibration's
    // RA rate, which differs from the compensated one by less than 0.07% even at the
    // limit: far inside what a calibration measures.
    struct Case
    {
        double cal, now;
    };
    for (Case c : { Case { 0., 0.001 }, Case { 0., 0.019 }, Case { 45., 45.001 }, Case { 45., 44.981 }, Case { 59.9, 59.919 },
                    Case { -59.9, -59.919 } })
    {
        EXPECT_STREQ(Name(Decide(c.cal, c.now)), "None") << c.cal << " -> " << c.now;
        EXPECT_LT(std::fabs(CompensatedRaRate(1.0, Rad(c.cal), Rad(c.now)) - 1.0), 0.0007) << c.cal << " -> " << c.now;
    }
}

TEST(DecComp, SameDecIsNone)
{
    for (double cal : { 0., 45., 70., 85., -70. })
        EXPECT_STREQ(Name(Decide(cal, cal)), "None") << cal;
}

TEST(DecComp, UnknownDecIsNone)
{
    double const nan = std::numeric_limits<double>::quiet_NaN();
    double const inf = std::numeric_limits<double>::infinity();
    EXPECT_STREQ(Name(DecideDecComp(std::nullopt, Rad(75.), DEC_COMP_LIMIT)), "None");
    EXPECT_STREQ(Name(DecideDecComp(Rad(70.), std::nullopt, DEC_COMP_LIMIT)), "None");
    EXPECT_STREQ(Name(DecideDecComp(std::nullopt, std::nullopt, DEC_COMP_LIMIT)), "None");
    // a driver that hands back a non-finite Dec has not told us where it points
    EXPECT_STREQ(Name(DecideDecComp(Rad(70.), nan, DEC_COMP_LIMIT)), "None");
    EXPECT_STREQ(Name(DecideDecComp(Rad(30.), nan, DEC_COMP_LIMIT)), "None");
    EXPECT_STREQ(Name(DecideDecComp(Rad(30.), inf, DEC_COMP_LIMIT)), "None");
    EXPECT_STREQ(Name(DecideDecComp(nan, Rad(30.), DEC_COMP_LIMIT)), "None");
}

TEST(DecComp, LimitItselfCompensates)
{
    // |cal Dec| > limit alerts, as before: a calibration at exactly the limit compensates.
    EXPECT_STREQ(Name(DecideDecComp(DEC_COMP_LIMIT, DEC_COMP_LIMIT + Rad(5.), DEC_COMP_LIMIT)), "Compensate");
    EXPECT_STREQ(Name(DecideDecComp(-DEC_COMP_LIMIT, -DEC_COMP_LIMIT - Rad(5.), DEC_COMP_LIMIT)), "Compensate");
    EXPECT_STREQ(Name(DecideDecComp(DEC_COMP_LIMIT + Rad(0.1), DEC_COMP_LIMIT + Rad(5.1), DEC_COMP_LIMIT)), "Alert");
}

TEST(DecComp, CompensatedRaRateScalesByCosine)
{
    EXPECT_NEAR(CompensatedRaRate(2.0, Rad(0.), Rad(60.)), 1.0, 1e-12);
    EXPECT_NEAR(CompensatedRaRate(1.0, Rad(60.), Rad(0.)), 2.0, 1e-12);
    EXPECT_NEAR(CompensatedRaRate(1.0, Rad(30.), Rad(-30.)), 1.0, 1e-12);
    EXPECT_NEAR(CompensatedRaRate(1.0, Rad(-45.), Rad(-30.)), std::cos(Rad(30.)) / std::cos(Rad(45.)), 1e-12);

    // the Dec now is held within +-89 deg, so the rate stays finite toward the pole
    double const at89 = CompensatedRaRate(1.0, Rad(0.), Rad(89.));
    EXPECT_NEAR(at89, std::cos(Rad(89.)), 1e-12);
    EXPECT_DOUBLE_EQ(CompensatedRaRate(1.0, Rad(0.), Rad(89.5)), at89);
    EXPECT_DOUBLE_EQ(CompensatedRaRate(1.0, Rad(0.), Rad(90.)), at89);
    EXPECT_DOUBLE_EQ(CompensatedRaRate(1.0, Rad(0.), Rad(-90.)), at89);
}
