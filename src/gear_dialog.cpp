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
#include "imaging_gear_alpaca.h"
#include "profile_wizard.h"
#include "connect_probe.h"

#include <wx/gbsizer.h>
#include <chrono>
#include <functional>

// clang-format off
wxBEGIN_EVENT_TABLE(GearDialog, wxDialog)
    EVT_CHOICE(GEAR_PROFILES, GearDialog::OnProfileChoice)
    EVT_BUTTON(GEAR_PROFILE_MANAGE, GearDialog::OnButtonProfileManage)
    EVT_MENU(GEAR_PROFILE_NEW, GearDialog::OnProfileNew)
    EVT_MENU(GEAR_PROFILE_DELETE, GearDialog::OnProfileDelete)
    EVT_MENU(GEAR_PROFILE_RENAME, GearDialog::OnProfileRename)
    EVT_MENU(GEAR_PROFILE_LOAD, GearDialog::OnProfileLoad)
    EVT_MENU(GEAR_PROFILE_SAVE, GearDialog::OnProfileSave)
    EVT_MENU(BUTTON_ADVANCED, GearDialog::OnAdvanced)
    EVT_MENU(GEAR_PROFILE_WIZARD, GearDialog::OnButtonWizard)

    EVT_BUTTON(GEAR_BUTTON_CONNECT_ALL, GearDialog::OnButtonConnectAll)
    EVT_BUTTON(GEAR_BUTTON_DISCONNECT_ALL, GearDialog::OnButtonDisconnectAll)

    EVT_CHOICE(GEAR_CHOICE_CAMERA, GearDialog::OnChoiceCamera)
    EVT_BUTTON(GEAR_BUTTON_SELECT_CAMERA, GearDialog::OnButtonSelectCamera)
    EVT_MENU_RANGE(MENU_SELECT_CAMERA_BEGIN, MENU_SELECT_CAMERA_END, GearDialog::OnMenuSelectCamera)
    EVT_BUTTON(GEAR_BUTTON_SETUP_CAMERA, GearDialog::OnButtonSetupCamera)
    EVT_TOGGLEBUTTON(GEAR_BUTTON_CONNECT_CAMERA, GearDialog::OnButtonConnectCamera)
    EVT_TOGGLEBUTTON(GEAR_BUTTON_DISCONNECT_CAMERA, GearDialog::OnButtonDisconnectCamera)

    EVT_CHOICE(GEAR_CHOICE_SCOPE, GearDialog::OnChoiceScope)
    EVT_BUTTON(GEAR_BUTTON_SETUP_SCOPE, GearDialog::OnButtonSetupScope)
    EVT_TOGGLEBUTTON(GEAR_BUTTON_CONNECT_SCOPE, GearDialog::OnButtonConnectScope)
    EVT_TOGGLEBUTTON(GEAR_BUTTON_DISCONNECT_SCOPE, GearDialog::OnButtonDisconnectScope)

    EVT_CHOICE(GEAR_CHOICE_ROTATOR, GearDialog::OnChoiceRotator)
    EVT_BUTTON(GEAR_BUTTON_SETUP_ROTATOR, GearDialog::OnButtonSetupRotator)
    EVT_TOGGLEBUTTON(GEAR_BUTTON_CONNECT_ROTATOR, GearDialog::OnButtonConnectRotator)
    EVT_TOGGLEBUTTON(GEAR_BUTTON_DISCONNECT_ROTATOR, GearDialog::OnButtonDisconnectRotator)

    EVT_CHAR_HOOK(GearDialog::OnChar)
wxEND_EVENT_TABLE();
// clang-format on

/*
 * The Gear Dialog allows the user to select and connect to their hardware.
 *
 * The dialog looks something like this:
 *
 * +--------------------------------------------------------------------------+
 * |                                                                          |
 * |                               Help text                                  |
 * |                                                                          |
 * +--------------------------------------------------------------------------+
 * |                                   |    +------------------------+        |
 * |  Camera Selection                 |    |Camera Connection Button|        |
 * |                                   |    +------------------------+        |
 * +--------------------------------------------------------------------------+
 * |                                   |    +-----------------------+         |
 * |  Mount Selection                  |    |Mount Connection Button|         |
 * |                                   |    +-----------------------+         |
 * +--------------------------------------------------------------------------+
 * +--------------------------------------------------------------------------+
 * |                                   |    +-------------------------+       |
 * |  Rotator Selection                |    |Rotator Connection Button|       |
 * |                                   |    +-------------------------+       |
 * +--------------------------------------------------------------------------+
 * |             +-------------------+   +-------------------+                |
 * |             |    Connect All    |   |  Disconnect All   |                |
 * |             +-------------------+   +-------------------+                |
 * +--------------------------------------------------------------------------+
 */
GearDialog::GearDialog(wxWindow *pParent)
    : wxDialog(pParent, wxID_ANY, _("Connect Equipment"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX),
      m_cameraUpdated(false), m_mountUpdated(false), m_rotatorUpdated(false), m_showDarksDialog(false),
      m_camWarningIssued(false), m_camChanged(false), m_imageScaleRatio(1.0), m_flushConfig(false), m_connectDepth(0),
      m_connectRetryStopped(false), m_probeCancel(false), m_probeRunning(false), m_probeDevice(GearDevice::Camera),
      m_probeToken(0)
{
    m_connectRetryTimer.SetOwner(this);
    Bind(wxEVT_TIMER, &GearDialog::OnConnectRetryTimer, this, m_connectRetryTimer.GetId());

    m_pCamera = nullptr;
    m_pScope = nullptr;
    m_pRotator = nullptr;

    m_pCameras = nullptr;
    m_pScopes = nullptr;
    m_pRotators = nullptr;

    m_pConnectCameraButton = nullptr;
    m_pConnectScopeButton = nullptr;
    m_pConnectRotatorButton = nullptr;

    m_pConnectAllButton = nullptr;
    m_pDisconnectAllButton = nullptr;

    m_profiles = nullptr;
    m_btnProfileManage = nullptr;
    m_menuProfileManage = nullptr;

    Initialize();

    CentreOnParent();
}

GearDialog::~GearDialog()
{
    // a joinable std::thread must not be destroyed; OnClose has normally joined it already
    StopConnectRetry(true);

    delete m_pCamera;
    delete m_pScope;
    delete m_pRotator;

    // prevent double frees
    pCamera = nullptr;
    pMount = nullptr;
    pSecondaryMount = nullptr;
    pPointingSource = nullptr;
    pRotator = nullptr;

    delete m_menuProfileManage;
}

static wxToggleButton *MakeConnectBtn(wxWindow *parent, wxWindowID id)
{
#include "icons/connected.png.h"
    wxBitmap connected_bmp(wxBITMAP_PNG_FROM_DATA(connected));
#include "icons/disconnected.png.h"
    wxBitmap disconnected_bmp(wxBITMAP_PNG_FROM_DATA(disconnected));

    wxToggleButton *btn = new wxToggleButton(parent, id, _("Disconnect"), wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);

    btn->SetBitmap(disconnected_bmp);
    btn->SetBitmapPressed(connected_bmp);

    // Layout the button now with the wider Disconnected label
    btn->Layout();
    wxSize sz(btn->GetSize());
#ifdef __WXGTK__
    // WXGTK bug? button layout excludes bitmap size?!
    sz.SetWidth(sz.GetWidth() + connected_bmp.GetWidth());
#endif
    btn->SetMinSize(sz);

    return btn;
}

void GearDialog::Initialize()
{
    wxSizerFlags sizerFlags = wxSizerFlags().Align(wxALIGN_CENTER).Border(wxALL, 2).Expand();
    wxSizerFlags sizerTextFlags = wxSizerFlags().Align(wxALIGN_CENTER).Border(wxALL, 2).Expand();
    wxSizerFlags sizerLabelFlags = wxSizerFlags().Align(wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL).Border(wxALL, 2);
    wxSizerFlags sizerButtonFlags = wxSizerFlags().Align(wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL).Border(wxALL, 2).Expand();

    wxBoxSizer *pTopLevelSizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *profilesSizer = new wxBoxSizer(wxHORIZONTAL);
    profilesSizer->Add(new wxStaticText(this, wxID_ANY, _("Equipment profile")), sizerLabelFlags);
    m_profiles = new wxChoice(this, GEAR_PROFILES, wxDefaultPosition, wxDefaultSize, pConfig->ProfileNames());
    m_profiles->SetToolTip(_("Select the Equipment Profile you would like to use. PHD stores all of your settings and "
                             "equipment selections in an Equipment Profile. "
                             "You can create multiple profiles and switch back and forth between them."));
    m_profiles->SetStringSelection(pConfig->GetCurrentProfile());
    profilesSizer->Add(m_profiles, sizerButtonFlags);

    m_menuProfileManage = new wxMenu();
    m_menuProfileManage->Append(GEAR_PROFILE_WIZARD, _("New using Wizard..."),
                                _("Run the first-light wizard to create a new profile"));
    m_menuProfileManage->Append(GEAR_PROFILE_NEW, _("New"), _("Create a new profile, optionally copying from another profile"));
    m_menuProfileManage->Append(GEAR_PROFILE_DELETE, _("Delete"), _("Delete the selected profile"));
    m_menuProfileManage->Append(GEAR_PROFILE_RENAME, _("Rename"), _("Rename the selected profile"));
    m_menuProfileManage->Append(GEAR_PROFILE_LOAD, _("Import..."), _("Load a profile from a file"));
    m_menuProfileManage->Append(GEAR_PROFILE_SAVE, _("Export..."), _("Save the selected profile to a file"));
    m_menuProfileManage->Append(BUTTON_ADVANCED, _("Settings..."), _("Open the advanced settings dialog"));

    m_btnProfileManage = new OptionsButton(this, GEAR_PROFILE_MANAGE, _("Manage Profiles"));
    m_btnProfileManage->SetToolTip(_("Create a new Equipment Profile, or delete or rename the selected Equipment Profile"));
    profilesSizer->Add(m_btnProfileManage, sizerButtonFlags);

    pTopLevelSizer->Add(profilesSizer, wxSizerFlags().Align(wxALIGN_CENTER).Border(wxALL, 2));
    pTopLevelSizer->AddSpacer(10);

    // text at the top.  I tried (really really hard) to get it to resize/Wrap()
    // with the rest of the sizer, but it just didn't want to work, and I needed
    // to get the rest of the dialog working.
    wxStaticText *pText = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                           wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);
    int width, height;
    pText->SetLabel(
        _("Select your equipment below and click Connect All to connect, or click Disconnect All to disconnect. You can also "
          "connect or disconnect individual equipment items by clicking the button next to the item."));
    pText->GetTextExtent(_T("MMMMMMMMMM"), &width, &height);
    pText->Wrap(4 * width);
    pTopLevelSizer->Add(pText, sizerTextFlags.Align(wxALIGN_CENTER));

    // The Gear grid in the middle of the screen
    m_gearSizer = new wxGridBagSizer();
    pTopLevelSizer->Add(m_gearSizer, wxSizerFlags().Align(wxALIGN_CENTER).Border(wxALL, 2));

    // Camera
    m_gearSizer->Add(new wxStaticText(this, wxID_ANY, _("Camera"), wxDefaultPosition, wxDefaultSize), wxGBPosition(0, 0),
                     wxGBSpan(1, 1), wxALL | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL, 5);
    m_pCameras = new wxChoice(this, GEAR_CHOICE_CAMERA, wxDefaultPosition, wxDefaultSize, 0, nullptr, 0, wxDefaultValidator,
                              _("Camera"));
    m_gearSizer->Add(m_pCameras, wxGBPosition(0, 1), wxGBSpan(1, 1), wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);

#include "icons/select.png.h"
    wxBitmap select_bmp(wxBITMAP_PNG_FROM_DATA(select));
#include "icons/setup.png.h"
    wxBitmap setup_bmp(wxBITMAP_PNG_FROM_DATA(setup));

    m_selectCameraButton = new wxBitmapButton(this, GEAR_BUTTON_SELECT_CAMERA, select_bmp);
    m_selectCameraButton->SetToolTip(_("Select which camera to connect to when there are multiple cameras of the same type."));
    m_selectCameraButton->Enable(false);
    m_gearSizer->Add(m_selectCameraButton, wxGBPosition(0, 2), wxGBSpan(1, 1), wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);
    m_pSetupCameraButton = new wxBitmapButton(this, GEAR_BUTTON_SETUP_CAMERA, setup_bmp);
    m_pSetupCameraButton->SetToolTip(_("Camera Setup"));
    m_gearSizer->Add(m_pSetupCameraButton, wxGBPosition(0, 3), wxGBSpan(1, 1), wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);
    m_pConnectCameraButton = MakeConnectBtn(this, GEAR_BUTTON_CONNECT_CAMERA);
    m_gearSizer->Add(m_pConnectCameraButton, wxGBPosition(0, 4), wxGBSpan(1, 1),
                     wxBOTTOM | wxTOP | wxRIGHT | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);

    // mount
    m_gearSizer->Add(new wxStaticText(this, wxID_ANY, _("Mount"), wxDefaultPosition, wxDefaultSize), wxGBPosition(1, 0),
                     wxGBSpan(1, 1), wxALL | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL, 5);
    m_pScopes =
        new wxChoice(this, GEAR_CHOICE_SCOPE, wxDefaultPosition, wxDefaultSize, 0, nullptr, 0, wxDefaultValidator, _("Mount"));
    m_pScopes->SetToolTip(
        _("Specify how guide commands will be sent to the mount - via an ASCOM or INDI driver, directly from the camera, "
          "or via one of the GPxxx devices. An ASCOM connection is recommended."));
    m_gearSizer->Add(m_pScopes, wxGBPosition(1, 1), wxGBSpan(1, 1), wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);
    m_pSetupScopeButton = new wxBitmapButton(this, GEAR_BUTTON_SETUP_SCOPE, setup_bmp);
    m_pSetupScopeButton->SetToolTip(_("Mount Setup"));
    m_gearSizer->Add(m_pSetupScopeButton, wxGBPosition(1, 3), wxGBSpan(1, 1), wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);
    m_pConnectScopeButton = MakeConnectBtn(this, GEAR_BUTTON_CONNECT_SCOPE);
    m_gearSizer->Add(m_pConnectScopeButton, wxGBPosition(1, 4), wxGBSpan(1, 1),
                     wxBOTTOM | wxTOP | wxRIGHT | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);

    // rotator
    m_gearSizer->Add(new wxStaticText(this, wxID_ANY, _("Rotator"), wxDefaultPosition, wxDefaultSize), wxGBPosition(2, 0),
                     wxGBSpan(1, 1), wxALL | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL, 5);
    m_pRotators = new wxChoice(this, GEAR_CHOICE_ROTATOR, wxDefaultPosition, wxDefaultSize, 0, nullptr, 0, wxDefaultValidator,
                               _("Rotator"));
    m_gearSizer->Add(m_pRotators, wxGBPosition(2, 1), wxGBSpan(1, 1), wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);
    m_pSetupRotatorButton = new wxBitmapButton(this, GEAR_BUTTON_SETUP_ROTATOR, setup_bmp);
    m_pSetupRotatorButton->SetToolTip(_("Rotator Setup"));
    m_gearSizer->Add(m_pSetupRotatorButton, wxGBPosition(2, 3), wxGBSpan(1, 1), wxALL | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);
    m_pConnectRotatorButton = MakeConnectBtn(this, GEAR_BUTTON_CONNECT_ROTATOR);
    m_gearSizer->Add(m_pConnectRotatorButton, wxGBPosition(2, 4), wxGBSpan(1, 1),
                     wxBOTTOM | wxTOP | wxRIGHT | wxEXPAND | wxALIGN_CENTER_VERTICAL, 5);

    // Setup the bottom row of buttons

    wxBoxSizer *pButtonSizer = new wxBoxSizer(wxHORIZONTAL);
    m_pConnectAllButton = new wxButton(this, GEAR_BUTTON_CONNECT_ALL, _("Connect All"));
    m_pConnectAllButton->SetToolTip(_("Connect all equipment and close the equipment selection window"));
    pButtonSizer->Add(m_pConnectAllButton, sizerFlags);

    m_pDisconnectAllButton = new wxButton(this, GEAR_BUTTON_DISCONNECT_ALL, _("Disconnect All"));
    m_pDisconnectAllButton->SetToolTip(_("Disconnect all equipment"));
    pButtonSizer->Add(m_pDisconnectAllButton, sizerFlags);

    wxButton *closeBtn = new wxButton(this, wxID_CANCEL, _("Close"));
    pButtonSizer->Add(closeBtn, sizerFlags);

    pTopLevelSizer->Add(pButtonSizer, wxSizerFlags().Align(wxALIGN_TOP | wxALIGN_CENTER_HORIZONTAL).Border(wxALL, 2));

    // preselect the choices
    LoadGearChoices();

    // fit everything with the sizers
    SetSizerAndFit(pTopLevelSizer);

    UpdateAdvancedDialog(false);
}

static bool DeviceSelectionMatches(const wxString& val, const wxString& item)
{
    if (val.Contains("INDI"))
        return item.Contains("INDI");
    return val == item;
}

static void SetMatchingSelection(wxChoice *ctrl, const wxString& val)
{
    if (ctrl->SetStringSelection(val))
        return;

    // special case for INDI:  INDI device selection will be "INDI Camera" or "INDI Camera [indi driver name]"
    // allow either one to match
    for (int i = 0; i < ctrl->GetCount(); ++i)
    {
        const wxString& item = ctrl->GetString(i);
        if (DeviceSelectionMatches(val, item))
        {
            ctrl->SetSelection(i);
            return;
        }
    }
}

static void LoadChoices(wxChoice *ctl, const wxArrayString& ary)
{
    ctl->Freeze();
    ctl->Clear();
    ctl->Append(ary);
    ctl->Thaw();
}

static void LoadCameras(wxChoice *cameras)
{
    LoadChoices(cameras, GuideCamera::GuideCameraList());
}

static void LoadMounts(wxChoice *mounts)
{
    LoadChoices(mounts, Scope::MountList());
}

static void LoadRotators(wxChoice *rots)
{
    LoadChoices(rots, Rotator::RotatorList());
}

void GearDialog::LoadGearChoices()
{
    LoadCameras(m_pCameras);
    LoadMounts(m_pScopes);
    LoadRotators(m_pRotators);

    wxCommandEvent dummyEvent;
    m_lastCamera = pConfig->Profile.GetString("/camera/LastMenuChoice", wxEmptyString);
    if (m_lastCamera.IsEmpty())
        m_lastCamera = pConfig->Profile.GetString("/camera/LastMenuchoice", _("None"));
    if (m_lastCamera.IsEmpty())
        m_lastCamera = _("None");
    SetMatchingSelection(m_pCameras, m_lastCamera);
    OnChoiceCamera(dummyEvent);

    wxString lastScope = pConfig->Profile.GetString("/scope/LastMenuChoice", _("None"));
    SetMatchingSelection(m_pScopes, lastScope);
    OnChoiceScope(dummyEvent);

    wxString lastRotator = pConfig->Profile.GetString("/rotator/LastMenuChoice", _("None"));
    SetMatchingSelection(m_pRotators, lastRotator);
    OnChoiceRotator(dummyEvent);
}

int GearDialog::ShowGearDialog(bool autoConnect)
{
    int ret = wxID_OK;
    int callSuper = true;

    assert(pCamera == nullptr || pCamera == m_pCamera);

    m_camChanged = false;
    m_camWarningIssued = false;

    assert(pMount == nullptr || pMount == m_pScope);
    assert(pSecondaryMount == nullptr);

    if (autoConnect)
    {
        Debug.Write("gear_dialog: connect all (auto) calls OnButtonConnectAll\n");

        wxCommandEvent dummyEvent;
        OnButtonConnectAll(dummyEvent);

        if (m_pCamera && m_pCamera->Connected && (!m_pScope || m_pScope->IsConnected()) &&
            (!m_pRotator || m_pRotator->IsConnected()))
        {
            callSuper = false;
        }
    }

    if (callSuper)
    {
        m_imageScaleRatio = 1.0;
        UpdateButtonState();

        GetSizer()->Fit(this);
        CenterOnParent();

        wxWindow *top = wxGetApp().GetTopWindow();
        wxGetApp().SetTopWindow(this);
        ret = wxDialog::ShowModal();
        wxGetApp().SetTopWindow(top);
    }
    else
    {
        EndModal(ret);
    }

    if (m_flushConfig)
    {
        pConfig->Flush();
        m_flushConfig = false;
    }

    return ret;
}

void GearDialog::EndModal(int retCode)
{
    assert(pCamera == m_pCamera);
    assert(pMount == m_pScope);
    assert(pSecondaryMount == nullptr);

    pFrame->UpdateButtonsStatus();
    pFrame->pGraphLog->UpdateControls();
    pFrame->pTarget->UpdateControls();

    if (pFrame->GetAutoLoadCalibration() && !m_camChanged) // ok to reload calibration
    {
        if (pMount && pMount->IsConnected() && (!pSecondaryMount || pSecondaryMount->IsConnected()))
        {
            Debug.Write("Auto-loading calibration data\n");
            pFrame->LoadCalibration();
        }
    }
    else
    {
        // Force a recalibration if the camera has changed
        if (m_camChanged)
        {
            Debug.Write("Clearing calibration data because camera was changed\n");

            if (pMount && pMount->IsConnected())
                pMount->ClearCalibration();
        }
    }

    wxDialog::EndModal(retCode);

    UpdateAdvancedDialog(true);

    if (fabs(m_imageScaleRatio - 1.0) >= 0.01)
    {
        Debug.Write("GearDialog::EndModal: imageScaleRatio changed\n");
        pFrame->HandleImageScaleChange();
    }

    if (m_showDarksDialog)
    {
        m_showDarksDialog = false;
        if (pCamera && pCamera->Connected)
        {
            wxCommandEvent dummy;
            pFrame->OnDark(dummy);
        }
    }

    // #104: the one place EquipmentReconnected goes out for a device that had failed to connect, after the
    // calibration auto-load above; every connect path (set_connected, ConnectAll, guide, the retry) ends here
    SendPendingReconnected();
}

void GearDialog::UpdateCameraButtonState()
{
    // Now set up the buttons to match our current state
    if (!m_pCamera)
    {
        m_selectCameraButton->Enable(false);
        m_pSetupCameraButton->Enable(false);
        m_pConnectCameraButton->Enable(false);
        m_pConnectCameraButton->SetLabel(_("Connect"));
        m_pConnectCameraButton->SetValue(false);
        m_pConnectCameraButton->SetToolTip(_("Connect to camera"));
        m_pConnectCameraButton->SetId(GEAR_BUTTON_CONNECT_CAMERA);
        m_pCameras->Enable(true);
    }
    else
    {
        bool enablePropDlg = ((m_pCamera->PropertyDialogType & PROPDLG_WHEN_CONNECTED) != 0 && m_pCamera->Connected) ||
            ((m_pCamera->PropertyDialogType & PROPDLG_WHEN_DISCONNECTED) != 0 && !m_pCamera->Connected);
        m_pSetupCameraButton->Enable(enablePropDlg);

        m_pConnectCameraButton->Enable(true);

        if (m_pCamera->Connected)
        {
            m_selectCameraButton->Enable(false);
            m_pConnectCameraButton->SetLabel(_("Disconnect"));
            m_pConnectCameraButton->SetValue(true);
            m_pConnectCameraButton->SetToolTip(_("Disconnect from camera"));
            m_pConnectCameraButton->SetId(GEAR_BUTTON_DISCONNECT_CAMERA);
            m_pCameras->Enable(false);
        }
        else
        {
            m_selectCameraButton->Enable(m_pCamera->CanSelectCamera());
            m_pConnectCameraButton->SetLabel(_("Connect"));
            m_pConnectCameraButton->SetValue(false);
            m_pConnectCameraButton->SetToolTip(_("Connect to camera"));
            m_pConnectCameraButton->SetId(GEAR_BUTTON_CONNECT_CAMERA);
            m_pCameras->Enable(true);
        }
    }
}

void GearDialog::UpdateScopeButtonState()
{
    // Now set up the buttons to match our current state
    if (!m_pScope)
    {
        m_pSetupScopeButton->Enable(false);
        m_pConnectScopeButton->Enable(false);
        m_pConnectScopeButton->SetLabel(_("Connect"));
        m_pConnectScopeButton->SetValue(false);
        m_pConnectScopeButton->SetToolTip(_("Connect to mount"));
        m_pConnectScopeButton->SetId(GEAR_BUTTON_CONNECT_SCOPE);
        m_pScopes->Enable(true);
    }
    else
    {
        m_pSetupScopeButton->Enable(m_pScope->HasSetupDialog());
        m_pConnectScopeButton->Enable(true);

        if (m_pScope->IsConnected())
        {
            m_pConnectScopeButton->SetLabel(_("Disconnect"));
            m_pConnectScopeButton->SetValue(true);
            m_pConnectScopeButton->SetToolTip(_("Disconnect from mount"));
            m_pConnectScopeButton->SetId(GEAR_BUTTON_DISCONNECT_SCOPE);
            m_pScopes->Enable(false);
        }
        else
        {
            m_pConnectScopeButton->SetLabel(_("Connect"));
            m_pConnectScopeButton->SetValue(false);
            m_pConnectScopeButton->SetToolTip(_("Connect to mount"));
            m_pConnectScopeButton->SetId(GEAR_BUTTON_CONNECT_SCOPE);
            m_pScopes->Enable(true);

            m_pConnectScopeButton->Enable(true);
        }
    }
}

void GearDialog::UpdateRotatorButtonState()
{
    // Now set up the buttons to match our current state
    if (!m_pRotator)
    {
        m_pSetupRotatorButton->Enable(false);
        m_pConnectRotatorButton->Enable(false);
        m_pConnectRotatorButton->SetLabel(_("Connect"));
        m_pConnectRotatorButton->SetValue(false);
        m_pConnectRotatorButton->SetToolTip(_("Connect to Rotator"));
        m_pConnectRotatorButton->SetId(GEAR_BUTTON_CONNECT_ROTATOR);
        m_pRotators->Enable(true);
    }
    else
    {
        m_pConnectRotatorButton->Enable(true);

        if (m_pRotator->IsConnected())
        {
            m_pConnectRotatorButton->SetLabel(_("Disconnect"));
            m_pConnectRotatorButton->SetValue(true);
            m_pConnectRotatorButton->SetToolTip(_("Disconnect from Rotator"));
            m_pConnectRotatorButton->SetId(GEAR_BUTTON_DISCONNECT_ROTATOR);
            m_pRotators->Enable(false);
            m_pSetupRotatorButton->Enable(false);
        }
        else
        {
            m_pConnectRotatorButton->SetLabel(_("Connect"));
            m_pConnectRotatorButton->SetValue(false);
            m_pConnectRotatorButton->SetToolTip(_("Connect to Rotator"));
            m_pConnectRotatorButton->SetId(GEAR_BUTTON_CONNECT_ROTATOR);
            m_pRotators->Enable(true);
            m_pSetupRotatorButton->Enable(true);
        }
    }
}

void GearDialog::UpdateConnectAllButtonState()
{
    if ((m_pCamera && !m_pCamera->Connected) || (m_pScope && !m_pScope->IsConnected()) ||
        (m_pRotator && !m_pRotator->IsConnected()))
    {
        m_pConnectAllButton->Enable(true);
    }
    else
    {
        m_pConnectAllButton->Enable(false);
    }
}

void GearDialog::UpdateDisconnectAllButtonState()
{
    if ((m_pCamera && m_pCamera->Connected) || (m_pScope && m_pScope->IsConnected()) ||
        (m_pRotator && m_pRotator->IsConnected()))
    {
        m_pDisconnectAllButton->Enable(true);
        m_profiles->Enable(false);
        m_btnProfileManage->Enable(false);
    }
    else
    {
        m_pDisconnectAllButton->Enable(false);
        // allow profiles to be selected / modified only when everything is disconnected.
        m_profiles->Enable(true);
        m_btnProfileManage->Enable(true);
    }
}

void GearDialog::UpdateButtonState()
{
    UpdateGearPointers();

    UpdateCameraButtonState();
    UpdateScopeButtonState();
    UpdateRotatorButtonState();
    UpdateConnectAllButtonState();
    UpdateDisconnectAllButtonState();

    // equipment may have connected or disconnected
    ImagingGearLog::EquipmentChanged();
}

void GearDialog::OnButtonConnectAll(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonConnectAll\n");

    bool canceled = DoConnectCamera(false);
    if (canceled)
        return;
    OnButtonConnectScope(event);
    OnButtonConnectRotator(event);

    bool done = true;
    if (m_pCamera && !m_pCamera->Connected)
        done = false;
    if (m_pScope && !m_pScope->IsConnected())
        done = false;
    if (m_pRotator && !m_pRotator->IsConnected())
        done = false;

    if (done)
        EndModal(0);
}

void GearDialog::OnButtonDisconnectAll(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonDisconnectAll\n");

    OnButtonDisconnectScope(event);
    OnButtonDisconnectCamera(event);
    OnButtonDisconnectRotator(event);
}

// Handle 'Esc' char as close trigger
void GearDialog::OnChar(wxKeyEvent& evt)
{
    if (evt.GetKeyCode() == WXK_ESCAPE && !evt.HasModifiers())
    {
        EndModal(0);
    }
    else
    {
        evt.Skip();
    }
}

void GearDialog::OnChoiceCamera(wxCommandEvent& event)
{
    if (DarkBuildActive())
    {
        // A dark capture is using m_pCamera on its own thread (#100); the build ends within one exposure. The
        // choice widget already shows the new name, so put the camera in use back.
        SetMatchingSelection(m_pCameras, pConfig->Profile.GetString("/camera/LastMenuChoice", wxEmptyString));
        pFrame->Alert(_("Dark-frame capture in progress; try again when the build has finished."));
        return;
    }
    try
    {
        wxString choice = m_pCameras->GetStringSelection();
        if (choice.IsEmpty() && m_pCameras->GetCount() > 0)
        {
            m_pCameras->SetSelection(0);
            choice = m_pCameras->GetStringSelection();
        }
        NoteChosen(GearDevice::Camera, choice); // another camera: its retry state starts over (#104)

        delete m_pCamera;
        m_pCamera = nullptr;

        UpdateGearPointers();

        m_pCamera = GuideCamera::Factory(choice);

        Debug.AddLine(wxString::Format("Created new camera of type %s = %p", choice, m_pCamera));

        if (pConfig->Profile.GetString("/camera/LastMenuChoice", wxEmptyString) != choice)
        {
            pConfig->Profile.SetString("/camera/LastMenuChoice", choice);
            m_flushConfig = true;
        }

        m_selectCameraButton->Enable(m_pCamera && m_pCamera->CanSelectCamera());

        if (!m_pCamera)
        {
            throw THROW_INFO("OnChoiceCamera: m_pCamera == NULL");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    UpdateButtonState();

    m_cameraUpdated = true;
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

wxString GearDialog::CameraSelectionKey(const wxString& camName)
{
    std::hash<std::string> hash_fn;
    std::string name(camName.c_str());
    return wxString::Format("/cam_hash/%lx/whichCamera", (unsigned long) hash_fn(name));
}

static wxString SelectedCameraId(const wxString& camName)
{
    wxString key = GearDialog::CameraSelectionKey(camName);
    return pConfig->Profile.GetString(key, GuideCamera::DEFAULT_CAMERA_ID);
}

wxString GearDialog::SelectedCameraId() const
{
    return ::SelectedCameraId(m_lastCamera);
}

void GearDialog::OnButtonSelectCamera(wxCommandEvent& event)
{
    if (!m_pCamera || !m_pCamera->CanSelectCamera())
        return;

    if (m_pCamera->HandleSelectCameraButtonClick(event))
        return;

    wxArrayString names;
    m_cameraIds.clear(); // otherwise camera selection only works randomly as EnumCameras tends to append to the camera Ids
    bool error = m_pCamera->EnumCameras(names, m_cameraIds);
    if (error || names.size() == 0)
    {
        names.clear();
        names.Add(_("No cameras found"));
        m_cameraIds.clear();
    }

    wxString selectedId = ::SelectedCameraId(m_lastCamera);

    wxMenu *menu = new wxMenu();
    int id = MENU_SELECT_CAMERA_BEGIN;
    for (unsigned int idx = 0; idx < names.size(); idx++)
    {
        wxMenuItem *item = menu->AppendRadioItem(id, names.Item(idx));
        if (idx < m_cameraIds.size())
        {
            const wxString& camId = m_cameraIds[idx];
            if (camId == selectedId || (idx == 0 && selectedId == GuideCamera::DEFAULT_CAMERA_ID))
                item->Check(true);
        }
        if (++id > MENU_SELECT_CAMERA_END)
        {
            Debug.AddLine("Truncating camera list!");
            break;
        }
    }

    PopupMenu(menu, m_selectCameraButton->GetPosition().x,
              m_selectCameraButton->GetPosition().y + m_selectCameraButton->GetSize().GetHeight());

    delete menu;
}

void GearDialog::OnMenuSelectCamera(wxCommandEvent& event)
{
    unsigned int idx = event.GetId() - MENU_SELECT_CAMERA_BEGIN;
    if (idx < m_cameraIds.size())
    {
        wxString key = CameraSelectionKey(m_lastCamera);
        const wxString& id = m_cameraIds[idx];
        if (pConfig->Profile.GetString(key, wxEmptyString) != id)
        {
            pConfig->Profile.SetString(key, id);
            m_flushConfig = true;
        }
    }
}

void GearDialog::OnButtonSetupCamera(wxCommandEvent& event)
{
    if (DarkBuildActive())
    {
        // The property dialog reconfigures the camera a dark capture is using on its own thread (#100).
        pFrame->Alert(_("Dark-frame capture in progress; try again when the build has finished."));
        return;
    }
    m_pCamera->ShowPropertyDialog();

    // camera setup may have changed camera name so re-load the camera list
    wxString selection = m_pCameras->GetStringSelection();
    LoadCameras(m_pCameras);
    SetMatchingSelection(m_pCameras, selection);
}

bool GearDialog::DoConnectCamera(bool autoReconnecting)
{
    bool canceled = false;

    try
    {
        if (!m_pCamera)
        {
            throw ERROR_INFO("DoConnectCamera called with m_pCamera == NULL");
        }

        if (m_pCamera->Connected)
        {
            throw THROW_INFO("DoConnectCamera: called when connected");
        }

        wxString newCam = m_pCameras->GetStringSelection();

        Debug.Write(wxString::Format(_T("gear_dialog: DoConnectCamera [%s]\n"), newCam));

        pFrame->StatusMsgNoTimeout(_("Connecting to Camera ..."));

        wxString cameraId = ::SelectedCameraId(m_lastCamera);

        Debug.Write(wxString::Format("Connecting to camera [%s] id = [%s]\n", newCam, cameraId));

        int profileBinning = m_pCamera->GetBinning();
        ConnectRetry::ClearConnectError(GearDevice::Camera); // the driver records why the connect failed (#104)
        if (GuideCamera::ConnectCamera(m_pCamera, cameraId))
        {
            throw THROW_INFO("DoConnectCamera: connect failed");
        }

        // update camera pixel size from the driver, cam must be connected for reliable results
        double prevPixelSize = m_pCamera->GetProfilePixelSize();
        double pixelSize;
        bool err = m_pCamera->GetDevicePixelSize(&pixelSize);
        if (!err)
        {
            m_pCamera->SetCameraPixelSize(pixelSize);
            if (prevPixelSize != GuideCamera::UnknownPixelSize)
                m_imageScaleRatio *= pixelSize / prevPixelSize;
        }

        Debug.Write(wxString::Format("DoConnectCamera: reconnecting=%d warningIssued=%d lastCam=[%s] scaleRatio=%.3f\n",
                                     autoReconnecting, m_camWarningIssued, m_lastCamera, m_imageScaleRatio));

        // No very reliable way to know if cam selection has changed - id's and name strings may be the same for different cams
        // from same mfr so do what we can here including consideration of image scale change Purpose is to warn user of
        // potential loss of dark/bpm files and later, to adjust guide params as best we can
        if (!m_camWarningIssued && !autoReconnecting)
        {
            if ((m_lastCamera != _("None") && newCam != _("None") && !DeviceSelectionMatches(m_lastCamera, newCam)) ||
                (fabs(m_imageScaleRatio - 1.0) >= 0.01))
            {
                int currProfileId = pConfig->GetCurrentProfileId();
                wxString darkName = MyFrame::DarkLibFileName(currProfileId);
                wxString bpmName = DefectMap::DefectMapFileName(currProfileId);

                m_camChanged = true;

                // Can't use standard checks because we don't want to consider sensor-size
                if (wxFileExists(darkName) || wxFileExists(bpmName))
                {
                    Debug.Write("DoConnectCamera: displaying camera-change warning\n");

                    wxString msg = _("By changing cameras in this profile, you won't be able to use the existing dark library "
                                     "or bad-pixel maps. You should consider"
                                     " creating a new profile for this set-up.  Do you want to connect to this camera anyway?");
                    if (wxGetApp().IsHeadless())
                    {
                        // headless: the connect arrived over JSON-RPC and a modal
                        // would hang the daemon — proceed (the API client made the
                        // selection deliberately) but surface the warning as a
                        // non-modal alert
                        pFrame->Alert(_("Camera changed in this profile - the existing dark library / bad-pixel map "
                                        "may no longer match. Consider a separate profile per camera."));
                        m_camWarningIssued = true;
                        m_lastCamera = newCam;
                    }
                    else if (wxMessageBox(msg, _("Camera Change Warning"), wxYES_NO, this) == wxYES)
                    {
                        m_camWarningIssued = true;
                        m_lastCamera = newCam; // make consistent with what's in the UI
                    }
                    else
                    {
                        m_pCamera->Disconnect();
                        SetMatchingSelection(m_pCameras, m_lastCamera);
                        wxCommandEvent dummy;
                        OnChoiceCamera(dummy);
                        canceled = true;
                        m_camChanged = false;
                        throw THROW_INFO("DoConnectCamera: user cancelled after camera-change warning");
                    }
                }

                Debug.Write("DoConnectCamera: camera changed\n");
            }
        }

        // update default gain setting from the driver
        if (m_pCamera->HasGainControl && !pConfig->Profile.HasEntry("/camera/gain"))
        {
            int defaultGain = m_pCamera->GetDefaultCameraGain();
            Debug.Write(wxString::Format("Initializing camera gain to %d%%\n", defaultGain));
            m_pCamera->SetCameraGain(defaultGain);
        }

        // See if the profile was created with a binning level that isn't supported by
        // the camera (user mistake) - if so, reset binning to 1. Must be done here
        // because orig binning level is not saved
        auto choices = m_pCamera->GetBinningChoices();
        if (choices.find(profileBinning) == choices.end())
        {
            int rslt;
            if (TheScope())
            {
                rslt = TheScope()->GetCalibrationDuration() / profileBinning;
                TheScope()->SetCalibrationDuration(rslt);
            }
            m_pCamera->SetBinning(1, 1);
            Debug.Write(wxString::Format("CamConfigDlg correcting bogus user binning value from %d to 1\n", profileBinning));
        }

        // force re-build of camera tab in case Connect updated any of
        // the camera properties that influence the camera tab. For
        // example, binning options.
        m_cameraUpdated = true;

        Debug.AddLine("Connected Camera: " + m_pCamera->Name);
        auto frameSize = m_pCamera->GetFrameSize();
        Debug.Write(wxString::Format("FrameSize=(%d,%d)\n", frameSize.x, frameSize.y));
        Debug.Write(wxString::Format("PixelSize=%.2f\n", m_pCamera->GetCameraPixelSize()));
        Debug.Write(wxString::Format("BitsPerPixel=%u\n", m_pCamera->BitsPerPixel()));
        Debug.Write(wxString::Format("HasGainControl=%d\n", m_pCamera->HasGainControl));

        if (m_pCamera->HasGainControl)
        {
            Debug.Write(wxString::Format("GuideCameraGain=%d\n", m_pCamera->GuideCameraGain));
        }

        Debug.Write(wxString::Format("HasShutter=%d\n", m_pCamera->HasShutter));
        Debug.Write(wxString::Format("HasSubFrames=%d\n", m_pCamera->HasSubframes));

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
        pFrame->pStatsWin->UpdateCooler();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        pFrame->StatusMsg(_("Camera Connect Failed"));
        pFrame->UpdateStatusBarStateLabels();
    }

    UpdateButtonState();

    // #104: record the result from the final state. The auto-reconnect (#105) sends its own EquipmentReconnected,
    // and its failure is recorded by NoteCameraReconnectFailed once DoTryReconnect has sent EquipmentReconnectFailed.
    if (m_pCamera && !canceled)
        NoteConnectResult(GearDevice::Camera, m_pCamera->Connected, autoReconnecting);

    return canceled;
}

void GearDialog::OnButtonConnectCamera(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonConnectCamera\n");
    if (DarkBuildActive())
    {
        pFrame->Alert(_("Dark-frame capture in progress; try again when the build has finished."));
        return;
    }
    DoConnectCamera(false);
}

bool GearDialog::ReconnectCamera()
{
    Debug.Write("gear_dialog: ReconnectCamera\n");
    DoConnectCamera(true);
    bool err = !m_pCamera || !m_pCamera->Connected;
    return err;
}

void GearDialog::OnButtonDisconnectCamera(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonDisconnectCamera\n");

    if (DarkBuildActive())
    {
        // A dark capture is using m_pCamera on its own thread (#100); the build ends within one exposure.
        pFrame->Alert(_("Dark-frame capture in progress; try again when the build has finished."));
        return;
    }

    try
    {
        if (!m_pCamera)
        {
            throw ERROR_INFO("OnButtonDisconnectCamera called with m_pCamera == NULL");
        }

        if (!m_pCamera->Connected)
        {
            throw THROW_INFO("OnButtonDisconnectCamera: called when not connected");
        }

        m_pCamera->Disconnect();
        NoteUserDisconnect(GearDevice::Camera);

        pFrame->StatusMsg(_("Camera Disconnected"));
        pFrame->UpdateStatusBarStateLabels();
        pFrame->pStatsWin->UpdateCooler();
        pFrame->pStatsWin->ResetImageSize();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    UpdateButtonState();
}

void GearDialog::UpdateGearPointers()
{
    pCamera = m_pCamera;
    pMount = m_pScope;
    pSecondaryMount = nullptr;
    pPointingSource = m_pScope;
    pRotator = m_pRotator;
}

void GearDialog::OnChoiceScope(wxCommandEvent& event)
{
    try
    {
        wxString choice = m_pScopes->GetStringSelection();
        NoteChosen(GearDevice::Mount, choice); // another mount: its retry state starts over (#104)

        delete m_pScope;
        m_pScope = nullptr;
        UpdateGearPointers();

        m_pScope = Scope::Factory(choice);
        Debug.AddLine(wxString::Format("Created new scope of type %s = %p", choice, m_pScope));

        if (pConfig->Profile.GetString("/scope/LastMenuChoice", wxEmptyString) != choice)
        {
            pConfig->Profile.SetString("/scope/LastMenuChoice", choice);
            m_flushConfig = true;
        }

        if (!m_pScope)
        {
            throw THROW_INFO("OnChoiceScope: m_pScope == NULL");
        }

        m_ascomScopeSelected = choice.Contains("ASCOM");
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    UpdateButtonState();

    m_mountUpdated = true;
}

void GearDialog::OnButtonSetupScope(wxCommandEvent& event)
{
    m_pScope->SetupDialog();

    // scope setup may have changed the scope name so re-load the scope list
    wxString selection = m_pScopes->GetStringSelection();
    LoadMounts(m_pScopes);
    SetMatchingSelection(m_pScopes, selection);
}

void GearDialog::OnButtonConnectScope(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonConnectScope\n");

    try
    {
        // m_pScope is NULL when scope selection is "None"

        if (m_pScope && m_pScope->IsConnected())
        {
            throw THROW_INFO("OnButtonConnectScope: called when connected");
        }

        if (m_pScope)
        {
            pFrame->StatusMsgNoTimeout(_("Connecting to Mount ..."));

            Debug.Write(wxString::Format("Connecting to mount [%s]\n", m_pScopes->GetStringSelection()));

            ConnectRetry::ClearConnectError(GearDevice::Mount); // the driver records why the connect failed (#104)
            if (m_pScope->Connect())
            {
                throw THROW_INFO("OnButtonConnectScope: connect failed");
            }

            if (m_pScope && m_ascomScopeSelected && !m_pScope->CanPulseGuide())
            {
                m_pScope->Disconnect();
                wxMessageBox(wxString::Format(_("Mount does not support the required PulseGuide interface"), _("Error")));
                throw THROW_INFO("OnButtonConnectScope: PulseGuide commands not supported");
            }

            pFrame->StatusMsg(_("Mount Connected"));
            GuidingAssistant::NotifyGearConnected();
            pFrame->UpdateStatusBarStateLabels();
        }
        else
        {
            pFrame->UpdateStatusBarStateLabels();
        }

        Debug.AddLine("Connected Scope:" + (m_pScope ? m_pScope->Name() : wxString("None")));
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        pFrame->StatusMsg(_("Mount Connect Failed"));
        pFrame->UpdateStatusBarStateLabels();
    }

    UpdateButtonState();

    // #104: from the final state, so the "called when connected" throw above is not a failure
    if (m_pScope)
        NoteConnectResult(GearDevice::Mount, m_pScope->IsConnected());
}

void GearDialog::OnButtonDisconnectScope(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonDisconnectScope\n");

    try
    {
        if (!m_pScope)
        {
            throw ERROR_INFO("OnButtonDisconnectScope called with m_pScope == NULL");
        }

        if (!m_pScope->IsConnected())
        {
            throw THROW_INFO("OnButtonDisconnectScope: called when not connected");
        }

        m_pScope->Disconnect();
        NoteUserDisconnect(GearDevice::Mount);

        pFrame->StatusMsg(_("Mount Disconnected"));
        pFrame->UpdateStatusBarStateLabels();

        if (pFrame->pManualGuide)
        {
            pFrame->pManualGuide->Destroy();
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    UpdateButtonState();
}

void GearDialog::OnChoiceRotator(wxCommandEvent& event)
{
    try
    {
        wxString choice = m_pRotators->GetStringSelection();
        NoteChosen(GearDevice::Rotator, choice); // another rotator: its retry state starts over (#104)

        delete m_pRotator;
        m_pRotator = NULL;
        UpdateGearPointers();

        m_pRotator = Rotator::Factory(choice);
        Debug.AddLine(wxString::Format("Created new Rotator of type %s = %p", choice, m_pRotator));

        if (pConfig->Profile.GetString("/rotator/LastMenuChoice", wxEmptyString) != choice)
        {
            pConfig->Profile.SetString("/rotator/LastMenuChoice", choice);
            m_flushConfig = true;
        }

        if (!m_pRotator)
        {
            throw THROW_INFO("OnChoiceRotator: m_pRotator == NULL");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    UpdateButtonState();

    m_rotatorUpdated = true;
}

void GearDialog::OnButtonSetupRotator(wxCommandEvent& event)
{
    m_pRotator->ShowPropertyDialog();

    // setup dialog may have changed device name so re-load the list
    wxString selection = m_pRotators->GetStringSelection();
    LoadRotators(m_pRotators);
    SetMatchingSelection(m_pRotators, selection);
}

void GearDialog::OnButtonConnectRotator(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonConnectRotator\n");

    try
    {
        // m_pRotator is NULL when stepguider selection is "None"

        if (m_pRotator && m_pRotator->IsConnected())
        {
            throw THROW_INFO("OnButtonConnectRotator: called when connected");
        }

        if (m_pRotator)
        {
            pFrame->StatusMsgNoTimeout(_("Connecting to Rotator ..."));

            Debug.Write(wxString::Format("Connecting to rotator [%s]\n", m_pRotators->GetStringSelection()));

            ConnectRetry::ClearConnectError(GearDevice::Rotator); // the driver records why the connect failed (#104)
            if (m_pRotator->Connect())
            {
                throw THROW_INFO("OnButtonConnectRotator: connect failed");
            }
        }

        if (m_pRotator)
        {
            pFrame->StatusMsg(_("Rotator Connected"));
            pFrame->UpdateStatusBarStateLabels();
        }
        else
        {
            pFrame->UpdateStatusBarStateLabels();
        }

        Debug.AddLine("Connected Rotator:" + (m_pRotator ? m_pRotator->Name() : wxString("None")));
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        pFrame->StatusMsg(_("Rotator Connect Failed"));
        pFrame->UpdateStatusBarStateLabels();
    }

    UpdateButtonState();

    // #104: from the final state, so the "called when connected" throw above is not a failure
    if (m_pRotator)
        NoteConnectResult(GearDevice::Rotator, m_pRotator->IsConnected());
}

void GearDialog::OnButtonDisconnectRotator(wxCommandEvent& event)
{
    Debug.Write("gear_dialog: OnButtonDisconnectRotator\n");

    try
    {
        if (m_pRotator == NULL)
        {
            throw ERROR_INFO("OnButtonDisconnectRotator called with m_pRotator == NULL");
        }

        if (!m_pRotator->IsConnected())
        {
            throw THROW_INFO("OnButtonDisconnectRotator: called when not connected");
        }

        m_pRotator->Disconnect();
        NoteUserDisconnect(GearDevice::Rotator);

        pFrame->StatusMsg(_("Rotator Disconnected"));
        pFrame->UpdateStatusBarStateLabels();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    UpdateButtonState();
}

void GearDialog::OnButtonProfileManage(wxCommandEvent& event)
{
    PopupMenu(m_menuProfileManage, m_btnProfileManage->GetPosition().x,
              m_btnProfileManage->GetPosition().y + m_btnProfileManage->GetSize().GetHeight());
}

void GearDialog::OnButtonWizard(wxCommandEvent& event)
{
    // these variables must be initialized before instantiating the ProfileWizard, since
    // the instantiation has a side-effect of installing a temporary empty profile
    bool firstLight = IsEmptyProfile();
    wxString current = m_profiles->GetStringSelection();

    bool showGreeting =
        event.GetId() == 0; // Event id of 0 comes from "first light" launch; show first light UI panel only then
    bool darks_requested;
    if (EquipmentProfileWizard::ShowModal(this, showGreeting, &darks_requested))
    {
        // a new profile was created and set as the current profile
        // if this was a first-light run, we may have left an empty "My Equipment" profile behind - if so, delete it
        if (firstLight)
        {
            pConfig->DeleteProfile(current); // Can't be any dark/bpm files associated with it
        }

        wxArrayString profiles = pConfig->ProfileNames();
        m_profiles->Set(profiles);
        m_profiles->SetStringSelection(pConfig->GetCurrentProfile());
        Layout();

        wxCommandEvent dummy;
        OnProfileChoice(dummy);

        if (darks_requested)
        {
            m_showDarksDialog = true;
            // if wizard was launched from dialog and darks are requested, connect-all and close dialog
            if (IsVisible())
            {
                Debug.Write("gear_dialog: wizard connecting all gear\n");
                wxCommandEvent dummyEvent;
                OnButtonConnectAll(dummyEvent);
            }
        }
    }
}

void GearDialog::ShowProfileWizard()
{
    wxCommandEvent dummy;
    OnButtonWizard(dummy);
    if (m_showDarksDialog)
    {
        ShowGearDialog(true); // connect equipment and launch darks dialog
    }
}

void GearDialog::ShowProfileWizard(wxCommandEvent& evt)
{
    OnButtonWizard(evt);
    if (!IsEmptyProfile()) // User didn't just cancel out
    {
        ShowGearDialog(m_showDarksDialog); // Maybe auto-connect, close, launch darks; or just display dialog
    }
}

bool GearDialog::IsEmptyProfile()
{
    wxString lastCamera = pConfig->Profile.GetString("/camera/LastMenuChoice", wxEmptyString);
    if (lastCamera.IsEmpty())
        lastCamera = pConfig->Profile.GetString("/camera/LastMenuchoice", _("None"));
    if (lastCamera.IsEmpty())
        lastCamera = _("None");
    wxString lastScope = pConfig->Profile.GetString("/scope/LastMenuChoice", _("None"));
    return lastCamera == _("None") && lastScope == _("None");
}

void GearDialog::OnProfileChoice(wxCommandEvent& event)
{
    wxString prev = pConfig->GetCurrentProfile();
    wxString selection = m_profiles->GetStringSelection();
    if (selection != prev)
        m_flushConfig = true;
    pConfig->SetCurrentProfile(selection);
    // another profile: its devices start over, even one chosen under the same name (#104)
    for (GearDevice dev : { GearDevice::Camera, GearDevice::Mount, GearDevice::Rotator })
        ForgetDevice(dev);
    LoadGearChoices();
    pFrame->LoadProfileSettings();
    pFrame->pGuider->LoadProfileSettings();
    pFrame->UpdateTitle();
    pFrame->pGraphLog->ResetData();
}

bool GearDialog::SetProfile(int profileId, wxString *error)
{
    if (profileId == pConfig->GetCurrentProfileId())
        return false;

    if (IsModal())
    {
        // these error messages are internal to the event server and are not translated
        *error = "cannot set profile when Connect Equipment dialog is open";
        return true;
    }

    if ((m_pCamera && m_pCamera->Connected) || (m_pScope && m_pScope->IsConnected()) ||
        (m_pRotator && m_pRotator->IsConnected()))
    {
        *error = "cannot set profile when equipment is connected";
        return true;
    }

    if (!pConfig->ProfileExists(profileId))
    {
        *error = "invalid profile id";
        return true;
    }

    wxString profile = pConfig->GetProfileName(profileId);

    // the choice list only refreshes when the dialog (or the profile wizard)
    // runs, so a profile created over the API since then won't be in it yet —
    // refresh before selecting
    if (!m_profiles->SetStringSelection(profile))
    {
        m_profiles->Set(pConfig->ProfileNames());
        if (!m_profiles->SetStringSelection(profile))
        {
            *error = "invalid profile id";
            return true;
        }
    }

    // need the side-effects for making the selection
    wxCommandEvent dummy;
    OnProfileChoice(dummy);

    // need the side-effects of closing the dialog
    EndModal(0);

    return false;
}

namespace
{
// #104: counts the connect / disconnect calls on the stack, so the retry timer, which can fire in any nested event
// loop, never starts an attempt in the middle of one
struct ConnectDepthGuard
{
    int& depth;
    explicit ConnectDepthGuard(int& d) : depth(d) { ++depth; }
    ~ConnectDepthGuard() { --depth; }
    ConnectDepthGuard(const ConnectDepthGuard&) = delete;
    ConnectDepthGuard& operator=(const ConnectDepthGuard&) = delete;
};

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

bool GearDialog::ConnectAll(wxString *error)
{
    ConnectDepthGuard depthGuard(m_connectDepth);

    if (m_pCamera && m_pCamera->Connected && (!m_pScope || m_pScope->IsConnected()) &&
        (!m_pRotator || m_pRotator->IsConnected()))
    {
        // everything already connected
        return false;
    }

    if (pFrame->CaptureActive)
    {
        // these error messages are internal to the event server and are not translated
        *error = "cannot connect equipment when capture is active";
        return true;
    }

    if (IsModal())
    {
        *error = "cannot connect equipment when Connect Equipment dialog is open";
        return true;
    }

    Debug.Write("gear_dialog: ConnectAll calls OnButtonConnectAll\n");

    m_connectRetry.OnConnectRequested(GearDevice::All);

    wxCommandEvent dummyEvent;
    OnButtonConnectAll(dummyEvent);

    // need the side-effects of closing the dialog
    EndModal(0);

    // each failed device with the reason its driver gave, when there is one (#104)
    auto failed = [this](const char *label, GearDevice dev)
    {
        const wxString& reason = m_lastConnectError[DeviceIndex(dev)];
        return wxString(" ") + label + (reason.IsEmpty() ? wxString() : " (" + reason + ")");
    };
    wxString fail;
    if (!m_pCamera)
        fail += " camera";
    else if (!m_pCamera->Connected)
        fail += failed("camera", GearDevice::Camera);
    if (m_pScope && !m_pScope->IsConnected())
        fail += failed("mount", GearDevice::Mount);
    if (m_pRotator && !m_pRotator->IsConnected())
        fail += failed("Rotator", GearDevice::Rotator);

    if (fail.IsEmpty())
    {
        return false;
    }
    else
    {
        *error = "equipment failed to connect:" + fail;
        return true;
    }
}

bool GearDialog::DisconnectAll(wxString *error)
{
    ConnectDepthGuard depthGuard(m_connectDepth);

    if ((!m_pCamera || !m_pCamera->Connected) && (!m_pScope || !m_pScope->IsConnected()) &&
        (!m_pRotator || !m_pRotator->IsConnected()))
    {
        // nothing connected; still the user's word that none of it is to be connected behind their back (#104)
        NoteUserDisconnect(GearDevice::All);
        return false;
    }

    if (pFrame->CaptureActive)
    {
        // these error messages are internal to the event server and are not translated
        *error = "cannot disconnect equipment while capture active";
        return true;
    }

    if (IsModal())
    {
        *error = "cannot disconnect equipment when Connect Equipment dialog is open";
        return true;
    }

    Debug.Write("gear_dialog: DisconnectAll calls OnButtonDisconnectAll\n");

    wxCommandEvent dummy;
    OnButtonDisconnectAll(dummy);
    // the devices that were not connected (a failed mount) included, now that the disconnect was accepted (#104)
    NoteUserDisconnect(GearDevice::All);

    EndModal(0); // need the side effects

    return false;
}

bool GearDialog::SetDeviceConnected(GearDevice device, bool connect, wxString *error)
{
    if (device == GearDevice::All)
        return connect ? ConnectAll(error) : DisconnectAll(error);

    ConnectDepthGuard depthGuard(m_connectDepth);

    bool selected, connected;
    switch (device)
    {
    case GearDevice::Camera:
        selected = m_pCamera != nullptr;
        connected = selected && m_pCamera->Connected;
        break;
    case GearDevice::Mount:
        selected = m_pScope != nullptr;
        connected = selected && m_pScope->IsConnected();
        break;
    default:
        selected = m_pRotator != nullptr;
        connected = selected && m_pRotator->IsConnected();
        break;
    }

    const char *name = GearDeviceName(device);
    if (!selected)
    {
        if (!connect)
            return false; // nothing to disconnect
        *error = wxString::Format("no %s is selected in the profile", name);
        return true;
    }
    if (connected == connect)
    {
        // already in the requested state; a disconnect request for a device that failed to connect still means
        // "leave it disconnected" (#104)
        if (!connect)
            NoteUserDisconnect(device);
        return false;
    }

    std::string refusal = ConnectRefusal(device, connect, pFrame->CaptureActive,
                                         pFrame->pGuider && pFrame->pGuider->IsCalibratingOrGuiding(), IsModal());
    if (!refusal.empty())
    {
        *error = refusal;
        return true;
    }

    Debug.Write(wxString::Format("gear_dialog: SetDeviceConnected %s %d\n", name, connect));

    if (connect)
        m_connectRetry.OnConnectRequested(device);

    wxCommandEvent dummy;
    switch (device)
    {
    case GearDevice::Camera:
        if (connect)
            DoConnectCamera(false);
        else
            OnButtonDisconnectCamera(dummy);
        break;
    case GearDevice::Mount:
        if (connect)
            OnButtonConnectScope(dummy);
        else
            OnButtonDisconnectScope(dummy);
        break;
    default:
        if (connect)
            OnButtonConnectRotator(dummy);
        else
            OnButtonDisconnectRotator(dummy);
        break;
    }

    EndModal(0); // need the side effects (calibration auto-load, controls)

    bool now = device == GearDevice::Camera ? m_pCamera && m_pCamera->Connected
        : device == GearDevice::Mount       ? m_pScope && m_pScope->IsConnected()
                                            : m_pRotator && m_pRotator->IsConnected();
    if (now != connect)
    {
        *error = wxString::Format("%s failed to %s", name, connect ? "connect" : "disconnect");
        // the reason the driver gave (#104)
        if (connect && !m_lastConnectError[DeviceIndex(device)].IsEmpty())
            *error += " (" + m_lastConnectError[DeviceIndex(device)] + ")";
        return true;
    }
    return false;
}

void GearDialog::Shutdown(bool forced)
{
    Debug.Write(wxString::Format("Shutdown: forced=%d\n", forced));

    // OnClose stopped the retry before it stopped the worker threads; this is the backstop (#104)
    StopConnectRetry(true);

    if (!forced && m_pScope && m_pScope->IsConnected())
    {
        Debug.AddLine("Shutdown: disconnect scope");
        m_pScope->Disconnect();
    }

    if (!forced && m_pCamera && m_pCamera->Connected)
    {
        Debug.AddLine("Shutdown: disconnect camera");
        m_pCamera->Disconnect();
    }

    if (m_pRotator && m_pRotator->IsConnected())
    {
        Debug.AddLine("Shutdown: disconnect rotator");
        m_pRotator->Disconnect();
    }

    Debug.AddLine("Shutdown complete");
}

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
    switch (dev)
    {
    case GearDevice::Camera:
        return m_pCamera != nullptr;
    case GearDevice::Mount:
        return m_pScope != nullptr;
    default:
        return m_pRotator != nullptr;
    }
}

bool GearDialog::DeviceConnected(GearDevice dev) const
{
    switch (dev)
    {
    case GearDevice::Camera:
        return m_pCamera && m_pCamera->Connected;
    case GearDevice::Mount:
        return m_pScope && m_pScope->IsConnected();
    default:
        return m_pRotator && m_pRotator->IsConnected();
    }
}

// The same choice-string tests the factories route by, and the profile keys the Alpaca drivers read.
ConnectRetry::ProbeKind GearDialog::ProbeTargetFor(GearDevice dev, AlpacaProbeTarget *target) const
{
    wxString choice;
    bool alpaca;
    const char *deviceKey;
    switch (dev)
    {
    case GearDevice::Camera:
        choice = m_pCameras ? m_pCameras->GetStringSelection() : wxString();
        alpaca = choice.StartsWith(_T("Alpaca Camera"));
        deviceKey = "/alpaca/camera_device";
        break;
    case GearDevice::Mount:
        choice = m_pScopes ? m_pScopes->GetStringSelection() : wxString();
        alpaca = choice.Contains(_("Alpaca"));
        deviceKey = "/alpaca/telescope_device";
        break;
    default:
        choice = m_pRotators ? m_pRotators->GetStringSelection() : wxString();
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
    gates.alignmentTool = AlignmentToolActive();
    gates.guidingAssistant = GuidingAssistant::ApiIsRunning();
    gates.connectInProgress = m_connectDepth > 0;
    gates.captureActive = pFrame->CaptureActive;
    gates.calibratingOrGuiding = pFrame->pGuider && pFrame->pGuider->IsCalibratingOrGuiding();
    gates.dialogOpen = IsModal();
    return gates;
}

// Called at the end of each connect leaf (DoConnectCamera, OnButtonConnectScope, OnButtonConnectRotator) with the
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
        m_reconnectedPending.push_back(dev);
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

// From the choice handlers, which also run when the same device is chosen again (set_selected_* with the current
// selection, the re-selection set_alpaca_server makes): only another device forgets the state, so choosing the same
// one again does not cancel its retries.
void GearDialog::NoteChosen(GearDevice dev, const wxString& choice)
{
    if (m_connectRetry.Chosen(dev, ToStd(choice)))
        ForgetDevice(dev);
}

void GearDialog::SendPendingReconnected()
{
    std::vector<GearDevice> pending;
    pending.swap(m_reconnectedPending);
    for (GearDevice dev : pending)
    {
        Debug.Write(wxString::Format("connect retry: EquipmentReconnected %s\n", GearDeviceName(dev)));
        EvtServer.NotifyEquipmentReconnected(GearDeviceName(dev));
    }
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
        // the Static PA tool opening...) cancels the probe before its Connected=true PUT if that is not sent yet;
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
// set_connected (EndModal: controls, calibration auto-load, EquipmentReconnected). Quiet: a driver that fails after
// the server reported the device connected logs its Alert instead of raising one at every attempt.
void GearDialog::RetryConnect(GearDevice dev)
{
    uint64_t before = m_connectRetry.Results(dev);
    wxString err;
    Debug.Write(wxString::Format("connect retry: connecting the %s\n", GearDeviceName(dev)));
    {
        ConnectRetry::QuietConnectScope quiet;
        SetDeviceConnected(dev, true, &err);
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

        std::string refusal = ConnectRefusal(dev, true, pFrame->CaptureActive,
                                             pFrame->pGuider && pFrame->pGuider->IsCalibratingOrGuiding(), IsModal());
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
        SetDeviceConnected(dev, true, &err);
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
    if (!RetryEnabled() || m_connectRetryStopped || !m_pCamera || m_pCamera->Connected)
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

struct NewProfileDialog : public wxDialog
{
    wxTextCtrl *m_name;
    wxChoice *m_copyFrom;

    NewProfileDialog(wxWindow *parent);
};

NewProfileDialog::NewProfileDialog(wxWindow *parent) : wxDialog(parent, wxID_ANY, _("New Equipment Profile"))
{
    wxSizerFlags sizerLabelFlags = wxSizerFlags().Align(wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL).Border(wxALL, 2).Expand();
    wxSizerFlags sizerTextFlags = wxSizerFlags().Align(wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL).Border(wxALL, 2).Expand();
    wxSizerFlags sizerButtonFlags = wxSizerFlags().Align(wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL).Border(wxALL, 2).Expand();

    wxSizer *sizer1 = new wxBoxSizer(wxHORIZONTAL);
    sizer1->Add(
        new wxStaticText(this, wxID_ANY, _("Name"), wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL),
        sizerLabelFlags);
    wxSize size = GetTextExtent("MMMMMMMMMMMMMMMMMMMMMMMMMMMM");
    size.SetHeight(-1);
    m_name = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, size);
    m_name->SetToolTip(_("Enter the name of the new equipment profile"));
    sizer1->Add(m_name, sizerTextFlags);

    wxArrayString choices = pConfig->ProfileNames();
    choices.Insert(_("PHD Defaults"), 0);

    wxSizer *sizer2 = new wxBoxSizer(wxHORIZONTAL);
    sizer2->Add(new wxStaticText(this, wxID_ANY, _("Profile initial settings"), wxDefaultPosition, wxDefaultSize,
                                 wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL),
                sizerLabelFlags);
    m_copyFrom = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    m_copyFrom->SetSelection(0);
    m_copyFrom->SetToolTip(_("Select PHD Defaults to create a profile with default PHD settings, or select an existing "
                             "Equipment Profile to copy its settings into your new profile."));
    sizer2->Add(m_copyFrom, sizerTextFlags);

    wxSizer *sizer3 = new wxBoxSizer(wxVERTICAL);
    sizer3->Add(sizer1);
    sizer3->Add(sizer2);
    sizer3->Add(CreateButtonSizer(wxOK | wxCANCEL), sizerButtonFlags);

    sizer3->SetSizeHints(this);
    SetSizerAndFit(sizer3);
    CentreOnParent();
}

void GearDialog::OnProfileNew(wxCommandEvent& event)
{
    NewProfileDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    wxString newname = dlg.m_name->GetValue();
    if (newname.IsEmpty())
        return;

    if (pConfig->GetProfileId(newname) > 0)
    {
        wxMessageBox(wxString::Format(_("Cannot create profile %s, there is already a profile with that name"), newname),
                     _("Error"));
        return;
    }

    if (dlg.m_copyFrom->GetSelection() != 0)
    {
        wxString copyFrom = dlg.m_copyFrom->GetStringSelection();
        if (pConfig->CloneProfile(newname, copyFrom))
        {
            wxMessageBox(wxString::Format(_("Could not create profile %s from profile %s"), newname, copyFrom), _("Error"));
            return;
        }
    }

    if (pConfig->SetCurrentProfile(newname))
    {
        wxMessageBox(wxString::Format(_("Could not create profile %s"), newname), _("Error"));
        return;
    }

    wxArrayString profiles = pConfig->ProfileNames();
    m_profiles->Set(profiles);
    m_profiles->SetStringSelection(pConfig->GetCurrentProfile());
    Layout();

    wxCommandEvent dummy;
    OnProfileChoice(dummy);
}

void GearDialog::OnProfileDelete(wxCommandEvent& event)
{
    wxString current = m_profiles->GetStringSelection();
    int result = wxMessageBox(wxString::Format(_("Delete profile %s?"), current), _("Delete Equipment Profile"),
                              wxOK | wxCANCEL | wxCENTRE);
    if (result != wxOK)
        return;
    int id = pConfig->GetProfileId(current);
    if (id > 0)
        pFrame->DeleteDarkLibraryFiles(id);
    pConfig->DeleteProfile(current);
    wxArrayString profiles = pConfig->ProfileNames();
    m_profiles->Set(profiles);
    m_profiles->SetStringSelection(pConfig->GetCurrentProfile());
    Layout();

    wxCommandEvent dummy;
    OnProfileChoice(dummy);
}

void GearDialog::OnProfileRename(wxCommandEvent& event)
{
    wxString current = m_profiles->GetStringSelection();
    wxTextEntryDialog dlg(this, wxString::Format(_("Rename %s"), current), _("Rename Equipment Profile"), current);
    if (dlg.ShowModal() != wxID_OK)
        return;

    wxString newname = dlg.GetValue();
    if (newname.IsEmpty())
        return;

    if (pConfig->GetProfileId(newname) > 0)
    {
        wxMessageBox(wxString::Format(_("Cannot rename profile to %s, there is already a profile with that name"), newname),
                     _("Error"));
        return;
    }

    if (pConfig->RenameProfile(current, newname))
    {
        wxMessageBox(_("Could not rename profile"), _("Error"));
        return;
    }

    int sel = m_profiles->GetSelection();
    m_profiles->SetString(sel, newname);
    pFrame->UpdateTitle();
    Layout();
}

void GearDialog::OnProfileLoad(wxCommandEvent& event)
{
    wxString default_path = pConfig->Global.GetString("/profileFilePath", wxEmptyString);

    wxFileDialog dlg(this, _("Import PHD Equipment Profiles"), default_path, wxEmptyString,
                     wxT("PHD profile files (*.phd)|*.phd"), wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);

    if (dlg.ShowModal() == wxID_CANCEL)
    {
        return;
    }

    wxArrayString paths;
    dlg.GetPaths(paths);

    for (size_t i = 0; i < paths.GetCount(); i++)
    {
        wxString path = paths[i];

        if (i == 0)
            pConfig->Global.SetString("/profileFilePath", wxFileName(path).GetPath());

        pConfig->ReadProfile(path);
    }

    wxArrayString profiles = pConfig->ProfileNames();
    m_profiles->Set(profiles);
    m_profiles->SetStringSelection(pConfig->GetCurrentProfile());
    Layout();

    wxCommandEvent dummy;
    OnProfileChoice(dummy);
}

void GearDialog::OnProfileSave(wxCommandEvent& event)
{
    wxString default_path = pConfig->Global.GetString("/profileFilePath", wxEmptyString);
    wxString fname =
        wxFileSelector(_("Export PHD Equipment Profile"), default_path, pConfig->GetCurrentProfile() + wxT(".phd"), wxT("phd"),
                       wxT("PHD profile files (*.phd)|*.phd"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT, this);

    if (fname.IsEmpty())
    {
        // dialog canceled
        return;
    }

    pConfig->Global.SetString("/profileFilePath", wxFileName(fname).GetPath());
    if (!fname.EndsWith(_T(".phd")))
    {
        fname.Append(_T(".phd"));
    }

    if (pConfig->WriteProfile(fname))
    {
        wxLogError("Cannot write file '%s'.", fname);
    }
}

void GearDialog::UpdateAdvancedDialog(bool preLoad)
{
    MyFrame *frame = static_cast<MyFrame *>(GetParent()); // global pFrame may not have initialized yet

    if (m_cameraUpdated)
    {
        frame->pAdvancedDialog->UpdateCameraPage();
        m_cameraUpdated = false;
    }

    if (m_mountUpdated)
    {
        frame->pAdvancedDialog->UpdateMountPage();
        m_mountUpdated = false;
    }

    if (m_rotatorUpdated)
    {
        frame->pAdvancedDialog->UpdateRotatorPage();
        m_rotatorUpdated = false;
    }

    if (preLoad)
    {
        frame->pAdvancedDialog->Preload();
    }
}

void GearDialog::OnAdvanced(wxCommandEvent& event)
{
    UpdateAdvancedDialog(false);
    pFrame->OnAdvanced(event);
}
