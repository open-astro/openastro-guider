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
#include "monotonic_timers.h"

#include <algorithm>
#include <memory>

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

// ---------------------------------------------------------------------------
// Used by the headless (JSON-RPC) Guiding Assistant: the
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
    // Whole seconds, deliberately: the dialog has always computed the logged / saved RA drift rate from a
    // `long` elapsed time (get_status reports the full-precision rate)
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

// How a completed backlash test ended, from the tool's measurement quality
static ga_math::BacklashOutcome BacklashOutcomeOf(BacklashTool::MeasurementResults qual)
{
    if (qual == BacklashTool::MEASUREMENT_VALID)
        return ga_math::BacklashOutcome::Valid;
    if (qual == BacklashTool::MEASUREMENT_TOO_FEW_NORTH)
        return ga_math::BacklashOutcome::Impaired;
    return ga_math::BacklashOutcome::Aborted;
}

// The "Declination Backlash" cell (and GA history entry) when the test produced no usable result
static wxString BacklashAbortedText()
{
    return _("Backlash test aborted, see graph...");
}

// Usable backlash test result as shown in the "Declination Backlash" cell; *logStr gets the untranslated form
static wxString FormatBacklashResult(ga_math::BacklashOutcome outcome, int backlashMs, double backlashSigmaMs,
                                     double backlashPx, double backlashSigmaPx, wxString *logStr)
{
    auto pixelScale = pFrame->GetCameraPixelScale();
    double bltGearAngle = (backlashPx * pixelScale);
    double bltGearAngleSigma = (backlashSigmaPx * pixelScale);
    *logStr = wxString::FromUTF8(
        ga_math::BacklashLogText(outcome, backlashMs, backlashSigmaMs, bltGearAngle, bltGearAngleSigma).c_str());
    wxString preamble = ga_math::BacklashIsLowerBound(outcome, backlashMs) ? ">=" : "";
    if (outcome == ga_math::BacklashOutcome::Valid)
        return wxString::Format("%s %d  +/-  %0.0f %s (%0.1f  +/-  %0.1f %s)", preamble, wxMax(0, backlashMs), backlashSigmaMs,
                                _("ms"), wxMax(0, bltGearAngle), bltGearAngleSigma, _("arc-sec"));
    return wxString::Format("%s %d  +/-  %s", preamble, wxMax(0, backlashMs), _("ms (test impaired)"));
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
    else if (r.pa == ga_math::PaAdvice::Redo)
        recs.push_back({ "PA", _("Polar alignment error > 10 arc-min; redo the polar alignment to improve it.") });

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

    AlgorithmSlot::Hold held = pMount->LockAlgorithms(); // a move can be computing on the worker (#142 T2b)

    if (algo->GetMinMove() >= 0.0)
    {
        if (!algo->SetMinMove(minMove))
        {
            Debug.Write(wxString::Format("GuideAssistant changed %s to %0.2f\n", debugName, minMove));
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
    {
        AlgorithmSlot::Hold held = pMount->LockAlgorithms(); // a move can be computing on the worker (#142 T2b)
        decAlgo->SetParam("aggressiveness", newAggr);
        decAlgo->SetParam("minMove", decMinMove);
    }
    Debug.Write(
        wxString::Format("GuideAssistant set Lowpass2 aggressiveness = %0.2f, min-move = %0.2f\n", newAggr, decMinMove));
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

// ---------------------------------------------------------------------------
// Headless Guiding Assistant (guiding_assistant_* JSON-RPC methods)
//
// The measurement PHD2's Guiding Assistant dialog made, without it: guide output is
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
    monotonic_timers::RunTimer sampling; // started at the first sample (monotonic_timers.cpp, #235)
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
    ga_math::BacklashOutcome backlashOutcome = ga_math::BacklashOutcome::NotRun;
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

    // snapshot when the run ends, so get_status keeps matching the recommendations / Done event
    bool hasFinalSnapshot = false;
    double finalPixelScale = 0.;
    double finalPaErrorArcmin = 0.;
    bool finalPaLowerBound = false;

    ga_math::SessionStamp session; // profile and gear the run was measured with
};

static HeadlessGA s_ga;
// Bumped by every run start (and app close): event-loop callbacks queued by an older run are ignored
static unsigned int s_gaRunGeneration;
// Camera / mount connections since startup (EquipmentManager reports them), for the apply session check
static unsigned int s_gaGearConnects;

static ga_math::SessionStamp CurrentGASession()
{
    ga_math::SessionStamp st;
    st.profileId = pConfig->GetCurrentProfileId();
    st.gearConnects = s_gaGearConnects;
    st.mountId = reinterpret_cast<std::uintptr_t>(pMount);
    st.cameraId = reinterpret_cast<std::uintptr_t>(pCamera);
    st.mountConnected = pMount && pMount->IsConnected();
    st.cameraConnected = pCamera && pCamera->Connected;
    return st;
}

// Run fn from the event loop, unless a newer run has started (or the app is closing) by then
template<typename F>
static void HeadlessGACallAfter(F fn)
{
    unsigned int generation = s_gaRunGeneration;
    pFrame->CallAfter(
        [generation, fn]()
        {
            if (generation == s_gaRunGeneration)
                fn();
        });
}

// Freeze the values get_status derives from live gear
static void HeadlessGASnapshot()
{
    s_ga.finalPixelScale = pFrame->GetCameraPixelScale();
    bool decKnown = false;
    s_ga.finalPaErrorArcmin = s_ga.stats.Count() > 1 ? GAPolarAlignError(s_ga.stats, &decKnown) : 0.;
    s_ga.finalPaLowerBound = !decKnown;
    s_ga.hasFinalSnapshot = true;
}

// The backlash test produced a result (possibly a lower bound)
static bool HeadlessGABacklashUsable()
{
    return ga_math::BacklashOutcomeUsable(s_ga.backlashOutcome);
}

static bool HeadlessGARunning()
{
    return s_ga.state == GuidingAssistant::API_MEASURING || s_ga.state == GuidingAssistant::API_MEASURING_BACKLASH;
}

static double HeadlessGASamplingElapsed()
{
    if (s_ga.stats.Count() == 0)
        return 0.;
    return s_ga.sampling.ElapsedS();
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
        // Guiding goes on with the same star and lock position, the secondary stars found again (#187); when the
        // run ended looping, this may force an auto-find to refresh secondary star data
        pFrame->pGuider->SetMultiStarMode(s_ga.origMultistarMode);
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
    HeadlessGASnapshot();
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
    s_ga.recs = ComputeGARecommendations(s_ga.stats, s_ga.origMultistarMode, HeadlessGABacklashUsable(), s_ga.backlashMs,
                                         s_ga.backlashTool && s_ga.backlashTool->GetBacklashExempted(), &calDetails);
    s_ga.recList = BuildGARecommendations(s_ga.recs, calDetails, s_ga.backlashMs);
    s_ga.hasRecs = true;
    HeadlessGASnapshot();

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
    if (!completed && !HeadlessGABacklashUsable())
    {
        // as the dialog's EndBacklashTest(false): no usable result (failed, aborted or stopped)
        s_ga.backlashTool->StopMeasurement();
        s_ga.backlashOutcome = ga_math::BacklashOutcome::Aborted;
        s_ga.backlashInfo = BacklashAbortedText();
        Debug.Write("GuidingAssistant(api): backlash test aborted, no usable result\n");
    }
    HeadlessGAComplete();
}

// From the guider's frame processing: finish the backlash test from the event loop
static void HeadlessGAScheduleEndBacklash(bool completed)
{
    if (s_ga.endBacklashPending)
        return;
    s_ga.endBacklashPending = true;
    HeadlessGACallAfter([completed]() { HeadlessGAEndBacklash(completed); });
}

static void HeadlessGAGuideStep(const GuideStepInfo& info)
{
    if (s_ga.state != GuidingAssistant::API_MEASURING || s_ga.endSamplingPending)
        return;

    s_ga.waitingForGuiding = false;
    if (s_ga.stats.Count() == 0)
        s_ga.sampling.Start();
    s_ga.stats.AddSample(MakeGASample(info));
    s_ga.elapsedS = HeadlessGASamplingElapsed();

    EvtServer.NotifyGuidingAssistantProgress();

    if (s_ga.elapsedS >= s_ga.durationS)
    {
        // like pressing Stop in the dialog; deferred out of the guide-step processing
        s_ga.endSamplingPending = true;
        HeadlessGACallAfter([]() { HeadlessGAEndSampling(); });
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
            ga_math::BacklashOutcome outcome = BacklashOutcomeOf(blt->GetMeasurementQuality());
            bool usable = ga_math::BacklashOutcomeUsable(outcome);
            if (usable)
            {
                s_ga.backlashOutcome = outcome;
                s_ga.backlashPx = blt->GetBacklashResultPx();
                s_ga.backlashMs = blt->GetBacklashResultMs();
                double bltSigmaPx;
                blt->GetBacklashSigma(&bltSigmaPx, &s_ga.backlashSigmaMs);
                wxString outStr;
                s_ga.backlashInfo =
                    FormatBacklashResult(outcome, s_ga.backlashMs, s_ga.backlashSigmaMs, s_ga.backlashPx, bltSigmaPx, &outStr);
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
        if (s_ga.waitingForGuiding && guider->IsCalibrating())
        {
            // entering guiding cleared the calibration (an image scale change, #255); the run's guide output is
            // disabled, so it cannot calibrate
            HeadlessGAFail(_("the calibration was cleared when guiding started (the image scale changed) - calibrate "
                             "before running the Guiding Assistant"));
            return;
        }
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

    ++s_gaRunGeneration;
    s_ga = HeadlessGA();
    s_ga.session = CurrentGASession();
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

// Every floating-point field is passed through FiniteOrZero: these go straight into JSON
void GuidingAssistant::ApiGetStatus(ApiStatus *st)
{
    using ga_math::FiniteOrZero;

    *st = ApiStatus();
    st->state = s_ga.state;
    if (s_ga.state == API_IDLE)
        return;

    st->durationS = s_ga.durationS;
    st->measureBacklash = s_ga.measureBacklash;
    st->elapsedS = FiniteOrZero(s_ga.state == API_MEASURING ? HeadlessGASamplingElapsed() : s_ga.elapsedS);
    st->samples = s_ga.stats.Count();
    // once the run has ended, report the values its results were computed with, not the live gear's
    bool ended = s_ga.hasFinalSnapshot && (s_ga.state == API_DONE || s_ga.state == API_FAILED);
    st->pixelScale = FiniteOrZero(ended ? s_ga.finalPixelScale : pFrame->GetCameraPixelScale());

    if (st->samples > 1)
    {
        const ga_math::Accumulator& stats = s_ga.stats;
        st->hasStats = true;
        st->raDriftPxMin = FiniteOrZero(stats.RaDriftPxPerMin(st->elapsedS));
        st->decDriftPxMin = FiniteOrZero(stats.DecDriftPxPerMin());
        st->raRmsPx = FiniteOrZero(stats.RaHighFreqRms());
        st->decRmsPx = FiniteOrZero(stats.DecHighFreqRms());
        st->highFreqRmsPx = FiniteOrZero(stats.TotalHighFreqRms());
        st->raPeakPx = FiniteOrZero(stats.RaPeak());
        st->decPeakPx = FiniteOrZero(stats.DecPeak());
        st->raPeakPeakPx = FiniteOrZero(stats.RaPeakToPeak());
        st->raMaxDriftPxSec = FiniteOrZero(stats.MaxRaRatePxPerSec());
        st->snr = FiniteOrZero(stats.AvgSnr());
        st->starMass = FiniteOrZero(stats.AvgMass());
        if (ended)
        {
            st->polarAlignErrorArcmin = FiniteOrZero(s_ga.finalPaErrorArcmin);
            st->polarAlignErrorIsLowerBound = s_ga.finalPaLowerBound;
        }
        else
        {
            bool decKnown;
            st->polarAlignErrorArcmin = FiniteOrZero(GAPolarAlignError(stats, &decKnown));
            st->polarAlignErrorIsLowerBound = !decKnown;
        }
    }

    if (s_ga.state == API_MEASURING_BACKLASH)
        st->backlashStatus = s_ga.backlashStatus;
    st->backlashResult = ga_math::BacklashOutcomeName(s_ga.backlashOutcome);

    if (HeadlessGABacklashUsable())
    {
        st->hasBacklash = true;
        st->backlashMs = std::max(0, s_ga.backlashMs);
        st->backlashSigmaMs = FiniteOrZero(s_ga.backlashSigmaMs);
        st->backlashPx = std::max(0., FiniteOrZero(s_ga.backlashPx));
    }

    if (s_ga.hasRecs)
    {
        const ga_math::Recommendations& r = s_ga.recs;
        ApiRecommendations& out = st->recommendations;
        st->hasRecommendations = true;
        out.minMoveRaPx = FiniteOrZero(r.minMove.ra);
        out.minMoveDecPx = FiniteOrZero(r.minMove.dec);
        out.minMoveSeeingBased = r.minMove.seeingBased;
        out.exposureMinS = FiniteOrZero(r.exposure.min);
        out.exposureMaxS = FiniteOrZero(r.exposure.max);
        out.driftLimitingExposureS = FiniteOrZero(r.driftLimitingExposure);
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

// Why a recommendation can't be applied right now, checked against the live mount / camera (empty = it can)
static wxString GAApplyBlocker(ga_math::ApplyItem item, const ga_math::Recommendations& r,
                               const std::vector<ga_math::ApplyItem>& plan, int *exposureMs)
{
    using ga_math::ApplyItem;

    switch (item)
    {
    case ApplyItem::Exposure:
        *exposureMs = GAExposureChoice(r.exposure);
        if (*exposureMs < 0)
            return wxString::Format(_("no exposure duration between %.1fs and %.1fs is available - use set_exposure"),
                                    r.exposure.min, r.exposure.max);
        break;

    case ApplyItem::MinMoveRa:
    {
        GuideAlgorithm *algo = pMount->GetXGuideAlgorithm();
        if (!algo || algo->GetMinMove() < 0.0)
            return _("the RA guide algorithm has no min-move setting");
        break;
    }

    case ApplyItem::DecAlgorithm:
    {
        if (!r.decAlgoLowpass2)
            return _("no Dec algorithm change is recommended");
        std::vector<GUIDE_ALGORITHM> valid = Mount::AvailableAlgorithms(pMount->IsStepGuider(), GUIDE_Y);
        if (std::find(valid.begin(), valid.end(), Mount::GuideAlgorithmFromName("Lowpass2")) == valid.end())
            return _("Lowpass2 is not available for Dec on this mount");
        break;
    }

    case ApplyItem::MinMoveDec:
    {
        // switching to Lowpass2 first gives Dec a min-move
        bool switching = std::find(plan.begin(), plan.end(), ApplyItem::DecAlgorithm) != plan.end();
        GuideAlgorithm *algo = pMount->GetYGuideAlgorithm();
        if (!switching && (!algo || algo->GetMinMove() < 0.0))
            return _("the Dec guide algorithm has no min-move setting");
        break;
    }

    case ApplyItem::DecBacklash:
        if (!r.hasBacklash || !r.backlash.applicable)
            return _("no Dec backlash compensation is recommended");
        if (!TheScope()->GetBacklashComp())
            return _("backlash compensation is not available");
        break;

    case ApplyItem::DecGuideMode:
        if (!r.hasBacklash || r.backlash.advice != ga_math::BacklashAdvice::TooLarge)
            return _("no Dec guide mode change is recommended");
        break;
    }

    return wxEmptyString;
}

static wxString GAApplyFailure(ga_math::ApplyItem item, const wxString& reason, const std::vector<ga_math::ApplyItem>& done)
{
    return wxString::FromUTF8(ga_math::ApplyFailureMessage(item, std::string(reason.utf8_str()), done).c_str());
}

bool GuidingAssistant::ApiApply(const std::vector<wxString>& which, std::vector<wxString> *applied, wxString *error)
{
    using ga_math::ApplyItem;

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
    std::string sessionWhy;
    if (!ga_math::SameSession(s_ga.session, CurrentGASession(), &sessionWhy) || !TheScope())
    {
        *error = wxString::Format(_("cannot apply the Guiding Assistant results: %s - run it again"),
                                  wxString::FromUTF8(sessionWhy.empty() ? "the mount is not connected" : sessionWhy.c_str()));
        return false;
    }

    std::vector<std::string> names;
    for (const wxString& w : which)
        names.push_back(std::string(w.utf8_str()));
    std::vector<ApplyItem> plan;
    std::string badName;
    if (!ga_math::PlanApply(names, &plan, &badName))
    {
        *error = wxString::Format(_("unknown recommendation '%s' (expected exposure, min_move_ra, min_move_dec, "
                                    "dec_backlash, dec_guide_mode or dec_algorithm)"),
                                  wxString::FromUTF8(badName.c_str()));
        return false;
    }

    // Check every item can be applied before changing anything, so a request is all-or-nothing
    const ga_math::Recommendations& r = s_ga.recs;
    int exposureMs = -1;
    for (ApplyItem item : plan)
    {
        wxString why = GAApplyBlocker(item, r, plan, &exposureMs);
        if (!why.IsEmpty())
        {
            *error = GAApplyFailure(item, why, {});
            return false;
        }
    }

    // Apply in dependency order. A setter can still refuse; then report what was already applied (and saved).
    std::vector<ApplyItem> done;
    for (ApplyItem item : plan)
    {
        bool ok = true;
        switch (item)
        {
        case ApplyItem::Exposure:
            ok = pFrame->SetExposureDuration(exposureMs);
            break;
        case ApplyItem::MinMoveRa:
            ok = ApplyGAMinMove(pMount->GetXGuideAlgorithm(), r.minMove.ra, "RA", "RA_MinMove");
            break;
        case ApplyItem::DecAlgorithm:
            pMount->SetYGuideAlgorithm(Mount::GuideAlgorithmFromName("Lowpass2"));
            Debug.Write("GuideAssistant(api) changed Dec algo to Lowpass2\n");
            ok = ConfigureGADecLowpass2(r.minMove.dec);
            break;
        case ApplyItem::MinMoveDec:
            ok = ApplyGAMinMove(pMount->GetYGuideAlgorithm(), r.minMove.dec, "Declination", "Dec_MinMove");
            break;
        case ApplyItem::DecBacklash:
            ApplyGADecBacklash(r.backlash.recommendedMs);
            break;
        case ApplyItem::DecGuideMode:
            TheScope()->SetDecGuideMode(r.singleDirection == ga_math::DecDirection::South ? DEC_SOUTH : DEC_NORTH);
            break;
        }

        if (!ok)
        {
            pConfig->Flush();
            *error = GAApplyFailure(item, _("the setting was rejected"), done);
            return false;
        }
        done.push_back(item);
        applied->push_back(ga_math::ApplyItemName(item));
    }

    pConfig->Flush();
    return true;
}

void GuidingAssistant::NotifyGearConnected()
{
    ++s_gaGearConnects;
}

// The app is closing (window close, `shutdown` RPC, SIGTERM -- all end in MyFrame::Shutdown) while a headless
// run is active. The usual end-of-run path may never get to run, so restore the persisted settings the run
// changed now, synchronously and without driving the guider or gear that is about to go away.
void GuidingAssistant::ApiShutdown()
{
    ++s_gaRunGeneration; // drop any queued end-of-run callbacks
    if (!HeadlessGARunning())
        return;

    Debug.Write("GuidingAssistant(api): app closing during a run, restoring settings\n");
    if (s_ga.guideOutputDisabled)
    {
        if (pMount)
            pMount->SetGuidingEnabled(s_ga.savePrimaryMountEnabled);
        if (pSecondaryMount)
            pSecondaryMount->SetGuidingEnabled(s_ga.saveSecondaryMountEnabled);
        // Guider::SetMultiStarMode(true) may auto-select a star and restart guiding; just restore the saved setting
        pConfig->Profile.SetBoolean("/guider/multistar/enabled", s_ga.origMultistarMode);
        pFrame->SetVariableDelayConfig(s_ga.origVarDelayConfig.enabled, s_ga.origVarDelayConfig.shortDelay,
                                       s_ga.origVarDelayConfig.longDelay);
        s_ga.guideOutputDisabled = false;
    }
    if (s_ga.origSubFrames != -1)
    {
        if (pCamera)
            pCamera->UseSubframes = s_ga.origSubFrames ? true : false;
        s_ga.origSubFrames = -1;
    }
    s_ga.startedGuiding = false; // capture is about to stop anyway
    s_ga.state = API_FAILED;
    s_ga.error = _("the guider is shutting down");
}

void GuidingAssistant::NotifyGuideStep(const GuideStepInfo& info)
{
    HeadlessGAGuideStep(info);
}

void GuidingAssistant::NotifyFrameDropped(const FrameDroppedInfo& info)
{
    if (s_ga.state == API_MEASURING && !s_ga.endSamplingPending && s_ga.stats.Count() > 0)
    {
        // guiding carries on through dropped frames; don't let them extend the run
        if (HeadlessGASamplingElapsed() >= s_ga.durationS)
        {
            s_ga.endSamplingPending = true;
            HeadlessGACallAfter([]() { HeadlessGAEndSampling(); });
        }
    }
}

void GuidingAssistant::NotifyBacklashStep(const PHD_Point& camLoc)
{
    HeadlessGABacklashStep(camLoc);
}

void GuidingAssistant::NotifyBacklashError()
{
    if (s_ga.state == API_MEASURING_BACKLASH)
        HeadlessGAScheduleEndBacklash(false);
}

void GuidingAssistant::UpdateUIControls()
{
    // headless run: re-check the guider state from the event loop
    if (pFrame && HeadlessGARunning() && !s_ga.appStateCheckPending)
    {
        s_ga.appStateCheckPending = true;
        HeadlessGACallAfter([]() { HeadlessGACheckAppState(); });
    }
}
