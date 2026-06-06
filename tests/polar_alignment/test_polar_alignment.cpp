// Unit tests for src/staticpa_geometry.cpp — the pure geometry/astrometry
// kernels extracted from StaticPaToolWin (see staticpa_geometry.h). The real
// production translation unit links here directly: it has no wx/Mount/clock
// coupling, so it compiles unmodified against the shadow phd.h.
//
// The tests lean on geometric invariants that are independent of the exact
// formula (a point on a circle is `r` from the centre; orthogonal error
// components satisfy Pythagoras; the pole projects to the centre of
// rotation), so they pin correctness rather than just snapshotting the
// current numbers. The precession test checks the output moves at the known
// ~50.3 arcsec/yr general-precession rate.

#include <gtest/gtest.h>

#include "staticpa_geometry.h"

#include <cmath>

using namespace staticpa_geom;

namespace
{
double dist(const Px& a, double x, double y)
{
    return std::hypot(a.x - x, a.y - y);
}
} // namespace

// --- CircleFrom3Points ------------------------------------------------------

TEST(PaGeometry, CircleFrom3PointsRecoversKnownCircle)
{
    // Three points on the circle centred (5, 5) with radius 5.
    Circle c = CircleFrom3Points({ 10., 5. }, { 0., 5. }, { 5., 10. });
    EXPECT_NEAR(c.cx, 5.0, 1e-9);
    EXPECT_NEAR(c.cy, 5.0, 1e-9);
    EXPECT_NEAR(c.r, 5.0, 1e-9);
}

TEST(PaGeometry, CircleFrom3PointsAllThreeOnTheCircle)
{
    // Arbitrary non-degenerate triangle: every vertex must end up exactly r
    // from the recovered centre.
    Px p1 { 3., 7. }, p2 { 11., 2. }, p3 { -4., -6. };
    Circle c = CircleFrom3Points(p1, p2, p3);
    EXPECT_NEAR(dist(p1, c.cx, c.cy), c.r, 1e-6);
    EXPECT_NEAR(dist(p2, c.cx, c.cy), c.r, 1e-6);
    EXPECT_NEAR(dist(p3, c.cx, c.cy), c.r, 1e-6);
}

// --- CircleFrom2PointsAndAngle ----------------------------------------------

TEST(PaGeometry, CircleFrom2PointsAndAnglePutsBothPointsOnCircle)
{
    // Whatever the rotation angle/hemisphere, the two measured points must sit
    // on the returned circle (equidistant from the centre by the radius).
    for (int hemi : { +1, -1 })
    {
        for (double deg : { 10., 35., 90., 150. })
        {
            double radiff = hemi * deg * M_PI / 180.0;
            Px p1 { 100., 120. }, p2 { 180., 90. };
            double slopebase = -999.;
            Circle c = CircleFrom2PointsAndAngle(p1, p2, radiff, hemi, &slopebase);

            EXPECT_GT(c.r, 0.0);
            EXPECT_TRUE(std::isfinite(c.r));
            EXPECT_NEAR(dist(p1, c.cx, c.cy), c.r, 1e-6) << "hemi=" << hemi << " deg=" << deg;
            EXPECT_NEAR(dist(p2, c.cx, c.cy), c.r, 1e-6) << "hemi=" << hemi << " deg=" << deg;
            EXPECT_NE(slopebase, -999.); // out-param populated
        }
    }
}

// --- DecomposeCoR -----------------------------------------------------------

TEST(PaGeometry, DecomposeCoRCentreGivesZeroCorrection)
{
    // CoR exactly at the sensor centre -> no Dec/Cone offset.
    Circle cor { 100., 100., 50. };
    CorrVectors v = DecomposeCoR(cor, 200, 200, 0.0);
    EXPECT_NEAR(std::hypot(v.a.x, v.a.y), 0.0, 1e-9);
    EXPECT_NEAR(std::hypot(v.b.x, v.b.y), 0.0, 1e-9);
}

TEST(PaGeometry, DecomposeCoRPurelyVerticalOffsetAtZeroCamAngle)
{
    // CoR 20px above the sensor centre, camera angle 0: the offset is entirely
    // "Dec" (vertical), zero "Cone".
    Circle cor { 100., 80., 30. };
    CorrVectors v = DecomposeCoR(cor, 200, 200, 0.0);
    EXPECT_NEAR(v.a.x, 0.0, 1e-9); // Dec.x
    EXPECT_NEAR(v.a.y, 20.0, 1e-9); // Dec.y == full offset
    EXPECT_NEAR(std::hypot(v.b.x, v.b.y), 0.0, 1e-9); // Cone == 0
}

TEST(PaGeometry, DecomposeCoRComponentsArePythagorean)
{
    // For any camera angle, |Dec|^2 + |Cone|^2 == |offset-from-centre|^2.
    Circle cor { 130., 60., 40. };
    int w = 200, h = 160;
    double offset = std::hypot(w / 2 - cor.cx, h / 2 - cor.cy);
    for (double cam : { 0., 17., 45., 123., -88. })
    {
        CorrVectors v = DecomposeCoR(cor, w, h, cam);
        double sum2 = std::hypot(v.a.x, v.a.y) * std::hypot(v.a.x, v.a.y) + std::hypot(v.b.x, v.b.y) * std::hypot(v.b.x, v.b.y);
        EXPECT_NEAR(std::sqrt(sum2), offset, 1e-6) << "cam=" << cam;
    }
}

// --- Radec2Px ---------------------------------------------------------------

TEST(PaGeometry, Radec2PxPoleMapsToCentre)
{
    // Dec = 90 (the celestial pole) sits exactly on the centre of rotation.
    Px px = Radec2Px(123.0, 90.0, 5.0, 200.0, 30.0, false, 1);
    EXPECT_NEAR(px.x, 0.0, 1e-9);
    EXPECT_NEAR(px.y, 0.0, 1e-9);
}

TEST(PaGeometry, Radec2PxRadiusFollowsDecAndScale)
{
    // Pixel radius from the CoR is (90 - |dec|) * 3600 / pxScale, independent
    // of all the rotation parameters.
    const double pxScale = 3600.0; // 1px == 1 arcsec
    for (double dec : { 89.0, 80.0, 45.0, 0.0, -60.0 })
    {
        for (bool flip : { false, true })
        {
            Px px = Radec2Px(50.0, dec, pxScale, 200.0, 30.0, flip, 1);
            double expectedR = (90.0 - std::fabs(dec)) * 3600.0 / pxScale;
            EXPECT_NEAR(std::hypot(px.x, px.y), expectedR, 1e-6) << "dec=" << dec << " flip=" << flip;
        }
    }
}

// --- DecomposeAltAz ---------------------------------------------------------

TEST(PaGeometry, DecomposeAltAzErrorsArePythagorean)
{
    Px target { 150., 90. }, measured { 120., 140. };
    double expectedTot = std::hypot(target.x - measured.x, target.y - measured.y);
    for (double cam : { 0., 25., 110. })
    {
        for (double ha : { 0., 45., 200. })
        {
            AltAzResult r = DecomposeAltAz(target, measured, cam, ha);
            EXPECT_NEAR(r.totErrPx, expectedTot, 1e-9);
            EXPECT_NEAR(std::hypot(r.altErrPx, r.azErrPx), expectedTot, 1e-6) << "cam=" << cam << " ha=" << ha;
            // Correction vector magnitudes match the scalar errors.
            EXPECT_NEAR(std::hypot(r.vec.a.x, r.vec.a.y), std::fabs(r.azErrPx), 1e-6);
            EXPECT_NEAR(std::hypot(r.vec.b.x, r.vec.b.y), std::fabs(r.altErrPx), 1e-6);
        }
    }
}

TEST(PaGeometry, DecomposeAltAzZeroResidualGivesZeroError)
{
    Px same { 77., 77. };
    AltAzResult r = DecomposeAltAz(same, same, 33.0, 120.0);
    EXPECT_NEAR(r.totErrPx, 0.0, 1e-12);
    EXPECT_NEAR(r.altErrPx, 0.0, 1e-12);
    EXPECT_NEAR(r.azErrPx, 0.0, 1e-12);
}

// --- PrecessJ2000 -----------------------------------------------------------

TEST(PaGeometry, PrecessJ2000OutputsAreInRange)
{
    Px p = PrecessJ2000(365.25 * 25.0, 83.6, 22.0); // ~Betelgeuse, 25yr on
    EXPECT_GE(p.x, 0.0);
    EXPECT_LT(p.x, 360.0);
    EXPECT_GE(p.y, -90.0);
    EXPECT_LE(p.y, 90.0);
}

TEST(PaGeometry, PrecessJ2000MovesAtGeneralPrecessionRate)
{
    // The general precession rate is ~50.3 arcsec/yr. Over 25 years a point
    // moves ~0.349 deg along the ecliptic. At RA=0/Dec=0 (the vernal equinox)
    // that motion splits across both RA and Dec, so check the great-circle
    // separation between the J2000 and precessed positions.
    const double years = 25.0;
    const double ra0 = 0.0, dec0 = 0.0;
    Px p = PrecessJ2000(365.25 * years, ra0, dec0);

    auto rad = [](double d) { return d * M_PI / 180.0; };
    double cosSep =
        std::sin(rad(dec0)) * std::sin(rad(p.y)) + std::cos(rad(dec0)) * std::cos(rad(p.y)) * std::cos(rad(ra0 - p.x));
    double sepDeg = std::acos(std::min(1.0, cosSep)) * 180.0 / M_PI;

    double expectedDeg = 50.3 * years / 3600.0; // ~0.349
    EXPECT_NEAR(sepDeg, expectedDeg, 0.01) << "precession rate off";
}

TEST(PaGeometry, PrecessJ2000IsIdentityAtZeroIsSmall)
{
    // At t=0 only the tiny constant terms of the series apply (~2.6 arcsec),
    // so the point should barely move.
    Px p = PrecessJ2000(0.0, 100.0, 20.0);
    EXPECT_NEAR(p.x, 100.0, 0.01);
    EXPECT_NEAR(p.y, 20.0, 0.01);
}
