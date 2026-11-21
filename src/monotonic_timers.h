/*
 *  monotonic_timers.h
 *  PHD Guiding
 *
 *  Two timers the guider keeps in whole seconds, and the clock they run on (#226):
 *  how long ago Guider last found the star (StarFoundTimer, behind
 *  Guider::CurrentError's 100 px "large distance"), and MyFrame's camera
 *  reconnect throttle (ReconnectThrottle, 3 attempts in 60 s). The timers take
 *  the time from their caller, so they are tested on an injected clock; the
 *  callers read it from ClockS. Kept free of wx so it can be unit tested
 *  (tests/guiding/test_monotonic_timers.cpp).
 */

#ifndef MONOTONIC_TIMERS_H_INCLUDED
#define MONOTONIC_TIMERS_H_INCLUDED

#include <cstdint>
#include <vector>

namespace monotonic_timers
{

// The clock, in whole seconds, StarFoundTimer and ReconnectThrottle are given the time from
int64_t ClockS();

// When Guider last found the star (Guider::UpdateCurrentDistance), for Guider::CurrentError and
// CurrentErrorSmoothed: their averaged distance, or LARGE_DISTANCE when no star has been found yet
// or the last one was found more than THRESHOLD_S ago
class StarFoundTimer
{
public:
    static constexpr int THRESHOLD_S = 20;
    static constexpr double LARGE_DISTANCE = 100.0;

    void Found(int64_t nowS);
    double CurrentError(int64_t nowS, double avgDist) const;

private:
    bool m_found = false;
    int64_t m_foundAtS = 0;
};

// MyFrame::OnTryReconnect's limit: at most MAX_ATTEMPTS camera reconnects in TIME_WINDOW_S
class ReconnectThrottle
{
public:
    static constexpr int TIME_WINDOW_S = 60;
    static constexpr int MAX_ATTEMPTS = 3;

    // Forgets the attempts more than TIME_WINDOW_S before nowS; then, if fewer than MAX_ATTEMPTS
    // are left, records one at nowS and returns true, else records nothing and returns false
    bool TryAttempt(int64_t nowS);
    // The attempts recorded in the window, the last TryAttempt's included when it returned true
    int Attempts() const { return static_cast<int>(m_attemptsS.size()); }

private:
    std::vector<int64_t> m_attemptsS;
};

} // namespace monotonic_timers

#endif // MONOTONIC_TIMERS_H_INCLUDED
