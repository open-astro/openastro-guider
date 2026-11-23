// The extracted guider state machine against the frozen copy of the code it came from (#142,
// slice T2c). legacy/guider_sm_legacy.cpp is a verbatim copy of Guider::SetState, StopGuiding,
// MoveLockPosition and the state-machine parts of UpdateGuideState from guider.cpp at 579829c3
// (legacy/check_twin.sh diffs it), compiled against mechanical stand-ins. Each test drives the
// twin and guider_state_machine (through the test Host in guider_sm_host.h, which does what
// GuiderStateHost in guider.cpp does, and for the dither the same calls Guider::MoveLockPosition
// now makes) with the same seeded calls and gear answers, and requires the same calls on the
// gear, the frame, the event server and the guide log, the same Debug lines and the same state
// after every call, doubles bit for bit. Coverage counters check every action and path is reached.
//
// The #285 fixes to the AO (secondary mount) calibration depart from the twin on purpose: with a
// connected secondary, a SetState to CALIBRATING_PRIMARY, CALIBRATING_SECONDARY or GUIDING, or a
// frame from CALIBRATING_PRIMARY to CALIBRATED (which may enter GUIDING, and #255 calibrate
// again), may end differently. Such an op is the only one
// allowed to differ, and the seed ends there (the gear's answers are no longer drawn in step);
// test_guider_state_machine.cpp pins what the fixed machine does instead. Without a connected
// secondary nothing may differ.

#include <gtest/gtest.h>

#include "guider_sm_host.h"
#include "legacy/guider_sm_legacy.h"

#include <cmath>
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

using legacy_sm::TwinCalStep;
using sm_test::CalStep;

// the gear's answers, drawn in call order: each side has its own generator on the same seed
struct Answers
{
    std::mt19937_64 rng;
    double pBeginFail;
    double pCalComplete;
    double pCalFail;

    double U() { return std::uniform_real_distribution<double>(0., 1.)(rng); }
    bool BeginFails() { return U() < pBeginFail; }
    int CalStepIndex()
    {
        double r = U();
        return r < pCalFail ? 2 : r < pCalFail + pCalComplete ? 1 : 0;
    }
};

struct Pair
{
    sm_test::World w;
    legacy_sm::Guider g;
    legacy_sm::Mount primary;
    legacy_sm::Mount secondary;
    bool secondaryExists = false;
    Answers ansNew;
    Answers ansOld;

    explicit Pair(uint64_t seed, double pBeginFail, double pCalComplete, double pCalFail)
        : ansNew { std::mt19937_64(seed), pBeginFail, pCalComplete, pCalFail },
          ansOld { std::mt19937_64(seed), pBeginFail, pCalComplete, pCalFail }
    {
        primary.name = "Primary";
        secondary.name = "Secondary";
        g.m_image.Size = { 640, 480 };
        w.beginFails = [this](bool) { return ansNew.BeginFails(); };
        w.calStep = [this](bool) { return static_cast<CalStep>(ansNew.CalStepIndex()); };
        primary.beginFails = secondary.beginFails = [this] { return ansOld.BeginFails(); };
        primary.calStep = secondary.calStep = [this] { return static_cast<TwinCalStep>(ansOld.CalStepIndex()); };
    }

    // point the twin's globals at this pair's gear
    void Bind()
    {
        legacy_sm::pMount = &primary;
        legacy_sm::pSecondaryMount = secondaryExists ? &secondary : nullptr;
    }

    void SetGear(bool pCal, bool sExists, bool sConn, bool sCal)
    {
        w.primaryCalibrated = primary.calibrated = pCal;
        w.secondaryExists = secondaryExists = sExists;
        w.secondaryConnected = secondary.connected = sConn;
        w.secondaryCalibrated = secondary.calibrated = sCal;
        Bind();
    }
};

// the state ExpectSame compares, as one answer
bool Matches(const Pair& p)
{
    const sm_test::World& w = p.w;
    const legacy_sm::Guider& g = p.g;
    return w.trace == legacy_sm::Twin::trace && w.state == g.m_state && w.lockValid == g.m_lockPosition.IsValid() &&
        w.primaryCalibrated == p.primary.calibrated && w.secondaryCalibrated == p.secondary.calibrated &&
        w.recenterActive == g.m_ditherRecenterRemaining.IsValid() && w.recenterStepsPending == g.m_ditherRecenterStepsPending &&
        w.recenterStalledSteps == g.m_ditherRecenterStalledSteps && w.recenteringForWorker == g.m_recenteringForWorker;
}

void ExpectSame(Pair& p, const std::string& what)
{
    const sm_test::World& w = p.w;
    const legacy_sm::Guider& g = p.g;
    ASSERT_EQ(w.trace, legacy_sm::Twin::trace) << what;
    ASSERT_EQ(w.state, g.m_state) << what;
    ASSERT_EQ(w.lockValid, g.m_lockPosition.IsValid()) << what;
    ASSERT_EQ(w.primaryCalibrated, p.primary.calibrated) << what;
    ASSERT_EQ(w.secondaryCalibrated, p.secondary.calibrated) << what;
    ASSERT_EQ(w.recenterActive, g.m_ditherRecenterRemaining.IsValid()) << what;
    ASSERT_EQ(w.recenterStepsPending, g.m_ditherRecenterStepsPending) << what;
    ASSERT_EQ(w.recenterStalledSteps, g.m_ditherRecenterStalledSteps) << what;
    ASSERT_EQ(w.recenteringForWorker, g.m_recenteringForWorker) << what;
}

std::string LegacyFrame(legacy_sm::Guider& g, bool starFound, bool paused)
{
    if (!starFound)
    {
        legacy_sm::FrameDroppedInfo info;
        g.LegacyStarLost(&g.m_image, info);
        return "lost";
    }
    if (paused)
    {
        g.LegacyPaused();
        return "Paused";
    }
    legacy_sm::GuiderOffset ofs;
    return g.LegacyStarFound(ofs).str();
}
} // namespace

TEST(GuiderStateMachineParity, SeededSequences)
{
    std::map<int, int> actionsSeen;
    std::map<int, int> statesSeen;
    int ops = 0;
    int departed = 0; // seeds ended by a #285 departure
    for (uint64_t seed = 1; seed <= 3500; seed++)
    {
        std::mt19937_64 rng(seed * 7919);
        auto u = [&rng] { return std::uniform_real_distribution<double>(0., 1.)(rng); };
        Pair p(seed, u() * 0.2, 0.1 + u() * 0.4, u() * 0.1);
        bool sExists = u() < 0.5;
        p.SetGear(u() < 0.5, sExists, sExists && u() < 0.7, u() < 0.5);
        p.w.lockSticky = p.g.m_lockPosIsSticky = u() < 0.4;
        p.w.measurementMode = p.g.m_measurementMode = u() < 0.15;
        p.w.primaryIsStepGuider = p.primary.stepGuider = u() < 0.3;
        std::string reason = u() < 0.3 ? "camera lost" : "";
        p.w.captureFailureReason = legacy_sm::pFrame->captureFailureReason = reason;
        GUIDER_STATE start = static_cast<GUIDER_STATE>(static_cast<int>(u() * 7));
        p.w.state = p.g.m_state = start;
        if (u() < 0.5)
        {
            p.w.lockValid = true;
            p.g.m_lockPosition.SetXY(100, 100);
        }

        for (int i = 0; i < 40; i++)
        {
            p.w.trace.clear();
            p.w.actions.clear();
            legacy_sm::Twin::trace.clear();
            std::string what = "seed " + std::to_string(seed) + " op " + std::to_string(i);
            const bool aoConnected = p.w.secondaryExists && p.w.secondaryConnected;
            const GUIDER_STATE before = p.w.state;
            bool mayDepart = false; // the op can reach the AO calibration #285 changed
            double r = u();
            if (r < 0.2)
            {
                GUIDER_STATE st = static_cast<GUIDER_STATE>(static_cast<int>(u() * 8));
                what += " SetState " + std::to_string(st);
                mayDepart = aoConnected &&
                    (st == STATE_CALIBRATING_PRIMARY || st == STATE_CALIBRATING_SECONDARY || st == STATE_GUIDING);
                sm_test::SetState(p.w, st);
                p.g.SetState(st);
            }
            else if (r < 0.27)
            {
                what += " StopGuiding";
                sm_test::StopGuiding(p.w);
                p.g.StopGuiding();
            }
            else if (r < 0.85)
            {
                bool found = u() < 0.85, paused = u() < 0.08;
                what += std::string(" Frame found=") + (found ? "1" : "0") + " paused=" + (paused ? "1" : "0");
                mayDepart =
                    aoConnected && found && !paused && before >= STATE_CALIBRATING_PRIMARY && before <= STATE_CALIBRATED;
                std::string a = sm_test::Frame(p.w, found, paused);
                std::string b = LegacyFrame(p.g, found, paused);
                if (mayDepart && (a != b || !Matches(p)))
                {
                    departed++;
                    break;
                }
                ASSERT_EQ(a, b) << what;
            }
            else
            {
                // the world changes between frames
                double c = u();
                what += " change";
                if (c < 0.2)
                {
                    // guide with recalibrate: the calibration is cleared
                    p.w.primaryCalibrated = p.primary.calibrated = false;
                    if (u() < 0.5)
                        p.w.secondaryCalibrated = p.secondary.calibrated = false;
                }
                else if (c < 0.35)
                {
                    p.w.adjustClearsPrimary = p.primary.adjustClears = u() < 0.3;
                    p.w.adjustClearsSecondary = p.secondary.adjustClears = u() < 0.3;
                }
                else if (c < 0.5)
                {
                    p.w.primaryBusy = p.primary.busy = u() < 0.3;
                    p.w.secondaryBusy = p.secondary.busy = u() < 0.3;
                }
                else if (c < 0.6)
                {
                    p.w.lockSticky = p.g.m_lockPosIsSticky = u() < 0.5;
                    p.w.measurementMode = p.g.m_measurementMode = u() < 0.2;
                }
                else if (c < 0.9)
                {
                    // a dither's fast recenter in progress, or ended
                    if (u() < 0.7)
                    {
                        double rx = std::floor(u() * 400) / 8, ry = std::floor(u() * 400) / 8;
                        double sx = std::floor(u() * 160) / 8, sy = std::floor(u() * 160) / 8;
                        int dx = u() < 0.5 ? 1 : -1, dy = u() < 0.5 ? 1 : -1;
                        p.w.recenterActive = true;
                        p.w.recenterRemainingX = rx;
                        p.w.recenterRemainingY = ry;
                        p.w.recenterStepX = sx;
                        p.w.recenterStepY = sy;
                        p.w.recenterDirX = dx;
                        p.w.recenterDirY = dy;
                        p.g.m_ditherRecenterRemaining.SetXY(rx, ry);
                        p.g.m_ditherRecenterStep.SetXY(sx, sy);
                        p.g.m_ditherRecenterDir = { dx, dy };
                    }
                    else
                    {
                        p.w.recenterActive = false;
                        p.g.m_ditherRecenterRemaining.Invalidate();
                    }
                    p.w.recenteringForWorker = p.g.m_recenteringForWorker = u() < 0.5;
                }
                else
                {
                    bool exists = u() < 0.5;
                    p.SetGear(p.w.primaryCalibrated, exists, exists && u() < 0.7, p.w.secondaryCalibrated);
                }
            }
            if (mayDepart && !Matches(p))
            {
                departed++;
                break;
            }
            ExpectSame(p, what);
            ops++;
            for (auto a : p.w.actions)
                actionsSeen[static_cast<int>(a)]++;
            statesSeen[p.w.state]++;
        }
    }

    EXPECT_GT(ops, 100000);
    EXPECT_GT(departed, 100);
    for (int a = 0; a <= static_cast<int>(guider_state_machine::Action::RecenterOver); a++)
        EXPECT_GT(actionsSeen[a], 0) << "action " << a << " never reached";
    for (GUIDER_STATE st : { STATE_SELECTING, STATE_SELECTED, STATE_CALIBRATING_PRIMARY, STATE_CALIBRATING_SECONDARY,
                             STATE_CALIBRATED, STATE_GUIDING })
        EXPECT_GT(statesSeen[st], 0) << "state " << st << " never reached";
}

// --- MoveLockPosition ---

namespace
{
// The calls Guider::MoveLockPosition makes now (guider.cpp), on the same stand-ins as the twin
struct NewDither
{
    bool lockValid = false;
    double lockX = 0, lockY = 0;
    int width = 640, height = 480;
    double margin = 10;
    bool calibrated = true;
    double angle = 0;
    int failAt = -1;
    int transformCalls = 0;
    bool fastRecenter = true;
    int maxMove = 20;

    bool recenterActive = false;
    double remX = 0, remY = 0, stepX = 0, stepY = 0;
    int dirX = 0, dirY = 0;
    int pending = 3, stalled = 2;
    bool forWorker = false;

    std::vector<std::string> trace;

    bool Move(bool argValid, double dx, double dy)
    {
        if (!argValid)
            return Err("Point is not valid");
        if (!calibrated)
            return Err("No mount");

        guider_state_machine::DitherChoice choice = guider_state_machine::ChooseDitherDelta(
            { lockX, lockY }, { dx, dy }, width, height,
            [this](const guider_state_machine::Point& m, guider_state_machine::Point *c)
            {
                trace.push_back("Primary.TransformMountCoordinatesToCameraCoordinates");
                if (transformCalls++ == failAt)
                    return true;
                *c = { m.x * cos(angle) - m.y * sin(angle), m.x * sin(angle) + m.y * cos(angle) };
                return false;
            },
            [this](const guider_state_machine::Point& q)
            { return q.x >= margin && q.x < width - margin && q.y >= margin && q.y < height - margin; },
            [this](const std::string& line) { trace.push_back("D:" + line); });
        if (!choice.ok)
            return Err("Transform failed");

        double nx = lockX + choice.camera.x, ny = lockY + choice.camera.y;
        trace.push_back("SetLockPosition");
        if (nx < 0.0 || nx >= width || ny < 0.0 || ny >= height)
            return Err("SetLockPosition failed");
        lockX = nx;
        lockY = ny;
        lockValid = true;

        double dist = std::hypot(choice.camera.x, choice.camera.y), distRA = std::fabs(choice.mount.x);
        char buf[100];
        std::snprintf(buf, sizeof buf, "Distance.OnDither(%a,%a)", dist, distRA);
        trace.push_back(buf);

        guider_state_machine::RecenterPlan plan =
            guider_state_machine::PlanFastRecenter(fastRecenter, dist, choice.mount, maxMove);
        if (plan.start)
        {
            recenterActive = true;
            remX = plan.remaining.x;
            remY = plan.remaining.y;
            pending = 0;
            stalled = 0;
            forWorker = true;
            dirX = plan.dirX;
            dirY = plan.dirY;
            stepX = plan.step.x;
            stepY = plan.step.y;
        }
        return false;
    }

    bool Err(const char *msg)
    {
        trace.push_back(std::string("D:E:") + msg);
        return true;
    }
};
} // namespace

TEST(GuiderStateMachineParity, MoveLockPosition)
{
    static legacy_sm::Mount primary; // the twin's pMount, set up again for each seed
    primary.name = "Primary";
    legacy_sm::pMount = &primary;
    legacy_sm::pSecondaryMount = nullptr;
    std::map<std::string, int> seen;
    int moves = 0;
    for (uint64_t seed = 1; seed <= 20000; seed++)
    {
        std::mt19937_64 rng(seed);
        auto u = [&rng] { return std::uniform_real_distribution<double>(0., 1.)(rng); };
        NewDither n;
        legacy_sm::Guider g;
        n.width = 200 + static_cast<int>(u() * 1000);
        n.height = 200 + static_cast<int>(u() * 800);
        g.m_image.Size = { n.width, n.height };
        n.margin = g.m_lockMargin = std::floor(u() * 30);
        n.calibrated = primary.calibrated = u() < 0.97;
        n.angle = primary.angle = u() < 0.3 ? 0 : (u() - 0.5) * 7;
        n.failAt = primary.failAt = u() < 0.1 ? static_cast<int>(u() * 4) : -1;
        n.transformCalls = primary.transformCalls = 0;
        n.fastRecenter = g.m_fastRecenterEnabled = u() < 0.7;
        n.maxMove = g.m_maxMovePixels = 5 + static_cast<int>(u() * 45);
        g.m_ditherRecenterStepsPending = n.pending;
        g.m_ditherRecenterStalledSteps = n.stalled;
        // the lock position: anywhere, often near an edge or a corner
        auto coord = [&](int size)
        {
            double r = u();
            return r < 0.3 ? u() * 40 : r < 0.6 ? size - u() * 40 : u() * size;
        };
        n.lockX = coord(n.width);
        n.lockY = coord(n.height);
        g.m_lockPosition.SetXY(n.lockX, n.lockY);
        n.lockValid = true;

        for (int i = 0; i < 8; i++)
        {
            n.trace.clear();
            legacy_sm::Twin::trace.clear();
            bool argValid = u() < 0.98;
            double scale = u() < 0.15 ? 3.0 * std::max(n.width, n.height) : u() < 0.5 ? 60 : 15;
            double dx = (u() - 0.5) * scale, dy = (u() - 0.5) * scale;
            if (u() < 0.05)
                dx = dy = 0;
            else if (u() < 0.05)
                dy = 0;

            bool errNew = n.Move(argValid, dx, dy);
            bool errOld = g.MoveLockPosition(argValid ? legacy_sm::PHD_Point(dx, dy) : legacy_sm::PHD_Point());
            std::string what = "seed " + std::to_string(seed) + " move " + std::to_string(i);
            ASSERT_EQ(errNew, errOld) << what;
            ASSERT_EQ(n.trace, legacy_sm::Twin::trace) << what;
            ASSERT_EQ(n.lockValid, g.m_lockPosition.IsValid()) << what;
            ASSERT_TRUE(Same(n.lockX, g.m_lockPosition.X) && Same(n.lockY, g.m_lockPosition.Y)) << what;
            ASSERT_EQ(n.recenterActive, g.m_ditherRecenterRemaining.IsValid()) << what;
            if (n.recenterActive)
            {
                ASSERT_TRUE(Same(n.remX, g.m_ditherRecenterRemaining.X) && Same(n.remY, g.m_ditherRecenterRemaining.Y)) << what;
                ASSERT_TRUE(Same(n.stepX, g.m_ditherRecenterStep.X) && Same(n.stepY, g.m_ditherRecenterStep.Y)) << what;
                ASSERT_EQ(n.dirX, g.m_ditherRecenterDir.x) << what;
                ASSERT_EQ(n.dirY, g.m_ditherRecenterDir.y) << what;
            }
            ASSERT_EQ(n.pending, g.m_ditherRecenterStepsPending) << what;
            ASSERT_EQ(n.stalled, g.m_ditherRecenterStalledSteps) << what;
            ASSERT_EQ(n.forWorker, g.m_recenteringForWorker) << what;
            moves++;

            int variations = static_cast<int>(
                std::count(n.trace.begin(), n.trace.end(), "D:dither produces an invalid lock position, try a variation\n"));
            seen["variations " + std::to_string(variations)]++;
            for (const std::string& line : n.trace)
                if (line.rfind("D:E:", 0) == 0)
                    seen[line]++;
            if (!errNew && dx == 0 && dy == 0)
                seen["zero length"]++;
            if (!errNew && dx == 0 && dy == 0 && n.fastRecenter)
                seen["zero length, fast recenter on"]++;
            // a recenter ends only when the guider's frames end it; start each move without one
            n.recenterActive = false;
            g.m_ditherRecenterRemaining.Invalidate();
        }
    }
    EXPECT_GT(moves, 100000);
    for (const char *key : { "variations 0", "variations 1", "variations 2", "variations 3", "variations 4",
                             "D:E:Point is not valid", "D:E:No mount", "D:E:Transform failed", "D:E:SetLockPosition failed",
                             "zero length", "zero length, fast recenter on" })
        EXPECT_GT(seen[key], 0) << key << " never reached";
}
