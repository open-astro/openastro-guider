/*
 *  pulse_wait.h
 *  PHD Guiding
 *
 *  How long ScopeAlpaca::Guide waits for an Alpaca PulseGuide to finish
 *  (#119). Both times are measured from just before the PulseGuide request
 *  is sent, so the time the request itself blocks counts toward the pulse.
 *  Kept free of wx so it can be unit tested (tests/equipment/test_pulse_wait.cpp).
 */

#ifndef PULSE_WAIT_H_INCLUDED
#define PULSE_WAIT_H_INCLUDED

namespace PulseWait
{
// How long to sleep after PulseGuide returns, before the first IsPulseGuiding
// check: what is left of the pulse plus a 10 ms margin, or 0 when the request
// returned at or after the end of the pulse.
long SleepBeforeFirstPollMs(long durationMs, long elapsedSinceSendMs);

// Whether a mount that still reports IsPulseGuiding has been waited on long
// enough: more than 2 s past the end of the pulse.
bool CompletionTimedOut(long durationMs, long elapsedSinceSendMs);
} // namespace PulseWait

#endif
