/*
 *  guider_state_machine.h
 *  PHD Guiding
 *
 *  The guider's state machine (#142, slice T2c of #115 steps 4-9), taken out of
 *  Guider::SetState, Guider::StopGuiding, Guider::UpdateGuideState and
 *  Guider::MoveLockPosition before step 5 turns Guider into a plain class.
 *
 *  The decisions are made here: which state a request leads to, which side
 *  effects run, and in what order. Guider performs each side effect (an Action)
 *  through Host, so its events and Debug lines are the ones it had. The dither
 *  helpers are pure functions of the lock position, the frame and the mount.
 *  Kept free of wx so it can be unit tested (tests/guiding/test_guider_state_machine.cpp).
 */

#ifndef GUIDER_STATE_MACHINE_H_INCLUDED
#define GUIDER_STATE_MACHINE_H_INCLUDED

#include "guider_state.h"

#include <functional>
#include <string>

namespace guider_state_machine
{

// "UNINITIALIZED", "SELECTING", ..., "STOP"; "??" for anything else
const char *StateName(GUIDER_STATE st);

// What Guider does when the state machine says so. Each one is a fixed piece of the old code.
enum class Action
{
    // SetState
    IllegalTransition, // ERROR_INFO("Illegal state transition")
    InvalidateLockPosition, // InvalidateLockPosition(): LockPositionLost if the lock was valid
    InvalidateCurrentPosition,
    ResetErrorCountPrimary, // pMount->ResetErrorCount()
    BeginCalibrationPrimary, // pMount->BeginCalibration(CurrentPosition()); Do returns true on failure
    BeginCalibrationFailedPrimary, // ERROR_INFO("pMount->BeginCalibration failed")
    CalibrationStartedPrimary, // GuideLog.StartCalibration, EvtServer.NotifyStartCalibration
    ResetErrorCountSecondary,
    BeginCalibrationSecondary,
    BeginCalibrationFailedSecondary,
    CalibrationStartedSecondary,
    EnterGuiding, // reset the fast recenter and the failed moves, AdjustCalibrationForScopePointing, status bar
    SetLockToCurrent, // SetLockPosition(CurrentPosition())

    // StopGuiding
    NotifyCalibrationFailedPrimary, // "Calibration manually stopped" or the capture failure's reason
    NotifyCalibrationFailedSecondary,
    NotifyGuidingStopped, // pFrame->NotifyGuidingStopped()

    // UpdateGuideState, a frame with no star
    NotifyLoopingNoStar, // EvtServer.NotifyLooping(frame, nullptr, &info)
    NotifyStarLost, // EvtServer.NotifyStarLost(info)
    CalibrationFrameDropped, // GuideLog.CalibrationFrameDropped(info)
    StatusStarLost, // pFrame->StatusMsg("star lost")
    GuidingFrameDropped, // GuideLog.FrameDropped(info)
    RecordDroppedFrame, // GuidingAssistant::NotifyFrameDropped(info), the guide history
    DeducedMove, // SchedulePrimaryMove(pMount, zero offset, MOVEOPTS_DEDUCED_MOVE)
    LostStarAlert, // the dark red flash and the beep

    // UpdateGuideState, a frame with the star
    LockOnSelectedStar, // SetLockPosition(CurrentPosition()), the current position being valid
    NotifyStarSelected, // EvtServer.NotifyStarSelected(CurrentPosition())
    UpdateCalibrationPrimary, // pMount->UpdateCalibrationState(CurrentPosition()); Do returns true on failure
    UpdateCalibrationSecondary,
    GuidingStarted, // NotifyGuidingStarted, the lock shift's camera rate and log, CheckCalibrationAutoLoad
    RecenterStep, // a fast recenter step after a dither
    BacklashStep, // GuidingAssistant::NotifyBacklashStep (measurement mode)
    GuideStep, // the deflection log and an ordinary guide step
    RecenterOver, // the next frame is not recentering
};

class Host
{
public:
    virtual ~Host() = default;

    virtual GUIDER_STATE State() const = 0;
    virtual void SetStateValue(GUIDER_STATE st) = 0; // m_state = st
    // Guider::SetState, for the transitions UpdateGuideState asks for (it forwards to SetState below)
    virtual void RequestState(GUIDER_STATE st) = 0;
    virtual void Log(const std::string& line) = 0; // Debug.Write
    // Performs the action. Returns true when a BeginCalibration* or UpdateCalibration* fails,
    // false otherwise.
    virtual bool Do(Action action) = 0;

    virtual bool PrimaryCalibrated() const = 0; // pMount->IsCalibrated()
    virtual bool SecondaryConnected() const = 0; // pSecondaryMount && pSecondaryMount->IsConnected()
    virtual bool SecondaryCalibrated() const = 0; // asked only when SecondaryConnected()
    virtual bool MountsIdle() const = 0; // neither mount IsBusy()
    virtual bool LockPositionValid() const = 0;
    virtual bool LockPositionSticky() const = 0;
    virtual bool RecenterActive() const = 0; // m_ditherRecenterRemaining.IsValid()
    virtual bool MeasurementMode() const = 0;
};

// Guider::SetState: STATE_STOP is remapped by the current state, a request more than one
// state ahead is refused, and entering a state may end in another one (SELECTING for
// UNINITIALIZED, CALIBRATED when there is nothing to calibrate, back to CALIBRATING_PRIMARY
// when entering GUIDING cleared the calibration, #255).
void SetState(Host& host, GUIDER_STATE newState);

// Guider::StopGuiding: the CalibrationFailed and GuidingStopped notices, then RequestState(STOP).
void StopGuiding(Host& host);

// UpdateGuideState for a frame where the star was not found.
void OnStarLost(Host& host);

// UpdateGuideState for a frame while paused: only a deduced move, and only when guiding.
void OnPausedFrame(Host& host);

enum class FrameResult
{
    Done,
    PrimaryCalibrationFailed, // the state is back to SELECTING
    SecondaryCalibrationFailed,
};

// UpdateGuideState for a frame where the star was found, not paused.
FrameResult OnStarFound(Host& host);

struct Point
{
    double x;
    double y;
};

struct DitherChoice
{
    bool ok; // false: the transform to camera coordinates failed
    Point camera; // the lock position moves by this
    Point mount; // the same move in mount coordinates
};

// MoveLockPosition near a frame edge: the dither as asked, then reflected in x, in y, and in
// both; the first that gives a valid lock position, or else the one that ends farthest from the
// frame edges (the earliest on a tie). toCamera returns true on error.
DitherChoice ChooseDitherDelta(Point lock, Point mountDelta, double width, double height,
                               const std::function<bool(const Point& mount, Point *camera)>& toCamera,
                               const std::function<bool(const Point& lockPos)>& isValidLock,
                               const std::function<void(const std::string& line)>& log);

struct RecenterPlan
{
    bool start; // false: no fast recenter for this dither
    Point remaining; // |mount delta| per axis
    int dirX; // +1 when the dither moved -x, else -1
    int dirY;
    Point step; // each step is 0.7 x max move along the remaining distance
};

// The fast recenter after a dither. A zero-length dither (dist 0) gets none, so it only starts
// a settle (and the step size would divide by zero).
RecenterPlan PlanFastRecenter(bool enabled, double dist, Point mountDelta, int maxMovePixels);

} // namespace guider_state_machine

#endif
