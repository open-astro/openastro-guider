// guide_log_replay: replays PHD2 guide logs through the wx-free guide algorithm kernels (#142 Tier 4).
//
//   guide_log_replay [--markdown FILE] [--mismatches] LOG...
//
// For each guiding session of each log: how many of the logged guide distances and pulses the
// kernels reproduce from the logged offsets with the algorithms and parameters of the session's
// header, the logged RMS against a closed-loop self-replay, and the RMS each algorithm is predicted
// to leave on the night's reconstructed motion (guide_log_replay.h). The report is Markdown, on
// stdout and in FILE. Exits 1 when a log cannot be read.

#include "guide_log_replay.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    const char *markdownPath = nullptr;
    bool mismatches = false;
    std::vector<std::string> logs;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--markdown") == 0 && i + 1 < argc)
            markdownPath = argv[++i];
        else if (std::strcmp(argv[i], "--mismatches") == 0)
            mismatches = true;
        else if (argv[i][0] == '-')
        {
            std::fprintf(stderr, "usage: %s [--markdown FILE] [--mismatches] LOG...\n", argv[0]);
            return 2;
        }
        else
            logs.emplace_back(argv[i]);
    }
    if (logs.empty())
    {
        std::fprintf(stderr, "usage: %s [--markdown FILE] [--mismatches] LOG...\n", argv[0]);
        return 2;
    }

    std::string all;
    for (const std::string& path : logs)
    {
        std::vector<guide_log::Session> sessions;
        std::string error;
        if (!guide_log::ParseFile(path, &sessions, &error))
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::vector<guide_log_replay::SessionReport> reports;
        for (const guide_log::Session& s : sessions)
            reports.push_back(guide_log_replay::ReplaySession(s));
        std::string name = path.substr(path.find_last_of('/') + 1);
        all += guide_log_replay::Markdown(name, reports) + "\n";
        if (mismatches)
        {
            for (const auto& r : reports)
            {
                for (const auto *a : { &r.ra, &r.dec })
                {
                    for (const auto& m : a->mismatches)
                        std::fprintf(
                            stderr, "%s:%d frame %d %c: input %.3f logged %.3f replayed %.4f, pulse logged %d replayed %d\n",
                            name.c_str(), m.line, m.frame, m.axis, m.input, m.logged, m.replayed, m.loggedMs, m.replayedMs);
                }
            }
        }
    }
    std::fputs(all.c_str(), stdout);
    if (markdownPath)
    {
        std::ofstream out(markdownPath);
        out << all;
        if (!out)
        {
            std::fprintf(stderr, "cannot write %s\n", markdownPath);
            return 1;
        }
    }
    return 0;
}
