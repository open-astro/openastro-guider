/*
 *  guiding_assistant_math.cpp
 *  PHD Guiding
 *
 *  Measurement statistics and recommendation logic for the Guiding
 *  Assistant; see guiding_assistant_math.h.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of openphdguiding.org nor the names of its
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

#include "guiding_assistant_math.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ga_math
{

double FiniteOrZero(double v)
{
    return std::isfinite(v) ? v : 0.0;
}

Accumulator::Accumulator()
{
    Reset(1.0);
}

void Accumulator::Reset(double exposureSecs)
{
    double lp_cutoff = std::max(6.0, 3.0 * exposureSecs);
    double hp_cutoff = 1.0;

    m_hpfRAStats.ClearAll();
    m_lpfRAStats.ClearAll();
    m_hpfDecStats.ClearAll();
    m_decAxisStats.ClearAll();
    m_raAxisStats.ClearAll();
    m_raHPF = HighPassFilter(hp_cutoff, exposureSecs);
    m_raLPF = LowPassFilter(lp_cutoff, exposureSecs);
    m_decHPF = HighPassFilter(hp_cutoff, exposureSecs);
    m_axisTimebase = 0;
    m_startRa = m_lastRa = 0.0;
    m_lastTime = 0.0;
    m_maxRateRA = 0.0;
    m_sumSNR = m_sumMass = 0.0;
}

void Accumulator::AddSample(const Sample& s)
{
    m_hpfRAStats.AddValue(m_raHPF.AddValue(s.ra));
    double prevRAlpf = m_raLPF.GetCurrentLPF();
    double newRAlpf = m_raLPF.AddValue(s.ra);
    if (m_lpfRAStats.GetCount() == 0)
        prevRAlpf = newRAlpf;
    m_lpfRAStats.AddValue(newRAlpf);
    m_hpfDecStats.AddValue(m_decHPF.AddValue(s.dec));
    if (m_decAxisStats.GetCount() == 0)
        m_axisTimebase = s.wallSecs;
    m_decAxisStats.AddGuideInfo(s.wallSecs - m_axisTimebase, s.dec, 0);
    m_raAxisStats.AddGuideInfo(s.wallSecs - m_axisTimebase, s.ra, 0);

    // Maximum interval RA movement rate, using the low-pass-filtered data
    if (m_lpfRAStats.GetCount() == 1)
    {
        m_startRa = s.ra;
        m_maxRateRA = 0.0;
    }
    else
    {
        double dt = s.time - m_lastTime;
        if (dt > 0.0001)
        {
            double raRate = fabs(newRAlpf - prevRAlpf) / dt;
            if (raRate > m_maxRateRA)
                m_maxRateRA = raRate;
        }
    }

    m_lastTime = s.time;
    m_lastRa = s.ra;
    m_sumSNR += s.snr;
    m_sumMass += s.mass;
}

unsigned int Accumulator::Count() const
{
    return m_lpfRAStats.GetCount();
}

double Accumulator::RaHighFreqRms() const
{
    return m_hpfRAStats.GetSigma();
}

double Accumulator::DecHighFreqRms() const
{
    return m_hpfDecStats.GetSigma();
}

double Accumulator::TotalHighFreqRms() const
{
    return hypot(RaHighFreqRms(), DecHighFreqRms());
}

double Accumulator::RaPeak() const
{
    return m_raAxisStats.GetMaxDelta();
}

double Accumulator::DecPeak() const
{
    return m_decAxisStats.GetMaxDelta();
}

double Accumulator::RaPeakToPeak() const
{
    if (Count() == 0)
        return 0.0;
    return m_lpfRAStats.GetMaximum() - m_lpfRAStats.GetMinimum();
}

double Accumulator::RaDriftPxPerMin(double elapsedSecs) const
{
    // Raw net displacement; can't smooth this one reliably
    if (Count() == 0 || elapsedSecs <= 0.0)
        return 0.0;
    return (m_lastRa - m_startRa) / elapsedSecs * 60.0;
}

double Accumulator::MaxRaRatePxPerSec() const
{
    return m_maxRateRA;
}

double Accumulator::DecDriftPxPerMin() const
{
    if (m_decAxisStats.GetCount() < 2)
        return 0.0;
    // The fit runs against whole-second timestamps (as it always has in the dialog), so with sub-second guide
    // cadence the first samples share one time value and the fit is 0/0 until the clock ticks over
    double slope, intcpt;
    m_decAxisStats.GetLinearFitResults(&slope, &intcpt);
    return FiniteOrZero(60.0 * slope);
}

double Accumulator::AvgSnr() const
{
    unsigned int n = Count();
    return n ? m_sumSNR / n : 0.0;
}

double Accumulator::AvgMass() const
{
    unsigned int n = Count();
    return n ? m_sumMass / n : 0.0;
}

double PolarAlignErrorArcmin(double decDriftPxPerMin, double pxScale, bool decKnown, double decRadians)
{
    double cosdec = decKnown ? cos(decRadians) : 1.0; // assume declination 0
    return FiniteOrZero(3.8197 * fabs(decDriftPxPerMin) * pxScale / cosdec);
}

MinMoveRecs ComputeMinMoveRecs(const AxisStats& decAxis, const MinMoveInputs& in)
{
    AxisStats decVals;
    double bestEstimate = 1000;
    double slope = 0;
    double intcpt = 0;
    double correctedRMS;
    const int MEASUREMENT_WINDOW_SIZE = 120; // seconds
    const int WINDOW_ADJUSTMENT = MEASUREMENT_WINDOW_SIZE / 2;

    int lastInx = (int) decAxis.GetCount() - 1;
    double tStart = decAxis.GetEntry(0).DeltaTime;
    double multiplier_dec = (in.pxScale < 1.5) ? 1.28 : 1.65; // 20% or 10% activity target based on normal distribution
    double minMoveFloor = 0.1;

    if (decAxis.GetLastEntry().DeltaTime - tStart > 1.2 * MEASUREMENT_WINDOW_SIZE) // Long GA run, more than 2.4 minutes
    {
        bool done = false;
        int inx = 0;
        while (!done)
        {
            StarDisplacement val = decAxis.GetEntry(inx);
            decVals.AddGuideInfo(val.DeltaTime, val.StarPos, 0);
            // Compute the minimum sigma for sliding, overlapping 2-min elapsed time intervals. Include the final interval if
            // it's >= 1.6 minutes
            if (val.DeltaTime - tStart >= MEASUREMENT_WINDOW_SIZE ||
                (inx == lastInx && val.DeltaTime - tStart >= 0.8 * MEASUREMENT_WINDOW_SIZE))
            {
                if (decVals.GetCount() > 1)
                {
                    double simpleSigma = decVals.GetSigma();
                    decVals.GetLinearFitResults(&slope, &intcpt, &correctedRMS);
                    // If there is little drift relative to the random movements, the drift-correction is irrelevant and can
                    // actually degrade the result.  So don't use the drift-corrected RMS unless it's smaller than the simple
                    // sigma
                    if (correctedRMS < simpleSigma)
                        bestEstimate = std::min(bestEstimate, correctedRMS); // Keep track of the smallest value seen
                    else
                        bestEstimate = std::min(bestEstimate, simpleSigma);
                }
                // Move the start of the next window earlier by 1 minute
                double targetTime = val.DeltaTime - WINDOW_ADJUSTMENT;
                while (decAxis.GetEntry(inx).DeltaTime > targetTime)
                    inx--;
                tStart = decAxis.GetEntry(inx).DeltaTime;

                decVals.ClearAll();
            }
            else
            {
                inx++;
            }
            done = (inx > lastInx);
        }
    }
    else // Normal GA run of <= 2.4 minutes, just use the entire interval for stats
    {
        if (decAxis.GetCount() > 1)
        {
            double simpleSigma = decAxis.GetSigma();
            decAxis.GetLinearFitResults(&slope, &intcpt, &correctedRMS);
            // Don't use the drift-corrected RMS unless it's smaller than the simple sigma (see above)
            bestEstimate = correctedRMS < simpleSigma ? correctedRMS : simpleSigma;
        }
    }

    if (in.multiStar)
    {
        bestEstimate *= 0.9;
        minMoveFloor = 0.05;
    }
    // 65% of Dec recommendation, but 100% for encoder mounts
    double multiplier_ra = in.hpEncoders ? 1.0 : 0.65;

    MinMoveRecs r;
    r.seeingEstimatePx = bestEstimate;
    // round up to next multiple of .05, but do not go below 0.05 pixel
    double const unit = 0.05;
    double roundUpEst = std::max(round(bestEstimate * multiplier_dec / unit + 0.5) * unit, 0.05);
    // Now apply a sanity check - there are still numerous things that could have gone wrong during the GA
    if (in.pxScale * roundUpEst <= 1.25) // Min-move below 1.25 arc-sec is credible
    {
        r.dec = roundUpEst;
        r.seeingBased = true;
    }
    else
    {
        // Just reiterate the estimates made in the new-profile-wiz
        r.dec = in.smartDefaultDecMinMove;
        r.seeingBased = false;
    }
    r.ra = std::max(minMoveFloor, r.dec * multiplier_ra);
    return r;
}

ExposureRange RecommendExposure(double raHighFreqRms, double maxRaRatePxPerSec, bool hpEncoders)
{
    double multiplier_ra = 1.0; // 66% prediction interval
    double ideal_min_exposure = hpEncoders ? 4.0 : 2.0;
    double ideal_max_exposure = hpEncoders ? 8.0 : 4.0;

    // Encoder mounts: fixed range, with variable delay controlling the guide cadence
    if (hpEncoders)
        return ExposureRange { ideal_min_exposure, ideal_max_exposure };

    // adjust the min-exposure downward if drift limiting exposure is lower; then adjust range accordingly
    double drift_exp;
    if (maxRaRatePxPerSec > 0)
        drift_exp = ceil((multiplier_ra * raHighFreqRms / maxRaRatePxPerSec) / 0.5) * 0.5; // Rounded up to nearest 0.5 sec
    else
        drift_exp = ideal_min_exposure;

    double min_rec_range = 2.0;
    ExposureRange e;
    e.min = std::max(1.0, std::min(drift_exp, ideal_min_exposure)); // smaller of drift and ideal, never less than 1.0

    if (drift_exp > e.min)
    {
        if (drift_exp < ideal_max_exposure)
            e.max = std::max(drift_exp, e.min + min_rec_range);
        else
            e.max = ideal_max_exposure;
    }
    else
        e.max = e.min + min_rec_range;

    return e;
}

double DriftLimitingExposure(double raMinMove, double maxRaRatePxPerSec)
{
    return maxRaRatePxPerSec > 0.0 ? raMinMove / maxRaRatePxPerSec : 0.0;
}

BacklashRec RecommendBacklash(int backlashMs, bool hpEncoders)
{
    BacklashRec r;
    if (backlashMs > 0)
        r.recommendedMs = std::max((backlashMs / 10) * 10, 10); // round down to nearest 10ms
    else
        r.recommendedMs = 0;

    bool largeBL = backlashMs > MAX_BACKLASH_COMP_MS;
    if (hpEncoders)
        r.advice = BacklashAdvice::Encoders;
    else if (backlashMs < 100)
        r.advice = BacklashAdvice::Small; // assume it was a small measurement error
    else if (!largeBL)
        r.advice = BacklashAdvice::Compensate;
    else
        r.advice = BacklashAdvice::TooLarge;

    r.applicable = r.advice == BacklashAdvice::Compensate && r.recommendedMs > 100;
    return r;
}

DecDirection SingleDirectionDecGuideMode(double decDriftPxPerMin)
{
    return decDriftPxPerMin >= 0 ? DecDirection::South : DecDirection::North;
}

bool LikelyBacklash(const LikelyBacklashInputs& in)
{
    // Just ran the BLT and the result is too big for BLC
    if (in.bltCompleted && in.bltMs > MAX_BACKLASH_COMP_MS)
        return true;
    // May have tried BLC in the past with a too-large pulse size
    if (in.compPulseMs > MAX_BACKLASH_COMP_MS)
        return true;
    // If guide mode isn't 'Auto' or 'None', user can benefit from larger polar alignment error
    if (!in.decModeAutoOrNone)
        return true;
    // See if the last calibration showed little or no Dec movement to the south
    if (in.calHasDecSteps)
        return in.calSouthDist <= 0.1 * in.calNorthDist;
    return false;
}

PaAdvice RecommendPolarAlignment(double errorArcmin, bool likelyBacklash)
{
    if (errorArcmin <= 5.0)
        return PaAdvice::None;
    // If the mount looks like it has large Dec backlash, ignore alignment error below 10 arc-min
    if (likelyBacklash)
        return errorArcmin > 10.0 ? PaAdvice::UseDriftAlign : PaAdvice::None;
    return errorArcmin < 10.0 ? PaAdvice::Improve : PaAdvice::UseDriftAlign;
}

bool RecommendLowpass2ForDec(bool hpEncoders, bool smallBacklash, const std::string& decAlgoClass)
{
    // Uses encoders or has zero backlash; ResistSwitch automatically rules out AO's
    return (hpEncoders || smallBacklash) && decAlgoClass == "ResistSwitch";
}

int RecommendedBinning(double currScale, int currBinning, double targetScale, const std::vector<int>& choices, int maxBinning)
{
    double bin1scale = currScale / currBinning;
    for (int binning : choices)
    {
        if (bin1scale * binning >= targetScale)
            return binning;
    }
    return maxBinning;
}

Recommendations Recommend(const AxisStats& decAxis, const RecommendationInputs& in)
{
    Recommendations r;

    MinMoveInputs mm;
    mm.pxScale = in.pxScale;
    mm.multiStar = in.multiStar;
    mm.hpEncoders = in.hpEncoders;
    mm.smartDefaultDecMinMove = in.smartDefaultDecMinMove;
    r.minMove = ComputeMinMoveRecs(decAxis, mm);

    r.exposure = RecommendExposure(in.raHighFreqRms, in.maxRaRatePxPerSec, in.hpEncoders);
    r.driftLimitingExposure = DriftLimitingExposure(r.minMove.ra, in.maxRaRatePxPerSec);

    // Binning opportunity if image scale is < 0.5
    r.recommendBinning = in.pxScale < 0.5 && in.currBinning < in.maxBinning;
    r.binning = r.recommendBinning ? RecommendedBinning(in.pxScale, in.currBinning, 0.5, in.binningChoices, in.maxBinning)
                                   : in.currBinning;

    r.recalibrate = in.suspectCalibration;
    r.brighterStar = in.avgSnr < 10.0;

    LikelyBacklashInputs likely = in.likely;
    likely.bltCompleted = in.bltCompleted;
    likely.bltMs = in.backlashMs;
    r.pa = RecommendPolarAlignment(in.polarAlignErrorArcmin, LikelyBacklash(likely));

    r.improveFocus = in.pxScale > 1.0 && in.starHfd > 4.5;
    r.raMinMove = in.raAlgoHasMinMove;
    r.decMinMove = in.decAlgoHasMinMove;

    r.hasBacklash = in.bltCompleted;
    r.backlash = RecommendBacklash(in.bltCompleted ? in.backlashMs : 0, in.hpEncoders);
    r.singleDirection = SingleDirectionDecGuideMode(in.decDriftPxPerMin);
    bool smallBacklash =
        in.bltCompleted && (r.backlash.advice == BacklashAdvice::Small || r.backlash.advice == BacklashAdvice::Encoders);
    r.decAlgoLowpass2 = RecommendLowpass2ForDec(in.hpEncoders, smallBacklash, in.decAlgoClass);

    return r;
}

static const ApplyItem APPLY_ORDER[] = { ApplyItem::Exposure,   ApplyItem::MinMoveRa,   ApplyItem::DecAlgorithm,
                                         ApplyItem::MinMoveDec, ApplyItem::DecBacklash, ApplyItem::DecGuideMode };

const char *ApplyItemName(ApplyItem item)
{
    switch (item)
    {
    case ApplyItem::Exposure:
        return "exposure";
    case ApplyItem::MinMoveRa:
        return "min_move_ra";
    case ApplyItem::DecAlgorithm:
        return "dec_algorithm";
    case ApplyItem::MinMoveDec:
        return "min_move_dec";
    case ApplyItem::DecBacklash:
        return "dec_backlash";
    case ApplyItem::DecGuideMode:
        return "dec_guide_mode";
    }
    return "";
}

bool PlanApply(const std::vector<std::string>& which, std::vector<ApplyItem> *plan, std::string *badName)
{
    plan->clear();
    std::vector<bool> wanted(std::size(APPLY_ORDER), false);
    for (const std::string& name : which)
    {
        size_t i = 0;
        while (i < std::size(APPLY_ORDER) && name != ApplyItemName(APPLY_ORDER[i]))
            i++;
        if (i == std::size(APPLY_ORDER))
        {
            *badName = name;
            return false;
        }
        wanted[i] = true;
    }
    for (size_t i = 0; i < std::size(APPLY_ORDER); i++)
        if (wanted[i])
            plan->push_back(APPLY_ORDER[i]);
    return !plan->empty();
}

std::string ApplyFailureMessage(ApplyItem failed, const std::string& reason, const std::vector<ApplyItem>& applied)
{
    std::string msg = std::string(ApplyItemName(failed)) + ": " + reason;
    if (applied.empty())
        return msg + "; nothing was applied";
    msg += "; already applied and saved: ";
    for (size_t i = 0; i < applied.size(); i++)
        msg += (i ? ", " : "") + std::string(ApplyItemName(applied[i]));
    return msg;
}

bool SameSession(const SessionStamp& run, const SessionStamp& now, std::string *why)
{
    const char *reason = nullptr;
    if (now.profileId != run.profileId)
        reason = "the profile has changed";
    else if (!now.mountConnected || !now.cameraConnected)
        reason = "the camera or mount is disconnected";
    else if (now.mountId != run.mountId || now.cameraId != run.cameraId || now.gearConnects != run.gearConnects)
        reason = "the camera or mount was reconnected";
    if (reason && why)
        *why = reason;
    return reason == nullptr;
}

} // namespace ga_math
