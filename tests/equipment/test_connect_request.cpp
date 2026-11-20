// Per-device set_connected / get_connected (#84).
//
// set_connected used to connect camera, mount and rotator as one step, so a
// mount that was switched off kept a client from looping the guide camera.
// It now takes an optional device ("camera", "mount", "rotator", "all"). The
// name parsing and the refuse-while-busy rules live in connect_request.cpp.

#include <gtest/gtest.h>

#include "connect_request.h"

TEST(ConnectRequest, DeviceNames)
{
    GearDevice d;
    EXPECT_TRUE(ParseGearDevice("camera", &d));
    EXPECT_EQ(d, GearDevice::Camera);
    EXPECT_TRUE(ParseGearDevice("mount", &d));
    EXPECT_EQ(d, GearDevice::Mount);
    EXPECT_TRUE(ParseGearDevice("rotator", &d));
    EXPECT_EQ(d, GearDevice::Rotator);
    EXPECT_TRUE(ParseGearDevice("all", &d));
    EXPECT_EQ(d, GearDevice::All);
    EXPECT_TRUE(ParseGearDevice("Camera", &d)); // case-insensitive
    EXPECT_EQ(d, GearDevice::Camera);
    EXPECT_FALSE(ParseGearDevice("scope", &d));
    EXPECT_FALSE(ParseGearDevice("", &d));
}

TEST(ConnectRequest, IdleAllowsEverything)
{
    for (GearDevice d : { GearDevice::All, GearDevice::Camera, GearDevice::Mount, GearDevice::Rotator })
    {
        EXPECT_EQ(ConnectRefusal(d, true, false, false, false), "");
        EXPECT_EQ(ConnectRefusal(d, false, false, false, false), "");
    }
}

TEST(ConnectRequest, MountAndRotatorCanJoinWhileLooping)
{
    // looping: capture active, not calibrating or guiding
    EXPECT_EQ(ConnectRefusal(GearDevice::Mount, true, true, false, false), "");
    EXPECT_EQ(ConnectRefusal(GearDevice::Rotator, true, true, false, false), "");
    EXPECT_EQ(ConnectRefusal(GearDevice::Mount, false, true, false, false), "");
}

TEST(ConnectRequest, CameraAndAllNeedCaptureStopped)
{
    EXPECT_EQ(ConnectRefusal(GearDevice::Camera, true, true, false, false),
              "cannot connect the camera while capture is active");
    EXPECT_EQ(ConnectRefusal(GearDevice::Camera, false, true, false, false),
              "cannot disconnect the camera while capture is active");
    EXPECT_EQ(ConnectRefusal(GearDevice::All, true, true, false, false), "cannot connect equipment when capture is active");
    EXPECT_EQ(ConnectRefusal(GearDevice::All, false, true, false, false), "cannot disconnect equipment while capture active");
}

TEST(ConnectRequest, NothingChangesWhileCalibratingOrGuiding)
{
    EXPECT_EQ(ConnectRefusal(GearDevice::Mount, true, true, true, false),
              "cannot connect the mount while calibrating or guiding");
    EXPECT_EQ(ConnectRefusal(GearDevice::Rotator, false, true, true, false),
              "cannot disconnect the rotator while calibrating or guiding");
}

TEST(ConnectRequest, NothingChangesWhileTheGearDialogIsOpen)
{
    EXPECT_EQ(ConnectRefusal(GearDevice::Mount, true, false, false, true),
              "cannot connect equipment when Connect Equipment dialog is open");
    EXPECT_EQ(ConnectRefusal(GearDevice::All, false, false, false, true),
              "cannot disconnect equipment when Connect Equipment dialog is open");
}

// #139: what one gear session saw of the camera. The headless daemon never ran ShowGearDialog, the only place the
// latch was reset, so a connect that saw a wrong profile pixel size left it set for the rest of the process: every
// later set_connected reset the calibration step and both MinMoves, and raised the pixel-size Alert again. Each gear
// request now begins a session (BeginRequest), and EndModal takes the scale change once. These tests cover the latch
// and the begin decision; that GearDialog calls them is pinned by the fake-Alpaca live test.

TEST(CameraChangeLatch, WrongProfilePixelSizeIsOneScaleChange)
{
    CameraChangeLatch latch;
    latch.Begin();
    latch.NotePixelSize(2.4, 3.75); // the profile said 2.4 um, the driver says 3.75 um
    EXPECT_TRUE(latch.ScaleChanged());
    EXPECT_TRUE(latch.TakeScaleChange()); // the EndModal inside connect all acts on it
    EXPECT_FALSE(latch.ScaleChanged());
    EXPECT_FALSE(latch.TakeScaleChange()); // ConnectAll's own EndModal does not act on it again
}

TEST(CameraChangeLatch, NextRequestBeginsAfresh)
{
    CameraChangeLatch latch;
    latch.Begin();
    latch.NotePixelSize(2.4, 3.75);
    latch.NoteCameraChange();
    latch.warningIssued = true;
    EXPECT_TRUE(latch.TakeScaleChange());
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::Clear);

    // the reconnect: the profile now holds the driver's pixel size
    latch.BeginRequest();
    EXPECT_FALSE(latch.warningIssued); // else a later real camera change is never noticed
    latch.NotePixelSize(3.75, 3.75);
    EXPECT_FALSE(latch.ScaleChanged());
    EXPECT_FALSE(latch.TakeScaleChange());
}

TEST(CameraChangeLatch, MountConnectedInALaterRequestClearsTheCalibration)
{
    // set_connected all while the mount is off: the camera connect sees the change, the mount fails, so that
    // EndModal has no mount to clear
    CameraChangeLatch latch;
    latch.BeginRequest();
    latch.NotePixelSize(2.4, 3.75);
    latch.NoteCameraChange();
    EXPECT_TRUE(latch.TakeScaleChange());
    EXPECT_EQ(latch.TakeCalibrationAction(true, false), GearCalibrationAction::None);

    // the #104 retry, guide's connect or set_connected mount connects it in a request of its own: the saved
    // calibration was made with the old camera, so it is cleared, not auto-loaded
    latch.BeginRequest();
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::Clear);

    // cleared once: a calibration made since is not wiped by a later request, and the old one is never auto-loaded
    latch.BeginRequest();
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::None);
    latch.BeginRequest();
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::None);

    // another camera change is cleared again
    latch.BeginRequest();
    latch.NoteCameraChange();
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::Clear);
}

TEST(CameraChangeLatch, AutoLoadWithoutACameraChange)
{
    CameraChangeLatch latch;
    latch.BeginRequest();
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::AutoLoad);
    EXPECT_EQ(latch.TakeCalibrationAction(true, false), GearCalibrationAction::None);
    EXPECT_EQ(latch.TakeCalibrationAction(false, true), GearCalibrationAction::None);
    // every EndModal with the mount connected loads it again, as before
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::AutoLoad);
}

TEST(CameraChangeLatch, ARunOfTheDialogStartsOver)
{
    // PHD2 behaviour, unchanged: a camera change does not outlive the dialog run that saw it
    CameraChangeLatch latch;
    latch.Begin();
    latch.NoteCameraChange();
    EXPECT_EQ(latch.TakeCalibrationAction(true, false), GearCalibrationAction::None);
    latch.Begin();
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::AutoLoad);

    // the user cancelled at the camera-change warning
    latch.Begin();
    latch.NoteCameraChange();
    latch.CancelCameraChange();
    EXPECT_EQ(latch.TakeCalibrationAction(true, true), GearCalibrationAction::AutoLoad);
}

TEST(CameraChangeLatch, OnlyAnOutermostRequestOutsideTheDialogBeginsASession)
{
    EXPECT_TRUE(BeginsGearRequest(0, false));
    EXPECT_FALSE(BeginsGearRequest(1, false)); // nested in another request, from a nested event loop
    // GUI: an RPC that the open Connect Equipment dialog refuses must not wipe the dialog's scale and camera change
    EXPECT_FALSE(BeginsGearRequest(0, true));
}

TEST(CameraChangeLatch, ChangesAddUpWithinOneSession)
{
    // one run of the dialog: connect (2.4 -> 3.75), disconnect, connect again (3.75 -> 3.75) is still one change
    CameraChangeLatch latch;
    latch.Begin();
    latch.NotePixelSize(2.4, 3.75);
    latch.NotePixelSize(3.75, 3.75);
    EXPECT_NEAR(latch.scaleRatio, 3.75 / 2.4, 1e-9);
    EXPECT_TRUE(latch.ScaleChanged());

    // and a change that is undone within the session is none
    latch.BeginRequest();
    latch.NotePixelSize(3.75, 2.4);
    latch.NotePixelSize(2.4, 3.75);
    EXPECT_FALSE(latch.ScaleChanged());
}

TEST(CameraChangeLatch, UnknownProfilePixelSizeIsNoChange)
{
    CameraChangeLatch latch;
    latch.Begin();
    latch.NotePixelSize(0.0, 3.75); // a new profile has no pixel size yet
    EXPECT_FALSE(latch.ScaleChanged());
}

TEST(CameraChangeLatch, OnePercentThreshold)
{
    CameraChangeLatch latch;
    latch.Begin();
    latch.NotePixelSize(3.75, 3.76); // 0.27%: rounding in a driver
    EXPECT_FALSE(latch.ScaleChanged());
    latch.Begin();
    latch.NotePixelSize(3.75, 3.80); // 1.3%
    EXPECT_TRUE(latch.ScaleChanged());
}
