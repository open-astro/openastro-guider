// Unit tests for src/staticpa_step_gate.cpp — the gate that keeps Static PA
// from star-tracking frames exposed while one of its own auto-mode RA steps
// is still moving (#98). The production translation unit is std-only and
// links here unmodified.
//
// Times are milliseconds on the steady clock. The #98 timeline test replays
// the log in the issue (slew at 06.814, two frames exposed during the slew,
// the second of which aborted the run).

#include <gtest/gtest.h>

#include "staticpa_step_gate.h"

using namespace staticpa_step;

TEST(StaticPaStepGate, SettleConstantIsZero)
{
    // endSeen is already an upper bound on the slew end; any settle above
    // zero would also drop the frame that starts right after it.
    EXPECT_EQ(kSettleMs, 0);
}

TEST(StaticPaStepGate, IdleTracksEveryFrameThatIsNotSlewing)
{
    StepGate gate;
    EXPECT_EQ(gate.phase(), Phase::Idle);
    EXPECT_FALSE(gate.IgnoreFrame(false, 1000, 2000));
    EXPECT_FALSE(gate.IgnoreFrame(false, 0, 0));
    EXPECT_FALSE(gate.IgnoreFrame(false, 5000, 4000)); // start after now: still tracked
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepGate, IdleFrameReadingSlewingArmsAndIsIgnored)
{
    // e.g. a run restarted while the mount is still braking from AbortSlew,
    // or a mount that is seen slewing on a frame the gate would release
    StepGate gate;
    EXPECT_TRUE(gate.IgnoreFrame(true, 1000, 2000));
    EXPECT_EQ(gate.phase(), Phase::Slewing);
}

TEST(StaticPaStepGate, ArmGoesToSlewing)
{
    StepGate gate;
    gate.Arm();
    EXPECT_EQ(gate.phase(), Phase::Slewing);
}

TEST(StaticPaStepGate, IgnoresEveryFrameWhileMountReportsSlewing)
{
    StepGate gate;
    gate.Arm();
    for (int i = 0; i < 5; i++)
    {
        std::int64_t start = 1000 + i * 1700;
        EXPECT_TRUE(gate.IgnoreFrame(true, start, start + 1100));
        EXPECT_EQ(gate.phase(), Phase::Slewing);
    }
}

TEST(StaticPaStepGate, FrameProcessedWhenSlewEndIsSeenIsIgnored)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(true, 0, 1000));
    // this frame's exposure began before the end of the slew was seen
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_EQ(gate.phase(), Phase::EndSeen);
    EXPECT_EQ(gate.EndSeenMs(), 2100);
}

TEST(StaticPaStepGate, FirstFrameStartedAtOrAfterEndSeenIsTrackedAndGateGoesIdle)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    // exact boundary: starts at endSeen + settle (settle 0)
    EXPECT_FALSE(gate.IgnoreFrame(false, 2100, 3200));
    EXPECT_EQ(gate.phase(), Phase::Idle);
    EXPECT_FALSE(gate.IgnoreFrame(false, 3205, 4300));
}

TEST(StaticPaStepGate, ClockSlackIsSmall)
{
    // absorbs ms truncation of two clocks; must stay far below any exposure
    // plus download
    EXPECT_GT(kClockSlackMs, 0);
    EXPECT_LE(kClockSlackMs, 10);
}

TEST(StaticPaStepGate, TransitionFrameIsIgnoredEvenWhenItsStartMapsToNow)
{
    // the frame on which the end is first seen was exposed before it was
    // processed; a start that maps to "now" (an invalid ImgStartTime, or a
    // clamped age) must not release it
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 2100, 2100));
    EXPECT_EQ(gate.phase(), Phase::EndSeen);
    EXPECT_EQ(gate.EndSeenMs(), 2100);
    EXPECT_FALSE(gate.IgnoreFrame(false, 2101, 3200));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepGate, TransitionFrameIsIgnoredEvenWhenItsStartIsAfterNow)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 2500, 2100));
    EXPECT_EQ(gate.phase(), Phase::EndSeen);
}

TEST(StaticPaStepGate, FrameStartedOneMsBeforeEndSeenThroughTheMappingIsTracked)
{
    // the next frame starts well under 1 ms after the end-seen frame was
    // processed; truncating the mapped start can put it a ms or two before
    // endSeen, and that must not cost a second extra frame
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_FALSE(gate.IgnoreFrame(false, 2099, 3200));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepGate, FrameStartedJustInsideTheSlackIsTracked)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_FALSE(gate.IgnoreFrame(false, 2100 - kClockSlackMs, 3200));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepGate, FrameStartedBeyondTheSlackBeforeEndSeenIsIgnored)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_TRUE(gate.IgnoreFrame(false, 2100 - kClockSlackMs - 1, 3200));
    EXPECT_EQ(gate.phase(), Phase::EndSeen);
    EXPECT_FALSE(gate.IgnoreFrame(false, 3204, 4300));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepGate, FrameWithUnknownStartIsIgnoredAndTheNextFrameIsTracked)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_TRUE(gate.IgnoreFrame(false, kUnknownFrameStart, 3200));
    EXPECT_EQ(gate.phase(), Phase::EndSeen);
    EXPECT_FALSE(gate.IgnoreFrame(false, 3204, 4300));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepGate, SettleIsAddedToEndSeen)
{
    StepGate gate(1000);
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_TRUE(gate.IgnoreFrame(false, 2105, 3200)); // inside the settle
    EXPECT_FALSE(gate.IgnoreFrame(false, 3100, 4200)); // exactly endSeen + settle
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepGate, ReleaseFrameReadingSlewingRearms)
{
    // the mount bounced back to slewing (or a fail-open read flipped) after the
    // end was seen: the frame is ignored and a new end must be seen
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_TRUE(gate.IgnoreFrame(true, 2105, 3200));
    EXPECT_EQ(gate.phase(), Phase::Slewing);
    EXPECT_TRUE(gate.IgnoreFrame(false, 3205, 4300)); // started before the new end
    EXPECT_EQ(gate.EndSeenMs(), 4300);
    EXPECT_FALSE(gate.IgnoreFrame(false, 4305, 5400));
}

TEST(StaticPaStepGate, MountThatReportsSlewingLateStillCostsOneFrame)
{
    // MoveWestBy armed the gate, but the mount does not report the slew on the
    // next frame (or cannot report it at all): that frame started before the
    // "end" was seen and is ignored; the frame after it is tracked
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    EXPECT_FALSE(gate.IgnoreFrame(false, 2105, 3200));
}

TEST(StaticPaStepGate, ResetReleasesImmediately)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(true, 0, 1000));
    gate.Reset();
    EXPECT_EQ(gate.phase(), Phase::Idle);
    EXPECT_FALSE(gate.IgnoreFrame(false, 0, 2000));
}

TEST(StaticPaStepGate, ArmAfterEndSeenWaitsForANewEnd)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 2100));
    gate.Arm();
    EXPECT_EQ(gate.phase(), Phase::Slewing);
    EXPECT_TRUE(gate.IgnoreFrame(false, 2105, 3200)); // new end seen at 3200
    EXPECT_FALSE(gate.IgnoreFrame(false, 3205, 4300));
}

TEST(StaticPaStepGate, IssueTimelineFromLog)
{
    // #98 log, ms after 04:58:00. Each frame is processed ~1.7 s after it
    // started (1 s exposure plus download).
    StepGate gate;
    // frame N: tracked; RotateMount issues the step slew at 06.814
    EXPECT_FALSE(gate.IgnoreFrame(false, 5100, 6800));
    gate.Arm();
    // frame N+1: startexposure 07.185, processed 08.899 while slewing (the
    // smeared Mass=5164 frame that moved the star)
    EXPECT_TRUE(gate.IgnoreFrame(true, 7185, 8899));
    // frame N+2: startexposure 08.904, processed 09.973 still slewing (the frame
    // whose star loss aborted the run)
    EXPECT_TRUE(gate.IgnoreFrame(true, 8904, 9973));
    // frame N+3: starts 09.978, the mount has stopped when it is processed; its
    // exposure overlapped the end of the slew
    EXPECT_TRUE(gate.IgnoreFrame(false, 9978, 11400));
    // frame N+4: the first frame whose exposure started after the end was seen
    EXPECT_FALSE(gate.IgnoreFrame(false, 11404, 12800));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepClock, FrameStartIsMappedByItsAge)
{
    // wall: frame started 1500 ms before now; steady now is 10000
    EXPECT_EQ(FrameStartOnSteadyClock(1700000000000LL, 1700000001500LL, 10000), 8500);
}

TEST(StaticPaStepClock, FrameStartInTheFutureFailsClosed)
{
    // the wall clock stepped back between the frame start and now: the start
    // is unknown, and the gate must not release the frame
    EXPECT_EQ(FrameStartOnSteadyClock(1700000001500LL, 1700000000000LL, 10000), kUnknownFrameStart);
    EXPECT_LT(kUnknownFrameStart + kClockSlackMs, 0);
}

TEST(StaticPaStepClock, WallClockSteppedBackAcrossTheEndSeenFrameIsSkipped)
{
    // a Pi without an RTC steps its clock back 2 s (NTP) while the frame
    // exposed across the end of the step downloads; that frame and, if its
    // start also maps into the future, the next are skipped, then the gate
    // releases
    StepGate gate;
    gate.Arm();
    std::int64_t wallNow = 1700000000000LL;
    std::int64_t steadyNow = 20000;
    EXPECT_TRUE(gate.IgnoreFrame(false, FrameStartOnSteadyClock(wallNow + 800, wallNow, steadyNow), steadyNow));
    EXPECT_EQ(gate.phase(), Phase::EndSeen);
    // next frame: 1 s exposure, started after the step, processed 1.1 s later
    EXPECT_FALSE(
        gate.IgnoreFrame(false, FrameStartOnSteadyClock(wallNow + 5, wallNow + 1105, steadyNow + 1105), steadyNow + 1105));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepClock, FrameStartInTheFutureWhileEndSeenIsIgnored)
{
    StepGate gate;
    gate.Arm();
    EXPECT_TRUE(gate.IgnoreFrame(false, 1000, 20000));
    std::int64_t wallNow = 1700000000000LL;
    EXPECT_TRUE(gate.IgnoreFrame(false, FrameStartOnSteadyClock(wallNow + 300, wallNow, 21100), 21100));
    EXPECT_EQ(gate.phase(), Phase::EndSeen);
    EXPECT_FALSE(gate.IgnoreFrame(false, FrameStartOnSteadyClock(wallNow + 10, wallNow + 1110, 22210), 22210));
    EXPECT_EQ(gate.phase(), Phase::Idle);
}

TEST(StaticPaStepClock, WallClockSteppedBackDoesNotStallTheGate)
{
    StepGate gate;
    gate.Arm();
    std::int64_t endSeenSteady = 20000;
    EXPECT_TRUE(gate.IgnoreFrame(false, FrameStartOnSteadyClock(5000, 6700, endSeenSteady), endSeenSteady));
    // the wall clock stepped back an hour before the next frame was processed;
    // the frame started 1.1 s before it was processed
    std::int64_t wallNow = 6700 - 3600000 + 1700;
    std::int64_t frameStartWall = wallNow - 1100;
    EXPECT_FALSE(gate.IgnoreFrame(false, FrameStartOnSteadyClock(frameStartWall, wallNow, 21700), 21700));
}
