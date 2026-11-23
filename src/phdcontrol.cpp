/*
 *  phdcontrol.cpp
 *  PHD Guiding
 *
 *  Created by Andy Galasso
 *  Copyright (c) 2013 Andy Galasso
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

#include "controller_host.h"
#include "settle_tracker.h"

enum State
{
    STATE_IDLE = 0,
    STATE_SETUP,
    STATE_ATTEMPT_START,
    STATE_SELECT_STAR,
    STATE_WAIT_SELECTED,
    STATE_CALIBRATE,
    STATE_CALIBRATION_WAIT,
    STATE_GUIDE,
    STATE_SETTLE_BEGIN,
    STATE_SETTLE_WAIT,
    STATE_FINISH,
};

enum SettleOp
{
    OP_DITHER,
    OP_GUIDE,
};

enum
{
    SETTLING_TIME_DISABLED = 9999
};

struct ControllerState
{
    State state;
    bool forceCalibration;
    bool haveSaveSticky;
    bool saveSticky;
    int autoFindAttemptsRemaining;
    int waitSelectedRemaining;
    bool useStickyLock;
    SettleOp settleOp;
    SettleParams settle;
    wxRect roi;
    SettleTracker settleTracker; // the settle timeout, the in-range timer and the frame counts
    ControllerDecMode saveDecGuideMode;
    bool overrideDecGuideMode;
    bool succeeded;
    wxString errorMsg;
};

static ControllerState ctrl;

// the frame, guider, mounts, camera, event server and guide log the controller works through (controller_host.h)
static ControllerHost *s_host;

static ControllerHost& host()
{
    return *s_host;
}

void PhdController::OnAppInit(ControllerHost& h)
{
    s_host = &h;
    // a new controller, idle, with its settle timed on the host's clock (settle_tracker.cpp)
    ctrl = ControllerState();
    ctrl.settleTracker = SettleTracker(h.SettleClock());
}

void PhdController::OnAppExit()
{
    // nothing to free: the settle timers are in ctrl.settleTracker (settle_tracker.cpp)
}

bool PhdController::IsSettling()
{
    return ctrl.state == STATE_SETTLE_BEGIN || ctrl.state == STATE_SETTLE_WAIT;
}

// Idle means controller is not in transitional states of starting/calibrating/stopping/settling
bool PhdController::IsIdle()
{
    return ctrl.state == STATE_IDLE;
}

// A guide request is waiting for a calibration to complete. The wait has no failure check: when the
// calibration fails the request stays here until something aborts it.
bool PhdController::IsWaitingForCalibration()
{
    return ctrl.state == STATE_CALIBRATION_WAIT;
}

#define SETSTATE(newstate)                                                                                                     \
    do                                                                                                                         \
    {                                                                                                                          \
        Debug.AddLine("PhdController: newstate " #newstate);                                                                   \
        ctrl.state = newstate;                                                                                                 \
    } while (false)

static wxString ReentrancyError(const char *op)
{
    return wxString::Format("Cannot initiate %s while %s is in progress", op, ctrl.settleOp == OP_DITHER ? "dither" : "guide");
}

bool PhdController::Guide(unsigned int options, const SettleParams& settle, const wxRect& roi, wxString *error)
{
    if (ctrl.state != STATE_IDLE)
    {
        Debug.Write(wxString::Format("PhdController::Guide reentrancy state = %d op = %d\n", ctrl.state, ctrl.settleOp));
        *error = ReentrancyError("guide");
        return false;
    }

    Debug.AddLine("PhdController::Guide begins");
    ctrl.forceCalibration = (options & GUIDEOPT_FORCE_RECAL) != 0;
    ctrl.useStickyLock = (options & GUIDEOPT_USE_STICKY_LOCK) != 0;
    ctrl.settleOp = OP_GUIDE;
    ctrl.settle = settle;
    ctrl.roi = roi;
    SETSTATE(STATE_SETUP);
    UpdateControllerState();
    return true;
}

// Put back the sticky-lock setting that STATE_CALIBRATE forced on for the calibration. Once only:
// every way out of the calibration (success, a failed start, an abort) comes through here.
static void restore_sticky_lock(void)
{
    if (ctrl.useStickyLock && ctrl.haveSaveSticky)
    {
        host().SetLockPosIsSticky(ctrl.saveSticky);
        ctrl.haveSaveSticky = false;
    }
}

static void do_fail(const wxString& msg)
{
    Debug.AddLine(wxString::Format("PhdController failed: %s", msg));
    restore_sticky_lock();
    ctrl.succeeded = false;
    ctrl.errorMsg = msg;
    SETSTATE(STATE_FINISH);
}

bool PhdController::Dither(double pixels, bool forceRaOnly, const SettleParams& settle, wxString *errMsg)

{
    if (ctrl.state != STATE_IDLE)
    {
        Debug.Write(wxString::Format("PhdController::Dither reentrancy state = %d op = %d\n", ctrl.state, ctrl.settleOp));
        *errMsg = ReentrancyError("dither");
        return false;
    }

    Debug.AddLine("PhdController::Dither begins");

    bool raOnly = host().DitherRaOnlySetting();
    if (forceRaOnly)
    {
        // event server client wants RA only
        raOnly = true;
    }

    bool overrideDecGuideMode = false;
    ControllerDecMode dgm = ControllerDecMode::None;

    if (host().HasMount(ControllerMount::Primary) && !host().PrimaryMountIsStepGuider() && !raOnly)
    {
        // Check to see if we need to force raOnly or temporarily adjust dec guide mode
        dgm = host().PrimaryScopeDecGuideMode();

        if (dgm != ControllerDecMode::Auto)
        {
            if ((dgm == ControllerDecMode::North || dgm == ControllerDecMode::South) &&
                settle.settleTimeSec != SETTLING_TIME_DISABLED)
            {
                // Temporarily override dec guide mode because user has not set ra-only
                overrideDecGuideMode = true;
            }
            else
            {
                // DEC_NONE or no settling parameters

                Debug.Write(wxString::Format("PhdController: forcing dither RA-only since Dec guide mode is %s\n",
                                             host().DecGuideModeStr(ctrl.saveDecGuideMode)));

                raOnly = true;
            }
        }
    }

    bool error = host().Dither(pixels, raOnly);
    if (error)
    {
        Debug.AddLine("PhdController::Dither pFrame->Dither failed");
        *errMsg = _T("Dither error");
        return false;
    }

    ctrl.settleOp = OP_DITHER;
    ctrl.settle = settle;
    ctrl.overrideDecGuideMode = overrideDecGuideMode;
    ctrl.saveDecGuideMode = dgm;
    SETSTATE(STATE_SETTLE_BEGIN);
    UpdateControllerState();

    return true;
}

bool PhdController::Dither(double pixels, int settleFrames, wxString *errMsg)
{
    SettleParams settle;

    settle.tolerancePx = 99.;
    settle.settleTimeSec = SETTLING_TIME_DISABLED;
    settle.timeoutSec = SETTLING_TIME_DISABLED;
    settle.frames = settleFrames;

    return Dither(pixels, false, settle, errMsg);
}

bool PhdController::DitherCompat(double pixels, wxString *errMsg)
{
    AbortController("manual or phd1-style dither");

    enum
    {
        SETTLE_FRAMES = 10
    };

    return Dither(pixels, SETTLE_FRAMES, errMsg);
}

void PhdController::AbortController(const wxString& reason)
{
    if (ctrl.state != STATE_IDLE)
    {
        do_fail(reason);
        UpdateControllerState();
    }
}

static bool all_gear_connected(void)
{
    return host().CameraConnected() &&
        (!host().HasMount(ControllerMount::Primary) || host().MountConnected(ControllerMount::Primary)) &&
        (!host().HasMount(ControllerMount::Secondary) || host().MountConnected(ControllerMount::Secondary));
}

// every mount there is calibrated (none counts as calibrated)
static bool all_mounts_calibrated(void)
{
    return (!host().HasMount(ControllerMount::Primary) || host().MountCalibrated(ControllerMount::Primary)) &&
        (!host().HasMount(ControllerMount::Secondary) || host().MountCalibrated(ControllerMount::Secondary));
}

static void do_notify(void)
{
    if (ctrl.succeeded)
    {
        Debug.AddLine("PhdController complete: success");
        host().NotifySettleDone(wxEmptyString, ctrl.settleTracker.FrameCount(), ctrl.settleTracker.DroppedFrameCount());
        host().LogSettlingStateChange("Settling complete");
    }
    else
    {
        Debug.AddLine(wxString::Format("PhdController complete: fail: %s", ctrl.errorMsg));
        host().NotifySettleDone(ctrl.errorMsg, ctrl.settleTracker.FrameCount(), ctrl.settleTracker.DroppedFrameCount());
        host().LogSettlingStateChange("Settling failed");
    }

    host().NotifyMountSettleDone(ctrl.succeeded); // with a mount
}

static bool start_capturing(void)
{
    if (!host().CameraConnected())
    {
        return false;
    }

    host().ResetGuider(true); // invalidate current position, etc.
    host().ForceFullFrame(); // we need a full frame to auto-select a star
    host().ResetAutoExposure();
    host().StartCapturing();

    return true;
}

static bool start_guiding(void)
{
    bool error = host().StartGuiding();
    return !error;
}

static bool IsAoBumpInProgress()
{
    return host().AoBumpInProgress();
}

bool PhdController::CanGuide(wxString *error)
{
    if (!all_gear_connected())
    {
        *error = _T("all equipment must be connected first");
        return false;
    }
    return true;
}

void PhdController::UpdateControllerState(bool frameDropped)
{
    bool done = false;

    while (!done)
    {
        switch (ctrl.state)
        {
        case STATE_IDLE:
            done = true;
            break;

        case STATE_SETUP:
            Debug.AddLine("PhdController: setup");
            ctrl.haveSaveSticky = false;
            // a request that ends before it settles (an abort while it waits for a calibration) reports 0 frames,
            // not the counts of the settle before it
            ctrl.settleTracker.ResetCounts();
            ctrl.autoFindAttemptsRemaining = 3;
            ctrl.overrideDecGuideMode = false; // guide stop/start with no dithering
            SETSTATE(STATE_ATTEMPT_START);
            break;

        case STATE_ATTEMPT_START:
        {

            wxString err;

            if (!CanGuide(&err))
            {
                Debug.Write(wxString::Format("PhdController: not ready: %s\n", err));
                do_fail(err);
            }
            else if (host().GuiderIsCalibratingOrGuiding())
            {
                if (ctrl.forceCalibration)
                {
                    SETSTATE(STATE_CALIBRATE);
                }
                else
                {
                    GUIDER_STATE state = host().GuiderState();
                    Debug.Write(wxString::Format("PhdController: guider state = %d\n", state));
                    if (state == STATE_CALIBRATED || state == STATE_GUIDING)
                    {
                        SETSTATE(STATE_SETTLE_BEGIN);
                    }
                    else
                    {
                        SETSTATE(STATE_CALIBRATION_WAIT);
                        done = true;
                    }
                }
            }
            else if (!host().CaptureActive())
            {
                Debug.AddLine("PhdController: start capturing");
                if (!start_capturing())
                {
                    do_fail(_T("unable to start capturing"));
                    break;
                }
                SETSTATE(STATE_SELECT_STAR);
                done = true;
            }
            else if (host().GuiderState() == STATE_SELECTED)
            {
                if (ctrl.forceCalibration && !host().SelectedStarClearForRecalibration())
                {
                    // #140: the star (find_star while looping on a calibrated mount) was picked with no margin for
                    // the calibration walk; select again on the next full frame, keeping the margin
                    Debug.AddLine("PhdController: selected star too close to the edge for the recalibration, reselect");
                    host().ResetGuider(true);
                    host().ForceFullFrame();
                    SETSTATE(STATE_SELECT_STAR);
                    done = true;
                }
                else
                {
                    SETSTATE(STATE_CALIBRATE);
                }
            }
            else
            {
                // capture is active, no star selected
                SETSTATE(STATE_SELECT_STAR);

                // if auto-exposure is enabled, reset to max exposure duration
                // and wait for the next camera frame
                if (host().AutoExposureEnabled())
                {
                    host().ResetAutoExposure();
                    done = true;
                }
            }
            break;
        }

        case STATE_SELECT_STAR:
        {
            // STATE_CALIBRATE clears the calibration of a forced recalibration only after this selection, so
            // tell the auto-select to keep the star far enough from the edge for the calibration walk (#140)
            bool error = host().AutoSelectStar(ctrl.roi, ctrl.forceCalibration);
            if (error)
            {
                Debug.Write(
                    wxString::Format("auto find star failed, attempts remaining = %d\n", ctrl.autoFindAttemptsRemaining));
                if (--ctrl.autoFindAttemptsRemaining == 0)
                {
                    do_fail(_T("failed to find a suitable guide star"));
                }
                else
                {
                    host().ResetGuider(true);
                    SETSTATE(STATE_ATTEMPT_START);
                    done = true;
                }
            }
            else
            {
                SETSTATE(STATE_WAIT_SELECTED);
                ctrl.waitSelectedRemaining = 3;
                done = true;
            }
            break;
        }

        case STATE_WAIT_SELECTED:
            if (host().GuiderState() == STATE_SELECTED)
            {
                SETSTATE(STATE_CALIBRATE);
            }
            else
            {
                Debug.Write(
                    wxString::Format("waiting for star selected, attempts remaining = %d\n", ctrl.waitSelectedRemaining));
                if (--ctrl.waitSelectedRemaining == 0)
                {
                    SETSTATE(STATE_ATTEMPT_START);
                }
                done = true;
            }
            break;

        case STATE_CALIBRATE:
            if (ctrl.forceCalibration)
            {
                Debug.AddLine("PhdController: clearing calibration");
                if (host().HasMount(ControllerMount::Primary))
                    host().ClearMountCalibration(ControllerMount::Primary);
                if (host().HasMount(ControllerMount::Secondary))
                    host().ClearMountCalibration(ControllerMount::Secondary);
            }

            if (!all_mounts_calibrated())
            {
                Debug.AddLine("PhdController: start calibration");

                if (ctrl.useStickyLock)
                {
                    ctrl.saveSticky = host().LockPosIsSticky();
                    ctrl.haveSaveSticky = true;
                    host().SetLockPosIsSticky(true);
                }

                if (!start_guiding())
                {
                    do_fail(_T("could not start calibration")); // puts the sticky-lock setting back
                    break;
                }

                SETSTATE(STATE_CALIBRATION_WAIT);
                done = true;
            }
            else
            {
                SETSTATE(STATE_GUIDE);
            }
            break;

        case STATE_CALIBRATION_WAIT:
            if (all_mounts_calibrated())
            {
                restore_sticky_lock();

                SETSTATE(STATE_SETTLE_BEGIN);
            }
            else
                done = true;
            break;

        case STATE_GUIDE:
            if (!start_guiding())
            {
                do_fail(_T("could not start guiding"));
                break;
            }
            SETSTATE(STATE_SETTLE_BEGIN);
            done = true;
            break;

        case STATE_SETTLE_BEGIN:
            if (ctrl.settleOp == OP_GUIDE && host().GuiderIsCalibrating())
            {
                // entering guiding cleared the calibration (an image scale change) and started a new one (#255)
                Debug.AddLine("PhdController: guider calibrating, wait for the calibration before settling");
                if (ctrl.useStickyLock && !ctrl.haveSaveSticky)
                {
                    // as STATE_CALIBRATE does: guide on the selected star's lock position after the calibration
                    ctrl.saveSticky = host().LockPosIsSticky();
                    ctrl.haveSaveSticky = true;
                    host().SetLockPosIsSticky(true);
                }
                SETSTATE(STATE_CALIBRATION_WAIT);
                break;
            }
            host().NotifySettleBegin();
            host().LogSettlingStateChange("Settling started");
            if (ctrl.overrideDecGuideMode)
            {
                Debug.Write(wxString::Format("PhdController: setting Dec guide mode to %s for dither settle\n",
                                             host().DecGuideModeStr(ControllerDecMode::Auto)));
                host().SetScopeDecGuideMode(ControllerDecMode::Auto);
            }
            ctrl.settleTracker.Begin();
            SETSTATE(STATE_SETTLE_WAIT);
            done = true;
            break;

        case STATE_SETTLE_WAIT:
        {
            if (ctrl.settleOp == OP_GUIDE && host().GuiderIsCalibrating())
            {
                // a dropped frame began the settle before the guider entered guiding and started a calibration (#255)
                SETSTATE(STATE_SETTLE_BEGIN);
                break;
            }
            // the decision and its timers are in the wx-free settle_frame.cpp and settle_tracker.cpp; #136: a dropped frame
            // (a transient capture failure while guiding) counts like a lost-star frame and neither ends nor completes an
            // in-range run
            bool locked = host().GuiderIsLocked();
            double error = host().CurrentGuideError();
            bool aoBump = IsAoBumpInProgress();
            SettleFrame::Result result = ctrl.settleTracker.OnFrame(ctrl.settle, frameDropped, locked, error, aoBump);

            Debug.Write(
                wxString::Format("PhdController: settling, locked = %d, distance = %.2f (%.2f) aobump = %d frame = %d / %d\n",
                                 result.lockedOnStar, error, ctrl.settle.tolerancePx, aoBump, ctrl.settleTracker.FrameCount(),
                                 ctrl.settle.frames));

            if (result.outcome == SettleFrame::Outcome::Settled)
            {
                ctrl.succeeded = true;
                SETSTATE(STATE_FINISH);
                break;
            }
            if (result.outcome == SettleFrame::Outcome::TimedOut)
            {
                do_fail(_T("timed-out waiting for guider to settle"));
                break;
            }
            host().NotifySettling(error, (double) result.timeInRangeMs / 1000., ctrl.settle.settleTimeSec, result.lockedOnStar);
            done = true;
            break;
        }

        case STATE_FINISH:
            if (ctrl.overrideDecGuideMode)
            {
                Debug.Write(wxString::Format("PhdController: restore Dec guide mode to %s after dither\n",
                                             host().DecGuideModeStr(ctrl.saveDecGuideMode)));
                host().SetScopeDecGuideMode(ctrl.saveDecGuideMode);
                ctrl.overrideDecGuideMode = false;
            }
            do_notify();
            SETSTATE(STATE_IDLE);
            done = true;
            break;
        }
    }
}
