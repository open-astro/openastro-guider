/*
 *  guide_distance_tracker.cpp
 *  PHD Guiding
 *
 *  See guide_distance_tracker.h.
 */

#include "guide_distance_tracker.h"

GuideDistanceTracker::GuideDistanceTracker(ClockS clock) : m_clock(clock) { }

void GuideDistanceTracker::OnStarFound(bool guiding, double distance, double distanceRA)
{
    m_starFound.Found(m_clock());

    if (guiding)
    {
        // update moving average distance
        static double const alpha = .3; // moderately high weighting for latest sample
        m_avg += alpha * (distance - m_avg);
        m_avgRA += alpha * (distanceRA - m_avgRA);

        ++m_count;

        if (m_count < 10)
        {
            // initialize smoothed running avg with mean of first 10 pts
            m_avgLong += (distance - m_avgLong) / m_count;
            m_avgLongRA += (distanceRA - m_avgLongRA) / m_count;
        }
        else
        {
            static double const alpha_long =
                .045; // heavy smoothing, low weighting for latest sample .045 => 15 frame half-life
            m_avgLong += alpha_long * (distance - m_avgLong);
            m_avgLongRA += alpha_long * (distanceRA - m_avgLongRA);
        }
    }
    else
    {
        // not yet guiding, reinitialize average distance
        m_avg = m_avgLong = distance;
        m_avgRA = m_avgLongRA = distanceRA;
        m_count = 1;
    }

    if (m_needReset)
    {
        // avg distance history invalidated
        m_avg = m_avgLong = distance;
        m_avgRA = m_avgLongRA = distanceRA;
        m_count = 1;
        m_needReset = false;
    }
}

void GuideDistanceTracker::OnDither(double dist, double distRA)
{
    m_avg += dist;
    m_avgLong += dist;
    m_avgRA += distRA;
    m_avgLongRA += distRA;
}

double GuideDistanceTracker::CurrentError(bool raOnly) const
{
    return m_starFound.CurrentError(m_clock(), raOnly ? m_avgRA : m_avg);
}

double GuideDistanceTracker::CurrentErrorSmoothed(bool raOnly) const
{
    return m_starFound.CurrentError(m_clock(), raOnly ? m_avgLongRA : m_avgLong);
}
