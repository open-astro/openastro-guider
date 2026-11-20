/*
 *  calibration_run.h
 *  PHD Guiding
 *
 *  The mount calibration state machine (West, East, clear Dec backlash,
 *  North, South, nudge South), factored out of Scope::UpdateCalibrationState
 *  so it can be driven and tested without wxWidgets, a Scope or a live mount
 *  (#142). Kept free of wx; tested in closed loop against a simulated mount
 *  and against a frozen copy of the code it replaced
 *  (tests/closed_loop/test_closed_loop_calibration.cpp).
 *
 *  The run decides; Scope does. Every side effect the calibration has (a
 *  guide-log row, a status message and Calibrating event, a guide pulse, a
 *  Debug line, an advisory) goes out through the Sink, synchronously and in
 *  the order the code produced it, and every mount read goes through the
 *  Probe at the same points as before, so Debug lines a read writes stay
 *  where they were. Translated texts, alerts, failure reporting and the
 *  completion (SetCalibration, the profile, the sanity check) stay in Scope.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Bret McKee, Dad Dog Development,
 *     Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef CALIBRATION_RUN_H_INCLUDED
#define CALIBRATION_RUN_H_INCLUDED

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace calibration_run
{

// A star position or mount coordinate pair with PHD_Point's semantics: a
// default point is invalid, and Angle is 0 for the (0,0) vector.
struct Pt
{
    double x = 0.;
    double y = 0.;
    bool valid = false;

    static Pt At(double x, double y) { return Pt { x, y, true }; }
};

// Same values as GUIDE_DIRECTION (mount.h); scope.cpp asserts it
enum class Dir
{
    North = 0,
    South = 1,
    East = 2,
    West = 3
};

// Same values as DEC_GUIDE_MODE (scope.h); scope.cpp asserts it
enum class DecMode
{
    None = 0,
    Auto,
    North,
    South
};

// Same values as GuideParity (mount.h); scope.cpp asserts it
enum class Parity
{
    Odd = -1,
    Unknown = 0,
    Even = 1
};

enum class State
{
    Cleared,
    GoWest,
    GoEast,
    ClearBacklash,
    GoNorth,
    GoSouth,
    NudgeSouth,
    Complete
};

// The direction text of a calibration step (CalibrationStepInfo.direction)
enum class Leg
{
    West,
    East,
    Backlash,
    North,
    South,
    NudgeSouth
};

// Why a run failed; Scope reports each one with its own message
enum class Fail
{
    Cleared, // the calibration was cleared while it ran (#137)
    RaDidNotMove, // "RA Calibration Failed: star did not move enough"
    BacklashClearingFailed, // "Backlash Clearing Failed: star did not move enough"
    DecDidNotMove, // "DEC Calibration Failed: star did not move enough"
};

constexpr int MAX_CALIBRATION_STEPS = 60;
constexpr int MAX_NUDGES = 3;
constexpr double NUDGE_TOLERANCE = 2.0;
constexpr double RATE_UNCALIBRATED = 123e4; // CALIBRATION_RATE_UNCALIBRATED (scope.h)

// The largest factor RaCalibrationStepMs grows the RA step by (#141)
constexpr double RA_STEP_MAX_SCALE = 4.0;

// The pulse the West and East legs use for a calibration step of stepMs at
// declinationRad (#141). A star's RA motion per ms of guide pulse shrinks with
// cos(Dec), so a step that suits the equator needs 1/cos(Dec) as many pulses
// near the pole and runs out of its MAX_CALIBRATION_STEPS above about 79 deg.
// No declination (nullopt) gives stepMs.
int RaCalibrationStepMs(int stepMs, std::optional<double> declinationRad);

// What the run reads from Scope and the guider. Scope fills it from its members
// on every frame; the run reads each field where the old code read the member.
struct Settings
{
    int stepMs = 750; // m_calibrationDuration
    int distancePx = 25; // GetCalibrationDistance()
    int maxRaMs = 2500; // m_maxRaDuration
    int maxDecMs = 2500; // m_maxDecDuration
    DecMode decMode = DecMode::Auto; // m_decGuideMode
    bool assumeOrthogonal = false; // m_assumeOrthogonal
    bool fastRecenter = true; // pFrame->pGuider->IsFastRecenterEnabled()
    int maxMovePx = 15; // pFrame->pGuider->GetMaxMovePixels()
};

// Reads that cost the mount a request; the run calls them only where the old code did
class Probe
{
public:
    virtual ~Probe() = default;
    // The mount's RA (hours) and Dec (degrees); false when they are not available
    virtual bool Coordinates(double *raHours, double *decDeg) = 0;
    // The mount's guide rates (degrees/s); false on an error (PHD's GetGuideRates returns true on an error)
    virtual bool GuideRates(double *raDegPerSec, double *decDegPerSec) = 0;
    virtual bool CanPulseGuide() = 0;
};

// One calibration step as the guide log and the Calibrating event report it (CalibrationStepInfo)
struct StepRecord
{
    Leg leg = Leg::West;
    int step = 0;
    double dx = 0.;
    double dy = 0.;
    Pt pos;
    double dist = 0.;
};

// The status message of a step; Scope formats and translates it
enum class Message
{
    WestStep, // "West step %3d, dist=%4.1f" (number, rec.dist)
    EastStep, // "East step %3d, dist=%4.1f"
    BacklashFirst, // "Clearing backlash step 1"
    BacklashStep, // "Clearing backlash step %3d" (number)
    NorthStep, // "North step %3d, dist=%4.1f"
    SouthStep, // "South step %3d, dist=%4.1f"
    NudgeSouth, // "Nudge South %3d" (number)
};

// Which advisory the South check chose; Scope logs it ("Omitted calibration alert: ...")
enum class SouthAdvice
{
    NoSouthCannotPulseGuide, // little or no south movement, and not pulse guiding
    LittleSouthCannotPulseGuide, // little south movement, and not pulse guiding
    LittleSouth, // little south movement, pulse guiding
};

// One side effect, sent to the Sink as it happens
struct Effect
{
    enum Kind
    {
        LogStep, // GuideLog.CalibrationStep(rec)
        Status, // StatusMsg and EvtServer.NotifyCalibrationStep(rec, message text)
        Move, // pFrame->ScheduleAxisMove(dir, ms, MOVEOPTS_CALIBRATION_MOVE)
        DirectComplete, // GuideLog.CalibrationDirectComplete("West" if ra else "North", angle, rate, parity)
        EastAdvisory, // little or no east movement and not pulse guiding: Alert, then Debug "Calibration alert: ..."
        SouthAdvisory, // little south movement: Debug "Omitted calibration alert: ..." (the alert itself is off)
        BacklashDebug, // Debug "Backlash: <the status message just sent>, Last Delta = a, CumDistance = b"
        Debug, // Debug.Write(text)
    } kind = Debug;

    StepRecord rec; // LogStep, Status
    Message message = Message::WestStep; // Status
    int number = 0; // Status: the step number in the message (its dist is rec.dist)

    Dir dir = Dir::North; // Move
    int ms = 0; // Move

    bool ra = true; // DirectComplete
    double angle = 0.; // DirectComplete
    double rate = 0.; // DirectComplete
    Parity parity = Parity::Unknown; // DirectComplete

    SouthAdvice southAdvice = SouthAdvice::LittleSouth; // SouthAdvisory

    double a = 0.; // BacklashDebug: the last clearing move, px
    double b = 0.; // BacklashDebug: the distance cleared so far, px

    std::string text; // Debug
};

class Sink
{
public:
    virtual ~Sink() = default;
    virtual void Emit(const Effect& effect) = 0;
};

// What the calibration measured: the six Calibration fields it sets and the
// step vectors and counts of CalibrationDetails
struct Measured
{
    double xAngle = 0.;
    double xRate = 0.;
    double yAngle = 0.;
    double yRate = 0.;
    Parity raParity = Parity::Unknown;
    Parity decParity = Parity::Unknown;
    std::vector<std::pair<double, double>> raSteps;
    std::vector<std::pair<double, double>> decSteps;
    int raStepCount = 0;
    int decStepCount = 0;
};

enum class Outcome
{
    Continue, // the calibration goes on
    Complete, // the calibration is complete: Scope stores and reports it
    Failed, // the calibration failed: Scope reports fail and clears the calibration
};

struct FrameResult
{
    Outcome outcome = Outcome::Continue;
    Fail fail = Fail::Cleared; // when Failed
    // Complete for the first time since Begin. A run in Complete stays there
    // and reports Complete on every later frame too (Scope's IsCalibrated()
    // can stay false after it, #148); those frames do not carry new results.
    bool firstComplete = false;
};

class CalibrationRun
{
public:
    // Starts a calibration at the lock position, as Scope::BeginCalibration did.
    // declinationRad is the mount's Dec when the calibration starts, nullopt when
    // it is not known; the West and East legs pulse
    // RaCalibrationStepMs(Settings.stepMs, declinationRad) (#141).
    void Begin(const Pt& initialLocation, std::optional<double> declinationRad);
    // The calibration was cleared (Scope::ClearCalibration): the next frame fails with Fail::Cleared
    void Clear();
    // One camera frame: what Scope::UpdateCalibrationState did with the star's position
    FrameResult OnFrame(const Pt& currentLocation, const Settings& s, Probe& probe, Sink& sink);

    State state() const { return m_state; }
    const Measured& measured() const { return m_measured; }

private:
    static void Status(Sink& sink, const StepRecord& info, Message message, int number);
    static void Debug(Sink& sink, const char *format, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;

    State m_state = State::Cleared;
    std::optional<double> m_declinationRad; // the mount's Dec at Begin, radians (#141)
    int m_calibrationSteps = 0;
    int m_recenterRemaining = 0;
    int m_recenterDuration = 0;
    Pt m_calibrationInitialLocation; // initial position of guide star
    Pt m_calibrationStartingLocation; // position of guide star at start of calibration measurement (after clear backlash etc.)
    Pt m_calibrationStartingCoords; // ra,dec coordinates at start of calibration measurement
    Pt m_southStartingLocation; // Needed to be sure nudging is in south-only direction
    Pt m_eastStartingLocation; // For basic sanity check that east moves worked at all
    Pt m_lastLocation;
    double m_totalSouthAmt = 0.;
    double m_northDirCosX = 0.;
    double m_northDirCosY = 0.;
    bool m_eastAlertShown = false;

    // backlash-related variables
    Pt m_blMarkerPoint;
    double m_blExpectedBacklashStep = 0.;
    double m_blLastCumDistance = 0.;
    int m_blAcceptedMoves = 0;
    double m_blDistanceMoved = 0.;
    int m_blMaxClearingPulses = 0;
    enum blConstants
    {
        BL_BACKLASH_MIN_COUNT = 3,
        BL_MAX_CLEARING_TIME = 60000,
        BL_MIN_CLEARING_DISTANCE = 3
    };

    Measured m_measured;
    bool m_completeReported = false;
};

} // namespace calibration_run

#endif // CALIBRATION_RUN_H_INCLUDED
