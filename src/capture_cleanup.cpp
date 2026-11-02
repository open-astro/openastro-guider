/*
 *  capture_cleanup.cpp
 *  PHD Guiding
 *
 *  See capture_cleanup.h.
 */

#include "capture_cleanup.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

static const char kPrefix[] = "save_image_";
static const std::size_t kPrefixLen = sizeof(kPrefix) - 1;
static const std::size_t kSuffixLen = 6; // mkstemp's XXXXXX

bool IsAutoSavedFrameName(const std::string& filename)
{
    if (filename.size() != kPrefixLen + kSuffixLen || filename.compare(0, kPrefixLen, kPrefix) != 0)
        return false;
    return std::all_of(filename.begin() + kPrefixLen, filename.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

std::size_t PruneAutoSavedFrames(const std::string& dir, std::size_t keepCount, std::chrono::seconds maxAge,
                                 const std::string& keepPath)
{
    struct Frame
    {
        fs::path path;
        fs::file_time_type mtime;
    };

    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return 0;

    fs::path keep;
    if (!keepPath.empty())
        keep = fs::weakly_canonical(fs::path(keepPath), ec);

    std::vector<Frame> frames;
    for (; it != fs::directory_iterator(); it.increment(ec))
    {
        if (ec)
            break;
        const fs::directory_entry& entry = *it;
        if (!IsAutoSavedFrameName(entry.path().filename().string()))
            continue;
        std::error_code ec2;
        // symlink_status: a link named like a frame is not ours to delete
        if (!fs::is_regular_file(entry.symlink_status(ec2)) || ec2)
            continue;
        if (!keep.empty() && fs::weakly_canonical(entry.path(), ec2) == keep && !ec2)
            continue;
        fs::file_time_type mtime = entry.last_write_time(ec2);
        if (ec2)
            continue;
        frames.push_back({ entry.path(), mtime });
    }

    // newest first; the kept file counts toward keepCount
    std::sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) { return a.mtime > b.mtime; });
    std::size_t room = keepCount;
    if (!keep.empty() && room > 0)
        --room;

    const fs::file_time_type cutoff = fs::file_time_type::clock::now() - maxAge;
    std::size_t removed = 0;
    for (std::size_t i = 0; i < frames.size(); i++)
    {
        if (i < room && frames[i].mtime >= cutoff)
            continue;
        std::error_code ec2;
        if (fs::remove(frames[i].path, ec2))
            ++removed;
    }
    return removed;
}
