/*
 *  equipment_manager.cpp
 *  PHD Guiding
 *
 *  See equipment_manager.h. The device selection below is GearDialog's choice handlers and LoadGearChoices, moved
 *  (#115 step 3, S4); the connect and disconnect leaves, the end-of-request hooks and Shutdown are GearDialog's, moved
 *  in S5. The debug-log lines keep their text, the GearDialog handler names in them included, which debug-log searches
 *  use.
 */

#include "phd.h"
#include "imaging_gear_alpaca.h"
#include "gear_selection.h"
#include "guiding_assistant.h"

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
    : m_camera(nullptr), m_mount(nullptr), m_rotator(nullptr), m_updated { false, false, false }, m_session(*this)
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

bool EquipmentManager::ProfileSwitchRefused(wxString *error) const
{
    // these error messages are internal to the event server and are not translated; the same refusal as the
    // session's SetProfile
    if ((m_camera && m_camera->Connected) || (m_mount && m_mount->IsConnected()) || (m_rotator && m_rotator->IsConnected()))
    {
        *error = "cannot set profile when equipment is connected";
        return true;
    }
    return false;
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
    if (!m_session.SetProfile(pConfig->GetCurrentProfileId(), profileId, selectTarget, reload, &err))
        return false;
    *error = wxString::FromUTF8(err.c_str());
    return true;
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

// ---- GearPort: what m_session (gear_session.cpp) sequences each gear request over ----

bool EquipmentManager::Selected(GearDevice dev) const
{
    switch (dev)
    {
    case GearDevice::Camera:
        return m_camera != nullptr;
    case GearDevice::Mount:
        return m_mount != nullptr;
    default:
        return m_rotator != nullptr;
    }
}

bool EquipmentManager::Connected(GearDevice dev) const
{
    switch (dev)
    {
    case GearDevice::Camera:
        return m_camera && m_camera->Connected;
    case GearDevice::Mount:
        return m_mount && m_mount->IsConnected();
    default:
        return m_rotator && m_rotator->IsConnected();
    }
}

bool EquipmentManager::CaptureActive() const
{
    return pFrame->CaptureActive;
}

bool EquipmentManager::CalibratingOrGuiding() const
{
    return pFrame->pGuider && pFrame->pGuider->IsCalibratingOrGuiding();
}

void EquipmentManager::ConnectLeaf(GearDevice dev)
{
    switch (dev)
    {
    case GearDevice::Camera:
        ConnectCamera(false);
        break;
    case GearDevice::Mount:
        ConnectMount();
        break;
    default:
        ConnectRotator();
        break;
    }
}

void EquipmentManager::DisconnectLeaf(GearDevice dev)
{
    switch (dev)
    {
    case GearDevice::Camera:
        DisconnectCamera();
        break;
    case GearDevice::Mount:
        DisconnectMount();
        break;
    default:
        DisconnectRotator();
        break;
    }
}

void EquipmentManager::OnConnectRequested(GearDevice dev)
{
    pFrame->pGearDialog->OnConnectRequested(dev);
}

// the #104 tracker and the reasons of the failed connects are GearDialog's until they move here too (#115 step 3)
std::string EquipmentManager::LastConnectError(GearDevice dev) const
{
    return pFrame->pGearDialog->LastConnectError(dev);
}

void EquipmentManager::NoteUserDisconnect(GearDevice dev)
{
    pFrame->pGearDialog->NoteUserDisconnect(dev);
}

void EquipmentManager::Trace(const std::string& line)
{
    Debug.Write(wxString::FromUTF8(line.c_str()));
}

void EquipmentManager::RefreshFrameControls()
{
    assert(pCamera == m_camera);
    assert(pMount == m_mount);
    assert(pSecondaryMount == nullptr);

    pFrame->UpdateButtonsStatus();
}

bool EquipmentManager::AutoLoadCalibration() const
{
    return pFrame->GetAutoLoadCalibration() && (!pSecondaryMount || pSecondaryMount->IsConnected());
}

void EquipmentManager::LoadCalibration()
{
    Debug.Write("Auto-loading calibration data\n");
    pFrame->LoadCalibration();
}

void EquipmentManager::ClearCalibration()
{
    // Force a recalibration if the camera has changed
    Debug.Write("Clearing calibration data because camera was changed\n");
    pMount->ClearCalibration();
}

void EquipmentManager::HandleImageScaleChange()
{
    // the log line keeps its old text, which debug-log searches use
    Debug.Write("GearDialog::EndModal: imageScaleRatio changed\n");
    pFrame->HandleImageScaleChange();
}

void EquipmentManager::SendReconnected(GearDevice dev)
{
    Debug.Write(wxString::Format("connect retry: EquipmentReconnected %s\n", GearDeviceName(dev)));
    EvtServer.NotifyEquipmentReconnected(GearDeviceName(dev));
}

void EquipmentManager::RefreshAdvancedDialog()
{
    UpdateAdvancedDialog(true);
}

static void AutoLoadDefectMap()
{
    if (pConfig->Profile.GetBoolean("/camera/AutoLoadDefectMap", true))
    {
        Debug.AddLine("auto-loading defect map");
        pFrame->LoadDefectMapHandler(true);
    }
}

static void AutoLoadDarks()
{
    if (pConfig->Profile.GetBoolean("/camera/AutoLoadDarks", true))
    {
        Debug.AddLine("Auto-loading dark library");
        pFrame->LoadDarkHandler(true);
    }
}

static wxString SelectedCameraId(const wxString& camName)
{
    wxString key = CameraSelectionKey(camName);
    return pConfig->Profile.GetString(key, GuideCamera::DEFAULT_CAMERA_ID);
}

void EquipmentManager::ConnectCamera(bool autoReconnecting)
{
    GuideCamera *camera = m_camera;
    try
    {
        if (!camera)
        {
            throw ERROR_INFO("DoConnectCamera called with m_pCamera == NULL");
        }

        if (camera->Connected)
        {
            throw THROW_INFO("DoConnectCamera: called when connected");
        }

        wxString newCam = SelectedChoice(GearDevice::Camera);

        Debug.Write(wxString::Format(_T("gear_dialog: DoConnectCamera [%s]\n"), newCam));

        pFrame->StatusMsgNoTimeout(_("Connecting to Camera ..."));

        // keyed from the baseline, not newCam: after a set_selected_camera or a server change the id is read under the
        // name the profile was loaded with (inert for the Alpaca camera, which ignores the id)
        wxString cameraId = ::SelectedCameraId(m_baselineCamera);

        Debug.Write(wxString::Format("Connecting to camera [%s] id = [%s]\n", newCam, cameraId));

        int profileBinning = camera->GetBinning();
        ConnectRetry::ClearConnectError(GearDevice::Camera); // the driver records why the connect failed (#104)
        if (GuideCamera::ConnectCamera(camera, cameraId))
        {
            throw THROW_INFO("DoConnectCamera: connect failed");
        }

        // update camera pixel size from the driver, cam must be connected for reliable results
        double prevPixelSize = camera->GetProfilePixelSize();
        double pixelSize;
        bool err = camera->GetDevicePixelSize(&pixelSize);
        if (!err)
        {
            camera->SetCameraPixelSize(pixelSize);
            m_session.Latch().NotePixelSize(prevPixelSize, pixelSize);
        }

        Debug.Write(wxString::Format("DoConnectCamera: reconnecting=%d warningIssued=%d lastCam=[%s] scaleRatio=%.3f\n",
                                     autoReconnecting, m_session.Latch().warningIssued, m_baselineCamera,
                                     m_session.Latch().scaleRatio));

        // No very reliable way to know if cam selection has changed - id's and name strings may be the same for different cams
        // from same mfr so do what we can here including consideration of image scale change Purpose is to warn user of
        // potential loss of dark/bpm files and later, to adjust guide params as best we can
        if (!m_session.Latch().warningIssued && !autoReconnecting)
        {
            if (GearSelection::CameraChanged(m_baselineCamera.ToStdWstring(), newCam.ToStdWstring(),
                                             wxString(_("None")).ToStdWstring()) ||
                m_session.Latch().ScaleChanged())
            {
                int currProfileId = pConfig->GetCurrentProfileId();
                wxString darkName = MyFrame::DarkLibFileName(currProfileId);
                wxString bpmName = DefectMap::DefectMapFileName(currProfileId);

                m_session.Latch().NoteCameraChange();

                // Can't use standard checks because we don't want to consider sensor-size
                if (wxFileExists(darkName) || wxFileExists(bpmName))
                {
                    Debug.Write("DoConnectCamera: displaying camera-change warning\n");

                    // the connect arrived over JSON-RPC and nobody is there to answer a modal: proceed (the API
                    // client made the selection deliberately) but surface the warning as a non-modal alert
                    pFrame->Alert(_("Camera changed in this profile - the existing dark library / bad-pixel map "
                                    "may no longer match. Consider a separate profile per camera."));
                    m_session.Latch().warningIssued = true;
                    m_baselineCamera = newCam;
                }

                Debug.Write("DoConnectCamera: camera changed\n");
            }
        }

        // update default gain setting from the driver
        if (camera->HasGainControl && !pConfig->Profile.HasEntry("/camera/gain"))
        {
            int defaultGain = camera->GetDefaultCameraGain();
            Debug.Write(wxString::Format("Initializing camera gain to %d%%\n", defaultGain));
            camera->SetCameraGain(defaultGain);
        }

        // See if the profile was created with a binning level that isn't supported by
        // the camera (user mistake) - if so, reset binning to 1. Must be done here
        // because orig binning level is not saved
        auto choices = camera->GetBinningChoices();
        if (choices.find(profileBinning) == choices.end())
        {
            int rslt;
            if (TheScope())
            {
                rslt = TheScope()->GetCalibrationDuration() / profileBinning;
                TheScope()->SetCalibrationDuration(rslt);
            }
            camera->SetBinning(1, 1);
            Debug.Write(wxString::Format("CamConfigDlg correcting bogus user binning value from %d to 1\n", profileBinning));
        }

        // force re-build of camera tab in case Connect updated any of
        // the camera properties that influence the camera tab. For
        // example, binning options.
        m_updated[DeviceIndex(GearDevice::Camera)] = true;

        Debug.AddLine("Connected Camera: " + camera->Name);
        auto frameSize = camera->GetFrameSize();
        Debug.Write(wxString::Format("FrameSize=(%d,%d)\n", frameSize.x, frameSize.y));
        Debug.Write(wxString::Format("PixelSize=%.2f\n", camera->GetCameraPixelSize()));
        Debug.Write(wxString::Format("BitsPerPixel=%u\n", camera->BitsPerPixel()));
        Debug.Write(wxString::Format("HasGainControl=%d\n", camera->HasGainControl));

        if (camera->HasGainControl)
        {
            Debug.Write(wxString::Format("GuideCameraGain=%d\n", camera->GuideCameraGain));
        }

        Debug.Write(wxString::Format("HasShutter=%d\n", camera->HasShutter));
        Debug.Write(wxString::Format("HasSubFrames=%d\n", camera->HasSubframes));

        if (!autoReconnecting) // On a reconnect, this stuff is already established
        {
            AutoLoadDefectMap();
            if (!pCamera->CurrentDefectMap)
            {
                AutoLoadDarks();
            }
            pFrame->SetDarkMenuState();
        }

        pFrame->StatusMsg(_("Camera Connected"));
        GuidingAssistant::NotifyGearConnected();

        pFrame->UpdateStatusBarStateLabels();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        pFrame->StatusMsg(_("Camera Connect Failed"));
        pFrame->UpdateStatusBarStateLabels();
    }

    PublishGear();

    // #104: record the result from the final state. The auto-reconnect (#105) sends its own EquipmentReconnected,
    // and its failure is recorded by NoteCameraReconnectFailed once DoTryReconnect has sent EquipmentReconnectFailed.
    if (camera)
        pFrame->pGearDialog->NoteConnectResult(GearDevice::Camera, camera->Connected, autoReconnecting);
}

void EquipmentManager::DisconnectCamera()
{
    Debug.Write("gear_dialog: OnButtonDisconnectCamera\n");

    if (DarkBuildActive())
    {
        // A dark capture is using the camera on its own thread (#100); the build ends within one exposure.
        pFrame->Alert(_("Dark-frame capture in progress; try again when the build has finished."));
        return;
    }

    GuideCamera *camera = m_camera;
    try
    {
        if (!camera)
        {
            throw ERROR_INFO("OnButtonDisconnectCamera called with m_pCamera == NULL");
        }

        if (!camera->Connected)
        {
            throw THROW_INFO("OnButtonDisconnectCamera: called when not connected");
        }

        camera->Disconnect();
        NoteUserDisconnect(GearDevice::Camera);

        pFrame->StatusMsg(_("Camera Disconnected"));
        pFrame->UpdateStatusBarStateLabels();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    PublishGear();
}

void EquipmentManager::ConnectMount()
{
    Debug.Write("gear_dialog: OnButtonConnectScope\n");

    Scope *mount = m_mount;
    try
    {
        // the mount is NULL when scope selection is "None"

        if (mount && mount->IsConnected())
        {
            throw THROW_INFO("OnButtonConnectScope: called when connected");
        }

        if (mount)
        {
            pFrame->StatusMsgNoTimeout(_("Connecting to Mount ..."));

            Debug.Write(wxString::Format("Connecting to mount [%s]\n", SelectedChoice(GearDevice::Mount)));

            ConnectRetry::ClearConnectError(GearDevice::Mount); // the driver records why the connect failed (#104)
            if (mount->Connect())
            {
                throw THROW_INFO("OnButtonConnectScope: connect failed");
            }

            pFrame->StatusMsg(_("Mount Connected"));
            GuidingAssistant::NotifyGearConnected();
            pFrame->UpdateStatusBarStateLabels();
        }
        else
        {
            pFrame->UpdateStatusBarStateLabels();
        }

        Debug.AddLine("Connected Scope:" + (mount ? mount->Name() : wxString("None")));
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        pFrame->StatusMsg(_("Mount Connect Failed"));
        pFrame->UpdateStatusBarStateLabels();
    }

    PublishGear();

    // #104: from the final state, so the "called when connected" throw above is not a failure
    if (mount)
        pFrame->pGearDialog->NoteConnectResult(GearDevice::Mount, mount->IsConnected());
}

void EquipmentManager::DisconnectMount()
{
    Debug.Write("gear_dialog: OnButtonDisconnectScope\n");

    Scope *mount = m_mount;
    try
    {
        if (!mount)
        {
            throw ERROR_INFO("OnButtonDisconnectScope called with m_pScope == NULL");
        }

        if (!mount->IsConnected())
        {
            throw THROW_INFO("OnButtonDisconnectScope: called when not connected");
        }

        mount->Disconnect();
        NoteUserDisconnect(GearDevice::Mount);

        pFrame->StatusMsg(_("Mount Disconnected"));
        pFrame->UpdateStatusBarStateLabels();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    PublishGear();
}

void EquipmentManager::ConnectRotator()
{
    Debug.Write("gear_dialog: OnButtonConnectRotator\n");

    Rotator *rotator = m_rotator;
    try
    {
        // the rotator is NULL when stepguider selection is "None"

        if (rotator && rotator->IsConnected())
        {
            throw THROW_INFO("OnButtonConnectRotator: called when connected");
        }

        if (rotator)
        {
            pFrame->StatusMsgNoTimeout(_("Connecting to Rotator ..."));

            Debug.Write(wxString::Format("Connecting to rotator [%s]\n", SelectedChoice(GearDevice::Rotator)));

            ConnectRetry::ClearConnectError(GearDevice::Rotator); // the driver records why the connect failed (#104)
            if (rotator->Connect())
            {
                throw THROW_INFO("OnButtonConnectRotator: connect failed");
            }
        }

        if (rotator)
        {
            pFrame->StatusMsg(_("Rotator Connected"));
            pFrame->UpdateStatusBarStateLabels();
        }
        else
        {
            pFrame->UpdateStatusBarStateLabels();
        }

        Debug.AddLine("Connected Rotator:" + (rotator ? rotator->Name() : wxString("None")));
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        pFrame->StatusMsg(_("Rotator Connect Failed"));
        pFrame->UpdateStatusBarStateLabels();
    }

    PublishGear();

    // #104: from the final state, so the "called when connected" throw above is not a failure
    if (rotator)
        pFrame->pGearDialog->NoteConnectResult(GearDevice::Rotator, rotator->IsConnected());
}

void EquipmentManager::DisconnectRotator()
{
    Debug.Write("gear_dialog: OnButtonDisconnectRotator\n");

    Rotator *rotator = m_rotator;
    try
    {
        if (rotator == NULL)
        {
            throw ERROR_INFO("OnButtonDisconnectRotator called with m_pRotator == NULL");
        }

        if (!rotator->IsConnected())
        {
            throw THROW_INFO("OnButtonDisconnectRotator: called when not connected");
        }

        rotator->Disconnect();
        NoteUserDisconnect(GearDevice::Rotator);

        pFrame->StatusMsg(_("Rotator Disconnected"));
        pFrame->UpdateStatusBarStateLabels();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    PublishGear();
}

// The gear requests are sequenced by m_session (gear_session.cpp): the latch session (#139) and the request depth
// (#104: the retry timer, which can fire in any nested event loop, never starts an attempt in the middle of one), the
// order of the leaves, the no-op and refusal checks, the error texts and the one end of request.
static bool SessionResult(bool failed, const std::string& err, wxString *error)
{
    if (failed)
        *error = wxString::FromUTF8(err.c_str());
    return failed;
}

bool EquipmentManager::ConnectAll(wxString *error)
{
    std::string err;
    return SessionResult(m_session.ConnectAll(&err), err, error);
}

bool EquipmentManager::DisconnectAll(wxString *error)
{
    std::string err;
    return SessionResult(m_session.DisconnectAll(&err), err, error);
}

bool EquipmentManager::SetDeviceConnected(GearDevice device, bool connect, wxString *error)
{
    std::string err;
    return SessionResult(m_session.SetDevice(device, connect, &err), err, error);
}

void EquipmentManager::Shutdown(bool forced)
{
    Debug.Write(wxString::Format("Shutdown: forced=%d\n", forced));

    // OnClose stopped the retry before it stopped the worker threads; this is the backstop (#104)
    pFrame->pGearDialog->StopConnectRetry(true);

    Scope *mount = m_mount;
    if (!forced && mount && mount->IsConnected())
    {
        Debug.AddLine("Shutdown: disconnect scope");
        mount->Disconnect();
    }

    GuideCamera *camera = m_camera;
    if (!forced && camera && camera->Connected)
    {
        Debug.AddLine("Shutdown: disconnect camera");
        camera->Disconnect();
    }

    Rotator *rotator = m_rotator;
    if (rotator && rotator->IsConnected())
    {
        Debug.AddLine("Shutdown: disconnect rotator");
        rotator->Disconnect();
    }

    Debug.AddLine("Shutdown complete");
}
