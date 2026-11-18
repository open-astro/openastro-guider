/*
 *  staticpa_step_gate.h
 *  PHD Guiding
 *
 *  Keeps Static PA from star-tracking frames exposed while one of its own
 *  auto-mode RA steps is moving (#98). The capture loop keeps running during a
 *  step, so the frame exposed during the slew has a smeared star (about 1/80
 *  of its mass, the centroid 30-40 px off). Tracking it either drags the
 *  reference star or loses it, and a star loss aborts the run. While Static PA
 *  is aligning in auto mode, the tool asks the mount whether it is slewing on
 *  every frame before the frame is tracked and passes the answer here.
 *
 *  The gate arms when the mount reads slewing, or when the tool commands a
 *  step (a mount may report the slew late, or not at all). On the first frame
 *  that reads not slewing after that, it records the moment the end was seen
 *  and ignores that frame, whose exposure began before the end was seen. A
 *  later frame is tracked again only when it reads not slewing and its exposure
 *  started at or after that moment plus the settle time (within kClockSlackMs).
 *  Ignored frames are shown and sent to clients, but not measured.
 *
 *  Kept free of wx so it can be unit tested
 *  (tests/polar_alignment/test_staticpa_step_gate.cpp). Times are milliseconds
 *  on a steady clock.
 */

#ifndef STATICPA_STEP_GATE_H_INCLUDED
#define STATICPA_STEP_GATE_H_INCLUDED

#include <cstdint>
#include <limits>

namespace staticpa_step
{
// Added to the moment the end of a step slew was seen. Zero: that moment is
// already an upper bound on when the slew actually ended, and any value above
// a few ms would also drop the frame the capture loop starts right after it.
constexpr std::int64_t kSettleMs = 0;

// Allowed when comparing a frame's start with the moment the end was seen. The
// two come from different clocks (a frame start is mapped from the wall clock)
// read at different instants and truncated to whole ms, which is good to about
// +/-2 ms, while the next frame starts well under 1 ms after the end-seen frame
// was processed. Far below any exposure plus download.
constexpr std::int64_t kClockSlackMs = 5;

// What FrameStartOnSteadyClock returns when the start cannot be mapped: earlier
// than any end-seen time, so the gate never releases that frame.
constexpr std::int64_t kUnknownFrameStart = std::numeric_limits<std::int64_t>::min() / 2;

enum class Phase
{
    Idle, // frames are tracked
    Slewing, // armed: waiting for a frame that reads not slewing
    EndSeen, // the end was seen: waiting for a frame that started after it
};

const char *PhaseName(Phase phase);

class StepGate
{
public:
    explicit StepGate(std::int64_t settleMs = kSettleMs);

    // A step slew was commanded. Goes to Slewing and forgets an earlier end.
    void Arm();
    // The run stopped or failed: frames are tracked again.
    void Reset();

    Phase phase() const { return m_phase; }
    // Valid in EndSeen and after the release that follows it.
    std::int64_t EndSeenMs() const { return m_endSeenMs; }

    // Called once per frame before the frame is star-tracked. mountSlewing is
    // the mount's Slewing() read for this frame, frameStartMs the frame's
    // exposure start and nowMs the time now, both on the same steady clock.
    // Returns true when the frame must not be star-tracked.
    bool IgnoreFrame(bool mountSlewing, std::int64_t frameStartMs, std::int64_t nowMs);

private:
    std::int64_t m_settleMs;
    Phase m_phase = Phase::Idle;
    std::int64_t m_endSeenMs = 0;
};

// Maps a frame's exposure start from the wall clock (usImage::ImgStartTime)
// onto the steady clock by its age: steadyNowMs - (wallNowMs - frameStartWallMs).
// A start later than wallNowMs (the wall clock stepped back) maps to
// kUnknownFrameStart, so the gate skips that frame rather than track one that
// may have overlapped the slew; the next frame's age is measured after the step.
std::int64_t FrameStartOnSteadyClock(std::int64_t frameStartWallMs, std::int64_t wallNowMs, std::int64_t steadyNowMs);

} // namespace staticpa_step

#endif
