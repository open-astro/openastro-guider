/*
 *  frame_events.cpp
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

#include "image_math.h"
#include "capture_cleanup.h"
#include "guide_move_plan.h"

#include <algorithm>
#include <memory>

#include <wx/spinctrl.h>
#include <wx/stdpaths.h>
#include <wx/textdlg.h>
#include <wx/textfile.h>
#include <wx/txtstrm.h>
#include <wx/utils.h>
#include <wx/wfstream.h>

void MyFrame::OnExposureDurationSelected(wxCommandEvent& evt)
{
    unsigned int idx = Dur_Choice->GetSelection();

    if (idx == 0)
    {
        // Auto-exposure
        if (!m_autoExp.enabled)
        {
            Debug.AddLine("AutoExp: enabled");
            m_autoExp.enabled = true;
            NotifyExposureChanged();
        }
    }
    else if (idx == Dur_Choice->GetCount() - 1)
    {
        // edit custom

        int customVal = *(GetExposureDurations().end() - 1);
        wxTextEntryDialog dlg(this, _("Custom exposure duration (seconds)"), _("Edit custom exposure"),
                              wxString::Format("%g", (double) customVal / 1000.), wxOK | wxCANCEL);
        if (dlg.ShowModal() == wxID_OK)
        {
            double val;
            if (dlg.GetValue().ToDouble(&val) && val > 0.0 && val < 3600.)
            {
                int ms = (int) (val * 1000.0);
                SetCustomExposureDuration(ms);
            }
        }

        // restore the actual selection
        if (m_autoExp.enabled)
        {
            Dur_Choice->SetSelection(0);
        }
        else
        {
            const std::vector<int>& dur(GetExposureDurations());
            auto pos = std::find(dur.begin(), dur.end(), m_exposureDuration);
            if (pos == dur.end())
                pos = std::find(dur.begin(), dur.end(), 1000); // safe fall-back, should not happen
            Dur_Choice->SetSelection(pos - dur.begin() + 1); // skip Auto
        }
    }
    else
    {
        // ordinary selection

        const std::vector<int>& dur(GetExposureDurations());
        int duration = dur[idx - 1];

        if (m_autoExp.enabled || m_exposureDuration != duration)
        {
            Debug.Write(wxString::Format("OnExposureDurationSelected: duration = %d\n", duration));

            m_exposureDuration = duration;
            m_autoExp.enabled = false;

            NotifyExposureChanged();

            if (pCamera)
            {
                // select the best matching dark frame
                pCamera->SelectDark(m_exposureDuration);
            }
        }
    }
}

void MyFrame::NotifyExposureChanged()
{
    NotifyGuidingParam("Exposure", ExposureDurationSummary());
    pConfig->Profile.SetInt("/ExposureDurationMs", m_autoExp.enabled ? -1 : m_exposureDuration);
}

int MyFrame::RequestedExposureDuration()
{
    if (!pCamera || !pCamera->Connected)
        return 0;

    return SelectedExposureDuration();
}

// The exposure the next frame would be taken with, whether or not the camera is connected: what get_exposure
// reports (#176). With Auto selected it is the current auto exposure.
int MyFrame::SelectedExposureDuration() const
{
    return m_singleExposure.enabled ? m_singleExposure.duration : m_exposureDuration;
}

void MyFrame::OnQuit(wxCommandEvent& WXUNUSED(event))
{
    Close(false);
}

void MyFrame::OnSave(wxCommandEvent& WXUNUSED(event))
{
    if (!pGuider->CurrentImage()->ImageData)
        return;

    wxString fname = wxFileSelector(_("Save FITS Image"), (const wxChar *) NULL, (const wxChar *) NULL, wxT("fit"),
                                    wxT("FITS files (*.fit)|*.fit"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT, this);

    if (fname.IsEmpty())
        return; // Check for canceled dialog

    if (pGuider->SaveCurrentImage(fname))
    {
        Alert(wxString::Format(_("The image could not be saved to %s"), fname));
    }
    else
    {
        pFrame->StatusMsg(wxString::Format(_("%s saved"), wxFileName(fname).GetFullName()));
    }
}

void MyFrame::OnIdle(wxIdleEvent& WXUNUSED(event)) { }

void MyFrame::StartLoopingInteractive(const wxString& context)
{
    Debug.Write(wxString::Format("StartLoopingInteractive: %s\n", context));

    if (!pCamera || !pCamera->Connected)
    {
        Debug.Write(_T("Camera not connected\n"));
        wxMessageBox(_("Please connect to a camera first"), _("Info"));
        return;
    }

    if (CaptureActive && !pGuider->IsCalibratingOrGuiding())
    {
        Debug.Write(_T("cannot start looping when capture active\n"));
        return;
    }

    StartLooping();
}

void MyFrame::OnButtonLoop(wxCommandEvent& WXUNUSED(event))
{
    StartLoopingInteractive(_T("Loop button clicked"));
}

void MyFrame::FinishStop(void)
{
    assert(!CaptureActive);
    if (m_singleExposure.enabled)
        m_singleExposure.Complete(true);
    EvtServer.NotifyLoopingStopped();
    // when looping resumes, start with at least one full frame. This enables applications
    // controlling PHD to auto-select a new star if the star is lost while looping was stopped.
    pGuider->ForceFullFrame();
    ResetAutoExposure();
    UpdateButtonsStatus();
    StatusMsg(_("Stopped."));
    wxString abortReason = m_captureFailureStopReason.empty() ? wxString("Stopped capturing") : m_captureFailureStopReason;
    m_captureFailureStopReason.clear();
    PhdController::AbortController(abortReason);
}

bool SingleExposure::Activate(int duration, wxByte binning, int gain, const wxRect& subframe, bool save, const wxString& path)
{
    // initiate a single exposure with the requested parameters. Returns true on success.
    // Parameters:
    //   duration - the exposure duration in milliseconds
    //   binning - the camera binning level - combined hardware + software binning
    //   gain - the camera gain value (phd2 gain units 0..100)
    //   subframe - the requested ROI; an empty wxRect means to use the full frame
    //   save - whether to automatically save the image when the exposure completes
    //   path - path to save the completed image, or empty string to allow phd2 to choose
    bool error;

    this->duration = duration;

    auto hwSwBinning = pCamera->GetHwAndSwBinning(binning);
    auto hwBinning = hwSwBinning.first;
    auto swBinning = hwSwBinning.second;

    prev_hw_binning = pCamera->HwBinning;
    prev_sw_binning = pCamera->SwBinning;
    error = pCamera->SetBinning(hwBinning, swBinning);
    if (error)
        return false;

    prev_gain = pCamera->GuideCameraGain;
    error = pCamera->SetCameraGain(gain);
    if (error)
        return false;

    this->subframe = subframe;
    this->save = save;
    this->path = path;

    enabled = true;
    return true;
}

void SingleExposure::Complete(bool succeeded, const wxString& errorMsgArg)
{
    wxString errorMsg(errorMsgArg);
    if (succeeded && save)
    {
        bool created = false;
        if (path.empty())
        {
            // save into the same directory that capture_single_frame's path
            // param is confined to, so the file is retrievable via
            // GET /api/capture/<filename> and by local clients alike
            wxString dir = pConfig->Global.GetString("/server/capture_frame_dir", MyFrame::GetDefaultFileDir());
            // GetDefaultFileDir self-creates, but a configured override may
            // not exist yet; without this, CreateTempFileName returns an
            // empty path and the save fails with an unhelpful error
            if (!wxDirExists(dir))
                wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
            path = wxFileName::CreateTempFileName(dir + PATHSEPSTR + "save_image_");
            created = true;
        }

        bool err = pFrame->pGuider->SaveCurrentImage(path);
        if (err)
        {
            if (created)
                ::wxRemove(path);
            succeeded = false;
            errorMsg = wxString::Format("error saving image file [%s] -- see debug log for more info", path);
        }
        else if (created)
        {
            // a remote client can fetch the file but not delete it (#93), so
            // drop older auto-named frames here; the one just written stays
            wxString dir = wxFileName(path).GetPath();
            size_t n = PruneAutoSavedFrames(std::string(dir.utf8_str()), kAutoSavedFrameKeepCount, kAutoSavedFrameMaxAge,
                                            std::string(path.utf8_str()));
            if (n)
                Debug.Write(wxString::Format("SingleExposure: pruned %u old saved frame(s) in [%s]\n", (unsigned) n, dir));
        }
    }

    Debug.Write(wxString::Format("SingleExposure::Complete ok=%d path=[%s]\n", succeeded, path));

    EvtServer.NotifySingleFrameComplete(succeeded, errorMsg, *this);

    if (pCamera->HwBinning != prev_hw_binning || pCamera->SwBinning != prev_sw_binning)
        pCamera->SetBinning(prev_hw_binning, prev_sw_binning);
    if (pCamera->GuideCameraGain != prev_gain)
        pCamera->SetCameraGain(prev_gain);
    enabled = false;
}

static wxString RawModeWarningKey(void)
{
    return wxString::Format("/Confirm/%d/RawModeWarningEnabled", pConfig->GetCurrentProfileId());
}

static void SuppressRawModeWarning(intptr_t)
{
    pConfig->Global.SetBoolean(RawModeWarningKey(), false);
}

static void WarnRawImageMode(void)
{
    if (pCamera->FrameSize != pCamera->DarkFrameSize())
    {
        if (!pCamera->LimitFrame.IsEmpty())
            // If a limited ROI is in effect, the camera's frame size may not match the
            // full frame size
            return;
        pFrame->SuppressibleAlert(RawModeWarningKey(),
                                  _("For refining the Bad-pixel Map PHD2 is now displaying raw camera data frames, which are a "
                                    "different size from ordinary guide frames for this camera."),
                                  SuppressRawModeWarning, 0);
    }
}

/*
 * OnExposeComplete is the dispatch routine that is called when an image has been taken
 * by the background thread.
 *
 * It:
 * - causes the image to be redrawn by calling pGuider->UpdateImageDisplay()
 * - calls the routine to update the guider state (which may do nothing)
 * - calls any other appropriate state update routine depending upon the current state
 * - updates button state based on appropriate state variables
 * - schedules another exposure if CaptureActive is still true
 *
 */
void MyFrame::OnExposeComplete(usImage *pNewFrame, bool err)
{
    try
    {
        Debug.Write("OnExposeComplete: enter\n");

        m_exposurePending = false;

        if (pGuider->GetPauseType() == PAUSE_FULL)
        {
            delete pNewFrame;
            Debug.Write("guider is paused, ignoring frame, not scheduling exposure\n");
            return;
        }

        if (err)
        {
            Debug.Write("OnExposeComplete: Capture Error reported\n");

            delete pNewFrame;

            CaptureFailures::Failure captureFailure = pCamera ? pCamera->TakeCaptureFailure() : CaptureFailures::Failure();

            // #97: the camera's reason for this failure, built once for every report of it:
            // "camera error: <reason>", or "camera error -- see debug log" when it gave none
            wxString failure = wxString::FromUTF8(CaptureFailures::Text(captureFailure).c_str());
            if (failure.empty())
                failure = CaptureFailures::kFallback;

            // #116: a calibration step's frame that failed with a transient error (Alpaca 1280, a timeout) is
            // exposed again instead of failing the calibration. The step's move finished before its exposure
            // started, so skipping UpdateGuideState repeats that step's frame without moving the mount again.
            // The camera already raised its alert, and the #103 tracker keeps counting, so a 3rd 1280 in a row
            // reconnects the camera instead. PhaseFor maps STATE_CALIBRATED to Other: that state lasts past a
            // frame only when guiding starts on an existing calibration, where no step is repeated.
            // #136: a guiding frame (guiding or settling) that failed the same way is dropped like a lost-star
            // frame instead of stopping guiding: StarLost with the #97 text, no guide pulse and no deduced move
            // (a failure can take up to the 30 s call timeout), the lock untouched, and a settle in progress
            // counts it as a dropped frame while its timeout keeps running. The same budget applies.
            CaptureFailures::FramePhase phase = CaptureFailures::PhaseFor(pGuider->GetState());
            if (m_frameRetries.OnFailedFrame(captureFailure, phase, m_continueCapturing && !m_singleExposure.enabled,
                                             pCamera && pCamera->Connected))
            {
                bool guiding = phase == CaptureFailures::FramePhase::Guiding;
                Debug.Write(wxString::Format("OnExposeComplete: %s frame failed (%s), retry %d of %d\n",
                                             guiding ? "guiding" : "calibration", failure, m_frameRetries.Count(),
                                             CaptureFailures::kFrameRetries));
                if (guiding)
                {
                    FrameDroppedInfo info {};
                    info.frameNumber = ++m_frameCounter;
                    info.time = TimeSinceGuidingStarted();
                    info.avgDist = CurrentGuideError();
                    info.status = failure;
                    GuideLog.FrameDropped(info);
                    EvtServer.NotifyStarLost(info);
                    PhdController::UpdateControllerState(true);
                }
                ScheduleExposure();
                return;
            }

            Debug.Write(wxString::Format("OnExposeComplete: %s\n", failure));

            bool stopping = !m_continueCapturing;
            // The camera the auto-reconnect brought back failed its first frame: that reconnect failed, and the
            // camera is disconnected below for the #104 retry. A frame cut short by a stop request proves nothing
            // either way, and the server reads the camera as connected, so that one confirms the reconnect.
            bool reconnectFailed = false;
            if (m_cameraReconnectUnconfirmed)
            {
                m_cameraReconnectUnconfirmed = false;
                if (stopping)
                    EvtServer.NotifyEquipmentReconnected("camera");
                else
                {
                    reconnectFailed = true;
                    EvtServer.NotifyEquipmentReconnectFailed(
                        "camera", m_cameraReconnectThrottle.Attempts(),
                        wxString::Format(_("the camera failed its first frame after reconnecting: %s"), failure));
                }
            }
            // the guide / dither request this ends fails with the reason, and so does an interrupted calibration
            // (NotifyGuidingStopped, FinishStop and Guider::StopGuiding read it, so the event order is unchanged)
            if (!stopping)
                m_captureFailureStopReason = failure;
            StopCapturing();
            m_captureFailureStopReason.clear(); // one-shot, even if StopCapturing did not reach FinishStop
            if (pGuider->IsCalibratingOrGuiding())
            {
                pGuider->StopGuiding();
                pGuider->UpdateImageDisplay();
            }
            if (m_singleExposure.enabled)
                m_singleExposure.Complete(false, failure);
            // #186: when capture was going on, StopCapturing reached FinishStop, which already sent
            // LoopingExposuresStopped; only a capture already stopping (or a single exposure) sends it here
            if (stopping)
                EvtServer.NotifyLoopingStopped();
            pGuider->Reset(false);
            CaptureActive = m_continueCapturing;
            UpdateButtonsStatus();
            PhdController::AbortController(stopping ? wxString("Image capture stopped") : failure);
            StatusMsg(_("Stopped."));

            if (reconnectFailed && pCamera->Connected)
            {
                Debug.Write("OnExposeComplete: disconnecting the camera whose reconnect was not confirmed\n");
                pCamera->Disconnect();
                pEquipment->NoteCameraReconnectFailed(failure);
            }

            throw ERROR_INFO("Error reported capturing image");
        }

        pNewFrame->FrameNum = ++m_frameCounter;
        m_frameRetries.OnGoodFrame(); // #116, #136: the retry budget is for consecutive failures

        // the first frame after a camera auto-reconnect: the camera is back
        if (m_cameraReconnectUnconfirmed)
        {
            m_cameraReconnectUnconfirmed = false;
            Debug.Write("OnExposeComplete: frame received, camera reconnect confirmed\n");
            EvtServer.NotifyEquipmentReconnected("camera");
        }

        if (m_rawImageMode && !m_rawImageModeWarningDone)
        {
            WarnRawImageMode();
            m_rawImageModeWarningDone = true;
        }

        // check for dark frame compatibility in case the frame size changed (binning changed)
        if (pCamera->DarkFrameSize() != m_prevDarkFrameSize)
        {
            CheckDarkFrameGeometry();
        }

        pGuider->UpdateGuideState(pNewFrame, !m_continueCapturing); // guider takes ownership of pNewFrame

        PhdController::UpdateControllerState();

        Debug.Write(wxString::Format("OnExposeComplete: CaptureActive=%d m_continueCapturing=%d\n", CaptureActive,
                                     m_continueCapturing));

        CaptureActive = m_continueCapturing;

        if (CaptureActive)
        {
            ScheduleExposure();
        }
        else
        {
            FinishStop();
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        UpdateButtonsStatus();
    }
}

void MyFrame::OnExposeComplete(wxThreadEvent& event)
{
    usImage *image = event.GetPayload<usImage *>();
    bool err = event.GetInt() != 0;
    OnExposeComplete(image, err);
}

void MyFrame::OnMoveComplete(wxThreadEvent& event_)
{
    try
    {
        MoveCompleteEvent& event = static_cast<MoveCompleteEvent&>(event_);

        if (event.moveOptions & MOVEOPT_MANUAL)
        {
            Debug.Write(wxString::Format("Manual Move completed, result = %d\n", event.result));
            ClearStatusBarGuiderInfo();
            return;
        }

        Mount *mount = event.mount;
        assert(mount->IsBusy());
        mount->DecrementRequestCount();

        Mount::MOVE_RESULT moveResult = event.result;

        mount->LogGuideStepInfo();

        // A dither's fast-recenter step counts what the mount moved (#165); an AO bump, the other
        // recovery-move kind, goes to the secondary mount
        if (mount == pMount && event.moveOptions == MOVEOPTS_RECOVERY_MOVE)
            pGuider->NotifyRecenterMoveComplete(event.mountMoved);

        // deliver the outstanding GuidingStopped notification if this is a late-arriving
        // move completion event
        if (!pGuider->IsCalibratingOrGuiding() && (!pMount || !pMount->IsBusy()) &&
            (!pSecondaryMount || !pSecondaryMount->IsBusy()))
        {
            pFrame->NotifyGuidingStopped();
        }

        // While guiding, a mount whose moves keep failing stops guiding (#213); a slew or the AO's
        // travel limit is handled below. With the mount's guide output off (set_guide_output_enabled,
        // the Guiding Assistant) MoveAxis refused the move, "Guiding disabled", and sent no pulse: a
        // refusal is not a failed move and leaves the count as it was
        bool stopForFailedMoves = false;
        if (pGuider->IsGuiding() && mount->GetGuidingEnabled())
        {
            guide_move_plan::FailedGuideMoves failed = guide_move_plan::AfterGuideMove(
                static_cast<guide_move_plan::MoveResult>(moveResult), mount->FailedGuideMovesInARow());
            mount->SetFailedGuideMovesInARow(failed.inARow);
            stopForFailedMoves = failed.stopGuiding && moveResult == Mount::MOVE_ERROR;
        }

        if (moveResult != Mount::MOVE_OK)
        {
            mount->IncrementErrorCount();

            if (moveResult == Mount::MOVE_ERROR_SLEWING)
            {
                Debug.Write("mount move error indicates guiding should stop\n");
                pGuider->StopGuiding();
            }
            else if (moveResult == Mount::MOVE_ERROR_AO_LIMIT_REACHED)
            {
                const StepInfo& step = TheAO()->GetFailedStepInfo();
                int newval = step.dx ? abs(step.x + step.dx) : abs(step.y + step.dy);

                if (pGuider->IsCalibrating())
                {
                    pFrame->Alert(wxString::Format(_("The AO exceeded its travel limit stepping"
                                                     " from (%d, %d) to (%d, %d) and calibration cannot proceed. Try reducing"
                                                     " the AO Travel setting to %d or lower and try again."),
                                                   step.x, step.y, step.x + step.dx, step.y + step.dy, newval));

                    pGuider->StopGuiding();
                }
                else // Guiding
                {
                    pFrame->Alert(wxString::Format(_("The AO exceeded its travel limit stepping"
                                                     " from (%d, %d) to (%d, %d) and has been re-centered. Try reducing the AO"
                                                     " Travel setting to %d or lower."),
                                                   step.x, step.y, step.x + step.dx, step.y + step.dy, newval));

                    // Attempt to restart guiding after centering, otherwise the AO will
                    // just bounce right back to the failing position
                    bool saveSticky = pGuider->LockPosIsSticky();
                    pGuider->SetLockPosIsSticky(false);
                    pGuider->StopGuiding();
                    pGuider->StartGuiding();
                    pGuider->SetLockPosIsSticky(saveSticky);
                }
            }
            else if (stopForFailedMoves)
            {
                Debug.Write(
                    wxString::Format("%d guide moves in a row failed, stopping guiding\n", mount->FailedGuideMovesInARow()));
                pFrame->Alert(wxString::Format(_("Guiding stopped: the last %d guide moves to %s failed. Its guide "
                                                 "pulses keep failing; check the mount and its connection."),
                                               mount->FailedGuideMovesInARow(), mount->Name()));
                pGuider->StopGuiding();
            }

            throw ERROR_INFO("Error reported moving");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }
}

void MyFrame::OnButtonStop(wxCommandEvent& WXUNUSED(event))
{
    Debug.Write("Stop button clicked\n");
    StopCapturing();
}

void MyFrame::OnButtonAutoStar(wxCommandEvent& WXUNUSED(event))
{
    if (!wxGetKeyState(WXK_SHIFT))
        AutoSelectStar();
    else
        pGuider->InvalidateCurrentPosition(true);
}

void MyFrame::OnGammaSlider(wxScrollEvent& WXUNUSED(event))
{
    int val = Gamma_Slider->GetValue();
    pConfig->Profile.SetInt("/Gamma", val);
    Stretch_gamma = (double) val / 100.0;
    pGuider->UpdateImageDisplay();
}

// Outside event handler because loading a dark library will automatically unload a defect map
bool MyFrame::LoadDarkHandler(bool checkIt)
{
    if (!pCamera || !pCamera->Connected)
    {
        Alert(_("You must connect a camera before loading a dark library"));
        m_useDarksMenuItem->Check(false);
        return false;
    }
    pConfig->Profile.SetBoolean("/camera/AutoLoadDarks", checkIt);
    if (checkIt) // enable it
    {
        m_useDarksMenuItem->Check(true);
        if (pCamera->CurrentDefectMap)
            LoadDefectMapHandler(false);
        if (LoadDarkLibrary())
            return true;
        else
        {
            m_useDarksMenuItem->Check(false);
            return false;
        }
    }
    else
    {
        if (!pCamera->CurrentDarkFrame)
        {
            m_useDarksMenuItem->Check(false); // shouldn't have gotten here
            return false;
        }
        pCamera->ClearDarks();
        m_useDarksMenuItem->Check(false);
        StatusMsg(_("Dark library unloaded"));
        return true;
    }
}

void MyFrame::OnLoadDark(wxCommandEvent& evt)
{
    LoadDarkHandler(evt.IsChecked());
    pFrame->UpdateStatusBarStateLabels();
}

// Outside event handler because loading a defect map will automatically unload a dark library
void MyFrame::LoadDefectMapHandler(bool checkIt)
{
    if (!pCamera || !pCamera->Connected)
    {
        Alert(_("You must connect a camera before loading a bad-pixel map"));
        darks_menu->FindItem(MENU_LOADDEFECTMAP)->Check(false);
        return;
    }
    pConfig->Profile.SetBoolean("/camera/AutoLoadDefectMap", checkIt);
    if (checkIt)
    {
        DefectMap *defectMap = DefectMap::LoadDefectMap(pConfig->GetCurrentProfileId());
        if (defectMap)
        {
            if (pCamera->CurrentDarkFrame)
                LoadDarkHandler(false);
            pCamera->SetDefectMap(defectMap);
            m_useDarksMenuItem->Check(false);
            m_useDefectMapMenuItem->Check(true);
            StatusMsg(_("Defect map loaded"));
        }
        else
        {
            StatusMsg(_("Defect map not loaded"));
        }
    }
    else
    {
        if (!pCamera->CurrentDefectMap)
        {
            m_useDefectMapMenuItem->Check(false); // Shouldn't have gotten here
            return;
        }
        pCamera->ClearDefectMap();
        m_useDefectMapMenuItem->Check(false);
        StatusMsg(_("Bad-pixel map unloaded"));
    }
}

void MyFrame::OnLoadDefectMap(wxCommandEvent& evt)
{
    LoadDefectMapHandler(evt.IsChecked());
    pFrame->UpdateStatusBarStateLabels();
}

void MyFrame::OnToolBar(wxCommandEvent& evt)
{
    if (evt.IsChecked())
    {
        // wxSize toolBarSize = MainToolbar->GetSize();
        m_mgr.GetPane(_T("MainToolBar")).Show().Bottom() /*.MinSize(toolBarSize)*/;
    }
    else
    {
        m_mgr.GetPane(_T("MainToolBar")).Hide();
    }
    m_mgr.Update();
}

// Re-dock windows and restore main window to size/position where everything should be readily accessible
void MyFrame::OnRestoreWindows(wxCommandEvent& evt)
{
    wxAuiPaneInfoArray& panes = m_mgr.GetAllPanes();

    // Start by restoring the main window although it doesn't seem like this could be much of a problem
    pFrame->SetSize(wxSize(800, 600));
    pFrame->SetPosition(wxPoint(20, 20)); // Should work on any screen size
    // Now re-dock all the windows that are being managed by wxAuiManager
    int lim = panes.GetCount();
    for (int i = 0; i < lim; i++)
    {
        panes.Item(i).Dock(); // Already docked, shown or not, doesn't matter
        if (panes.Item(i).name == _("Guider"))
            panes.Item(i).Show(true);
    }
    m_mgr.Update();
}

bool MyFrame::FlipCalibrationData()
{
    bool bError = false;
    Mount *scope = TheScope();

    if (scope)
    {
        bError = scope->FlipCalibration();

        if (!bError)
        {
            EvtServer.NotifyCalibrationDataFlipped(scope);
        }
    }

    return bError;
}

void MyFrame::OnAutoStar(wxCommandEvent& WXUNUSED(evt))
{
    AutoSelectStar();
}

static wxString DarksWarningEnabledKey()
{
    // we want the key to be under "/Confirm" so ConfirmDialog::ResetAllDontAskAgain() resets it, but we also want the setting
    // to be per-profile
    return wxString::Format("/Confirm/%d/DarksWarningEnabled", pConfig->GetCurrentProfileId());
}

static void SuppressDarksAlert(intptr_t)
{
    pConfig->Global.SetBoolean(DarksWarningEnabledKey(), false);
}

static void ValidateDarksLoaded(void)
{
    if (!pCamera->CurrentDarkFrame && !pCamera->CurrentDefectMap)
    {
        pFrame->SuppressibleAlert(DarksWarningEnabledKey(),
                                  _("For best results, use a Dark Library or a Bad-pixel Map "
                                    "while guiding. This will help prevent PHD from locking on to a hot pixel. "
                                    "Use the Darks menu to build a Dark Library or Bad-pixel Map."),
                                  SuppressDarksAlert, 0);
    }
}

void MyFrame::GuideButtonClick(bool interactive, const wxString& context)
{
    Debug.Write(wxString::Format(_T("GuideButtonClick i=%d ctx=%s\n"), interactive, context));

    try
    {
        if (pMount == NULL)
        {
            // no mount selected -- should never happen
            throw ERROR_INFO("pMount == NULL");
        }

        if (!pMount->IsConnected())
        {
            throw ERROR_INFO("Unable to guide with no scope Connected");
        }

        if (!pCamera || !pCamera->Connected)
        {
            throw ERROR_INFO("Unable to guide with no camera Connected");
        }

        if (pGuider->GetState() < STATE_SELECTED)
        {
            wxMessageBox(_T("Please select a guide star before attempting to guide"));
            throw ERROR_INFO("Unable to guide with state < STATE_SELECTED");
        }

        ValidateDarksLoaded();

        if (wxGetKeyState(WXK_SHIFT))
        {
            // Only if user did shift-click; calib may have been cleared by other means
            bool recalibrate = true;
            if (pMount->IsCalibrated() || (pSecondaryMount && pSecondaryMount->IsCalibrated()))
            {
                recalibrate = ConfirmDialog::Confirm(_("Are you sure you want to force recalibration?"),
                                                     "/force_recalibration_ok", _("Force Recalibration"));
            }
            if (recalibrate)
            {
                pMount->ClearCalibration();
                if (pSecondaryMount)
                    pSecondaryMount->ClearCalibration();
            }
            else
                return;
        }

        if (interactive && pPointingSource && pPointingSource->IsConnected() && pPointingSource->CanReportPosition())
        {
            // If it's being used, the ask-for-coordinates dialog will populate scope position info needed for
            // StartGuiding()
            if (pPointingSource->PreparePositionInteractive()) // Returns true only if user cancels ask-for-coordinates dialog
                return;
        }

        StartGuiding();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        pGuider->Reset(false);
    }
}

void MyFrame::OnButtonGuide(wxCommandEvent& WXUNUSED(event))
{
    GuideButtonClick(true, _T("Guide button clicked"));
}

void MyFrame::OnPanelClose(wxAuiManagerEvent& evt)
{
    wxAuiPaneInfo *p = evt.GetPane();
    if (p->name == _T("MainToolBar"))
    {
        Menubar->Check(MENU_TOOLBAR, false);
    }
}
