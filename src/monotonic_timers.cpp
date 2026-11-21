/*
 *  monotonic_timers.cpp
 *  PHD Guiding
 *
 *  See monotonic_timers.h.
 */

#include "monotonic_timers.h"

#include <ctime>

namespace monotonic_timers
{

int64_t ClockS()
{
    return static_cast<int64_t>(std::time(nullptr));
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
