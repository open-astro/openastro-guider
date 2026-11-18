/*
 *  capture_failure.h
 *  PHD Guiding
 *
 *  Why the guide camera's last capture failed, and the text clients get for it (#97):
 *  SingleFrameComplete.Error, the SettleDone.Error of a guide or dither request the
 *  failure ended, the dark-build error, and the reason appended to the capture alert.
 *  Kept free of wx so it can be unit tested (tests/capture/test_capture_failure.cpp).
 */

#ifndef CAPTURE_FAILURE_H_INCLUDED
#define CAPTURE_FAILURE_H_INCLUDED

#include "alpaca_errors.h"

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
}

#endif
