/*
 *  guide_distance_tracker.h
 *  PHD Guiding
 *
 *  The guider's averaged distance of the star from the lock position, behind
 *  Guider::CurrentError (what the settle and the variable delay compare) and
 *  CurrentErrorSmoothed (the multi-star DistanceChecker's threshold): a short
 *  moving average (alpha 0.3) and a long one (the mean of the first 10
 *  samples, then alpha 0.045), each also for RA alone, bumped by a dither and
 *  re-seeded on request, and 100 px once no star has been found for 20 s
 *  (monotonic_timers::StarFoundTimer, on an injected clock). Guider keeps when
 *  to call it. Kept free of wx so it can be unit tested
 *  (tests/guiding/test_guide_distance_tracker.cpp).
 */

#ifndef GUIDE_DISTANCE_TRACKER_H_INCLUDED
#define GUIDE_DISTANCE_TRACKER_H_INCLUDED

#include "monotonic_timers.h"

#include <cstdint>

class GuideDistanceTracker
{
public:
    // A reading of the clock in whole seconds the star-found timer reads. Only the time between
    // two readings is used
    using ClockS = int64_t (*)();

    // Production reads monotonic_timers::ClockS, the steady clock (#226)
    explicit GuideDistanceTracker(ClockS clock = &monotonic_timers::ClockS);

    // The star was found at distance (and distanceRA in RA alone) from the lock position. While
    // guiding the averages take the sample in; otherwise they restart from it. A RequestReset
    // re-seeds them from it either way
    void OnStarFound(bool guiding, double distance, double distanceRA);
    // A dither moved the lock position dist away (distRA in RA): the averages read it at once
    void OnDither(double dist, double distRA);
    // The averages restart from the next sample (a resume from a full pause, a fast recenter done)
    void RequestReset() { m_needReset = true; }

    // The short average (RA alone when raOnly), or StarFoundTimer::LARGE_DISTANCE when no star was
    // found in the last THRESHOLD_S
    double CurrentError(bool raOnly) const;
    // The same for the long average
    double CurrentErrorSmoothed(bool raOnly) const;
    // The samples in the averages since they last restarted
    unsigned int FrameCount() const { return m_count; }

private:
    ClockS m_clock;
    monotonic_timers::StarFoundTimer m_starFound;
    double m_avg = 0.;
    double m_avgRA = 0.;
    double m_avgLong = 0.;
    double m_avgLongRA = 0.;
    unsigned int m_count = 0;
    bool m_needReset = false;
};

#endif // GUIDE_DISTANCE_TRACKER_H_INCLUDED
