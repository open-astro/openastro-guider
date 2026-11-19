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
    if (elapsedSinceSendMs >= durationMs)
        return 0;
    return durationMs - elapsedSinceSendMs + POLL_MARGIN_MS;
}

bool CompletionTimedOut(long durationMs, long elapsedSinceSendMs)
{
    return elapsedSinceSendMs > durationMs + TIMEOUT_MS;
}
} // namespace PulseWait
