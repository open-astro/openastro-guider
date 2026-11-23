/*
 *  controller_host.h
 *  PHD Guiding
 *
 *  What PhdController (phdcontrol.cpp) asks of the rest of the guider (#142,
 *  slice T2f of #115 steps 4-9): the frame, the guider, the mounts, the camera,
 *  the event server and the guide log, behind one interface so a test can drive
 *  the controller's state machine with a recording fake
 *  (tests/guiding/test_phd_controller.cpp). controller_host.cpp is the
 *  production glue, one forward per call. Every decision stays in
 *  phdcontrol.cpp; nothing here decides anything.
 *
 *  wxString and wxRect come from phd.h in production and from the test's
 *  stand-ins (tests/guiding/phdcontrol_shim.h), as phdcontrol.h's do.
 */

#ifndef CONTROLLER_HOST_H_INCLUDED
#define CONTROLLER_HOST_H_INCLUDED

#include "guider_state.h" // enum GUIDER_STATE
#include "settle_tracker.h" // SettleTracker::ClockMs

// == DEC_GUIDE_MODE (scope.h); controller_host.cpp checks the values
enum class ControllerDecMode
{
    None = 0,
    Auto,
    North,
    South,
};

// pMount and pSecondaryMount
enum class ControllerMount
{
    Primary,
    Secondary,
};

class ControllerHost
{
public:
    virtual ~ControllerHost() = default;

    // the settle's clock (SettleTracker); production's is SettleTracker::ProductionClockMs
    virtual SettleTracker::ClockMs SettleClock() const = 0;

    // equipment
    virtual bool CameraConnected() const = 0; // pCamera && pCamera->Connected
    virtual bool HasMount(ControllerMount m) const = 0;
    virtual bool MountConnected(ControllerMount m) const = 0;
    virtual bool MountCalibrated(ControllerMount m) const = 0;
    virtual void ClearMountCalibration(ControllerMount m) = 0;
    virtual bool PrimaryMountIsStepGuider() const = 0;
    virtual bool AoBumpInProgress() const = 0; // pMount is a step guider with a bump in progress
    virtual ControllerDecMode PrimaryScopeDecGuideMode() const = 0; // pMount is a scope
    virtual void SetScopeDecGuideMode(ControllerDecMode mode) = 0; // TheScope()
    virtual wxString DecGuideModeStr(ControllerDecMode mode) const = 0;
    virtual void NotifyMountSettleDone(bool success) = 0; // pMount->NotifyGuidingDitherSettleDone, with a pMount

    // the frame
    virtual bool CaptureActive() const = 0;
    virtual void StartCapturing() = 0;
    virtual bool StartGuiding() = 0; // true on error, as MyFrame::StartGuiding
    virtual bool AutoSelectStar(const wxRect& roi, bool willForceRecal) = 0; // true on error
    virtual bool AutoExposureEnabled() const = 0;
    virtual void ResetAutoExposure() = 0;
    virtual bool DitherRaOnlySetting() const = 0; // MyFrame::GetDitherRaOnly
    virtual bool Dither(double pixels, bool raOnly) = 0; // true on error
    virtual double CurrentGuideError() const = 0;

    // the guider
    virtual GUIDER_STATE GuiderState() const = 0;
    virtual bool GuiderIsCalibratingOrGuiding() const = 0;
    virtual bool GuiderIsCalibrating() const = 0;
    virtual bool GuiderIsLocked() const = 0;
    virtual bool SelectedStarClearForRecalibration() const = 0;
    virtual void ResetGuider(bool fullReset) = 0;
    virtual void ForceFullFrame() = 0;
    virtual bool LockPosIsSticky() const = 0;
    virtual void SetLockPosIsSticky(bool sticky) = 0;

    // the event server and the guide log
    virtual void NotifySettleBegin() = 0;
    virtual void NotifySettling(double distance, double time, double settleTime, bool starLocked) = 0;
    virtual void NotifySettleDone(const wxString& errorMsg, int totalFrames, int droppedFrames) = 0;
    virtual void LogSettlingStateChange(const wxString& msg) = 0; // GuideLog.NotifySettlingStateChange
};

// The production host (controller_host.cpp), the one phd.cpp gives PhdController::OnAppInit
ControllerHost& ProductionControllerHost();

#endif // CONTROLLER_HOST_H_INCLUDED
