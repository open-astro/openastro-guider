// Mechanical stand-ins that let the frozen copy of the guider's state machine
// (guider_sm_legacy.cpp: Guider::SetState, StopGuiding, MoveLockPosition and the three
// state-machine parts of UpdateGuideState) compile without wxWidgets or the app (#142, slice T2c).
//
// Nothing here decides anything the copied code depends on. wxString is a std::string wrapper
// with Format through snprintf. Every call the copied code makes on the gear, the frame, the
// event server, the guide log and Debug is written to Twin::trace, with the names
// tests/guiding/guider_sm_host.h uses for the same calls. The gear's answers (BeginCalibration
// failing, UpdateCalibrationState's outcome) come from the test. Guider has only the members the
// copied code uses, with the same names.
//
// Everything lives in namespace legacy_sm, so the copied lines compile verbatim.

#ifndef GUIDING_LEGACY_GUIDER_SM_LEGACY_H
#define GUIDING_LEGACY_GUIDER_SM_LEGACY_H

#include "guide_move_plan.h"
#include "guider_state.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace legacy_sm
{

class wxString
{
    std::string m_s;

public:
    wxString() = default;
    wxString(const char *s) : m_s(s ? s : "") { }
    wxString(std::string s) : m_s(std::move(s)) { }

    const char *c_str() const { return m_s.c_str(); }
    const std::string& str() const { return m_s; }
    bool empty() const { return m_s.empty(); }

    friend wxString operator+(const wxString& a, const wxString& b) { return wxString(a.m_s + b.m_s); }

    template<class... A>
    static wxString Format(const wxString& fmt, const A&...args)
    {
        char buf[512];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
        std::snprintf(buf, sizeof buf, fmt.c_str(), args...);
#pragma GCC diagnostic pop
        return wxString(buf);
    }
};

typedef char wxStringCharType;

struct Twin
{
    static std::vector<std::string> trace;
    static void Record(const std::string& s) { trace.push_back(s); }
};

struct TwinDebug
{
    void Write(const wxString& s) { Twin::Record("D:" + s.str()); }
    wxString AddLine(const wxString& s)
    {
        Twin::Record("D:" + s.str());
        return s;
    }
};
extern TwinDebug Debug;

// point.h's PHD_Point, the parts the copied code uses
class PHD_Point
{
    bool m_valid = false;

public:
    double X = 0.;
    double Y = 0.;

    PHD_Point() = default;
    PHD_Point(double x, double y) { SetXY(x, y); }
    bool IsValid() const { return m_valid; }
    void Invalidate() { m_valid = false; }
    void SetXY(double x, double y)
    {
        X = x;
        Y = y;
        m_valid = true;
    }
    double Distance() const { return hypot(X, Y); }
    PHD_Point operator+(const PHD_Point& a) const { return PHD_Point(X + a.X, Y + a.Y); }
};

class ShiftPoint : public PHD_Point
{
    PHD_Point m_rate;

public:
    const PHD_Point& ShiftRate() const { return m_rate; }
};

struct wxSize
{
    int x = 0;
    int y = 0;
    int GetWidth() const { return x; }
    int GetHeight() const { return y; }
};

struct wxPoint
{
    int x = 0;
    int y = 0;
};

struct wxColour
{
    wxColour() = default;
    wxColour(int, int, int) { }
};
typedef wxColour wxColor;

template<class T>
inline T wxMin(T a, T b)
{
    return a < b ? a : b;
}
inline void wxBell() { }
inline void wxMilliSleep(int) { }

struct usImage
{
    wxSize Size;
    int FrameNum = 0;
};

struct Star;

struct FrameDroppedInfo
{
    unsigned int frameNumber = 0;
    double time = 0;
    double avgDist = 0;
    wxString status;
};

struct GuiderOffset
{
    PHD_Point cameraOfs;
    PHD_Point mountOfs;
};

enum
{
    MOVEOPTS_DEDUCED_MOVE = 1,
    MOVEOPTS_RECOVERY_MOVE = 2,
    MOVEOPTS_GUIDE_STEP = 3,
};

// Mount::UpdateCalibrationState's outcome, == sm_test::CalStep
enum class TwinCalStep
{
    Continue,
    Complete,
    Fail,
};

class Mount
{
public:
    std::string name; // "Primary", "Secondary"
    bool calibrated = false;
    bool connected = true;
    bool busy = false;
    bool stepGuider = false;
    bool adjustClears = false; // AdjustCalibrationForScopePointing clears the calibration (#255)
    std::function<bool()> beginFails = [] { return false; };
    std::function<TwinCalStep()> calStep = [] { return TwinCalStep::Continue; };
    // TransformMountCoordinatesToCameraCoordinates: a rotation by angle; fails on the failAt'th call
    double angle = 0;
    int failAt = -1;
    int transformCalls = 0;

    bool IsCalibrated() const { return calibrated; }
    bool IsConnected() const { return connected; }
    bool IsBusy() const { return busy; }
    bool IsStepGuider() const { return stepGuider; }
    void ResetErrorCount() { Twin::Record(name + ".ResetErrorCount"); }
    bool BeginCalibration(const PHD_Point&)
    {
        Twin::Record(name + ".BeginCalibration");
        return beginFails();
    }
    bool UpdateCalibrationState(const PHD_Point&)
    {
        Twin::Record(name + ".UpdateCalibrationState");
        TwinCalStep s = calStep();
        if (s == TwinCalStep::Complete)
            calibrated = true;
        return s == TwinCalStep::Fail;
    }
    void SetFailedGuideMovesInARow(int n) { Twin::Record(name + ".SetFailedGuideMovesInARow(" + std::to_string(n) + ")"); }
    void AdjustCalibrationForScopePointing()
    {
        Twin::Record(name + ".AdjustCalibrationForScopePointing");
        if (adjustClears)
            calibrated = false;
    }
    bool TransformMountCoordinatesToCameraCoordinates(const PHD_Point& m, PHD_Point& c)
    {
        Twin::Record(name + ".TransformMountCoordinatesToCameraCoordinates");
        if (transformCalls++ == failAt)
            return true;
        c.SetXY(m.X * cos(angle) - m.Y * sin(angle), m.X * sin(angle) + m.Y * cos(angle));
        return false;
    }
    void NotifyDirectMove(const PHD_Point&) { Twin::Record(name + ".NotifyDirectMove"); }
};
extern Mount *pMount;
extern Mount *pSecondaryMount;

struct TwinHistory
{
    void AppendData(const FrameDroppedInfo&) { Twin::Record("History.AppendData"); }
};

struct TwinFrame
{
    TwinHistory history;
    TwinHistory *pGuideHistory = &history;
    std::string captureFailureReason;

    void UpdateStatusBarCalibrationStatus() { Twin::Record("Frame.UpdateStatusBarCalibrationStatus"); }
    wxString CaptureFailureStopReason() const { return wxString(captureFailureReason); }
    void NotifyGuidingStopped() { Twin::Record("Frame.NotifyGuidingStopped"); }
    void NotifyGuidingStarted() { Twin::Record("Frame.NotifyGuidingStarted"); }
    void SchedulePrimaryMove(Mount *, const GuiderOffset&, int opts)
    {
        Twin::Record(opts == MOVEOPTS_DEDUCED_MOVE        ? "Frame.SchedulePrimaryMove(DEDUCED)"
                         : opts == MOVEOPTS_RECOVERY_MOVE ? "Frame.SchedulePrimaryMove(RECOVERY)"
                                                          : "Frame.SchedulePrimaryMove(GUIDE_STEP)");
    }
    void StatusMsg(const wxString& s) { Twin::Record("Frame.StatusMsg(" + s.str() + ")"); }
    bool GetBeepForLostStar() const { return false; }
};
extern TwinFrame *pFrame;

struct TwinEvtServer
{
    void NotifyStartCalibration(Mount *m) { Twin::Record("Evt.StartCalibration(" + m->name + ")"); }
    void NotifyCalibrationFailed(Mount *m, const wxString& msg)
    {
        Twin::Record("Evt.CalibrationFailed(" + (m ? m->name : "null") + "," + msg.str() + ")");
    }
    void NotifyStarLost(const FrameDroppedInfo&) { Twin::Record("Evt.StarLost"); }
    void NotifyLooping(unsigned int, const Star *star, const FrameDroppedInfo *)
    {
        Twin::Record(star ? "Evt.Looping(star)" : "Evt.Looping(no star)");
    }
    void NotifyStarSelected(const PHD_Point&) { Twin::Record("Evt.StarSelected"); }
};
extern TwinEvtServer EvtServer;

struct LockPosShiftParams
{
};

struct TwinGuideLog
{
    void StartCalibration(Mount *m) { Twin::Record("GuideLog.StartCalibration(" + m->name + ")"); }
    void CalibrationFrameDropped(const FrameDroppedInfo&) { Twin::Record("GuideLog.CalibrationFrameDropped"); }
    void FrameDropped(const FrameDroppedInfo&) { Twin::Record("GuideLog.FrameDropped"); }
    void NotifyLockShiftParams(const LockPosShiftParams&, const PHD_Point&) { Twin::Record("GuideLog.NotifyLockShiftParams"); }
};
extern TwinGuideLog GuideLog;

struct GuidingAssistant
{
    static void NotifyFrameDropped(const FrameDroppedInfo&) { Twin::Record("GA.NotifyFrameDropped"); }
    static void NotifyBacklashStep(const PHD_Point&) { Twin::Record("GA.NotifyBacklashStep"); }
};

struct DeflectionLogger
{
    void Log(const PHD_Point&) { }
};

struct TwinDistance
{
    void OnDither(double dist, double distRA)
    {
        char buf[100];
        std::snprintf(buf, sizeof buf, "Distance.OnDither(%a,%a)", dist, distRA);
        Twin::Record(buf);
    }
};

inline void CheckCalibrationAutoLoad()
{
    Twin::Record("CheckCalibrationAutoLoad");
}

class Guider
{
public:
    GUIDER_STATE m_state = STATE_UNINITIALIZED;
    ShiftPoint m_lockPosition;
    bool m_lockPosIsSticky = false;
    PHD_Point m_ditherRecenterStep;
    wxPoint m_ditherRecenterDir;
    PHD_Point m_ditherRecenterRemaining;
    int m_ditherRecenterStepsPending = 0;
    int m_ditherRecenterStalledSteps = 0;
    bool m_recenteringForWorker = false;
    bool m_measurementMode = false;
    bool m_fastRecenterEnabled = true;
    int m_maxMovePixels = 20;
    double m_lockMargin = 10; // IsValidLockPosition: at least this far inside the frame
    TwinDistance m_distance;
    LockPosShiftParams m_lockPosShift;
    usImage m_image;
    PHD_Point m_currentPosition { 100, 100 };

    void SetState(GUIDER_STATE newState);
    void StopGuiding();
    bool MoveLockPosition(const PHD_Point& mountDeltaArg);

    // UpdateGuideState's state-machine parts, each copied into a function
    void LegacyStarLost(usImage *pImage, FrameDroppedInfo& info); // the star was not found
    void LegacyPaused(); // the frame while paused
    wxString LegacyStarFound(GuiderOffset& ofs); // the star was found; the status message it set

    void InvalidateLockPosition()
    {
        if (m_lockPosition.IsValid())
            Twin::Record("Evt.LockPositionLost");
        m_lockPosition.Invalidate();
    }
    void InvalidateCurrentPosition() { Twin::Record("InvalidateCurrentPosition"); }
    // Guider::SetLockPosition: the position inside the frame
    bool SetLockPosition(const PHD_Point& p)
    {
        Twin::Record("SetLockPosition");
        if (!p.IsValid() || p.X < 0.0 || p.X >= m_image.Size.x || p.Y < 0.0 || p.Y >= m_image.Size.y)
            return true;
        m_lockPosition.SetXY(p.X, p.Y);
        return false;
    }
    const PHD_Point& CurrentPosition() const { return m_currentPosition; }
    const usImage *CurrentImage() const { return &m_image; }
    bool IsValidLockPosition(const PHD_Point& p) const
    {
        return p.X >= m_lockMargin && p.X < m_image.Size.x - m_lockMargin && p.Y >= m_lockMargin &&
            p.Y < m_image.Size.y - m_lockMargin;
    }
    bool IsFastRecenterEnabled() const { return m_fastRecenterEnabled; }
    int GetMaxMovePixels() const { return m_maxMovePixels; }
    void UpdateLockPosShiftCameraCoords() { Twin::Record("UpdateLockPosShiftCameraCoords"); }
    bool LockPosShiftEnabled() const { return false; }
    wxColour GetBackgroundColour() const { return wxColour(); }
    void SetBackgroundColour(const wxColour&) { }
    void ClearBackground() { Twin::Record("ClearBackground"); }
};

} // namespace legacy_sm

#endif
