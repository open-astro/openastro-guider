/*
 *  alpaca_poll.h
 *  PHD Guiding
 *
 *  Pacing of the Alpaca camera's ImageReady poll. Every poll is an HTTP round
 *  trip, and some servers ask the camera SDK each time, so the guider backs
 *  off once the exposure should have ended instead of polling every 20 ms for
 *  the whole readout (open-astro/openastro-guider#120).
 *  Kept free of wx so it can be unit tested (tests/camera/test_alpaca_poll.cpp).
 */

#ifndef ALPACA_POLL_H_INCLUDED
#define ALPACA_POLL_H_INCLUDED

namespace AlpacaPoll
{
// msPastExpectedEnd is the time since startexposure succeeded minus the
// exposure duration, in ms (negative while the exposure is still running).
// Returns how long to wait before the next ImageReady poll: 20 ms until the
// exposure is due, then a quarter of the time waited past that, 20-250 ms, so
// a frame is seen at most 25 % of its readout (and never more than 250 ms)
// after it is ready.
int ImageReadyIntervalMs(long msPastExpectedEnd);
}

#endif
