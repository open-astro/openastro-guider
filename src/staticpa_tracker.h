/*
 *  staticpa_tracker.h
 *  PHD Guiding
 *
 *  Keeps the Static PA live readout on the reference star while the user turns
 *  the alt/az bolts (#99). Upstream PHD2's adjust phase is a fixed overlay the
 *  user follows by eye; this fork also reports a live adjustment over JSON-RPC,
 *  which used to come from the guider's own star. The guider searches only
 *  +/-SearchRegion px (7..50) around its last position, so a normal bolt turn
 *  (1' is about 15 px at 3.87"/px, a rough alignment is 10-40') lost the star
 *  and the readout disappeared until some star wandered back into that box.
 *
 *  A bolt turn moves the whole star field rigidly (plus a fraction of a degree
 *  of rotation for an azimuth turn), so this tracks the field, not one star.
 *  When the solution is calculated the tracker is seeded from the frame that
 *  produced the measurement: the reference star plus up to kMaxSeedStars stars
 *  in that frame. On each adjust-phase frame it lists the stars of the full
 *  frame and estimates the field's motion with a 2-D vote histogram over every
 *  (seed star, current star) offset, then refines it with a rigid fit. The fix
 *  is accepted only when enough of the seed stars predicted inside the frame
 *  agree and the next best explanation is clearly worse; otherwise the frame is
 *  a miss ("searching", or "ambiguous"). The reference star's position is the
 *  seed position carried through the fitted motion, snapped to the detected
 *  star when there is one, so a move of any size within the frame is found
 *  again on the first frame after it and a different star is never taken for
 *  the reference star. A field with fewer than kMinFieldStars seed stars has
 *  nothing to vote with and uses the guider's own position instead.
 *
 *  Kept free of wx so it can be unit tested
 *  (tests/polar_alignment/test_staticpa_tracker.cpp). Times are milliseconds
 *  on a steady clock.
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

#ifndef STATICPA_TRACKER_H_INCLUDED
#define STATICPA_TRACKER_H_INCLUDED

#include "star_find_core.h"

#include <cstdint>
#include <vector>

namespace staticpa_track
{
// --- Thresholds (constants, not tunable over RPC) ---------------------------

// Seed stars kept from the measurement frame, and stars listed per frame.
constexpr int kMaxSeedStars = 30;
constexpr int kMaxFrameStars = 60;
// Fewer seed stars than this: nothing to vote with, use the guider's position.
constexpr int kMinFieldStars = 3;
// A fix needs at least max(kMinMatchVotes, kMinMatchFraction x the seed stars
// predicted inside the frame) seed stars matched.
constexpr int kMinMatchVotes = 3;
constexpr double kMinMatchFraction = 0.30;
// ... and must beat the best explanation left after removing its stars by at
// least kMinVoteLead votes and a factor of kMinVoteRatio.
constexpr int kMinVoteLead = 2;
constexpr double kMinVoteRatio = 1.5;
// Vote histogram bin (px). A bin's score counts the pairs in it and its eight
// neighbours, so offsets that differ by up to about two bins vote together.
constexpr double kBinPx = 4.0;
// First match around the histogram peak (translation only), then the matches
// after the rigid fit.
constexpr double kCoarseTolPx = 10.0;
constexpr double kMatchTolPx = 3.0;
// Largest field rotation the rigid fit may use (an azimuth turn of 40' at
// latitude 60 rotates the field by about 0.6 deg).
constexpr double kMaxRotationDeg = 2.0;
// A seed star counts as predicted inside the frame only this far from an edge.
constexpr double kEdgeMarginPx = 8.0;
// Pair photometry: exposure-normalised mass ratio allowed between a seed star
// and a current star (not checked when either is saturated).
constexpr double kMassRatioMax = 4.0;
// Matched stars of one field keep one brightness ratio: a pair more than this
// factor from the median ratio does not vote. And they sit on the fitted
// motion: a fit whose RMS residual exceeds kMaxFitRmsPx is no fix. A chance
// alignment of a few stars of an unrelated field fits to about 0.8 px; a match
// with kRelaxedRmsVotes or more stars cannot be one and may use the looser
// limit (centroid noise plus the optics' distortion across a large move).
constexpr double kMassSpread = 2.0;
constexpr double kMaxFitRmsPx = 0.75;
constexpr int kRelaxedRmsVotes = 6;
constexpr double kMaxFitRmsRelaxedPx = 1.5;
// A match of fewer stars that explains nearly all the seed stars predicted
// inside (at least kCompleteMatchFraction of them) may use kMaxFitRmsCompletePx:
// a sparse field's 3-5 stars with 0.35 px of centroid jitter in both frames fit
// to more than 0.75 px about 1 time in 10, while a chance alignment in a dense
// field explains only a few of the seed stars it puts in view.
constexpr double kCompleteMatchFraction = 0.75;
constexpr double kMaxFitRmsCompletePx = 1.0;
// Seed stars predicted inside and at least as bright as the median matched one
// that found no match: allowed up to max(kMinBrightMissesAllowed,
// kBrightMissFraction x votes).
constexpr int kMinBrightMissesAllowed = 1;
constexpr double kBrightMissFraction = 0.25;
// The reference star's predicted position is snapped to a detected star within
// this distance. A seed taken on a later frame (the measurement frame was a
// subframe) needs a detected star this close to the measured position.
constexpr double kRefSnapPx = 3.0;
// "lost" after this many frames AND seconds without a fix; searching goes on.
constexpr int kLostFrames = 10;
constexpr double kLostSeconds = 15.0;
// A fix is fresh for max(kFreshExposures x exposure, kMinFreshMs).
constexpr double kFreshExposures = 3.0;
constexpr std::int64_t kMinFreshMs = 5000;
// Star detection: the local-maximum threshold above the background (sigma) and
// the most pixels the detection pass reads after binning (larger sensors are
// binned 2x2, 3x3, ... to stay under it; stars are then measured at full
// resolution).
constexpr double kDetectSigma = 5.0;
constexpr long kMaxDetectPixels = 800000;

// --- Star lists ---------------------------------------------------------------

struct Candidate
{
    double x = 0.;
    double y = 0.;
    double mass = 0.;
    double snr = 0.;
    double hfd = 0.;
    bool saturated = false;
};

// Inclusive pixel rectangle.
struct Rect
{
    int left = 0;
    int top = 0;
    int right = -1;
    int bottom = -1;
    int Width() const { return right - left + 1; }
    int Height() const { return bottom - top + 1; }
    bool Contains(double x, double y, double margin) const
    {
        return x >= left + margin && x <= right - margin && y >= top + margin && y <= bottom - margin;
    }
};

// The image's subframe, or the whole image.
Rect ValidRect(const star_find::StarImage& img);

struct DetectParams
{
    double minHFD = 1.5;
    double maxHFD = 10.0;
    double minSNR = 6.0;
    unsigned short maxADU = 0; // 0: unknown
    int maxStars = kMaxFrameStars;
};

// Binning factor for the detection pass of a w x h area.
int DetectBinning(int w, int h);

// The stars of the image's valid area, brightest (by mass) first: local maxima
// of a smoothed, possibly binned copy above the background, each measured with
// star_find::FindStar at full resolution, duplicates merged.
std::vector<Candidate> FindCandidates(const star_find::StarImage& img, const DetectParams& p);

// --- Field match ----------------------------------------------------------------

// Rotation by rot (radians) about (ox, oy), then translation by (dx, dy).
struct Motion
{
    double ox = 0.;
    double oy = 0.;
    double rot = 0.;
    double dx = 0.;
    double dy = 0.;
    void Apply(double x, double y, double *outX, double *outY) const;
};

enum class Verdict
{
    Matched,
    NoMatch, // too few seed stars agree on any motion
    Ambiguous, // enough agree, but a different motion explains nearly as many
};

struct FieldMatch
{
    Verdict verdict = Verdict::NoMatch;
    Motion motion;
    int votes = 0; // seed stars matched one-to-one within kMatchTolPx
    int predictedInside = 0; // seed stars the motion puts inside the frame
    int needed = 0; // votes a fix needs for that predictedInside
    int runnerUp = 0; // best vote of a different motion, from the other stars
    double rmsPx = 0.; // RMS residual of the matched stars about the motion
    int brightMisses = 0; // bright seed stars predicted inside without a match
};

int MaxBrightMisses(int votes);

// Where the seed field (seen at seedExposureMs) is in the current star list
// (exposed for curExposureMs, valid area `valid`). Exposures <= 0 skip the
// mass check.
FieldMatch MatchField(const std::vector<Candidate>& seeds, double seedExposureMs, const std::vector<Candidate>& cur,
                      double curExposureMs, const Rect& valid);

// --- Live tracker -----------------------------------------------------------------

enum class LiveState
{
    Tracking,
    Searching,
    Lost,
};

const char *LiveStateName(LiveState state);

struct FrameInfo
{
    std::int64_t nowMs = 0; // steady clock
    double exposureMs = 0.; // the frame's actual exposure
};

enum class StepResult
{
    Fix,
    Miss,
    Skipped, // not evaluated (subframe): neither a fix nor a miss
};

class LiveTracker
{
public:
    void Reset();

    // Seed from the measurement frame. refX/refY is the reference star the
    // solution was measured with.
    void Seed(const star_find::StarImage& img, double refX, double refY, const DetectParams& dp, const FrameInfo& fi);
    // Seed from a full frame taken after the measurement (the measurement frame
    // was a subframe): only when a detected star lies within kRefSnapPx of the
    // measured position refX/refY, which then is the reference star. Otherwise
    // the field may have moved in between and nothing ties the reference star
    // to a star of this frame: stays unseeded and returns false.
    bool SeedAtMeasuredStar(const star_find::StarImage& img, double refX, double refY, const DetectParams& dp,
                            const FrameInfo& fi);
    void SeedFromStars(double refX, double refY, const std::vector<Candidate>& stars, int frameW, int frameH,
                       const FrameInfo& fi);

    bool Seeded() const { return m_seeded; }
    // At least kMinFieldStars seed stars: the field is tracked. Otherwise the
    // caller feeds the guider's own result through StepGuider.
    bool FieldMode() const { return m_seeded && (int) m_seeds.size() >= kMinFieldStars; }
    int SeedCount() const { return (int) m_seeds.size(); }
    int FrameWidth() const { return m_frameW; }
    int FrameHeight() const { return m_frameH; }

    // Field mode, one adjust-phase frame. A subframed image is skipped: it is
    // not a miss, but the last fix is no longer the latest frame's (stale).
    StepResult Step(const star_find::StarImage& img, const DetectParams& dp, const FrameInfo& fi);
    StepResult StepWithStars(const std::vector<Candidate>& stars, const Rect& valid, const FrameInfo& fi);
    // Fallback mode: whether the guider found its star on this frame, and where.
    StepResult StepGuider(bool found, double x, double y, const FrameInfo& fi);
    // A frame that cannot show the seed field (another frame size): a miss, so
    // the counters keep running from the last fix and the state reaches lost.
    StepResult StepUnusable(const FrameInfo& fi);

    // The last fix must not be reported any more (capture error, guider reset,
    // capture stopped). Not a miss: the next frame decides again.
    void MarkStale();

    // Tracking while the last evaluated frame gave a fix that is still fresh
    // and capture is running; lost after kLostFrames frames and kLostSeconds
    // without one; searching otherwise.
    LiveState State(std::int64_t nowMs, bool captureActive) const;
    // The last fix may be reported: it came from the last evaluated frame and
    // is within max(kFreshExposures x exposure, kMinFreshMs).
    bool FixIsFresh(std::int64_t nowMs) const;

    double FixX() const { return m_fixX; }
    double FixY() const { return m_fixY; }
    bool FixDetected() const { return m_fixDetected; } // snapped to a detected star
    int FramesSinceFix() const { return m_framesSinceFix; }
    double SecondsSinceFix(std::int64_t nowMs) const;
    int ConfirmedBy() const { return m_confirmedBy; }
    bool Ambiguous() const { return m_ambiguous; }
    const FieldMatch& LastMatch() const { return m_lastMatch; }
    int LastStarCount() const { return m_lastStarCount; }

private:
    void SetFix(double x, double y, bool detected, int confirmedBy, const FrameInfo& fi);
    void SetMiss(const FrameInfo& fi);

    bool m_seeded = false;
    std::vector<Candidate> m_seeds;
    double m_seedExposureMs = 0.;
    double m_refX = 0.;
    double m_refY = 0.;
    int m_frameW = 0;
    int m_frameH = 0;

    bool m_lastWasFix = false;
    double m_fixX = 0.;
    double m_fixY = 0.;
    bool m_fixDetected = false;
    std::int64_t m_fixMs = 0;
    double m_lastExposureMs = 0.;
    int m_framesSinceFix = 0;
    int m_confirmedBy = 0;
    bool m_ambiguous = false;
    FieldMatch m_lastMatch;
    int m_lastStarCount = 0;
};

// --- Measurement run ------------------------------------------------------------

// Which points were measured since the latest start (staticpa_start, or the
// GUI's Rotate / first-position button). Manual mode keeps the previous run's
// points, so a new point 1 recalculates the solution with the old points 2 and
// 3; the live readout must not follow such a mixed solution.
class RunPoints
{
public:
    void Start() { m_mask = 0; }
    void Measured(int npos)
    {
        if (npos >= 1 && npos <= 3)
            m_mask |= 1 << npos;
    }
    // Every point the solution uses was measured in this run: 1-2 (auto) or 1-3
    // (manual).
    bool Covers(bool autoMode) const
    {
        int need = autoMode ? (1 << 1) | (1 << 2) : (1 << 1) | (1 << 2) | (1 << 3);
        return (m_mask & need) == need;
    }

private:
    int m_mask = 0;
};

} // namespace staticpa_track

#endif // STATICPA_TRACKER_H_INCLUDED
