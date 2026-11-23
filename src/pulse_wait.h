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
// returned at or after the end of the pulse. A negative elapsed time (the
// clock stepped back) counts as 0, so the sleep is never longer than the
// whole pulse plus the margin.
long SleepBeforeFirstPollMs(long durationMs, long elapsedSinceSendMs);

// Whether a mount that still reports IsPulseGuiding has been waited on long
// enough: more than 2 s past the end of the pulse, or a negative elapsed time
// (the clock stepped back, so the wait can no longer be measured).
bool CompletionTimedOut(long durationMs, long elapsedSinceSendMs);

// The PulseGuide request's own timeout (#294): the pulse plus 10 s, and at least 15 s, so a mount
// that blocks for the whole pulse is waited for, while one that holds the request fails the move
// in about 15 s rather than the client's 30 s. A negative duration counts as 0.
long RequestTimeoutMs(long durationMs);
} // namespace PulseWait

#endif
