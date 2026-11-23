// A seeded closed-loop guiding simulation for benchmarks (#142 Tier 4). See guide_sim.h.

#include "guide_sim.h"

#include "calibration_transform.h"
#include "guide_algorithm_lowpass_math.h"
#include "guide_move_plan.h"

#include <algorithm>
#include <cmath>

using namespace guide_move_plan;

namespace guide_sim
{

namespace
{
    constexpr double DEG = M_PI / 180.0;
    constexpr unsigned GUIDE_STEP = ALGO_RESULT | USE_BLC | GRAPH; // MOVEOPTS_GUIDE_STEP

    constexpr double ARCSEC_PER_PX = 206.265 * 3.75 / 400.0; // 1.934"/px
    constexpr double GUIDE_ARCSEC_PER_MS = 0.5 * 15.0411 / 1000.0; // 0.5x sidereal

    // GuideAlgorithmLowpass::HISTORY_SIZE and GuideAlgorithmLowpass2::HISTORY_SIZE
    constexpr int LOWPASS_HISTORY = 10;

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
            AxisAlgorithm raAlgo(m_sc.raAlgo);
            AxisAlgorithm decAlgo(m_sc.decAlgo);
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
                double x = raAlgo.Result(m.mount.x);
                double y = decAlgo.Result(m.mount.y);
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

        // The ms of a Dec pulse that move the mount, less the backlash a reversal takes up
        double DecMovingMs(bool north, int ms)
        {
            if (m_sc.decBacklashMs <= 0.)
                return ms;
            if (north != m_decEngagedNorth)
            {
                m_decEngagedNorth = north;
                m_backlashLeftMs = m_sc.decBacklashMs;
            }
            double taken = std::min((double) ms, m_backlashLeftMs);
            m_backlashLeftMs -= taken;
            return ms - taken;
        }

        // Moves the axis at the guide rate; returns the seconds the pulse took
        double Pulse(Dir dir, int ms)
        {
            switch (dir)
            {
            case Dir::West:
                m_raCorrection += GUIDE_ARCSEC_PER_MS * ms;
                break;
            case Dir::East:
                m_raCorrection -= GUIDE_ARCSEC_PER_MS * ms;
                break;
            case Dir::North:
                if (ms > 0)
                    m_decCorrection += GUIDE_ARCSEC_PER_MS * DecMovingMs(true, ms);
                break;
            case Dir::South:
                if (ms > 0)
                    m_decCorrection -= GUIDE_ARCSEC_PER_MS * DecMovingMs(false, ms);
                break;
            case Dir::None:
                return 0.;
            }
            return ms / 1000.0;
        }

        // The angles and rates Scope::UpdateCalibrationState measures: xAngle is start.Angle(end) of
        // the West leg, yAngle is end.Angle(start) of the North leg, each rate the distance over the
        // leg's pulse time. The calibration runs on the mount model alone, with the sky held still.
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
        }

        Scenario m_sc;
        Pt m_west; // unit image vector of a West move
        Pt m_north; // unit image vector of a North move
        Gauss m_noise;
        double m_raCorrection = 0.; // arcsec the pulses have moved each axis
        double m_decCorrection = 0.;
        bool m_decEngagedNorth = false; // the Dec gears start engaged South
        double m_backlashLeftMs = 0.;
        double m_xAngle = 0.;
        double m_xRate = 0.;
        double m_yAngle = 0.;
        double m_yRate = 0.;
    };

} // namespace

double Gauss::Next()
{
    double u1 = ((double) (m_rng() >> 11) + 0.5) * 0x1.0p-53;
    double u2 = (double) (m_rng() >> 11) * 0x1.0p-53;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
}

const char *AlgoName(Algo a)
{
    switch (a)
    {
    case Algo::Hysteresis:
        return "Hysteresis";
    case Algo::Lowpass:
        return "Lowpass";
    case Algo::Lowpass2:
        return "Lowpass2";
    case Algo::ResistSwitch:
        return "ResistSwitch";
    case Algo::None:
        break;
    }
    return "None";
}

bool AlgoFromName(const std::string& name, Algo *a)
{
    for (Algo k : { Algo::None, Algo::Hysteresis, Algo::Lowpass, Algo::Lowpass2, Algo::ResistSwitch })
    {
        if (name == AlgoName(k))
        {
            *a = k;
            return true;
        }
    }
    if (name == "Resist Switch")
    {
        *a = Algo::ResistSwitch;
        return true;
    }
    return false;
}

AxisAlgorithm::AxisAlgorithm(Algo algo) : m_algo(algo)
{
    // GuideAlgorithmLowpass keeps its own window, GuideAlgorithmLowpass2 an auto-windowed one
    if (algo == Algo::Lowpass2)
        m_stats = WindowedAxisStats(LOWPASS_HISTORY);
    else
        m_stats = WindowedAxisStats(0);
    Reset();
}

void AxisAlgorithm::Reset()
{
    switch (m_algo)
    {
    case Algo::Hysteresis:
        m_hysteresis = guide_algorithm_kernels::HysteresisState();
        break;
    case Algo::ResistSwitch:
        guide_algorithm_kernels::ResistSwitchReset(m_resistSwitch);
        break;
    case Algo::Lowpass:
        guide_lowpass::Reset(m_stats, m_timeBase, LOWPASS_HISTORY);
        break;
    case Algo::Lowpass2:
        guide_lowpass2::Reset(m_stats, m_timeBase, m_rejects);
        break;
    case Algo::None:
        break;
    }
}

double AxisAlgorithm::Result(double input)
{
    switch (m_algo)
    {
    case Algo::Hysteresis:
        return guide_algorithm_kernels::HysteresisResult(m_hysteresis, hysteresis, input);
    case Algo::ResistSwitch:
        return guide_algorithm_kernels::ResistSwitchResult(m_resistSwitch, resistSwitch, input).result;
    case Algo::Lowpass:
        return guide_lowpass::Compute(m_stats, m_timeBase, lowpassSlopeWeight, lowpassMinMove, input).value;
    case Algo::Lowpass2:
        return guide_lowpass2::Compute(m_stats, m_timeBase, m_rejects, lowpass2Aggressiveness, lowpass2MinMove, input).value;
    case Algo::None:
        break;
    }
    return 0.;
}

Outcome Simulate(const Scenario& sc)
{
    return Simulation(sc).Run();
}

} // namespace guide_sim
