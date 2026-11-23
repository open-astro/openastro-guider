/*
 *  advanced_dialog.cpp
 *  PHD Guiding
 *
 *  Created by Craig Stark.
 *  Copyright (c) 2006-2010 Craig Stark.
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
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
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
#include "calstep_dialog.h"

#include <wx/tipwin.h>

const double AdvancedDialog::MIN_FOCAL_LENGTH = 50.0;
const double AdvancedDialog::MAX_FOCAL_LENGTH = 10000.0;

// a place to save id of selected panel so we can select the same panel next time the dialog is opened
static int s_selectedPage = -1;

static void EnableValidators(wxWindow *win)
{
    win->SetExtraStyle(wxWS_EX_VALIDATE_RECURSIVELY);
    for (auto kid : win->GetChildren())
        EnableValidators(kid);
}

AdvancedDialog::AdvancedDialog(MyFrame *pFrame)
    : wxDialog(pFrame, wxID_ANY, _("Advanced Settings"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX),
      m_tip(nullptr), m_tipTimer(nullptr)
{
    /*
     * The advanced dialog is made up of a number of "on the fly" generated panels that configure different things.
     *
     * pTopLevelSizer is a top level Box Sizer in wxVERTICAL mode that contains a wxNotebook object
     * and an unnamed button sizer with OK and CANCEL buttons.
     *
     * Each tab of the notebook contains one or more ConfigDialogPane(s) which are basically vertical
     * sizers to hold a bunch of UI controls.  The UI controls are constructed and managed by ConfigDialogCtrlSet
     * objects.  These reflect the internal organization of the app and generally bind one-to-one with the major internal
     * classes: MyFrame, Guider, Camera, Mount, Scope, AO, Rotator, etc.  The controls created by the ConfigDialogCtrlSet
     * objects are laid out on the various panes by the ConfigDialogPane instances.  So there is a level of indirection here
     * such that the controls can generally be placed anywhere, and the ConfigDialogCtrlSet objects don't care.  This means the
     * overall UI can be optimized for end-users while allowing the underlying controls to reside where they should from an
     * internal architecture perspective.
     * +------------------------------------+------------------------------------+
     * | |   Notebook tabs                                                    |  |
     * | + -------------------------------------------------------------------+  |
     * | |                                                                    |  |
     * | |                                                                    |  |
     * | |           One or more config dialog panes on each tab of the       |  |
     * | |           notebook, possibly nested                                |  |
     * | |                                                                    |  |
     * | |                                                                    |  |
     * | |                                                                    |  |
     * | |                                                                    |  |
     * | |                                                                    |  |
     * + |                                                                    |  |
     * | |                                                                    |  |
     * + +--------------------------------------------------------------------+  |
     * |                              OK and Cancel Buttons                      |
     * +-------------------------------------------------------------------------+
     *
     */

    m_pNotebook = new wxNotebook(this, wxID_ANY);
    m_pFrame = pFrame; // We get called before global var is initialized

    // Ok and cancel buttons for the entire dialog box
    wxBoxSizer *pTopLevelSizer = new wxBoxSizer(wxVERTICAL);
    pTopLevelSizer->Add(m_pNotebook, wxSizerFlags(0).Expand().Border(wxALL, 5));
    wxSizer *bsz = CreateButtonSizer(wxOK | wxCANCEL);
    bsz->PrependStretchSpacer();
    wxButton *helpbtn = new wxButton(this, wxID_ANY, _("Help"));
#include "icons/help22.png.h"
    wxBitmap help_bmp(wxBITMAP_PNG_FROM_DATA(help22));
    helpbtn->SetBitmap(help_bmp, wxLEFT);
    // Bundled GUI help was removed for the headless build; open the online help.
    helpbtn->Bind(wxEVT_COMMAND_BUTTON_CLICKED,
                  [](wxCommandEvent& evt) { wxLaunchDefaultBrowser("https://openphdguiding.org/getting-help/"); });
    bsz->Prepend(helpbtn);
    pTopLevelSizer->Add(bsz, wxSizerFlags(0).Expand().Border(wxALL, 5));
    SetSizerAndFit(pTopLevelSizer);

    EnableValidators(this);
}

AdvancedDialog::~AdvancedDialog()
{
    delete m_tipTimer;
}

void AdvancedDialog::LoadValues()
{
    // Load all the current params
    m_imageScaleChanged = false;

    if (s_selectedPage != -1)
        m_pNotebook->ChangeSelection(s_selectedPage);
}

void AdvancedDialog::UnloadValues()
{
    // Unload all the current params
    if (m_imageScaleChanged)
    {
        m_pFrame->MakeImageScaleAdjustments();
        m_imageScaleChanged = false;
    }
}

// Any un-do ops need to be handled at the ConfigDialogPane level
void AdvancedDialog::Undo()
{
    m_imageScaleChanged = false;
}

void AdvancedDialog::EndModal(int retCode)
{
    s_selectedPage = m_pNotebook->GetSelection();
    wxDialog::EndModal(retCode);
}

// Properties and methods needed by step-size calculator dialog
int AdvancedDialog::GetFocalLength()
{
    return m_pFrame->GetFocalLength();
}

double AdvancedDialog::GetPixelSize()
{
    return pCamera ? pCamera->GetCameraPixelSize() : 0.0;
}

int AdvancedDialog::GetBinning()
{
    return pCamera ? pCamera->GetBinning() : 1;
}

struct ClearTipTimer : public wxTimer
{
    wxTipWindow **m_w;
    ClearTipTimer(wxTipWindow **w) : m_w(w) { }
    void Notify() override
    {
        if (*m_w)
            (*m_w)->Destroy();
    }
};

void AdvancedDialog::ShowInvalid(wxWindow *ctrl, const wxString& message)
{
    if (m_tip)
        return; // already another error shown

    ctrl->SetFocus();
    m_tip = new wxTipWindow(ctrl, message, 100, &m_tip);
    m_tip->SetPosition(ctrl->GetScreenPosition() + wxPoint(ctrl->GetSize().GetWidth(), 0));

    if (!m_tipTimer)
        m_tipTimer = new ClearTipTimer(&m_tip);
    enum
    {
        TIP_TIMER_MILLISECONDS = 9000
    };
    m_tipTimer->StartOnce(TIP_TIMER_MILLISECONDS);
}

bool AdvancedDialog::Validate()
{
    if (m_tip)
    {
        wxWindow *t = m_tip;
        m_tip = nullptr;
        delete t;
    }
    return wxDialog::Validate();
}
