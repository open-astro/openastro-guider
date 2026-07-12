/*
 *  phd.cpp
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

#include <curl/curl.h>
#include <csignal>
#include <cstring>
#include <memory>
#include <wx/cmdline.h>
#include <wx/evtloop.h>
#include <wx/snglinst.h>

#include <X11/Xlib.h>

// #define DEVBUILD

// Globals

PhdConfig *pConfig = nullptr;
Mount *pMount = nullptr;
Mount *pSecondaryMount = nullptr;
Scope *pPointingSource = nullptr;
MyFrame *pFrame = nullptr;
GuideCamera *pCamera = nullptr;

DebugLog Debug;
GuidingLog GuideLog;

int XWinSize = 640;
int YWinSize = 512;

static const wxCmdLineEntryDesc cmdLineDesc[] = {
    { wxCMD_LINE_SWITCH, "?", "help", "display this help and exit" },
    { wxCMD_LINE_SWITCH, "g", "gui", "show the GUI window (default is headless)" },
    { wxCMD_LINE_SWITCH, "a", "auto-connect", "auto-connect the selected equipment on startup" },
    { wxCMD_LINE_SWITCH, "H", "headless", "deprecated: headless is now the default (accepted for compatibility)" },
    { wxCMD_LINE_SWITCH, "A", "headless-auto-connect", "deprecated alias for --auto-connect" },
    { wxCMD_LINE_OPTION, "i", "instanceNumber", "sets the PHD2 instance number (default = 1)", wxCMD_LINE_VAL_NUMBER,
      wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, "l", "load", "load settings from file and exit", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, "R", "Reset", "Reset all PHD2 settings to default values" },
    { wxCMD_LINE_OPTION, "s", "save", "save settings to file and exit", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, "v", "version", "print the program version and exit" },
    { wxCMD_LINE_NONE }
};

enum ConfigOp
{
    CONFIG_OP_NONE,
    CONFIG_OP_SAVE,
    CONFIG_OP_LOAD,
};
static ConfigOp s_configOp = CONFIG_OP_NONE;
static wxString s_configPath;

wxIMPLEMENT_APP(PhdApp);

static void DisableOSXAppNap() { }

// ------------------------  Phd App stuff -----------------------------

struct ExecFuncThreadEvent;
wxDEFINE_EVENT(EXEC_IN_MAIN_THREAD_EVENT, ExecFuncThreadEvent);

struct ExecFuncThreadEvent : public wxThreadEvent
{
    std::function<void()> func;
    ExecFuncThreadEvent(std::function<void()> func_) : wxThreadEvent(EXEC_IN_MAIN_THREAD_EVENT), func(func_) { }
};

PhdApp::PhdApp()
{
    m_resetConfig = false;
    m_headless = true; // headless is the default run mode; --gui opts into the window
    m_headlessAutoConnect = false;
    m_instanceNumber = 1;
    XInitThreads();

    Bind(EXEC_IN_MAIN_THREAD_EVENT, [](ExecFuncThreadEvent& evt) { evt.func(); });
};

void PhdApp::HandleRestart()
{
    // wait until prev instance (parent) terminates
    while (true)
    {
        std::unique_ptr<wxSingleInstanceChecker> si(
            new wxSingleInstanceChecker(wxString::Format("%s.%ld", GetAppName(), m_instanceNumber)));
        if (!si->IsAnotherRunning())
            break;
        wxMilliSleep(200);
    }

    // copy command-line args skipping "restart"
    wchar_t **targv = new wchar_t *[argc * sizeof(*targv)];
    targv[0] = wxStrdup(argv[0].wc_str());
    int src = 2, dst = 1;
    while (src < argc)
        targv[dst++] = wxStrdup(argv[src++].wc_str());
    targv[dst] = 0;

    // launch a new instance
    wxExecute(targv, wxEXEC_ASYNC);

    // exit the helper instance
    wxExit();
}

void PhdApp::RestartApp()
{
    // copy command-line args inserting "restart" as the first arg
    wchar_t **targv = new wchar_t *[(argc + 2) * sizeof(*targv)];
    targv[0] = wxStrdup(argv[0].wc_str());
    targv[1] = wxStrdup(_T("restart"));
    int src = 1, dst = 2;
    while (src < argc)
        targv[dst++] = wxStrdup(argv[src++].wc_str());
    targv[dst] = 0;

    // launch the restart process
    wxExecute(targv, wxEXEC_ASYNC);

    // gracefully exit this instance
    TerminateApp();
}

namespace
{
// Set by the async-signal handler; polled on the main thread by the timer
// below. A signal handler must not touch wx/heap state, so it only flips this.
volatile sig_atomic_t s_shutdownSignal = 0;

extern "C" void HandleTermSignal(int sig)
{
    s_shutdownSignal = sig;
}

// Polls the signal flag and runs a graceful shutdown (the same path as the
// JSON-RPC `shutdown` command: camera/mount disconnect, guide-log summary,
// config flush) on the main thread. Without this, `systemctl stop` (SIGTERM)
// kills the daemon with the default disposition and none of that runs.
class SignalWatchTimer : public wxTimer
{
public:
    void Notify() override
    {
        if (s_shutdownSignal)
        {
            int sig = s_shutdownSignal;
            s_shutdownSignal = 0;
            Stop();
            Debug.Write(wxString::Format("received signal %d, shutting down gracefully\n", sig));
            wxGetApp().TerminateApp();
        }
    }
};

SignalWatchTimer *s_signalWatchTimer = nullptr;
} // namespace

static void IdleClosing(wxIdleEvent& evt)
{
    Debug.Write("IdleClosing\n");

    // If a modal dialog box is up then the app will crash if we try to close pFrame,
    // As a workaround keep exiting any nested event loops until we get back to the
    // main event loop, then close pFrame

    wxEventLoopBase *el = wxEventLoopBase::GetActive();
    if (!el->IsMain())
    {
        el->Exit(-1);
        evt.RequestMore();
        return;
    }

    wxGetApp().Unbind(wxEVT_IDLE, &IdleClosing);
    pFrame->Close(true /*force*/);
}

void PhdApp::TerminateApp()
{
    // The wxEVT_CLOSE_WINDOW message may not be processed if phd2 is sitting idle
    // when the client invokes shutdown. As a workaround pump some timer event messages to
    // keep the event loop from stalling and ensure that the wxEVT_CLOSE_WINDOW is processed.

    (new wxTimer(&wxGetApp()))->Start(20); // this object leaks but we don't care

    wxGetApp().Bind(wxEVT_IDLE, &IdleClosing);
    wxGetApp().WakeUpIdle();
}

static wxString GetOsDescription()
{
    return wxGetOsDescription();
}

static void OpenLogs(bool rollover)
{
    bool debugEnabled = rollover ? Debug.IsEnabled() : true;
    bool forceDebugOpen = rollover ? true : false;
    Debug.InitDebugLog(debugEnabled, forceDebugOpen);

    Debug.Write("OpenAstro Alpaca Support\n");
    Debug.Write(wxString::Format("PHD2 version %s %s execution with:\n", FULLVER, rollover ? "continues" : "begins"));
    Debug.Write(wxString::Format("   %s\n", GetOsDescription()));
    Debug.Write(wxString::Format("   %s\n", wxGetLinuxDistributionInfo().Description));
    Debug.Write(wxString::Format("   %s\n", wxVERSION_STRING));
    float dummy;
    Debug.Write(wxString::Format("   cfitsio %.2lf\n", ffvers(&dummy)));
#if defined(CV_VERSION)
    // CV_VERSION is a string literal from <opencv2/core/version.hpp>; cppcheck
    // doesn't parse that header and mis-types it as int.
    // cppcheck-suppress invalidPrintfArgType_s
    Debug.Write(wxString::Format("   opencv %s\n", CV_VERSION));
#endif

    if (rollover)
    {
        bool guideEnabled = GuideLog.IsEnabled();
        GuideLog.CloseGuideLog();
        GuideLog.EnableLogging(guideEnabled);
    }
    else
        GuideLog.EnableLogging(true);
}

struct LogToStderr
{
    wxLog *m_prev;
    LogToStderr() { m_prev = wxLog::SetActiveTarget(new wxLogStderr()); }
    ~LogToStderr() { delete wxLog::SetActiveTarget(m_prev); }
};

// A log class for duplicating wxWidgets error messages to the debug log
//
struct EarlyLogger : public wxLog
{
    bool m_closed;
    wxLog *m_prev;
    wxString m_buf;
    EarlyLogger() : m_closed(false)
    {
        wxASSERT(wxThread::IsMain());
        m_prev = wxLog::SetActiveTarget(this);
        DisableTimestamp();
    }
    ~EarlyLogger() { Close(); }
    void Close()
    {
        if (m_closed)
            return;

        wxLog::SetActiveTarget(m_prev);
        m_prev = nullptr;

        if (!m_buf.empty())
        {
            if (Debug.IsOpened())
                Debug.Write(wxString::Format("wx error: %s\n", m_buf));

            wxLogError(m_buf);

            m_buf.clear();
        }

        m_closed = true;
    }
    void DoLogText(const wxString& msg) override
    {
        if (!m_buf.empty() && *m_buf.rbegin() != '\n')
            m_buf += '\n';
        m_buf += msg;
    }
};

bool PhdApp::OnInit()
{

    // capture wx error messages until the debug log has been opened
    EarlyLogger logger;

    if (argc > 1 && argv[1] == _T("restart"))
        HandleRestart(); // exits

    if (!wxApp::OnInit())
    {
        return false;
    }

    // Vendor + app name determine wxConfig + wxStandardPaths locations and the
    // wxSingleInstanceChecker lock name below. Pre-2.0.0 this fork wrote to
    // upstream PHD2's location (StarkLabs / PHD2|phd2), which meant running
    // both apps shared settings (and Inno Setup's uninsdeletekey on the
    // StarkLabs key would wipe upstream's HKCU on uninstall). 2.0.0 moves to
    // an OpenAstro-specific namespace; PhdConfig's constructor handles the
    // one-time migration from the legacy location.
    //
    // These must be set BEFORE the wxSingleInstanceChecker construction below:
    // GetAppName() falls back to the executable basename if SetAppName hasn't
    // been called yet. If the basename collides with upstream PHD2's, both apps
    // end up taking the lock named "phd2.1" — the second one to launch hits
    // "PHD2 instance 1 is already running".
    SetVendorName(_T("OpenAstro"));
    // use SetAppName() to ensure the local data directory is found even if the name of the executable is changed
    SetAppName(_T("openastro-guider"));

    m_instanceChecker = new wxSingleInstanceChecker(wxString::Format("%s.%ld", GetAppName(), m_instanceNumber));
    if (m_instanceChecker->IsAnotherRunning())
    {
        wxLogError(wxString::Format(_("PHD2 instance %ld is already running. Use the "
                                      "-i INSTANCE_NUM command-line option to start a different instance."),
                                    m_instanceNumber));
        delete m_instanceChecker; // OnExit() won't be called if we return false
        m_instanceChecker = 0;
        return false;
    }

#ifndef DEBUG
# if (wxMAJOR_VERSION > 2 || wxMINOR_VERSION > 8)
    wxDisableAsserts();
# endif
#endif
    pConfig = new PhdConfig(m_instanceNumber);

    if (s_configOp == CONFIG_OP_LOAD)
    {
        bool err = pConfig->RestoreAll(s_configPath);

        // the config is ordinarily flushed to disk when we exit
        // gracefully and the pConfig object is destroyed, but the
        // following call to exit() bypasses ordinary cleanup and we
        // need to explicitly flush the config to disk before exiting
        if (!err)
            err = !pConfig->Flush();

        ::exit(err ? 1 : 0);
        return false;
    }
    else if (s_configOp == CONFIG_OP_SAVE)
    {
        bool err = pConfig->SaveAll(s_configPath);
        ::exit(err ? 1 : 0);
        return false;
    }

    m_logFileTime = DebugLog::GetLogFileTime(); // GetLogFileTime implements grouping by imaging-day, the 24-hour period
                                                // starting at 09:00 am local time
    OpenLogs(false /* not for rollover */);

    logger.Close(); // writes any deferrred error messages to the debug log

    DisableOSXAppNap();

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (m_resetConfig)
    {
        ResetConfiguration();
    }

    // on Linux look in the build tree first, otherwise use the system location
    m_resourcesDir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath() + "/share/phd2";
    if (!wxDirExists(m_resourcesDir))
        m_resourcesDir = wxStandardPaths::Get().GetResourcesDir();

    wxString ldir = GetLocalesDir();
    Debug.Write(wxString::Format("locale: using dir %s exists=%d\n", ldir, wxDirExists(ldir)));
    wxLocale::AddCatalogLookupPathPrefix(ldir);

    int langid = pConfig->Global.GetInt("/wxLanguage", wxLANGUAGE_DEFAULT);
    bool ok = m_locale.Init(langid);
    Debug.Write(wxString::Format("locale: initialized with lang id %d (r=%d)\n", langid, ok));
    if (!m_locale.AddCatalog(PHD_MESSAGES_CATALOG))
    {
        Debug.Write(wxString::Format("locale: AddCatalog(%s) failed\n", PHD_MESSAGES_CATALOG));
    }
    wxSetlocale(LC_NUMERIC, "C");

    wxTranslations::Get()->SetLanguage((wxLanguage) langid);
    Debug.Write(wxString::Format("locale: wxTranslations language set to %d\n", langid));

    Debug.RemoveOldFiles();
    GuideLog.RemoveOldFiles();

    pConfig->InitializeProfile();

    if (m_headless)
    {
        // Ensure the event/JSON-RPC server is always enabled in headless mode.
        pConfig->Global.SetBoolean("/ServerMode", true);
    }

    PhdController::OnAppInit();

    ImageLogger::Init();

    wxImage::AddHandler(new wxJPEGHandler);
    wxImage::AddHandler(new wxPNGHandler);

    pFrame = new MyFrame();

    if (m_headless)
    {
        pFrame->Show(false);

        // Graceful shutdown on `systemctl stop` (SIGTERM) and Ctrl-C (SIGINT).
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = HandleTermSignal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        s_signalWatchTimer = new SignalWatchTimer();
        s_signalWatchTimer->Start(100);

        if (m_headlessAutoConnect)
        {
            wxString errMsg;
            bool error = pFrame->pGearDialog->ConnectAll(&errMsg);
            if (error)
                Debug.Write(wxString::Format("headless: auto-connect failed: %s\n", errMsg));
            else
                Debug.Write("headless: auto-connect succeeded\n");
        }
    }
    else
    {
        pFrame->Show(true);

        if (pConfig->IsNewInstance() || (pConfig->NumProfiles() == 1 && pFrame->pGearDialog->IsEmptyProfile()))
        {
            pFrame->pGearDialog->ShowProfileWizard(); // First-light version of profile wizard
        }
    }

    return true;
}

int PhdApp::OnExit()
{
    assert(!pMount);
    assert(!pSecondaryMount);
    assert(!pCamera);

    ImageLogger::Destroy();

    PhdController::OnAppExit();

    delete pConfig;
    pConfig = nullptr;

    curl_global_cleanup();

    delete m_instanceChecker;
    m_instanceChecker = nullptr;

    return wxApp::OnExit();
}

void PhdApp::OnInitCmdLine(wxCmdLineParser& parser)
{
    parser.SetDesc(cmdLineDesc);
    parser.SetSwitchChars(wxT("-"));
}

bool PhdApp::OnCmdLineParsed(wxCmdLineParser& parser)
{
    if (parser.Found("?"))
    {
        parser.Usage();
        ::exit(0);
    }
    else if (parser.Found("v"))
    {
        wxPrintf("%s\n", FULLVER);
        ::exit(0);
    }

    (void) parser.Found("i", &m_instanceNumber);
    // Headless is the default run mode for the guiding daemon; pass --gui to
    // show the window for local debugging. --headless is still accepted (now a
    // no-op) so existing scripts/units don't break.
    m_headless = !parser.Found("gui");
    m_headlessAutoConnect = parser.Found("auto-connect") || parser.Found("headless-auto-connect");

    if (parser.Found("l", &s_configPath))
        s_configOp = CONFIG_OP_LOAD;
    if (parser.Found("s", &s_configPath))
        s_configOp = CONFIG_OP_SAVE;

    m_resetConfig = parser.Found("R");

    return true;
}

bool PhdApp::Yield(bool onlyIfNeeded)
{
    bool bReturn = !onlyIfNeeded;

    if (wxThread::IsMain())
    {
        bReturn = wxApp::Yield(onlyIfNeeded);
    }

    return bReturn;
}

void PhdApp::ExecInMainThread(std::function<void()> func)
{
    if (wxThread::IsMain())
    {
        func();
    }
    else
    {
        wxQueueEvent(&wxGetApp(), new ExecFuncThreadEvent(func));
    }
}

wxString PhdApp::GetLocalesDir() const
{
    return m_resourcesDir + PATHSEPSTR + _T("locale");
}

// get the imaging date for a given date-time
//    log files started after midnight belong to the prior imaging day
//    imaging day rolls over at 9am
wxDateTime PhdApp::ImagingDay(const wxDateTime& dt)
{
    if (dt.GetHour() >= 9)
        return dt.GetDateOnly();
    // midnight .. 9am belongs to previous day
    static wxDateSpan ONE_DAY(0 /* years */, 0 /* months */, 0 /* weeks */, 1 /* days */);
    return dt.GetDateOnly().Subtract(ONE_DAY);
}

bool PhdApp::IsSameImagingDay(const wxDateTime& a, const wxDateTime& b)
{
    return ImagingDay(a).IsSameDate(ImagingDay(b));
}

void PhdApp::CheckLogRollover()
{
    wxDateTime now = wxDateTime::Now();
    if (IsSameImagingDay(m_logFileTime, now))
        return;

    m_logFileTime = now;
    OpenLogs(true /* rollover */);
}

wxString PhdApp::UserAgent() const
{
    // cppcheck-suppress unknownMacro  // FULLVER / PHD_OSNAME are platform #define string literals
    return _T("phd2/") FULLVER _T(" (") PHD_OSNAME _T(")");
}

void PhdApp::ResetConfiguration()
{
    for (unsigned int i = 0; i < pConfig->NumProfiles(); i++)
        MyFrame::DeleteDarkLibraryFiles(i);

    pConfig->DeleteAll();
}
