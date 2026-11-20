// How far beyond the search region auto-select keeps a star from the frame edge
// (open-astro/openastro-guider#140).
//
// The calibration walk moves the star by the mount's calibration distance (an AO's by 25 px), so
// a star selected for a calibration has to be that much farther from the edge. guide with
// recalibrate: true selects its star (STATE_SELECT_STAR) before STATE_CALIBRATE clears the
// previous target's calibration; until #140 the allowance looked only at whether the mount was
// calibrated at selection time, so it was 0 and the star could be as close as the search region.

#include <gtest/gtest.h>

#include <algorithm>

#include "edge_allowance.h"

namespace
{
constexpr int kScopeDist = 25; // Scope::CalibrationTotDistance, the profile's calibration distance
constexpr int kAoDist = 25; // StepGuider::CalibrationTotDistance

// GuiderMultiStar::AutoSelect: the largest allowance over the connected mount and AO
int AutoSelectAllowance(bool scopeCalibrated, int scopeDist, bool aoCalibrated, bool willForceRecal)
{
    return std::max(EdgeAllowance(aoCalibrated, willForceRecal, kAoDist),
                    EdgeAllowance(scopeCalibrated, willForceRecal, scopeDist));
}
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
    EXPECT_EQ(AutoSelectAllowance(true, 40, true, true), 40);
    EXPECT_EQ(AutoSelectAllowance(true, 10, true, true), kAoDist);
    // no recalibration: only the uncalibrated one counts
    EXPECT_EQ(AutoSelectAllowance(true, 40, false, false), kAoDist);
    EXPECT_EQ(AutoSelectAllowance(false, 40, true, false), 40);
    EXPECT_EQ(AutoSelectAllowance(true, 40, true, false), 0);
}
