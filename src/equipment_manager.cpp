/*
 *  equipment_manager.cpp
 *  PHD Guiding
 *
 *  See equipment_manager.h. The device selection below is GearDialog's choice handlers and LoadGearChoices, moved
 *  (#115 step 3, S4): the debug-log lines keep their text.
 */

#include "phd.h"
#include "imaging_gear_alpaca.h"
#include "gear_selection.h"

#include <string>
#include <vector>

namespace
{
int DeviceIndex(GearDevice dev)
{
    return dev == GearDevice::Mount ? 1 : dev == GearDevice::Rotator ? 2 : 0;
}

std::vector<std::wstring> ListItems(const wxArrayString& list)
{
    std::vector<std::wstring> items;
    for (const wxString& item : list)
        items.push_back(item.ToStdWstring());
    return items;
}
} // namespace

EquipmentManager::EquipmentManager()
    : m_camera(nullptr), m_mount(nullptr), m_rotator(nullptr), m_updated { false, false, false }
{
}

EquipmentManager::~EquipmentManager()
{
    ReleaseDevices();
}

// The #171 order, camera, mount, rotator. Each global is cleared before its device is deleted, so nothing reads a
// device whose destructor is running: a mount or rotator disconnecting in its destructor updates the status bar,
// which reads pCamera, pMount and pPointingSource. This also prevents double frees.
void EquipmentManager::ReleaseDevices()
{
    pCamera = nullptr;
    delete m_camera;
    m_camera = nullptr;

    pMount = nullptr;
    pSecondaryMount = nullptr;
    pPointingSource = nullptr;
    delete m_mount;
    m_mount = nullptr;

    pRotator = nullptr;
    delete m_rotator;
    m_rotator = nullptr;
}

void EquipmentManager::PublishGear()
{
    pCamera = m_camera;
    pMount = m_mount;
    pSecondaryMount = nullptr;
    pPointingSource = m_mount;
    pRotator = m_rotator;

    // equipment may have connected or disconnected
    ImagingGearLog::EquipmentChanged();
}

void EquipmentManager::LoadFromProfile()
{
    // the three lists first, then the saved choices, as GearDialog loaded its choice controls (the lists read the
    // profile's Alpaca settings, and the debug log keeps those reads in this order)
    std::vector<std::wstring> cameras = ListItems(GuideCamera::GuideCameraList());
    std::vector<std::wstring> mounts = ListItems(Scope::MountList());
    std::vector<std::wstring> rotators = ListItems(Rotator::RotatorList());

    // the camera baseline: the camera-change test of DoConnectCamera, and the name its camera id is keyed from. The
    // raw saved string, not the resolved choice (an inherited quirk, kept in step 3).
    m_baselineCamera = GearSelection::SavedChoice([](const std::string& key, const std::wstring& defaultValue)
                                                  { return pConfig->Profile.GetString(key, defaultValue).ToStdWstring(); },
                                                  wxString(_("None")).ToStdWstring());
    SelectDevice(GearDevice::Camera,
                 wxString(GearSelection::ResolveChoice(GearDevice::Camera, cameras, m_baselineCamera.ToStdWstring())));

    wxString lastScope = pConfig->Profile.GetString("/scope/LastMenuChoice", _("None"));
    SelectDevice(GearDevice::Mount,
                 wxString(GearSelection::ResolveChoice(GearDevice::Mount, mounts, lastScope.ToStdWstring())));

    wxString lastRotator = pConfig->Profile.GetString("/rotator/LastMenuChoice", _("None"));
    SelectDevice(GearDevice::Rotator,
                 wxString(GearSelection::ResolveChoice(GearDevice::Rotator, rotators, lastRotator.ToStdWstring())));
}

void EquipmentManager::SelectDevice(GearDevice dev, const wxString& choice)
{
    switch (dev)
    {
    case GearDevice::Camera:
        SelectCamera(choice);
        break;
    case GearDevice::Mount:
        SelectMount(choice);
        break;
    case GearDevice::Rotator:
        SelectRotator(choice);
        break;
    case GearDevice::All:
        break;
    }
}

wxString EquipmentManager::SelectedChoice(GearDevice dev) const
{
    return m_choice[DeviceIndex(dev)];
}

void EquipmentManager::SelectCamera(const wxString& choice)
{
    // the choice is recorded even when the dark build below refuses it, as GearDialog's choice control kept showing
    // it (every caller has already written it to LastMenuChoice, which the control was put back to)
    m_choice[DeviceIndex(GearDevice::Camera)] = choice;

    if (DarkBuildActive())
    {
        // A dark capture is using the camera on its own thread (#100); the build ends within one exposure. The camera
        // in use stays.
        pFrame->Alert(_("Dark-frame capture in progress; try again when the build has finished."));
        return;
    }
    try
    {
        pFrame->pGearDialog->NoteChosen(GearDevice::Camera, choice); // another camera: its retry state starts over (#104)

        pCamera = nullptr; // before the delete: nothing reads a camera whose destructor is running
        delete m_camera;
        m_camera = nullptr;

        m_camera = GuideCamera::Factory(choice);

        Debug.AddLine(wxString::Format("Created new camera of type %s = %p", choice, m_camera));

        if (pConfig->Profile.GetString("/camera/LastMenuChoice", wxEmptyString) != choice)
        {
            pConfig->Profile.SetString("/camera/LastMenuChoice", choice);
        }

        if (!m_camera)
        {
            throw THROW_INFO("OnChoiceCamera: m_pCamera == NULL");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    PublishGear();

    m_updated[DeviceIndex(GearDevice::Camera)] = true;
}

void EquipmentManager::SelectMount(const wxString& choice)
{
    m_choice[DeviceIndex(GearDevice::Mount)] = choice;
    try
    {
        pFrame->pGearDialog->NoteChosen(GearDevice::Mount, choice); // another mount: its retry state starts over (#104)

        pMount = nullptr; // before the delete: nothing reads a mount whose destructor is running
        pSecondaryMount = nullptr;
        pPointingSource = nullptr;
        delete m_mount;
        m_mount = nullptr;

        m_mount = Scope::Factory(choice);
        Debug.AddLine(wxString::Format("Created new scope of type %s = %p", choice, m_mount));

        if (pConfig->Profile.GetString("/scope/LastMenuChoice", wxEmptyString) != choice)
        {
            pConfig->Profile.SetString("/scope/LastMenuChoice", choice);
        }

        if (!m_mount)
        {
            throw THROW_INFO("OnChoiceScope: m_pScope == NULL");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    PublishGear();

    m_updated[DeviceIndex(GearDevice::Mount)] = true;
}

void EquipmentManager::SelectRotator(const wxString& choice)
{
    m_choice[DeviceIndex(GearDevice::Rotator)] = choice;
    try
    {
        pFrame->pGearDialog->NoteChosen(GearDevice::Rotator, choice); // another rotator: its retry state starts over (#104)

        pRotator = nullptr; // before the delete: nothing reads a rotator whose destructor is running
        delete m_rotator;
        m_rotator = nullptr;

        m_rotator = Rotator::Factory(choice);
        Debug.AddLine(wxString::Format("Created new Rotator of type %s = %p", choice, m_rotator));

        if (pConfig->Profile.GetString("/rotator/LastMenuChoice", wxEmptyString) != choice)
        {
            pConfig->Profile.SetString("/rotator/LastMenuChoice", choice);
        }

        if (!m_rotator)
        {
            throw THROW_INFO("OnChoiceRotator: m_pRotator == NULL");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    PublishGear();

    m_updated[DeviceIndex(GearDevice::Rotator)] = true;
}

void EquipmentManager::ReloadProfileGear()
{
    // another profile: its devices start over, even one chosen under the same name (#104)
    for (GearDevice dev : { GearDevice::Camera, GearDevice::Mount, GearDevice::Rotator })
        pFrame->pGearDialog->ForgetDevice(dev);
    LoadFromProfile();
}

bool EquipmentManager::SetProfile(int profileId, wxString *error)
{
    // Run by the session only once the profile is not the current one and nothing is connected, so its profile reads
    // (and their debug-log lines) come where they always did. GearDialog selected the name in a wxChoice of the
    // profile names first, without case, and switched to the item selected; PhdConfig::SetCurrentProfile matches the
    // name without case itself, so switching to the name is the same switch.
    wxString name;
    auto selectTarget = [profileId, &name]()
    {
        if (!pConfig->ProfileExists(profileId))
            return false;
        name = pConfig->GetProfileName(profileId);
        return true;
    };
    auto reload = [this, &name]()
    {
        pConfig->SetCurrentProfile(name);
        ReloadProfileGear();
        pFrame->LoadProfileSettings();
        pFrame->pGuider->LoadProfileSettings();
        pFrame->UpdateTitle();
        pFrame->pGuideHistory->ResetData();
    };

    std::string err;
    if (!pFrame->pGearDialog->Session().SetProfile(pConfig->GetCurrentProfileId(), profileId, selectTarget, reload, &err))
        return false;
    *error = wxString::FromUTF8(err.c_str());
    return true;
}

void EquipmentManager::MarkUpdated(GearDevice dev)
{
    m_updated[DeviceIndex(dev)] = true;
}

void EquipmentManager::UpdateAdvancedDialog(bool preLoad)
{
    if (m_updated[DeviceIndex(GearDevice::Camera)])
    {
        pFrame->pAdvancedDialog->UpdateCameraPage();
        m_updated[DeviceIndex(GearDevice::Camera)] = false;
    }

    if (m_updated[DeviceIndex(GearDevice::Mount)])
    {
        pFrame->pAdvancedDialog->UpdateMountPage();
        m_updated[DeviceIndex(GearDevice::Mount)] = false;
    }

    if (m_updated[DeviceIndex(GearDevice::Rotator)])
    {
        pFrame->pAdvancedDialog->UpdateRotatorPage();
        m_updated[DeviceIndex(GearDevice::Rotator)] = false;
    }

    if (preLoad)
    {
        pFrame->pAdvancedDialog->Preload();
    }
}
