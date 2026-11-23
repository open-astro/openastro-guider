/*
 *  settle_tracker.h
 *  PHD Guiding
 *
 *  A guide or dither settle as PhdController runs it (STATE_SETTLE_BEGIN and
 *  STATE_SETTLE_WAIT): the settle timeout, timed from the settle's start, the
 *  timer of the run of in-range frames, and the frame counts SettleDone
 *  reports. Each frame is decided by SettleFrame::OnFrame (settle_frame.cpp);
 *  this adds the two timers, on an injected clock so the tests can drive and
 *  step it. PhdController keeps the states, the events and the Dec guide mode.
 *  Kept free of wx so it can be unit tested (tests/guiding/test_settle_tracker.cpp).
 */

#ifndef SETTLE_TRACKER_H_INCLUDED
#define SETTLE_TRACKER_H_INCLUDED

#include "settle_frame.h"

#include <cstdint>

class SettleTracker
{
public:
    // A reading of the clock the settle is timed on, in milliseconds. Only the time between two
    // readings is used
    using ClockMs = int64_t (*)();

    // The clock PhdController's settle is timed on: monotonic_timers::ClockMs, the steady clock, so a
    // step of the system time moves neither timer (#257)
    static int64_t ProductionClockMs();

    explicit SettleTracker(ClockMs clock = &SettleTracker::ProductionClockMs);

    // A guide or dither request starts: TotalFrames and DroppedFrames are 0 until its settle
    // begins, so a request that ends before it settles reports 0 frames
    void ResetCounts();
    // The settle begins: no frames yet, no in-range run, and the settle timeout runs from now
    void Begin();
    // A frame of the settle: counts it and decides the settle (SettleFrame::OnFrame) on the
    // times read from the clock now, and times an in-range run from the frame that begins it
    SettleFrame::Result OnFrame(const SettleParams& params, bool dropped, bool locked, double error, bool aoBumpInProgress);

    int FrameCount() const { return m_progress.frameCount; } // SettleDone.TotalFrames
    int DroppedFrameCount() const { return m_progress.droppedFrameCount; } // SettleDone.DroppedFrames

private:
    ClockMs m_clock;
    SettleFrame::Progress m_progress;
    int64_t m_settleStartMs = 0;
    int64_t m_inRangeStartMs = 0;
};

#endif // SETTLE_TRACKER_H_INCLUDED
