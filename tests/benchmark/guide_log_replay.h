// Replays a PHD2 guide log through the wx-free guide algorithm kernels (#142 Tier 4).
//
// Two things per guiding session of a log (guide_log.h):
//
// Reproduction, open loop. Each frame's logged mount offset (RARawDistance, DECRawDistance: what
// Mount::MoveOffset fed the algorithms) goes through the algorithm and parameters the session
// header names, with the resets production does (a dither, guiding enabled again). The kernel's
// guide distance is compared with the logged RAGuideDistance / DECGuideDistance, and the pulse
// PlanOffset and PlanAxis make of it (the header's rates, limits and Dec guide mode) with the
// logged RADuration / DECDuration. The log prints the offsets and distances to 0.001 px and the
// rates to 0.001 px/s, so the comparison has a tolerance; a step whose input is within the
// rounding of a threshold (minMove) can go either way and is counted apart, not as a mismatch.
//
// Prediction, closed loop. Within each stretch between resets, the logged offsets plus the logged
// pulses (rate x duration, signed) rebuild the star's uncorrected motion: drift, periodic error and
// seeing. Another algorithm is then run against that motion in a loop, its own pulses moving the
// star, which predicts the residual it would have left. Replaying the logged algorithm this way gives
// the logged offsets back, which checks the reconstruction. The prediction assumes the mount moves
// exactly rate x duration at once: backlash, stiction and rate errors are not in it, so it ranks
// algorithms on the night's sky rather than promising an RMS.

#ifndef GUIDE_LOG_REPLAY_H_INCLUDED
#define GUIDE_LOG_REPLAY_H_INCLUDED

#include "guide_log.h"
#include "guide_sim.h"

#include <memory>
#include <string>
#include <vector>

namespace guide_log_replay
{

struct Options
{
    double distanceTolerance = 0.002; // px: the log's 0.0005 rounding on the input and on the output, and the state
    double rateRounding = 0.0005; // px/s: the header's %.3f rates
    int maxMismatchesKept = 20;
};

struct Mismatch
{
    int line = 0;
    int frame = 0;
    char axis = 'R'; // 'R' or 'D'
    double input = 0.;
    double logged = 0.;
    double replayed = 0.;
    int loggedMs = 0;
    int replayedMs = 0;
};

struct AxisReproduction
{
    bool supported = false; // the kernel set has this algorithm (not ZFilter, Predictive PEC or None)
    std::string algorithm;
    int steps = 0; // frames whose guide distance was checked
    int distanceMatches = 0;
    int thresholdAmbiguous = 0; // within the rounding of minMove: either answer is right
    double maxDistanceError = 0.; // px, over the matched and mismatched steps (not the ambiguous ones)
    int durationsChecked = 0;
    int durationMatches = 0;
    int unsent = 0; // a pulse planned and logged as 0 ms: the mount did not take it (not counted as checked)
    std::vector<Mismatch> mismatches; // the first Options::maxMismatchesKept
};

// Standard deviations about the mean (what PHD2 reports as RMS), px, over the frames the
// prediction counts: guided frames outside settling
struct Rms
{
    double ra = 0.;
    double dec = 0.;
    int frames = 0;
};

struct Prediction
{
    std::string algorithm; // its name and parameters
    double rms = 0.; // px, on the axis it was run on
};

struct SessionReport
{
    int line = 0;
    std::string begins;
    double pixelScale = 0.; // arcsec/px
    int frames = 0;
    AxisReproduction ra;
    AxisReproduction dec;
    Rms logged; // the guide log's own offsets
    Rms selfReplay; // the logged algorithms, closed loop on the reconstruction: should be the logged
    double selfReplayMaxError = 0.; // px, largest |replayed - logged offset|
    Rms uncorrected; // the reconstructed motion with no guiding at all
    std::vector<Prediction> raPredictions;
    std::vector<Prediction> decPredictions;
};

// The algorithm of one axis as a header names it, with its parameters; null for one the kernels do
// not have
std::unique_ptr<guide_sim::AxisAlgorithm> MakeAlgorithm(const guide_log::Algorithm& logged);

SessionReport ReplaySession(const guide_log::Session& session, const Options& options = Options());

// Markdown report of a log's sessions (sessions with no frames are left out)
std::string Markdown(const std::string& title, const std::vector<SessionReport>& reports);

} // namespace guide_log_replay

#endif // GUIDE_LOG_REPLAY_H_INCLUDED
