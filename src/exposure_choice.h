/*
 *  exposure_choice.h
 *  PHD Guiding
 *
 *  The guide exposure choice MyFrame offers (#115, slice N5 of steps 4-9): the
 *  list of durations, the last of them the custom duration, and how an
 *  exposure value maps onto the entries of the Exposure choice and back. The
 *  choice lists Auto, one entry per duration (the custom one last) and Edit
 *  Custom...; set_exposure selects an entry by value and the selection decides
 *  the exposure (MyFrame::SetExposureDuration, OnExposureDurationSelected).
 *  Moved out of myframe.cpp so slice S6.2 can drop the combo box behind it.
 *  Kept free of wx so it can be unit tested (tests/capture/test_exposure_choice.cpp).
 */

#ifndef EXPOSURE_CHOICE_H_INCLUDED
#define EXPOSURE_CHOICE_H_INCLUDED

#include <vector>

namespace exposure_choice
{

// The custom duration of a profile that has none saved (/CustomExposureDuration), ms
constexpr int DEFAULT_CUSTOM_MS = 30000;
// The duration Edit Custom... falls back to when the exposure is none of the durations
constexpr int FALLBACK_MS = 1000;

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

// The entry to show again after Edit Custom...: Auto, or the exposure's duration, or FALLBACK_MS's when the
// exposure is none of the durations
int EntryAfterEdit(const std::vector<int>& durations, bool autoExposure, int exposureMs);

} // namespace exposure_choice

#endif // EXPOSURE_CHOICE_H_INCLUDED
