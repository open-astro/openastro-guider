/*
 *  connect_probe.h
 *  PHD Guiding
 *
 *  The Alpaca reachability-and-connect probe GearDialog runs before it connects a device that failed to connect
 *  (#104), on a short-timeout AlpacaClient of its own. The sequence is ConnectRetry::RunProbe (connect_retry.h,
 *  unit tested); this file only binds it to AlpacaClient.
 */

#ifndef CONNECT_PROBE_H_INCLUDED
#define CONNECT_PROBE_H_INCLUDED

#include "connect_retry.h"

#include <atomic>

// Where a selected Alpaca device lives, read from the profile on the main thread.
struct AlpacaProbeTarget
{
    GearDevice device = GearDevice::Camera;
    wxString host;
    long port = 0;
    long deviceNumber = 0;
};

// With no deadline (the background retry, on its own thread) it blocks for at most a GET timeout, the PUT timeout,
// connectWaitMs and one more GET timeout (ConnectRetry::kProbe*: 3 + 10 + connectWaitMs + 3 s), and within the
// 1.5 s connect timeout when the server is not reachable. With deadlineMs (steady-clock ms; the guide RPC on the main
// thread, ConnectRetry::kGuideConnectBudgetMs) every request gets only what is left of it, so it returns by then,
// give or take a poll's bookkeeping. Safe to call on any thread: it touches no guider state. cancel (optional) is
// polled between requests.
ConnectRetry::ProbeResult ProbeAlpacaDevice(const AlpacaProbeTarget& target, const std::atomic<bool> *cancel, int connectWaitMs,
                                            int64_t deadlineMs = ConnectRetry::kNoDeadline);

#endif
