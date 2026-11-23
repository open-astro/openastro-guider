// AlgorithmSlot, the guard between a mount's guide computation and the changes to it (#142 T2b).
//
// Built with -fsanitize=thread (tests/CMakeLists.txt), so a data race in the pattern below fails
// the test with a ThreadSanitizer report. The pattern is Mount's (src/mount.cpp): a "worker"
// thread runs MoveOffset's computation under the guard (the algorithms' result() and the BLC's
// tracking), then a MoveAxis outside it; the "main" thread does what set_algo (delete one
// algorithm, create another), set_algo_param, guiding_assistant_apply and set_backlash_comp do,
// under the guard. mount.cpp itself needs wx and cannot be built here, so this pins the guard and
// the way Mount holds it, not Mount's own lines.

#include <gtest/gtest.h>

#include "algorithm_slot.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
# define PHD_TSAN 1
#elif defined(__has_feature)
# if __has_feature(thread_sanitizer)
#  define PHD_TSAN 1
# endif
#endif

namespace
{
// A stand-in for a GuideAlgorithm with state a result() call reads and writes (a history, like
// Lowpass2's or the GP's), parameters the main thread sets, and a value result() changes that
// the main thread reads back (the GP's period length, estimated as it guides).
struct FakeAlgo
{
    std::vector<double> history;
    double minMove = 0.2;
    double aggression = 0.7;
    double period = 480.0;

    double result(double input)
    {
        history.push_back(input);
        if (history.size() > 64)
            history.erase(history.begin());
        double mean = std::accumulate(history.begin(), history.end(), 0.0) / history.size();
        period = 0.999 * period + 0.001 * (400.0 + mean);
        double out = aggression * (input - 0.1 * mean);
        return std::abs(out) < minMove ? 0.0 : out;
    }
};

// A stand-in for the BLC: a pulse width the worker adjusts and the main thread sets
struct FakeBlc
{
    int pulseWidth = 300;
    std::vector<double> deflections;
    void Track(double miss)
    {
        deflections.push_back(miss);
        if (deflections.size() > 10)
            deflections.clear();
        pulseWidth += miss > 0 ? 1 : -1;
    }
};

struct FakeMount
{
    AlgorithmSlot slot;
    FakeAlgo *x = new FakeAlgo;
    FakeAlgo *y = new FakeAlgo;
    FakeBlc blc;

    ~FakeMount()
    {
        delete x;
        delete y;
    }

    // Mount::MoveOffset: the computation under the guard, then the pulses outside it
    template<typename MoveAxis>
    double MoveOffset(double xDist, double yDist, MoveAxis moveAxis)
    {
        double xOut, yOut;
        {
            AlgorithmSlot::Hold held = slot.Take();
            blc.Track(yDist);
            xOut = x ? x->result(xDist) : xDist;
            yOut = y ? y->result(yDist) : yDist;
        }
        moveAxis(xOut);
        moveAxis(yOut);
        return xOut + yOut;
    }

    // Mount::SetXGuideAlgorithm
    void SwapX()
    {
        AlgorithmSlot::Hold held = slot.Take();
        delete x;
        x = new FakeAlgo;
    }
};

double Input(int i)
{
    return ((i * 7919) % 200 - 100) / 50.0;
}
} // namespace

TEST(AlgorithmSlot, BuiltWithThreadSanitizer)
{
#if defined(PHD_EXPECT_TSAN) && !defined(PHD_TSAN)
    FAIL() << "test_algorithm_slot is meant to be built with -fsanitize=thread";
#elif !defined(PHD_TSAN)
    GTEST_SKIP() << "not a ThreadSanitizer build: the race tests below run but cannot see a race";
#endif
}

// set_algo while guiding: the algorithm a move is using is never freed under it
TEST(AlgorithmSlot, SwapRacesTheComputation)
{
    FakeMount mount;
    constexpr int MOVES = 3000;
    std::atomic<bool> done { false };
    int moves = 0;

    std::thread worker(
        [&]
        {
            for (int i = 0; i < MOVES; i++)
            {
                mount.MoveOffset(Input(i), Input(i + 1), [](double) { });
                moves++;
            }
            done = true;
        });

    int swaps = 0;
    while (!done)
    {
        mount.SwapX();
        AlgorithmSlot::Hold held = mount.slot.Take(); // Mount::SetYGuideAlgorithm, the Dec swap
        delete mount.y;
        mount.y = new FakeAlgo;
        swaps++;
    }
    worker.join();

    EXPECT_EQ(moves, MOVES);
    EXPECT_GT(swaps, 0);
}

// set_algo_param, guiding_assistant_apply's min-move and Lowpass2 settings, set_backlash_comp,
// get_algo_param of a value the computation changes: all against the computation
TEST(AlgorithmSlot, ParamSetsAndReadsRaceTheComputation)
{
    FakeMount mount;
    constexpr int MOVES = 3000;
    std::atomic<bool> done { false };

    std::thread worker(
        [&]
        {
            for (int i = 0; i < MOVES; i++)
                mount.MoveOffset(Input(i), Input(i + 3), [](double) { });
            done = true;
        });

    int sets = 0;
    double periodSeen = 0.0;
    while (!done)
    {
        AlgorithmSlot::Hold held = mount.slot.Take();
        mount.x->minMove = 0.1 + (sets % 5) * 0.05; // set_algo_param minMove
        mount.x->period = 480.0; // set_algo_param periodLength
        mount.y->aggression = 0.8; // guiding_assistant_apply: Lowpass2 aggressiveness
        mount.blc.pulseWidth = 250 + sets % 50; // set_backlash_comp
        mount.blc.deflections.clear(); // SetBacklashPulseWidth's ClearHistory
        periodSeen = mount.x->period; // get_algo_param periodLength
        sets++;
    }
    worker.join();

    EXPECT_GT(sets, 0);
    EXPECT_GT(periodSeen, 0.0);
    EXPECT_GE(mount.x->minMove, 0.1);
}

// The guard is not held across MoveAxis: a mount stalled in a pulse does not keep the main thread
// (and RPC) from changing the algorithms
TEST(AlgorithmSlot, StalledMoveDoesNotHoldTheGuard)
{
    FakeMount mount;
    std::promise<void> inMove, release;
    std::shared_future<void> released = release.get_future().share();
    bool first = true;

    std::thread worker(
        [&]
        {
            mount.MoveOffset(1.0, -1.0,
                             [&](double)
                             {
                                 if (first)
                                 {
                                     first = false;
                                     inMove.set_value();
                                     released.wait(); // the mount does not answer
                                 }
                             });
        });

    inMove.get_future().wait();
    auto change = std::async(std::launch::async,
                             [&]
                             {
                                 mount.SwapX();
                                 AlgorithmSlot::Hold held = mount.slot.Take();
                                 mount.y->minMove = 0.3;
                             });
    EXPECT_EQ(change.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "the main thread waited on the guard while the worker was in MoveAxis";
    release.set_value();
    worker.join();
    change.wait();
}

// A move waits for a change in progress and then uses the new algorithm, not the freed one
TEST(AlgorithmSlot, MoveWaitsForAChangeInProgress)
{
    FakeMount mount;
    std::promise<void> holding;
    std::atomic<bool> moved { false };
    FakeAlgo *replacement = nullptr;

    AlgorithmSlot::Hold held = mount.slot.Take();
    std::thread worker(
        [&]
        {
            holding.get_future().wait();
            mount.MoveOffset(2.0, 2.0, [](double) { });
            moved = true;
        });
    holding.set_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(moved) << "the move ran while the main thread held the guard";
    delete mount.x;
    mount.x = replacement = new FakeAlgo;
    held.unlock();
    worker.join();

    EXPECT_TRUE(moved);
    AlgorithmSlot::Hold after = mount.slot.Take();
    ASSERT_EQ(mount.x, replacement);
    EXPECT_EQ(replacement->history.size(), 1u);
}
