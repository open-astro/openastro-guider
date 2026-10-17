/*
 *  guiding_assistant_math.h
 *  PHD Guiding
 *
 *  Measurement statistics and recommendation logic for the Guiding
 *  Assistant, factored out of GuidingAsstWin (a wxDialog) so the dialog and
 *  the headless guiding_assistant_* JSON-RPC controller share one
 *  implementation, and so it can be unit tested without a GUI or a mount.
 *
 *  Everything here works on plain numbers: no wxWidgets, no Debug log, no
 *  globals. Callers gather the instrument / profile inputs (pixel scale,
 *  mount encoders, guide algorithms, calibration details, ...) and pass them
 *  in.
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

#ifndef GUIDING_ASSISTANT_MATH_H_INCLUDED
#define GUIDING_ASSISTANT_MATH_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "guiding_stats.h"

namespace ga_math
{

// Backlash beyond this cannot reasonably be compensated (ms)
constexpr int MAX_BACKLASH_COMP_MS = 3000;

// v if it is finite, else 0 -- for values going into JSON
double FiniteOrZero(double v);

// One unguided guide-step sample
struct Sample
{
    double ra = 0.; // RA-axis star offset, px
    double dec = 0.; // Dec-axis star offset, px
    double time = 0.; // guide-step timestamp, seconds (GuideStepInfo::time)
    long wallSecs = 0; // whole-second wall clock (wxGetCurrentTime()); the drift fits run against it
    double snr = 0.;
    double mass = 0.;
};

// Running statistics over the unguided samples: high-pass-filtered RMS
// (seeing), low-pass-filtered RA rate, peaks, and the Dec drift fit.
class Accumulator
{
public:
    Accumulator();

    // Start over; the filter cut-offs depend on the exposure time
    void Reset(double exposureSecs);
    void AddSample(const Sample& s);

    unsigned int Count() const;
    double RaHighFreqRms() const;
    double DecHighFreqRms() const;
    double TotalHighFreqRms() const;
    double RaPeak() const; // largest sample-to-sample RA move
    double DecPeak() const; // largest sample-to-sample Dec move
    double RaPeakToPeak() const; // of the low-pass-filtered RA
    double RaDriftPxPerMin(double elapsedSecs) const; // net RA displacement rate
    double MaxRaRatePxPerSec() const; // fastest low-pass-filtered RA rate
    double DecDriftPxPerMin() const; // linear-fit Dec drift, 0 until the samples span 2 wall-clock seconds
    double AvgSnr() const;
    double AvgMass() const;
    const AxisStats& DecAxis() const { return m_decAxisStats; }

private:
    // DescriptiveStats getters are non-const
    mutable DescriptiveStats m_hpfRAStats;
    mutable DescriptiveStats m_lpfRAStats;
    mutable DescriptiveStats m_hpfDecStats;
    AxisStats m_decAxisStats;
    AxisStats m_raAxisStats;
    HighPassFilter m_raHPF;
    LowPassFilter m_raLPF;
    HighPassFilter m_decHPF;
    long m_axisTimebase;
    double m_startRa;
    double m_lastRa;
    double m_lastTime;
    double m_maxRateRA;
    double m_sumSNR;
    double m_sumMass;
};

// Polar alignment error (arc-min) from the Dec drift rate (Barrett,
// http://celestialwonders.com/articles/polaralignment/PolarAlignmentAccuracy.pdf).
// With an unknown declination, dec 0 is assumed and the result is a lower bound. Non-finite results are 0.
double PolarAlignErrorArcmin(double decDriftPxPerMin, double pxScale, bool decKnown, double decRadians);

struct MinMoveInputs
{
    double pxScale = 0.;
    bool multiStar = false; // multi-star guiding was on when the run started
    bool hpEncoders = false;
    double smartDefaultDecMinMove = 0.; // fallback when the measurement is not credible
};

struct MinMoveRecs
{
    double ra = 0.;
    double dec = 0.;
    bool seeingBased = false; // false = fell back to the smart default
    double seeingEstimatePx = 0.; // best drift-corrected Dec RMS
};

// Seeing-based min-move from the drift-corrected Dec scatter; long runs use
// the quietest overlapping 2-minute window.
MinMoveRecs ComputeMinMoveRecs(const AxisStats& decAxis, const MinMoveInputs& in);

struct ExposureRange
{
    double min = 0.;
    double max = 0.;
};

ExposureRange RecommendExposure(double raHighFreqRms, double maxRaRatePxPerSec, bool hpEncoders);

// Exposure that keeps the fastest RA drift below the RA min-move (0 = n/a)
double DriftLimitingExposure(double raMinMove, double maxRaRatePxPerSec);

enum class BacklashAdvice
{
    Encoders, // absolute encoders: no compensation needed
    Small, // < 100 ms: no compensation needed
    Compensate, // try backlash compensation of recommendedMs
    TooLarge // > MAX_BACKLASH_COMP_MS: guide Dec in one direction only
};

struct BacklashRec
{
    BacklashAdvice advice = BacklashAdvice::Small;
    int recommendedMs = 0; // rounded down to 10 ms, at least 10 ms (0 if no backlash)
    bool applicable = false; // worth applying as backlash compensation
};

BacklashRec RecommendBacklash(int backlashMs, bool hpEncoders);

// How the Dec backlash test ended
enum class BacklashOutcome
{
    NotRun,
    Valid,
    Impaired, // the star ran out of room going north: the result is a lower bound ("test impaired")
    Aborted // no usable result: the test failed, or was stopped
};

const char *BacklashOutcomeName(BacklashOutcome outcome); // "valid", "impaired", "aborted" ("" if not run)
bool BacklashOutcomeUsable(BacklashOutcome outcome); // Valid or Impaired: there is a backlash result

// The result is shown as ">= ms": an impaired test, or at least 5 s of backlash
bool BacklashIsLowerBound(BacklashOutcome outcome, int backlashMs);

// The backlash result as the guide and debug logs show it (untranslated, after "Backlash="),
// e.g. " 640  +/-  35 ms (2.1  +/-  0.3 arc-sec)" or ">= 1200  +/-  ms (test impaired)".
// Empty without a usable result: the dialog logs nothing then.
std::string BacklashLogText(BacklashOutcome outcome, int backlashMs, double sigmaMs, double gearAngleArcsec,
                            double gearAngleSigmaArcsec);

enum class DecDirection
{
    North,
    South
};

// One-direction Dec guide mode for mounts with too much backlash
DecDirection SingleDirectionDecGuideMode(double decDriftPxPerMin);

struct LikelyBacklashInputs
{
    bool bltCompleted = false;
    int bltMs = 0;
    int compPulseMs = 0; // currently configured backlash compensation pulse
    bool decModeAutoOrNone = false;
    bool calHasDecSteps = false; // last calibration recorded Dec steps
    double calNorthDist = 0.; // px moved north during the last calibration
    double calSouthDist = 0.; // px moved back south
};

// Does the mount probably have large Dec backlash? If so the polar-alignment
// advice is relaxed.
bool LikelyBacklash(const LikelyBacklashInputs& in);

enum class PaAdvice
{
    None,
    Improve, // > 5'
    UseDriftAlign // > 10'
};

PaAdvice RecommendPolarAlignment(double errorArcmin, bool likelyBacklash);

bool RecommendLowpass2ForDec(bool hpEncoders, bool smallBacklash, const std::string& decAlgoClass);

// Binning needed to reach targetScale ("/px), from the camera's choices
int RecommendedBinning(double currScale, int currBinning, double targetScale, const std::vector<int>& choices, int maxBinning);

struct RecommendationInputs
{
    double pxScale = 0.;
    bool hpEncoders = false;
    bool multiStar = false;
    double smartDefaultDecMinMove = 0.;
    double raHighFreqRms = 0.;
    double maxRaRatePxPerSec = 0.;
    double decDriftPxPerMin = 0.;
    double polarAlignErrorArcmin = 0.;
    double avgSnr = 0.;
    double starHfd = 0.;
    int currBinning = 0;
    int maxBinning = 0;
    std::vector<int> binningChoices;
    bool suspectCalibration = false; // last calibration had an issue / backlash exemption
    bool raAlgoHasMinMove = false;
    bool decAlgoHasMinMove = false;
    std::string decAlgoClass;
    bool bltCompleted = false; // backlash test completed with a usable result
    int backlashMs = 0;
    LikelyBacklashInputs likely = {}; // bltCompleted/bltMs are taken from the fields above
};

struct Recommendations
{
    MinMoveRecs minMove = {};
    ExposureRange exposure = {};
    double driftLimitingExposure = 0.;
    bool recommendBinning = false;
    int binning = 0;
    bool recalibrate = false;
    bool brighterStar = false;
    PaAdvice pa = PaAdvice::None;
    bool improveFocus = false;
    bool raMinMove = false; // RA algorithm has a min-move to set
    bool decMinMove = false;
    bool hasBacklash = false; // backlash test completed
    BacklashRec backlash = {};
    DecDirection singleDirection = DecDirection::South; // meaningful when backlash.advice == TooLarge
    bool decAlgoLowpass2 = false;
};

Recommendations Recommend(const AxisStats& decAxis, const RecommendationInputs& in);

// Recommendations guiding_assistant_apply can write to the profile, in the
// order they must be applied (switching the Dec algorithm resets its min-move)
enum class ApplyItem
{
    Exposure,
    MinMoveRa,
    DecAlgorithm,
    MinMoveDec,
    DecBacklash,
    DecGuideMode
};

const char *ApplyItemName(ApplyItem item); // "exposure", "min_move_ra", ...

// Parse the requested names into a de-duplicated plan in application order.
// Fails on an empty request, or on an unknown name (returned in *badName).
bool PlanApply(const std::vector<std::string>& which, std::vector<ApplyItem> *plan, std::string *badName);

// "<item>: <reason>; already applied and saved: a, b" (or "; nothing was applied")
std::string ApplyFailureMessage(ApplyItem failed, const std::string& reason, const std::vector<ApplyItem>& applied);

// The session a GA run was measured in. Results are only applied to the same
// profile with the same camera and mount, never reconnected in between.
struct SessionStamp
{
    int profileId = 0;
    unsigned int gearConnects = 0; // camera / mount connections since startup
    std::uintptr_t mountId = 0; // identity of the mount object
    std::uintptr_t cameraId = 0;
    bool mountConnected = false;
    bool cameraConnected = false;
};

// true if `now` is still the session of `run`; otherwise *why (if given) says what changed
bool SameSession(const SessionStamp& run, const SessionStamp& now, std::string *why);

} // namespace ga_math

#endif
