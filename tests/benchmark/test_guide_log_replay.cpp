// Guide log replay against a real RC51 guide log (#142 Tier 4).
//
// tests/benchmark/fixtures/rc51_guidelog_2026-11-22.txt is the first 1760 lines of the RC51 guide
// log of the night of 2026-11-22 (RedCat 51, 120 mm guide scope at 6.45"/px, ASI120MM Mini,
// iOptron HAE29C, guided by this daemon driven by Ara): a calibration and five guiding sessions
// with Hysteresis on RA (hysteresis 0.10, aggression 0.90, minMove 0.08) and Resist Switch on Dec
// (aggression 75 %, minMove 0.08), 1/s exposures, dithers with their fast recenter, settling,
// pulses the mount did not take, and guiding disabled and enabled again. Its sessions have no
// parameter change the log does not record, so every guide distance and pulse it logs can be
// reproduced: the logged offsets go through the real kernels with the header's parameters and the
// logged guide distances and pulses must come back, within the log's 0.001 px rounding. That pins
// the wx-free kernels to what production computed on the rig, and the replay's reading of the log
// (where the dither reset falls, the recenter, the frame in flight at a switch).
//
// The log does not record a parameter set over JSON-RPC (set_algo_param) while guiding, so a
// session with such a change cannot be reproduced past it; other RC51 nights have them (see
// design/RC51_CHECKLIST.md). DetunedParametersAreCaught shows such a change is caught.

#include "guide_log_replay.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#ifndef GUIDE_LOG_FIXTURE
# error GUIDE_LOG_FIXTURE must name the RC51 guide log fixture
#endif

using namespace guide_log_replay;

namespace
{

std::vector<guide_log::Session> Sessions()
{
    std::vector<guide_log::Session> sessions;
    std::string error;
    EXPECT_TRUE(guide_log::ParseFile(GUIDE_LOG_FIXTURE, &sessions, &error)) << error;
    return sessions;
}

bool SameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}

} // namespace

TEST(GuideLogReplay, ReadsTheSessionHeaders)
{
    std::vector<guide_log::Session> sessions = Sessions();
    ASSERT_EQ(sessions.size(), 5u);
    const int lines[] = { 92, 283, 627, 743, 1086 };
    const int frames[] = { 114, 295, 88, 299, 561 };
    for (size_t i = 0; i < sessions.size(); i++)
    {
        const guide_log::Session& s = sessions[i];
        SCOPED_TRACE(s.begins);
        EXPECT_EQ(s.line, lines[i]);
        EXPECT_EQ(s.FrameCount(), frames[i]);
        EXPECT_TRUE(s.mount);
        EXPECT_DOUBLE_EQ(s.pixelScale, 6.45);
        EXPECT_DOUBLE_EQ(s.exposureMs, 1000.);
        EXPECT_DOUBLE_EQ(s.xRate, 1.196);
        EXPECT_DOUBLE_EQ(s.yRate, 1.067);
        EXPECT_DOUBLE_EQ(s.xAngle, -96.7);
        EXPECT_DOUBLE_EQ(s.yAngle, -11.6);
        EXPECT_EQ(s.x.name, "Hysteresis");
        EXPECT_DOUBLE_EQ(s.x.hysteresis, 0.1);
        EXPECT_DOUBLE_EQ(s.x.aggression, 0.9);
        EXPECT_DOUBLE_EQ(s.x.minMove, 0.08);
        EXPECT_EQ(s.y.name, "Resist Switch");
        EXPECT_DOUBLE_EQ(s.y.aggression, 0.75);
        EXPECT_DOUBLE_EQ(s.y.minMove, 0.08);
        EXPECT_TRUE(s.y.fastSwitch);
        EXPECT_FALSE(s.backlashComp);
        EXPECT_EQ(s.maxRaMs, 2500);
        EXPECT_EQ(s.maxDecMs, 2500);
        EXPECT_EQ(s.decGuideMode, "Auto");
    }
    // the last session disables guiding and enables it again; the others dither
    int dithers = 0;
    int switches = 0;
    for (const guide_log::Session& s : sessions)
    {
        for (const guide_log::Event& e : s.events)
        {
            dithers += e.kind == guide_log::Event::Dither;
            switches += e.kind == guide_log::Event::GuidingEnabled || e.kind == guide_log::Event::GuidingDisabled;
        }
    }
    EXPECT_GE(dithers, 15);
    EXPECT_EQ(switches, 2);
}

TEST(GuideLogReplay, KernelsReproduceTheLoggedCorrections)
{
    int steps = 0;
    int ambiguous = 0;
    int unsent = 0;
    for (const guide_log::Session& s : Sessions())
    {
        SCOPED_TRACE(s.begins);
        SessionReport r = ReplaySession(s);
        for (const AxisReproduction *a : { &r.ra, &r.dec })
        {
            SCOPED_TRACE(a->algorithm);
            ASSERT_TRUE(a->supported);
            for (const Mismatch& m : a->mismatches)
                ADD_FAILURE() << "line " << m.line << " frame " << m.frame << " " << m.axis << ": input " << m.input
                              << " logged " << m.logged << " replayed " << m.replayed << ", pulse logged " << m.loggedMs
                              << " replayed " << m.replayedMs;
            EXPECT_EQ(a->distanceMatches + a->thresholdAmbiguous, a->steps);
            EXPECT_LE(a->maxDistanceError, 0.002);
            EXPECT_EQ(a->durationMatches, a->durationsChecked);
            EXPECT_GT(a->durationsChecked, 0);
            steps += a->steps;
            ambiguous += a->thresholdAmbiguous;
            unsent += a->unsent;
        }
    }
    // a threshold inside the log's rounding is rare; so is a pulse the mount did not take (the
    // last frame of a session, mostly)
    EXPECT_GT(steps, 2500);
    EXPECT_LE(ambiguous * 100, steps);
    EXPECT_LE(unsent, 10);
}

TEST(GuideLogReplay, SelfReplayGivesBackTheLoggedOffsets)
{
    // the logged algorithm, closed loop on the motion rebuilt from the log, puts the star where the
    // log has it, frame for frame: the reconstruction and the pulse model are the log's
    for (const guide_log::Session& s : Sessions())
    {
        SCOPED_TRACE(s.begins);
        SessionReport r = ReplaySession(s);
        EXPECT_LT(r.selfReplayMaxError, 1e-6);
        EXPECT_NEAR(r.selfReplay.ra, r.logged.ra, 1e-6);
        EXPECT_NEAR(r.selfReplay.dec, r.logged.dec, 1e-6);
        EXPECT_GT(r.logged.frames, 0);
    }
}

TEST(GuideLogReplay, ReplayIsDeterministic)
{
    std::vector<guide_log::Session> sessions = Sessions();
    for (const guide_log::Session& s : sessions)
    {
        SCOPED_TRACE(s.begins);
        SessionReport a = ReplaySession(s);
        SessionReport b = ReplaySession(s);
        EXPECT_EQ(a.ra.distanceMatches, b.ra.distanceMatches);
        EXPECT_EQ(a.dec.distanceMatches, b.dec.distanceMatches);
        EXPECT_TRUE(SameBits(a.selfReplay.ra, b.selfReplay.ra));
        EXPECT_TRUE(SameBits(a.selfReplay.dec, b.selfReplay.dec));
        EXPECT_TRUE(SameBits(a.uncorrected.ra, b.uncorrected.ra));
        ASSERT_EQ(a.raPredictions.size(), b.raPredictions.size());
        for (size_t i = 0; i < a.raPredictions.size(); i++)
            EXPECT_TRUE(SameBits(a.raPredictions[i].rms, b.raPredictions[i].rms)) << a.raPredictions[i].algorithm;
        ASSERT_EQ(a.decPredictions.size(), b.decPredictions.size());
        for (size_t i = 0; i < a.decPredictions.size(); i++)
            EXPECT_TRUE(SameBits(a.decPredictions[i].rms, b.decPredictions[i].rms)) << a.decPredictions[i].algorithm;
    }
    // and the report, byte for byte
    std::vector<SessionReport> first;
    std::vector<SessionReport> second;
    for (const guide_log::Session& s : sessions)
    {
        first.push_back(ReplaySession(s));
        second.push_back(ReplaySession(s));
    }
    EXPECT_EQ(Markdown("fixture", first), Markdown("fixture", second));
}

TEST(GuideLogReplay, DetunedParametersAreCaught)
{
    // the check is not vacuous: the RA aggression the night did not use (0.7 for 0.9), or a Dec
    // minMove of 0.15 for 0.08, leaves most of the guide distances unreproduced
    std::vector<guide_log::Session> sessions = Sessions();
    guide_log::Session s = sessions[4];
    s.x.aggression = 0.7;
    s.y.minMove = 0.15;
    SessionReport r = ReplaySession(s);
    EXPECT_LT(r.ra.distanceMatches * 2, r.ra.steps);
    EXPECT_LT(r.dec.distanceMatches + r.dec.thresholdAmbiguous, r.dec.steps * 9 / 10);
    EXPECT_FALSE(r.ra.mismatches.empty());
}

TEST(GuideLogReplay, PredictionsAreFiniteAndGuidingHelped)
{
    for (const guide_log::Session& s : Sessions())
    {
        SCOPED_TRACE(s.begins);
        SessionReport r = ReplaySession(s);
        ASSERT_EQ(r.raPredictions.size(), 5u); // as logged, then the four kernels' defaults
        ASSERT_EQ(r.decPredictions.size(), 5u);
        EXPECT_EQ(r.raPredictions[0].algorithm.find("Hysteresis"), 0u);
        EXPECT_NE(r.raPredictions[0].algorithm.find("as logged"), std::string::npos);
        // as logged, closed loop, is the self-replay
        EXPECT_NEAR(r.raPredictions[0].rms, r.logged.ra, 0.05 * r.logged.ra + 1e-9);
        EXPECT_NEAR(r.decPredictions[0].rms, r.logged.dec, 0.05 * r.logged.dec + 1e-9);
        for (const Prediction& p : r.raPredictions)
            EXPECT_TRUE(std::isfinite(p.rms) && p.rms > 0.) << p.algorithm;
        for (const Prediction& p : r.decPredictions)
            EXPECT_TRUE(std::isfinite(p.rms) && p.rms > 0.) << p.algorithm;
        // a long session: the rebuilt motion is far worse than what was guided
        if (r.logged.frames > 200)
        {
            EXPECT_GT(r.uncorrected.ra, 3. * r.logged.ra);
            EXPECT_GT(r.uncorrected.dec, 3. * r.logged.dec);
        }
    }
}
