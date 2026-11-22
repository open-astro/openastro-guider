/*
 *  alpaca_errors.h
 *  PHD Guiding
 *
 *  Which Alpaca errors from a capture call mean the server has lost the camera,
 *  reading an Alpaca error out of a JSON response, and the one-line reason a
 *  failed AlpacaClient call gives clients (#97).
 *  Kept free of wx so it can be unit tested (tests/camera/test_alpaca_errors.cpp).
 */

#ifndef ALPACA_ERRORS_H_INCLUDED
#define ALPACA_ERRORS_H_INCLUDED

#include <atomic>
#include <cstddef>
#include <string>

struct json_value;

namespace AlpacaErrors
{
// ASCOM NotConnectedException (0x407)
const long kNotConnected = 0x407;
// ASCOM driver error (0x500). AlpacaBridge reports a failed image acquisition with it,
// for example after a soft USB drop once the driver's own SDK retries have run out.
const long kUnspecifiedError = 0x500;
// Consecutive 1280 capture failures that make the camera reconnect (#103). One 1280
// already spans the driver's SDK retries, and an isolated one is a transient the camera
// recovers from, so it keeps the alert.
const int kAcquisitionFailuresBeforeReconnect = 3;

// True when a capture call failed because the server reports the camera as not
// connected: the camera must be disconnected and reconnected (issue #103).
bool CaptureNeedsReconnect(long errorNumber);

// CURLE_COULDNT_CONNECT, a CallError's curlCode when the server refused the connection:
// nothing is listening on the Alpaca port (#190). alpaca_client.cpp checks it against curl.h.
const long kCurlCouldntConnect = 7;
// CURLE_OPERATION_TIMEDOUT, a CallError's curlCode when the call timed out with no response: a capture
// call that a new exposure can clear (CaptureFailures::IsTransient, #227). alpaca_client.cpp checks it
// against curl.h.
const long kCurlOperationTimedOut = 28;

enum class CaptureFailureAction
{
    Alert, // keep the alert and fail the frame
    Reconnect, // disconnect with RECONNECT: EquipmentDisconnected{reconnecting:true}, then the auto-reconnect
};

struct CallError;

// Counts consecutive 1280 capture failures across capture attempts. 1031 reconnects at
// once; the kAcquisitionFailuresBeforeReconnect-th 1280 in a row reconnects. Either one
// starts a new run. A good frame, Connect and Disconnect reset the count (OnSuccess /
// Reset); any other error neither counts nor resets. Capture runs on the worker thread or
// the dark-build capture thread while Disconnect can run on the main thread, so the count
// is atomic.
class CaptureFailureTracker
{
public:
    CaptureFailureAction OnFailure(long errorNumber);
    // The failed capture call's CallError: an Alpaca error goes by its number. A refused connection
    // (Transport with curlCode kCurlCouldntConnect: the Alpaca server is gone) reconnects at once and
    // starts a new run, like 1031 (#190); any other HTTP, transport or parse failure carries no number
    // and counts as 0.
    CaptureFailureAction OnFailure(const CallError& error);
    void OnSuccess() { m_consecutive.store(0); }
    void Reset() { m_consecutive.store(0); }
    int Consecutive() const { return m_consecutive.load(); }

private:
    std::atomic<int> m_consecutive { 0 };
};

// Reads ErrorNumber (an integer, or a float the server sent for one) and ErrorMessage
// from an Alpaca JSON response object. Returns true when ErrorNumber is non-zero. Fields
// that are missing or of another type read as 0 / "".
bool ReadError(const json_value *root, long *errorNumber, std::string *errorMessage);

// What a failed AlpacaClient call (Get, GetRaw, Put, PutAction, GetBool) reports through its optional
// callError out-param, so a caller can tell an Alpaca error from an HTTP or transport failure (#97).
// errorCode mixes them: it holds the HTTP status, the Alpaca number, or 0 when no response came.
struct CallError
{
    enum class Kind
    {
        None, // the call succeeded
        Transport, // no HTTP response: message is the curl error text
        Http, // a status other than 200: message is the response body (raw, possibly cut)
        Parse, // a response that is not the expected Alpaca JSON: message is the parser's description
        Alpaca, // ErrorNumber != 0: alpacaError and the server's ErrorMessage
    };
    Kind kind = Kind::None;
    long httpStatus = 0; // 0 when no response came
    long alpacaError = 0; // the Alpaca ErrorNumber; 0 unless kind is Alpaca
    std::string message; // raw bytes from the server or curl, not necessarily valid UTF-8
    long curlCode = 0; // the curl error (CURLcode) of a Transport failure; 0 otherwise
};

// The cap, in bytes, on a reason built by CallErrorReason or OneLine.
const std::size_t kReasonMaxBytes = 200;
// An HTTP error body is shown only when it is at most this long and has no '<' (no HTML error page).
const std::size_t kHttpBodyMaxBytes = 120;

// One line of valid UTF-8: invalid sequences become U+FFFD; control characters (\r, \n, \t, NUL, ...)
// and runs of whitespace become one space; trimmed; when longer than maxBytes, cut to fit with "..." and
// never inside a UTF-8 sequence.
std::string OneLine(const std::string& text, std::size_t maxBytes = kReasonMaxBytes);

// The one-line reason for a failed call, or "" for Kind::None:
//   Alpaca:    "<message> (Alpaca <n>)", or "Alpaca error <n>" when there is no message
//   Http:      "HTTP <status>", plus ": <body>" when the body is short and has no '<'
//   Transport: "no response from the Alpaca server (<curl text>)"
//   Parse:     "invalid response from the Alpaca server"
// The result is OneLine-clean and at most kReasonMaxBytes long (a cut message keeps the Alpaca suffix).
std::string CallErrorReason(const CallError& error);

// Fills a call's optional CallError out-param on every return. It clears *out on entry, so a stale
// value never survives a call; on scope exit it copies the failure recorded with Set, or a generic
// Kind::Parse failure when the function returned without calling Set or Succeeded, so a new return
// path can't leave it unset. Succeeded leaves Kind::None. Target is where a nested call (GetBool's
// Get) writes its failure.
class CallErrorScope
{
public:
    explicit CallErrorScope(CallError *out);
    ~CallErrorScope();
    CallErrorScope(const CallErrorScope&) = delete;
    CallErrorScope& operator=(const CallErrorScope&) = delete;

    void Set(CallError::Kind kind, long httpStatus, long alpacaError, const std::string& message, long curlCode = 0);
    void Succeeded() { m_succeeded = true; }
    CallError *Target() { return &m_error; }

private:
    CallError *m_out;
    CallError m_error;
    bool m_succeeded = false;
};
}

#endif
