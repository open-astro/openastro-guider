// The sequencing of a gear request (#115 step 3): GearSession over a recording fake GearPort.
//
// GearDialog's ConnectAll, DisconnectAll, SetDeviceConnected and SetProfile became calls into GearSession
// (src/gear_session.cpp). These tests pin the order of its calls into the port, every error text byte-for-byte (they
// are the set_connected and set_profile RPC errors), what a no-op and a refusal leave alone, the request depth and
// latch session, and the end of request, which now runs once on a full connect all (it ran twice, loading the
// calibration twice).

#include <gtest/gtest.h>

#include "gear_session.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace
{

const GearDevice kDevices[] = { GearDevice::Camera, GearDevice::Mount, GearDevice::Rotator };

struct FakeDevice
{
    bool selected = true;
    bool connected = false;
    bool connectWorks = true; // a connect leaf connects it
    bool disconnectWorks = true;
    std::string reason; // the driver's reason for a failed connect
};

struct FakePort : GearPort
{
    std::map<GearDevice, FakeDevice> dev;
    bool captureActive = false;
    bool calibratingOrGuiding = false;
    bool autoLoad = false;
    std::vector<std::string> calls;
    // run inside a leaf, after it connected or failed (the camera's latch updates, a nested request, ...)
    std::function<void(GearDevice)> onLeaf;

    FakePort()
    {
        for (GearDevice d : kDevices)
            dev[d] = FakeDevice();
    }

    void Record(const std::string& what, GearDevice d) { calls.push_back(what + " " + GearDeviceName(d)); }
    int Count(const std::string& call) const
    {
        int n = 0;
        for (const std::string& c : calls)
            n += c == call;
        return n;
    }

    bool Selected(GearDevice d) const override { return dev.at(d).selected; }
    bool Connected(GearDevice d) const override { return dev.at(d).selected && dev.at(d).connected; }
    bool CaptureActive() const override { return captureActive; }
    bool CalibratingOrGuiding() const override { return calibratingOrGuiding; }
    void ConnectLeaf(GearDevice d) override
    {
        Record("connect", d);
        FakeDevice& f = dev[d];
        if (f.selected && !f.connected && f.connectWorks)
            f.connected = true;
        if (onLeaf)
            onLeaf(d);
    }
    void DisconnectLeaf(GearDevice d) override
    {
        Record("disconnect", d);
        FakeDevice& f = dev[d];
        if (f.selected && f.connected && f.disconnectWorks)
            f.connected = false;
        if (onLeaf)
            onLeaf(d);
    }
    std::string LastConnectError(GearDevice d) const override { return dev.at(d).reason; }
    void OnConnectRequested(GearDevice d) override { Record("requested", d); }
    void NoteUserDisconnect(GearDevice d) override { Record("userDisconnect", d); }
    void Trace(const std::string& line) override { calls.push_back("trace " + line); }
    void RefreshFrameControls() override { calls.push_back("frame"); }
    bool AutoLoadCalibration() const override { return autoLoad; }
    void LoadCalibration() override { calls.push_back("loadCal"); }
    void ClearCalibration() override { calls.push_back("clearCal"); }
    void RefreshAdvancedDialog() override { calls.push_back("advanced"); }
    void HandleImageScaleChange() override { calls.push_back("scale"); }
    void SendReconnected(GearDevice d) override { Record("reconnected", d); }
};

using Calls = std::vector<std::string>;

const Calls kEnd = { "frame", "advanced" };

Calls Concat(Calls a, const Calls& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

struct GearSessionTest : ::testing::Test
{
    FakePort port;
    GearSession session { port };
    std::string error;
};

} // namespace

// ---- connect all ----

TEST_F(GearSessionTest, ConnectAllRunsTheLeavesCameraMountRotatorThenEndsOnce)
{
    port.autoLoad = true;
    EXPECT_FALSE(session.ConnectAll(&error));
    EXPECT_EQ(error, "");
    EXPECT_EQ(port.calls,
              (Calls { "trace gear_dialog: ConnectAll calls OnButtonConnectAll\n", "requested all",
                       "trace gear_dialog: OnButtonConnectAll\n", "connect camera", "connect mount", "connect rotator", "frame",
                       "loadCal", "advanced" }));
}

// The single intended change of the slice: GearDialog ran the end of request once inside OnButtonConnectAll when
// everything connected and again in ConnectAll, so the profile's calibration was loaded twice. It is loaded once.
TEST_F(GearSessionTest, AFullConnectAllEndsOnceAndLoadsTheCalibrationOnce)
{
    port.autoLoad = true;
    EXPECT_FALSE(session.ConnectAll(&error));
    EXPECT_EQ(port.Count("frame"), 1);
    EXPECT_EQ(port.Count("loadCal"), 1);
    EXPECT_EQ(port.Count("advanced"), 1);
}

TEST_F(GearSessionTest, ConnectAllCallsEveryLeafWhateverItsState)
{
    // an unselected camera still has its leaf run (its "Camera Connect Failed" status line), and a connected mount
    // too (its #104 result); the rotator is not selected either
    port.dev[GearDevice::Camera].selected = false;
    port.dev[GearDevice::Mount].connected = true;
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_TRUE(session.ConnectAll(&error));
    EXPECT_EQ(error, "equipment failed to connect: camera");
    EXPECT_EQ(port.Count("connect camera"), 1);
    EXPECT_EQ(port.Count("connect mount"), 1);
    EXPECT_EQ(port.Count("connect rotator"), 1);
    EXPECT_EQ(port.Count("frame"), 1); // a failed connect all ends once too
}

TEST_F(GearSessionTest, ConnectAllNamesEachFailedDeviceWithItsReason)
{
    port.dev[GearDevice::Camera].connectWorks = false;
    port.dev[GearDevice::Camera].reason = "timed out";
    port.dev[GearDevice::Mount].connectWorks = false; // no reason given
    port.dev[GearDevice::Rotator].connectWorks = false;
    port.dev[GearDevice::Rotator].reason = "refused";
    EXPECT_TRUE(session.ConnectAll(&error));
    // 'Rotator' capitalised, as it always was
    EXPECT_EQ(error, "equipment failed to connect: camera (timed out) mount Rotator (refused)");
}

TEST_F(GearSessionTest, ConnectAllFailureOfTheMountAlone)
{
    port.dev[GearDevice::Mount].connectWorks = false;
    port.dev[GearDevice::Mount].reason = "Alpaca error 0x407: not connected";
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_TRUE(session.ConnectAll(&error));
    EXPECT_EQ(error, "equipment failed to connect: mount (Alpaca error 0x407: not connected)");
}

TEST_F(GearSessionTest, ConnectAllWithEverythingConnectedIsANoOp)
{
    for (GearDevice d : kDevices)
        port.dev[d].connected = true;
    EXPECT_FALSE(session.ConnectAll(&error));
    EXPECT_EQ(error, "");
    EXPECT_TRUE(port.calls.empty());

    // the mount and rotator count only when selected
    port.dev[GearDevice::Mount].selected = false;
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_FALSE(session.ConnectAll(&error));
    EXPECT_TRUE(port.calls.empty());
}

TEST_F(GearSessionTest, ConnectAllWithNoCameraIsNeverANoOp)
{
    port.dev[GearDevice::Camera].selected = false;
    port.dev[GearDevice::Mount].connected = true;
    port.dev[GearDevice::Rotator].connected = true;
    EXPECT_TRUE(session.ConnectAll(&error));
    EXPECT_EQ(error, "equipment failed to connect: camera");
    EXPECT_EQ(port.Count("connect camera"), 1);
}

TEST_F(GearSessionTest, ConnectAllIsRefusedWhileCapturing)
{
    port.captureActive = true;
    EXPECT_TRUE(session.ConnectAll(&error));
    EXPECT_EQ(error, "cannot connect equipment when capture is active");
    EXPECT_TRUE(port.calls.empty());
}

// ---- disconnect all ----

TEST_F(GearSessionTest, DisconnectAllRunsTheLeavesMountCameraRotator)
{
    for (GearDevice d : kDevices)
        port.dev[d].connected = true;
    EXPECT_FALSE(session.DisconnectAll(&error));
    EXPECT_EQ(error, "");
    EXPECT_EQ(
        port.calls,
        Concat({ "trace gear_dialog: DisconnectAll calls OnButtonDisconnectAll\n", "trace gear_dialog: OnButtonDisconnectAll\n",
                 "disconnect mount", "disconnect camera", "disconnect rotator", "userDisconnect all" },
               kEnd));
}

TEST_F(GearSessionTest, DisconnectAllCallsEveryLeafWhateverItsState)
{
    // only the camera connected; the mount failed to connect and the rotator is not selected
    port.dev[GearDevice::Camera].connected = true;
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_FALSE(session.DisconnectAll(&error));
    EXPECT_EQ(port.Count("disconnect mount"), 1);
    EXPECT_EQ(port.Count("disconnect camera"), 1);
    EXPECT_EQ(port.Count("disconnect rotator"), 1);
    EXPECT_EQ(port.Count("userDisconnect all"), 1);
}

TEST_F(GearSessionTest, DisconnectAllWithNothingConnectedStillRecordsTheUsersWord)
{
    EXPECT_FALSE(session.DisconnectAll(&error));
    EXPECT_EQ(error, "");
    EXPECT_EQ(port.calls, (Calls { "userDisconnect all" })); // no leaf, no end of request
}

TEST_F(GearSessionTest, DisconnectAllIsRefusedWhileCapturing)
{
    port.dev[GearDevice::Camera].connected = true;
    port.captureActive = true;
    EXPECT_TRUE(session.DisconnectAll(&error));
    EXPECT_EQ(error, "cannot disconnect equipment while capture active");
    EXPECT_TRUE(port.calls.empty());
}

// ---- one device ----

TEST_F(GearSessionTest, SetDeviceAllForwardsToConnectAllAndDisconnectAll)
{
    port.captureActive = true;
    EXPECT_TRUE(session.SetDevice(GearDevice::All, true, &error));
    EXPECT_EQ(error, "cannot connect equipment when capture is active");
    EXPECT_FALSE(session.SetDevice(GearDevice::All, false, &error)); // nothing connected
    EXPECT_EQ(port.calls, (Calls { "userDisconnect all" }));
}

TEST_F(GearSessionTest, SetDeviceConnectsOneDevice)
{
    port.autoLoad = true;
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(error, "");
    // the request is noted before the leaf runs
    EXPECT_EQ(port.calls,
              (Calls { "trace gear_dialog: SetDeviceConnected mount 1\n", "requested mount", "connect mount", "frame",
                       "loadCal", "advanced" }));
}

TEST_F(GearSessionTest, SetDeviceDisconnectsOneDevice)
{
    port.dev[GearDevice::Camera].connected = true;
    EXPECT_FALSE(session.SetDevice(GearDevice::Camera, false, &error));
    // the leaf records the user's disconnect itself (EquipmentManager's leaves do); the session adds none
    EXPECT_EQ(port.calls, Concat({ "trace gear_dialog: SetDeviceConnected camera 0\n", "disconnect camera" }, kEnd));
}

TEST_F(GearSessionTest, SetDeviceConnectOfAnUnselectedDeviceIsAnError)
{
    port.dev[GearDevice::Mount].selected = false;
    EXPECT_TRUE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(error, "no mount is selected in the profile");
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_TRUE(session.SetDevice(GearDevice::Rotator, true, &error));
    EXPECT_EQ(error, "no rotator is selected in the profile");
    port.dev[GearDevice::Camera].selected = false;
    EXPECT_TRUE(session.SetDevice(GearDevice::Camera, true, &error));
    EXPECT_EQ(error, "no camera is selected in the profile");
    EXPECT_TRUE(port.calls.empty());
}

TEST_F(GearSessionTest, SetDeviceDisconnectOfAnUnselectedDeviceRecordsNothing)
{
    // returns before the #104 user-disconnect note: only an already-disconnected selected device records it
    port.dev[GearDevice::Mount].selected = false;
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, false, &error));
    EXPECT_EQ(error, "");
    EXPECT_TRUE(port.calls.empty());
}

TEST_F(GearSessionTest, SetDeviceAlreadyInTheRequestedState)
{
    port.dev[GearDevice::Mount].connected = true;
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_TRUE(port.calls.empty());

    // a disconnect of a device that is not connected (one that failed to connect) still means "leave it
    // disconnected" (#104), with no leaf and no end of request
    EXPECT_FALSE(session.SetDevice(GearDevice::Camera, false, &error));
    EXPECT_EQ(error, "");
    EXPECT_EQ(port.calls, (Calls { "userDisconnect camera" }));
}

TEST_F(GearSessionTest, SetDeviceRefusalsCallNoLeafAndNoEnd)
{
    port.captureActive = true;
    EXPECT_TRUE(session.SetDevice(GearDevice::Camera, true, &error));
    EXPECT_EQ(error, "cannot connect the camera while capture is active");
    port.dev[GearDevice::Camera].connected = true;
    EXPECT_TRUE(session.SetDevice(GearDevice::Camera, false, &error));
    EXPECT_EQ(error, "cannot disconnect the camera while capture is active");

    // the mount and rotator may join while looping, but not while calibrating or guiding
    port.calibratingOrGuiding = true;
    EXPECT_TRUE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(error, "cannot connect the mount while calibrating or guiding");
    port.dev[GearDevice::Rotator].connected = true;
    EXPECT_TRUE(session.SetDevice(GearDevice::Rotator, false, &error));
    EXPECT_EQ(error, "cannot disconnect the rotator while calibrating or guiding");
    EXPECT_TRUE(port.calls.empty());
}

TEST_F(GearSessionTest, SetDeviceMountMayJoinWhileLooping)
{
    port.captureActive = true;
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(port.Count("connect mount"), 1);
}

TEST_F(GearSessionTest, SetDeviceFailureTexts)
{
    port.dev[GearDevice::Mount].connectWorks = false;
    port.dev[GearDevice::Mount].reason = "Alpaca error 0x407: not connected";
    EXPECT_TRUE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(error, "mount failed to connect (Alpaca error 0x407: not connected)");
    EXPECT_EQ(port.Count("frame"), 1); // a failed connect still ends the request

    port.dev[GearDevice::Rotator].connectWorks = false;
    EXPECT_TRUE(session.SetDevice(GearDevice::Rotator, true, &error));
    EXPECT_EQ(error, "rotator failed to connect");

    // a disconnect that did not happen (a dark build holds the camera) has no reason, even with one recorded
    port.dev[GearDevice::Camera].connected = true;
    port.dev[GearDevice::Camera].disconnectWorks = false;
    port.dev[GearDevice::Camera].reason = "stale";
    EXPECT_TRUE(session.SetDevice(GearDevice::Camera, false, &error));
    EXPECT_EQ(error, "camera failed to disconnect");
}

// ---- the request depth and the latch session (#139) ----

TEST_F(GearSessionTest, TheSessionBeginsBeforeTheNoOpAndRefusalChecks)
{
    // what an earlier request left (a #105 reconnect has no request of its own) is reset by any request that is
    // entered, even one that then does nothing
    auto dirty = [this]()
    {
        session.Latch().warningIssued = true;
        session.Latch().NotePixelSize(3.75, 2.9);
    };

    dirty();
    port.dev[GearDevice::Camera].connected = true;
    port.dev[GearDevice::Mount].connected = true;
    port.dev[GearDevice::Rotator].connected = true;
    EXPECT_FALSE(session.ConnectAll(&error)); // no-op
    EXPECT_FALSE(session.Latch().warningIssued);
    EXPECT_FALSE(session.Latch().ScaleChanged());

    dirty();
    port.captureActive = true;
    EXPECT_TRUE(session.DisconnectAll(&error)); // refused
    EXPECT_FALSE(session.Latch().warningIssued);

    dirty();
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_FALSE(session.SetDevice(GearDevice::Rotator, false, &error)); // unselected
    EXPECT_FALSE(session.Latch().warningIssued);
    EXPECT_TRUE(port.calls.empty());
}

TEST_F(GearSessionTest, TheDepthCountsTheRequestOnTheStack)
{
    std::vector<int> depths;
    port.onLeaf = [&](GearDevice) { depths.push_back(session.Depth()); };
    EXPECT_EQ(session.Depth(), 0);
    session.ConnectAll(&error);
    EXPECT_EQ(depths, (std::vector<int> { 1, 1, 1 }));
    EXPECT_EQ(session.Depth(), 0);
    session.SetDevice(GearDevice::Mount, false, &error);
    EXPECT_EQ(depths.back(), 1);
    EXPECT_EQ(session.Depth(), 0);
}

TEST_F(GearSessionTest, ANestedRequestDoesNotBeginASession)
{
    // a request from a nested event loop (a retry tick inside a connect) is part of the outer request
    port.dev[GearDevice::Rotator].selected = false;
    bool nested = false;
    port.onLeaf = [&](GearDevice d)
    {
        if (d != GearDevice::Camera || nested)
            return;
        nested = true;
        session.Latch().warningIssued = true; // the camera connect gave the warning
        std::string err;
        session.SetDevice(GearDevice::Mount, true, &err);
        EXPECT_EQ(session.Depth(), 1);
        EXPECT_TRUE(session.Latch().warningIssued);
    };
    session.ConnectAll(&error);
    EXPECT_TRUE(nested);
    EXPECT_TRUE(session.Latch().warningIssued);
    EXPECT_EQ(session.Depth(), 0);
}

// ---- the end of request ----

TEST_F(GearSessionTest, EndRunsInItsFixedOrder)
{
    // the camera connect finds another camera with another scale, and the mount had failed to connect before (#104)
    port.onLeaf = [this](GearDevice d)
    {
        if (d == GearDevice::Camera)
        {
            session.Latch().NotePixelSize(3.75, 2.9);
            session.Latch().NoteCameraChange();
        }
        if (d == GearDevice::Mount)
            session.QueueReconnected(GearDevice::Mount);
    };
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_FALSE(session.ConnectAll(&error));
    EXPECT_EQ(port.calls,
              (Calls { "trace gear_dialog: ConnectAll calls OnButtonConnectAll\n", "requested all",
                       "trace gear_dialog: OnButtonConnectAll\n", "connect camera", "connect mount", "connect rotator", "frame",
                       "clearCal", "advanced", "scale", "reconnected mount" }));
}

TEST_F(GearSessionTest, TheCalibrationActionComesBeforeEquipmentReconnected)
{
    port.autoLoad = true;
    port.onLeaf = [this](GearDevice d) { session.QueueReconnected(d); };
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(port.calls,
              (Calls { "trace gear_dialog: SetDeviceConnected mount 1\n", "requested mount", "connect mount", "frame",
                       "loadCal", "advanced", "reconnected mount" }));

    // sent once
    port.calls.clear();
    port.onLeaf = nullptr;
    EXPECT_FALSE(session.SetDevice(GearDevice::Camera, true, &error));
    EXPECT_EQ(port.Count("reconnected mount"), 0);
}

TEST_F(GearSessionTest, NoAutoLoadWithoutTheMount)
{
    port.autoLoad = true;
    port.dev[GearDevice::Mount].selected = false;
    port.dev[GearDevice::Rotator].selected = false;
    EXPECT_FALSE(session.ConnectAll(&error));
    EXPECT_EQ(port.Count("loadCal"), 0);
}

TEST_F(GearSessionTest, AClearAndAScaleChangeAreTakenOnce)
{
    port.autoLoad = true;
    // a camera-only connect with another camera: the mount is not connected, so the clear waits for it
    port.onLeaf = [this](GearDevice d)
    {
        if (d == GearDevice::Camera)
        {
            session.Latch().NotePixelSize(3.75, 2.9);
            session.Latch().NoteCameraChange();
        }
    };
    EXPECT_FALSE(session.SetDevice(GearDevice::Camera, true, &error));
    EXPECT_EQ(port.Count("clearCal"), 0);
    EXPECT_EQ(port.Count("loadCal"), 0);
    EXPECT_EQ(port.Count("scale"), 1);

    // the mount connects in a later request: its calibration is cleared then, and the scale change is not redone
    port.onLeaf = nullptr;
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(port.Count("clearCal"), 1);
    EXPECT_EQ(port.Count("scale"), 1);

    // and the saved calibration, made with the old camera, is never auto-loaded again
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, false, &error));
    EXPECT_FALSE(session.SetDevice(GearDevice::Mount, true, &error));
    EXPECT_EQ(port.Count("clearCal"), 1);
    EXPECT_EQ(port.Count("loadCal"), 0);
}

// ---- the profile switch ----

struct ProfileSwitch
{
    std::vector<std::string> *calls;
    bool selectable = true;
    std::function<bool()> Select()
    {
        return [this]()
        {
            calls->push_back("select");
            return selectable;
        };
    }
    std::function<void()> Reload()
    {
        return [this]() { calls->push_back("reload"); };
    }
};

TEST_F(GearSessionTest, SetProfileToTheCurrentProfileIsANoOp)
{
    ProfileSwitch p { &port.calls };
    EXPECT_FALSE(session.SetProfile(2, 2, p.Select(), p.Reload(), &error));
    EXPECT_EQ(error, "");
    EXPECT_TRUE(port.calls.empty()); // not even the profile reads of the target
}

TEST_F(GearSessionTest, SetProfileIsRefusedWhileAnythingIsConnected)
{
    ProfileSwitch p { &port.calls };
    for (GearDevice d : kDevices)
    {
        port.dev[d].connected = true;
        error.clear();
        EXPECT_TRUE(session.SetProfile(1, 2, p.Select(), p.Reload(), &error));
        EXPECT_EQ(error, "cannot set profile when equipment is connected");
        port.dev[d].connected = false;
    }
    EXPECT_TRUE(port.calls.empty());
}

TEST_F(GearSessionTest, SetProfileToAnInvalidProfile)
{
    ProfileSwitch p { &port.calls };
    p.selectable = false;
    session.Latch().warningIssued = true;
    EXPECT_TRUE(session.SetProfile(1, 7, p.Select(), p.Reload(), &error));
    EXPECT_EQ(error, "invalid profile id");
    EXPECT_EQ(port.calls, (Calls { "select" })); // no reload, no end of request
    EXPECT_TRUE(session.Latch().warningIssued); // the session begins only after the validation
}

TEST_F(GearSessionTest, SetProfileReloadsAndEndsOnce)
{
    ProfileSwitch p { &port.calls };
    session.Latch().warningIssued = true;
    std::vector<int> depths;
    port.onLeaf = nullptr;
    auto reload = [&]()
    {
        p.Reload()();
        depths.push_back(session.Depth());
        EXPECT_FALSE(session.Latch().warningIssued); // a session began before the reload
    };
    EXPECT_FALSE(session.SetProfile(1, 2, p.Select(), reload, &error));
    EXPECT_EQ(error, "");
    EXPECT_EQ(port.calls, Concat({ "select", "reload" }, kEnd));
    EXPECT_EQ(depths, (std::vector<int> { 0 })); // set_profile takes no request depth of its own
}

TEST_F(GearSessionTest, SetProfileNestedInARequestDoesNotBeginASession)
{
    ProfileSwitch p { &port.calls };
    port.onLeaf = [&](GearDevice d)
    {
        if (d != GearDevice::Mount)
            return;
        session.Latch().warningIssued = true;
        port.dev[GearDevice::Camera].connected = false; // nothing connected for the switch
        port.dev[GearDevice::Mount].connected = false;
        std::string err;
        EXPECT_FALSE(session.SetProfile(1, 2, p.Select(), p.Reload(), &err));
        EXPECT_TRUE(session.Latch().warningIssued);
    };
    port.dev[GearDevice::Rotator].selected = false;
    session.ConnectAll(&error);
    EXPECT_EQ(port.Count("reload"), 1);
}
