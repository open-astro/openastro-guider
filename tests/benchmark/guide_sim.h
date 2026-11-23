// A seeded closed-loop guiding simulation for benchmarks (#142 Tier 4).
//
// The model of tests/guiding/test_guiding_rms_baseline.cpp, widened for the algorithm matrix:
// the guide algorithm of each axis is chosen (Hysteresis, Lowpass, Lowpass2 or ResistSwitch, each
// through its REAL wx-free kernel with the production constructor's defaults), and the mount can
// have Dec backlash. One frame:
//
//   the star, averaged over the exposure, plus Gaussian seeing per image axis
//   cameraOfs = measured star - lock
//   mountOfs = CameraToMount(cameraOfs, xAngle, yErr)                 (calibration_transform)
//   x = RA algorithm(mountOfs.x), y = Dec algorithm(mountOfs.y)       (guide_algorithm_kernels,
//                                                                      guide_algorithm_lowpass_math)
//   pulses = PlanOffset(x, y, xRate, yRate), each through PlanAxis    (guide_move_plan)
//
// The sky: a sinusoidal RA periodic error and constant RA and Dec drifts, in arcsec, from the lock
// at t = 0. The mount moves each axis at 0.5x sidereal for a pulse's length while the sky keeps
// moving; Dec backlash is dead time, the first decBacklashMs of Dec pulse after a reversal moving
// nothing (the mount starts engaged South, as the fake Alpaca mount does). The calibration is
// measured on the mount model without noise or backlash, as a backlash-clearing calibration would.
//
// The error of a frame is the true star position averaged over its exposure (what an imaging
// camera on the same mount sees), and the RMS is about the lock position after the warm-up.
//
// What it cannot prove: that a tuning is right for a real mount (stiction, rate errors, wind),
// real seeing or undersampled stars. It ranks algorithms on a model and catches regressions.

#ifndef GUIDE_SIM_H_INCLUDED
#define GUIDE_SIM_H_INCLUDED

#include "guide_algorithm_kernels.h"
#include "guiding_stats.h"

#include <cstdint>
#include <random>
#include <string>

namespace guide_sim
{

enum class Algo
{
    None, // no guiding on this axis
    Hysteresis,
    Lowpass,
    Lowpass2,
    ResistSwitch,
};

// "Hysteresis", "Lowpass", "Lowpass2", "ResistSwitch", "None"
const char *AlgoName(Algo a);
// The inverse of AlgoName (also "Resist Switch", the daemon's name); false for an unknown name
bool AlgoFromName(const std::string& name, Algo *a);

// One guide algorithm on one axis, with the production constructor's defaults: Hysteresis
// minMove 0.2, hysteresis 0.1, aggression 0.7; ResistSwitch minMove 0.2, aggression 1.0, fast
// switch on; Lowpass minMove 0.2, slope weight 5; Lowpass2 minMove 0.2, aggressiveness 80.
class AxisAlgorithm
{
public:
    explicit AxisAlgorithm(Algo algo);

    Algo Kind() const { return m_algo; }

    // GuideAlgorithm::result(): the guide distance for a raw mount-axis distance, px
    double Result(double input);

    // GuideAlgorithm::reset()
    void Reset();

    guide_algorithm_kernels::HysteresisParams hysteresis;
    guide_algorithm_kernels::ResistSwitchParams resistSwitch;
    double lowpassMinMove = 0.2;
    double lowpassSlopeWeight = 5.0;
    double lowpass2MinMove = 0.2;
    double lowpass2Aggressiveness = 80.0;

private:
    Algo m_algo;
    guide_algorithm_kernels::HysteresisState m_hysteresis;
    guide_algorithm_kernels::ResistSwitchState m_resistSwitch;
    WindowedAxisStats m_stats;
    int m_timeBase = 0;
    int m_rejects = 0;
};

struct Scenario
{
    double peAmplitude = 10.0; // arcsec, RA
    double pePeriod = 480.0; // s
    double raDrift = 0.01; // arcsec/s
    double decDrift = 0.02; // arcsec/s
    double seeing = 0.5; // arcsec, sigma per image axis per frame
    double decBacklashMs = 0.0; // ms of Dec pulse lost after each reversal
    double exposure = 2.0; // s
    double overhead = 0.5; // s, download and star finding before the guide step
    double duration = 3 * 480.0; // s
    double warmup = 60.0; // s left out of the RMS
    double cameraDeg = 30.0; // image angle of the star's motion on a West pulse
    bool guide = true;
    Algo raAlgo = Algo::Hysteresis;
    Algo decAlgo = Algo::ResistSwitch;
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

Outcome Simulate(const Scenario& sc);

// Gaussian deviates from mt19937_64 by Box-Muller, the same on every standard library
class Gauss
{
public:
    explicit Gauss(uint64_t seed) : m_rng(seed) { }

    double Next();

private:
    std::mt19937_64 m_rng;
};

} // namespace guide_sim

#endif // GUIDE_SIM_H_INCLUDED
