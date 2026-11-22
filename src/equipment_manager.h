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
 *  disconnect leaves, the end-of-request hooks and Shutdown are here.
 *
 *  Main thread only. MyFrame creates it with no devices; PhdApp::OnInit calls LoadFromProfile once pFrame is set,
 *  because the device constructors call through pFrame.
 *
 *  Until S6 of step 3, GearDialog still runs the #104 retry, the guide RPC's connect-first and the #105 reconnect
 *  glue. The members marked "GearDialog" below are the temporary forwarders it uses; this class calls GearDialog's
 *  NoteChosen, ForgetDevice, NoteConnectResult, LastConnectError, OnConnectRequested, NoteUserDisconnect and
 *  StopConnectRetry in return.
 */

#ifndef EQUIPMENT_MANAGER_H_INCLUDED
#define EQUIPMENT_MANAGER_H_INCLUDED

#include "connect_request.h"
#include "gear_session.h"

#include <wx/string.h>

class GuideCamera;
class Scope;
class Rotator;

class EquipmentManager : private GearPort
{
public:
    // no devices yet: LoadFromProfile creates them
    EquipmentManager();
    // releases the devices camera, mount, rotator (#171), each global cleared before its device is deleted
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
    // MyFrame::OnClose, at exit: disconnect the mount and camera (not when a worker thread had to be killed), then the
    // rotator; nothing is deleted
    void Shutdown(bool forced);

    // Rebuild the AdvancedDialog page of each device changed since the last call, then preload the pages if asked
    // (the end of a gear request; startup rebuilds without the preload).
    void UpdateAdvancedDialog(bool preLoad);

    // GearDialog: the camera connect leaf, also the #105 auto-reconnect's (autoReconnecting, which runs outside any
    // gear request). The camera id is keyed from the camera baseline, not the current choice, as it always was.
    void ConnectCamera(bool autoReconnecting);
    // GearDialog: the sequencer, for the #104 retry's "connect in progress" (Depth) and its queued
    // EquipmentReconnected
    GearSession& Session() { return m_session; }

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
    // the calibration auto-load or clear, the AdvancedDialog pages, the image-scale change and EquipmentReconnected
    void RefreshFrameControls() override;
    bool AutoLoadCalibration() const override;
    void LoadCalibration() override;
    void ClearCalibration() override;
    void RefreshAdvancedDialog() override;
    void HandleImageScaleChange() override;
    void SendReconnected(GearDevice dev) override;

    // the connect and disconnect leaves; StatusMsg, the status-bar labels and SetDarkMenuState still go through
    // pFrame (step 6)
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

    GuideCamera *m_camera;
    Scope *m_mount;
    Rotator *m_rotator;
    wxString m_choice[3]; // camera, mount, rotator
    wxString m_baselineCamera; // the camera-change test of ConnectCamera and the name its camera id is keyed from
    bool m_updated[3]; // the AdvancedDialog page of camera, mount, rotator needs a rebuild
    // the gear requests: their depth (the #104 retry's "connect in progress"), the camera-change latch (#139), the
    // queued EquipmentReconnected (#104) and the end of request
    GearSession m_session;
};

#endif // EQUIPMENT_MANAGER_H_INCLUDED
