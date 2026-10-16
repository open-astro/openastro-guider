// Unit tests for src/guiding_assistant_math.cpp — the Guiding Assistant's
// measurement statistics and recommendation logic, extracted from
// GuidingAsstWin so the dialog and the headless JSON-RPC controller share
// one implementation.
//
// Links the REAL production guiding_assistant_math.cpp and guiding_stats.cpp
// (both wx-free). Expected values are derived by hand from the formulas the
// dialog has always used (see the comments on each test).

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "guiding_assistant_math.h"

using namespace ga_math;

namespace
{

// One sample every `cadence` seconds, wall clock advancing in whole seconds
// like wxGetCurrentTime() does in the app.
Sample MakeSample(int i, double cadence, double ra, double dec, double snr = 20.0, double mass = 1000.0)
{
    Sample s;
    s.ra = ra;
    s.dec = dec;
    s.time = i * cadence;
    s.wallSecs = 1000 + (long) std::floor(i * cadence);
    s.snr = snr;
    s.mass = mass;
    return s;
}

// Deterministic "seeing": alternate +/- amplitude.
double Jitter(int i, double amplitude)
{
    return (i % 2 == 0) ? amplitude : -amplitude;
}

} // namespace

// ---------------------------------------------------------------------------
// Accumulator: the per-sample statistics the dialog's UpdateInfo() maintains
// ---------------------------------------------------------------------------

TEST(GuidingAssistantAccumulator, StartsEmpty)
{
    Accumulator acc;
    acc.Reset(2.0);
    EXPECT_EQ(acc.Count(), 0u);
    EXPECT_DOUBLE_EQ(acc.DecDriftPxPerMin(), 0.0);
    EXPECT_DOUBLE_EQ(acc.MaxRaRatePxPerSec(), 0.0);
    EXPECT_DOUBLE_EQ(acc.AvgSnr(), 0.0);
}

TEST(GuidingAssistantAccumulator, CountsSamplesAndAveragesSnrAndMass)
{
    Accumulator acc;
    acc.Reset(2.0);
    acc.AddSample(MakeSample(0, 2.0, 0, 0, 10.0, 100.0));
    acc.AddSample(MakeSample(1, 2.0, 0, 0, 20.0, 300.0));
    EXPECT_EQ(acc.Count(), 2u);
    EXPECT_DOUBLE_EQ(acc.AvgSnr(), 15.0);
    EXPECT_DOUBLE_EQ(acc.AvgMass(), 200.0);
}

TEST(GuidingAssistantAccumulator, LinearDecDriftIsRecoveredInPxPerMinute)
{
    // dec drifts 0.5 px/min = 1/120 px/s; the dialog fits dec against the
    // integer wall-clock seconds and scales the slope by 60.
    Accumulator acc;
    acc.Reset(2.0);
    for (int i = 0; i < 60; i++)
        acc.AddSample(MakeSample(i, 2.0, 0.0, (i * 2.0) / 120.0));
    EXPECT_NEAR(acc.DecDriftPxPerMin(), 0.5, 1e-9);
}

TEST(GuidingAssistantAccumulator, RaDriftIsNetDisplacementOverElapsedTime)
{
    // (last ra - first ra) / elapsed seconds * 60
    Accumulator acc;
    acc.Reset(2.0);
    for (int i = 0; i < 31; i++)
        acc.AddSample(MakeSample(i, 2.0, 0.05 * i, 0.0));
    // last ra = 1.5 px after 60 s -> 1.5 px/min
    EXPECT_NEAR(acc.RaDriftPxPerMin(60.0), 1.5, 1e-9);
    // elapsed of zero must not divide by zero
    EXPECT_DOUBLE_EQ(acc.RaDriftPxPerMin(0.0), 0.0);
}

TEST(GuidingAssistantAccumulator, HighFrequencyRmsIgnoresSlowDriftButSeesJitter)
{
    Accumulator drift;
    drift.Reset(2.0);
    Accumulator jitter;
    jitter.Reset(2.0);
    for (int i = 0; i < 100; i++)
    {
        drift.AddSample(MakeSample(i, 2.0, 0.01 * i, 0.01 * i));
        jitter.AddSample(MakeSample(i, 2.0, Jitter(i, 0.5), Jitter(i, 0.5)));
    }
    EXPECT_LT(drift.RaHighFreqRms(), 0.05);
    EXPECT_LT(drift.DecHighFreqRms(), 0.05);
    // the 1 s high-pass passes most of a 2 s-cadence +/-0.5 px jitter
    EXPECT_GT(jitter.RaHighFreqRms(), 0.2);
    EXPECT_GT(jitter.DecHighFreqRms(), 0.2);
    EXPECT_NEAR(jitter.TotalHighFreqRms(), std::hypot(jitter.RaHighFreqRms(), jitter.DecHighFreqRms()), 1e-12);
}

TEST(GuidingAssistantAccumulator, PeaksAreLargestSampleToSampleDeltas)
{
    Accumulator acc;
    acc.Reset(2.0);
    const double ra[] = { 0.0, 0.1, 0.9, 0.8 }; // largest RA step 0.8
    const double dec[] = { 0.0, -0.3, -0.2, 0.2 }; // largest Dec step 0.4
    for (int i = 0; i < 4; i++)
        acc.AddSample(MakeSample(i, 2.0, ra[i], dec[i]));
    EXPECT_NEAR(acc.RaPeak(), 0.8, 1e-12);
    EXPECT_NEAR(acc.DecPeak(), 0.4, 1e-12);
}

TEST(GuidingAssistantAccumulator, MaxRaRateTracksLowPassFilteredSlope)
{
    // A steady 0.05 px/s RA ramp: once the low-pass filter has caught up, the
    // filtered rate approaches the true slope and never exceeds it.
    Accumulator acc;
    acc.Reset(1.0);
    for (int i = 0; i < 200; i++)
        acc.AddSample(MakeSample(i, 1.0, 0.05 * i, 0.0));
    EXPECT_GT(acc.MaxRaRatePxPerSec(), 0.045);
    EXPECT_LE(acc.MaxRaRatePxPerSec(), 0.05 + 1e-9);
    EXPECT_GE(acc.RaPeakToPeak(), 0.0);
}

TEST(GuidingAssistantAccumulator, ResetClearsEverything)
{
    Accumulator acc;
    acc.Reset(2.0);
    for (int i = 0; i < 10; i++)
        acc.AddSample(MakeSample(i, 2.0, 0.1 * i, 0.1 * i));
    acc.Reset(2.0);
    EXPECT_EQ(acc.Count(), 0u);
    EXPECT_EQ(acc.DecAxis().GetCount(), 0u);
    EXPECT_DOUBLE_EQ(acc.DecDriftPxPerMin(), 0.0);
}

// ---------------------------------------------------------------------------
// Polar alignment error (Barrett): 3.8197 * |dec drift px/min| * scale / cos(dec)
// ---------------------------------------------------------------------------

TEST(GuidingAssistantMath, PolarAlignmentErrorFromDecDrift)
{
    EXPECT_NEAR(PolarAlignErrorArcmin(1.0, 2.0, false, 0.0), 7.6394, 1e-9);
    EXPECT_NEAR(PolarAlignErrorArcmin(-1.0, 2.0, false, 0.0), 7.6394, 1e-9); // sign-insensitive
    // at dec 60 deg cos = 0.5 -> doubled
    EXPECT_NEAR(PolarAlignErrorArcmin(1.0, 2.0, true, M_PI / 3.0), 15.2788, 1e-9);
}

// ---------------------------------------------------------------------------
// Min-move recommendation (dialog's GetMinMoveRecs)
// ---------------------------------------------------------------------------

namespace
{
AxisStats JitterDecAxis(int seconds, double amplitude, double driftPxPerMin = 0.0)
{
    AxisStats s;
    for (int t = 0; t <= seconds; t += 2)
        s.AddGuideInfo(t, Jitter(t / 2, amplitude) + driftPxPerMin * t / 60.0, 0);
    return s;
}

MinMoveInputs Inputs(double pxScale = 1.0)
{
    MinMoveInputs in;
    in.pxScale = pxScale;
    in.multiStar = false;
    in.hpEncoders = false;
    in.smartDefaultDecMinMove = 0.42;
    return in;
}
} // namespace

TEST(GuidingAssistantMinMove, SeeingBasedFromDecScatter)
{
    // sigma ~0.2 px; pxscale < 1.5 -> x1.28 = ~0.257, rounded up to the next
    // 0.05 -> 0.30. RA = 65% of Dec (non-encoder mount), floor 0.1.
    MinMoveRecs r = ComputeMinMoveRecs(JitterDecAxis(120, 0.2), Inputs());
    EXPECT_TRUE(r.seeingBased);
    EXPECT_NEAR(r.seeingEstimatePx, 0.2, 0.01);
    EXPECT_NEAR(r.dec, 0.30, 1e-9);
    EXPECT_NEAR(r.ra, 0.195, 1e-9);
}

TEST(GuidingAssistantMinMove, LargeImageScaleUsesLargerMultiplier)
{
    // pxscale >= 1.5 -> x1.65: 0.2*1.65 = 0.33 -> 0.35
    MinMoveRecs r = ComputeMinMoveRecs(JitterDecAxis(120, 0.2), Inputs(2.0));
    EXPECT_TRUE(r.seeingBased);
    EXPECT_NEAR(r.dec, 0.35, 1e-9);
}

TEST(GuidingAssistantMinMove, EncoderMountUsesFullDecValueForRa)
{
    MinMoveInputs in = Inputs();
    in.hpEncoders = true;
    MinMoveRecs r = ComputeMinMoveRecs(JitterDecAxis(120, 0.2), in);
    EXPECT_NEAR(r.ra, r.dec, 1e-12);
}

TEST(GuidingAssistantMinMove, MultiStarShrinksEstimateAndFloor)
{
    // 0.9 * ~0.2 = ~0.18 -> x1.28 = ~0.232 -> 0.25; RA = 0.1625 (above 0.05 floor)
    MinMoveInputs in = Inputs();
    in.multiStar = true;
    MinMoveRecs r = ComputeMinMoveRecs(JitterDecAxis(120, 0.2), in);
    EXPECT_NEAR(r.dec, 0.25, 1e-9);
    EXPECT_NEAR(r.ra, 0.1625, 1e-9);
}

TEST(GuidingAssistantMinMove, RaFloorApplies)
{
    // tiny scatter -> dec rounds to 0.05, RA floor 0.1 (single star)
    MinMoveRecs r = ComputeMinMoveRecs(JitterDecAxis(120, 0.001), Inputs());
    EXPECT_NEAR(r.dec, 0.05, 1e-9);
    EXPECT_NEAR(r.ra, 0.1, 1e-9);
}

TEST(GuidingAssistantMinMove, FailedSanityCheckFallsBackToSmartDefault)
{
    // 0.35 px at 10"/px = 3.5" > 1.25" -> not credible
    MinMoveRecs r = ComputeMinMoveRecs(JitterDecAxis(120, 0.2), Inputs(10.0));
    EXPECT_FALSE(r.seeingBased);
    EXPECT_NEAR(r.dec, 0.42, 1e-12);
    EXPECT_NEAR(r.ra, 0.42 * 0.65, 1e-12);
}

TEST(GuidingAssistantMinMove, NoDataFallsBackToSmartDefault)
{
    AxisStats empty;
    MinMoveRecs r = ComputeMinMoveRecs(empty, Inputs());
    EXPECT_FALSE(r.seeingBased);
    EXPECT_NEAR(r.dec, 0.42, 1e-12);
}

TEST(GuidingAssistantMinMove, LongRunRemovesDriftUsingSlidingWindows)
{
    // 6 minutes with a strong 3 px/min dec drift on top of 0.2 px jitter: the
    // raw scatter is several px, but the windowed drift-corrected RMS stays
    // near the seeing floor. (AxisStats' corrected sigma reads ~0.24 px for
    // this alternating series; 0.24 * 1.28 rounds up to 0.35.)
    AxisStats dec = JitterDecAxis(360, 0.2, 3.0);
    ASSERT_GT(dec.GetSigma(), 3.0);
    MinMoveRecs r = ComputeMinMoveRecs(dec, Inputs());
    EXPECT_TRUE(r.seeingBased);
    EXPECT_LT(r.seeingEstimatePx, 0.25);
    EXPECT_NEAR(r.dec, 0.35, 1e-9);
}

// ---------------------------------------------------------------------------
// Exposure recommendation (dialog's MakeRecommendations)
// ---------------------------------------------------------------------------

TEST(GuidingAssistantExposure, DriftLimitedRangeForNonEncoderMount)
{
    // drift_exp = ceil((0.5/0.1)/0.5)*0.5 = 5; min = max(1, min(5, 2)) = 2;
    // 5 > 2 and 5 >= ideal max 4 -> max = 4
    ExposureRange e = RecommendExposure(0.5, 0.1, false);
    EXPECT_DOUBLE_EQ(e.min, 2.0);
    EXPECT_DOUBLE_EQ(e.max, 4.0);

    // drift_exp = 3 -> min 2, max = max(3, 2 + 2) = 4
    e = RecommendExposure(0.3, 0.1, false);
    EXPECT_DOUBLE_EQ(e.min, 2.0);
    EXPECT_DOUBLE_EQ(e.max, 4.0);

    // drift_exp = 1.5 -> min = 1.5, not greater than min -> max = 3.5
    e = RecommendExposure(0.15, 0.1, false);
    EXPECT_DOUBLE_EQ(e.min, 1.5);
    EXPECT_DOUBLE_EQ(e.max, 3.5);

    // drift_exp below 1 s is clamped to 1 s
    e = RecommendExposure(0.01, 0.1, false);
    EXPECT_DOUBLE_EQ(e.min, 1.0);
    EXPECT_DOUBLE_EQ(e.max, 3.0);
}

TEST(GuidingAssistantExposure, NoRaMotionUsesIdealRange)
{
    ExposureRange e = RecommendExposure(0.5, 0.0, false);
    EXPECT_DOUBLE_EQ(e.min, 2.0);
    EXPECT_DOUBLE_EQ(e.max, 4.0);
}

TEST(GuidingAssistantExposure, EncoderMountRecommendsFourToEight)
{
    ExposureRange e = RecommendExposure(0.5, 0.1, true);
    EXPECT_DOUBLE_EQ(e.min, 4.0);
    EXPECT_DOUBLE_EQ(e.max, 8.0);
}

TEST(GuidingAssistantExposure, DriftLimitingExposure)
{
    EXPECT_NEAR(DriftLimitingExposure(0.3, 0.1), 3.0, 1e-9);
    EXPECT_DOUBLE_EQ(DriftLimitingExposure(0.3, 0.0), 0.0);
}

// ---------------------------------------------------------------------------
// Dec backlash
// ---------------------------------------------------------------------------

TEST(GuidingAssistantBacklash, ClassifiesAndRoundsDownToTenMs)
{
    BacklashRec r = RecommendBacklash(1234, false);
    EXPECT_EQ(r.advice, BacklashAdvice::Compensate);
    EXPECT_EQ(r.recommendedMs, 1230);
    EXPECT_TRUE(r.applicable);

    r = RecommendBacklash(50, false);
    EXPECT_EQ(r.advice, BacklashAdvice::Small);
    EXPECT_EQ(r.recommendedMs, 50);
    EXPECT_FALSE(r.applicable);

    r = RecommendBacklash(5, false); // never recommends less than 10 ms
    EXPECT_EQ(r.recommendedMs, 10);

    r = RecommendBacklash(0, false);
    EXPECT_EQ(r.advice, BacklashAdvice::Small);
    EXPECT_EQ(r.recommendedMs, 0);

    r = RecommendBacklash(3500, false);
    EXPECT_EQ(r.advice, BacklashAdvice::TooLarge);
    EXPECT_FALSE(r.applicable);

    r = RecommendBacklash(3000, false); // the limit itself is still compensable
    EXPECT_EQ(r.advice, BacklashAdvice::Compensate);

    r = RecommendBacklash(1000, true);
    EXPECT_EQ(r.advice, BacklashAdvice::Encoders);
    EXPECT_FALSE(r.applicable);
}

TEST(GuidingAssistantBacklash, ApplyNeedsMoreThan100Ms)
{
    // the dialog only enables "Apply" for a recommendation > 100 ms
    EXPECT_FALSE(RecommendBacklash(105, false).applicable); // rounds to 100
    EXPECT_TRUE(RecommendBacklash(110, false).applicable);
}

TEST(GuidingAssistantBacklash, SingleDirectionFollowsDecDrift)
{
    EXPECT_EQ(SingleDirectionDecGuideMode(0.5), DecDirection::South);
    EXPECT_EQ(SingleDirectionDecGuideMode(0.0), DecDirection::South);
    EXPECT_EQ(SingleDirectionDecGuideMode(-0.5), DecDirection::North);
}

TEST(GuidingAssistantBacklash, LikelyBacklash)
{
    LikelyBacklashInputs in;
    in.bltCompleted = false;
    in.bltMs = 0;
    in.compPulseMs = 0;
    in.decModeAutoOrNone = true;
    in.calHasDecSteps = false;
    in.calNorthDist = 0;
    in.calSouthDist = 0;
    EXPECT_FALSE(LikelyBacklash(in));

    LikelyBacklashInputs bigBlt = in;
    bigBlt.bltCompleted = true;
    bigBlt.bltMs = 3500;
    EXPECT_TRUE(LikelyBacklash(bigBlt));

    LikelyBacklashInputs bigComp = in;
    bigComp.compPulseMs = 3500;
    EXPECT_TRUE(LikelyBacklash(bigComp));

    LikelyBacklashInputs oneWay = in;
    oneWay.decModeAutoOrNone = false;
    EXPECT_TRUE(LikelyBacklash(oneWay));

    LikelyBacklashInputs cal = in;
    cal.calHasDecSteps = true;
    cal.calNorthDist = 20.0;
    cal.calSouthDist = 1.5; // <= 10% of north
    EXPECT_TRUE(LikelyBacklash(cal));
    cal.calSouthDist = 15.0;
    EXPECT_FALSE(LikelyBacklash(cal));
}

// ---------------------------------------------------------------------------
// Polar-alignment, Dec algorithm and binning advice
// ---------------------------------------------------------------------------

TEST(GuidingAssistantMath, PolarAlignmentAdvice)
{
    EXPECT_EQ(RecommendPolarAlignment(4.0, false), PaAdvice::None);
    EXPECT_EQ(RecommendPolarAlignment(7.0, false), PaAdvice::Improve);
    EXPECT_EQ(RecommendPolarAlignment(12.0, false), PaAdvice::UseDriftAlign);
    // likely backlash: only errors above 10' are worth mentioning
    EXPECT_EQ(RecommendPolarAlignment(7.0, true), PaAdvice::None);
    EXPECT_EQ(RecommendPolarAlignment(12.0, true), PaAdvice::UseDriftAlign);
    // boundaries as the dialog draws them: from 10' the stronger advice, but
    // with likely backlash only strictly above 10'
    EXPECT_EQ(RecommendPolarAlignment(5.0, false), PaAdvice::None);
    EXPECT_EQ(RecommendPolarAlignment(10.0, false), PaAdvice::UseDriftAlign);
    EXPECT_EQ(RecommendPolarAlignment(10.0, true), PaAdvice::None);
}

TEST(GuidingAssistantMath, Lowpass2ForDecOnlyReplacesResistSwitch)
{
    EXPECT_TRUE(RecommendLowpass2ForDec(true, false, "ResistSwitch"));
    EXPECT_TRUE(RecommendLowpass2ForDec(false, true, "ResistSwitch"));
    EXPECT_FALSE(RecommendLowpass2ForDec(false, false, "ResistSwitch"));
    EXPECT_FALSE(RecommendLowpass2ForDec(true, true, "Lowpass2"));
}

TEST(GuidingAssistantMath, RecommendedBinningReachesTargetScale)
{
    std::vector<int> choices = { 1, 2, 3, 4 };
    EXPECT_EQ(RecommendedBinning(0.3, 1, 0.5, choices, 4), 2);
    EXPECT_EQ(RecommendedBinning(0.6, 2, 0.5, choices, 4), 2); // 0.3"/px at bin 1
    EXPECT_EQ(RecommendedBinning(0.1, 1, 0.5, choices, 4), 4); // unreachable -> max
}

// ---------------------------------------------------------------------------
// Recommend(): the full recommendation set from one input snapshot
// ---------------------------------------------------------------------------

namespace
{
RecommendationInputs BaseInputs()
{
    RecommendationInputs in;
    in.pxScale = 1.0;
    in.hpEncoders = false;
    in.multiStar = false;
    in.smartDefaultDecMinMove = 0.42;
    in.raHighFreqRms = 0.5;
    in.maxRaRatePxPerSec = 0.1;
    in.decDriftPxPerMin = 0.2;
    in.polarAlignErrorArcmin = 2.0;
    in.avgSnr = 30.0;
    in.starHfd = 3.0;
    in.currBinning = 1;
    in.maxBinning = 4;
    in.binningChoices = { 1, 2, 3, 4 };
    in.suspectCalibration = false;
    in.raAlgoHasMinMove = true;
    in.decAlgoHasMinMove = true;
    in.decAlgoClass = "ResistSwitch";
    in.bltCompleted = false;
    in.backlashMs = 0;
    in.likely.bltCompleted = false;
    in.likely.bltMs = 0;
    in.likely.compPulseMs = 0;
    in.likely.decModeAutoOrNone = true;
    in.likely.calHasDecSteps = false;
    in.likely.calNorthDist = 0;
    in.likely.calSouthDist = 0;
    return in;
}
} // namespace

TEST(GuidingAssistantRecommend, QuietSessionRecommendsMinMoveAndExposureOnly)
{
    Recommendations r = Recommend(JitterDecAxis(120, 0.2), BaseInputs());
    EXPECT_NEAR(r.minMove.dec, 0.30, 1e-9);
    EXPECT_NEAR(r.minMove.ra, 0.195, 1e-9);
    EXPECT_DOUBLE_EQ(r.exposure.min, 2.0);
    EXPECT_DOUBLE_EQ(r.exposure.max, 4.0);
    // drift-limiting exposure is refined with the RA min-move: 0.195 / 0.1
    EXPECT_NEAR(r.driftLimitingExposure, 1.95, 1e-9);
    EXPECT_TRUE(r.raMinMove);
    EXPECT_TRUE(r.decMinMove);
    EXPECT_FALSE(r.recommendBinning);
    EXPECT_FALSE(r.recalibrate);
    EXPECT_FALSE(r.brighterStar);
    EXPECT_EQ(r.pa, PaAdvice::None);
    EXPECT_FALSE(r.improveFocus);
    EXPECT_FALSE(r.hasBacklash);
    EXPECT_FALSE(r.decAlgoLowpass2);
}

TEST(GuidingAssistantRecommend, FlagsProblems)
{
    RecommendationInputs in = BaseInputs();
    in.pxScale = 0.3; // oversampled -> bin 2
    in.avgSnr = 6.0; // faint star
    in.polarAlignErrorArcmin = 12.0;
    in.suspectCalibration = true;
    Recommendations r = Recommend(JitterDecAxis(120, 0.2), in);
    EXPECT_TRUE(r.recommendBinning);
    EXPECT_EQ(r.binning, 2);
    EXPECT_TRUE(r.brighterStar);
    EXPECT_EQ(r.pa, PaAdvice::UseDriftAlign);
    EXPECT_TRUE(r.recalibrate);

    in = BaseInputs();
    in.pxScale = 2.0;
    in.starHfd = 5.0;
    r = Recommend(JitterDecAxis(120, 0.2), in);
    EXPECT_TRUE(r.improveFocus);
}

TEST(GuidingAssistantRecommend, AlgorithmsWithoutMinMoveGetNoMinMoveAdvice)
{
    RecommendationInputs in = BaseInputs();
    in.raAlgoHasMinMove = false;
    in.decAlgoHasMinMove = false;
    Recommendations r = Recommend(JitterDecAxis(120, 0.2), in);
    EXPECT_FALSE(r.raMinMove);
    EXPECT_FALSE(r.decMinMove);
}

TEST(GuidingAssistantRecommend, BacklashResultDrivesCompAndDecAlgorithm)
{
    RecommendationInputs in = BaseInputs();
    in.bltCompleted = true;
    in.backlashMs = 1234;
    Recommendations r = Recommend(JitterDecAxis(120, 0.2), in);
    EXPECT_TRUE(r.hasBacklash);
    EXPECT_EQ(r.backlash.advice, BacklashAdvice::Compensate);
    EXPECT_EQ(r.backlash.recommendedMs, 1230);
    EXPECT_FALSE(r.decAlgoLowpass2); // real backlash -> keep ResistSwitch

    in.backlashMs = 40; // negligible backlash -> Lowpass2 suits Dec better
    r = Recommend(JitterDecAxis(120, 0.2), in);
    EXPECT_EQ(r.backlash.advice, BacklashAdvice::Small);
    EXPECT_TRUE(r.decAlgoLowpass2);

    in.backlashMs = 4000;
    in.decDriftPxPerMin = -0.3;
    r = Recommend(JitterDecAxis(120, 0.2), in);
    EXPECT_EQ(r.backlash.advice, BacklashAdvice::TooLarge);
    EXPECT_EQ(r.singleDirection, DecDirection::North);
}

TEST(GuidingAssistantRecommend, EncoderMountRecommendsLowpass2WithoutBacklashTest)
{
    RecommendationInputs in = BaseInputs();
    in.hpEncoders = true;
    Recommendations r = Recommend(JitterDecAxis(120, 0.2), in);
    EXPECT_TRUE(r.decAlgoLowpass2);
    EXPECT_DOUBLE_EQ(r.exposure.min, 4.0);
    EXPECT_DOUBLE_EQ(r.exposure.max, 8.0);
}

// ---------------------------------------------------------------------------
// guiding_assistant_apply: request parsing / ordering, and the failure message
// ---------------------------------------------------------------------------

TEST(GuidingAssistantApply, PlansInDependencyOrderWithoutDuplicates)
{
    std::vector<ApplyItem> plan;
    std::string bad;
    ASSERT_TRUE(PlanApply(
        { "min_move_dec", "dec_guide_mode", "dec_algorithm", "exposure", "min_move_dec", "min_move_ra", "dec_backlash" }, &plan,
        &bad));
    // switching the Dec algorithm resets its min-move, so it must come first
    std::vector<ApplyItem> expected = { ApplyItem::Exposure,   ApplyItem::MinMoveRa,   ApplyItem::DecAlgorithm,
                                        ApplyItem::MinMoveDec, ApplyItem::DecBacklash, ApplyItem::DecGuideMode };
    EXPECT_EQ(plan, expected);
}

TEST(GuidingAssistantApply, RejectsUnknownNamesAndEmptyRequests)
{
    std::vector<ApplyItem> plan;
    std::string bad;
    EXPECT_FALSE(PlanApply({ "min_move_ra", "aggressiveness" }, &plan, &bad));
    EXPECT_EQ(bad, "aggressiveness");
    EXPECT_FALSE(PlanApply({}, &plan, &bad));
}

TEST(GuidingAssistantApply, ItemNamesRoundTrip)
{
    for (ApplyItem item : { ApplyItem::Exposure, ApplyItem::MinMoveRa, ApplyItem::DecAlgorithm, ApplyItem::MinMoveDec,
                            ApplyItem::DecBacklash, ApplyItem::DecGuideMode })
    {
        std::vector<ApplyItem> plan;
        std::string bad;
        ASSERT_TRUE(PlanApply({ ApplyItemName(item) }, &plan, &bad));
        ASSERT_EQ(plan.size(), 1u);
        EXPECT_EQ(plan[0], item);
    }
}

TEST(GuidingAssistantApply, FailureMessageNamesFailedItemAndWhatWasAlreadySaved)
{
    EXPECT_EQ(
        ApplyFailureMessage(ApplyItem::DecAlgorithm, "Lowpass2 not available", { ApplyItem::Exposure, ApplyItem::MinMoveRa }),
        "dec_algorithm: Lowpass2 not available; already applied and saved: exposure, min_move_ra");
    EXPECT_EQ(ApplyFailureMessage(ApplyItem::Exposure, "no exposure in range", {}),
              "exposure: no exposure in range; nothing was applied");
}
