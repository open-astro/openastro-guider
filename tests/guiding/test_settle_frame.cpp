// What one guide frame does to a guide or dither settle (open-astro/openastro-guider#136).
//
// PhdController's STATE_SETTLE_WAIT counts each frame, times the run of in-range frames and
// decides whether the settle is done, timed out or goes on. Since #136 a frame whose capture
// failed with a transient error while guiding is dropped instead of stopping guiding; the settle
// counts it like a lost-star frame, keeps its timeout running, and neither ends nor completes
// the in-range run it falls in.

#include <gtest/gtest.h>

#include "settle_frame.h"

using SettleFrame::Frame;
using SettleFrame::Outcome;
using SettleFrame::Progress;
using SettleFrame::Result;

namespace
{
// 1.5 px for 10 s within 60 s, no frame limit to speak of
SettleParams Params(int frames = 99)
{
    SettleParams p;
    p.tolerancePx = 1.5;
    p.settleTimeSec = 10;
    p.timeoutSec = 60;
    p.frames = frames;
    return p;
}

Frame InRange(long inRangeMs, long settlingMs)
{
    Frame f;
    f.locked = true;
    f.error = 0.5;
    f.inRangeMs = inRangeMs;
    f.settlingMs = settlingMs;
    return f;
}

Frame OutOfRange(long inRangeMs, long settlingMs)
{
    Frame f = InRange(inRangeMs, settlingMs);
    f.error = 3.;
    return f;
}

Frame LostStar(long inRangeMs, long settlingMs)
{
    Frame f = InRange(inRangeMs, settlingMs);
    f.locked = false;
    return f;
}

// #136: the capture failed. The guider is still locked (the lock is untouched), and the guide
// error is the last frame's.
Frame Dropped(long inRangeMs, long settlingMs)
{
    Frame f = InRange(inRangeMs, settlingMs);
    f.dropped = true;
    return f;
}
} // namespace

TEST(SettleFrame, AnInRangeRunCompletesAfterTheSettleTime)
{
    Progress p;
    Result r = SettleFrame::OnFrame(p, Params(), InRange(0, 2000));
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_TRUE(r.startInRangeRun);
    EXPECT_TRUE(r.lockedOnStar);
    EXPECT_EQ(r.timeInRangeMs, 0);
    EXPECT_TRUE(p.priorFrameInRange);

    r = SettleFrame::OnFrame(p, Params(), InRange(5000, 7000));
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_FALSE(r.startInRangeRun);
    EXPECT_EQ(r.timeInRangeMs, 5000);

    r = SettleFrame::OnFrame(p, Params(), InRange(10000, 12000));
    EXPECT_EQ(r.outcome, Outcome::Settled);
    EXPECT_EQ(p.frameCount, 3);
    EXPECT_EQ(p.droppedFrameCount, 0);
}

TEST(SettleFrame, NoSettleTimeSettlesOnTheFirstInRangeFrame)
{
    SettleParams params = Params();
    params.settleTimeSec = 0;
    Progress p;
    EXPECT_EQ(SettleFrame::OnFrame(p, params, OutOfRange(0, 1000)).outcome, Outcome::Settling);
    EXPECT_EQ(SettleFrame::OnFrame(p, params, InRange(0, 2000)).outcome, Outcome::Settled);
}

TEST(SettleFrame, AnAoBumpHoldsTheSettle)
{
    Progress p;
    SettleFrame::OnFrame(p, Params(), InRange(0, 1000));
    Frame f = InRange(10000, 11000);
    f.aoBumpInProgress = true;
    EXPECT_EQ(SettleFrame::OnFrame(p, Params(), f).outcome, Outcome::Settling);
    EXPECT_EQ(SettleFrame::OnFrame(p, Params(), InRange(12000, 13000)).outcome, Outcome::Settled);
}

TEST(SettleFrame, AnOutOfRangeOrLostStarFrameEndsTheRun)
{
    Progress p;
    SettleFrame::OnFrame(p, Params(), InRange(0, 1000));
    Result r = SettleFrame::OnFrame(p, Params(), OutOfRange(4000, 5000));
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_TRUE(r.lockedOnStar);
    EXPECT_FALSE(p.priorFrameInRange);
    EXPECT_EQ(p.droppedFrameCount, 0);

    // the next in-range frame starts a new run
    r = SettleFrame::OnFrame(p, Params(), InRange(9000, 6000));
    EXPECT_TRUE(r.startInRangeRun);
    EXPECT_EQ(r.outcome, Outcome::Settling);

    r = SettleFrame::OnFrame(p, Params(), LostStar(4000, 7000));
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_FALSE(r.lockedOnStar);
    EXPECT_EQ(r.timeInRangeMs, 0);
    EXPECT_FALSE(p.priorFrameInRange);
    EXPECT_EQ(p.frameCount, 4);
    EXPECT_EQ(p.droppedFrameCount, 1);
}

TEST(SettleFrame, ADroppedFrameMidRunKeepsTheRun)
{
    Progress p;
    SettleFrame::OnFrame(p, Params(), InRange(0, 1000));
    Result r = SettleFrame::OnFrame(p, Params(), Dropped(4000, 5000));
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_FALSE(r.lockedOnStar); // even though the guider is still locked
    EXPECT_FALSE(r.startInRangeRun);
    EXPECT_EQ(r.timeInRangeMs, 4000); // the run goes on
    EXPECT_TRUE(p.priorFrameInRange);
    EXPECT_EQ(p.frameCount, 2);
    EXPECT_EQ(p.droppedFrameCount, 1);

    // the next in-range frame times the run from its first frame: no new run
    r = SettleFrame::OnFrame(p, Params(), InRange(10000, 11000));
    EXPECT_FALSE(r.startInRangeRun);
    EXPECT_EQ(r.outcome, Outcome::Settled);
    EXPECT_EQ(p.frameCount, 3);
    EXPECT_EQ(p.droppedFrameCount, 1);
}

TEST(SettleFrame, ADroppedFrameCannotCompleteTheRun)
{
    Progress p;
    SettleFrame::OnFrame(p, Params(), InRange(0, 1000));
    // the settle time has passed, but the dropped frame saw no star
    Result r = SettleFrame::OnFrame(p, Params(), Dropped(20000, 21000));
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_EQ(r.timeInRangeMs, 20000);
    EXPECT_TRUE(p.priorFrameInRange);
    // two dropped frames in a row neither
    EXPECT_EQ(SettleFrame::OnFrame(p, Params(), Dropped(22000, 23000)).outcome, Outcome::Settling);
    EXPECT_TRUE(p.priorFrameInRange);
    // the next good in-range frame completes it
    EXPECT_EQ(SettleFrame::OnFrame(p, Params(), InRange(24000, 25000)).outcome, Outcome::Settled);
}

TEST(SettleFrame, ADroppedFrameOutsideARunStartsNone)
{
    Progress p;
    Result r = SettleFrame::OnFrame(p, Params(), Dropped(0, 1000));
    EXPECT_EQ(r.outcome, Outcome::Settling);
    EXPECT_FALSE(r.startInRangeRun);
    EXPECT_EQ(r.timeInRangeMs, 0);
    EXPECT_FALSE(p.priorFrameInRange);
    EXPECT_EQ(p.droppedFrameCount, 1);

    // after an out-of-range frame: still no run
    SettleFrame::OnFrame(p, Params(), OutOfRange(0, 2000));
    r = SettleFrame::OnFrame(p, Params(), Dropped(0, 3000));
    EXPECT_FALSE(p.priorFrameInRange);
    EXPECT_EQ(r.timeInRangeMs, 0);
    EXPECT_TRUE(SettleFrame::OnFrame(p, Params(), InRange(0, 4000)).startInRangeRun);
}

TEST(SettleFrame, TheTimeoutIsCheckedOnADroppedFrame)
{
    Progress p;
    SettleFrame::OnFrame(p, Params(), InRange(0, 50000));
    Result r = SettleFrame::OnFrame(p, Params(), Dropped(5000, 60000));
    EXPECT_EQ(r.outcome, Outcome::TimedOut);
    EXPECT_EQ(p.frameCount, 2);
    EXPECT_EQ(p.droppedFrameCount, 1);

    // just short of the timeout it goes on
    Progress q;
    EXPECT_EQ(SettleFrame::OnFrame(q, Params(), Dropped(0, 59999)).outcome, Outcome::Settling);
}

TEST(SettleFrame, ADroppedFrameReachesTheFrameLimit)
{
    // a frame-count settle (dither with settle frames) ends on its last frame, dropped or not
    SettleParams params = Params(3);
    Progress p;
    EXPECT_EQ(SettleFrame::OnFrame(p, params, OutOfRange(0, 1000)).outcome, Outcome::Settling);
    EXPECT_EQ(SettleFrame::OnFrame(p, params, Dropped(0, 2000)).outcome, Outcome::Settling);
    // the frame limit wins over the timeout
    EXPECT_EQ(SettleFrame::OnFrame(p, params, Dropped(0, 90000)).outcome, Outcome::Settled);
    EXPECT_EQ(p.frameCount, 3);
    EXPECT_EQ(p.droppedFrameCount, 2);
}
