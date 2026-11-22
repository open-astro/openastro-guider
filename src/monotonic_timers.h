/*
 *  monotonic_timers.h
 *  PHD Guiding
 *
 *  The guider's monotonic clock, in whole seconds (ClockS) and in milliseconds
 *  (ClockMs), both readings of the same steady_clock, and the timers that run
 *  on it. In whole seconds (#226): how long ago Guider last found the star
 *  (StarFoundTimer, behind Guider::CurrentError's 100 px "large distance") and
 *  MyFrame's camera reconnect throttle (ReconnectThrottle, 3 attempts in 60 s);
 *  StepGuider's AO bump timeout reads ClockS too (#231). In milliseconds
 *  (#231): the set_pa_session lease (PaSessionLease); the multi-star
 *  DistanceChecker's wait and the star mass-change window read ClockMs. The
 *  timers take the time from their caller, so they are tested on an injected
 *  clock. RunTimer (#235) times the headless Guiding Assistant's sampling and
 *  the backlash measurement's north moves; it reads the clock itself, or takes
 *  it from its caller to be tested on an injected clock. Kept free of wx so it
 *  can be unit tested
 *  (tests/guiding/test_monotonic_timers.cpp).
 */

#ifndef MONOTONIC_TIMERS_H_INCLUDED
#define MONOTONIC_TIMERS_H_INCLUDED

#include <cstdint>
#include <vector>

namespace monotonic_timers
{

// The clock, in whole seconds, StarFoundTimer, ReconnectThrottle and the AO bump timeout are given
// the time from: the monotonic one, which a wall-clock step does not move. Its zero is arbitrary,
// so it only measures time between two of its own readings.
int64_t ClockS();

// The same clock in milliseconds, for the set_pa_session lease, the multi-star DistanceChecker's
// wait and the star mass-change window
int64_t ClockMs();

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

// The set_pa_session lease (design/POLAR_ALIGNMENT_DESIGN.md §4): Start holds it for timeoutS
// from nowMs (a renewal starts it again from nowMs), End drops it, and it is active until the
// clock reaches its expiry
class PaSessionLease
{
public:
    struct Status
    {
        bool active = false;
        int expiresInS = 0; // whole seconds left, rounded down; 0 when not active
    };

    void Start(int64_t nowMs, long timeoutS);
    void End();
    bool Active(int64_t nowMs) const;
    // Active and expiresInS from one reading, so a lease expiring mid-call is not reported
    // active with a negative expiresInS
    Status StatusAt(int64_t nowMs) const;

private:
    bool m_held = false;
    int64_t m_expiresMs = 0;
};

// How long a run has lasted since Start (#235): the headless Guiding Assistant's sampling, which
// ends once it has lasted its duration, and the backlash measurement's north moves, whose length
// scales the drift correction. The overloads without nowMs read the clock themselves; those with
// it take the time from the caller, for the tests' injected clock
class RunTimer
{
public:
    void Start();
    void Start(int64_t nowMs);
    int64_t ElapsedMs() const;
    int64_t ElapsedMs(int64_t nowMs) const;
    double ElapsedS() const;
    double ElapsedS(int64_t nowMs) const;

private:
    int64_t m_startMs = 0;
};

} // namespace monotonic_timers

#endif // MONOTONIC_TIMERS_H_INCLUDED
