// The extracted frame rejection and refine step against the frozen copy of the code they came
// from (#142). legacy/multistar_legacy.cpp is a verbatim copy of MassChecker, DistanceChecker and
// GuiderMultiStar::RefineOffset from guider_multistar.cpp at 7755af25 (legacy/check_twin.sh diffs
// it), compiled against mechanical stand-ins. Each test drives the twin and the production code
// (frame_reject.cpp, and multistar_refine::Refine called the way GuiderMultiStar::RefineOffset now
// calls it) with the same seeded calls and requires the same results, the same state, the same
// star finder calls and the same Debug lines after every call, doubles bit for bit. Coverage
// counters check every path is reached. The intended differences are #261 (DistanceChecker back
// from recovering clears the forced tolerance): the sequence must match up to it; and #262 (the star
// after a zero-count drop counted once): each multi-star scenario must match up to the frame it
// happens on.

#include <gtest/gtest.h>

#include "frame_reject.h"
#include "legacy/multistar_legacy.h"
#include "multistar_refine.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
bool Same(double a, double b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}
} // namespace


// --- MassChecker ---

TEST(MultistarParity, MassChecker)
{
    int checks = 0, rejects = 0, resets = 0;
    for (uint64_t seed = 1; seed <= 300; seed++)
    {
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double> u(0., 1.);
        legacy_api::MassChecker twin;
        frame_reject::MassChecker prod;
        int64_t t = 1000 + static_cast<int64_t>(u(rng) * 1e6);
        legacy::monotonic_timers::nowMs = t;
        double base = 200 + u(rng) * 5000;
        const int exposures[] = { 500, 1000, 2000, 3000 };
        const double thresholds[] = { 0., 0.1, 0.3, 0.5, 1.0 };
        int exposure = 1000;
        bool autoExp = u(rng) < 0.5;
        for (int i = 0; i < 400; i++)
        {
            double r = u(rng);
            if (r < 0.02)
            {
                exposure = exposures[static_cast<int>(u(rng) * 4)];
                if (u(rng) < 0.3)
                    autoExp = !autoExp;
                twin.SetExposure(exposure, autoExp);
                prod.SetExposure(exposure, autoExp);
            }
            else if (r < 0.025)
            {
                twin.Reset();
                prod.Reset();
                ++resets;
            }
            else if (r < 0.03)
            {
                unsigned int w = 2000 + static_cast<unsigned int>(u(rng) * 40000);
                twin.SetTimeWindow(w);
                prod.SetTimeWindow(w);
            }
            // the sky: a drifting base, clouds, spikes and noise
            base *= 1 + (u(rng) - 0.5) * 0.02;
            double mass = base * (1 + (u(rng) - 0.5) * 0.2);
            double kind = u(rng);
            if (kind < 0.05)
                mass *= 0.3;
            else if (kind < 0.08)
                mass *= 3;
            if (autoExp)
                mass *= exposure / 1000.;
            double threshold = thresholds[static_cast<int>(u(rng) * 5)];
            double lt[4] = { -1, -1, -1, -1 }, lp[4] = { -1, -1, -1, -1 };
            bool rt = twin.CheckMass(mass, threshold, lt);
            bool rp = prod.CheckMass(mass, threshold, lp);
            ASSERT_EQ(rt, rp) << "seed " << seed << " call " << i;
            for (int k = 0; k < 4; k++)
                ASSERT_TRUE(Same(lt[k], lp[k])) << "seed " << seed << " call " << i << " limit " << k;
            ++checks;
            rejects += rp;
            t += static_cast<int64_t>(u(rng) < 0.02 ? u(rng) * 120000 : u(rng) * 5000);
            legacy::monotonic_timers::nowMs = t;
            twin.AppendData(mass);
            prod.AppendData(mass, t);
        }
    }
    EXPECT_GT(rejects, checks / 50);
    EXPECT_LT(rejects, checks / 2);
    EXPECT_GT(resets, 0);
}

// --- DistanceChecker ---

TEST(MultistarParity, DistanceChecker)
{
    std::map<int, int> states;
    int activations = 0, keptTolerance = 0;
    for (uint64_t seed = 1; seed <= 300; seed++)
    {
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double> u(0., 1.);
        legacy::Debug.lines.clear();
        legacy_api::DistanceChecker twin;
        std::vector<std::string> log;
        frame_reject::DistanceChecker prod([&log](const std::string& s) { log.push_back(s); });
        int64_t t = static_cast<int64_t>(u(rng) * 1e7);
        legacy::Guider& g = *legacy::pFrame->pGuider;
        const double tolerances[] = { 9e99, 2., 4., 8. };
        for (int i = 0; i < 400; i++)
        {
            t += static_cast<int64_t>(u(rng) * 3000);
            legacy::monotonic_timers::nowMs = t;
            if (u(rng) < 0.04)
            {
                twin.Activate();
                prod.Activate(t);
                ++activations;
            }
            g.guiding = u(rng) < 0.95;
            g.paused = u(rng) < 0.03;
            legacy::PhdController::settling = u(rng) < 0.03;
            g.frameCount = u(rng) < 0.05 ? static_cast<unsigned int>(u(rng) * 10) : 20;
            g.avgDist = 0.2 + u(rng);
            g.avgDistRA = 0.2 + u(rng) * 0.7;
            bool raOnly = u(rng) < 0.2;
            double tolerance = tolerances[static_cast<int>(u(rng) * 4)];
            double distance = u(rng) < 0.2 ? u(rng) * 10 : u(rng) * 1.5;

            frame_reject::GuiderStatus st;
            st.guiding = g.guiding;
            st.paused = g.paused;
            st.settling = legacy::PhdController::settling;
            st.frameCount = g.frameCount;
            st.avgDist = g.CurrentErrorSmoothed(raOnly);
            bool rt = twin.CheckDistance(distance, raOnly, tolerance);
            bool rp = prod.CheckDistance(distance, tolerance, st, t);
            ASSERT_EQ(rt, rp) << "seed " << seed << " call " << i;
            ASSERT_EQ(twin.State(), static_cast<int>(prod.GetState())) << "seed " << seed << " call " << i;
            if (twin.State() == frame_reject::DistanceChecker::ST_GUIDING && twin.ForceTolerance() != 0.)
            {
                // The intended difference (#261): the twin came back from recovering with the forced
                // tolerance still set; production cleared it. They match up to here.
                ASSERT_EQ(prod.ForceTolerance(), 0.) << "seed " << seed << " call " << i;
                ++keptTolerance;
                break;
            }
            ASSERT_TRUE(Same(twin.ForceTolerance(), prod.ForceTolerance())) << "seed " << seed << " call " << i;
            ++states[twin.State()];
        }
        ASSERT_EQ(legacy::Debug.lines, log) << "seed " << seed;
    }
    EXPECT_GT(activations, 0);
    EXPECT_GT(states[frame_reject::DistanceChecker::ST_GUIDING], 0);
    EXPECT_GT(states[frame_reject::DistanceChecker::ST_WAITING], 0);
    EXPECT_GT(states[frame_reject::DistanceChecker::ST_RECOVERING], 0);
    EXPECT_GT(keptTolerance, 0); // the #261 path is reached
}

namespace
{
// --- RefineOffset ---

struct FindCall
{
    int id;
    int baseX;
    int baseY;
    bool verbose;
    bool operator==(const FindCall& o) const
    {
        return id == o.id && baseX == o.baseX && baseY == o.baseY && verbose == o.verbose;
    }
};

// Where a star is found on a frame: a function of the seed, the frame, the star and its reference
// point only, so a second Find of the same star on a frame finds the same. Stars with an id that
// is a multiple of 4 often find exactly no movement on one axis, so their zero count reaches 5;
// those with an id of 1 mod 6 mostly sit 6 px off their reference point.
bool SkyFind(uint64_t seed, int frame, legacy::GuideStar& s, int baseX, int baseY)
{
    std::mt19937_64 rng(seed * 1000003u ^ static_cast<uint64_t>(frame) * 7919u ^ static_cast<uint64_t>(s.id) * 104729u);
    std::uniform_real_distribution<double> u(0., 1.);
    std::normal_distribution<double> seeing(0., 0.4);
    if (u(rng) < 0.08)
    {
        // Star::Find moves the star to where it looked even when it is not found
        s.X = baseX + 0.25;
        s.Y = baseY + 0.25;
        s.SNR = 0.;
        return false;
    }
    double r = u(rng);
    double zeroAxis = s.id % 4 == 0 ? 0.6 : 0.05;
    double dx = seeing(rng), dy = seeing(rng);
    if (r < 0.04)
        dx = dy = 0.; // a hot pixel
    else if (r < 0.04 + zeroAxis)
        (u(rng) < 0.5 ? dx : dy) = 0.;
    else if (r < 0.04 + zeroAxis + 0.1)
        dx += (u(rng) < 0.5 ? -1 : 1) * (3 + u(rng) * 10); // an excursion
    else if (s.id % 6 == 1 && u(rng) < 0.9)
        dx += 6; // a star that sits off its reference: misses, then a reset after ten
    s.X = s.referencePoint.X + dx;
    s.Y = s.referencePoint.Y + dy;
    s.SNR = 5 + u(rng) * 45;
    return true;
}

void ExpectSameStars(const std::vector<legacy::GuideStar>& a, const std::vector<legacy::GuideStar>& b, const std::string& where)
{
    ASSERT_EQ(a.size(), b.size()) << where;
    for (size_t i = 0; i < a.size(); i++)
    {
        const legacy::GuideStar& x = a[i];
        const legacy::GuideStar& y = b[i];
        ASSERT_EQ(x.id, y.id) << where << " star " << i;
        ASSERT_TRUE(Same(x.X, y.X) && Same(x.Y, y.Y) && Same(x.SNR, y.SNR)) << where << " star " << i;
        ASSERT_TRUE(Same(x.referencePoint.X, y.referencePoint.X) && Same(x.referencePoint.Y, y.referencePoint.Y))
            << where << " star " << i;
        ASSERT_EQ(x.missCount, y.missCount) << where << " star " << i;
        ASSERT_EQ(x.zeroCount, y.zeroCount) << where << " star " << i;
        ASSERT_EQ(x.wasLost, y.wasLost) << where << " star " << i;
    }
}

size_t Count(const std::vector<std::string>& lines, const std::string& token)
{
    size_t n = 0;
    for (const std::string& l : lines)
        for (size_t p = l.find(token); p != std::string::npos; p = l.find(token, p + 1))
            ++n;
    return n;
}

} // namespace

TEST(MultistarParity, RefineOffset)
{
    std::map<std::string, size_t> seen;
    size_t diverged = 0, refinedFrames = 0;
    const char *tokens[] = { "entering stabilization",
                             "exiting stabilization",
                             "updating star positions",
                             ",U]",
                             ",DZ]",
                             ",L]",
                             ",M",
                             ",R]",
                             "refined,",
                             "single-star," };

    for (uint64_t seed = 1; seed <= 1500; seed++)
    {
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double> u(0., 1.);
        std::normal_distribution<double> wander(0., 0.7);

        // the twin
        legacy::GuiderMultiStar twin;
        DescriptiveStats twinStats;
        twin.m_primaryDistStats = &twinStats;
        twin.m_maxStars = 2 + static_cast<unsigned int>(u(rng) * 10);
        twin.m_stabilitySigmaX = u(rng) < 0.5 ? 5 : 2 + u(rng) * 8;
        legacy::Debug.lines.clear();
        std::vector<FindCall> twinCalls;

        // production: GuiderMultiStar's members and RefineOffset's call of Refine
        std::vector<legacy::GuideStar> prodStars;
        multistar_refine::State prodState;
        DescriptiveStats prodStats;
        bool prodMultiStarMode = true;
        unsigned int prodStarsUsed = 0;
        std::vector<std::string> prodLog;
        std::vector<FindCall> prodCalls;

        legacy::GuideStar primary;
        primary.X = 640;
        primary.Y = 480;
        twin.m_guideStars.push_back(primary);
        int n = static_cast<int>(u(rng) * 12);
        for (int id = 1; id <= n; id++)
        {
            legacy::GuideStar s;
            s.id = id;
            s.X = s.referencePoint.X = u(rng) * 1280;
            s.Y = s.referencePoint.Y = u(rng) * 960;
            s.SNR = 5 + u(rng) * 45;
            s.offsetFromPrimary = legacy::PHD_Point(s.X - primary.X, s.Y - primary.Y);
            twin.m_guideStars.push_back(s);
        }
        prodStars = twin.m_guideStars;

        double px = primary.X, py = primary.Y;
        for (int frame = 0; frame < 80; frame++)
        {
            std::string where = "seed " + std::to_string(seed) + " frame " + std::to_string(frame);
            if (u(rng) < 0.03)
            {
                // GuiderMultiStar::SetLockPosition
                twin.m_lockPositionMoved = twin.m_stabilizing = true;
                prodState.lockPositionMoved = prodState.stabilizing = true;
            }
            if (u(rng) < 0.01)
            {
                twinStats.ClearAll();
                prodStats.ClearAll();
            }
            bool guiding = u(rng) < 0.95;
            bool guidingEnabled = u(rng) < 0.97;
            bool settling = u(rng) < 0.04;
            twin.guiding = guiding;
            legacy::pMount->guidingEnabled = guidingEnabled;
            legacy::PhdController::settling = settling;

            double ox, oy;
            double kind = u(rng);
            if (kind < 0.05)
                ox = oy = 0.;
            else if (kind < 0.12)
            {
                ox = (u(rng) < 0.5 ? -1 : 1) * (5 + u(rng) * 10);
                oy = wander(rng);
            }
            else
            {
                ox = wander(rng);
                oy = wander(rng);
            }
            px = 640 + ox;
            py = 480 + oy;
            double snr = 5 + u(rng) * 45;

            // the twin
            twin.m_primaryStar.X = px;
            twin.m_primaryStar.Y = py;
            twin.m_primaryStar.SNR = snr;
            legacy::g_find = [&](legacy::GuideStar& s, int bx, int by, bool verbose)
            {
                twinCalls.push_back({ s.id, bx, by, verbose });
                return SkyFind(seed, frame, s, bx, by);
            };
            legacy::GuiderOffset twinOfs;
            twinOfs.cameraOfs = legacy::PHD_Point(ox, oy);
            size_t twinCallsBefore = twinCalls.size();
            bool twinRefined = twin.RefineOffset(nullptr, &twinOfs);

            // production, as GuiderMultiStar::RefineOffset calls Refine
            legacy::GuiderOffset prodOfs;
            prodOfs.cameraOfs = legacy::PHD_Point(ox, oy);
            size_t prodCallsBefore = prodCalls.size();
            prodStarsUsed = 1;
            bool prodRefined = false;
            if (guiding && prodStars.size() > 1 && guidingEnabled && !settling)
            {
                multistar_refine::Frame f;
                f.primaryX = px;
                f.primaryY = py;
                f.primarySNR = snr;
                f.offsetX = prodOfs.cameraOfs.X;
                f.offsetY = prodOfs.cameraOfs.Y;
                f.maxStars = twin.m_maxStars;
                f.stabilitySigmaX = twin.m_stabilitySigmaX;
                auto find = [&](legacy::GuideStar& s, double x, double y, bool verbose)
                {
                    // Star::Find's base_x and base_y are int
                    int bx = static_cast<int>(x), by = static_cast<int>(y);
                    prodCalls.push_back({ s.id, bx, by, verbose });
                    return SkyFind(seed, frame, s, bx, by);
                };
                auto valid = [&twin](double x, double y) { return twin.IsValidSecondaryStarPosition(legacy::PHD_Point(x, y)); };
                multistar_refine::Result r =
                    multistar_refine::Refine(prodStars, f, prodState, prodStats, find, valid,
                                             [&prodLog](const std::string& s) { prodLog.push_back(s); });
                prodStarsUsed = r.starsUsed;
                if (r.refined)
                {
                    prodOfs.cameraOfs.X = r.offsetX;
                    prodOfs.cameraOfs.Y = r.offsetY;
                    prodRefined = true;
                }
            }

            // A star found twice in the frame's loop: the twin's zero-count double count (#262)
            auto loopCalls = [](const std::vector<FindCall>& calls, size_t from, bool *twice)
            {
                std::vector<FindCall> once;
                std::map<int, int> perStar;
                *twice = false;
                for (size_t i = from; i < calls.size(); i++)
                {
                    if (!calls[i].verbose && ++perStar[calls[i].id] == 2)
                        *twice = true;
                    else
                        once.push_back(calls[i]);
                }
                return once;
            };
            bool prodTwice, twinTwice;
            loopCalls(prodCalls, prodCallsBefore, &prodTwice);
            ASSERT_FALSE(prodTwice) << where;
            std::vector<FindCall> twinOnce = loopCalls(twinCalls, twinCallsBefore, &twinTwice);
            if (twinTwice)
            {
                // The intended difference (#262): the twin found a star a second time after a
                // zero-count drop. They match up to this frame; on it production makes the twin's
                // Find calls without the repeat, and goes on to more stars when the twin's double
                // count reached maxStars first.
                std::vector<FindCall> prodFrame(prodCalls.begin() + static_cast<std::ptrdiff_t>(prodCallsBefore),
                                                prodCalls.end());
                ASSERT_LE(twinOnce.size(), prodFrame.size()) << where;
                ASSERT_TRUE(std::equal(twinOnce.begin(), twinOnce.end(), prodFrame.begin())) << where;
                ASSERT_LE(prodStarsUsed, twin.m_starsUsed) << where;
                ++diverged;
                break;
            }

            ASSERT_EQ(twinRefined, prodRefined) << where;
            ASSERT_TRUE(Same(twinOfs.cameraOfs.X, prodOfs.cameraOfs.X) && Same(twinOfs.cameraOfs.Y, prodOfs.cameraOfs.Y))
                << where;
            ASSERT_EQ(twin.m_starsUsed, prodStarsUsed) << where;
            ASSERT_EQ(twin.m_stabilizing, prodState.stabilizing) << where;
            ASSERT_EQ(twin.m_lockPositionMoved, prodState.lockPositionMoved) << where;
            ASSERT_EQ(twin.m_multiStarMode, prodMultiStarMode) << where;
            ASSERT_EQ(twinStats.GetCount(), prodStats.GetCount()) << where;
            ASSERT_TRUE(Same(twinStats.GetSigma(), prodStats.GetSigma())) << where;
            ExpectSameStars(twin.m_guideStars, prodStars, where);
            ASSERT_EQ(twinCalls, prodCalls) << where;
            ASSERT_EQ(legacy::Debug.lines, prodLog) << where;

            refinedFrames += prodRefined;
        }
        for (const char *t : tokens)
            seen[t] += Count(prodLog, t);
    }
    for (const char *t : tokens)
        EXPECT_GT(seen[t], 0u) << t;
    EXPECT_GT(diverged, 0u); // the #262 path is reached
    EXPECT_GT(refinedFrames, 0u);
}
