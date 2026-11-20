/*
 *  settle_frame.cpp
 *  PHD Guiding
 *
 *  See settle_frame.h.
 */

#include "settle_frame.h"

namespace SettleFrame
{
Result OnFrame(Progress& progress, const SettleParams& params, const Frame& frame)
{
    Result result;
    // #136: a dropped frame counts like a lost-star frame
    result.lockedOnStar = !frame.dropped && frame.locked;
    bool inRange = result.lockedOnStar && frame.error <= params.tolerancePx;

    ++progress.frameCount;
    if (!result.lockedOnStar)
        ++progress.droppedFrameCount;

    if (progress.frameCount >= params.frames)
    {
        result.outcome = Outcome::Settled;
        return result;
    }

    if (inRange)
    {
        if (!progress.priorFrameInRange)
        {
            // the first frame of a run
            if (params.settleTimeSec <= 0)
            {
                result.outcome = Outcome::Settled;
                return result;
            }
            result.startInRangeRun = true;
        }
        else
        {
            result.timeInRangeMs = frame.inRangeMs;
            if (frame.inRangeMs / 1000 >= params.settleTimeSec && !frame.aoBumpInProgress)
            {
                result.outcome = Outcome::Settled;
                return result;
            }
        }
    }
    else if (frame.dropped && progress.priorFrameInRange)
        result.timeInRangeMs = frame.inRangeMs; // #136: the run goes on

    if (frame.settlingMs / 1000 >= params.timeoutSec)
    {
        result.outcome = Outcome::TimedOut;
        return result;
    }

    // #136: a dropped frame neither ends nor completes an in-range run (it saw no star), while the
    // settle timeout above keeps running
    if (!frame.dropped)
        progress.priorFrameInRange = inRange;
    result.outcome = Outcome::Settling;
    return result;
}
} // namespace SettleFrame
