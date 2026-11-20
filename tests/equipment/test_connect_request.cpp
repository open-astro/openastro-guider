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
// request now begins a session, and EndModal takes the scale change once.

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
    latch.camChanged = true;
    latch.warningIssued = true;
    EXPECT_TRUE(latch.TakeScaleChange());

    // the reconnect: the profile now holds the driver's pixel size
    latch.Begin();
    EXPECT_FALSE(latch.camChanged); // else EndModal clears the calibration and never auto-loads one
    EXPECT_FALSE(latch.warningIssued); // else a later real camera change is never noticed
    latch.NotePixelSize(3.75, 3.75);
    EXPECT_FALSE(latch.ScaleChanged());
    EXPECT_FALSE(latch.TakeScaleChange());
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
    latch.Begin();
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
