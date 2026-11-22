// Capture failure reasons (open-astro/openastro-guider#97).
//
// When the guide camera failed a frame, clients were told only "camera error --
// see debug log" (SingleFrameComplete) or "Stopped capturing" (SettleDone of a
// guide start), and the alert blamed the exposure parameters, while the reason
// ("Camera not connected", Alpaca 1031) sat in the debug log. The camera now
// keeps why its last capture failed (CaptureFailures::Failure on GuideCamera),
// and these helpers build the only text clients see from it.

#include <gtest/gtest.h>

#include "capture_failure.h"

#include <string>

using AlpacaErrors::CallError;
using CaptureFailures::Failure;
using CaptureFailures::FramePhase;

namespace
{
CallError Err(CallError::Kind kind, long http, long alpaca, const std::string& message, long curlCode = 0)
{
    CallError e;
    e.kind = kind;
    e.httpStatus = http;
    e.alpacaError = alpaca;
    e.message = message;
    e.curlCode = curlCode;
    return e;
}

Failure FromCall(const CallError& e)
{
    Failure f;
    CaptureFailures::RecordCallError(f, e);
    return f;
}

Failure FromAlert(const std::string& alert)
{
    Failure f;
    CaptureFailures::NoteDriverAlert(f, alert);
    return f;
}
} // namespace

TEST(CaptureFailure, FallbackTextIsUnchanged)
{
    // clients may match it; it is today's text
    EXPECT_STREQ(CaptureFailures::kFallback, "camera error -- see debug log");
}

TEST(CaptureFailure, NothingGivesExactFallback)
{
    Failure f;
    EXPECT_TRUE(f.Empty());
    EXPECT_EQ(CaptureFailures::Reason(f), "");
    EXPECT_EQ(CaptureFailures::Text(f), "camera error -- see debug log");
}

TEST(CaptureFailure, AlpacaNumberAndMessage)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected"));
    EXPECT_EQ(CaptureFailures::Reason(f), "Camera not connected (Alpaca 1031)");
    EXPECT_EQ(CaptureFailures::Text(f), "camera error: Camera not connected (Alpaca 1031)");
}

TEST(CaptureFailure, NumberWithoutMessage)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, ""));
    EXPECT_EQ(CaptureFailures::Text(f), "camera error: Alpaca error 1280");
}

TEST(CaptureFailure, HttpAndTransport)
{
    EXPECT_EQ(CaptureFailures::Text(FromCall(Err(CallError::Kind::Http, 400, 0, "Invalid Duration"))),
              "camera error: HTTP 400: Invalid Duration");
    EXPECT_EQ(CaptureFailures::Text(FromCall(Err(CallError::Kind::Transport, 0, 0, "Couldn't connect to server"))),
              "camera error: no response from the Alpaca server (Couldn't connect to server)");
}

TEST(CaptureFailure, NoneCallRecordsNothing)
{
    Failure f = FromCall(CallError());
    EXPECT_TRUE(f.Empty());
    EXPECT_EQ(CaptureFailures::Text(f), CaptureFailures::kFallback);
}

TEST(CaptureFailure, LatestCallWins)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1025, "first"));
    CaptureFailures::RecordCallError(f, Err(CallError::Kind::Alpaca, 200, 1031, "second"));
    EXPECT_EQ(CaptureFailures::Reason(f), "second (Alpaca 1031)");
}

TEST(CaptureFailure, DriverAlertOnly)
{
    // the timeout (story 5) and other driver alerts with no failed Alpaca call
    EXPECT_EQ(CaptureFailures::Text(FromAlert("Error reading image")), "camera error: Error reading image");
}

TEST(CaptureFailure, AlpacaWinsOverDriverAlert)
{
    // a 1031 reconnect: CaptureFailed records the call, then DisconnectWithAlert notes its alert
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected"));
    CaptureFailures::NoteDriverAlert(f, "The Alpaca server reports the camera as not connected: Camera not connected");
    EXPECT_EQ(CaptureFailures::Reason(f), "Camera not connected (Alpaca 1031)");
}

TEST(CaptureFailure, FirstDriverAlertKept)
{
    Failure f = FromAlert("first alert");
    CaptureFailures::NoteDriverAlert(f, "second alert");
    EXPECT_EQ(CaptureFailures::Reason(f), "first alert");
}

TEST(CaptureFailure, BlankDriverAlertIgnored)
{
    Failure f = FromAlert(" \n ");
    EXPECT_EQ(CaptureFailures::Text(f), CaptureFailures::kFallback);
    CaptureFailures::NoteDriverAlert(f, "real alert");
    EXPECT_EQ(CaptureFailures::Reason(f), "real alert");
}

TEST(CaptureFailure, MultiLineDriverAlertCollapsed)
{
    Failure f = FromAlert("line1\r\n  line2\n\tline3");
    std::string text = CaptureFailures::Text(f);
    EXPECT_EQ(text, "camera error: line1 line2 line3");
    EXPECT_EQ(text.find('\n'), std::string::npos);
    EXPECT_EQ(text.find('\r'), std::string::npos);
}

TEST(CaptureFailure, LongDriverAlertCapped)
{
    std::string timeout = "After 32.0 sec the camera has not completed a 2.0 sec exposure, so it has been disconnected to "
                          "prevent other problems. Refer to Trouble-shooting section of Help.";
    std::string reason = CaptureFailures::Reason(FromAlert(timeout + " " + timeout));
    EXPECT_LE(reason.size(), AlpacaErrors::kReasonMaxBytes);
    EXPECT_EQ(reason.substr(0, 20), timeout.substr(0, 20));
    EXPECT_EQ(reason.substr(reason.size() - 3), "...");
}

TEST(CaptureFailure, InvalidUtf8NeverGivesAnEmptyText)
{
    // wxString::FromUTF8 of invalid UTF-8 is empty; the text must stay valid UTF-8
    std::string text = CaptureFailures::Text(FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "\xE9\xE8")));
    EXPECT_EQ(text, "camera error: \xEF\xBF\xBD\xEF\xBF\xBD (Alpaca 1280)");
    EXPECT_EQ(CaptureFailures::Text(FromAlert("\xFF")), "camera error: \xEF\xBF\xBD");
}

TEST(CaptureFailure, ClearForgetsEverything)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected"));
    CaptureFailures::NoteDriverAlert(f, "alert");
    f = Failure();
    EXPECT_TRUE(f.Empty());
    EXPECT_EQ(CaptureFailures::Text(f), CaptureFailures::kFallback);
}

TEST(AlertWithReason, AppendsReason)
{
    EXPECT_EQ(CaptureFailures::AlertWithReason("Alpaca error -- Cannot start exposure", "Duration out of range (Alpaca 1025)"),
              "Alpaca error -- Cannot start exposure: Duration out of range (Alpaca 1025)");
}

TEST(AlertWithReason, DropsTrailingPeriodAndSpace)
{
    EXPECT_EQ(CaptureFailures::AlertWithReason("The camera failed. ", "HTTP 500"), "The camera failed: HTTP 500");
}

TEST(AlertWithReason, EmptyReasonKeepsAlert)
{
    EXPECT_EQ(CaptureFailures::AlertWithReason("Error reading image.", ""), "Error reading image.");
}

// Retrying a calibration frame after a transient failure (open-astro/openastro-guider#116).
//
// One failed exposure (Alpaca 1280 after the driver's SDK retries, on RC51) ended a calibration
// that had been going well for minutes: every frame error was terminal. A transient failure is
// now exposed again, up to kFrameRetries times per step, while disconnects and other
// errors fail the calibration at once as before.

TEST(CalibrationFrameRetry, Alpaca1280IsTransient)
{
    EXPECT_TRUE(CaptureFailures::IsTransient(FromCall(
        Err(CallError::Kind::Alpaca, 200, 1280, "ZWO image acquisition failed: camera exposure failed after SDK retries"))));
    EXPECT_TRUE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Alpaca, 200, 1280, ""))));
    // the image payload's own 1280, with a driver alert noted too
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    CaptureFailures::NoteDriverAlert(f, "Error reading image");
    EXPECT_TRUE(CaptureFailures::IsTransient(f));
}

TEST(CalibrationFrameRetry, TimeoutIsTransient)
{
    EXPECT_STREQ(CaptureFailures::kCurlTimeoutText, "Timeout was reached");
    EXPECT_TRUE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Transport, 0, 0, "Timeout was reached"))));
}

TEST(CalibrationFrameRetry, TimeoutIsTransientByCurlCode)
{
    // #227: a timed-out call is known by its CURLcode (CURLE_OPERATION_TIMEDOUT, 28), not by curl's
    // text, which curl has reworded before (#190: "Couldn't connect" became "Could not connect")
    EXPECT_TRUE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Transport, 0, 0, "Operation timed out", 28))));
    EXPECT_TRUE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Transport, 0, 0, "", 28))));
}

TEST(CalibrationFrameRetry, OtherFailuresAreNotTransient)
{
    // a disconnect reconnects the camera (#103) instead
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected"))));
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Alpaca, 200, 1024, "Not implemented"))));
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Alpaca, 200, 1025, "Duration out of range"))));
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Http, 400, 0, "Bad request"))));
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Http, 500, 0, "Internal error"))));
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Parse, 200, 0, "unexpected token"))));
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Transport, 0, 0, "Couldn't connect to server"))));
    // a 1280 number carried by an HTTP status is not an Alpaca error
    EXPECT_FALSE(CaptureFailures::IsTransient(FromCall(Err(CallError::Kind::Http, 1280, 0, ""))));
    // a driver alert alone: the capture watchdog's timeout, which reconnects the camera itself
    EXPECT_FALSE(CaptureFailures::IsTransient(
        FromAlert("After 17.0 sec the camera has not completed a 2.0 sec exposure, so it has been disconnected")));
    // an aborted or interrupted exposure records nothing
    EXPECT_FALSE(CaptureFailures::IsTransient(Failure()));
}

TEST(CalibrationFrameRetry, RetriesUpToTheBudget)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    EXPECT_EQ(CaptureFailures::kFrameRetries, 2);
    EXPECT_TRUE(CaptureFailures::ShouldRetryFrame(f, 0, FramePhase::Calibrating, true, true));
    EXPECT_TRUE(CaptureFailures::ShouldRetryFrame(f, 1, FramePhase::Calibrating, true, true));
    EXPECT_FALSE(CaptureFailures::ShouldRetryFrame(f, 2, FramePhase::Calibrating, true, true));
    EXPECT_FALSE(CaptureFailures::ShouldRetryFrame(f, 3, FramePhase::Calibrating, true, true));

    Failure timeout = FromCall(Err(CallError::Kind::Transport, 0, 0, "Timeout was reached"));
    EXPECT_TRUE(CaptureFailures::ShouldRetryFrame(timeout, 0, FramePhase::Calibrating, true, true));
}

TEST(CalibrationFrameRetry, TheReconnectStaysReachable)
{
    // the first frame plus every retry fails with 1280 before the #103 tracker reconnects
    EXPECT_EQ(1 + CaptureFailures::kFrameRetries, AlpacaErrors::kAcquisitionFailuresBeforeReconnect);

    AlpacaErrors::CaptureFailureTracker tracker;
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    int retries = 0;
    // frame and retries: each 1280 keeps the alert and is exposed again
    while (CaptureFailures::ShouldRetryFrame(f, retries, FramePhase::Calibrating, true, true))
    {
        EXPECT_EQ(tracker.OnFailure(1280), AlpacaErrors::CaptureFailureAction::Alert);
        ++retries;
    }
    EXPECT_EQ(retries, CaptureFailures::kFrameRetries);
    // the next 1280 in a row is the one that reconnects
    EXPECT_EQ(tracker.OnFailure(1280), AlpacaErrors::CaptureFailureAction::Reconnect);
}

TEST(CalibrationFrameRetry, NoRetryOutsideACalibrationOrWhenStopping)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    // looping, a star selected or STATE_CALIBRATED: unchanged, the frame fails
    EXPECT_FALSE(CaptureFailures::ShouldRetryFrame(f, 0, FramePhase::Other, true, true));
    // a stop was requested
    EXPECT_FALSE(CaptureFailures::ShouldRetryFrame(f, 0, FramePhase::Calibrating, false, true));
    // the camera is disconnected (a failed reconnect, the throttle)
    EXPECT_FALSE(CaptureFailures::ShouldRetryFrame(f, 0, FramePhase::Calibrating, true, false));
    // a disconnect while calibrating
    EXPECT_FALSE(CaptureFailures::ShouldRetryFrame(FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected")), 0,
                                                   FramePhase::Calibrating, true, true));
    EXPECT_FALSE(CaptureFailures::ShouldRetryFrame(Failure(), 0, FramePhase::Calibrating, true, true));
}

TEST(CalibrationFrameRetry, BudgetCountsAndResets)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    CaptureFailures::FrameRetries budget;
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    EXPECT_EQ(budget.Count(), 1);
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    EXPECT_EQ(budget.Count(), 2);
    EXPECT_FALSE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true)); // the calibration fails, and a new count starts
    EXPECT_EQ(budget.Count(), 0);

    // a good frame ends the run: the next step gets the full budget
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    budget.OnGoodFrame();
    EXPECT_EQ(budget.Count(), 0);
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));

    // a failure that is not retried starts a new count too
    CaptureFailures::FrameRetries other;
    EXPECT_TRUE(other.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    EXPECT_FALSE(other.OnFailedFrame(f, FramePhase::Other, true, true)); // no longer calibrating
    EXPECT_EQ(other.Count(), 0);
}

TEST(CalibrationFrameRetry, AReconnectRefillsOncePerRunOfFailures)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    CaptureFailures::FrameRetries budget;

    // a reconnect with no retry used (a 1031) changes nothing and does not use up the refill
    budget.OnReconnect();
    EXPECT_EQ(budget.Count(), 0);

    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    budget.OnReconnect(); // the 3rd 1280 reconnected the camera
    EXPECT_EQ(budget.Count(), 0);
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    budget.OnReconnect(); // a second reconnect in the same run gives no new count
    EXPECT_EQ(budget.Count(), 2);
    EXPECT_FALSE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));

    // a good frame makes the refill available again
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    budget.OnGoodFrame();
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Calibrating, true, true));
    budget.OnReconnect();
    EXPECT_EQ(budget.Count(), 0);
}

TEST(CalibrationFrameRetry, ACameraThatFailsEveryFrameEndsTheCalibration)
{
    // Every frame fails with 1280 and every reconnect succeeds, slowly enough that the reconnect throttle
    // never stops it: MyFrame's sequence of OnExposeComplete, the #103 tracker and DoTryReconnect. The
    // calibration must still fail, after a bounded number of frames.
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    AlpacaErrors::CaptureFailureTracker tracker;
    CaptureFailures::FrameRetries budget;
    int frames = 0, reconnects = 0;
    bool failed = false;
    while (!failed && frames < 100)
    {
        ++frames;
        if (tracker.OnFailure(1280) == AlpacaErrors::CaptureFailureAction::Reconnect)
        {
            // the worker skips the expose-complete; the reconnect succeeds and resumes the exposure
            ++reconnects;
            tracker.Reset();
            budget.OnReconnect();
            continue;
        }
        failed = !budget.OnFailedFrame(f, FramePhase::Calibrating, true, true);
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(reconnects, 2);
    EXPECT_EQ(frames, 7);
}

// Dropping a guiding frame after a transient failure (open-astro/openastro-guider#136).
//
// While guiding or settling, one failed exposure (Alpaca 1280, a timeout) stopped guiding and failed
// the settle; RC51's ASI120 fails 10-30% of its exposures, so guiding stopped every few minutes. A
// transient failure while guiding is now dropped like a lost-star frame, with the same budget as a
// calibration step (#116); looping, a star selected and STATE_CALIBRATED still stop.

TEST(FrameRetry, PhaseOfEachGuiderState)
{
    struct Case
    {
        GUIDER_STATE state;
        FramePhase phase;
    };
    const Case cases[] = {
        { STATE_UNINITIALIZED, FramePhase::Other }, // looping, no star
        { STATE_SELECTING, FramePhase::Other }, // a guide request still selecting its star
        { STATE_SELECTED, FramePhase::Other },
        { STATE_CALIBRATING_PRIMARY, FramePhase::Calibrating },
        { STATE_CALIBRATING_SECONDARY, FramePhase::Calibrating },
        { STATE_CALIBRATED, FramePhase::Other }, // no step to repeat, not guiding yet
        { STATE_GUIDING, FramePhase::Guiding }, // guiding, settling or paused
        { STATE_STOP, FramePhase::Other },
    };
    for (const Case& c : cases)
        EXPECT_EQ(CaptureFailures::PhaseFor(c.state), c.phase) << "state " << c.state;
}

TEST(FrameRetry, DecisionTable)
{
    struct Case
    {
        const char *name;
        Failure failure;
        bool transient;
    };
    const Case failures[] = {
        { "1280", FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "ZWO image acquisition failed")), true },
        { "1280 no message", FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "")), true },
        { "curl timeout", FromCall(Err(CallError::Kind::Transport, 0, 0, "Timeout was reached")), true },
        { "1031", FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected")), false },
        { "1024", FromCall(Err(CallError::Kind::Alpaca, 200, 1024, "Not implemented")), false },
        { "1025", FromCall(Err(CallError::Kind::Alpaca, 200, 1025, "Duration out of range")), false },
        { "HTTP 500", FromCall(Err(CallError::Kind::Http, 500, 0, "Internal error")), false },
        { "transport", FromCall(Err(CallError::Kind::Transport, 0, 0, "Couldn't connect to server")), false },
        { "driver alert", FromAlert("After 17.0 sec the camera has not completed a 2.0 sec exposure"), false },
        { "empty", Failure(), false },
    };
    const FramePhase phases[] = { FramePhase::Other, FramePhase::Calibrating, FramePhase::Guiding };

    for (const Case& c : failures)
        for (FramePhase phase : phases)
            for (int retries = 0; retries <= 2; ++retries)
                for (int flags = 0; flags < 4; ++flags) // every continuing x connected pair
                {
                    const bool continuing = (flags & 1) != 0;
                    const bool connected = (flags & 2) != 0;
                    bool want = phase != FramePhase::Other && c.transient && retries < CaptureFailures::kFrameRetries &&
                        continuing && connected;
                    EXPECT_EQ(CaptureFailures::ShouldRetryFrame(c.failure, retries, phase, continuing, connected), want)
                        << c.name << " phase " << static_cast<int>(phase) << " retries " << retries << " continuing "
                        << continuing << " connected " << connected;
                }
}

TEST(FrameRetry, GuidingBudgetCountsAndResets)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    CaptureFailures::FrameRetries budget;
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    EXPECT_EQ(budget.Count(), 1);
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    EXPECT_EQ(budget.Count(), 2);
    EXPECT_FALSE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true)); // guiding stops, and a new count starts
    EXPECT_EQ(budget.Count(), 0);

    // a good frame ends the run of failures: the next two are dropped again
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    budget.OnGoodFrame();
    EXPECT_EQ(budget.Count(), 0);
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));

    // a failure that is not dropped (1031) starts a new count too
    CaptureFailures::FrameRetries other;
    EXPECT_TRUE(other.OnFailedFrame(f, FramePhase::Guiding, true, true));
    EXPECT_FALSE(other.OnFailedFrame(FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected")),
                                     FramePhase::Guiding, true, true));
    EXPECT_EQ(other.Count(), 0);
    EXPECT_TRUE(other.OnFailedFrame(f, FramePhase::Guiding, true, true));
}

TEST(FrameRetry, TheReconnectStaysReachableWhileGuiding)
{
    EXPECT_EQ(1 + CaptureFailures::kFrameRetries, AlpacaErrors::kAcquisitionFailuresBeforeReconnect);

    AlpacaErrors::CaptureFailureTracker tracker;
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    int retries = 0;
    // each dropped guiding frame's 1280 only raises the alert
    while (CaptureFailures::ShouldRetryFrame(f, retries, FramePhase::Guiding, true, true))
    {
        EXPECT_EQ(tracker.OnFailure(1280), AlpacaErrors::CaptureFailureAction::Alert);
        ++retries;
    }
    EXPECT_EQ(retries, CaptureFailures::kFrameRetries);
    // the next 1280 in a row reconnects the camera before guiding would stop
    EXPECT_EQ(tracker.OnFailure(1280), AlpacaErrors::CaptureFailureAction::Reconnect);
}

TEST(FrameRetry, GuidingReconnectRefillsOncePerRun)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    CaptureFailures::FrameRetries budget;

    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    budget.OnReconnect(); // the 3rd 1280 reconnected the camera
    EXPECT_EQ(budget.Count(), 0);
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    budget.OnReconnect(); // a second reconnect in the same run gives no new count
    EXPECT_EQ(budget.Count(), 2);
    EXPECT_FALSE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));

    // a good frame makes the refill available again
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    budget.OnGoodFrame();
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, FramePhase::Guiding, true, true));
    budget.OnReconnect();
    EXPECT_EQ(budget.Count(), 0);
}

TEST(FrameRetry, ACameraThatFailsEveryFrameStopsGuiding)
{
    // As CalibrationFrameRetry.ACameraThatFailsEveryFrameEndsTheCalibration, while guiding: 4 frames are
    // dropped, 2 reconnect the camera, and the 7th stops guiding.
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    AlpacaErrors::CaptureFailureTracker tracker;
    CaptureFailures::FrameRetries budget;
    int frames = 0, reconnects = 0, dropped = 0;
    bool stopped = false;
    while (!stopped && frames < 100)
    {
        ++frames;
        if (tracker.OnFailure(1280) == AlpacaErrors::CaptureFailureAction::Reconnect)
        {
            ++reconnects;
            tracker.Reset();
            budget.OnReconnect();
            continue;
        }
        if (budget.OnFailedFrame(f, FramePhase::Guiding, true, true))
            ++dropped;
        else
            stopped = true;
    }
    EXPECT_TRUE(stopped);
    EXPECT_EQ(reconnects, 2);
    EXPECT_EQ(dropped, 4);
    EXPECT_EQ(frames, 7);
}
