/*
 *  capture_failure.h
 *  PHD Guiding
 *
 *  Why the guide camera's last capture failed, and the text clients get for it (#97):
 *  SingleFrameComplete.Error, the SettleDone.Error of a guide or dither request the
 *  failure ended, the dark-build error, and the reason appended to the capture alert;
 *  and whether a failed calibration frame is exposed again (#116) or a failed guiding frame
 *  is dropped (#136) instead of stopping.
 *  Kept free of wx so it can be unit tested (tests/capture/test_capture_failure.cpp).
 */

#ifndef CAPTURE_FAILURE_H_INCLUDED
#define CAPTURE_FAILURE_H_INCLUDED

#include "alpaca_errors.h"
#include "guider_state.h"

#include <string>

namespace CaptureFailures
{
// The text when the camera gave no reason (the text clients got before #97)
extern const char kFallback[];

// Kept on the camera for the capture in progress (GuideCamera::m_lastCaptureFailure)
struct Failure
{
    AlpacaErrors::CallError call; // the last failed Alpaca call of this capture
    std::string driverAlert; // the first alert the driver raised for this capture (UTF-8)

    bool Empty() const { return call.kind == AlpacaErrors::CallError::Kind::None && driverAlert.empty(); }
};

// Records a failed call; a later failure of the same capture replaces an earlier one. Kind::None is ignored.
void RecordCallError(Failure& failure, const AlpacaErrors::CallError& error);
// Records the driver's alert text if none is recorded yet (blank text is ignored).
void NoteDriverAlert(Failure& failure, const std::string& alert);

// The one-line reason: the failed call's (AlpacaErrors::CallErrorReason), else the driver alert
// (OneLine-cleaned and capped), else "".
std::string Reason(const Failure& failure);

// "camera error: <reason>", or exactly kFallback when there is no reason.
std::string Text(const Failure& failure);

// "<alert>: <reason>", the alert's trailing whitespace and '.' dropped; the alert unchanged when the
// reason is empty. The alert is the translated prefix; the reason is the server's text, untranslated.
std::string AlertWithReason(const std::string& alert, const std::string& reason);

// Failed frames in a row that are exposed again, as a calibration step's frame (#116) or a dropped
// guiding frame (#136), before the calibration fails or guiding stops. The first frame plus these
// retries is exactly kAcquisitionFailuresBeforeReconnect 1280s, so a 3rd 1280 in a row still
// reconnects the camera (#103) instead of being retried.
const int kFrameRetries = 2;
static_assert(kFrameRetries < AlpacaErrors::kAcquisitionFailuresBeforeReconnect,
              "the #103 reconnect must stay reachable while a calibration or guiding frame is retried");

// What the guider was doing when a frame failed
enum class FramePhase
{
    Other, // looping, a star selected, STATE_CALIBRATED or uninitialized: a failure stops capture
    Calibrating, // a calibration step: the step's frame is exposed again (#116)
    Guiding, // guiding or settling: the frame is dropped, like a lost-star frame (#136)
};

// The phase of a guider state: STATE_CALIBRATING_PRIMARY and _SECONDARY (Guider::IsCalibrating) are
// Calibrating, STATE_GUIDING (Guider::IsGuiding, including a settle and a paused guider) is Guiding,
// every other state is Other. STATE_CALIBRATED is Other: it lasts past a frame only when guiding
// starts on an existing calibration, where there is no step to repeat.
FramePhase PhaseFor(GUIDER_STATE state);

// True for a failure that a new exposure can clear: Alpaca 1280 (the driver's image
// acquisition failed), a call that timed out with no response (Transport with curlCode
// kCurlOperationTimedOut; the code, not curl's text, which curl rewords between versions, #227),
// and a call the server died during, with no response or a cut one (kCurlGotNothing, kCurlRecvError,
// kCurlPartialFile, #237): if the server is really gone, the retried frame's call is refused and
// reconnects the camera (#190).
// False for anything else:
// 1031 and other Alpaca errors, an HTTP status, an invalid response, another transport failure,
// a driver alert alone (the capture watchdog's timeout reconnects instead), and an empty
// failure (an aborted or interrupted exposure records none).
bool IsTransient(const Failure& failure);

// Whether a failed frame is exposed again instead of failing the calibration (#116) or stopping
// guiding (#136): calibrating or guiding, capture continuing (no stop requested), the camera
// connected, a transient failure, and fewer than kFrameRetries retries in a row so far.
bool ShouldRetryFrame(const Failure& failure, int retriesSoFar, FramePhase phase, bool continuing, bool cameraConnected);

// The count of failed frames in a row retried so far (#116, #136), kept by MyFrame. A good frame and a
// failure that is not retried start a new count. A camera reconnect (#103, which resets the camera's
// own count of 1280s) also starts one, but only once per run of failed frames: a camera that fails
// every frame would otherwise go round "2 retries, a 3rd 1280 reconnects, a fresh count" for ever
// whenever such a round takes longer than the reconnect throttle's window (3 reconnects a minute),
// and the calibration or guiding would never end. With the one refill, a run of failures ends the
// calibration or stops guiding after at most 2 reconnects.
class FrameRetries
{
public:
    // Whether this failed frame is exposed again (ShouldRetryFrame). Counts the retry when it is;
    // starts a new count when it is not.
    bool OnFailedFrame(const Failure& failure, FramePhase phase, bool continuing, bool cameraConnected);
    // A good frame ends the run of failed frames.
    void OnGoodFrame();
    // The camera reconnected and the exposure resumes: a new count, once per run of failed frames.
    void OnReconnect();
    // Failed frames in a row retried so far
    int Count() const { return m_retries; }

private:
    int m_retries = 0;
    bool m_refilled = false; // a reconnect already started a new count in this run of failed frames
};
}

#endif
