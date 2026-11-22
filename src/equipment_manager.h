/*
 *  equipment_manager.h
 *  PHD Guiding
 *
 *  The owner of the camera, mount and rotator objects and of the three device choices (#115 step 3). It creates the
 *  devices from the profile's LastMenuChoice keys, replaces one when set_selected_* or set_alpaca_server selects
 *  another, rebuilds them all on a profile switch, and is the only writer of pCamera, pMount, pSecondaryMount (always
 *  nullptr), pPointingSource and pRotator.
 *
 *  Main thread only. MyFrame creates it with no devices; PhdApp::OnInit calls LoadFromProfile once pFrame is set,
 *  because the device constructors call through pFrame.
 *
 *  Until the later slices of step 3, GearDialog still runs the connect and disconnect leaves, the request sequencer
 *  (GearSession) and the #104 retry. It reaches the devices and the choices here, and the members marked "GearDialog"
 *  below are the temporary forwarders it uses; this class calls GearDialog's NoteChosen, ForgetDevice and Session in
 *  return.
 */

#ifndef EQUIPMENT_MANAGER_H_INCLUDED
#define EQUIPMENT_MANAGER_H_INCLUDED

#include "connect_request.h"

#include <wx/string.h>

class GuideCamera;
class Scope;
class Rotator;

class EquipmentManager
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

    // Rebuild the AdvancedDialog page of each device changed since the last call, then preload the pages if asked
    // (the end of a gear request; startup rebuilds without the preload).
    void UpdateAdvancedDialog(bool preLoad);

    // GearDialog: a connect changed the camera's properties, so its page is rebuilt at the end of the request
    void MarkUpdated(GearDevice dev);
    // GearDialog: the camera baseline of DoConnectCamera (today's m_lastCamera)
    const wxString& BaselineCamera() const { return m_baselineCamera; }
    void SetBaselineCamera(const wxString& camera) { m_baselineCamera = camera; }
    // GearDialog: publish the devices to the globals and tell the imaging-gear log, after a connect or disconnect
    void PublishGear();

private:
    void SelectCamera(const wxString& choice);
    void SelectMount(const wxString& choice);
    void SelectRotator(const wxString& choice);
    void ReleaseDevices();

    GuideCamera *m_camera;
    Scope *m_mount;
    Rotator *m_rotator;
    wxString m_choice[3]; // camera, mount, rotator
    wxString m_baselineCamera;
    bool m_updated[3]; // the AdvancedDialog page of camera, mount, rotator needs a rebuild
};

#endif // EQUIPMENT_MANAGER_H_INCLUDED
