// Where the instance lock goes (open-astro/openastro-guider#135).
//
// After a power loss on RC51 the wxSingleInstanceChecker lock in the home
// directory survived, and on the next boot its PID belonged to the new Xvfb,
// so the guider thought another instance was running. Under systemd the
// lock now goes in $RUNTIME_DIRECTORY, which is on tmpfs and removed when the
// service stops (instance_lock.cpp).

#include <gtest/gtest.h>

#include "instance_lock.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using InstanceLock::LockDir;

namespace
{
class InstanceLockDir : public ::testing::Test
{
protected:
    fs::path dir;

    void SetUp() override
    {
        std::string tmpl = (fs::temp_directory_path() / "instance_lock_XXXXXX").string();
        ASSERT_NE(mkdtemp(tmpl.data()), nullptr);
        dir = tmpl;
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, ec);
        fs::remove_all(dir, ec);
    }
};
} // namespace

TEST_F(InstanceLockDir, UnsetUsesTheWxDefault)
{
    EXPECT_EQ(LockDir(nullptr), "");
}

TEST_F(InstanceLockDir, EmptyUsesTheWxDefault)
{
    EXPECT_EQ(LockDir(""), "");
}

TEST_F(InstanceLockDir, ExistingWritableDirectoryIsUsed)
{
    EXPECT_EQ(LockDir(dir.c_str()), dir.string());
}

TEST_F(InstanceLockDir, FirstOfSeveralRuntimeDirectoriesIsUsed)
{
    fs::path other = dir / "other";
    fs::create_directory(other);
    std::string value = dir.string() + ":" + other.string();
    EXPECT_EQ(LockDir(value.c_str()), dir.string());
}

TEST_F(InstanceLockDir, MissingDirectoryUsesTheWxDefault)
{
    fs::path missing = dir / "missing";
    EXPECT_EQ(LockDir(missing.c_str()), "");
}

TEST_F(InstanceLockDir, RegularFileUsesTheWxDefault)
{
    fs::path file = dir / "file";
    std::ofstream(file) << "x";
    EXPECT_EQ(LockDir(file.c_str()), "");
}

TEST_F(InstanceLockDir, RelativePathUsesTheWxDefault)
{
    EXPECT_EQ(LockDir("run/openastro-guider"), "");
}

TEST_F(InstanceLockDir, ReadOnlyDirectoryUsesTheWxDefault)
{
    if (geteuid() == 0)
        GTEST_SKIP() << "root can write to a read-only directory";
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
    EXPECT_EQ(LockDir(dir.c_str()), "");
}
