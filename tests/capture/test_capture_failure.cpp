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
