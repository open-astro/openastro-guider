// restore_calibration (#102).
//
// guide with recalibrate:true erases the mount's calibration before it
// calibrates; restore_calibration puts back the one saved in the profile.
// When the calibration failed, the guide request is left waiting for it, and
// restore_calibration ends that request before restoring so the client can
// guide again. The decision lives in restore_calibration.cpp, with the
// clear_calibration refusal rule (#137).

#include <gtest/gtest.h>

#include "restore_calibration.h"

namespace
{
// a scope-only rig, connected, calibration saved, nothing in flight
RestoreCalibrationState Ready()
{
    RestoreCalibrationState s;
    s.calibratingOrGuiding = false;
    s.primaryIsAo = false;
    s.primaryConnected = true;
    s.secondaryPresent = false;
    s.secondaryConnected = false;
    s.primarySaved = true;
    s.secondarySaved = false;
    s.controllerIdle = true;
    s.controllerWaitingForCalibration = false;
    return s;
}

// an AO rig: the AO is the primary mount and the scope the secondary
RestoreCalibrationState ReadyAo()
{
    RestoreCalibrationState s = Ready();
    s.primaryIsAo = true;
    s.secondaryPresent = true;
    s.secondaryConnected = true;
    s.secondarySaved = true;
    return s;
}

// the guide request left behind by a failed calibration
RestoreCalibrationState StaleGuide(RestoreCalibrationState s)
{
    s.controllerIdle = false;
    s.controllerWaitingForCalibration = true;
    return s;
}
} // namespace

TEST(RestoreCalibration, RestoresWhenIdle)
{
    RestoreCalibrationDecision d = DecideRestoreCalibration(Ready());
    EXPECT_EQ(d.action, RestoreCalibrationAction::Restore);
    EXPECT_EQ(d.error, "");

    EXPECT_EQ(DecideRestoreCalibration(ReadyAo()).action, RestoreCalibrationAction::Restore);
}

TEST(RestoreCalibration, EndsTheStaleGuideRequestFirst)
{
    // CalibrationFailed -> restore_calibration -> guide, with no stop_capture
    EXPECT_EQ(DecideRestoreCalibration(StaleGuide(Ready())).action, RestoreCalibrationAction::EndStaleGuideThenRestore);
    EXPECT_EQ(DecideRestoreCalibration(StaleGuide(ReadyAo())).action, RestoreCalibrationAction::EndStaleGuideThenRestore);
    EXPECT_STREQ(RESTORE_CALIBRATION_ABORT_REASON, "calibration failed; saved calibration restored");
}

TEST(RestoreCalibration, RefusedWhileCalibratingOrGuiding)
{
    RestoreCalibrationState s = Ready();
    s.calibratingOrGuiding = true;
    RestoreCalibrationDecision d = DecideRestoreCalibration(s);
    EXPECT_EQ(d.action, RestoreCalibrationAction::Refuse);
    EXPECT_EQ(d.error, "cannot restore calibration while calibrating or guiding");

    // a guide request waiting on a calibration that is still running is not stale
    d = DecideRestoreCalibration(StaleGuide(s));
    EXPECT_EQ(d.action, RestoreCalibrationAction::Refuse);
    EXPECT_EQ(d.error, "cannot restore calibration while calibrating or guiding");
}

TEST(RestoreCalibration, RefusedWhenAMountIsNotConnected)
{
    RestoreCalibrationState s = Ready();
    s.primaryConnected = false; // also no mount selected at all
    RestoreCalibrationDecision d = DecideRestoreCalibration(s);
    EXPECT_EQ(d.action, RestoreCalibrationAction::Refuse);
    EXPECT_EQ(d.error, "cannot restore calibration: mount not connected");

    s = ReadyAo();
    s.primaryConnected = false;
    d = DecideRestoreCalibration(s);
    EXPECT_EQ(d.action, RestoreCalibrationAction::Refuse);
    EXPECT_EQ(d.error, "cannot restore calibration: AO not connected");

    s = ReadyAo();
    s.secondaryConnected = false;
    d = DecideRestoreCalibration(s);
    EXPECT_EQ(d.action, RestoreCalibrationAction::Refuse);
    EXPECT_EQ(d.error, "cannot restore calibration: mount not connected");

    // refused before the saved-calibration check: an error, not "nothing saved"
    s = Ready();
    s.primaryConnected = false;
    s.primarySaved = false;
    EXPECT_EQ(DecideRestoreCalibration(s).action, RestoreCalibrationAction::Refuse);

    // and the stale request is not ended
    s = StaleGuide(Ready());
    s.primaryConnected = false;
    EXPECT_EQ(DecideRestoreCalibration(s).action, RestoreCalibrationAction::Refuse);
}

TEST(RestoreCalibration, RefusedWhileAnotherRequestIsInFlight)
{
    // e.g. a guide request still selecting a star: the client's, not stale
    RestoreCalibrationState s = Ready();
    s.controllerIdle = false;
    s.controllerWaitingForCalibration = false;
    RestoreCalibrationDecision d = DecideRestoreCalibration(s);
    EXPECT_EQ(d.action, RestoreCalibrationAction::Refuse);
    EXPECT_EQ(d.error, "cannot restore calibration while a guide or dither request is in progress");

    // refused even when nothing is saved
    s.primarySaved = false;
    EXPECT_EQ(DecideRestoreCalibration(s).action, RestoreCalibrationAction::Refuse);
}

TEST(RestoreCalibration, NothingSavedChangesNothing)
{
    RestoreCalibrationState s = Ready();
    s.primarySaved = false;
    RestoreCalibrationDecision d = DecideRestoreCalibration(s);
    EXPECT_EQ(d.action, RestoreCalibrationAction::NothingSaved);
    EXPECT_EQ(d.error, "");

    // the stale guide request is left alone too
    EXPECT_EQ(DecideRestoreCalibration(StaleGuide(s)).action, RestoreCalibrationAction::NothingSaved);
}

TEST(RestoreCalibration, NoPartialRestoreWithAnAo)
{
    RestoreCalibrationState s = ReadyAo();
    s.secondarySaved = false; // AO saved, scope not
    EXPECT_EQ(DecideRestoreCalibration(s).action, RestoreCalibrationAction::NothingSaved);
    EXPECT_EQ(DecideRestoreCalibration(StaleGuide(s)).action, RestoreCalibrationAction::NothingSaved);

    s = ReadyAo();
    s.primarySaved = false; // scope saved, AO not
    EXPECT_EQ(DecideRestoreCalibration(s).action, RestoreCalibrationAction::NothingSaved);
}

TEST(RestoreCalibration, SecondarySavedFlagIgnoredWithoutASecondaryMount)
{
    RestoreCalibrationState s = Ready();
    s.secondarySaved = false;
    s.secondaryConnected = false;
    EXPECT_EQ(DecideRestoreCalibration(s).action, RestoreCalibrationAction::Restore);
}

// clear_calibration (#137): a calibration cleared while it runs has no next step, so the
// guider used to abort on it (assert) or stay in Calibrating forever (NDEBUG)
TEST(ClearCalibration, RefusedWhileCalibrating)
{
    const char *refusal = ClearCalibrationRefusal(true);
    ASSERT_NE(refusal, nullptr);
    EXPECT_STREQ(refusal, "cannot clear calibration while calibrating");
}

TEST(ClearCalibration, AllowedOtherwiseIncludingWhileGuiding)
{
    // not calibrating: stopped, looping, star selected, or guiding (as in PHD2, guiding keeps the
    // calibration it started with and the next guide start calibrates again)
    EXPECT_EQ(ClearCalibrationRefusal(false), nullptr);
}
