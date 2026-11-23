/*
 *  exposure_choice.h
 *  PHD Guiding
 *
 *  The guide exposure choice MyFrame offers (#115, slice N5 of steps 4-9): the
 *  list of durations, the last of them the custom duration, and how an
 *  exposure value maps onto the entries of the Exposure choice and back. The
 *  choice lists Auto, one entry per duration (the custom one last) and Edit
 *  Custom...; set_exposure maps a value to its entry and the entry decides the
 *  exposure (MyFrame::SetExposureDuration, ApplyExposureChoice). Slice S6.2
 *  dropped the combo box and its Edit Custom... dialog: the entries are no
 *  longer shown, only mapped.
 *  Kept free of wx so it can be unit tested (tests/capture/test_exposure_choice.cpp).
 */

#ifndef EXPOSURE_CHOICE_H_INCLUDED
#define EXPOSURE_CHOICE_H_INCLUDED

#include <vector>

namespace exposure_choice
{

// The custom duration of a profile that has none saved (/CustomExposureDuration), ms
constexpr int DEFAULT_CUSTOM_MS = 30000;

// The durations of the choice, ms, in order; the last one is the custom duration, DEFAULT_CUSTOM_MS here
std::vector<int> DefaultDurations();

// The index of `ms` in `durations`, or -1
int DurationIndex(const std::vector<int>& durations, int ms);

// The choice entry set_exposure selects for `ms`: 0 (Auto) for any negative value, the duration's index + 1,
// or -1 when `ms` is none of the durations
int SelectionFor(const std::vector<int>& durations, int ms);

// The number of entries of the choice: Auto, the durations, Edit Custom...
int EntryCount(const std::vector<int>& durations);

// The entry of the custom duration (the last duration)
int CustomEntry(const std::vector<int>& durations);

enum class Pick
{
    Auto, // auto exposure
    Duration, // one of the durations, the custom one included
    EditCustom, // Edit Custom...: ask for a new custom duration
};

struct Selected
{
    Pick pick = Pick::Auto;
    int durationMs = 0; // the duration of Pick::Duration
};

// What selecting `entry` of the choice means
Selected Interpret(const std::vector<int>& durations, unsigned entry);

// SetCustomExposureDuration's check: a custom duration equal to one of the other durations is refused
bool IsOtherDuration(const std::vector<int>& durations, int ms);

// Whether a new custom duration `ms` also changes the exposure: the exposure was the old custom duration, and
// the new one differs from it
bool CustomChangesExposure(const std::vector<int>& durations, int exposureMs, int ms);

} // namespace exposure_choice

#endif // EXPOSURE_CHOICE_H_INCLUDED
