// Recovery of a selected device that failed to connect (#104).
//
// A mount that did not answer when the guider started was never tried again, so every later guide request was
// refused with "all equipment must be connected first" until someone sent set_connected by hand. The guider now
// retries a failed camera, mount or rotator in the background (15 s, 30 s, then every 60 s), probing the Alpaca
// server on a thread of its own first; the guide RPC tries a fresh connect before it refuses and names the reason;
// and EquipmentReconnected reports the device's return. The schedule, the per-device state, the gates, the probe's
// sequence and the texts live in connect_retry.cpp.

#include <gtest/gtest.h>

#include "connect_retry.h"

#include <chrono>
#include <deque>
#include <thread>
#include <vector>

using namespace ConnectRetry;

namespace
{
const GearDevice kDevices[] = { GearDevice::Camera, GearDevice::Mount, GearDevice::Rotator };
const int64_t kT0 = 1000000;
} // namespace

TEST(ConnectRetry, DelaySchedule)
{
    EXPECT_EQ(RetryDelaySeconds(1), 15);
    EXPECT_EQ(RetryDelaySeconds(2), 30);
    EXPECT_EQ(RetryDelaySeconds(3), 60);
    EXPECT_EQ(RetryDelaySeconds(4), 60);
    EXPECT_EQ(RetryDelaySeconds(10), 60);
    EXPECT_EQ(RetryDelaySeconds(100000), kMaxRetryDelaySec);
    // never 0, even for a nonsense attempt number
    EXPECT_EQ(RetryDelaySeconds(0), kFirstRetryDelaySec);
    EXPECT_EQ(RetryDelaySeconds(-3), kFirstRetryDelaySec);
}

TEST(ConnectRetry, FreshTrackerHasNoWork)
{
    Tracker t;
    for (GearDevice d : kDevices)
    {
        EXPECT_FALSE(t.IsFailed(d));
        EXPECT_FALSE(t.Retrying(d));
        EXPECT_FALSE(t.Due(d, kT0));
        EXPECT_FALSE(t.UserDisconnected(d));
        EXPECT_EQ(t.GetIntent(d), Intent::None);
        EXPECT_EQ(t.Attempts(d), 0);
        EXPECT_EQ(t.Results(d), 0u);
        EXPECT_EQ(t.LastReason(d), "");
    }
    EXPECT_FALSE(t.AnyRetrying());
}

TEST(ConnectRetry, FirstConnectSuccessIsNotAReconnect)
{
    Tracker t;
    EXPECT_FALSE(t.OnConnectResult(GearDevice::Mount, true, kT0, ""));
    EXPECT_FALSE(t.IsFailed(GearDevice::Mount));
    EXPECT_FALSE(t.AnyRetrying());
    EXPECT_EQ(t.GetIntent(GearDevice::Mount), Intent::Wanted);
    EXPECT_EQ(t.Results(GearDevice::Mount), 1u);
}

TEST(ConnectRetry, FailureSchedulesRetry)
{
    Tracker t;
    EXPECT_FALSE(t.OnConnectResult(GearDevice::Mount, false, kT0, "The mount did not reply (Alpaca 1280)"));
    EXPECT_TRUE(t.IsFailed(GearDevice::Mount));
    EXPECT_TRUE(t.Retrying(GearDevice::Mount));
    EXPECT_TRUE(t.AnyRetrying());
    EXPECT_EQ(t.GetIntent(GearDevice::Mount), Intent::Wanted);
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 1);
    EXPECT_EQ(t.LastReason(GearDevice::Mount), "The mount did not reply (Alpaca 1280)");
    EXPECT_FALSE(t.Due(GearDevice::Mount, kT0));
    EXPECT_FALSE(t.Due(GearDevice::Mount, kT0 + 14999));
    EXPECT_TRUE(t.Due(GearDevice::Mount, kT0 + 15000));
    EXPECT_EQ(t.NextDueMs(GearDevice::Mount), kT0 + 15000);
}

TEST(ConnectRetry, RepeatedFailuresBackOffAndNeverStop)
{
    Tracker t;
    int64_t now = kT0;
    const int expected[] = { 15, 30, 60, 60, 60, 60, 60, 60, 60, 60 };
    for (int i = 0; i < 10; i++)
    {
        t.OnConnectResult(GearDevice::Mount, false, now, "x");
        EXPECT_EQ(t.Attempts(GearDevice::Mount), i + 1);
        EXPECT_EQ(t.NextDueMs(GearDevice::Mount), now + expected[i] * 1000LL) << "attempt " << i + 1;
        EXPECT_TRUE(t.Retrying(GearDevice::Mount));
        now = t.NextDueMs(GearDevice::Mount);
    }
}

TEST(ConnectRetry, RecoveryReportsReconnectedOnce)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    t.OnConnectResult(GearDevice::Mount, false, kT0 + 15000, "y");
    EXPECT_TRUE(t.OnConnectResult(GearDevice::Mount, true, kT0 + 45000, ""));
    EXPECT_FALSE(t.IsFailed(GearDevice::Mount));
    EXPECT_FALSE(t.Retrying(GearDevice::Mount));
    EXPECT_FALSE(t.AnyRetrying());
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 0);
    EXPECT_EQ(t.LastReason(GearDevice::Mount), "");
    // a second success is not another recovery
    EXPECT_FALSE(t.OnConnectResult(GearDevice::Mount, true, kT0 + 50000, ""));
    // and a new failure starts the schedule from the beginning
    t.OnConnectResult(GearDevice::Mount, false, kT0 + 60000, "z");
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 1);
    EXPECT_EQ(t.NextDueMs(GearDevice::Mount), kT0 + 75000);
}

TEST(ConnectRetry, UserDisconnectStopsRetryButKeepsTheFailure)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    t.OnUserDisconnect(GearDevice::Mount);
    EXPECT_TRUE(t.UserDisconnected(GearDevice::Mount));
    EXPECT_FALSE(t.Retrying(GearDevice::Mount));
    EXPECT_FALSE(t.Due(GearDevice::Mount, kT0 + 3600 * 1000LL));
    EXPECT_FALSE(t.AnyRetrying());
    // clients were told it failed; when it comes back (by an explicit connect), they hear it once
    EXPECT_TRUE(t.IsFailed(GearDevice::Mount));
    t.OnConnectRequested(GearDevice::Mount);
    EXPECT_EQ(t.GetIntent(GearDevice::Mount), Intent::Wanted);
    EXPECT_TRUE(t.OnConnectResult(GearDevice::Mount, true, kT0 + 5000, ""));
}

TEST(ConnectRetry, ExplicitConnectFailureAfterUserDisconnectRearms)
{
    Tracker t;
    t.OnUserDisconnect(GearDevice::Mount);
    EXPECT_FALSE(t.IsFailed(GearDevice::Mount)); // was connected, so nothing to report
    t.OnConnectRequested(GearDevice::Mount);
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    EXPECT_EQ(t.GetIntent(GearDevice::Mount), Intent::Wanted);
    EXPECT_TRUE(t.Retrying(GearDevice::Mount));
}

TEST(ConnectRetry, UserDisconnectAllCoversEveryDevice)
{
    Tracker t;
    for (GearDevice d : kDevices)
        t.OnConnectResult(d, false, kT0, "x");
    t.OnUserDisconnect(GearDevice::All);
    for (GearDevice d : kDevices)
    {
        EXPECT_TRUE(t.UserDisconnected(d));
        EXPECT_FALSE(t.Retrying(d));
    }
    EXPECT_FALSE(t.AnyRetrying());
    t.OnConnectRequested(GearDevice::All);
    for (GearDevice d : kDevices)
        EXPECT_EQ(t.GetIntent(d), Intent::Wanted);
    EXPECT_TRUE(t.AnyRetrying()); // still failed, Wanted again
}

TEST(ConnectRetry, ForgetOnReselect)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    t.Forget(GearDevice::Mount);
    EXPECT_FALSE(t.IsFailed(GearDevice::Mount));
    EXPECT_FALSE(t.Retrying(GearDevice::Mount));
    EXPECT_EQ(t.GetIntent(GearDevice::Mount), Intent::None);
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 0);
    // a success afterwards is the new device's first connect, not a recovery
    EXPECT_FALSE(t.OnConnectResult(GearDevice::Mount, true, kT0 + 1, ""));

    t.OnUserDisconnect(GearDevice::Camera);
    t.Forget(GearDevice::Camera);
    EXPECT_FALSE(t.UserDisconnected(GearDevice::Camera));
}

TEST(ConnectRetry, DevicesAreIndependent)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    EXPECT_FALSE(t.Due(GearDevice::Camera, kT0 + 60000));
    EXPECT_FALSE(t.Due(GearDevice::Rotator, kT0 + 60000));
    t.OnConnectResult(GearDevice::Rotator, false, kT0 + 1000, "y");
    t.OnConnectResult(GearDevice::Mount, true, kT0 + 2000, "");
    EXPECT_TRUE(t.AnyRetrying());
    EXPECT_TRUE(t.Retrying(GearDevice::Rotator));
    t.OnConnectResult(GearDevice::Rotator, true, kT0 + 3000, "");
    EXPECT_FALSE(t.AnyRetrying());
}

TEST(ConnectRetry, NotConfiguredIsParkedUntilTheServerChanges)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Camera, false, kT0, kNotConfiguredReason, false);
    EXPECT_TRUE(t.IsFailed(GearDevice::Camera));
    EXPECT_TRUE(t.IsParked(GearDevice::Camera));
    EXPECT_FALSE(t.Retrying(GearDevice::Camera));
    EXPECT_FALSE(t.Due(GearDevice::Camera, kT0 + 3600 * 1000LL));
    EXPECT_FALSE(t.AnyRetrying());
    EXPECT_EQ(t.LastReason(GearDevice::Camera), kNotConfiguredReason);

    t.AfterServerChange(t.BeforeServerChange(), kT0 + 5000);
    EXPECT_FALSE(t.IsParked(GearDevice::Camera));
    EXPECT_TRUE(t.Due(GearDevice::Camera, kT0 + 5000));
    EXPECT_TRUE(t.AnyRetrying());

    // a retryable failure clears the park, a later not-configured one parks again
    t.OnConnectResult(GearDevice::Camera, false, kT0 + 6000, "HTTP 500");
    EXPECT_TRUE(t.Retrying(GearDevice::Camera));
    t.OnConnectResult(GearDevice::Camera, false, kT0 + 7000, kNotConfiguredReason, false);
    EXPECT_FALSE(t.Retrying(GearDevice::Camera));
    t.Forget(GearDevice::Camera);
    EXPECT_FALSE(t.IsParked(GearDevice::Camera));
}

// set_alpaca_server re-selects every Alpaca device for the new server, and the choice handler forgets a device whose
// choice string changed ("Alpaca Mount [host:0/0]" to "[host:11111/0]"). The review found that this wiped the request
// of a mount parked as not configured, so set_alpaca_server alone never led to a connect.
TEST(ConnectRetry, ServerChangeKeepsAParkedRequestAcrossTheReselection)
{
    Tracker t;
    EXPECT_TRUE(t.Chosen(GearDevice::Mount, "Alpaca Mount [localhost:0/0]"));
    t.OnConnectRequested(GearDevice::All); // --auto-connect
    t.OnConnectResult(GearDevice::Mount, false, kT0, kNotConfiguredReason, false);
    ASSERT_TRUE(t.IsParked(GearDevice::Mount));

    Tracker::ServerChangeCarry carry = t.BeforeServerChange();
    // the re-selection for the new port
    if (t.Chosen(GearDevice::Mount, "Alpaca Mount [localhost:11111/0]"))
        t.Forget(GearDevice::Mount);
    EXPECT_EQ(t.GetIntent(GearDevice::Mount), Intent::None);
    t.AfterServerChange(carry, kT0 + 5000);

    EXPECT_EQ(t.GetIntent(GearDevice::Mount), Intent::Wanted);
    EXPECT_FALSE(t.IsParked(GearDevice::Mount));
    EXPECT_TRUE(t.Retrying(GearDevice::Mount));
    EXPECT_TRUE(t.Due(GearDevice::Mount, kT0 + 5000));
    EXPECT_EQ(t.LastReason(GearDevice::Mount), kNotConfiguredReason);
    // a fresh schedule: the next failure waits the first delay
    t.OnConnectResult(GearDevice::Mount, false, kT0 + 6000, "The mount did not reply (Alpaca 1280)");
    EXPECT_EQ(t.NextDueMs(GearDevice::Mount), kT0 + 6000 + kFirstRetryDelaySec * 1000LL);
    // it stayed failed across the change, so its connect is the one recovery
    EXPECT_TRUE(t.OnConnectResult(GearDevice::Mount, true, kT0 + 21000, ""));
}

// The Pi boots before the bridge: camera and mount both retrying. An accepted set_alpaca_server must not stop them.
TEST(ConnectRetry, ServerChangeKeepsRetriesGoingAndMakesThemDueNow)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Camera, false, kT0, "no response from the Alpaca server (Couldn't connect to server)");
    t.OnConnectResult(GearDevice::Mount, false, kT0, "no response from the Alpaca server (Couldn't connect to server)");
    t.OnConnectResult(GearDevice::Mount, false, kT0 + 15000, "y");
    uint64_t token = t.BeginAttempt(GearDevice::Mount); // a probe of the old server in flight

    Tracker::ServerChangeCarry carry = t.BeforeServerChange();
    t.Forget(GearDevice::Camera); // re-selected for the new server
    t.Forget(GearDevice::Mount);
    t.AfterServerChange(carry, kT0 + 20000);

    EXPECT_TRUE(t.Due(GearDevice::Camera, kT0 + 20000));
    EXPECT_TRUE(t.Due(GearDevice::Mount, kT0 + 20000));
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 0);
    EXPECT_FALSE(t.AttemptCurrent(GearDevice::Mount, token)); // the old server's answer is dropped
    EXPECT_FALSE(t.Retrying(GearDevice::Rotator));
}

TEST(ConnectRetry, ServerChangeLeavesOtherDevicesAlone)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, true, kT0, ""); // connected once, never failed
    t.OnUserDisconnect(GearDevice::Camera);
    t.OnConnectResult(GearDevice::Rotator, false, kT0, "x");
    t.OnUserDisconnect(GearDevice::Rotator); // failed, then the user said leave it

    t.AfterServerChange(t.BeforeServerChange(), kT0 + 1);
    EXPECT_FALSE(t.AnyRetrying());
    EXPECT_TRUE(t.UserDisconnected(GearDevice::Camera));
    EXPECT_TRUE(t.UserDisconnected(GearDevice::Rotator));
    EXPECT_FALSE(t.IsFailed(GearDevice::Mount));

    // a disconnect between the snapshot and the change wins
    Tracker u;
    u.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    Tracker::ServerChangeCarry carry = u.BeforeServerChange();
    u.OnUserDisconnect(GearDevice::Mount);
    u.AfterServerChange(carry, kT0 + 1);
    EXPECT_FALSE(u.Retrying(GearDevice::Mount));
    EXPECT_TRUE(u.UserDisconnected(GearDevice::Mount));
}

// set_selected_* with the current selection re-runs the choice handler; only another device forgets the state.
TEST(ConnectRetry, ChoosingTheSameDeviceAgainKeepsItsState)
{
    Tracker t;
    EXPECT_TRUE(t.Chosen(GearDevice::Camera, "Alpaca Camera [localhost:11111/0]"));
    EXPECT_FALSE(t.Chosen(GearDevice::Camera, "Alpaca Camera [localhost:11111/0]"));
    EXPECT_TRUE(t.Chosen(GearDevice::Camera, "Simulator"));
    EXPECT_TRUE(t.Chosen(GearDevice::Camera, "Alpaca Camera [localhost:11111/0]"));
    // per device, and Forget (a profile change) keeps the last choice, so the choice handler that follows it does
    // not forget a second time
    EXPECT_TRUE(t.Chosen(GearDevice::Mount, "Alpaca Camera [localhost:11111/0]"));
    t.Forget(GearDevice::Mount);
    EXPECT_FALSE(t.Chosen(GearDevice::Mount, "Alpaca Camera [localhost:11111/0]"));
}

TEST(ConnectRetry, AttemptTokens)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    uint64_t token = t.BeginAttempt(GearDevice::Mount);
    EXPECT_TRUE(t.AttemptInFlight(GearDevice::Mount));
    EXPECT_TRUE(t.AttemptCurrent(GearDevice::Mount, token));
    // an attempt in flight is not due again
    EXPECT_FALSE(t.Due(GearDevice::Mount, kT0 + 60000));
    t.EndAttempt(GearDevice::Mount);
    EXPECT_FALSE(t.AttemptInFlight(GearDevice::Mount));
    EXPECT_TRUE(t.Due(GearDevice::Mount, kT0 + 15000)); // EndAttempt sets no new delay
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 1);

    // a result recorded meanwhile, by any path, makes the attempt stale
    token = t.BeginAttempt(GearDevice::Mount);
    t.OnConnectResult(GearDevice::Mount, false, kT0 + 16000, "y");
    EXPECT_FALSE(t.AttemptCurrent(GearDevice::Mount, token));
    EXPECT_FALSE(t.AttemptInFlight(GearDevice::Mount));

    token = t.BeginAttempt(GearDevice::Mount);
    t.OnUserDisconnect(GearDevice::Mount);
    EXPECT_FALSE(t.AttemptCurrent(GearDevice::Mount, token));

    t.OnConnectRequested(GearDevice::Mount);
    token = t.BeginAttempt(GearDevice::Mount);
    t.Forget(GearDevice::Mount);
    EXPECT_FALSE(t.AttemptCurrent(GearDevice::Mount, token));

    // tokens are per device
    t.OnConnectResult(GearDevice::Camera, false, kT0, "x");
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    token = t.BeginAttempt(GearDevice::Mount);
    t.OnConnectResult(GearDevice::Camera, true, kT0 + 1, "");
    EXPECT_TRUE(t.AttemptCurrent(GearDevice::Mount, token));
}

TEST(ConnectRetry, ResultsCountEveryResult)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Rotator, false, kT0, "x");
    t.OnConnectResult(GearDevice::Rotator, true, kT0, "");
    t.OnConnectResult(GearDevice::Rotator, true, kT0, "");
    EXPECT_EQ(t.Results(GearDevice::Rotator), 3u);
    t.Forget(GearDevice::Rotator);
    EXPECT_EQ(t.Results(GearDevice::Rotator), 3u); // monotonic, so a caller's before/after compare still works
}

TEST(ConnectRetry, RetryBlockerGates)
{
    for (GearDevice d : kDevices)
    {
        EXPECT_EQ(RetryBlocker(d, Gates()), "");

        Gates g;
        g.shuttingDown = true;
        EXPECT_EQ(RetryBlocker(d, g), "shutting down");
        g = Gates();
        g.darkBuild = true;
        EXPECT_EQ(RetryBlocker(d, g), "dark-frame capture in progress");
        g = Gates();
        g.paSession = true;
        EXPECT_EQ(RetryBlocker(d, g), "polar-alignment session in progress");
        g = Gates();
        g.alignmentTool = true;
        EXPECT_EQ(RetryBlocker(d, g), "polar or drift alignment in progress");
        g = Gates();
        g.guidingAssistant = true;
        EXPECT_EQ(RetryBlocker(d, g), "Guiding Assistant is running");
        g = Gates();
        g.connectInProgress = true;
        EXPECT_EQ(RetryBlocker(d, g), "connect in progress");
        g = Gates();
        g.dialogOpen = true;
        EXPECT_EQ(RetryBlocker(d, g), "cannot connect equipment when Connect Equipment dialog is open");
    }

    Gates looping;
    looping.captureActive = true;
    EXPECT_EQ(RetryBlocker(GearDevice::Camera, looping), "cannot connect the camera while capture is active");
    EXPECT_EQ(RetryBlocker(GearDevice::Mount, looping), "");
    EXPECT_EQ(RetryBlocker(GearDevice::Rotator, looping), "");

    Gates guiding;
    guiding.captureActive = true;
    guiding.calibratingOrGuiding = true;
    EXPECT_EQ(RetryBlocker(GearDevice::Mount, guiding), "cannot connect the mount while calibrating or guiding");
    EXPECT_EQ(RetryBlocker(GearDevice::Rotator, guiding), "cannot connect the rotator while calibrating or guiding");

    // the first gate wins
    Gates many;
    many.darkBuild = true;
    many.paSession = true;
    many.captureActive = true;
    EXPECT_EQ(RetryBlocker(GearDevice::Camera, many), "dark-frame capture in progress");
}

TEST(ConnectRetry, AlignmentToolsThatBlockARetry)
{
    EXPECT_FALSE(AlignmentToolBlocks(AlignmentTools()));

    AlignmentTools pd;
    pd.polarDriftDrifting = true;
    EXPECT_TRUE(AlignmentToolBlocks(pd));

    AlignmentTools drift;
    drift.driftToolOpen = true;
    EXPECT_TRUE(AlignmentToolBlocks(drift));

    Gates g;
    g.alignmentTool = AlignmentToolBlocks(pd);
    for (GearDevice d : kDevices)
        EXPECT_EQ(RetryBlocker(d, g), "polar or drift alignment in progress");
}

TEST(ConnectRetry, WithReason)
{
    EXPECT_EQ(WithReason("mount failed to connect", "The mount did not reply (Alpaca 1280)"),
              "mount failed to connect (The mount did not reply (Alpaca 1280))");
    EXPECT_EQ(WithReason("mount failed to connect", ""), "mount failed to connect");
}

TEST(ConnectRetry, GuideRefusalText)
{
    using K = GuideFailure::Kind;
    const std::string prefix = "all equipment must be connected first";
    EXPECT_EQ(GuideRefusal({}), prefix);
    EXPECT_EQ(GuideRefusal({ { GearDevice::Mount, K::ConnectFailed, "The mount did not reply (Alpaca 1280)" } }),
              prefix + ": the mount failed to connect: The mount did not reply (Alpaca 1280)");
    EXPECT_EQ(GuideRefusal({ { GearDevice::Mount, K::ConnectFailed, "" } }), prefix + ": the mount failed to connect");
    EXPECT_EQ(GuideRefusal({ { GearDevice::Mount, K::UserDisconnected, "ignored" } }),
              prefix + ": the mount was disconnected by the user");
    EXPECT_EQ(GuideRefusal({ { GearDevice::Camera, K::Refused, "cannot connect the camera while capture is active" } }),
              prefix + ": the camera is not connected: cannot connect the camera while capture is active");
    EXPECT_EQ(GuideRefusal({ { GearDevice::Mount, K::StillConnecting, "" } }), prefix + ": the mount is still connecting");
    EXPECT_EQ(GuideRefusal({ { GearDevice::Mount, K::OutOfTime, "" } }), prefix + ": the mount was not tried in time");
    EXPECT_EQ(
        GuideRefusal({ { GearDevice::Camera, K::ConnectFailed, "HTTP 500" }, { GearDevice::Mount, K::UserDisconnected, "" } }),
        prefix + ": the camera failed to connect: HTTP 500; the mount was disconnected by the user");
}

TEST(ConnectRetry, RecordAndTakeConnectError)
{
    ClearConnectError(GearDevice::Camera);
    ClearConnectError(GearDevice::Mount);
    EXPECT_EQ(TakeConnectError(GearDevice::Mount).reason, "");
    EXPECT_TRUE(TakeConnectError(GearDevice::Mount).retryable);

    RecordConnectError(GearDevice::Mount, "The mount did not reply (Alpaca 1280)");
    RecordConnectError(GearDevice::Camera, kNotConfiguredReason, false);
    ConnectError e = TakeConnectError(GearDevice::Mount);
    EXPECT_EQ(e.reason, "The mount did not reply (Alpaca 1280)");
    EXPECT_TRUE(e.retryable);
    EXPECT_EQ(TakeConnectError(GearDevice::Mount).reason, ""); // taken

    ConnectError c = TakeConnectError(GearDevice::Camera);
    EXPECT_EQ(c.reason, kNotConfiguredReason);
    EXPECT_FALSE(c.retryable);

    RecordConnectError(GearDevice::Rotator, "x");
    ClearConnectError(GearDevice::Rotator);
    EXPECT_EQ(TakeConnectError(GearDevice::Rotator).reason, "");
}

TEST(ConnectRetry, RecordConnectErrorIfNoneKeepsTheSpecificReason)
{
    ClearConnectError(GearDevice::Camera);
    // CamConnectFailed after a path that recorded the server's reason
    RecordConnectError(GearDevice::Camera, "Camera not connected (Alpaca 1031)");
    RecordConnectErrorIfNone(GearDevice::Camera, "Alpaca Camera driver missing the CameraXSize property.");
    EXPECT_EQ(TakeConnectError(GearDevice::Camera).reason, "Camera not connected (Alpaca 1031)");
    // a path that recorded nothing (the review: a property call after the connected check)
    RecordConnectErrorIfNone(GearDevice::Camera, "Alpaca Camera driver missing the CameraXSize property.");
    ConnectError e = TakeConnectError(GearDevice::Camera);
    EXPECT_EQ(e.reason, "Alpaca Camera driver missing the CameraXSize property.");
    EXPECT_TRUE(e.retryable);
}

TEST(ConnectRetry, QuietConnectScopeNests)
{
    EXPECT_FALSE(QuietConnect());
    {
        QuietConnectScope outer;
        EXPECT_TRUE(QuietConnect());
        {
            QuietConnectScope inner;
            EXPECT_TRUE(QuietConnect());
        }
        EXPECT_TRUE(QuietConnect());
    }
    EXPECT_FALSE(QuietConnect());
}

// ---- one EquipmentReconnected per recovery, by every path ----
//
// GearDialog::NoteConnectResult feeds each connect leaf's final state to OnLeafResult and queues EquipmentReconnected
// when it says reconnected; the #105 camera auto-reconnect sends its own from DoTryReconnect. The retry, guide,
// set_connected and ConnectAll all end in a leaf.

namespace
{
// what the clients see from one tracker: EquipmentReconnected from the leaves, plus DoTryReconnect's own
struct Events
{
    Tracker t;
    int reconnected = 0;

    void Leaf(GearDevice d, bool connected, bool autoReconnecting = false, int64_t now = kT0)
    {
        if (t.OnLeafResult(d, connected, autoReconnecting, now, connected ? "" : "x").reconnected)
            reconnected++;
    }
    // DoTryReconnect: the camera auto-reconnect, which sends EquipmentReconnected on success and
    // EquipmentReconnectFailed (then NoteCameraReconnectFailed) on failure
    void AutoReconnect(bool connected, int64_t now = kT0)
    {
        Leaf(GearDevice::Camera, connected, true, now);
        if (connected)
            reconnected++;
        else
            t.OnConnectResult(GearDevice::Camera, false, now, "the camera did not reconnect");
    }
};
} // namespace

TEST(ConnectRetry, LeafFailureThenAnyConnectGivesOneReconnected)
{
    // the retry, guide, set_connected or ConnectAll: each ends in the same leaf
    Events ev;
    ev.Leaf(GearDevice::Mount, false);
    EXPECT_TRUE(ev.t.Retrying(GearDevice::Mount));
    ev.Leaf(GearDevice::Mount, false);
    ev.Leaf(GearDevice::Mount, true);
    EXPECT_EQ(ev.reconnected, 1);
    ev.Leaf(GearDevice::Mount, true); // a later connect is not another recovery
    EXPECT_EQ(ev.reconnected, 1);
}

TEST(ConnectRetry, FirstTimeConnectSendsNoReconnected)
{
    // a daemon started without --auto-connect, then guide connects the mount
    Events ev;
    ev.Leaf(GearDevice::Mount, true);
    EXPECT_EQ(ev.reconnected, 0);
    // nor after a forget (another device chosen)
    ev.Leaf(GearDevice::Camera, false);
    ev.t.Forget(GearDevice::Camera);
    ev.Leaf(GearDevice::Camera, true);
    EXPECT_EQ(ev.reconnected, 0);
}

TEST(ConnectRetry, AutoReconnectSuccessOfAFailedCameraIsReportedOnce)
{
    // the camera had failed to connect (the tracker has it failed), then the #105 auto-reconnect brings it back: only
    // DoTryReconnect's event, not a second one from the leaf
    Events ev;
    ev.Leaf(GearDevice::Camera, false);
    ev.AutoReconnect(true);
    EXPECT_EQ(ev.reconnected, 1);
    EXPECT_FALSE(ev.t.IsFailed(GearDevice::Camera));
    EXPECT_FALSE(ev.t.AnyRetrying());
}

TEST(ConnectRetry, AutoReconnectFailureJoinsTheRetryAndReportsTheRecoveryOnce)
{
    Events ev;
    ev.Leaf(GearDevice::Camera, true); // connected at startup
    // a drop; the reconnect's own leaf failure is not recorded, only the failure DoTryReconnect reports after
    // EquipmentReconnectFailed
    Tracker::LeafOutcome out = ev.t.OnLeafResult(GearDevice::Camera, false, true, kT0, "x");
    EXPECT_FALSE(out.failed);
    EXPECT_FALSE(ev.t.IsFailed(GearDevice::Camera));
    ev.AutoReconnect(false);
    EXPECT_TRUE(ev.t.Retrying(GearDevice::Camera));
    // the RC51 finding: the next connect (set_connected, or the retry) reports it
    ev.Leaf(GearDevice::Camera, true);
    EXPECT_EQ(ev.reconnected, 1);
}

TEST(ConnectRetry, ConnectCallThatNeverReachedALeafStillMovesTheSchedule)
{
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, false, kT0, "x");
    uint64_t before = t.Results(GearDevice::Mount);
    // SetDeviceConnected returned before a connect (a refusal the gates missed)
    EXPECT_TRUE(t.OnConnectCallEnded(GearDevice::Mount, before, false, kT0 + 15000, "mount: cannot connect while guiding"));
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 2);
    EXPECT_EQ(t.NextDueMs(GearDevice::Mount), kT0 + 15000 + kSecondRetryDelaySec * 1000LL);
    EXPECT_EQ(t.LastReason(GearDevice::Mount), "mount: cannot connect while guiding");

    // a leaf ran and recorded the driver's reason: nothing more
    before = t.Results(GearDevice::Mount);
    t.OnLeafResult(GearDevice::Mount, false, false, kT0 + 50000, "The mount did not reply (Alpaca 1280)");
    EXPECT_FALSE(t.OnConnectCallEnded(GearDevice::Mount, before, false, kT0 + 50000, "mount failed to connect"));
    EXPECT_EQ(t.LastReason(GearDevice::Mount), "The mount did not reply (Alpaca 1280)");
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 3);

    // connected: nothing to record
    before = t.Results(GearDevice::Mount);
    EXPECT_FALSE(t.OnConnectCallEnded(GearDevice::Mount, before, true, kT0 + 60000, ""));
}

// ---- the guide RPC's plan for one device ----

TEST(ConnectRetry, GuideOutOfTimeCountsNoAttempt)
{
    // a retrying mount the guide RPC had no time to try keeps its failure count, its schedule and its reason
    Tracker t;
    t.OnConnectResult(GearDevice::Mount, false, kT0, "no response from the Alpaca server (Couldn't connect to server)");
    uint64_t results = t.Results(GearDevice::Mount);
    int64_t due = t.NextDueMs(GearDevice::Mount);
    t.OnGuideOutOfTime(GearDevice::Mount, kT0 + 2000, false);
    t.OnGuideOutOfTime(GearDevice::Mount, kT0 + 3000, false); // a repeated guide adds nothing either
    EXPECT_TRUE(t.Retrying(GearDevice::Mount));
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 1);
    EXPECT_EQ(t.NextDueMs(GearDevice::Mount), due);
    EXPECT_EQ(t.Results(GearDevice::Mount), results);
    EXPECT_EQ(t.LastReason(GearDevice::Mount), "no response from the Alpaca server (Couldn't connect to server)");

    // the server reports it connected with too little time left: the next attempt is brought forward, nothing else
    t.OnGuideOutOfTime(GearDevice::Mount, kT0 + 4000, true);
    EXPECT_EQ(t.NextDueMs(GearDevice::Mount), kT0 + 4000);
    EXPECT_TRUE(t.Due(GearDevice::Mount, kT0 + 4000));
    EXPECT_EQ(t.Attempts(GearDevice::Mount), 1);
    // never pushed back
    t.OnGuideOutOfTime(GearDevice::Mount, kT0 + 9000, true);
    EXPECT_EQ(t.NextDueMs(GearDevice::Mount), kT0 + 4000);

    // a device that was not retrying is handed to the retry, due now, with no failure counted
    Tracker fresh;
    fresh.OnGuideOutOfTime(GearDevice::Camera, kT0, false);
    EXPECT_TRUE(fresh.Retrying(GearDevice::Camera));
    EXPECT_TRUE(fresh.Due(GearDevice::Camera, kT0));
    EXPECT_EQ(fresh.Attempts(GearDevice::Camera), 0);
    EXPECT_EQ(fresh.Results(GearDevice::Camera), 0u);
    // its first real failure starts the schedule at 15 s
    fresh.OnConnectResult(GearDevice::Camera, false, kT0 + 500, "HTTP 500");
    EXPECT_EQ(fresh.Attempts(GearDevice::Camera), 1);
    EXPECT_EQ(fresh.NextDueMs(GearDevice::Camera), kT0 + 500 + 15000);

    // a device the user disconnected is left alone
    Tracker off;
    off.OnUserDisconnect(GearDevice::Mount);
    off.OnGuideOutOfTime(GearDevice::Mount, kT0, true);
    EXPECT_FALSE(off.Retrying(GearDevice::Mount));
    EXPECT_TRUE(off.UserDisconnected(GearDevice::Mount));
}

TEST(ConnectRetry, GuidePlanOrder)
{
    const ProbeKind kinds[] = { ProbeKind::Alpaca, ProbeKind::NotAlpaca, ProbeKind::NotConfigured };
    const Intent intents[] = { Intent::None, Intent::Wanted, Intent::UserDisconnected };
    for (ProbeKind k : kinds)
    {
        for (Intent i : intents)
        {
            for (bool refused : { false, true })
            {
                // nothing to do for a device that is not selected or already connected
                EXPECT_EQ(PlanGuideConnect(false, false, i, refused, k), GuideStep::Nothing);
                EXPECT_EQ(PlanGuideConnect(true, true, i, refused, k), GuideStep::Nothing);
            }
            // a device the user disconnected is never probed or connected by guide, whatever else holds
            if (i == Intent::UserDisconnected)
            {
                EXPECT_EQ(PlanGuideConnect(true, false, i, false, k), GuideStep::UserDisconnected);
                EXPECT_EQ(PlanGuideConnect(true, false, i, true, k), GuideStep::UserDisconnected);
            }
            else
            {
                // a refused connect is not probed either
                EXPECT_EQ(PlanGuideConnect(true, false, i, true, k), GuideStep::Refused);
            }
        }
    }
    for (Intent i : { Intent::None, Intent::Wanted })
    {
        EXPECT_EQ(PlanGuideConnect(true, false, i, false, ProbeKind::NotConfigured), GuideStep::NotConfigured);
        EXPECT_EQ(PlanGuideConnect(true, false, i, false, ProbeKind::Alpaca), GuideStep::ProbeThenConnect);
        EXPECT_EQ(PlanGuideConnect(true, false, i, false, ProbeKind::NotAlpaca), GuideStep::Connect);
    }
}

TEST(ConnectRetry, GuideWaitsForTheBackgroundProbeInsteadOfProbingAgain)
{
    EXPECT_EQ(PlanGuideConnect(true, false, Intent::Wanted, false, ProbeKind::Alpaca, true), GuideStep::WaitForProbe);
    // the earlier rules still win
    EXPECT_EQ(PlanGuideConnect(true, true, Intent::Wanted, false, ProbeKind::Alpaca, true), GuideStep::Nothing);
    EXPECT_EQ(PlanGuideConnect(true, false, Intent::UserDisconnected, false, ProbeKind::Alpaca, true),
              GuideStep::UserDisconnected);
    EXPECT_EQ(PlanGuideConnect(true, false, Intent::Wanted, true, ProbeKind::Alpaca, true), GuideStep::Refused);
    EXPECT_EQ(PlanGuideConnect(true, false, Intent::Wanted, false, ProbeKind::Alpaca, false), GuideStep::ProbeThenConnect);
}

TEST(ConnectRetry, ProbeHandoffGuideTakesAFinishedResult)
{
    ProbeHandoff h;
    h.Begin();
    ProbeResult in;
    in.connected = true;
    h.Finish(in);
    ProbeResult out;
    EXPECT_TRUE(h.WaitAndTake(0, &out));
    EXPECT_TRUE(out.connected);
    // the CallAfter delivery that follows finds it taken
    ProbeResult again;
    EXPECT_FALSE(h.TakeDelivered(&again));
    EXPECT_FALSE(h.WaitAndTake(0, &again));
}

TEST(ConnectRetry, ProbeHandoffGuideWaitsForTheProbeThread)
{
    ProbeHandoff h;
    h.Begin();
    std::thread probe(
        [&h]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ProbeResult r;
            r.reason = "The mount did not reply (Alpaca 1280)";
            h.Finish(r);
        });
    ProbeResult out;
    EXPECT_TRUE(h.WaitAndTake(5000, &out));
    probe.join();
    EXPECT_FALSE(out.connected);
    EXPECT_EQ(out.reason, "The mount did not reply (Alpaca 1280)");
    EXPECT_FALSE(h.TakeDelivered(&out));
}

TEST(ConnectRetry, ProbeHandoffGuideOutOfTimeLeavesTheResultToTheDelivery)
{
    ProbeHandoff h;
    h.Begin();
    ProbeResult out;
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(h.WaitAndTake(30, &out)); // still connecting
    EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(25));
    EXPECT_FALSE(h.WaitAndTake(-5, &out)); // no time left: no wait at all

    ProbeResult in;
    in.connected = true;
    h.Finish(in);
    EXPECT_TRUE(h.TakeDelivered(&out)); // the retry finishes the job
    EXPECT_TRUE(out.connected);
    EXPECT_FALSE(h.TakeDelivered(&out)); // once

    // the next probe starts clean
    h.Begin();
    EXPECT_FALSE(h.TakeDelivered(&out));
}

// ---- the probe ----

namespace
{
// A scripted Alpaca server: each GET / PUT takes the next scripted answer (the last one repeats).
struct FakeOps : public ProbeOps
{
    struct Get
    {
        bool ok;
        bool connected;
        AlpacaErrors::CallError error;
    };
    std::deque<Get> gets;
    bool putOk = true;
    AlpacaErrors::CallError putError;
    int64_t now = 0;
    int getCalls = 0;
    int putCalls = 0;
    bool cancelAfterPut = false;
    bool cancelled = false;
    // how long each call takes; a call longer than its budget times out at the budget, as curl does
    int64_t getCostMs = 10;
    int64_t putCostMs = 10;
    std::vector<int64_t> budgets; // every SetCallBudgetMs
    int64_t budget = -1;

    // spends the call's time; false when the budget ran out first
    bool Spend(int64_t cost, AlpacaErrors::CallError *error)
    {
        int64_t b = budget;
        budget = -1;
        if (b > 0 && cost > b)
        {
            now += b;
            error->kind = AlpacaErrors::CallError::Kind::Transport;
            error->message = "Timeout was reached";
            return false;
        }
        now += cost;
        return true;
    }

    bool GetConnected(bool *connected, AlpacaErrors::CallError *error) override
    {
        getCalls++;
        Get g = gets.front();
        if (gets.size() > 1)
            gets.pop_front();
        if (!Spend(getCostMs, error))
            return false;
        *connected = g.connected;
        if (!g.ok)
            *error = g.error;
        return g.ok;
    }
    bool PutConnected(AlpacaErrors::CallError *error) override
    {
        putCalls++;
        if (!Spend(putCostMs, error))
            return false;
        if (cancelAfterPut)
            cancelled = true;
        if (!putOk)
            *error = putError;
        return putOk;
    }
    void SetCallBudgetMs(int64_t ms) override
    {
        budgets.push_back(ms);
        budget = ms;
    }
    void SleepMs(int ms) override { now += ms; }
    int64_t NowMs() override { return now; }
    bool Cancelled() override { return cancelled; }
};

AlpacaErrors::CallError AlpacaErr(long n, const std::string& msg)
{
    AlpacaErrors::CallError e;
    e.kind = AlpacaErrors::CallError::Kind::Alpaca;
    e.httpStatus = 200;
    e.alpacaError = n;
    e.message = msg;
    return e;
}

AlpacaErrors::CallError TransportErr(const std::string& msg)
{
    AlpacaErrors::CallError e;
    e.kind = AlpacaErrors::CallError::Kind::Transport;
    e.message = msg;
    return e;
}
} // namespace

TEST(ConnectProbe, AlreadyConnectedNeedsNoPut)
{
    FakeOps ops;
    ops.gets = { { true, true, {} } };
    ProbeResult r = RunProbe(ops, "mount");
    EXPECT_TRUE(r.connected);
    EXPECT_EQ(r.reason, "");
    EXPECT_EQ(ops.putCalls, 0);
}

TEST(ConnectProbe, UnreachableServer)
{
    FakeOps ops;
    ops.gets = { { false, false, TransportErr("Couldn't connect to server") } };
    ProbeResult r = RunProbe(ops, "mount");
    EXPECT_FALSE(r.connected);
    EXPECT_EQ(r.reason, "no response from the Alpaca server (Couldn't connect to server)");
    EXPECT_EQ(ops.putCalls, 0);
}

TEST(ConnectProbe, RefusedConnectGivesTheServerReason)
{
    FakeOps ops;
    ops.gets = { { true, false, {} } };
    ops.putOk = false;
    ops.putError = AlpacaErr(1280, "The Sky-Watcher mount did not reply (Timeout waiting for motor controller reply)");
    ProbeResult r = RunProbe(ops, "mount");
    EXPECT_FALSE(r.connected);
    EXPECT_EQ(r.reason, "The Sky-Watcher mount did not reply (Timeout waiting for motor controller reply) (Alpaca 1280)");
    EXPECT_EQ(ops.putCalls, 1);
}

TEST(ConnectProbe, WaitsForTheServerToReportConnected)
{
    FakeOps ops;
    ops.gets = { { true, false, {} }, { true, false, {} }, { true, false, {} }, { true, true, {} } };
    ProbeResult r = RunProbe(ops, "camera");
    EXPECT_TRUE(r.connected);
    EXPECT_EQ(ops.putCalls, 1);
    EXPECT_EQ(ops.getCalls, 4);
}

TEST(ConnectProbe, GivesUpWhenTheDeviceNeverReportsConnected)
{
    FakeOps ops;
    ops.gets = { { true, false, {} } };
    ProbeResult r = RunProbe(ops, "camera", 1000, 250);
    EXPECT_FALSE(r.connected);
    EXPECT_EQ(r.reason, "the camera did not report connected within 1 s");
    // bounded: about connectWait / pollInterval polls, not more
    EXPECT_LE(ops.getCalls, 1 + 1000 / 250 + 1);
}

TEST(ConnectProbe, PollErrorsAreKeptAsTheReason)
{
    FakeOps ops;
    ops.gets = { { true, false, {} }, { false, false, AlpacaErr(1031, "Camera not connected") } };
    ProbeResult r = RunProbe(ops, "camera", 1000, 250);
    EXPECT_FALSE(r.connected);
    EXPECT_EQ(r.reason, "the camera did not report connected within 1 s: Camera not connected (Alpaca 1031)");
}

TEST(ConnectProbe, CancelStopsThePolls)
{
    FakeOps ops;
    ops.gets = { { true, false, {} } };
    ops.cancelAfterPut = true;
    ProbeResult r = RunProbe(ops, "mount");
    EXPECT_FALSE(r.connected);
    EXPECT_TRUE(r.cancelled);
    EXPECT_EQ(r.reason, "cancelled");
    EXPECT_EQ(ops.getCalls, 1);

    FakeOps early;
    early.cancelled = true;
    early.gets = { { true, true, {} } };
    ProbeResult e = RunProbe(early, "mount");
    EXPECT_FALSE(e.connected);
    EXPECT_TRUE(e.cancelled);
    EXPECT_EQ(early.getCalls, 0);
}

TEST(ConnectProbe, CancelBetweenTheGetAndThePutSendsNoPut)
{
    // the user disconnects the mount (or changes its selection or server) while the probe's GET is on the wire
    struct CancelDuringGet : public FakeOps
    {
        bool GetConnected(bool *connected, AlpacaErrors::CallError *error) override
        {
            bool ok = FakeOps::GetConnected(connected, error);
            cancelled = true;
            return ok;
        }
    } ops;
    ops.gets = { { true, false, {} } };
    ProbeResult r = RunProbe(ops, "mount");
    EXPECT_FALSE(r.connected);
    EXPECT_TRUE(r.cancelled);
    EXPECT_EQ(ops.getCalls, 1);
    EXPECT_EQ(ops.putCalls, 0);

    // an answer that the probe got is not marked cancelled
    FakeOps done;
    done.gets = { { false, false, TransportErr("Couldn't connect to server") } };
    EXPECT_FALSE(RunProbe(done, "mount").cancelled);
}

TEST(ConnectProbe, AnEmptyErrorStillGivesAReason)
{
    FakeOps ops;
    ops.gets = { { false, false, AlpacaErrors::CallError() } }; // Kind::None from a broken caller
    ProbeResult r = RunProbe(ops, "rotator");
    EXPECT_FALSE(r.connected);
    EXPECT_EQ(r.reason, "the rotator did not answer");
}

// ---- the guide RPC's budget (a deadline over the whole probe) ----

TEST(ConnectProbe, NoDeadlineSetsNoBudget)
{
    FakeOps ops;
    ops.gets = { { true, false, {} }, { true, true, {} } };
    ops.putCostMs = 9000; // the background retry's long PUT is allowed its own timeout
    EXPECT_TRUE(RunProbe(ops, "mount").connected);
    EXPECT_TRUE(ops.budgets.empty());
}

TEST(ConnectProbe, AStalledPutEndsAtTheDeadline)
{
    // the server answers GET connected at once, then holds PUT Connected=true (a bridge opening a powered-off mount)
    FakeOps ops;
    ops.gets = { { true, false, {} } };
    ops.putCostMs = 10000;
    ProbeResult r = RunProbe(ops, "mount", 2000, 250, 4000);
    EXPECT_FALSE(r.connected);
    EXPECT_FALSE(r.cancelled);
    EXPECT_EQ(r.reason, "no response from the Alpaca server (Timeout was reached)");
    EXPECT_FALSE(r.outOfTime); // the PUT was sent: a real attempt that timed out
    EXPECT_EQ(ops.now, 4000);
    // each call got what was left: the GET all of it, the PUT the rest
    ASSERT_EQ(ops.budgets.size(), 2u);
    EXPECT_EQ(ops.budgets[0], 4000);
    EXPECT_EQ(ops.budgets[1], 3990);
}

TEST(ConnectProbe, NoTimeLeftMakesNoRequest)
{
    FakeOps ops;
    ops.now = 5000;
    ops.gets = { { true, true, {} } };
    ProbeResult r = RunProbe(ops, "mount", 2000, 250, 4000);
    EXPECT_FALSE(r.connected);
    EXPECT_EQ(r.reason, kOutOfTimeReason);
    EXPECT_EQ(r.reason, "no response from the Alpaca server (Timeout was reached)");
    EXPECT_TRUE(r.outOfTime);
    EXPECT_EQ(ops.getCalls, 0);

    // time runs out between the GET and the PUT
    FakeOps slowGet;
    slowGet.gets = { { true, false, {} } };
    slowGet.getCostMs = 1000;
    ProbeResult r2 = RunProbe(slowGet, "mount", 2000, 250, 1000);
    EXPECT_EQ(r2.reason, kOutOfTimeReason);
    EXPECT_TRUE(r2.outOfTime);
    EXPECT_EQ(slowGet.putCalls, 0);
}

TEST(ConnectProbe, TheWaitForConnectedEndsWithTheDeadline)
{
    FakeOps ops;
    ops.gets = { { true, false, {} } };
    ProbeResult r = RunProbe(ops, "camera", 2000, 250, 1000);
    EXPECT_FALSE(r.connected);
    EXPECT_EQ(r.reason, "the camera did not report connected in time");
    EXPECT_FALSE(r.outOfTime);
    EXPECT_LE(ops.now, 1000);
    for (int64_t b : ops.budgets)
        EXPECT_GT(b, 0);

    // a deadline later than the wait leaves the wait's own text
    FakeOps later;
    later.gets = { { true, false, {} } };
    ProbeResult r2 = RunProbe(later, "camera", 1000, 250, 100000);
    EXPECT_EQ(r2.reason, "the camera did not report connected within 1 s");
}

TEST(ConnectProbe, ADeviceThatConnectsWithinTheBudgetConnects)
{
    FakeOps ops;
    ops.gets = { { true, false, {} }, { true, false, {} }, { true, true, {} } };
    ops.putCostMs = 2500; // a synchronous connect that finishes inside the budget
    ProbeResult r = RunProbe(ops, "mount", 2000, 250, 4000);
    EXPECT_TRUE(r.connected);
    EXPECT_FALSE(r.outOfTime);
    EXPECT_LE(ops.now, 4000);
}
