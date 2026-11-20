// ExtractedRun: the harness's view of calibration_run::CalibrationRun as
// Scope drives it (#142).
//
// It plays Scope's part around the extracted run the way src/scope.cpp does
// after the extraction, and records the same Events the legacy twin's
// recorders do:
//   - Begin is Scope::BeginCalibration: clear the calibration, Begin the run;
//   - OnFrame is Scope::UpdateCalibrationState: build the Settings, run one
//     frame with the SimMount as the Probe, replay each Effect as the call
//     Scope makes for it (guide-log row, status message and Calibrating
//     event, scheduled move, Debug line, alert), report a failure with
//     Scope's messages and clear the calibration, or store and report a
//     completed calibration; then the IsCalibrated() test the guider makes;
//   - Clear is Scope::ClearCalibration.
// The English texts are Scope's; the app translates them, the tests do not.

#ifndef CLOSED_LOOP_EXTRACTED_RUN_H
#define CLOSED_LOOP_EXTRACTED_RUN_H

#include "harness.h"

#include "calibration_run.h"

namespace closed_loop
{

class ExtractedRun
{
public:
    explicit ExtractedRun(SimMount& sim) : m_sim(sim) { }
    ExtractedRun(const ExtractedRun&) = delete;
    ExtractedRun& operator=(const ExtractedRun&) = delete;

    void Begin(const Pt& lock, const Settings& settings);
    void Clear();
    FrameReport OnFrame(const Pt& star, const Settings& settings);
    Measured Result() const;
    const std::vector<Event>& Events() const { return m_events; }

    const calibration_run::CalibrationRun& Run() const { return m_run; }

    // The six Calibration fields Scope (m_calibration) and the mount hold
    struct Cal
    {
        double xAngle = 0., xRate = 0., yAngle = 0., yRate = 0.;
        int raParity = 0, decParity = 0;
        double declination = 997.0; // UNKNOWN_DECLINATION
    };

private:
    Event& Add(Event::Kind kind, const std::string& text = std::string());
    void Report(const calibration_run::Effect& e);
    void ClearCalibration();
    bool IsCalibrated() const;

    SimMount& m_sim;
    calibration_run::CalibrationRun m_run;
    std::vector<Event> m_events;
    int m_frame = -1; // the frame being recorded, -1 for Begin/Clear
    int m_frames = 0;
    DecMode m_decMode = DecMode::Auto;
    std::string m_lastStatus;

    // Mount and Scope state the completion and IsCalibrated() use
    bool m_calibrated = false; // Mount::m_calibrated
    Cal m_calibration; // Scope::m_calibration
};

} // namespace closed_loop

#endif // CLOSED_LOOP_EXTRACTED_RUN_H
