/*
 *  restore_calibration.cpp
 *  PHD Guiding
 *
 *  See restore_calibration.h.
 */

#include "restore_calibration.h"

const char *const RESTORE_CALIBRATION_ABORT_REASON = "calibration failed; saved calibration restored";

RestoreCalibrationDecision DecideRestoreCalibration(const RestoreCalibrationState& s)
{
    // these error messages are internal to the event server and are not translated
    if (s.calibratingOrGuiding)
        return { RestoreCalibrationAction::Refuse, "cannot restore calibration while calibrating or guiding" };

    // a disconnected mount reports itself uncalibrated whatever it holds, so the result
    // would say nothing about the restore
    const char *primary = s.primaryIsAo ? "AO" : "mount";
    if (!s.primaryConnected)
        return { RestoreCalibrationAction::Refuse, std::string("cannot restore calibration: ") + primary + " not connected" };
    if (s.secondaryPresent && !s.secondaryConnected)
        return { RestoreCalibrationAction::Refuse, "cannot restore calibration: mount not connected" };

    // a guide request still waiting for a calibration that is no longer running is stale (the
    // calibration failed); any other request in flight is the client's and is left alone
    if (!s.controllerIdle && !s.controllerWaitingForCalibration)
        return { RestoreCalibrationAction::Refuse,
                 "cannot restore calibration while a guide or dither request is in progress" };

    // all or nothing: a partial restore would leave the AO and the mount disagreeing
    if (!s.primarySaved || (s.secondaryPresent && !s.secondarySaved))
        return { RestoreCalibrationAction::NothingSaved, "" };

    if (!s.controllerIdle)
        return { RestoreCalibrationAction::EndStaleGuideThenRestore, "" };

    return { RestoreCalibrationAction::Restore, "" };
}

const char *ClearCalibrationRefusal(bool calibrating)
{
    // not translated, like the restore_calibration errors
    if (calibrating)
        return "cannot clear calibration while calibrating";
    return nullptr;
}
