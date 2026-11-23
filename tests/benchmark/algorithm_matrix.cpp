// The in-process guide algorithm matrix (#142 Tier 4).
//
// Runs guide_sim (the seeded closed loop over the REAL wx-free algorithm kernels, guide_sim.h)
// for each RA/Dec algorithm pair under each sky and mount condition, over several seeds, and
// reports the mean RMS of each cell as a Markdown table (and JSON). With --thresholds it checks
// each cell against a checked-in CSV of generous ceilings and fails on a cell over its ceiling,
// on a cell guiding no better than half the unguided RMS, and on a threshold row with no cell.
//
//   guide_algorithm_matrix [--seeds N] [--markdown FILE] [--json FILE]
//                          [--thresholds FILE] [--write-thresholds FILE]
//
// The nightly workflow (.github/workflows/nightly-algorithm-matrix.yml) runs it with the
// thresholds and puts the table in the job summary; ctest runs it the same way, as it takes
// a few seconds. --write-thresholds writes ceilings of 1.25 x the measured mean + 0.1" for the
// current tree, to refresh the CSV after an intended change.

#include "guide_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace guide_sim;

namespace
{

struct Condition
{
    const char *name;
    const char *what;
    void (*apply)(Scenario&);
};

// Each condition is the RMS baseline's sky (10" PE over 480 s, 0.01"/s RA and 0.02"/s Dec drift,
// 0.5" seeing, 2 s exposures, 1.93"/px, guide rate 0.5x sidereal) with one thing changed
const Condition CONDITIONS[] = {
    { "nominal", "the RMS baseline's sky", [](Scenario&) {} },
    { "poor-seeing", "seeing 1.5\"", [](Scenario& sc) { sc.seeing = 1.5; } },
    { "fast-pe", "PE 15\" over 120 s",
      [](Scenario& sc)
      {
          sc.peAmplitude = 15.0;
          sc.pePeriod = 120.0;
      } },
    { "poor-pa", "drift 0.05\"/s RA, 0.15\"/s Dec",
      [](Scenario& sc)
      {
          sc.raDrift = 0.05;
          sc.decDrift = 0.15;
      } },
    { "dec-backlash", "1000 ms Dec backlash", [](Scenario& sc) { sc.decBacklashMs = 1000.0; } },
};

struct Pair
{
    Algo ra;
    Algo dec;
};

// Each RA algorithm with the Dec default, then each Dec algorithm with the RA default
const Pair PAIRS[] = {
    { Algo::Hysteresis, Algo::ResistSwitch },   { Algo::Lowpass, Algo::ResistSwitch },  { Algo::Lowpass2, Algo::ResistSwitch },
    { Algo::ResistSwitch, Algo::ResistSwitch }, { Algo::Hysteresis, Algo::Hysteresis }, { Algo::Hysteresis, Algo::Lowpass },
    { Algo::Hysteresis, Algo::Lowpass2 },
};

struct Cell
{
    std::string ra;
    std::string dec;
    std::string condition;
    double raRms = 0.;
    double decRms = 0.;
    double totalRms = 0.;
    double worstTotal = 0.; // the worst seed's total
    double unguidedTotal = 0.;
};

std::string Key(const std::string& ra, const std::string& dec, const std::string& condition)
{
    return ra + "," + dec + "," + condition;
}

Cell RunCell(const Pair& p, const Condition& c, int seeds)
{
    Cell cell;
    cell.ra = AlgoName(p.ra);
    cell.dec = AlgoName(p.dec);
    cell.condition = c.name;
    for (int s = 1; s <= seeds; s++)
    {
        Scenario sc;
        c.apply(sc);
        sc.raAlgo = p.ra;
        sc.decAlgo = p.dec;
        sc.seed = (uint64_t) s;
        Outcome o = Simulate(sc);
        cell.raRms += o.raRms / seeds;
        cell.decRms += o.decRms / seeds;
        cell.totalRms += o.totalRms / seeds;
        cell.worstTotal = std::max(cell.worstTotal, o.totalRms);
    }
    Scenario unguided;
    c.apply(unguided);
    unguided.guide = false;
    cell.unguidedTotal = Simulate(unguided).totalRms;
    return cell;
}

std::string Markdown(const std::vector<Cell>& cells, int seeds)
{
    std::ostringstream md;
    md << "### In-process algorithm matrix\n\n"
       << "Mean RMS over " << seeds
       << " seeds, arcsec: **total** (RA / Dec). The seeded closed loop of tests/benchmark/guide_sim.h over the "
          "real wx-free kernels with the production defaults; 2 s exposures at 1.93\"/px, 1440 s per seed.\n\n";
    md << "| RA algo | Dec algo |";
    for (const Condition& c : CONDITIONS)
        md << " " << c.name << " |";
    md << "\n|---|---|";
    for (size_t i = 0; i < std::size(CONDITIONS); i++)
        md << "---|";
    md << "\n";
    for (const Pair& p : PAIRS)
    {
        md << "| " << AlgoName(p.ra) << " | " << AlgoName(p.dec) << " |";
        for (const Condition& c : CONDITIONS)
        {
            for (const Cell& cell : cells)
            {
                if (cell.ra == AlgoName(p.ra) && cell.dec == AlgoName(p.dec) && cell.condition == c.name)
                {
                    char buf[96];
                    std::snprintf(buf, sizeof buf, " **%.2f** (%.2f / %.2f) |", cell.totalRms, cell.raRms, cell.decRms);
                    md << buf;
                }
            }
        }
        md << "\n";
    }
    md << "| unguided | |";
    for (const Condition& c : CONDITIONS)
    {
        for (const Cell& cell : cells)
        {
            if (cell.condition == c.name)
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, " %.2f |", cell.unguidedTotal);
                md << buf;
                break;
            }
        }
    }
    md << "\n\nConditions (each the baseline sky with one change):";
    for (const Condition& c : CONDITIONS)
        md << " **" << c.name << "**: " << c.what << ";";
    md << "\n";
    return md.str();
}

std::string Json(const std::vector<Cell>& cells, int seeds)
{
    std::ostringstream js;
    js << "{\"seeds\": " << seeds << ", \"cells\": [";
    for (size_t i = 0; i < cells.size(); i++)
    {
        const Cell& c = cells[i];
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "%s\n  {\"ra\": \"%s\", \"dec\": \"%s\", \"condition\": \"%s\", \"ra_rms\": %.4f, "
                      "\"dec_rms\": %.4f, \"total_rms\": %.4f, \"worst_total_rms\": %.4f, \"unguided_total_rms\": %.4f}",
                      i ? "," : "", c.ra.c_str(), c.dec.c_str(), c.condition.c_str(), c.raRms, c.decRms, c.totalRms,
                      c.worstTotal, c.unguidedTotal);
        js << buf;
    }
    js << "\n]}\n";
    return js.str();
}

// ra,dec,condition,max_total_rms rows; # comments and blank lines skipped
bool ReadThresholds(const char *path, std::map<std::string, double> *out)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, ','))
            f.push_back(item);
        if (f.size() != 4)
        {
            std::fprintf(stderr, "bad threshold row: %s\n", line.c_str());
            return false;
        }
        (*out)[Key(f[0], f[1], f[2])] = std::atof(f[3].c_str());
    }
    return true;
}

bool WriteFile(const char *path, const std::string& text)
{
    std::ofstream out(path);
    out << text;
    return (bool) out;
}

} // namespace

int main(int argc, char **argv)
{
    int seeds = 20;
    const char *markdownPath = nullptr;
    const char *jsonPath = nullptr;
    const char *thresholdsPath = nullptr;
    const char *writeThresholdsPath = nullptr;
    for (int i = 1; i < argc; i++)
    {
        auto value = [&](const char *opt) -> const char *
        {
            if (std::strcmp(argv[i], opt) != 0)
                return nullptr;
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "%s needs a value\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        const char *v;
        if ((v = value("--seeds")) != nullptr)
            seeds = std::max(1, std::atoi(v));
        else if ((v = value("--markdown")) != nullptr)
            markdownPath = v;
        else if ((v = value("--json")) != nullptr)
            jsonPath = v;
        else if ((v = value("--thresholds")) != nullptr)
            thresholdsPath = v;
        else if ((v = value("--write-thresholds")) != nullptr)
            writeThresholdsPath = v;
        else
        {
            std::fprintf(stderr,
                         "usage: %s [--seeds N] [--markdown FILE] [--json FILE] [--thresholds FILE] "
                         "[--write-thresholds FILE]\n",
                         argv[0]);
            return 2;
        }
    }

    std::vector<Cell> cells;
    for (const Pair& p : PAIRS)
        for (const Condition& c : CONDITIONS)
            cells.push_back(RunCell(p, c, seeds));

    std::string md = Markdown(cells, seeds);
    std::fputs(md.c_str(), stdout);
    if (markdownPath && !WriteFile(markdownPath, md))
    {
        std::fprintf(stderr, "cannot write %s\n", markdownPath);
        return 2;
    }
    if (jsonPath && !WriteFile(jsonPath, Json(cells, seeds)))
    {
        std::fprintf(stderr, "cannot write %s\n", jsonPath);
        return 2;
    }

    if (writeThresholdsPath)
    {
        std::ostringstream csv;
        csv << "# The in-process algorithm matrix's ceilings (tests/benchmark/algorithm_matrix.cpp): the mean total\n"
            << "# RMS, arcsec, over " << seeds << " seeds may not exceed max_total_rms. Written by --write-thresholds as\n"
            << "# 1.25 x the measured mean + 0.1, so only a clear regression fails.\n"
            << "# ra,dec,condition,max_total_rms\n";
        for (const Cell& c : cells)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", std::ceil((1.25 * c.totalRms + 0.1) * 100.) / 100.);
            csv << c.ra << "," << c.dec << "," << c.condition << "," << buf << "\n";
        }
        if (!WriteFile(writeThresholdsPath, csv.str()))
        {
            std::fprintf(stderr, "cannot write %s\n", writeThresholdsPath);
            return 2;
        }
    }

    if (!thresholdsPath)
        return 0;

    std::map<std::string, double> limits;
    if (!ReadThresholds(thresholdsPath, &limits))
    {
        std::fprintf(stderr, "cannot read thresholds %s\n", thresholdsPath);
        return 2;
    }
    int failures = 0;
    for (const Cell& c : cells)
    {
        std::string key = Key(c.ra, c.dec, c.condition);
        auto it = limits.find(key);
        if (it == limits.end())
        {
            std::printf("FAIL %s: no threshold row\n", key.c_str());
            failures++;
            continue;
        }
        if (!(c.totalRms <= it->second))
        {
            std::printf("FAIL %s: total RMS %.3f\" over its ceiling %.2f\"\n", key.c_str(), c.totalRms, it->second);
            failures++;
        }
        if (!(c.totalRms < 0.5 * c.unguidedTotal))
        {
            std::printf("FAIL %s: total RMS %.3f\" not under half the unguided %.3f\"\n", key.c_str(), c.totalRms,
                        c.unguidedTotal);
            failures++;
        }
        limits.erase(it);
    }
    for (const auto& kv : limits)
    {
        std::printf("FAIL %s: a threshold row with no cell (remove it or add the cell)\n", kv.first.c_str());
        failures++;
    }
    std::printf("\n%s: %zu cells, %d failures\n", failures ? "FAILED" : "PASSED", cells.size(), failures);
    return failures ? 1 : 0;
}
