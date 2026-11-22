/*
 *  frame_reject.h
 *  PHD Guiding
 *
 *  The two checks GuiderMultiStar::UpdateCurrentPosition drops a guide frame on
 *  after the primary star was found (#142): MassChecker (the star's mass moved
 *  too far from the median of the last frames: a cloud, or a jump to another
 *  star or a hot pixel) and DistanceChecker (the star is much farther from the
 *  lock position than the recent guide error: a jump, with a 5 s wait and a
 *  recovery). Moved here from guider_multistar.cpp unchanged; the time comes
 *  from the caller (monotonic_timers::ClockMs in production) and the guider's
 *  state is passed in, so they are tested on an injected clock. Kept free of wx
 *  so it can be unit tested (tests/guiding/test_frame_reject.cpp).
 */

#ifndef FRAME_REJECT_H_INCLUDED
#define FRAME_REJECT_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace frame_reject
{

// Where the checks' Debug lines go (each ends in a newline); none when empty
using LogFn = std::function<void(const std::string&)>;

// The star mass over a sliding time window. CheckMass rejects a mass below the low-water mark
// times (1 - threshold), above the high-water mark times (1 + threshold), or above the median
// times (1 + 2 threshold). With auto exposure the masses are divided by the exposure.
class MassChecker
{
public:
    enum
    {
        DefaultTimeWindowMs = 22500
    };

    MassChecker();

    // The window kept is twice this: an abrupt change in mass moves the median after about
    // milliseconds
    void SetTimeWindow(unsigned int milliseconds);
    // A change of auto/manual resets the data, so does a new exposure in manual mode
    void SetExposure(int exposure, bool isAutoExp);
    double AdjustedMass(double mass) const;
    // Drops the entries older than the window before nowMs, then adds mass at nowMs
    void AppendData(double mass, int64_t nowMs);
    // False with fewer than 5 entries. limits gets {low limit, median, high limit, median spike
    // limit}, in mass units (multiplied back by the exposure) when rejecting with auto exposure
    bool CheckMass(double mass, double threshold, double limits[4]);
    void Reset();

    // For the tests
    size_t Count() const { return m_data.size(); }
    double HighMass() const { return m_highMass; }
    double LowMass() const { return m_lowMass; }

private:
    struct Entry
    {
        int64_t time;
        double mass;
    };

    std::deque<Entry> m_data;
    double m_highMass; // high-water mark
    double m_lowMass; // low-water mark
    unsigned long m_timeWindow;
    std::vector<double> m_tmp;
    int m_exposure;
    bool m_isAutoExposure;
};

// What DistanceChecker reads from the guider for a frame
struct GuiderStatus
{
    bool guiding = false; // Guider::IsGuiding
    bool paused = false; // Guider::IsPaused
    bool settling = false; // PhdController::IsSettling
    unsigned int frameCount = 0; // Guider::CurrentErrorFrameCount
    double avgDist = 0.; // Guider::CurrentErrorSmoothed(raOnly)
};

// Rejects a frame whose distance from the lock position is over tolerance times the smoothed
// guide error, then waits up to WAIT_INTERVAL_MS for a small one, rejecting the large ones, and
// then accepts them (recovering) until a small one. Activate (a lost star or a mass change)
// starts the wait and forces the tolerance to 2.
class DistanceChecker
{
public:
    enum State
    {
        ST_GUIDING,
        ST_WAITING,
        ST_RECOVERING,
    };

    enum
    {
        WAIT_INTERVAL_MS = 5000,
        MIN_FRAMES_FOR_STATS = 10
    };

    explicit DistanceChecker(LogFn log = LogFn());

    void Activate(int64_t nowMs);
    // True to keep the frame
    bool CheckDistance(double distance, double tolerance, const GuiderStatus& guider, int64_t nowMs);

    State GetState() const { return m_state; }
    double ForceTolerance() const { return m_forceTolerance; }

private:
    bool SmallOffset(double distance, double tolerance, const GuiderStatus& guider) const;
    void Log(const std::string& line) const;

    LogFn m_log;
    State m_state;
    int64_t m_expires = 0;
    double m_forceTolerance = 0.;
};

} // namespace frame_reject

#endif // FRAME_REJECT_H_INCLUDED
