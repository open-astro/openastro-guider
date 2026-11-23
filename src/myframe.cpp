/*
 *  myframe.cpp
 *  PHD Guiding
 *
 *  Created by Craig Stark.
 *  Copyright (c) 2006-2010 Craig Stark.
 *  Refactored by Bret McKee
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
#include "variable_delay.h"
#include "exposure_choice.h"
#include "image_scale.h"
#include "calstep_dialog.h"

#include <algorithm>
#include <memory>

static const int DefaultNoiseReductionMethod = 0;
static const double DefaultDitherScaleFactor = 1.00;
static const bool DefaultDitherRaOnly = false;
static const DitherMode DefaultDitherMode = DITHER_RANDOM;
static const bool DefaultServerMode = true;
static const int DefaultTimelapse = 0;
static const int DefaultFocalLength = 0;
static const int DefaultExposureDuration = 1000;
static const int DefaultAutoExpMin = 1000;
static const int DefaultAutoExpMax = 5000;
static const double DefaultAutoExpSNR = 6.0;

wxDEFINE_EVENT(SET_STATUS_TEXT_EVENT, wxThreadEvent);
wxDEFINE_EVENT(ALERT_FROM_THREAD_EVENT, wxThreadEvent);
wxDEFINE_EVENT(RECONNECT_CAMERA_EVENT, wxThreadEvent);

// clang-format off
// clang-format off
wxBEGIN_EVENT_TABLE(MyFrame, wxEvtHandler)
EVT_SOCKET(SOCK_SERVER_ID, MyFrame::OnSockServerEvent)
EVT_SOCKET(SOCK_SERVER_CLIENT_ID, MyFrame::OnSockServerClientEvent)
EVT_THREAD(MYFRAME_WORKER_THREAD_EXPOSE_COMPLETE, MyFrame::OnExposeComplete)
EVT_THREAD(MYFRAME_WORKER_THREAD_MOVE_COMPLETE, MyFrame::OnMoveComplete)
EVT_THREAD(SET_STATUS_TEXT_EVENT, MyFrame::OnStatusMsg)
EVT_THREAD(ALERT_FROM_THREAD_EVENT, MyFrame::OnAlertFromThread)
EVT_THREAD(RECONNECT_CAMERA_EVENT, MyFrame::OnReconnectCameraFromThread)
wxEND_EVENT_TABLE();
// clang-format on

// The guide-step history behind get_guide_history, sized from the /graph/* keys the
// History graph used (the key names are kept so existing configs still apply).
static GuideHistory *NewGuideHistory()
{
    int cfgMinLength = pConfig->Global.GetInt("/graph/minLength", GuideHistory::DefaultMinLength);
    int cfgMaxLength = pConfig->Global.GetInt("/graph/maxLength", GuideHistory::DefaultMaxLength);
    unsigned int minLength = GuideHistory::ValidMinLength(cfgMinLength);
    unsigned int length = pConfig->Global.GetInt("/graph/length", minLength * 2);

    GuideHistory *history = new GuideHistory(cfgMinLength, cfgMaxLength, length, ::wxGetUTCTimeMillis().GetValue());

    if (history->MinLength() != (unsigned int) cfgMinLength)
        Debug.Write(
            wxString::Format("GuideHistory: /graph/minLength %d invalid, using %u\n", cfgMinLength, history->MinLength()));
    if (history->Capacity() != (unsigned int) cfgMaxLength)
        Debug.Write(
            wxString::Format("GuideHistory: /graph/maxLength %d invalid, using %u\n", cfgMaxLength, history->Capacity()));

    pConfig->Global.SetInt("/graph/minLength", history->MinLength());
    pConfig->Global.SetInt("/graph/maxLength", history->Capacity());
    Debug.Write(wxString::Format("GraphStats window size = %d\n", (int) history->Length()));

    return history;
}

// ---------------------- Main Frame -------------------------------------
// frame constructor
MyFrame::MyFrame() : wxEvtHandler(), pGuideHistory(nullptr)
{
    // #168: set the global before building anything. PhdApp::OnInit's pFrame = new MyFrame() assigns it only
    // when this constructor returns, but the guider, the dialogs and the mount built below already call
    // MyFrame members through pFrame, which were member calls on a null pointer.
    pFrame = this;

    m_frameCounter = 0;
    m_pPrimaryWorkerThread = nullptr;
    StartWorkerThread(m_pPrimaryWorkerThread);
    m_pSecondaryWorkerThread = nullptr;
    StartWorkerThread(m_pSecondaryWorkerThread);

    SocketServer = nullptr;

    bool serverMode = pConfig->Global.GetBoolean("/ServerMode", DefaultServerMode);
    SetServerMode(serverMode);

    m_sampling = 1.0;

    LoadProfileSettings();

    pGuider = new GuiderMultiStar();

    pGuider->LoadProfileSettings();

    bool sticky = pConfig->Global.GetBoolean("/StickyLockPosition", false);
    pGuider->SetLockPosIsSticky(sticky);

    pGuideHistory = NewGuideHistory();

    // no devices yet: PhdApp::OnInit loads them from the profile once pFrame is set (their constructors call through it)
    pEquipment = new EquipmentManager();

    m_starFindMode = Star::FIND_CENTROID;
    m_rawImageMode = false;
    m_rawImageModeWarningDone = false;

    UpdateTitle();
    StatusMsg(_("OpenAstro PHD2 - Alpaca and full device support"));

    if (m_serverMode)
    {
        StartServer(true);
    }

    m_continueCapturing = false;
    CaptureActive = false;
    m_exposurePending = false;
}

MyFrame::~MyFrame()
{
    // the manager stops the #104 retry (it joins a probe thread) before it deletes the devices, while the status bar
    // a disconnecting device updates is still there
    delete pEquipment;
    pEquipment = nullptr;

    delete pGuideHistory;
    pGuideHistory = nullptr;

    // #115 S5.4: the guider is no longer a child window that wx deletes after this destructor, so delete it here, at
    // the same point: Shutdown has stopped the worker threads, the #104 probe and the servers, and the devices and the
    // guide history it is used with are gone
    delete pGuider;
    pGuider = nullptr;
}

void MyFrame::UpdateTitle()
{
    int inst = wxGetApp().GetInstanceNumber();
    wxString prof = pConfig->GetCurrentProfile();

    wxString title = inst > 1 ? wxString::Format(_T("%s(#%d) %s - %s"), APPNAME, inst, FULLVER, prof)
                              : wxString::Format(_T("%s %s - %s"), APPNAME, FULLVER, prof);

    // #115 S6.9: there is no window to title
    Debug.Write(wxString::Format("UpdateTitle: %s\n", title));
}

// Get either timelapse value or state-dependent variable-exposure-delay
// #182: main thread only, where the state it reads is written; ScheduleExposure hands the result to the worker
int MyFrame::GetExposureDelay()
{
    assert(wxThread::IsMain());

    VariableDelay::FrameState state;
    state.guiding = pGuider->IsGuiding();
    state.controllerIdle = PhdController::IsIdle();
    state.recentering = pGuider->IsRecentering();
    state.guidingEnabled = pMount && pMount->GetGuidingEnabled();

    int rslt = VariableDelay::ForNextFrame(m_varDelayConfig.enabled, m_varDelayConfig.shortDelay, m_varDelayConfig.longDelay,
                                           m_timeLapse, state);

    static int s_lastExposureDelay = -1;
    if (rslt != s_lastExposureDelay)
    {
        Debug.Write(wxString::Format("Exposure delay set to %d\n", rslt));
        s_lastExposureDelay = rslt;
    }

    return rslt;
}

// the final entry is the custom value (exposure_choice.cpp)
static std::vector<int> exposure_durations = exposure_choice::DefaultDurations();

const std::vector<int>& MyFrame::GetExposureDurations() const
{
    return exposure_durations;
}

bool MyFrame::SetCustomExposureDuration(int ms)
{
    if (exposure_choice::IsOtherDuration(exposure_durations, ms))
        return true; // error, duplicate value
    if (exposure_choice::CustomChangesExposure(exposure_durations, m_exposureDuration, ms))
    {
        m_exposureDuration = ms;
        NotifyExposureChanged();
    }
    exposure_durations.back() = ms;
    pConfig->Profile.SetInt("/CustomExposureDuration", ms);
    return false;
}

void MyFrame::GetExposureInfo(int *currExpMs, bool *autoExp) const
{
    if (!pCamera || !pCamera->Connected)
    {
        *currExpMs = 0;
        *autoExp = false;
    }
    else
    {
        *currExpMs = m_exposureDuration;
        *autoExp = m_autoExp.enabled;
    }
}

bool MyFrame::SetExposureDuration(int val)
{
    // Auto for any negative value, otherwise the duration's entry (exposure_choice.cpp)
    int entry = exposure_choice::SelectionFor(exposure_durations, val);
    if (entry == -1)
        return false;
    // what selecting that entry did, with no Exposure choice to select it in (#115 S6.2)
    ApplyExposureChoice(exposure_choice::Interpret(exposure_durations, entry));
    return true;
}

bool MyFrame::SetAutoExposureCfg(int minExp, int maxExp, double targetSNR)
{
    Debug.Write(wxString::Format("AutoExp: config min = %d max = %d snr = %.2f\n", minExp, maxExp, targetSNR));

    pConfig->Profile.SetInt("/auto_exp/exposure_min", minExp);
    pConfig->Profile.SetInt("/auto_exp/exposure_max", maxExp);
    pConfig->Profile.SetDouble("/auto_exp/target_snr", targetSNR);

    bool changed = m_autoExp.minExposure != minExp || m_autoExp.maxExposure != maxExp || m_autoExp.targetSNR != targetSNR;

    m_autoExp.minExposure = minExp;
    m_autoExp.maxExposure = maxExp;
    m_autoExp.targetSNR = targetSNR;

    return changed;
}

wxString MyFrame::ExposureDurationSummary() const
{
    wxString rslt;
    if (m_autoExp.enabled)
        rslt = wxString::Format("Auto (min = %d ms, max = %d ms, SNR = %.2f)", m_autoExp.minExposure, m_autoExp.maxExposure,
                                m_autoExp.targetSNR);
    else
        rslt = wxString::Format("%d ms", m_exposureDuration);
    if (m_varDelayConfig.enabled)
        rslt += wxString::Format(", VarDelay (short = %d ms, long = %d ms)", m_varDelayConfig.shortDelay,
                                 m_varDelayConfig.longDelay);
    return rslt;
}

void MyFrame::ResetAutoExposure()
{
    if (m_autoExp.enabled)
    {
        Debug.Write(wxString::Format("AutoExp: reset exp to %d\n", m_autoExp.maxExposure));
        m_exposureDuration = m_autoExp.maxExposure;
    }
}

void MyFrame::AdjustAutoExposure(double curSNR)
{
    if (m_autoExp.enabled)
    {
        if (curSNR < 1.0)
        {
            Debug.Write(wxString::Format("AutoExp: low SNR (%.2f), reset exp to %d\n", curSNR, m_autoExp.maxExposure));
            m_exposureDuration = m_autoExp.maxExposure;
        }
        else
        {
            double r = m_autoExp.targetSNR / curSNR;
            double exp = (double) m_exposureDuration;
            // assume snr ~ sqrt(exposure)
            double newExp = exp * r * r;
            // use hysteresis to avoid overshooting
            // if our snr is below target, increase exposure rapidly (weak hysteresis, large alpha)
            // if our snr is above target, decrease exposure slowly (strong hysteresis, small alpha)
            static double const alpha_slow = .15; // low weighting for latest sample
            static double const alpha_fast = .20; // high weighting for latest sample
            double alpha = curSNR < m_autoExp.targetSNR ? alpha_fast : alpha_slow;
            exp += alpha * (newExp - exp);
            m_exposureDuration = (int) floor(exp + 0.5);
            if (m_exposureDuration < m_autoExp.minExposure)
                m_exposureDuration = m_autoExp.minExposure;
            else if (m_exposureDuration > m_autoExp.maxExposure)
                m_exposureDuration = m_autoExp.maxExposure;
            Debug.Write(wxString::Format("AutoExp: adjust SNR=%.2f new exposure %d\n", curSNR, m_exposureDuration));
        }
    }
}

static void LoadImageLoggerSettings()
{
    ImageLoggerSettings settings;

    settings.loggingEnabled = pConfig->Profile.GetBoolean("/ImageLogger/LoggingEnabled", false);
    settings.logFramesOverThreshRel = pConfig->Profile.GetBoolean("/ImageLogger/LogFramesOverThreshRel", false);
    settings.logFramesOverThreshPx = pConfig->Profile.GetBoolean("/ImageLogger/LogFramesOverThreshPx", false);
    settings.logFramesDropped = pConfig->Profile.GetBoolean("/ImageLogger/LogFramesDropped", false);
    settings.logAutoSelectFrames = pConfig->Profile.GetBoolean("/ImageLogger/LogAutoSelectFrames", false);
    settings.logNextNFrames = false;
    settings.logNextNFramesCount = 1;
    settings.guideErrorThreshRel = pConfig->Profile.GetDouble("/ImageLogger/ErrorThreshRel", 4.0);
    settings.guideErrorThreshPx = pConfig->Profile.GetDouble("/ImageLogger/ErrorThreshPx", 4.0);

    ImageLogger::ApplySettings(settings);
}

enum
{
    GAMMA_MIN = 10,
    GAMMA_MAX = 300,
    GAMMA_DEFAULT = 100,
};

void MyFrame::LoadProfileSettings()
{
    int noiseReductionMethod = pConfig->Profile.GetInt("/NoiseReductionMethod", DefaultNoiseReductionMethod);
    SetNoiseReductionMethod(noiseReductionMethod);

    double ditherScaleFactor = pConfig->Profile.GetDouble("/DitherScaleFactor", DefaultDitherScaleFactor);
    SetDitherScaleFactor(ditherScaleFactor);

    bool ditherRaOnly = pConfig->Profile.GetBoolean("/DitherRaOnly", DefaultDitherRaOnly);
    SetDitherRaOnly(ditherRaOnly);

    int ditherMode = pConfig->Profile.GetInt("/DitherMode", DefaultDitherMode);
    SetDitherMode(ditherMode == DITHER_RANDOM ? DITHER_RANDOM : DITHER_SPIRAL);

    int timeLapse = pConfig->Profile.GetInt("/frame/timeLapse", DefaultTimelapse);
    SetTimeLapse(timeLapse);

    SetVariableDelayConfig(pConfig->Profile.GetBoolean("/frame/var_delay/enabled", false),
                           pConfig->Profile.GetInt("/frame/var_delay/short_delay", 1000),
                           pConfig->Profile.GetInt("/frame/var_delay/long_delay", 10000));

    // Don't re-save the setting here with a call to SetAutoLoadCalibration().  An un-initialized registry key (-1) will
    // be populated after the 1st calibration
    int autoLoad = pConfig->Profile.GetInt("/AutoLoadCalibration", -1);
    m_autoLoadCalibration = (autoLoad == 1); // new profile=> false

    int focalLength = pConfig->Profile.GetInt("/frame/focalLength", DefaultFocalLength);
    SetFocalLength(focalLength);

    int minExp = pConfig->Profile.GetInt("/auto_exp/exposure_min", DefaultAutoExpMin);
    int maxExp = pConfig->Profile.GetInt("/auto_exp/exposure_max", DefaultAutoExpMax);
    double targetSNR = pConfig->Profile.GetDouble("/auto_exp/target_snr", DefaultAutoExpSNR);
    SetAutoExposureCfg(minExp, maxExp, targetSNR);
    // force reset of auto-exposure state
    m_autoExp.enabled = true; // SetExposureDuration below will set the actual value
    ResetAutoExposure();

    // set the custom exposure duration from the profile setting
    int customDuration = pConfig->Profile.GetInt("/CustomExposureDuration", exposure_choice::DEFAULT_CUSTOM_MS);
    *exposure_durations.rbegin() = customDuration;

    // for backward compatibity:
    // exposure duration used to be stored as the formatted string value appearing in the drop-down list,
    // but this does not work if the locale is changed
    // TODO: remove this code after a few releases (code added 5/30/2017)
    // replace with:
    // int exposureDuration = pConfig->Profile.GetInt("/ExposureDurationMs", DefaultExposureDuration);
    int exposureDuration = DefaultExposureDuration;
    if (pConfig->Profile.HasEntry("/ExposureDurationMs"))
        exposureDuration = pConfig->Profile.GetInt("/ExposureDurationMs", DefaultExposureDuration);
    else if (pConfig->Profile.HasEntry("/ExposureDuration"))
    {
        wxString s = pConfig->Profile.GetString("/ExposureDuration", wxEmptyString);
        const wxStringCharType *start = s.wx_str();
        wxStringCharType *end;
        double d = wxStrtod(start, &end);
        if (end != start)
            exposureDuration = (int) (d * 1000.0);
        else if (s == _("Auto"))
            exposureDuration = -1;
        pConfig->Profile.DeleteEntry("/ExposureDuration");
    }
    SetExposureDuration(exposureDuration);
    m_beepForLostStar = pConfig->Profile.GetBoolean("/BeepForLostStar", true);

    int val = pConfig->Profile.GetInt("/Gamma", GAMMA_DEFAULT);
    if (val < GAMMA_MIN)
        val = GAMMA_MIN;
    if (val > GAMMA_MAX)
        val = GAMMA_MAX;
    Stretch_gamma = (double) val / 100.0;

    LoadImageLoggerSettings();
}

void MyFrame::UpdateButtonsStatus()
{
    assert(wxThread::IsMain());

    // #115 S6.5: there are no toolbar buttons or menu items to enable any more. This still drives a headless
    // (JSON-RPC) Guiding Assistant run, which has no window (H5).
    GuidingAssistant::UpdateUIControls();
}

struct alert_params
{
    wxString msg;
    int flags;
};

// An alert is a Debug line and the Alert event, in that order (#115 S6.3). The info bar that showed it, with its
// 'Don't show', custom-action, help and close buttons, is gone, so the alert_fn callbacks are never invoked.
void MyFrame::DoAlert(const alert_params& params)
{
    Debug.Write(wxString::Format("Alert: %s\n", params.msg));
    EvtServer.NotifyAlert(params.msg, params.flags);
}

// The button arguments are kept for the callers; with no info bar there are no buttons to offer them on
void MyFrame::Alert(const wxString& msg, alert_fn * /*DontShowFn*/, const wxString& /*buttonLabel*/, alert_fn * /*SpecialFn*/,
                    intptr_t /*arg*/, bool /*showHelpButton*/, int flags)
{
    if (wxThread::IsMain())
    {
        alert_params params;
        params.msg = msg;
        params.flags = flags;
        DoAlert(params);
    }
    else
    {
        alert_params *params = new alert_params;
        params->msg = msg;
        params->flags = flags;
        wxThreadEvent *event = new wxThreadEvent(wxEVT_THREAD, ALERT_FROM_THREAD_EVENT);
        event->SetExtraLong((long) params);
        wxQueueEvent(this, event);
    }
}

// Standardized version for building an alert that has the "don't show again" option button.  Insures that debug log entry is
// made if the user has blocked the alert for this type of problem
void MyFrame::SuppressibleAlert(const wxString& configPropKey, const wxString& msg, alert_fn *dontShowFn, intptr_t arg,
                                bool showHelpButton, int flags)
{
    if (pConfig->Global.GetBoolean(configPropKey, true))
    {
        Alert(msg, dontShowFn, wxEmptyString, 0, arg, showHelpButton);
    }
    else
        Debug.Write(wxString::Format("Suppressed alert:  %s\n", msg));
}

void MyFrame::Alert(const wxString& msg, int flags)
{
    Alert(msg, 0, wxEmptyString, 0, 0, false, flags);
}

void MyFrame::OnAlertFromThread(wxThreadEvent& event)
{
    alert_params *params = (alert_params *) event.GetExtraLong();
    DoAlert(*params);
    delete params;
}

void MyFrame::OnReconnectCameraFromThread(wxThreadEvent& event)
{
    DoTryReconnect();
}

void MyFrame::TryReconnect()
{
    if (wxThread::IsMain())
        DoTryReconnect();
    else
    {
        Debug.Write("worker thread queueing reconnect event to GUI thread\n");
        wxThreadEvent *event = new wxThreadEvent(wxEVT_THREAD, RECONNECT_CAMERA_EVENT);
        wxQueueEvent(this, event);
    }
}

void MyFrame::DoTryReconnect()
{
    if (DarkBuildActive())
    {
        // #100: a dark frame is being captured on its own thread, and a capture error inside it (the capture
        // timeout, or an Alpaca camera the server reports as not connected or that keeps failing acquisitions,
        // #103) is what queued this reconnect; the capture thread is still unwinding through the camera object.
        // The build fails on that frame, and runs the reconnect once its capture has returned.
        Debug.Write("camera reconnect deferred: dark-frame capture in progress\n");
        DeferCameraReconnectUntilDarkBuildEnds();
        return;
    }

    // The worker thread skipped the exposure-complete notification of the frame that failed
    // (InitiateReconnect), so that exposure is still pending and this finishes it. A reconnect
    // deferred past a dark build (#100), or raised by a capture outside the worker thread, has
    // no exposure pending: it only sends the events, and must not start or stop the capture loop.
    bool exposurePending = m_exposurePending;

    // The camera the last reconnect brought back failed again before it delivered a frame: the server read it
    // as connected while it was not (AlpacaBridge does, for an ASI camera unbound from USB). That reconnect
    // failed, and reconnecting again at once would fail the same way, so the #104 retry takes over.
    if (m_cameraReconnectUnconfirmed)
    {
        m_cameraReconnectUnconfirmed = false;
        const wxString reason(_("the camera failed its first frame after reconnecting"));
        Debug.Write("Camera Re-connect not confirmed: " + reason + "\n");
        EvtServer.NotifyEquipmentReconnectFailed("camera", m_cameraReconnectThrottle.Attempts(), reason);
        if (exposurePending)
            OnExposeComplete(0, true);
        pEquipment->NoteCameraReconnectFailed(reason);
        return;
    }

    // do not reconnect more than 3 times in 1 minute (monotonic_timers.cpp, #226)
    enum
    {
        TIME_WINDOW = monotonic_timers::ReconnectThrottle::TIME_WINDOW_S,
        MAX_ATTEMPTS = monotonic_timers::ReconnectThrottle::MAX_ATTEMPTS
    };
    int64_t now = monotonic_timers::ClockS();
    Debug.Write(wxString::Format("Try camera reconnect, now = %lld\n", (long long) now));
    if (!m_cameraReconnectThrottle.TryAttempt(now))
    {
        Debug.Write(wxString::Format("More than %d camera reconnect attempts in less than %d seconds, "
                                     "return without reconnect.\n",
                                     MAX_ATTEMPTS, TIME_WINDOW));
        // Terminal signal so clients waiting on EquipmentDisconnected{reconnecting:true}
        // don't hang forever on a permanent unplug (they get no EquipmentReconnected).
        EvtServer.NotifyEquipmentReconnectFailed(
            "camera", m_cameraReconnectThrottle.Attempts(),
            wxString::Format("auto-reconnect throttle exhausted (%d attempts in %d s)", MAX_ATTEMPTS, TIME_WINDOW));
        if (exposurePending)
            OnExposeComplete(0, true);
        // #104: the headless daemon keeps retrying slowly; EquipmentReconnected follows when the camera is back
        pEquipment->NoteCameraReconnectFailed(
            wxString::Format("auto-reconnect throttle exhausted (%d attempts in %d s)", MAX_ATTEMPTS, TIME_WINDOW));
        return;
    }
    bool err = pEquipment->ReconnectCamera();
    if (err)
    {
        Debug.Write("Camera Re-connect failed\n");
        // Terminal too: EquipmentDisconnected{reconnecting:true} is always followed by
        // EquipmentReconnected or EquipmentReconnectFailed.
        EvtServer.NotifyEquipmentReconnectFailed("camera", m_cameraReconnectThrottle.Attempts(),
                                                 "the camera did not reconnect");
        // complete the pending exposure notification
        if (exposurePending)
            OnExposeComplete(0, true);
        // #104: the headless daemon keeps retrying slowly, with the reason the reconnect failed
        pEquipment->NoteCameraReconnectFailed(wxEmptyString);
    }
    else
    {
        Debug.Write(exposurePending ? "Camera Re-connect succeeded, resume exposures\n"
                                    : "Camera Re-connect succeeded, no exposure pending\n");
        // ARA integration (PHD2-GAP gap 2): the recovered twin of
        // EquipmentDisconnected, so clients can clear their fault state. During a capture the server's word that
        // the camera is connected is not enough: the next frame sends it once it arrives (OnExposeComplete).
        // With no capture going on there is no frame to wait for.
        if (exposurePending)
        {
            Debug.Write("Camera Re-connect: EquipmentReconnected waits for the next frame\n");
            m_cameraReconnectUnconfirmed = true;
        }
        else
            EvtServer.NotifyEquipmentReconnected("camera");
        UpdateStatusBarStateLabels();
        if (exposurePending)
        {
            m_exposurePending = false; // exposure no longer pending
            // #116, #136: the reconnected camera starts a new count of 1280s (Connect resets the #103 tracker), so
            // a calibration or guiding frame retried before the reconnect gets its full retry budget again, once
            // per run of failed frames: the throttle above does not bound a camera that fails every frame when its
            // 3 failed frames take longer than 20 s, so without the limit the calibration or guiding would never end.
            m_frameRetries.OnReconnect();
            ScheduleExposure();
        }
    }
}

/*
 * The base class wxFrame::StatusMsg() is not
 * safe to call from worker threads.
 *
 * For non-main threads this routine queues the request
 * to the frames event queue, and it gets displayed by the main
 * thread as part of event processing.
 *
 */

// There is no status bar since #115 step 2; a status message is only written to the debug log
static void SetStatusMsg(const wxString& text)
{
    Debug.Write(wxString::Format("Status Line: %s\n", text));
}

enum StatusBarThreadMsgType
{
    THR_SB_MSG_TEXT,
    THR_SB_BUTTON_STATE,
};

static void QueueStatusBarTextMsg(wxEvtHandler *frame, const wxString& text)
{
    wxThreadEvent *event = new wxThreadEvent(wxEVT_THREAD, SET_STATUS_TEXT_EVENT);
    event->SetExtraLong(THR_SB_MSG_TEXT);
    event->SetString(text);
    wxQueueEvent(frame, event);
}

static void QueueStatusBarUpdateMsg(wxEvtHandler *frame, StatusBarThreadMsgType type)
{
    wxThreadEvent *event = new wxThreadEvent(wxEVT_THREAD, SET_STATUS_TEXT_EVENT);
    event->SetExtraLong(type);
    wxQueueEvent(frame, event);
}

void MyFrame::StatusMsg(const wxString& text)
{
    if (wxThread::IsMain())
        SetStatusMsg(text);
    else
        QueueStatusBarTextMsg(this, text);
}

void MyFrame::StatusMsgNoTimeout(const wxString& text)
{
    if (wxThread::IsMain())
        SetStatusMsg(text);
    else
        QueueStatusBarTextMsg(this, text);
}

void MyFrame::OnStatusMsg(wxThreadEvent& event)
{
    switch (event.GetExtraLong())
    {
    case THR_SB_MSG_TEXT:
        SetStatusMsg(event.GetString());
        break;
    case THR_SB_BUTTON_STATE:
        UpdateButtonsStatus();
        break;
    }
}

void MyFrame::UpdateStatusBarStarInfo(double, bool)
{
    // no status bar since #115 step 2; MyFrame goes in step 6
}

void MyFrame::UpdateStatusBarStateLabels()
{
    // no status bar since #115 step 2; MyFrame goes in step 6
}

void MyFrame::UpdateStatusBarCalibrationStatus()
{
    // no status bar since #115 step 2; MyFrame goes in step 6
}

void MyFrame::NotifyUpdateButtonsStatus()
{
    if (wxThread::IsMain())
    {
        UpdateButtonsStatus();
    }
    else
    {
        QueueStatusBarUpdateMsg(this, THR_SB_BUTTON_STATE);
    }
}

void MyFrame::UpdateStatusBarGuiderInfo(const GuideStepInfo& info)
{
    Debug.Write(wxString::Format("GuideStep: %.1f px %d ms %s, %.1f px %d ms %s\n", info.mountOffset.X, info.durationRA,
                                 info.directionRA == EAST ? "EAST" : "WEST", info.mountOffset.Y, info.durationDec,
                                 info.directionDec == NORTH ? "NORTH" : "SOUTH"));
}

void MyFrame::ClearStatusBarGuiderInfo()
{
    // no status bar since #115 step 2; MyFrame goes in step 6
}

bool MyFrame::StartWorkerThread(WorkerThread *& pWorkerThread)
{
    bool bError = false;
    wxCriticalSectionLocker lock(m_CSpWorkerThread);

    try
    {
        Debug.Write(wxString::Format("StartWorkerThread(%p) begins\n", pWorkerThread));

        if (!pWorkerThread || !pWorkerThread->IsRunning())
        {
            delete pWorkerThread;
            pWorkerThread = new WorkerThread(this);

            if (pWorkerThread->Create() != wxTHREAD_NO_ERROR)
            {
                throw ERROR_INFO("Could not Create() the worker thread!");
            }

            if (pWorkerThread->Run() != wxTHREAD_NO_ERROR)
            {
                throw ERROR_INFO("Could not Run() the worker thread!");
            }
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        delete pWorkerThread;
        pWorkerThread = nullptr;
        bError = true;
    }

    Debug.Write(wxString::Format("StartWorkerThread(%p) ends\n", pWorkerThread));

    return bError;
}

bool MyFrame::StopWorkerThread(WorkerThread *& pWorkerThread)
{
    bool killed = false;

    wxCriticalSectionLocker lock(m_CSpWorkerThread);

    Debug.Write(wxString::Format("StopWorkerThread(0x%p) begins\n", pWorkerThread));

    if (pWorkerThread && pWorkerThread->IsRunning())
    {
        pWorkerThread->EnqueueWorkerThreadTerminateRequest();

        enum
        {
            TIMEOUT_MS = 1000
        };
        wxStopWatch swatch;
        while (pWorkerThread->IsAlive() && swatch.Time() < TIMEOUT_MS)
            wxGetApp().Yield();

        if (pWorkerThread->IsAlive())
        {
            while (pWorkerThread->IsAlive() && !pWorkerThread->IsKillable())
            {
                Debug.Write(wxString::Format("Worker thread 0x%p is not killable, waiting...\n", pWorkerThread));
                wxStopWatch swatch2;
                while (pWorkerThread->IsAlive() && !pWorkerThread->IsKillable() && swatch2.Time() < TIMEOUT_MS)
                    wxGetApp().Yield();
            }
            if (pWorkerThread->IsAlive())
            {
                Debug.Write(wxString::Format("StopWorkerThread(0x%p) thread did not terminate, force kill\n", pWorkerThread));
                pWorkerThread->Kill();
                killed = true;
            }
        }
        else
        {
            wxThread::ExitCode threadExitCode = pWorkerThread->Wait();
            Debug.Write(wxString::Format("StopWorkerThread() threadExitCode=%d\n", threadExitCode));
        }
    }

    Debug.Write(wxString::Format("StopWorkerThread(0x%p) ends\n", pWorkerThread));

    delete pWorkerThread;
    pWorkerThread = nullptr;

    return killed;
}

void MyFrame::ScheduleExposure()
{
    CaptureParams captureParams;
    captureParams.duration = RequestedExposureDuration();
    captureParams.subframe = m_singleExposure.enabled ? m_singleExposure.subframe : pGuider->GetBoundingBox();
    captureParams.hwBinning = pCamera->HwBinning;
    captureParams.swBinning = pCamera->SwBinning;
    captureParams.bpp = pCamera->BitsPerPixel();
    captureParams.limitFrame = pCamera->LimitFrame;
    captureParams.gain = pCamera->GuideCameraGain;
    captureParams.captureOptions = GetRawImageMode() ? CAPTURE_BPM_REVIEW : CAPTURE_LIGHT;

    Debug.Write(wxString::Format("ScheduleExposure(%d,%x,%d) exposurePending=%d\n", captureParams.duration,
                                 captureParams.captureOptions, !captureParams.subframe.IsEmpty(), m_exposurePending));

    assert(wxThread::IsMain()); // m_exposurePending only updated in main thread
    assert(!m_exposurePending);

    m_exposurePending = true;

    // #97: this exposure must not report the reason an earlier one failed
    if (pCamera)
        pCamera->ClearCaptureFailure();

    // #182: decided here, after the previous frame updated the guide and controller state, not on the worker
    int delayMs = GetExposureDelay();
    // #210: likewise the noise reduction, which set_noise_reduction writes on this thread
    NOISE_REDUCTION_METHOD noiseReduction = GetNoiseReductionMethod();

    usImage *img = new usImage();

    wxCriticalSectionLocker lock(m_CSpWorkerThread);

    if (m_pPrimaryWorkerThread) // can be null when app is shutting down (unlikely but possible)
        m_pPrimaryWorkerThread->EnqueueWorkerThreadExposeRequest(img, captureParams, delayMs, noiseReduction);
}

void MyFrame::SchedulePrimaryMove(Mount *mount, const GuiderOffset& ofs, unsigned int moveOptions)
{
    Debug.Write(wxString::Format("SchedulePrimaryMove(%p, x=%.2f, y=%.2f, opts=%u)\n", mount, ofs.cameraOfs.X, ofs.cameraOfs.Y,
                                 moveOptions));

    wxCriticalSectionLocker lock(m_CSpWorkerThread);

    assert(mount);

    // #137: null once shutdown has stopped the thread; StopWorkerThread pumps the event loop, so an RPC can get here
    if (!m_pPrimaryWorkerThread)
    {
        Debug.Write("SchedulePrimaryMove: no worker thread (shutting down), move dropped\n");
        return;
    }

    // Manual moves do not affect the request count for IsBusy()
    if ((moveOptions & MOVEOPT_MANUAL) == 0)
        mount->IncrementRequestCount();

    m_pPrimaryWorkerThread->EnqueueWorkerThreadMoveRequest(mount, ofs, moveOptions);
}

void MyFrame::ScheduleSecondaryMove(Mount *mount, const GuiderOffset& ofs, unsigned int moveOptions)
{
    Debug.Write(wxString::Format("ScheduleSecondaryMove(%p, x=%.2f, y=%.2f, opts=%u)\n", mount, ofs.cameraOfs.X,
                                 ofs.cameraOfs.Y, moveOptions));

    wxCriticalSectionLocker lock(m_CSpWorkerThread);

    assert(mount);

    if (mount->SynchronousOnly())
    {
        // some mounts must run on the Primary thread even if the secondary is requested
        // to ensure synchronous ST4 guide / camera exposure
        SchedulePrimaryMove(mount, ofs, moveOptions);
    }
    else
    {
        // #137: null once shutdown has stopped the thread
        if (!m_pSecondaryWorkerThread)
        {
            Debug.Write("ScheduleSecondaryMove: no worker thread (shutting down), move dropped\n");
            return;
        }

        if ((moveOptions & MOVEOPT_MANUAL) == 0)
            mount->IncrementRequestCount();

        m_pSecondaryWorkerThread->EnqueueWorkerThreadMoveRequest(mount, ofs, moveOptions);
    }
}

void MyFrame::ScheduleAxisMove(Mount *mount, const GUIDE_DIRECTION direction, int duration, unsigned int moveOptions)
{
    wxCriticalSectionLocker lock(m_CSpWorkerThread);

    assert(mount);

    // #137: null once shutdown has stopped the thread; StopWorkerThread pumps the event loop, so an RPC can get here
    if (!m_pPrimaryWorkerThread)
    {
        Debug.Write("ScheduleAxisMove: no worker thread (shutting down), move dropped\n");
        return;
    }

    if ((moveOptions & MOVEOPT_MANUAL) == 0)
        mount->IncrementRequestCount();

    m_pPrimaryWorkerThread->EnqueueWorkerThreadAxisMove(mount, direction, duration, moveOptions);
}

void MyFrame::ScheduleManualMove(Mount *mount, const GUIDE_DIRECTION direction, int duration)
{
    GuideLog.NotifyManualGuide(mount, direction, duration);

    GuideStepInfo step = { 0 };
    step.mount = mount;
    step.moveOptions = MOVEOPT_MANUAL;
    step.mountOffset.SetXY(0., 0.);
    switch (direction)
    {
    case GUIDE_DIRECTION::NORTH:
    case GUIDE_DIRECTION::SOUTH:
        step.durationDec = duration;
        step.directionDec = direction;
        break;
    case GUIDE_DIRECTION::EAST:
    case GUIDE_DIRECTION::WEST:
        step.durationRA = duration;
        step.directionRA = direction;
        break;
    default:
        break;
    }
    UpdateStatusBarGuiderInfo(step);

    ScheduleAxisMove(mount, direction, duration, MOVEOPT_MANUAL);
}

bool MyFrame::StartSingleExposure(int duration, wxByte binning, int gain, const wxRect& subframe, bool save,
                                  const wxString& path)
{
    Debug.Write(wxString::Format("StartSingleExposure duration=%d binning=%d gain=%d save=%d path=[%s]\n", duration, binning,
                                 gain, save, path));

    if (!pCamera || !pCamera->Connected)
    {
        Debug.Write("StartSingleExposure: camera not connected\n");
        return true;
    }

    StatusMsgNoTimeout(_("Capturing single exposure"));

    if (!m_singleExposure.Activate(duration, binning, gain, subframe, save, path))
    {
        Debug.Write("StartSingleExposure: invalid exposure parameters\n");
        return true;
    }

    StartCapturing();

    return false;
}

bool MyFrame::AutoSelectStar(const wxRect& roi, bool willForceRecal)
{
    if (pGuider->IsCalibratingOrGuiding())
    {
        Debug.Write("cannot auto-select star while calibrating or guiding\n");
        return true; // error
    }

    return pGuider->AutoSelect(roi, willForceRecal);
}

void MyFrame::StartCapturing()
{
    Debug.Write(wxString::Format("StartCapturing CaptureActive=%d continueCapturing=%d exposurePending=%d\n", CaptureActive,
                                 m_continueCapturing, m_exposurePending));

    if (!CaptureActive)
    {
        if (!m_singleExposure.enabled)
            m_continueCapturing = true;

        CaptureActive = true;
        m_frameCounter = 0;

        CheckDarkFrameGeometry();
        UpdateButtonsStatus();

        // m_exposurePending should always be false here since CaptureActive is cleared on exposure
        // completion, but be paranoid and check it anyway

        if (!m_exposurePending)
        {
            pCamera->InitCapture();
            ScheduleExposure();
        }
    }
}

bool MyFrame::StopCapturing()
{
    Debug.Write(wxString::Format("StopCapturing CaptureActive=%d continueCapturing=%d exposurePending=%d\n", CaptureActive,
                                 m_continueCapturing, m_exposurePending));

    bool finished = true;
    bool continueCapturing = m_continueCapturing;

    if (pGuider->IsPaused())
    {
        // setting m_continueCapturing to false before calling
        // SetPaused(PAUSE_NONE) ensures that SetPaused(PAUSE_NONE)
        // does not schedule another exposure
        m_continueCapturing = false;
        SetPaused(PAUSE_NONE);
    }

    if (continueCapturing || m_exposurePending)
    {
        StatusMsgNoTimeout(_("Waiting for devices..."));
        m_continueCapturing = false;

        if (m_exposurePending)
        {
            m_pPrimaryWorkerThread->RequestStop();
            finished = false;
        }
        else
        {
            CaptureActive = false;
            if (pGuider->IsCalibratingOrGuiding())
            {
                pGuider->StopGuiding();
                pGuider->UpdateImageDisplay();
            }
            FinishStop();
        }
    }

    return finished;
}

void MyFrame::SetPaused(PauseType pause)
{
    bool const isPaused = pGuider->IsPaused();

    Debug.Write(wxString::Format("SetPaused type=%d isPaused=%d exposurePending=%d\n", pause, isPaused, m_exposurePending));

    if (pause != PAUSE_NONE && !isPaused)
    {
        pGuider->SetPaused(pause);
        StatusMsgNoTimeout(_("Paused") + (pause == PAUSE_FULL ? _("/full") : _("/looping")));
        GuideLog.ServerCommand(pGuider, "PAUSE");
        EvtServer.NotifyPaused();
    }
    else if (pause == PAUSE_NONE && isPaused)
    {
        pGuider->SetPaused(PAUSE_NONE);
        if (pMount)
        {
            Debug.Write("un-pause: clearing mount guide algorithm history\n");
            pMount->NotifyGuidingResumed();
        }
        if (m_continueCapturing && !m_exposurePending)
            ScheduleExposure();
        StatusMsg(_("Resumed"));
        GuideLog.ServerCommand(pGuider, "RESUME");
        EvtServer.NotifyResumed();
    }
}

bool MyFrame::StartLooping()
{
    bool error = false;

    try
    {
        if (!pCamera || !pCamera->Connected)
        {
            throw ERROR_INFO("Camera not connected");
        }

        if (CaptureActive)
        {
            // if we are guiding, stop guiding and go back to looping
            if (pGuider->IsCalibratingOrGuiding())
            {
                pGuider->StopGuiding();
            }
            else
            {
                // already looping, nothing to do
                return false;
            }
        }

        StatusMsg(_("Looping"));
        StartCapturing();
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        error = true;
    }

    return error;
}

bool MyFrame::StartGuiding()
{
    bool error = true;

    wxGetApp().CheckLogRollover();

    if (pMount && pMount->IsConnected() && pCamera && pCamera->Connected && pGuider->GetState() >= STATE_SELECTED)
    {
        pGuider->StartGuiding();
        StartCapturing();
        UpdateButtonsStatus();
        // reset dither state when guiding starts
        m_ditherSpiral.Reset();
        error = false;
    }

    return error;
}

void DitherSpiral::Reset()
{
    Debug.Write("reset dither spiral\n");
    x = y = 0;
    dx = -1;
    dy = 0;
    prevRaOnly = false;
}

inline static void ROT(int& dx, int& dy)
{
    int t = -dx;
    dx = dy;
    dy = t;
}

void DitherSpiral::GetDither(double amount, bool raOnly, double *dRa, double *dDec)
{
    // reset state when switching between ra only and ra/dec
    if (raOnly != prevRaOnly)
    {
        Reset();
        prevRaOnly = raOnly;
    }

    if (raOnly)
    {
        // x = 0,1,-1,-2,2,3,-3,-4,4,5,...
        ROT(dx, dy);
        int x0 = x;
        if (dy == 0)
            x = -x;
        else
            x += dy;

        *dRa = (double) (x - x0) * amount;
        *dDec = 0.0;
    }
    else
    {
        if (x == y || (x > 0 && x == -y) || (x <= 0 && y == 1 - x))
            ROT(dx, dy);

        x += dx;
        y += dy;

        *dRa = (double) dx * amount;
        *dDec = (double) dy * amount;
    }
}

void MyFrame::SetDitherMode(DitherMode mode)
{
    Debug.Write(wxString::Format("set dither mode %d\n", mode));
    m_ditherMode = mode;
    pConfig->Profile.SetInt("/DitherMode", mode);
}

bool MyFrame::Dither(double amount, bool raOnly)
{
    bool error = false;

    try
    {
        if (!pGuider->IsGuiding())
        {
            throw ERROR_INFO("cannot dither if not guiding");
        }

        amount *= m_ditherScaleFactor;

        double dRa = 0.0;
        double dDec = 0.0;

        if (m_ditherMode == DITHER_SPIRAL)
        {
            m_ditherSpiral.GetDither(amount, raOnly, &dRa, &dDec);
        }
        else
        {
            // DITHER_RANDOM; #173: its own generator, so a start does not repeat the last start's dithers
            m_ditherRandom.GetDither(amount, raOnly, &dRa, &dDec);
        }

        Debug.Write(wxString::Format("dither: size=%.2f, dRA=%.2f dDec=%.2f\n", amount, dRa, dDec));

        bool err = pGuider->MoveLockPosition(PHD_Point(dRa, dDec));
        if (err)
        {
            throw ERROR_INFO("move lock failed");
        }

        StatusMsg(wxString::Format(_("Dither by %.2f,%.2f"), dRa, dDec));
        GuideLog.NotifyGuidingDithered(pGuider, dRa, dDec);
        EvtServer.NotifyGuidingDithered(dRa, dDec);
        DitherInfo info;
        info.timestamp = ::wxGetUTCTimeMillis().GetValue();
        info.dRa = dRa;
        info.dDec = dDec;
        pGuideHistory->AppendData(info);

        if (pMount->IsStepGuider())
        {
            StepGuider *ao = static_cast<StepGuider *>(pMount);
            if (ao->GetBumpOnDither())
            {
                Debug.Write("Dither: starting AO bump\n");
                ao->ForceStartBump();
            }
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        error = true;
    }

    return error;
}

bool MyFrame::GuidingRAOnly()
{
    const Scope *const scope = TheScope();
    return scope && scope->GetDecGuideMode() == DEC_NONE;
}

double MyFrame::CurrentGuideError() const
{
    return pGuider->CurrentError(GuidingRAOnly());
}

double MyFrame::CurrentGuideErrorSmoothed() const
{
    return pGuider->CurrentErrorSmoothed(GuidingRAOnly());
}

// #115 S6.8: the exit sequence MyFrame::OnClose ran on every exit, in the same order. The `shutdown` RPC, SIGTERM,
// SIGINT and a restart reach it from IdleClosing (phd.cpp), which then deletes the frame and exits the main loop.
void MyFrame::Shutdown()
{
    // once only: a signal and the `shutdown` RPC can both reach IdleClosing
    if (m_shutDown)
    {
        Debug.Write("MyFrame::Shutdown: already shut down\n");
        return;
    }
    m_shutDown = true;

    Debug.Write("MyFrame::OnClose proceeding\n");

    // #104: before StopWorkerThread pumps the event loop below, so no background connect attempt starts (and could
    // connect equipment that is about to be disconnected) while the app winds down; waits for a probe in flight
    pEquipment->StopConnectRetry(true);

    // a headless Guiding Assistant run can't finish normally any more; put back the settings it changed
    GuidingAssistant::ApiShutdown();

    StopCapturing();

    bool killed = StopWorkerThread(m_pPrimaryWorkerThread);
    if (StopWorkerThread(m_pSecondaryWorkerThread))
        killed = true;

    // disconnect all gear
    pEquipment->Shutdown(killed);

    // stop the socket server and event server
    StartServer(false);

    GuideLog.CloseGuideLog();
}

bool MyFrame::SetNoiseReductionMethod(int noiseReductionMethod)
{
    bool bError = false;

    try
    {
        switch (noiseReductionMethod)
        {
        case NR_NONE:
        case NR_2x2MEAN:
        case NR_3x3MEDIAN:
            break;
        default:
            throw ERROR_INFO("invalid noiseReductionMethod");
        }
        m_noiseReductionMethod = (NOISE_REDUCTION_METHOD) noiseReductionMethod;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);

        bError = true;
        m_noiseReductionMethod = (NOISE_REDUCTION_METHOD) DefaultNoiseReductionMethod;
    }

    pConfig->Profile.SetInt("/NoiseReductionMethod", m_noiseReductionMethod);

    return bError;
}

bool MyFrame::SetDitherScaleFactor(double ditherScaleFactor)
{
    bool bError = false;

    try
    {
        if (ditherScaleFactor <= 0)
        {
            throw ERROR_INFO("ditherScaleFactor <= 0");
        }
        m_ditherScaleFactor = ditherScaleFactor;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_ditherScaleFactor = DefaultDitherScaleFactor;
    }

    pConfig->Profile.SetDouble("/DitherScaleFactor", m_ditherScaleFactor);

    return bError;
}

bool MyFrame::SetDitherRaOnly(bool ditherRaOnly)
{
    bool bError = false;

    m_ditherRaOnly = ditherRaOnly;

    pConfig->Profile.SetBoolean("/DitherRaOnly", m_ditherRaOnly);

    return bError;
}

void MyFrame::NotifyGuidingStarted()
{
    StatusMsg(_("Guiding"));

    m_guidingStarted = wxDateTime::UNow();
    m_guidingElapsed.Start();
    m_frameCounter = 0;

    if (pMount)
        pMount->NotifyGuidingStarted();
    if (pSecondaryMount)
        pSecondaryMount->NotifyGuidingStarted();

    GuideLog.GuidingStarted();
    EvtServer.NotifyGuidingStarted();
}

void MyFrame::NotifyGuidingStopped()
{
    assert(!pMount || !pMount->IsBusy());
    assert(!pSecondaryMount || !pSecondaryMount->IsBusy());

    if (pMount)
        pMount->NotifyGuidingStopped();
    if (pSecondaryMount)
        pSecondaryMount->NotifyGuidingStopped();

    EvtServer.NotifyGuidingStopped();
    GuideLog.GuidingStopped();
    PhdController::AbortController(m_captureFailureStopReason.empty() ? wxString("Guiding stopped")
                                                                      : m_captureFailureStopReason);
}

void MyFrame::SetAutoLoadCalibration(bool val)
{
    if (m_autoLoadCalibration != val)
    {
        m_autoLoadCalibration = val;
        pConfig->Profile.SetBoolean("/AutoLoadCalibration", m_autoLoadCalibration);
    }
}

inline static GuideParity guide_parity(int p)
{
    switch (p)
    {
    case GUIDE_PARITY_EVEN:
        return GUIDE_PARITY_EVEN;
    case GUIDE_PARITY_ODD:
        return GUIDE_PARITY_ODD;
    default:
        return GUIDE_PARITY_UNKNOWN;
    }
}

static void load_calibration(Mount *mnt)
{
    wxString prefix = "/" + mnt->GetMountClassName() + "/calibration/";
    if (!pConfig->Profile.HasEntry(prefix + "timestamp"))
        return;

    Calibration cal;
    cal.xRate = pConfig->Profile.GetDouble(prefix + "xRate", 1.0);
    cal.yRate = pConfig->Profile.GetDouble(prefix + "yRate", 1.0);
    cal.binning = (unsigned short) pConfig->Profile.GetInt(prefix + "binning", 1);
    cal.xAngle = pConfig->Profile.GetDouble(prefix + "xAngle", 0.0);
    cal.yAngle = pConfig->Profile.GetDouble(prefix + "yAngle", M_PI / 2.0);
    cal.declination = pConfig->Profile.GetDouble(prefix + "declination", 0.0);
    int t = pConfig->Profile.GetInt(prefix + "pierSide", PIER_SIDE_UNKNOWN);
    cal.pierSide = t == PIER_SIDE_EAST ? PIER_SIDE_EAST : t == PIER_SIDE_WEST ? PIER_SIDE_WEST : PIER_SIDE_UNKNOWN;
    cal.raGuideParity = guide_parity(pConfig->Profile.GetInt(prefix + "raGuideParity", GUIDE_PARITY_UNKNOWN));
    cal.decGuideParity = guide_parity(pConfig->Profile.GetInt(prefix + "decGuideParity", GUIDE_PARITY_UNKNOWN));
    cal.rotatorAngle = pConfig->Profile.GetDouble(prefix + "rotatorAngle", Rotator::POSITION_UNKNOWN);
    cal.isValid = true;

    mnt->SetCalibration(cal);
}

void MyFrame::LoadCalibration()
{
    if (pMount)
    {
        load_calibration(pMount);
    }
    if (pSecondaryMount)
    {
        load_calibration(pSecondaryMount);
    }
}

static bool save_multi_darks(const ExposureImgMap& darks, const wxString& fname, const wxString& note)
{
    bool bError = false;

    try
    {
        fitsfile *fptr; // FITS file pointer
        int status = 0; // CFITSIO status value MUST be initialized to zero!

        PHD_fits_create_file(&fptr, fname, true, &status);
        if (status)
            throw ERROR_INFO("fits_create_file failed");

        for (ExposureImgMap::const_iterator it = darks.begin(); it != darks.end(); ++it)
        {
            const usImage *const img = it->second;
            long fsize[] = {
                (long) img->Size.GetWidth(),
                (long) img->Size.GetHeight(),
            };
            if (!status)
                fits_create_img(fptr, USHORT_IMG, 2, fsize, &status);

            if (!status)
            {
                char *keyname = const_cast<char *>("EXPOSURE");
                char *comment = const_cast<char *>("Exposure time in seconds");
                float exposure = (float) img->ImgExpDur / 1000.0f;
                fits_write_key(fptr, TFLOAT, keyname, &exposure, comment, &status);
            }

            if (!status)
            {
                char *keyname = const_cast<char *>("XBINNING");
                char *comment = const_cast<char *>("Camera X Bin");
                int binning = img->Binning;
                fits_write_key(fptr, TINT, keyname, &binning, comment, &status);
            }

            if (!status)
            {
                char *keyname = const_cast<char *>("YBINNING");
                char *comment = const_cast<char *>("Camera Y Bin");
                int binning = img->Binning;
                fits_write_key(fptr, TINT, keyname, &binning, comment, &status);
            }

            if (!status)
            {
                char *keyname = const_cast<char *>("SATURATE");
                char *comment = const_cast<char *>("Data value at which saturation occurs");
                int saturate = (1U << img->BitsPerPixel) - 1;
                fits_write_key(fptr, TINT, keyname, &saturate, comment, &status);
            }

            if (!status)
            {
                char *keyname = const_cast<char *>("GAIN");
                char *comment = const_cast<char *>("PHD Gain Value (0-100)");
                int gain = img->Gain;
                fits_write_key(fptr, TINT, keyname, &gain, comment, &status);
            }

            if (!note.IsEmpty())
            {
                char *USERNOTE = const_cast<char *>("USERNOTE");
                if (!status)
                    fits_write_key(fptr, TSTRING, USERNOTE, note.char_str(), nullptr, &status);
            }

            if (!status)
            {
                long fpixel[3] = { 1, 1, 1 };
                fits_write_pix(fptr, TUSHORT, fpixel, img->NPixels, img->ImageData, &status);
            }

            Debug.Write(wxString::Format("saving dark frame exposure = %d\n", img->ImgExpDur));
        }

        PHD_fits_close_file(fptr);
        bError = status ? true : false;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    return bError;
}

static bool load_multi_darks(GuideCamera *camera, const wxString& fname)
{
    bool bError = false;
    fitsfile *fptr = 0;
    int status = 0; // CFITSIO status value MUST be initialized to zero!
    long last_frame_size[] = { -1L, -1L };

    try
    {
        if (!wxFileExists(fname))
        {
            throw ERROR_INFO("File does not exist");
        }

        if (PHD_fits_open_diskfile(&fptr, fname, READONLY, &status) == 0)
        {
            int nhdus = 0;
            fits_get_num_hdus(fptr, &nhdus, &status);

            while (true)
            {
                int hdutype;
                fits_get_hdu_type(fptr, &hdutype, &status);
                if (hdutype != IMAGE_HDU)
                {
                    pFrame->Alert(wxString::Format(_("FITS file is not of an image: %s"), fname));
                    throw ERROR_INFO("FITS file is not an image");
                }

                int naxis;
                fits_get_img_dim(fptr, &naxis, &status);
                if (naxis != 2)
                {
                    pFrame->Alert(wxString::Format(_("Unsupported type or read error loading FITS file %s"), fname));
                    throw ERROR_INFO("unsupported type");
                }

                long fsize[2];
                fits_get_img_size(fptr, 2, fsize, &status);
                if (last_frame_size[0] != -1L)
                {
                    if (last_frame_size[0] != fsize[0] || last_frame_size[1] != fsize[1])
                    {
                        pFrame->Alert(_("Existing dark library has frames with incompatible formats - please rebuild the dark "
                                        "library from scratch."));
                        throw ERROR_INFO("Incompatible frame sizes in dark library");
                    }
                }
                last_frame_size[0] = fsize[0];
                last_frame_size[1] = fsize[1];

                std::unique_ptr<usImage> img(new usImage());

                if (img->Init((int) fsize[0], (int) fsize[1]))
                {
                    pFrame->Alert(wxString::Format(_("Memory allocation error reading FITS file %s"), fname));
                    throw ERROR_INFO("Memory Allocation failure");
                }

                long fpixel[] = { 1, 1, 1 };
                if (fits_read_pix(fptr, TUSHORT, fpixel, fsize[0] * fsize[1], nullptr, img->ImageData, nullptr, &status))
                {
                    pFrame->Alert(wxString::Format(_("Error reading data from %s"), fname));
                    throw ERROR_INFO("Error reading");
                }

                char keyname[] = "EXPOSURE";
                float exposure;
                if (fits_read_key(fptr, TFLOAT, keyname, &exposure, nullptr, &status))
                {
                    exposure = (float) pFrame->RequestedExposureDuration() / 1000.0;
                    Debug.Write(wxString::Format("missing EXPOSURE value, assume %.3f\n", exposure));
                    status = 0;
                }
                img->ImgExpDur = ROUNDF(exposure * 1000.0);

                char binning_key[] = "XBINNING";
                int binning = 1;
                fits_read_key(fptr, TINT, binning_key, &binning, nullptr, &status);
                img->Binning = wxMax(binning, 1);
                status = 0;

                char saturate_key[] = "SATURATE";
                int saturate = 65535;
                fits_read_key(fptr, TINT, saturate_key, &saturate, nullptr, &status);
                img->BitsPerPixel = saturate >= 256 ? 16 : 8;
                status = 0;

                char gain_key[] = "GAIN";
                int gain = 0;
                fits_read_key(fptr, TINT, gain_key, &gain, nullptr, &status);
                img->Gain = gain;
                status = 0;

                img->CalcStats();

                Debug.Write(wxString::Format("loaded dark frame exposure = %d, med = %u, %dx%d bin %d, bpp = %d, gain = %d\n",
                                             img->ImgExpDur, img->MedianADU, img->Size.x, img->Size.y, img->Binning,
                                             img->BitsPerPixel, (int) img->Gain));

                camera->AddDark(img.release());

                // if this is the last hdu, we are done
                int hdunr = 0;
                fits_get_hdu_num(fptr, &hdunr);
                if (status || hdunr >= nhdus)
                    break;

                // move to the next hdu
                fits_movrel_hdu(fptr, +1, nullptr, &status);
            }
        }
        else
        {
            pFrame->Alert(wxString::Format(_("Error opening FITS file %s"), fname));
            throw ERROR_INFO("error opening file");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    if (fptr)
    {
        PHD_fits_close_file(fptr);
    }

    return bError;
}

wxString MyFrame::GetDarksDir()
{
    wxString dirpath = GetDefaultFileDir() + PATHSEPSTR + "darks_defects";
    if (!wxDirExists(dirpath))
        if (!wxFileName::Mkdir(dirpath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL))
            dirpath = GetDefaultFileDir(); // should never happen
    return dirpath;
}

wxString MyFrame::DarkLibFileName(int profileId)
{
    int inst = wxGetApp().GetInstanceNumber();
    return MyFrame::GetDarksDir() + PATHSEPSTR +
        wxString::Format("PHD2_dark_lib%s_%d.fit", inst > 1 ? wxString::Format("_%d", inst) : wxString(), profileId);
}

bool MyFrame::DarkLibExists(int profileId, bool showAlert)
{
    bool bOk = false;
    wxString fileName = MyFrame::DarkLibFileName(profileId);

    if (wxFileExists(fileName))
    {
        const wxSize& sensorSize = pCamera->DarkFrameSize();
        if (sensorSize == UNDEFINED_FRAME_SIZE)
        {
            bOk = true;
            Debug.Write("DarkLib check: undefined frame size for current camera\n");
        }
        else
        {
            fitsfile *fptr;
            int status = 0; // CFITSIO status value MUST be initialized to zero!

            if (PHD_fits_open_diskfile(&fptr, fileName, READONLY, &status) == 0)
            {
                long fsize[2];
                fits_get_img_size(fptr, 2, fsize, &status);
                if (status == 0 && fsize[0] == sensorSize.x && fsize[1] == sensorSize.y)
                    bOk = true;
                else
                {
                    Debug.Write(
                        wxString::Format("DarkLib check: failed geometry check - fits status = %d, cam dimensions = {%d,%d}, "
                                         " dark dimensions = {%d,%d}\n",
                                         status, sensorSize.x, sensorSize.y, (int) fsize[0], (int) fsize[1]));

                    if (showAlert)
                        Alert(_("Dark library does not match the camera in this profile. Check that you are "
                                "connected to the camera you want to use for guiding."));
                }

                PHD_fits_close_file(fptr);
            }
            else
                Debug.Write(wxString::Format("DarkLib check: fitsio error on open_diskfile = %d\n", status));
        }
    }

    return bOk;
}

// Confirm that in-use darks or bpms have the same sensor size as the current camera.  Added to protect against
// surprise changes in binning
void MyFrame::CheckDarkFrameGeometry()
{
    bool haveDefectMap = DefectMap::DefectMapExists(pConfig->GetCurrentProfileId(), m_defectMapAvailable);
    bool haveDarkLib = DarkLibExists(pConfig->GetCurrentProfileId(), m_darksAvailable);
    bool defectMapOk = true;

    if (m_defectMapAvailable)
    {
        if (!haveDefectMap)
        {
            if (m_defectMapInUse)
                LoadDefectMapHandler(false);
            m_defectMapAvailable = false;
            Debug.Write("CheckDarkFrameGeometry: BPM incompatibility found\n");
            defectMapOk = false;
        }
    }
    else if (haveDefectMap)
    {
        m_defectMapAvailable = true;
    }

    if (m_darksAvailable)
    {
        if (!haveDarkLib)
        {
            if (m_darksInUse)
                LoadDarkHandler(false);
            m_darksAvailable = false;
            Debug.Write("CheckDarkFrameGeometry: Dark lib incompatibility found\n");
            if (!defectMapOk)
                pFrame->Alert(_("Dark library and bad-pixel maps don't match the current camera. "
                                "Check that you are connected to the camera you want to use for guiding."));
        }
    }
    else if (haveDarkLib)
    {
        m_darksAvailable = true;
    }

    m_prevDarkFrameSize = pCamera->DarkFrameSize();
}

void MyFrame::SetDarkState()
{
    bool haveDarkLib = DarkLibExists(pConfig->GetCurrentProfileId(), true);
    m_darksAvailable = haveDarkLib;
    if (!haveDarkLib)
        m_darksInUse = false;
    bool haveDefectMap = DefectMap::DefectMapExists(pConfig->GetCurrentProfileId(), true);
    m_defectMapAvailable = haveDefectMap;
    if (!haveDefectMap)
        m_defectMapInUse = false;
}

bool MyFrame::LoadDarkLibrary()
{
    wxString filename = MyFrame::DarkLibFileName(pConfig->GetCurrentProfileId());

    if (!pCamera || !pCamera->Connected)
    {
        Alert(_("You must connect a camera before loading dark frames"));
        return false;
    }

    if (load_multi_darks(pCamera, filename))
    {
        Debug.Write(wxString::Format("failed to load dark frames from %s\n", filename));
        StatusMsg(_("Darks not loaded"));
        return false;
    }
    else
    {
        Debug.Write(wxString::Format("loaded dark library from %s\n", filename));
        pCamera->SelectDark(m_exposureDuration);
        StatusMsg(_("Darks loaded"));
        return true;
    }
}

void MyFrame::SaveDarkLibrary(const wxString& note)
{
    wxString filename = MyFrame::DarkLibFileName(pConfig->GetCurrentProfileId());

    Debug.Write("saving dark library\n");

    if (save_multi_darks(pCamera->Darks, filename, note))
    {
        Alert(wxString::Format(_("Error saving darks FITS file %s"), filename));
    }
}

// Delete both the dark library file and any defect map file for this profile
void MyFrame::DeleteDarkLibraryFiles(int profileId)
{
    wxString filename = MyFrame::DarkLibFileName(profileId);

    if (wxFileExists(filename))
    {
        Debug.Write(wxString::Format("Removing dark library file: %s\n", filename));
        wxRemoveFile(filename);
    }

    DefectMap::DeleteDefectMap(profileId);
}

bool MyFrame::SetServerMode(bool serverMode)
{
    bool bError = false;

    m_serverMode = serverMode;

    pConfig->Global.SetBoolean("/ServerMode", m_serverMode);

    return bError;
}

bool MyFrame::SetTimeLapse(int timeLapse)
{
    bool bError = false;

    try
    {
        if (timeLapse < 0)
        {
            throw ERROR_INFO("timeLapse < 0");
        }

        m_timeLapse = timeLapse;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_timeLapse = DefaultTimelapse;
    }

    pConfig->Profile.SetInt("/frame/timeLapse", m_timeLapse);

    return bError;
}

bool MyFrame::SetFocalLength(int focalLength)
{
    bool bError = false;

    try
    {
        if (focalLength < 0)
        {
            throw ERROR_INFO("focal length < 0");
        }

        m_focalLength = focalLength;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_focalLength = DefaultFocalLength;
    }

    pConfig->Profile.SetInt("/frame/focalLength", m_focalLength);

    return bError;
}

void MyFrame::SetVariableDelayConfig(bool varDelayEnabled, int ShortDelayMS, int LongDelayMS)
{
    Debug.Write(wxString::Format("Variable delay: %s, Short = %d ms, Long = %d ms\n", varDelayEnabled ? "Enabled" : "Disabled",
                                 ShortDelayMS, LongDelayMS));

    m_varDelayConfig.enabled = varDelayEnabled;
    m_varDelayConfig.shortDelay = ShortDelayMS;
    m_varDelayConfig.longDelay = wxMax(LongDelayMS, ShortDelayMS);

    pConfig->Profile.SetInt("/frame/var_delay/short_delay", ShortDelayMS);
    pConfig->Profile.SetInt("/frame/var_delay/long_delay", LongDelayMS);
    pConfig->Profile.SetBoolean("/frame/var_delay/enabled", varDelayEnabled);
}

wxString MyFrame::GetDefaultFileDir()
{
    wxStandardPathsBase& stdpath = wxStandardPaths::Get();
    wxString rslt = stdpath.GetUserLocalDataDir(); // Automatically includes app name

    if (!wxDirExists(rslt))
        if (!wxFileName::Mkdir(rslt, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL))
            rslt = stdpath.GetUserLocalDataDir(); // should never happen

    return rslt;
}

double MyFrame::GetCameraPixelScale() const
{
    if (!pCamera)
        return 1.0;
    auto pixelSize = pCamera->GetCameraPixelSize();
    if (pixelSize == 0.0 || m_focalLength == 0)
        return 1.0;

    auto binning = pCamera->GetBinning();
    return GetPixelScale(pixelSize, m_focalLength, binning);
}

wxString MyFrame::PixelScaleSummary() const
{
    double pixelScale = GetCameraPixelScale();
    wxString scaleStr;
    if (pixelScale == 1.0)
        scaleStr = "unspecified";
    else
        scaleStr = wxString::Format("%.2f arc-sec/px", pixelScale);

    wxString focalLengthStr;
    if (m_focalLength == 0)
        focalLengthStr = "unspecified";
    else
        focalLengthStr = wxString::Format("%d mm", m_focalLength);

    int binning = pCamera->GetBinning();
    return wxString::Format("Pixel scale = %s, Binning = %d, Focal length = %s", scaleStr, binning, focalLengthStr);
}

bool MyFrame::GetBeepForLostStar()
{
    return m_beepForLostStar;
}

void MyFrame::SetBeepForLostStar(bool beep)
{
    m_beepForLostStar = beep;
    pConfig->Profile.SetBoolean("/BeepForLostStar", beep);
    Debug.Write(wxString::Format("Beep for lost star set to %s\n", beep ? "true" : "false"));
}

wxString MyFrame::GetSettingsSummary() const
{
    // return a loggable summary of current global configs managed by MyFrame
    return wxString::Format(
        "Dither = %s, Dither scale = %.3f, Image noise reduction = %s, Guide-frame time lapse = %d, Server %s\n"
        "%s\n",
        m_ditherRaOnly ? "RA only" : "both axes", m_ditherScaleFactor,
        m_noiseReductionMethod == NR_NONE          ? "none"
            : m_noiseReductionMethod == NR_2x2MEAN ? "2x2 mean"
                                                   : "3x3 median",
        m_timeLapse, m_serverMode ? "enabled" : "disabled", PixelScaleSummary());
}

// Get the best estimate for the current mount guide speeds
double MyFrame::DetermineGuideSpeed()
{
    double sidRate = 0.5;

    if (pPointingSource && pPointingSource->IsConnected())
    {
        double raSpd;
        double decSpd;

        if (!pPointingSource->GetGuideRates(&raSpd, &decSpd))
        {
            if (pPointingSource->ValidGuideRates(raSpd, decSpd))
            {
                double minSpd;
                if (decSpd != -1)
                    minSpd = wxMin(raSpd, decSpd);
                else
                    minSpd = raSpd;
                sidRate = image_scale::SiderealRateFromGuideSpeed(minSpd);
            }
        }
        else
        {
            CalibrationDetails calDetails;
            TheScope()->LoadCalibrationDetails(&calDetails);
            if (calDetails.IsValid())
            {
                if (TheScope()->ValidGuideRates(calDetails.raGuideSpeed, calDetails.decGuideSpeed))
                {
                    sidRate = image_scale::SiderealRateFromGuideSpeed(wxMin(calDetails.raGuideSpeed, calDetails.decGuideSpeed));
                }
            }
        }
    }
    return sidRate;
}

// Reacts to param changes in the AD that change the image scale.  Calibration step-size is recalculated, calibration is
// cleared, MinMoves are set to defaults based on new image scale
void MyFrame::MakeImageScaleAdjustments()
{
    Scope *scope = TheScope();
    if (!scope || !pCamera)
    {
        Debug.Write("Image scale changed but scope/camera unavailable; skipping adjustments\n");
        return;
    }

    auto binning = pCamera->GetBinning();
    auto focalLength = pFrame->GetFocalLength();
    auto pixelSize = pCamera->GetCameraPixelSize();
    double guideSpeedX;
    Debug.Write("Image scale has changed via AD UI - step-size and algo adjustments will be made\n");
    Debug.Write(wxString::Format("New image scale properties:  fl= %d, px= %.3fu, bin= %d\n", focalLength, pixelSize, binning));

    // Determine a calibration step-size based on recommended distance and best estimator of mount guide speeds
    guideSpeedX = DetermineGuideSpeed();
    int recDistance = CalstepDialog::GetCalibrationDistance(focalLength, pixelSize, binning);
    int oldStepSize = scope->GetCalibrationDuration();
    int calibrationStep = oldStepSize;
    if (CalstepDialog::GetCalibrationStepSize(focalLength, pixelSize, binning, guideSpeedX, CalstepDialog::DEFAULT_STEPS, 0,
                                              recDistance, nullptr, &calibrationStep))
    {
        scope->SetCalibrationDuration(calibrationStep);
        Debug.Write(wxString::Format("Cal step-size changed from %d ms to %d ms\n", oldStepSize, calibrationStep));
    }
    else
        Debug.Write(
            wxString::Format("Cal step-size kept at %d ms: no step size for this image scale and guide speed\n", oldStepSize));
    // Clear the calibration to force a new one and reset the min-move values
    if (pMount)
    {
        pMount->ClearCalibration();
        if (pMount->IsStepGuider() && pSecondaryMount)
            pSecondaryMount->ClearCalibration();
        Debug.Write("Calibrations cleared because of image scale change\n");

        double defMinMove = GuideAlgorithm::SmartDefaultMinMove(focalLength, pixelSize, binning);
        Debug.Write(wxString::Format("Guide algo min moves reset to %.3fu\n", defMinMove));
        AlgorithmSlot::Hold held = pMount->LockAlgorithms(); // a move can be computing on the worker (#142 T2b)
        pMount->GetXGuideAlgorithm()->SetMinMove(defMinMove);
        pMount->GetYGuideAlgorithm()->SetMinMove(defMinMove);
    }
}

// Reset the guiding parameters and the various graphical displays when image scale is changed outside the AD UI.  Goal is to
// restore basic guiding behavior until a fresh calibration is done.
void MyFrame::HandleImageScaleChange()
{
    Scope *scope = TheScope();
    if (scope)
    {
        // Adjust calibration step-size, clear existing calibrations, set reasonable MinMoves
        MakeImageScaleAdjustments();
    }

    if (pGuideHistory)
        pGuideHistory->ResetData();

    Alert(_("Binning or camera pixel size changed unexpectedly. "
            "You should use separate profiles for different image scales."));
}

#if defined(__LINUX__)
// ugly workaround for Issue 83 - link error on Linux
//  undefined reference to wxPluralFormsCalculatorPtr::~wxPluralFormsCalculatorPtr
wxPluralFormsCalculatorPtr::~wxPluralFormsCalculatorPtr() { }
#endif

template<typename T>
static void NotifyGuidingParam(const wxString& name, T val)
{
    GuideLog.SetGuidingParam(name, val);
    EvtServer.NotifyGuidingParam(name, val);
}

void MyFrame::NotifyGuidingParam(const wxString& name, double val)
{
    ::NotifyGuidingParam(name, val);
}

void MyFrame::NotifyGuidingParam(const wxString& name, int val)
{
    ::NotifyGuidingParam(name, val);
}

void MyFrame::NotifyGuidingParam(const wxString& name, bool val)
{
    ::NotifyGuidingParam(name, val);
}

void MyFrame::NotifyGuidingParam(const wxString& name, const wxString& val)
{
    ::NotifyGuidingParam(name, val);
}

// Interface to force logging if guiding is not active
void MyFrame::NotifyGuidingParam(const wxString& name, const wxString& val, bool ForceLog)
{
    GuideLog.SetGuidingParam(name, val, true);
    EvtServer.NotifyGuidingParam(name, val);
}
