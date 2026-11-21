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

} // namespace monotonic_timers
