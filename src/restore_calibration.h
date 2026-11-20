/*
 *  restore_calibration.h
 *  PHD Guiding
 *
 *  restore_calibration (#102): whether the saved calibration can be put back
 *  now, and what has to happen first; clear_calibration (#137): whether the
 *  calibration can be cleared now. Kept free of wx so it can be unit tested
 *  (tests/calibration/test_restore_calibration.cpp).
 */

#ifndef RESTORE_CALIBRATION_H_INCLUDED
#define RESTORE_CALIBRATION_H_INCLUDED

#include <string>

struct RestoreCalibrationState
{
    bool calibratingOrGuiding; // the guider is calibrating or guiding
    bool primaryIsAo; // the primary mount is an AO (the scope is then the secondary mount)
    bool primaryConnected; // a primary mount is selected and connected
    bool secondaryPresent; // there is a secondary mount (AO setups)
    bool secondaryConnected;
    bool primarySaved; // the profile holds a saved calibration for the primary mount
    bool secondarySaved; // ... for the secondary mount
    bool controllerIdle; // no guide or dither request is in progress
    bool controllerWaitingForCalibration; // a guide request is waiting for a calibration to finish
};

enum class RestoreCalibrationAction
{
    Refuse, // error 1 with RestoreCalibrationDecision::error; nothing changes
    NothingSaved, // result false; nothing changes
    Restore, // load the saved calibration
    EndStaleGuideThenRestore, // end the stale guide request (AbortController), then load
};

struct RestoreCalibrationDecision
{
    RestoreCalibrationAction action;
    std::string error; // for Refuse
};

// The reason the stale guide request ends with; its SettleDone carries it as Error.
extern const char *const RESTORE_CALIBRATION_ABORT_REASON;

RestoreCalibrationDecision DecideRestoreCalibration(const RestoreCalibrationState& s);

// clear_calibration (#137): nullptr when the calibration can be cleared now, else the error (code 1).
// Refused while calibrating: the running calibration would lose its state and never finish. Allowed
// while guiding, as in PHD2: guide corrections keep using the calibration guiding started with, but
// dither (and flip_calibration) fail until the next guide start calibrates again.
const char *ClearCalibrationRefusal(bool calibrating);

#endif
