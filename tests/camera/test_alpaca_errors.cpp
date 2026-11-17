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
