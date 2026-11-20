/*
 *  eegg.cpp
 *  PHD Guiding
 *
 *  Created by Craig Stark.
 *  Copyright (c) 2007-2010 Craig Stark.
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
#include "guiding_assistant.h"

void MyFrame::OnEEGG(wxCommandEvent& evt)
{
    if (evt.GetId() == EEGG_STICKY_LOCK)
    {
        bool sticky = evt.IsChecked();
        pFrame->pGuider->SetLockPosIsSticky(sticky);
        pConfig->Global.SetBoolean("/StickyLockPosition", sticky);
    }
    else
    {
        evt.Skip();
    }
}

void MyFrame::OnGuidingAssistant(wxCommandEvent& WXUNUSED(evt))
{
    if (!pCamera)
    {
        wxMessageBox(_("Please connect a camera first."));
        return;
    }
    if (!TheScope())
    {
        wxMessageBox(_("Please connect a mount first."));
        return;
    }
    if (GuidingAssistant::ApiIsRunning())
    {
        wxMessageBox(_("The Guiding Assistant is already running (started remotely)."));
        return;
    }

    if (!pGuidingAssistant)
    {
        bool ok = true;

        if (pFrame->pGuider->IsGuiding())
        {
            ok = ConfirmDialog::Confirm(_("The Guiding Assitant will disable guide output and\n"
                                          "allow the guide star to drift.\n"
                                          "\n"
                                          "Ok to disable guide output?"),
                                        "/guiding_assistant_while_guiding", _("Confirm Disable Guiding"));
        }

        if (ok)
            pGuidingAssistant = GuidingAssistant::CreateDialogBox();
    }

    if (pGuidingAssistant)
    {
        pGuidingAssistant->Show();
    }
}
