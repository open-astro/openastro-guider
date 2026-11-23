// guider_state_machine (#142, slice T2c of #115 steps 4-9).
//
// The guider's state machine as Guider::SetState, StopGuiding, UpdateGuideState and
// MoveLockPosition run it, pinned before step 5 turns Guider into a plain class: STOP remapped per
// state, the one-state-ahead rule, what entering each state does, star lost per state, paused
// frames, the calibration's progress to guiding, the #255 return to calibrating, and the dither's
// edge reflection and fast recenter. A recalibration always goes through CALIBRATING_PRIMARY.
// The rules live in src/guider_state_machine.cpp; Guider performs the actions (guider.cpp,
// GuiderStateHost). Built with NDEBUG, like the shipped daemon, so the paths past an assert are
// pinned as they run in the field.

#include <gtest/gtest.h>

#include "guider_sm_host.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using guider_state_machine::Action;
using sm_test::CalStep;
using sm_test::World;

namespace
{
const GUIDER_STATE kStates[] = { STATE_UNINITIALIZED,         STATE_SELECTING,  STATE_SELECTED, STATE_CALIBRATING_PRIMARY,
                                 STATE_CALIBRATING_SECONDARY, STATE_CALIBRATED, STATE_GUIDING };

World At(GUIDER_STATE st)
{
    World w;
    w.state = st;
    return w;
}

bool Contains(const std::vector<Action>& v, Action a)
{
    return std::find(v.begin(), v.end(), a) != v.end();
}

// a World with a calibrated mount and the star selected
World Selected()
{
    World w = At(STATE_SELECTED);
    w.primaryCalibrated = true;
    w.lockValid = true;
    return w;
}
} // namespace

TEST(GuiderStateMachine, StateNames)
{
    EXPECT_STREQ(guider_state_machine::StateName(STATE_UNINITIALIZED), "UNINITIALIZED");
    EXPECT_STREQ(guider_state_machine::StateName(STATE_SELECTING), "SELECTING");
    EXPECT_STREQ(guider_state_machine::StateName(STATE_SELECTED), "SELECTED");
    EXPECT_STREQ(guider_state_machine::StateName(STATE_CALIBRATING_PRIMARY), "CALIBRATING_PRIMARY");
    EXPECT_STREQ(guider_state_machine::StateName(STATE_CALIBRATING_SECONDARY), "CALIBRATING_SECONDARY");
    EXPECT_STREQ(guider_state_machine::StateName(STATE_CALIBRATED), "CALIBRATED");
    EXPECT_STREQ(guider_state_machine::StateName(STATE_GUIDING), "GUIDING");
    EXPECT_STREQ(guider_state_machine::StateName(STATE_STOP), "STOP");
    EXPECT_STREQ(guider_state_machine::StateName(static_cast<GUIDER_STATE>(42)), "??");
}

// --- SetState ---

TEST(GuiderStateMachine, ARequestMoreThanOneStateAheadIsRefused)
{
    for (GUIDER_STATE from : kStates)
        for (GUIDER_STATE to : kStates)
        {
            World w = At(from);
            w.primaryCalibrated = true;
            sm_test::SetState(w, to);
            if (to > from + 1)
            {
                EXPECT_EQ(w.state, from) << from << "->" << to;
                EXPECT_TRUE(w.states.empty());
                EXPECT_EQ(w.actions, std::vector<Action>({ Action::IllegalTransition }));
                ASSERT_EQ(w.trace.size(), 3u);
                EXPECT_EQ(w.trace[1],
                          std::string("D:Cannot transition from ") + guider_state_machine::StateName(from) +
                              " to newState=" + guider_state_machine::StateName(to) + "\n");
            }
            else
            {
                EXPECT_FALSE(Contains(w.actions, Action::IllegalTransition)) << from << "->" << to;
                EXPECT_FALSE(w.states.empty());
            }
        }
}

TEST(GuiderStateMachine, UninitializedInvalidatesThePositionsAndEndsSelecting)
{
    World w = Selected();
    sm_test::SetState(w, STATE_UNINITIALIZED);
    EXPECT_EQ(w.state, STATE_SELECTING);
    EXPECT_EQ(w.states, std::vector<GUIDER_STATE>({ STATE_SELECTING }));
    EXPECT_EQ(w.actions, std::vector<Action>({ Action::InvalidateLockPosition, Action::InvalidateCurrentPosition }));
    EXPECT_FALSE(w.lockValid);
    EXPECT_EQ(w.trace,
              std::vector<std::string>({ "D:Changing from state SELECTED to UNINITIALIZED\n", "Evt.LockPositionLost",
                                         "InvalidateCurrentPosition", "D:guider state => SELECTING\n" }));
}

TEST(GuiderStateMachine, CalibratingAnUncalibratedMountBeginsItsCalibration)
{
    World w = Selected();
    w.primaryCalibrated = false;
    sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.actions,
              std::vector<Action>(
                  { Action::ResetErrorCountPrimary, Action::BeginCalibrationPrimary, Action::CalibrationStartedPrimary }));
}

TEST(GuiderStateMachine, ABeginCalibrationFailureStartsOver)
{
    World w = Selected();
    w.primaryCalibrated = false;
    w.beginFails = [](bool) { return true; };
    sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.state, STATE_SELECTING);
    EXPECT_EQ(w.states, std::vector<GUIDER_STATE>({ STATE_SELECTING }));
    EXPECT_EQ(w.actions,
              std::vector<Action>({ Action::ResetErrorCountPrimary, Action::BeginCalibrationPrimary,
                                    Action::BeginCalibrationFailedPrimary, Action::InvalidateLockPosition,
                                    Action::InvalidateCurrentPosition }));
    EXPECT_NE(std::find(w.trace.begin(), w.trace.end(), "D:SetState recurses newState UNINITIALIZED\n"), w.trace.end());
}

TEST(GuiderStateMachine, ACalibratedMountSkipsToCalibrated)
{
    // StartGuiding with a calibration: CALIBRATING_PRIMARY asked, CALIBRATED set, nothing begun
    World w = Selected();
    sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.state, STATE_CALIBRATED);
    EXPECT_TRUE(w.actions.empty());

    // a secondary that is not connected does not count
    World w2 = Selected();
    w2.secondaryExists = true;
    sm_test::SetState(w2, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w2.state, STATE_CALIBRATED);
}

TEST(GuiderStateMachine, ACalibratedSecondaryTakesAFrameMore)
{
    // Pinned as it is (inherited PHD2 code): with a connected, calibrated secondary (an AO), the
    // fall-through from CALIBRATING_PRIMARY into the secondary's case sets CALIBRATED only when
    // there is no connected secondary, so the state stays CALIBRATING_PRIMARY; the next frame asks
    // for CALIBRATING_SECONDARY and stops there; the frame after guides, one frame later than
    // without the AO. Nothing is calibrated in between, but a stop in those two frames reports
    // CalibrationFailed.
    World w = Selected();
    w.secondaryExists = w.secondaryConnected = w.secondaryCalibrated = true;
    sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    EXPECT_TRUE(w.actions.empty());
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_CALIBRATING_SECONDARY);
    EXPECT_TRUE(w.actions.empty());
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_GUIDING);
    EXPECT_EQ(w.actions, std::vector<Action>({ Action::EnterGuiding, Action::SetLockToCurrent, Action::GuidingStarted }));
}

TEST(GuiderStateMachine, AnUncalibratedSecondaryIsBegunUnderCalibratingPrimary)
{
    // Pinned as it is (inherited PHD2 code): the primary is calibrated, so CALIBRATING_PRIMARY
    // falls through to the secondary's BeginCalibration but keeps the state it was asked for,
    // CALIBRATING_PRIMARY. The next frame then asks for CALIBRATING_SECONDARY, which begins the
    // secondary's calibration again: two StartCalibration notices for the AO.
    World w = Selected();
    w.secondaryExists = w.secondaryConnected = true;
    sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.actions,
              std::vector<Action>({ Action::ResetErrorCountSecondary, Action::BeginCalibrationSecondary,
                                    Action::CalibrationStartedSecondary }));

    w.actions.clear();
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_CALIBRATING_SECONDARY);
    EXPECT_EQ(w.actions,
              std::vector<Action>({ Action::ResetErrorCountSecondary, Action::BeginCalibrationSecondary,
                                    Action::CalibrationStartedSecondary }));
}

TEST(GuiderStateMachine, EnteringGuidingTakesTheCurrentPositionAsLock)
{
    World w = At(STATE_CALIBRATED);
    w.primaryCalibrated = true;
    sm_test::SetState(w, STATE_GUIDING);
    EXPECT_EQ(w.state, STATE_GUIDING);
    EXPECT_EQ(w.actions, std::vector<Action>({ Action::EnterGuiding, Action::SetLockToCurrent }));
}

TEST(GuiderStateMachine, AStickyLockIsKeptWhenValid)
{
    World w = At(STATE_CALIBRATED);
    w.primaryCalibrated = true;
    w.lockSticky = true;
    w.lockValid = true;
    sm_test::SetState(w, STATE_GUIDING);
    EXPECT_EQ(w.state, STATE_GUIDING);
    EXPECT_EQ(w.actions, std::vector<Action>({ Action::EnterGuiding }));
    EXPECT_NE(std::find(w.trace.begin(), w.trace.end(), "D:keeping sticky lock position\n"), w.trace.end());

    // sticky, but no lock position: the current position
    World w2 = At(STATE_CALIBRATED);
    w2.primaryCalibrated = true;
    w2.lockSticky = true;
    sm_test::SetState(w2, STATE_GUIDING);
    EXPECT_EQ(w2.actions, std::vector<Action>({ Action::EnterGuiding, Action::SetLockToCurrent }));

    // valid but not sticky: the current position
    World w3 = At(STATE_CALIBRATED);
    w3.primaryCalibrated = true;
    w3.lockValid = true;
    sm_test::SetState(w3, STATE_GUIDING);
    EXPECT_EQ(w3.actions, std::vector<Action>({ Action::EnterGuiding, Action::SetLockToCurrent }));
}

TEST(GuiderStateMachine, EnteringGuidingResetsTheFastRecenter)
{
    World w = At(STATE_CALIBRATED);
    w.primaryCalibrated = true;
    w.recenterActive = true;
    w.recenterStepsPending = 2;
    w.recenterStalledSteps = 1;
    w.recenteringForWorker = true;
    sm_test::SetState(w, STATE_GUIDING);
    EXPECT_FALSE(w.recenterActive);
    EXPECT_EQ(w.recenterStepsPending, 0);
    EXPECT_EQ(w.recenterStalledSteps, 0);
    EXPECT_FALSE(w.recenteringForWorker);
}

// #255: AdjustCalibrationForScopePointing cleared the calibration on entering guiding
TEST(GuiderStateMachine, CalibrationClearedOnGuidingStartCalibrates)
{
    World w = At(STATE_CALIBRATED);
    w.primaryCalibrated = true;
    w.adjustClearsPrimary = true;
    sm_test::SetState(w, STATE_GUIDING);
    EXPECT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.states, std::vector<GUIDER_STATE>({ STATE_CALIBRATING_PRIMARY }));
    EXPECT_EQ(w.actions,
              std::vector<Action>({ Action::EnterGuiding, Action::ResetErrorCountPrimary, Action::BeginCalibrationPrimary,
                                    Action::CalibrationStartedPrimary }));
    EXPECT_NE(std::find(w.trace.begin(), w.trace.end(), "D:calibration cleared on guiding start, calibrating\n"),
              w.trace.end());
    EXPECT_NE(std::find(w.trace.begin(), w.trace.end(), "D:SetState recurses newState CALIBRATING_PRIMARY\n"), w.trace.end());
}

TEST(GuiderStateMachine, SecondaryCalibrationClearedOnGuidingStartCalibratesIt)
{
    World w = At(STATE_CALIBRATED);
    w.primaryCalibrated = true;
    w.secondaryExists = w.secondaryConnected = w.secondaryCalibrated = true;
    w.adjustClearsSecondary = true;
    sm_test::SetState(w, STATE_GUIDING);
    EXPECT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.actions,
              std::vector<Action>({ Action::EnterGuiding, Action::ResetErrorCountSecondary, Action::BeginCalibrationSecondary,
                                    Action::CalibrationStartedSecondary }));

    // a disconnected secondary's calibration does not matter
    World w2 = At(STATE_CALIBRATED);
    w2.primaryCalibrated = true;
    w2.secondaryExists = true;
    sm_test::SetState(w2, STATE_GUIDING);
    EXPECT_EQ(w2.state, STATE_GUIDING);
}

TEST(GuiderStateMachine, CalibrationClearedOnGuidingStartFromAFrame)
{
    // the frame that completes the calibration enters guiding; the adjustment clears it (#255)
    World w = At(STATE_CALIBRATING_PRIMARY);
    w.adjustClearsPrimary = true;
    w.calStep = [](bool) { return CalStep::Complete; };
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.states, std::vector<GUIDER_STATE>({ STATE_CALIBRATED, STATE_CALIBRATED, STATE_CALIBRATING_PRIMARY }));
    EXPECT_FALSE(Contains(w.actions, Action::GuidingStarted));
    EXPECT_TRUE(Contains(w.actions, Action::BeginCalibrationPrimary));
}

TEST(GuiderStateMachine, CalibrationClearedOnGuidingStartAndBeginFails)
{
    World w = At(STATE_CALIBRATED);
    w.primaryCalibrated = true;
    w.adjustClearsPrimary = true;
    w.beginFails = [](bool) { return true; };
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_SELECTING);
    EXPECT_FALSE(Contains(w.actions, Action::GuidingStarted));
}

// --- StopGuiding, from each state ---

struct StopCase
{
    GUIDER_STATE from;
    GUIDER_STATE to;
    std::vector<Action> actions; // mounts idle
};

TEST(GuiderStateMachine, StopFromEachState)
{
    const StopCase cases[] = {
        // Pinned: a stop while UNINITIALIZED enters UNINITIALIZED again, so it ends SELECTING
        { STATE_UNINITIALIZED, STATE_SELECTING, { Action::InvalidateLockPosition, Action::InvalidateCurrentPosition } },
        { STATE_SELECTING, STATE_SELECTING, {} },
        { STATE_SELECTED, STATE_SELECTED, {} },
        // the moves of a calibration lose the star position: start over
        { STATE_CALIBRATING_PRIMARY,
          STATE_SELECTING,
          { Action::NotifyCalibrationFailedPrimary, Action::NotifyGuidingStopped, Action::InvalidateLockPosition,
            Action::InvalidateCurrentPosition } },
        { STATE_CALIBRATING_SECONDARY,
          STATE_SELECTING,
          { Action::NotifyCalibrationFailedSecondary, Action::NotifyGuidingStopped, Action::InvalidateLockPosition,
            Action::InvalidateCurrentPosition } },
        // Pinned: CALIBRATED notifies CalibrationFailed although the calibration completed. It is
        // only seen at rest after a StartGuiding while guiding (or a #255 begin that failed)
        { STATE_CALIBRATED, STATE_SELECTED, { Action::NotifyCalibrationFailedPrimary, Action::NotifyGuidingStopped } },
        { STATE_GUIDING, STATE_SELECTED, { Action::NotifyGuidingStopped } },
    };
    for (const StopCase& c : cases)
    {
        World w = At(c.from);
        w.lockValid = true;
        sm_test::StopGuiding(w);
        EXPECT_EQ(w.state, c.to) << c.from;
        EXPECT_EQ(w.actions, c.actions) << c.from;
        EXPECT_FALSE(Contains(w.actions, Action::IllegalTransition));
        // the request is STOP, remapped
        EXPECT_NE(std::find(w.trace.begin(), w.trace.end(),
                            std::string("D:Changing from state ") + guider_state_machine::StateName(c.from) + " to STOP\n"),
                  w.trace.end())
            << c.from;

        // a busy mount: GuidingStopped comes after its last guide step, not here
        World busy = At(c.from);
        busy.lockValid = true;
        busy.primaryBusy = true;
        sm_test::StopGuiding(busy);
        EXPECT_EQ(busy.state, c.to);
        EXPECT_FALSE(Contains(busy.actions, Action::NotifyGuidingStopped)) << c.from;

        World busy2 = At(c.from);
        busy2.secondaryExists = true;
        busy2.secondaryBusy = true;
        sm_test::StopGuiding(busy2);
        EXPECT_FALSE(Contains(busy2.actions, Action::NotifyGuidingStopped)) << c.from;
    }
}

TEST(GuiderStateMachine, StopReportsTheCaptureFailureAsTheReason)
{
    World w = At(STATE_CALIBRATING_PRIMARY);
    sm_test::StopGuiding(w);
    EXPECT_EQ(w.trace[0], "Evt.CalibrationFailed(Primary,Calibration manually stopped)");

    World w2 = At(STATE_CALIBRATING_SECONDARY);
    w2.secondaryExists = w2.secondaryConnected = true;
    w2.captureFailureReason = "camera lost";
    sm_test::StopGuiding(w2);
    EXPECT_EQ(w2.trace[0], "Evt.CalibrationFailed(Secondary,camera lost)");
}

TEST(GuiderStateMachine, StopThenStartGuidingAgain)
{
    // stopped from guiding, the star is still selected: guiding resumes on the same calibration
    World w = At(STATE_GUIDING);
    w.primaryCalibrated = true;
    w.lockValid = true;
    sm_test::StopGuiding(w);
    ASSERT_EQ(w.state, STATE_SELECTED);
    sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.state, STATE_CALIBRATED);
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_GUIDING);
}

// --- a frame with no star ---

TEST(GuiderStateMachine, StarLostPerState)
{
    struct Case
    {
        GUIDER_STATE from;
        GUIDER_STATE to;
        std::vector<Action> actions;
    };
    const Case cases[] = {
        { STATE_UNINITIALIZED, STATE_UNINITIALIZED, { Action::NotifyLoopingNoStar } },
        { STATE_SELECTING, STATE_SELECTING, { Action::NotifyLoopingNoStar } },
        // the selected star is gone: the lock position too
        { STATE_SELECTED,
          STATE_SELECTING,
          { Action::NotifyLoopingNoStar, Action::InvalidateLockPosition, Action::InvalidateCurrentPosition,
            Action::NotifyStarLost } },
        // the calibration blunders on
        { STATE_CALIBRATING_PRIMARY,
          STATE_CALIBRATING_PRIMARY,
          { Action::CalibrationFrameDropped, Action::NotifyStarLost, Action::StatusStarLost } },
        { STATE_CALIBRATING_SECONDARY,
          STATE_CALIBRATING_SECONDARY,
          { Action::CalibrationFrameDropped, Action::NotifyStarLost, Action::StatusStarLost } },
        { STATE_CALIBRATED, STATE_CALIBRATED, {} },
        // guiding goes on with a deduced move
        { STATE_GUIDING,
          STATE_GUIDING,
          { Action::GuidingFrameDropped, Action::NotifyStarLost, Action::RecordDroppedFrame, Action::DeducedMove,
            Action::LostStarAlert } },
    };
    for (const Case& c : cases)
    {
        World w = At(c.from);
        w.lockValid = true;
        sm_test::Frame(w, false, false);
        EXPECT_EQ(w.state, c.to) << c.from;
        EXPECT_EQ(w.actions, c.actions) << c.from;
    }

    World w = At(STATE_SELECTED);
    w.lockValid = true;
    sm_test::Frame(w, false, false);
    EXPECT_EQ(w.trace,
              std::vector<std::string>({ "Evt.Looping(no star)", "D:Changing from state SELECTED to UNINITIALIZED\n",
                                         "Evt.LockPositionLost", "InvalidateCurrentPosition", "D:guider state => SELECTING\n",
                                         "Evt.StarLost" }));
}

// --- a frame while paused ---

TEST(GuiderStateMachine, PausedGivesOnlyADeducedMoveWhileGuiding)
{
    for (GUIDER_STATE st : kStates)
    {
        World w = At(st);
        w.primaryCalibrated = true;
        w.calStep = [](bool) { return CalStep::Complete; };
        sm_test::Frame(w, true, true);
        EXPECT_EQ(w.state, st);
        if (st == STATE_GUIDING)
            EXPECT_EQ(w.actions, std::vector<Action>({ Action::DeducedMove }));
        else
            EXPECT_TRUE(w.actions.empty()) << st;
    }
}

// --- a frame with the star ---

TEST(GuiderStateMachine, SelectingSelectsTheStar)
{
    World w = At(STATE_SELECTING);
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_SELECTED);
    EXPECT_EQ(w.actions, std::vector<Action>({ Action::LockOnSelectedStar, Action::NotifyStarSelected }));
    EXPECT_EQ(w.trace[1], "D:CurrentPosition() valid, moving to STATE_SELECTED\n");

    World s = Selected();
    sm_test::Frame(s, true, false);
    EXPECT_EQ(s.state, STATE_SELECTED);
    EXPECT_TRUE(s.actions.empty());

    World u = At(STATE_UNINITIALIZED);
    sm_test::Frame(u, true, false);
    EXPECT_EQ(u.state, STATE_UNINITIALIZED);
    EXPECT_TRUE(u.actions.empty());
}

TEST(GuiderStateMachine, CalibrationContinuesUntilComplete)
{
    World w = At(STATE_CALIBRATING_PRIMARY);
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    EXPECT_EQ(w.actions, std::vector<Action>({ Action::UpdateCalibrationPrimary }));
}

TEST(GuiderStateMachine, ACompletedCalibrationGuidesInTheSameFrame)
{
    World w = At(STATE_CALIBRATING_PRIMARY);
    w.calStep = [](bool) { return CalStep::Complete; };
    EXPECT_EQ(sm_test::Frame(w, true, false), "");
    EXPECT_EQ(w.state, STATE_GUIDING);
    // CALIBRATING_SECONDARY asked with no secondary sets CALIBRATED, then CALIBRATED, then GUIDING
    EXPECT_EQ(w.states, std::vector<GUIDER_STATE>({ STATE_CALIBRATED, STATE_CALIBRATED, STATE_GUIDING }));
    EXPECT_EQ(w.actions,
              std::vector<Action>({ Action::UpdateCalibrationPrimary, Action::EnterGuiding, Action::SetLockToCurrent,
                                    Action::GuidingStarted }));
}

TEST(GuiderStateMachine, AFailedCalibrationStartsOver)
{
    World w = At(STATE_CALIBRATING_PRIMARY);
    w.lockValid = true;
    w.calStep = [](bool) { return CalStep::Fail; };
    EXPECT_EQ(sm_test::Frame(w, true, false), "calibration failed");
    EXPECT_EQ(w.state, STATE_SELECTING);
    EXPECT_FALSE(w.lockValid);
    EXPECT_EQ(w.trace.back(), "D:E:Calibration failed");

    World ao = At(STATE_CALIBRATING_PRIMARY);
    ao.primaryIsStepGuider = true;
    ao.calStep = [](bool) { return CalStep::Fail; };
    EXPECT_EQ(sm_test::Frame(ao, true, false), "AO calibration failed");

    World sec = At(STATE_CALIBRATING_SECONDARY);
    sec.primaryCalibrated = true;
    sec.secondaryExists = sec.secondaryConnected = true;
    sec.calStep = [](bool) { return CalStep::Fail; };
    EXPECT_EQ(sm_test::Frame(sec, true, false), "calibration failed");
    EXPECT_EQ(sec.state, STATE_SELECTING);
    EXPECT_EQ(sec.actions.front(), Action::UpdateCalibrationSecondary);
}

TEST(GuiderStateMachine, PrimaryThenSecondaryCalibration)
{
    World w = Selected();
    w.primaryCalibrated = false;
    w.secondaryExists = w.secondaryConnected = true;
    int frames = 0;
    w.calStep = [&frames](bool) { return frames >= 2 ? CalStep::Complete : CalStep::Continue; };
    sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);
    ASSERT_EQ(w.state, STATE_CALIBRATING_PRIMARY);
    for (; frames < 3; frames++)
        sm_test::Frame(w, true, false);
    // the primary completed on the third frame; the secondary is begun and waits for a frame
    EXPECT_EQ(w.state, STATE_CALIBRATING_SECONDARY);
    EXPECT_TRUE(w.primaryCalibrated);
    EXPECT_FALSE(Contains(w.actions, Action::UpdateCalibrationSecondary));
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_GUIDING);
    EXPECT_TRUE(w.secondaryCalibrated);
    EXPECT_EQ(w.actions.back(), Action::GuidingStarted);
}

TEST(GuiderStateMachine, ASecondaryBeginFailureStillUpdatesItsCalibration)
{
    // Pinned as it is (inherited PHD2 code): when the frame that completes the primary's
    // calibration asks for CALIBRATING_SECONDARY and the secondary's BeginCalibration fails, the
    // state goes back to SELECTING but UpdateGuideState falls through into CALIBRATING_SECONDARY
    // and updates the secondary's calibration anyway, on a calibration that never began.
    World w = At(STATE_CALIBRATING_PRIMARY);
    w.secondaryExists = w.secondaryConnected = true;
    w.beginFails = [](bool secondary) { return secondary; };
    w.calStep = [](bool secondary) { return secondary ? CalStep::Continue : CalStep::Complete; };
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.state, STATE_SELECTING);
    EXPECT_EQ(w.actions,
              std::vector<Action>({ Action::UpdateCalibrationPrimary, Action::ResetErrorCountSecondary,
                                    Action::BeginCalibrationSecondary, Action::BeginCalibrationFailedSecondary,
                                    Action::InvalidateLockPosition, Action::InvalidateCurrentPosition,
                                    Action::UpdateCalibrationSecondary }));

    // and when that update says calibrated: CALIBRATED and GUIDING are refused from SELECTING
    // (the assert that the state is CALIBRATED is off in the shipped build)
    World w2 = At(STATE_CALIBRATING_PRIMARY);
    w2.secondaryExists = w2.secondaryConnected = true;
    w2.beginFails = [](bool secondary) { return secondary; };
    w2.calStep = [](bool) { return CalStep::Complete; };
    sm_test::Frame(w2, true, false);
    EXPECT_EQ(w2.state, STATE_SELECTING);
    EXPECT_EQ(std::count(w2.actions.begin(), w2.actions.end(), Action::IllegalTransition), 2);
    EXPECT_FALSE(Contains(w2.actions, Action::GuidingStarted));
}

TEST(GuiderStateMachine, GuidingFrames)
{
    World w = At(STATE_GUIDING);
    w.primaryCalibrated = true;
    sm_test::Frame(w, true, false);
    EXPECT_EQ(w.actions, std::vector<Action>({ Action::GuideStep, Action::RecenterOver }));

    World m = At(STATE_GUIDING);
    m.measurementMode = true;
    sm_test::Frame(m, true, false);
    EXPECT_EQ(m.actions, std::vector<Action>({ Action::BacklashStep, Action::RecenterOver }));

    // a fast recenter goes before the measurement and the guide step
    World r = At(STATE_GUIDING);
    r.measurementMode = true;
    r.recenterActive = true;
    r.recenterRemainingX = 30;
    r.recenterRemainingY = 40;
    r.recenterStepX = 8.4;
    r.recenterStepY = 11.2;
    sm_test::Frame(r, true, false);
    EXPECT_EQ(r.actions, std::vector<Action>({ Action::RecenterStep }));
    EXPECT_EQ(r.state, STATE_GUIDING);
}

// --- recalibration ---

// After a recalibrate request (the calibration cleared, then StartGuiding), the guider goes through
// CALIBRATING_PRIMARY and begins a calibration before it is CALIBRATED or GUIDING, from every state
// StartGuiding is accepted in and whatever the gear and the frames do. Never skips calibration.
TEST(GuiderStateMachine, ARecalibrationAlwaysCalibrates)
{
    std::mt19937 rng(255);
    std::uniform_int_distribution<int> pct(0, 99);
    int guided = 0, refused = 0, failed = 0;
    for (int run = 0; run < 4000; run++)
    {
        GUIDER_STATE from = kStates[run % 7];
        World w = At(from);
        w.lockValid = from >= STATE_SELECTED;
        w.lockSticky = pct(rng) < 50;
        w.secondaryExists = pct(rng) < 50;
        w.secondaryConnected = w.secondaryExists && pct(rng) < 70;
        w.secondaryCalibrated = pct(rng) < 50;
        w.adjustClearsPrimary = pct(rng) < 10;
        w.beginFails = [&](bool) { return pct(rng) < 5; };
        w.calStep = [&](bool)
        {
            int r = pct(rng);
            return r < 3 ? CalStep::Fail : r < 30 ? CalStep::Complete : CalStep::Continue;
        };

        // guide with recalibrate: the calibration is cleared, then StartGuiding
        w.primaryCalibrated = false;
        if (pct(rng) < 50)
            w.secondaryCalibrated = false;
        sm_test::SetState(w, STATE_CALIBRATING_PRIMARY);

        if (from < STATE_SELECTED)
        {
            // StartGuiding needs a selected star
            EXPECT_EQ(w.state, from);
            EXPECT_EQ(w.actions, std::vector<Action>({ Action::IllegalTransition }));
            refused++;
            continue;
        }

        for (int frame = 0; frame < 60 && w.state != STATE_GUIDING && w.state >= STATE_CALIBRATING_PRIMARY; frame++)
            sm_test::Frame(w, pct(rng) < 90, pct(rng) < 5);

        // the first CALIBRATED or GUIDING comes after CALIBRATING_PRIMARY, a begun calibration and
        // an update that completed it
        auto beyond = std::find_if(w.states.begin(), w.states.end(),
                                   [](GUIDER_STATE s) { return s == STATE_CALIBRATED || s == STATE_GUIDING; });
        if (beyond != w.states.end())
        {
            EXPECT_NE(std::find(w.states.begin(), beyond, STATE_CALIBRATING_PRIMARY), beyond) << run;
            auto begun = std::find(w.actions.begin(), w.actions.end(), Action::BeginCalibrationPrimary);
            auto updated = std::find(w.actions.begin(), w.actions.end(), Action::UpdateCalibrationPrimary);
            EXPECT_NE(begun, w.actions.end()) << run;
            EXPECT_TRUE(updated > begun) << run;
        }
        if (w.state == STATE_GUIDING)
        {
            EXPECT_TRUE(w.primaryCalibrated) << run;
            EXPECT_TRUE(!w.secondaryExists || !w.secondaryConnected || w.secondaryCalibrated) << run;
            guided++;
        }
        else if (w.state == STATE_SELECTING)
            failed++;
    }
    EXPECT_GT(guided, 1000);
    EXPECT_GT(refused, 1000);
    EXPECT_GT(failed, 10);
}

// --- MoveLockPosition: the dither near a frame edge ---

namespace
{
using guider_state_machine::Point;

// a camera rotated by angle against the mount, the lock position valid at least margin from the edges
guider_state_machine::DitherChoice Choose(Point lock, Point mountDelta, double angle = 0, double margin = 10,
                                          int failAtCall = -1, std::vector<std::string> *log = nullptr)
{
    int calls = 0;
    std::vector<std::string> sink;
    return guider_state_machine::ChooseDitherDelta(
        lock, mountDelta, 640, 480,
        [&](const Point& m, Point *c)
        {
            if (calls++ == failAtCall)
                return true;
            c->x = m.x * std::cos(angle) - m.y * std::sin(angle);
            c->y = m.x * std::sin(angle) + m.y * std::cos(angle);
            return false;
        },
        [&](const Point& p) { return p.x >= margin && p.x < 640 - margin && p.y >= margin && p.y < 480 - margin; },
        [&](const std::string& line) { (log ? *log : sink).push_back(line); });
}
} // namespace

TEST(GuiderStateMachine, DitherAwayFromTheEdgesIsAsAsked)
{
    std::vector<std::string> log;
    auto c = Choose({ 320, 240 }, { 5, -3 }, 0, 10, -1, &log);
    EXPECT_TRUE(c.ok);
    EXPECT_DOUBLE_EQ(c.mount.x, 5);
    EXPECT_DOUBLE_EQ(c.mount.y, -3);
    EXPECT_DOUBLE_EQ(c.camera.x, 5);
    EXPECT_DOUBLE_EQ(c.camera.y, -3);
    EXPECT_TRUE(log.empty());
}

TEST(GuiderStateMachine, DitherReflectsAtTheFrameEdge)
{
    // near the right edge: +x would leave the frame, -x is the first variation that fits
    std::vector<std::string> log;
    auto c = Choose({ 625, 240 }, { 8, 3 }, 0, 10, -1, &log);
    EXPECT_TRUE(c.ok);
    EXPECT_DOUBLE_EQ(c.mount.x, -8);
    EXPECT_DOUBLE_EQ(c.mount.y, 3);
    EXPECT_DOUBLE_EQ(c.camera.x, -8);
    EXPECT_EQ(log, std::vector<std::string>({ "dither produces an invalid lock position, try a variation\n" }));

    // near the bottom edge: the y reflection, after x was tried
    log.clear();
    c = Choose({ 320, 465 }, { 8, 6 }, 0, 10, -1, &log);
    EXPECT_DOUBLE_EQ(c.mount.x, 8);
    EXPECT_DOUBLE_EQ(c.mount.y, -6);
    EXPECT_EQ(log.size(), 2u);

    // near the bottom-right corner: both
    log.clear();
    c = Choose({ 625, 465 }, { 8, 6 }, 0, 10, -1, &log);
    EXPECT_DOUBLE_EQ(c.mount.x, -8);
    EXPECT_DOUBLE_EQ(c.mount.y, -6);
    EXPECT_EQ(log.size(), 3u);

    // the reflection is in mount coordinates: a camera at 90 degrees reflects the camera's y for mount x
    c = Choose({ 320, 465 }, { 8, 0 }, std::acos(-1.0) / 2);
    EXPECT_DOUBLE_EQ(c.mount.x, -8);
    EXPECT_NEAR(c.camera.y, -8, 1e-12);
}

TEST(GuiderStateMachine, DitherWithNoValidVariationGoesFarthestFromTheEdge)
{
    // a dither larger than the frame: none is valid; the one ending farthest from the edges
    std::vector<std::string> log;
    auto c = Choose({ 600, 50 }, { 700, 30 }, 0, 10, -1, &log);
    EXPECT_TRUE(c.ok);
    EXPECT_EQ(log.size(), 4u);
    // (1300,80), (-100,80), (1300,20), (-100,20): edge distances -660, -100, -660, -100; the
    // earlier of the two farthest
    EXPECT_DOUBLE_EQ(c.mount.x, -700);
    EXPECT_DOUBLE_EQ(c.mount.y, 30);

    // all equally far: the first (as asked)
    c = Choose({ 320, 240 }, { 1000, 1000 });
    EXPECT_DOUBLE_EQ(c.mount.x, 1000);
    EXPECT_DOUBLE_EQ(c.mount.y, 1000);
}

TEST(GuiderStateMachine, DitherTransformFailure)
{
    // fails on the third variation, after two invalid ones were logged
    std::vector<std::string> log;
    auto c = Choose({ 625, 465 }, { 8, 6 }, 0, 10, 2, &log);
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(log.size(), 2u);

    c = Choose({ 320, 240 }, { 8, 6 }, 0, 10, 0);
    EXPECT_FALSE(c.ok);
}

TEST(GuiderStateMachine, FastRecenterSteps)
{
    // the audit's example: remaining (30,40), max move 20: f = 14/50 = 0.28
    auto p = guider_state_machine::PlanFastRecenter(true, 50, { 30, -40 }, 20);
    EXPECT_TRUE(p.start);
    EXPECT_DOUBLE_EQ(p.remaining.x, 30);
    EXPECT_DOUBLE_EQ(p.remaining.y, 40);
    EXPECT_EQ(p.dirX, -1); // the recenter moves back against the dither
    EXPECT_EQ(p.dirY, 1);
    EXPECT_DOUBLE_EQ(p.step.x, 0.28 * 30);
    EXPECT_DOUBLE_EQ(p.step.y, 0.28 * 40);
    EXPECT_DOUBLE_EQ(std::hypot(p.step.x, p.step.y), 0.7 * 20);

    // fast recenter off
    EXPECT_FALSE(guider_state_machine::PlanFastRecenter(false, 50, { 30, -40 }, 20).start);
}

TEST(GuiderStateMachine, AZeroLengthDitherHasNoFastRecenter)
{
    // a zero-length dither only starts a settle; f would divide by zero
    auto p = guider_state_machine::PlanFastRecenter(true, 0, { 0, 0 }, 20);
    EXPECT_FALSE(p.start);
    EXPECT_TRUE(std::isfinite(p.step.x));
    EXPECT_TRUE(std::isfinite(p.step.y));
}
