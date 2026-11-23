/*
 *  equipment_manager.h
 *  PHD Guiding
 *
 *  The owner of the camera, mount and rotator objects and of the three device choices (#115 step 3). It creates the
 *  devices from the profile's LastMenuChoice keys, replaces one when set_selected_* or set_alpaca_server selects
 *  another, rebuilds them all on a profile switch, and is the only writer of pCamera, pMount, pSecondaryMount (always
 *  nullptr), pPointingSource and pRotator.
 *
 *  It also runs the gear requests (set_connected, set_profile and friends, the startup auto-connect, the #104 retry's
 *  connect): their sequencing is GearSession's (gear_session.h), for which this is the GearPort, and the connect and
 *  disconnect leaves, the end-of-request hooks and Shutdown are here. So are the #104 retry of a selected device that
 *  failed to connect, the guide RPC's connect-first and the #105 camera reconnect glue.
 *
 *  Main thread only: RPC dispatch, the retry timer's tick, the probe's CallAfter, the startup auto-connect and
 *  MyFrame::Shutdown. The one exception is the #104 probe thread, which reads only its own copy of the target and
 *  m_probeCancel, hands its result back through m_probeHandoff, and ends with a CallAfter to this handler; that is why
 *  this is a wxEvtHandler. The destructor joins the probe thread before any member goes.
 *
 *  MyFrame creates it with no devices; PhdApp::OnInit calls LoadFromProfile once pFrame is set, because the device
 *  constructors call through pFrame.
 */

#ifndef EQUIPMENT_MANAGER_H_INCLUDED
#define EQUIPMENT_MANAGER_H_INCLUDED

#include "connect_request.h"
#include "connect_retry.h"
#include "gear_session.h"

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class GuideCamera;
class Scope;
class Rotator;
struct AlpacaProbeTarget;

class EquipmentManager : public wxEvtHandler, private GearPort
{
public:
    // no devices yet: LoadFromProfile creates them
    EquipmentManager();
    // stops the #104 retry and joins its probe thread, then releases the devices camera, mount, rotator (#171), each
    // global cleared before its device is deleted
    ~EquipmentManager();
    EquipmentManager(const EquipmentManager&) = delete;
    EquipmentManager& operator=(const EquipmentManager&) = delete;

    // Create the camera, mount and rotator from the current profile's saved choices, each resolved against its
    // device list (GearSelection::ResolveChoice: a stale camera takes the first item, a stale mount or rotator none).
    // Never connects. The camera baseline (the camera-change test, and the name the camera id is keyed from) is reset
    // to the saved camera choice.
    void LoadFromProfile();

    // Select a device by its choice string, an item of its device list (set_selected_*, set_alpaca_server): the old
    // device object is deleted and a new one made, even for the same choice, which the re-selection set_alpaca_server
    // makes relies on. LastMenuChoice is written only when it differs. A camera choice made during a dark build is
    // refused with an Alert and keeps the camera in use. GearDevice::All selects nothing.
    void SelectDevice(GearDevice dev, const wxString& choice);
    // the device's current choice string ('' for a stale mount or rotator choice); dev is not All
    wxString SelectedChoice(GearDevice dev) const;

    // set_profile and friends: switch to the profile, rebuild its gear and reload its settings. Returns true with
    // *error set when refused ("cannot set profile when equipment is connected", "invalid profile id"); a switch to
    // the current profile does nothing.
    bool SetProfile(int profileId, wxString *error);
    // SetProfile's refusal, made before create_profile and clone_profile create anything (#185): true with
    // "cannot set profile when equipment is connected" when the camera, mount or rotator is connected
    bool ProfileSwitchRefused(wxString *error) const;
    // The current profile changed without a switch (delete_profile of the current profile, #117): rebuild the camera,
    // mount and rotator from it. Gear only: the caller reloads the profile's other settings.
    void ReloadProfileGear();

    GuideCamera *CameraDevice() const { return m_camera; }
    Scope *MountDevice() const { return m_mount; }
    Rotator *RotatorDevice() const { return m_rotator; }

    // Each returns true with *error set when the request failed or was refused (the set_connected error texts),
    // false otherwise.
    bool ConnectAll(wxString *error);
    bool DisconnectAll(wxString *error);
    // one device, or all with GearDevice::All
    bool SetDeviceConnected(GearDevice device, bool connect, wxString *error);
    // MyFrame::Shutdown, at exit: disconnect the mount and camera (not when a worker thread had to be killed), then the
    // rotator; nothing is deleted
    void Shutdown(bool forced);

    // The #105 camera auto-reconnect (MyFrame::DoTryReconnect): the camera connect leaf outside any gear request, so
    // no end of request runs. Returns true when the camera is still not connected.
    bool ReconnectCamera();

    // #104
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

private:
    // GearPort, for m_session
    bool Selected(GearDevice dev) const override;
    bool Connected(GearDevice dev) const override;
    bool CaptureActive() const override;
    bool CalibratingOrGuiding() const override;
    void ConnectLeaf(GearDevice dev) override;
    void DisconnectLeaf(GearDevice dev) override;
    std::string LastConnectError(GearDevice dev) const override;
    void OnConnectRequested(GearDevice dev) override;
    void NoteUserDisconnect(GearDevice dev) override;
    void Trace(const std::string& line) override;
    // the end of a gear request (connect, disconnect, profile), in GearSession::End's order (#139): frame controls,
    // the calibration auto-load or clear, the image-scale change and EquipmentReconnected
    void RefreshFrameControls() override;
    bool AutoLoadCalibration() const override;
    void LoadCalibration() override;
    void ClearCalibration() override;
    void HandleImageScaleChange() override;
    void SendReconnected(GearDevice dev) override;

    // the connect and disconnect leaves; StatusMsg, the status-bar labels and SetDarkState still go through
    // pFrame (step 6). The camera's is also the #105 auto-reconnect's (autoReconnecting, which runs outside any gear
    // request). The camera id is keyed from the camera baseline, not the current choice, as it always was.
    void ConnectCamera(bool autoReconnecting);
    void DisconnectCamera();
    void ConnectMount();
    void DisconnectMount();
    void ConnectRotator();
    void DisconnectRotator();

    // publish the devices to the globals and tell the imaging-gear log, after a selection, connect or disconnect
    void PublishGear();
    void SelectCamera(const wxString& choice);
    void SelectMount(const wxString& choice);
    void SelectRotator(const wxString& choice);
    void ReleaseDevices();

    // #104: always true; the daemon has no GUI mode (#115 S8.2)
    bool RetryEnabled() const;
    // a device was chosen (SelectDevice, also for the same choice again): only another device forgets its #104 state
    void NoteChosen(GearDevice dev, const wxString& choice);
    // another profile: the device's #104 state starts over
    void ForgetDevice(GearDevice dev);
    // the end of a connect leaf, with the device's final state
    void NoteConnectResult(GearDevice dev, bool connected, bool autoReconnecting = false);
    ConnectRetry::ProbeKind ProbeTargetFor(GearDevice dev, AlpacaProbeTarget *target) const;
    ConnectRetry::Gates CurrentRetryGates() const;
    void CancelProbeFor(GearDevice dev);
    void StartConnectRetryTimer();
    void LogNextRetry(GearDevice dev);
    void OnConnectRetryTimer(wxTimerEvent& event);
    void OnProbeDone(GearDevice dev, uint64_t token);
    void RetryConnect(GearDevice dev);

    // #104: recovery of a selected device that failed to connect. Main thread only, except the
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

    GuideCamera *m_camera;
    Scope *m_mount;
    Rotator *m_rotator;
    wxString m_choice[3]; // camera, mount, rotator
    wxString m_baselineCamera; // the camera-change test of ConnectCamera and the name its camera id is keyed from
    // the gear requests: their depth (the #104 retry's "connect in progress"), the camera-change latch (#139), the
    // queued EquipmentReconnected (#104) and the end of request
    GearSession m_session;
};

#endif // EQUIPMENT_MANAGER_H_INCLUDED
