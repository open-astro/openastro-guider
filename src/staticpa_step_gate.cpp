/*
 *  staticpa_step_gate.cpp
 *  PHD Guiding
 *
 *  See staticpa_step_gate.h (#98).
 */

#include "staticpa_step_gate.h"

namespace staticpa_step
{

const char *PhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::Idle:
        return "idle";
    case Phase::Slewing:
        return "slewing";
    case Phase::EndSeen:
        return "end seen";
    }
    return "?";
}

StepGate::StepGate(std::int64_t settleMs) : m_settleMs(settleMs) { }

void StepGate::Arm()
{
    m_phase = Phase::Slewing;
    m_endSeenMs = 0;
}

void StepGate::Reset()
{
    m_phase = Phase::Idle;
    m_endSeenMs = 0;
}

bool StepGate::IgnoreFrame(bool mountSlewing, std::int64_t frameStartMs, std::int64_t nowMs)
{
    if (mountSlewing)
    {
        // whatever the phase: a frame read as slewing is never tracked, and a
        // new end has to be seen
        Arm();
        return true;
    }

    if (m_phase == Phase::Idle)
        return false;

    if (m_phase == Phase::Slewing)
    {
        // the frame on which the end is first seen was exposed before it was
        // processed, so it overlapped the slew whatever its start time maps to
        m_phase = Phase::EndSeen;
        m_endSeenMs = nowMs;
        return true;
    }

    // every frame judged here was started after the end-seen frame had been
    // processed (the next exposure is scheduled only after it), so the slack
    // only absorbs clock truncation and cannot release a frame that overlapped
    // the slew
    if (frameStartMs + kClockSlackMs >= m_endSeenMs + m_settleMs)
    {
        m_phase = Phase::Idle;
        return false;
    }

    return true;
}

std::int64_t FrameStartOnSteadyClock(std::int64_t frameStartWallMs, std::int64_t wallNowMs, std::int64_t steadyNowMs)
{
    std::int64_t age = wallNowMs - frameStartWallMs;
    if (age < 0)
        return kUnknownFrameStart; // fail closed: such a frame is never released
    return steadyNowMs - age;
}

} // namespace staticpa_step
