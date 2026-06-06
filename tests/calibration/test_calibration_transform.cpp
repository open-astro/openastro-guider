// Unit tests for src/calibration_transform.cpp — the camera<->mount coordinate
// transforms extracted from Mount (see calibration_transform.h). The real
// production translation unit links here directly: it has no wx/Mount coupling,
// so it compiles unmodified against the shadow phd.h.
//
// The transforms rotate a vector by the calibration RA-axis angle and skew the
// Dec component by the axis-orthogonality error. Tests lean on invariants:
//   - with perfect orthogonality (yAngleError == 0) the transform is a pure
//     rotation, so it preserves length and round-trips exactly;
//   - a known 90-degree calibration rotates (1,0) to (0,-1);
//   - the documented mountTheta sign flip kicks in for |yAngleError| > pi/2;
//   - the exposed intermediates match hypot()/atan2() of the input.

#include <gtest/gtest.h>

#include "calibration_transform.h"

#include <cmath>

using namespace calibration_transform;

TEST(CalibrationTransform, PerfectOrthogonalityPreservesLength)
{
    // yAngleError == 0 -> pure rotation by the calibration angle -> |v| kept.
    const double cal = 0.7; // arbitrary RA-axis angle (radians)
    for (auto v : { XY { 3., 4. }, XY { -5., 12. }, XY { 1., 0. }, XY { 0., -8. } })
    {
        CamToMount r = CameraToMount(v.x, v.y, cal, 0.0);
        EXPECT_NEAR(std::hypot(r.mount.x, r.mount.y), std::hypot(v.x, v.y), 1e-9);
    }
}

TEST(CalibrationTransform, RoundTripsWhenOrthogonal)
{
    // With yAngleError == 0, MountToCamera o CameraToMount == identity.
    const double cal = 1.1;
    for (auto v : { XY { 3., 4. }, XY { -5., 12. }, XY { 7., -2. }, XY { 0.5, 0.25 } })
    {
        CamToMount fwd = CameraToMount(v.x, v.y, cal, 0.0);
        MountToCam back = MountToCamera(fwd.mount.x, fwd.mount.y, cal, 0.0);
        EXPECT_NEAR(back.camera.x, v.x, 1e-9) << "v=(" << v.x << "," << v.y << ")";
        EXPECT_NEAR(back.camera.y, v.y, 1e-9) << "v=(" << v.x << "," << v.y << ")";
    }
}

TEST(CalibrationTransform, KnownNinetyDegreeRotation)
{
    // Camera unit vector +X, calibration angle 90 deg, perfect orthogonality:
    // rotate by -cal -> points to -Y.
    CamToMount r = CameraToMount(1.0, 0.0, M_PI / 2., 0.0);
    EXPECT_NEAR(r.mount.x, 0.0, 1e-12);
    EXPECT_NEAR(r.mount.y, -1.0, 1e-12);
    EXPECT_NEAR(r.hyp, 1.0, 1e-12);
    EXPECT_NEAR(r.cameraTheta, 0.0, 1e-12);
}

TEST(CalibrationTransform, OrthogonalityErrorSkewsOnlyDecComponent)
{
    // The X (RA) component depends only on cal; the Y (Dec) component carries
    // the extra yAngleError term. So changing yAngleError must leave mount.x
    // unchanged but move mount.y.
    const double cal = 0.3;
    CamToMount a = CameraToMount(2.0, 1.0, cal, 0.0);
    CamToMount b = CameraToMount(2.0, 1.0, cal, 0.4);
    EXPECT_NEAR(a.mount.x, b.mount.x, 1e-12); // RA component unaffected
    EXPECT_NE(a.mount.y, b.mount.y); // Dec component skewed
}

TEST(CalibrationTransform, MountToCameraSignFlipsForLargeOrthogonalityError)
{
    // |yAngleError| > pi/2 means the Dec axis was reversed during calibration;
    // MountToCamera negates mountTheta to compensate.
    const double cal = 0.2;
    MountToCam normal = MountToCamera(1.0, 1.0, cal, 0.0);
    MountToCam flipped = MountToCamera(1.0, 1.0, cal, M_PI); // > pi/2

    EXPECT_NEAR(normal.mountTheta, M_PI / 4., 1e-12); // atan2(1,1)
    EXPECT_NEAR(flipped.mountTheta, -M_PI / 4., 1e-12); // sign-flipped
    EXPECT_NEAR(flipped.xAngle, -M_PI / 4. + cal, 1e-12);
}

TEST(CalibrationTransform, ExposedIntermediatesMatchInputGeometry)
{
    CamToMount r = CameraToMount(3.0, 4.0, 0.5, 0.1);
    EXPECT_NEAR(r.hyp, 5.0, 1e-12); // hypot(3,4)
    EXPECT_NEAR(r.cameraTheta, std::atan2(4.0, 3.0), 1e-12);
    EXPECT_NEAR(r.xAngle, r.cameraTheta - 0.5, 1e-12);
    EXPECT_NEAR(r.yAngle, r.cameraTheta - (0.5 + 0.1), 1e-12);
}

TEST(CalibrationTransform, ZeroVectorMapsToZero)
{
    // atan2(0,0) is implementation-defined; the transform pins angle 0 and
    // returns a zero vector (matching PHD_Point::Angle()).
    CamToMount r = CameraToMount(0.0, 0.0, 0.9, 0.2);
    EXPECT_EQ(r.cameraTheta, 0.0);
    EXPECT_EQ(r.hyp, 0.0);
    EXPECT_EQ(r.mount.x, 0.0);
    EXPECT_EQ(r.mount.y, 0.0);
}
