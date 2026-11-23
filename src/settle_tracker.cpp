/*
 *  settle_tracker.cpp
 *  PHD Guiding
 *
 *  See settle_tracker.h.
 */

#include "settle_tracker.h"

#include "monotonic_timers.h"

int64_t SettleTracker::ProductionClockMs()
{
    // Not the wall clock, which wxStopWatch read: a step of the system time (a Pi syncing a stale clock
    // at boot) settled a run or timed the settle out on the next frame, or held both for the size of
    // the step (#257)
    return monotonic_timers::ClockMs();
}

SettleTracker::SettleTracker(ClockMs clock) : m_clock(clock) { }

void SettleTracker::ResetCounts()
{
    m_progress.frameCount = 0;
    m_progress.droppedFrameCount = 0;
}

void SettleTracker::Begin()
{
    m_progress = SettleFrame::Progress();
    m_settleStartMs = m_clock();
}

SettleFrame::Result SettleTracker::OnFrame(const SettleParams& params, bool dropped, bool locked, double error,
                                           bool aoBumpInProgress)
{
    int64_t now = m_clock();

    SettleFrame::Frame frame;
    frame.dropped = dropped;
    frame.locked = locked;
    frame.error = error;
    frame.aoBumpInProgress = aoBumpInProgress;
    frame.inRangeMs = static_cast<long>(now - m_inRangeStartMs);
    frame.settlingMs = static_cast<long>(now - m_settleStartMs);

    SettleFrame::Result result = SettleFrame::OnFrame(m_progress, params, frame);
    if (result.startInRangeRun)
        m_inRangeStartMs = now;
    return result;
}
