// Unit tests for src/staticpa_tracker.cpp — the field tracker that keeps the
// Static PA live readout on the reference star while the user turns the
// alt/az bolts (#99). The production translation unit depends only on
// star_find_core.cpp and links here unmodified.
//
// Images are synthetic: a 800 +/- 15 ADU noisy background with Gaussian stars
// (sigma 2 px), like scripts/fake_alpaca_camera.py. At 3.87"/px one arcminute
// is 15.5 px, so the 588 px move below is the 37.9' readout in the issue.

#include <gtest/gtest.h>

#include "staticpa_tracker.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace staticpa_track;

namespace
{

struct SynthStar
{
    double x, y, amp;
};

struct SynthImage
{
    int w = 0, h = 0;
    std::vector<unsigned short> pix;
    star_find::StarImage View() const
    {
        star_find::StarImage v;
        v.data = pix.data();
        v.width = w;
        v.height = h;
        return v;
    }
};

SynthImage Render(int w, int h, const std::vector<SynthStar>& stars, unsigned noiseSeed = 7, double sigma = 2.0)
{
    SynthImage img;
    img.w = w;
    img.h = h;
    std::vector<double> buf((size_t) w * h);
    std::mt19937 rng(noiseSeed);
    std::normal_distribution<double> noise(800.0, 15.0);
    for (double& v : buf)
        v = noise(rng);
    const int R = 12;
    for (const SynthStar& s : stars)
    {
        int x0 = (int) std::floor(s.x) - R, x1 = (int) std::ceil(s.x) + R;
        int y0 = (int) std::floor(s.y) - R, y1 = (int) std::ceil(s.y) + R;
        for (int y = std::max(0, y0); y <= std::min(h - 1, y1); y++)
            for (int x = std::max(0, x0); x <= std::min(w - 1, x1); x++)
            {
                double dx = x - s.x, dy = y - s.y;
                buf[(size_t) y * w + x] += s.amp * std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
            }
    }
    img.pix.resize(buf.size());
    for (size_t i = 0; i < buf.size(); i++)
        img.pix[i] = (unsigned short) std::min(65535.0, std::max(0.0, buf[i]));
    return img;
}

// the fake camera's 12 stars (scripts/fake_alpaca_camera.py STARS)
std::vector<SynthStar> FakeCameraStars()
{
    return { { 80, 60, 30000 },   { 200, 120, 25000 }, { 320, 240, 40000 }, { 500, 100, 20000 },
             { 120, 350, 35000 }, { 420, 380, 28000 }, { 560, 300, 22000 }, { 250, 420, 18000 },
             { 380, 60, 26000 },  { 60, 250, 32000 },  { 520, 430, 24000 }, { 300, 160, 15000 } };
}

// stars spread uniformly over a field larger than the frame so that a shifted
// view still has stars entering from the edges
std::vector<SynthStar> RandomSky(unsigned seed, int n, double x0, double y0, double x1, double y1, double minAmp = 2500,
                                 double maxAmp = 40000)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> ux(x0, x1), uy(y0, y1), ua(std::log(minAmp), std::log(maxAmp));
    std::vector<SynthStar> out;
    while ((int) out.size() < n)
    {
        SynthStar s { ux(rng), uy(rng), std::exp(ua(rng)) };
        bool tooClose = false;
        for (const SynthStar& o : out)
            if (std::hypot(o.x - s.x, o.y - s.y) < 14)
                tooClose = true;
        if (!tooClose)
            out.push_back(s);
    }
    return out;
}

std::vector<SynthStar> Shifted(const std::vector<SynthStar>& in, double dx, double dy)
{
    std::vector<SynthStar> out = in;
    for (SynthStar& s : out)
    {
        s.x += dx;
        s.y += dy;
    }
    return out;
}

std::vector<SynthStar> Rotated(const std::vector<SynthStar>& in, double cx, double cy, double deg)
{
    double c = std::cos(deg * M_PI / 180.), s = std::sin(deg * M_PI / 180.);
    std::vector<SynthStar> out = in;
    for (SynthStar& p : out)
    {
        double rx = p.x - cx, ry = p.y - cy;
        p.x = cx + c * rx - s * ry;
        p.y = cy + s * rx + c * ry;
    }
    return out;
}

// candidate lists for the pure matcher: mass of a sigma-2 Gaussian of that amp
std::vector<Candidate> AsCandidates(const std::vector<SynthStar>& stars, int w, int h, int maxN = kMaxFrameStars)
{
    std::vector<SynthStar> in;
    for (const SynthStar& s : stars)
        if (s.x >= 0 && s.x < w && s.y >= 0 && s.y < h)
            in.push_back(s);
    std::sort(in.begin(), in.end(), [](const SynthStar& a, const SynthStar& b) { return a.amp > b.amp; });
    std::vector<Candidate> out;
    for (const SynthStar& s : in)
    {
        if ((int) out.size() >= maxN)
            break;
        Candidate c;
        c.x = s.x;
        c.y = s.y;
        c.mass = s.amp * 2 * M_PI * 4;
        c.snr = 50;
        c.hfd = 4.7;
        out.push_back(c);
    }
    return out;
}

const SynthStar *Nearest(const std::vector<SynthStar>& stars, double x, double y)
{
    const SynthStar *best = nullptr;
    double bd = 1e18;
    for (const SynthStar& s : stars)
    {
        double d = std::hypot(s.x - x, s.y - y);
        if (d < bd)
        {
            bd = d;
            best = &s;
        }
    }
    return best;
}

Rect FullRect(int w, int h)
{
    Rect r;
    r.left = 0;
    r.top = 0;
    r.right = w - 1;
    r.bottom = h - 1;
    return r;
}

FrameInfo At(std::int64_t ms, double expMs = 1000.)
{
    FrameInfo fi;
    fi.nowMs = ms;
    fi.exposureMs = expMs;
    return fi;
}

} // namespace

// --- FindCandidates ---------------------------------------------------------------

TEST(StaticPaTrackerDetect, BinningKeepsTheDetectionPassSmall)
{
    EXPECT_EQ(DetectBinning(640, 480), 1);
    EXPECT_EQ(DetectBinning(1280, 960), 2);
    EXPECT_EQ(DetectBinning(1936, 1216), 2);
    EXPECT_EQ(DetectBinning(3096, 2080), 3);
    for (int w : { 640, 1280, 1936, 3096, 4144, 6248 })
    {
        int h = w * 2 / 3;
        int b = DetectBinning(w, h);
        EXPECT_LE((long) (w / b) * (h / b), kMaxDetectPixels) << w;
    }
}

TEST(StaticPaTrackerDetect, FindsAllFakeCameraStarsWithinQuarterPixel)
{
    std::vector<SynthStar> stars = FakeCameraStars();
    SynthImage img = Render(640, 480, stars);
    std::vector<Candidate> c = FindCandidates(img.View(), DetectParams());
    ASSERT_EQ(c.size(), stars.size());
    for (const SynthStar& s : stars)
    {
        bool found = false;
        for (const Candidate& k : c)
            if (std::hypot(k.x - s.x, k.y - s.y) < 0.25)
                found = true;
        EXPECT_TRUE(found) << s.x << "," << s.y;
    }
    // brightest first
    for (size_t i = 1; i < c.size(); i++)
        EXPECT_GE(c[i - 1].mass, c[i].mass);
}

TEST(StaticPaTrackerDetect, FindsStarsOnABinnedSensorAtFullResolution)
{
    std::vector<SynthStar> stars = Shifted(FakeCameraStars(), 333.3, 211.7);
    SynthImage img = Render(1280, 960, stars);
    std::vector<Candidate> c = FindCandidates(img.View(), DetectParams());
    ASSERT_EQ(c.size(), stars.size());
    for (const Candidate& k : c)
    {
        const SynthStar *s = Nearest(stars, k.x, k.y);
        EXPECT_LT(std::hypot(k.x - s->x, k.y - s->y), 0.25);
    }
}

TEST(StaticPaTrackerDetect, RejectsHotPixels)
{
    std::vector<SynthStar> stars = FakeCameraStars();
    SynthImage img = Render(640, 480, stars);
    const int hot[][2] = { { 150, 200 }, { 600, 50 }, { 33, 440 }, { 450, 222 } };
    for (auto& p : hot)
        img.pix[(size_t) p[1] * img.w + p[0]] = 30000;
    std::vector<Candidate> c = FindCandidates(img.View(), DetectParams());
    EXPECT_EQ(c.size(), stars.size());
    for (const Candidate& k : c)
        for (auto& p : hot)
            EXPECT_GT(std::hypot(k.x - p[0], k.y - p[1]), 3.0);
}

TEST(StaticPaTrackerDetect, RespectsTheSubframe)
{
    std::vector<SynthStar> stars = FakeCameraStars();
    SynthImage img = Render(640, 480, stars);
    star_find::StarImage v = img.View();
    v.subframeEmpty = false;
    v.subLeft = 260;
    v.subTop = 100;
    v.subRight = 459;
    v.subBottom = 299;
    std::vector<Candidate> c = FindCandidates(v, DetectParams());
    // (320,240) and (300,160) are inside; (380,60) and (420,380) are not
    ASSERT_EQ(c.size(), 2u);
    for (const Candidate& k : c)
        EXPECT_TRUE(ValidRect(v).Contains(k.x, k.y, 0));
}

TEST(StaticPaTrackerDetect, MergesACloseDoubleAndATwoLobeStar)
{
    // two maxima 6 px apart refine to one centroid each time; never two
    // candidates for the same star
    std::vector<SynthStar> stars = { { 200, 200, 20000 },
                                     { 206, 200, 15000 },
                                     // a star smeared into two lobes
                                     { 400, 300, 9000 },
                                     { 404, 303, 9000 },
                                     { 100, 400, 25000 } };
    SynthImage img = Render(640, 480, stars);
    std::vector<Candidate> c = FindCandidates(img.View(), DetectParams());
    ASSERT_GE(c.size(), 3u);
    for (size_t i = 0; i < c.size(); i++)
        for (size_t j = i + 1; j < c.size(); j++)
            EXPECT_GT(std::hypot(c[i].x - c[j].x, c[i].y - c[j].y), std::max(2.0, std::max(c[i].hfd, c[j].hfd)))
                << i << " " << j;
}

TEST(StaticPaTrackerDetect, EmptyFrameHasNoStars)
{
    SynthImage img = Render(640, 480, {});
    EXPECT_TRUE(FindCandidates(img.View(), DetectParams()).empty());
}

// --- MatchField ---------------------------------------------------------------------

TEST(StaticPaTrackerMatch, RecoversALargeTranslation)
{
    std::vector<SynthStar> sky = RandomSky(11, 120, -700, -700, 1980, 1660);
    std::vector<Candidate> seeds = AsCandidates(sky, 1280, 960, kMaxSeedStars);
    for (auto mv : { std::make_pair(300.0, -120.0), std::make_pair(-588.0, 0.0), std::make_pair(410.0, 415.0),
                     std::make_pair(1.5, -2.0) })
    {
        std::vector<Candidate> cur = AsCandidates(Shifted(sky, mv.first, mv.second), 1280, 960);
        FieldMatch m = MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960));
        ASSERT_EQ(m.verdict, Verdict::Matched) << mv.first << "," << mv.second;
        double x, y;
        m.motion.Apply(640, 480, &x, &y);
        EXPECT_NEAR(x, 640 + mv.first, 0.05);
        EXPECT_NEAR(y, 480 + mv.second, 0.05);
        EXPECT_GE(m.votes, m.needed);
        EXPECT_LT(m.runnerUp, m.votes);
    }
}

TEST(StaticPaTrackerMatch, RecoversHalfADegreeOfRotationPlusTranslation)
{
    std::vector<SynthStar> sky = RandomSky(12, 120, -400, -400, 1680, 1360);
    std::vector<Candidate> seeds = AsCandidates(sky, 1280, 960, kMaxSeedStars);
    for (double deg : { 0.5, -0.7, 1.5 })
    {
        std::vector<SynthStar> moved = Shifted(Rotated(sky, 900, -300, deg), -250, 140);
        std::vector<Candidate> cur = AsCandidates(moved, 1280, 960);
        FieldMatch m = MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960));
        ASSERT_EQ(m.verdict, Verdict::Matched) << deg;
        // every seed star lands on its moved self
        for (size_t i = 0; i < seeds.size(); i++)
        {
            const SynthStar *orig = Nearest(sky, seeds[i].x, seeds[i].y);
            size_t k = orig - sky.data();
            double x, y;
            m.motion.Apply(seeds[i].x, seeds[i].y, &x, &y);
            EXPECT_NEAR(x, moved[k].x, 0.1) << deg << " seed " << i;
            EXPECT_NEAR(y, moved[k].y, 0.1) << deg << " seed " << i;
        }
    }
}

TEST(StaticPaTrackerMatch, ToleratesCentroidJitterAndLensDistortion)
{
    // real frames: every centroid jitters (seeing, noise) and a guide scope's
    // distortion bends a large move; a true match must survive both
    std::mt19937 rng(5);
    std::normal_distribution<double> jitter(0.0, 0.35);
    auto distort = [](std::vector<SynthStar> v)
    {
        for (SynthStar& p : v)
        {
            double rx = p.x - 640, ry = p.y - 480, r2 = rx * rx + ry * ry;
            double k = 1.0 + 3.9e-9 * r2; // 2 px at 800 px from the centre
            p.x = 640 + rx * k;
            p.y = 480 + ry * k;
        }
        return v;
    };
    std::vector<SynthStar> sky = RandomSky(14, 160, -800, -800, 2080, 1760);
    std::vector<Candidate> seeds = AsCandidates(distort(sky), 1280, 960, kMaxSeedStars);
    for (auto mv : { std::make_pair(588.0, 0.0), std::make_pair(-300.0, 250.0), std::make_pair(20.0, -10.0) })
    {
        std::vector<Candidate> cur = AsCandidates(distort(Shifted(sky, mv.first, mv.second)), 1280, 960);
        for (Candidate& c : cur)
        {
            c.x += jitter(rng);
            c.y += jitter(rng);
        }
        FieldMatch m = MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960));
        EXPECT_EQ(m.verdict, Verdict::Matched)
            << mv.first << "," << mv.second << " votes " << m.votes << "/" << m.needed << " rms " << m.rmsPx;
    }
}

TEST(StaticPaTrackerMatch, SparseFieldToleratesCentroidJitterInBothFrames)
{
    // a sparse near-pole field: 5-8 stars in the frame, so 3-5 of them match
    // after a move, below the 6-star relaxed residual limit. Seeing and noise
    // move every centroid in the measurement frame and in the current one.
    std::mt19937 rng(17);
    std::normal_distribution<double> jitter(0.0, 0.35);
    std::uniform_real_distribution<double> mv(-60.0, 60.0);
    int trials = 0, matched = 0, few = 0, fewMatched = 0;
    for (int n = 5; n <= 8; n++)
        for (unsigned seed = 0; seed < 250; seed++)
        {
            std::vector<SynthStar> sky = RandomSky(40000 + 1000 * n + seed, n, 0, 0, 1280, 960);
            std::vector<Candidate> seeds = AsCandidates(sky, 1280, 960, kMaxSeedStars);
            for (Candidate& c : seeds)
            {
                c.x += jitter(rng);
                c.y += jitter(rng);
            }
            std::vector<Candidate> cur = AsCandidates(Shifted(sky, mv(rng), mv(rng)), 1280, 960);
            for (Candidate& c : cur)
            {
                c.x += jitter(rng);
                c.y += jitter(rng);
            }
            FieldMatch m = MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960));
            trials++;
            if (m.verdict == Verdict::Matched)
                matched++;
            if (m.votes >= kMinMatchVotes && m.votes < kRelaxedRmsVotes)
            {
                few++;
                if (m.verdict == Verdict::Matched)
                    fewMatched++;
            }
        }
    ASSERT_GT(few, 300); // the 3-5 star matches are the case under test
    // with the 0.75 px limit alone about 1 in 10 of these was refused
    EXPECT_GE(fewMatched, few * 98 / 100) << fewMatched << "/" << few;
    EXPECT_GE(matched, trials * 98 / 100) << matched << "/" << trials;
}

TEST(StaticPaTrackerMatch, UnrelatedDenseFieldNeverMatches)
{
    // The case that matters most: the reference star and its whole field have
    // left the frame (or a different field is in view). Dense random fields,
    // many seeds: zero false fixes allowed.
    int ambiguous = 0;
    for (unsigned seed = 0; seed < 400; seed++)
    {
        std::vector<SynthStar> a = RandomSky(1000 + seed, 150, 0, 0, 1280, 960);
        std::vector<SynthStar> b = RandomSky(5000 + seed, 150, 0, 0, 1280, 960);
        std::vector<Candidate> seeds = AsCandidates(a, 1280, 960, kMaxSeedStars);
        std::vector<Candidate> cur = AsCandidates(b, 1280, 960);
        // and a decoy of the reference star's mass where the reference was
        cur.push_back(seeds[0]);
        FieldMatch m = MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960));
        ASSERT_NE(m.verdict, Verdict::Matched) << "seed " << seed << " votes " << m.votes << "/" << m.needed;
        if (m.verdict == Verdict::Ambiguous)
            ambiguous++;
    }
    EXPECT_EQ(ambiguous, 0);
}

TEST(StaticPaTrackerMatch, UnrelatedFieldWithFewStarsNeverMatches)
{
    // sparse fields: the 3-vote floor is what protects here
    for (unsigned seed = 0; seed < 400; seed++)
    {
        std::vector<SynthStar> a = RandomSky(9000 + seed, 8, 0, 0, 1280, 960);
        std::vector<SynthStar> b = RandomSky(13000 + seed, 25, 0, 0, 1280, 960);
        FieldMatch m =
            MatchField(AsCandidates(a, 1280, 960, kMaxSeedStars), 1000, AsCandidates(b, 1280, 960), 1000, FullRect(1280, 960));
        ASSERT_NE(m.verdict, Verdict::Matched) << "seed " << seed;
    }
}

TEST(StaticPaTrackerMatch, FieldMostlyOffFrameIsNoMatch)
{
    // shifted so far that fewer than kMinMatchVotes seed stars remain
    std::vector<SynthStar> sky = RandomSky(21, 40, 0, 0, 1280, 960);
    std::vector<Candidate> seeds = AsCandidates(sky, 1280, 960, kMaxSeedStars);
    std::vector<SynthStar> moved;
    for (const SynthStar& s : Shifted(sky, 1270, 950))
        moved.push_back(s);
    FieldMatch m = MatchField(seeds, 1000, AsCandidates(moved, 1280, 960), 1000, FullRect(1280, 960));
    EXPECT_NE(m.verdict, Verdict::Matched);
}

TEST(StaticPaTrackerMatch, TwoCopiesOfTheFieldIsAmbiguous)
{
    // the same pattern at two offsets explains the frame equally well
    std::vector<SynthStar> pat = RandomSky(31, 12, 0, 0, 400, 300);
    std::vector<Candidate> seeds = AsCandidates(pat, 1280, 960, kMaxSeedStars);
    std::vector<SynthStar> both = Shifted(pat, 100, 100);
    for (const SynthStar& s : Shifted(pat, 700, 550))
        both.push_back(s);
    FieldMatch m = MatchField(seeds, 1000, AsCandidates(both, 1280, 960), 1000, FullRect(1280, 960));
    EXPECT_EQ(m.verdict, Verdict::Ambiguous);
}

TEST(StaticPaTrackerMatch, MassOutsideTheRatioDoesNotVote)
{
    std::vector<SynthStar> sky = RandomSky(41, 60, 0, 0, 1280, 960);
    std::vector<Candidate> seeds = AsCandidates(sky, 1280, 960, kMaxSeedStars);
    std::vector<Candidate> cur = AsCandidates(Shifted(sky, 50, 20), 1280, 960);
    for (Candidate& c : cur)
        c.mass /= 80; // a frame smeared by a bolt turn during the exposure
    EXPECT_EQ(MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960)).verdict, Verdict::NoMatch);
    // saturated stars skip the mass check
    for (Candidate& c : cur)
        c.saturated = true;
    EXPECT_EQ(MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960)).verdict, Verdict::Matched);
}

TEST(StaticPaTrackerMatch, MassIsNormalisedForExposure)
{
    std::vector<SynthStar> sky = RandomSky(42, 60, 0, 0, 1280, 960);
    std::vector<Candidate> seeds = AsCandidates(sky, 1280, 960, kMaxSeedStars);
    std::vector<Candidate> cur = AsCandidates(Shifted(sky, -30, 70), 1280, 960);
    for (Candidate& c : cur)
        c.mass *= 6; // exposure 1 s -> 6 s
    EXPECT_EQ(MatchField(seeds, 1000, cur, 6000, FullRect(1280, 960)).verdict, Verdict::Matched);
    EXPECT_EQ(MatchField(seeds, 1000, cur, 1000, FullRect(1280, 960)).verdict, Verdict::NoMatch);
}

TEST(StaticPaTrackerMatch, PredictedInsideUsesTheValidArea)
{
    std::vector<SynthStar> sky = RandomSky(43, 80, 0, 0, 1280, 960);
    std::vector<Candidate> seeds = AsCandidates(sky, 1280, 960, kMaxSeedStars);
    Rect sub;
    sub.left = 300;
    sub.top = 200;
    sub.right = 899;
    sub.bottom = 699;
    std::vector<Candidate> cur;
    for (const Candidate& c : AsCandidates(sky, 1280, 960, 200))
        if (sub.Contains(c.x, c.y, 0))
            cur.push_back(c);
    FieldMatch m = MatchField(seeds, 1000, cur, 1000, sub);
    EXPECT_EQ(m.verdict, Verdict::Matched);
    EXPECT_LT(m.predictedInside, (int) seeds.size());
}

// --- LiveTracker on images ---------------------------------------------------------------

namespace
{
struct Scene
{
    std::vector<SynthStar> sky;
    SynthStar ref;
};

// a 1280x960 field (82.6' x 61.9' at 3.87"/px) with stars beyond its edges
Scene MakeScene(unsigned seed = 77)
{
    Scene s;
    s.sky = RandomSky(seed, 160, -800, -800, 2080, 1760, 3000, 40000);
    s.ref = { 640.4, 480.6, 30000 };
    s.sky.push_back(s.ref);
    return s;
}

void SeedOn(LiveTracker& t, const Scene& s, std::int64_t ms = 0)
{
    SynthImage img = Render(1280, 960, s.sky);
    t.Seed(img.View(), s.ref.x, s.ref.y, DetectParams(), At(ms));
}
} // namespace

TEST(StaticPaTrackerLive, SeedsFromTheMeasurementFrame)
{
    Scene s = MakeScene();
    LiveTracker t;
    EXPECT_FALSE(t.Seeded());
    SeedOn(t, s, 1000);
    EXPECT_TRUE(t.Seeded());
    EXPECT_TRUE(t.FieldMode());
    EXPECT_EQ(t.SeedCount(), kMaxSeedStars);
    // the measurement itself is the first fix
    EXPECT_EQ(t.State(1000, true), LiveState::Tracking);
    EXPECT_TRUE(t.FixIsFresh(1500));
    EXPECT_DOUBLE_EQ(t.FixX(), s.ref.x);
    EXPECT_DOUBLE_EQ(t.FixY(), s.ref.y);
}

TEST(StaticPaTrackerLive, SmallMotionStaysTracking)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    for (int i = 1; i <= 10; i++)
    {
        std::vector<SynthStar> moved = Shifted(s.sky, 3.0 * i, -1.0 * i);
        SynthImage img = Render(1280, 960, moved, 100 + i);
        ASSERT_EQ(t.Step(img.View(), DetectParams(), At(i * 1000)), StepResult::Fix) << i;
        EXPECT_EQ(t.State(i * 1000, true), LiveState::Tracking);
        EXPECT_NEAR(t.FixX(), s.ref.x + 3.0 * i, 0.25);
        EXPECT_NEAR(t.FixY(), s.ref.y - 1.0 * i, 0.25);
        EXPECT_TRUE(t.FixDetected());
        EXPECT_EQ(t.FramesSinceFix(), 0);
        EXPECT_GE(t.ConfirmedBy(), kMinMatchVotes);
    }
}

TEST(StaticPaTrackerLive, LargeJumpIsFoundOnTheFirstFrame)
{
    // far beyond SearchRegion 15 or 50: 300 px is 19.4', 588 px is 37.9'
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    const double moves[][2] = { { 300, -120 }, { -588, 0 }, { 250, 400 } };
    int f = 1;
    for (auto& mv : moves)
    {
        SynthImage img = Render(1280, 960, Shifted(s.sky, mv[0], mv[1]), 200 + f);
        ASSERT_EQ(t.Step(img.View(), DetectParams(), At(f * 1000)), StepResult::Fix) << mv[0] << "," << mv[1];
        EXPECT_NEAR(t.FixX(), s.ref.x + mv[0], 0.5);
        EXPECT_NEAR(t.FixY(), s.ref.y + mv[1], 0.5);
        EXPECT_FALSE(t.Ambiguous());
        f++;
    }
}

TEST(StaticPaTrackerLive, ReferenceMovedOffFrameIsCarriedByTheFieldNotLatchedToADecoy)
{
    // the reference star leaves the sensor; a star of the same flux sits
    // where the reference was. The field still matches, so the readout keeps
    // the reference star's true (off-frame) position and never the decoy.
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    std::vector<SynthStar> moved = Shifted(s.sky, -700, 0);
    moved.push_back({ s.ref.x, s.ref.y, s.ref.amp });
    SynthImage img = Render(1280, 960, moved, 301);
    ASSERT_EQ(t.Step(img.View(), DetectParams(), At(1000)), StepResult::Fix);
    EXPECT_NEAR(t.FixX(), s.ref.x - 700, 0.5);
    EXPECT_NEAR(t.FixY(), s.ref.y, 0.5);
    EXPECT_FALSE(t.FixDetected());
}

TEST(StaticPaTrackerLive, DecoyNearTheLastFixWhileTheFieldMoves)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    std::vector<SynthStar> moved = Shifted(s.sky, 40, 25);
    moved.push_back({ s.ref.x + 4, s.ref.y - 3, s.ref.amp }); // decoy right next to the old position
    SynthImage img = Render(1280, 960, moved, 302);
    ASSERT_EQ(t.Step(img.View(), DetectParams(), At(1000)), StepResult::Fix);
    EXPECT_NEAR(t.FixX(), s.ref.x + 40, 0.5);
    EXPECT_NEAR(t.FixY(), s.ref.y + 25, 0.5);
    EXPECT_TRUE(t.FixDetected());
}

TEST(StaticPaTrackerLive, SmearedFrameIsAMissNotALoss)
{
    // one frame with every star at 1/80 of its flux (measured on the #98 rig
    // for a frame exposed while the mount moved), then normal again
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    std::vector<SynthStar> dim = Shifted(s.sky, 20, 0);
    for (SynthStar& st : dim)
        st.amp /= 80;
    SynthImage smeared = Render(1280, 960, dim, 401);
    EXPECT_EQ(t.Step(smeared.View(), DetectParams(), At(1000)), StepResult::Miss);
    EXPECT_EQ(t.State(1000, true), LiveState::Searching);
    EXPECT_FALSE(t.FixIsFresh(1000));
    EXPECT_EQ(t.FramesSinceFix(), 1);
    SynthImage normal = Render(1280, 960, Shifted(s.sky, 40, 0), 402);
    EXPECT_EQ(t.Step(normal.View(), DetectParams(), At(2000)), StepResult::Fix);
    EXPECT_EQ(t.State(2000, true), LiveState::Tracking);
    EXPECT_NEAR(t.FixX(), s.ref.x + 40, 0.5);
}

TEST(StaticPaTrackerLive, FieldGoneGoesLostAfterFramesAndSecondsThenRecovers)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    // the scope points somewhere else entirely (a decoy at the old position too)
    std::vector<SynthStar> other = RandomSky(999, 120, 0, 0, 1280, 960);
    other.push_back(s.ref);
    SynthImage away = Render(1280, 960, other, 501);
    for (int i = 1; i <= 9; i++)
    {
        EXPECT_EQ(t.Step(away.View(), DetectParams(), At(i * 2000)), StepResult::Miss);
        EXPECT_EQ(t.State(i * 2000, true), LiveState::Searching) << i;
    }
    // 10 frames but only 10 s: still searching (exposures could be short)
    LiveTracker fast;
    SeedOn(fast, s);
    for (int i = 1; i <= 10; i++)
        fast.Step(away.View(), DetectParams(), At(i * 1000));
    EXPECT_EQ(fast.State(10000, true), LiveState::Searching);
    EXPECT_EQ(fast.State(15000, true), LiveState::Lost);

    EXPECT_EQ(t.Step(away.View(), DetectParams(), At(20000)), StepResult::Miss);
    EXPECT_EQ(t.FramesSinceFix(), 10);
    EXPECT_NEAR(t.SecondsSinceFix(20000), 20.0, 1e-9);
    EXPECT_EQ(t.State(20000, true), LiveState::Lost);
    EXPECT_FALSE(t.FixIsFresh(20000));

    SynthImage back = Render(1280, 960, Shifted(s.sky, -10, 12), 502);
    EXPECT_EQ(t.Step(back.View(), DetectParams(), At(22000)), StepResult::Fix);
    EXPECT_EQ(t.State(22000, true), LiveState::Tracking);
    EXPECT_NEAR(t.FixX(), s.ref.x - 10, 0.5);
}

TEST(StaticPaTrackerLive, ExposureChangeIsNormalised)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    std::vector<SynthStar> brighter = Shifted(s.sky, 5, 5);
    for (SynthStar& st : brighter)
        st.amp = std::min(st.amp * 1.5, 60000.0);
    SynthImage img = Render(1280, 960, brighter, 601);
    EXPECT_EQ(t.Step(img.View(), DetectParams(), At(1000, 1500)), StepResult::Fix);
}

TEST(StaticPaTrackerLive, FreshnessWindowFollowsTheExposure)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s, 0);
    SynthImage img = Render(1280, 960, s.sky, 701);
    ASSERT_EQ(t.Step(img.View(), DetectParams(), At(1000, 1000)), StepResult::Fix);
    EXPECT_TRUE(t.FixIsFresh(1000 + kMinFreshMs));
    EXPECT_FALSE(t.FixIsFresh(1000 + kMinFreshMs + 1));
    EXPECT_EQ(t.State(1000 + kMinFreshMs + 1, true), LiveState::Searching);

    // 4 s exposures: three exposures, 12 s
    LiveTracker slow;
    slow.Seed(img.View(), s.ref.x, s.ref.y, DetectParams(), At(0, 4000));
    ASSERT_EQ(slow.Step(img.View(), DetectParams(), At(10000, 4000)), StepResult::Fix);
    EXPECT_TRUE(slow.FixIsFresh(10000 + 12000));
    EXPECT_FALSE(slow.FixIsFresh(10000 + 12001));
}

TEST(StaticPaTrackerLive, MarkStaleStopsReportingUntilTheNextFix)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    SynthImage img = Render(1280, 960, s.sky, 801);
    ASSERT_EQ(t.Step(img.View(), DetectParams(), At(1000)), StepResult::Fix);
    t.MarkStale(); // e.g. a capture error
    EXPECT_FALSE(t.FixIsFresh(1001));
    EXPECT_EQ(t.State(1001, true), LiveState::Searching);
    EXPECT_EQ(t.FramesSinceFix(), 0); // not a miss
    ASSERT_EQ(t.Step(img.View(), DetectParams(), At(2000)), StepResult::Fix);
    EXPECT_TRUE(t.FixIsFresh(2000));
}

TEST(StaticPaTrackerLive, NotTrackingWhileCaptureIsStopped)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    EXPECT_EQ(t.State(10, false), LiveState::Searching);
    EXPECT_EQ(t.State(10, true), LiveState::Tracking);
}

TEST(StaticPaTrackerLive, SubframeIsSkipped)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    SynthImage img = Render(1280, 960, s.sky, 901);
    star_find::StarImage v = img.View();
    v.subframeEmpty = false;
    v.subLeft = 600;
    v.subTop = 440;
    v.subRight = 680;
    v.subBottom = 520;
    EXPECT_EQ(t.Step(v, DetectParams(), At(1000)), StepResult::Skipped);
    EXPECT_EQ(t.FramesSinceFix(), 0); // not a miss
    // but the last fix is not the latest frame's any more
    EXPECT_FALSE(t.FixIsFresh(1000));
    EXPECT_EQ(t.State(1000, true), LiveState::Searching);
    ASSERT_EQ(t.Step(img.View(), DetectParams(), At(2000)), StepResult::Fix);
    EXPECT_TRUE(t.FixIsFresh(2000));
}

TEST(StaticPaTrackerLive, DeferredSeedNeedsTheMeasuredStarInPlace)
{
    // the measurement frame was a subframe, so the seed is taken on the next
    // full frame: only if the measured star is still where it was measured
    Scene s = MakeScene();
    SynthImage same = Render(1280, 960, Shifted(s.sky, 0.4, -0.3), 1101);
    LiveTracker t;
    ASSERT_TRUE(t.SeedAtMeasuredStar(same.View(), s.ref.x, s.ref.y, DetectParams(), At(1000)));
    EXPECT_TRUE(t.FieldMode());
    EXPECT_NEAR(t.FixX(), s.ref.x + 0.4, 0.25);
    EXPECT_NEAR(t.FixY(), s.ref.y - 0.3, 0.25);
    EXPECT_EQ(t.State(1000, true), LiveState::Tracking);
    SynthImage later = Render(1280, 960, Shifted(s.sky, 120.4, -79.7), 1102);
    ASSERT_EQ(t.Step(later.View(), DetectParams(), At(2000)), StepResult::Fix);
    EXPECT_NEAR(t.FixX(), s.ref.x + 120.4, 0.3);

    // a bolt turn started between the measurement and the full frame: the
    // field moved 40 px, so nothing ties the reference star to a star here
    SynthImage moved = Render(1280, 960, Shifted(s.sky, 40, 0), 1103);
    LiveTracker m;
    EXPECT_FALSE(m.SeedAtMeasuredStar(moved.View(), s.ref.x, s.ref.y, DetectParams(), At(1000)));
    EXPECT_FALSE(m.Seeded());
    EXPECT_FALSE(m.FixIsFresh(1000));

    // ... and a different star now near the measured position is not taken
    // either when it is beyond kRefSnapPx
    std::vector<SynthStar> decoy = Shifted(s.sky, 40, 0);
    decoy.push_back({ s.ref.x + 4.5, s.ref.y, 20000 });
    SynthImage withDecoy = Render(1280, 960, decoy, 1104);
    LiveTracker d;
    EXPECT_FALSE(d.SeedAtMeasuredStar(withDecoy.View(), s.ref.x, s.ref.y, DetectParams(), At(1000)));

    // a subframe cannot seed
    star_find::StarImage sub = same.View();
    sub.subframeEmpty = false;
    sub.subLeft = 600;
    sub.subTop = 440;
    sub.subRight = 680;
    sub.subBottom = 520;
    LiveTracker f;
    EXPECT_FALSE(f.SeedAtMeasuredStar(sub, s.ref.x, s.ref.y, DetectParams(), At(1000)));
}

TEST(StaticPaTrackerLive, UnusableFramesCountAsMissesFromTheLastFix)
{
    // another frame size (binning changed): misses, so the counters keep
    // running from the last fix and lost means what it says
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    for (int i = 1; i <= 10; i++)
        EXPECT_EQ(t.StepUnusable(At(i * 2000)), StepResult::Miss);
    EXPECT_EQ(t.FramesSinceFix(), 10);
    EXPECT_NEAR(t.SecondsSinceFix(20000), 20.0, 1e-9);
    EXPECT_EQ(t.State(20000, true), LiveState::Lost);
    // the size comes back: the field is found again
    SynthImage img = Render(1280, 960, s.sky, 1201);
    EXPECT_EQ(t.Step(img.View(), DetectParams(), At(22000)), StepResult::Fix);
    EXPECT_EQ(t.State(22000, true), LiveState::Tracking);
    LiveTracker unseeded;
    EXPECT_EQ(unseeded.StepUnusable(At(1000)), StepResult::Skipped);
}

TEST(StaticPaTrackerRun, ManualReMeasureOverAManualSolutionIsNotFresh)
{
    RunPoints run;
    EXPECT_FALSE(run.Covers(true));
    EXPECT_FALSE(run.Covers(false));
    // a full manual run
    run.Start();
    run.Measured(1);
    run.Measured(2);
    EXPECT_FALSE(run.Covers(false));
    run.Measured(3);
    EXPECT_TRUE(run.Covers(false));
    // staticpa_start {auto:false} again: the old points 2 and 3 are still
    // set and point 1 recalculates the solution, which is not this run's
    run.Start();
    run.Measured(1);
    EXPECT_FALSE(run.Covers(false));
    run.Measured(2);
    EXPECT_FALSE(run.Covers(false));
    run.Measured(3);
    EXPECT_TRUE(run.Covers(false));
    // re-measuring one point within the run keeps it fresh
    run.Measured(3);
    EXPECT_TRUE(run.Covers(false));
}

TEST(StaticPaTrackerRun, AutoNeedsPointsOneAndTwo)
{
    RunPoints run;
    run.Start();
    run.Measured(1);
    EXPECT_FALSE(run.Covers(true));
    run.Measured(2);
    EXPECT_TRUE(run.Covers(true));
    EXPECT_FALSE(run.Covers(false));
    run.Measured(0); // out of range: ignored
    run.Measured(4);
    EXPECT_FALSE(run.Covers(false));
}

TEST(StaticPaTrackerLive, FewStarsFallsBackToTheGuider)
{
    std::vector<SynthStar> two = { { 300, 300, 30000 }, { 700, 500, 20000 } };
    SynthImage img = Render(1280, 960, two);
    LiveTracker t;
    t.Seed(img.View(), 300, 300, DetectParams(), At(0));
    EXPECT_TRUE(t.Seeded());
    EXPECT_FALSE(t.FieldMode());
    EXPECT_EQ(t.StepGuider(true, 310, 305, At(1000)), StepResult::Fix);
    EXPECT_EQ(t.State(1000, true), LiveState::Tracking);
    EXPECT_DOUBLE_EQ(t.FixX(), 310);
    EXPECT_EQ(t.ConfirmedBy(), 0);
    EXPECT_EQ(t.StepGuider(false, 0, 0, At(2000)), StepResult::Miss);
    EXPECT_FALSE(t.FixIsFresh(2000));
    EXPECT_EQ(t.State(2000, true), LiveState::Searching);
}

TEST(StaticPaTrackerLive, ResetClearsTheSeedAndCounters)
{
    Scene s = MakeScene();
    LiveTracker t;
    SeedOn(t, s);
    SynthImage away = Render(1280, 960, RandomSky(5, 50, 0, 0, 1280, 960), 1001);
    t.Step(away.View(), DetectParams(), At(1000));
    t.Reset();
    EXPECT_FALSE(t.Seeded());
    EXPECT_FALSE(t.FieldMode());
    EXPECT_EQ(t.SeedCount(), 0);
    EXPECT_EQ(t.FramesSinceFix(), 0);
    EXPECT_FALSE(t.FixIsFresh(1000));
    EXPECT_FALSE(t.Ambiguous());
}

TEST(StaticPaTrackerLive, LargeSensorFitsTheTimeBudget)
{
    // IMX178-class 3096x2080: the detection pass is binned 3x3. The work runs
    // on the main thread (the same thread that serves JSON-RPC) once per frame.
    std::vector<SynthStar> sky = RandomSky(88, 300, -600, -600, 3700, 2680, 3000, 40000);
    sky.push_back({ 1548.3, 1040.2, 30000 });
    SynthImage seedImg = Render(3096, 2080, sky);
    SynthImage moved = Render(3096, 2080, Shifted(sky, 588, -200), 2);
    LiveTracker t;
    t.Seed(seedImg.View(), 1548.3, 1040.2, DetectParams(), At(0));
    ASSERT_TRUE(t.FieldMode());
    auto t0 = std::chrono::steady_clock::now();
    StepResult r = t.Step(moved.View(), DetectParams(), At(1000));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ(r, StepResult::Fix);
    EXPECT_NEAR(t.FixX(), 1548.3 + 588, 0.5);
    EXPECT_NEAR(t.FixY(), 1040.2 - 200, 0.5);
    RecordProperty("step_ms", std::to_string(ms));
    std::printf("3096x2080 step: %.1f ms\n", ms);
    EXPECT_LT(ms, 250.0);
}

TEST(StaticPaTrackerLive, StateNames)
{
    EXPECT_STREQ(LiveStateName(LiveState::Tracking), "tracking");
    EXPECT_STREQ(LiveStateName(LiveState::Searching), "searching");
    EXPECT_STREQ(LiveStateName(LiveState::Lost), "lost");
}
