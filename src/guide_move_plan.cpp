/*
 *  guide_move_plan.cpp
 *  PHD Guiding
 *
 *  See guide_move_plan.h.
 */

#include "guide_move_plan.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>

namespace guide_move_plan
{
int PulseMs(double distance, double rate)
{
    double ms = std::fabs(distance / rate);
    if (std::isnan(ms))
        return 0;
    if (ms >= static_cast<double>(INT_MAX))
        return INT_MAX;
    return static_cast<int>(std::floor(ms + 0.5));
}

OffsetPlan PlanOffset(double xDistance, double yDistance, double xRate, double yRate)
{
    OffsetPlan plan;
    plan.raDir = xDistance > 0.0 ? Dir::West : Dir::East;
    plan.decDir = yDistance > 0.0 ? Dir::South : Dir::North;
    plan.raMs = PulseMs(xDistance, xRate);
    plan.decMs = PulseMs(yDistance, yRate);
    return plan;
}

// The MAX_PULSE_MS ceiling of every move sized from a star offset
static void CapPulse(AxisPlan& plan)
{
    if (plan.ms > MAX_PULSE_MS)
    {
        plan.ms = MAX_PULSE_MS;
        plan.limited = true;
        plan.capped = true;
    }
}

// The clamp and the limit-reached count of one axis
static void LimitAxis(Dir dir, int maxMs, AxisPlan& plan, LimitTracker& tracker)
{
    if (plan.ms > maxMs)
    {
        plan.ms = maxMs;
        plan.limited = true;
    }
    CapPulse(plan);

    // count the clamped steps in a row in one direction, this one included (#153)
    if (!plan.limited)
        tracker.count = 0;
    else if (dir == tracker.lastLimited)
        ++tracker.count;
    else
        tracker.count = 1;

    if (tracker.count >= LIMIT_REACHED_WARN_COUNT)
        plan.alert = true;

    tracker.lastLimited = plan.limited ? dir : Dir::None;
}

AxisPlan PlanAxis(Dir dir, int ms, unsigned moveOptions, const AxisPolicy& policy, LimitTracker& ra, LimitTracker& dec)
{
    AxisPlan plan;

    if (!policy.guidingEnabled && (moveOptions & MANUAL) == 0)
    {
        plan.refused = true;
        return plan;
    }

    plan.ms = ms;

    // Only guide steps and deduced moves are filtered by the Dec guide mode and clamped to the max
    // duration. Recovery moves (a dither's fast recenter) and AO bumps are exempt, as upstream:
    // they are sized by the search region and the bump step, PHD2 dithers in Dec with a North or
    // South mode (PhdController sets Auto for the settle), and a bump has to unload the AO
    // whichever way it leans. They still get the MAX_PULSE_MS ceiling (#152). Calibration and
    // manual moves keep the length their caller chose.
    if ((moveOptions & (ALGO_RESULT | ALGO_DEDUCE)) == 0)
    {
        if ((moveOptions & USE_BLC) != 0)
            CapPulse(plan);
        return plan;
    }

    switch (dir)
    {
    case Dir::North:
    case Dir::South:
        if (policy.decMode == DecMode::None || (dir == Dir::South && policy.decMode == DecMode::North) ||
            (dir == Dir::North && policy.decMode == DecMode::South))
        {
            plan.ms = 0;
            plan.zeroedByDecMode = true;
        }
        LimitAxis(dir, policy.maxDecMs, plan, dec);
        break;

    case Dir::East:
    case Dir::West:
        LimitAxis(dir, policy.maxRaMs, plan, ra);
        break;

    case Dir::None:
        break;
    }

    return plan;
}

bool LimitAlertThrottle::ShouldRaise(Axis axis, int64_t nowS, int64_t deferredUntilS)
{
    if (nowS < deferredUntilS)
        return false;

    // One time per axis, restarted only by a raised alert: one shared time, restarted by every
    // call that got this far, held a Dec alert back after an RA one and the first alert after the
    // grace period back after a guide step during it (#164)
    int i = axis == Axis::Ra ? 0 : 1;
    if (m_raised[i] && nowS < m_lastRaisedS[i] + LIMIT_ALERT_THROTTLE_S)
        return false;

    m_raised[i] = true;
    m_lastRaisedS[i] = nowS;
    return true;
}

bool PulseFailedAlertThrottle::ShouldRaise(int64_t nowS)
{
    // One alert per failed pulse, two a frame when both axes correct, flooded the client (#188)
    if (m_raised && nowS < m_lastRaisedS + PULSE_FAILED_ALERT_THROTTLE_S)
        return false;

    m_raised = true;
    m_lastRaisedS = nowS;
    return true;
}

int64_t AlertClockS()
{
    // Not the wall clock: a step back of the system time held the alert back for as long as the
    // step, and a step forward cut the grace period short (#212)
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double MovedPx(double distancePx, int requestedMs, int sentMs, double rate)
{
    double distance = std::fabs(distancePx);
    if (sentMs >= requestedMs)
        return distance;
    if (sentMs <= 0)
        return 0.0;

    // A cut pulse: backlash compensation is taken up first, so the unsent part is the step's
    double unsentPx = (static_cast<double>(requestedMs) - static_cast<double>(sentMs)) * std::fabs(rate);
    return distance > unsentPx ? distance - unsentPx : 0.0;
}

RecenterProgress AfterRecenterStep(double remainingX, double remainingY, double movedX, double movedY, int stalledSteps)
{
    RecenterProgress progress;
    progress.remainingX = std::max(0.0, remainingX - movedX);
    progress.remainingY = std::max(0.0, remainingY - movedY);
    bool movedNothing = movedX <= 0.0 && movedY <= 0.0;
    progress.stalledSteps = movedNothing ? stalledSteps + 1 : 0;
    progress.done =
        (progress.remainingX < 0.5 && progress.remainingY < 0.5) || progress.stalledSteps >= RECENTER_MAX_STALLED_STEPS;
    return progress;
}

bool SendDecAfter(MoveResult ra)
{
    return ra != MoveResult::ErrorSlewing && ra != MoveResult::ErrorAoLimitReached;
}

MoveResult OffsetResult(MoveResult ra, MoveResult dec)
{
    // A failed Dec pulse fails the move; else the RA pulse's result stands. With the Dec pulse
    // sent, RA was Ok or Error, so a Dec slew or AO limit, which OnMoveComplete acts on, wins
    return dec != MoveResult::Ok ? dec : ra;
}

FailedGuideMoves AfterGuideMove(MoveResult result, int failedInARow)
{
    // As today: Mount::ErrorCount() counts every failed move and nothing reads it while guiding
    FailedGuideMoves failed;
    failed.inARow = result != MoveResult::Ok ? failedInARow + 1 : failedInARow;
    return failed;
}

} // namespace guide_move_plan
