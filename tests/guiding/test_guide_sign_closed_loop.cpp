// Guide sign in a closed loop (#142).
//
// A minimal planar mount model calibrates the way Scope::UpdateCalibrationState measures (the
// angle and rate of the star's motion over the West and North legs), and then guides through the
// production chain that turns a star offset into pulses:
//
//   cameraOfs = star - lock                              (guider_multistar.cpp)
//   m_yAngleError = YAngleError(xAngle, yAngle)          (Mount::SetCalibration)
//   mountOfs = CameraToMount(cameraOfs, xAngle, yErr)    (Mount::TransformCameraCoordinatesToMountCoordinates)
//   pulses = PlanOffset(mountOfs, xRate, yRate)          (Mount::MoveOffset)
//   each pulse through PlanAxis(GUIDE_STEP)              (Scope::MoveAxis)
//
// with the identity guide algorithm. Whatever the camera angle, Dec mirror or pier side, a
// displaced star must get a correction that brings it back toward the lock position. A sign
// error anywhere in that chain makes the error grow instead.

#include <gtest/gtest.h>

#include "calibration_transform.h"
#include "guide_move_plan.h"

#include <cmath>

using namespace guide_move_plan;

namespace
{
constexpr double DEG = M_PI / 180.0;
constexpr unsigned GUIDE_STEP = ALGO_RESULT | USE_BLC | GRAPH; // MOVEOPTS_GUIDE_STEP

// norm_angle() from image_math.h
double NormAngle(double a)
{
    return a - std::floor((a + M_PI) / (2.0 * M_PI)) * (2.0 * M_PI);
}

struct Pt
{
    double x = 0.;
    double y = 0.;
};

// PHD_Point::Angle: a.Angle(b) is the angle of the vector a - b, 0 for a zero vector
double Angle(const Pt& a, const Pt& b)
{
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return dx != 0. || dy != 0. ? std::atan2(dy, dx) : 0.;
}

double Distance(const Pt& a, const Pt& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

// The star on the sensor, moved instantly and exactly by each pulse
class PlanarMount
{
public:
    double westDeg = 30.; // image angle of the star's motion on a West pulse
    bool decMirrored = false; // false: a North pulse moves the star at westDeg - 90, true: westDeg + 90
    double nonOrthoDeg = 0.; // extra rotation of the North motion
    double raPxPerMs = 0.003878; // FL400 / 3.75 um / bin 1 / 0.5x guide speed
    double decPxPerMs = 0.003878;
    Pt star { 512., 384. };

    double NorthDeg() const { return westDeg + (decMirrored ? 90. : -90.) + nonOrthoDeg; }

    void Pulse(Dir dir, int ms)
    {
        double angle = 0., px = 0.;
        switch (dir)
        {
        case Dir::West:
            angle = westDeg;
            px = raPxPerMs * ms;
            break;
        case Dir::East:
            angle = westDeg + 180.;
            px = raPxPerMs * ms;
            break;
        case Dir::North:
            angle = NorthDeg();
            px = decPxPerMs * ms;
            break;
        case Dir::South:
            angle = NorthDeg() + 180.;
            px = decPxPerMs * ms;
            break;
        case Dir::None:
            return;
        }
        star.x += px * std::cos(angle * DEG);
        star.y += px * std::sin(angle * DEG);
    }

    // A meridian flip turns the camera 180 degrees on the sky. A mount that reverses Dec after
    // the flip (the usual case, flipMirrorsDec) keeps the North motion where it was on the
    // sensor; one that does not turns it by 180 degrees too.
    void FlipPier(bool flipMirrorsDec)
    {
        westDeg += 180.;
        if (flipMirrorsDec)
            decMirrored = !decMirrored;
    }
};

struct Cal
{
    double xAngle = 0.;
    double xRate = 0.;
    double yAngle = 0.;
    double yRate = 0.;
};

// The angles and rates Scope::UpdateCalibrationState measures: xAngle is
// start.Angle(current) at the end of the West leg (scope.cpp GO_WEST), yAngle is
// current.Angle(start) at the end of the North leg (GO_NORTH), each rate is the distance over
// the leg's total pulse time. The star is put back after each leg.
Cal Calibrate(PlanarMount& mount, int stepMs = 550, int steps = 12)
{
    Cal cal;
    Pt start = mount.star;
    for (int i = 0; i < steps; i++)
        mount.Pulse(Dir::West, stepMs);
    cal.xAngle = Angle(start, mount.star);
    cal.xRate = Distance(start, mount.star) / (steps * stepMs);
    mount.star = start;

    for (int i = 0; i < steps; i++)
        mount.Pulse(Dir::North, stepMs);
    cal.yAngle = Angle(mount.star, start);
    cal.yRate = Distance(start, mount.star) / (steps * stepMs);
    mount.star = start;
    return cal;
}

// Mount::FlipCalibration: xAngle + pi, and yAngle + pi only when the mount needs a Dec flip
Cal FlipCalibration(const Cal& cal, bool decFlipRequired)
{
    Cal flipped = cal;
    flipped.xAngle = NormAngle(cal.xAngle + M_PI);
    flipped.yAngle = NormAngle(cal.yAngle + (decFlipRequired ? M_PI : 0.));
    return flipped;
}

struct Step
{
    OffsetPlan offset;
    AxisPlan ra;
    AxisPlan dec;
};

class Guider
{
public:
    // the lock position is where the star is now
    Guider(PlanarMount& mount, const Cal& cal, AxisPolicy policy)
        : m_mount(mount), m_cal(cal), m_policy(policy), m_lock(mount.star)
    {
    }

    // One guide step: measure, plan RA then Dec, pulse
    Step GuideStep()
    {
        Pt cameraOfs { m_mount.star.x - m_lock.x, m_mount.star.y - m_lock.y };
        calibration_transform::CamToMount m = calibration_transform::CameraToMount(
            cameraOfs.x, cameraOfs.y, m_cal.xAngle, calibration_transform::YAngleError(m_cal.xAngle, m_cal.yAngle));

        Step step;
        step.offset = PlanOffset(m.mount.x, m.mount.y, m_cal.xRate, m_cal.yRate);
        step.ra = PlanAxis(step.offset.raDir, step.offset.raMs, GUIDE_STEP, m_policy, m_raLimit, m_decLimit);
        m_mount.Pulse(step.offset.raDir, step.ra.ms);
        step.dec = PlanAxis(step.offset.decDir, step.offset.decMs, GUIDE_STEP, m_policy, m_raLimit, m_decLimit);
        m_mount.Pulse(step.offset.decDir, step.dec.ms);
        return step;
    }

    double Error() const { return Distance(m_mount.star, m_lock); }

private:
    PlanarMount& m_mount;
    Cal m_cal;
    AxisPolicy m_policy;
    Pt m_lock;
    LimitTracker m_raLimit;
    LimitTracker m_decLimit;
};

AxisPolicy GuidePolicy(DecMode mode = DecMode::Auto)
{
    AxisPolicy p;
    p.guidingEnabled = true;
    p.decMode = mode;
    p.maxRaMs = 2500;
    p.maxDecMs = 2500;
    return p;
}

const double ANGLES[] = { 0., 30., 90., 200., -135. };

} // namespace

TEST(GuideSignClosedLoop, CalibrationMatchesTheMount)
{
    // xAngle points opposite the West motion; yAngleError is about 0, or pi when mirrored
    for (double theta : ANGLES)
    {
        for (int mirrored = 0; mirrored <= 1; mirrored++)
        {
            PlanarMount mount;
            mount.westDeg = theta;
            mount.decMirrored = mirrored != 0;
            Cal cal = Calibrate(mount);
            SCOPED_TRACE(testing::Message() << "theta=" << theta << " mirrored=" << mirrored);
            EXPECT_NEAR(std::fabs(NormAngle(cal.xAngle - (theta + 180.) * DEG)), 0., 1e-9);
            EXPECT_NEAR(cal.xRate, mount.raPxPerMs, 1e-12);
            EXPECT_NEAR(cal.yRate, mount.decPxPerMs, 1e-12);
            double yErr = calibration_transform::YAngleError(cal.xAngle, cal.yAngle);
            EXPECT_NEAR(std::fabs(yErr), mirrored ? M_PI : 0., 1e-9);
        }
    }
}

TEST(GuideSignClosedLoop, StarMovedWestIsGuidedEast)
{
    for (double theta : ANGLES)
    {
        for (int mirrored = 0; mirrored <= 1; mirrored++)
        {
            PlanarMount mount;
            mount.westDeg = theta;
            mount.decMirrored = mirrored != 0;
            Guider guider(mount, Calibrate(mount), GuidePolicy());
            mount.Pulse(Dir::West, 500);

            SCOPED_TRACE(testing::Message() << "theta=" << theta << " mirrored=" << mirrored);
            Step step = guider.GuideStep();
            EXPECT_EQ(step.offset.raDir, Dir::East);
            EXPECT_NEAR(step.ra.ms, 500, 1);
            EXPECT_LE(step.dec.ms, 1);
            EXPECT_LT(guider.Error(), 0.1);
        }
    }
}

TEST(GuideSignClosedLoop, StarMovedNorthIsGuidedSouth)
{
    for (double theta : ANGLES)
    {
        for (int mirrored = 0; mirrored <= 1; mirrored++)
        {
            PlanarMount mount;
            mount.westDeg = theta;
            mount.decMirrored = mirrored != 0;
            Guider guider(mount, Calibrate(mount), GuidePolicy());
            mount.Pulse(Dir::North, 500);

            SCOPED_TRACE(testing::Message() << "theta=" << theta << " mirrored=" << mirrored);
            Step step = guider.GuideStep();
            EXPECT_EQ(step.offset.decDir, Dir::South);
            EXPECT_NEAR(step.dec.ms, 500, 1);
            EXPECT_LE(step.ra.ms, 1);
            EXPECT_LT(guider.Error(), 0.1);
        }
    }
}

TEST(GuideSignClosedLoop, StarMovedEastAndSouthIsGuidedWestAndNorth)
{
    for (double theta : ANGLES)
    {
        for (int mirrored = 0; mirrored <= 1; mirrored++)
        {
            PlanarMount mount;
            mount.westDeg = theta;
            mount.decMirrored = mirrored != 0;
            Guider guider(mount, Calibrate(mount), GuidePolicy());
            mount.Pulse(Dir::East, 700);
            mount.Pulse(Dir::South, 300);

            SCOPED_TRACE(testing::Message() << "theta=" << theta << " mirrored=" << mirrored);
            Step step = guider.GuideStep();
            EXPECT_EQ(step.offset.raDir, Dir::West);
            EXPECT_NEAR(step.ra.ms, 700, 1);
            EXPECT_EQ(step.offset.decDir, Dir::North);
            EXPECT_NEAR(step.dec.ms, 300, 1);
            EXPECT_LT(guider.Error(), 0.1);
        }
    }
}

TEST(GuideSignClosedLoop, FlippedCalibrationKeepsGuidingAfterAMeridianFlip)
{
    // flipMirrorsDec (the mount reverses Dec after a flip) goes with decFlipRequired false, the
    // default; a mount that does not reverse Dec needs CalibrationFlipRequiresDecFlip
    for (double theta : ANGLES)
    {
        for (int mirrored = 0; mirrored <= 1; mirrored++)
        {
            for (int flipMirrorsDec = 0; flipMirrorsDec <= 1; flipMirrorsDec++)
            {
                PlanarMount mount;
                mount.westDeg = theta;
                mount.decMirrored = mirrored != 0;
                Cal cal = Calibrate(mount);
                mount.FlipPier(flipMirrorsDec != 0);
                Guider guider(mount, FlipCalibration(cal, flipMirrorsDec == 0), GuidePolicy());
                mount.Pulse(Dir::West, 400);
                mount.Pulse(Dir::North, 600);

                SCOPED_TRACE(testing::Message()
                             << "theta=" << theta << " mirrored=" << mirrored << " flipMirrorsDec=" << flipMirrorsDec);
                Step step = guider.GuideStep();
                EXPECT_EQ(step.offset.raDir, Dir::East);
                EXPECT_NEAR(step.ra.ms, 400, 1);
                EXPECT_EQ(step.offset.decDir, Dir::South);
                EXPECT_NEAR(step.dec.ms, 600, 1);
                EXPECT_LT(guider.Error(), 0.1);
            }
        }
    }
}

TEST(GuideSignClosedLoop, FlipTwiceIsTheOriginalCalibration)
{
    for (double theta : ANGLES)
    {
        for (int decFlip = 0; decFlip <= 1; decFlip++)
        {
            PlanarMount mount;
            mount.westDeg = theta;
            Cal cal = Calibrate(mount);
            Cal twice = FlipCalibration(FlipCalibration(cal, decFlip != 0), decFlip != 0);
            EXPECT_NEAR(std::fabs(NormAngle(twice.xAngle - cal.xAngle)), 0., 1e-12);
            EXPECT_NEAR(std::fabs(NormAngle(twice.yAngle - cal.yAngle)), 0., 1e-12);
        }
    }
}

TEST(GuideSignClosedLoop, StaleCalibrationAfterAFlipDrivesTheStarAway)
{
    // The check above can fail: guiding on the pre-flip calibration sends RA the wrong way
    for (double theta : ANGLES)
    {
        PlanarMount mount;
        mount.westDeg = theta;
        Cal cal = Calibrate(mount);
        mount.FlipPier(true);
        Guider guider(mount, cal, GuidePolicy());
        mount.Pulse(Dir::West, 500);
        double before = guider.Error();

        guider.GuideStep();
        EXPECT_GT(guider.Error(), 1.9 * before) << "theta=" << theta;
    }
}

TEST(GuideSignClosedLoop, NonOrthogonalAxesStillConverge)
{
    // With skewed axes the transform's projection leaves a small residual each step, which the
    // next steps take out
    for (double theta : ANGLES)
    {
        for (int skew = -10; skew <= 10; skew += 5)
        {
            PlanarMount mount;
            mount.westDeg = theta;
            mount.nonOrthoDeg = skew;
            Guider guider(mount, Calibrate(mount), GuidePolicy());
            mount.Pulse(Dir::West, 1000);
            mount.Pulse(Dir::South, 800);

            SCOPED_TRACE(testing::Message() << "theta=" << theta << " skew=" << skew);
            double error = guider.Error();
            for (int i = 0; i < 5; i++)
            {
                guider.GuideStep();
                EXPECT_LT(guider.Error(), error);
                error = guider.Error();
                if (error < 0.1)
                    break;
            }
            EXPECT_LT(error, 0.1);
        }
    }
}

TEST(GuideSignClosedLoop, RunawayIsClampedToMaxRaEveryStep)
{
    // A 9 s West runaway is corrected 2500 ms at a time, never more, and comes back
    PlanarMount mount;
    mount.westDeg = 200.;
    Guider guider(mount, Calibrate(mount), GuidePolicy());
    mount.Pulse(Dir::West, 9000);

    double error = guider.Error();
    int limitedSteps = 0;
    for (int i = 0; i < 6 && error >= 0.1; i++)
    {
        Step step = guider.GuideStep();
        EXPECT_EQ(step.offset.raDir, Dir::East);
        EXPECT_LE(step.ra.ms, 2500);
        EXPECT_FALSE(step.ra.alert);
        if (step.ra.limited)
            limitedSteps++;
        EXPECT_LT(guider.Error(), error);
        error = guider.Error();
    }
    EXPECT_EQ(limitedSteps, 3); // 9000 = 2500 * 3 + 1500
    EXPECT_LT(error, 0.1);
}

TEST(GuideSignClosedLoop, DecNorthOnlyCorrectsWithNorthPulses)
{
    for (double theta : ANGLES)
    {
        PlanarMount mount;
        mount.westDeg = theta;
        mount.decMirrored = true;
        Guider guider(mount, Calibrate(mount), GuidePolicy(DecMode::North));

        // star moved South: the correction is North, allowed
        mount.Pulse(Dir::South, 500);
        Step back = guider.GuideStep();
        EXPECT_EQ(back.offset.decDir, Dir::North);
        EXPECT_NEAR(back.dec.ms, 500, 1);
        EXPECT_LT(guider.Error(), 0.1);

        // star moved North: the correction would be South, so Dec is left alone
        mount.Pulse(Dir::North, 500);
        double before = guider.Error();
        Step stay = guider.GuideStep();
        EXPECT_EQ(stay.offset.decDir, Dir::South);
        EXPECT_TRUE(stay.dec.zeroedByDecMode);
        EXPECT_EQ(stay.dec.ms, 0);
        EXPECT_NEAR(guider.Error(), before, 0.01);
    }
}

TEST(GuideSignClosedLoop, DecNoneGuidesRaOnly)
{
    PlanarMount mount;
    mount.westDeg = 90.;
    Guider guider(mount, Calibrate(mount), GuidePolicy(DecMode::None));
    mount.Pulse(Dir::East, 500);
    mount.Pulse(Dir::North, 500);
    double decPx = mount.decPxPerMs * 500;

    Step step = guider.GuideStep();
    EXPECT_EQ(step.offset.raDir, Dir::West);
    EXPECT_NEAR(step.ra.ms, 500, 1);
    EXPECT_EQ(step.dec.ms, 0);
    EXPECT_NEAR(guider.Error(), decPx, 0.01);
}
