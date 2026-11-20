#include "sim_mount.h"

#include <algorithm>
#include <cmath>

namespace closed_loop
{

double NormAngle(double rad)
{
    // image_math.h norm(val, -M_PI, M_PI), same arithmetic
    double const range = M_PI - -M_PI;
    double const ofs = rad - -M_PI;
    return rad - floor(ofs / range) * range;
}

double Radians(double deg)
{
    return deg * M_PI / 180.;
}

double Degrees(double rad)
{
    return rad * 180. / M_PI;
}

SimMount::SimMount(const SimMountConfig& cfg)
    : m_cfg(cfg), m_x(cfg.startX), m_y(cfg.startY), m_raHours(cfg.startRaHours), m_decEngagedNorth(!cfg.decEngagedSouth),
      m_rng(cfg.seed)
{
}

double SimMount::Gaussian()
{
    if (m_haveSpare)
    {
        m_haveSpare = false;
        return m_spare;
    }
    // 53-bit uniforms in (0, 1]
    double u1 = (double) ((m_rng() >> 11) + 1) * (1.0 / 9007199254740992.0);
    double u2 = (double) ((m_rng() >> 11) + 1) * (1.0 / 9007199254740992.0);
    double r = std::sqrt(-2.0 * std::log(u1));
    double a = 2.0 * M_PI * u2;
    m_spare = r * std::sin(a);
    m_haveSpare = true;
    return r * std::cos(a);
}

Pt SimMount::Observe()
{
    Pt p;
    p.x = m_x;
    p.y = m_y;
    if (m_cfg.noiseSigmaPx > 0.)
    {
        p.x += m_cfg.noiseSigmaPx * Gaussian();
        p.y += m_cfg.noiseSigmaPx * Gaussian();
    }
    p.valid = true;
    return p;
}

double SimMount::NorthAngleRad() const
{
    double base = m_cfg.decMirrored ? m_cfg.westAngleDeg + 90. : m_cfg.westAngleDeg - 90.;
    return Radians(base + m_cfg.nonOrthoDeg);
}

double SimMount::RaPxPerMs() const
{
    return m_cfg.raPxPerMsAtEquator * std::cos(Radians(m_cfg.trueDecDeg));
}

double SimMount::DecPxPerMs() const
{
    return m_cfg.decPxPerMs;
}

void SimMount::Pulse(Dir dir, int ms)
{
    m_pulses.emplace_back(dir, ms);
    if (ms <= 0)
        return;

    switch (dir)
    {
    case Dir::West:
    case Dir::East:
    {
        double sign = dir == Dir::West ? 1. : -1.;
        double d = sign * RaPxPerMs() * ms;
        double a = Radians(m_cfg.westAngleDeg);
        m_x += d * std::cos(a);
        m_y += d * std::sin(a);
        // true westward motion decreases RA (scope.cpp GO_WEST parity check)
        double dRaHours = m_cfg.raGuideDegPerSec * ms / 1000. / 15.;
        m_raHours += (m_cfg.raParityOdd ? 1. : -1.) * sign * dRaHours;
        break;
    }
    case Dir::North:
    case Dir::South:
    {
        bool north = dir == Dir::North;
        if (north != m_decEngagedNorth)
        {
            m_decEngagedNorth = north;
            m_decSlackMs = m_cfg.decBacklashMs;
        }
        double taken = std::min((double) ms, m_decSlackMs);
        m_decSlackMs -= taken;
        double moving = ms - taken;

        double sign = north ? 1. : -1.;
        double d = sign * m_cfg.decPxPerMs * moving;
        double a = NorthAngleRad();
        m_x += d * std::cos(a);
        m_y += d * std::sin(a);
        // the mount's encoder reports the commanded motion, backlash or not
        double dDecDeg = m_cfg.decGuideDegPerSec * ms / 1000.;
        m_decDeltaDeg += (m_cfg.decParityOdd ? -1. : 1.) * sign * dDecDeg;
        break;
    }
    }
}

void SimMount::Displace(double dx, double dy)
{
    m_x += dx;
    m_y += dy;
}

void SimMount::MoveTo(double x, double y)
{
    m_x = x;
    m_y = y;
}

void SimMount::FlipPier()
{
    m_cfg.westAngleDeg += 180.;
    if (m_cfg.flipMirrorsDec)
    {
        // North keeps its image direction while the sky turns 180 deg, so it now
        // moves the scope south: Dec parity reverses (Mount::FlipCalibration
        // without decFlipRequired). RA parity is unchanged.
        m_cfg.decMirrored = !m_cfg.decMirrored;
        m_cfg.decParityOdd = !m_cfg.decParityOdd;
    }
}

bool SimMount::Coordinates(double *raHours, double *decDeg)
{
    ++probeCalls[(int) ProbeKind::Coordinates];
    m_probeLog.push_back(ProbeKind::Coordinates);
    if (!m_cfg.coordinates)
        return false;
    *raHours = m_raHours;
    double base = m_cfg.reportedDecDeg ? *m_cfg.reportedDecDeg : m_cfg.trueDecDeg;
    *decDeg = base + m_decDeltaDeg;
    return true;
}

bool SimMount::GuideRates(double *raDegPerSec, double *decDegPerSec)
{
    ++probeCalls[(int) ProbeKind::GuideRates];
    m_probeLog.push_back(ProbeKind::GuideRates);
    if (!m_cfg.guideRates)
        return false;
    *raDegPerSec = m_cfg.raGuideDegPerSec;
    *decDegPerSec = m_cfg.decGuideDegPerSec;
    return true;
}

bool SimMount::CanPulseGuide()
{
    ++probeCalls[(int) ProbeKind::CanPulseGuide];
    m_probeLog.push_back(ProbeKind::CanPulseGuide);
    return m_cfg.canPulseGuide;
}

std::optional<double> SimMount::ReportedDecRad() const
{
    if (!m_cfg.reportDec)
        return std::nullopt;
    return Radians(m_cfg.reportedDecDeg ? *m_cfg.reportedDecDeg : m_cfg.trueDecDeg);
}

double SimMount::ExpectedXAngle() const
{
    return NormAngle(Radians(m_cfg.westAngleDeg + 180.));
}

double SimMount::ExpectedYAngle() const
{
    return NormAngle(NorthAngleRad());
}

Pt SimMount::TruePosition() const
{
    Pt p;
    p.x = m_x;
    p.y = m_y;
    p.valid = true;
    return p;
}

} // namespace closed_loop
