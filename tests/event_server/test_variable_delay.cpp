// Variable exposure delay seconds <-> ms (#174).
//
// set_variable_delay_settings takes ShortDelaySeconds and LongDelaySeconds and
// stores the delays in ms; get_variable_delay_settings reports them in
// seconds. The setter used to compute (int) seconds * 1000, which casts before
// it multiplies, so 2.5 s was stored as 2000 ms, and the getter divided the
// stored ms by 1000 in integers, so a 2500 ms delay read back as 2 s. The
// conversion lives in variable_delay.cpp.

#include <gtest/gtest.h>

#include "variable_delay.h"

using VariableDelay::MsToSeconds;
using VariableDelay::SecondsToMs;

TEST(VariableDelay, IssueReproFractionalSecondsAreKept)
{
    EXPECT_EQ(SecondsToMs(2.5), 2500);
    EXPECT_EQ(SecondsToMs(0.25), 250);
    EXPECT_EQ(SecondsToMs(45.75), 45750);
}

TEST(VariableDelay, WholeSecondsAreUnchanged)
{
    // what PHD2 documents (integer seconds) and what its dialog's spin controls send
    EXPECT_EQ(SecondsToMs(0), 0);
    EXPECT_EQ(SecondsToMs(1), 1000);
    EXPECT_EQ(SecondsToMs(10), 10000);
    EXPECT_EQ(SecondsToMs(120), 120000);
}

TEST(VariableDelay, RoundsToTheNearestMs)
{
    // 1.001 * 1000 is 1000.9999999999999 in binary, so truncating the product would lose a ms
    EXPECT_EQ(SecondsToMs(1.001), 1001);
    EXPECT_EQ(SecondsToMs(0.001), 1);
    EXPECT_EQ(SecondsToMs(1.0004), 1000);
    EXPECT_EQ(SecondsToMs(1.0006), 1001);
}

TEST(VariableDelay, StoredMsReadBackAsFractionalSeconds)
{
    EXPECT_DOUBLE_EQ(MsToSeconds(2500), 2.5);
    EXPECT_DOUBLE_EQ(MsToSeconds(250), 0.25);
    EXPECT_DOUBLE_EQ(MsToSeconds(1), 0.001);
}

TEST(VariableDelay, WholeSecondsReadBackWhole)
{
    // the profile defaults (1000 and 10000 ms) still read as 1 and 10, which "%g" prints as integers
    EXPECT_DOUBLE_EQ(MsToSeconds(0), 0.0);
    EXPECT_DOUBLE_EQ(MsToSeconds(1000), 1.0);
    EXPECT_DOUBLE_EQ(MsToSeconds(10000), 10.0);
}

TEST(VariableDelay, RoundTripsEveryMs)
{
    for (int ms = 0; ms <= 120000; ++ms)
        ASSERT_EQ(SecondsToMs(MsToSeconds(ms)), ms) << "ms=" << ms;
}
