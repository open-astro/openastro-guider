/*
 *  pulse_wait.cpp
 *  PHD Guiding
 *
 *  See pulse_wait.h.
 */

#include "pulse_wait.h"

namespace PulseWait
{
enum
{
    POLL_MARGIN_MS = 10,
    GRACE_PERIOD_MS = 1000,
    TIMEOUT_MS = GRACE_PERIOD_MS + 1000,
};

long SleepBeforeFirstPollMs(long durationMs, long elapsedSinceSendMs)
{
    // A negative elapsed time is a step back of the clock, not time still to come: wait no longer
    // than the whole pulse.
    if (elapsedSinceSendMs < 0)
        elapsedSinceSendMs = 0;
    if (elapsedSinceSendMs >= durationMs)
        return 0;
    return durationMs - elapsedSinceSendMs + POLL_MARGIN_MS;
}

bool CompletionTimedOut(long durationMs, long elapsedSinceSendMs)
{
    // A negative elapsed time is a step back of the clock: how long the mount has been waited on
    // is unknown, and waiting for the clock to catch up could take as long as the step.
    if (elapsedSinceSendMs < 0)
        return true;
    return elapsedSinceSendMs > durationMs + TIMEOUT_MS;
}
} // namespace PulseWait
