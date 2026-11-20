// ExtractedRun: the harness's view of calibration_run::CalibrationRun as
// Scope drives it (#142).
//
// It plays Scope's part around the extracted run the way src/scope.cpp does
// after the extraction, and records the same Events the legacy twin's
// recorders do:
//   - Begin is Scope::BeginCalibration: clear the calibration, read the
//     mount's Dec once and write the RA step it gives to Debug (#141), Begin
//     the run with that Dec;
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
    // readDeclination false: the Dec read at Begin fails (UNKNOWN_DECLINATION), so
    // the run keeps the step for every leg, as the code before #141 did. The Dec
    // the completion stores is still the mount's.
    explicit ExtractedRun(SimMount& sim, bool readDeclination = true) : m_sim(sim), m_readDeclination(readDeclination) { }
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
    bool m_readDeclination;
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

// The two records of the Dec read at Begin (#141): the read and the Debug line
// with the RA step it gave. The legacy twin has no counterpart for them.
inline bool IsBeginDecRead(const Event& e)
{
    return e.frame == -1 &&
        ((e.kind == Event::Probe && e.text == "Declination") ||
         (e.kind == Event::Debug && e.text.rfind("RA calibration step ", 0) == 0));
}

} // namespace closed_loop

#endif // CLOSED_LOOP_EXTRACTED_RUN_H
