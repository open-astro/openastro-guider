/*
 *  exposure_choice.cpp
 *  PHD Guiding
 *
 *  See exposure_choice.h.
 */

#include "exposure_choice.h"

#include <algorithm>

namespace exposure_choice
{

std::vector<int> DefaultDurations()
{
    // the final entry is the custom value
    return { 10,   20,   50,   100,  200,  500,  1000, 1500, 2000,  2500,  3000,
             3500, 4000, 4500, 5000, 6000, 7000, 8000, 9000, 10000, 15000, DEFAULT_CUSTOM_MS };
}

int DurationIndex(const std::vector<int>& durations, int ms)
{
    for (auto it = durations.begin(); it != durations.end(); ++it)
        if (ms == *it)
            return static_cast<int>(it - durations.begin());
    return -1;
}

int SelectionFor(const std::vector<int>& durations, int ms)
{
    if (ms < 0)
        return 0; // Auto
    int idx = DurationIndex(durations, ms);
    return idx == -1 ? -1 : idx + 1; // skip Auto
}

int EntryCount(const std::vector<int>& durations)
{
    return static_cast<int>(durations.size()) + 2;
}

int CustomEntry(const std::vector<int>& durations)
{
    return static_cast<int>(durations.size());
}

Selected Interpret(const std::vector<int>& durations, unsigned entry)
{
    Selected s;
    if (entry == 0)
        s.pick = Pick::Auto;
    else if (entry == static_cast<unsigned>(EntryCount(durations) - 1))
        s.pick = Pick::EditCustom;
    else
    {
        s.pick = Pick::Duration;
        s.durationMs = durations[entry - 1];
    }
    return s;
}

bool IsOtherDuration(const std::vector<int>& durations, int ms)
{
    if (durations.empty())
        return false;
    return std::find(durations.begin(), durations.end() - 1, ms) != durations.end() - 1;
}

bool CustomChangesExposure(const std::vector<int>& durations, int exposureMs, int ms)
{
    if (durations.empty())
        return false;
    int custom = durations.back();
    return exposureMs == custom && custom != ms;
}

} // namespace exposure_choice
