/*
 *  settle_frame.h
 *  PHD Guiding
 *
 *  What one guide frame does to a guide or dither settle (PhdController's
 *  STATE_SETTLE_WAIT): the frame counts, the run of in-range frames, and
 *  whether the settle is done, timed out or goes on, including a frame dropped
 *  after a transient capture failure (#136).
 *  Kept free of wx so it can be unit tested (tests/guiding/test_settle_frame.cpp).
 */

#ifndef SETTLE_FRAME_H_INCLUDED
#define SETTLE_FRAME_H_INCLUDED

struct SettleParams
{
    double tolerancePx; // settle threshold, pixels
    int settleTimeSec; // time to be within tolerance
    int timeoutSec; // timeout value
    int frames; // number of frames
};

namespace SettleFrame
{
// The settle so far, kept by PhdController; all zero / false when the settle begins
struct Progress
{
    int frameCount = 0; // SettleDone.TotalFrames
    int droppedFrameCount = 0; // SettleDone.DroppedFrames
    bool priorFrameInRange = false; // an in-range run is under way, timed from its first frame
};

// The frame that just completed
struct Frame
{
    bool dropped = false; // #136: the capture failed and the frame was dropped while guiding
    bool locked = false; // the guider is locked on its star
    double error = 0.; // the current guide error, pixels
    bool aoBumpInProgress = false;
    long inRangeMs = 0; // time since the first frame of the in-range run (read when priorFrameInRange)
    long settlingMs = 0; // time since the settle began
};

enum class Outcome
{
    Settling, // the settle goes on: send a Settling event
    Settled, // the settle succeeded
    TimedOut, // the settle failed: timed out
};

struct Result
{
    Outcome outcome = Outcome::Settling;
    bool lockedOnStar = false; // the frame counts as locked on the star (Settling.StarLocked)
    bool startInRangeRun = false; // the first in-range frame of a run: start timing the run
    long timeInRangeMs = 0; // Settling.SettleTime
};

// Counts the frame and decides the settle. A frame not locked on the star (a lost star, or a dropped
// frame, which saw no star) is a dropped frame, ends a run of in-range frames, and cannot complete one;
// a dropped frame (#136) leaves the run as it was: the next in-range frame times the run from its first
// frame. The frame limit is checked first, then the in-range run, then the settle timeout.
Result OnFrame(Progress& progress, const SettleParams& params, const Frame& frame);
} // namespace SettleFrame

#endif
