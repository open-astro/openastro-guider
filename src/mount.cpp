/*
 *  mount.cpp
 *  PHD Guiding
 *
 *  Created by Bret McKee
 *  Copyright (c) 2012 Bret McKee
 *  All rights reserved.
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

#include "phd.h"
#include "backlash_comp.h"
#include "calibration_adjust.h"
#include "calibration_codec.h"
#include "calibration_transform.h"
#include "guide_move_plan.h"
#include "guiding_assistant.h"
#include "gaussian_process_guider.h"

#include <wx/tokenzr.h>
#include <cstdarg>

// Mount::MoveOffset casts its moves' results into guide_move_plan (#181)
static_assert(static_cast<int>(guide_move_plan::MoveResult::Ok) == Mount::MOVE_OK &&
              static_cast<int>(guide_move_plan::MoveResult::Error) == Mount::MOVE_ERROR &&
              static_cast<int>(guide_move_plan::MoveResult::ErrorSlewing) == Mount::MOVE_ERROR_SLEWING &&
              static_cast<int>(guide_move_plan::MoveResult::ErrorAoLimitReached) == Mount::MOVE_ERROR_AO_LIMIT_REACHED);

// The calibration codec and adjustment hold the app's pier side, parity and unknown values as ints (#142)
static_assert(calibration_codec::PIER_SIDE_UNKNOWN == PIER_SIDE_UNKNOWN &&
              calibration_codec::PIER_SIDE_EAST == PIER_SIDE_EAST && calibration_codec::PIER_SIDE_WEST == PIER_SIDE_WEST);
static_assert(calibration_codec::GUIDE_PARITY_EVEN == GUIDE_PARITY_EVEN &&
              calibration_codec::GUIDE_PARITY_ODD == GUIDE_PARITY_ODD &&
              calibration_codec::GUIDE_PARITY_UNKNOWN == GUIDE_PARITY_UNKNOWN);
static_assert(calibration_codec::UNKNOWN_DEC == UNKNOWN_DECLINATION);

enum
{
    Algo_RA_Layout_Height = 200,
    Algo_Dec_Layout_Height = 150
};

inline static PierSide OppositeSide(PierSide p)
{
    switch (p)
    {
    case PIER_SIDE_EAST:
        return PIER_SIDE_WEST;
    case PIER_SIDE_WEST:
        return PIER_SIDE_EAST;
    default:
        return PIER_SIDE_UNKNOWN;
    }
}

inline static bool IsOppositeSide(PierSide a, PierSide b)
{
    return (a == PIER_SIDE_EAST && b == PIER_SIDE_WEST) || (a == PIER_SIDE_WEST && b == PIER_SIDE_EAST);
}

inline static wxString PierSideStr(PierSide p)
{
    switch (p)
    {
    case PIER_SIDE_EAST:
        return "East";
    case PIER_SIDE_WEST:
        return "West";
    default:
        return "Unknown";
    }
}

inline static wxString PierSideStrTr(PierSide p, const wxString& unknown = _("Unknown"))
{
    switch (p)
    {
    case PIER_SIDE_EAST:
        return _("East");
    case PIER_SIDE_WEST:
        return _("West");
    default:
        return unknown;
    }
}

wxString Mount::PierSideStr(PierSide p)
{
    return ::PierSideStr(p);
}

wxString Mount::PierSideStrTr(PierSide p)
{
    return ::PierSideStrTr(p);
}

inline static const char *ParityStr(GuideParity par)
{
    switch (par)
    {
    case GUIDE_PARITY_EVEN:
        return "+";
    case GUIDE_PARITY_ODD:
        return "-";
    case GUIDE_PARITY_UNKNOWN:
        return "?";
    default:
    case GUIDE_PARITY_UNCHANGED:
        return "x";
    }
}

inline static GuideParity OppositeParity(GuideParity p)
{
    switch (p)
    {
    case GUIDE_PARITY_EVEN:
        return GUIDE_PARITY_ODD;
    case GUIDE_PARITY_ODD:
        return GUIDE_PARITY_EVEN;
    default:
        return p;
    }
}

wxString Mount::DeclinationStr(double dec, const wxString& numFormatStr)
{
    return dec == UNKNOWN_DECLINATION ? wxString("Unknown") : wxString::Format(numFormatStr, degrees(dec));
}

wxString Mount::DeclinationStrTr(double dec, const wxString& numFormatStr)
{
    return dec == UNKNOWN_DECLINATION ? _("Unknown") : wxString::Format(numFormatStr, degrees(dec));
}

// The full set of guide algorithms, listed explicitly (not an enum-range loop) so
// a future gap in the GUIDE_ALGORITHM enum can't slip an unnamed value through.
std::vector<GUIDE_ALGORITHM> Mount::AllAlgorithms()
{
    return { GUIDE_ALGORITHM_IDENTITY,      GUIDE_ALGORITHM_HYSTERESIS,       GUIDE_ALGORITHM_LOWPASS, GUIDE_ALGORITHM_LOWPASS2,
             GUIDE_ALGORITHM_RESIST_SWITCH, GUIDE_ALGORITHM_GAUSSIAN_PROCESS, GUIDE_ALGORITHM_ZFILTER };
}

GUIDE_ALGORITHM Mount::GuideAlgorithmFromName(const wxString& s)
{
    // Accept either the untranslated wire name (the API path: get_algos / set_algo,
    // which speaks GuideAlgorithmName()'s bare strings) or the translated display
    // name (the GUI choice list via GetStringSelection()). ZFilter's GUI label can
    // carry a suffix, so it is matched by prefix. "None" maps to the identity algo.
    for (GUIDE_ALGORITHM a : AllAlgorithms())
    {
        wxString untranslated = GuideAlgorithmName(a);
        wxString translated = wxGetTranslation(untranslated);
        if (a == GUIDE_ALGORITHM_ZFILTER)
        {
            if (s.StartsWith(untranslated) || s.StartsWith(translated))
                return GUIDE_ALGORITHM_ZFILTER;
        }
        else if (s == untranslated || s == translated)
        {
            return a;
        }
    }
    return GUIDE_ALGORITHM_NONE;
}

// returns the untranslated name
wxString Mount::GuideAlgorithmName(int algo)
{
    switch (algo)
    {
    case GUIDE_ALGORITHM_NONE:
    case GUIDE_ALGORITHM_IDENTITY:
    default:
        return wxTRANSLATE("None");
    case GUIDE_ALGORITHM_HYSTERESIS:
        return wxTRANSLATE("Hysteresis");
    case GUIDE_ALGORITHM_LOWPASS:
        return wxTRANSLATE("Lowpass");
    case GUIDE_ALGORITHM_LOWPASS2:
        return wxTRANSLATE("Lowpass2");
    case GUIDE_ALGORITHM_RESIST_SWITCH:
        return wxTRANSLATE("Resist Switch");
    case GUIDE_ALGORITHM_GAUSSIAN_PROCESS:
        return wxTRANSLATE("Predictive PEC");
    case GUIDE_ALGORITHM_ZFILTER:
        return wxTRANSLATE("ZFilter");
    }
}

std::vector<GUIDE_ALGORITHM> Mount::AvailableAlgorithms(bool isStepGuider, GuideAxis axis)
{
    if (isStepGuider)
        return { GUIDE_ALGORITHM_HYSTERESIS, GUIDE_ALGORITHM_LOWPASS, GUIDE_ALGORITHM_LOWPASS2, GUIDE_ALGORITHM_ZFILTER };
    if (axis == GUIDE_X) // Right Ascension
        return { GUIDE_ALGORITHM_HYSTERESIS,    GUIDE_ALGORITHM_LOWPASS,          GUIDE_ALGORITHM_LOWPASS2,
                 GUIDE_ALGORITHM_RESIST_SWITCH, GUIDE_ALGORITHM_GAUSSIAN_PROCESS, GUIDE_ALGORITHM_ZFILTER };
    // Declination (Gaussian Process / Predictive PEC is RA-only)
    return { GUIDE_ALGORITHM_HYSTERESIS, GUIDE_ALGORITHM_LOWPASS, GUIDE_ALGORITHM_LOWPASS2, GUIDE_ALGORITHM_RESIST_SWITCH,
             GUIDE_ALGORITHM_ZFILTER };
}

GUIDE_ALGORITHM Mount::GetXGuideAlgorithmSelection() const
{
    return GetGuideAlgorithm(m_pXGuideAlgorithm);
}

void Mount::SetXGuideAlgorithm(int guideAlgorithm)
{
    if (!m_pXGuideAlgorithm || m_pXGuideAlgorithm->Algorithm() != guideAlgorithm)
    {
        // a move can be using the algorithm on the worker: the swap waits for it (#142 T2b)
        AlgorithmSlot::Hold held = LockAlgorithms();
        delete m_pXGuideAlgorithm;

        if (guideAlgorithm == GUIDE_ALGORITHM_NONE)
            guideAlgorithm = DefaultXGuideAlgorithm();

        if (CreateGuideAlgorithm(guideAlgorithm, this, GUIDE_X, &m_pXGuideAlgorithm))
        {
            GUIDE_ALGORITHM defaultAlgorithm = DefaultXGuideAlgorithm();
            CreateGuideAlgorithm(defaultAlgorithm, this, GUIDE_X, &m_pXGuideAlgorithm);
            guideAlgorithm = defaultAlgorithm;
        }

        pConfig->Profile.SetInt("/" + GetMountClassName() + "/XGuideAlgorithm", guideAlgorithm);
    }
}

GUIDE_ALGORITHM Mount::GetYGuideAlgorithmSelection() const
{
    return GetGuideAlgorithm(m_pYGuideAlgorithm);
}

void Mount::SetYGuideAlgorithm(int guideAlgorithm)
{
    if (!m_pYGuideAlgorithm || m_pYGuideAlgorithm->Algorithm() != guideAlgorithm)
    {
        // a move can be using the algorithm on the worker: the swap waits for it (#142 T2b)
        AlgorithmSlot::Hold held = LockAlgorithms();
        delete m_pYGuideAlgorithm;

        if (guideAlgorithm == GUIDE_ALGORITHM_NONE)
            guideAlgorithm = DefaultYGuideAlgorithm();

        if (CreateGuideAlgorithm(guideAlgorithm, this, GUIDE_Y, &m_pYGuideAlgorithm))
        {
            GUIDE_ALGORITHM defaultAlgorithm = DefaultYGuideAlgorithm();
            CreateGuideAlgorithm(defaultAlgorithm, this, GUIDE_Y, &m_pYGuideAlgorithm);
            guideAlgorithm = defaultAlgorithm;
        }

        pConfig->Profile.SetInt("/" + GetMountClassName() + "/YGuideAlgorithm", guideAlgorithm);
    }
}

static void NotifyGuidingEnabled(GuideAlgorithm *xAlgo, GuideAlgorithm *yAlgo, bool enabled)
{
    if (enabled)
    {
        if (xAlgo)
            xAlgo->GuidingEnabled();

        if (yAlgo)
            yAlgo->GuidingEnabled();
    }
    else
    {
        if (xAlgo)
            xAlgo->GuidingDisabled();

        if (yAlgo)
            yAlgo->GuidingDisabled();
    }
}

void Mount::SetGuidingEnabled(bool guidingEnabled)
{
    if (guidingEnabled != m_guidingEnabled)
    {
        const char *s = IsStepGuider() ? "AOGuidingEnabled" : "MountGuidingEnabled";
        Debug.Write(wxString::Format("%s: %d\n", s, guidingEnabled));
        pFrame->NotifyGuidingParam(s, guidingEnabled);
        m_guidingEnabled = guidingEnabled;
        {
            AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)
            NotifyGuidingEnabled(m_pXGuideAlgorithm, m_pYGuideAlgorithm, guidingEnabled);
        }
        if (guidingEnabled)
        {
            // avoid sending false positive alerts after guiding is
            // re-enabled
            DeferPulseLimitAlertCheck();
        }
    }
}

void Mount::DeferPulseLimitAlertCheck() { }

GUIDE_ALGORITHM Mount::GetGuideAlgorithm(const GuideAlgorithm *pAlgorithm)
{
    return pAlgorithm ? pAlgorithm->Algorithm() : GUIDE_ALGORITHM_NONE;
}

static GuideAlgorithm *MakeGaussianProcessGuideAlgo(Mount *mount, GuideAxis axis)
{
    static bool s_gp_debug_inited;
    if (!s_gp_debug_inited)
    {
        class PHD2DebugLogger : public GPDebug
        {
            void Log(const char *format, ...)
            {
                va_list ap;
                va_start(ap, format);
                Debug.Write(wxString::FormatV(format + wxString("\n"), ap));
                va_end(ap);
            }
            void Write(const char *what) { Debug.Write(wxString(what) + _T("\n")); }
        };
        GPDebug::SetGPDebug(new PHD2DebugLogger());
        s_gp_debug_inited = true;
    }
    return new GuideAlgorithmGaussianProcess(mount, axis);
}

bool Mount::CreateGuideAlgorithm(int guideAlgorithm, Mount *mount, GuideAxis axis, GuideAlgorithm **ppAlgorithm)
{
    bool error = false;

    try
    {
        switch (guideAlgorithm)
        {
        case GUIDE_ALGORITHM_NONE:
        case GUIDE_ALGORITHM_IDENTITY:
            *ppAlgorithm = new GuideAlgorithmIdentity(mount, axis);
            break;
        case GUIDE_ALGORITHM_HYSTERESIS:
            *ppAlgorithm = new GuideAlgorithmHysteresis(mount, axis);
            break;
        case GUIDE_ALGORITHM_LOWPASS:
            *ppAlgorithm = new GuideAlgorithmLowpass(mount, axis);
            break;
        case GUIDE_ALGORITHM_LOWPASS2:
            *ppAlgorithm = new GuideAlgorithmLowpass2(mount, axis);
            break;
        case GUIDE_ALGORITHM_RESIST_SWITCH:
            *ppAlgorithm = new GuideAlgorithmResistSwitch(mount, axis);
            break;
        case GUIDE_ALGORITHM_GAUSSIAN_PROCESS:
            *ppAlgorithm = MakeGaussianProcessGuideAlgo(mount, axis);
            break;
        case GUIDE_ALGORITHM_ZFILTER:
            *ppAlgorithm = new GuideAlgorithmZFilter(mount, axis);
            break;

        default:
            throw ERROR_INFO("invalid guideAlgorithm");
            break;
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        error = true;
    }

    return error;
}

#ifdef TEST_TRANSFORMS
/*
 * TestTransforms() is a routine to do some sanity checking on the transform routines, which
 * have been a source of a huge number of headaches involving getting the reverse transforms
 * to work
 *
 */
void Mount::TestTransforms()
{
    static bool bTested = false;

    if (!bTested)
    {
        for (int inverted = 0; inverted < 2; inverted++)
        {
            // we test every 15 degrees, starting at -195 degrees and
            // ending at 375 degrees

            for (int i = -13; i < 14; i++)
            {
                double xAngle = ((double) i) * M_PI / 12.0;
                double yAngle;

                yAngle = xAngle + M_PI / 2.0;

                if (inverted)
                {
                    yAngle -= M_PI;
                }

                // normalize the angles since real callers of setCalibration
                // will get the angle from atan2().

                xAngle = atan2(sin(xAngle), cos(xAngle));
                yAngle = atan2(sin(yAngle), cos(yAngle));

                Debug.Write(wxString::Format("xidx=%.2f, yIdx=%.2f\n", xAngle / M_PI * 180.0 / 15, yAngle / M_PI * 180.0 / 15));

                SetCalibration(xAngle, yAngle, 1.0, 1.0);

                for (int j = -13; j < 14; j++)
                {
                    double cameraAngle = ((double) i) * M_PI / 12.0;
                    cameraAngle = atan2(sin(cameraAngle), cos(cameraAngle));

                    PHD_Point p0(cos(cameraAngle), sin(cameraAngle));
                    assert(fabs((p0.X * p0.X + p0.Y * p0.Y) - 1.00) < 0.01);

                    double p0Angle = p0.Angle();
                    assert(fabs(cameraAngle - p0Angle) < 0.01);

                    PHD_Point p1, p2;

                    if (TransformCameraCoordinatesToMountCoordinates(p0, p1))
                    {
                        assert(false);
                    }

                    assert(fabs((p1.X * p1.X + p1.Y * p1.Y) - 1.00) < 0.01);

                    double adjustedCameraAngle = cameraAngle - xAngle;

                    double cosAdjustedCameraAngle = cos(adjustedCameraAngle);

                    // check that the X value is correct
                    assert(fabs(cosAdjustedCameraAngle - p1.X) < 0.01);

                    double sinAdjustedCameraAngle = sin(adjustedCameraAngle);

                    if (inverted)
                    {
                        sinAdjustedCameraAngle *= -1;
                    }
                    // and check that the Y value is correct;
                    assert(fabs(sinAdjustedCameraAngle - p1.Y) < 0.01);

                    if (TransformMountCoordinatesToCameraCoordinates(p1, p2))
                    {
                        assert(false);
                    }

                    double p2Angle = p2.Angle();
                    // assert(fabs(p0Angle - p2Angle) < 0.01);
                    assert(fabs((p2.X * p2.X + p2.Y * p2.Y) - 1.00) < 0.01);
                    assert(fabs(p0.X - p2.X) < 0.01);
                    assert(fabs(p0.Y - p2.Y) < 0.01);
                }
            }
        }

        bTested = true;
        ClearCalibration();
    }
}
#endif

Mount::Mount()
{
    m_connected = false;
    m_requestCount = 0;
    m_errorCount = 0;

    m_pYGuideAlgorithm = nullptr;
    m_pXGuideAlgorithm = nullptr;
    m_guidingEnabled = true;

    m_backlashComp = nullptr;
    m_lastStep.mount = this;
    m_lastStep.frameNumber = -1; // invalidate

    ClearCalibration();

#ifdef TEST_TRANSFORMS
    TestTransforms();
#endif
}

Mount::~Mount()
{
    delete m_pXGuideAlgorithm;
    delete m_pYGuideAlgorithm;
    delete m_backlashComp;
}

double Mount::yAngle() const
{
    return norm_angle(m_cal.xAngle - m_yAngleError + M_PI / 2.);
}

static wxString RotAngleStr(double rotAngle)
{
    if (rotAngle == Rotator::POSITION_UNKNOWN)
        return _("None");
    return wxString::Format("%.1f", rotAngle);
}

// The profile values under a mount's "/<mount class>/calibration/" prefix, for the calibration codec (#142)
class ProfileCalibrationStore : public calibration_codec::Store
{
    wxString m_prefix;

    wxString Key(const std::string& key) const { return m_prefix + wxString::FromUTF8(key.c_str()); }

public:
    explicit ProfileCalibrationStore(const wxString& mountClassName) : m_prefix("/" + mountClassName + "/calibration/") { }

    std::string GetString(const std::string& key, const std::string& dflt) override
    {
        return std::string(pConfig->Profile.GetString(Key(key), wxString::FromUTF8(dflt.c_str())).utf8_str());
    }
    double GetDouble(const std::string& key, double dflt) override { return pConfig->Profile.GetDouble(Key(key), dflt); }
    int GetInt(const std::string& key, int dflt) override { return pConfig->Profile.GetInt(Key(key), dflt); }
    void SetString(const std::string& key, const std::string& value) override
    {
        pConfig->Profile.SetString(Key(key), wxString::FromUTF8(value.c_str()));
    }
    void SetDouble(const std::string& key, double value) override { pConfig->Profile.SetDouble(Key(key), value); }
    void SetInt(const std::string& key, int value) override { pConfig->Profile.SetInt(Key(key), value); }
};

static calibration_codec::CalibrationRecord ToRecord(const Calibration& cal)
{
    calibration_codec::CalibrationRecord r;
    r.xRate = cal.xRate;
    r.yRate = cal.yRate;
    r.xAngle = cal.xAngle;
    r.yAngle = cal.yAngle;
    r.declination = cal.declination;
    r.rotatorAngle = cal.rotatorAngle;
    r.binning = cal.binning;
    r.pierSide = cal.pierSide;
    r.raGuideParity = cal.raGuideParity;
    r.decGuideParity = cal.decGuideParity;
    r.isValid = cal.isValid;
    r.timestamp = std::string(cal.timestamp.utf8_str());
    return r;
}

bool Mount::FlipCalibration()
{
    bool bError = false;

    try
    {
        if (!IsCalibrated())
        {
            throw ERROR_INFO("cannot flip if not calibrated");
        }

        double origX = xAngle();
        double origY = yAngle();

        bool decFlipRequired = CalibrationFlipRequiresDecFlip();

        Debug.Write(wxString::Format(
            "FlipCalibration before: x=%.1f, y=%.1f decFlipRequired=%d sideOfPier=%s rotAngle=%s parity=%s/%s\n",
            degrees(origX), degrees(origY), decFlipRequired, ::PierSideStr(m_cal.pierSide), RotAngleStr(m_cal.rotatorAngle),
            ParityStr(m_cal.raGuideParity), ParityStr(m_cal.decGuideParity)));

        Debug.Write(wxString::Format("FlipCalibration pre-normalize: x=%.1f, y=%.1f\n", degrees(origX + M_PI),
                                     degrees(decFlipRequired ? origY + M_PI : origY)));

        // x + pi, y (+ pi when decFlipRequired), normalized; the opposite side; the Dec parity inverted
        // unless decFlipRequired (calibration_adjust, #142)
        calibration_adjust::FlippedCalibration flipped =
            calibration_adjust::FlipCalibrationData(origX, origY, m_cal.pierSide, m_cal.decGuideParity, decFlipRequired);
        double newX = flipped.xAngle;
        double newY = flipped.yAngle;

        PierSide priorPierSide = m_cal.pierSide;
        PierSide newPierSide = (PierSide) flipped.pierSide;
        GuideParity newDecParity = (GuideParity) flipped.decGuideParity;

        Debug.Write(wxString::Format("FlipCalibration after: x=%.1f y=%.1f sideOfPier=%s parity=%s/%s\n", degrees(newX),
                                     degrees(newY), ::PierSideStr(newPierSide), ParityStr(m_cal.raGuideParity),
                                     ParityStr(newDecParity)));

        Calibration cal(m_cal);
        cal.xAngle = newX;
        cal.yAngle = newY;
        cal.pierSide = newPierSide;
        cal.decGuideParity = newDecParity;

        SetCalibration(cal);

        pFrame->StatusMsg(wxString::Format(_("CAL: %s(%.f,%.f)->%s(%.f,%.f)"), ::PierSideStrTr(priorPierSide, wxEmptyString),
                                           degrees(origX), degrees(origY), ::PierSideStrTr(newPierSide, wxEmptyString),
                                           degrees(newX), degrees(newY)));
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    return bError;
}

static S_HISTORY GuideHistoryEntry(const GuideStepInfo& step)
{
    S_HISTORY h;
    h.timestamp = ::wxGetUTCTimeMillis().GetValue();
    h.dx = step.cameraOffset.X;
    h.dy = step.cameraOffset.Y;
    h.ra = step.mountOffset.X;
    h.dec = step.mountOffset.Y;
    h.starSNR = step.starSNR;
    h.starMass = step.starMass;
    h.raDur = step.durationRA;
    h.decDur = step.durationDec;
    h.raDir = step.directionRA;
    h.decDir = step.directionDec;
    h.raLimited = step.raLimited;
    h.decLimited = step.decLimited;
    return h;
}

void Mount::LogGuideStepInfo()
{
    if (m_lastStep.frameNumber < 0)
        return;

    pFrame->UpdateStatusBarGuiderInfo(m_lastStep);
    GuideLog.GuideStep(m_lastStep);
    EvtServer.NotifyGuideStep(m_lastStep);

    if (m_lastStep.moveOptions & MOVEOPT_GRAPH)
    {
        pFrame->pGuideHistory->AppendData(GuideHistoryEntry(m_lastStep), PhdController::IsSettling());
        GuidingAssistant::NotifyGuideStep(m_lastStep);
    }

    m_lastStep.frameNumber = -1; // invalidate
}

Mount::MOVE_RESULT Mount::MoveOffset(GuiderOffset *ofs, unsigned int moveOptions)
{
    MOVE_RESULT result = MOVE_OK;

    try
    {
        double xDistance, yDistance;

        if (moveOptions & MOVEOPT_ALGO_DEDUCE)
        {
            {
                // the main thread can change or replace the algorithms (#142 T2b)
                AlgorithmSlot::Hold held = LockAlgorithms();
                xDistance = m_pXGuideAlgorithm ? m_pXGuideAlgorithm->deduceResult() : 0.0;
                yDistance = m_pYGuideAlgorithm ? m_pYGuideAlgorithm->deduceResult() : 0.0;
            }
            if (xDistance == 0.0 && yDistance == 0.0)
                return result;
            ofs->mountOfs.SetXY(xDistance, yDistance);

            Debug.Write(wxString::Format("Dead-reckoning move xDistance=%.2f yDistance=%.2f\n", xDistance, yDistance));
        }
        else
        {
            // direct move or guide step

            if (!ofs->mountOfs.IsValid())
            {
                if (TransformCameraCoordinatesToMountCoordinates(ofs->cameraOfs, ofs->mountOfs, true))
                {
                    throw ERROR_INFO("Unable to transform camera coordinates");
                }
            }

            xDistance = ofs->mountOfs.X;
            yDistance = ofs->mountOfs.Y;

            Debug.Write(wxString::Format("Moving (%.2f, %.2f) raw xDistance=%.2f yDistance=%.2f\n", ofs->cameraOfs.X,
                                         ofs->cameraOfs.Y, xDistance, yDistance));

            // The guide computation, under the guard: the main thread can change or replace the algorithms and
            // the BLC while this runs on the worker (#142 T2b). Not held across the MoveAxis calls below
            AlgorithmSlot::Hold held = LockAlgorithms();

            // Let BLC track the raw offsets in Dec
            if (m_backlashComp)
                m_backlashComp->TrackBLCResults(moveOptions, yDistance);

            if (moveOptions & MOVEOPT_ALGO_RESULT)
            {
                // Feed the raw distances to the guide algorithms
                if (m_pXGuideAlgorithm)
                {
                    xDistance = m_pXGuideAlgorithm->result(xDistance);
                }

                if (m_pYGuideAlgorithm)
                {
                    yDistance = m_pYGuideAlgorithm->result(yDistance);
                }
            }
        }

        // Figure out the guide directions and durations based on the (possibly) updated distances
        guide_move_plan::OffsetPlan plan = guide_move_plan::PlanOffset(xDistance, yDistance, m_xRate, m_cal.yRate);
        GUIDE_DIRECTION xDirection = static_cast<GUIDE_DIRECTION>(plan.raDir);
        GUIDE_DIRECTION yDirection = static_cast<GUIDE_DIRECTION>(plan.decDir);

        int requestedXAmount = plan.raMs;
        MoveResultInfo xMoveResult;
        // Time the moves: the worker starts the next exposure only after they return (#119)
        wxStopWatch moveWatch;
        MOVE_RESULT xResult = MoveAxis(xDirection, requestedXAmount, moveOptions, &xMoveResult);
        long raMoveMs = moveWatch.Time();

        MoveResultInfo yMoveResult;
        int requestedYAmount = plan.decMs;
        MOVE_RESULT yResult = MOVE_OK;
        if (guide_move_plan::SendDecAfter(static_cast<guide_move_plan::MoveResult>(xResult)))
        {
            if (m_backlashComp)
            {
                // the BLC's state, not the pulse: MoveAxis is outside the guard (#142 T2b)
                AlgorithmSlot::Hold held = LockAlgorithms();
                m_backlashComp->ApplyBacklashComp(moveOptions, yDistance, &requestedYAmount);
            }

            yResult = MoveAxis(yDirection, requestedYAmount, moveOptions, &yMoveResult);
        }
        else
        {
            // the RA move failed: the frame is old, the next one corrects Dec (SendDecAfter, #294)
            Debug.Write(wxString::Format("Dec move not sent: the RA move returned %d\n", xResult));
        }
        result = static_cast<MOVE_RESULT>(guide_move_plan::OffsetResult(static_cast<guide_move_plan::MoveResult>(xResult),
                                                                        static_cast<guide_move_plan::MoveResult>(yResult)));

        long moveMs = moveWatch.Time();
        Debug.Write(
            wxString::Format("guide step move time %ld ms (RA %ld ms, Dec %ld ms)\n", moveMs, raMoveMs, moveMs - raMoveMs));

        // What the mount moved of the distances: MoveAxis can cut a pulse or send none (guide_move_plan.cpp,
        // #165); a dither's fast recenter counts only this
        double xMoved = guide_move_plan::MovedPx(xDistance, requestedXAmount, xMoveResult.amountMoved, m_xRate);
        double yMoved = guide_move_plan::MovedPx(yDistance, requestedYAmount, yMoveResult.amountMoved, m_cal.yRate);
        ofs->mountMoved.SetXY(std::copysign(xMoved, xDistance), std::copysign(yMoved, yDistance));

        // Record the info about the guide step. The info will be picked up back in the main UI thread.
        // We don't want to do anything with the info here in the worker thread since UI operations are
        // not allowed outside the main UI thread.

        GuideStepInfo& info = m_lastStep;

        info.moveOptions = moveOptions;
        info.frameNumber = pFrame->m_frameCounter;
        info.time = pFrame->TimeSinceGuidingStarted();
        info.cameraOffset = ofs->cameraOfs;
        info.mountOffset = ofs->mountOfs;
        info.guideDistanceRA = xDistance;
        info.guideDistanceDec = yDistance;
        info.durationRA = xMoveResult.amountMoved;
        info.directionRA = xDirection;
        info.durationDec = yMoveResult.amountMoved;
        info.directionDec = yDirection;
        info.raLimited = xMoveResult.limited;
        info.decLimited = yMoveResult.limited;
        info.moveTimeMs = (int) moveMs;
        info.aoPos = GetAoPos();
        const Star& star = pFrame->pGuider->PrimaryStar();
        info.starMass = star.Mass;
        info.starSNR = star.SNR;
        info.starHFD = star.HFD;
        info.avgDist = pFrame->CurrentGuideError();
        info.starError = star.GetError();
    }
    catch (const wxString& errMsg)
    {
        POSSIBLY_UNUSED(errMsg);
        if (result == MOVE_OK)
            result = MOVE_ERROR;
    }

    return result;
}

/*
 * The transform code has proven really tricky to get right.  For future generations
 * (and for me the next time I try to work on it), I'm going to put some notes here.
 *
 * The goal of TransformCameraCoordinatesToMountCoordinates is to transform a camera
 * pixel coordinate into an x and y, and TransformMountCoordinatesToCameraCoordinates
 * does the reverse, converting a mount x and y into pixels coordinates.
 *
 * Note: If a mount's x and y axis are not perfectly perpendicular, the reverse transform
 * will not be able to accurately reverse the forward transform. The amount of inaccuracy
 * depends upon the perpendicular error.
 *
 * The mount is calibrated but moving 1 axis, then the other, watching where the mount
 * started and stopped.  Looking at the coordinates of the start and stop point, we
 * can compute the angle and the speed.
 *
 * The original PHD code used cos() against both the angles, but that code had issues
 * with sign reversal for some of the reverse transforms.  I spent some time looking
 * at that code (OK, a lot of time) and I never managed to get it to work, so I
 * rewrote it.
 *
 * After calibration, we use the x axis angle, and compute the y axis error by
 * subtracting 90 degrees.  If the mount was perfectly perpendicular, the error
 * will either be 0 or 180, depending on whether the axis motion was reversed
 * during calibration.
 *
 * I put this quote in when I was trying to fix the cos/cos code. Hopefully
 * since completely rewrote that code, I understand the new code. But I'm going
 * to leave it here as it still seems relevant, given the amount of trouble
 * I had getting this code to work.
 *
 * In the words of Stevie Wonder, noted Computer Scientist and singer/songwriter
 * of some repute:
 *
 * "When you believe in things that you don't understand
 * Then you suffer
 * Superstition ain't the way"
 *
 * As part of trying to get the transforms to work, I wrote a routine called TestTransforms()
 * which does a bunch of forward/reverse transform pairs, checking the results as it goes.
 *
 * If you ever have change the transform functions, it would be wise to #define TEST_TRANSFORMS
 * to make sure that you (at least) didn't break anything that TestTransforms() checks for.
 *
 */

bool Mount::TransformCameraCoordinatesToMountCoordinates(const PHD_Point& cameraVectorEndpoint, PHD_Point& mountVectorEndpoint,
                                                         bool logged)
{
    bool bError = false;

    try
    {
        if (!cameraVectorEndpoint.IsValid())
        {
            throw ERROR_INFO("invalid cameraVectorEndPoint");
        }

        // Convert theta and hyp into X and Y (pure math in calibration_transform)
        calibration_transform::CamToMount r =
            calibration_transform::CameraToMount(cameraVectorEndpoint.X, cameraVectorEndpoint.Y, m_cal.xAngle, m_yAngleError);

        mountVectorEndpoint.SetXY(r.mount.x, r.mount.y);

        if (logged)
        {
            Debug.Write(wxString::Format("CameraToMount -- cameraTheta (%.2f) - m_xAngle (%.2f) = xAngle (%.2f = %.2f)\n",
                                         r.cameraTheta, m_cal.xAngle, r.xAngle, norm_angle(r.xAngle)));
            Debug.Write(wxString::Format(
                "CameraToMount -- cameraTheta (%.2f) - (m_xAngle (%.2f) + m_yAngleError (%.2f)) = yAngle (%.2f = %.2f)\n",
                r.cameraTheta, m_cal.xAngle, m_yAngleError, r.yAngle, norm_angle(r.yAngle)));
            Debug.Write(wxString::Format("CameraToMount -- cameraX=%.2f cameraY=%.2f hyp=%.2f cameraTheta=%.2f mountX=%.2f "
                                         "mountY=%.2f, mountTheta=%.2f\n",
                                         cameraVectorEndpoint.X, cameraVectorEndpoint.Y, r.hyp, r.cameraTheta,
                                         mountVectorEndpoint.X, mountVectorEndpoint.Y, mountVectorEndpoint.Angle()));
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        mountVectorEndpoint.Invalidate();
    }

    return bError;
}

bool Mount::TransformMountCoordinatesToCameraCoordinates(const PHD_Point& mountVectorEndpoint, PHD_Point& cameraVectorEndpoint,
                                                         bool logged)
{
    bool bError = false;

    try
    {
        if (!mountVectorEndpoint.IsValid())
        {
            throw ERROR_INFO("invalid mountVectorEndPoint");
        }

        calibration_transform::MountToCam r =
            calibration_transform::MountToCamera(mountVectorEndpoint.X, mountVectorEndpoint.Y, m_cal.xAngle, m_yAngleError);

        cameraVectorEndpoint.SetXY(r.camera.x, r.camera.y);

        if (logged)
        {
            Debug.Write(wxString::Format("MountToCamera -- mountTheta (%.2f) + m_xAngle (%.2f) = xAngle (%.2f = %.2f)\n",
                                         r.mountTheta, m_cal.xAngle, r.xAngle, norm_angle(r.xAngle)));
            Debug.Write(wxString::Format("MountToCamera -- mountX=%.2f mountY=%.2f hyp=%.2f mountTheta=%.2f cameraX=%.2f, "
                                         "cameraY=%.2f cameraTheta=%.2f\n",
                                         mountVectorEndpoint.X, mountVectorEndpoint.Y, r.hyp, r.mountTheta,
                                         cameraVectorEndpoint.X, cameraVectorEndpoint.Y, cameraVectorEndpoint.Angle()));
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        cameraVectorEndpoint.Invalidate();
    }

    return bError;
}

bool Mount::DecCompensationEnabled() const
{
    return false;
}

/*
 * Adjust the calibration data for the scope's current coordinates.
 *
 * This includes adjusting the xRate to compensate for changes in declination
 * relative to the declination where calibration was done, and possibly flipping
 * the calibration data if the mount is known to be on the other side of the
 * pier from where calibration was done.
 */
void Mount::AdjustCalibrationForScopePointing()
{
    double newDeclination = pPointingSource->GetDeclinationRadians();
    PierSide newPierSide = pPointingSource->SideOfPier();
    double newRotatorAngle = Rotator::RotatorPosition();
    unsigned short binning = pCamera->GetBinning();

    Debug.AddLine(wxString::Format(
        "AdjustCalibrationForScopePointing (%s): current dec=%s pierSide=%d, cal dec=%s pierSide=%d rotAngle=%s bin=%hu",
        GetMountClassName(), DeclinationStr(newDeclination), newPierSide, DeclinationStr(m_cal.declination), m_cal.pierSide,
        RotAngleStr(newRotatorAngle), binning));

    // The decisions and the arithmetic are calibration_adjust's (#142): PlanPointing from the calibration as guiding
    // starts, FinishPointing from the calibration after the flip. The alerts, the Debug lines and the calls stay here.
    calibration_adjust::PointingInput in;
    in.cal = ToRecord(m_cal);
    in.xRate = m_xRate;
    in.newDeclination = newDeclination;
    in.newPierSide = newPierSide;
    in.newRotatorAngle = newRotatorAngle;
    in.binning = binning;

    // See if the user has changed mount guide speeds after the last calibration.  If so, raise an alert that can't be avoided
    if (pPointingSource->CanReportPosition())
    {
        CalibrationDetails calDetails;
        LoadCalibrationDetails(&calDetails);

        if (calDetails.raGuideSpeed > 0 && calDetails.decGuideSpeed > 0)
        {
            double currRASpeed;
            double currDecSpeed;
            if (!pPointingSource->GetGuideRates(&currRASpeed, &currDecSpeed))
            {
                in.compareGuideSpeeds = true;
                in.calRaGuideSpeed = calDetails.raGuideSpeed;
                in.calDecGuideSpeed = calDetails.decGuideSpeed;
                in.raGuideSpeed = currRASpeed;
                in.decGuideSpeed = currDecSpeed;
                if (calibration_adjust::GuideSpeedsChanged(calDetails.raGuideSpeed, calDetails.decGuideSpeed, currRASpeed,
                                                           currDecSpeed))
                {
                    pFrame->Alert(_("Mount guide speeds are different from those used in last calibration.  Do a new "
                                    "calibration or reset mount guide speed settings to previous values. "));
                    Debug.Write(
                        wxString::Format("Guide speeds have changed since calibration.  Orig RA = %0.1f, Orig Dec = %0.1f, "
                                         "Curr RA = %0.1f, Curr Dec = %0.1f, Units are arc-sec/sec\n",
                                         calDetails.raGuideSpeed * 3600.0, calDetails.decGuideSpeed * 3600.0,
                                         currRASpeed * 3600.0, currDecSpeed * 3600.0));
                }
            }
        }
    }

    in.cameraPixelSize = pCamera->GetCameraPixelSize();
    in.profilePixelSize = GuideCamera::GetProfilePixelSize();
    calibration_adjust::PointingPlan plan = calibration_adjust::PlanPointing(in);

    if (plan.lacksPierSide)
    {
        pFrame->Alert(_("Current calibration did not have side-of-pier information, so PHD2 can't automatically correct for "
                        "meridian flips. "
                        "You should do a fresh calibration to correct this problem."));
    }

    // Compensate for binning or pixel size changes. At least one cam driver (ASCOM/Lodestar) can lie about the binning while
    // changing the reported pixel size
    if (plan.pixelSizeChanged)
    {
        // Punt on this, it probably means the user-supplied pixel size doesn't match what the camera is reporting
        pFrame->Alert(
            _("Profile pixel size doesn't match camera-reported pixel size.  Re-calibrate to restore correct guiding."));
        Debug.Write(wxString::Format("Camera pixel size changed from %0.1f to %0.1f\n", GuideCamera::GetProfilePixelSize(),
                                     pCamera->GetCameraPixelSize()));
        pCamera->SetCameraPixelSize(pCamera->GetCameraPixelSize());
    }

    // The following won't happen unless the camera binning is changed outside the AD UI. Goal is to revert to baseline guiding
    // params while keeping the UI consistent with the apparent image scale
    if (plan.binning.apply)
    {
        Calibration cal(m_cal);

        cal.xRate = plan.binning.xRate;
        cal.yRate = plan.binning.yRate;
        cal.binning = binning;

        Debug.Write(wxString::Format("Binning %hu -> %hu, rates (%.3f, %.3f) -> (%.3f, %.3f)\n", m_cal.binning, binning,
                                     m_cal.xRate * 1000., m_cal.yRate * 1000., cal.xRate * 1000., cal.yRate * 1000.));

        SetCalibration(cal);
    }

    // If the image scale has changed, make some other adjustments
    if (plan.imageScaleChanged)
    {
        Debug.Write("Mount::AdjustCalibrationForScopePointing: imageScaleRatio changed\n");
        pFrame->HandleImageScaleChange(); // Clear calibration, get the various UIs sorted out
    }

    if (plan.flip)
    {
        Debug.AddLine(wxString::Format("Guiding starts on opposite side of pier: calibration data "
                                       "side is %s, current side is %s",
                                       ::PierSideStr(m_cal.pierSide), ::PierSideStr(newPierSide)));
        FlipCalibration();
    }

    calibration_adjust::PointingFinish finish = calibration_adjust::FinishPointing(
        ToRecord(m_cal), newDeclination, newRotatorAngle, DecCompensationEnabled(), Scope::DEC_COMP_LIMIT);

    switch (finish.rotator)
    {
    case calibration_adjust::RotatorAction::None:
        break;
    case calibration_adjust::RotatorAction::UnknownAtCalibration:
        // we do not know the rotator position at calibration time so cannot automatically adjust calibration
        pFrame->Alert(_("Rotator position has changed, recalibration is needed."));
        m_cal.rotatorAngle = newRotatorAngle;
        break;
    case calibration_adjust::RotatorAction::Rotate:
    {
        Debug.Write(wxString::Format("New rotator position %.1f deg, prev = %.1f deg, delta = %.1f deg\n", newRotatorAngle,
                                     m_cal.rotatorAngle, finish.rotatorDeltaDeg));

        Calibration cal(m_cal);
        cal.xAngle = finish.xAngle;
        cal.yAngle = finish.yAngle;
        cal.rotatorAngle = newRotatorAngle;

        SetCalibration(cal);
        break;
    }
    }

    // compensate RA guide rate for declination if the declination changed and we know both the
    // calibration declination and the current declination.  This must be done after all other adjustments that may
    // update the calibration data in the registry.  The adjusted x_Rate is never persisted
    // A Dec that moved by no more than a driver's rounding has not changed, and above the Dec comp limit only a move
    // that changes the RA rate by more than 5% needs a new calibration (#157): calibration_transform::DecideDecComp.
    bool deccomp = false;

    switch (finish.decComp)
    {
    case calibration_transform::DecComp::Alert:
        // avoid division by zero and gross errors.  If the user didn't calibrate
        // somewhere near the celestial equator, we don't do this
        Debug.AddLine("skipping Dec comp: initial calibration too far from equator");
        pFrame->Alert(_("Calibration was too far from equator, recalibration is needed."));
        break;
    case calibration_transform::DecComp::Compensate:
        if (!finish.decCompensated)
        {
            Debug.AddLine("skipping Dec comp: Dec Comp not enabled");
        }
        else
        {
            // Don't do a full dec comp too close to pole - xRate will become a huge number and will cause problems downstream:
            // finish.clampedDeclination is held within +-89 deg
            m_xRate = finish.xRate;
            deccomp = true;

            Debug.Write(wxString::Format("Dec comp: XRate %.3f -> %.3f for dec %.1f -> dec %.1f\n", m_cal.xRate * 1000.0,
                                         m_xRate * 1000.0, degrees(m_cal.declination), degrees(finish.clampedDeclination)));
        }
        break;
    case calibration_transform::DecComp::None:
        if (m_cal.declination != UNKNOWN_DECLINATION && newDeclination != UNKNOWN_DECLINATION &&
            newDeclination != m_cal.declination)
            Debug.Write(wxString::Format("skipping Dec comp: dec %.4f -> dec %.4f is not a meaningful change\n",
                                         degrees(m_cal.declination), degrees(newDeclination)));
        break;
    }
    if (!deccomp && m_xRate != m_cal.xRate)
    {
        Debug.Write(wxString::Format("No dec comp, asserted base xRate %.3f\n", m_cal.xRate * 1000.0));
        m_xRate = m_cal.xRate;
    }
}

void Mount::IncrementRequestCount()
{
    m_requestCount++;

    // for the moment we never enqueue requests if the mount is busy, but we can
    // enqueue them two at a time.  There is no reason we can't, it's just that
    // right now we don't, and this might catch an error
    assert(m_requestCount <= 2);
}

void Mount::DecrementRequestCount()
{
    assert(m_requestCount > 0);
    m_requestCount--;
}

bool Mount::HasNonGuiMove()
{
    return false;
}

bool Mount::SynchronousOnly()
{
    return false;
}

const wxString& Mount::Name() const
{
    return m_Name;
}

bool Mount::IsStepGuider() const
{
    return false;
}

wxPoint Mount::GetAoPos() const
{
    return wxPoint();
}

wxPoint Mount::GetAoMaxPos() const
{
    return wxPoint();
}

const char *Mount::DirectionStr(GUIDE_DIRECTION d) const
{
    // these are used internally in the guide log and event server and are not translated
    switch (d)
    {
    case NONE:
        return "None";
    case NORTH:
        return "North";
    case SOUTH:
        return "South";
    case EAST:
        return "East";
    case WEST:
        return "West";
    default:
        return "?";
    }
}

const char *Mount::DirectionChar(GUIDE_DIRECTION d) const
{
    // these are used internally in the guide log and event server and are not translated
    switch (d)
    {
    case NONE:
        return "-";
    case NORTH:
        return "N";
    case SOUTH:
        return "S";
    case EAST:
        return "E";
    case WEST:
        return "W";
    default:
        return "?";
    }
}

wxString DumpMoveOptionBits(unsigned int moveOptions)
{
    if (!moveOptions)
        return "-";
    char buf[16];
    char *p = &buf[0];
    if (moveOptions & MOVEOPT_ALGO_RESULT)
        *p++ = 'A';
    if (moveOptions & MOVEOPT_ALGO_DEDUCE)
        *p++ = 'D';
    if (moveOptions & MOVEOPT_USE_BLC)
        *p++ = 'B';
    if (moveOptions & MOVEOPT_GRAPH)
        *p++ = 'G';
    if (moveOptions & MOVEOPT_MANUAL)
        *p++ = 'M';
    *p = 0;
    return buf;
}

bool Mount::IsCalibrated() const
{
    bool bReturn = false;

    if (IsConnected())
    {
        bReturn = m_calibrated;
    }

    return bReturn;
}

void Mount::ClearCalibration()
{
    m_calibrated = false;
    if (pFrame)
        pFrame->UpdateStatusBarCalibrationStatus();
}

void Mount::SetCalibration(const Calibration& cal)
{
    Debug.Write(wxString::Format("Mount::SetCalibration (%s) -- xAngle=%.1f yAngle=%.1f xRate=%.3f yRate=%.3f bin=%hu dec=%s "
                                 "pierSide=%d par=%s/%s rotAng=%s\n",
                                 GetMountClassName(), degrees(cal.xAngle), degrees(cal.yAngle), cal.xRate * 1000.0,
                                 cal.yRate * 1000.0, cal.binning, DeclinationStr(cal.declination), cal.pierSide,
                                 ParityStr(cal.raGuideParity), ParityStr(cal.decGuideParity), RotAngleStr(cal.rotatorAngle)));

    // we do the rates first, since they just get stored
    m_cal.xRate = cal.xRate;
    m_cal.yRate = cal.yRate;
    m_cal.binning = cal.binning;
    m_cal.declination = cal.declination;
    m_cal.pierSide = cal.pierSide;
    if (cal.raGuideParity != GUIDE_PARITY_UNCHANGED)
        m_cal.raGuideParity = cal.raGuideParity;
    if (cal.decGuideParity != GUIDE_PARITY_UNCHANGED)
        m_cal.decGuideParity = cal.decGuideParity;
    m_cal.rotatorAngle = cal.rotatorAngle;
    m_cal.isValid = true;

    m_xRate = cal.xRate;

    // the angles are more difficult because we have to turn yAngle into a yError.
    m_cal.xAngle = cal.xAngle;
    m_cal.yAngle = cal.yAngle;
    m_yAngleError = calibration_transform::YAngleError(cal.xAngle, cal.yAngle);

    Debug.AddLine(wxString::Format("Mount::SetCalibration (%s) -- sets m_xAngle=%.1f m_yAngleError=%.1f", GetMountClassName(),
                                   degrees(m_cal.xAngle), degrees(m_yAngleError)));

    m_calibrated = true;

    if (pFrame)
        pFrame->UpdateStatusBarCalibrationStatus();

    // store calibration data (calibration_codec, #142)
    ProfileCalibrationStore store(GetMountClassName());
    calibration_codec::SaveCalibration(store, ToRecord(m_cal), std::string(wxDateTime::Now().Format().utf8_str()));
}

void Mount::SaveCalibrationDetails(const CalibrationDetails& calDetails) const
{
    bool guideSpeedsOk = true;

    if (pPointingSource)
    {
        if (!(calDetails.raGuideSpeed == -1.0 && calDetails.decGuideSpeed == -1.0))
        {
            if (!pPointingSource->ValidGuideRates(calDetails.raGuideSpeed, calDetails.decGuideSpeed))
            {
                guideSpeedsOk = false;
            }
        }
    }
    if (!guideSpeedsOk)
        Debug.Write("Bogus guide speeds over-written in SaveCalibrationDetails\n");

    calibration_codec::DetailsRecord det;
    det.focalLength = calDetails.focalLength;
    det.imageScale = calDetails.imageScale;
    det.raGuideSpeed = calDetails.raGuideSpeed;
    det.decGuideSpeed = calDetails.decGuideSpeed;
    det.orthoError = calDetails.orthoError;
    det.origBinning = calDetails.origBinning;
    for (const wxRealPoint& p : calDetails.raSteps)
        det.raSteps.push_back(calibration_codec::StepPoint { p.x, p.y });
    for (const wxRealPoint& p : calDetails.decSteps)
        det.decSteps.push_back(calibration_codec::StepPoint { p.x, p.y });
    det.raStepCount = calDetails.raStepCount;
    det.decStepCount = calDetails.decStepCount;
    det.lastIssue = (int) calDetails.lastIssue;
    det.origTimestamp = std::string(calDetails.origTimestamp.utf8_str());
    det.origPierSide = calDetails.origPierSide;

    // the profile write is calibration_codec's (#142)
    ProfileCalibrationStore store(GetMountClassName());
    calibration_codec::SaveDetails(store, det, guideSpeedsOk);
}

void Mount::NotifyGuidingStarted()
{
    Debug.Write("Mount: notify guiding started\n");

    AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)

    if (m_pXGuideAlgorithm)
        m_pXGuideAlgorithm->GuidingStarted();

    if (m_pYGuideAlgorithm)
        m_pYGuideAlgorithm->GuidingStarted();
}

void Mount::NotifyGuidingStopped()
{
    Debug.Write("Mount: notify guiding stopped\n");

    {
        AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)

        if (m_pXGuideAlgorithm)
            m_pXGuideAlgorithm->GuidingStopped();

        if (m_pYGuideAlgorithm)
            m_pYGuideAlgorithm->GuidingStopped();
    }

    if (m_backlashComp)
        m_backlashComp->ResetBLCState(); // takes the guard itself
}

void Mount::NotifyGuidingPaused()
{
    Debug.Write("Mount: notify guiding paused\n");

    AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)

    if (m_pXGuideAlgorithm)
        m_pXGuideAlgorithm->GuidingPaused();

    if (m_pYGuideAlgorithm)
        m_pYGuideAlgorithm->GuidingPaused();
}

void Mount::NotifyGuidingResumed()
{
    Debug.Write("Mount: notify guiding resumed\n");

    AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)

    if (m_pXGuideAlgorithm)
        m_pXGuideAlgorithm->GuidingResumed();

    if (m_pYGuideAlgorithm)
        m_pYGuideAlgorithm->GuidingResumed();
}

void Mount::NotifyGuidingDithered(double dx, double dy, bool mountCoords)
{
    Debug.Write(wxString::Format("Mount: notify guiding dithered (%.1f, %.1f)\n", dx, dy));

    if (!mountCoords)
    {
        PHD_Point cam(dx, dy);
        PHD_Point mnt;
        bool err = TransformCameraCoordinatesToMountCoordinates(cam, mnt, false);
        if (!err)
        {
            dx = mnt.X;
            dy = mnt.Y;
        }
    }

    AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)

    if (m_pXGuideAlgorithm)
        m_pXGuideAlgorithm->GuidingDithered(dx);

    if (m_pYGuideAlgorithm)
        m_pYGuideAlgorithm->GuidingDithered(dy);
}

void Mount::NotifyGuidingDitherSettleDone(bool success)
{
    Debug.Write(wxString::Format("Mount: notify guiding dither settle done success=%d\n", success));
    AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)
    if (m_pXGuideAlgorithm)
        m_pXGuideAlgorithm->GuidingDitherSettleDone(success);
    if (m_pYGuideAlgorithm)
        m_pYGuideAlgorithm->GuidingDitherSettleDone(success);
}

void Mount::NotifyDirectMove(const PHD_Point& dist)
{
    Debug.Write(wxString::Format("Mount: notify direct move %.2f,%.2f\n", dist.X, dist.Y));
    AlgorithmSlot::Hold held = LockAlgorithms(); // a move can be computing on the worker (#142 T2b)
    if (m_pXGuideAlgorithm)
        m_pXGuideAlgorithm->DirectMoveApplied(dist.X);
    if (m_pYGuideAlgorithm)
        m_pYGuideAlgorithm->DirectMoveApplied(dist.Y);
}

void Mount::GetLastCalibration(Calibration *cal) const
{
    // the profile read is calibration_codec's (#142)
    ProfileCalibrationStore store(GetMountClassName());
    calibration_codec::CalibrationRecord r;
    if (calibration_codec::LoadCalibration(store, &r))
    {
        cal->xRate = r.xRate;
        cal->yRate = r.yRate;
        cal->binning = r.binning;
        cal->xAngle = r.xAngle;
        cal->yAngle = r.yAngle;
        cal->declination = r.declination;
        cal->pierSide = (PierSide) r.pierSide;
        cal->raGuideParity = (GuideParity) r.raGuideParity;
        cal->decGuideParity = (GuideParity) r.decGuideParity;
        cal->rotatorAngle = r.rotatorAngle;
        cal->timestamp = wxString::FromUTF8(r.timestamp.c_str());
        cal->isValid = true;
    }
    else
    {
        cal->isValid = false;
    }
}

void Mount::LoadCalibrationDetails(CalibrationDetails *details) const
{
    // the profile read is calibration_codec's (#142); the saved guide speeds are checked once per pointing source
    std::function<bool(double, double)> validGuideRates;
    if (pPointingSource && !pPointingSource->m_CalDetailsValidated)
        validGuideRates = [](double ra, double dec) { return pPointingSource->ValidGuideRates(ra, dec); };

    ProfileCalibrationStore store(GetMountClassName());
    calibration_codec::DetailsRecord det;
    if (calibration_codec::LoadDetails(store, &det, validGuideRates))
    {
        // Need to prevent old bogus values in registry from being propagated
        Debug.Write("Bogus guide speeds cleared in LoadCalibrationDetails\n");
    }

    details->focalLength = det.focalLength;
    details->imageScale = det.imageScale;
    details->raGuideSpeed = det.raGuideSpeed;
    details->decGuideSpeed = det.decGuideSpeed;
    details->orthoError = det.orthoError;
    details->raStepCount = det.raStepCount;
    details->decStepCount = det.decStepCount;
    details->origBinning = det.origBinning;
    details->lastIssue = (CalibrationIssueType) det.lastIssue;
    details->origTimestamp = wxString::FromUTF8(det.origTimestamp.c_str());
    details->origPierSide = (PierSide) det.origPierSide;
    details->raSteps.clear();
    for (const calibration_codec::StepPoint& p : det.raSteps)
        details->raSteps.push_back(wxRealPoint(p.x, p.y));
    details->decSteps.clear();
    for (const calibration_codec::StepPoint& p : det.decSteps)
        details->decSteps.push_back(wxRealPoint(p.x, p.y));
}

bool Mount::Connect()
{
    m_connected = true;
    ResetErrorCount();

    if (pFrame)
        pFrame->UpdateStatusBarCalibrationStatus();

    return false;
}

bool Mount::Disconnect()
{
    m_connected = false;

    if (pFrame)
        pFrame->UpdateStatusBarCalibrationStatus();

    return false;
}

inline static wxString OrthoErrorStr(const CalibrationDetails& det)
{
    return det.IsValid() ? wxString::Format("%.1f deg", det.orthoError) : wxString("unknown");
}

wxString Mount::GetSettingsSummary() const
{
    // return a loggable summary of current mount settings

    wxString s = wxString::Format("%s = %s,%s connected, guiding %s, ", IsStepGuider() ? "AO" : "Mount", m_Name,
                                  IsConnected() ? "" : " not", m_guidingEnabled ? "enabled" : "disabled");

    if (pPointingSource && pPointingSource->IsConnected() && pPointingSource->CanReportPosition() &&
        pPointingSource != TheScope())
    {
        s += wxString::Format("AuxMount=%s, ", pPointingSource->Name());
    }

    if (IsCalibrated())
    {
        double xRatePx = xRate() * 1000.0;
        double yRatePx = yRate() * 1000.0;

        s += wxString::Format("xAngle = %.1f, xRate = %.3f, "
                              "yAngle = %.1f, yRate = %.3f, "
                              "parity = %s/%s\n",
                              degrees(xAngle()), xRatePx, degrees(yAngle()), yRatePx, ParityStr(m_cal.raGuideParity),
                              ParityStr(m_cal.decGuideParity));

        CalibrationDetails det;
        LoadCalibrationDetails(&det);

        double scale = det.IsValid() ? det.imageScale : 1.0;

        if (IsStepGuider())
        {
            s += wxString::Format("Norm rates X = %.f mas/step, "
                                  "Y = %.f mas/step; ortho.err. = %s\n",
                                  m_cal.xRate * 1000. * scale, m_cal.yRate * 1000. * scale, OrthoErrorStr(det));
        }
        else
        {
            double xRateAsD0;
            if (m_cal.declination != UNKNOWN_DECLINATION)
                xRateAsD0 = m_cal.xRate * 1000. / cos(m_cal.declination) * scale;
            else
                xRateAsD0 = m_cal.xRate * 1000. * scale;
            double yRateAs = yRatePx * scale;

            s += wxString::Format("Norm rates RA = %.1f\"/s @ dec 0, "
                                  "Dec = %.1f\"/s; ortho.err. = %s\n",
                                  xRateAsD0, yRateAs, OrthoErrorStr(det));
        }
    }
    else
        s += "not calibrated\n";

    // the worker's guide computation changes what the algorithms and the BLC report (#142 T2b)
    AlgorithmSlot::Hold held = LockAlgorithms();
    s += wxString::Format("X guide algorithm = %s, %s", GuideAlgorithmName(GetXGuideAlgorithmSelection()),
                          m_pXGuideAlgorithm->GetSettingsSummary()) +
        wxString::Format("Y guide algorithm = %s, %s", GuideAlgorithmName(GetYGuideAlgorithmSelection()),
                         m_pYGuideAlgorithm->GetSettingsSummary());

    if (m_backlashComp)
    {
        s += wxString::Format("Backlash comp = %s, pulse = %d ms\n", m_backlashComp->IsEnabled() ? "enabled" : "disabled",
                              m_backlashComp->GetBacklashPulseWidth());
    }

    return s;
}

bool Mount::CalibrationFlipRequiresDecFlip()
{
    return false;
}

bool Mount::IsDecDrifting() const
{
    return false;
}
