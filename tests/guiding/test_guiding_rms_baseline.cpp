// RMS baseline: the default guide algorithms on a simulated sky and mount (#142).
//
// A deterministic, seeded simulation of guiding one star with the production chain that turns
// a star offset into pulses, the way the guider runs it frame by frame:
//
//   cameraOfs = measured star - lock                      (guider_multistar.cpp)
//   mountOfs = CameraToMount(cameraOfs, xAngle, yErr)     (Mount::TransformCameraCoordinatesToMountCoordinates)
//   x = HysteresisResult(mountOfs.x)                      (GuideAlgorithmHysteresis::result, the RA default)
//   y = ResistSwitchResult(mountOfs.y)                    (GuideAlgorithmResistSwitch::result, the Dec default)
//   pulses = PlanOffset(x, y, xRate, yRate)               (Mount::MoveOffset)
//   each pulse through PlanAxis(guide step)               (Scope::MoveAxis), RA then Dec
//
// with the parameters the production constructors and Scope use with nothing in the profile:
// Hysteresis minMove 0.2 px, hysteresis 0.1, aggression 0.7; ResistSwitch minMove 0.2 px,
// aggression 1.0, fast switch on; max RA and Dec pulse 2500 ms; Dec guide mode Auto.
//
// The sky: the star drifts on the RA axis by a sinusoidal periodic error (480 s, +/-10") and a
// constant drift (0.01"/s), and on the Dec axis by a constant drift (0.02"/s, a polar
// alignment error), from the lock position at t = 0. The guide camera: 400 mm focal length,
// 3.75 um pixels, 1.93"/px, rotated 30 degrees; each frame is a 2 s exposure that sees the
// star's position averaged over the exposure plus Gaussian seeing of 0.5" per image axis,
// then 0.5 s of download and star finding. The mount: each pulse moves its axis at 0.5x
// sidereal (7.52"/s) for its length while the sky keeps moving, and the next exposure starts
// after the pulses, as in the guider (#119). Calibration is measured on the mount model the
// way Scope measures it, without noise.
//
// The error of a frame is the true star position averaged over its exposure, what an imaging
// camera on the same mount would see; the RMS is taken about the lock position after a 60 s
// warm-up over 1440 s (three PE periods, about 540 frames).
//
// What this catches: a sign error in the chain (a flipped calibration makes guiding diverge),
// gain or gating regressions in the kernels or the pulse planning (the guided RMS leaves its
// band), and guiding that does not beat no guiding.
// What it cannot prove: that the defaults are well tuned for a real mount, real seeing,
// backlash or undersampled stars. That needs rig data.

#include <gtest/gtest.h>

#include "calibration_transform.h"
#include "guide_algorithm_kernels.h"
#include "guide_move_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

using namespace guide_move_plan;

namespace
{
constexpr double DEG = M_PI / 180.0;
constexpr unsigned GUIDE_STEP = ALGO_RESULT | USE_BLC | GRAPH; // MOVEOPTS_GUIDE_STEP

constexpr double ARCSEC_PER_PX = 206.265 * 3.75 / 400.0; // 1.934"/px
constexpr double GUIDE_ARCSEC_PER_MS = 0.5 * 15.0411 / 1000.0; // 0.5x sidereal

struct Scenario
{
    double peAmplitude = 10.0; // arcsec, RA
    double pePeriod = 480.0; // s
    double raDrift = 0.01; // arcsec/s
    double decDrift = 0.02; // arcsec/s
    double seeing = 0.5; // arcsec, sigma per image axis per frame
    double exposure = 2.0; // s
    double overhead = 0.5; // s, download and star finding before the guide step
    double duration = 3 * 480.0; // s
    double warmup = 60.0; // s left out of the RMS
    double cameraDeg = 30.0; // image angle of the star's motion on a West pulse
    bool guide = true;
    guide_algorithm_kernels::HysteresisParams ra; // the production defaults
    guide_algorithm_kernels::ResistSwitchParams dec;
    bool flipRaCalibration = false; // calibration xAngle off by 180 degrees: the RA guide sign is wrong
    bool flipDecCalibration = false; // calibration yAngle off by 180 degrees: the Dec guide sign is wrong
    uint64_t seed = 1;
};

struct Outcome
{
    double raRms = 0.; // arcsec
    double decRms = 0.;
    double totalRms = 0.;
    double maxError = 0.; // arcsec, largest |error| after the warm-up
    int frames = 0;
    int raPulses = 0;
    int decPulses = 0;
};

// Gaussian deviates from mt19937_64 by Box-Muller, the same on every standard library
class Gauss
{
public:
    explicit Gauss(uint64_t seed) : m_rng(seed) { }

    double Next()
    {
        double u1 = ((double) (m_rng() >> 11) + 0.5) * 0x1.0p-53;
        double u2 = (double) (m_rng() >> 11) * 0x1.0p-53;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    }

private:
    std::mt19937_64 m_rng;
};

struct Pt
{
    double x = 0.;
    double y = 0.;
};

class Simulation
{
public:
    explicit Simulation(const Scenario& sc)
        : m_sc(sc), m_west { std::cos(sc.cameraDeg * DEG), std::sin(sc.cameraDeg * DEG) },
          m_north { std::cos((sc.cameraDeg - 90.) * DEG), std::sin((sc.cameraDeg - 90.) * DEG) }, m_noise(sc.seed)
    {
    }

    Outcome Run()
    {
        Calibrate();

        double yAngleError = calibration_transform::YAngleError(m_xAngle, m_yAngle);
        guide_algorithm_kernels::HysteresisState raState;
        guide_algorithm_kernels::ResistSwitchState decState;
        AxisPolicy policy;
        policy.guidingEnabled = true;
        policy.decMode = DecMode::Auto;
        policy.maxRaMs = 2500;
        policy.maxDecMs = 2500;
        LimitTracker raLimit;
        LimitTracker decLimit;

        Outcome out;
        double sumRa2 = 0.;
        double sumDec2 = 0.;
        double t = 0.;
        while (t + m_sc.exposure <= m_sc.duration)
        {
            // expose: the star averaged over the exposure, with seeing
            double ra = MeanSkyRa(t, t + m_sc.exposure) + m_raCorrection;
            double dec = MeanSkyDec(t, t + m_sc.exposure) + m_decCorrection;
            if (t >= m_sc.warmup)
            {
                sumRa2 += ra * ra;
                sumDec2 += dec * dec;
                out.maxError = std::max(out.maxError, std::hypot(ra, dec));
                out.frames++;
            }
            Pt star = Image(ra, dec);
            star.x += m_noise.Next() * m_sc.seeing / ARCSEC_PER_PX;
            star.y += m_noise.Next() * m_sc.seeing / ARCSEC_PER_PX;
            t += m_sc.exposure + m_sc.overhead;

            if (!m_sc.guide)
                continue;

            // guide step
            Pt lock = Image(0., 0.);
            calibration_transform::CamToMount m =
                calibration_transform::CameraToMount(star.x - lock.x, star.y - lock.y, m_xAngle, yAngleError);
            double x = guide_algorithm_kernels::HysteresisResult(raState, m_sc.ra, m.mount.x);
            double y = guide_algorithm_kernels::ResistSwitchResult(decState, m_sc.dec, m.mount.y).result;
            OffsetPlan plan = PlanOffset(x, y, m_xRate, m_yRate);
            AxisPlan raPlan = PlanAxis(plan.raDir, plan.raMs, GUIDE_STEP, policy, raLimit, decLimit);
            t += Pulse(plan.raDir, raPlan.ms);
            out.raPulses += raPlan.ms > 0;
            AxisPlan decPlan = PlanAxis(plan.decDir, plan.decMs, GUIDE_STEP, policy, raLimit, decLimit);
            t += Pulse(plan.decDir, decPlan.ms);
            out.decPulses += decPlan.ms > 0;
        }

        out.raRms = std::sqrt(sumRa2 / out.frames);
        out.decRms = std::sqrt(sumDec2 / out.frames);
        out.totalRms = std::hypot(out.raRms, out.decRms);
        return out;
    }

private:
    // The sky's own motion of the star on each axis, arcsec, and its mean over [t0, t1]
    double MeanSkyRa(double t0, double t1) const
    {
        double w = 2.0 * M_PI / m_sc.pePeriod;
        double pe = m_sc.peAmplitude * (std::cos(w * t0) - std::cos(w * t1)) / (w * (t1 - t0));
        return pe + m_sc.raDrift * 0.5 * (t0 + t1);
    }

    double MeanSkyDec(double t0, double t1) const { return m_sc.decDrift * 0.5 * (t0 + t1); }

    // Where a star at these axis errors (arcsec) is on the sensor
    Pt Image(double ra, double dec) const
    {
        Pt p { 512., 384. };
        p.x += (ra * m_west.x + dec * m_north.x) / ARCSEC_PER_PX;
        p.y += (ra * m_west.y + dec * m_north.y) / ARCSEC_PER_PX;
        return p;
    }

    // Moves the axis at the guide rate; returns the seconds the pulse took
    double Pulse(Dir dir, int ms)
    {
        double arcsec = GUIDE_ARCSEC_PER_MS * ms;
        switch (dir)
        {
        case Dir::West:
            m_raCorrection += arcsec;
            break;
        case Dir::East:
            m_raCorrection -= arcsec;
            break;
        case Dir::North:
            m_decCorrection += arcsec;
            break;
        case Dir::South:
            m_decCorrection -= arcsec;
            break;
        case Dir::None:
            return 0.;
        }
        return ms / 1000.0;
    }

    // The angles and rates Scope::UpdateCalibrationState measures (see
    // test_guide_sign_closed_loop.cpp): xAngle is start.Angle(end) of the West leg, yAngle is
    // end.Angle(start) of the North leg, each rate the distance over the leg's pulse time.
    // The calibration runs on the mount model alone, with the sky held still.
    void Calibrate()
    {
        const int steps = 12;
        const int stepMs = 550;
        Pt start = Image(0., 0.);
        Pt west = Image(GUIDE_ARCSEC_PER_MS * steps * stepMs, 0.);
        Pt north = Image(0., GUIDE_ARCSEC_PER_MS * steps * stepMs);
        m_xAngle = std::atan2(start.y - west.y, start.x - west.x);
        m_xRate = std::hypot(west.x - start.x, west.y - start.y) / (steps * stepMs);
        m_yAngle = std::atan2(north.y - start.y, north.x - start.x);
        m_yRate = std::hypot(north.x - start.x, north.y - start.y) / (steps * stepMs);
        if (m_sc.flipRaCalibration)
            m_xAngle += M_PI;
        if (m_sc.flipDecCalibration)
            m_yAngle += M_PI;
    }

    Scenario m_sc;
    Pt m_west; // unit image vector of a West move
    Pt m_north; // unit image vector of a North move
    Gauss m_noise;
    double m_raCorrection = 0.; // arcsec the pulses have moved each axis
    double m_decCorrection = 0.;
    double m_xAngle = 0.;
    double m_xRate = 0.;
    double m_yAngle = 0.;
    double m_yRate = 0.;
};

Outcome Simulate(const Scenario& sc)
{
    return Simulation(sc).Run();
}

// The baseline (#142). Measured on 2026-11-20 on the arm64 dev VM with GCC 14.2 by running
// this simulation with the production defaults, in a -O0 build and a -O3 build, which agree to
// the fourth decimal. The guided RMS, seeds 1-20:
//   total 0.723-0.833", mean 0.777"; RA 0.488-0.541", mean 0.510"; Dec 0.527-0.669", mean 0.586"
// and seeds 1-60: total 0.713-0.833". Unguided, the same sky gives 19.89" total: RA 10.36" (the
// PE's 7.07" and the drift) and Dec 16.98" (0.02"/s over 60-1440 s), the same for every seed,
// since seeing only reaches the measurement.
// The bands on the 20-seed means are about +/-11 % around them. That leaves room for libm and
// FMA differences between compilers, which can move a seed's trajectory by a frame here and
// there, and still catches a detuned default (BandCatchesDetunedDefaults: halving or doubling
// the RA aggression, minMove 0.5 px, hysteresis 0.5 or Dec aggression 0.5 each leave it). The
// per-seed band is wider, for one seed's luck.
constexpr int SEEDS = 20;
constexpr double TOTAL_MEAN = 0.777, TOTAL_MEAN_LO = 0.69, TOTAL_MEAN_HI = 0.86;
constexpr double RA_MEAN_LO = 0.455, RA_MEAN_HI = 0.565;
constexpr double DEC_MEAN_LO = 0.52, DEC_MEAN_HI = 0.65;
constexpr double TOTAL_SEED_LO = 0.62, TOTAL_SEED_HI = 0.95;

struct Means
{
    double total = 0.;
    double ra = 0.;
    double dec = 0.;
};

Means MeanOverSeeds(Scenario sc)
{
    Means m;
    for (uint64_t seed = 1; seed <= SEEDS; seed++)
    {
        sc.seed = seed;
        Outcome o = Simulate(sc);
        m.total += o.totalRms / SEEDS;
        m.ra += o.raRms / SEEDS;
        m.dec += o.decRms / SEEDS;
    }
    return m;
}

bool InBand(const Means& m)
{
    return m.total >= TOTAL_MEAN_LO && m.total <= TOTAL_MEAN_HI && m.ra >= RA_MEAN_LO && m.ra <= RA_MEAN_HI &&
        m.dec >= DEC_MEAN_LO && m.dec <= DEC_MEAN_HI;
}

} // namespace

TEST(GuidingRmsBaseline, UnguidedSkyIsAsDesigned)
{
    // nothing random reaches the true error without guiding. Dec: 0.02 t over t = 60..1440 s
    // has an RMS of 0.02 sqrt((1440^3 - 60^3) / (3 x 1380)) = 16.98"
    Scenario sc;
    sc.guide = false;
    Outcome u = Simulate(sc);
    EXPECT_NEAR(u.raRms, 10.36, 0.01);
    EXPECT_NEAR(u.decRms, 16.98, 0.01);
    EXPECT_NEAR(u.totalRms, 19.89, 0.01);
    EXPECT_GE(u.frames, 530);
    EXPECT_EQ(u.raPulses + u.decPulses, 0);
}

TEST(GuidingRmsBaseline, GuidedRmsIsWellBelowUnguided)
{
    for (uint64_t seed = 1; seed <= SEEDS; seed++)
    {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        Scenario sc;
        sc.seed = seed;
        Outcome g = Simulate(sc);
        sc.guide = false;
        Outcome u = Simulate(sc);
        EXPECT_LT(g.totalRms, 0.1 * u.totalRms);
        EXPECT_LT(g.raRms, 0.15 * u.raRms);
        EXPECT_LT(g.decRms, 0.1 * u.decRms);
        // the PE alone is 10 / sqrt(2) = 7.07" RMS
        EXPECT_LT(g.raRms, 0.15 * 10. / std::sqrt(2.));
        EXPECT_LT(g.maxError, 3.0);
        // both axes are guided
        EXPECT_GT(g.raPulses, 200);
        EXPECT_GT(g.decPulses, 80);
    }
}

TEST(GuidingRmsBaseline, GuidedRmsStaysInItsBaselineBand)
{
    Means m = MeanOverSeeds(Scenario());
    EXPECT_GE(m.total, TOTAL_MEAN_LO);
    EXPECT_LE(m.total, TOTAL_MEAN_HI);
    EXPECT_NEAR(m.total, TOTAL_MEAN, 0.11 * TOTAL_MEAN);
    EXPECT_GE(m.ra, RA_MEAN_LO);
    EXPECT_LE(m.ra, RA_MEAN_HI);
    EXPECT_GE(m.dec, DEC_MEAN_LO);
    EXPECT_LE(m.dec, DEC_MEAN_HI);
}

TEST(GuidingRmsBaseline, GuidedRmsIsStableAcrossSeeds)
{
    double lo = 1e9;
    double hi = 0.;
    for (uint64_t seed = 1; seed <= 3 * SEEDS; seed++)
    {
        SCOPED_TRACE(testing::Message() << "seed " << seed);
        Scenario sc;
        sc.seed = seed;
        Outcome g = Simulate(sc);
        EXPECT_GE(g.totalRms, TOTAL_SEED_LO);
        EXPECT_LE(g.totalRms, TOTAL_SEED_HI);
        lo = std::min(lo, g.totalRms);
        hi = std::max(hi, g.totalRms);
    }
    EXPECT_LT(hi / lo, 1.35);
}

TEST(GuidingRmsBaseline, BandCatchesDetunedDefaults)
{
    // The band is not vacuous: each of these leaves it
    struct Detune
    {
        const char *what;
        void (*apply)(Scenario&);
    };
    const Detune detunes[] = {
        { "RA aggression 0.35", [](Scenario& sc) { sc.ra.aggression = 0.35; } },
        { "RA aggression 1.4", [](Scenario& sc) { sc.ra.aggression = 1.4; } },
        { "RA minMove 0.5 px", [](Scenario& sc) { sc.ra.minMove = 0.5; } },
        { "RA hysteresis 0.5", [](Scenario& sc) { sc.ra.hysteresis = 0.5; } },
        { "Dec aggression 0.5", [](Scenario& sc) { sc.dec.aggression = 0.5; } },
    };
    for (const Detune& d : detunes)
    {
        Scenario sc;
        d.apply(sc);
        Means m = MeanOverSeeds(sc);
        EXPECT_FALSE(InBand(m)) << d.what << ": total " << m.total << " RA " << m.ra << " Dec " << m.dec;
    }
}

TEST(GuidingRmsBaseline, FlippedRaCalibrationDiverges)
{
    // An RA calibration 180 degrees off turns every RA correction around: the error runs away
    // at the max pulse a frame, while Dec still guides
    Scenario sc;
    sc.guide = false;
    Outcome u = Simulate(sc);
    sc.guide = true;
    sc.flipRaCalibration = true;
    Outcome f = Simulate(sc);
    EXPECT_GT(f.raRms, 10. * u.raRms);
    EXPECT_GT(f.maxError, 1000.);
    EXPECT_LT(f.decRms, DEC_MEAN_HI * 1.5);
}

TEST(GuidingRmsBaseline, FlippedDecCalibrationDiverges)
{
    Scenario sc;
    sc.guide = false;
    Outcome u = Simulate(sc);
    sc.guide = true;
    sc.flipDecCalibration = true;
    Outcome f = Simulate(sc);
    EXPECT_GT(f.decRms, 10. * u.decRms);
    EXPECT_GT(f.maxError, 1000.);
    EXPECT_LT(f.raRms, RA_MEAN_HI * 1.5);
}
