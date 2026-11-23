/*
 *  scope.cpp
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

#include "backlash_comp.h"
#include "calibration_sanity.h"
#include "calstep_dialog.h"
#include "image_math.h"
#include "socket_server.h"

#include <wx/textfile.h>

static const int DefaultCalibrationDuration = 750;
static const int DefaultMaxDecDuration = 2500;
static const int DefaultMaxRaDuration = 2500;
enum
{
    MAX_DURATION_MIN = 50,
    MAX_DURATION_MAX = 8000,
};

static const DEC_GUIDE_MODE DefaultDecGuideMode = DEC_AUTO;
static const GUIDE_ALGORITHM DefaultRaGuideAlgorithm = GUIDE_ALGORITHM_HYSTERESIS;
static const GUIDE_ALGORITHM DefaultDecGuideAlgorithm = GUIDE_ALGORITHM_RESIST_SWITCH;
static const bool SANITY_CHECKING_ACTIVE = true; // Control calibration sanity checking

// The calibration run's enums carry the values of the app's (#142)
static_assert((int) calibration_run::Dir::North == NORTH && (int) calibration_run::Dir::South == SOUTH &&
              (int) calibration_run::Dir::East == EAST && (int) calibration_run::Dir::West == WEST);
static_assert((int) calibration_run::DecMode::None == DEC_NONE && (int) calibration_run::DecMode::Auto == DEC_AUTO &&
              (int) calibration_run::DecMode::North == DEC_NORTH && (int) calibration_run::DecMode::South == DEC_SOUTH);
static_assert((int) calibration_run::Parity::Odd == GUIDE_PARITY_ODD &&
              (int) calibration_run::Parity::Unknown == GUIDE_PARITY_UNKNOWN &&
              (int) calibration_run::Parity::Even == GUIDE_PARITY_EVEN);
static_assert(calibration_run::RATE_UNCALIBRATED == CALIBRATION_RATE_UNCALIBRATED);

// The sanity classifier's issues and constants are the app's (#142)
static_assert((int) calibration_sanity::Issue::None == CI_None && (int) calibration_sanity::Issue::Steps == CI_Steps &&
              (int) calibration_sanity::Issue::Angle == CI_Angle && (int) calibration_sanity::Issue::Rates == CI_Rates &&
              (int) calibration_sanity::Issue::Different == CI_Different);
static_assert(calibration_sanity::RATE_UNCALIBRATED == CALIBRATION_RATE_UNCALIBRATED &&
              calibration_sanity::UNKNOWN_DEC == UNKNOWN_DECLINATION);

// Scope::MoveAxis casts these into guide_move_plan
static_assert(static_cast<int>(guide_move_plan::Dir::None) == NONE && static_cast<int>(guide_move_plan::Dir::North) == NORTH &&
              static_cast<int>(guide_move_plan::Dir::South) == SOUTH && static_cast<int>(guide_move_plan::Dir::East) == EAST &&
              static_cast<int>(guide_move_plan::Dir::West) == WEST);
static_assert(static_cast<int>(guide_move_plan::DecMode::None) == DEC_NONE &&
              static_cast<int>(guide_move_plan::DecMode::Auto) == DEC_AUTO &&
              static_cast<int>(guide_move_plan::DecMode::North) == DEC_NORTH &&
              static_cast<int>(guide_move_plan::DecMode::South) == DEC_SOUTH);
static_assert(guide_move_plan::ALGO_RESULT == MOVEOPT_ALGO_RESULT && guide_move_plan::ALGO_DEDUCE == MOVEOPT_ALGO_DEDUCE &&
              guide_move_plan::USE_BLC == MOVEOPT_USE_BLC && guide_move_plan::GRAPH == MOVEOPT_GRAPH &&
              guide_move_plan::MANUAL == MOVEOPT_MANUAL);

// enable dec compensation when calibration declination is less than this
const double Scope::DEC_COMP_LIMIT = M_PI / 2.0 * 2.0 / 3.0; // 60 degrees
const double Scope::DEFAULT_MOUNT_GUIDE_SPEED = 0.5;

Scope::Scope()
    : m_maxDecDuration(0), m_maxRaDuration(0), m_decGuideMode(DEC_NONE), m_bogusGuideRatesFlagged(0),
      m_limitReachedDeferralTime(guide_move_plan::AlertClockS())
{
    m_CalDetailsValidated = false;

    wxString prefix = "/" + GetMountClassName();
    LoadProfileSetup();

    int maxRaDuration = pConfig->Profile.GetInt(prefix + "/MaxRaDuration", DefaultMaxRaDuration);
    SetMaxRaDuration(maxRaDuration);

    int maxDecDuration = pConfig->Profile.GetInt(prefix + "/MaxDecDuration", DefaultMaxDecDuration);
    SetMaxDecDuration(maxDecDuration);

    int decGuideMode = pConfig->Profile.GetInt(prefix + "/DecGuideMode", DefaultDecGuideMode);
    SetDecGuideMode(decGuideMode);

    int raGuideAlgorithm = pConfig->Profile.GetInt(prefix + "/XGuideAlgorithm", DefaultRaGuideAlgorithm);
    SetXGuideAlgorithm(raGuideAlgorithm);

    int decGuideAlgorithm = pConfig->Profile.GetInt(prefix + "/YGuideAlgorithm", DefaultDecGuideAlgorithm);
    SetYGuideAlgorithm(decGuideAlgorithm);

    bool val = pConfig->Profile.GetBoolean(prefix + "/CalFlipRequiresDecFlip", false);
    SetCalibrationFlipRequiresDecFlip(val);

    val = pConfig->Profile.GetBoolean(prefix + "/AssumeOrthogonal", false);
    SetAssumeOrthogonal(val);

    val = pConfig->Profile.GetBoolean(prefix + "/UseDecComp", true);
    EnableDecCompensation(val);

    m_backlashComp = new BacklashComp(this);
}

void Scope::LoadProfileSetup()
{
    // The constructor reads these too. set_profile_setup changes them while the mount object lives, and the
    // mount object is rebuilt only on a mount or profile change, so set_profile_setup calls this too (#117).
    // GetMountClassName() is "scope" for every mount, and the setters write "/scope/...".
    SetCalibrationDuration(pConfig->Profile.GetInt("/scope/CalibrationDuration", DefaultCalibrationDuration));
    SetCalibrationDistance(pConfig->Profile.GetInt("/scope/CalibrationDistance", CalstepDialog::DEFAULT_DISTANCE));
    m_hasHPEncoders = pConfig->Profile.GetBoolean("/scope/HiResEncoders", false);
}

Scope::~Scope() { }

GUIDE_ALGORITHM Scope::DefaultXGuideAlgorithm() const
{
    return DefaultRaGuideAlgorithm;
}

GUIDE_ALGORITHM Scope::DefaultYGuideAlgorithm() const
{
    return DefaultDecGuideAlgorithm;
}

bool Scope::SetCalibrationDuration(int calibrationDuration)
{
    bool bError = false;

    try
    {
        if (calibrationDuration <= 0)
        {
            throw ERROR_INFO("invalid calibrationDuration");
        }

        m_calibrationDuration = calibrationDuration;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_calibrationDuration = DefaultCalibrationDuration;
    }

    pConfig->Profile.SetInt("/scope/CalibrationDuration", m_calibrationDuration);

    return bError;
}

bool Scope::SetCalibrationDistance(int calibrationDistance)
{
    bool bError = false;

    try
    {
        if (calibrationDistance <= 0)
        {
            throw ERROR_INFO("invalid calibrationDistance");
        }

        m_calibrationDistance = calibrationDistance;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_calibrationDistance = CalstepDialog::DEFAULT_DISTANCE;
    }

    pConfig->Profile.SetInt("/scope/CalibrationDistance", m_calibrationDistance);
    return bError;
}

bool Scope::SetMaxDecDuration(int maxDecDuration)
{
    bool bError = false;

    try
    {
        if (maxDecDuration < 0)
        {
            throw ERROR_INFO("maxDecDuration < 0");
        }

        if (m_maxDecDuration != maxDecDuration)
            pFrame->NotifyGuidingParam("Dec Max Duration", maxDecDuration);

        m_maxDecDuration = maxDecDuration;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_maxDecDuration = DefaultMaxDecDuration;
    }

    pConfig->Profile.SetInt("/scope/MaxDecDuration", m_maxDecDuration);

    return bError;
}

bool Scope::SetMaxRaDuration(int maxRaDuration)
{
    bool bError = false;

    try
    {
        if (maxRaDuration < 0)
        {
            throw ERROR_INFO("maxRaDuration < 0");
        }

        if (m_maxRaDuration != maxRaDuration)
        {
            pFrame->NotifyGuidingParam("RA Max Duration", maxRaDuration);

            m_maxRaDuration = maxRaDuration;
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_maxRaDuration = DefaultMaxRaDuration;
    }

    pConfig->Profile.SetInt("/scope/MaxRaDuration", m_maxRaDuration);

    return bError;
}

wxString Scope::DecGuideModeStr(DEC_GUIDE_MODE m)
{
    switch (m)
    {
    case DEC_NONE:
        return "Off";
    case DEC_AUTO:
        return "Auto";
    case DEC_NORTH:
        return "North";
    case DEC_SOUTH:
        return "South";
    default:
        return "Invalid";
    }
}

bool Scope::SetDecGuideMode(int decGuideMode)
{
    bool bError = false;

    try
    {
        switch (decGuideMode)
        {
        case DEC_NONE:
        case DEC_AUTO:
        case DEC_NORTH:
        case DEC_SOUTH:
            break;
        default:
            throw ERROR_INFO("invalid decGuideMode");
            break;
        }

        if (m_decGuideMode != decGuideMode)
        {
            // atomic: the worker reads it during a guide move (#280)
            m_decGuideMode = (DEC_GUIDE_MODE) decGuideMode;

            wxString s = DecGuideModeStr(m_decGuideMode.load());
            Debug.Write(wxString::Format("DecGuideMode set to %s (%d)\n", s, decGuideMode));
            pFrame->NotifyGuidingParam("Dec Guide Mode", s);
            BacklashComp *blc = GetBacklashComp();
            if (blc)
            {
                if (decGuideMode != DEC_AUTO)
                    blc->EnableBacklashComp(
                        false); // Can't do blc in uni-direction mode because there's no recovery from over-shoots
                else
                    blc->ResetBLCState();
            }
            pConfig->Profile.SetInt("/scope/DecGuideMode", m_decGuideMode.load());
            if (pFrame)
                pFrame->UpdateStatusBarCalibrationStatus();
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    return bError;
}

static int CompareNoCase(const wxString& first, const wxString& second)
{
    return first.CmpNoCase(second);
}

static wxString AlpacaMountName()
{
    wxString host = pConfig->Profile.GetString("/alpaca/host", wxEmptyString);
    long port = pConfig->Profile.GetLong("/alpaca/port", 0);
    long device = pConfig->Profile.GetLong("/alpaca/telescope_device", 0);
    if (host.empty() || port <= 0)
        return _("Alpaca Mount");
    return wxString::Format(_("Alpaca Mount [%s:%ld/%ld]"), host, port, device);
}

wxArrayString Scope::MountList()
{
    wxArrayString ScopeList;

    ScopeList.Add(_("None"));
#ifdef GUIDE_ALPACA
    ScopeList.Add(AlpacaMountName());
#endif

    ScopeList.Sort(&CompareNoCase);

    return ScopeList;
}

Scope *Scope::Factory(const wxString& choice)
{
    Scope *pReturn = nullptr;

    try
    {
        if (choice.IsEmpty())
        {
            throw ERROR_INFO("ScopeFactory called with choice.IsEmpty()");
        }

        Debug.Write(wxString::Format("ScopeFactory(%s)\n", choice));

        if (false) // so else ifs can follow
        {
        }
        // Alpaca first since its choice string could match generic strings below
#ifdef GUIDE_ALPACA
        else if (choice.Contains(_("Alpaca")))
            pReturn = AlpacaScopeFactory::MakeAlpacaScope();
#endif
        else if (choice == _("None"))
            pReturn = nullptr;
        else if (choice.Contains(ScopeManualPointing::GetDisplayName()))
            pReturn = new ScopeManualPointing();
        else
        {
            throw ERROR_INFO("ScopeFactory: Unknown Scope choice");
        }

        if (pReturn)
        {
            // virtual function call means we cannot do this in the Scope constructor
            pReturn->EnableStopGuidingWhenSlewing(
                pConfig->Profile.GetBoolean("/scope/StopGuidingWhenSlewing", pReturn->CanCheckSlewing()));
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        if (pReturn)
        {
            delete pReturn;
            pReturn = nullptr;
        }
    }

    return pReturn;
}

bool Scope::CalibrationFlipRequiresDecFlip()
{
    return m_calibrationFlipRequiresDecFlip;
}

// No 'Set' function, property is set via the /scope/HiResEncoders profile key (set_profile_setup)
bool Scope::HasHPEncoders() const
{
    return m_hasHPEncoders;
}

void Scope::SetCalibrationFlipRequiresDecFlip(bool val)
{
    m_calibrationFlipRequiresDecFlip = val;
    pConfig->Profile.SetBoolean("/scope/CalFlipRequiresDecFlip", val);
}

void Scope::SetAssumeOrthogonal(bool val)
{
    m_assumeOrthogonal = val;
    pConfig->Profile.SetBoolean("/scope/AssumeOrthogonal", val);
}

void Scope::EnableStopGuidingWhenSlewing(bool enable)
{
    if (enable)
        Debug.Write("Scope: enabling slew check, guiding will stop when slew is detected\n");
    else
        Debug.Write("Scope: slew check disabled\n");

    pConfig->Profile.SetBoolean("/scope/StopGuidingWhenSlewing", enable);
    m_stopGuidingWhenSlewing = enable;
}

bool Scope::IsDecDrifting() const
{
    return m_decGuideMode == DEC_NONE;
}

Mount::MOVE_RESULT Scope::MoveAxis(GUIDE_DIRECTION direction, int duration, unsigned int moveOptions)
{
    MOVE_RESULT result = MOVE_OK;

    Debug.Write(wxString::Format("scope move axis dir= %d dur= %d opts= 0x%x\n", direction, duration, moveOptions));

    try
    {
        MoveResultInfo move;
        result = MoveAxis(direction, duration, moveOptions, &move);

        if (result != MOVE_OK)
        {
            throw THROW_INFO("Move failed");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    return result;
}

int Scope::CalibrationMoveSize()
{
    return m_calibrationDuration;
}

static wxString LimitReachedWarningKey(intptr_t axis)
{
    // we want the key to be under "/Confirm" so ConfirmDialog::ResetAllDontAskAgain() resets it, but we also want the setting
    // to be per-profile
    return wxString::Format("/Confirm/%d/Max%sLimitWarningEnabled", pConfig->GetCurrentProfileId(),
                            axis == GUIDE_RA ? "RA" : "Dec");
}

static void SuppressLimitReachedWarning(intptr_t axis)
{
    pConfig->Global.SetBoolean(LimitReachedWarningKey(axis), false);
}

void Scope::DeferPulseLimitAlertCheck()
{
    enum
    {
        LIMIT_REACHED_GRACE_PERIOD_SECONDS = 120
    };

    m_limitReachedDeferralTime = guide_move_plan::AlertClockS() + LIMIT_REACHED_GRACE_PERIOD_SECONDS;
}

void Scope::AlertLimitReached(int duration, GuideAxis axis)
{
    // Not during the grace period, and at most once every 30 s per axis (guide_move_plan.cpp, #164),
    // both on the monotonic clock (#212)
    guide_move_plan::Axis throttleAxis = axis == GUIDE_RA ? guide_move_plan::Axis::Ra : guide_move_plan::Axis::Dec;
    if (!m_limitAlertThrottle.ShouldRaise(throttleAxis, guide_move_plan::AlertClockS(), m_limitReachedDeferralTime))
        return;

    if (duration < MAX_DURATION_MAX)
    {
        int defaultVal = axis == GUIDE_RA ? DefaultMaxRaDuration : DefaultMaxDecDuration;
        if (duration >= defaultVal) // Max duration is probably ok, some other kind of problem
        {
            wxString msg;
            if (axis == GUIDE_RA)
            {
                if (CanPulseGuide())
                {
                    msg = _("PHD2 is not able to make sufficient corrections in RA.  Check for cable snags, try re-doing your "
                            "calibration, and "
                            "check for problems with the mount mechanics.");
                }
                else
                {
                    msg = _("PHD2 is not able to make sufficient corrections in RA.  Check for cable snags, try re-doing your "
                            "calibration, and "
                            "confirm the ST-4 cable is working properly.");
                }
            }
            else // Dec axis problems
            {
                if (CanPulseGuide())
                {
                    msg = _("PHD2 is not able to make sufficient corrections in Dec.  If the side-of-pier has changed from "
                            "where you last calibrated, "
                            "check to see if the 'Reverse Dec output option' on the Advanced Dialog guiding tab is wrong. If "
                            "so, fix it and recalibrate.  "
                            "Otherwise, "
                            "check for cable snags, try re-doing your calibration, and check for problems with the mount "
                            "mechanics.");
                }
                else
                {
                    msg = _("PHD2 is not able to make sufficient corrections in Dec.  Check for cable snags, try re-doing your "
                            "calibration and "
                            "confirm the ST-4 cable is working properly.");
                }
            }
            pFrame->SuppressibleAlert(LimitReachedWarningKey(axis), msg, SuppressLimitReachedWarning, axis, false,
                                      wxICON_INFORMATION);
        }
        else // Max duration has been decreased by user, start by recommending use of default value
        {
            wxString s = axis == GUIDE_RA ? _("Max RA Duration setting") : _("Max Dec Duration setting");
            pFrame->SuppressibleAlert(
                LimitReachedWarningKey(axis),
                wxString::Format(_("Your %s is preventing PHD from making adequate corrections to keep the guide star locked. "
                                   "Try restoring %s to its default value to allow PHD2 to make larger corrections."),
                                 s, s),
                SuppressLimitReachedWarning, axis, false, wxICON_INFORMATION);
        }
    }
    else // Already at maximum allowed value
    {
        wxString which_axis = axis == GUIDE_RA ? _("RA") : _("Dec");
        pFrame->SuppressibleAlert(
            LimitReachedWarningKey(axis),
            wxString::Format(
                _("Even using the maximum moves, PHD2 can't properly correct for the large guide star movements in %s. "
                  "Guiding will be impaired until you can eliminate the source of these problems."),
                which_axis),
            SuppressLimitReachedWarning, axis, false, wxICON_INFORMATION);
    }
}

Mount::MOVE_RESULT Scope::MoveAxis(GUIDE_DIRECTION direction, int duration, unsigned int moveOptions,
                                   MoveResultInfo *moveResult)
{
    MOVE_RESULT result = MOVE_OK;
    bool limitReached = false;

    try
    {
        Debug.Write(
            wxString::Format("MoveAxis(%s, %d, %s)\n", DirectionChar(direction), duration, DumpMoveOptionBits(moveOptions)));

        // Compute the actual guide durations: guiding disabled, Dec guide mode, max duration and
        // the limit-reached count (guide_move_plan.cpp)
        guide_move_plan::AxisPolicy policy;
        policy.guidingEnabled = m_guidingEnabled;
        policy.decMode = static_cast<guide_move_plan::DecMode>(m_decGuideMode.load());
        policy.maxRaMs = m_maxRaDuration;
        policy.maxDecMs = m_maxDecDuration;
        guide_move_plan::AxisPlan plan = guide_move_plan::PlanAxis(static_cast<guide_move_plan::Dir>(direction), duration,
                                                                   moveOptions, policy, m_raLimitReached, m_decLimitReached);

        if (plan.refused)
        {
            throw THROW_INFO("Guiding disabled");
        }

        bool decAxis = direction == NORTH || direction == SOUTH;
        duration = plan.ms;
        limitReached = plan.limited;

        if (plan.zeroedByDecMode)
            Debug.Write("duration set to 0 by GuideMode\n");

        if (plan.capped)
            Debug.Write(wxString::Format("duration set to %d by the pulse ceiling\n", duration));
        else if (plan.limited)
            Debug.Write(wxString::Format("duration set to %d by %s\n", duration, decAxis ? "maxDecDuration" : "maxRaDuration"));

        if (plan.alert)
            AlertLimitReached(duration, decAxis ? GUIDE_DEC : GUIDE_RA);

        // Actually do the guide
        if (duration > 0)
        {
            result = Guide(direction, duration);
            if (result != MOVE_OK)
            {
                throw ERROR_INFO("guide failed");
            }
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        if (result == MOVE_OK)
            result = MOVE_ERROR;
        duration = 0;
    }

    Debug.Write(wxString::Format("Move returns status %d, amount %d\n", result, duration));

    if (moveResult)
    {
        moveResult->amountMoved = duration;
        moveResult->limited = limitReached;
    }

    return result;
}

static wxString CalibrationWarningKey(CalibrationIssueType etype)
{
    wxString qual;
    switch (etype)
    {
    case CI_Angle:
        qual = "Angle";
        break;
    case CI_Different:
        qual = "Diff";
        break;
    case CI_Steps:
        qual = "Steps";
        break;
    case CI_Rates:
        qual = "Rates";
        break;
    case CI_None:
        qual = "Bogus";
        break;
    }
    wxString rtn = wxString::Format("/Confirm/%d/CalWarning_%s", pConfig->GetCurrentProfileId(), qual);
    return rtn;
}

// Do some basic sanity checking on the just-completed calibration, looking for things that are fishy.  Do the checking in the
// order of importance/confidence, since we only alert about a single condition
void Scope::SanityCheckCalibration(const Calibration& oldCal, const CalibrationDetails& oldDetails)
{
    Calibration newCal;
    GetLastCalibration(&newCal);

    CalibrationDetails newDetails;
    LoadCalibrationDetails(&newDetails);

    // The checks are calibration_sanity::Classify (#142); the advisory and the Debug lines stay here
    calibration_sanity::Input in;
    in.raStepCount = newDetails.raStepCount;
    in.decStepCount = newDetails.decStepCount;
    in.xAngle = newCal.xAngle;
    in.yAngle = newCal.yAngle;
    in.xRate = newCal.xRate;
    in.yRate = newCal.yRate;
    in.declination = newCal.declination;
    in.raGuideSpeed = newDetails.raGuideSpeed;
    in.decGuideSpeed = newDetails.decGuideSpeed;
    in.imageScale = newDetails.imageScale;
    in.oldValid = oldCal.isValid;
    in.oldXAngle = oldCal.xAngle;
    in.oldYRate = oldCal.yRate;
    in.oldImageScale = oldDetails.imageScale;
    in.decCompEnabled = DecCompensationEnabled();
    in.decCompLimit = Scope::DEC_COMP_LIMIT;

    calibration_sanity::Result check = calibration_sanity::Classify(in);
    m_lastCalibrationIssue = (CalibrationIssueType) check.issue;

    wxString detailInfo;
    switch (check.issue)
    {
    case calibration_sanity::Issue::None:
        break;
    case calibration_sanity::Issue::Steps:
        detailInfo =
            wxString::Format("Actual RA calibration steps = %d, Dec calibration steps = %d", check.raSteps, check.decSteps);
        break;
    case calibration_sanity::Issue::Angle:
        detailInfo = wxString::Format("Non-orthogonality = %0.3f", check.nonOrthoDeg);
        break;
    case calibration_sanity::Issue::Rates:
        detailInfo = wxString::Format("Expected ratio at dec=%0.1f is %0.3f, actual is %0.3f", check.declinationDeg,
                                      check.expectedRatio, check.actualRatio);
        break;
    case calibration_sanity::Issue::Different:
        detailInfo = wxString::Format("Current/previous Dec rate ratio is %0.3f", check.decRateRatio);
        break;
    }

    if (m_lastCalibrationIssue != CI_None)
    {
        wxString alertMsg;

        FlagCalibrationIssue(newDetails, m_lastCalibrationIssue);
        switch (m_lastCalibrationIssue)
        {
        case CI_Steps:
            alertMsg = _("Advisory: Calibration completed but few guide steps were used, so accuracy is questionable");
            break;
        case CI_Angle:
            alertMsg = _("Advisory: Calibration completed but RA/Dec axis angles are questionable and guiding may be impaired");
            break;
        case CI_Different:
            alertMsg = _("Advisory: This calibration is substantially different from the previous one - have you changed "
                         "configurations?");
            break;
        case CI_Rates:
            alertMsg = _("Advisory: Calibration completed but RA and Dec rates vary by an unexpected amount (often caused by "
                         "large Dec backlash)");
        default:
            break;
        }

        // CalWarning_* keys that an older build saved are still honored; nothing writes them any more
        if (pConfig->Global.GetBoolean(CalibrationWarningKey(m_lastCalibrationIssue),
                                       true)) // User hasn't disabled this type of alert
        {
            pFrame->Alert(alertMsg);
        }
        else
        {
            Debug.Write(wxString::Format(
                "Alert detected in scope calibration but not shown to user - suppressed message was: %s\n", alertMsg));
        }

        Debug.Write(wxString::Format("Calibration alert details: %s\n", detailInfo));
    }
    else
    {
        Debug.Write("Calibration passed sanity checks...\n");
    }
}

void Scope::ClearCalibration()
{
    Mount::ClearCalibration();

    m_calRun.Clear();
}

// Handle gross errors in cal_step duration when user changes mount guide speeds or binning.
void Scope::CheckCalibrationDuration(int currDuration)
{
    CalibrationDetails calDetails;
    LoadCalibrationDetails(&calDetails);

    auto binning = pCamera->GetBinning();
    bool binningChange = binning != calDetails.origBinning;

    // if binning changed, may need to update the calibration distance
    if (binningChange)
    {
        int prevDistance = GetCalibrationDistance();
        auto focalLength = pFrame->GetFocalLength();
        auto pixelSize = pCamera->GetCameraPixelSize();
        int newDistance = CalstepDialog::GetCalibrationDistance(focalLength, pixelSize, binning);

        if (newDistance != prevDistance)
        {
            Debug.Write(
                wxString::Format("CalDistance adjusted at start of calibration from %d to %d because of binning change\n",
                                 prevDistance, newDistance));

            SetCalibrationDistance(newDistance);
        }
    }

    double raSpd;
    double decSpd;
    double const siderealSecsPerSec = 0.9973;
    bool haveRates = !pPointingSource->GetGuideRates(&raSpd, &decSpd); // units of degrees/sec as in ASCOM

    double currSpdX = raSpd * 3600.0 / (15.0 * siderealSecsPerSec); // multiple of sidereal

    // Don't check the step size on very first calibration and don't adjust if the reported mount guide speeds are bogus
    if (!haveRates || calDetails.raGuideSpeed <= 0)
        return;

    bool refineStepSize =
        binningChange || (fabs(1.0 - raSpd / calDetails.raGuideSpeed) > 0.05); // binning change or speed change of > 5%
    if (!refineStepSize)
        return;

    auto focalLength = pFrame->GetFocalLength();
    auto pixelSize = pCamera->GetCameraPixelSize();
    int rslt;
    if (!CalstepDialog::GetCalibrationStepSize(focalLength, pixelSize, binning, currSpdX, CalstepDialog::DEFAULT_STEPS, 0.0,
                                               GetCalibrationDistance(), 0, &rslt))
    {
        Debug.Write(wxString::Format("CalDuration not adjusted: no step size for fl= %d, px= %.3fu, bin= %d, speed= %.3fx\n",
                                     focalLength, pixelSize, binning, currSpdX));
        return;
    }

    wxString why = binningChange ? " binning " : " mount guide speed ";
    Debug.Write(wxString::Format("CalDuration adjusted at start of calibration from %d to %d because of %s change\n",
                                 currDuration, rslt, why));

    SetCalibrationDuration(rslt);
}

bool Scope::BeginCalibration(const PHD_Point& currentLocation)
{
    bool bError = false;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Not connected");
        }

        if (!currentLocation.IsValid())
        {
            throw ERROR_INFO("Must have a valid lock position");
        }

        CheckCalibrationDuration(m_calibrationDuration); // Make sure guide speeds or binning haven't changed underneath us
        ClearCalibration();

        // Size the West/East pulses for the Dec, read once here (#141). Not stored: m_calibrationDuration
        // and the profile keep the Dec 0 step, and the Dec leg uses it. An unknown Dec keeps the step.
        double declination = pPointingSource->GetDeclinationRadians();
        std::optional<double> decRad;
        if (declination != UNKNOWN_DECLINATION)
            decRad = declination;
        Debug.Write(wxString::Format("RA calibration step %d ms for Dec %s (Dec step %d ms)\n",
                                     calibration_run::RaCalibrationStepMs(m_calibrationDuration, decRad),
                                     DeclinationStr(declination), m_calibrationDuration));

        m_calRun.Begin(calibration_run::Pt::At(currentLocation.X, currentLocation.Y), decRad);
        m_calibrationDetails.raSteps.clear();
        m_calibrationDetails.decSteps.clear();
        m_calibrationDetails.lastIssue = CI_None;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    return bError;
}

void Scope::SetCalibration(const Calibration& cal)
{
    m_calibration = cal;
    Mount::SetCalibration(cal);
}

void Scope::SetCalibrationDetails(const CalibrationDetails& calDetails, double xAngle, double yAngle, int binning)
{
    m_calibrationDetails = calDetails;

    double ra_rate;
    double dec_rate;
    if (pPointingSource->GetGuideRates(&ra_rate, &dec_rate)) // true means error
    {
        ra_rate = -1.0;
        dec_rate = -1.0;
    }

    m_calibrationDetails.raGuideSpeed = ra_rate;
    m_calibrationDetails.decGuideSpeed = dec_rate;
    m_calibrationDetails.focalLength = pFrame->GetFocalLength();
    m_calibrationDetails.imageScale = pFrame->GetCameraPixelScale();
    m_calibrationDetails.orthoError =
        degrees(fabs(fabs(norm_angle(xAngle - yAngle)) - M_PI / 2.)); // Delta from the nearest multiple of 90 degrees
    m_calibrationDetails.origBinning = binning;
    m_calibrationDetails.origTimestamp = wxDateTime::Now().Format();
    m_calibrationDetails.origPierSide = pPointingSource->SideOfPier();

    SaveCalibrationDetails(m_calibrationDetails);
}

void Scope::FlagCalibrationIssue(const CalibrationDetails& calDetails, CalibrationIssueType issue)
{
    m_calibrationDetails = calDetails;
    m_calibrationDetails.lastIssue = issue;

    SaveCalibrationDetails(m_calibrationDetails);
}

bool Scope::IsCalibrated() const
{
    if (!Mount::IsCalibrated())
        return false;

    switch (m_decGuideMode.load())
    {
    case DEC_NONE:
        return true;
    case DEC_AUTO:
    case DEC_NORTH:
    case DEC_SOUTH:
    {
        bool have_ns_calibration = m_calibration.yRate != CALIBRATION_RATE_UNCALIBRATED;
        return have_ns_calibration;
    }
    default:
        assert(false);
        return true;
    }
}

void Scope::EnableDecCompensation(bool enable)
{
    m_useDecCompensation = enable;
    wxString prefix = "/" + GetMountClassName();
    pConfig->Profile.SetBoolean(prefix + "/UseDecComp", enable);
}

int Scope::CalibrationTotDistance()
{
    return GetCalibrationDistance();
}

static wxString DecBacklashAlertKey()
{
    // we want the key to be under "/Confirm" so ConfirmDialog::ResetAllDontAskAgain() resets it, but we also want the setting
    // to be per-profile
    return wxString::Format("/Confirm/%d/DecBacklashWarningEnabled", pConfig->GetCurrentProfileId());
}

static void SuppressDecBacklashAlert(long)
{
    pConfig->Global.SetBoolean(DecBacklashAlertKey(), false);
}

static void CalibrationStatus(CalibrationStepInfo& info, const wxString& msg)
{
    info.msg = msg;
    pFrame->StatusMsg(info.msg);
    EvtServer.NotifyCalibrationStep(info);
}

static const wxString& CalibrationLegName(calibration_run::Leg leg)
{
    static const wxString names[] = { _T("West"), _T("East"), _T("Backlash"), _T("North"), _T("South"), _T("NudgeSouth") };
    return names[static_cast<int>(leg)];
}

static CalibrationStepInfo CalibrationStepFor(Mount *mount, const calibration_run::StepRecord& rec)
{
    PHD_Point pos(rec.pos.x, rec.pos.y);
    if (!rec.pos.valid)
        pos.Invalidate();
    return CalibrationStepInfo(mount, CalibrationLegName(rec.leg), rec.step, rec.dx, rec.dy, pos, rec.dist);
}

static wxString CalibrationStepMessage(const calibration_run::Effect& e)
{
    switch (e.message)
    {
    case calibration_run::Message::WestStep:
        return wxString::Format(_("West step %3d, dist=%4.1f"), e.number, e.rec.dist);
    case calibration_run::Message::EastStep:
        return wxString::Format(_("East step %3d, dist=%4.1f"), e.number, e.rec.dist);
    case calibration_run::Message::BacklashFirst:
        return _("Clearing backlash step 1");
    case calibration_run::Message::BacklashStep:
        return wxString::Format(_("Clearing backlash step %3d"), e.number);
    case calibration_run::Message::NorthStep:
        return wxString::Format(_("North step %3d, dist=%4.1f"), e.number, e.rec.dist);
    case calibration_run::Message::SouthStep:
        return wxString::Format(_("South step %3d, dist=%4.1f"), e.number, e.rec.dist);
    case calibration_run::Message::NudgeSouth:
        return wxString::Format(_("Nudge South %3d"), e.number);
    }
    return wxEmptyString;
}

// What the calibration run reads from the mount, and where its side effects go (#142).
// Each effect is the call UpdateCalibrationState made at that point before the run moved out of Scope.
class ScopeCalibrationHost : public calibration_run::Probe, public calibration_run::Sink
{
    Scope *m_scope;
    wxString m_lastStatus; // the last status message sent, for the backlash Debug line

public:
    explicit ScopeCalibrationHost(Scope *scope) : m_scope(scope) { }

    bool Coordinates(double *raHours, double *decDeg) override
    {
        double lst;
        return !pPointingSource->GetCoordinates(raHours, decDeg, &lst); // true means error
    }

    bool GuideRates(double *raDegPerSec, double *decDegPerSec) override
    {
        return !pPointingSource->GetGuideRates(raDegPerSec, decDegPerSec); // true means error
    }

    bool CanPulseGuide() override { return m_scope->CanPulseGuide(); }

    void Emit(const calibration_run::Effect& e) override
    {
        switch (e.kind)
        {
        case calibration_run::Effect::LogStep:
        {
            CalibrationStepInfo info(CalibrationStepFor(m_scope, e.rec));
            GuideLog.CalibrationStep(info);
            break;
        }
        case calibration_run::Effect::Status:
        {
            CalibrationStepInfo info(CalibrationStepFor(m_scope, e.rec));
            CalibrationStatus(info, CalibrationStepMessage(e));
            m_lastStatus = info.msg;
            break;
        }
        case calibration_run::Effect::Move:
            pFrame->ScheduleAxisMove(m_scope, static_cast<GUIDE_DIRECTION>(e.dir), e.ms, MOVEOPTS_CALIBRATION_MOVE);
            break;
        case calibration_run::Effect::DirectComplete:
            GuideLog.CalibrationDirectComplete(m_scope, e.ra ? "West" : "North", e.angle, e.rate,
                                               static_cast<GuideParity>(e.parity));
            break;
        case calibration_run::Effect::EastAdvisory:
        {
            wxString msg(
                wxTRANSLATE("Advisory: Little or no east movement was measured, so guiding will probably be impaired. "
                            "Check the guide cable and use the Manual Guide tool to confirm basic operation of the mount."));
            const wxString& translated(wxGetTranslation(msg));
            pFrame->Alert(translated, 0, wxEmptyString, 0, 0, true);
            Debug.Write("Calibration alert: " + msg + "\n");
            break;
        }
        case calibration_run::Effect::SouthAdvisory:
        {
            wxString msg;
            switch (e.southAdvice)
            {
            case calibration_run::SouthAdvice::NoSouthCannotPulseGuide:
                msg = wxTRANSLATE("Advisory: Calibration successful but little or no south movement was measured, so "
                                  "guiding will probably be impaired.\n "
                                  "This is usually caused by a faulty guide cable or very large Dec backlash. \n"
                                  "Check the guide cable and read the online Help for how to identify these types of "
                                  "problems (Manual Guide, Declination backlash).");
                break;
            case calibration_run::SouthAdvice::LittleSouthCannotPulseGuide:
                msg = wxTRANSLATE(
                    "Advisory: Calibration successful but little south movement was measured, so guiding will probably "
                    "be impaired. \n"
                    "This is usually caused by very large Dec backlash or other problems with the mount mechanics. \n"
                    "Read the online Help for how to identify these types of problems (Manual Guide, Declination "
                    "backlash).");
                break;
            case calibration_run::SouthAdvice::LittleSouth:
                msg = wxTRANSLATE(
                    "Advisory: Calibration successful but little south movement was measured, so guiding may be "
                    "impaired.\n "
                    "This is usually caused by very large Dec backlash or other problems with the mount mechanics. \n"
                    "Read the online help for how to deal with this type of problem (Declination backlash).");
                break;
            }

            // if (!m_eastAlertShown)
            //{
            //     const wxString& translated(wxGetTranslation(msg));
            //     pFrame->SuppressibleAlert(DecBacklashAlertKey(), translated, SuppressDecBacklashAlert, 0, true);
            // }
            Debug.Write("Omitted calibration alert: " + msg + "\n");
            break;
        }
        case calibration_run::Effect::BacklashDebug:
            Debug.Write(
                wxString::Format("Backlash: %s, Last Delta = %0.2f px, CumDistance = %0.2f px\n", m_lastStatus, e.a, e.b));
            break;
        case calibration_run::Effect::Debug:
            Debug.Write(wxString(e.text));
            break;
        }
    }
};

bool Scope::UpdateCalibrationState(const PHD_Point& currentLocation)
{
    bool bError = false;

    try
    {
        calibration_run::Settings settings;
        settings.stepMs = m_calibrationDuration;
        settings.distancePx = GetCalibrationDistance();
        settings.maxRaMs = m_maxRaDuration;
        settings.maxDecMs = m_maxDecDuration;
        settings.decMode = static_cast<calibration_run::DecMode>(m_decGuideMode.load());
        settings.assumeOrthogonal = m_assumeOrthogonal;
        settings.fastRecenter = pFrame->pGuider->IsFastRecenterEnabled();
        settings.maxMovePx = pFrame->pGuider->GetMaxMovePixels();

        calibration_run::Pt location { currentLocation.X, currentLocation.Y, currentLocation.IsValid() };

        ScopeCalibrationHost host(this);
        calibration_run::FrameResult result = m_calRun.OnFrame(location, settings, host, host);

        switch (result.outcome)
        {
        case calibration_run::Outcome::Continue:
            break;

        case calibration_run::Outcome::Failed:
            switch (result.fail)
            {
            case calibration_run::Fail::Cleared:
            {
                // #137: the calibration was cleared while it ran (the legacy socket server's CLEARCAL still can);
                // end it as failed instead of aborting, or staying in a state that has no next step
                wxString msg(wxTRANSLATE("Calibration failed: the calibration was cleared while calibrating"));
                GuideLog.CalibrationFailed(this, msg);
                EvtServer.NotifyCalibrationFailed(this, msg);
                throw ERROR_INFO("calibration cleared while calibrating");
            }
            case calibration_run::Fail::RaDidNotMove:
            {
                wxString msg(wxTRANSLATE("RA Calibration Failed: star did not move enough"));
                const wxString& translated(wxGetTranslation(msg));
                pFrame->Alert(translated);
                GuideLog.CalibrationFailed(this, msg);
                EvtServer.NotifyCalibrationFailed(this, msg);
                throw ERROR_INFO("RA calibration failed");
            }
            case calibration_run::Fail::BacklashClearingFailed:
            {
                wxString msg(wxTRANSLATE("Backlash Clearing Failed: star did not move enough"));
                const wxString& translated(wxGetTranslation(msg));
                pFrame->Alert(translated);
                GuideLog.CalibrationFailed(this, msg);
                EvtServer.NotifyCalibrationFailed(this, msg);
                throw ERROR_INFO("Clear backlash failed");
            }
            case calibration_run::Fail::DecDidNotMove:
            {
                wxString msg(wxTRANSLATE("DEC Calibration Failed: star did not move enough"));
                const wxString& translated(wxGetTranslation(msg));
                pFrame->Alert(translated);
                GuideLog.CalibrationFailed(this, msg);
                EvtServer.NotifyCalibrationFailed(this, msg);
                throw ERROR_INFO("Dec calibration failed");
            }
            case calibration_run::Fail::DecModeChanged:
            {
                // #148: the Dec leg was skipped for DEC_NONE and the mode was then switched to one that
                // guides Dec; a calibration with no Dec rate would leave IsCalibrated() false for good
                wxString msg(wxTRANSLATE("Calibration failed: the Dec guide mode was changed from Off during calibration"));
                const wxString& translated(wxGetTranslation(msg));
                pFrame->Alert(translated);
                GuideLog.CalibrationFailed(this, msg);
                EvtServer.NotifyCalibrationFailed(this, msg);
                throw ERROR_INFO("Dec guide mode changed from Off during calibration");
            }
            }
            break;

        case calibration_run::Outcome::Complete:
        {
            const calibration_run::Measured& measured = m_calRun.measured();
            if (result.firstComplete)
            {
                // What the run measured. On a later frame in this state (the mount
                // disconnected, so Mount::IsCalibrated() is false) these members
                // already hold the calibration stored on the first one.
                m_calibration.xAngle = measured.xAngle;
                m_calibration.xRate = measured.xRate;
                m_calibration.yAngle = measured.yAngle;
                m_calibration.yRate = measured.yRate;
                m_calibration.raGuideParity = static_cast<GuideParity>(measured.raParity);
                m_calibration.decGuideParity = static_cast<GuideParity>(measured.decParity);
                m_calibrationDetails.raSteps.clear();
                for (const auto& step : measured.raSteps)
                    m_calibrationDetails.raSteps.push_back(wxRealPoint(step.first, step.second));
                m_calibrationDetails.decSteps.clear();
                for (const auto& step : measured.decSteps)
                    m_calibrationDetails.decSteps.push_back(wxRealPoint(step.first, step.second));
            }

            GetLastCalibration(&m_prevCalibration);
            LoadCalibrationDetails(&m_prevCalibrationDetails);
            Calibration cal(m_calibration);
            cal.declination = pPointingSource->GetDeclinationRadians();
            cal.pierSide = pPointingSource->SideOfPier();
            cal.rotatorAngle = Rotator::RotatorPosition();
            cal.binning = pCamera->GetBinning();
            SetCalibration(cal);
            m_calibrationDetails.raStepCount = measured.raStepCount;
            m_calibrationDetails.decStepCount = measured.decStepCount;
            SetCalibrationDetails(m_calibrationDetails, m_calibration.xAngle, m_calibration.yAngle, m_calibration.binning);
            if (SANITY_CHECKING_ACTIVE)
                SanityCheckCalibration(m_prevCalibration, m_prevCalibrationDetails); // method gets "new" info itself
            pFrame->StatusMsg(_("Calibration complete"));
            GuideLog.CalibrationComplete(this);
            EvtServer.NotifyCalibrationComplete(this);
            Debug.Write("Calibration Complete\n");
            pConfig->Flush();
            break;
        }
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);

        ClearCalibration();

        bError = true;
    }

    return bError;
}

// Get a value of declination, in radians, that can be used for adjusting the RA guide rate,
// or UNKNOWN_DECLINATION if the declination is not known.
double Scope::GetDeclinationRadians()
{
    return UNKNOWN_DECLINATION;
}

// Baseline implementations for non-ASCOM subclasses.  Methods will
// return a sensible default or an error (true)
bool Scope::GetGuideRates(double *pRAGuideRate, double *pDecGuideRate)
{
    return true; // error, not implemented
}

bool Scope::GetCoordinates(double *ra, double *dec, double *siderealTime)
{
    return true; // error
}

bool Scope::GetSiteLatLong(double *latitude, double *longitude)
{
    return true; // error
}

bool Scope::CanReportPosition()
{
    return false;
}

bool Scope::CanPulseGuide()
{
    return false;
}

bool Scope::CanCheckSlewing()
{
    return false;
}

bool Scope::Slewing()
{
    return false;
}

PierSide Scope::SideOfPier()
{
    return PIER_SIDE_UNKNOWN;
}

// Sanity check that guide speeds reported by mount are sensible
bool Scope::ValidGuideRates(double RAGuideRate, double DecGuideRate)
{
    double const siderealSecsPerSec = 0.9973;
    double spd;
    bool err = false;

    spd = RAGuideRate * 3600.0 / (15.0 * siderealSecsPerSec);
    if (spd > 0.1 && spd < 1.2)
    {
        spd = DecGuideRate * 3600.0 / (15.0 * siderealSecsPerSec);
        if (spd != -1.0) // RA-only tracking devices
            if (spd < 0.1 || spd > 1.2)
                err = true;
    }
    else
        err = true;
    m_CalDetailsValidated = true;

    if (err)
    {
        Debug.Write(wxString::Format("Invalid mount guide speeds: RA: %0.4f, Dec: %0.4f\n", RAGuideRate, DecGuideRate));
        return false;
    }
    else
        return true;
}

static wxString GuideSpeedSummary()
{
    // use the pointing source's guide speeds on the assumption that
    // the ASCOM/INDI reported guide rate will be the same as the ST4
    // guide rate
    Scope *scope = pPointingSource;

    double raSpeed, decSpeed;
    if (!scope->GetGuideRates(&raSpeed, &decSpeed))
    {
        return wxString::Format("RA Guide Speed = %0.1f a-s/s, Dec Guide Speed = %0.1f a-s/s", 3600.0 * raSpeed,
                                3600.0 * decSpeed);
    }
    else
        return "RA Guide Speed = Unknown, Dec Guide Speed = Unknown";
}

// untranslated settings summary
wxString Scope::GetSettingsSummary() const
{
    Calibration calInfo;
    GetLastCalibration(&calInfo);

    CalibrationDetails calDetails;
    LoadCalibrationDetails(&calDetails);

    // return a loggable summary of current mount settings
    wxString ret = Mount::GetSettingsSummary() +
        wxString::Format("Max RA duration = %d, Max DEC duration = %d, DEC guide mode = %s\n", GetMaxRaDuration(),
                         GetMaxDecDuration(), DecGuideModeStr(GetDecGuideMode()));

    ret += GuideSpeedSummary() + ", ";

    ret += wxString::Format("Cal Dec = %s, Last Cal Issue = %s, Timestamp = %s\n", DeclinationStr(calInfo.declination, "%0.1f"),
                            Mount::GetIssueString(calDetails.lastIssue), calDetails.origTimestamp);

    return ret;
}

wxString Scope::CalibrationSettingsSummary() const
{
    return wxString::Format("Calibration Step = %d ms, Calibration Distance = %d px, "
                            "Assume orthogonal axes = %s\n",
                            GetCalibrationDuration(), GetCalibrationDistance(), IsAssumeOrthogonal() ? "yes" : "no") +
        GuideSpeedSummary();
}

wxString Scope::GetMountClassName() const
{
    return wxString("scope");
}
