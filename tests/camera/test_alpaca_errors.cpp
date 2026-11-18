// Alpaca capture error tests (open-astro/openastro-guider#103).
//
// When the guide camera drops off USB, AlpacaBridge either sets connected=false
// and answers every capture call with error 1031 (NotConnectedException), or,
// on a soft drop, keeps connected=true and fails each image acquisition with
// 1280 once the driver's own SDK retries have run out. CameraAlpaca::Capture
// feeds every capture call's Alpaca error (binx/biny, startexposure, imageready,
// an ImageBytes error payload, a JSON imagearray error) to a
// CaptureFailureTracker: 1031 disconnects the camera with RECONNECT at once
// (EquipmentDisconnected with reconnecting:true, then the auto-reconnect), and
// so does the third 1280 in a row, counted across capture attempts. Any capture
// error still fails the frame, which stops looping and guiding, so a run of
// 1280s only reaches 3 when the client restarts after each failure. An isolated
// 1280 and any other error keep the alert.

#include <gtest/gtest.h>

#include "alpaca_errors.h"
#include "json_parser.h"

#include <limits>
#include <string>

using AlpacaErrors::CaptureFailureAction;
using AlpacaErrors::CaptureFailureTracker;

TEST(AlpacaErrors, NotConnectedNeedsReconnect)
{
    EXPECT_EQ(AlpacaErrors::kNotConnected, 1031);
    EXPECT_TRUE(AlpacaErrors::CaptureNeedsReconnect(1031));
}

TEST(AlpacaErrors, OtherErrorsDoNotReconnectAtOnce)
{
    EXPECT_FALSE(AlpacaErrors::CaptureNeedsReconnect(0)); // no Alpaca error (curl failure)
    EXPECT_FALSE(AlpacaErrors::CaptureNeedsReconnect(1024)); // NotImplemented
    EXPECT_FALSE(AlpacaErrors::CaptureNeedsReconnect(1025)); // InvalidValue
    EXPECT_FALSE(AlpacaErrors::CaptureNeedsReconnect(1035)); // InvalidOperation
    EXPECT_FALSE(AlpacaErrors::CaptureNeedsReconnect(1280)); // driver error: only repeated ones reconnect
    EXPECT_FALSE(AlpacaErrors::CaptureNeedsReconnect(400)); // HTTP status
    EXPECT_FALSE(AlpacaErrors::CaptureNeedsReconnect(500)); // HTTP status
}

TEST(AlpacaErrors, ThresholdIsThreeAcquisitionFailures)
{
    EXPECT_EQ(AlpacaErrors::kUnspecifiedError, 1280);
    EXPECT_EQ(AlpacaErrors::kAcquisitionFailuresBeforeReconnect, 3);
}

TEST(CaptureFailureTracker, NotConnectedReconnectsOnFirstError)
{
    CaptureFailureTracker t;
    EXPECT_EQ(t.OnFailure(1031), CaptureFailureAction::Reconnect);
    EXPECT_EQ(t.Consecutive(), 0);
    EXPECT_EQ(t.OnFailure(1031), CaptureFailureAction::Reconnect);
}

TEST(CaptureFailureTracker, IsolatedAcquisitionFailureOnlyAlerts)
{
    CaptureFailureTracker t;
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
    EXPECT_EQ(t.Consecutive(), 1);
}

TEST(CaptureFailureTracker, ThirdConsecutiveAcquisitionFailureReconnects)
{
    CaptureFailureTracker t;
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
    EXPECT_EQ(t.Consecutive(), 2);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Reconnect);
    EXPECT_EQ(t.Consecutive(), 0);
}

TEST(CaptureFailureTracker, GoodFrameEndsTheRun)
{
    CaptureFailureTracker t;
    t.OnFailure(1280);
    t.OnFailure(1280);
    t.OnSuccess();
    EXPECT_EQ(t.Consecutive(), 0);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Reconnect);
}

TEST(CaptureFailureTracker, ConnectOrDisconnectEndsTheRun)
{
    CaptureFailureTracker t;
    t.OnFailure(1280);
    t.OnFailure(1280);
    t.Reset();
    EXPECT_EQ(t.Consecutive(), 0);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
}

TEST(CaptureFailureTracker, ReconnectStartsANewRun)
{
    // After a reconnect it takes 3 more failures before the next one, so a long outage
    // does not spend one of the 3-per-60 s reconnect attempts on every failed frame.
    CaptureFailureTracker t;
    for (int i = 0; i < 2; i++)
        t.OnFailure(1280);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Reconnect);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
    EXPECT_EQ(t.Consecutive(), 1);
}

TEST(CaptureFailureTracker, OtherErrorsNeitherCountNorReset)
{
    CaptureFailureTracker t;
    t.OnFailure(1280);
    t.OnFailure(1280);
    EXPECT_EQ(t.OnFailure(0), CaptureFailureAction::Alert); // curl failure
    EXPECT_EQ(t.OnFailure(500), CaptureFailureAction::Alert); // HTTP status
    EXPECT_EQ(t.OnFailure(1024), CaptureFailureAction::Alert); // NotImplemented
    EXPECT_EQ(t.OnFailure(1025), CaptureFailureAction::Alert); // InvalidValue
    EXPECT_EQ(t.OnFailure(1035), CaptureFailureAction::Alert); // InvalidOperation
    EXPECT_EQ(t.Consecutive(), 2);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Reconnect);
}

TEST(CaptureFailureTracker, OtherErrorsAloneNeverReconnect)
{
    CaptureFailureTracker t;
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(t.OnFailure(1035), CaptureFailureAction::Alert);
    EXPECT_EQ(t.Consecutive(), 0);
}

TEST(CaptureFailureTracker, NotConnectedAlsoEndsAnAcquisitionRun)
{
    CaptureFailureTracker t;
    t.OnFailure(1280);
    t.OnFailure(1280);
    EXPECT_EQ(t.OnFailure(1031), CaptureFailureAction::Reconnect);
    EXPECT_EQ(t.Consecutive(), 0);
    EXPECT_EQ(t.OnFailure(1280), CaptureFailureAction::Alert);
}

// ReadError: the JSON imagearray path reads ErrorNumber/ErrorMessage before Value (a
// server sends an imagearray error as JSON with HTTP 200), and AlpacaClient's
// Get/Put/PutAction read Alpaca errors with it too.

namespace
{
struct Parsed
{
    bool isError;
    long number;
    std::string message;
};

Parsed Read(const std::string& body)
{
    JsonParser parser;
    EXPECT_TRUE(parser.Parse(body)) << body;
    Parsed p { false, -1, "unset" };
    p.isError = AlpacaErrors::ReadError(parser.Root(), &p.number, &p.message);
    return p;
}
}

TEST(ReadError, NotConnectedImageArray)
{
    Parsed p = Read(R"({"ClientTransactionID":1,"ServerTransactionID":2,"ErrorNumber":1031,)"
                    R"("ErrorMessage":"camera removed from USB"})");
    EXPECT_TRUE(p.isError);
    EXPECT_EQ(p.number, 1031);
    EXPECT_EQ(p.message, "camera removed from USB");
}

TEST(ReadError, AcquisitionFailureWithEmptyValue)
{
    Parsed p = Read(R"({"Value":[],"ErrorNumber":1280,)"
                    R"("ErrorMessage":"ZWO image acquisition failed: camera exposure failed after SDK retries"})");
    EXPECT_TRUE(p.isError);
    EXPECT_EQ(p.number, 1280);
    EXPECT_EQ(p.message, "ZWO image acquisition failed: camera exposure failed after SDK retries");
}

TEST(ReadError, FloatErrorNumberIsCoerced)
{
    Parsed p = Read(R"({"ErrorNumber":1031.0,"ErrorMessage":"x"})");
    EXPECT_TRUE(p.isError);
    EXPECT_EQ(p.number, 1031);
}

TEST(ReadError, GoodFrameIsNotAnError)
{
    Parsed p = Read(R"({"Value":[[1,2],[3,4]],"ErrorNumber":0,"ErrorMessage":""})");
    EXPECT_FALSE(p.isError);
    EXPECT_EQ(p.number, 0);
    EXPECT_EQ(p.message, "");
}

TEST(ReadError, MissingOrMistypedFieldsReadAsNoError)
{
    Parsed p = Read(R"({"Value":[[1]]})");
    EXPECT_FALSE(p.isError);
    EXPECT_EQ(p.number, 0);
    EXPECT_EQ(p.message, "");

    p = Read(R"({"ErrorNumber":"1031","ErrorMessage":5})");
    EXPECT_FALSE(p.isError);
    EXPECT_EQ(p.number, 0);
    EXPECT_EQ(p.message, "");
}

TEST(ReadError, MessageWithoutNumberIsNotAnError)
{
    Parsed p = Read(R"({"ErrorNumber":0,"ErrorMessage":"warning only"})");
    EXPECT_FALSE(p.isError);
    EXPECT_EQ(p.message, "warning only");
}

TEST(ReadError, NonObjectRootIsNotAnError)
{
    long number = -1;
    std::string message = "unset";
    EXPECT_FALSE(AlpacaErrors::ReadError(nullptr, &number, &message));
    EXPECT_EQ(number, -1); // left untouched
    Parsed p = Read("[1,2,3]");
    EXPECT_FALSE(p.isError);
    EXPECT_EQ(p.number, -1); // left untouched
}

// ---------------------------------------------------------------------------
// CallError: what a failed AlpacaClient call reports, and the one-line reason
// built from it (#97, also used by #104). The reason goes into client-visible
// strings (SingleFrameComplete.Error, SettleDone.Error, the dark-build error,
// the Alert), so it must be one line of valid UTF-8 within the cap.
// ---------------------------------------------------------------------------

using AlpacaErrors::CallError;
using AlpacaErrors::CallErrorReason;
using AlpacaErrors::OneLine;

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

bool IsValidUtf8(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size())
            return false;
        for (std::size_t k = 1; k < len; k++)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}
} // namespace

TEST(CallErrorReason, NoneIsEmpty)
{
    EXPECT_EQ(CallErrorReason(CallError()), "");
}

TEST(CallErrorReason, AlpacaNumberAndMessage)
{
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1031, "Camera not connected")),
              "Camera not connected (Alpaca 1031)");
}

TEST(CallErrorReason, AlpacaNumberWithoutMessage)
{
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1280, "")), "Alpaca error 1280");
    // a message that is empty once cleaned counts as none
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1025, "  \n\t ")), "Alpaca error 1025");
}

TEST(CallErrorReason, AlpacaMessageTrimmedAndOneLine)
{
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1031, "  Camera not connected \n")),
              "Camera not connected (Alpaca 1031)");
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1280, "line1\r\n  line2\n\tline3")),
              "line1 line2 line3 (Alpaca 1280)");
}

TEST(CallErrorReason, LongAlpacaMessageCappedKeepsSuffix)
{
    std::string r = CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1280, std::string(1000, 'x')));
    EXPECT_LE(r.size(), AlpacaErrors::kReasonMaxBytes);
    EXPECT_EQ(r.substr(r.size() - std::string("... (Alpaca 1280)").size()), "... (Alpaca 1280)");
}

// #110: the message is cut once, to kReasonMaxBytes less the suffix. " (Alpaca 1280)" is 14 bytes, so a cleaned
// message of up to 186 bytes is kept whole and one of 187 bytes or more (200 or more included) is cut to 183 + "...".
TEST(CallErrorReason, AlpacaMessageCutAtTheCapLessTheSuffix)
{
    const std::string suffix = " (Alpaca 1280)";
    ASSERT_EQ(suffix.size(), 14u);
    auto reason = [](const std::string& message) { return CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1280, message)); };
    EXPECT_EQ(reason(std::string(186, 'a')), std::string(186, 'a') + suffix);
    const std::string cut = std::string(183, 'a') + "..." + suffix;
    for (std::size_t n : { 187u, 199u, 200u, 201u, 1000u })
    {
        std::string r = reason(std::string(n, 'a'));
        EXPECT_EQ(r, cut) << n;
        EXPECT_EQ(r.size(), AlpacaErrors::kReasonMaxBytes) << n;
    }
    // the cap applies to the cleaned text: a raw message well over 200 bytes that cleans to 186 is kept whole
    EXPECT_EQ(reason("  x" + std::string(400, ' ') + std::string(184, 'y') + " \r\n "), "x " + std::string(184, 'y') + suffix);
    // a space left before the "..." is dropped
    EXPECT_EQ(reason(std::string(182, 'a') + " " + std::string(10, 'b')), std::string(182, 'a') + "..." + suffix);
    // a UTF-8 sequence is not split at the cut: 94 two-byte characters (188 bytes) keep 91 of them
    std::string accents;
    for (int i = 0; i < 94; i++)
        accents += "\xC3\xA9";
    std::string kept;
    for (int i = 0; i < 91; i++)
        kept += "\xC3\xA9";
    EXPECT_EQ(reason(accents), kept + "..." + suffix);
}

TEST(CallErrorReason, LongestAlpacaSuffixStillFitsTheCap)
{
    CallError e = Err(CallError::Kind::Alpaca, 200, std::numeric_limits<long>::min(), std::string(1000, 'x'));
    std::string suffix = " (Alpaca " + std::to_string(std::numeric_limits<long>::min()) + ")";
    std::string r = CallErrorReason(e);
    EXPECT_EQ(r, std::string(AlpacaErrors::kReasonMaxBytes - suffix.size() - 3, 'x') + "..." + suffix);
    EXPECT_EQ(r.size(), AlpacaErrors::kReasonMaxBytes);
}

TEST(CallErrorReason, HttpStatusWithShortBody)
{
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 400, 0, "Invalid Duration\r\n")), "HTTP 400: Invalid Duration");
}

TEST(CallErrorReason, HttpStatusWithoutBody)
{
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 500, 0, "")), "HTTP 500");
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 500, 0, " \n ")), "HTTP 500");
}

TEST(CallErrorReason, HttpHtmlOrLongBodyLeftOut)
{
    // a proxy's error page is never shipped to clients
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 502, 0, "<html><body>Bad Gateway</body></html>")), "HTTP 502");
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 503, 0, std::string(121, 'a'))), "HTTP 503");
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 503, 0, std::string(120, 'a'))), "HTTP 503: " + std::string(120, 'a'));
}

TEST(CallErrorReason, HttpLongBodyLeftOutEvenWhenItsCapWouldFit)
{
    // a space at byte 117: capping to 121 bytes would drop it before the "..." and come out at 120
    std::string spaced = std::string(117, 'a') + " " + std::string(82, 'b');
    ASSERT_EQ(spaced.size(), 200u);
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 500, 0, spaced)), "HTTP 500");
    // a 3-byte UTF-8 character straddling byte 118: the cap would back off to 117 bytes
    std::string straddle = std::string(117, 'a') + "\xE2\x82\xAC" + std::string(80, 'b');
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 500, 0, straddle)), "HTTP 500");
    // a '<' past where a cap would cut is still seen (and the body is too long anyway)
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 500, 0, std::string(117, 'a') + " <html>")), "HTTP 500");
    // cleaning happens before the length test: whitespace runs that collapse to fit are fine
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Http, 400, 0, "bad" + std::string(200, ' ') + "duration")),
              "HTTP 400: bad duration");
}

TEST(CallErrorReason, HttpStatusIsNeverLabelledAlpaca)
{
    std::string r = CallErrorReason(Err(CallError::Kind::Http, 400, 0, "bad"));
    EXPECT_EQ(r.find("Alpaca"), std::string::npos);
}

TEST(CallErrorReason, Transport)
{
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Transport, 0, 0, "Couldn't connect to server")),
              "no response from the Alpaca server (Couldn't connect to server)");
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Transport, 0, 0, "")), "no response from the Alpaca server");
}

TEST(CallErrorReason, Parse)
{
    EXPECT_EQ(CallErrorReason(Err(CallError::Kind::Parse, 200, 0, "unexpected token at 1:1")),
              "invalid response from the Alpaca server");
}

TEST(CallErrorReason, InvalidUtf8MessageReplacedNotEmptied)
{
    // a Latin-1 message from a Windows ASCOM Remote server: "café" with é = 0xE9
    std::string r = CallErrorReason(Err(CallError::Kind::Alpaca, 200, 1280, "caf\xE9 failed"));
    EXPECT_TRUE(IsValidUtf8(r));
    EXPECT_EQ(r, "caf\xEF\xBF\xBD failed (Alpaca 1280)");
}

TEST(OneLine, CollapsesWhitespaceAndControls)
{
    EXPECT_EQ(OneLine("  a\r\n\r\n b\t\tc  "), "a b c");
    EXPECT_EQ(OneLine(std::string("a\0b", 3)), "a b");
    EXPECT_EQ(OneLine(""), "");
    EXPECT_EQ(OneLine(" \r\n\t "), "");
}

TEST(OneLine, KeepsValidUtf8)
{
    EXPECT_EQ(OneLine("C\xC3\xA2mera n\xC3\xA3o conectada"), "C\xC3\xA2mera n\xC3\xA3o conectada");
    EXPECT_EQ(OneLine("\xE2\x82\xAC \xF0\x9F\x94\xAD"), "\xE2\x82\xAC \xF0\x9F\x94\xAD");
}

TEST(OneLine, ReplacesInvalidUtf8)
{
    EXPECT_EQ(OneLine(std::string("a\xFF") + "b"), std::string("a\xEF\xBF\xBD") + "b");
    EXPECT_EQ(OneLine("a\xC3"), "a\xEF\xBF\xBD"); // truncated sequence
    EXPECT_EQ(OneLine("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD"); // overlong encoding
    EXPECT_EQ(OneLine("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"); // UTF-16 surrogate
    EXPECT_TRUE(IsValidUtf8(OneLine("\x80\x81\x82 \xF5\x80")));
}

TEST(OneLine, CapsWithoutSplittingUtf8)
{
    std::string s;
    for (int i = 0; i < 300; i++)
        s += "\xC3\xA9";
    for (std::size_t cap : { 10u, 11u, 12u, 13u, 200u })
    {
        std::string r = OneLine(s, cap);
        EXPECT_LE(r.size(), cap);
        EXPECT_TRUE(IsValidUtf8(r)) << cap;
        EXPECT_EQ(r.substr(r.size() - 3), "...") << cap;
    }
    std::string ascii(500, 'x');
    std::string r = OneLine(ascii, 200);
    EXPECT_EQ(r.size(), 200u);
    EXPECT_EQ(r, std::string(197, 'x') + "...");
    EXPECT_EQ(OneLine(std::string(200, 'y'), 200), std::string(200, 'y')); // exactly at the cap: untouched
}

// The guard AlpacaClient's Get / GetRaw / Put / PutAction / GetBool use, so no
// failure return can leave *callError unset.
TEST(CallErrorScope, ClearsOnEntryAndFillsGenericFailure)
{
    CallError out = Err(CallError::Kind::Alpaca, 200, 1031, "stale");
    {
        AlpacaErrors::CallErrorScope scope(&out);
        EXPECT_EQ(out.kind, CallError::Kind::None); // stale value never survives the call
    }
    EXPECT_EQ(out.kind, CallError::Kind::Parse);
    EXPECT_EQ(CallErrorReason(out), "invalid response from the Alpaca server");
}

TEST(CallErrorScope, SpecificFailureKept)
{
    CallError out;
    {
        AlpacaErrors::CallErrorScope scope(&out);
        scope.Set(CallError::Kind::Http, 404, 0, "Not Found");
    }
    EXPECT_EQ(out.kind, CallError::Kind::Http);
    EXPECT_EQ(out.httpStatus, 404);
    EXPECT_EQ(out.message, "Not Found");
}

TEST(CallErrorScope, SuccessLeavesNone)
{
    CallError out = Err(CallError::Kind::Transport, 0, 0, "old");
    {
        AlpacaErrors::CallErrorScope scope(&out);
        scope.Set(CallError::Kind::Http, 500, 0, "");
        scope.Succeeded();
    }
    EXPECT_EQ(out.kind, CallError::Kind::None);
    EXPECT_EQ(out.message, "");
}

TEST(CallErrorScope, NestedCallFillsTheOuterScope)
{
    // GetBool passes its scope's target to Get; Get's failure is GetBool's failure
    CallError out;
    {
        AlpacaErrors::CallErrorScope outer(&out);
        {
            AlpacaErrors::CallErrorScope inner(outer.Target());
            inner.Set(CallError::Kind::Alpaca, 200, 1031, "Camera not connected");
        }
    }
    EXPECT_EQ(out.kind, CallError::Kind::Alpaca);
    EXPECT_EQ(out.alpacaError, 1031);
}

TEST(CallErrorScope, NullTargetIsFine)
{
    AlpacaErrors::CallErrorScope scope(nullptr);
    scope.Set(CallError::Kind::Transport, 0, 0, "x");
    EXPECT_NE(scope.Target(), nullptr);
}
