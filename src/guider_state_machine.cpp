/*
 *  guider_state_machine.cpp
 *  PHD Guiding
 *
 *  The guider's state machine, out of guider.cpp (#142, slice T2c). See guider_state_machine.h.
 */

#include "guider_state_machine.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace guider_state_machine
{

const char *StateName(GUIDER_STATE st)
{
    switch (st)
    {
    case STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case STATE_SELECTING:
        return "SELECTING";
    case STATE_SELECTED:
        return "SELECTED";
    case STATE_CALIBRATING_PRIMARY:
        return "CALIBRATING_PRIMARY";
    case STATE_CALIBRATING_SECONDARY:
        return "CALIBRATING_SECONDARY";
    case STATE_CALIBRATED:
        return "CALIBRATED";
    case STATE_GUIDING:
        return "GUIDING";
    case STATE_STOP:
        return "STOP";
    default:
        return "??";
    }
}

static std::string StateLine(const char *fmt, GUIDER_STATE a)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), fmt, StateName(a));
    return buf;
}

static std::string StateLine(const char *fmt, GUIDER_STATE a, GUIDER_STATE b)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), fmt, StateName(a), StateName(b));
    return buf;
}

void SetState(Host& host, GUIDER_STATE newState)
{
    const GUIDER_STATE current = host.State();

    host.Log(StateLine("Changing from state %s to %s\n", current, newState));

    if (newState == STATE_STOP)
    {
        // we are going to stop looping exposures.  We should put
        // ourselves into a good state to restart looping later
        switch (current)
        {
        case STATE_UNINITIALIZED:
        case STATE_SELECTING:
        case STATE_SELECTED:
            newState = current;
            break;
        case STATE_CALIBRATING_PRIMARY:
        case STATE_CALIBRATING_SECONDARY:
            // because we have done some moving here, we need to just
            // start over...
            newState = STATE_UNINITIALIZED;
            break;
        case STATE_CALIBRATED:
        case STATE_GUIDING:
            newState = STATE_SELECTED;
            break;
        case STATE_STOP:
            break;
        }
    }

    assert(newState != STATE_STOP);

    if (newState > current + 1)
    {
        host.Log(StateLine("Cannot transition from %s to newState=%s\n", current, newState));
        host.Do(Action::IllegalTransition);
        return;
    }

    const GUIDER_STATE requestedState = newState;

    switch (requestedState)
    {
    case STATE_UNINITIALIZED:
        host.Do(Action::InvalidateLockPosition);
        host.Do(Action::InvalidateCurrentPosition);
        newState = STATE_SELECTING;
        break;
    case STATE_SELECTED:
        break;
    case STATE_CALIBRATING_PRIMARY:
        if (!host.PrimaryCalibrated())
        {
            host.Do(Action::ResetErrorCountPrimary);
            if (host.Do(Action::BeginCalibrationPrimary))
            {
                newState = STATE_UNINITIALIZED;
                host.Do(Action::BeginCalibrationFailedPrimary);
            }
            else
            {
                host.Do(Action::CalibrationStartedPrimary);
            }
            break;
        }
        [[fallthrough]];
    case STATE_CALIBRATING_SECONDARY:
        if (!host.SecondaryConnected())
        {
            newState = STATE_CALIBRATED;
        }
        else if (!host.SecondaryCalibrated())
        {
            host.Do(Action::ResetErrorCountSecondary);
            if (host.Do(Action::BeginCalibrationSecondary))
            {
                newState = STATE_UNINITIALIZED;
                host.Do(Action::BeginCalibrationFailedSecondary);
            }
            else
            {
                host.Do(Action::CalibrationStartedSecondary);
            }
        }
        break;
    case STATE_GUIDING:
        host.Do(Action::EnterGuiding);

        // #255: the adjustment clears the calibration when the image scale changed (HandleImageScaleChange), after
        // the calibrating states decided there was nothing to calibrate; calibrate now instead of guiding uncalibrated
        if (!host.PrimaryCalibrated() || (host.SecondaryConnected() && !host.SecondaryCalibrated()))
        {
            host.Log("calibration cleared on guiding start, calibrating\n");
            newState = STATE_CALIBRATING_PRIMARY;
            break;
        }

        if (host.LockPositionValid() && host.LockPositionSticky())
        {
            host.Log("keeping sticky lock position\n");
        }
        else
        {
            host.Do(Action::SetLockToCurrent);
        }
        break;

    case STATE_SELECTING:
    case STATE_CALIBRATED:
    case STATE_STOP:
        break;
    }

    if (newState >= requestedState)
    {
        host.SetStateValue(newState);
        host.Log(StateLine("guider state => %s\n", newState));
    }
    else
    {
        host.Log(StateLine("SetState recurses newState %s\n", newState));
        SetState(host, newState);
    }
}

void StopGuiding(Host& host)
{
    // first, send a notification that we stopped
    const GUIDER_STATE current = host.State();
    switch (current)
    {
    case STATE_UNINITIALIZED:
    case STATE_SELECTING:
    case STATE_SELECTED:
        break;
    case STATE_CALIBRATING_PRIMARY:
    case STATE_CALIBRATING_SECONDARY:
    case STATE_CALIBRATED:
        host.Do(current == STATE_CALIBRATING_SECONDARY ? Action::NotifyCalibrationFailedSecondary
                                                       : Action::NotifyCalibrationFailedPrimary);
        [[fallthrough]];
    case STATE_GUIDING:
        // Notify guiding stopped if there are no outstanding guide steps.  The Guiding
        // Stopped notification must come after the final GuideStep notification otherwise
        // event server clients and the guide log will show the guide step happening after
        // guiding stopped.
        if (host.MountsIdle())
            host.Do(Action::NotifyGuidingStopped);
        break;
    case STATE_STOP:
        break;
    }

    host.RequestState(STATE_STOP);
}

void OnStarLost(Host& host)
{
    switch (host.State())
    {
    case STATE_UNINITIALIZED:
    case STATE_SELECTING:
        host.Do(Action::NotifyLoopingNoStar);
        break;
    case STATE_SELECTED:
        // we had a current position and lost it
        host.Do(Action::NotifyLoopingNoStar);
        host.RequestState(STATE_UNINITIALIZED);
        host.Do(Action::NotifyStarLost);
        break;
    case STATE_CALIBRATING_PRIMARY:
    case STATE_CALIBRATING_SECONDARY:
        host.Do(Action::CalibrationFrameDropped);
        host.Log("Star lost during calibration... blundering on\n");
        host.Do(Action::NotifyStarLost);
        host.Do(Action::StatusStarLost);
        break;
    case STATE_GUIDING:
        host.Do(Action::GuidingFrameDropped);
        host.Do(Action::NotifyStarLost);
        host.Do(Action::RecordDroppedFrame);
        // allow guide algorithms to attempt dead reckoning
        host.Do(Action::DeducedMove);
        host.Do(Action::LostStarAlert);
        break;
    case STATE_CALIBRATED:
    case STATE_STOP:
        break;
    }
}

void OnPausedFrame(Host& host)
{
    if (host.State() == STATE_GUIDING)
    {
        // allow guide algorithms to attempt dead reckoning
        host.Do(Action::DeducedMove);
    }
}

FrameResult OnStarFound(Host& host)
{
    switch (host.State())
    {
    case STATE_SELECTING:
        host.Do(Action::LockOnSelectedStar);
        host.Log("CurrentPosition() valid, moving to STATE_SELECTED\n");
        host.Do(Action::NotifyStarSelected);
        host.RequestState(STATE_SELECTED);
        break;
    case STATE_SELECTED:
        break;
    case STATE_CALIBRATING_PRIMARY:
        if (!host.PrimaryCalibrated())
        {
            if (host.Do(Action::UpdateCalibrationPrimary))
            {
                host.RequestState(STATE_UNINITIALIZED);
                return FrameResult::PrimaryCalibrationFailed;
            }

            if (!host.PrimaryCalibrated())
            {
                break;
            }
        }

        host.RequestState(STATE_CALIBRATING_SECONDARY);

        if (host.State() == STATE_CALIBRATING_SECONDARY)
        {
            // if we really have a secondary mount, and it isn't calibrated, we need to take
            // another exposure before going on, so the frame does not show the lock position in
            // the wrong place
            break;
        }

        [[fallthrough]];
    case STATE_CALIBRATING_SECONDARY:
        if (host.SecondaryConnected())
        {
            if (!host.SecondaryCalibrated())
            {
                if (host.Do(Action::UpdateCalibrationSecondary))
                {
                    host.RequestState(STATE_UNINITIALIZED);
                    return FrameResult::SecondaryCalibrationFailed;
                }
            }

            if (!host.SecondaryCalibrated())
            {
                break;
            }
        }
        assert(!host.SecondaryConnected() || host.SecondaryCalibrated());

        host.RequestState(STATE_CALIBRATED);
        [[fallthrough]];
    case STATE_CALIBRATED:
        assert(host.State() == STATE_CALIBRATED);
        host.RequestState(STATE_GUIDING);
        if (host.State() != STATE_GUIDING)
            break; // entering guiding cleared the calibration and started a new one (#255)

        host.Do(Action::GuidingStarted);
        break;
    case STATE_GUIDING:
        if (host.RecenterActive())
        {
            // fast recenter after dither taking large steps and bypassing guide algorithms
            host.Do(Action::RecenterStep);
        }
        else if (host.MeasurementMode())
        {
            host.Do(Action::BacklashStep);
        }
        else
        {
            host.Do(Action::GuideStep);
        }
        // a recenter that NotifyRecenterMoveComplete ended is over for the next frame too (#165)
        if (!host.RecenterActive())
            host.Do(Action::RecenterOver);
        break;

    case STATE_UNINITIALIZED:
    case STATE_STOP:
        break;
    }

    return FrameResult::Done;
}

// distance to nearest edge
static double EdgeDist(const Point& pt, double width, double height)
{
    auto min = [](double a, double b) { return a < b ? a : b; };
    return min(pt.x, min(width - pt.x, min(pt.y, height - pt.y)));
}

DitherChoice ChooseDitherDelta(Point lock, Point mountDelta, double width, double height,
                               const std::function<bool(const Point& mount, Point *camera)>& toCamera,
                               const std::function<bool(const Point& lockPos)>& isValidLock,
                               const std::function<void(const std::string& line)>& log)
{
    DitherChoice choice { true, { 0.0, 0.0 }, { 0.0, 0.0 } };
    double dbest = 0.0;

    for (int q = 0; q < 4; q++)
    {
        int sx = 1 - ((q & 1) << 1);
        int sy = 1 - (q & 2);

        Point tmpMount { mountDelta.x * sx, mountDelta.y * sy };
        Point tmpCamera { 0.0, 0.0 };

        if (toCamera(tmpMount, &tmpCamera))
        {
            choice.ok = false;
            return choice;
        }

        Point tmpLockPosition { lock.x + tmpCamera.x, lock.y + tmpCamera.y };

        if (isValidLock(tmpLockPosition))
        {
            choice.camera = tmpCamera;
            choice.mount = tmpMount;
            break;
        }

        log("dither produces an invalid lock position, try a variation\n");

        double d = EdgeDist(tmpLockPosition, width, height);
        if (q == 0 || d > dbest)
        {
            choice.camera = tmpCamera;
            choice.mount = tmpMount;
            dbest = d;
        }
    }

    return choice;
}

RecenterPlan PlanFastRecenter(bool enabled, double dist, Point mountDelta, int maxMovePixels)
{
    RecenterPlan plan { false, { 0.0, 0.0 }, 0, 0, { 0.0, 0.0 } };

    // Zero-length dithers result in div-by-zero in calculation of 'f'.  We can still allow zero-length dithers as a way of
    // triggering a settling period if we temporarily ignore the 'fast re-center' option
    if (!enabled || dist == 0)
        return plan;

    plan.start = true;
    plan.remaining = { std::fabs(mountDelta.x), std::fabs(mountDelta.y) };
    plan.dirX = mountDelta.x < 0.0 ? 1 : -1;
    plan.dirY = mountDelta.y < 0.0 ? 1 : -1;
    // make each step a bit less than the full search region distance to avoid losing the star
    double f = ((double) maxMovePixels * 0.7) / std::hypot(plan.remaining.x, plan.remaining.y);
    plan.step = { f * plan.remaining.x, f * plan.remaining.y };
    return plan;
}

} // namespace guider_state_machine
