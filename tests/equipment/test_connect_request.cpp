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
