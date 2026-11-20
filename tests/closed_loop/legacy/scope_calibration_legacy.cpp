// FROZEN COPY, DO NOT EDIT: the legacy twin of Scope's calibration (#142).
//
// Source: src/scope.cpp at origin/main abe96d85 (blob 59910af3), last changed
// by cf47a1c7 "Refuse clear_calibration while calibrating (#137)", merged in
// #145, which made the CLEARED case fail the calibration. Line ranges copied,
// CRLF converted to LF and nothing else changed:
//   55-65      MAX_CALIBRATION_STEPS, the CAL_ALERT_* constants,
//              SANITY_CHECKING_ACTIVE, MAX_NUDGES, NUDGE_TOLERANCE
//   838-844    Scope::ClearCalibration
//   901-945    Scope::BeginCalibration, Scope::SetCalibration
//   979-1000   Scope::IsCalibrated
//   1013-1641  MountCoords, GetRADecCoordinates, DecBacklashAlertKey,
//              SuppressDecBacklashAlert, CalibrationStatus,
//              Scope::UpdateCalibrationState
//
// Check it against the source with
//   bash tests/closed_loop/legacy/check_twin.sh
// which diffs these ranges of src/scope.cpp at abe96d85 against the copy
// below and must print nothing. Everything the copy calls outside these lines
// is a mechanical stand-in in legacy_shim.h and scope_calibration_legacy.h.

#include "scope_calibration_legacy.h"

// The app build gives these same warnings for these lines of scope.cpp: the
// switch falls through between cases on purpose, SuppressDecBacklashAlert is
// dead (only commented-out code calls it), PHD_Point has a user copy
// constructor and is assigned, and the translated alert text is bound to a
// const reference. Some of the copied constants are used only by code that
// was not copied. The copy keeps all of it; the test target adds no warnings.
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#pragma GCC diagnostic ignored "-Wdangling-reference"

namespace legacy
{

TwinContext *g_twin = nullptr;
static TwinGuider s_guider;
static TwinFrame s_frame { &s_guider };
TwinFrame *pFrame = &s_frame;
static TwinPointingSource s_pointingSource;
TwinPointingSource *pPointingSource = &s_pointingSource;
static TwinCamera s_camera;
TwinCamera *pCamera = &s_camera;
static TwinConfig s_config;
TwinConfig *pConfig = &s_config;
TwinGuideLog GuideLog;
TwinEvtServer EvtServer;
TwinDebug Debug;

// BEGIN COPY
static const int MAX_CALIBRATION_STEPS = 60;
static const int CAL_ALERT_MINSTEPS = 4;
static const double CAL_ALERT_ORTHOGONALITY_TOLERANCE = 12.5; // Degrees
static const double CAL_ALERT_DECRATE_DIFFERENCE = 0.20; // Ratio tolerance
static const double CAL_ALERT_AXISRATES_TOLERANCE = 0.20; // Ratio tolerance
static const bool SANITY_CHECKING_ACTIVE = true; // Control calibration sanity checking

static int LIMIT_REACHED_WARN_COUNT = 5;
static int MAX_NUDGES = 3;
static double NUDGE_TOLERANCE = 2.0;

void Scope::ClearCalibration()
{
    Mount::ClearCalibration();

    m_calibrationState = CALIBRATION_STATE_CLEARED;
}

bool Scope::BeginCalibration(const PHD_Point& currentLocation)
{
    bool bError = false;

    try
    {
        if (!IsConnected())
        {
            throw ERROR_INFO("Not connected");
        }

        if (!currentLocation.IsValid())
        {
            throw ERROR_INFO("Must have a valid lock position");
        }

        CheckCalibrationDuration(m_calibrationDuration); // Make sure guide speeds or binning haven't changed underneath us
        ClearCalibration();
        m_calibrationSteps = 0;
        m_calibrationInitialLocation = currentLocation;
        m_calibrationStartingLocation.Invalidate();
        m_calibrationStartingCoords.Invalidate();
        m_calibrationState = CALIBRATION_STATE_GO_WEST;
        m_calibrationDetails.raSteps.clear();
        m_calibrationDetails.decSteps.clear();
        m_raSteps = 0;
        m_decSteps = 0;
        m_calibrationDetails.lastIssue = CI_None;
        m_eastAlertShown = false;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    return bError;
}

void Scope::SetCalibration(const Calibration& cal)
{
    m_calibration = cal;
    Mount::SetCalibration(cal);
}

bool Scope::IsCalibrated() const
{
    if (!Mount::IsCalibrated())
        return false;

    switch (m_decGuideMode)
    {
    case DEC_NONE:
        return true;
    case DEC_AUTO:
    case DEC_NORTH:
    case DEC_SOUTH:
    {
        bool have_ns_calibration = m_calibration.yRate != CALIBRATION_RATE_UNCALIBRATED;
        return have_ns_calibration;
    }
    default:
        assert(false);
        return true;
    }
}

// Convert camera coords to mount coords
static PHD_Point MountCoords(const PHD_Point& cameraVector, double xCalibAngle, double yCalibAngle)
{
    double hyp = cameraVector.Distance();
    double cameraTheta = cameraVector.Angle();
    double yAngleError = norm_angle((xCalibAngle - yCalibAngle) + M_PI / 2);
    double xAngle = cameraTheta - xCalibAngle;
    double yAngle = cameraTheta - (xCalibAngle + yAngleError);
    return PHD_Point(hyp * cos(xAngle), hyp * sin(yAngle));
}

static void GetRADecCoordinates(PHD_Point *coords)
{
    double ra, dec, lst;
    bool err = pPointingSource->GetCoordinates(&ra, &dec, &lst);
    if (err)
        coords->Invalidate();
    else
        coords->SetXY(ra, dec);
}

static wxString DecBacklashAlertKey()
{
    // we want the key to be under "/Confirm" so ConfirmDialog::ResetAllDontAskAgain() resets it, but we also want the setting
    // to be per-profile
    return wxString::Format("/Confirm/%d/DecBacklashWarningEnabled", pConfig->GetCurrentProfileId());
}

static void SuppressDecBacklashAlert(long)
{
    pConfig->Global.SetBoolean(DecBacklashAlertKey(), false);
}

static void CalibrationStatus(CalibrationStepInfo& info, const wxString& msg)
{
    info.msg = msg;
    pFrame->StatusMsg(info.msg);
    EvtServer.NotifyCalibrationStep(info);
}

bool Scope::UpdateCalibrationState(const PHD_Point& currentLocation)
{
    bool bError = false;

    try
    {
        if (!m_calibrationStartingLocation.IsValid())
        {
            m_calibrationStartingLocation = currentLocation;
            GetRADecCoordinates(&m_calibrationStartingCoords);

            Debug.Write(wxString::Format(
                "Scope::UpdateCalibrationState: starting location = %.2f,%.2f coords = %s\n", currentLocation.X,
                currentLocation.Y,
                m_calibrationStartingCoords.IsValid()
                    ? wxString::Format("%.2f,%.1f", m_calibrationStartingCoords.X, m_calibrationStartingCoords.Y)
                    : wxString("N/A")));
        }

        double dX = m_calibrationStartingLocation.dX(currentLocation);
        double dY = m_calibrationStartingLocation.dY(currentLocation);
        double dist = m_calibrationStartingLocation.Distance(currentLocation);
        double dist_crit = GetCalibrationDistance();
        double blDelta;
        double blCumDelta;
        double nudge_amt;
        double nudgeDirCosX;
        double nudgeDirCosY;
        double cos_theta;
        double theta;
        double southDistMoved;
        double northDistMoved;
        double southAngle;

        switch (m_calibrationState)
        {
        case CALIBRATION_STATE_CLEARED:
        {
            // #137: the calibration was cleared while it ran (the legacy socket server's CLEARCAL still can);
            // end it as failed instead of aborting, or staying in a state that has no next step
            wxString msg(wxTRANSLATE("Calibration failed: the calibration was cleared while calibrating"));
            GuideLog.CalibrationFailed(this, msg);
            EvtServer.NotifyCalibrationFailed(this, msg);
            throw ERROR_INFO("calibration cleared while calibrating");
        }

        case CALIBRATION_STATE_GO_WEST:
        {

            // step number in the log is the step that just finished
            CalibrationStepInfo info(this, _T("West"), m_calibrationSteps, dX, dY, currentLocation, dist);
            GuideLog.CalibrationStep(info);
            m_calibrationDetails.raSteps.push_back(wxRealPoint(dX, dY));

            if (dist < dist_crit)
            {
                if (m_calibrationSteps++ > MAX_CALIBRATION_STEPS)
                {
                    wxString msg(wxTRANSLATE("RA Calibration Failed: star did not move enough"));
                    const wxString& translated(wxGetTranslation(msg));
                    pFrame->Alert(translated);
                    GuideLog.CalibrationFailed(this, msg);
                    EvtServer.NotifyCalibrationFailed(this, msg);
                    throw ERROR_INFO("RA calibration failed");
                }
                CalibrationStatus(info, wxString::Format(_("West step %3d, dist=%4.1f"), m_calibrationSteps, dist));
                pFrame->ScheduleAxisMove(this, WEST, m_calibrationDuration, MOVEOPTS_CALIBRATION_MOVE);
                break;
            }

            // West calibration complete

            m_calibration.xAngle = m_calibrationStartingLocation.Angle(currentLocation);
            m_calibration.xRate = dist / (m_calibrationSteps * m_calibrationDuration);

            m_calibration.raGuideParity = GUIDE_PARITY_UNKNOWN;
            if (m_calibrationStartingCoords.IsValid())
            {
                PHD_Point endingCoords;
                GetRADecCoordinates(&endingCoords);
                if (endingCoords.IsValid())
                {
                    // true westward motion decreases RA
                    double ONE_ARCSEC = 24.0 / (360. * 60. * 60.); // hours
                    double dra = endingCoords.X - m_calibrationStartingCoords.X;
                    if (dra < -ONE_ARCSEC)
                        m_calibration.raGuideParity = GUIDE_PARITY_EVEN;
                    else if (dra > ONE_ARCSEC)
                        m_calibration.raGuideParity = GUIDE_PARITY_ODD;
                }
            }

            Debug.Write(wxString::Format("WEST calibration completes with steps=%d angle=%.1f rate=%.3f parity=%d\n",
                                         m_calibrationSteps, degrees(m_calibration.xAngle), m_calibration.xRate * 1000.0,
                                         m_calibration.raGuideParity));

            m_raSteps = m_calibrationSteps;
            GuideLog.CalibrationDirectComplete(this, "West", m_calibration.xAngle, m_calibration.xRate,
                                               m_calibration.raGuideParity);

            // for GO_EAST m_recenterRemaining contains the total remaining duration.
            // Choose the largest pulse size that will not lose the guide star or exceed
            // the user-specified max pulse

            m_recenterRemaining = m_calibrationSteps * m_calibrationDuration;

            if (pFrame->pGuider->IsFastRecenterEnabled())
            {
                m_recenterDuration = (int) floor((double) pFrame->pGuider->GetMaxMovePixels() / m_calibration.xRate);
                if (m_recenterDuration > m_maxRaDuration)
                    m_recenterDuration = m_maxRaDuration;
                if (m_recenterDuration < m_calibrationDuration)
                    m_recenterDuration = m_calibrationDuration;
            }
            else
                m_recenterDuration = m_calibrationDuration;

            m_calibrationSteps = DIV_ROUND_UP(m_recenterRemaining, m_recenterDuration);
            m_calibrationState = CALIBRATION_STATE_GO_EAST;
            m_eastStartingLocation = currentLocation;

            // fall through
            Debug.Write("Falling Through to state GO_EAST\n");
        }

        case CALIBRATION_STATE_GO_EAST:
        {

            CalibrationStepInfo info(this, _T("East"), m_calibrationSteps, dX, dY, currentLocation, dist);
            GuideLog.CalibrationStep(info);
            m_calibrationDetails.raSteps.push_back(wxRealPoint(dX, dY));

            if (m_recenterRemaining > 0)
            {
                int duration = m_recenterDuration;
                if (duration > m_recenterRemaining)
                    duration = m_recenterRemaining;

                CalibrationStatus(info, wxString::Format(_("East step %3d, dist=%4.1f"), m_calibrationSteps, dist));

                m_recenterRemaining -= duration;
                --m_calibrationSteps;
                m_lastLocation = currentLocation;

                pFrame->ScheduleAxisMove(this, EAST, duration, MOVEOPTS_CALIBRATION_MOVE);
                break;
            }

            // If not pulse-guiding check for obvious guide cable problem and no useful east moves
            if (!CanPulseGuide())
            {
                double eastDistMoved = m_eastStartingLocation.Distance(currentLocation);
                double westDistMoved = m_calibrationStartingLocation.Distance(m_eastStartingLocation);
                double eastAngle = m_eastStartingLocation.Angle(currentLocation);
                // Want a significant east movement that re-traces the west vector to within 30 degrees
                if (fabs(eastDistMoved) < 0.25 * westDistMoved ||
                    fabs(norm_angle(eastAngle - (m_calibration.xAngle + M_PI))) > radians(30))
                {
                    wxString msg(wxTRANSLATE(
                        "Advisory: Little or no east movement was measured, so guiding will probably be impaired. "
                        "Check the guide cable and use the Manual Guide tool to confirm basic operation of the mount."));
                    const wxString& translated(wxGetTranslation(msg));
                    pFrame->Alert(translated, 0, wxEmptyString, 0, 0, true);
                    Debug.Write("Calibration alert: " + msg + "\n");
                    m_eastAlertShown = true;
                }
            }
            // setup for clear backlash

            m_calibrationSteps = 0;
            dist = dX = dY = 0.0;
            m_calibrationStartingLocation = currentLocation;

            if (m_decGuideMode == DEC_NONE)
            {
                Debug.Write("Skipping Dec calibration as DecGuideMode == NONE\n");
                m_calibrationState = CALIBRATION_STATE_COMPLETE;
                m_calibration.yAngle =
                    norm_angle(m_calibration.xAngle + M_PI / 2.); // choose an arbitrary angle perpendicular to xAngle
                // indicate lack of Dec calibration data, see Scope::IsCalibrated.
                m_calibration.yRate = CALIBRATION_RATE_UNCALIBRATED;
                m_calibration.decGuideParity = GUIDE_PARITY_UNKNOWN;
                break;
            }

            m_calibrationState = CALIBRATION_STATE_CLEAR_BACKLASH;
            m_blMarkerPoint = currentLocation;
            GetRADecCoordinates(&m_calibrationStartingCoords);
            m_blExpectedBacklashStep = m_calibration.xRate * m_calibrationDuration * 0.6;

            double RASpeed;
            double DecSpeed;
            if (!pPointingSource->GetGuideRates(&RASpeed, &DecSpeed) && RASpeed != 0.0 && RASpeed != DecSpeed)
            {
                m_blExpectedBacklashStep *= DecSpeed / RASpeed;
            }

            m_blMaxClearingPulses = wxMax(8, BL_MAX_CLEARING_TIME / m_calibrationDuration);
            m_blLastCumDistance = 0;
            m_blAcceptedMoves = 0;
            Debug.Write(wxString::Format("Backlash: Looking for 3 moves of %0.1f px, max attempts = %d\n",
                                         m_blExpectedBacklashStep, m_blMaxClearingPulses));
            // fall through
            Debug.Write("Falling Through to state CLEAR_BACKLASH\n");
        }

        case CALIBRATION_STATE_CLEAR_BACKLASH:
        {

            CalibrationStepInfo info(this, _T("Backlash"), m_calibrationSteps, dX, dY, currentLocation, dist);
            GuideLog.CalibrationStep(info);
            blDelta = m_blMarkerPoint.Distance(currentLocation);
            blCumDelta = dist;

            // Want to see the mount moving north for 3 moves of >= expected distance pixels without any direction reversals
            if (m_calibrationSteps == 0)
            {
                // Get things moving with the first clearing pulse
                Debug.Write(
                    wxString::Format("Backlash: Starting north clearing using pulse width of %d\n", m_calibrationDuration));
                pFrame->ScheduleAxisMove(this, NORTH, m_calibrationDuration, MOVEOPTS_CALIBRATION_MOVE);
                m_calibrationSteps = 1;
                CalibrationStatus(info, _("Clearing backlash step 1"));
                break;
            }

            if (blDelta >= m_blExpectedBacklashStep)
            {
                if (m_blAcceptedMoves == 0 ||
                    (blCumDelta > m_blLastCumDistance)) // Just starting or still moving in same direction
                {
                    m_blAcceptedMoves++;
                    Debug.Write(wxString::Format("Backlash: Accepted clearing move of %0.1f\n", blDelta));
                }
                else
                {
                    m_blAcceptedMoves = 0; // Reset on a direction reversal
                    Debug.Write(wxString::Format("Backlash: Rejected clearing move of %0.1f, direction reversal\n", blDelta));
                }
            }
            else
            {
                if (blCumDelta < m_blLastCumDistance)
                {
                    m_blAcceptedMoves = 0;
                    Debug.Write(wxString::Format("Backlash: Rejected small direction reversal of %0.1f px\n", blDelta));
                }
                else
                    Debug.Write(wxString::Format("Backlash: Rejected small move of %0.1f px\n", blDelta));
            }

            if (m_blAcceptedMoves < BL_BACKLASH_MIN_COUNT) // More work to do
            {
                if (m_calibrationSteps < m_blMaxClearingPulses && blCumDelta < dist_crit)
                {
                    // Still have attempts left, haven't moved the star by 25 px yet
                    pFrame->ScheduleAxisMove(this, NORTH, m_calibrationDuration, MOVEOPTS_CALIBRATION_MOVE);
                    m_calibrationSteps++;
                    m_blMarkerPoint = currentLocation;
                    GetRADecCoordinates(&m_calibrationStartingCoords);
                    m_blLastCumDistance = blCumDelta;
                    CalibrationStatus(info, wxString::Format(_("Clearing backlash step %3d"), m_calibrationSteps));
                    Debug.Write(wxString::Format("Backlash: %s, Last Delta = %0.2f px, CumDistance = %0.2f px\n", info.msg,
                                                 blDelta, blCumDelta));
                    break;
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
                        Debug.Write(
                            "Backlash: Reached clearing limit but total displacement > 3px - proceeding with calibration\n");
                    }
                    else
                    {
                        wxString msg(wxTRANSLATE("Backlash Clearing Failed: star did not move enough"));
                        const wxString& translated(wxGetTranslation(msg));
                        pFrame->Alert(translated);
                        GuideLog.CalibrationFailed(this, msg);
                        EvtServer.NotifyCalibrationFailed(this, msg);
                        throw ERROR_INFO("Clear backlash failed");
                    }
                }
            }
            else // Got our 3 moves, move ahead
            {
                // We know the last backlash clearing move was big enough - include that as a north calibration move

                // log the starting point
                CalibrationStepInfo info(this, _T("North"), 0, 0.0, 0.0, m_blMarkerPoint, 0.0);
                GuideLog.CalibrationStep(info);
                m_calibrationDetails.decSteps.push_back(wxRealPoint(0.0, 0.0));

                m_calibrationSteps = 1;
                m_calibrationStartingLocation = m_blMarkerPoint;
                dX = m_blMarkerPoint.dX(currentLocation);
                dY = m_blMarkerPoint.dY(currentLocation);
                dist = m_blMarkerPoint.Distance(currentLocation);
                Debug.Write("Backlash: Got 3 acceptable moves, using last move as step 1 of N calibration\n");
            }

            m_blDistanceMoved = m_blMarkerPoint.Distance(m_calibrationInitialLocation); // Need this to set nudging limit

            Debug.Write(wxString::Format("Backlash: North calibration moves starting at {%0.1f,%0.1f}, Offset = %0.1f px\n",
                                         m_blMarkerPoint.X, m_blMarkerPoint.Y, m_blDistanceMoved));
            Debug.Write(wxString::Format("Backlash: Total distance moved = %0.1f\n",
                                         currentLocation.Distance(m_calibrationInitialLocation)));

            m_calibrationState = CALIBRATION_STATE_GO_NORTH;
            // falling through to start moving north
            Debug.Write("Backlash: Falling Through to state GO_NORTH\n");
        }

        case CALIBRATION_STATE_GO_NORTH:
        {

            CalibrationStepInfo info(this, _T("North"), m_calibrationSteps, dX, dY, currentLocation, dist);
            GuideLog.CalibrationStep(info);
            m_calibrationDetails.decSteps.push_back(wxRealPoint(dX, dY));

            if (dist < dist_crit)
            {
                if (m_calibrationSteps++ > MAX_CALIBRATION_STEPS)
                {
                    wxString msg(wxTRANSLATE("DEC Calibration Failed: star did not move enough"));
                    const wxString& translated(wxGetTranslation(msg));
                    pFrame->Alert(translated);
                    GuideLog.CalibrationFailed(this, msg);
                    EvtServer.NotifyCalibrationFailed(this, msg);
                    throw ERROR_INFO("Dec calibration failed");
                }
                CalibrationStatus(info, wxString::Format(_("North step %3d, dist=%4.1f"), m_calibrationSteps, dist));
                pFrame->ScheduleAxisMove(this, NORTH, m_calibrationDuration, MOVEOPTS_CALIBRATION_MOVE);
                break;
            }

            // note: this calculation is reversed from the ra calculation, because
            // that one was calibrating WEST, but the angle is really relative
            // to EAST
            if (m_assumeOrthogonal)
            {
                double a1 = norm_angle(m_calibration.xAngle + M_PI / 2.);
                double a2 = norm_angle(m_calibration.xAngle - M_PI / 2.);
                double yAngle = currentLocation.Angle(m_calibrationStartingLocation);
                m_calibration.yAngle = fabs(norm_angle(a1 - yAngle)) < fabs(norm_angle(a2 - yAngle)) ? a1 : a2;
                double dec_dist = dist * cos(yAngle - m_calibration.yAngle);
                m_calibration.yRate = dec_dist / (m_calibrationSteps * m_calibrationDuration);

                Debug.Write(wxString::Format("Assuming orthogonal axes: measured Y angle = %.1f, X angle = %.1f, orthogonal = "
                                             "%.1f, %.1f, best = %.1f, dist = %.2f, dec_dist = %.2f\n",
                                             degrees(yAngle), degrees(m_calibration.xAngle), degrees(a1), degrees(a2),
                                             degrees(m_calibration.yAngle), dist, dec_dist));
            }
            else
            {
                m_calibration.yAngle = currentLocation.Angle(m_calibrationStartingLocation);
                m_calibration.yRate = dist / (m_calibrationSteps * m_calibrationDuration);
            }

            m_decSteps = m_calibrationSteps;

            m_calibration.decGuideParity = GUIDE_PARITY_UNKNOWN;
            if (m_calibrationStartingCoords.IsValid())
            {
                PHD_Point endingCoords;
                GetRADecCoordinates(&endingCoords);
                if (endingCoords.IsValid())
                {
                    // real Northward motion increases Dec
                    double ONE_ARCSEC = 1.0 / (60. * 60.); // degrees
                    double ddec = endingCoords.Y - m_calibrationStartingCoords.Y;
                    if (ddec > ONE_ARCSEC)
                        m_calibration.decGuideParity = GUIDE_PARITY_EVEN;
                    else if (ddec < -ONE_ARCSEC)
                        m_calibration.decGuideParity = GUIDE_PARITY_ODD;
                }
            }

            Debug.Write(wxString::Format("NORTH calibration completes with angle=%.1f rate=%.3f parity=%d\n",
                                         degrees(m_calibration.yAngle), m_calibration.yRate * 1000.0,
                                         m_calibration.decGuideParity));

            GuideLog.CalibrationDirectComplete(this, "North", m_calibration.yAngle, m_calibration.yRate,
                                               m_calibration.decGuideParity);

            // for GO_SOUTH m_recenterRemaining contains the total remaining duration.
            // Choose the largest pulse size that will not lose the guide star or exceed
            // the user-specified max pulse
            m_recenterRemaining = m_calibrationSteps * m_calibrationDuration;

            if (pFrame->pGuider->IsFastRecenterEnabled())
            {
                m_recenterDuration = (int) floor(0.8 * (double) pFrame->pGuider->GetMaxMovePixels() / m_calibration.yRate);
                if (m_recenterDuration > m_maxDecDuration)
                    m_recenterDuration = m_maxDecDuration;
                if (m_recenterDuration < m_calibrationDuration)
                    m_recenterDuration = m_calibrationDuration;
            }
            else
                m_recenterDuration = m_calibrationDuration;

            m_calibrationSteps = DIV_ROUND_UP(m_recenterRemaining, m_recenterDuration);
            m_calibrationState = CALIBRATION_STATE_GO_SOUTH;
            m_southStartingLocation = currentLocation;

            // fall through
            Debug.Write("Falling Through to state GO_SOUTH\n");
        }

        case CALIBRATION_STATE_GO_SOUTH:
        {

            CalibrationStepInfo info(this, _T("South"), m_calibrationSteps, dX, dY, currentLocation, dist);
            GuideLog.CalibrationStep(info);
            m_calibrationDetails.decSteps.push_back(wxRealPoint(dX, dY));

            if (m_recenterRemaining > 0)
            {
                int duration = m_recenterDuration;
                if (duration > m_recenterRemaining)
                    duration = m_recenterRemaining;

                CalibrationStatus(info, wxString::Format(_("South step %3d, dist=%4.1f"), m_calibrationSteps, dist));

                m_recenterRemaining -= duration;
                --m_calibrationSteps;

                pFrame->ScheduleAxisMove(this, SOUTH, duration, MOVEOPTS_CALIBRATION_MOVE);
                break;
            }

            // Check for obvious guide cable problem and no useful south moves
            southDistMoved = m_southStartingLocation.Distance(currentLocation);
            northDistMoved = m_calibrationStartingLocation.Distance(m_southStartingLocation);
            southAngle = currentLocation.Angle(m_southStartingLocation);
            // Want a significant south movement that re-traces the north vector to within 30 degrees
            if (fabs(southDistMoved) < 0.25 * northDistMoved ||
                fabs(norm_angle(southAngle - (m_calibration.yAngle + M_PI))) > radians(30))
            {
                wxString msg;
                if (!CanPulseGuide())
                {
                    if (fabs(southDistMoved) < 0.10 * northDistMoved)
                        msg = wxTRANSLATE("Advisory: Calibration successful but little or no south movement was measured, so "
                                          "guiding will probably be impaired.\n "
                                          "This is usually caused by a faulty guide cable or very large Dec backlash. \n"
                                          "Check the guide cable and read the online Help for how to identify these types of "
                                          "problems (Manual Guide, Declination backlash).");
                    else
                        msg = wxTRANSLATE(
                            "Advisory: Calibration successful but little south movement was measured, so guiding will probably "
                            "be impaired. \n"
                            "This is usually caused by very large Dec backlash or other problems with the mount mechanics. \n"
                            "Read the online Help for how to identify these types of problems (Manual Guide, Declination "
                            "backlash).");
                }
                else
                    msg = wxTRANSLATE(
                        "Advisory: Calibration successful but little south movement was measured, so guiding may be "
                        "impaired.\n "
                        "This is usually caused by very large Dec backlash or other problems with the mount mechanics. \n"
                        "Read the online help for how to deal with this type of problem (Declination backlash).");

                // if (!m_eastAlertShown)
                //{
                //     const wxString& translated(wxGetTranslation(msg));
                //     pFrame->SuppressibleAlert(DecBacklashAlertKey(), translated, SuppressDecBacklashAlert, 0, true);
                // }
                Debug.Write("Omitted calibration alert: " + msg + "\n");
            }

            m_lastLocation = currentLocation;
            // Compute the vector for the north moves we made - use it to make sure any nudging is going in the correct
            // direction These are the direction cosines of the vector
            m_northDirCosX = m_calibrationInitialLocation.dX(m_southStartingLocation) /
                m_calibrationInitialLocation.Distance(m_southStartingLocation);
            m_northDirCosY = m_calibrationInitialLocation.dY(m_southStartingLocation) /
                m_calibrationInitialLocation.Distance(m_southStartingLocation);
            // Debug.Write(wxString::Format("Nudge: InitStart:{%0.1f,%0.1f}, southStart:{%.1f,%0.1f}, north_l:%0.2f,
            // north_m:%0.2f\n",
            //     m_calibrationInitialLocation.X, m_calibrationInitialLocation.Y,
            //     m_southStartingLocation.X, m_southStartingLocation.Y, m_northDirCosX, m_northDirCosY));
            //  Get magnitude and sign convention for the south moves we already made
            m_totalSouthAmt =
                MountCoords(m_southStartingLocation - m_lastLocation, m_calibration.xAngle, m_calibration.yAngle).Y;
            m_calibrationState = CALIBRATION_STATE_NUDGE_SOUTH;
            m_calibrationSteps = 0;
            // Fall through to nudging
            Debug.Write("Falling Through to state CALIBRATION_STATE_NUDGE_SOUTH\n");
        }

        case CALIBRATION_STATE_NUDGE_SOUTH:
            // Nudge further South on Dec, get within 2 px North/South of starting point, don't try more than 3 times and don't
            // do nudging at all if we're starting too far away from the target
            nudge_amt = currentLocation.Distance(m_calibrationInitialLocation);
            // Compute the direction cosines for the expected nudge op
            nudgeDirCosX = currentLocation.dX(m_calibrationInitialLocation) / nudge_amt;
            nudgeDirCosY = currentLocation.dY(m_calibrationInitialLocation) / nudge_amt;
            // Compute the angle between the nudge and north move vector - they should be reversed, i.e. something close to 180
            // deg
            cos_theta = nudgeDirCosX * m_northDirCosX + nudgeDirCosY * m_northDirCosY;
            theta = acos(cos_theta);
            // Debug.Write(wxString::Format("Nudge: currLoc:{%0.1f,%0.1f}, m_nudgeDirCosX: %0.2f, nudgeDirCosY: %0.2f,
            // cos_theta: %0.2f\n",
            //     currentLocation.X, currentLocation.Y, nudgeDirCosX, nudgeDirCosY, cos_theta));
            Debug.Write(wxString::Format("Nudge: theta = %0.2f\n", theta));
            if (fabs(fabs(theta) * 180.0 / M_PI - 180.0) < 40.0) // We're going at least roughly in the right direction
            {
                if (m_calibrationSteps <= MAX_NUDGES && nudge_amt > NUDGE_TOLERANCE &&
                    nudge_amt < dist_crit + m_blDistanceMoved)
                {
                    // Compute how much more south we need to go
                    double decAmt =
                        MountCoords(currentLocation - m_calibrationInitialLocation, m_calibration.xAngle, m_calibration.yAngle)
                            .Y;
                    Debug.Write(
                        wxString::Format("South nudging, decAmt = %.3f, Normal south moves = %.3f\n", decAmt, m_totalSouthAmt));

                    if (decAmt * m_totalSouthAmt > 0.0) // still need to move south to reach target based on matching sign
                    {
                        decAmt = fabs(decAmt); // Sign doesn't matter now, we're always moving south
                        decAmt = wxMin(decAmt, (double) pFrame->pGuider->GetMaxMovePixels());
                        int pulseAmt = (int) floor(decAmt / m_calibration.yRate);
                        if (pulseAmt > m_calibrationDuration)
                            pulseAmt =
                                m_calibrationDuration; // Be conservative, use durations that pushed us north in the first place
                        Debug.Write(wxString::Format("Sending NudgeSouth pulse of duration %d ms\n", pulseAmt));
                        ++m_calibrationSteps;
                        CalibrationStepInfo info(this, _T("NudgeSouth"), m_calibrationSteps, dX, dY, currentLocation, dist);
                        CalibrationStatus(info, wxString::Format(_("Nudge South %3d"), m_calibrationSteps));
                        pFrame->ScheduleAxisMove(this, SOUTH, pulseAmt, MOVEOPTS_CALIBRATION_MOVE);
                        break;
                    }
                }
            }
            else
            {
                Debug.Write(wxString::Format("Nudging discontinued, wrong direction: %0.2f\n", theta));
            }

            Debug.Write(wxString::Format("Final south nudging status: Current loc = {%.3f,%.3f}, targeting {%.3f,%.3f}\n",
                                         currentLocation.X, currentLocation.Y, m_calibrationInitialLocation.X,
                                         m_calibrationInitialLocation.Y));

            m_calibrationState = CALIBRATION_STATE_COMPLETE;
            // fall through
            Debug.Write("Falling Through to state CALIBRATION_COMPLETE\n");

        case CALIBRATION_STATE_COMPLETE:
            GetLastCalibration(&m_prevCalibration);
            LoadCalibrationDetails(&m_prevCalibrationDetails);
            Calibration cal(m_calibration);
            cal.declination = pPointingSource->GetDeclinationRadians();
            cal.pierSide = pPointingSource->SideOfPier();
            cal.rotatorAngle = Rotator::RotatorPosition();
            cal.binning = pCamera->GetBinning();
            SetCalibration(cal);
            m_calibrationDetails.raStepCount = m_raSteps;
            m_calibrationDetails.decStepCount = m_decSteps;
            SetCalibrationDetails(m_calibrationDetails, m_calibration.xAngle, m_calibration.yAngle, m_calibration.binning);
            if (SANITY_CHECKING_ACTIVE)
                SanityCheckCalibration(m_prevCalibration, m_prevCalibrationDetails); // method gets "new" info itself
            pFrame->StatusMsg(_("Calibration complete"));
            GuideLog.CalibrationComplete(this);
            EvtServer.NotifyCalibrationComplete(this);
            Debug.Write("Calibration Complete\n");
            pConfig->Flush();
            break;
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);

        ClearCalibration();

        bError = true;
    }

    return bError;
}
// END COPY

} // namespace legacy
