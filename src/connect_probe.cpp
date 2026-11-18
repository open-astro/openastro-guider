/*
 *  connect_probe.cpp
 *  PHD Guiding
 *
 *  See connect_probe.h.
 */

#include "phd.h"
#include "connect_probe.h"
#include "alpaca_client.h"

#include <algorithm>
#include <chrono>

namespace
{
// the Alpaca device type in the URL
const char *AlpacaDeviceType(GearDevice device)
{
    switch (device)
    {
    case GearDevice::Mount:
        return "telescope";
    case GearDevice::Rotator:
        return "rotator";
    default:
        return "camera";
    }
}

class AlpacaProbeOps : public ConnectRetry::ProbeOps
{
    AlpacaClient m_client;
    wxString m_endpoint;
    const std::atomic<bool> *m_cancel;
    int64_t m_budgetMs = -1; // SetCallBudgetMs; -1: no limit

    // the request's own timeout, cut to the caller's budget
    void ApplyTimeout(long requestMs)
    {
        long total = requestMs;
        if (m_budgetMs > 0 && m_budgetMs < total)
            total = (long) m_budgetMs;
        m_client.SetTimeoutMs(total, std::min(ConnectRetry::kProbeConnectTimeoutMs, total));
    }

public:
    AlpacaProbeOps(const AlpacaProbeTarget& target, const std::atomic<bool> *cancel)
        : m_client(target.host, target.port, target.deviceNumber),
          m_endpoint(wxString::Format("%s/%ld/connected", AlpacaDeviceType(target.device), target.deviceNumber)),
          m_cancel(cancel)
    {
        ApplyTimeout(ConnectRetry::kProbeRequestTimeoutMs);
        // one GET per poll: a dropped connection is answered at the next poll, not by three retries in a row
        m_client.SetGetAttempts(1);
    }

    bool GetConnected(bool *connected, AlpacaErrors::CallError *error) override
    {
        ApplyTimeout(ConnectRetry::kProbeRequestTimeoutMs);
        return m_client.GetBool(m_endpoint, connected, nullptr, nullptr, error);
    }

    bool PutConnected(AlpacaErrors::CallError *error) override
    {
        // a synchronous connect can take seconds (the background retry's PUT); the GETs keep the short timeout
        ApplyTimeout(ConnectRetry::kProbePutTimeoutMs);
        JsonParser parser;
        return m_client.Put(m_endpoint, "Connected=true", parser, nullptr, nullptr, error);
    }

    void SetCallBudgetMs(int64_t ms) override { m_budgetMs = ms; }

    void SleepMs(int ms) override { wxMilliSleep(ms); }

    int64_t NowMs() override
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool Cancelled() override { return m_cancel && m_cancel->load(); }
};
} // namespace

ConnectRetry::ProbeResult ProbeAlpacaDevice(const AlpacaProbeTarget& target, const std::atomic<bool> *cancel, int connectWaitMs,
                                            int64_t deadlineMs)
{
    Debug.Write(wxString::Format("connect probe: %s at %s:%ld device %ld\n", GearDeviceName(target.device), target.host,
                                 target.port, target.deviceNumber));
    AlpacaProbeOps ops(target, cancel);
    ConnectRetry::ProbeResult result = ConnectRetry::RunProbe(ops, GearDeviceName(target.device), connectWaitMs,
                                                              ConnectRetry::kProbePollIntervalMs, deadlineMs);
    if (result.connected)
        Debug.Write(wxString::Format("connect probe: the server reports the %s connected\n", GearDeviceName(target.device)));
    else if (result.outOfTime)
        Debug.Write(wxString::Format("connect probe: %s not tried: no time left for its next request\n",
                                     GearDeviceName(target.device)));
    else
        Debug.Write(wxString::Format("connect probe: %s not connected: %s\n", GearDeviceName(target.device),
                                     wxString(result.reason.c_str(), wxConvUTF8)));
    return result;
}
