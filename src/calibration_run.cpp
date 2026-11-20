/*
 *  calibration_run.cpp
 *  PHD Guiding
 *
 *  The mount calibration state machine, moved out of
 *  Scope::UpdateCalibrationState (#142). The statements are the old ones in
 *  the old order, with the Scope members renamed into this class, the
 *  m_calibration fields into Measured, and each wx/app call turned into an
 *  Effect, a Probe call or a Fail.
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

#include "calibration_run.h"

#include "calibration_transform.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace calibration_run
{

namespace
{
    // PHD_Point (point.h): a.dX(b), a.dY(b), a.Distance(b), a.Angle(b)
    double DX(const Pt& a, const Pt& b)
    {
        return a.x - b.x;
    }

    double DY(const Pt& a, const Pt& b)
    {
        return a.y - b.y;
    }

    double Distance(const Pt& a, const Pt& b)
    {
        return std::hypot(DX(a, b), DY(a, b));
    }

    double Angle(const Pt& a, const Pt& b)
    {
        double dX = DX(a, b);
        double dY = DY(a, b);
        double dRet = 0.0;
        if (dX != 0 || dY != 0)
            dRet = std::atan2(dY, dX);
        return dRet;
    }

    // image_math.h
    double NormAngle(double val)
    {
        double const start = -M_PI;
        double const end = M_PI;
        double const range = end - start;
        double const ofs = val - start;
        return val - std::floor(ofs / range) * range;
    }

    double Degrees(double radians)
    {
        return radians * 180. / M_PI;
    }

    double Radians(double degrees)
    {
        return degrees * M_PI / 180.;
    }

    // phd.h
    int DivRoundUp(int x, int y)
    {
        return (x + y - 1) / y;
    }

    // Convert camera coords to mount coords (Scope's MountCoords): the Dec component
    double MountY(const Pt& cameraVector, double xCalibAngle, double yCalibAngle)
    {
        double yAngleError = NormAngle((xCalibAngle - yCalibAngle) + M_PI / 2);
        return calibration_transform::CameraToMount(cameraVector.x, cameraVector.y, xCalibAngle, yAngleError).mount.y;
    }

    Pt Minus(const Pt& a, const Pt& b)
    {
        return Pt::At(a.x - b.x, a.y - b.y);
    }

    // Scope's GetRADecCoordinates: an unavailable reading invalidates the point
    void GetRADecCoordinates(Probe& probe, Pt *coords)
    {
        double ra;
        double dec;
        if (probe.Coordinates(&ra, &dec))
            *coords = Pt::At(ra, dec);
        else
            coords->valid = false;
    }

    std::string VFormat(const char *format, va_list args)
    {
        va_list copy;
        va_copy(copy, args);
        int n = std::vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        if (n <= 0)
            return std::string();
        std::string buf((size_t) n + 1, '\0');
        std::vsnprintf(buf.data(), buf.size(), format, args);
        buf.resize((size_t) n);
        return buf;
    }

    void LogStep(Sink& sink, const StepRecord& info)
    {
        Effect e;
        e.kind = Effect::LogStep;
        e.rec = info;
        sink.Emit(e);
    }

    void Move(Sink& sink, Dir dir, int ms)
    {
        Effect e;
        e.kind = Effect::Move;
        e.dir = dir;
        e.ms = ms;
        sink.Emit(e);
    }

    void DirectComplete(Sink& sink, bool ra, double angle, double rate, Parity parity)
    {
        Effect e;
        e.kind = Effect::DirectComplete;
        e.ra = ra;
        e.angle = angle;
        e.rate = rate;
        e.parity = parity;
        sink.Emit(e);
    }

    FrameResult Failed(Fail fail)
    {
        FrameResult r;
        r.outcome = Outcome::Failed;
        r.fail = fail;
        return r;
    }
} // namespace

void CalibrationRun::Status(Sink& sink, const StepRecord& info, Message message, int number)
{
    Effect e;
    e.kind = Effect::Status;
    e.rec = info;
    e.message = message;
    e.number = number;
    sink.Emit(e);
}

void CalibrationRun::Debug(Sink& sink, const char *format, ...)
{
    Effect e;
    e.kind = Effect::Debug;
    va_list args;
    va_start(args, format);
    e.text = VFormat(format, args);
    va_end(args);
    sink.Emit(e);
}

// #141. The step is sized for the equator (the calibration step calculator,
// or the user). At Dec d a pulse moves the star cos(d) as far in RA, so the
// West leg needs 1/cos(d) as many pulses: 46 at 75 deg for the 12 a 550 ms
// step takes at the equator, and over the 61 it may send above about 79 deg.
// Growing the RA pulse by 1/cos(d) keeps the leg near its equator count;
// the total pulse time, and so the time the leg takes, stays the same.
//
// The cap. CalstepDialog::GetCalibrationStepSize limits its own Dec
// adjustment to 2x (MaxPulse keeps MIN_STEPS steps), because there one step
// serves both axes and a longer one would cut the Dec leg below MIN_STEPS.
// Only the RA leg scales here, so that reason does not apply; the cap only
// bounds the harm of a wrong Dec reading. At 4x a mount that reports 85 deg
// while it points at the equator moves the star 4 * 2.13 = 8.5 px a pulse on
// the audit profile, under the default 15 px search region, and the
// calibration still completes in 3 steps (with the too-few-steps advisory).
// A 2x cap would still fail at 85 deg (68 pulses); 4x calibrates to about
// 87 deg.
//
// RA_STEP_MAX_MS keeps a grown pulse under the 30 s HTTP timeout AlpacaClient
// gives a PulseGuide that blocks while it runs: a step over 6250 ms would
// otherwise reach it near the pole. A step already longer is left as it was.
int RaCalibrationStepMs(int stepMs, std::optional<double> declinationRad)
{
    if (!declinationRad || !std::isfinite(*declinationRad))
        return stepMs;
    double c = std::max(std::fabs(std::cos(*declinationRad)), 1.0 / RA_STEP_MAX_SCALE);
    double scaled = std::min(std::ceil(stepMs / c), (double) std::max(stepMs, RA_STEP_MAX_MS));
    return std::max(stepMs, (int) scaled);
}

// #154. Two unit vectors exactly antiparallel have a cosine of -1, but the
// rounded direction cosines and products can put it a hair past -1 (or past
// +1 for parallel ones), where acos gives NaN and the nudge would stop as
// "wrong direction" when it is the right one. Clamped, the angle is pi (or
// 0). std::clamp passes a NaN through (both comparisons are false), so the
// star exactly on the lock (0/0) still gives NaN and ends the nudge there.
double NudgeAngle(double nudgeDirCosX, double nudgeDirCosY, double northDirCosX, double northDirCosY)
{
    double cos_theta = nudgeDirCosX * northDirCosX + nudgeDirCosY * northDirCosY;
    return std::acos(std::clamp(cos_theta, -1.0, 1.0));
}

void CalibrationRun::Begin(const Pt& initialLocation, std::optional<double> declinationRad)
{
    m_declinationRad = declinationRad;
    m_calibrationSteps = 0;
    m_calibrationInitialLocation = initialLocation;
    m_calibrationStartingLocation.valid = false;
    m_calibrationStartingCoords.valid = false;
    m_state = State::GoWest;
    m_measured = Measured();
    m_eastAlertShown = false;
    m_completeReported = false;
}

void CalibrationRun::Clear()
{
    m_state = State::Cleared;
}

FrameResult CalibrationRun::OnFrame(const Pt& currentLocation, const Settings& s, Probe& probe, Sink& sink)
{
    FrameResult result;

    if (!m_calibrationStartingLocation.valid)
    {
        m_calibrationStartingLocation = currentLocation;
        GetRADecCoordinates(probe, &m_calibrationStartingCoords);

        char coords[64] = "N/A";
        if (m_calibrationStartingCoords.valid)
            std::snprintf(coords, sizeof(coords), "%.2f,%.1f", m_calibrationStartingCoords.x, m_calibrationStartingCoords.y);
        Debug(sink, "Scope::UpdateCalibrationState: starting location = %.2f,%.2f coords = %s\n", currentLocation.x,
              currentLocation.y, coords);
    }

    double dX = DX(m_calibrationStartingLocation, currentLocation);
    double dY = DY(m_calibrationStartingLocation, currentLocation);
    double dist = Distance(m_calibrationStartingLocation, currentLocation);
    double dist_crit = s.distancePx;
    // the West and East legs pulse for the Dec (#141); the Dec leg, backlash clearing and nudge use s.stepMs
    int raStepMs = RaCalibrationStepMs(s.stepMs, m_declinationRad);
    double blDelta;
    double blCumDelta;
    double nudge_amt;
    double nudgeDirCosX;
    double nudgeDirCosY;
    double theta;
    double southDistMoved;
    double northDistMoved;
    double southAngle;

    switch (m_state)
    {
    case State::Cleared:
        // #137: the calibration was cleared while it ran (the legacy socket server's CLEARCAL still can);
        // end it as failed instead of aborting, or staying in a state that has no next step
        return Failed(Fail::Cleared);

    case State::GoWest:
    {
        // step number in the log is the step that just finished
        StepRecord info { Leg::West, m_calibrationSteps, dX, dY, currentLocation, dist };
        LogStep(sink, info);
        m_measured.raSteps.emplace_back(dX, dY);

        if (dist < dist_crit)
        {
            if (m_calibrationSteps++ > MAX_CALIBRATION_STEPS)
                return Failed(Fail::RaDidNotMove);
            Status(sink, info, Message::WestStep, m_calibrationSteps);
            Move(sink, Dir::West, raStepMs);
            return result;
        }

        // West calibration complete

        m_measured.xAngle = Angle(m_calibrationStartingLocation, currentLocation);
        m_measured.xRate = dist / (m_calibrationSteps * raStepMs);

        m_measured.raParity = Parity::Unknown;
        if (m_calibrationStartingCoords.valid)
        {
            Pt endingCoords;
            GetRADecCoordinates(probe, &endingCoords);
            if (endingCoords.valid)
            {
                // true westward motion decreases RA
                double ONE_ARCSEC = 24.0 / (360. * 60. * 60.); // hours
                double dra = endingCoords.x - m_calibrationStartingCoords.x;
                if (dra < -ONE_ARCSEC)
                    m_measured.raParity = Parity::Even;
                else if (dra > ONE_ARCSEC)
                    m_measured.raParity = Parity::Odd;
            }
        }

        Debug(sink, "WEST calibration completes with steps=%d angle=%.1f rate=%.3f parity=%d\n", m_calibrationSteps,
              Degrees(m_measured.xAngle), m_measured.xRate * 1000.0, static_cast<int>(m_measured.raParity));

        m_measured.raStepCount = m_calibrationSteps;
        DirectComplete(sink, true, m_measured.xAngle, m_measured.xRate, m_measured.raParity);

        // for GO_EAST m_recenterRemaining contains the total remaining duration.
        // Choose the largest pulse size that will not lose the guide star or exceed
        // the user-specified max pulse

        m_recenterRemaining = m_calibrationSteps * raStepMs;

        if (s.fastRecenter)
        {
            m_recenterDuration = (int) std::floor((double) s.maxMovePx / m_measured.xRate);
            if (m_recenterDuration > s.maxRaMs)
                m_recenterDuration = s.maxRaMs;
            if (m_recenterDuration < raStepMs)
                m_recenterDuration = raStepMs;
        }
        else
            m_recenterDuration = raStepMs;

        m_calibrationSteps = DivRoundUp(m_recenterRemaining, m_recenterDuration);
        m_state = State::GoEast;
        m_eastStartingLocation = currentLocation;

        // fall through
        Debug(sink, "Falling Through to state GO_EAST\n");
    }
        [[fallthrough]];

    case State::GoEast:
    {
        StepRecord info { Leg::East, m_calibrationSteps, dX, dY, currentLocation, dist };
        LogStep(sink, info);
        m_measured.raSteps.emplace_back(dX, dY);

        if (m_recenterRemaining > 0)
        {
            int duration = m_recenterDuration;
            if (duration > m_recenterRemaining)
                duration = m_recenterRemaining;

            Status(sink, info, Message::EastStep, m_calibrationSteps);

            m_recenterRemaining -= duration;
            --m_calibrationSteps;
            m_lastLocation = currentLocation;

            Move(sink, Dir::East, duration);
            return result;
        }

        // If not pulse-guiding check for obvious guide cable problem and no useful east moves
        if (!probe.CanPulseGuide())
        {
            double eastDistMoved = Distance(m_eastStartingLocation, currentLocation);
            double westDistMoved = Distance(m_calibrationStartingLocation, m_eastStartingLocation);
            double eastAngle = Angle(m_eastStartingLocation, currentLocation);
            // Want a significant east movement that re-traces the west vector to within 30 degrees
            if (std::fabs(eastDistMoved) < 0.25 * westDistMoved ||
                std::fabs(NormAngle(eastAngle - (m_measured.xAngle + M_PI))) > Radians(30))
            {
                Effect e;
                e.kind = Effect::EastAdvisory;
                sink.Emit(e);
                m_eastAlertShown = true;
            }
        }
        // setup for clear backlash

        m_calibrationSteps = 0;
        dist = dX = dY = 0.0;
        m_calibrationStartingLocation = currentLocation;

        if (s.decMode == DecMode::None)
        {
            Debug(sink, "Skipping Dec calibration as DecGuideMode == NONE\n");
            m_state = State::Complete;
            m_measured.yAngle = NormAngle(m_measured.xAngle + M_PI / 2.); // choose an arbitrary angle perpendicular to xAngle
            // indicate lack of Dec calibration data, see Scope::IsCalibrated.
            m_measured.yRate = RATE_UNCALIBRATED;
            m_measured.decParity = Parity::Unknown;
            return result;
        }

        m_state = State::ClearBacklash;
        m_blMarkerPoint = currentLocation;
        GetRADecCoordinates(probe, &m_calibrationStartingCoords);
        m_blExpectedBacklashStep = m_measured.xRate * s.stepMs * 0.6;

        double RASpeed = 0.;
        double DecSpeed = 0.;
        if (probe.GuideRates(&RASpeed, &DecSpeed) && RASpeed != 0.0 && RASpeed != DecSpeed)
        {
            m_blExpectedBacklashStep *= DecSpeed / RASpeed;
        }

        m_blMaxClearingPulses = 8 > BL_MAX_CLEARING_TIME / s.stepMs ? 8 : BL_MAX_CLEARING_TIME / s.stepMs;
        m_blLastCumDistance = 0;
        m_blAcceptedMoves = 0;
        Debug(sink, "Backlash: Looking for 3 moves of %0.1f px, max attempts = %d\n", m_blExpectedBacklashStep,
              m_blMaxClearingPulses);
        // fall through
        Debug(sink, "Falling Through to state CLEAR_BACKLASH\n");
    }
        [[fallthrough]];

    case State::ClearBacklash:
    {
        StepRecord info { Leg::Backlash, m_calibrationSteps, dX, dY, currentLocation, dist };
        LogStep(sink, info);
        blDelta = Distance(m_blMarkerPoint, currentLocation);
        blCumDelta = dist;

        // Want to see the mount moving north for 3 moves of >= expected distance pixels without any direction reversals
        if (m_calibrationSteps == 0)
        {
            // Get things moving with the first clearing pulse
            Debug(sink, "Backlash: Starting north clearing using pulse width of %d\n", s.stepMs);
            Move(sink, Dir::North, s.stepMs);
            m_calibrationSteps = 1;
            Status(sink, info, Message::BacklashFirst, 1);
            return result;
        }

        if (blDelta >= m_blExpectedBacklashStep)
        {
            if (m_blAcceptedMoves == 0 || (blCumDelta > m_blLastCumDistance)) // Just starting or still moving in same direction
            {
                m_blAcceptedMoves++;
                Debug(sink, "Backlash: Accepted clearing move of %0.1f\n", blDelta);
            }
            else
            {
                m_blAcceptedMoves = 0; // Reset on a direction reversal
                Debug(sink, "Backlash: Rejected clearing move of %0.1f, direction reversal\n", blDelta);
            }
        }
        else
        {
            if (blCumDelta < m_blLastCumDistance)
            {
                m_blAcceptedMoves = 0;
                Debug(sink, "Backlash: Rejected small direction reversal of %0.1f px\n", blDelta);
            }
            else
                Debug(sink, "Backlash: Rejected small move of %0.1f px\n", blDelta);
        }

        if (m_blAcceptedMoves < BL_BACKLASH_MIN_COUNT) // More work to do
        {
            if (m_calibrationSteps < m_blMaxClearingPulses && blCumDelta < dist_crit)
            {
                // Still have attempts left, haven't moved the star by 25 px yet
                Move(sink, Dir::North, s.stepMs);
                m_calibrationSteps++;
                m_blMarkerPoint = currentLocation;
                GetRADecCoordinates(probe, &m_calibrationStartingCoords);
                m_blLastCumDistance = blCumDelta;
                Status(sink, info, Message::BacklashStep, m_calibrationSteps);
                Effect e;
                e.kind = Effect::BacklashDebug;
                e.a = blDelta;
                e.b = blCumDelta;
                sink.Emit(e);
                return result;
            }
            else
            {
                // Used up all our attempts - might be ok or not
                if (blCumDelta >= (double) BL_MIN_CLEARING_DISTANCE)
                {
                    // Exhausted all the clearing pulses without reaching the goal - but we did move the mount > 3 px (same
                    // as PHD1)
                    m_calibrationSteps = 0;
                    m_calibrationStartingLocation = currentLocation;
                    dX = 0;
                    dY = 0;
                    dist = 0;
                    Debug(sink,
                          "Backlash: Reached clearing limit but total displacement > 3px - proceeding with calibration\n");
                }
                else
                    return Failed(Fail::BacklashClearingFailed);
            }
        }
        else // Got our 3 moves, move ahead
        {
            // We know the last backlash clearing move was big enough - include that as a north calibration move

            // log the starting point
            StepRecord northInfo { Leg::North, 0, 0.0, 0.0, m_blMarkerPoint, 0.0 };
            LogStep(sink, northInfo);
            m_measured.decSteps.emplace_back(0.0, 0.0);

            m_calibrationSteps = 1;
            m_calibrationStartingLocation = m_blMarkerPoint;
            dX = DX(m_blMarkerPoint, currentLocation);
            dY = DY(m_blMarkerPoint, currentLocation);
            dist = Distance(m_blMarkerPoint, currentLocation);
            Debug(sink, "Backlash: Got 3 acceptable moves, using last move as step 1 of N calibration\n");
        }

        m_blDistanceMoved = Distance(m_blMarkerPoint, m_calibrationInitialLocation); // Need this to set nudging limit

        Debug(sink, "Backlash: North calibration moves starting at {%0.1f,%0.1f}, Offset = %0.1f px\n", m_blMarkerPoint.x,
              m_blMarkerPoint.y, m_blDistanceMoved);
        Debug(sink, "Backlash: Total distance moved = %0.1f\n", Distance(currentLocation, m_calibrationInitialLocation));

        m_state = State::GoNorth;
        // falling through to start moving north
        Debug(sink, "Backlash: Falling Through to state GO_NORTH\n");
    }
        [[fallthrough]];

    case State::GoNorth:
    {
        StepRecord info { Leg::North, m_calibrationSteps, dX, dY, currentLocation, dist };
        LogStep(sink, info);
        m_measured.decSteps.emplace_back(dX, dY);

        if (dist < dist_crit)
        {
            if (m_calibrationSteps++ > MAX_CALIBRATION_STEPS)
                return Failed(Fail::DecDidNotMove);
            Status(sink, info, Message::NorthStep, m_calibrationSteps);
            Move(sink, Dir::North, s.stepMs);
            return result;
        }

        // note: this calculation is reversed from the ra calculation, because
        // that one was calibrating WEST, but the angle is really relative
        // to EAST
        if (s.assumeOrthogonal)
        {
            double a1 = NormAngle(m_measured.xAngle + M_PI / 2.);
            double a2 = NormAngle(m_measured.xAngle - M_PI / 2.);
            double yAngle = Angle(currentLocation, m_calibrationStartingLocation);
            m_measured.yAngle = std::fabs(NormAngle(a1 - yAngle)) < std::fabs(NormAngle(a2 - yAngle)) ? a1 : a2;
            double dec_dist = dist * std::cos(yAngle - m_measured.yAngle);
            m_measured.yRate = dec_dist / (m_calibrationSteps * s.stepMs);

            Debug(sink,
                  "Assuming orthogonal axes: measured Y angle = %.1f, X angle = %.1f, orthogonal = "
                  "%.1f, %.1f, best = %.1f, dist = %.2f, dec_dist = %.2f\n",
                  Degrees(yAngle), Degrees(m_measured.xAngle), Degrees(a1), Degrees(a2), Degrees(m_measured.yAngle), dist,
                  dec_dist);
        }
        else
        {
            m_measured.yAngle = Angle(currentLocation, m_calibrationStartingLocation);
            m_measured.yRate = dist / (m_calibrationSteps * s.stepMs);
        }

        m_measured.decStepCount = m_calibrationSteps;

        m_measured.decParity = Parity::Unknown;
        if (m_calibrationStartingCoords.valid)
        {
            Pt endingCoords;
            GetRADecCoordinates(probe, &endingCoords);
            if (endingCoords.valid)
            {
                // real Northward motion increases Dec
                double ONE_ARCSEC = 1.0 / (60. * 60.); // degrees
                double ddec = endingCoords.y - m_calibrationStartingCoords.y;
                if (ddec > ONE_ARCSEC)
                    m_measured.decParity = Parity::Even;
                else if (ddec < -ONE_ARCSEC)
                    m_measured.decParity = Parity::Odd;
            }
        }

        Debug(sink, "NORTH calibration completes with angle=%.1f rate=%.3f parity=%d\n", Degrees(m_measured.yAngle),
              m_measured.yRate * 1000.0, static_cast<int>(m_measured.decParity));

        DirectComplete(sink, false, m_measured.yAngle, m_measured.yRate, m_measured.decParity);

        // for GO_SOUTH m_recenterRemaining contains the total remaining duration.
        // Choose the largest pulse size that will not lose the guide star or exceed
        // the user-specified max pulse
        m_recenterRemaining = m_calibrationSteps * s.stepMs;

        if (s.fastRecenter)
        {
            m_recenterDuration = (int) std::floor(0.8 * (double) s.maxMovePx / m_measured.yRate);
            if (m_recenterDuration > s.maxDecMs)
                m_recenterDuration = s.maxDecMs;
            if (m_recenterDuration < s.stepMs)
                m_recenterDuration = s.stepMs;
        }
        else
            m_recenterDuration = s.stepMs;

        m_calibrationSteps = DivRoundUp(m_recenterRemaining, m_recenterDuration);
        m_state = State::GoSouth;
        m_southStartingLocation = currentLocation;

        // fall through
        Debug(sink, "Falling Through to state GO_SOUTH\n");
    }
        [[fallthrough]];

    case State::GoSouth:
    {
        StepRecord info { Leg::South, m_calibrationSteps, dX, dY, currentLocation, dist };
        LogStep(sink, info);
        m_measured.decSteps.emplace_back(dX, dY);

        if (m_recenterRemaining > 0)
        {
            int duration = m_recenterDuration;
            if (duration > m_recenterRemaining)
                duration = m_recenterRemaining;

            Status(sink, info, Message::SouthStep, m_calibrationSteps);

            m_recenterRemaining -= duration;
            --m_calibrationSteps;

            Move(sink, Dir::South, duration);
            return result;
        }

        // Check for obvious guide cable problem and no useful south moves
        southDistMoved = Distance(m_southStartingLocation, currentLocation);
        northDistMoved = Distance(m_calibrationStartingLocation, m_southStartingLocation);
        southAngle = Angle(currentLocation, m_southStartingLocation);
        // Want a significant south movement that re-traces the north vector to within 30 degrees
        if (std::fabs(southDistMoved) < 0.25 * northDistMoved ||
            std::fabs(NormAngle(southAngle - (m_measured.yAngle + M_PI))) > Radians(30))
        {
            Effect e;
            e.kind = Effect::SouthAdvisory;
            if (!probe.CanPulseGuide())
            {
                if (std::fabs(southDistMoved) < 0.10 * northDistMoved)
                    e.southAdvice = SouthAdvice::NoSouthCannotPulseGuide;
                else
                    e.southAdvice = SouthAdvice::LittleSouthCannotPulseGuide;
            }
            else
                e.southAdvice = SouthAdvice::LittleSouth;
            sink.Emit(e);
        }

        m_lastLocation = currentLocation;
        // Compute the vector for the north moves we made - use it to make sure any nudging is going in the correct
        // direction These are the direction cosines of the vector
        m_northDirCosX = DX(m_calibrationInitialLocation, m_southStartingLocation) /
            Distance(m_calibrationInitialLocation, m_southStartingLocation);
        m_northDirCosY = DY(m_calibrationInitialLocation, m_southStartingLocation) /
            Distance(m_calibrationInitialLocation, m_southStartingLocation);
        //  Get magnitude and sign convention for the south moves we already made
        m_totalSouthAmt = MountY(Minus(m_southStartingLocation, m_lastLocation), m_measured.xAngle, m_measured.yAngle);
        m_state = State::NudgeSouth;
        m_calibrationSteps = 0;
        // Fall through to nudging
        Debug(sink, "Falling Through to state CALIBRATION_STATE_NUDGE_SOUTH\n");
    }
        [[fallthrough]];

    case State::NudgeSouth:
        // Nudge further South on Dec, get within 2 px North/South of starting point, don't try more than 3 times and don't
        // do nudging at all if we're starting too far away from the target
        nudge_amt = Distance(currentLocation, m_calibrationInitialLocation);
        // Compute the direction cosines for the expected nudge op
        nudgeDirCosX = DX(currentLocation, m_calibrationInitialLocation) / nudge_amt;
        nudgeDirCosY = DY(currentLocation, m_calibrationInitialLocation) / nudge_amt;
        // Compute the angle between the nudge and north move vector - they should be reversed, i.e. something close to 180
        // deg (the cosine clamped to [-1, 1], #154)
        theta = NudgeAngle(nudgeDirCosX, nudgeDirCosY, m_northDirCosX, m_northDirCosY);
        Debug(sink, "Nudge: theta = %0.2f\n", theta);
        if (std::fabs(std::fabs(theta) * 180.0 / M_PI - 180.0) < 40.0) // We're going at least roughly in the right direction
        {
            if (m_calibrationSteps <= MAX_NUDGES && nudge_amt > NUDGE_TOLERANCE && nudge_amt < dist_crit + m_blDistanceMoved)
            {
                // Compute how much more south we need to go
                double decAmt =
                    MountY(Minus(currentLocation, m_calibrationInitialLocation), m_measured.xAngle, m_measured.yAngle);
                Debug(sink, "South nudging, decAmt = %.3f, Normal south moves = %.3f\n", decAmt, m_totalSouthAmt);

                if (decAmt * m_totalSouthAmt > 0.0) // still need to move south to reach target based on matching sign
                {
                    decAmt = std::fabs(decAmt); // Sign doesn't matter now, we're always moving south
                    // wxMin, which keeps the second value when the first is NaN
                    decAmt = decAmt < (double) s.maxMovePx ? decAmt : (double) s.maxMovePx;
                    int pulseAmt = (int) std::floor(decAmt / m_measured.yRate);
                    if (pulseAmt > s.stepMs)
                        pulseAmt = s.stepMs; // Be conservative, use durations that pushed us north in the first place
                    Debug(sink, "Sending NudgeSouth pulse of duration %d ms\n", pulseAmt);
                    ++m_calibrationSteps;
                    StepRecord info { Leg::NudgeSouth, m_calibrationSteps, dX, dY, currentLocation, dist };
                    Status(sink, info, Message::NudgeSouth, m_calibrationSteps);
                    Move(sink, Dir::South, pulseAmt);
                    return result;
                }
            }
        }
        else
        {
            Debug(sink, "Nudging discontinued, wrong direction: %0.2f\n", theta);
        }

        Debug(sink, "Final south nudging status: Current loc = {%.3f,%.3f}, targeting {%.3f,%.3f}\n", currentLocation.x,
              currentLocation.y, m_calibrationInitialLocation.x, m_calibrationInitialLocation.y);

        m_state = State::Complete;
        // fall through
        Debug(sink, "Falling Through to state CALIBRATION_COMPLETE\n");
        [[fallthrough]];

    case State::Complete:
        // #148: the Dec leg was skipped for DEC_NONE (the only way yRate stays
        // uncalibrated here) and the mode was switched to one that guides Dec
        // before this frame. Scope's IsCalibrated() would stay false after it
        // stored this RA-only calibration, so the guider would enter again on
        // every frame. Fail before anything is stored; the run is left cleared,
        // so a later frame fails too rather than reporting Complete.
        if (s.decMode != DecMode::None && m_measured.yRate == RATE_UNCALIBRATED)
        {
            m_state = State::Cleared;
            return Failed(Fail::DecModeChanged);
        }
        // Scope stores and reports the calibration. If Scope enters the run
        // again in this state (Mount::IsCalibrated() is false while the mount
        // is disconnected), it reports Complete again, with firstComplete false.
        result.outcome = Outcome::Complete;
        result.firstComplete = !m_completeReported;
        m_completeReported = true;
        return result;
    }

    return result;
}

} // namespace calibration_run
