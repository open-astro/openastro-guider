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

// The clamp and the limit-reached count of one axis
static void LimitAxis(Dir dir, int maxMs, AxisPlan& plan, LimitTracker& tracker)
{
    if (plan.ms > maxMs)
    {
        plan.ms = maxMs;
        plan.limited = true;
    }

    if (plan.limited && dir == tracker.lastLimited)
    {
        if (++tracker.count >= LIMIT_REACHED_WARN_COUNT)
            plan.alert = true;
    }
    else
        tracker.count = 0;

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

    // only guide steps and deduced moves are filtered and clamped
    if ((moveOptions & (ALGO_RESULT | ALGO_DEDUCE)) == 0)
        return plan;

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

} // namespace guide_move_plan
