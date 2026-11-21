/*
 *  monotonic_timers.cpp
 *  PHD Guiding
 *
 *  See monotonic_timers.h.
 */

#include "monotonic_timers.h"

#include <chrono>

namespace monotonic_timers
{

int64_t ClockS()
{
    // Not the wall clock: a step of the system time (a Pi syncing a stale clock at boot) made the
    // guider read a 100 px error and the reconnect throttle count the wrong attempts (#226)
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t ClockMs()
{
    // ClockS's clock in milliseconds. Not the wall clock: a step of the system time lengthened or
    // cut short a running set_pa_session lease by the size of the step (#231)
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void StarFoundTimer::Found(int64_t nowS)
{
    m_found = true;
    m_foundAtS = nowS;
}

double StarFoundTimer::CurrentError(int64_t nowS, double avgDist) const
{
    if (!m_found || nowS - m_foundAtS > THRESHOLD_S)
        return LARGE_DISTANCE;
    return avgDist;
}

bool ReconnectThrottle::TryAttempt(int64_t nowS)
{
    while (!m_attemptsS.empty() && nowS - m_attemptsS.front() > TIME_WINDOW_S)
        m_attemptsS.erase(m_attemptsS.begin());
    if (m_attemptsS.size() + 1 > MAX_ATTEMPTS)
        return false;
    m_attemptsS.push_back(nowS);
    return true;
}

void PaSessionLease::Start(int64_t nowMs, long timeoutS)
{
    m_held = true;
    m_expiresMs = nowMs + static_cast<int64_t>(timeoutS) * 1000;
}

void PaSessionLease::End()
{
    m_held = false;
    m_expiresMs = 0;
}

bool PaSessionLease::Active(int64_t nowMs) const
{
    return m_held && nowMs < m_expiresMs;
}

PaSessionLease::Status PaSessionLease::StatusAt(int64_t nowMs) const
{
    Status s;
    s.active = Active(nowMs);
    if (s.active)
        s.expiresInS = static_cast<int>((m_expiresMs - nowMs) / 1000);
    return s;
}

} // namespace monotonic_timers
