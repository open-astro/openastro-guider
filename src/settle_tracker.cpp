/*
 *  settle_tracker.cpp
 *  PHD Guiding
 *
 *  See settle_tracker.h.
 */

#include "settle_tracker.h"

#include <chrono>

int64_t SettleTracker::ProductionClockMs()
{
    // The wall clock, as wxStopWatch is: a step of the system time moves both settle timers
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
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
