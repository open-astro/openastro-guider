// FROZEN COPY, DO NOT EDIT: the legacy twin of the guider's state machine (#142, slice T2c).
//
// Source: src/guider.cpp at origin/main 579829c3 (blob 31564817), with the #255 return to
// calibrating. Line ranges copied, CRLF made LF, nothing else changed:
//   136-159    StateStr
//   816-913    edgeDist, Guider::MoveLockPosition
//   954-1104   Guider::SetState
//   1130-1162  Guider::StopGuiding
//   1325-1368  UpdateGuideState: the star not found, per state   (in LegacyStarLost)
//   1388-1394  UpdateGuideState: the frame while paused           (in LegacyPaused)
//   1399-1524  UpdateGuideState: the star found, per state        (in LegacyStarFound)
//
// Check it against the source with
//   bash tests/guiding/legacy/check_twin.sh
// which diffs these ranges of src/guider.cpp at 579829c3 against the copies below and must print
// nothing. Everything the copies call outside these lines is a mechanical stand-in in
// guider_sm_legacy.h; the three UpdateGuideState parts are wrapped in functions here, outside the
// copies, with the locals they use.

#include "guider_sm_legacy.h"

#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"

#undef THROW_INFO
#undef ERROR_INFO
#define THROW_INFO(s) (Debug.AddLine(wxString("T:" s)))
#define ERROR_INFO(s) (Debug.AddLine(wxString("E:" s)))
#define _(s) wxString(s)
#define wxS(s) s

namespace legacy_sm
{

std::vector<std::string> Twin::trace;
TwinDebug Debug;
static Mount s_primary;
Mount *pMount = &s_primary;
Mount *pSecondaryMount = nullptr;
static TwinFrame s_frame;
TwinFrame *pFrame = &s_frame;
TwinEvtServer EvtServer;
TwinGuideLog GuideLog;
static DeflectionLogger s_deflectionLogger;

// BEGIN COPY
static const wxStringCharType *StateStr(GUIDER_STATE st)
{
    switch (st)
    {
    case STATE_UNINITIALIZED:
        return wxS("UNINITIALIZED");
    case STATE_SELECTING:
        return wxS("SELECTING");
    case STATE_SELECTED:
        return wxS("SELECTED");
    case STATE_CALIBRATING_PRIMARY:
        return wxS("CALIBRATING_PRIMARY");
    case STATE_CALIBRATING_SECONDARY:
        return wxS("CALIBRATING_SECONDARY");
    case STATE_CALIBRATED:
        return wxS("CALIBRATED");
    case STATE_GUIDING:
        return wxS("GUIDING");
    case STATE_STOP:
        return wxS("STOP");
    default:
        return wxS("??");
    }
}
// END COPY

// BEGIN COPY
// distance to nearest edge
static double edgeDist(const PHD_Point& pt, const wxSize& size)
{
    return wxMin(pt.X, wxMin(size.GetWidth() - pt.X, wxMin(pt.Y, size.GetHeight() - pt.Y)));
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

        // This loop is to handle dithers when the star is near the edge of the frame. The strategy
        // is to try reflecting the requested dither in 4 directions along the RA/Dec axes; if any
        // of the projections results in a valid lock position, use it. Otherwise, choose the
        // direction that moves farthest from the edge of the frame.

        PHD_Point cameraDelta, mountDelta;
        double dbest;

        for (int q = 0; q < 4; q++)
        {
            int sx = 1 - ((q & 1) << 1);
            int sy = 1 - (q & 2);

            PHD_Point tmpMount(mountDeltaArg.X * sx, mountDeltaArg.Y * sy);
            PHD_Point tmpCamera;

            if (pMount->TransformMountCoordinatesToCameraCoordinates(tmpMount, tmpCamera))
            {
                throw ERROR_INFO("Transform failed");
            }

            PHD_Point tmpLockPosition = m_lockPosition + tmpCamera;

            if (IsValidLockPosition(tmpLockPosition))
            {
                cameraDelta = tmpCamera;
                mountDelta = tmpMount;
                break;
            }

            Debug.Write("dither produces an invalid lock position, try a variation\n");

            double d = edgeDist(tmpLockPosition, image->Size);
            if (q == 0 || d > dbest)
            {
                cameraDelta = tmpCamera;
                mountDelta = tmpMount;
                dbest = d;
            }
        }

        PHD_Point newLockPosition = m_lockPosition + cameraDelta;
        if (SetLockPosition(newLockPosition))
        {
            throw ERROR_INFO("SetLockPosition failed");
        }

        // update average distance right away so GetCurrentDistance reflects the increased distance from the dither
        double dist = cameraDelta.Distance(), distRA = fabs(mountDelta.X);
        m_distance.OnDither(dist, distRA);

        // Zero-length dithers result in div-by-zero in calculation of 'f'.  We can still allow zero-length dithers as a way of
        // triggering a settling period if we temporarily ignore the 'fast re-center' option
        if (IsFastRecenterEnabled() && dist != 0)
        {
            m_ditherRecenterRemaining.SetXY(fabs(mountDelta.X), fabs(mountDelta.Y));
            m_ditherRecenterStepsPending = 0; // a step of an earlier recenter no longer counts
            m_ditherRecenterStalledSteps = 0;
            m_recenteringForWorker = true;
            m_ditherRecenterDir.x = mountDelta.X < 0.0 ? 1 : -1;
            m_ditherRecenterDir.y = mountDelta.Y < 0.0 ? 1 : -1;
            // make each step a bit less than the full search region distance to avoid losing the star
            double f = ((double) GetMaxMovePixels() * 0.7) / m_ditherRecenterRemaining.Distance();
            m_ditherRecenterStep.SetXY(f * m_ditherRecenterRemaining.X, f * m_ditherRecenterRemaining.Y);
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        err = true;
    }

    return err;
}
// END COPY

// BEGIN COPY
void Guider::SetState(GUIDER_STATE newState)
{
    try
    {
        Debug.Write(wxString::Format("Changing from state %s to %s\n", StateStr(m_state), StateStr(newState)));

        if (newState == STATE_STOP)
        {
            // we are going to stop looping exposures.  We should put
            // ourselves into a good state to restart looping later
            switch (m_state)
            {
            case STATE_UNINITIALIZED:
            case STATE_SELECTING:
            case STATE_SELECTED:
                newState = m_state;
                break;
            case STATE_CALIBRATING_PRIMARY:
            case STATE_CALIBRATING_SECONDARY:
                // because we have done some moving here, we need to just
                // start over...
                newState = STATE_UNINITIALIZED;
                break;
            case STATE_CALIBRATED:
            case STATE_GUIDING:
                newState = STATE_SELECTED;
                break;
            case STATE_STOP:
                break;
            }
        }

        assert(newState != STATE_STOP);

        if (newState > m_state + 1)
        {
            Debug.Write(wxString::Format("Cannot transition from %s to newState=%s\n", StateStr(m_state), StateStr(newState)));
            throw ERROR_INFO("Illegal state transition");
        }

        GUIDER_STATE requestedState = newState;

        switch (requestedState)
        {
        case STATE_UNINITIALIZED:
            InvalidateLockPosition();
            InvalidateCurrentPosition();
            newState = STATE_SELECTING;
            break;
        case STATE_SELECTED:
            break;
        case STATE_CALIBRATING_PRIMARY:
            if (!pMount->IsCalibrated())
            {
                pMount->ResetErrorCount();
                if (pMount->BeginCalibration(CurrentPosition()))
                {
                    newState = STATE_UNINITIALIZED;
                    Debug.Write(ERROR_INFO("pMount->BeginCalibration failed"));
                }
                else
                {
                    GuideLog.StartCalibration(pMount);
                    EvtServer.NotifyStartCalibration(pMount);
                }
                break;
            }
            // fall through
        case STATE_CALIBRATING_SECONDARY:
            if (!pSecondaryMount || !pSecondaryMount->IsConnected())
            {
                newState = STATE_CALIBRATED;
            }
            else if (!pSecondaryMount->IsCalibrated())
            {
                pSecondaryMount->ResetErrorCount();
                if (pSecondaryMount->BeginCalibration(CurrentPosition()))
                {
                    newState = STATE_UNINITIALIZED;
                    Debug.Write(ERROR_INFO("pSecondaryMount->BeginCalibration failed"));
                }
                else
                {
                    GuideLog.StartCalibration(pSecondaryMount);
                    EvtServer.NotifyStartCalibration(pSecondaryMount);
                }
            }
            break;
        case STATE_GUIDING:
            assert(pMount);

            m_ditherRecenterRemaining.Invalidate(); // reset dither fast recenter state
            m_ditherRecenterStepsPending = 0;
            m_ditherRecenterStalledSteps = 0;
            m_recenteringForWorker = false;

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

            // #255: the adjustment clears the calibration when the image scale changed (HandleImageScaleChange), after
            // the calibrating states decided there was nothing to calibrate; calibrate now instead of guiding uncalibrated
            if (!pMount->IsCalibrated() ||
                (pSecondaryMount && pSecondaryMount->IsConnected() && !pSecondaryMount->IsCalibrated()))
            {
                Debug.Write("calibration cleared on guiding start, calibrating\n");
                newState = STATE_CALIBRATING_PRIMARY;
                break;
            }

            if (m_lockPosition.IsValid() && m_lockPosIsSticky)
            {
                Debug.Write("keeping sticky lock position\n");
            }
            else
            {
                SetLockPosition(CurrentPosition());
            }
            break;

        case STATE_SELECTING:
        case STATE_CALIBRATED:
        case STATE_STOP:
            break;
        }

        if (newState >= requestedState)
        {
            m_state = newState;
            Debug.Write(wxString::Format("guider state => %s\n", StateStr(m_state)));
        }
        else
        {
            Debug.Write(wxString::Format("SetState recurses newState %s\n", StateStr(newState)));
            SetState(newState);
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }
}
// END COPY

// BEGIN COPY
void Guider::StopGuiding()
{
    // first, send a notification that we stopped
    switch (m_state)
    {
    case STATE_UNINITIALIZED:
    case STATE_SELECTING:
    case STATE_SELECTED:
        break;
    case STATE_CALIBRATING_PRIMARY:
    case STATE_CALIBRATING_SECONDARY:
    case STATE_CALIBRATED:
        EvtServer.NotifyCalibrationFailed(m_state == STATE_CALIBRATING_SECONDARY ? pSecondaryMount : pMount,
                                          pFrame->CaptureFailureStopReason().empty() ? _("Calibration manually stopped")
                                                                                     : pFrame->CaptureFailureStopReason());
        // fall through to notify guiding stopped
    case STATE_GUIDING:
        if ((!pMount || !pMount->IsBusy()) && (!pSecondaryMount || !pSecondaryMount->IsBusy()))
        {
            // Notify guiding stopped if there are no outstanding guide steps.  The Guiding
            // Stopped notification must come after the final GuideStep notification otherwise
            // event server clients and the guide log will show the guide step happening after
            // guiding stopped.

            pFrame->NotifyGuidingStopped();
        }
        break;
    case STATE_STOP:
        break;
    }

    SetState(STATE_STOP);
}
// END COPY

void Guider::LegacyStarLost(usImage *pImage, FrameDroppedInfo& info)
{
// BEGIN COPY
            switch (m_state)
            {
            case STATE_UNINITIALIZED:
            case STATE_SELECTING:
                EvtServer.NotifyLooping(pImage->FrameNum, nullptr, &info);
                break;
            case STATE_SELECTED:
                // we had a current position and lost it
                EvtServer.NotifyLooping(pImage->FrameNum, nullptr, &info);
                SetState(STATE_UNINITIALIZED);
                EvtServer.NotifyStarLost(info);
                break;
            case STATE_CALIBRATING_PRIMARY:
            case STATE_CALIBRATING_SECONDARY:
                GuideLog.CalibrationFrameDropped(info);
                Debug.Write("Star lost during calibration... blundering on\n");
                EvtServer.NotifyStarLost(info);
                pFrame->StatusMsg(_("star lost"));
                break;
            case STATE_GUIDING:
            {
                GuideLog.FrameDropped(info);
                EvtServer.NotifyStarLost(info);
                GuidingAssistant::NotifyFrameDropped(info);
                pFrame->pGuideHistory->AppendData(info);

                // allow guide algorithms to attempt dead reckoning
                static GuiderOffset ZERO_OFS;
                pFrame->SchedulePrimaryMove(pMount, ZERO_OFS, MOVEOPTS_DEDUCED_MOVE);

                wxColor prevColor = GetBackgroundColour();
                SetBackgroundColour(wxColour(64, 0, 0));
                ClearBackground();
                if (pFrame->GetBeepForLostStar())
                    wxBell();
                wxMilliSleep(100);
                SetBackgroundColour(prevColor);
                break;
            }

            case STATE_CALIBRATED:
            case STATE_STOP:
                break;
            }
// END COPY
}

void Guider::LegacyPaused()
{
// BEGIN COPY
            if (m_state == STATE_GUIDING)
            {
                // allow guide algorithms to attempt dead reckoning
                static GuiderOffset ZERO_OFS;
                pFrame->SchedulePrimaryMove(pMount, ZERO_OFS, MOVEOPTS_DEDUCED_MOVE);
            }

// END COPY
}

wxString Guider::LegacyStarFound(GuiderOffset& ofs)
{
    wxString statusMessage;
    try
    {
// BEGIN COPY
        switch (m_state)
        {
        case STATE_SELECTING:
            assert(CurrentPosition().IsValid());
            SetLockPosition(CurrentPosition());
            Debug.Write("CurrentPosition() valid, moving to STATE_SELECTED\n");
            EvtServer.NotifyStarSelected(CurrentPosition());
            SetState(STATE_SELECTED);
            break;
        case STATE_SELECTED:
            break;
        case STATE_CALIBRATING_PRIMARY:
            if (!pMount->IsCalibrated())
            {
                if (pMount->UpdateCalibrationState(CurrentPosition()))
                {
                    SetState(STATE_UNINITIALIZED);
                    statusMessage = pMount->IsStepGuider() ? _("AO calibration failed") : _("calibration failed");
                    throw ERROR_INFO("Calibration failed");
                }

                if (!pMount->IsCalibrated())
                {
                    break;
                }
            }

            SetState(STATE_CALIBRATING_SECONDARY);

            if (m_state == STATE_CALIBRATING_SECONDARY)
            {
                // if we really have a secondary mount, and it isn't calibrated,
                // we need to take another exposure before falling into the code
                // below.  If we don't have one, or it is calibrated, we can fall
                // through.  If we don't fall through, we end up displaying a frame
                // which has the lockpoint in the wrong place, and while I thought I
                // could live with it when I originally wrote the code, it bothered
                // me so I did this.  Ick.
                break;
            }

            // Fall through
        case STATE_CALIBRATING_SECONDARY:
            if (pSecondaryMount && pSecondaryMount->IsConnected())
            {
                if (!pSecondaryMount->IsCalibrated())
                {
                    if (pSecondaryMount->UpdateCalibrationState(CurrentPosition()))
                    {
                        SetState(STATE_UNINITIALIZED);
                        statusMessage = _("calibration failed");
                        throw ERROR_INFO("Calibration failed");
                    }
                }

                if (!pSecondaryMount->IsCalibrated())
                {
                    break;
                }
            }
            assert(!pSecondaryMount || !pSecondaryMount->IsConnected() || pSecondaryMount->IsCalibrated());

            SetState(STATE_CALIBRATED);
            // fall through
        case STATE_CALIBRATED:
            assert(m_state == STATE_CALIBRATED);
            SetState(STATE_GUIDING);
            if (m_state != STATE_GUIDING)
                break; // entering guiding cleared the calibration and started a new one (#255)

            pFrame->NotifyGuidingStarted();

            // camera angle is known, so ok to calculate shift rate camera coords
            UpdateLockPosShiftCameraCoords();
            if (LockPosShiftEnabled())
                GuideLog.NotifyLockShiftParams(m_lockPosShift, m_lockPosition.ShiftRate());

            CheckCalibrationAutoLoad();
            break;
        case STATE_GUIDING:
            if (m_ditherRecenterRemaining.IsValid())
            {
                // fast recenter after dither taking large steps and bypassing
                // guide algorithms

                PHD_Point step(wxMin(m_ditherRecenterRemaining.X, m_ditherRecenterStep.X),
                               wxMin(m_ditherRecenterRemaining.Y, m_ditherRecenterStep.Y));

                Debug.Write(wxString::Format("dither recenter: remaining=(%.1f,%.1f) step=(%.1f,%.1f)\n",
                                             m_ditherRecenterRemaining.X * m_ditherRecenterDir.x,
                                             m_ditherRecenterRemaining.Y * m_ditherRecenterDir.y,
                                             step.X * m_ditherRecenterDir.x, step.Y * m_ditherRecenterDir.y));

                // The remaining distance goes down by what the mount moved of the step, when its move
                // completes (NotifyRecenterMoveComplete), not by the step: MoveAxis can cut or refuse it (#165)
                ofs.mountOfs.SetXY(step.X * m_ditherRecenterDir.x, step.Y * m_ditherRecenterDir.y);
                pMount->TransformMountCoordinatesToCameraCoordinates(ofs.mountOfs, ofs.cameraOfs);
                ++m_ditherRecenterStepsPending;
                // The next frame's exposure delay: still recentering unless this step, moved whole, ends it,
                // as when the step was counted on sending; set before the move and exposure are queued (#165)
                guide_move_plan::RecenterProgress ifWhole = guide_move_plan::AfterRecenterStep(
                    m_ditherRecenterRemaining.X, m_ditherRecenterRemaining.Y, step.X, step.Y, 0);
                m_recenteringForWorker = !ifWhole.done;
                pFrame->SchedulePrimaryMove(pMount, ofs, MOVEOPTS_RECOVERY_MOVE);
                // let guide algorithms know about the direct move
                pMount->NotifyDirectMove(ofs.mountOfs);
            }
            else if (m_measurementMode)
            {
                GuidingAssistant::NotifyBacklashStep(CurrentPosition());
            }
            else
            {
                // ordinary guide step
                s_deflectionLogger.Log(CurrentPosition());
                pFrame->SchedulePrimaryMove(pMount, ofs, MOVEOPTS_GUIDE_STEP);
            }
            // a recenter that NotifyRecenterMoveComplete ended is over for the next frame too (#165)
            if (!m_ditherRecenterRemaining.IsValid())
                m_recenteringForWorker = false;
            break;

        case STATE_UNINITIALIZED:
        case STATE_STOP:
            break;
        }
// END COPY
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }
    return statusMessage;
}

} // namespace legacy_sm
