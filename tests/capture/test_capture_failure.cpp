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

namespace
{
CallError Err(CallError::Kind kind, long http, long alpaca, const std::string& message)
{
    CallError e;
    e.kind = kind;
    e.httpStatus = http;
    e.alpacaError = alpaca;
    e.message = message;
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
// now exposed again, up to kCalibrationFrameRetries times per step, while disconnects and other
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
    EXPECT_EQ(CaptureFailures::kCalibrationFrameRetries, 2);
    EXPECT_TRUE(CaptureFailures::ShouldRetryCalibrationFrame(f, 0, true, true, true));
    EXPECT_TRUE(CaptureFailures::ShouldRetryCalibrationFrame(f, 1, true, true, true));
    EXPECT_FALSE(CaptureFailures::ShouldRetryCalibrationFrame(f, 2, true, true, true));
    EXPECT_FALSE(CaptureFailures::ShouldRetryCalibrationFrame(f, 3, true, true, true));

    Failure timeout = FromCall(Err(CallError::Kind::Transport, 0, 0, "Timeout was reached"));
    EXPECT_TRUE(CaptureFailures::ShouldRetryCalibrationFrame(timeout, 0, true, true, true));
}

TEST(CalibrationFrameRetry, TheReconnectStaysReachable)
{
    // the first frame plus every retry fails with 1280 before the #103 tracker reconnects
    EXPECT_EQ(1 + CaptureFailures::kCalibrationFrameRetries, AlpacaErrors::kAcquisitionFailuresBeforeReconnect);

    AlpacaErrors::CaptureFailureTracker tracker;
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    int retries = 0;
    // frame and retries: each 1280 keeps the alert and is exposed again
    while (CaptureFailures::ShouldRetryCalibrationFrame(f, retries, true, true, true))
    {
        EXPECT_EQ(tracker.OnFailure(1280), AlpacaErrors::CaptureFailureAction::Alert);
        ++retries;
    }
    EXPECT_EQ(retries, CaptureFailures::kCalibrationFrameRetries);
    // the next 1280 in a row is the one that reconnects
    EXPECT_EQ(tracker.OnFailure(1280), AlpacaErrors::CaptureFailureAction::Reconnect);
}

TEST(CalibrationFrameRetry, NoRetryOutsideACalibrationOrWhenStopping)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    // looping or guiding: unchanged, the frame fails
    EXPECT_FALSE(CaptureFailures::ShouldRetryCalibrationFrame(f, 0, false, true, true));
    // a stop was requested
    EXPECT_FALSE(CaptureFailures::ShouldRetryCalibrationFrame(f, 0, true, false, true));
    // the camera is disconnected (a failed reconnect, the throttle)
    EXPECT_FALSE(CaptureFailures::ShouldRetryCalibrationFrame(f, 0, true, true, false));
    // a disconnect while calibrating
    EXPECT_FALSE(CaptureFailures::ShouldRetryCalibrationFrame(
        FromCall(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected")), 0, true, true, true));
    EXPECT_FALSE(CaptureFailures::ShouldRetryCalibrationFrame(Failure(), 0, true, true, true));
}

TEST(CalibrationFrameRetry, BudgetCountsAndResets)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    CaptureFailures::CalibrationFrameRetries budget;
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    EXPECT_EQ(budget.Count(), 1);
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    EXPECT_EQ(budget.Count(), 2);
    EXPECT_FALSE(budget.OnFailedFrame(f, true, true, true)); // the calibration fails, and a new count starts
    EXPECT_EQ(budget.Count(), 0);

    // a good frame ends the run: the next step gets the full budget
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    budget.OnGoodFrame();
    EXPECT_EQ(budget.Count(), 0);
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));

    // a failure that is not retried starts a new count too
    CaptureFailures::CalibrationFrameRetries other;
    EXPECT_TRUE(other.OnFailedFrame(f, true, true, true));
    EXPECT_FALSE(other.OnFailedFrame(f, false, true, true)); // no longer calibrating
    EXPECT_EQ(other.Count(), 0);
}

TEST(CalibrationFrameRetry, AReconnectRefillsOncePerRunOfFailures)
{
    Failure f = FromCall(Err(CallError::Kind::Alpaca, 200, 1280, "acquisition failed"));
    CaptureFailures::CalibrationFrameRetries budget;

    // a reconnect with no retry used (a 1031) changes nothing and does not use up the refill
    budget.OnReconnect();
    EXPECT_EQ(budget.Count(), 0);

    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    budget.OnReconnect(); // the 3rd 1280 reconnected the camera
    EXPECT_EQ(budget.Count(), 0);
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    budget.OnReconnect(); // a second reconnect in the same run gives no new count
    EXPECT_EQ(budget.Count(), 2);
    EXPECT_FALSE(budget.OnFailedFrame(f, true, true, true));

    // a good frame makes the refill available again
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    budget.OnGoodFrame();
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
    EXPECT_TRUE(budget.OnFailedFrame(f, true, true, true));
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
    CaptureFailures::CalibrationFrameRetries budget;
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
        failed = !budget.OnFailedFrame(f, true, true, true);
    }
    EXPECT_TRUE(failed);
    EXPECT_EQ(reconnects, 2);
    EXPECT_EQ(frames, 7);
}
