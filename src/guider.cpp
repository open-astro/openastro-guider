/*
 *  guider.cpp
 *  PHD Guiding
 *
 *  Created by Bret McKee
 *  Copyright (c) 2012 Bret McKee
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
#include "edge_allowance.h"
#include "guide_move_plan.h"
#include "guider_state_machine.h"

// un-comment to log star deflections to a file
// #define CAPTURE_DEFLECTIONS

struct DeflectionLogger
{
#ifdef CAPTURE_DEFLECTIONS
    wxFFile *m_file;
    PHD_Point m_lastPos;
#endif

    void Init();
    void Uninit();
    void Log(const PHD_Point& pos);
};
static DeflectionLogger s_deflectionLogger;

#ifdef CAPTURE_DEFLECTIONS

void DeflectionLogger::Init()
{
    m_file = new wxFFile();
    wxDateTime now = wxDateTime::UNow();
    wxString pathname = Debug.GetLogDir() + PATHSEPSTR + now.Format(_T("star_displacement_%Y-%m-%d_%H%M%S.csv"));
    m_file->Open(pathname, "w");
    m_lastPos.Invalidate();
}

void DeflectionLogger::Uninit()
{
    delete m_file;
    m_file = 0;
}

void DeflectionLogger::Log(const PHD_Point& pos)
{
    if (m_lastPos.IsValid())
    {
        PHD_Point mountpt;
        pMount->TransformCameraCoordinatesToMountCoordinates(pos - m_lastPos, mountpt);
        m_file->Write(wxString::Format("%0.2f,%0.2f\n", mountpt.X, mountpt.Y));
    }
    else
    {
        m_file->Write(wxString::Format("DeltaRA, DeltaDec, Scale=%0.2f\n", pFrame->GetCameraPixelScale()));
        if (pMount->GetGuidingEnabled())
            pFrame->Alert("GUIDING IS ACTIVE!!!  Star displacements will be useless!");
    }

    m_lastPos = pos;
}

#else // CAPTURE_DEFLECTIONS

inline void DeflectionLogger::Init() { }
inline void DeflectionLogger::Uninit() { }
inline void DeflectionLogger::Log(const PHD_Point&) { }

#endif // CAPTURE_DEFLECTIONS

static const bool DefaultScaleImage = true;

Guider::Guider()
{
    m_state = STATE_UNINITIALIZED;
    Debug.Write(wxString::Format("guider state => %s\n", guider_state_machine::StateName(m_state)));
    m_paused = PAUSE_NONE;
    m_ditherRecenterStepsPending = 0;
    m_ditherRecenterStalledSteps = 0;
    m_recenteringForWorker = false;
    m_lockPosShift.shiftEnabled = false;
    m_lockPosShift.shiftRate.SetXY(0., 0.);
    m_lockPosShift.shiftUnits = UNIT_ARCSEC;
    m_lockPosShift.shiftIsMountCoords = true;
    m_lockPosIsSticky = false;
    m_forceFullFrame = false;
    m_measurementMode = false;
    m_searchRegion = 0;
    m_pCurrentImage = new usImage(); // so we always have one

    m_userMultiStarMode = GetMultiStarMode();

    s_deflectionLogger.Init();
}

Guider::~Guider()
{
    delete m_pCurrentImage;

    s_deflectionLogger.Uninit();
}

void Guider::LoadProfileSettings()
{
    bool enableFastRecenter = pConfig->Profile.GetBoolean("/guider/FastRecenter", true);
    EnableFastRecenter(enableFastRecenter);

    bool scaleImage = pConfig->Profile.GetBoolean("/guider/ScaleImage", DefaultScaleImage);
    SetScaleImage(scaleImage);

    double minHFD = pConfig->Profile.GetDouble("/guider/StarMinHFD", GetMinStarHFDDefault());
    // Handle upgrades from earlier releases that allowed zero MinHFD values.  Values below floor
    // are considered to be bogus and usually zero.  Set those to the default value (1.5).
    // Values between floor and default may be valid and would have come from specific user
    // configuration - so leave them alone.  Any values >= default are also acceptable and will be
    // left alone.  NOTE: first-generation LodeStar cameras create extremely low HFD values when binned 2x2
    if (minHFD < GetMinStarHFDFloor())
        minHFD = GetMinStarHFDFloor();
    SetMinStarHFD(minHFD);
    double maxHFD = pConfig->Profile.GetDouble("/guider/StarMaxHFD", 20.0);
    SetMaxStarHFD(maxHFD);

    double minSNR = pConfig->Profile.GetDouble("/guider/StarMinSNR", 6.0);
    SetAFMinStarSNR(minSNR);

    unsigned int autoSelDownsample = wxMax(0, pConfig->Profile.GetInt("/guider/AutoSelDownsample", 0));
    SetAutoSelDownsample(autoSelDownsample);

    // clear the display
    if (m_pCurrentImage->ImageData)
    {
        DisplayImage(new usImage());
    }
}

PauseType Guider::SetPaused(PauseType pause)
{
    Debug.Write(wxString::Format("Guider::SetPaused(%d)\n", pause));

    PauseType prev = m_paused;
    m_paused = pause;

    if (prev == PAUSE_FULL && pause != prev)
    {
        Debug.Write("Guider::SetPaused: resetting avg dist filter\n");
        m_distance.RequestReset();
    }

    return prev;
}

void Guider::ForceFullFrame()
{
    if (!m_forceFullFrame)
    {
        Debug.Write("setting force full frames = true\n");
        m_forceFullFrame = true;
    }
}

void Guider::EnableMeasurementMode(bool enable)
{
    if (enable)
    {
        if (m_state == STATE_GUIDING)
            m_measurementMode = true;
    }
    else
        m_measurementMode = false;
}

void Guider::EnableFastRecenter(bool enable)
{
    m_fastRecenterEnabled = enable;
    pConfig->Profile.SetInt("/guider/FastRecenter", m_fastRecenterEnabled);
}

bool Guider::SetScaleImage(bool newScaleValue)
{
    bool bError = false;

    try
    {
        m_scaleImage = newScaleValue;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    pConfig->Profile.SetBoolean("/guider/ScaleImage", m_scaleImage);
    return bError;
}

void Guider::UpdateImageDisplay(usImage *pImage)
{
    if (!pImage)
    {
        pImage = m_pCurrentImage;
    }

    Debug.Write(
        wxString::Format("UpdateImageDisplay: Size=(%d,%d) min=%u, max=%u, med=%u, FiltMin=%u, FiltMax=%u, Gamma=%.3f\n",
                         pImage->Size.x, pImage->Size.y, pImage->MinADU, pImage->MaxADU, pImage->MedianADU, pImage->FiltMin,
                         pImage->FiltMax, pFrame->Stretch_gamma));
}

bool Guider::SaveCurrentImage(const wxString& fileName)
{
    return m_pCurrentImage->Save(fileName);
}

void Guider::InvalidateLockPosition()
{
    if (m_lockPosition.IsValid())
        EvtServer.NotifyLockPositionLost();
    m_lockPosition.Invalidate();
}

bool Guider::SetLockPosition(const PHD_Point& position)
{
    bool bError = false;

    try
    {
        if (!position.IsValid())
        {
            throw ERROR_INFO("Point is not valid");
        }

        double x = position.X;
        double y = position.Y;
        Debug.Write(wxString::Format("setting lock position to (%.2f, %.2f)\n", x, y));

        if ((x < 0.0) || (x >= m_pCurrentImage->Size.x))
        {
            throw ERROR_INFO("invalid x value");
        }

        if ((y < 0.0) || (y >= m_pCurrentImage->Size.y))
        {
            throw ERROR_INFO("invalid y value");
        }

        if (!m_lockPosition.IsValid() || position.X != m_lockPosition.X || position.Y != m_lockPosition.Y)
        {
            EvtServer.NotifySetLockPosition(position);
            if (m_state == STATE_GUIDING)
            {
                // let guide algorithms react to the updated lock pos
                pMount->NotifyGuidingDithered(position.X - m_lockPosition.X, position.Y - m_lockPosition.Y, false);
                GuideLog.NotifySetLockPosition(this);
            }
        }

        m_lockPosition.SetXY(x, y);
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    return bError;
}

bool Guider::SetLockPosToStarAtPosition(const PHD_Point& starPosHint)
{
    bool error = SetCurrentPosition(m_pCurrentImage, starPosHint);

    if (!error && CurrentPosition().IsValid())
    {
        SetLockPosition(CurrentPosition());
    }

    return error;
}

bool Guider::MoveLockPosition(const PHD_Point& mountDeltaArg)
{
    bool err = false;

    try
    {
        if (!mountDeltaArg.IsValid())
        {
            throw ERROR_INFO("Point is not valid");
        }

        if (!pMount || !pMount->IsCalibrated())
        {
            throw ERROR_INFO("No mount");
        }

        const usImage *image = CurrentImage();

        // This handles dithers when the star is near the edge of the frame: the requested dither,
        // reflected in 4 directions along the RA/Dec axes (guider_state_machine::ChooseDitherDelta)
        guider_state_machine::DitherChoice choice = guider_state_machine::ChooseDitherDelta(
            { m_lockPosition.X, m_lockPosition.Y }, { mountDeltaArg.X, mountDeltaArg.Y }, image->Size.GetWidth(),
            image->Size.GetHeight(),
            [](const guider_state_machine::Point& mount, guider_state_machine::Point *camera)
            {
                PHD_Point tmpCamera;
                if (pMount->TransformMountCoordinatesToCameraCoordinates(PHD_Point(mount.x, mount.y), tmpCamera))
                    return true;
                *camera = { tmpCamera.X, tmpCamera.Y };
                return false;
            },
            [this](const guider_state_machine::Point& lockPos) { return IsValidLockPosition(PHD_Point(lockPos.x, lockPos.y)); },
            [](const std::string& line) { Debug.Write(wxString(line)); });
        if (!choice.ok)
        {
            throw ERROR_INFO("Transform failed");
        }

        PHD_Point cameraDelta(choice.camera.x, choice.camera.y), mountDelta(choice.mount.x, choice.mount.y);

        PHD_Point newLockPosition = m_lockPosition + cameraDelta;
        if (SetLockPosition(newLockPosition))
        {
            throw ERROR_INFO("SetLockPosition failed");
        }

        // update average distance right away so GetCurrentDistance reflects the increased distance from the dither
        double dist = cameraDelta.Distance(), distRA = fabs(mountDelta.X);
        m_distance.OnDither(dist, distRA);

        // no fast recenter for a zero-length dither (guider_state_machine::PlanFastRecenter)
        guider_state_machine::RecenterPlan recenter = guider_state_machine::PlanFastRecenter(
            IsFastRecenterEnabled(), dist, { mountDelta.X, mountDelta.Y }, GetMaxMovePixels());
        if (recenter.start)
        {
            m_ditherRecenterRemaining.SetXY(recenter.remaining.x, recenter.remaining.y);
            m_ditherRecenterStepsPending = 0; // a step of an earlier recenter no longer counts
            m_ditherRecenterStalledSteps = 0;
            m_recenteringForWorker = true;
            m_ditherRecenterDir.x = recenter.dirX;
            m_ditherRecenterDir.y = recenter.dirY;
            m_ditherRecenterStep.SetXY(recenter.step.x, recenter.step.y);
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        err = true;
    }

    return err;
}

// A fast-recenter step's move has completed (MyFrame::OnMoveComplete). mountMoved is what the mount
// moved of it (Mount::MoveOffset), invalid when MoveOffset failed before it sent a pulse (#165).
void Guider::NotifyRecenterMoveComplete(const PHD_Point& mountMoved)
{
    if (m_ditherRecenterStepsPending <= 0 || !m_ditherRecenterRemaining.IsValid())
    {
        Debug.Write("dither recenter: move completed for a recenter that has ended, ignored\n");
        return;
    }
    --m_ditherRecenterStepsPending;

    double movedX = mountMoved.IsValid() ? fabs(mountMoved.X) : 0.0;
    double movedY = mountMoved.IsValid() ? fabs(mountMoved.Y) : 0.0;
    guide_move_plan::RecenterProgress progress = guide_move_plan::AfterRecenterStep(
        m_ditherRecenterRemaining.X, m_ditherRecenterRemaining.Y, movedX, movedY, m_ditherRecenterStalledSteps);
    m_ditherRecenterStalledSteps = progress.stalledSteps;

    wxString outcome;
    if (progress.stalledSteps > 0)
        outcome = wxString::Format(", step moved nothing (%d in a row)", progress.stalledSteps);
    if (progress.done)
        outcome += ", done";
    Debug.Write(wxString::Format("dither recenter: moved=(%.1f,%.1f) remaining=(%.1f,%.1f)%s\n", movedX * m_ditherRecenterDir.x,
                                 movedY * m_ditherRecenterDir.y, progress.remainingX * m_ditherRecenterDir.x,
                                 progress.remainingY * m_ditherRecenterDir.y, outcome));

    if (progress.done)
    {
        // fast recenter is done; the guide algorithm corrects what is left
        m_ditherRecenterRemaining.Invalidate();
        m_ditherRecenterStepsPending = 0;
        m_ditherRecenterStalledSteps = 0;
        // reset distance tracker
        m_distance.RequestReset();
    }
    else
        m_ditherRecenterRemaining.SetXY(progress.remainingX, progress.remainingY);
}

static void CheckCalibrationAutoLoad();

// Performs the guider state machine's actions (guider_state_machine.h) on a Guider, each as the
// code it came from did
class GuiderStateHost : public guider_state_machine::Host
{
    Guider& m_guider;

public:
    // UpdateGuideState's frame, its dropped-frame info and its offset
    usImage *image = nullptr;
    FrameDroppedInfo *info = nullptr;
    GuiderOffset *ofs = nullptr;

    explicit GuiderStateHost(Guider& guider) : m_guider(guider) { }

    GUIDER_STATE State() const override { return m_guider.m_state; }
    void SetStateValue(GUIDER_STATE st) override { m_guider.m_state = st; }
    void RequestState(GUIDER_STATE st) override { m_guider.SetState(st); }
    void Log(const std::string& line) override { Debug.Write(wxString(line)); }

    bool PrimaryCalibrated() const override { return pMount->IsCalibrated(); }
    bool SecondaryConnected() const override { return pSecondaryMount && pSecondaryMount->IsConnected(); }
    bool SecondaryCalibrated() const override { return pSecondaryMount->IsCalibrated(); }
    bool MountsIdle() const override
    {
        return (!pMount || !pMount->IsBusy()) && (!pSecondaryMount || !pSecondaryMount->IsBusy());
    }
    bool LockPositionValid() const override { return m_guider.m_lockPosition.IsValid(); }
    bool LockPositionSticky() const override { return m_guider.m_lockPosIsSticky; }
    bool RecenterActive() const override { return m_guider.m_ditherRecenterRemaining.IsValid(); }
    bool MeasurementMode() const override { return m_guider.m_measurementMode; }

    bool Do(guider_state_machine::Action action) override;
};

bool GuiderStateHost::Do(guider_state_machine::Action action)
{
    using guider_state_machine::Action;
    Guider& g = m_guider;

    switch (action)
    {
    case Action::IllegalTransition:
        ERROR_INFO("Illegal state transition");
        break;
    case Action::InvalidateLockPosition:
        g.InvalidateLockPosition();
        break;
    case Action::InvalidateCurrentPosition:
        g.InvalidateCurrentPosition();
        break;
    case Action::ResetErrorCountPrimary:
        pMount->ResetErrorCount();
        break;
    case Action::BeginCalibrationPrimary:
        return pMount->BeginCalibration(g.CurrentPosition());
    case Action::BeginCalibrationFailedPrimary:
        Debug.Write(ERROR_INFO("pMount->BeginCalibration failed"));
        break;
    case Action::CalibrationStartedPrimary:
        GuideLog.StartCalibration(pMount);
        EvtServer.NotifyStartCalibration(pMount);
        break;
    case Action::ResetErrorCountSecondary:
        pSecondaryMount->ResetErrorCount();
        break;
    case Action::BeginCalibrationSecondary:
        return pSecondaryMount->BeginCalibration(g.CurrentPosition());
    case Action::BeginCalibrationFailedSecondary:
        Debug.Write(ERROR_INFO("pSecondaryMount->BeginCalibration failed"));
        break;
    case Action::CalibrationStartedSecondary:
        GuideLog.StartCalibration(pSecondaryMount);
        EvtServer.NotifyStartCalibration(pSecondaryMount);
        break;
    case Action::EnterGuiding:
        assert(pMount);

        g.m_ditherRecenterRemaining.Invalidate(); // reset dither fast recenter state
        g.m_ditherRecenterStepsPending = 0;
        g.m_ditherRecenterStalledSteps = 0;
        g.m_recenteringForWorker = false;

        // count the failed guide moves of this guiding run only (#213)
        pMount->SetFailedGuideMovesInARow(0);
        if (pSecondaryMount)
            pSecondaryMount->SetFailedGuideMovesInARow(0);

        pMount->AdjustCalibrationForScopePointing();
        if (pSecondaryMount)
        {
            pSecondaryMount->AdjustCalibrationForScopePointing();
        }

        pFrame->UpdateStatusBarCalibrationStatus();
        break;
    case Action::SetLockToCurrent:
        g.SetLockPosition(g.CurrentPosition());
        break;

    case Action::NotifyCalibrationFailedPrimary:
    case Action::NotifyCalibrationFailedSecondary:
        EvtServer.NotifyCalibrationFailed(action == Action::NotifyCalibrationFailedSecondary ? pSecondaryMount : pMount,
                                          pFrame->CaptureFailureStopReason().empty() ? _("Calibration manually stopped")
                                                                                     : pFrame->CaptureFailureStopReason());
        break;
    case Action::NotifyGuidingStopped:
        pFrame->NotifyGuidingStopped();
        break;

    case Action::NotifyLoopingNoStar:
        EvtServer.NotifyLooping(image->FrameNum, nullptr, info);
        break;
    case Action::NotifyStarLost:
        EvtServer.NotifyStarLost(*info);
        break;
    case Action::CalibrationFrameDropped:
        GuideLog.CalibrationFrameDropped(*info);
        break;
    case Action::StatusStarLost:
        pFrame->StatusMsg(_("star lost"));
        break;
    case Action::GuidingFrameDropped:
        GuideLog.FrameDropped(*info);
        break;
    case Action::RecordDroppedFrame:
        GuidingAssistant::NotifyFrameDropped(*info);
        pFrame->pGuideHistory->AppendData(*info);
        break;
    case Action::DeducedMove:
    {
        static GuiderOffset ZERO_OFS;
        pFrame->SchedulePrimaryMove(pMount, ZERO_OFS, MOVEOPTS_DEDUCED_MOVE);
        break;
    }
    case Action::LostStarAlert:
    {
        // the GUI's dark red flash and beep went with the guider window (#115 S5.3); the pause stays so the
        // lost-star frame's timing does not change
        wxMilliSleep(100);
        break;
    }

    case Action::LockOnSelectedStar:
        assert(g.CurrentPosition().IsValid());
        g.SetLockPosition(g.CurrentPosition());
        break;
    case Action::NotifyStarSelected:
        EvtServer.NotifyStarSelected(g.CurrentPosition());
        break;
    case Action::UpdateCalibrationPrimary:
        return pMount->UpdateCalibrationState(g.CurrentPosition());
    case Action::UpdateCalibrationSecondary:
        return pSecondaryMount->UpdateCalibrationState(g.CurrentPosition());
    case Action::GuidingStarted:
        pFrame->NotifyGuidingStarted();

        // camera angle is known, so ok to calculate shift rate camera coords
        g.UpdateLockPosShiftCameraCoords();
        if (g.LockPosShiftEnabled())
            GuideLog.NotifyLockShiftParams(g.m_lockPosShift, g.m_lockPosition.ShiftRate());

        CheckCalibrationAutoLoad();
        break;
    case Action::RecenterStep:
    {
        // fast recenter after dither taking large steps and bypassing
        // guide algorithms

        PHD_Point step(wxMin(g.m_ditherRecenterRemaining.X, g.m_ditherRecenterStep.X),
                       wxMin(g.m_ditherRecenterRemaining.Y, g.m_ditherRecenterStep.Y));

        Debug.Write(wxString::Format("dither recenter: remaining=(%.1f,%.1f) step=(%.1f,%.1f)\n",
                                     g.m_ditherRecenterRemaining.X * g.m_ditherRecenterDir.x,
                                     g.m_ditherRecenterRemaining.Y * g.m_ditherRecenterDir.y, step.X * g.m_ditherRecenterDir.x,
                                     step.Y * g.m_ditherRecenterDir.y));

        // The remaining distance goes down by what the mount moved of the step, when its move
        // completes (NotifyRecenterMoveComplete), not by the step: MoveAxis can cut or refuse it (#165)
        ofs->mountOfs.SetXY(step.X * g.m_ditherRecenterDir.x, step.Y * g.m_ditherRecenterDir.y);
        pMount->TransformMountCoordinatesToCameraCoordinates(ofs->mountOfs, ofs->cameraOfs);
        ++g.m_ditherRecenterStepsPending;
        // The next frame's exposure delay: still recentering unless this step, moved whole, ends it,
        // as when the step was counted on sending; set before the move and exposure are queued (#165)
        guide_move_plan::RecenterProgress ifWhole =
            guide_move_plan::AfterRecenterStep(g.m_ditherRecenterRemaining.X, g.m_ditherRecenterRemaining.Y, step.X, step.Y, 0);
        g.m_recenteringForWorker = !ifWhole.done;
        pFrame->SchedulePrimaryMove(pMount, *ofs, MOVEOPTS_RECOVERY_MOVE);
        // let guide algorithms know about the direct move
        pMount->NotifyDirectMove(ofs->mountOfs);
        break;
    }
    case Action::BacklashStep:
        GuidingAssistant::NotifyBacklashStep(g.CurrentPosition());
        break;
    case Action::GuideStep:
        // ordinary guide step
        s_deflectionLogger.Log(g.CurrentPosition());
        pFrame->SchedulePrimaryMove(pMount, *ofs, MOVEOPTS_GUIDE_STEP);
        break;
    case Action::RecenterOver:
        g.m_recenteringForWorker = false;
        break;
    }

    return false;
}

void Guider::SetState(GUIDER_STATE newState)
{
    try
    {
        GuiderStateHost host(*this);
        guider_state_machine::SetState(host, newState);
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }
}

void Guider::UpdateCurrentDistance(double distance, double distanceRA)
{
    m_distance.OnStarFound(IsGuiding(), distance, distanceRA);
}

// The averaged distance, or 100 px when no star was found in the last 20 s (guide_distance_tracker.cpp, #226)
double Guider::CurrentError(bool raOnly)
{
    return m_distance.CurrentError(raOnly);
}

double Guider::CurrentErrorSmoothed(bool raOnly)
{
    return m_distance.CurrentErrorSmoothed(raOnly);
}

void Guider::StartGuiding()
{
    // we set the state to calibrating.  The state machine will
    // automatically move from calibrating->calibrated->guiding
    // when it can
    SetState(STATE_CALIBRATING_PRIMARY);
}

void Guider::StopGuiding()
{
    GuiderStateHost host(*this);
    guider_state_machine::StopGuiding(host);
}

void Guider::Reset(bool fullReset)
{
    SetState(STATE_UNINITIALIZED);
    if (fullReset)
    {
        InvalidateCurrentPosition(true);
    }
}

static CalibrationGear CalibrationGearOf(Mount *mount)
{
    CalibrationGear gear;
    if (mount && mount->IsConnected())
    {
        gear.connected = true;
        gear.calibrated = mount->IsCalibrated();
        gear.calTotDist = mount->CalibrationTotDistance();
    }
    return gear;
}

int Guider::CalibrationEdgeAllowance(bool willForceRecal)
{
    // the rule is in the wx-free edge_allowance.cpp
    return EdgeAllowance(CalibrationGearOf(pMount), CalibrationGearOf(pSecondaryMount), willForceRecal);
}

bool Guider::SelectedStarClearForRecalibration() const
{
    // a star picked with no calibration margin (find_star on a calibrated mount) can be too close to the edge for
    // the walk of the calibration that guide with recalibrate: true runs on it (#140); rule in edge_allowance.cpp
    const usImage *image = CurrentImage();
    const PHD_Point& pos = CurrentPosition();
    if (!image || !image->ImageData || !pos.IsValid())
        return true; // nothing to check
    return StarClearForRecalibration(pos.X, pos.Y, image->Size.GetWidth(), image->Size.GetHeight(), m_searchRegion,
                                     CalibrationGearOf(pMount), CalibrationGearOf(pSecondaryMount));
}

// Called from the alert to offer auto-restore calibration
static void SetAutoLoad(intptr_t)
{
    pFrame->SetAutoLoadCalibration(true);
    pFrame->m_infoBar->Dismiss();
}

// Generate an alert if the user is likely to be missing the opportunity for auto-restore of
// the just-completed calibration
static void CheckCalibrationAutoLoad()
{
    int autoLoadProfileVal = pConfig->Profile.GetInt("/AutoLoadCalibration", -1);
    bool shouldAutoLoad = pPointingSource && pPointingSource->CanReportPosition();

    if (autoLoadProfileVal == -1)
    {
        // new profile, assert appropriate choice as default
        pFrame->SetAutoLoadCalibration(shouldAutoLoad);
    }
    else if (autoLoadProfileVal == 0 && shouldAutoLoad)
    {
        bool alreadyAsked = pConfig->Profile.GetBoolean("/AlreadyAskedCalibAutoload", false);

        if (!alreadyAsked)
        {
            wxString msg =
                wxString::Format(_("Do you want to automatically restore this calibration whenever the profile is used?"));
            pFrame->Alert(msg, 0, "Auto-restore", &SetAutoLoad, 0, false, 0);
            pConfig->Profile.SetBoolean("/AlreadyAskedCalibAutoload", true);
        }
    }
}

void Guider::DisplayImage(usImage *img)
{
    if (IsCalibratingOrGuiding())
        return;

    // switch in the new image
    usImage *prev = m_pCurrentImage;
    m_pCurrentImage = img;

    ImageLogger::SaveImage(prev);

    UpdateImageDisplay();
}

inline static bool IsLoopingState(GUIDER_STATE state)
{
    // returns true for looping, but non-guiding states
    switch (state)
    {
    case STATE_UNINITIALIZED:
    case STATE_SELECTING:
    case STATE_SELECTED:
        return true;

    case STATE_CALIBRATING_PRIMARY:
    case STATE_CALIBRATING_SECONDARY:
    case STATE_CALIBRATED:
    case STATE_GUIDING:
    case STATE_STOP:
        return false;
    }

    return false;
}

/*************  A new image is ready ************************/

void Guider::UpdateGuideState(usImage *pImage, bool bStopping)
{
    wxString statusMessage;
    bool someException = false;

    try
    {
        Debug.Write(wxString::Format("UpdateGuideState(): m_state=%d\n", m_state));

        if (pImage)
        {
            // switch in the new image

            usImage *pPrevImage = m_pCurrentImage;
            m_pCurrentImage = pImage;

            ImageLogger::SaveImage(pPrevImage);
        }
        else
        {
            pImage = m_pCurrentImage;
        }

        if (bStopping)
        {
            StopGuiding();
            statusMessage = _("Stopped Guiding");
            throw THROW_INFO("Stopped Guiding");
        }

        assert(!pMount || !pMount->IsBusy());

        // shift lock position
        if (LockPosShiftEnabled() && IsGuiding())
        {
            if (ShiftLockPosition())
            {
                EvtServer.NotifyLockShiftLimitReached();
                pFrame->Alert(_("Shifted lock position outside allowable area. Lock Position Shift disabled."));
                EnableLockPosShift(false);
            }
        }

        GuiderOffset ofs;
        FrameDroppedInfo info;
        GuiderStateHost host(*this);
        host.image = pImage;
        host.info = &info;
        host.ofs = &ofs;

        if (UpdateCurrentPosition(pImage, &ofs, &info)) // true means error
        {
            info.frameNumber = pImage->FrameNum;
            info.time = pFrame->TimeSinceGuidingStarted();
            info.avgDist = pFrame->CurrentGuideError();

            guider_state_machine::OnStarLost(host);

            statusMessage = info.status;
            throw THROW_INFO("unable to update current position");
        }

        statusMessage = info.status;

        if (IsLoopingState(m_state))
            EvtServer.NotifyLooping(pImage->FrameNum, &PrimaryStar(), nullptr);

        // we have a star selected, so re-enable subframes
        if (m_forceFullFrame)
        {
            Debug.Write("setting force full frames = false\n");
            m_forceFullFrame = false;
        }

        if (IsPaused())
        {
            guider_state_machine::OnPausedFrame(host);
            statusMessage = _("Paused") + (GetPauseType() == PAUSE_FULL ? _("/full") : _("/looping"));
            throw THROW_INFO("Skipping frame - guider is paused");
        }

        switch (guider_state_machine::OnStarFound(host))
        {
        case guider_state_machine::FrameResult::Done:
            break;
        case guider_state_machine::FrameResult::PrimaryCalibrationFailed:
            statusMessage = pMount->IsStepGuider() ? _("AO calibration failed") : _("calibration failed");
            throw ERROR_INFO("Calibration failed");
        case guider_state_machine::FrameResult::SecondaryCalibrationFailed:
            statusMessage = _("calibration failed");
            throw ERROR_INFO("Calibration failed");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        someException = true;
    }

    // during calibration, the mount is responsible for updating the status message
    if (someException && m_state != STATE_CALIBRATING_PRIMARY && m_state != STATE_CALIBRATING_SECONDARY)
    {
        pFrame->StatusMsg(statusMessage);
    }

    if (m_measurementMode && m_state != STATE_GUIDING)
    {
        GuidingAssistant::NotifyBacklashError();
        m_measurementMode = false;
    }

    pFrame->UpdateButtonsStatus();

    UpdateImageDisplay(pImage);

    Debug.AddLine("UpdateGuideState exits: " + statusMessage);
}

bool Guider::ShiftLockPosition()
{
    m_lockPosition.UpdateShift();
    bool isValid = IsValidLockPosition(m_lockPosition);
    Debug.Write(wxString::Format("ShiftLockPos: new pos = %.2f, %.2f valid=%d\n", m_lockPosition.X, m_lockPosition.Y, isValid));
    return !isValid;
}

void Guider::SetLockPosShiftRate(const PHD_Point& rate, GRAPH_UNITS units, bool isMountCoords)
{
    Debug.Write(wxString::Format("SetLockPosShiftRate: rate = %.2f,%.2f units = %d isMountCoords = %d\n", rate.X, rate.Y, units,
                                 isMountCoords));

    m_lockPosShift.shiftRate = rate;
    m_lockPosShift.shiftUnits = units;
    m_lockPosShift.shiftIsMountCoords = isMountCoords;

    if (m_state == STATE_CALIBRATED || m_state == STATE_GUIDING)
    {
        UpdateLockPosShiftCameraCoords();
        if (LockPosShiftEnabled())
        {
            GuideLog.NotifyLockShiftParams(m_lockPosShift, m_lockPosition.ShiftRate());
        }
    }
}

void Guider::EnableLockPosShift(bool enable)
{
    if (enable != m_lockPosShift.shiftEnabled)
    {
        Debug.Write(wxString::Format("EnableLockPosShift: enable = %d\n", enable));
        m_lockPosShift.shiftEnabled = enable;
        if (enable)
        {
            m_userMultiStarMode = GetMultiStarMode();
            SetMultiStarMode(false);
            Debug.Write("Multi-star mode disabled for comet tracking\n");
            m_lockPosition.BeginShift();
        }
        else
        {
            SetMultiStarMode(m_userMultiStarMode);
            Debug.Write("Comet tracking disabled, user's multi-star mode restored\n");
        }
        if (m_state == STATE_CALIBRATED || m_state == STATE_GUIDING)
        {
            GuideLog.NotifyLockShiftParams(m_lockPosShift, m_lockPosition.ShiftRate());
        }
    }
}

void Guider::UpdateLockPosShiftCameraCoords()
{
    if (!m_lockPosShift.shiftRate.IsValid())
    {
        Debug.Write("UpdateLockPosShiftCameraCoords: no shift rate set\n");
        m_lockPosition.DisableShift();
        return;
    }

    PHD_Point rate(0., 0.);

    // convert shift rate to camera coordinates
    if (m_lockPosShift.shiftIsMountCoords)
    {
        PHD_Point radec_rates = m_lockPosShift.shiftRate;

        Debug.Write(wxString::Format("UpdateLockPosShiftCameraCoords: shift rate mount coords = %.2f,%.2f\n", radec_rates.X,
                                     radec_rates.Y));

        Mount *scope = TheScope();
        if (scope)
        {
            if (m_lockPosShift.shiftUnits == UNIT_ARCSEC)
            {
                // if rates are RA/Dec arc-seconds, assume they are ephemeris rates

                // account for parity if known
                GuideParity raParity = scope->RAParity();
                GuideParity decParity = scope->DecParity();
                if (raParity != GUIDE_PARITY_UNKNOWN || decParity != GUIDE_PARITY_UNKNOWN)
                {
                    if (raParity == GUIDE_PARITY_ODD)
                        radec_rates.X = -radec_rates.X;
                    if (decParity == GUIDE_PARITY_ODD)
                        radec_rates.Y = -radec_rates.Y;

                    Debug.Write(wxString::Format("UpdateLockPosShiftCameraCoords: after parity adjustment: %.2f,%.2f\n",
                                                 radec_rates.X, radec_rates.Y));
                }

                // account for scope declination
                if (pPointingSource)
                {
                    double dec = pPointingSource->GetDeclinationRadians();
                    if (dec != UNKNOWN_DECLINATION)
                    {
                        radec_rates.X *= cos(dec);
                        Debug.Write(wxString::Format(
                            "UpdateLockPosShiftCameraCoords: RA shift rate adjusted for declination %.1f\n", degrees(dec)));
                    }
                }
            }

            scope->TransformMountCoordinatesToCameraCoordinates(radec_rates, rate);
        }
    }
    else
    {
        rate = m_lockPosShift.shiftRate;
    }

    Debug.Write(wxString::Format("UpdateLockPosShiftCameraCoords: shift rate camera coords = %.2f,%.2f %s/hr\n", rate.X, rate.Y,
                                 m_lockPosShift.shiftUnits == UNIT_ARCSEC ? "arcsec" : "pixels"));

    // convert arc-seconds to pixels
    if (m_lockPosShift.shiftUnits == UNIT_ARCSEC)
    {
        rate /= pFrame->GetCameraPixelScale();
    }
    rate /= 3600.0; // per hour => per second

    Debug.Write(wxString::Format("UpdateLockPosShiftCameraCoords: shift rate %.2g,%.2g px/sec\n", rate.X, rate.Y));

    m_lockPosition.SetShiftRate(rate.X, rate.Y);
}

wxString Guider::GetSettingsSummary() const
{
    // return a loggable summary of current global configs managed by MyFrame
    return wxEmptyString;
}

EXPOSED_STATE Guider::GetExposedState()
{
    EXPOSED_STATE rval;
    Guider *guider = pFrame->pGuider;

    if (!guider)
        rval = EXPOSED_STATE_NONE;

    else if (guider->IsPaused())
        rval = EXPOSED_STATE_PAUSED;

    else if (!pFrame->CaptureActive)
        rval = EXPOSED_STATE_NONE;

    else
    {
        // map the guider internal state into a server reported state

        switch (guider->GetState())
        {
        case STATE_UNINITIALIZED:
        case STATE_STOP:
        default:
            rval = EXPOSED_STATE_NONE;
            break;

        case STATE_SELECTING:
            // only report "looping" if no star is selected
            if (guider->CurrentPosition().IsValid())
                rval = EXPOSED_STATE_SELECTED;
            else
                rval = EXPOSED_STATE_LOOPING;
            break;

        case STATE_SELECTED:
        case STATE_CALIBRATED:
            rval = EXPOSED_STATE_SELECTED;
            break;

        case STATE_CALIBRATING_PRIMARY:
        case STATE_CALIBRATING_SECONDARY:
            rval = EXPOSED_STATE_CALIBRATING;
            break;

        case STATE_GUIDING:
            if (guider->IsLocked())
                rval = EXPOSED_STATE_GUIDING_LOCKED;
            else
                rval = EXPOSED_STATE_GUIDING_LOST;
        }

        Debug.Write(wxString::Format("case statement mapped state %d to %d\n", guider->GetState(), rval));
    }

    return rval;
}

void Guider::SetMinStarHFD(double val)
{
    Debug.Write(wxString::Format("Setting StarMinHFD = %.2f\n", val));
    pConfig->Profile.SetDouble("/guider/StarMinHFD", val);
    m_minStarHFD = val;
}

// Minimum star SNR for auto-find; minimum SNR for normal star tracking is hard-wired to 3
void Guider::SetAFMinStarSNR(double val)
{
    Debug.Write(wxString::Format("Setting StarMinSNR = %0.1f\n", val));
    pConfig->Profile.SetDouble("/guider/StarMinSNR", val);
    m_minAFStarSNR = val;
}

// Maximum star HFD
void Guider::SetMaxStarHFD(double val)
{
    Debug.Write(wxString::Format("Setting MaxHFD = %0.1f\n", val));
    pConfig->Profile.SetDouble("/guider/StarMaxHFD", val);
    m_maxStarHFD = val;
}

void Guider::SetAutoSelDownsample(unsigned int val)
{
    Debug.Write(wxString::Format("Setting AutoSelDownsample = %u\n", val));
    pConfig->Profile.SetInt("/guider/AutoSelDownsample", val);
    m_autoSelDownsample = val;
}
