// AtomicFileWrite (#142): how PhdConfig::Flush writes the config file.
//
// RC51 gets its power cut in the field. wxFileConfig::Flush renamed a file it never fsynced over the config, and
// before wxWidgets 3.2.8 deleted the config first, so a kill or power cut could leave no config or an empty one and
// the next start made a new one, losing every profile. AtomicFileWrite fsyncs the new file, renames it over the old
// one and fsyncs the directory. A power cut cannot be made here; these tests pin what can be seen from a process:
// the contents and mode, the temporary file, a failure leaving the old file alone, and a reader never finding the
// file missing or partly written while it is rewritten. scripts/e2e_persistence.py checks the syscalls of the real
// daemon's writes under strace.

#include <gtest/gtest.h>

#include "atomic_file_write.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace
{
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const std::string& path, const std::string& data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

bool Exists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

mode_t ModeOf(const std::string& path)
{
    struct stat st;
    EXPECT_EQ(::stat(path.c_str(), &st), 0) << path;
    return st.st_mode & 07777;
}

mode_t CurrentUmask()
{
    mode_t m = ::umask(0);
    ::umask(m);
    return m;
}

class AtomicFileWriteTest : public ::testing::Test
{
protected:
    std::string dir;
    std::string path;

    void SetUp() override
    {
        std::string tmpl = ::testing::TempDir() + "atomic_file_write_XXXXXX";
        ASSERT_NE(::mkdtemp(&tmpl[0]), nullptr) << tmpl;
        dir = tmpl;
        path = dir + "/.OpenAstroPHD2";
    }

    void TearDown() override
    {
        std::string cmd = "rm -rf '" + dir + "'";
        EXPECT_EQ(std::system(cmd.c_str()), 0);
    }
};
} // namespace

TEST_F(AtomicFileWriteTest, TempFileIsBesideTheFile)
{
    EXPECT_EQ(AtomicFileWriteTempPath("/home/astro/.OpenAstroPHD2"), "/home/astro/.OpenAstroPHD2.tmp");
}

TEST_F(AtomicFileWriteTest, CreatesAFileWithTheUmaskMode)
{
    std::string error;
    ASSERT_TRUE(AtomicFileWrite(path, "[profile]\nname=a\n", &error)) << error;
    EXPECT_EQ(ReadFile(path), "[profile]\nname=a\n");
    EXPECT_EQ(ModeOf(path), 0666 & ~CurrentUmask());
    EXPECT_FALSE(Exists(AtomicFileWriteTempPath(path)));
}

TEST_F(AtomicFileWriteTest, ReplacesTheFileAndKeepsItsMode)
{
    WriteFile(path, "old contents, longer than the new ones\n");
    ASSERT_EQ(::chmod(path.c_str(), 0640), 0);
    std::string error;
    ASSERT_TRUE(AtomicFileWrite(path, "new\n", &error)) << error;
    EXPECT_EQ(ReadFile(path), "new\n");
    EXPECT_EQ(ModeOf(path), 0640u);
    EXPECT_FALSE(Exists(AtomicFileWriteTempPath(path)));
}

TEST_F(AtomicFileWriteTest, WritesEmptyAndLargeContents)
{
    std::string error;
    ASSERT_TRUE(AtomicFileWrite(path, "", &error)) << error;
    EXPECT_TRUE(Exists(path));
    EXPECT_EQ(ReadFile(path), "");

    std::string big(3 << 20, 'x');
    for (size_t i = 0; i < big.size(); i += 4096)
        big[i] = '\n';
    ASSERT_TRUE(AtomicFileWrite(path, big, &error)) << error;
    EXPECT_EQ(ReadFile(path), big);
}

TEST_F(AtomicFileWriteTest, OverwritesATempFileAKilledWriteLeft)
{
    WriteFile(path, "old\n");
    WriteFile(AtomicFileWriteTempPath(path), "the start of a write that was killed, longer than the new contents");
    std::string error;
    ASSERT_TRUE(AtomicFileWrite(path, "new\n", &error)) << error;
    EXPECT_EQ(ReadFile(path), "new\n");
    EXPECT_FALSE(Exists(AtomicFileWriteTempPath(path)));
}

TEST_F(AtomicFileWriteTest, AFailedWriteLeavesTheOldFile)
{
    WriteFile(path, "old\n");
    // a directory where the temporary file goes: the write cannot start
    ASSERT_EQ(::mkdir(AtomicFileWriteTempPath(path).c_str(), 0755), 0);
    std::string error;
    EXPECT_FALSE(AtomicFileWrite(path, "new\n", &error));
    EXPECT_NE(error.find("cannot create"), std::string::npos) << error;
    EXPECT_NE(error.find(AtomicFileWriteTempPath(path)), std::string::npos) << error;
    EXPECT_EQ(ReadFile(path), "old\n");
    EXPECT_EQ(::rmdir(AtomicFileWriteTempPath(path).c_str()), 0);
}

TEST_F(AtomicFileWriteTest, AFailedRenameLeavesTheOldFileAndNoTempFile)
{
    // the destination is a non-empty directory: everything up to the rename works, the rename fails
    ASSERT_EQ(::mkdir(path.c_str(), 0755), 0);
    WriteFile(path + "/keep", "kept\n");
    std::string error;
    EXPECT_FALSE(AtomicFileWrite(path, "new\n", &error));
    EXPECT_NE(error.find("cannot rename onto " + path), std::string::npos) << error;
    EXPECT_EQ(ReadFile(path + "/keep"), "kept\n");
    EXPECT_FALSE(Exists(AtomicFileWriteTempPath(path)));
}

TEST_F(AtomicFileWriteTest, AMissingDirectoryFails)
{
    std::string error;
    EXPECT_FALSE(AtomicFileWrite(dir + "/no/such/dir/cfg", "x", &error));
    EXPECT_NE(error.find("No such file or directory"), std::string::npos) << error;
    EXPECT_FALSE(AtomicFileWrite(dir + "/no/cfg", "x", nullptr)); // no error string asked for
}

TEST_F(AtomicFileWriteTest, ARelativePathIsWrittenInTheCurrentDirectory)
{
    char cwd[4096];
    ASSERT_NE(::getcwd(cwd, sizeof(cwd)), nullptr);
    ASSERT_EQ(::chdir(dir.c_str()), 0);
    std::string error;
    bool ok = AtomicFileWrite("relative.cfg", "rel\n", &error);
    ASSERT_EQ(::chdir(cwd), 0);
    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(ReadFile(dir + "/relative.cfg"), "rel\n");
}

// The config is never missing or partly written while it is rewritten: what wxFileConfig did before 3.2.8 (delete
// the old file, then rename the new one in) leaves a window with no file at all.
TEST_F(AtomicFileWriteTest, AReaderAlwaysSeesAWholeFile)
{
    const std::string a(20000, 'a');
    const std::string b(30000, 'b');
    std::string error;
    ASSERT_TRUE(AtomicFileWrite(path, a, &error)) << error;

    std::atomic<bool> done(false), started(false);
    std::atomic<int> reads(0), missing(0), torn(0);
    std::thread reader(
        [&]
        {
            started = true;
            while (!done)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    ++missing;
                    continue;
                }
                std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (got != a && got != b)
                    ++torn;
                ++reads;
            }
        });
    // The writes must overlap the reads: on tmpfs 200 of them take a few ms, which a loaded machine can spend before
    // the reader is ever scheduled. Start once the reader runs, and keep writing until it has read during the writes
    // (bounded, so a reader that never gets a turn fails the reads check instead of hanging)
    while (!started)
        std::this_thread::yield();
    bool ok = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (int i = 0; ok && (i < 200 || reads < 20) && std::chrono::steady_clock::now() < deadline; i++)
        ok = AtomicFileWrite(path, i % 2 ? a : b, &error);
    done = true;
    reader.join();

    ASSERT_TRUE(ok) << error;
    EXPECT_GE(reads.load(), 20);
    EXPECT_EQ(missing.load(), 0);
    EXPECT_EQ(torn.load(), 0);
}
