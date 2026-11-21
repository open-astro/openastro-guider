/*
 *  guide_move_plan.h
 *  PHD Guiding
 *
 *  How a guide correction becomes mount pulses (#142): the direction and
 *  length of each axis's pulse from Mount::MoveOffset (PlanOffset), and the
 *  guiding-disabled refusal, Dec guide mode filter, max-duration clamp and
 *  limit-reached counting from Scope::MoveAxis (PlanAxis), the limit-reached
 *  alert's throttle from Scope::AlertLimitReached (LimitAlertThrottle), what a
 *  dither's fast-recenter step moved (MovedPx, AfterRecenterStep), and the result
 *  Mount::MoveOffset reports for its two pulses (SendDecAfter, OffsetResult).
 *  Kept free of wx so it can be unit tested (tests/guiding/test_guide_move_plan.cpp).
 */

#ifndef GUIDE_MOVE_PLAN_H_INCLUDED
#define GUIDE_MOVE_PLAN_H_INCLUDED

#include <cstdint>

namespace guide_move_plan
{
// == GUIDE_DIRECTION (mount.h); scope.cpp static_asserts the values
enum class Dir
{
    None = -1,
    North = 0, // UP
    South = 1, // DOWN
    East = 2, // RIGHT
    West = 3, // LEFT
};

// == DEC_GUIDE_MODE (scope.h)
enum class DecMode
{
    None = 0,
    Auto,
    North,
    South,
};

// == MountMoveOptionBits (mount.h)
constexpr unsigned ALGO_RESULT = 1u << 0;
constexpr unsigned ALGO_DEDUCE = 1u << 1;
constexpr unsigned USE_BLC = 1u << 2;
constexpr unsigned GRAPH = 1u << 3;
constexpr unsigned MANUAL = 1u << 4;

// The clamped guide steps in a row in one direction reaching this raises the limit-reached
// alert, so the 5th is the first to alert (#153; the first clamp of a run used to count 0)
constexpr int LIMIT_REACHED_WARN_COUNT = 5;

// The longest pulse PlanAxis lets a guide step, deduced move, recovery move or AO bump send
// (#152): under the 30 s HTTP timeout AlpacaClient gives a PulseGuide that blocks while it runs,
// the ceiling #141 gave the calibration's grown RA step (calibration_run.h, RA_STEP_MAX_MS).
// A rate of 0 makes PulseMs return INT_MAX, which a recovery move or AO bump otherwise sends as is.
constexpr int MAX_PULSE_MS = 25000;

// ROUND(fabs(distance / rate)), the pulse Mount::MoveOffset asks for. A quotient that does
// not fit an int (a rate of 0, or one Dec compensation made tiny) gives INT_MAX and a NaN
// (0 / 0) gives 0, which is what the plain cast does on arm64; MoveAxis then clamps a guide
// step to the max duration.
int PulseMs(double distance, double rate);

struct OffsetPlan
{
    Dir raDir = Dir::East;
    int raMs = 0;
    Dir decDir = Dir::North;
    int decMs = 0;
};

// Mount::MoveOffset's pulses for a mount-axis offset in pixels: x > 0 is West (LEFT), else
// East; y > 0 is South (DOWN), else North. xRate is the Dec-compensated RA rate, yRate the
// calibration's Dec rate, both in pixels per ms.
OffsetPlan PlanOffset(double xDistance, double yDistance, double xRate, double yRate);

// Scope's m_ra/decLimitReachedDirection and count: the direction of the last guide step's clamp
// (None when it was not clamped) and the clamped steps in a row in that direction
struct LimitTracker
{
    Dir lastLimited = Dir::None;
    int count = 0;
};

struct AxisPolicy
{
    bool guidingEnabled = true;
    DecMode decMode = DecMode::Auto;
    int maxRaMs = 0;
    int maxDecMs = 0;
};

struct AxisPlan
{
    bool refused = false; // guiding is disabled and this is not a manual move: no pulse
    int ms = 0; // the pulse to send (0: none)
    bool zeroedByDecMode = false; // the Dec guide mode zeroed it
    bool limited = false; // clamped to the axis's max duration or to MAX_PULSE_MS
    bool capped = false; // MAX_PULSE_MS, not the max duration, set the pulse
    bool alert = false; // raise the limit-reached alert for this axis
};

// Scope::MoveAxis's pulse for a requested one. Only guide steps and deduced moves
// (ALGO_RESULT or ALGO_DEDUCE) are filtered by the Dec guide mode, clamped to the max duration
// and counted; recovery and AO-bump moves (USE_BLC alone) are not, but they and the guide steps
// are capped at MAX_PULSE_MS. Calibration (no bits) and manual moves pass through as asked. The
// tracker of the move's axis is updated.
AxisPlan PlanAxis(Dir dir, int ms, unsigned moveOptions, const AxisPolicy& policy, LimitTracker& ra, LimitTracker& dec);

// == GuideAxis (guide_algorithm.h)
enum class Axis
{
    Ra = 0,
    Dec = 1,
};

// The shortest time between two limit-reached alerts of one axis
constexpr int64_t LIMIT_ALERT_THROTTLE_S = 30;

// Scope::AlertLimitReached's throttle (#164), one per axis, on the caller's clock in seconds.
// No alert is raised before the grace period Scope::DeferPulseLimitAlertCheck starts after a
// calibration or a guide start ends, and an axis's alert is raised at most once every
// LIMIT_ALERT_THROTTLE_S. Only a raised alert restarts its axis's throttle.
class LimitAlertThrottle
{
public:
    // Whether to raise the axis's alert at nowS, when the grace period ends at deferredUntilS
    bool ShouldRaise(Axis axis, int64_t nowS, int64_t deferredUntilS);

private:
    bool m_raised[2] = { false, false };
    int64_t m_lastRaisedS[2] = { 0, 0 };
};

// The px one axis of a Mount::MoveOffset moved of distancePx (#165). MoveOffset asked MoveAxis for
// requestedMs (the axis's PulseMs, plus any Dec backlash compensation) and MoveAxis sent sentMs
// (MoveResultInfo::amountMoved), which is less when PlanAxis caps the pulse at MAX_PULSE_MS or an
// AO's travel cuts its steps, and 0 when the move fails or is refused. rate is the px per ms (per
// step for an AO) MoveOffset sized the pulse with. All of |distancePx| when the whole pulse was
// sent, none when nothing was, else |distancePx| less the unsent part at rate, at least 0.
double MovedPx(double distancePx, int requestedMs, int sentMs, double rate);

// Steps in a row that moved nothing after which a dither's fast recenter ends (#165)
constexpr int RECENTER_MAX_STALLED_STEPS = 3;

struct RecenterProgress
{
    double remainingX = 0.0; // px still to move on each mount axis
    double remainingY = 0.0;
    int stalledSteps = 0; // steps in a row, this one included, that moved nothing
    bool done = false; // the fast recenter is over: the guide algorithm corrects what is left
};

// Guider's dither fast recenter after one of its steps (#165): remainingX/Y px (magnitudes) less
// the movedX/Y px the step moved; stalledSteps is the steps in a row before this one that moved
// nothing. It is done when under 0.5 px is left on both axes, or when RECENTER_MAX_STALLED_STEPS
// steps in a row moved nothing (they failed or were refused), so a mount that does not move ends
// the recenter and a single failed step is made up by the next ones.
RecenterProgress AfterRecenterStep(double remainingX, double remainingY, double movedX, double movedY, int stalledSteps);

// == Mount::MOVE_RESULT (mount.h); mount.cpp static_asserts the values
enum class MoveResult
{
    Ok = 0,
    Error, // failed for an unspecified reason
    ErrorSlewing, // the scope is slewing
    ErrorAoLimitReached, // the AO hit its travel limit
};

// Whether Mount::MoveOffset sends its Dec pulse after its RA move returned ra: not when the
// scope is slewing or the AO hit its travel limit
bool SendDecAfter(MoveResult ra);

// Mount::MoveOffset's result for its RA move's ra and its Dec move's dec (Ok when SendDecAfter
// said not to send it) (#181)
MoveResult OffsetResult(MoveResult ra, MoveResult dec);

} // namespace guide_move_plan

#endif // GUIDE_MOVE_PLAN_H_INCLUDED
