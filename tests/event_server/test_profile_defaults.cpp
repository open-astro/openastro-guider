// Profile key defaults that two places read must agree (#191).
//
// A profile key can be read in more than one place, each with its own
// default for a profile that does not have the key yet. get_profile_setup
// read /guider/multistar/enabled with default true, but
// GuiderMultiStar::LoadProfileSettings reads it with default false and writes
// what it read back, so a new profile has multi-star off. get_profile_setup
// only reported the right value because the guider had written the key first.
//
// Nothing reaches get_profile_setup before the guider has loaded the profile,
// so the disagreement cannot be shown over JSON-RPC. This test reads the
// sources instead: every Profile.GetBoolean of the key in src/*.cpp must pass
// the default the multi-star guider loads it with.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct Read
{
    std::string file;
    int line;
    std::string defaultArg;
};

std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// every Profile.<getter>("<key>", <default>) call in the sources, with its default argument as written
std::vector<Read> FindReads(const std::string& getter, const std::string& key)
{
    const std::string call = "Profile." + getter + "(\"" + key + "\",";
    std::vector<Read> reads;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(PHD_SRC_DIR))
        if (entry.path().extension() == ".cpp")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    for (const auto& path : files)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (size_t pos = text.find(call); pos != std::string::npos; pos = text.find(call, pos + 1))
        {
            // the default runs to the parenthesis that closes the call
            size_t i = pos + call.size();
            size_t start = i;
            int depth = 0;
            for (; i < text.size(); ++i)
            {
                if (text[i] == '(')
                    ++depth;
                else if (text[i] == ')' && depth-- == 0)
                    break;
            }
            int line = 1 + (int) std::count(text.begin(), text.begin() + pos, '\n');
            reads.push_back({ path.filename().string(), line, Trim(text.substr(start, i - start)) });
        }
    }
    return reads;
}

const Read *FindIn(const std::vector<Read>& reads, const std::string& file)
{
    for (const auto& r : reads)
        if (r.file == file)
            return &r;
    return nullptr;
}
} // namespace

TEST(ProfileDefaults, MultiStarEnabledReadsUseTheGuidersDefault)
{
    std::vector<Read> reads = FindReads("GetBoolean", "/guider/multistar/enabled");

    // the multi-star guider loads the key when a profile is selected, and writes it back
    const Read *guider = FindIn(reads, "guider_multistar.cpp");
    ASSERT_NE(guider, nullptr) << "GuiderMultiStar::LoadProfileSettings no longer reads /guider/multistar/enabled";
    EXPECT_EQ(guider->defaultArg, "false") << "a new profile has multi-star off, as in upstream PHD2";

    // get_profile_setup reports it
    ASSERT_NE(FindIn(reads, "event_server.cpp"), nullptr) << "get_profile_setup no longer reads /guider/multistar/enabled";

    for (const auto& r : reads)
        EXPECT_EQ(r.defaultArg, guider->defaultArg)
            << r.file << ":" << r.line << " reads /guider/multistar/enabled with default " << r.defaultArg
            << ", the guider with " << guider->defaultArg;
}
