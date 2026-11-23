// How far beyond the search region auto-select keeps a star from the frame edge
// (open-astro/openastro-guider#140).
//
// The calibration walk moves the star by the mount's calibration distance (an AO's by 25 px), so
// a star selected for a calibration has to be that much farther from the edge. guide with
// recalibrate: true selects its star (STATE_SELECT_STAR) before STATE_CALIBRATE clears the
// previous target's calibration; until #140 the allowance looked only at whether the mount was
// calibrated at selection time, so it was 0 and the star could be as close as the search region.

#include <gtest/gtest.h>

#include "edge_allowance.h"

namespace
{
constexpr int kScopeDist = 25; // Scope::CalibrationTotDistance, the profile's calibration distance
constexpr int kAoDist = 25; // StepGuider::CalibrationTotDistance

CalibrationGear Gear(bool calibrated, int calTotDist)
{
    CalibrationGear gear;
    gear.connected = true;
    gear.calibrated = calibrated;
    gear.calTotDist = calTotDist;
    return gear;
}

const CalibrationGear kNoAo; // pSecondaryMount null
} // namespace

TEST(EdgeAllowance, ForcedRecalibrationOfACalibratedMountKeepsTheCalibrationDistance)
{
    // #140: the previous target's calibration is still loaded when the star is selected
    EXPECT_EQ(EdgeAllowance(true, true, kScopeDist), kScopeDist);
}

TEST(EdgeAllowance, UncalibratedMountKeepsTheCalibrationDistance)
{
    EXPECT_EQ(EdgeAllowance(false, false, kScopeDist), kScopeDist);
    EXPECT_EQ(EdgeAllowance(false, true, kScopeDist), kScopeDist);
}

TEST(EdgeAllowance, CalibratedMountThatWillNotRecalibrateNeedsNoAllowance)
{
    // find_star, or guide without recalibrate on a calibrated mount: nothing moves the star
    EXPECT_EQ(EdgeAllowance(true, false, kScopeDist), 0);
}

TEST(EdgeAllowance, EveryCombination)
{
    // index: bit 0 mountCalibrated, bit 1 willForceRecal
    const int expected[4] = { kScopeDist, 0, kScopeDist, kScopeDist };
    for (int i = 0; i < 4; ++i)
    {
        bool calibrated = (i & 1) != 0;
        bool recal = (i & 2) != 0;
        EXPECT_EQ(EdgeAllowance(calibrated, recal, kScopeDist), expected[i])
            << "calibrated=" << calibrated << " willForceRecal=" << recal;
    }
}

TEST(EdgeAllowance, FollowsTheCalibrationDistance)
{
    EXPECT_EQ(EdgeAllowance(true, true, 40), 40);
    EXPECT_EQ(EdgeAllowance(false, false, 0), 0);
    EXPECT_EQ(EdgeAllowance(true, true, 0), 0);
}

TEST(EdgeAllowance, AoAndMountTakeTheLarger)
{
    // a forced recalibration clears both the AO and the mount, so both walks count
    EXPECT_EQ(EdgeAllowance(Gear(true, 40), Gear(true, kAoDist), true), 40);
    EXPECT_EQ(EdgeAllowance(Gear(true, 10), Gear(true, kAoDist), true), kAoDist);
    // no recalibration: only the uncalibrated one counts
    EXPECT_EQ(EdgeAllowance(Gear(true, 40), Gear(false, kAoDist), false), kAoDist);
    EXPECT_EQ(EdgeAllowance(Gear(false, 40), Gear(true, kAoDist), false), 40);
    EXPECT_EQ(EdgeAllowance(Gear(true, 40), Gear(true, kAoDist), false), 0);
}

TEST(EdgeAllowance, MountWithoutAo)
{
    // #140: the previous target's calibration is still loaded when guide recalibrate: true selects its star
    EXPECT_EQ(EdgeAllowance(Gear(true, kScopeDist), kNoAo, true), kScopeDist);
    EXPECT_EQ(EdgeAllowance(Gear(false, kScopeDist), kNoAo, false), kScopeDist);
    EXPECT_EQ(EdgeAllowance(Gear(true, kScopeDist), kNoAo, false), 0);
}

TEST(EdgeAllowance, DisconnectedGearDoesNotCount)
{
    CalibrationGear mount = Gear(false, 40);
    mount.connected = false;
    EXPECT_EQ(EdgeAllowance(mount, kNoAo, true), 0);
    EXPECT_EQ(EdgeAllowance(mount, Gear(true, kAoDist), true), kAoDist);
}

TEST(ClearOfEdge, MatchesAutoFindsRule)
{
    // AutoFind drops a peak at x <= edgeDist or x + 1 + edgeDist >= width (and the same for y)
    EXPECT_FALSE(ClearOfEdge(40, 240, 640, 480, 40));
    EXPECT_TRUE(ClearOfEdge(41, 240, 640, 480, 40));
    EXPECT_FALSE(ClearOfEdge(600, 240, 640, 480, 40));
    EXPECT_FALSE(ClearOfEdge(599, 240, 640, 480, 40)); // kept until #142, an invalid lock position with no allowance
    EXPECT_TRUE(ClearOfEdge(598, 240, 640, 480, 40));
    EXPECT_FALSE(ClearOfEdge(320, 40, 640, 480, 40));
    EXPECT_FALSE(ClearOfEdge(320, 440, 640, 480, 40));
    EXPECT_FALSE(ClearOfEdge(320, 439, 640, 480, 40));
    EXPECT_TRUE(ClearOfEdge(320, 240, 640, 480, 40));
    EXPECT_TRUE(ClearOfEdge(320.5, 438.5, 640, 480, 40));
    EXPECT_FALSE(ClearOfEdge(320.5, 439.5, 640, 480, 40));
}

TEST(ClearOfEdge, StarSelectedByFindStarIsReselectedForARecalibration)
{
    // #140, Ara's GuiderFull meridian flip: find_star while looping on a calibrated mount keeps only the 15 px
    // search region, then guide recalibrate: true would walk that star 25 px off the frame
    const int kSearch = 15;
    EXPECT_TRUE(ClearOfEdge(20, 240, 640, 480, kSearch + EdgeAllowance(Gear(true, kScopeDist), kNoAo, false)));
    EXPECT_FALSE(StarClearForRecalibration(20, 240, 640, 480, kSearch, Gear(true, kScopeDist), kNoAo));
    EXPECT_FALSE(StarClearForRecalibration(620, 240, 640, 480, kSearch, Gear(true, kScopeDist), kNoAo));
    EXPECT_FALSE(StarClearForRecalibration(320, 30, 640, 480, kSearch, Gear(true, 10), Gear(true, kAoDist)));
    // far enough for the walk: kept, so a good star the client chose is not replaced
    EXPECT_TRUE(StarClearForRecalibration(41, 240, 640, 480, kSearch, Gear(true, kScopeDist), kNoAo));
    EXPECT_TRUE(StarClearForRecalibration(320, 240, 640, 480, kSearch, Gear(true, kScopeDist), Gear(true, kAoDist)));
}

TEST(ValidLockPosition, LeavesTheSearchRegionAndAPixelInsideTheFrame)
{
    // GuiderMultiStar::IsValidLockPosition, with a 15 px search region in 640 x 480
    EXPECT_FALSE(ValidLockPosition(15, 240, 640, 480, 15));
    EXPECT_TRUE(ValidLockPosition(16, 240, 640, 480, 15));
    EXPECT_TRUE(ValidLockPosition(623, 240, 640, 480, 15));
    EXPECT_FALSE(ValidLockPosition(624, 240, 640, 480, 15));
    EXPECT_FALSE(ValidLockPosition(320, 15, 640, 480, 15));
    EXPECT_TRUE(ValidLockPosition(320, 463, 640, 480, 15));
    EXPECT_FALSE(ValidLockPosition(320, 464, 640, 480, 15));
}

// With no edge allowance (a calibrated mount, find_star and get_star_centroids), AutoFind's edge
// rule kept a peak at x = width - searchRegion - 1 (or y = height - searchRegion - 1), which
// IsValidLockPosition rejects, so the star AutoSelect locked on already failed the guider's own
// lock-position check: with a lock-position shift on, any shift not inward raised
// LockPositionShiftLimitReached at once and turned the shift off. Every peak AutoFind keeps must be a
// valid lock position.
TEST(ClearOfEdge, KeepsOnlyValidLockPositions)
{
    for (int searchRegion : { 7, 15, 50 })
        for (int allowance : { 0, 25 })
        {
            const int width = 640, height = 480;
            int kept = 0;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    if (ClearOfEdge(x, y, width, height, searchRegion + allowance))
                    {
                        ++kept;
                        ASSERT_TRUE(ValidLockPosition(x, y, width, height, searchRegion))
                            << x << ", " << y << " search region " << searchRegion << " allowance " << allowance;
                    }
            EXPECT_GT(kept, 0) << searchRegion << " " << allowance;
        }
}

TEST(ClearOfEdge, KeepsEveryValidLockPositionWithNoAllowance)
{
    // and drops nothing more than it has to: with no allowance the two rules agree on every pixel
    const int width = 160, height = 120, searchRegion = 15;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            ASSERT_EQ(ClearOfEdge(x, y, width, height, searchRegion), ValidLockPosition(x, y, width, height, searchRegion))
                << x << ", " << y;
}
