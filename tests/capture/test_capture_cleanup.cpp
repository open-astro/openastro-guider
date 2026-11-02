// Pruning of auto-named saved frames (open-astro/openastro-guider#93).
//
// capture_single_frame {save:true} without a path, and save_image, write a
// new save_image_XXXXXX file per call. A remote client fetches it over
// GET /api/capture/<filename> but can't delete it, so a client polling one
// frame a second filled an ASIAIR's root filesystem overnight. The daemon
// now prunes old ones (capture_cleanup.cpp).

#include <gtest/gtest.h>

#include "capture_cleanup.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
class CaptureCleanup : public ::testing::Test
{
protected:
    fs::path dir;

    void SetUp() override
    {
        std::string tmpl = (fs::temp_directory_path() / "capture_cleanup_XXXXXX").string();
        ASSERT_NE(mkdtemp(tmpl.data()), nullptr);
        dir = tmpl;
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // a file whose mtime is ageSeconds in the past
    fs::path Make(const std::string& name, int ageSeconds = 0)
    {
        fs::path p = dir / name;
        std::ofstream(p) << "frame";
        fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds));
        return p;
    }

    std::size_t Prune(std::size_t keep, std::chrono::seconds age, const fs::path& keepPath = fs::path())
    {
        return PruneAutoSavedFrames(dir.string(), keep, age, keepPath.string());
    }
};
} // namespace

TEST(CaptureCleanupName, MatchesOnlyTempFileNames)
{
    EXPECT_TRUE(IsAutoSavedFrameName("save_image_a1B2c3"));
    EXPECT_TRUE(IsAutoSavedFrameName("save_image_XXXXXX"));

    EXPECT_FALSE(IsAutoSavedFrameName("save_image_"));
    EXPECT_FALSE(IsAutoSavedFrameName("save_image_abc"));
    EXPECT_FALSE(IsAutoSavedFrameName("save_image_abcdefg"));
    EXPECT_FALSE(IsAutoSavedFrameName("save_image_abc.fit"));
    EXPECT_FALSE(IsAutoSavedFrameName("save_image_abcdef.fits"));
    EXPECT_FALSE(IsAutoSavedFrameName("my_save_image_abcdef"));
    EXPECT_FALSE(IsAutoSavedFrameName("frame.fits"));
    EXPECT_FALSE(IsAutoSavedFrameName("phd2_settings.txt"));
}

TEST_F(CaptureCleanup, KeepsNewestCount)
{
    for (int i = 0; i < 15; i++)
        Make("save_image_frm" + std::to_string(100 + i), 15 - i); // frm114 newest

    EXPECT_EQ(Prune(10, 3600s), 5u);
    for (int i = 0; i < 5; i++)
        EXPECT_FALSE(fs::exists(dir / ("save_image_frm" + std::to_string(100 + i))));
    for (int i = 5; i < 15; i++)
        EXPECT_TRUE(fs::exists(dir / ("save_image_frm" + std::to_string(100 + i))));
}

TEST_F(CaptureCleanup, RemovesOlderThanMaxAge)
{
    Make("save_image_old001", 1000);
    Make("save_image_old002", 901);
    Make("save_image_new001", 10);

    EXPECT_EQ(Prune(10, 900s), 2u);
    EXPECT_FALSE(fs::exists(dir / "save_image_old001"));
    EXPECT_FALSE(fs::exists(dir / "save_image_old002"));
    EXPECT_TRUE(fs::exists(dir / "save_image_new001"));
}

TEST_F(CaptureCleanup, NeverRemovesKeepPath)
{
    for (int i = 0; i < 5; i++)
        Make("save_image_frm" + std::to_string(100 + i), 10);
    // the frame just written, even if its mtime is old (clock skew) or ties
    fs::path just = Make("save_image_justnw", 5000);

    EXPECT_EQ(Prune(3, 900s, just), 3u);
    EXPECT_TRUE(fs::exists(just));
    // keepPath counts toward the cap: just + 2 others remain
    std::size_t left = 0;
    for (auto& e : fs::directory_iterator(dir))
        left += IsAutoSavedFrameName(e.path().filename().string()) ? 1 : 0;
    EXPECT_EQ(left, 3u);
}

TEST_F(CaptureCleanup, LeavesOtherFilesAlone)
{
    Make("save_image_frm001", 5000);
    fs::path named = Make("polar_align.fits", 5000);
    fs::path ext = Make("save_image_abcdef.fits", 5000);
    fs::path log = Make("PHD2_GuideLog_2026-11-01.txt", 5000);
    fs::create_directory(dir / "save_image_subdir");

    EXPECT_EQ(Prune(0, 1s), 1u);
    EXPECT_TRUE(fs::exists(named));
    EXPECT_TRUE(fs::exists(ext));
    EXPECT_TRUE(fs::exists(log));
    EXPECT_TRUE(fs::is_directory(dir / "save_image_subdir"));
}

TEST_F(CaptureCleanup, DoesNotFollowOrRemoveSymlinks)
{
    fs::path outside = dir / "outside";
    fs::create_directory(outside);
    fs::path target = outside / "precious";
    std::ofstream(target) << "keep me";
    fs::create_symlink(target, dir / "save_image_link01");

    EXPECT_EQ(Prune(0, 1s), 0u);
    EXPECT_TRUE(fs::exists(target));
    EXPECT_TRUE(fs::is_symlink(dir / "save_image_link01"));
}

TEST_F(CaptureCleanup, MissingDirIsANoOp)
{
    EXPECT_EQ(PruneAutoSavedFrames((dir / "nope").string(), 10, 900s), 0u);
    EXPECT_EQ(PruneAutoSavedFrames("", 10, 900s), 0u);
}
