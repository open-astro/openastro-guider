/*
 *  gear_dialog.h
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

#ifndef GEAR_DIALOG_H_INCLUDED
#define GEAR_DIALOG_H_INCLUDED

#include "connect_request.h"
#include "connect_retry.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct AlpacaProbeTarget;

// The #104 retry of a selected device that failed to connect, the guide RPC's connect-first and the #105 camera
// reconnect glue, on the devices EquipmentManager (pFrame->pEquipment) owns. It is never shown: the daemon is driven
// over JSON-RPC. The gear requests themselves (set_connected, set_profile, the startup auto-connect) run in the
// manager; a retry connects through pEquipment->SetDeviceConnected. #115 step 3 moves the rest of it into
// EquipmentManager.
class GearDialog : public wxDialog
{
    // #104: recovery of a selected device that failed to connect (headless only). Main thread only, except the
    // probe thread, which reads only its own copy of the target and m_probeCancel and hands its result back through
    // m_probeHandoff and a CallAfter.
    ConnectRetry::Tracker m_connectRetry;
    wxTimer m_connectRetryTimer;
    bool m_connectRetryStopped; // shutting down: no more attempts
    wxString m_lastConnectError[3]; // the reason of each device's last failed connect, for the RPC error texts
    std::string m_retryDeferral[3]; // the gate a due attempt last waited on, logged once per stretch
    std::thread m_probeThread;
    std::atomic<bool> m_probeCancel;
    bool m_probeRunning;
    GearDevice m_probeDevice; // the device the running probe is for
    uint64_t m_probeToken; // its attempt (Tracker::BeginAttempt)
    ConnectRetry::ProbeHandoff m_probeHandoff;
    ConnectRetry::Tracker::ServerChangeCarry m_serverChangeCarry; // AlpacaServerChanging to AlpacaServerChanged

public:
    GearDialog(wxWindow *pParent);
    ~GearDialog();

    bool ReconnectCamera();

    // #104 (headless only; each returns at once in the GUI)
    // The guide RPC: connect the selected camera and mount if they are not connected, probing an Alpaca server
    // first. Returns true with *error set when one is still not connected (a device the user disconnected is left
    // alone); false when there was nothing to do or everything connected.
    bool ConnectForGuide(wxString *error);
    // The camera auto-reconnect (#105) ended in EquipmentReconnectFailed: the camera joins the slow retry. An empty
    // reason uses the camera's last connect failure.
    void NoteCameraReconnectFailed(const wxString& reason);
    // set_alpaca_server: Changing just before it re-selects the Alpaca devices for the new settings, Changed after.
    // A device the guider was still asked to connect (retrying, or parked as not configured) stays asked across the
    // re-selection and is retried at once.
    void AlpacaServerChanging();
    void AlpacaServerChanged();
    // Shutdown: stop retrying and ask a probe in flight to stop; join also waits for it (a request timeout at most).
    void StopConnectRetry(bool join);

    // EquipmentManager, until the #104 tracker lives there too (#115 step 3)
    // a device was chosen (SelectDevice, also for the same choice again): only another device forgets its #104 state
    void NoteChosen(GearDevice dev, const wxString& choice);
    // another profile: the device's #104 state starts over
    void ForgetDevice(GearDevice dev);
    // the end of a connect leaf, with the device's final state (GearPort::ConnectLeaf)
    void NoteConnectResult(GearDevice dev, bool connected, bool autoReconnecting = false);
    // GearPort::LastConnectError, OnConnectRequested and NoteUserDisconnect
    std::string LastConnectError(GearDevice dev) const;
    void OnConnectRequested(GearDevice dev);
    void NoteUserDisconnect(GearDevice dev);

private:
    bool RetryEnabled() const;
    bool DeviceSelected(GearDevice dev) const;
    bool DeviceConnected(GearDevice dev) const;
    ConnectRetry::ProbeKind ProbeTargetFor(GearDevice dev, AlpacaProbeTarget *target) const;
    ConnectRetry::Gates CurrentRetryGates() const;
    void CancelProbeFor(GearDevice dev);
    void StartConnectRetryTimer();
    void LogNextRetry(GearDevice dev);
    void OnConnectRetryTimer(wxTimerEvent& event);
    void OnProbeDone(GearDevice dev, uint64_t token);
    void RetryConnect(GearDevice dev);
};

#endif // GEAR_DIALOG_H_INCLUDED
