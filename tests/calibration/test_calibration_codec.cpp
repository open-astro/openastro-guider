// Unit tests for src/calibration_codec.cpp (#142): how a mount's calibration and its
// details are written to the profile and read back, extracted from Mount::SetCalibration,
// GetLastCalibration, SaveCalibrationDetails and LoadCalibrationDetails.
//
// restore_calibration, guide{recalibrate:false} and the image-scale checks read the
// calibration back through this code, so a key renamed on one side, a default that drifts,
// or a step list that no longer parses would restore a different calibration from the one
// that was measured. These pin the keys, the defaults and the step-list text as they are.

#include <gtest/gtest.h>

#include "calibration_codec.h"

#include <cmath>
#include <map>
#include <set>
#include <string>

using namespace calibration_codec;

namespace
{

constexpr double RATE_UNCALIBRATED = 123e4; // CALIBRATION_RATE_UNCALIBRATED (scope.h)

// A profile under one mount's calibration prefix, recording each key read and written
class MapStore : public Store
{
public:
    std::map<std::string, std::string> strings;
    std::map<std::string, double> doubles;
    std::map<std::string, int> ints;
    std::set<std::string> read;
    std::set<std::string> written;

    std::string GetString(const std::string& key, const std::string& dflt) override
    {
        read.insert(key);
        auto it = strings.find(key);
        return it == strings.end() ? dflt : it->second;
    }
    double GetDouble(const std::string& key, double dflt) override
    {
        read.insert(key);
        auto it = doubles.find(key);
        return it == doubles.end() ? dflt : it->second;
    }
    int GetInt(const std::string& key, int dflt) override
    {
        read.insert(key);
        auto it = ints.find(key);
        return it == ints.end() ? dflt : it->second;
    }
    void SetString(const std::string& key, const std::string& value) override
    {
        written.insert(key);
        strings[key] = value;
    }
    void SetDouble(const std::string& key, double value) override
    {
        written.insert(key);
        doubles[key] = value;
    }
    void SetInt(const std::string& key, int value) override
    {
        written.insert(key);
        ints[key] = value;
    }
};

CalibrationRecord Sample()
{
    CalibrationRecord c;
    c.xRate = 0.0123;
    c.yRate = RATE_UNCALIBRATED;
    c.xAngle = -2.5;
    c.yAngle = -0.9;
    c.declination = -0.3;
    c.rotatorAngle = ROTATOR_POSITION_UNKNOWN;
    c.binning = 2;
    c.pierSide = PIER_SIDE_WEST;
    c.raGuideParity = GUIDE_PARITY_ODD;
    c.decGuideParity = GUIDE_PARITY_EVEN;
    c.isValid = true;
    return c;
}

DetailsRecord SampleDetails()
{
    DetailsRecord d;
    d.focalLength = 400;
    d.imageScale = 1.934;
    d.raGuideSpeed = 0.5 * 15.0 / 3600.0;
    d.decGuideSpeed = 0.5 * 15.0 / 3600.0;
    d.orthoError = 1.25;
    d.origBinning = 2;
    d.raSteps = { { 1.5, -2.0 }, { 3.0, 4.0 } };
    d.decSteps = { { -0.5, 7.5 } };
    d.raStepCount = 12;
    d.decStepCount = 9;
    d.lastIssue = 3;
    d.origTimestamp = "Sun Nov 22 21:04:05 2026";
    d.origPierSide = PIER_SIDE_EAST;
    return d;
}

} // namespace

TEST(CalibrationCodec, CalibrationRoundTripsFieldForField)
{
    MapStore store;
    SaveCalibration(store, Sample(), "Sun Nov 22 21:04:05 2026");

    CalibrationRecord got;
    ASSERT_TRUE(LoadCalibration(store, &got));

    CalibrationRecord want = Sample();
    EXPECT_EQ(got.xRate, want.xRate);
    EXPECT_EQ(got.yRate, want.yRate);
    EXPECT_EQ(got.xAngle, want.xAngle);
    EXPECT_EQ(got.yAngle, want.yAngle);
    EXPECT_EQ(got.declination, want.declination);
    EXPECT_EQ(got.rotatorAngle, want.rotatorAngle);
    EXPECT_EQ(got.binning, want.binning);
    EXPECT_EQ(got.pierSide, want.pierSide);
    EXPECT_EQ(got.raGuideParity, want.raGuideParity);
    EXPECT_EQ(got.decGuideParity, want.decGuideParity);
    EXPECT_TRUE(got.isValid);
    EXPECT_EQ(got.timestamp, "Sun Nov 22 21:04:05 2026");
}

TEST(CalibrationCodec, UnknownDeclinationAndRotatorRoundTrip)
{
    CalibrationRecord c = Sample();
    c.declination = UNKNOWN_DEC;
    c.rotatorAngle = 12.5;
    c.pierSide = PIER_SIDE_UNKNOWN;
    c.raGuideParity = GUIDE_PARITY_UNKNOWN;

    MapStore store;
    SaveCalibration(store, c, "t");
    CalibrationRecord got;
    ASSERT_TRUE(LoadCalibration(store, &got));
    EXPECT_EQ(got.declination, UNKNOWN_DEC);
    EXPECT_EQ(got.rotatorAngle, 12.5);
    EXPECT_EQ(got.pierSide, PIER_SIDE_UNKNOWN);
    EXPECT_EQ(got.raGuideParity, GUIDE_PARITY_UNKNOWN);
}

TEST(CalibrationCodec, KeysReadAreTheKeysWritten)
{
    MapStore store;
    SaveCalibration(store, Sample(), "t");
    CalibrationRecord got;
    ASSERT_TRUE(LoadCalibration(store, &got));
    EXPECT_EQ(store.read, store.written);
    EXPECT_EQ(store.written.size(), 11u);
}

TEST(CalibrationCodec, NoTimestampIsNoSavedCalibrationAndLeavesTheRecordAlone)
{
    MapStore store;
    SaveCalibration(store, Sample(), "t");
    store.strings.erase("timestamp");

    CalibrationRecord got;
    got.xRate = 42.;
    got.isValid = true;
    EXPECT_FALSE(LoadCalibration(store, &got));
    EXPECT_EQ(got.xRate, 42.);
    EXPECT_TRUE(got.isValid); // Mount::GetLastCalibration sets isValid false itself
    EXPECT_EQ(store.read, std::set<std::string> { "timestamp" });

    store.strings["timestamp"] = "";
    EXPECT_FALSE(LoadCalibration(store, &got));
}

TEST(CalibrationCodec, MissingKeysTakeTheirDefaults)
{
    // A calibration with only a timestamp. The declination default is 0 (the equator), not
    // UNKNOWN_DECLINATION, and yAngle's is 0, not pi/2: pinned as they are.
    MapStore store;
    store.strings["timestamp"] = "t";

    CalibrationRecord got;
    ASSERT_TRUE(LoadCalibration(store, &got));
    EXPECT_EQ(got.xRate, 1.0);
    EXPECT_EQ(got.yRate, 1.0);
    EXPECT_EQ(got.binning, 1);
    EXPECT_EQ(got.xAngle, 0.0);
    EXPECT_EQ(got.yAngle, 0.0);
    EXPECT_EQ(got.declination, 0.0);
    EXPECT_EQ(got.pierSide, PIER_SIDE_UNKNOWN);
    EXPECT_EQ(got.raGuideParity, GUIDE_PARITY_UNKNOWN);
    EXPECT_EQ(got.decGuideParity, GUIDE_PARITY_UNKNOWN);
    EXPECT_EQ(got.rotatorAngle, ROTATOR_POSITION_UNKNOWN);
    EXPECT_TRUE(got.isValid);
}

TEST(CalibrationCodec, OutOfRangePierSideAndParityReadAsUnknown)
{
    MapStore store;
    SaveCalibration(store, Sample(), "t");
    store.ints["pierSide"] = 5;
    store.ints["raGuideParity"] = 7;
    store.ints["decGuideParity"] = -5; // GUIDE_PARITY_UNCHANGED is not a stored value

    CalibrationRecord got;
    ASSERT_TRUE(LoadCalibration(store, &got));
    EXPECT_EQ(got.pierSide, PIER_SIDE_UNKNOWN);
    EXPECT_EQ(got.raGuideParity, GUIDE_PARITY_UNKNOWN);
    EXPECT_EQ(got.decGuideParity, GUIDE_PARITY_UNKNOWN);
}

TEST(CalibrationCodec, BinningIsReadAsAnUnsignedShort)
{
    MapStore store;
    SaveCalibration(store, Sample(), "t");
    store.ints["binning"] = 65536 + 3;

    CalibrationRecord got;
    ASSERT_TRUE(LoadCalibration(store, &got));
    EXPECT_EQ(got.binning, 3);
}

TEST(CalibrationCodec, StepsFormatWithOneDecimal)
{
    EXPECT_EQ(FormatSteps({ { 1.5, -2.0 }, { 3.0, 4.0 } }), "{1.5 -2.0}, {3.0 4.0}");
    EXPECT_EQ(FormatSteps({ { 1.04, -0.96 } }), "{1.0 -1.0}");
    EXPECT_EQ(FormatSteps({}), "");
}

TEST(CalibrationCodec, StepsParseBack)
{
    std::vector<StepPoint> steps;
    bool err = false;
    ParseSteps("{1.5 -2.0}, {3.0 4.0}", &steps, &err);
    EXPECT_FALSE(err);
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0].x, 1.5);
    EXPECT_EQ(steps[0].y, -2.0);
    EXPECT_EQ(steps[1].x, 3.0);
    EXPECT_EQ(steps[1].y, 4.0);

    ParseSteps("", &steps, &err);
    EXPECT_FALSE(err);
    EXPECT_TRUE(steps.empty());
}

TEST(CalibrationCodec, MalformedStepsGiveTheStepsBeforeThem)
{
    std::vector<StepPoint> steps;
    bool err = false;
    ParseSteps("{1.0 2.0}, {abc", &steps, &err);
    EXPECT_TRUE(err);
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].x, 1.0);
    EXPECT_EQ(steps[0].y, 2.0);

    // no blank: x parses from the rest, y is the whole token and does not
    err = false;
    ParseSteps("{1.0}", &steps, &err);
    EXPECT_TRUE(err);
    EXPECT_TRUE(steps.empty());

    // trailing text after a number fails it
    err = false;
    ParseSteps("{1.0 2.0x}", &steps, &err);
    EXPECT_TRUE(err);
    EXPECT_TRUE(steps.empty());

    // a run of blanks only
    err = false;
    ParseSteps("{1.0 2.0},   ,{3.0 4.0}", &steps, &err);
    EXPECT_TRUE(err);
    EXPECT_EQ(steps.size(), 1u);
}

TEST(CalibrationCodec, ParseStepsParsesNothingAfterAnEarlierError)
{
    std::vector<StepPoint> steps = { { 9., 9. } };
    bool err = true;
    ParseSteps("{1.0 2.0}", &steps, &err);
    EXPECT_TRUE(steps.empty());
}

TEST(CalibrationCodec, DetailsRoundTripFieldForField)
{
    MapStore store;
    SaveDetails(store, SampleDetails(), true);

    DetailsRecord got;
    EXPECT_FALSE(LoadDetails(store, &got, nullptr));

    DetailsRecord want = SampleDetails();
    EXPECT_EQ(got.focalLength, want.focalLength);
    EXPECT_EQ(got.imageScale, want.imageScale);
    EXPECT_EQ(got.raGuideSpeed, want.raGuideSpeed);
    EXPECT_EQ(got.decGuideSpeed, want.decGuideSpeed);
    EXPECT_EQ(got.orthoError, want.orthoError);
    EXPECT_EQ(got.origBinning, want.origBinning);
    EXPECT_EQ(got.raStepCount, want.raStepCount);
    EXPECT_EQ(got.decStepCount, want.decStepCount);
    EXPECT_EQ(got.lastIssue, want.lastIssue);
    EXPECT_EQ(got.origTimestamp, want.origTimestamp);
    EXPECT_EQ(got.origPierSide, want.origPierSide);
    ASSERT_EQ(got.raSteps.size(), 2u);
    EXPECT_EQ(got.raSteps[1].x, 3.0);
    EXPECT_EQ(got.raSteps[1].y, 4.0);
    ASSERT_EQ(got.decSteps.size(), 1u);
    EXPECT_EQ(got.decSteps[0].x, -0.5);
    EXPECT_EQ(got.decSteps[0].y, 7.5);

    EXPECT_EQ(store.read, store.written);
    EXPECT_EQ(store.written.size(), 13u);
}

TEST(CalibrationCodec, MissingDetailsTakeTheirDefaults)
{
    MapStore store;
    DetailsRecord got;
    EXPECT_FALSE(LoadDetails(store, &got, nullptr));
    EXPECT_EQ(got.focalLength, 0);
    EXPECT_EQ(got.imageScale, 1.0);
    EXPECT_EQ(got.raGuideSpeed, -1.0);
    EXPECT_EQ(got.decGuideSpeed, -1.0);
    EXPECT_EQ(got.orthoError, 0.0);
    EXPECT_EQ(got.origBinning, 1);
    EXPECT_EQ(got.raStepCount, 0);
    EXPECT_EQ(got.decStepCount, 0);
    EXPECT_EQ(got.lastIssue, 0);
    EXPECT_EQ(got.origTimestamp, "Unknown");
    EXPECT_EQ(got.origPierSide, PIER_SIDE_UNKNOWN);
    EXPECT_TRUE(got.raSteps.empty());
    EXPECT_TRUE(got.decSteps.empty());
}

TEST(CalibrationCodec, MalformedRaStepsSkipTheDecSteps)
{
    // LoadCalibrationDetails shares one error flag between the lists: Dec steps after a bad RA list are not read
    MapStore store;
    SaveDetails(store, SampleDetails(), true);
    store.strings["ra_steps"] = "{1.0 2.0}, {abc";
    store.ints["orig_pierside"] = 9;

    DetailsRecord got;
    LoadDetails(store, &got, nullptr);
    EXPECT_EQ(got.raSteps.size(), 1u);
    EXPECT_TRUE(got.decSteps.empty());
    EXPECT_EQ(got.origPierSide, PIER_SIDE_UNKNOWN);
}

TEST(CalibrationCodec, BogusGuideSpeedsAreSavedAsUnknown)
{
    MapStore store;
    SaveDetails(store, SampleDetails(), false);
    EXPECT_EQ(store.doubles["ra_guide_rate"], -1.0);
    EXPECT_EQ(store.doubles["dec_guide_rate"], -1.0);
}

TEST(CalibrationCodec, BogusSavedGuideSpeedsAreClearedOnLoad)
{
    MapStore store;
    SaveDetails(store, SampleDetails(), true);

    int calls = 0;
    auto reject = [&calls](double, double)
    {
        ++calls;
        return false;
    };
    DetailsRecord got;
    EXPECT_TRUE(LoadDetails(store, &got, reject));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(got.raGuideSpeed, -1.0);
    EXPECT_EQ(got.decGuideSpeed, -1.0);
    EXPECT_EQ(store.doubles["ra_guide_rate"], -1.0);
    EXPECT_EQ(store.doubles["dec_guide_rate"], -1.0);

    // accepted speeds stay
    MapStore ok;
    SaveDetails(ok, SampleDetails(), true);
    EXPECT_FALSE(LoadDetails(ok, &got, [](double, double) { return true; }));
    EXPECT_EQ(got.raGuideSpeed, SampleDetails().raGuideSpeed);
}

TEST(CalibrationCodec, GuideSpeedsAreNotCheckedWhenEitherIsUnknown)
{
    MapStore store;
    DetailsRecord d = SampleDetails();
    d.decGuideSpeed = -1.0;
    SaveDetails(store, d, true);

    int calls = 0;
    DetailsRecord got;
    EXPECT_FALSE(LoadDetails(store, &got,
                             [&calls](double, double)
                             {
                                 ++calls;
                                 return false;
                             }));
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(got.raGuideSpeed, d.raGuideSpeed);
}
