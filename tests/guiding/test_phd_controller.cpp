// PhdController's state machine driven through a recording ControllerHost (open-astro/openastro-guider#142,
// slice T2f of #115 steps 4-9).
//
// A guide or dither request is accepted by PhdController::Guide / Dither and then worked through frame by
// frame (UpdateControllerState, after the guider's own update): start capturing, select a star, calibrate,
// guide, settle, finish. Ara waits for exactly one SettleDone per accepted request before it opens the next
// sub, so a missing one costs it the settle timeout and a stray one ends a later wait early. These tests run
// the production phdcontrol.cpp against a fake of everything it asks (FakeHost below), record what it asks
// for in order, and pin: one SettleDone on every way out (settled, timed out, each failure, a stop or an
// abort in each state, and the #255 calibration that entering guiding starts); the Dec guide mode plan of a
// dither (RA only, or Auto for the settle and the user's mode back after it); and what a star that is
// auto-selected but never becomes selected does (STATE_WAIT_SELECTED).
//
// #115 step 6 rewrites every collaborator behind the host; the host keeps the controller's decisions here.

#include <gtest/gtest.h>

#include "controller_host.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace
{

int64_t g_nowMs = 0;

int64_t NowMs()
{
    return g_nowMs;
}

const char *ModeName(ControllerDecMode m)
{
    switch (m)
    {
    case ControllerDecMode::None:
        return "None";
    case ControllerDecMode::Auto:
        return "Auto";
    case ControllerDecMode::North:
        return "North";
    case ControllerDecMode::South:
        return "South";
    }
    return "?";
}

int Idx(ControllerMount m)
{
    return m == ControllerMount::Primary ? 0 : 1;
}

// The guider, frame, mounts and event server as PhdController sees them. Queries answer from the fields and
// are not recorded; every call that does something is recorded, in order.
class FakeHost : public ControllerHost
{
public:
    // equipment
    bool camera = true;
    bool hasMount[2] = { true, false };
    bool connected[2] = { true, true };
    bool calibrated[2] = { true, true };
    bool stepGuider = false;
    bool aoBump = false;
    ControllerDecMode decMode = ControllerDecMode::Auto;

    // the frame
    bool captureActive = false;
    bool autoExposure = false;
    bool ditherRaOnlySetting = false;
    bool ditherFails = false;
    bool startGuidingFails = false;
    bool startGuidingCalibrates = false; // #255: entering guiding clears the calibration and starts a new one
    std::deque<bool> autoSelectErrors; // the next AutoSelectStar results; none left: success
    double guideError = 0.;

    // the guider
    GUIDER_STATE state = STATE_UNINITIALIZED;
    bool starFoundAfterSelect = true; // a SELECTING star is found, SELECTED, on the next frame
    bool locked = true;
    bool clearForRecalibration = true;
    bool sticky = false;

    std::vector<std::string> log;

    // a frame, as MyFrame::OnExposeComplete runs it: the guider's update, then the controller's
    void Frame(bool dropped = false)
    {
        if (state == STATE_SELECTING && starFoundAfterSelect)
            state = STATE_SELECTED;
        PhdController::UpdateControllerState(dropped);
    }

    // the guider's calibration completes
    void CompleteCalibration()
    {
        calibrated[0] = calibrated[1] = true;
        state = STATE_GUIDING;
    }

    int Count(const std::string& entry) const
    {
        int n = 0;
        for (const auto& e : log)
            if (e == entry)
                n++;
        return n;
    }
    int CountPrefix(const std::string& prefix) const
    {
        int n = 0;
        for (const auto& e : log)
            if (e.rfind(prefix, 0) == 0)
                n++;
        return n;
    }
    std::vector<std::string> SettleDones() const
    {
        std::vector<std::string> out;
        for (const auto& e : log)
            if (e.rfind("SettleDone", 0) == 0)
                out.push_back(e);
        return out;
    }
    // the log without the Settling events, which each settle frame sends
    std::vector<std::string> Calls() const
    {
        std::vector<std::string> out;
        for (const auto& e : log)
            if (e != "Settling")
                out.push_back(e);
        return out;
    }

    // ControllerHost
    SettleTracker::ClockMs SettleClock() const override { return &NowMs; }

    bool CameraConnected() const override { return camera; }
    bool HasMount(ControllerMount m) const override { return hasMount[Idx(m)]; }
    bool MountConnected(ControllerMount m) const override { return connected[Idx(m)]; }
    bool MountCalibrated(ControllerMount m) const override { return calibrated[Idx(m)]; }
    void ClearMountCalibration(ControllerMount m) override
    {
        log.push_back(m == ControllerMount::Primary ? "ClearCalibration(primary)" : "ClearCalibration(secondary)");
        calibrated[Idx(m)] = false;
    }
    bool PrimaryMountIsStepGuider() const override { return stepGuider; }
    bool AoBumpInProgress() const override { return aoBump; }
    ControllerDecMode PrimaryScopeDecGuideMode() const override { return decMode; }
    void SetScopeDecGuideMode(ControllerDecMode mode) override
    {
        log.push_back(std::string("SetDecGuideMode(") + ModeName(mode) + ")");
        decMode = mode;
    }
    wxString DecGuideModeStr(ControllerDecMode mode) const override { return ModeName(mode); }
    void NotifyMountSettleDone(bool success) override { log.push_back(success ? "MountSettleDone(1)" : "MountSettleDone(0)"); }

    bool CaptureActive() const override { return captureActive; }
    void StartCapturing() override
    {
        log.push_back("StartCapturing");
        captureActive = true;
    }
    bool StartGuiding() override
    {
        log.push_back("StartGuiding");
        if (startGuidingFails)
            return true;
        if (startGuidingCalibrates)
            calibrated[0] = false;
        state = calibrated[0] && (!hasMount[1] || calibrated[1]) ? STATE_GUIDING : STATE_CALIBRATING_PRIMARY;
        return false;
    }
    bool AutoSelectStar(const wxRect&, bool willForceRecal) override
    {
        log.push_back(willForceRecal ? "AutoSelectStar(recal)" : "AutoSelectStar");
        bool error = false;
        if (!autoSelectErrors.empty())
        {
            error = autoSelectErrors.front();
            autoSelectErrors.pop_front();
        }
        if (!error)
            state = STATE_SELECTING;
        return error;
    }
    bool AutoExposureEnabled() const override { return autoExposure; }
    void ResetAutoExposure() override { log.push_back("ResetAutoExposure"); }
    bool DitherRaOnlySetting() const override { return ditherRaOnlySetting; }
    bool Dither(double pixels, bool raOnly) override
    {
        log.push_back(std::string("Dither(") + std::to_string((int) pixels) + (raOnly ? ", RA only)" : ")"));
        return ditherFails;
    }
    double CurrentGuideError() const override { return guideError; }

    GUIDER_STATE GuiderState() const override { return state; }
    bool GuiderIsCalibratingOrGuiding() const override { return state >= STATE_CALIBRATING_PRIMARY && state <= STATE_GUIDING; }
    bool GuiderIsCalibrating() const override { return state >= STATE_CALIBRATING_PRIMARY && state < STATE_CALIBRATED; }
    bool GuiderIsLocked() const override { return locked; }
    bool SelectedStarClearForRecalibration() const override { return clearForRecalibration; }
    void ResetGuider(bool fullReset) override
    {
        log.push_back(fullReset ? "ResetGuider(full)" : "ResetGuider");
        state = STATE_UNINITIALIZED;
    }
    void ForceFullFrame() override { log.push_back("ForceFullFrame"); }
    bool LockPosIsSticky() const override { return sticky; }
    void SetLockPosIsSticky(bool s) override
    {
        log.push_back(s ? "SetLockPosIsSticky(1)" : "SetLockPosIsSticky(0)");
        sticky = s;
    }

    void NotifySettleBegin() override { log.push_back("SettleBegin"); }
    void NotifySettling(double, double, double, bool) override { log.push_back("Settling"); }
    void NotifySettleDone(const wxString& errorMsg, int totalFrames, int droppedFrames) override
    {
        log.push_back("SettleDone(" + errorMsg.str() + "|" + std::to_string(totalFrames) + "|" + std::to_string(droppedFrames) +
                      ")");
    }
    void LogSettlingStateChange(const wxString& msg) override { log.push_back("Log(" + msg.str() + ")"); }
};

SettleParams Settle(double px = 1.5, int timeSec = 2, int timeoutSec = 30)
{
    SettleParams s;
    s.tolerancePx = px;
    s.settleTimeSec = timeSec;
    s.timeoutSec = timeoutSec;
    s.frames = 99999; // what the guide and dither RPCs give a settle by pixels and time
    return s;
}

const std::string kDoneOk = "SettleDone(|3|0)"; // settled on its 3rd frame, at 2 s in range
const std::string kNoStar = "failed to find a suitable guide star";

class PhdControllerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_nowMs = 1000000;
        PhdController::OnAppInit(host);
    }

    // frames 1 s apart until the controller is idle; returns the frames it took
    int RunToIdle(int maxFrames = 200)
    {
        for (int i = 1; i <= maxFrames; i++)
        {
            g_nowMs += 1000;
            host.Frame();
            if (PhdController::IsIdle())
                return i;
        }
        return -1;
    }

    // guiding on a calibrated mount with a star, capture active: where a dither or a guide without
    // recalibration starts
    void Guiding()
    {
        host.captureActive = true;
        host.state = STATE_GUIDING;
    }

    bool Guide(unsigned options, const SettleParams& settle = Settle())
    {
        wxString err;
        bool ok = PhdController::Guide(options, settle, wxRect(), &err);
        lastError = err.str();
        return ok;
    }

    bool Dither(double px, bool raOnly, const SettleParams& settle = Settle())
    {
        wxString err;
        bool ok = PhdController::Dither(px, raOnly, settle, &err);
        lastError = err.str();
        return ok;
    }

    FakeHost host;
    std::string lastError;
};

} // namespace

// --- the guide request, on every way out

TEST_F(PhdControllerTest, GuideFromStoppedWithRecalibrationCalibratesThenSettlesOnce)
{
    // a new target: Ara's guide with recalibrate true (GUIDEOPT_FORCE_RECAL) and the sticky lock it asks for
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL | GUIDEOPT_USE_STICKY_LOCK));
    EXPECT_FALSE(PhdController::IsIdle());
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> { "ResetGuider(full)", "ForceFullFrame", "ResetAutoExposure", "StartCapturing" }));
    host.log.clear();

    host.Frame(); // STATE_SELECT_STAR, with the calibration walk's edge margin (#140)
    host.Frame(); // the star selected: STATE_CALIBRATE clears the calibration and starts it
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> { "AutoSelectStar(recal)", "ClearCalibration(primary)", "SetLockPosIsSticky(1)",
                                          "StartGuiding" }));
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
    EXPECT_FALSE(host.calibrated[0]);
    host.log.clear();

    for (int i = 0; i < 5; i++)
        host.Frame(); // calibrating
    EXPECT_TRUE(host.log.empty());

    host.CompleteCalibration();
    host.guideError = 0.5;
    EXPECT_EQ(RunToIdle(), 4); // the frame that begins the settle, then 3
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> { "SetLockPosIsSticky(0)", "SettleBegin", "Log(Settling started)", kDoneOk,
                                          "Log(Settling complete)", "MountSettleDone(1)" }));
    EXPECT_EQ(host.Count("Settling"), 2);
    EXPECT_FALSE(host.sticky);
}

TEST_F(PhdControllerTest, GuideWithRecalibrationClearsBothMounts)
{
    host.hasMount[1] = true; // an AO: the step guider's calibration and the mount's
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL));
    host.Frame();
    host.Frame();
    EXPECT_EQ(host.Count("ClearCalibration(primary)"), 1);
    EXPECT_EQ(host.Count("ClearCalibration(secondary)"), 1);
    EXPECT_EQ(host.CountPrefix("SetLockPosIsSticky"), 0); // no sticky lock asked for
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
    host.CompleteCalibration();
    EXPECT_EQ(RunToIdle(), 4);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { kDoneOk }));
}

TEST_F(PhdControllerTest, GuideWithRecalibrationWhileGuidingCalibratesAgain)
{
    Guiding();
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL | GUIDEOPT_USE_STICKY_LOCK));
    // straight to STATE_CALIBRATE within the request: no new capture, no star selection
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> { "ClearCalibration(primary)", "SetLockPosIsSticky(1)", "StartGuiding" }));
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
    host.CompleteCalibration();
    EXPECT_EQ(RunToIdle(), 4);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { kDoneOk }));
    EXPECT_EQ(host.Count("SettleBegin"), 1);
}

TEST_F(PhdControllerTest, GuideWithoutRecalibrationWhileGuidingBeginsTheSettleBeforeItReturns)
{
    Guiding();
    ASSERT_TRUE(Guide(0));
    EXPECT_EQ(host.Calls(), (std::vector<std::string> { "SettleBegin", "Log(Settling started)" }));
    EXPECT_EQ(RunToIdle(), 3);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { kDoneOk }));
    EXPECT_EQ(host.CountPrefix("StartGuiding") + host.CountPrefix("ClearCalibration"), 0);
}

TEST_F(PhdControllerTest, GuideOnACalibratedMountFromASelectedStarStartsGuiding)
{
    host.captureActive = true;
    host.state = STATE_SELECTED;
    ASSERT_TRUE(Guide(0));
    EXPECT_EQ(host.Calls(), (std::vector<std::string> { "StartGuiding" }));
    EXPECT_EQ(RunToIdle(), 4); // the frame that begins the settle, then 3
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> { "StartGuiding", "SettleBegin", "Log(Settling started)", kDoneOk,
                                          "Log(Settling complete)", "MountSettleDone(1)" }));
}

TEST_F(PhdControllerTest, SettleTimeoutEndsTheRequestWithOneSettleDone)
{
    Guiding();
    host.guideError = 5.0; // never within 1.5 px
    ASSERT_TRUE(Guide(0, Settle(1.5, 2, 10)));
    EXPECT_EQ(RunToIdle(), 10);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { "SettleDone(timed-out waiting for guider to settle|10|0)" }));
    EXPECT_EQ(host.Count("MountSettleDone(0)"), 1);
    EXPECT_EQ(host.Count("Log(Settling failed)"), 1);
}

TEST_F(PhdControllerTest, RefusedWhileARequestIsInProgress)
{
    Guiding();
    ASSERT_TRUE(Guide(0));
    EXPECT_FALSE(Guide(0));
    EXPECT_EQ(lastError, "Cannot initiate guide while guide is in progress");
    EXPECT_FALSE(Dither(3, false));
    EXPECT_EQ(lastError, "Cannot initiate dither while guide is in progress");
    EXPECT_EQ(RunToIdle(), 3);
    ASSERT_TRUE(Dither(3, false));
    EXPECT_FALSE(Guide(0));
    EXPECT_EQ(lastError, "Cannot initiate guide while dither is in progress");
    EXPECT_EQ(RunToIdle(), 3);
    EXPECT_EQ(host.SettleDones().size(), 2u);
}

// each failure of a guide request: one SettleDone with its reason, 0 frames, and no SettleBegin
struct FailureCase
{
    const char *name;
    void (*arrange)(FakeHost&);
    unsigned options;
    const char *reason;
};

class PhdControllerFailureTest : public PhdControllerTest, public ::testing::WithParamInterface<FailureCase>
{
};

TEST_P(PhdControllerFailureTest, OneSettleDoneWithTheReason)
{
    const FailureCase& c = GetParam();
    c.arrange(host);
    ASSERT_TRUE(Guide(c.options)); // accepted: the failure is reported in the SettleDone
    EXPECT_GE(RunToIdle(), 0);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { std::string("SettleDone(") + c.reason + "|0|0)" }));
    EXPECT_EQ(host.Count("SettleBegin"), 0);
    EXPECT_EQ(host.Count("MountSettleDone(0)"), 1);
    EXPECT_EQ(host.CountPrefix("SetDecGuideMode"), 0);
    EXPECT_FALSE(host.sticky); // the sticky-lock setting is back
    // nothing more after it
    size_t n = host.log.size();
    RunToIdle(5);
    PhdController::AbortController("late abort");
    EXPECT_EQ(host.log.size(), n);
}

INSTANTIATE_TEST_SUITE_P(
    Failures, PhdControllerFailureTest,
    ::testing::Values(FailureCase { "CameraDisconnected", [](FakeHost& h) { h.camera = false; }, GUIDEOPT_FORCE_RECAL,
                                    "all equipment must be connected first" },
                      FailureCase { "MountDisconnected", [](FakeHost& h) { h.connected[0] = false; }, 0,
                                    "all equipment must be connected first" },
                      FailureCase { "SecondaryMountDisconnected",
                                    [](FakeHost& h)
                                    {
                                        h.hasMount[1] = true;
                                        h.connected[1] = false;
                                    },
                                    0, "all equipment must be connected first" },
                      FailureCase { "AutoSelectFailsThreeTimes", [](FakeHost& h) { h.autoSelectErrors = { true, true, true }; },
                                    GUIDEOPT_FORCE_RECAL | GUIDEOPT_USE_STICKY_LOCK, "failed to find a suitable guide star" },
                      FailureCase { "StartCalibrationFails", [](FakeHost& h) { h.startGuidingFails = true; },
                                    GUIDEOPT_FORCE_RECAL | GUIDEOPT_USE_STICKY_LOCK, "could not start calibration" },
                      FailureCase { "StartGuidingFails",
                                    [](FakeHost& h)
                                    {
                                        h.captureActive = true;
                                        h.state = STATE_SELECTED;
                                        h.startGuidingFails = true;
                                    },
                                    GUIDEOPT_USE_STICKY_LOCK, "could not start guiding" }),
    [](const ::testing::TestParamInfo<FailureCase>& info) { return std::string(info.param.name); });

TEST_F(PhdControllerTest, AutoSelectFailuresResetTheGuiderAndTryAgainOnTheNextFrame)
{
    host.autoSelectErrors = { true, true, false };
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL));
    host.log.clear();
    host.Frame(); // 1st try fails
    host.Frame(); // STATE_ATTEMPT_START, then the 2nd try fails
    host.Frame(); // the 3rd succeeds
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> { "AutoSelectStar(recal)", "ResetGuider(full)", "AutoSelectStar(recal)",
                                          "ResetGuider(full)", "AutoSelectStar(recal)" }));
    host.Frame();
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
}

TEST_F(PhdControllerTest, StartCalibrationFailureLeavesTheCalibrationCleared)
{
    host.startGuidingFails = true;
    host.sticky = false;
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL | GUIDEOPT_USE_STICKY_LOCK));
    RunToIdle();
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> {
                  "ResetGuider(full)", "ForceFullFrame", "ResetAutoExposure", "StartCapturing", "AutoSelectStar(recal)",
                  "ClearCalibration(primary)", "SetLockPosIsSticky(1)", "StartGuiding", "SetLockPosIsSticky(0)",
                  "SettleDone(could not start calibration|0|0)", "Log(Settling failed)", "MountSettleDone(0)" }));
    EXPECT_FALSE(host.calibrated[0]); // pinned: the request fails with the mount's calibration gone
}

// --- a stop or an abort: one SettleDone with its reason, in each state of a request

TEST_F(PhdControllerTest, AbortInEachStateSendsOneSettleDone)
{
    struct Case
    {
        const char *state;
        int frames; // after the guide request, before the abort
        bool guiding; // guiding already, no recalibration
    };
    for (const Case& c : { Case { "select star", 0, false }, Case { "wait selected", 1, false },
                           Case { "calibration wait", 3, false }, Case { "settle wait", 2, true } })
    {
        SCOPED_TRACE(c.state);
        host = FakeHost();
        PhdController::OnAppInit(host);
        if (c.guiding)
        {
            Guiding();
            host.guideError = 5.0;
        }
        ASSERT_TRUE(Guide(c.guiding ? 0 : GUIDEOPT_FORCE_RECAL | GUIDEOPT_USE_STICKY_LOCK));
        for (int i = 0; i < c.frames; i++)
        {
            g_nowMs += 1000;
            host.Frame();
        }
        ASSERT_FALSE(PhdController::IsIdle());
        // MyFrame::StopCapturing's reason (myframe_events.cpp)
        PhdController::AbortController("Image capture stopped");
        EXPECT_TRUE(PhdController::IsIdle());
        int frames = c.guiding ? c.frames : 0;
        int dropped = 0;
        EXPECT_EQ(host.SettleDones(),
                  (std::vector<std::string> { "SettleDone(Image capture stopped|" + std::to_string(frames) + "|" +
                                              std::to_string(dropped) + ")" }));
        EXPECT_EQ(host.Count("MountSettleDone(0)"), 1);
        EXPECT_FALSE(host.sticky);
        // idle: an abort and frames after it send nothing
        size_t n = host.log.size();
        PhdController::AbortController("Guiding stopped");
        host.Frame();
        EXPECT_EQ(host.log.size(), n);
    }
}

TEST_F(PhdControllerTest, AbortWhileIdleSendsNothing)
{
    PhdController::AbortController("Guiding stopped");
    EXPECT_TRUE(host.log.empty());
}

TEST_F(PhdControllerTest, AnAbortWaitingForTheCalibrationReportsNoFramesOfTheSettleBefore)
{
    Guiding();
    ASSERT_TRUE(Guide(0));
    host.guideError = 5.0;
    for (int i = 0; i < 12; i++)
    {
        g_nowMs += 1000;
        host.Frame();
    }
    PhdController::AbortController("Image capture stopped");
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { "SettleDone(Image capture stopped|12|0)" }));
    host.log.clear();
    host.guideError = 0.;
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL));
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
    PhdController::AbortController("Guiding stopped");
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { "SettleDone(Guiding stopped|0|0)" }));
}

// --- #255: entering guiding starts a calibration (an image scale change cleared the old one)

TEST_F(PhdControllerTest, ACalibrationStartedByEnteringGuidingIsWaitedForBeforeTheSettle)
{
    host.captureActive = true;
    host.state = STATE_SELECTED;
    host.startGuidingCalibrates = true;
    ASSERT_TRUE(Guide(GUIDEOPT_USE_STICKY_LOCK));
    EXPECT_EQ(host.Calls(), (std::vector<std::string> { "StartGuiding" }));
    host.Frame(); // STATE_SETTLE_BEGIN sees the calibration: the sticky lock as STATE_CALIBRATE sets it
    EXPECT_EQ(host.Calls(), (std::vector<std::string> { "StartGuiding", "SetLockPosIsSticky(1)" }));
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
    for (int i = 0; i < 4; i++)
        host.Frame();
    EXPECT_EQ(host.Count("SettleBegin"), 0);
    host.CompleteCalibration();
    EXPECT_EQ(RunToIdle(), 4);
    EXPECT_EQ(host.Calls(),
              (std::vector<std::string> { "StartGuiding", "SetLockPosIsSticky(1)", "SetLockPosIsSticky(0)", "SettleBegin",
                                          "Log(Settling started)", kDoneOk, "Log(Settling complete)", "MountSettleDone(1)" }));
}

TEST_F(PhdControllerTest, ACalibrationStartedAfterTheSettleBeganBeginsTheSettleAgain)
{
    // a dropped frame began the settle before the guider entered guiding and started a calibration (#255)
    Guiding();
    ASSERT_TRUE(Guide(0));
    EXPECT_EQ(host.Count("SettleBegin"), 1);
    host.calibrated[0] = false;
    host.state = STATE_CALIBRATING_PRIMARY;
    host.Frame(true);
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
    host.CompleteCalibration();
    EXPECT_EQ(RunToIdle(), 4);
    // pinned: two SettleBegin events for the one request, one SettleDone
    EXPECT_EQ(host.Count("SettleBegin"), 2);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { kDoneOk }));
}

TEST_F(PhdControllerTest, AnAbortDuringThe255CalibrationSendsOneSettleDone)
{
    host.captureActive = true;
    host.state = STATE_SELECTED;
    host.startGuidingCalibrates = true;
    ASSERT_TRUE(Guide(GUIDEOPT_USE_STICKY_LOCK));
    host.Frame();
    ASSERT_TRUE(PhdController::IsWaitingForCalibration());
    PhdController::AbortController("Image capture stopped");
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { "SettleDone(Image capture stopped|0|0)" }));
    EXPECT_EQ(host.Count("SettleBegin"), 0);
    EXPECT_FALSE(host.sticky);
}

// --- the Dec guide mode plan of a dither

struct DitherCase
{
    const char *name;
    ControllerDecMode mode;
    bool stepGuider;
    bool raOnlySetting;
    bool forceRaOnly;
    bool settleByFrames; // Dither(pixels, frames): no settle time (SETTLING_TIME_DISABLED)
    const char *dither; // the Dither call
    bool override; // Auto for the settle, the mode back after it
};

class PhdControllerDitherTest : public PhdControllerTest, public ::testing::WithParamInterface<DitherCase>
{
};

TEST_P(PhdControllerDitherTest, DecGuideModePlan)
{
    const DitherCase& c = GetParam();
    Guiding();
    host.decMode = c.mode;
    host.stepGuider = c.stepGuider;
    host.ditherRaOnlySetting = c.raOnlySetting;
    wxString err;
    if (c.settleByFrames)
        ASSERT_TRUE(PhdController::Dither(3, 5, &err));
    else
        ASSERT_TRUE(Dither(3, c.forceRaOnly));
    std::vector<std::string> begin = { c.dither, "SettleBegin", "Log(Settling started)" };
    if (c.override)
        begin.push_back("SetDecGuideMode(Auto)");
    EXPECT_EQ(host.Calls(), begin);
    host.log.clear();
    EXPECT_GE(RunToIdle(), 0);
    std::vector<std::string> end;
    if (c.override)
        end.push_back(std::string("SetDecGuideMode(") + ModeName(c.mode) + ")");
    end.insert(end.end(), { c.settleByFrames ? "SettleDone(|5|0)" : kDoneOk, "Log(Settling complete)", "MountSettleDone(1)" });
    EXPECT_EQ(host.Calls(), end);
    EXPECT_EQ(host.decMode, c.mode);
}

INSTANTIATE_TEST_SUITE_P(
    Dither, PhdControllerDitherTest,
    ::testing::Values(
        DitherCase { "Auto", ControllerDecMode::Auto, false, false, false, false, "Dither(3)", false },
        DitherCase { "NoneForcesRaOnly", ControllerDecMode::None, false, false, false, false, "Dither(3, RA only)", false },
        DitherCase { "NorthGoesAutoForTheSettle", ControllerDecMode::North, false, false, false, false, "Dither(3)", true },
        DitherCase { "SouthGoesAutoForTheSettle", ControllerDecMode::South, false, false, false, false, "Dither(3)", true },
        DitherCase { "NorthWithNoSettleTimeIsRaOnly", ControllerDecMode::North, false, false, false, true, "Dither(3, RA only)",
                     false },
        DitherCase { "NoneWithNoSettleTimeIsRaOnly", ControllerDecMode::None, false, false, false, true, "Dither(3, RA only)",
                     false },
        DitherCase { "ClientRaOnlyKeepsNorth", ControllerDecMode::North, false, false, true, false, "Dither(3, RA only)",
                     false },
        DitherCase { "SettingRaOnlyKeepsNorth", ControllerDecMode::North, false, true, false, false, "Dither(3, RA only)",
                     false },
        DitherCase { "StepGuiderIsNotAsked", ControllerDecMode::North, true, false, false, false, "Dither(3)", false }),
    [](const ::testing::TestParamInfo<DitherCase>& info) { return std::string(info.param.name); });

TEST_F(PhdControllerTest, TheDecGuideModeComesBackOnATimeoutAndOnAnAbort)
{
    for (bool timeout : { true, false })
    {
        SCOPED_TRACE(timeout ? "timeout" : "abort");
        host = FakeHost();
        PhdController::OnAppInit(host);
        Guiding();
        host.decMode = ControllerDecMode::North;
        host.guideError = 5.0;
        ASSERT_TRUE(Dither(3, false, Settle(1.5, 2, 4)));
        EXPECT_EQ(host.decMode, ControllerDecMode::Auto);
        if (timeout)
            EXPECT_EQ(RunToIdle(), 4);
        else
        {
            g_nowMs += 1000;
            host.Frame();
            PhdController::AbortController("Guiding stopped");
        }
        const std::string done =
            timeout ? "SettleDone(timed-out waiting for guider to settle|4|0)" : "SettleDone(Guiding stopped|1|0)";
        EXPECT_EQ(host.Calls(),
                  (std::vector<std::string> { "Dither(3)", "SettleBegin", "Log(Settling started)", "SetDecGuideMode(Auto)",
                                              "SetDecGuideMode(North)", done, "Log(Settling failed)", "MountSettleDone(0)" }));
        EXPECT_EQ(host.decMode, ControllerDecMode::North);
    }
}

TEST_F(PhdControllerTest, AFailedDitherIsRefusedAndSendsNothing)
{
    Guiding();
    host.decMode = ControllerDecMode::North;
    host.ditherFails = true;
    EXPECT_FALSE(Dither(3, false));
    EXPECT_EQ(lastError, "Dither error");
    EXPECT_EQ(host.Calls(), (std::vector<std::string> { "Dither(3)" }));
    EXPECT_TRUE(PhdController::IsIdle());
    EXPECT_EQ(host.decMode, ControllerDecMode::North);
}

TEST_F(PhdControllerTest, AGuideAfterADitherDoesNotRestoreItsDecGuideMode)
{
    // the override belongs to the dither's settle only: STATE_SETUP clears it for a guide request
    Guiding();
    host.decMode = ControllerDecMode::South;
    ASSERT_TRUE(Dither(3, false));
    EXPECT_EQ(RunToIdle(), 3);
    host.log.clear();
    ASSERT_TRUE(Guide(0));
    EXPECT_EQ(RunToIdle(), 3);
    EXPECT_EQ(host.CountPrefix("SetDecGuideMode"), 0);
}

// --- STATE_WAIT_SELECTED: a star auto-selected that never becomes selected

TEST_F(PhdControllerTest, AStarThatIsNeverSelectedEndsTheRequestAfterThreeAttempts)
{
    // AutoSelectStar finds a star (SELECTING), but the guider never finds it again on the next frames, so it
    // never reads SELECTED. STATE_WAIT_SELECTED waits 3 frames, and a wait that runs out is a failed attempt
    // like a failed auto-select (#289): after 3 the request ends with the SettleDone of 3 failed auto-selects.
    // It used to go back without spending an attempt and select again every 4 frames with no end.
    host.starFoundAfterSelect = false;
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL));
    EXPECT_EQ(RunToIdle(), 12); // 3 x (the selection and 3 frames of waiting)
    EXPECT_EQ(host.Count("AutoSelectStar(recal)"), 3);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { "SettleDone(" + kNoStar + "|0|0)" }));
    EXPECT_EQ(host.Count("SettleBegin") + host.CountPrefix("ClearCalibration") + host.CountPrefix("StartGuiding"), 0);
    EXPECT_EQ(host.Count("MountSettleDone(0)"), 1);
    EXPECT_TRUE(host.calibrated[0]); // never cleared: no calibration was started
}

TEST_F(PhdControllerTest, WaitsThatRunOutAndFailedAutoSelectsShareTheThreeAttempts)
{
    host.starFoundAfterSelect = false;
    host.autoSelectErrors = { false, true }; // a star never selected, then no star, then a star never selected
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL));
    EXPECT_EQ(RunToIdle(), 9);
    EXPECT_EQ(host.Count("AutoSelectStar(recal)"), 3);
    EXPECT_EQ(host.SettleDones(), (std::vector<std::string> { "SettleDone(" + kNoStar + "|0|0)" }));
}

TEST_F(PhdControllerTest, AStarSelectedWithinThreeFramesGoesOnToCalibrate)
{
    host.starFoundAfterSelect = false;
    ASSERT_TRUE(Guide(GUIDEOPT_FORCE_RECAL));
    host.Frame(); // selected: SELECTING
    host.Frame();
    host.Frame();
    host.starFoundAfterSelect = true; // found on the 3rd frame of the wait
    host.Frame();
    EXPECT_TRUE(PhdController::IsWaitingForCalibration());
    EXPECT_EQ(host.Count("AutoSelectStar(recal)"), 1);
}
