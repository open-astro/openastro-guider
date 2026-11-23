// The guide exposure choice (open-astro/openastro-guider#115, slice N5 of steps 4-9).
//
// set_exposure selects an entry of MyFrame's Exposure choice by value, and the selected entry
// decides the exposure: Auto, one entry per duration (the custom duration last), then Edit
// Custom..., which asks for a new custom duration. Slice S6.2 drops the combo box behind it;
// these tests pin the mapping it keeps, as MyFrame did it before the move into
// src/exposure_choice.cpp. The daemon end to end (each value of get_exposure_durations, Auto, the
// custom duration and a refused value through set_exposure, with the GuideParamChange "Exposure"
// event and get_exposure) is scripts/e2e_guide_scenarios.py's "exposure" scenario.

#include <gtest/gtest.h>

#include "exposure_choice.h"

#include <algorithm>
#include <vector>

using namespace exposure_choice;

namespace
{
const std::vector<int> kDurations = DefaultDurations();
} // namespace

TEST(ExposureChoice, DefaultDurationsAreTheListGetExposureDurationsReports)
{
    const std::vector<int> want = { 10,   20,   50,   100,  200,  500,  1000, 1500, 2000,  2500,  3000,
                                    3500, 4000, 4500, 5000, 6000, 7000, 8000, 9000, 10000, 15000, 30000 };
    EXPECT_EQ(kDurations, want);
    EXPECT_EQ(kDurations.back(), DEFAULT_CUSTOM_MS);
    EXPECT_TRUE(std::is_sorted(kDurations.begin(), kDurations.end()));
}

TEST(ExposureChoice, DurationIndexFindsEachDurationAndNothingElse)
{
    for (size_t i = 0; i < kDurations.size(); i++)
        EXPECT_EQ(DurationIndex(kDurations, kDurations[i]), static_cast<int>(i)) << kDurations[i];
    for (int ms : { -1, 0, 1, 11, 999, 1234, 29999, 30001, 60000 })
        EXPECT_EQ(DurationIndex(kDurations, ms), -1) << ms;
    EXPECT_EQ(DurationIndex({}, 1000), -1);
}

TEST(ExposureChoice, SelectionForSkipsAutoAndTakesAnyNegativeValueAsAuto)
{
    EXPECT_EQ(SelectionFor(kDurations, -1), 0);
    EXPECT_EQ(SelectionFor(kDurations, -2), 0); // any negative value, not just set_exposure's -1
    EXPECT_EQ(SelectionFor(kDurations, -30000), 0);
    EXPECT_EQ(SelectionFor(kDurations, 10), 1);
    EXPECT_EQ(SelectionFor(kDurations, 1000), 7);
    EXPECT_EQ(SelectionFor(kDurations, 15000), 21);
    EXPECT_EQ(SelectionFor(kDurations, 30000), 22); // the custom duration's entry
    EXPECT_EQ(SelectionFor(kDurations, 0), -1); // 0 is a value, and none of the durations
    EXPECT_EQ(SelectionFor(kDurations, 1234), -1);
}

TEST(ExposureChoice, EntriesAreAutoTheDurationsAndEditCustom)
{
    EXPECT_EQ(EntryCount(kDurations), 24);
    EXPECT_EQ(CustomEntry(kDurations), 22);

    Selected s = Interpret(kDurations, 0);
    EXPECT_EQ(s.pick, Pick::Auto);
    for (size_t i = 0; i < kDurations.size(); i++)
    {
        s = Interpret(kDurations, static_cast<unsigned>(i + 1));
        EXPECT_EQ(s.pick, Pick::Duration) << i;
        EXPECT_EQ(s.durationMs, kDurations[i]) << i;
    }
    // selecting the custom duration is an ordinary selection; only the entry after it edits
    s = Interpret(kDurations, CustomEntry(kDurations));
    EXPECT_EQ(s.pick, Pick::Duration);
    EXPECT_EQ(s.durationMs, DEFAULT_CUSTOM_MS);
    EXPECT_EQ(Interpret(kDurations, 23).pick, Pick::EditCustom);
}

TEST(ExposureChoice, SelectionForAndInterpretRoundTrip)
{
    for (int ms : kDurations)
    {
        Selected s = Interpret(kDurations, static_cast<unsigned>(SelectionFor(kDurations, ms)));
        EXPECT_EQ(s.pick, Pick::Duration) << ms;
        EXPECT_EQ(s.durationMs, ms);
    }
    EXPECT_EQ(Interpret(kDurations, static_cast<unsigned>(SelectionFor(kDurations, -1))).pick, Pick::Auto);
}

TEST(ExposureChoice, ACustomDurationEqualToAnotherDurationIsRefused)
{
    for (size_t i = 0; i + 1 < kDurations.size(); i++)
        EXPECT_TRUE(IsOtherDuration(kDurations, kDurations[i])) << kDurations[i];
    EXPECT_FALSE(IsOtherDuration(kDurations, DEFAULT_CUSTOM_MS)); // the custom duration itself
    EXPECT_FALSE(IsOtherDuration(kDurations, 45000));
    EXPECT_FALSE(IsOtherDuration(kDurations, 1234));
    EXPECT_FALSE(IsOtherDuration({}, 1000));
}

TEST(ExposureChoice, ANewCustomDurationMovesTheExposureOnlyWhenItWasTheCustomOne)
{
    EXPECT_TRUE(CustomChangesExposure(kDurations, 30000, 45000));
    EXPECT_FALSE(CustomChangesExposure(kDurations, 30000, 30000)); // the same value
    EXPECT_FALSE(CustomChangesExposure(kDurations, 2000, 45000)); // the exposure is another duration
    std::vector<int> custom2000 = kDurations;
    custom2000.back() = 2000; // a custom duration equal to another one (from an old profile)
    EXPECT_TRUE(CustomChangesExposure(custom2000, 2000, 45000));
}

TEST(ExposureChoice, TheEntryAfterEditCustomIsAutoTheExposureOrOneSecond)
{
    EXPECT_EQ(EntryAfterEdit(kDurations, true, 2000), 0);
    EXPECT_EQ(EntryAfterEdit(kDurations, false, 2000), 9);
    EXPECT_EQ(EntryAfterEdit(kDurations, false, 30000), 22);
    EXPECT_EQ(EntryAfterEdit(kDurations, false, 1234), 7); // none of the durations: 1 s
    EXPECT_EQ(Interpret(kDurations, EntryAfterEdit(kDurations, false, 1234)).durationMs, FALLBACK_MS);
}
