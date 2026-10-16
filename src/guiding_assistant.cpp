/*
 *  guiding_assistant.cpp
 *  PHD Guiding
 *
 *  Created by Andy Galasso and Bruce Waddington
 *  Copyright (c) 2015 Andy Galasso and Bruce Waddington
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
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
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
#include "guiding_assistant.h"
#include "backlash_comp.h"
#include "guiding_stats.h"
#include "guiding_assistant_math.h"
#include "optionsbutton.h"

#include <wx/textwrapper.h>
#include <wx/tokenzr.h>

#include <algorithm>
#include <memory>
#include <set>

struct GADetails
{
    wxString TimeStamp;
    wxString SNR;
    wxString StarMass;
    wxString SampleCount;
    wxString ElapsedTime;
    wxString ExposureTime;
    wxString RA_HPF_RMS;
    wxString Dec_HPF_RMS;
    wxString Total_HPF_RMS;
    wxString RAPeak;
    wxString RAPeak_Peak;
    wxString RADriftRate;
    wxString RAMaxDriftRate;
    wxString DriftLimitingExposure;
    wxString DecDriftRate;
    wxString DecPeak;
    wxString PAError;
    wxString BackLashInfo;
    wxString Dec_LF_DriftRate;
    wxString DecCorrectedRMS;
    wxString RecRAMinMove;
    wxString RecDecMinMove;
    std::vector<double> BLTNorthMoves;
    std::vector<double> BLTSouthMoves;
    int BLTMsmtPulse;
    wxString BLTAmount;
    wxString Recommendations;
};

inline static void StartRow(int& row, int& column)
{
    ++row;
    column = 0;
}

static void MakeBold(wxControl *ctrl)
{
    wxFont font = ctrl->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    ctrl->SetFont(font);
}

// ---------------------------------------------------------------------------
// Shared by the dialog and the headless (JSON-RPC) Guiding Assistant: the
// statistics and recommendation math live in guiding_assistant_math.cpp;
// these helpers gather the app-side inputs and format/log/apply the results.
// ---------------------------------------------------------------------------

static const int GA_MAX_HISTORY = 3;

// A guide step as an unguided GA sample, in mount-axis coordinates
static ga_math::Sample MakeGASample(const GuideStepInfo& info)
{
    double ra = info.mountOffset.X;
    double dec = info.mountOffset.Y;
    if (pMount->IsStepGuider())
    {
        PHD_Point mountLoc;
        TheScope()->TransformCameraCoordinatesToMountCoordinates(info.cameraOffset, mountLoc);
        ra = mountLoc.X;
        dec = mountLoc.Y;
    }
    ga_math::Sample s;
    s.ra = ra;
    s.dec = dec;
    s.time = info.time;
    s.wallSecs = wxGetCurrentTime();
    s.snr = info.starSNR;
    s.mass = info.starMass;
    return s;
}

// Running polar alignment error estimate from the linear-fit dec drift rate
static double GAPolarAlignError(const ga_math::Accumulator& stats, bool *decKnown)
{
    double declination = pPointingSource ? pPointingSource->GetDeclinationRadians() : UNKNOWN_DECLINATION;
    *decKnown = declination != UNKNOWN_DECLINATION;
    return ga_math::PolarAlignErrorArcmin(stats.DecDriftPxPerMin(), pFrame->GetCameraPixelScale(), *decKnown, declination);
}

static wxString FormatResultCell(double pxVal, double asVal, const wxString& units1, const wxString& units2,
                                 const wxString& extraInfo = wxEmptyString)
{
    return wxString::Format("%6.2f %s (%6.2f %s %s)", pxVal, units1, asVal, units2, extraInfo);
}

// The live measurement as the dialog displays it (and logs it / saves it in the GA history).
// The star-motion fields are only filled once there are at least 2 samples.
static void FormatGAResults(const ga_math::Accumulator& stats, const wxString& startStr, wxLongLong_t elapsedMs, GADetails *d)
{
    wxString SEC(_("s"));
    wxString PX(_("px"));
    wxString ARCSEC(_("arc-sec"));
    wxString ARCMIN(_("arc-min"));
    wxString PXPERMIN(_("px/min"));
    wxString PXPERSEC(_("px/sec"));
    wxString ARCSECPERMIN(_("arc-sec/min"));
    wxString ARCSECPERSEC(_("arc-sec/sec"));

    double n = (double) stats.Count();
    long elapsedSecs = (long) (elapsedMs / 1000);

    d->TimeStamp = startStr;
    d->ExposureTime = wxString::Format("%g%s", (double) pFrame->RequestedExposureDuration() / 1000.0, SEC);
    d->SNR = wxString::Format("%.1f", stats.AvgSnr());
    d->StarMass = wxString::Format("%.1f", stats.AvgMass());
    d->ElapsedTime = wxString::Format("%u%s", (unsigned int) elapsedSecs, SEC);
    d->SampleCount = wxString::Format("%.0f", n);

    if (n > 1)
    {
        double pxscale = pFrame->GetCameraPixelScale();
        double rarms = stats.RaHighFreqRms();
        double decrms = stats.DecHighFreqRms();
        double combined = stats.TotalHighFreqRms();
        double maxRateRA = stats.MaxRaRatePxPerSec();
        double decDriftPerMin = stats.DecDriftPxPerMin();
        bool decKnown;
        double alignmentError = GAPolarAlignError(stats, &decKnown);

        d->RA_HPF_RMS = FormatResultCell(rarms, rarms * pxscale, PX, ARCSEC);
        d->Dec_HPF_RMS = FormatResultCell(decrms, decrms * pxscale, PX, ARCSEC);
        d->Total_HPF_RMS = FormatResultCell(combined, combined * pxscale, PX, ARCSEC);
        d->RAPeak = FormatResultCell(stats.RaPeak(), stats.RaPeak() * pxscale, PX, ARCSEC);
        d->DecPeak = FormatResultCell(stats.DecPeak(), stats.DecPeak() * pxscale, PX, ARCSEC);
        double raPkPk = stats.RaPeakToPeak();
        d->RAPeak_Peak = FormatResultCell(raPkPk, raPkPk * pxscale, PX, ARCSEC);
        double raDriftRate = stats.RaDriftPxPerMin(elapsedSecs);
        d->RADriftRate = FormatResultCell(raDriftRate, raDriftRate * pxscale, PXPERMIN, ARCSECPERMIN);
        d->RAMaxDriftRate = FormatResultCell(maxRateRA, maxRateRA * pxscale, PXPERSEC, ARCSECPERSEC);
        d->DriftLimitingExposure = maxRateRA <= 0.0
            ? _(" ")
            : wxString::Format("%6.1f %s ", 1.3 * rarms / maxRateRA, SEC); // Will get revised when min-move is computed
        d->DecDriftRate = FormatResultCell(decDriftPerMin, decDriftPerMin * pxscale, PXPERMIN, ARCSECPERMIN);
        d->PAError = wxString::Format("%s %.1f %s", decKnown ? "" : "> ", alignmentError, ARCMIN);
    }
}

static wxString FormatRefinedDriftLimitingExposure(const ga_math::Recommendations& recs)
{
    return recs.driftLimitingExposure <= 0.0 ? _(" ") : wxString::Format("%6.1f %s ", recs.driftLimitingExposure, _("s"));
}

// Dump the raw statistics to the debug and guide logs
static void LogGAResults(const GADetails& d)
{
    wxString str;
    Debug.Write("Guiding Assistant results follow:\n");
    str = wxString::Format("SNR=%s, Samples=%s, Elapsed Time=%s, RA HPF-RMS=%s, Dec HPF-RMS=%s, Total HPF-RMS=%s\n", d.SNR,
                           d.SampleCount, d.ElapsedTime, d.RA_HPF_RMS, d.Dec_HPF_RMS, d.Total_HPF_RMS);
    GuideLog.NotifyGAResult(str);
    Debug.Write(str);
    str = wxString::Format("RA Peak=%s, RA Peak-Peak %s, RA Drift Rate=%s, Max RA Drift Rate=%s, Drift-Limiting Exp=%s\n",
                           d.RAPeak, d.RAPeak_Peak, d.RADriftRate, d.RAMaxDriftRate, d.DriftLimitingExposure);
    GuideLog.NotifyGAResult(str);
    Debug.Write(str);
    str = wxString::Format("Dec Drift Rate=%s, Dec Peak=%s, PA Error=%s\n", d.DecDriftRate, d.DecPeak, d.PAError);
    GuideLog.NotifyGAResult(str);
    Debug.Write(str);
}

// Backlash test result as shown in the "Declination Backlash" cell; *logStr gets the untranslated form
static wxString FormatBacklashResult(BacklashTool::MeasurementResults qual, int backlashMs, double backlashSigmaMs,
                                     double backlashPx, double backlashSigmaPx, wxString *logStr)
{
    auto pixelScale = pFrame->GetCameraPixelScale();
    double bltGearAngle = (backlashPx * pixelScale);
    double bltGearAngleSigma = (backlashSigmaPx * pixelScale);
    wxString preamble = ((backlashMs >= 5000 || qual == BacklashTool::MEASUREMENT_TOO_FEW_NORTH) ? ">=" : "");
    wxString outStrTr;
    if (qual == BacklashTool::MEASUREMENT_VALID)
    {
        *logStr = wxString::Format("%s %d  +/-  %0.0f ms (%0.1f  +/-  %0.1f arc-sec)", preamble, wxMax(0, backlashMs),
                                   backlashSigmaMs, wxMax(0, bltGearAngle), bltGearAngleSigma);
        outStrTr = wxString::Format("%s %d  +/-  %0.0f %s (%0.1f  +/-  %0.1f %s)", preamble, wxMax(0, backlashMs),
                                    backlashSigmaMs, _("ms"), wxMax(0, bltGearAngle), bltGearAngleSigma, _("arc-sec"));
    }
    else
    {
        *logStr = wxString::Format("%s %d  +/-  ms (test impaired)", preamble, wxMax(0, backlashMs));
        outStrTr = wxString::Format("%s %d  +/-  %s", preamble, wxMax(0, backlashMs), _("ms (test impaired)"));
    }
    return outStrTr;
}

// Gather the mount / camera / profile inputs and compute the recommendations
static ga_math::Recommendations ComputeGARecommendations(const ga_math::Accumulator& stats, bool origMultistarMode,
                                                         bool bltCompleted, int backlashMs, bool backlashExempted,
                                                         CalibrationDetails *calDetails)
{
    Scope *scope = TheScope();
    scope->LoadCalibrationDetails(calDetails);

    ga_math::RecommendationInputs in;
    in.pxScale = pFrame->GetCameraPixelScale();
    in.hpEncoders = pMount->HasHPEncoders();
    in.multiStar = origMultistarMode;
    in.smartDefaultDecMinMove =
        GuideAlgorithm::SmartDefaultMinMove(pFrame->GetFocalLength(), pCamera->GetCameraPixelSize(), pCamera->GetBinning());
    in.raHighFreqRms = stats.RaHighFreqRms();
    in.maxRaRatePxPerSec = stats.MaxRaRatePxPerSec();
    in.decDriftPxPerMin = stats.DecDriftPxPerMin();
    bool decKnown;
    in.polarAlignErrorArcmin = GAPolarAlignError(stats, &decKnown);
    in.avgSnr = stats.AvgSnr();
    in.starHfd = pFrame->pGuider->PrimaryStar().HFD;
    in.currBinning = pCamera->GetBinning();
    in.maxBinning = pCamera->MaxCombinedBinning();
    for (auto choice : pCamera->GetBinningChoices())
        in.binningChoices.push_back(choice.first);
    in.suspectCalibration = calDetails->lastIssue != CI_None || backlashExempted;
    GuideAlgorithm *raAlgo = pMount->GetXGuideAlgorithm();
    GuideAlgorithm *decAlgo = pMount->GetYGuideAlgorithm();
    in.raAlgoHasMinMove = raAlgo && raAlgo->GetMinMove() >= 0.0;
    in.decAlgoHasMinMove = decAlgo && decAlgo->GetMinMove() >= 0.0;
    in.decAlgoClass = decAlgo ? std::string(decAlgo->GetGuideAlgorithmClassName().ToStdString()) : std::string();
    in.bltCompleted = bltCompleted;
    in.backlashMs = backlashMs;

    int pulseSize = 0;
    int floor;
    int ceiling;
    scope->GetBacklashComp()->GetBacklashCompSettings(&pulseSize, &floor, &ceiling);
    in.likely.compPulseMs = pulseSize;
    DEC_GUIDE_MODE decMode = scope->GetDecGuideMode();
    in.likely.decModeAutoOrNone = decMode == DEC_AUTO || decMode == DEC_NONE;
    in.likely.calHasDecSteps = calDetails->decStepCount > 0 && !calDetails->decSteps.empty();
    in.likely.calNorthDist = in.likely.calSouthDist = 0.;
    if (in.likely.calHasDecSteps)
    {
        // How far the last calibration moved north, and how far back south
        wxRealPoint northStart = calDetails->decSteps[0];
        wxRealPoint northEnd = calDetails->decSteps[calDetails->decStepCount - 1];
        wxRealPoint southEnd = calDetails->decSteps[calDetails->decSteps.size() - 1];
        in.likely.calNorthDist = sqrt(pow(northStart.x - northEnd.x, 2) + pow(northStart.y - northEnd.y, 2));
        in.likely.calSouthDist = sqrt(pow(northEnd.x - southEnd.x, 2) + pow(northEnd.y - southEnd.y, 2));
    }

    ga_math::Recommendations recs = ga_math::Recommend(stats.DecAxis(), in);
    Debug.Write(wxString::Format("GA Min-Move recommendations (%s): Dec=%0.3f, RA=%0.3f, seeing estimate=%0.3fpx\n",
                                 recs.minMove.seeingBased ? "seeing-based" : "smart defaults", recs.minMove.dec,
                                 recs.minMove.ra, recs.minMove.seeingEstimatePx));
    return recs;
}

struct GARecommendation
{
    wxString which; // key in the saved GA history, e.g. "RAMinMove"
    wxString msg;
};

// The recommendation texts, in the order the dialog lists them
static std::vector<GARecommendation> BuildGARecommendations(const ga_math::Recommendations& r,
                                                            const CalibrationDetails& calDetails, int backlashMs)
{
    std::vector<GARecommendation> recs;

    // Always make a recommendation on exposure times
    if (!pMount->HasHPEncoders())
        recs.push_back(
            { "Exp",
              wxString::Format(_("Use exposure times in the range of %.1fs to %.1fs"), r.exposure.min, r.exposure.max) });
    else
        recs.push_back(
            { "Exp",
              wxString::Format(_("Use exposure times from %.1fs to %0.1fs and variable delay feature to control guide cadence"),
                               r.exposure.min, r.exposure.max) });

    if (r.recommendBinning)
        recs.push_back(
            { "Bin",
              wxString::Format(_("Make a new profile and set binning to %d. Use software binning if necessary"), r.binning) });

    // Previous calibration alert
    if (r.recalibrate)
    {
        wxString msg = _("Consider re-doing your calibration ");
        if (calDetails.lastIssue != CI_None)
            msg += _("(Prior alert)");
        else
            msg += _("(Backlash clearing)");
        recs.push_back({ "Cal", msg });
    }

    if (r.brighterStar)
        recs.push_back({ "Star", _("Consider using a brighter star for the test or increasing the exposure time") });

    if (r.pa == ga_math::PaAdvice::Improve)
        recs.push_back({ "PA", _("Polar alignment error > 5 arc-min; that could probably be improved.") });
    else if (r.pa == ga_math::PaAdvice::UseDriftAlign)
        recs.push_back({ "PA", _("Polar alignment error > 10 arc-min; try using the Drift Align tool to improve alignment.") });

    if (r.improveFocus)
        recs.push_back({ "StarHFD", _("Consider trying to improve focus on the guide camera") });

    if (r.raMinMove)
        recs.push_back({ "RAMinMove", wxString::Format(_("Try setting RA min-move to %0.2f"), r.minMove.ra) });

    if (r.decMinMove)
        recs.push_back({ "DecMinMove", wxString::Format(_("Try setting Dec min-move to %0.2f"), r.minMove.dec) });

    if (r.hasBacklash)
    {
        wxString msg;
        switch (r.backlash.advice)
        {
        case ga_math::BacklashAdvice::Encoders:
            msg = _("Mount has absolute encoders, no compensation needed");
            break;
        case ga_math::BacklashAdvice::Small:
            msg = _("Backlash is small, no compensation needed"); // assume it was a small measurement error
            break;
        case ga_math::BacklashAdvice::Compensate:
            msg = wxString::Format(_("Try starting with a Dec backlash compensation of %d ms"), r.backlash.recommendedMs);
            break;
        case ga_math::BacklashAdvice::TooLarge:
            msg = wxString::Format(_("Backlash is >= %d ms; you may need to guide in only one Dec direction (currently %s)"),
                                   backlashMs, r.singleDirection == ga_math::DecDirection::South ? _("South") : _("North"));
            break;
        }
        recs.push_back({ "BLT", msg });
    }

    if (r.decAlgoLowpass2)
        recs.push_back({ "DecAlgo", _("Try using Lowpass2 for Dec guiding") });

    return recs;
}

static void LogGARecommendations(const std::vector<GARecommendation>& recs)
{
    for (const GARecommendation& rec : recs)
    {
        wxString logStr = wxString::Format("Recommendation: %s\n", rec.msg);
        Debug.Write(logStr);
        if (rec.which != "Exp") // the exposure advice only goes to the debug log
            GuideLog.NotifyGAResult(logStr);
    }
}

static wxString JoinGARecommendations(const std::vector<GARecommendation>& recs)
{
    wxString all;
    for (const GARecommendation& rec : recs)
        all += rec.which + ":" + rec.msg + "\n";
    return all;
}

static void TrimGAHistory(bool FreshBLT, int HistoryDepth);

// Save the results from the most recent GA run in the profile
static void SaveGAHistory(const wxString& startStr, const GADetails& d, double decCorrectedRMS, double decDriftPerMin,
                          const ga_math::Recommendations& recs, int backlashMs, int backlashRecommendedMs,
                          const wxString& allRecommendations, BacklashTool *backlashTool)
{
    wxString prefix = "/GA/" + startStr;

    pConfig->Profile.SetString(prefix + "/timestamp", d.TimeStamp);
    pConfig->Profile.SetString(prefix + "/snr", d.SNR);
    pConfig->Profile.SetString(prefix + "/star_mass", d.StarMass);
    pConfig->Profile.SetString(prefix + "/sample_count", d.SampleCount);
    pConfig->Profile.SetString(prefix + "/elapsed_time", d.ElapsedTime);
    pConfig->Profile.SetString(prefix + "/exposure_time", d.ExposureTime);
    pConfig->Profile.SetString(prefix + "/ra_hpf_rms", d.RA_HPF_RMS);
    pConfig->Profile.SetString(prefix + "/dec_hpf_rms", d.Dec_HPF_RMS);
    pConfig->Profile.SetString(prefix + "/total_hpf_rms", d.Total_HPF_RMS);
    pConfig->Profile.SetString(prefix + "/ra_peak", d.RAPeak);
    pConfig->Profile.SetString(prefix + "/ra_peak_peak", d.RAPeak_Peak);
    pConfig->Profile.SetString(prefix + "/ra_drift_rate", d.RADriftRate);
    pConfig->Profile.SetString(prefix + "/ra_peak_drift_rate", d.RAMaxDriftRate);
    pConfig->Profile.SetString(prefix + "/ra_drift_exposure", d.DriftLimitingExposure);
    pConfig->Profile.SetString(prefix + "/dec_drift_rate", d.DecDriftRate);
    pConfig->Profile.SetString(prefix + "/dec_peak", d.DecPeak);
    pConfig->Profile.SetString(prefix + "/pa_error", d.PAError);
    pConfig->Profile.SetString(prefix + "/dec_corrected_rms", std::to_string(decCorrectedRMS));
    pConfig->Profile.SetString(prefix + "/backlash_info", d.BackLashInfo);
    pConfig->Profile.SetString(prefix + "/dec_lf_drift_rate", std::to_string(decDriftPerMin));
    pConfig->Profile.SetString(prefix + "/rec_ra_minmove", std::to_string(recs.minMove.ra));
    pConfig->Profile.SetString(prefix + "/rec_dec_minmove", std::to_string(recs.minMove.dec));
    if (backlashRecommendedMs > 0)
        pConfig->Profile.SetString(prefix + "/BLT_pulse", std::to_string(backlashMs));
    pConfig->Profile.SetString(prefix + "/recommendations", allRecommendations);
    bool freshBLT = backlashTool && backlashTool->IsGraphable(); // Just did a BLT that is viewable
    if (freshBLT)
    {
        pConfig->Profile.SetInt(prefix + "/BLT_MsmtPulse", backlashTool->GetBLTMsmtPulseSize());
        std::vector<double> northSteps = backlashTool->GetNorthSteps();
        std::vector<double> southSteps = backlashTool->GetSouthSteps();
        wxString stepStr = "";

        for (std::vector<double>::const_iterator it = northSteps.begin(); it != northSteps.end(); ++it)
        {
            stepStr += wxString::Format("%0.1f,", *it);
        }
        stepStr = stepStr.Left(stepStr.length() - 2);
        pConfig->Profile.SetString(prefix + "/BLT_North", stepStr);

        stepStr = "";

        for (std::vector<double>::const_iterator it = southSteps.begin(); it != southSteps.end(); ++it)
        {
            stepStr += wxString::Format("%0.1f,", *it);
        }
        stepStr = stepStr.Left(stepStr.length() - 2);
        pConfig->Profile.SetString(prefix + "/BLT_South", stepStr);
    }
    TrimGAHistory(freshBLT, GA_MAX_HISTORY);
}

// Applying recommendations. Each returns true if the setting was changed.

static bool ApplyGAMinMove(GuideAlgorithm *algo, double minMove, const wxString& axisName, const char *debugName)
{
    if (!algo)
        return false;

    if (algo->GetMinMove() >= 0.0)
    {
        if (!algo->SetMinMove(minMove))
        {
            Debug.Write(wxString::Format("GuideAssistant changed %s to %0.2f\n", debugName, minMove));
            pFrame->pGraphLog->UpdateControls();
            pFrame->NotifyGuidingParam(axisName + " " + algo->GetGuideAlgorithmClassName() + " MinMove ", minMove);
            return true;
        }
        else
            Debug.Write(wxString::Format("GuideAssistant could not change %s\n", debugName));
    }
    else
        Debug.Write(wxString::Format("GuideAssistant logic flaw, %s algorithm has no MinMove property\n", axisName));

    return false;
}

// Called once the Dec algorithm has been switched to Lowpass2
static bool ConfigureGADecLowpass2(double decMinMove)
{
    GuideAlgorithm *decAlgo = pMount->GetYGuideAlgorithm();
    if (!decAlgo || decAlgo->GetGuideAlgorithmClassName() != "Lowpass2")
    {
        Debug.Write("GuideAssistant could not set Dec algo to Lowpass2\n");
        return false;
    }

    double newAggr = 80.0;
    decAlgo->SetParam("aggressiveness", newAggr);
    decAlgo->SetParam("minMove", decMinMove);
    Debug.Write(
        wxString::Format("GuideAssistant set Lowpass2 aggressiveness = %0.2f, min-move = %0.2f\n", newAggr, decMinMove));
    pFrame->pGraphLog->UpdateControls();
    pFrame->NotifyGuidingParam("Declination algorithm", wxString("Lowpass2"));
    pFrame->NotifyGuidingParam("Declination Lowpass2 aggressivness", newAggr);
    pFrame->NotifyGuidingParam("Declination Lowpass2 MinMove", decMinMove);
    return true;
}

static void ApplyGADecBacklash(int backlashMs)
{
    BacklashComp *pComp = TheScope()->GetBacklashComp();

    pComp->SetBacklashPulseWidth(backlashMs, 0. /* floor */, 0. /* ceiling */);
    pComp->EnableBacklashComp(!pMount->IsStepGuider());
}

// Dialog for making sure sampling period is adequate for decent measurements
struct SampleWait : public wxDialog
{
    wxStaticText *m_CountdownAmount;
    wxTimer m_SecondsTimer;
    int m_SecondsLeft;

    SampleWait(int SamplePeriod, bool BltNeeded);
    void OnTimer(wxTimerEvent& evt);
    void OnCancel(wxCommandEvent& event);
};

SampleWait::SampleWait(int SecondsLeft, bool BltNeeded) : wxDialog(pFrame, wxID_ANY, _("Extended Sampling"))
{
    wxBoxSizer *vSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *amtSizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *explanation = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    wxString msg;
    if (BltNeeded)
        msg = _("Additional data sampling is being done to better meaure Dec drift. Backlash testing \nwill start "
                "automatically when sampling is completed.");
    else
        msg =
            _("Additional sampling is being done for accurate measurements.  Results will be shown when sampling is complete.");
    explanation->SetLabelText(msg);
    MakeBold(explanation);
    wxStaticText *countDownLabel =
        new wxStaticText(this, wxID_ANY, _("Seconds remaining: "), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_SecondsLeft = SecondsLeft;
    m_CountdownAmount = new wxStaticText(this, wxID_ANY, std::to_string(wxMax(0, m_SecondsLeft)));
    amtSizer->Add(countDownLabel, wxSizerFlags(0).Border(wxALL, 8));
    amtSizer->Add(m_CountdownAmount, wxSizerFlags(0).Border(wxALL, 8));
    wxButton *cancelBtn = new wxButton(this, wxID_ANY, _("Cancel"));
    cancelBtn->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(SampleWait::OnCancel), nullptr, this);
    wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->Add(cancelBtn, wxSizerFlags(0).Border(wxALL, 8).Center());

    vSizer->Add(explanation, wxSizerFlags(0).Border(wxALL, 8).Center());
    vSizer->Add(amtSizer, wxSizerFlags(0).Border(wxALL, 8).Center());
    vSizer->Add(btnSizer, wxSizerFlags(0).Border(wxALL, 8).Center());

    SetAutoLayout(true);
    SetSizerAndFit(vSizer);

    m_SecondsTimer.Connect(wxEVT_TIMER, wxTimerEventHandler(SampleWait::OnTimer), nullptr, this);
    m_SecondsTimer.Start(1000);
}

void SampleWait::OnTimer(wxTimerEvent& evt)
{
    m_SecondsLeft -= 1;
    if (m_SecondsLeft > 0)
    {
        m_CountdownAmount->SetLabelText(std::to_string(m_SecondsLeft));
        m_CountdownAmount->Update();
    }
    else
    {
        m_SecondsTimer.Stop();
        EndDialog(wxOK);
    }
}

void SampleWait::OnCancel(wxCommandEvent& event)
{
    m_SecondsTimer.Stop();
    if (wxGetKeyState(WXK_CONTROL))
        EndDialog(wxOK);
    else
        EndDialog(wxCANCEL);
}

// Encapsulated struct for implementing the dialog box
struct GuidingAsstWin : public wxDialog
{
    enum DialogState
    {
        STATE_NO_STAR = 0,
        STATE_START_READY = 1,
        STATE_MEASURING = 2,
        STATE_STOPPED = 3
    };
    enum DlgConstants
    {
        MAX_BACKLASH_COMP = 3000,
        GA_MIN_SAMPLING_PERIOD = 120
    };

    wxButton *m_start;
    wxButton *m_stop;
    OptionsButton *btnReviewPrev;
    wxTextCtrl *m_report;
    wxStaticText *m_instructions;
    wxGrid *m_statusgrid;
    wxGrid *m_displacementgrid;
    wxGrid *m_othergrid;
    wxFlexGridSizer *m_recommendgrid;
    wxBoxSizer *m_vSizer;
    wxStaticBoxSizer *m_recommend_group;
    wxCheckBox *m_backlashCB;
    wxStaticText *m_gaStatus;
    wxButton *m_graphBtn;

    wxGridCellCoords m_timestamp_loc;
    wxGridCellCoords m_starmass_loc;
    wxGridCellCoords m_samplecount_loc;
    wxGridCellCoords m_snr_loc;
    wxGridCellCoords m_elapsedtime_loc;
    wxGridCellCoords m_exposuretime_loc;
    wxGridCellCoords m_hfcutoff_loc;
    wxGridCellCoords m_ra_rms_loc;
    wxGridCellCoords m_dec_rms_loc;
    wxGridCellCoords m_total_rms_loc;
    wxGridCellCoords m_ra_peak_loc;
    wxGridCellCoords m_dec_peak_loc;
    wxGridCellCoords m_ra_peakpeak_loc;
    wxGridCellCoords m_ra_drift_loc;
    wxGridCellCoords m_ra_drift_exp_loc;
    wxGridCellCoords m_dec_drift_loc;
    wxGridCellCoords m_pae_loc;
    wxGridCellCoords m_ra_peak_drift_loc;
    wxGridCellCoords m_backlash_loc;
    wxButton *m_raMinMoveButton;
    wxButton *m_decMinMoveButton;
    wxButton *m_decBacklashButton;
    wxButton *m_decAlgoButton;
    wxStaticText *m_ra_msg;
    wxStaticText *m_dec_msg;
    wxStaticText *m_snr_msg;
    wxStaticText *m_pae_msg;
    wxStaticText *m_hfd_msg;
    wxStaticText *m_backlash_msg;
    wxStaticText *m_exposure_msg;
    wxStaticText *m_calibration_msg;
    wxStaticText *m_binning_msg;
    wxStaticText *m_decAlgo_msg;
    double m_ra_minmove_rec; // recommended value
    double m_dec_minmove_rec; // recommended value
    double m_min_exp_rec;
    double m_max_exp_rec;

    DialogState m_dlgState;
    bool m_measuring;
    wxLongLong_t m_startTime;
    long m_elapsedSecs;
    wxString startStr;
    ga_math::Accumulator m_stats;
    ga_math::Recommendations m_recs;
    double decDriftPerMin; // px per minute
    double decCorrectedRMS; // RMS of drift-corrected Dec dataset
    double alignmentError; // arc-minutes
    double m_backlashPx;
    int m_backlashMs;
    double m_backlashSigmaMs;
    int m_backlashRecommendedMs;

    bool m_guideOutputDisabled;
    bool m_savePrimaryMountEnabled;
    bool m_saveSecondaryMountEnabled;
    bool m_measurementsTaken;
    int m_origSubFrames;
    bool inBLTWrapUp = false;
    bool origMultistarMode;
    VarDelayCfg origVarDelayConfig;

    bool m_measuringBacklash;
    BacklashTool *m_backlashTool;
    bool reviewMode;
    GADetails gaDetails;
    bool m_flushConfig;

    GuidingAsstWin();
    ~GuidingAsstWin();

    void OnClose(wxCloseEvent& event);
    void OnMouseMove(wxMouseEvent&);
    void OnAppStateNotify(wxCommandEvent& event);
    void OnStart(wxCommandEvent& event);
    void DoStop(const wxString& status = wxEmptyString);
    void OnStop(wxCommandEvent& event);
    void OnRAMinMove(wxCommandEvent& event);
    void OnDecMinMove(wxCommandEvent& event);
    void OnDecBacklash(wxCommandEvent& event);
    void OnDecAlgoChange(wxCommandEvent& event);
    void OnGraph(wxCommandEvent& event);
    void OnHelp(wxCommandEvent& event);
    void OnReviewPrevious(wxCommandEvent& event);
    void OnGAReviewSelection(wxCommandEvent& evt);

    wxStaticText *AddRecommendationBtn(const wxString& msg, void (GuidingAsstWin::*handler)(wxCommandEvent&),
                                       wxButton **ppButton);
    wxStaticText *AddRecommendationMsg(const wxString& msg);
    void AddRecommendationRow(const wxString& which, const wxString& what);
    void UpdateInfo(const GuideStepInfo& info);
    void DisplayStaticResults(const GADetails& details);
    void FillInstructions(DialogState eState);
    void MakeRecommendations();
    void DisplayStaticRecommendations(const GADetails& details);
    void GetDisplayedResults(GADetails *details);
    void LogResults();
    void BacklashStep(const PHD_Point& camLoc);
    void EndBacklashTest(bool completed);
    void BacklashError();
    void StatsReset();
    void LoadGAResults(const wxString& TimeStamp, GADetails *Details);
    void SaveGAResults(const wxString *AllRecommendations);
    int GetGAHistoryCount();
};

static void HighlightCell(wxGrid *pGrid, wxGridCellCoords where)
{
    pGrid->SetCellBackgroundColour(where.GetRow(), where.GetCol(), "DARK SLATE GREY");
    pGrid->SetCellTextColour(where.GetRow(), where.GetCol(), "white");
}

struct GridTooltipInfo : public wxObject
{
    wxGrid *grid;
    int gridNum;
    wxGridCellCoords prevCoords;
    GridTooltipInfo(wxGrid *g, int i) : grid(g), gridNum(i) { }
};

struct TextWrapper
{
    wxWindow *win;
    int width;
    TextWrapper(wxWindow *win_, int width_) : win(win_), width(width_) { }
    wxString Wrap(const wxString& text) const
    {
        struct Wrapper : public wxTextWrapper
        {
            wxString str;
            Wrapper(wxWindow *win_, const wxString& text, int width_) { Wrap(win_, text, width_); }
            void OnOutputLine(const wxString& line) { str += line; }
            void OnNewLine() { str += '\n'; }
        };
        return Wrapper(win, text, width).str;
    }
};

// Constructor
GuidingAsstWin::GuidingAsstWin()
    : wxDialog(pFrame, wxID_ANY, wxGetTranslation(_("Guiding Assistant"))), m_measuring(false), m_guideOutputDisabled(false),
      m_measurementsTaken(false), m_origSubFrames(-1), m_backlashTool(nullptr), m_flushConfig(false)
{
    decDriftPerMin = 0.;
    decCorrectedRMS = 0.;
    alignmentError = 0.;
    m_backlashMs = 0;
    m_backlashRecommendedMs = 0;

    // Sizer hierarchy:
    // m_vSizer has {instructions, vResultsSizer, m_gaStatus, btnSizer}
    // vResultsSizer has {hTopSizer, hBottomSizer}
    // hTopSizer has {status_group, displacement_group}
    // hBottomSizer has {other_group, m_recommendation_group}
    m_vSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *vResultsSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *hTopSizer = new wxBoxSizer(wxHORIZONTAL); // Measurement status and high-frequency results
    wxBoxSizer *hBottomSizer = new wxBoxSizer(wxHORIZONTAL); // Low-frequency results and recommendations

    m_instructions =
        new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(700, 50), wxALIGN_LEFT | wxST_NO_AUTORESIZE);
    MakeBold(m_instructions);
    m_vSizer->Add(m_instructions, wxSizerFlags(0).Border(wxALL, 8));

    // Grids have either 3 or 4 columns, so compute width of largest label as scaling term for column widths
    double minLeftCol = StringWidth(this, _(" -999.99 px/min (-999.99 arc-sec/min )")) + 6;
    double minRightCol = 1.25 * (StringWidth(this, _(" 9.99 px ( 9.99 arc-sec)")) + 6);
    // Start of status group
    wxStaticBoxSizer *status_group = new wxStaticBoxSizer(wxVERTICAL, this, _("Measurement Status"));
    m_statusgrid = new wxGrid(this, wxID_ANY);
    m_statusgrid->CreateGrid(3, 4);
    m_statusgrid->GetGridWindow()->Bind(wxEVT_MOTION, &GuidingAsstWin::OnMouseMove, this, wxID_ANY, wxID_ANY,
                                        new GridTooltipInfo(m_statusgrid, 1));
    m_statusgrid->SetRowLabelSize(1);
    m_statusgrid->SetColLabelSize(1);
    m_statusgrid->EnableEditing(false);
    m_statusgrid->SetDefaultColSize(round(2.0 * minLeftCol / 4.0) + 0.5);

    int col = 0;
    int row = 0;
    m_statusgrid->SetCellValue(row, col++, _("Start time"));
    m_timestamp_loc.Set(row, col++);
    m_statusgrid->SetCellValue(row, col++, _("Exposure time"));
    m_exposuretime_loc.Set(row, col++);

    StartRow(row, col);
    m_statusgrid->SetCellValue(row, col++, _("SNR"));
    m_snr_loc.Set(row, col++);
    m_statusgrid->SetCellValue(row, col++, _("Star mass"));
    m_starmass_loc.Set(row, col++);

    StartRow(row, col);
    m_statusgrid->SetCellValue(row, col++, _("Elapsed time"));
    m_elapsedtime_loc.Set(row, col++);
    m_statusgrid->SetCellValue(row, col++, _("Sample count"));
    m_samplecount_loc.Set(row, col++);

    // StartRow(row, col);
    // m_statusgrid->SetCellValue(_("Frequency cut-off:"), row, col++);   // Leave out for now, probably not useful to users
    // m_hfcutoff_loc.Set(row, col++);

    status_group->Add(m_statusgrid);
    hTopSizer->Add(status_group, wxSizerFlags(0).Border(wxALL, 8));
    // End of status group

    // Start of star displacement group
    wxStaticBoxSizer *displacement_group = new wxStaticBoxSizer(wxVERTICAL, this, _("High-frequency Star Motion"));
    m_displacementgrid = new wxGrid(this, wxID_ANY);
    m_displacementgrid->CreateGrid(3, 2);
    m_displacementgrid->GetGridWindow()->Bind(wxEVT_MOTION, &GuidingAsstWin::OnMouseMove, this, wxID_ANY, wxID_ANY,
                                              new GridTooltipInfo(m_displacementgrid, 2));
    m_displacementgrid->SetRowLabelSize(1);
    m_displacementgrid->SetColLabelSize(1);
    m_displacementgrid->EnableEditing(false);
    m_displacementgrid->SetDefaultColSize(minRightCol);

    row = 0;
    col = 0;
    m_displacementgrid->SetCellValue(row, col++, _("Right ascension, RMS"));
    m_ra_rms_loc.Set(row, col++);

    StartRow(row, col);
    m_displacementgrid->SetCellValue(row, col++, _("Declination, RMS"));
    m_dec_rms_loc.Set(row, col++);

    StartRow(row, col);
    m_displacementgrid->SetCellValue(row, col++, _("Total, RMS"));
    m_total_rms_loc.Set(row, col++);

    displacement_group->Add(m_displacementgrid);
    hTopSizer->Add(displacement_group, wxSizerFlags(0).Border(wxALL, 8));
    vResultsSizer->Add(hTopSizer);
    // End of displacement group

    // Start of "Other" (peak and drift) group
    wxStaticBoxSizer *other_group = new wxStaticBoxSizer(wxVERTICAL, this, _("Other Star Motion"));
    m_othergrid = new wxGrid(this, wxID_ANY);
    m_othergrid->CreateGrid(9, 2);
    m_othergrid->GetGridWindow()->Bind(wxEVT_MOTION, &GuidingAsstWin::OnMouseMove, this, wxID_ANY, wxID_ANY,
                                       new GridTooltipInfo(m_othergrid, 3));
    m_othergrid->SetRowLabelSize(1);
    m_othergrid->SetColLabelSize(1);
    m_othergrid->EnableEditing(false);
    m_othergrid->SetDefaultColSize(minLeftCol);

    TextWrapper w(this, minLeftCol);

    row = 0;
    col = 0;
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Right ascension, Peak")));
    m_ra_peak_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Declination, Peak")));
    m_dec_peak_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Right ascension, Peak-Peak")));
    m_ra_peakpeak_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Right ascension Drift Rate")));
    m_ra_drift_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Right ascension Max Drift Rate")));
    m_ra_peak_drift_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Drift-limiting exposure")));
    m_ra_drift_exp_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Declination Drift Rate")));
    m_dec_drift_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Declination Backlash")));
    m_backlash_loc.Set(row, col++);

    StartRow(row, col);
    m_othergrid->SetCellValue(row, col++, w.Wrap(_("Polar Alignment Error")));
    m_pae_loc.Set(row, col++);

    m_othergrid->AutoSizeColumn(0);
    m_othergrid->AutoSizeRows();

    other_group->Add(m_othergrid);
    hBottomSizer->Add(other_group, wxSizerFlags(0).Border(wxALL, 8));
    // End of peak and drift group

    // Start of Recommendations group - just a place-holder for layout, populated in MakeRecommendations
    m_recommend_group = new wxStaticBoxSizer(wxVERTICAL, this, _("Recommendations"));
    m_recommendgrid = new wxFlexGridSizer(2, 0, 0);
    m_recommendgrid->AddGrowableCol(0);
    m_ra_msg = nullptr;
    m_dec_msg = nullptr;
    m_snr_msg = nullptr;
    m_backlash_msg = nullptr;
    m_pae_msg = nullptr;
    m_hfd_msg = nullptr;
    m_exposure_msg = nullptr;
    m_calibration_msg = nullptr;
    m_binning_msg = nullptr;

    m_recommend_group->Add(m_recommendgrid, wxSizerFlags(1).Expand());
    // Add buttons for viewing the Dec backlash graph or getting help
    wxBoxSizer *hBtnSizer = new wxBoxSizer(wxHORIZONTAL);
    m_graphBtn = new wxButton(this, wxID_ANY, _("Show Backlash Graph"));
    m_graphBtn->SetToolTip(_("Show graph of backlash measurement points"));
    m_graphBtn->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(GuidingAsstWin::OnGraph), nullptr, this);
    m_graphBtn->Enable(false);
    hBtnSizer->Add(m_graphBtn, wxSizerFlags(0).Border(wxALL, 5));
    wxButton *helpBtn = new wxButton(this, wxID_ANY, _("Help"));
    helpBtn->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(GuidingAsstWin::OnHelp), nullptr, this);
    hBtnSizer->Add(50, 0);
    hBtnSizer->Add(helpBtn, wxSizerFlags(0).Border(wxALL, 5));
    m_recommend_group->Add(hBtnSizer, wxSizerFlags(0).Border(wxALL, 5));
    // Recommendations will be hidden/shown depending on state
    hBottomSizer->Add(m_recommend_group, wxSizerFlags(0).Border(wxALL, 8));
    vResultsSizer->Add(hBottomSizer);

    m_vSizer->Add(vResultsSizer);
    m_recommend_group->Show(false);
    // End of recommendations

    m_backlashCB = new wxCheckBox(this, wxID_ANY, _("Measure Declination Backlash"));
    m_backlashCB->SetToolTip(_("PHD2 will move the guide star a considerable distance north, then south to measure backlash. "
                               "Be sure the selected star has "
                               "plenty of room to move in the north direction.  If the guide star is lost, increase the size "
                               "of the search region to at least 20 px"));
    if (TheScope())
    {
        m_backlashCB->SetValue(!pMount->HasHPEncoders());
        m_backlashCB->Enable(true);
    }
    else
    {
        m_backlashCB->SetValue(false);
        m_backlashCB->Enable(false);
    }
    // Text area for showing backlash measuring steps
    m_gaStatus = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(500, 40), wxALIGN_CENTER);
    MakeBold(m_gaStatus);
    m_vSizer->Add(m_gaStatus, wxSizerFlags(0).Border(wxALL, 8).Center());

    wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->Add(10, 0); // a little spacing left of Start button
    btnSizer->Add(m_backlashCB, wxSizerFlags(0).Border(wxALL, 8));
    btnSizer->Add(40, 0); // Put a spacer between the button and checkbox

    m_start = new wxButton(this, wxID_ANY, _("Start"), wxDefaultPosition, wxDefaultSize, 0);
    m_start->SetToolTip(_("Start measuring (disables guiding)"));
    btnSizer->Add(m_start, 0, wxALL, 5);
    m_start->Enable(false);

    btnReviewPrev = new OptionsButton(this, GA_REVIEW_BUTTON, _("Review previous"), wxDefaultPosition, wxDefaultSize, 0);
    btnReviewPrev->SetToolTip(_("Review previous Guiding Assistant results"));
    btnReviewPrev->Enable(GetGAHistoryCount() > 0);
    btnSizer->Add(btnReviewPrev, 0, wxALL, 5);

    m_stop = new wxButton(this, wxID_ANY, _("Stop"), wxDefaultPosition, wxDefaultSize, 0);
    m_stop->SetToolTip(_("Stop measuring and re-enable guiding"));
    m_stop->Enable(false);

    btnSizer->Add(m_stop, 0, wxALL, 5);
    m_vSizer->Add(btnSizer, 0, wxEXPAND, 5);

    SetAutoLayout(true);
    SetSizerAndFit(m_vSizer);

    Connect(wxEVT_CLOSE_WINDOW, wxCloseEventHandler(GuidingAsstWin::OnClose));
    Connect(APPSTATE_NOTIFY_EVENT, wxCommandEventHandler(GuidingAsstWin::OnAppStateNotify));
    m_start->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(GuidingAsstWin::OnStart), nullptr, this);
    m_stop->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(GuidingAsstWin::OnStop), nullptr, this);
    Bind(wxEVT_BUTTON, &GuidingAsstWin::OnReviewPrevious, this, GA_REVIEW_BUTTON, GA_REVIEW_BUTTON);
    Bind(wxEVT_MENU, &GuidingAsstWin::OnGAReviewSelection, this, GA_REVIEW_ITEMS_BASE, GA_REVIEW_ITEMS_LIMIT);

    if (m_backlashCB->IsEnabled())
        m_backlashTool = new BacklashTool();

    m_measuringBacklash = false;
    origMultistarMode = pFrame->pGuider->GetMultiStarMode();
    origVarDelayConfig = pFrame->GetVariableDelayConfig();
    pFrame->SetVariableDelayConfig(false, origVarDelayConfig.shortDelay, origVarDelayConfig.longDelay);

    int xpos = pConfig->Global.GetInt("/GuidingAssistant/pos.x", -1);
    int ypos = pConfig->Global.GetInt("/GuidingAssistant/pos.y", -1);
    MyFrame::PlaceWindowOnScreen(this, xpos, ypos);

    wxCommandEvent dummy;
    OnAppStateNotify(dummy); // init state-dependent controls

    reviewMode = false;
    if (pFrame->pGuider->IsGuiding())
    {
        OnStart(dummy); // Auto-start if we're already guiding
    }
}

GuidingAsstWin::~GuidingAsstWin(void)
{
    pFrame->pGuidingAssistant = 0;
    delete m_backlashTool;
}

void GuidingAsstWin::StatsReset()
{
    m_stats.Reset((double) pFrame->RequestedExposureDuration() / 1000.0);
}

static bool GetGridToolTip(int gridNum, const wxGridCellCoords& coords, wxString *s)
{
    int col = coords.GetCol();

    if (gridNum > 1 && col != 0)
        return false;
    else if (col != 0 && col != 2)
        return false;

    switch (gridNum * 100 + coords.GetRow())
    {
    // status grid
    case 101:
    {
        if (col == 0)
            *s = _("Signal-to-noise ratio; a measure of how well PHD2 can isolate the star from the sky/noise background");
        else
            *s = _("Measure of overall star brightness. Consider using 'Auto-select Star' (Alt-S) to choose the star.");
        break;
    }

    // displacement grid
    case 200:
        *s = _("Measure of typical high-frequency right ascension star movements; guiding usually cannot correct for "
               "fluctuations this small.");
        break;
    case 201:
        *s = _("Measure of typical high-frequency declination star movements; guiding usually cannot correct for fluctuations "
               "this small.");
        break;

    // other grid
    case 300:
        *s = _("Maximum sample-sample deflection seen in right ascension.");
        break;
    case 301:
        *s = _("Maximum sample-sample deflection seen in declination.");
        break;
    case 302:
        *s = _("Maximum peak-peak deflection seen in right ascension during sampling period.");
        break;
    case 303:
        *s = _("Estimated overall drift rate in right ascension.");
        break;
    case 304:
        *s = _("Maximum drift rate in right ascension during sampling period.");
        break;
    case 305:
        *s = _("Exposure time to keep maximum RA drift below the recommended min-move level.");
        break;
    case 306:
        *s = _("Estimated overall drift rate in declination.");
        break;
    case 307:
        *s = _("Estimated declination backlash if test was completed. Results are time to clear backlash (ms) and "
               "corresponding gear angle (arc-sec). Uncertainty estimate is one unit of standard deviation");
        break;
    case 308:
        *s = _("Estimate of polar alignment error. If the scope declination is unknown, the value displayed is a lower bound "
               "and the actual error may be larger.");
        break;

    default:
        return false;
    }

    return true;
}

void GuidingAsstWin::OnMouseMove(wxMouseEvent& ev)
{
    GridTooltipInfo *info = static_cast<GridTooltipInfo *>(ev.GetEventUserData());
    wxGridCellCoords coords(info->grid->XYToCell(info->grid->CalcUnscrolledPosition(ev.GetPosition())));
    if (coords != info->prevCoords)
    {
        info->prevCoords = coords;
        wxString s;
        if (GetGridToolTip(info->gridNum, coords, &s))
            info->grid->GetGridWindow()->SetToolTip(s);
        else
            info->grid->GetGridWindow()->UnsetToolTip();
    }
    ev.Skip();
}

void GuidingAsstWin::FillInstructions(DialogState eState)
{
    wxString instr;

    switch (eState)
    {
    case STATE_NO_STAR:
        instr = _("Choose a non-saturated star with a good SNR (>= 10) and begin guiding");
        break;
    case STATE_START_READY:
        if (!m_measurementsTaken)
            instr = _(
                "Click Start to begin measurements.  Guiding will be disabled during this time so the star will move around.");
        else
            instr = m_instructions->GetLabel();
        break;
    case STATE_MEASURING:
        instr = _("Guiding output is disabled and star movement is being measured.  Click Stop after 2 minutes (longer if "
                  "you're measuring RA tracking accuracy of the mount).");
        break;
    case STATE_STOPPED:
        instr = _("Guiding has been resumed. Look at the recommendations and make any desired changes.  Click Start to repeat "
                  "the measurements, or close the window to continue guiding.");
        break;
    }
    m_instructions->SetLabel(instr);
    m_instructions->Wrap(700);
    m_instructions->Layout();
}

void GuidingAsstWin::BacklashStep(const PHD_Point& camLoc)
{
    BacklashTool::MeasurementResults qual;
    m_backlashTool->DecMeasurementStep(camLoc);
    wxString bl_msg = _("Measuring backlash: ") + m_backlashTool->GetLastStatus();
    m_gaStatus->SetLabel(bl_msg);
    if (m_backlashTool->GetBltState() == BacklashTool::BLT_STATE_COMPLETED)
    {
        wxString bl_msg = _("Measuring backlash: ") + m_backlashTool->GetLastStatus();
        m_gaStatus->SetLabel(bl_msg);
        if (m_backlashTool->GetBltState() == BacklashTool::BLT_STATE_COMPLETED)
        {
            try
            {
                if (inBLTWrapUp)
                {
                    Debug.Write("GA-BLT: Re-entrancy in Backlash step!\n");
                    return;
                }
                else
                    inBLTWrapUp = true;
                Debug.Write("GA-BLT: state = completed\n");
                qual = m_backlashTool->GetMeasurementQuality();
                if (qual == BacklashTool::MEASUREMENT_VALID || qual == BacklashTool::MEASUREMENT_TOO_FEW_NORTH)
                {
                    Debug.Write("GA-BLT: Wrap-up after normal completion\n");
                    // populate result variables
                    m_backlashPx = m_backlashTool->GetBacklashResultPx();
                    m_backlashMs = m_backlashTool->GetBacklashResultMs();
                    double bltSigmaPx;
                    m_backlashTool->GetBacklashSigma(&bltSigmaPx, &m_backlashSigmaMs);
                    wxString outStr; // untranslated
                    wxString outStrTr =
                        FormatBacklashResult(qual, m_backlashMs, m_backlashSigmaMs, m_backlashPx, bltSigmaPx, &outStr);
                    m_othergrid->SetCellValue(m_backlash_loc, outStrTr);
                    HighlightCell(m_othergrid, m_backlash_loc);
                    outStr += "\n";
                    GuideLog.NotifyGAResult("Backlash=" + outStr);
                    Debug.Write("BLT: Reported result = " + outStr);
                    m_graphBtn->Enable(true);
                }
                else
                {
                    m_othergrid->SetCellValue(m_backlash_loc, "");
                }
                EndBacklashTest(qual == BacklashTool::MEASUREMENT_VALID || qual == BacklashTool::MEASUREMENT_TOO_FEW_NORTH);
            }
            catch (const wxString& msg)
            {
                Debug.Write(wxString::Format("GA-BLT: fault in completion-processing at %d, %s\n", __LINE__, msg));
                EndBacklashTest(false);
            }
        }
    }
    else if (m_backlashTool->GetBltState() == BacklashTool::BLT_STATE_ABORTED)
        EndBacklashTest(false);

    inBLTWrapUp = false;
}

void GuidingAsstWin::BacklashError()
{
    EndBacklashTest(false);
}

// Event handlers for applying recommendations
void GuidingAsstWin::OnRAMinMove(wxCommandEvent& event)
{
    if (ApplyGAMinMove(pMount->GetXGuideAlgorithm(), m_ra_minmove_rec, "RA", "RA_MinMove"))
    {
        m_raMinMoveButton->Enable(false);
        m_flushConfig = true;
    }
}

void GuidingAsstWin::OnDecMinMove(wxCommandEvent& event)
{
    if (ApplyGAMinMove(pMount->GetYGuideAlgorithm(), m_dec_minmove_rec, "Declination", "Dec_MinMove"))
    {
        m_decMinMoveButton->Enable(false);
        m_flushConfig = true;
    }
}

void GuidingAsstWin::OnDecAlgoChange(wxCommandEvent& event)
{
    if (pMount->IsStepGuider())
        return; // should never happen
    pMount->SetGuidingEnabled(false);
    // Need to make algo change through AD UI controls to keep everything in-synch
    Mount::MountConfigDialogPane *currMountPane = pFrame->pAdvancedDialog->GetCurrentMountPane();
    currMountPane->ChangeYAlgorithm("Lowpass2");
    Debug.Write("GuideAssistant changed Dec algo to Lowpass2\n");
    if (!ConfigureGADecLowpass2(m_dec_minmove_rec))
        return;
    pMount->SetGuidingEnabled(true);

    m_decAlgoButton->Enable(false);

    m_flushConfig = true;
}

void GuidingAsstWin::OnDecBacklash(wxCommandEvent& event)
{
    ApplyGADecBacklash(m_backlashRecommendedMs);
    m_decBacklashButton->Enable(false);
    m_flushConfig = true;
}

void GuidingAsstWin::OnGraph(wxCommandEvent& event)
{
    if (reviewMode)
        m_backlashTool->ShowGraph(this, gaDetails.BLTNorthMoves, gaDetails.BLTSouthMoves, gaDetails.BLTMsmtPulse);
    else
        m_backlashTool->ShowGraph(this, m_backlashTool->GetNorthSteps(), m_backlashTool->GetSouthSteps(),
                                  m_backlashTool->GetBLTMsmtPulseSize());
}

void GuidingAsstWin::OnHelp(wxCommandEvent& event)
{
    // Bundled GUI help was removed for the headless build; open the online help.
    wxLaunchDefaultBrowser("https://openphdguiding.org/getting-help/");
}

static wxString SizedMsg(const wxString& msg)
{
    if (msg.length() < 70)
        return msg + wxString(' ', 70 - msg.length());
    else
        return msg;
}

// Adds a recommendation string and a button bound to the passed event handler
wxStaticText *GuidingAsstWin::AddRecommendationBtn(const wxString& msg, void (GuidingAsstWin::*handler)(wxCommandEvent&),
                                                   wxButton **ppButton)
{
    wxStaticText *rec_label;

    rec_label = new wxStaticText(this, wxID_ANY, SizedMsg(msg));
    rec_label->Wrap(250);
    m_recommendgrid->Add(rec_label, 1, wxALIGN_LEFT | wxALL, 5);
    if (handler)
    {
        int min_h;
        int min_w;
        this->GetTextExtent(_("Apply"), &min_w, &min_h);
        *ppButton = new wxButton(this, wxID_ANY, _("Apply"), wxDefaultPosition, wxSize(min_w + 8, min_h + 8), 0);
        m_recommendgrid->Add(*ppButton, 0, wxALIGN_RIGHT | wxALL, 5);
        (*ppButton)->Connect(wxEVT_COMMAND_BUTTON_CLICKED, reinterpret_cast<wxObjectEventFunction>(handler), nullptr, this);
    }
    else
    {
        wxStaticText *rec_tmp = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
        m_recommendgrid->Add(rec_tmp, 0, wxALL, 5);
    }
    return rec_label;
}

// Jacket for simple addition of a text-only recommendation
wxStaticText *GuidingAsstWin::AddRecommendationMsg(const wxString& msg)
{
    return AddRecommendationBtn(msg, nullptr, nullptr);
}

// The results currently shown in the grids
void GuidingAsstWin::GetDisplayedResults(GADetails *d)
{
    d->TimeStamp = m_statusgrid->GetCellValue(m_timestamp_loc);
    d->SNR = m_statusgrid->GetCellValue(m_snr_loc);
    d->StarMass = m_statusgrid->GetCellValue(m_starmass_loc);
    d->SampleCount = m_statusgrid->GetCellValue(m_samplecount_loc);
    d->ElapsedTime = m_statusgrid->GetCellValue(m_elapsedtime_loc);
    d->ExposureTime = m_statusgrid->GetCellValue(m_exposuretime_loc);
    d->RA_HPF_RMS = m_displacementgrid->GetCellValue(m_ra_rms_loc);
    d->Dec_HPF_RMS = m_displacementgrid->GetCellValue(m_dec_rms_loc);
    d->Total_HPF_RMS = m_displacementgrid->GetCellValue(m_total_rms_loc);
    d->RAPeak = m_othergrid->GetCellValue(m_ra_peak_loc);
    d->RAPeak_Peak = m_othergrid->GetCellValue(m_ra_peakpeak_loc);
    d->RADriftRate = m_othergrid->GetCellValue(m_ra_drift_loc);
    d->RAMaxDriftRate = m_othergrid->GetCellValue(m_ra_peak_drift_loc);
    d->DriftLimitingExposure = m_othergrid->GetCellValue(m_ra_drift_exp_loc);
    d->DecDriftRate = m_othergrid->GetCellValue(m_dec_drift_loc);
    d->DecPeak = m_othergrid->GetCellValue(m_dec_peak_loc);
    d->PAError = m_othergrid->GetCellValue(m_pae_loc);
    d->BackLashInfo = m_othergrid->GetCellValue(m_backlash_loc);
}

void GuidingAsstWin::LogResults()
{
    GADetails d;
    GetDisplayedResults(&d);
    LogGAResults(d);
}

// Get info regarding any saved GA sessions that include a BLT
static void GetBLTHistory(const std::vector<wxString>& Timestamps, int *oldestBLTInx, int *BLTCount)
{
    int oldestInx = -1;
    int bltCount = 0;
    for (int inx = 0; inx < Timestamps.size(); inx++)
    {
        wxString northBLT = "/GA/" + Timestamps[inx] + "/BLT_North";
        if (pConfig->Profile.GetString(northBLT, wxEmptyString) != wxEmptyString)
        {
            bltCount++;
            if (oldestInx < 0)
                oldestInx = inx;
        }
    }
    *oldestBLTInx = oldestInx;
    *BLTCount = bltCount;
}

int GuidingAsstWin::GetGAHistoryCount()
{
    std::vector<wxString> timeStamps = pConfig->Profile.GetGroupNames("/GA");
    return timeStamps.size();
}

// Insure that no more than 3 GA sessions are kept in the profile while also keeping at least one BLT measurement if one exists
static void TrimGAHistory(bool FreshBLT, int HistoryDepth)
{
    int bltCount;
    int oldestBLTInx;
    int totalGAs;
    wxString targetEntry;

    std::vector<wxString> timeStamps;
    timeStamps = pConfig->Profile.GetGroupNames("/GA");
    totalGAs = timeStamps.size();
    GetBLTHistory(timeStamps, &oldestBLTInx, &bltCount);
    if (totalGAs > HistoryDepth)
    {
        if (FreshBLT || bltCount == 0 || oldestBLTInx > 0 || bltCount > 1 || bltCount == totalGAs)
            targetEntry = timeStamps[0];
        else
            targetEntry = timeStamps[1];
        pConfig->Profile.DeleteGroup("/GA/" + targetEntry);
        Debug.Write(wxString::Format("GA-History: removed entry for %s\n", targetEntry));
    }
}

// Save the results from the most recent GA run in the profile
void GuidingAsstWin::SaveGAResults(const wxString *AllRecommendations)
{
    GADetails d;
    GetDisplayedResults(&d);
    SaveGAHistory(startStr, d, decCorrectedRMS, decDriftPerMin, m_recs, m_backlashMs, m_backlashRecommendedMs,
                  *AllRecommendations, m_backlashTool);
}

// Reload GA results for the passed timestamp
void GuidingAsstWin::LoadGAResults(const wxString& TimeStamp, GADetails *Details)
{
    wxString prefix = "/GA/" + TimeStamp;
    *Details = {}; // Reset all vars
    Details->TimeStamp = pConfig->Profile.GetString(prefix + "/timestamp", wxEmptyString);
    Details->SNR = pConfig->Profile.GetString(prefix + "/snr", wxEmptyString);
    Details->StarMass = pConfig->Profile.GetString(prefix + "/star_mass", wxEmptyString);
    Details->SampleCount = pConfig->Profile.GetString(prefix + "/sample_count", wxEmptyString);
    Details->ExposureTime = pConfig->Profile.GetString(prefix + "/exposure_time", wxEmptyString);
    Details->ElapsedTime = pConfig->Profile.GetString(prefix + "/elapsed_time", wxEmptyString);
    Details->RA_HPF_RMS = pConfig->Profile.GetString(prefix + "/ra_hpf_rms", wxEmptyString);
    Details->Dec_HPF_RMS = pConfig->Profile.GetString(prefix + "/dec_hpf_rms", wxEmptyString);
    Details->Total_HPF_RMS = pConfig->Profile.GetString(prefix + "/total_hpf_rms", wxEmptyString);
    Details->RAPeak = pConfig->Profile.GetString(prefix + "/ra_peak", wxEmptyString);
    Details->RAPeak_Peak = pConfig->Profile.GetString(prefix + "/ra_peak_peak", wxEmptyString);
    Details->RADriftRate = pConfig->Profile.GetString(prefix + "/ra_drift_rate", wxEmptyString);
    Details->RAMaxDriftRate = pConfig->Profile.GetString(prefix + "/ra_peak_drift_rate", wxEmptyString);
    Details->DriftLimitingExposure = pConfig->Profile.GetString(prefix + "/ra_drift_exposure", wxEmptyString);
    Details->DecDriftRate = pConfig->Profile.GetString(prefix + "/dec_drift_rate", wxEmptyString);
    Details->DecPeak = pConfig->Profile.GetString(prefix + "/dec_peak", wxEmptyString);
    Details->PAError = pConfig->Profile.GetString(prefix + "/pa_error", wxEmptyString);
    Details->DecCorrectedRMS = pConfig->Profile.GetString(prefix + "/dec_corrected_rms", wxEmptyString);
    Details->BackLashInfo = pConfig->Profile.GetString(prefix + "/backlash_info", wxEmptyString);
    Details->Dec_LF_DriftRate = pConfig->Profile.GetString(prefix + "/dec_lf_drift_rate", wxEmptyString);
    Details->RecRAMinMove = pConfig->Profile.GetString(prefix + "/rec_ra_minmove", wxEmptyString);
    Details->RecDecMinMove = pConfig->Profile.GetString(prefix + "/rec_dec_minmove", wxEmptyString);
    Details->BLTAmount = pConfig->Profile.GetString(prefix + "/BLT_pulse", wxEmptyString);
    Details->Recommendations = pConfig->Profile.GetString(prefix + "/recommendations", wxEmptyString);
    wxString northBLT = pConfig->Profile.GetString(prefix + "/BLT_North", wxEmptyString);
    wxString southBLT = pConfig->Profile.GetString(prefix + "/BLT_South", wxEmptyString);
    Details->BLTMsmtPulse = pConfig->Profile.GetInt(prefix + "/BLT_MsmtPulse", -1);
    if (northBLT.Length() > 0 && southBLT.Length() > 0)
    {
        wxStringTokenizer tok;
        wxString strVal;
        double ptVal;
        tok.SetString(northBLT, ",");
        while (tok.HasMoreTokens())
        {
            strVal = tok.GetNextToken();
            strVal.ToDouble(&ptVal);
            Details->BLTNorthMoves.push_back(ptVal);
        }

        tok.SetString(southBLT, ",");
        while (tok.HasMoreTokens())
        {
            strVal = tok.GetNextToken();
            strVal.ToDouble(&ptVal);
            Details->BLTSouthMoves.push_back(ptVal);
        }
    }
}

// Produce recommendations for "live" GA run
void GuidingAsstWin::MakeRecommendations()
{
    CalibrationDetails calDetails;
    bool bltCompleted = m_backlashTool->GetBltState() == BacklashTool::BLT_STATE_COMPLETED;
    m_recs = ComputeGARecommendations(m_stats, origMultistarMode, bltCompleted, m_backlashMs,
                                      m_backlashTool->GetBacklashExempted(), &calDetails);
    m_ra_minmove_rec = m_recs.minMove.ra;
    m_dec_minmove_rec = m_recs.minMove.dec;
    decCorrectedRMS = m_recs.minMove.seeingEstimatePx;
    m_min_exp_rec = m_recs.exposure.min;
    m_max_exp_rec = m_recs.exposure.max;
    m_backlashRecommendedMs = m_recs.hasBacklash ? m_recs.backlash.recommendedMs : 0;

    // Refine the drift-limiting exposure value based on the ra_min_move recommendation
    m_othergrid->SetCellValue(m_ra_drift_exp_loc, FormatRefinedDriftLimitingExposure(m_recs));

    LogResults(); // Dump the raw statistics

    // REMINDER: Any new recommendations must also be handled in 'AddRecommendationRow'
    m_recommendgrid->Clear(true);

    std::vector<GARecommendation> recs = BuildGARecommendations(m_recs, calDetails, m_backlashMs);
    for (const GARecommendation& rec : recs)
        AddRecommendationRow(rec.which, rec.msg);
    LogGARecommendations(recs);

    wxString allRecommendations = JoinGARecommendations(recs);
    GuideLog.NotifyGACompleted();
    SaveGAResults(&allRecommendations);
    m_recommend_group->Show(true);

    // cppcheck-suppress ignoredReturnValue
    m_statusgrid->Layout();
    Layout();
    GetSizer()->Fit(this);
    Debug.Write("End of Guiding Assistant output....\n");
}

// Adds the recommendation row for a GA-history key ("Exp", "RAMinMove", ...), with an Apply button where applicable
void GuidingAsstWin::AddRecommendationRow(const wxString& which, const wxString& what)
{
    if (which == "Exp")
    {
        m_exposure_msg = AddRecommendationMsg(what);
    }
    else if (which == "Bin")
    {
        m_binning_msg = AddRecommendationMsg(what);
    }
    else if (which == "Cal")
    {
        m_calibration_msg = AddRecommendationMsg(what);
    }
    else if (which == "Star")
    {
        m_snr_msg = AddRecommendationMsg(what);
    }
    else if (which == "PA")
    {
        m_pae_msg = AddRecommendationMsg(what);
    }
    else if (which == "StarHFD")
    {
        m_hfd_msg = AddRecommendationMsg(what);
    }
    else if (which == "RAMinMove")
    {
        m_ra_msg = AddRecommendationBtn(what, &GuidingAsstWin::OnRAMinMove, &m_raMinMoveButton);
    }
    else if (which == "DecMinMove")
    {
        m_dec_msg = AddRecommendationBtn(what, &GuidingAsstWin::OnDecMinMove, &m_decMinMoveButton);
    }
    else if (which == "DecAlgo")
    {
        m_decAlgo_msg = AddRecommendationBtn(what, &GuidingAsstWin::OnDecAlgoChange, &m_decAlgoButton);
    }
    else if (which == "BLT")
    {
        bool largeBL = m_backlashMs > MAX_BACKLASH_COMP;
        m_backlash_msg = AddRecommendationBtn(what, &GuidingAsstWin::OnDecBacklash, &m_decBacklashButton);
        m_decBacklashButton->Enable(!largeBL && m_backlashRecommendedMs > 100);
    }
}

// Show recommendations from a previous GA that is being reviewed
void GuidingAsstWin::DisplayStaticRecommendations(const GADetails& details)
{
    std::vector<wxString> recList;
    wxString allRecs = details.Recommendations;
    bool done = false;
    size_t end;

    m_recommendgrid->Clear(true); // Always start fresh, delete any child buttons
    while (!done)
    {
        end = allRecs.find_first_of("\n");
        if (end > 0)
        {
            wxString rec = allRecs.Left(end);
            size_t colPos = rec.find_first_of(":");
            wxString which = rec.SubString(0, colPos - 1);
            wxString what = rec.SubString(colPos + 1, end);
            if (which == "RAMinMove")
                details.RecRAMinMove.ToDouble(&m_ra_minmove_rec);
            else if (which == "DecMinMove")
                details.RecDecMinMove.ToDouble(&m_dec_minmove_rec);
            else if (which == "BLT")
                m_backlashMs = wxAtoi(details.BLTAmount);
            AddRecommendationRow(which, what);
            allRecs = allRecs.Mid(end + 1);
            done = allRecs.size() == 0;
        }
    }
    m_recommend_group->Show(true);

    m_statusgrid->Layout();
    Layout();
    GetSizer()->Fit(this);
}

void GuidingAsstWin::OnStart(wxCommandEvent& event)
{
    if (!pFrame->pGuider->IsGuiding())
        return;

    pFrame->pGuider->SetMultiStarMode(false);
    StatsReset();

    m_start->Enable(false);
    m_stop->Enable(true);
    btnReviewPrev->Enable(false);
    reviewMode = false;
    m_dlgState = STATE_MEASURING;
    FillInstructions(m_dlgState);
    m_gaStatus->SetLabel(_("Measuring..."));
    m_recommend_group->Show(false);
    HighlightCell(m_displacementgrid, m_ra_rms_loc);
    HighlightCell(m_displacementgrid, m_dec_rms_loc);
    HighlightCell(m_displacementgrid, m_total_rms_loc);

    Debug.AddLine("GuidingAssistant: Disabling guide output");

    if (pMount)
    {
        m_savePrimaryMountEnabled = pMount->GetGuidingEnabled();
        pMount->SetGuidingEnabled(false);
    }
    if (pSecondaryMount)
    {
        m_saveSecondaryMountEnabled = pSecondaryMount->GetGuidingEnabled();
        pSecondaryMount->SetGuidingEnabled(false);
    }

    m_guideOutputDisabled = true;

    startStr = wxDateTime::Now().FormatISOCombined(' ');
    m_measuring = true;
    m_startTime = ::wxGetUTCTimeMillis().GetValue();
    SetSizerAndFit(m_vSizer);
}

// For a mouse-click on the 'Review Previous' options button, show a pop-up menu for whatever saved GAs are available
void GuidingAsstWin::OnReviewPrevious(wxCommandEvent& event)
{
    std::vector<wxString> entryNames;
    entryNames = pConfig->Profile.GetGroupNames("/GA");

    wxMenu *reviewList = new wxMenu();
    for (int inx = 0; inx < entryNames.size(); inx++)
    {
        reviewList->Append(GA_REVIEW_ITEMS_BASE + inx, entryNames[inx]);
    }
    PopupMenu(reviewList, btnReviewPrev->GetPosition().x,
              btnReviewPrev->GetPosition().y + btnReviewPrev->GetSize().GetHeight());

    wxDELETE(reviewList);
}

// Handle the user's choice of a GA entry for review (see above)
void GuidingAsstWin::OnGAReviewSelection(wxCommandEvent& evt)
{
    int id = evt.GetId();
    wxMenu *menu = static_cast<wxMenu *>(evt.GetEventObject());
    wxString timeStamp = menu->GetLabelText(id);

    reviewMode = true;
    LoadGAResults(timeStamp, &gaDetails);
    m_graphBtn->Enable(gaDetails.BLTNorthMoves.size() > 0);
    DisplayStaticResults(gaDetails);
}

void GuidingAsstWin::DoStop(const wxString& status)
{
    m_measuring = false;
    m_recommendgrid->Show(true);
    m_dlgState = STATE_STOPPED;
    m_measurementsTaken = true;

    FillInstructions(m_dlgState);

    if (m_guideOutputDisabled)
    {
        Debug.Write(wxString::Format("GuidingAssistant: Re-enabling guide output (%d, %d)\n", m_savePrimaryMountEnabled,
                                     m_saveSecondaryMountEnabled));

        if (pMount)
            pMount->SetGuidingEnabled(m_savePrimaryMountEnabled);
        if (pSecondaryMount)
            pSecondaryMount->SetGuidingEnabled(m_saveSecondaryMountEnabled);

        m_guideOutputDisabled = false;
        pFrame->pGuider->SetMultiStarMode(origMultistarMode); // may force an auto-find to refresh secondary star data
        pFrame->SetVariableDelayConfig(origVarDelayConfig.enabled, origVarDelayConfig.shortDelay, origVarDelayConfig.longDelay);
    }

    m_start->Enable(pFrame->pGuider->IsGuiding());
    btnReviewPrev->Enable(GetGAHistoryCount() > 0);
    m_stop->Enable(false);

    if (m_origSubFrames != -1)
    {
        pCamera->UseSubframes = m_origSubFrames ? true : false;
        m_origSubFrames = -1;
    }
}

void GuidingAsstWin::EndBacklashTest(bool completed)
{
    if (!completed)
    {
        m_backlashTool->StopMeasurement();
        m_othergrid->SetCellValue(m_backlash_loc, _("Backlash test aborted, see graph..."));
        m_graphBtn->Enable(m_backlashTool->IsGraphable());
    }

    m_measuringBacklash = false;
    m_backlashCB->Enable(true);
    Layout();
    GetSizer()->Fit(this);

    m_start->Enable(pFrame->pGuider->IsGuiding());
    m_stop->Enable(false);
    MakeRecommendations();
    if (!completed)
    {
        wxCommandEvent dummy;
        OnAppStateNotify(dummy); // Make sure UI is in synch
    }
    DoStop();
}

void GuidingAsstWin::OnStop(wxCommandEvent& event)
{
    bool performBLT = m_backlashCB->IsChecked();
    bool longEnough;
    if (m_elapsedSecs < GA_MIN_SAMPLING_PERIOD && !m_measuringBacklash)
    {
        SampleWait waitDlg(GA_MIN_SAMPLING_PERIOD - m_elapsedSecs, performBLT);
        longEnough = (waitDlg.ShowModal() == wxOK);
    }
    else
        longEnough = true;

    m_gaStatus->SetLabel(wxEmptyString);
    if (longEnough && performBLT)
    {
        if (!m_measuringBacklash) // Run the backlash test after the sampling was completed
        {
            m_measuringBacklash = true;
            if (m_origSubFrames == -1)
                m_origSubFrames = pCamera->UseSubframes ? 1 : 0;
            pCamera->UseSubframes = false;

            m_gaStatus->SetLabelText(_("Measuring backlash... ") + m_backlashTool->GetLastStatus());
            Layout();
            GetSizer()->Fit(this);
            m_backlashCB->Enable(false); // Don't let user turn it off once we've started
            m_measuring = false;
            m_backlashTool->StartMeasurement(decDriftPerMin);
            m_instructions->SetLabel(_("Measuring backlash... "));
        }
        else
        {
            // User hit stop during bl test
            m_gaStatus->SetLabelText(wxEmptyString);
            EndBacklashTest(false);
        }
    }
    else
    {
        if (longEnough)
            MakeRecommendations();
        DoStop();
    }
}

void GuidingAsstWin::OnAppStateNotify(wxCommandEvent& WXUNUSED(event))
{
    if (m_measuring || m_measuringBacklash)
    {
        if (!pFrame->pGuider->IsGuiding())
        {
            // if guiding stopped, stop measuring
            DoStop(_("Guiding stopped"));
        }
    }
    else
    {
        bool can_start = pFrame->pGuider->IsGuiding();
        m_start->Enable(can_start);
        if (can_start)
            m_dlgState = STATE_START_READY;
        else
            m_dlgState = STATE_NO_STAR;
        FillInstructions(m_dlgState);
    }
}

void GuidingAsstWin::OnClose(wxCloseEvent& evt)
{
    DoStop();

    // save the window position
    int x, y;
    GetPosition(&x, &y);
    pConfig->Global.SetInt("/GuidingAssistant/pos.x", x);
    pConfig->Global.SetInt("/GuidingAssistant/pos.y", y);

    if (m_flushConfig)
    {
        pConfig->Flush();
        m_flushConfig = false;
    }

    Destroy();
}

void GuidingAsstWin::DisplayStaticResults(const GADetails& details)
{
    wxString SEC(_("s"));
    wxString MSEC(_("ms"));
    wxString PX(_("px"));
    wxString ARCSEC(_("arc-sec"));
    wxString ARCMIN(_("arc-min"));
    wxString PXPERMIN(_("px/min"));
    wxString PXPERSEC(_("px/sec"));
    wxString ARCSECPERMIN(_("arc-sec/min"));
    wxString ARCSECPERSEC(_("arc-sec/sec"));

    // Display high-freq stats
    m_statusgrid->SetCellValue(m_timestamp_loc, details.TimeStamp);
    m_statusgrid->SetCellValue(m_exposuretime_loc, details.ExposureTime);
    m_statusgrid->SetCellValue(m_snr_loc, details.SNR);
    m_statusgrid->SetCellValue(m_starmass_loc, details.StarMass);
    m_statusgrid->SetCellValue(m_elapsedtime_loc, details.ElapsedTime);
    m_statusgrid->SetCellValue(m_samplecount_loc, details.SampleCount);

    // Fill other grids
    m_displacementgrid->SetCellValue(m_ra_rms_loc, details.RA_HPF_RMS);
    m_displacementgrid->SetCellValue(m_dec_rms_loc, details.Dec_HPF_RMS);
    m_displacementgrid->SetCellValue(m_total_rms_loc, details.Total_HPF_RMS);
    m_othergrid->SetCellValue(m_ra_peak_loc, details.RAPeak);
    m_othergrid->SetCellValue(m_dec_peak_loc, details.DecPeak);
    m_othergrid->SetCellValue(m_ra_peakpeak_loc, details.RAPeak_Peak);
    m_othergrid->SetCellValue(m_ra_drift_loc, details.RADriftRate);
    m_othergrid->SetCellValue(m_ra_peak_drift_loc, details.RAMaxDriftRate);
    m_othergrid->SetCellValue(m_ra_drift_exp_loc, details.DriftLimitingExposure);
    m_othergrid->SetCellValue(m_dec_drift_loc, details.DecDriftRate);
    m_othergrid->SetCellValue(m_backlash_loc, details.BackLashInfo);
    m_othergrid->SetCellValue(m_pae_loc, details.PAError);

    if (details.Recommendations.size() > 0)
        DisplayStaticRecommendations(details);
}

void GuidingAsstWin::UpdateInfo(const GuideStepInfo& info)
{
    // Update the time measures
    wxLongLong_t elapsedms = ::wxGetUTCTimeMillis().GetValue() - m_startTime;
    m_elapsedSecs = (double) elapsedms / 1000.0;
    // add offset info to various stats accumulations
    m_stats.AddSample(MakeGASample(info));

    GADetails d;
    FormatGAResults(m_stats, startStr, elapsedms, &d);

    m_statusgrid->SetCellValue(m_timestamp_loc, d.TimeStamp);
    m_statusgrid->SetCellValue(m_exposuretime_loc, d.ExposureTime);
    m_statusgrid->SetCellValue(m_snr_loc, d.SNR);
    m_statusgrid->SetCellValue(m_starmass_loc, d.StarMass);
    m_statusgrid->SetCellValue(m_elapsedtime_loc, d.ElapsedTime);
    m_statusgrid->SetCellValue(m_samplecount_loc, d.SampleCount);

    if (m_stats.Count() > 1)
    {
        // Update the running estimates of dec drift and polar alignment error
        decDriftPerMin = m_stats.DecDriftPxPerMin();
        bool decKnown;
        alignmentError = GAPolarAlignError(m_stats, &decKnown);

        // update grid display w/ running stats
        m_displacementgrid->SetCellValue(m_ra_rms_loc, d.RA_HPF_RMS);
        m_displacementgrid->SetCellValue(m_dec_rms_loc, d.Dec_HPF_RMS);
        m_displacementgrid->SetCellValue(m_total_rms_loc, d.Total_HPF_RMS);
        m_othergrid->SetCellValue(m_ra_peak_loc, d.RAPeak);
        m_othergrid->SetCellValue(m_dec_peak_loc, d.DecPeak);
        m_othergrid->SetCellValue(m_ra_peakpeak_loc, d.RAPeak_Peak);
        m_othergrid->SetCellValue(m_ra_drift_loc, d.RADriftRate);
        m_othergrid->SetCellValue(m_ra_peak_drift_loc, d.RAMaxDriftRate);
        m_othergrid->SetCellValue(m_ra_drift_exp_loc, d.DriftLimitingExposure);
        m_othergrid->SetCellValue(m_dec_drift_loc, d.DecDriftRate);
        m_othergrid->SetCellValue(m_pae_loc, d.PAError);
    }
}

// ---------------------------------------------------------------------------
// Headless Guiding Assistant (guiding_assistant_* JSON-RPC methods)
//
// The same measurement as the dialog, without the dialog: guide output is
// disabled while the star drifts (guiding keeps running, so samples arrive
// through the same NotifyGuideStep hook in mount-axis coordinates), then the
// optional Dec backlash test runs through the same BacklashTool, and the
// shared helpers above compute, log and save the recommendations. Every
// setting changed for the run is restored at the end, whether it completes,
// is stopped, or fails. The run always ends from the event loop (never from
// inside the guider's frame processing), so it can stop guiding it started.
// ---------------------------------------------------------------------------

struct HeadlessGA
{
    GuidingAssistant::ApiState state = GuidingAssistant::API_IDLE;
    double durationS = 0.;
    bool measureBacklash = false;
    ga_math::Accumulator stats;
    wxString startStr;
    wxLongLong_t startTime = 0; // ms, at the first sample
    double elapsedS = 0.;
    bool waitingForGuiding = false; // guiding was started for the run and has not begun yet
    bool startedGuiding = false; // return to looping at the end
    bool endSamplingPending = false;
    bool endBacklashPending = false;
    bool appStateCheckPending = false;

    // settings changed for the run
    bool guideOutputDisabled = false;
    bool savePrimaryMountEnabled = false;
    bool saveSecondaryMountEnabled = false;
    bool origMultistarMode = false;
    VarDelayCfg origVarDelayConfig = {};
    int origSubFrames = -1;

    // backlash test
    std::unique_ptr<BacklashTool> backlashTool;
    bool backlashValid = false;
    int backlashMs = 0;
    double backlashSigmaMs = 0.;
    double backlashPx = 0.;
    wxString backlashInfo; // formatted result, as saved in the GA history
    wxString backlashStatus;

    // results
    bool hasRecs = false;
    ga_math::Recommendations recs = {};
    std::vector<GARecommendation> recList;
    wxString error;
};

static HeadlessGA s_ga;

static bool HeadlessGARunning()
{
    return s_ga.state == GuidingAssistant::API_MEASURING || s_ga.state == GuidingAssistant::API_MEASURING_BACKLASH;
}

static double HeadlessGASamplingElapsed()
{
    if (s_ga.stats.Count() == 0)
        return 0.;
    return (double) (::wxGetUTCTimeMillis().GetValue() - s_ga.startTime) / 1000.0;
}

static void HeadlessGARestore()
{
    if (s_ga.startedGuiding)
    {
        // The run started guiding from looping: go back to looping before guide output is re-enabled
        s_ga.startedGuiding = false;
        if (pFrame->pGuider->IsCalibratingOrGuiding())
        {
            Debug.Write("GuidingAssistant(api): returning to looping\n");
            pFrame->StartLooping();
        }
    }

    if (s_ga.guideOutputDisabled)
    {
        Debug.Write(wxString::Format("GuidingAssistant(api): Re-enabling guide output (%d, %d)\n", s_ga.savePrimaryMountEnabled,
                                     s_ga.saveSecondaryMountEnabled));

        if (pMount)
            pMount->SetGuidingEnabled(s_ga.savePrimaryMountEnabled);
        if (pSecondaryMount)
            pSecondaryMount->SetGuidingEnabled(s_ga.saveSecondaryMountEnabled);

        s_ga.guideOutputDisabled = false;
        pFrame->pGuider->SetMultiStarMode(s_ga.origMultistarMode); // may force an auto-find to refresh secondary star data
        pFrame->SetVariableDelayConfig(s_ga.origVarDelayConfig.enabled, s_ga.origVarDelayConfig.shortDelay,
                                       s_ga.origVarDelayConfig.longDelay);
    }

    if (s_ga.origSubFrames != -1)
    {
        if (pCamera)
            pCamera->UseSubframes = s_ga.origSubFrames ? true : false;
        s_ga.origSubFrames = -1;
    }
}

static void HeadlessGAFail(const wxString& msg)
{
    Debug.Write("GuidingAssistant(api): failed: " + msg + "\n");
    if (s_ga.state == GuidingAssistant::API_MEASURING)
        s_ga.elapsedS = HeadlessGASamplingElapsed();
    s_ga.state = GuidingAssistant::API_FAILED;
    s_ga.error = msg;
    HeadlessGARestore();
    EvtServer.NotifyGuidingAssistantDone();
}

static void HeadlessGAComplete()
{
    if (!pCamera || !pMount || !TheScope())
    {
        HeadlessGAFail(_("equipment disconnected"));
        return;
    }

    CalibrationDetails calDetails;
    s_ga.recs = ComputeGARecommendations(s_ga.stats, s_ga.origMultistarMode, s_ga.backlashValid, s_ga.backlashMs,
                                         s_ga.backlashTool && s_ga.backlashTool->GetBacklashExempted(), &calDetails);
    s_ga.recList = BuildGARecommendations(s_ga.recs, calDetails, s_ga.backlashMs);
    s_ga.hasRecs = true;

    // Same guide-log output and GA history entry as the dialog, so headless runs show up in "Review previous"
    GADetails d;
    FormatGAResults(s_ga.stats, s_ga.startStr, (wxLongLong_t) (s_ga.elapsedS * 1000.0), &d);
    d.DriftLimitingExposure = FormatRefinedDriftLimitingExposure(s_ga.recs);
    d.BackLashInfo = s_ga.backlashInfo;
    LogGAResults(d);
    LogGARecommendations(s_ga.recList);
    GuideLog.NotifyGACompleted();
    int backlashRecommendedMs = s_ga.recs.hasBacklash ? s_ga.recs.backlash.recommendedMs : 0;
    SaveGAHistory(s_ga.startStr, d, s_ga.recs.minMove.seeingEstimatePx, s_ga.stats.DecDriftPxPerMin(), s_ga.recs,
                  s_ga.backlashMs, backlashRecommendedMs, JoinGARecommendations(s_ga.recList), s_ga.backlashTool.get());
    Debug.Write("End of Guiding Assistant output....\n");

    s_ga.state = GuidingAssistant::API_DONE;
    HeadlessGARestore();
    EvtServer.NotifyGuidingAssistantDone();
}

// The sampling period is over: run the backlash test, or finish
static void HeadlessGAEndSampling()
{
    s_ga.endSamplingPending = false;
    if (s_ga.state != GuidingAssistant::API_MEASURING)
        return;

    s_ga.elapsedS = HeadlessGASamplingElapsed();

    if (s_ga.stats.Count() < 2)
    {
        HeadlessGAFail(_("not enough samples were collected"));
        return;
    }

    if (s_ga.measureBacklash && s_ga.backlashTool)
    {
        Debug.Write("GuidingAssistant(api): starting backlash test\n");
        s_ga.state = GuidingAssistant::API_MEASURING_BACKLASH;
        if (s_ga.origSubFrames == -1)
            s_ga.origSubFrames = pCamera->UseSubframes ? 1 : 0;
        pCamera->UseSubframes = false;
        s_ga.backlashTool->StartMeasurement(s_ga.stats.DecDriftPxPerMin());
        s_ga.backlashStatus = s_ga.backlashTool->GetLastStatus();
        EvtServer.NotifyGuidingAssistantProgress();
    }
    else
        HeadlessGAComplete();
}

static void HeadlessGAEndBacklash(bool completed)
{
    s_ga.endBacklashPending = false;
    if (s_ga.state != GuidingAssistant::API_MEASURING_BACKLASH)
        return;
    if (!completed && !s_ga.backlashValid)
        s_ga.backlashTool->StopMeasurement();
    HeadlessGAComplete();
}

// From the guider's frame processing: finish the backlash test from the event loop
static void HeadlessGAScheduleEndBacklash(bool completed)
{
    if (s_ga.endBacklashPending)
        return;
    s_ga.endBacklashPending = true;
    pFrame->CallAfter([completed]() { HeadlessGAEndBacklash(completed); });
}

static void HeadlessGAGuideStep(const GuideStepInfo& info)
{
    if (s_ga.state != GuidingAssistant::API_MEASURING || s_ga.endSamplingPending)
        return;

    s_ga.waitingForGuiding = false;
    if (s_ga.stats.Count() == 0)
        s_ga.startTime = ::wxGetUTCTimeMillis().GetValue();
    s_ga.stats.AddSample(MakeGASample(info));
    s_ga.elapsedS = HeadlessGASamplingElapsed();

    EvtServer.NotifyGuidingAssistantProgress();

    if (s_ga.elapsedS >= s_ga.durationS)
    {
        // like pressing Stop in the dialog; deferred out of the guide-step processing
        s_ga.endSamplingPending = true;
        pFrame->CallAfter([]() { HeadlessGAEndSampling(); });
    }
}

static void HeadlessGABacklashStep(const PHD_Point& camLoc)
{
    if (s_ga.state != GuidingAssistant::API_MEASURING_BACKLASH || s_ga.endBacklashPending)
        return;

    BacklashTool *blt = s_ga.backlashTool.get();
    blt->DecMeasurementStep(camLoc);
    s_ga.backlashStatus = blt->GetLastStatus();

    if (blt->GetBltState() == BacklashTool::BLT_STATE_COMPLETED)
    {
        try
        {
            BacklashTool::MeasurementResults qual = blt->GetMeasurementQuality();
            bool usable = qual == BacklashTool::MEASUREMENT_VALID || qual == BacklashTool::MEASUREMENT_TOO_FEW_NORTH;
            if (usable)
            {
                s_ga.backlashValid = true;
                s_ga.backlashPx = blt->GetBacklashResultPx();
                s_ga.backlashMs = blt->GetBacklashResultMs();
                double bltSigmaPx;
                blt->GetBacklashSigma(&bltSigmaPx, &s_ga.backlashSigmaMs);
                wxString outStr;
                s_ga.backlashInfo =
                    FormatBacklashResult(qual, s_ga.backlashMs, s_ga.backlashSigmaMs, s_ga.backlashPx, bltSigmaPx, &outStr);
                outStr += "\n";
                GuideLog.NotifyGAResult("Backlash=" + outStr);
                Debug.Write("BLT: Reported result = " + outStr);
            }
            HeadlessGAScheduleEndBacklash(usable);
        }
        catch (const wxString& msg)
        {
            Debug.Write(wxString::Format("GA-BLT(api): fault in completion-processing at %d, %s\n", __LINE__, msg));
            HeadlessGAScheduleEndBacklash(false);
        }
    }
    else if (blt->GetBltState() == BacklashTool::BLT_STATE_ABORTED)
        HeadlessGAScheduleEndBacklash(false);
    else
        EvtServer.NotifyGuidingAssistantProgress();
}

// Posted on every app-state change while a run is active
static void HeadlessGACheckAppState()
{
    s_ga.appStateCheckPending = false;

    if (s_ga.state == GuidingAssistant::API_MEASURING)
    {
        Guider *guider = pFrame->pGuider;
        if (guider->IsGuiding())
            return;
        if (s_ga.waitingForGuiding && guider->IsCalibratingOrGuiding())
            return; // guiding is still starting
        HeadlessGAFail(s_ga.waitingForGuiding ? _("guiding did not start") : _("guiding stopped"));
    }
    else if (s_ga.state == GuidingAssistant::API_MEASURING_BACKLASH)
    {
        // The measurement itself is complete; report it without the backlash result
        if (!pFrame->pGuider->IsGuiding())
            HeadlessGAEndBacklash(false);
    }
}

const char *GuidingAssistant::ApiStateName(ApiState state)
{
    switch (state)
    {
    case API_MEASURING:
        return "measuring";
    case API_MEASURING_BACKLASH:
        return "measuring_backlash";
    case API_DONE:
        return "done";
    case API_FAILED:
        return "failed";
    case API_IDLE:
    default:
        return "idle";
    }
}

bool GuidingAssistant::ApiIsRunning()
{
    return HeadlessGARunning();
}

bool GuidingAssistant::ApiStart(double durationS, bool measureBacklash, wxString *error)
{
    if (HeadlessGARunning())
    {
        *error = _("the Guiding Assistant is already running - call guiding_assistant_stop first");
        return false;
    }
    if (pFrame->pGuidingAssistant)
    {
        *error = _("the Guiding Assistant dialog is open");
        return false;
    }
    if (!pCamera || !pCamera->Connected)
    {
        *error = _("camera is not connected");
        return false;
    }
    if (!pMount || !pMount->IsConnected() || !TheScope())
    {
        *error = _("mount is not connected");
        return false;
    }

    Guider *guider = pFrame->pGuider;
    if (guider->IsCalibrating())
    {
        *error = _("cannot start the Guiding Assistant while calibrating");
        return false;
    }
    if (guider->IsPaused())
    {
        *error = _("guiding is paused");
        return false;
    }
    bool guiding = guider->IsGuiding();
    if (!guiding)
    {
        if (!pFrame->CaptureActive || guider->GetState() != STATE_SELECTED)
        {
            *error = _("no star selected - start looping and select a star first (loop + find_star), or start guiding");
            return false;
        }
        if (!pMount->IsCalibrated() || (pSecondaryMount && !pSecondaryMount->IsCalibrated()))
        {
            *error = _("the mount is not calibrated - calibrate (start guiding once) before running the Guiding Assistant");
            return false;
        }
    }

    std::unique_ptr<BacklashTool> backlashTool;
    if (measureBacklash)
    {
        backlashTool.reset(new BacklashTool());
        if (backlashTool->GetBltState() == BacklashTool::BLT_STATE_ABORTED)
        {
            *error = backlashTool->GetLastStatus();
            return false;
        }
    }

    s_ga = HeadlessGA();
    s_ga.durationS = durationS;
    s_ga.measureBacklash = measureBacklash;
    s_ga.backlashTool = std::move(backlashTool);
    s_ga.stats.Reset((double) pFrame->RequestedExposureDuration() / 1000.0);
    s_ga.startStr = wxDateTime::Now().FormatISOCombined(' ');

    // What the dialog does on open (variable delay off) and on Start (single star, guide output off)
    s_ga.origMultistarMode = guider->GetMultiStarMode();
    guider->SetMultiStarMode(false);
    s_ga.origVarDelayConfig = pFrame->GetVariableDelayConfig();
    pFrame->SetVariableDelayConfig(false, s_ga.origVarDelayConfig.shortDelay, s_ga.origVarDelayConfig.longDelay);

    Debug.AddLine(wxString::Format("GuidingAssistant(api): start duration=%.0fs backlash=%d; disabling guide output", durationS,
                                   measureBacklash));
    s_ga.savePrimaryMountEnabled = pMount->GetGuidingEnabled();
    pMount->SetGuidingEnabled(false);
    if (pSecondaryMount)
    {
        s_ga.saveSecondaryMountEnabled = pSecondaryMount->GetGuidingEnabled();
        pSecondaryMount->SetGuidingEnabled(false);
    }
    s_ga.guideOutputDisabled = true;
    s_ga.state = API_MEASURING;

    if (!guiding)
    {
        // Looping with a calibrated mount: start guiding, with output already disabled, so the star drifts
        s_ga.waitingForGuiding = true;
        s_ga.startedGuiding = true;
        pFrame->GuideButtonClick(false, _T("GuidingAssistant:start"));
        if (!guider->IsCalibratingOrGuiding())
        {
            s_ga.startedGuiding = false;
            HeadlessGAFail(_("could not start guiding"));
            *error = s_ga.error;
            return false;
        }
    }

    return true;
}

bool GuidingAssistant::ApiStop(wxString *error)
{
    switch (s_ga.state)
    {
    case API_MEASURING:
        if (s_ga.stats.Count() < 2)
        {
            HeadlessGAFail(_("stopped before enough samples were collected"));
            return true;
        }
        // finish now with the samples so far; the backlash test is skipped
        Debug.Write("GuidingAssistant(api): stopped by client, finishing early\n");
        s_ga.elapsedS = HeadlessGASamplingElapsed();
        HeadlessGAComplete();
        return true;

    case API_MEASURING_BACKLASH:
        // like the dialog's Stop during the backlash test: results without the backlash measurement
        Debug.Write("GuidingAssistant(api): backlash test stopped by client\n");
        HeadlessGAEndBacklash(false);
        return true;

    default:
        *error = _("the Guiding Assistant is not running");
        return false;
    }
}

void GuidingAssistant::ApiGetStatus(ApiStatus *st)
{
    *st = ApiStatus();
    st->state = s_ga.state;
    if (s_ga.state == API_IDLE)
        return;

    st->durationS = s_ga.durationS;
    st->measureBacklash = s_ga.measureBacklash;
    st->elapsedS = s_ga.state == API_MEASURING ? HeadlessGASamplingElapsed() : s_ga.elapsedS;
    st->samples = s_ga.stats.Count();
    st->pixelScale = pFrame->GetCameraPixelScale();

    if (st->samples > 1)
    {
        const ga_math::Accumulator& stats = s_ga.stats;
        st->hasStats = true;
        st->raDriftPxMin = stats.RaDriftPxPerMin(st->elapsedS);
        st->decDriftPxMin = stats.DecDriftPxPerMin();
        st->raRmsPx = stats.RaHighFreqRms();
        st->decRmsPx = stats.DecHighFreqRms();
        st->highFreqRmsPx = stats.TotalHighFreqRms();
        st->raPeakPx = stats.RaPeak();
        st->decPeakPx = stats.DecPeak();
        st->raPeakPeakPx = stats.RaPeakToPeak();
        st->raMaxDriftPxSec = stats.MaxRaRatePxPerSec();
        st->snr = stats.AvgSnr();
        st->starMass = stats.AvgMass();
        bool decKnown;
        st->polarAlignErrorArcmin = GAPolarAlignError(stats, &decKnown);
        st->polarAlignErrorIsLowerBound = !decKnown;
    }

    if (s_ga.state == API_MEASURING_BACKLASH)
        st->backlashStatus = s_ga.backlashStatus;

    if (s_ga.backlashValid)
    {
        st->hasBacklash = true;
        st->backlashMs = std::max(0, s_ga.backlashMs);
        st->backlashSigmaMs = s_ga.backlashSigmaMs;
        st->backlashPx = std::max(0., s_ga.backlashPx);
    }

    if (s_ga.hasRecs)
    {
        const ga_math::Recommendations& r = s_ga.recs;
        ApiRecommendations& out = st->recommendations;
        st->hasRecommendations = true;
        out.minMoveRaPx = r.minMove.ra;
        out.minMoveDecPx = r.minMove.dec;
        out.minMoveSeeingBased = r.minMove.seeingBased;
        out.exposureMinS = r.exposure.min;
        out.exposureMaxS = r.exposure.max;
        out.driftLimitingExposureS = r.driftLimitingExposure;
        if (r.hasBacklash && r.backlash.applicable)
        {
            out.hasDecBacklashMs = true;
            out.decBacklashMs = r.backlash.recommendedMs;
        }
        if (r.hasBacklash && r.backlash.advice == ga_math::BacklashAdvice::TooLarge)
            out.decGuideMode =
                Scope::DecGuideModeStr(r.singleDirection == ga_math::DecDirection::South ? DEC_SOUTH : DEC_NORTH);
        if (r.decAlgoLowpass2)
            out.decAlgorithm = "Lowpass2";
        for (const GARecommendation& rec : s_ga.recList)
            out.messages.push_back({ rec.which, rec.msg });
    }

    if (s_ga.state == API_FAILED)
        st->error = s_ga.error;
}

// Shortest available exposure duration (ms) within the recommended range, or -1
static int GAExposureChoice(const ga_math::ExposureRange& range)
{
    for (int ms : pFrame->GetExposureDurations())
    {
        if (ms >= range.min * 1000.0 - 0.5 && ms <= range.max * 1000.0 + 0.5)
            return ms;
    }
    return -1;
}

bool GuidingAssistant::ApiApply(const std::vector<wxString>& which, std::vector<wxString> *applied, wxString *error)
{
    if (HeadlessGARunning())
    {
        *error = _("the Guiding Assistant is still running");
        return false;
    }
    if (s_ga.state != API_DONE || !s_ga.hasRecs)
    {
        *error = _("no Guiding Assistant results to apply - run guiding_assistant_start first");
        return false;
    }
    if (!pMount || !TheScope())
    {
        *error = _("mount is not connected");
        return false;
    }

    // Validate everything before changing anything
    static const char *const ORDER[] = { "exposure",     "min_move_ra",  "dec_algorithm",
                                         "min_move_dec", "dec_backlash", "dec_guide_mode" };
    const ga_math::Recommendations& r = s_ga.recs;
    std::set<wxString> wanted;
    int exposureMs = -1;
    for (const wxString& w : which)
    {
        if (w == "exposure")
        {
            exposureMs = GAExposureChoice(r.exposure);
            if (exposureMs < 0)
            {
                *error = wxString::Format(_("no exposure duration between %.1fs and %.1fs is available - use set_exposure"),
                                          r.exposure.min, r.exposure.max);
                return false;
            }
        }
        else if (w == "min_move_ra")
        {
            if (!r.raMinMove)
            {
                *error = _("the RA guide algorithm has no min-move setting");
                return false;
            }
        }
        else if (w == "min_move_dec")
        {
            if (!r.decMinMove && !(r.decAlgoLowpass2 && std::find(which.begin(), which.end(), "dec_algorithm") != which.end()))
            {
                *error = _("the Dec guide algorithm has no min-move setting");
                return false;
            }
        }
        else if (w == "dec_backlash")
        {
            if (!r.hasBacklash || !r.backlash.applicable)
            {
                *error = _("no Dec backlash compensation is recommended");
                return false;
            }
        }
        else if (w == "dec_guide_mode")
        {
            if (!r.hasBacklash || r.backlash.advice != ga_math::BacklashAdvice::TooLarge)
            {
                *error = _("no Dec guide mode change is recommended");
                return false;
            }
        }
        else if (w == "dec_algorithm")
        {
            if (!r.decAlgoLowpass2)
            {
                *error = _("no Dec algorithm change is recommended");
                return false;
            }
        }
        else
        {
            *error = wxString::Format(_("unknown recommendation '%s' (expected exposure, min_move_ra, min_move_dec, "
                                        "dec_backlash, dec_guide_mode or dec_algorithm)"),
                                      w);
            return false;
        }
        wanted.insert(w);
    }

    // Apply in dependency order: switching the Dec algorithm resets its min-move
    for (const char *name : ORDER)
    {
        if (!wanted.count(name))
            continue;
        wxString w(name);
        bool ok = true;
        if (w == "exposure")
            ok = pFrame->SetExposureDuration(exposureMs);
        else if (w == "min_move_ra")
            ok = ApplyGAMinMove(pMount->GetXGuideAlgorithm(), r.minMove.ra, "RA", "RA_MinMove");
        else if (w == "dec_algorithm")
        {
            pMount->SetYGuideAlgorithm(Mount::GuideAlgorithmFromName("Lowpass2"));
            Debug.Write("GuideAssistant(api) changed Dec algo to Lowpass2\n");
            ok = ConfigureGADecLowpass2(r.minMove.dec);
        }
        else if (w == "min_move_dec")
            ok = ApplyGAMinMove(pMount->GetYGuideAlgorithm(), r.minMove.dec, "Declination", "Dec_MinMove");
        else if (w == "dec_backlash")
            ApplyGADecBacklash(r.backlash.recommendedMs);
        else if (w == "dec_guide_mode")
        {
            TheScope()->SetDecGuideMode(r.singleDirection == ga_math::DecDirection::South ? DEC_SOUTH : DEC_NORTH);
            if (pFrame->pGraphLog)
                pFrame->pGraphLog->UpdateControls();
        }

        if (!ok)
        {
            *error = wxString::Format(_("could not apply %s"), w);
            pConfig->Flush();
            return false;
        }
        applied->push_back(w);
    }

    pConfig->Flush();
    return true;
}

wxWindow *GuidingAssistant::CreateDialogBox()
{
    return new GuidingAsstWin();
}

void GuidingAssistant::NotifyGuideStep(const GuideStepInfo& info)
{
    if (pFrame && pFrame->pGuidingAssistant)
    {
        GuidingAsstWin *win = static_cast<GuidingAsstWin *>(pFrame->pGuidingAssistant);
        if (win->m_measuring)
            win->UpdateInfo(info);
    }
    else
        HeadlessGAGuideStep(info);
}

void GuidingAssistant::NotifyFrameDropped(const FrameDroppedInfo& info)
{
    if (pFrame && pFrame->pGuidingAssistant)
    {
        // anything needed?
    }
    else if (s_ga.state == API_MEASURING && !s_ga.endSamplingPending && s_ga.stats.Count() > 0)
    {
        // guiding carries on through dropped frames; don't let them extend the run
        if (HeadlessGASamplingElapsed() >= s_ga.durationS)
        {
            s_ga.endSamplingPending = true;
            pFrame->CallAfter([]() { HeadlessGAEndSampling(); });
        }
    }
}

void GuidingAssistant::NotifyBacklashStep(const PHD_Point& camLoc)
{
    if (pFrame && pFrame->pGuidingAssistant)
    {
        GuidingAsstWin *win = static_cast<GuidingAsstWin *>(pFrame->pGuidingAssistant);
        if (win->m_measuringBacklash)
            win->BacklashStep(camLoc);
    }
    else
        HeadlessGABacklashStep(camLoc);
}

void GuidingAssistant::NotifyBacklashError()
{
    if (pFrame && pFrame->pGuidingAssistant)
    {
        GuidingAsstWin *win = static_cast<GuidingAsstWin *>(pFrame->pGuidingAssistant);
        if (win->m_measuringBacklash)
            win->BacklashError();
    }
    else if (s_ga.state == API_MEASURING_BACKLASH)
        HeadlessGAScheduleEndBacklash(false);
}

void GuidingAssistant::UpdateUIControls()
{
    // notify GuidingAssistant window to update its controls
    if (pFrame && pFrame->pGuidingAssistant)
    {
        wxCommandEvent event(APPSTATE_NOTIFY_EVENT, pFrame->GetId());
        event.SetEventObject(pFrame);
        wxPostEvent(pFrame->pGuidingAssistant, event);
    }

    // headless run: re-check the guider state from the event loop
    if (pFrame && HeadlessGARunning() && !s_ga.appStateCheckPending)
    {
        s_ga.appStateCheckPending = true;
        pFrame->CallAfter([]() { HeadlessGACheckAppState(); });
    }
}
