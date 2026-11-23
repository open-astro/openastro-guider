// A Host for guider_state_machine (#142, slice T2c) over a scripted World, for
// test_guider_state_machine.cpp and test_guider_state_machine_parity.cpp.
//
// Each Action does to the World what GuiderStateHost::Do in guider.cpp does to the guider and the
// gear, and writes the calls it stands for to World::trace with the names the legacy twin's
// stand-ins use (legacy/guider_sm_legacy.h), so the two traces compare line by line. The Action
// itself is also kept in World::actions, and each state the machine sets in World::states.

#ifndef GUIDING_GUIDER_SM_HOST_H
#define GUIDING_GUIDER_SM_HOST_H

#include "guide_move_plan.h"
#include "guider_state_machine.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sm_test
{

using guider_state_machine::Action;

// Mount::UpdateCalibrationState's outcome for a frame
enum class CalStep
{
    Continue,
    Complete, // the mount is calibrated now
    Fail, // returns true
};

struct World
{
    GUIDER_STATE state = STATE_UNINITIALIZED;

    // the gear
    bool primaryCalibrated = false;
    bool secondaryExists = false; // pSecondaryMount != nullptr
    bool secondaryConnected = false;
    bool secondaryCalibrated = false;
    bool primaryBusy = false;
    bool secondaryBusy = false;
    bool primaryIsStepGuider = false;
    std::string captureFailureReason; // pFrame->CaptureFailureStopReason()
    // AdjustCalibrationForScopePointing clears the calibration (an image scale change, #255)
    bool adjustClearsPrimary = false;
    bool adjustClearsSecondary = false;
    // BeginCalibration fails; UpdateCalibrationState's outcome
    std::function<bool(bool secondary)> beginFails = [](bool) { return false; };
    std::function<CalStep(bool secondary)> calStep = [](bool) { return CalStep::Continue; };

    // the guider
    bool lockValid = false;
    bool lockSticky = false;
    bool measurementMode = false;
    bool recenterActive = false; // m_ditherRecenterRemaining.IsValid()
    double recenterRemainingX = 0, recenterRemainingY = 0;
    double recenterStepX = 0, recenterStepY = 0;
    int recenterDirX = 1, recenterDirY = 1;
    int recenterStepsPending = 0;
    int recenterStalledSteps = 0;
    bool recenteringForWorker = false;

    std::vector<std::string> trace;
    std::vector<Action> actions;
    std::vector<GUIDER_STATE> states;

    void Record(const std::string& s) { trace.push_back(s); }
};

class Host : public guider_state_machine::Host
{
public:
    explicit Host(World& w) : m_w(w) { }

    GUIDER_STATE State() const override { return m_w.state; }
    void SetStateValue(GUIDER_STATE st) override
    {
        m_w.state = st;
        m_w.states.push_back(st);
    }
    // Guider::SetState
    void RequestState(GUIDER_STATE st) override { guider_state_machine::SetState(*this, st); }
    void Log(const std::string& line) override { m_w.Record("D:" + line); }

    bool PrimaryCalibrated() const override { return m_w.primaryCalibrated; }
    bool SecondaryConnected() const override { return m_w.secondaryExists && m_w.secondaryConnected; }
    bool SecondaryCalibrated() const override { return m_w.secondaryCalibrated; }
    bool MountsIdle() const override { return !m_w.primaryBusy && (!m_w.secondaryExists || !m_w.secondaryBusy); }
    bool LockPositionValid() const override { return m_w.lockValid; }
    bool LockPositionSticky() const override { return m_w.lockSticky; }
    bool RecenterActive() const override { return m_w.recenterActive; }
    bool MeasurementMode() const override { return m_w.measurementMode; }

    bool Do(Action a) override
    {
        m_w.actions.push_back(a);
        World& w = m_w;
        switch (a)
        {
        case Action::IllegalTransition:
            w.Record("D:E:Illegal state transition");
            break;
        case Action::InvalidateLockPosition:
            if (w.lockValid)
                w.Record("Evt.LockPositionLost");
            w.lockValid = false;
            break;
        case Action::InvalidateCurrentPosition:
            w.Record("InvalidateCurrentPosition");
            break;
        case Action::ResetErrorCountPrimary:
            w.Record("Primary.ResetErrorCount");
            break;
        case Action::BeginCalibrationPrimary:
            w.Record("Primary.BeginCalibration");
            return w.beginFails(false);
        case Action::BeginCalibrationFailedPrimary:
            w.Record("D:E:pMount->BeginCalibration failed");
            w.Record("D:E:pMount->BeginCalibration failed");
            break;
        case Action::CalibrationStartedPrimary:
            w.Record("GuideLog.StartCalibration(Primary)");
            w.Record("Evt.StartCalibration(Primary)");
            break;
        case Action::ResetErrorCountSecondary:
            w.Record("Secondary.ResetErrorCount");
            break;
        case Action::BeginCalibrationSecondary:
            w.Record("Secondary.BeginCalibration");
            return w.beginFails(true);
        case Action::BeginCalibrationFailedSecondary:
            w.Record("D:E:pSecondaryMount->BeginCalibration failed");
            w.Record("D:E:pSecondaryMount->BeginCalibration failed");
            break;
        case Action::CalibrationStartedSecondary:
            w.Record("GuideLog.StartCalibration(Secondary)");
            w.Record("Evt.StartCalibration(Secondary)");
            break;
        case Action::EnterGuiding:
            w.recenterActive = false;
            w.recenterStepsPending = 0;
            w.recenterStalledSteps = 0;
            w.recenteringForWorker = false;
            w.Record("Primary.SetFailedGuideMovesInARow(0)");
            if (w.secondaryExists)
                w.Record("Secondary.SetFailedGuideMovesInARow(0)");
            w.Record("Primary.AdjustCalibrationForScopePointing");
            if (w.adjustClearsPrimary)
                w.primaryCalibrated = false;
            if (w.secondaryExists)
            {
                w.Record("Secondary.AdjustCalibrationForScopePointing");
                if (w.adjustClearsSecondary)
                    w.secondaryCalibrated = false;
            }
            w.Record("Frame.UpdateStatusBarCalibrationStatus");
            break;
        case Action::SetLockToCurrent:
        case Action::LockOnSelectedStar:
            w.Record("SetLockPosition");
            w.lockValid = true;
            break;

        case Action::NotifyCalibrationFailedPrimary:
        case Action::NotifyCalibrationFailedSecondary:
            w.Record(std::string("Evt.CalibrationFailed(") +
                     (a == Action::NotifyCalibrationFailedPrimary ? "Primary"
                          : w.secondaryExists                     ? "Secondary"
                                                                  : "null") +
                     "," + (w.captureFailureReason.empty() ? "Calibration manually stopped" : w.captureFailureReason) + ")");
            break;
        case Action::NotifyGuidingStopped:
            w.Record("Frame.NotifyGuidingStopped");
            break;

        case Action::NotifyLoopingNoStar:
            w.Record("Evt.Looping(no star)");
            break;
        case Action::NotifyStarLost:
            w.Record("Evt.StarLost");
            break;
        case Action::CalibrationFrameDropped:
            w.Record("GuideLog.CalibrationFrameDropped");
            break;
        case Action::StatusStarLost:
            w.Record("Frame.StatusMsg(star lost)");
            break;
        case Action::GuidingFrameDropped:
            w.Record("GuideLog.FrameDropped");
            break;
        case Action::RecordDroppedFrame:
            w.Record("GA.NotifyFrameDropped");
            w.Record("History.AppendData");
            break;
        case Action::DeducedMove:
            w.Record("Frame.SchedulePrimaryMove(DEDUCED)");
            break;
        case Action::LostStarAlert:
            w.Record("ClearBackground");
            break;

        case Action::NotifyStarSelected:
            w.Record("Evt.StarSelected");
            break;
        case Action::UpdateCalibrationPrimary:
        case Action::UpdateCalibrationSecondary:
        {
            bool secondary = a == Action::UpdateCalibrationSecondary;
            w.Record(secondary ? "Secondary.UpdateCalibrationState" : "Primary.UpdateCalibrationState");
            CalStep step = w.calStep(secondary);
            if (step == CalStep::Complete)
                (secondary ? w.secondaryCalibrated : w.primaryCalibrated) = true;
            return step == CalStep::Fail;
        }
        case Action::GuidingStarted:
            w.Record("Frame.NotifyGuidingStarted");
            w.Record("UpdateLockPosShiftCameraCoords");
            w.Record("CheckCalibrationAutoLoad");
            break;
        case Action::RecenterStep:
        {
            double sx = w.recenterRemainingX < w.recenterStepX ? w.recenterRemainingX : w.recenterStepX;
            double sy = w.recenterRemainingY < w.recenterStepY ? w.recenterRemainingY : w.recenterStepY;
            char buf[200];
            std::snprintf(buf, sizeof buf, "D:dither recenter: remaining=(%.1f,%.1f) step=(%.1f,%.1f)\n",
                          w.recenterRemainingX * w.recenterDirX, w.recenterRemainingY * w.recenterDirY, sx * w.recenterDirX,
                          sy * w.recenterDirY);
            w.Record(buf);
            w.Record("Primary.TransformMountCoordinatesToCameraCoordinates");
            ++w.recenterStepsPending;
            guide_move_plan::RecenterProgress ifWhole =
                guide_move_plan::AfterRecenterStep(w.recenterRemainingX, w.recenterRemainingY, sx, sy, 0);
            w.recenteringForWorker = !ifWhole.done;
            w.Record("Frame.SchedulePrimaryMove(RECOVERY)");
            w.Record("Primary.NotifyDirectMove");
            break;
        }
        case Action::BacklashStep:
            w.Record("GA.NotifyBacklashStep");
            break;
        case Action::GuideStep:
            w.Record("Frame.SchedulePrimaryMove(GUIDE_STEP)");
            break;
        case Action::RecenterOver:
            w.recenteringForWorker = false;
            break;
        }
        return false;
    }

private:
    World& m_w;
};

// What Guider::UpdateGuideState does with the state machine for a frame: the star lost, paused,
// or found (with the status and the ERROR_INFO line of a failed calibration). Returns the status
// message it sets, "" when none.
inline std::string Frame(World& w, bool starFound, bool paused)
{
    Host host(w);
    if (!starFound)
    {
        guider_state_machine::OnStarLost(host);
        return "lost";
    }
    if (paused)
    {
        guider_state_machine::OnPausedFrame(host);
        return "Paused";
    }
    switch (guider_state_machine::OnStarFound(host))
    {
    case guider_state_machine::FrameResult::Done:
        break;
    case guider_state_machine::FrameResult::PrimaryCalibrationFailed:
        w.Record("D:E:Calibration failed");
        return w.primaryIsStepGuider ? "AO calibration failed" : "calibration failed";
    case guider_state_machine::FrameResult::SecondaryCalibrationFailed:
        w.Record("D:E:Calibration failed");
        return "calibration failed";
    }
    return "";
}

inline void SetState(World& w, GUIDER_STATE st)
{
    Host host(w);
    guider_state_machine::SetState(host, st);
}

inline void StopGuiding(World& w)
{
    Host host(w);
    guider_state_machine::StopGuiding(host);
}

} // namespace sm_test

#endif
