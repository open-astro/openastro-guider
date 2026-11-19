/*
 *  alpaca_poll.cpp
 *  PHD Guiding
 *
 *  See alpaca_poll.h.
 */

#include "alpaca_poll.h"

namespace AlpacaPoll
{
static const int kMinMs = 20; // the interval PHD2's ASCOM camera uses
static const int kMaxMs = 250;
static const long kDivisor = 4; // added latency at most 1/kDivisor of the readout

int ImageReadyIntervalMs(long msPastExpectedEnd)
{
    if (msPastExpectedEnd <= 0)
        return kMinMs;
    long interval = msPastExpectedEnd / kDivisor;
    if (interval < kMinMs)
        return kMinMs;
    if (interval > kMaxMs)
        return kMaxMs;
    return (int) interval;
}
}
