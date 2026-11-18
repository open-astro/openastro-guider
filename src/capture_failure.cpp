/*
 *  capture_failure.cpp
 *  PHD Guiding
 *
 *  See capture_failure.h.
 */

#include "capture_failure.h"

namespace CaptureFailures
{
const char kFallback[] = "camera error -- see debug log";

void RecordCallError(Failure& failure, const AlpacaErrors::CallError& error)
{
    if (error.kind != AlpacaErrors::CallError::Kind::None)
        failure.call = error;
}

void NoteDriverAlert(Failure& failure, const std::string& alert)
{
    if (failure.driverAlert.empty() && !AlpacaErrors::OneLine(alert).empty())
        failure.driverAlert = alert;
}

std::string Reason(const Failure& failure)
{
    std::string reason = AlpacaErrors::CallErrorReason(failure.call);
    if (reason.empty())
        reason = AlpacaErrors::OneLine(failure.driverAlert, AlpacaErrors::kReasonMaxBytes);
    return reason;
}

std::string Text(const Failure& failure)
{
    std::string reason = Reason(failure);
    if (reason.empty())
        return kFallback;
    return "camera error: " + reason;
}

std::string AlertWithReason(const std::string& alert, const std::string& reason)
{
    if (reason.empty())
        return alert;
    std::string prefix(alert);
    while (!prefix.empty() &&
           (prefix.back() == '.' || prefix.back() == ' ' || prefix.back() == '\n' || prefix.back() == '\r' ||
            prefix.back() == '\t'))
        prefix.pop_back();
    if (prefix.empty())
        return reason;
    return prefix + ": " + reason;
}
}
