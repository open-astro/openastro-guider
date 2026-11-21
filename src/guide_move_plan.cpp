/*
 *  guide_move_plan.cpp
 *  PHD Guiding
 *
 *  See guide_move_plan.h.
 */

#include "guide_move_plan.h"

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

} // namespace guide_move_plan
