/*
 *  alpaca_errors.h
 *  PHD Guiding
 *
 *  Which Alpaca errors from a capture call mean the server has lost the camera,
 *  and reading an Alpaca error out of a JSON response.
 *  Kept free of wx so it can be unit tested (tests/camera/test_alpaca_errors.cpp).
 */

#ifndef ALPACA_ERRORS_H_INCLUDED
#define ALPACA_ERRORS_H_INCLUDED

#include <atomic>
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

enum class CaptureFailureAction
{
    Alert, // keep the alert and fail the frame
    Reconnect, // disconnect with RECONNECT: EquipmentDisconnected{reconnecting:true}, then the auto-reconnect
};

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
}

#endif
