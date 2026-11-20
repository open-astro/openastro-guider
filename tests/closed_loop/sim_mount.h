// SimMount: a simulated equatorial mount seen through a guide camera, for
// closed-loop tests of the calibration state machine (#142).
//
// It is not a physics model. It is the smallest model that reproduces what
// Scope's calibration measures: the image direction and rate of a West and a
// North guide pulse, a Dec mirror, non-orthogonal axes, RA motion shrinking
// with cos(Dec), Dec backlash as dead time on each reversal, centroid noise,
// and the RA/Dec coordinates and guide rates the mount reports.
//
// Image conventions (PHD's, see PHD_Point::Angle):
//   - a West pulse moves the star along westAngleDeg in the image, so the
//     calibration's xAngle, start.Angle(cur) = atan2(start - cur), is
//     norm(westAngle + 180);
//   - a North pulse moves the star along westAngle - 90 (decMirrored false)
//     or westAngle + 90 (decMirrored true), plus nonOrthoDeg. The
//     calibration's yAngle, cur.Angle(start), is that direction;
//   - East and South are the opposite directions.
//
// All randomness comes from a seeded mt19937_64 with a local Box-Muller, so a
// seed gives the same frames on every platform (std::normal_distribution is
// implementation-defined).

#ifndef CLOSED_LOOP_SIM_MOUNT_H
#define CLOSED_LOOP_SIM_MOUNT_H

#include <cstdint>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace closed_loop
{

struct Pt
{
    double x = 0.;
    double y = 0.;
    bool valid = false;
};

// Same values as GUIDE_DIRECTION in src/mount.h
enum class Dir
{
    North = 0,
    South = 1,
    East = 2,
    West = 3
};

enum class ProbeKind
{
    Coordinates = 0,
    GuideRates = 1,
    CanPulseGuide = 2
};

// PHD's norm_angle (image_math.h): radians into [-pi, pi)
double NormAngle(double rad);
double Radians(double deg);
double Degrees(double rad);

struct SimMountConfig
{
    double westAngleDeg = 30.; // image direction of the star's motion on a West pulse
    bool decMirrored = false; // false: North moves the star at west - 90 deg; true: west + 90
    double nonOrthoDeg = 0.; // extra rotation of the North axis
    double raPxPerMsAtEquator = 0.003878; // FL400, 3.75 um, bin 1, 0.5x sidereal
    double decPxPerMs = 0.003878;
    double trueDecDeg = 0.; // RA image motion scales with cos(trueDec)
    std::optional<double> reportedDecDeg; // the Dec the mount reports; unset = trueDecDeg
    bool reportDec = true; // false: the mount reports no Dec (UNKNOWN_DECLINATION)
    double decBacklashMs = 0.; // dead time consumed by the first pulse(s) after a Dec reversal
    bool decEngagedSouth = true; // the Dec gear starts engaged for South, so the first North pulse meets the backlash
    double noiseSigmaPx = 0.; // Gaussian centroid noise, per axis
    uint64_t seed = 1;
    bool coordinates = true; // GetCoordinates succeeds
    bool raParityOdd = false; // West increases RA instead of decreasing it
    bool decParityOdd = false; // North decreases Dec instead of increasing it
    bool guideRates = true; // GetGuideRates succeeds
    double raGuideDegPerSec = 0.5 * 15. / 3600.; // 0.5x sidereal
    double decGuideDegPerSec = 0.5 * 15. / 3600.;
    bool canPulseGuide = true;
    double startX = 512.;
    double startY = 384.;
    double startRaHours = 6.;
    bool flipMirrorsDec = true; // a pier flip also mirrors Dec (German equatorial mount)
};

class SimMount
{
public:
    explicit SimMount(const SimMountConfig& cfg);

    // The star centroid on the next frame: true position plus noise
    Pt Observe();

    // A guide pulse, applied at once. RA/Dec coordinates move with it.
    void Pulse(Dir dir, int ms);

    // Moves the star in the image without a pulse (drift, a bump)
    void Displace(double dx, double dy);
    void MoveTo(double x, double y);

    // Meridian flip: the West direction turns 180 deg, Dec mirrors if flipMirrorsDec
    void FlipPier();

    // Probes, in the mount's own terms. Each call is counted and logged.
    bool Coordinates(double *raHours, double *decDeg); // false = unavailable
    bool GuideRates(double *raDegPerSec, double *decDegPerSec); // false = error
    bool CanPulseGuide();

    // The mount's reported Dec in radians, or nullopt when it reports none. Not counted as a probe.
    std::optional<double> ReportedDecRad() const;

    // Ground truth
    double ExpectedXAngle() const; // radians, the calibration's xAngle
    double ExpectedYAngle() const; // radians, the calibration's yAngle
    double RaPxPerMs() const; // image rate of RA at the true Dec
    double DecPxPerMs() const;
    Pt TruePosition() const;
    const std::vector<std::pair<Dir, int>>& Pulses() const { return m_pulses; }
    const std::vector<ProbeKind>& ProbeLog() const { return m_probeLog; }
    const SimMountConfig& Config() const { return m_cfg; }

    int probeCalls[3] = { 0, 0, 0 }; // indexed by ProbeKind

private:
    double NorthAngleRad() const;
    double Gaussian();

    SimMountConfig m_cfg;
    double m_x;
    double m_y;
    double m_raHours;
    double m_decDeltaDeg = 0.;
    bool m_decEngagedNorth;
    double m_decSlackMs = 0.;
    std::mt19937_64 m_rng;
    bool m_haveSpare = false;
    double m_spare = 0.;
    std::vector<std::pair<Dir, int>> m_pulses;
    std::vector<ProbeKind> m_probeLog;
};

} // namespace closed_loop

#endif // CLOSED_LOOP_SIM_MOUNT_H
