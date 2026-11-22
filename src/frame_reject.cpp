/*
 *  frame_reject.cpp
 *  PHD Guiding
 *
 *  See frame_reject.h. Moved from guider_multistar.cpp (#142): the same
 *  arithmetic in the same order, with the time and the guider's state given by
 *  the caller.
 */

#include "frame_reject.h"

#include <algorithm>
#include <cstdio>

namespace
{
// snprintf into a std::string of the size the text needs, as wxString::Format gives
template<class... A>
std::string Format(const char *fmt, A... args)
{
    int n = snprintf(nullptr, 0, fmt, args...);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0)
        snprintf(s.data(), s.size() + 1, fmt, args...);
    return s;
}
} // namespace

namespace frame_reject
{

MassChecker::MassChecker() : m_highMass(0.), m_lowMass(9e99), m_exposure(0), m_isAutoExposure(false)
{
    SetTimeWindow(DefaultTimeWindowMs);
}

void MassChecker::SetTimeWindow(unsigned int milliseconds)
{
    // an abrupt change in mass will affect the median after approx m_timeWindow/2
    m_timeWindow = milliseconds * 2;
}

void MassChecker::SetExposure(int exposure, bool isAutoExp)
{
    if (isAutoExp != m_isAutoExposure)
    {
        m_isAutoExposure = isAutoExp;
        m_exposure = exposure;
        Reset();
    }
    else if (exposure != m_exposure)
    {
        m_exposure = exposure;
        if (!m_isAutoExposure)
        {
            Reset();
        }
    }
}

double MassChecker::AdjustedMass(double mass) const
{
    return m_isAutoExposure ? mass / (double) m_exposure : mass;
}

void MassChecker::AppendData(double mass, int64_t nowMs)
{
    int64_t oldest = nowMs - m_timeWindow;

    while (m_data.size() > 0 && m_data.front().time < oldest)
        m_data.pop_front();

    Entry entry;
    entry.time = nowMs;
    entry.mass = AdjustedMass(mass);
    m_data.push_back(entry);
}

bool MassChecker::CheckMass(double mass, double threshold, double limits[4])
{
    if (m_data.size() < 5)
        return false;

    if (m_tmp.size() < m_data.size())
        m_tmp.resize(m_data.size() + 10);

    double *p = m_tmp.data();
    for (const Entry& e : m_data)
        *p++ = e.mass;

    size_t mid = m_data.size() / 2;
    std::nth_element(m_tmp.data(), m_tmp.data() + mid, m_tmp.data() + m_data.size());
    double med = m_tmp[mid];

    if (med > m_highMass)
        m_highMass = med;
    if (med < m_lowMass)
        m_lowMass = med;

    // let the low water mark drift to follow the median so that it moves back up after a
    // period of intermittent clouds has brought it down
    m_lowMass += .05 * (med - m_lowMass);

    limits[0] = m_lowMass * (1. - threshold);
    limits[1] = med;
    limits[2] = m_highMass * (1. + threshold);
    // when mass is depressed by sky conditions, we still want to trigger a rejection when
    // there is a large spike in mass, even if it is still below the high water mark-based
    // threshold
    limits[3] = med * (1. + 2.0 * threshold);

    double adjmass = AdjustedMass(mass);
    bool reject = adjmass < limits[0] || adjmass > limits[2] || adjmass > limits[3];

    if (reject && m_isAutoExposure)
    {
        // convert back to mass-like numbers for logging by caller
        for (int i = 0; i < 4; i++)
            limits[i] *= (double) m_exposure;
    }

    return reject;
}

void MassChecker::Reset()
{
    m_data.clear();
    m_highMass = 0.;
    m_lowMass = 9e99;
}

DistanceChecker::DistanceChecker(LogFn log) : m_log(std::move(log)), m_state(ST_GUIDING) { }

void DistanceChecker::Log(const std::string& line) const
{
    if (m_log)
        m_log(line);
}

void DistanceChecker::Activate(int64_t nowMs)
{
    if (m_state == ST_GUIDING)
    {
        Log("DistanceChecker: activated\n");
        m_state = ST_WAITING;
        m_expires = nowMs + WAIT_INTERVAL_MS;
        m_forceTolerance = 2.0;
    }
}

bool DistanceChecker::SmallOffset(double distance, double tolerance, const GuiderStatus& guider) const
{
    if (!guider.guiding || guider.paused || guider.settling || guider.frameCount < MIN_FRAMES_FOR_STATS)
    {
        return true;
    }
    double avgDist = guider.avgDist;
    double threshold = tolerance * avgDist;
    if (distance > threshold)
    {
        Log(Format("DistanceChecker: reject for large offset (%.2f > %.2f) avgDist = %.2f count = %u\n", distance, threshold,
                   avgDist, guider.frameCount));
        return false;
    }
    return true;
}

bool DistanceChecker::CheckDistance(double distance, double tolerance, const GuiderStatus& guider, int64_t nowMs)
{
    if (m_forceTolerance != 0.)
        tolerance = m_forceTolerance;

    bool small_offset = SmallOffset(distance, tolerance, guider);

    switch (m_state)
    {
    default:
    case ST_GUIDING:
        if (small_offset)
            return true;

        Log("DistanceChecker: activated\n");
        m_state = ST_WAITING;
        m_expires = nowMs + WAIT_INTERVAL_MS;
        return false;

    case ST_WAITING:
    {
        if (small_offset)
        {
            Log("DistanceChecker: deactivated\n");
            m_state = ST_GUIDING;
            m_forceTolerance = 0.;
            return true;
        }
        // large distance
        if (nowMs < m_expires)
        {
            // reject frame
            return false;
        }
        // timed-out
        Log("DistanceChecker: begin recovering\n");
        m_state = ST_RECOVERING;
        [[fallthrough]];
    }

    case ST_RECOVERING:
        if (small_offset)
        {
            // Known bug, kept as is here (#142, pinned by test_frame_reject's
            // RecoveringToGuidingKeepsTheForcedTolerance): unlike the ST_WAITING path above, this
            // does not clear m_forceTolerance, so the 2.0 Activate set stays in force back in
            // ST_GUIDING, over the caller's tolerance (9e99 with tolerate-jumps off), until a
            // large offset goes through ST_WAITING again. Upstream PHD2 has the same code.
            Log("DistanceChecker: deactivated\n");
            m_state = ST_GUIDING;
        }
        return true;
    }
}

} // namespace frame_reject
