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

class wxGridBagSizer;
struct AlpacaProbeTarget;

class GearDialog : public wxDialog
{
    GuideCamera *m_pCamera;
    Scope *m_pScope;
    Rotator *m_pRotator;

    bool m_cameraUpdated;
    bool m_mountUpdated;
    bool m_rotatorUpdated;
    bool m_showDarksDialog;
    bool m_ascomScopeSelected;
    wxString m_lastCamera;
    CameraChangeLatch m_camLatch; // what this dialog run, or this headless gear request, saw of the camera (#139)
    wxArrayString m_cameraIds;

    wxGridBagSizer *m_gearSizer;

    wxChoice *m_profiles;
    OptionsButton *m_btnProfileManage;
    wxMenu *m_menuProfileManage;

    wxChoice *m_pCameras;
    wxButton *m_selectCameraButton;
    wxButton *m_pSetupCameraButton;
    wxToggleButton *m_pConnectCameraButton;

    wxChoice *m_pScopes;
    wxButton *m_pSetupScopeButton;
    wxToggleButton *m_pConnectScopeButton;

    wxChoice *m_pRotators;
    wxButton *m_pSetupRotatorButton;
    wxToggleButton *m_pConnectRotatorButton;

    wxButton *m_pConnectAllButton;
    wxButton *m_pDisconnectAllButton;

    bool m_flushConfig;

    // #104: recovery of a selected device that failed to connect (headless only). Main thread only, except the
    // probe thread, which reads only its own copy of the target and m_probeCancel and hands its result back through
    // m_probeHandoff and a CallAfter.
    ConnectRetry::Tracker m_connectRetry;
    wxTimer m_connectRetryTimer;
    int m_connectDepth; // connect / disconnect calls on the stack
    bool m_connectRetryStopped; // shutting down: no more attempts
    std::vector<GearDevice> m_reconnectedPending; // EquipmentReconnected to send at the end of EndModal
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

    void Initialize();
    int ShowGearDialog(bool autoConnect);
    void EndModal(int retCode) override;

    bool SetProfile(int profileId, wxString *error);
    // SetProfile's refusals, the Connect Equipment dialog open or the camera, mount or rotator connected:
    // true with the error text it would give (#185)
    bool ProfileSwitchRefused(wxString *error) const;
    // The current profile changed without a profile choice (delete_profile of the current profile): rebuild the
    // camera, mount and rotator from the new current profile, as a profile choice does (#117).
    void ReloadProfileGear();
    bool ConnectAll(wxString *error);
    bool DisconnectAll(wxString *error);
    // one device, or all with GearDevice::All; returns true on error, like ConnectAll
    bool SetDeviceConnected(GearDevice device, bool connect, wxString *error);
    void Shutdown(bool forced);
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
    wxString SelectedCameraId() const;
    static wxString CameraSelectionKey(const wxString& camName);

private:
    void LoadGearChoices();
    void UpdateGearPointers();

    void UpdateCameraButtonState();
    void UpdateScopeButtonState();
    void UpdateRotatorButtonState();
    void UpdateConnectAllButtonState();
    void UpdateDisconnectAllButtonState();
    void UpdateButtonState();
    void UpdateAdvancedDialog(bool preLoad);

    void OnProfileChoice(wxCommandEvent& event);
    void OnButtonProfileManage(wxCommandEvent& event);
    void OnProfileNew(wxCommandEvent& event);
    void OnProfileDelete(wxCommandEvent& event);
    void OnProfileRename(wxCommandEvent& event);
    void OnProfileLoad(wxCommandEvent& event);
    void OnProfileSave(wxCommandEvent& event);
    void OnAdvanced(wxCommandEvent& event);

    void OnButtonConnectAll(wxCommandEvent& event);
    void OnButtonDisconnectAll(wxCommandEvent& event);
    void OnChar(wxKeyEvent& event);

    void OnChoiceCamera(wxCommandEvent& event);
    void OnButtonSelectCamera(wxCommandEvent& event);
    void OnMenuSelectCamera(wxCommandEvent& evt);

    void OnButtonSetupCamera(wxCommandEvent& event);
    bool DoConnectCamera(bool autoReconnecting);
    void BeginGearRequest(); // #139: the headless daemon's ShowGearDialog
    void OnButtonConnectCamera(wxCommandEvent& event);
    void OnButtonDisconnectCamera(wxCommandEvent& event);

    void OnChoiceScope(wxCommandEvent& event);
    void OnButtonSetupScope(wxCommandEvent& event);
    void OnButtonConnectScope(wxCommandEvent& event);
    void OnButtonDisconnectScope(wxCommandEvent& event);

    void OnChoiceRotator(wxCommandEvent& event);
    void OnButtonSetupRotator(wxCommandEvent& event);
    void OnButtonConnectRotator(wxCommandEvent& event);
    void OnButtonDisconnectRotator(wxCommandEvent& event);

    bool RetryEnabled() const;
    bool DeviceSelected(GearDevice dev) const;
    bool DeviceConnected(GearDevice dev) const;
    ConnectRetry::ProbeKind ProbeTargetFor(GearDevice dev, AlpacaProbeTarget *target) const;
    ConnectRetry::Gates CurrentRetryGates() const;
    void NoteConnectResult(GearDevice dev, bool connected, bool autoReconnecting = false);
    void NoteUserDisconnect(GearDevice dev);
    void CancelProbeFor(GearDevice dev);
    void ForgetDevice(GearDevice dev);
    void NoteChosen(GearDevice dev, const wxString& choice);
    void SendPendingReconnected();
    void StartConnectRetryTimer();
    void LogNextRetry(GearDevice dev);
    void OnConnectRetryTimer(wxTimerEvent& event);
    void OnProbeDone(GearDevice dev, uint64_t token);
    void RetryConnect(GearDevice dev);

    wxDECLARE_EVENT_TABLE();
};

#endif // GEAR_DIALOG_H_INCLUDED
