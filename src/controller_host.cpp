/*
 *  controller_host.cpp
 *  PHD Guiding
 *
 *  The production ControllerHost (controller_host.h): each call forwards to
 *  pFrame, pFrame->pGuider, pCamera, pMount, pSecondaryMount, EvtServer or
 *  GuideLog, as phdcontrol.cpp called them before the seam (#142, slice T2f).
 */

#include "phd.h"

#include "controller_host.h"

static_assert(static_cast<int>(ControllerDecMode::None) == DEC_NONE && static_cast<int>(ControllerDecMode::Auto) == DEC_AUTO &&
              static_cast<int>(ControllerDecMode::North) == DEC_NORTH &&
              static_cast<int>(ControllerDecMode::South) == DEC_SOUTH);

namespace
{

Mount *MountOf(ControllerMount m)
{
    return m == ControllerMount::Primary ? pMount : pSecondaryMount;
}

class ProductionHost : public ControllerHost
{
public:
    SettleTracker::ClockMs SettleClock() const override { return &SettleTracker::ProductionClockMs; }

    bool CameraConnected() const override { return pCamera && pCamera->Connected; }
    bool HasMount(ControllerMount m) const override { return MountOf(m) != nullptr; }
    bool MountConnected(ControllerMount m) const override { return MountOf(m)->IsConnected(); }
    bool MountCalibrated(ControllerMount m) const override { return MountOf(m)->IsCalibrated(); }
    void ClearMountCalibration(ControllerMount m) override { MountOf(m)->ClearCalibration(); }
    bool PrimaryMountIsStepGuider() const override { return pMount->IsStepGuider(); }
    bool AoBumpInProgress() const override
    {
        return pMount && pMount->IsStepGuider() && static_cast<StepGuider *>(pMount)->IsBumpInProgress();
    }
    ControllerDecMode PrimaryScopeDecGuideMode() const override
    {
        return static_cast<ControllerDecMode>(static_cast<Scope *>(pMount)->GetDecGuideMode());
    }
    void SetScopeDecGuideMode(ControllerDecMode mode) override { TheScope()->SetDecGuideMode(static_cast<int>(mode)); }
    wxString DecGuideModeStr(ControllerDecMode mode) const override
    {
        return Scope::DecGuideModeStr(static_cast<DEC_GUIDE_MODE>(mode));
    }
    void NotifyMountSettleDone(bool success) override
    {
        if (pMount)
            pMount->NotifyGuidingDitherSettleDone(success);
    }

    bool CaptureActive() const override { return pFrame->CaptureActive; }
    void StartCapturing() override { pFrame->StartCapturing(); }
    bool StartGuiding() override { return pFrame->StartGuiding(); }
    bool AutoSelectStar(const wxRect& roi, bool willForceRecal) override { return pFrame->AutoSelectStar(roi, willForceRecal); }
    bool AutoExposureEnabled() const override { return pFrame->GetAutoExposureCfg().enabled; }
    void ResetAutoExposure() override { pFrame->ResetAutoExposure(); }
    bool DitherRaOnlySetting() const override { return pFrame->GetDitherRaOnly(); }
    bool Dither(double pixels, bool raOnly) override { return pFrame->Dither(pixels, raOnly); }
    double CurrentGuideError() const override { return pFrame->CurrentGuideError(); }

    GUIDER_STATE GuiderState() const override { return pFrame->pGuider->GetState(); }
    bool GuiderIsCalibratingOrGuiding() const override { return pFrame->pGuider->IsCalibratingOrGuiding(); }
    bool GuiderIsCalibrating() const override { return pFrame->pGuider->IsCalibrating(); }
    bool GuiderIsLocked() const override { return pFrame->pGuider->IsLocked(); }
    bool SelectedStarClearForRecalibration() const override { return pFrame->pGuider->SelectedStarClearForRecalibration(); }
    void ResetGuider(bool fullReset) override { pFrame->pGuider->Reset(fullReset); }
    void ForceFullFrame() override { pFrame->pGuider->ForceFullFrame(); }
    bool LockPosIsSticky() const override { return pFrame->pGuider->LockPosIsSticky(); }
    void SetLockPosIsSticky(bool sticky) override { pFrame->pGuider->SetLockPosIsSticky(sticky); }

    void NotifySettleBegin() override { EvtServer.NotifySettleBegin(); }
    void NotifySettling(double distance, double time, double settleTime, bool starLocked) override
    {
        EvtServer.NotifySettling(distance, time, settleTime, starLocked);
    }
    void NotifySettleDone(const wxString& errorMsg, int totalFrames, int droppedFrames) override
    {
        EvtServer.NotifySettleDone(errorMsg, totalFrames, droppedFrames);
    }
    void LogSettlingStateChange(const wxString& msg) override { GuideLog.NotifySettlingStateChange(msg); }
};

} // namespace

ControllerHost& ProductionControllerHost()
{
    static ProductionHost host;
    return host;
}
