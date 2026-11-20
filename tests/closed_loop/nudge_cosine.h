// A South nudge whose direction cosine rounds past -1 or +1 (#154).
//
// The nudge checks that it goes back down the North leg through the angle
// between two unit vectors: the North leg's, from the lock position to where
// the South leg started, and the nudge's, from the lock position to the star.
// In a noiseless run the two are exactly antiparallel, so their cosine is -1
// in exact arithmetic, and rounding the direction cosines and their products
// can put it a hair past -1, where acos gives NaN. Which way it rounds depends
// on the positions and on whether the compiler fuses a multiply and an add,
// so a geometry the sim reaches by pulsing is not deterministic.
//
// This one is. The star is put at exact positions along (115, 252), a
// Pythagorean triple's legs (hypotenuse 277), scaled by powers of 2: the
// differences and hypot of the positions are exact, every direction cosine is
// 115/277 or 252/277 rounded, and the cosine of the two directions is
// -1.0000000000000002 (+1.0000000000000002 for parallel vectors) whether the
// dot product is computed as two rounded products and a sum or with either
// product fused into the add. On the frame the North leg ends the star is
// moved to 277/8 = 34.625 px up the North line from the lock (the South leg
// then starts there), and on the frame the nudge starts it is moved to
// 277/64 = 4.328125 px from the lock: north of it (antiparallel, the nudge is
// needed) or, with pastLock, south of it (parallel, the star went past).

#ifndef CLOSED_LOOP_NUDGE_COSINE_H
#define CLOSED_LOOP_NUDGE_COSINE_H

#include "harness.h"
#include "sim_mount.h"

#include <cmath>
#include <string>
#include <utility>

namespace closed_loop
{
namespace nudge_cosine
{

    constexpr double kDirX = 115.;
    constexpr double kDirY = 252.;
    constexpr double kLength = 277.;
    constexpr double kSouthStartScale = 1. / 8.; // 34.625 px from the lock
    constexpr double kNudgeScale = 1. / 64.; // 4.328125 px from the lock

    // A noiseless mount whose North pulse moves the star along (115, 252)
    inline SimMountConfig Mount()
    {
        SimMountConfig c;
        c.westAngleDeg = Degrees(std::atan2(kDirY, kDirX)) + 90.;
        return c;
    }

    // Where the South leg starts: up the North line from the lock
    inline Pt SouthStart(const SimMountConfig& c)
    {
        return Pt { c.startX + kDirX * kSouthStartScale, c.startY + kDirY * kSouthStartScale, true };
    }

    // Where the nudge sees the star: north of the lock, or south of it with pastLock
    inline Pt NudgeStar(const SimMountConfig& c, bool pastLock)
    {
        double s = pastLock ? -kNudgeScale : kNudgeScale;
        return Pt { c.startX + kDirX * s, c.startY + kDirY * s, true };
    }

    inline int FirstDebugFrame(const CalOutcome& o, const std::string& fragment)
    {
        for (const Event& e : o.events)
            if (e.kind == Event::Debug && e.text.find(fragment) != std::string::npos)
                return e.frame;
        return -1;
    }

    // Moves the star to SouthStart before southStartFrame and to NudgeStar before nudgeFrame
    template<class Run>
    FrameHook<Run> Placing(const SimMountConfig& c, int southStartFrame, int nudgeFrame, bool pastLock)
    {
        Pt south = SouthStart(c);
        Pt nudge = NudgeStar(c, pastLock);
        return [=](int frame, Settings&, SimMount& sim, Run&)
        {
            if (frame == southStartFrame)
                sim.MoveTo(south.x, south.y);
            if (frame == nudgeFrame)
                sim.MoveTo(nudge.x, nudge.y);
        };
    }

    // The frame the North leg ends on, and the frame the nudge starts on once
    // the South leg started at SouthStart; -1 when a run does not get there
    template<class Run>
    std::pair<int, int> Frames(const SimMountConfig& c, const Settings& s)
    {
        SimMount first(c);
        int southStart = FirstDebugFrame(RunCalibration<Run>(first, s), "Falling Through to state GO_SOUTH");
        if (southStart < 0)
            return { -1, -1 };
        SimMount second(c);
        CalOutcome o = RunCalibration<Run>(second, s, 2000, Placing<Run>(c, southStart, -1, false));
        return { southStart, FirstDebugFrame(o, "Nudge: theta") };
    }

} // namespace nudge_cosine
} // namespace closed_loop

#endif // CLOSED_LOOP_NUDGE_COSINE_H
