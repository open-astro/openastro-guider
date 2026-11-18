/*
 *  staticpa_tracker.cpp
 *  PHD Guiding
 *
 *  Field tracker for the Static PA live readout (#99). See staticpa_tracker.h.
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

#include "staticpa_tracker.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace staticpa_track
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // a local maximum of the detection pass
    struct Peak
    {
        float value;
        int x;
        int y;
    };

    double Median(std::vector<float>& v)
    {
        if (v.empty())
            return 0.;
        size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + mid, v.end());
        return v[mid];
    }

    bool MassCompatible(const Candidate& seed, double seedExpMs, const Candidate& cur, double curExpMs)
    {
        if (seed.saturated || cur.saturated || seedExpMs <= 0. || curExpMs <= 0. || seed.mass <= 0. || cur.mass <= 0.)
            return true;
        double ratio = (cur.mass / curExpMs) / (seed.mass / seedExpMs);
        return ratio <= kMassRatioMax && ratio >= 1.0 / kMassRatioMax;
    }

    struct Pair
    {
        int seed;
        int cur;
        double dx;
        double dy;
    };

    // bins are offset so the key's halves are non-negative
    constexpr long long kBinOffset = 1LL << 20;

    long long BinKey(int bx, int by)
    {
        return ((bx + kBinOffset) << 32) | (by + kBinOffset);
    }

    int BinOf(double v)
    {
        return (int) std::floor(v / kBinPx);
    }

    // one-to-one matches of the seeds carried through `m` onto `cur` within tol,
    // closest pairs first
    std::vector<Pair> MatchUnder(const Motion& m, const std::vector<Candidate>& seeds, const std::vector<Candidate>& cur,
                                 const std::vector<std::vector<bool>>& compatible, const std::vector<bool>& seedFree,
                                 const std::vector<bool>& curFree, double tol)
    {
        struct Near
        {
            double d;
            int s;
            int c;
        };
        std::vector<Near> near;
        for (int i = 0; i < (int) seeds.size(); i++)
        {
            if (!seedFree[i])
                continue;
            double px, py;
            m.Apply(seeds[i].x, seeds[i].y, &px, &py);
            for (int j = 0; j < (int) cur.size(); j++)
            {
                if (!curFree[j] || !compatible[i][j])
                    continue;
                double d = std::hypot(cur[j].x - px, cur[j].y - py);
                if (d <= tol)
                    near.push_back({ d, i, j });
            }
        }
        std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
        std::vector<bool> sUsed(seeds.size(), false), cUsed(cur.size(), false);
        std::vector<Pair> out;
        for (const Near& n : near)
        {
            if (sUsed[n.s] || cUsed[n.c])
                continue;
            sUsed[n.s] = cUsed[n.c] = true;
            out.push_back({ n.s, n.c, cur[n.c].x - seeds[n.s].x, cur[n.c].y - seeds[n.s].y });
        }
        return out;
    }

    // least-squares rotation + translation taking the matched seeds onto their
    // current stars, the rotation clamped to kMaxRotationDeg
    Motion FitRigid(const std::vector<Pair>& pairs, const std::vector<Candidate>& seeds, const std::vector<Candidate>& cur)
    {
        Motion m;
        if (pairs.empty())
            return m;
        double sx = 0, sy = 0, cx = 0, cy = 0;
        for (const Pair& p : pairs)
        {
            sx += seeds[p.seed].x;
            sy += seeds[p.seed].y;
            cx += cur[p.cur].x;
            cy += cur[p.cur].y;
        }
        double n = (double) pairs.size();
        sx /= n;
        sy /= n;
        cx /= n;
        cy /= n;
        double rot = 0.;
        if (pairs.size() >= 2)
        {
            double num = 0, den = 0;
            for (const Pair& p : pairs)
            {
                double ax = seeds[p.seed].x - sx, ay = seeds[p.seed].y - sy;
                double bx = cur[p.cur].x - cx, by = cur[p.cur].y - cy;
                num += ax * by - ay * bx;
                den += ax * bx + ay * by;
            }
            rot = std::atan2(num, den);
            double lim = kMaxRotationDeg * kPi / 180.;
            rot = std::max(-lim, std::min(lim, rot));
        }
        m.ox = sx;
        m.oy = sy;
        m.rot = rot;
        m.dx = cx - sx;
        m.dy = cy - sy;
        return m;
    }

    struct Hypothesis
    {
        int score = 0; // distinct seeds (and distinct current stars) voting
        double dx = 0.;
        double dy = 0.;
    };

    // the histogram peak among the pairs whose seed and current star are both free
    Hypothesis BestBin(const std::vector<Pair>& pairs, const std::vector<bool>& seedFree, const std::vector<bool>& curFree)
    {
        std::unordered_map<long long, std::vector<int>> bins;
        bins.reserve(pairs.size() * 2);
        for (int k = 0; k < (int) pairs.size(); k++)
        {
            const Pair& p = pairs[k];
            if (seedFree[p.seed] && curFree[p.cur])
                bins[BinKey(BinOf(p.dx), BinOf(p.dy))].push_back(k);
        }

        Hypothesis best;
        std::vector<int> seen;
        std::vector<int> seenCur;
        for (const auto& kv : bins)
        {
            int bx = (int) ((kv.first >> 32) - kBinOffset);
            int by = (int) ((kv.first & 0xffffffffLL) - kBinOffset);
            // a bin's score: pairs in it and its eight neighbours, at most one
            // vote per seed star and per current star
            seen.clear();
            seenCur.clear();
            double sdx = 0, sdy = 0;
            int n = 0;
            for (int ix = -1; ix <= 1; ix++)
                for (int iy = -1; iy <= 1; iy++)
                {
                    auto it = bins.find(BinKey(bx + ix, by + iy));
                    if (it == bins.end())
                        continue;
                    for (int k : it->second)
                    {
                        const Pair& p = pairs[k];
                        if (std::find(seen.begin(), seen.end(), p.seed) != seen.end() ||
                            std::find(seenCur.begin(), seenCur.end(), p.cur) != seenCur.end())
                            continue;
                        seen.push_back(p.seed);
                        seenCur.push_back(p.cur);
                        sdx += p.dx;
                        sdy += p.dy;
                        n++;
                    }
                }
            if (n > best.score)
            {
                best.score = n;
                best.dx = sdx / n;
                best.dy = sdy / n;
            }
        }
        return best;
    }

    // Seed stars the motion puts inside the frame that found no match although
    // they are at least as bright as the median matched seed. A true match finds
    // its bright stars (a faint one may drop below the detection threshold);
    // a chance alignment of a few stars of an unrelated field leaves brighter
    // seed stars unexplained.
    int BrightMisses(const std::vector<Pair>& matched, const Motion& m, const std::vector<Candidate>& seeds, const Rect& valid)
    {
        if (matched.empty())
            return 0;
        std::vector<float> masses;
        std::vector<bool> used(seeds.size(), false);
        for (const Pair& p : matched)
        {
            masses.push_back((float) seeds[p.seed].mass);
            used[p.seed] = true;
        }
        double med = Median(masses);
        int n = 0;
        for (int i = 0; i < (int) seeds.size(); i++)
        {
            if (used[i] || seeds[i].mass < med)
                continue;
            double x, y;
            m.Apply(seeds[i].x, seeds[i].y, &x, &y);
            if (valid.Contains(x, y, kEdgeMarginPx))
                n++;
        }
        return n;
    }

    int PredictedInside(const Motion& m, const std::vector<Candidate>& seeds, const Rect& valid)
    {
        int n = 0;
        for (const Candidate& s : seeds)
        {
            double x, y;
            m.Apply(s.x, s.y, &x, &y);
            if (valid.Contains(x, y, kEdgeMarginPx))
                n++;
        }
        return n;
    }

    // refine a translation hypothesis into a rigid motion and its one-to-one votes
    std::vector<Pair> Refine(const Hypothesis& h, const std::vector<Candidate>& seeds, const std::vector<Candidate>& cur,
                             const std::vector<std::vector<bool>>& compatible, const std::vector<bool>& seedFree,
                             const std::vector<bool>& curFree, Motion *out)
    {
        Motion m;
        m.dx = h.dx;
        m.dy = h.dy;
        std::vector<Pair> matched = MatchUnder(m, seeds, cur, compatible, seedFree, curFree, kCoarseTolPx);
        for (int iter = 0; iter < 3 && !matched.empty(); iter++)
        {
            m = FitRigid(matched, seeds, cur);
            matched = MatchUnder(m, seeds, cur, compatible, seedFree, curFree, iter == 0 ? 2 * kMatchTolPx : kMatchTolPx);
        }
        if (!matched.empty())
            m = FitRigid(matched, seeds, cur);
        *out = m;
        return matched;
    }

    // A true match moves every star the same way, so its matched stars keep one
    // brightness ratio and sit on the fitted motion to within the centroid noise.
    // Chance alignments of an unrelated field have neither. Drops the pairs whose
    // exposure-normalised mass ratio is more than kMassSpread from the median
    // (saturated stars are kept), then refits; returns the RMS residual.
    double Tighten(std::vector<Pair>& matched, const std::vector<Candidate>& seeds, double seedExpMs,
                   const std::vector<Candidate>& cur, double curExpMs, Motion *m)
    {
        double norm = (seedExpMs > 0. && curExpMs > 0.) ? seedExpMs / curExpMs : 1.0;
        std::vector<float> ratios;
        for (const Pair& p : matched)
            if (!seeds[p.seed].saturated && !cur[p.cur].saturated && seeds[p.seed].mass > 0. && cur[p.cur].mass > 0.)
                ratios.push_back((float) std::log(cur[p.cur].mass * norm / seeds[p.seed].mass));
        if (ratios.size() >= 3)
        {
            std::vector<float> tmp = ratios;
            double med = Median(tmp);
            double lim = std::log(kMassSpread);
            std::vector<Pair> kept;
            for (const Pair& p : matched)
            {
                const Candidate& a = seeds[p.seed];
                const Candidate& b = cur[p.cur];
                if (a.saturated || b.saturated || a.mass <= 0. || b.mass <= 0. ||
                    std::fabs(std::log(b.mass * norm / a.mass) - med) <= lim)
                    kept.push_back(p);
            }
            if (kept.size() != matched.size() && !kept.empty())
            {
                matched.swap(kept);
                *m = FitRigid(matched, seeds, cur);
            }
        }
        double ss = 0.;
        for (const Pair& p : matched)
        {
            double x, y;
            m->Apply(seeds[p.seed].x, seeds[p.seed].y, &x, &y);
            ss += (x - cur[p.cur].x) * (x - cur[p.cur].x) + (y - cur[p.cur].y) * (y - cur[p.cur].y);
        }
        return matched.empty() ? 0. : std::sqrt(ss / matched.size());
    }

} // namespace

int MaxBrightMisses(int votes)
{
    return std::max(kMinBrightMissesAllowed, (int) (kBrightMissFraction * votes));
}

void Motion::Apply(double x, double y, double *outX, double *outY) const
{
    double c = std::cos(rot), s = std::sin(rot);
    double rx = x - ox, ry = y - oy;
    *outX = ox + c * rx - s * ry + dx;
    *outY = oy + s * rx + c * ry + dy;
}

Rect ValidRect(const star_find::StarImage& img)
{
    Rect r;
    if (img.subframeEmpty)
    {
        r.left = 0;
        r.top = 0;
        r.right = img.width - 1;
        r.bottom = img.height - 1;
    }
    else
    {
        r.left = std::max(0, img.subLeft);
        r.top = std::max(0, img.subTop);
        r.right = std::min(img.width - 1, img.subRight);
        r.bottom = std::min(img.height - 1, img.subBottom);
    }
    return r;
}

int DetectBinning(int w, int h)
{
    if (w <= 0 || h <= 0)
        return 1;
    int b = 1;
    while ((long) (w / b) * (long) (h / b) > kMaxDetectPixels)
        b++;
    return b;
}

bool DetectGridUsable(int w, int h)
{
    const int b = DetectBinning(w, h);
    return w / b >= kMinDetectBinnedSide && h / b >= kMinDetectBinnedSide;
}

std::vector<Candidate> FindCandidates(const star_find::StarImage& img, const DetectParams& p)
{
    std::vector<Candidate> out;
    if (!img.data || img.width <= 0 || img.height <= 0)
        return out;
    Rect r = ValidRect(img);
    if (r.Width() < 16 || r.Height() < 16)
        return out;
    // a binned side under 5 px holds no peak; under 2 px (0 or 1) it would also
    // make the sample stride NaN and, at 0, leave the binned copy empty (#110)
    if (!DetectGridUsable(r.Width(), r.Height()))
        return out;

    // detection pass on a binned copy of the valid area
    const int b = DetectBinning(r.Width(), r.Height());
    const int bw = r.Width() / b, bh = r.Height() / b;
    std::vector<float> bin((size_t) bw * bh, 0.f);
    for (int y = 0; y < bh * b; y++)
    {
        const unsigned short *row = img.data + (size_t) (r.top + y) * img.width + r.left;
        float *brow = &bin[(size_t) (y / b) * bw];
        for (int x = 0; x < bw * b; x++)
            brow[x / b] += row[x];
    }

    // 3x3 smoothing, the kernel star_find::FindStar uses (1 2 1 / 2 4 2 / 1 2 1)
    std::vector<float> sm((size_t) bw * bh, 0.f);
    for (int y = 1; y < bh - 1; y++)
    {
        const float *a = &bin[(size_t) (y - 1) * bw], *c = &bin[(size_t) y * bw], *d = &bin[(size_t) (y + 1) * bw];
        float *o = &sm[(size_t) y * bw];
        for (int x = 1; x < bw - 1; x++)
            o[x] = (a[x - 1] + a[x + 1] + d[x - 1] + d[x + 1] + 2.f * (a[x] + d[x] + c[x - 1] + c[x + 1]) + 4.f * c[x]) / 16.f;
    }

    // background and noise of the smoothed copy from a sparse sample (median,
    // MAD), robust to the stars themselves
    std::vector<float> sample;
    const long interior = (long) (bw - 2) * (bh - 2);
    const int stride = std::max(1, (int) std::sqrt((double) interior / 20000.));
    for (int y = 1; y < bh - 1; y += stride)
        for (int x = 1; x < bw - 1; x += stride)
            sample.push_back(sm[(size_t) y * bw + x]);
    double med = Median(sample);
    for (float& v : sample)
        v = (float) std::fabs(v - med);
    double sigma = 1.4826 * Median(sample);
    if (sigma < 1e-3)
        sigma = 1e-3;
    const float thresh = (float) (med + kDetectSigma * sigma);

    std::vector<Peak> peaks;
    for (int y = 2; y < bh - 2; y++)
    {
        const float *c = &sm[(size_t) y * bw];
        for (int x = 2; x < bw - 2; x++)
        {
            float v = c[x];
            if (v <= thresh)
                continue;
            // strict maximum against the neighbours before it, >= after it,
            // so a flat top yields one peak
            const float *a = c - bw, *d = c + bw;
            if (!(v > a[x - 1] && v > a[x] && v > a[x + 1] && v > c[x - 1] && v >= c[x + 1] && v >= d[x - 1] && v >= d[x] &&
                  v >= d[x + 1]))
                continue;
            peaks.push_back({ v, x, y });
        }
    }
    const size_t maxPeaks = (size_t) std::max(1, p.maxStars) * 4;
    if (peaks.size() > maxPeaks)
    {
        std::partial_sort(peaks.begin(), peaks.begin() + maxPeaks, peaks.end(),
                          [](const Peak& a, const Peak& c) { return a.value > c.value; });
        peaks.resize(maxPeaks);
    }

    // measure each at full resolution
    const int refineRegion = b + 3;
    std::vector<Candidate> found;
    for (const Peak& pk : peaks)
    {
        int fx = r.left + pk.x * b + b / 2;
        int fy = r.top + pk.y * b + b / 2;
        star_find::Output o =
            star_find::FindStar(img, fx, fy, refineRegion, star_find::Mode::Centroid, p.minHFD, p.maxHFD, p.maxADU);
        if (!o.found || o.snr < p.minSNR || !r.Contains(o.x, o.y, 0))
            continue;
        Candidate c;
        c.x = o.x;
        c.y = o.y;
        c.mass = o.mass;
        c.snr = o.snr;
        c.hfd = o.hfd;
        c.saturated = o.result == star_find::Result::Saturated;
        found.push_back(c);
    }

    // merge duplicates (two maxima refined onto the same star), keeping the
    // brighter measurement
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& c) { return a.mass > c.mass; });
    for (const Candidate& c : found)
    {
        bool dup = false;
        for (const Candidate& k : out)
        {
            if (std::hypot(k.x - c.x, k.y - c.y) <= std::max(2.0, std::max(k.hfd, c.hfd)))
            {
                dup = true;
                break;
            }
        }
        if (!dup)
            out.push_back(c);
        if ((int) out.size() >= p.maxStars)
            break;
    }
    return out;
}

FieldMatch MatchField(const std::vector<Candidate>& seeds, double seedExposureMs, const std::vector<Candidate>& cur,
                      double curExposureMs, const Rect& valid)
{
    FieldMatch res;
    if (seeds.empty() || cur.empty())
        return res;

    std::vector<std::vector<bool>> compatible(seeds.size(), std::vector<bool>(cur.size(), false));
    std::vector<Pair> pairs;
    pairs.reserve(seeds.size() * cur.size());
    for (int i = 0; i < (int) seeds.size(); i++)
        for (int j = 0; j < (int) cur.size(); j++)
        {
            if (!MassCompatible(seeds[i], seedExposureMs, cur[j], curExposureMs))
                continue;
            compatible[i][j] = true;
            pairs.push_back({ i, j, cur[j].x - seeds[i].x, cur[j].y - seeds[i].y });
        }
    if (pairs.empty())
        return res;

    std::vector<bool> seedFree(seeds.size(), true), curFree(cur.size(), true);
    Hypothesis best = BestBin(pairs, seedFree, curFree);
    if (best.score == 0)
        return res;

    Motion m;
    std::vector<Pair> matched = Refine(best, seeds, cur, compatible, seedFree, curFree, &m);
    res.rmsPx = Tighten(matched, seeds, seedExposureMs, cur, curExposureMs, &m);
    res.motion = m;
    res.votes = (int) matched.size();
    res.predictedInside = PredictedInside(m, seeds, valid);
    res.needed = std::max(kMinMatchVotes, (int) std::ceil(kMinMatchFraction * res.predictedInside));
    res.brightMisses = BrightMisses(matched, m, seeds, valid);

    // the best different explanation: the same seed field placed on the
    // current stars this motion did not use (a repeated pattern, or a second
    // field that fits nearly as well)
    for (const Pair& p : matched)
        curFree[p.cur] = false;
    Hypothesis second = BestBin(pairs, seedFree, curFree);
    if (second.score > 0)
    {
        Motion m2;
        std::vector<Pair> other = Refine(second, seeds, cur, compatible, seedFree, curFree, &m2);
        Tighten(other, seeds, seedExposureMs, cur, curExposureMs, &m2);
        res.runnerUp = (int) other.size();
    }

    double rmsLimit = kMaxFitRmsPx;
    if (res.votes >= kRelaxedRmsVotes)
        rmsLimit = kMaxFitRmsRelaxedPx;
    else if (res.votes >= std::ceil(kCompleteMatchFraction * res.predictedInside))
        rmsLimit = kMaxFitRmsCompletePx;
    if (res.votes < res.needed || res.rmsPx > rmsLimit || res.brightMisses > MaxBrightMisses(res.votes))
        res.verdict = Verdict::NoMatch;
    else if (res.votes - res.runnerUp < kMinVoteLead || res.votes < kMinVoteRatio * res.runnerUp)
        res.verdict = Verdict::Ambiguous;
    else
        res.verdict = Verdict::Matched;
    return res;
}

const char *LiveStateName(LiveState state)
{
    switch (state)
    {
    case LiveState::Tracking:
        return "tracking";
    case LiveState::Searching:
        return "searching";
    case LiveState::Lost:
        return "lost";
    }
    return "searching";
}

void LiveTracker::Reset()
{
    *this = LiveTracker();
}

void LiveTracker::Seed(const star_find::StarImage& img, double refX, double refY, const DetectParams& dp, const FrameInfo& fi)
{
    DetectParams seedParams = dp;
    seedParams.maxStars = kMaxSeedStars;
    SeedFromStars(refX, refY, FindCandidates(img, seedParams), img.width, img.height, fi);
}

bool LiveTracker::SeedAtMeasuredStar(const star_find::StarImage& img, double refX, double refY, const DetectParams& dp,
                                     const FrameInfo& fi)
{
    Reset();
    if (!img.subframeEmpty)
        return false;
    // the brightest star near the measured position, measured like the guider
    // measures its star; a brighter neighbour inside the box fails the distance
    // check below and the seed is refused rather than guessed
    const int box = (int) std::ceil(kRefSnapPx) + 3;
    star_find::Output o = star_find::FindStar(img, (int) std::lround(refX), (int) std::lround(refY), box,
                                              star_find::Mode::Centroid, dp.minHFD, dp.maxHFD, dp.maxADU);
    if (!o.found || o.snr < dp.minSNR || std::hypot(o.x - refX, o.y - refY) > kRefSnapPx)
        return false;
    Seed(img, o.x, o.y, dp, fi);
    return true;
}

void LiveTracker::SeedFromStars(double refX, double refY, const std::vector<Candidate>& stars, int frameW, int frameH,
                                const FrameInfo& fi)
{
    Reset();
    m_seeded = true;
    m_seeds.assign(stars.begin(), stars.begin() + std::min((int) stars.size(), kMaxSeedStars));
    m_seedExposureMs = fi.exposureMs;
    m_refX = refX;
    m_refY = refY;
    m_frameW = frameW;
    m_frameH = frameH;
    m_lastStarCount = (int) stars.size();
    // the measurement is the first fix
    SetFix(refX, refY, true, 0, fi);
}

void LiveTracker::SetFix(double x, double y, bool detected, int confirmedBy, const FrameInfo& fi)
{
    m_lastWasFix = true;
    m_fixX = x;
    m_fixY = y;
    m_fixDetected = detected;
    m_fixMs = fi.nowMs;
    m_lastExposureMs = fi.exposureMs;
    m_framesSinceFix = 0;
    m_confirmedBy = confirmedBy;
}

void LiveTracker::SetMiss(const FrameInfo& fi)
{
    m_lastWasFix = false;
    m_lastExposureMs = fi.exposureMs;
    m_framesSinceFix++;
}

StepResult LiveTracker::Step(const star_find::StarImage& img, const DetectParams& dp, const FrameInfo& fi)
{
    if (!m_seeded)
        return StepResult::Skipped;
    if (!img.subframeEmpty)
    {
        // not evaluated: neither a fix nor a miss, but the last fix is no
        // longer the latest frame's
        MarkStale();
        return StepResult::Skipped;
    }
    return StepWithStars(FindCandidates(img, dp), ValidRect(img), fi);
}

StepResult LiveTracker::StepWithStars(const std::vector<Candidate>& stars, const Rect& valid, const FrameInfo& fi)
{
    if (!m_seeded)
        return StepResult::Skipped;
    m_lastStarCount = (int) stars.size();
    m_lastMatch = MatchField(m_seeds, m_seedExposureMs, stars, fi.exposureMs, valid);
    m_ambiguous = m_lastMatch.verdict == Verdict::Ambiguous;
    if (m_lastMatch.verdict != Verdict::Matched)
    {
        SetMiss(fi);
        return StepResult::Miss;
    }

    double px, py;
    m_lastMatch.motion.Apply(m_refX, m_refY, &px, &py);
    // snap to the detected reference star when it is there; when it is not
    // (saturated, smeared, off the frame) the field still places it
    const Candidate *snap = nullptr;
    double best = kRefSnapPx;
    for (const Candidate& c : stars)
    {
        double d = std::hypot(c.x - px, c.y - py);
        if (d <= best)
        {
            best = d;
            snap = &c;
        }
    }
    if (snap)
        SetFix(snap->x, snap->y, true, m_lastMatch.votes, fi);
    else
        SetFix(px, py, false, m_lastMatch.votes, fi);
    return StepResult::Fix;
}

StepResult LiveTracker::StepGuider(bool found, double x, double y, const FrameInfo& fi)
{
    if (!m_seeded)
        return StepResult::Skipped;
    m_ambiguous = false;
    if (!found)
    {
        SetMiss(fi);
        return StepResult::Miss;
    }
    SetFix(x, y, true, 0, fi);
    return StepResult::Fix;
}

StepResult LiveTracker::StepUnusable(const FrameInfo& fi)
{
    if (!m_seeded)
        return StepResult::Skipped;
    m_ambiguous = false;
    SetMiss(fi);
    return StepResult::Miss;
}

void LiveTracker::MarkStale()
{
    m_lastWasFix = false;
}

bool LiveTracker::FixIsFresh(std::int64_t nowMs) const
{
    if (!m_seeded || !m_lastWasFix)
        return false;
    double window = std::max(kFreshExposures * m_lastExposureMs, (double) kMinFreshMs);
    return (double) (nowMs - m_fixMs) <= window;
}

double LiveTracker::SecondsSinceFix(std::int64_t nowMs) const
{
    if (!m_seeded)
        return 0.;
    return std::max(0.0, (double) (nowMs - m_fixMs) / 1000.0);
}

LiveState LiveTracker::State(std::int64_t nowMs, bool captureActive) const
{
    if (captureActive && FixIsFresh(nowMs))
        return LiveState::Tracking;
    if (m_seeded && m_framesSinceFix >= kLostFrames && SecondsSinceFix(nowMs) >= kLostSeconds)
        return LiveState::Lost;
    return LiveState::Searching;
}

} // namespace staticpa_track
