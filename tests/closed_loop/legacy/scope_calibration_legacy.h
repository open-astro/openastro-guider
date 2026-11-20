// The frozen legacy twin of Scope's calibration state machine (#142).
//
// legacy::Scope has the calibration members of src/scope.h with the same
// names and types, so scope_calibration_legacy.cpp can hold the functions of
// src/scope.cpp copied verbatim. legacy::Mount stands in for the parts of
// Mount those functions call: IsCalibrated, ClearCalibration and
// SetCalibration as Mount does them (the calibrated flag and m_cal), the last
// calibration and details as the profile would hold them, and no persistence.
//
// The twin pins today's behaviour so the extraction of a wx-free
// CalibrationRun (PR-B) can be proven identical by differential testing. It is
// deleted with #115 step 5, after the high-Dec fix (#141) has landed.

#ifndef CLOSED_LOOP_SCOPE_CALIBRATION_LEGACY_H
#define CLOSED_LOOP_SCOPE_CALIBRATION_LEGACY_H

#include "legacy_shim.h"

namespace legacy
{

class Mount
{
protected:
    bool m_calibrated = false;
    Calibration m_cal;
    CalibrationDetails m_savedDetails;

public:
    virtual ~Mount() = default;

    bool IsConnected() const { return true; }

    // Mount::IsCalibrated (mount.cpp)
    virtual bool IsCalibrated() const { return IsConnected() && m_calibrated; }

    // Mount::ClearCalibration (mount.cpp), without the status bar update
    virtual void ClearCalibration() { m_calibrated = false; }

    // Mount::SetCalibration (mount.cpp): stores the calibration and marks the mount calibrated
    virtual void SetCalibration(const Calibration& cal)
    {
        GuideParity ra = m_cal.raGuideParity;
        GuideParity dec = m_cal.decGuideParity;
        m_cal = cal;
        if (cal.raGuideParity == GUIDE_PARITY_UNCHANGED)
            m_cal.raGuideParity = ra;
        if (cal.decGuideParity == GUIDE_PARITY_UNCHANGED)
            m_cal.decGuideParity = dec;
        m_cal.isValid = true;
        m_calibrated = true;
        closed_loop::Event& e = g_twin->Add(closed_loop::Event::SetCalibration);
        e.angle = cal.xAngle;
        e.rate = cal.xRate;
    }

    void GetLastCalibration(Calibration *cal) const { *cal = m_cal; }
    void LoadCalibrationDetails(CalibrationDetails *details) const { *details = m_savedDetails; }
};

class Scope : public Mount
{
public:
    // scope.h, the members the calibration reads or writes
    int m_calibrationDuration = 750;
    int m_maxDecDuration = 2500;
    int m_maxRaDuration = 2500;
    DEC_GUIDE_MODE m_decGuideMode = DEC_AUTO;

    int m_calibrationSteps = 0;
    int m_calibrationDistance = 25;
    int m_recenterRemaining = 0;
    int m_recenterDuration = 0;
    PHD_Point m_calibrationInitialLocation;
    PHD_Point m_calibrationStartingLocation;
    PHD_Point m_calibrationStartingCoords;
    PHD_Point m_southStartingLocation;
    PHD_Point m_eastStartingLocation;
    PHD_Point m_lastLocation;
    double m_totalSouthAmt = 0.;
    double m_northDirCosX = 0.;
    double m_northDirCosY = 0.;
    bool m_eastAlertShown = false;

    PHD_Point m_blMarkerPoint;
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

    Calibration m_calibration;
    CalibrationDetails m_calibrationDetails;
    bool m_assumeOrthogonal = false;
    int m_raSteps = 0;
    int m_decSteps = 0;

    Calibration m_prevCalibration;
    CalibrationDetails m_prevCalibrationDetails;

    enum CALIBRATION_STATE
    {
        CALIBRATION_STATE_CLEARED,
        CALIBRATION_STATE_GO_WEST,
        CALIBRATION_STATE_GO_EAST,
        CALIBRATION_STATE_CLEAR_BACKLASH,
        CALIBRATION_STATE_GO_NORTH,
        CALIBRATION_STATE_GO_SOUTH,
        CALIBRATION_STATE_NUDGE_SOUTH,
        CALIBRATION_STATE_COMPLETE
    };
    // Scope's constructor leaves this uninitialized; the twin starts CLEARED
    CALIBRATION_STATE m_calibrationState = CALIBRATION_STATE_CLEARED;

    // Copied from src/scope.cpp (scope_calibration_legacy.cpp)
    void ClearCalibration() override;
    bool BeginCalibration(const PHD_Point& currentLocation);
    void SetCalibration(const Calibration& cal) override;
    bool IsCalibrated() const override;
    bool UpdateCalibrationState(const PHD_Point& currentLocation);

    // Stand-ins for what the copied code calls on Scope
    int GetCalibrationDistance() const { return m_calibrationDistance; }
    bool CanPulseGuide() { return g_twin->sim->CanPulseGuide(); }
    // Scope::CheckCalibrationDuration reads the profile and the camera and may
    // change the step or the distance; that stays in Scope and is not modelled.
    void CheckCalibrationDuration(int) { }
    // Scope::SetCalibrationDetails also reads the guide rates, focal length and
    // image scale and saves the details to the profile; the twin only keeps them.
    void SetCalibrationDetails(const CalibrationDetails& calDetails, double, double, int)
    {
        m_calibrationDetails = calDetails;
        m_savedDetails = calDetails;
    }
    // Scope::SanityCheckCalibration alerts on the result; it stays in Scope.
    void SanityCheckCalibration(const Calibration&, const CalibrationDetails&) { g_twin->Add(closed_loop::Event::SanityCheck); }
};

} // namespace legacy

#endif // CLOSED_LOOP_SCOPE_CALIBRATION_LEGACY_H
