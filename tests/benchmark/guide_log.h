// A reader for PHD2 guide logs (PHD2_GuideLog_*.txt, log version 2.5), wx-free (#142 Tier 4).
//
// It reads the guiding sessions: each "Guiding Begins" header (the calibration in use, the guide
// algorithms and their parameters as GetSettingsSummary() prints them, the pulse limits and the Dec
// guide mode, the pixel scale and the exposure), then the frame rows GuidingLog::GuideStep writes and
// the INFO lines that change what the guide algorithms do: a dither (Mount::NotifyGuidingDithered
// resets them) and guiding enabled or disabled (GuidingEnabled resets them). Calibration sections are
// skipped. Numbers keep the log's precision: %.3f for the offsets and distances, %.3f px/s for the
// rates, %.1f deg for the angles.

#ifndef GUIDE_LOG_H_INCLUDED
#define GUIDE_LOG_H_INCLUDED

#include <istream>
#include <string>
#include <vector>

namespace guide_log
{

// One axis's algorithm as the header names it (Mount::GuideAlgorithmName) with the parameters its
// GetSettingsSummary() prints; a parameter the summary does not print stays NaN
struct Algorithm
{
    std::string name; // "Hysteresis", "Resist Switch", "Lowpass", "Lowpass2", "ZFilter", "Predictive PEC", "None"
    double minMove;
    double hysteresis; // Hysteresis
    double aggression; // Hysteresis (0..2) and Resist Switch (the log's percent / 100)
    bool fastSwitch = true; // Resist Switch
    double slopeWeight; // Lowpass
    double aggressiveness; // Lowpass2 (percent, as the algorithm keeps it)

    Algorithm();
};

// A frame row: GuidingLog::GuideStep ("Mount") or GuidingLog::FrameDropped ("DROP")
struct Frame
{
    int line = 0; // in the log, from 1
    int frame = 0;
    double time = 0.; // s since guiding started
    bool dropped = false;
    double dx = 0., dy = 0.; // camera offset from the lock, px
    double raRaw = 0., decRaw = 0.; // mount offset, px: what went into the algorithms
    double raGuide = 0., decGuide = 0.; // what came out of them, px
    int raMs = 0, decMs = 0; // the pulses sent (MoveResultInfo::amountMoved)
    char raDir = 0, decDir = 0; // 'E', 'W', 'N', 'S', or 0 with no pulse
    double snr = 0.;
    int errorCode = 0;
};

struct Event
{
    enum Kind
    {
        FrameRow,
        Dither, // INFO: DITHER by ...: the algorithms are reset
        GuidingEnabled, // MountGuidingEnabled = true: the algorithms are reset
        GuidingDisabled, // MountGuidingEnabled = false: no pulses until enabled
        SettleBegin, // INFO: SETTLING STATE CHANGE, Settling started
        SettleEnd, // Settling complete or failed
    };
    Kind kind = FrameRow;
    Frame frame; // FrameRow only
    double ditherRa = 0., ditherDec = 0.; // Dither only: the lock's move, px in mount axes
    int line = 0;
};

struct Session
{
    int line = 0; // of "Guiding Begins"
    std::string begins; // the date and time after "Guiding Begins at "
    bool mount = true; // false for an AO session ("AO = ..."), which the replay skips
    bool guidingEnabled = true; // the header's "guiding enabled"
    double pixelScale = 0.; // arcsec/px
    double exposureMs = 0.;
    double xAngle = 0., yAngle = 0.; // deg
    double xRate = 0., yRate = 0.; // px/s: the RA rate the Dec in use gives, and the Dec rate
    Algorithm x, y;
    bool backlashComp = false;
    int backlashPulseMs = 0;
    int maxRaMs = 0, maxDecMs = 0;
    std::string decGuideMode; // "Auto", "North", "South", "Off"
    double dec = 0.; // deg, the "RA = ..., Dec = ..." line
    std::vector<Event> events;

    int FrameCount() const;
};

// Reads every guiding session of a guide log. A malformed frame row is an error (with its line in
// *error); unknown INFO lines and sections are skipped.
bool Parse(std::istream& in, std::vector<Session> *sessions, std::string *error);
bool ParseFile(const std::string& path, std::vector<Session> *sessions, std::string *error);

} // namespace guide_log

#endif // GUIDE_LOG_H_INCLUDED
