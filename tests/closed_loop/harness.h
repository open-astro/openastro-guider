// Closed-loop calibration harness (#142).
//
// RunCalibration drives a calibration implementation frame by frame the way
// Guider::UpdateGuideState does (guider.cpp STATE_CALIBRATING_PRIMARY):
//   - Begin with the lock position (the first observed star position);
//   - on every frame, while the mount is not calibrated, call the
//     implementation with the star's position on that frame. An error ends
//     the run as Failed; the mount reporting itself calibrated ends it as
//     Complete;
//   - every guide pulse the frame scheduled is applied to the SimMount
//     before the next frame is observed, as the worker thread runs the
//     scheduled move before the next exposure.
// A frame that schedules no move is allowed (the DEC_NONE frame).
//
// The implementation (Run) is the frozen legacy twin today
// (legacy/legacy_twin.h); the extracted CalibrationRun (#142 PR-B) gets an
// adapter with the same three calls, so one suite runs against both:
//   void Begin(const Pt& lock, const Settings&);
//   void Clear();                                  // Scope::ClearCalibration
//   FrameReport OnFrame(const Pt& star, const Settings&); // UpdateCalibrationState + IsCalibrated
//   Measured Result() const;
//   const std::vector<Event>& Events() const;

#ifndef CLOSED_LOOP_HARNESS_H
#define CLOSED_LOOP_HARNESS_H

#include "sim_mount.h"

#include <functional>
#include <string>
#include <vector>

namespace closed_loop
{

// Same values as DEC_GUIDE_MODE (scope.h) and GuideParity (mount.h)
enum class DecMode
{
    None = 0,
    Auto,
    North,
    South
};

enum class Parity
{
    Odd = -1,
    Unknown = 0,
    Even = 1
};

enum class Outcome
{
    Continue, // ran out of frames
    Complete,
    Failed
};

enum class Fail
{
    None,
    Cleared,
    RaDidNotMove,
    BacklashClearingFailed,
    DecDidNotMove,
    Other
};

constexpr double RATE_UNCALIBRATED = 123e4; // CALIBRATION_RATE_UNCALIBRATED (scope.h)

// Everything Scope reads from its members and pFrame->pGuider while it calibrates.
// The adapter writes them into the implementation before every frame.
struct Settings
{
    int stepMs = 550; // m_calibrationDuration
    int distancePx = 25; // GetCalibrationDistance()
    int maxRaMs = 2500; // m_maxRaDuration
    int maxDecMs = 2500; // m_maxDecDuration
    DecMode decMode = DecMode::Auto; // m_decGuideMode
    bool assumeOrthogonal = false; // m_assumeOrthogonal
    bool fastRecenter = true; // pGuider->IsFastRecenterEnabled()
    int maxMovePx = 15; // pGuider->GetMaxMovePixels() (the search region)
};

struct Measured
{
    double xAngle = 0., xRate = 0., yAngle = 0., yRate = 0.;
    Parity raParity = Parity::Unknown;
    Parity decParity = Parity::Unknown;
    std::vector<std::pair<double, double>> raSteps, decSteps;
    int raStepCount = 0;
    int decStepCount = 0;
};

// One observable side effect of the calibration, in the order it happened
struct Event
{
    enum Kind
    {
        LogStep, // GuideLog.CalibrationStep: text = direction
        Status, // pFrame->StatusMsg
        NotifyStep, // EvtServer.NotifyCalibrationStep: text = info.msg
        Move, // pFrame->ScheduleAxisMove
        DirectComplete, // GuideLog.CalibrationDirectComplete: text = "West"/"North"
        LogFailed, // GuideLog.CalibrationFailed
        NotifyFailed, // EvtServer.NotifyCalibrationFailed
        Alert, // pFrame->Alert
        Debug, // Debug.Write
        LogComplete, // GuideLog.CalibrationComplete
        NotifyComplete, // EvtServer.NotifyCalibrationComplete
        SetCalibration, // Mount::SetCalibration
        SanityCheck, // Scope::SanityCheckCalibration
    } kind = Debug;
    int frame = -1; // index of the OnFrame call, -1 for Begin/Clear
    std::string text;
    int step = 0;
    double dx = 0., dy = 0., x = 0., y = 0., dist = 0.;
    Dir dir = Dir::North;
    int ms = 0;
    unsigned int moveOptions = 0;
    double angle = 0., rate = 0.;
    int parity = 0;
};

struct FrameReport
{
    bool error = false; // UpdateCalibrationState returned true
    bool calibrated = false; // IsCalibrated() after the frame
};

struct CalOutcome
{
    Outcome outcome = Outcome::Continue;
    Fail fail = Fail::None;
    int frames = 0; // OnFrame calls made
    Measured measured;
    std::vector<Event> events;
    Pt lock;
    Pt finalTrue; // the star's true position when the run ended

    int Count(Event::Kind k) const
    {
        int n = 0;
        for (const Event& e : events)
            n += e.kind == k;
        return n;
    }
    std::vector<Event> Of(Event::Kind k) const
    {
        std::vector<Event> v;
        for (const Event& e : events)
            if (e.kind == k)
                v.push_back(e);
        return v;
    }
    std::vector<int> Moves(Dir d) const
    {
        std::vector<int> v;
        for (const Event& e : events)
            if (e.kind == Event::Move && e.dir == d)
                v.push_back(e.ms);
        return v;
    }
    int MoveTotal(Dir d) const
    {
        int t = 0;
        for (int ms : Moves(d))
            t += ms;
        return t;
    }
    bool HasDebug(const std::string& fragment) const
    {
        for (const Event& e : events)
            if (e.kind == Event::Debug && e.text.find(fragment) != std::string::npos)
                return true;
        return false;
    }
};

// The failure a CalibrationFailed message reports
inline Fail ClassifyFailure(const std::string& msg)
{
    if (msg.rfind("RA Calibration Failed", 0) == 0)
        return Fail::RaDidNotMove;
    if (msg.rfind("Backlash Clearing Failed", 0) == 0)
        return Fail::BacklashClearingFailed;
    if (msg.rfind("DEC Calibration Failed", 0) == 0)
        return Fail::DecDidNotMove;
    if (msg.rfind("Calibration failed: the calibration was cleared", 0) == 0)
        return Fail::Cleared;
    return Fail::Other;
}

// Called before each frame; may change the settings, the sim or the run (Clear, a mode change)
template<class Run>
using FrameHook = std::function<void(int frame, Settings& settings, SimMount& sim, Run& run)>;

template<class Run>
CalOutcome RunCalibration(SimMount& sim, Settings settings, int maxFrames = 2000, const FrameHook<Run>& hook = nullptr)
{
    Run run(sim);
    CalOutcome out;
    out.lock = sim.Observe();
    run.Begin(out.lock, settings);

    for (int frame = 0; frame < maxFrames; frame++)
    {
        if (hook)
            hook(frame, settings, sim, run);

        size_t before = run.Events().size();
        FrameReport r = run.OnFrame(sim.Observe(), settings);
        out.frames = frame + 1;

        const std::vector<Event>& ev = run.Events();
        for (size_t i = before; i < ev.size(); i++)
            if (ev[i].kind == Event::Move)
                sim.Pulse(ev[i].dir, ev[i].ms);

        if (r.error)
        {
            out.outcome = Outcome::Failed;
            out.fail = Fail::Other;
            for (size_t i = before; i < ev.size(); i++)
                if (ev[i].kind == Event::LogFailed)
                    out.fail = ClassifyFailure(ev[i].text);
            break;
        }
        if (r.calibrated)
        {
            out.outcome = Outcome::Complete;
            break;
        }
    }

    out.measured = run.Result();
    out.events = run.Events();
    out.finalTrue = sim.TruePosition();
    return out;
}

} // namespace closed_loop

#endif // CLOSED_LOOP_HARNESS_H
