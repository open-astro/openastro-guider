/*
 *  gear_dialog.cpp
 *  PHD Guiding
 *
 *  Created by Bret McKee
 *  Copyright (c) 2013 Bret McKee
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Bret McKee, Dad Dog Development, Ltd. nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "phd.h"
#include "guiding_assistant.h"
#include "connect_probe.h"

#include <chrono>

GearDialog::GearDialog(wxWindow *pParent)
    : wxDialog(pParent, wxID_ANY, _("Connect Equipment"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX),
      m_connectRetryStopped(false), m_probeCancel(false), m_probeRunning(false), m_probeDevice(GearDevice::Camera),
      m_probeToken(0)
{
    m_connectRetryTimer.SetOwner(this);
    Bind(wxEVT_TIMER, &GearDialog::OnConnectRetryTimer, this, m_connectRetryTimer.GetId());
}

GearDialog::~GearDialog()
{
    // a joinable std::thread must not be destroyed; OnClose has normally joined it already
    StopConnectRetry(true);
}

bool GearDialog::ReconnectCamera()
{
    Debug.Write("gear_dialog: ReconnectCamera\n");
    pFrame->pEquipment->ConnectCamera(true);
    GuideCamera *camera = pFrame->pEquipment->CameraDevice();
    bool err = !camera || !camera->Connected;
    return err;
}

namespace
{
// the retry schedule runs on a steady clock: the wall clock of a Pi without an RTC jumps when NTP syncs after boot,
// which is exactly when a mount that was not powered yet fails
int64_t SteadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int DeviceIndex(GearDevice dev)
{
    return dev == GearDevice::Mount ? 1 : dev == GearDevice::Rotator ? 2 : 0;
}

const GearDevice kRetryDevices[] = { GearDevice::Camera, GearDevice::Mount, GearDevice::Rotator };
} // namespace

// ---- #104: recovery of a selected device that failed to connect ----
//
// A device the guider was asked to connect (startup auto-connect, set_connected, ConnectAll, guide, the camera
// auto-reconnect giving up) that is still not connected is retried in the background: 15 s, 30 s, then every 60 s.
// An attempt first probes the Alpaca server on a thread of its own (connect_probe.h) with short timeouts, so a
// powered-off mount or a stalled server never blocks the main thread; only when the server reports the device
// connected does the main thread connect it through SetDeviceConnected, which is then fast and has every side
// effect a manual set_connected has. Headless only: the desktop GUI keeps upstream PHD2's behaviour.

namespace
{
std::string ToStd(const wxString& s)
{
    return std::string(s.utf8_str());
}

wxString FromStd(const std::string& s)
{
    return wxString::FromUTF8(s.c_str());
}

// the guide RPC waits less than the background retry for a device to report connected after its PUT
const int kGuideProbeConnectWaitMs = 2000;
} // namespace

bool GearDialog::RetryEnabled() const
{
    return wxGetApp().IsHeadless();
}

bool GearDialog::DeviceSelected(GearDevice dev) const
{
    const EquipmentManager *gear = pFrame->pEquipment;
    switch (dev)
    {
    case GearDevice::Camera:
        return gear->CameraDevice() != nullptr;
    case GearDevice::Mount:
        return gear->MountDevice() != nullptr;
    default:
        return gear->RotatorDevice() != nullptr;
    }
}

bool GearDialog::DeviceConnected(GearDevice dev) const
{
    const EquipmentManager *gear = pFrame->pEquipment;
    switch (dev)
    {
    case GearDevice::Camera:
        return gear->CameraDevice() && gear->CameraDevice()->Connected;
    case GearDevice::Mount:
        return gear->MountDevice() && gear->MountDevice()->IsConnected();
    default:
        return gear->RotatorDevice() && gear->RotatorDevice()->IsConnected();
    }
}

// The same choice-string tests the factories route by, and the profile keys the Alpaca drivers read.
ConnectRetry::ProbeKind GearDialog::ProbeTargetFor(GearDevice dev, AlpacaProbeTarget *target) const
{
    wxString choice = pFrame->pEquipment->SelectedChoice(dev);
    bool alpaca;
    const char *deviceKey;
    switch (dev)
    {
    case GearDevice::Camera:
        alpaca = choice.StartsWith(_T("Alpaca Camera"));
        deviceKey = "/alpaca/camera_device";
        break;
    case GearDevice::Mount:
        alpaca = choice.Contains(_("Alpaca"));
        deviceKey = "/alpaca/telescope_device";
        break;
    default:
        alpaca = choice.Contains(_T("Alpaca"));
        deviceKey = "/alpaca/rotator_device";
        break;
    }
    if (!alpaca)
        return ConnectRetry::ProbeKind::NotAlpaca;

    target->device = dev;
    target->host = pConfig->Profile.GetString("/alpaca/host", _T("localhost"));
    target->port = pConfig->Profile.GetLong("/alpaca/port", 0);
    target->deviceNumber = pConfig->Profile.GetLong(deviceKey, 0);
    return target->port > 0 ? ConnectRetry::ProbeKind::Alpaca : ConnectRetry::ProbeKind::NotConfigured;
}

ConnectRetry::Gates GearDialog::CurrentRetryGates() const
{
    ConnectRetry::Gates gates;
    gates.shuttingDown = m_connectRetryStopped;
    gates.darkBuild = DarkBuildActive();
    gates.paSession = PaSessionActive();
    gates.guidingAssistant = GuidingAssistant::ApiIsRunning();
    gates.connectInProgress = pFrame->pEquipment->Session().Depth() > 0;
    gates.captureActive = pFrame->CaptureActive;
    gates.calibratingOrGuiding = pFrame->pGuider && pFrame->pGuider->IsCalibratingOrGuiding();
    return gates;
}

// Called at the end of each connect leaf (EquipmentManager::ConnectCamera, ConnectMount, ConnectRotator) with the
// device's final state; autoReconnecting for the camera auto-reconnect (#105), which sends its own events
// (Tracker::OnLeafResult). The reason is kept for the RPC error texts either way.
void GearDialog::NoteConnectResult(GearDevice dev, bool connected, bool autoReconnecting)
{
    ConnectRetry::ConnectError e = ConnectRetry::TakeConnectError(dev);
    m_lastConnectError[DeviceIndex(dev)] = connected ? wxString() : FromStd(e.reason);

    if (!RetryEnabled())
        return;

    ConnectRetry::Tracker::LeafOutcome out =
        m_connectRetry.OnLeafResult(dev, connected, autoReconnecting, SteadyNowMs(), e.reason, e.retryable);
    if (out.reconnected)
    {
        Debug.Write(wxString::Format("connect retry: the %s is connected again\n", GearDeviceName(dev)));
        pFrame->pEquipment->Session().QueueReconnected(dev);
    }
    if (!out.failed)
        return;

    m_retryDeferral[DeviceIndex(dev)].clear();
    LogNextRetry(dev);
    StartConnectRetryTimer();
}

void GearDialog::LogNextRetry(GearDevice dev)
{
    const char *name = GearDeviceName(dev);
    wxString reason = FromStd(m_connectRetry.LastReason(dev));
    if (reason.IsEmpty())
        reason = "no reason given";
    if (m_connectRetry.IsParked(dev))
        Debug.Write(wxString::Format("connect retry: the %s failed to connect (%s); not retried until the Alpaca "
                                     "server settings change\n",
                                     name, reason));
    else if (m_connectRetry.Retrying(dev))
    {
        int64_t inMs = m_connectRetry.NextDueMs(dev) - SteadyNowMs();
        Debug.Write(wxString::Format("connect retry: the %s failed to connect (%s); next attempt in %d s (failure %d)\n", name,
                                     reason, (int) ((inMs + 999) / 1000), m_connectRetry.Attempts(dev)));
    }
}

void GearDialog::OnConnectRequested(GearDevice dev)
{
    m_connectRetry.OnConnectRequested(dev);
}

void GearDialog::NoteUserDisconnect(GearDevice dev)
{
    if (!RetryEnabled())
        return;
    if (dev == GearDevice::All)
    {
        for (GearDevice d : kRetryDevices)
            NoteUserDisconnect(d);
        return;
    }
    if (m_connectRetry.Retrying(dev))
        Debug.Write(
            wxString::Format("connect retry: the %s was disconnected by the user; no more attempts\n", GearDeviceName(dev)));
    CancelProbeFor(dev);
    m_connectRetry.OnUserDisconnect(dev);
}

// A user disconnect, another selection or another Alpaca server while the background probe for dev runs: stop it
// before its Connected=true PUT if that is not sent yet (a PUT already on the wire cannot be taken back). Its result
// is dropped either way (OnProbeDone).
void GearDialog::CancelProbeFor(GearDevice dev)
{
    if (!m_probeRunning || m_probeDevice != dev || m_probeCancel)
        return;
    Debug.Write(wxString::Format("connect retry: cancelling the %s probe in flight\n", GearDeviceName(dev)));
    m_probeCancel = true;
}

void GearDialog::ForgetDevice(GearDevice dev)
{
    if (m_connectRetry.Retrying(dev))
        Debug.Write(wxString::Format("connect retry: the %s selection changed; no more attempts\n", GearDeviceName(dev)));
    CancelProbeFor(dev);
    m_connectRetry.Forget(dev);
    m_lastConnectError[DeviceIndex(dev)].clear();
    m_retryDeferral[DeviceIndex(dev)].clear();
}

// From EquipmentManager::SelectDevice, which also runs when the same device is chosen again (set_selected_* with the current
// selection, the re-selection set_alpaca_server makes): only another device forgets the state, so choosing the same
// one again does not cancel its retries.
void GearDialog::NoteChosen(GearDevice dev, const wxString& choice)
{
    if (m_connectRetry.Chosen(dev, ToStd(choice)))
        ForgetDevice(dev);
}

std::string GearDialog::LastConnectError(GearDevice dev) const
{
    return ToStd(m_lastConnectError[DeviceIndex(dev)]);
}

void GearDialog::StartConnectRetryTimer()
{
    if (!m_connectRetryStopped && RetryEnabled() && !m_connectRetryTimer.IsRunning())
        m_connectRetryTimer.Start(ConnectRetry::kTimerTickMs);
}

void GearDialog::OnConnectRetryTimer(wxTimerEvent& event)
{
    if (m_connectRetryStopped || !RetryEnabled() || !m_connectRetry.AnyRetrying())
    {
        // OnProbeDone restarts it if the probe in flight leaves work
        m_connectRetryTimer.Stop();
        return;
    }
    if (m_probeRunning)
    {
        // The gates were checked when the probe started. One that has closed since (a dark build, calibration,
        // a Guiding Assistant run starting...) cancels the probe before its Connected=true PUT if that is not sent yet;
        // the device then waits on the gate like any other due attempt. A connect on the stack is left out: that
        // connect records its own result.
        ConnectRetry::Gates now = CurrentRetryGates();
        now.connectInProgress = false;
        std::string blocker = ConnectRetry::RetryBlocker(m_probeDevice, now);
        if (!blocker.empty() && !m_probeCancel)
        {
            Debug.Write(wxString::Format("connect retry: the %s probe in flight waits: %s\n", GearDeviceName(m_probeDevice),
                                         FromStd(blocker)));
            CancelProbeFor(m_probeDevice);
        }
        return;
    }

    int64_t now = SteadyNowMs();
    ConnectRetry::Gates gates = CurrentRetryGates();
    for (GearDevice dev : kRetryDevices)
    {
        if (!m_connectRetry.Due(dev, now))
            continue;
        const char *name = GearDeviceName(dev);
        int idx = DeviceIndex(dev);

        if (!DeviceSelected(dev) || DeviceConnected(dev))
        {
            // every connect path records its result, so this is a device nothing is left to do for
            Debug.Write(wxString::Format("connect retry: the %s is %s; no more attempts\n", name,
                                         DeviceSelected(dev) ? "connected" : "not selected"));
            m_connectRetry.Forget(dev);
            continue;
        }

        std::string blocker = ConnectRetry::RetryBlocker(dev, gates);
        if (!blocker.empty())
        {
            // stays due: the attempt runs once the gate opens
            if (blocker != m_retryDeferral[idx])
            {
                Debug.Write(wxString::Format("connect retry: the %s attempt waits: %s\n", name, FromStd(blocker)));
                m_retryDeferral[idx] = blocker;
            }
            continue;
        }
        m_retryDeferral[idx].clear();

        AlpacaProbeTarget target;
        switch (ProbeTargetFor(dev, &target))
        {
        case ConnectRetry::ProbeKind::NotConfigured:
            m_connectRetry.OnConnectResult(dev, false, now, ConnectRetry::kNotConfiguredReason, false);
            LogNextRetry(dev);
            continue;
        case ConnectRetry::ProbeKind::NotAlpaca:
            // a local driver: nothing to probe, and its connect does not wait on a network
            Debug.Write(wxString::Format("connect retry: attempt %d for the %s\n", m_connectRetry.Attempts(dev) + 1, name));
            RetryConnect(dev);
            return; // one attempt per tick
        case ConnectRetry::ProbeKind::Alpaca:
            break;
        }

        uint64_t token = m_connectRetry.BeginAttempt(dev);
        Debug.Write(wxString::Format("connect retry: attempt %d for the %s: probing the Alpaca server\n",
                                     m_connectRetry.Attempts(dev) + 1, name));
        if (m_probeThread.joinable())
            m_probeThread.join();
        m_probeCancel = false;
        m_probeRunning = true;
        m_probeDevice = dev;
        m_probeToken = token;
        m_probeHandoff.Begin();
        try
        {
            m_probeThread = std::thread(
                [this, target, dev, token]()
                {
                    ConnectRetry::ProbeResult result;
                    try
                    {
                        result = ProbeAlpacaDevice(target, &m_probeCancel, ConnectRetry::kProbeConnectWaitMs);
                    }
                    catch (...)
                    {
                        result.connected = false;
                        result.reason = "the connect probe failed";
                    }
                    // the guide RPC may take it from here while it holds the main thread (ConnectForGuide)
                    m_probeHandoff.Finish(result);
                    // the last thing the thread does; GearDialog joins it in OnProbeDone or StopConnectRetry
                    CallAfter([this, dev, token]() { OnProbeDone(dev, token); });
                });
        }
        catch (const std::exception& ex)
        {
            Debug.Write(wxString::Format("connect retry: cannot start the probe thread: %s\n", ex.what()));
            m_probeRunning = false;
            m_connectRetry.EndAttempt(dev);
        }
        return; // one attempt per tick
    }
}

void GearDialog::OnProbeDone(GearDevice dev, uint64_t token)
{
    if (m_probeThread.joinable())
        m_probeThread.join();
    m_probeRunning = false;

    if (m_connectRetryStopped)
        return;

    const char *name = GearDeviceName(dev);
    ConnectRetry::ProbeResult result;
    if (!m_probeHandoff.TakeDelivered(&result))
    {
        // the guide RPC took the result and acted on it (ConnectForGuide)
        Debug.Write(wxString::Format("connect retry: the %s probe result was taken by guide\n", name));
        if (m_connectRetry.AnyRetrying())
            StartConnectRetryTimer();
        return;
    }
    if (!m_connectRetry.AttemptCurrent(dev, token))
    {
        // a connect, a disconnect or a selection change happened meanwhile: that result stands
        Debug.Write(wxString::Format("connect retry: the %s probe result is out of date; dropped\n", name));
        if (m_connectRetry.AnyRetrying())
            StartConnectRetryTimer();
        return;
    }
    if (result.cancelled)
    {
        // cancelled for a server change that kept the device's state (CancelProbeFor): still due, no new delay
        Debug.Write(wxString::Format("connect retry: the %s probe was cancelled; dropped\n", name));
        m_connectRetry.EndAttempt(dev);
        StartConnectRetryTimer();
        return;
    }

    if (!result.connected)
    {
        m_connectRetry.OnConnectResult(dev, false, SteadyNowMs(), result.reason);
        LogNextRetry(dev);
        StartConnectRetryTimer();
        return;
    }

    // the gates may have closed while the probe ran (this can run in a nested event loop, too)
    m_connectRetry.EndAttempt(dev);
    std::string blocker = ConnectRetry::RetryBlocker(dev, CurrentRetryGates());
    if (!blocker.empty())
    {
        Debug.Write(
            wxString::Format("connect retry: the %s is up on the server; the connect waits: %s\n", name, FromStd(blocker)));
        m_retryDeferral[DeviceIndex(dev)] = blocker;
        StartConnectRetryTimer();
        return;
    }
    RetryConnect(dev);
}

// One background connect on the main thread through SetDeviceConnected, so it has the side effects of a manual
// set_connected (GearSession::End: controls, calibration auto-load, EquipmentReconnected). Quiet: a driver that fails after
// the server reported the device connected logs its Alert instead of raising one at every attempt.
void GearDialog::RetryConnect(GearDevice dev)
{
    uint64_t before = m_connectRetry.Results(dev);
    wxString err;
    Debug.Write(wxString::Format("connect retry: connecting the %s\n", GearDeviceName(dev)));
    {
        ConnectRetry::QuietConnectScope quiet;
        pFrame->pEquipment->SetDeviceConnected(dev, true, &err);
    }
    // returned before reaching a connect (a refusal the gates missed): the schedule still moves on, or the device
    // would be due again at every tick
    if (m_connectRetry.OnConnectCallEnded(dev, before, DeviceConnected(dev), SteadyNowMs(), ToStd(err)))
        LogNextRetry(dev);
    if (!DeviceConnected(dev))
        StartConnectRetryTimer();
}

bool GearDialog::ConnectForGuide(wxString *error)
{
    if (!RetryEnabled())
        return false;

    using Failure = ConnectRetry::GuideFailure;
    std::vector<Failure> failures;
    // This runs on the main thread, which serves no other RPC or event meanwhile: the camera and the mount share one
    // budget, below a client's per-call and ping timeouts. What has not connected by then is left to the background
    // retry, which has its longer timeouts on its own thread.
    const int64_t deadline = SteadyNowMs() + ConnectRetry::kGuideConnectBudgetMs;
    // all_gear_connected (phdcontrol.cpp) needs the camera and the mount; a rotator does not gate guide
    for (GearDevice dev : { GearDevice::Camera, GearDevice::Mount })
    {
        const char *name = GearDeviceName(dev);
        int idx = DeviceIndex(dev);

        std::string refusal =
            ConnectRefusal(dev, true, pFrame->CaptureActive, pFrame->pGuider && pFrame->pGuider->IsCalibratingOrGuiding());
        AlpacaProbeTarget target;
        ConnectRetry::ProbeKind kind = ProbeTargetFor(dev, &target);
        // the background probe for this device, still current (not cancelled, nothing changed the device since)
        bool probeInFlight =
            m_probeRunning && m_probeDevice == dev && !m_probeCancel && m_connectRetry.AttemptCurrent(dev, m_probeToken);
        ConnectRetry::GuideStep step = ConnectRetry::PlanGuideConnect(
            DeviceSelected(dev), DeviceConnected(dev), m_connectRetry.GetIntent(dev), !refusal.empty(), kind, probeInFlight);
        // the Alpaca server reports the device connected (the background attempt's result or this probe's)
        bool serverConnected = false;
        if (step == ConnectRetry::GuideStep::WaitForProbe)
        {
            // no second probe: a server that serialises connects would refuse it, and the background result would be
            // lost. Its CallAfter delivery cannot run while this handler holds the main thread, so take the result
            // here; the delivery then does nothing.
            Debug.Write(wxString::Format("guide: the %s background attempt is in flight; waiting for it\n", name));
            ConnectRetry::ProbeResult bg;
            if (!m_probeHandoff.WaitAndTake(deadline - SteadyNowMs(), &bg))
            {
                Debug.Write(wxString::Format("guide: the %s is still connecting; left to the background attempt\n", name));
                failures.push_back({ dev, Failure::Kind::StillConnecting, std::string() });
                continue;
            }
            m_connectRetry.EndAttempt(dev);
            if (bg.cancelled)
                step = ConnectRetry::GuideStep::ProbeThenConnect; // cannot happen with !m_probeCancel; probe afresh
            else if (!bg.connected)
            {
                m_connectRetry.OnConnectResult(dev, false, SteadyNowMs(), bg.reason);
                LogNextRetry(dev);
                StartConnectRetryTimer();
                failures.push_back({ dev, Failure::Kind::ConnectFailed, bg.reason });
                continue;
            }
            else
            {
                Debug.Write(wxString::Format("guide: the background attempt reports the %s connected\n", name));
                step = ConnectRetry::GuideStep::Connect; // the server has it connected: no probe needed
                serverConnected = true;
            }
        }
        switch (step)
        {
        case ConnectRetry::GuideStep::Nothing:
            continue;
        case ConnectRetry::GuideStep::UserDisconnected:
            Debug.Write(wxString::Format("guide: the %s was disconnected by the user; not connecting it\n", name));
            failures.push_back({ dev, Failure::Kind::UserDisconnected, std::string() });
            continue;
        case ConnectRetry::GuideStep::Refused:
            failures.push_back({ dev, Failure::Kind::Refused, refusal });
            continue;
        case ConnectRetry::GuideStep::NotConfigured:
            m_connectRetry.OnConnectResult(dev, false, SteadyNowMs(), ConnectRetry::kNotConfiguredReason, false);
            failures.push_back({ dev, Failure::Kind::ConnectFailed, ConnectRetry::kNotConfiguredReason });
            continue;
        case ConnectRetry::GuideStep::WaitForProbe: // handled above
        case ConnectRetry::GuideStep::ProbeThenConnect:
        case ConnectRetry::GuideStep::Connect:
            break;
        }

        Debug.Write(wxString::Format("guide: the %s is not connected; connecting it first\n", name));
        if (step == ConnectRetry::GuideStep::ProbeThenConnect)
        {
            // on this thread, each request within what is left of the budget
            ConnectRetry::ProbeResult probe = ProbeAlpacaDevice(target, nullptr, kGuideProbeConnectWaitMs, deadline);
            if (probe.outOfTime)
            {
                // nothing was asked of the server for it (the camera used the budget, or no time was left for the
                // PUT after a GET that read false): no attempt, so its schedule and its last reason stay
                Debug.Write(wxString::Format("guide: no time left to try the %s; left to the background retry\n", name));
                m_connectRetry.OnGuideOutOfTime(dev, SteadyNowMs(), false);
                StartConnectRetryTimer();
                failures.push_back({ dev, Failure::Kind::OutOfTime, std::string() });
                continue;
            }
            if (!probe.connected)
            {
                m_connectRetry.OnConnectResult(dev, false, SteadyNowMs(), probe.reason);
                LogNextRetry(dev);
                StartConnectRetryTimer();
                failures.push_back({ dev, Failure::Kind::ConnectFailed, probe.reason });
                continue;
            }
            serverConnected = true;
        }

        // the driver's connect of a device the server has connected is not bounded by the budget: start it only
        // with enough of the budget left, else the background retry connects it at once (no PUT: it reads connected)
        if (serverConnected && deadline - SteadyNowMs() < ConnectRetry::kGuideDriverConnectMarginMs)
        {
            Debug.Write(wxString::Format("guide: the server reports the %s connected, but too little time is left to "
                                         "connect it; left to the background retry\n",
                                         name));
            m_connectRetry.OnGuideOutOfTime(dev, SteadyNowMs(), true);
            StartConnectRetryTimer();
            failures.push_back({ dev, Failure::Kind::StillConnecting, std::string() });
            continue;
        }

        uint64_t before = m_connectRetry.Results(dev);
        wxString err;
        pFrame->pEquipment->SetDeviceConnected(dev, true, &err);
        if (DeviceConnected(dev))
            continue;
        // the driver's reason when a connect ran (its leaf recorded the failure), else SetDeviceConnected's own
        std::string reason = ToStd(m_lastConnectError[idx]);
        if (m_connectRetry.OnConnectCallEnded(dev, before, false, SteadyNowMs(), ToStd(err)))
        {
            reason = ToStd(err);
            LogNextRetry(dev);
            StartConnectRetryTimer();
        }
        failures.push_back({ dev, Failure::Kind::ConnectFailed, reason });
    }

    if (failures.empty())
        return false;
    *error = FromStd(ConnectRetry::GuideRefusal(failures));
    Debug.Write(wxString::Format("guide: refused: %s\n", *error));
    return true;
}

void GearDialog::NoteCameraReconnectFailed(const wxString& reason)
{
    GuideCamera *camera = pFrame->pEquipment->CameraDevice();
    if (!RetryEnabled() || m_connectRetryStopped || !camera || camera->Connected)
        return;
    wxString why = reason.IsEmpty() ? m_lastConnectError[DeviceIndex(GearDevice::Camera)] : reason;
    m_connectRetry.OnConnectResult(GearDevice::Camera, false, SteadyNowMs(), ToStd(why));
    LogNextRetry(GearDevice::Camera);
    StartConnectRetryTimer();
}

void GearDialog::AlpacaServerChanging()
{
    // a probe in flight is for the old server
    for (GearDevice dev : kRetryDevices)
        CancelProbeFor(dev);
    m_serverChangeCarry = m_connectRetry.BeforeServerChange();
}

// The re-selection in between forgot each Alpaca device whose choice string the new settings changed; a device the
// guider was still asked to connect is asked again and due now, including one parked as not configured.
void GearDialog::AlpacaServerChanged()
{
    ConnectRetry::Tracker::ServerChangeCarry carry = m_serverChangeCarry;
    m_serverChangeCarry = ConnectRetry::Tracker::ServerChangeCarry();
    if (!RetryEnabled())
        return;
    m_connectRetry.AfterServerChange(carry, SteadyNowMs());
    for (GearDevice dev : kRetryDevices)
    {
        if (m_connectRetry.Retrying(dev))
            Debug.Write(wxString::Format("connect retry: the Alpaca server settings changed; the %s is tried again now\n",
                                         GearDeviceName(dev)));
    }
    if (m_connectRetry.AnyRetrying())
        StartConnectRetryTimer();
}

void GearDialog::StopConnectRetry(bool join)
{
    if (!m_connectRetryStopped && m_connectRetry.AnyRetrying())
        Debug.Write("connect retry: stopped (shutting down)\n");
    m_connectRetryStopped = true;
    m_connectRetryTimer.Stop();
    m_probeCancel = true;
    if (join && m_probeThread.joinable())
        m_probeThread.join();
}
