/*
 *  calibration_codec.h
 *  PHD Guiding
 *
 *  How a mount's calibration and its details are written to and read back
 *  from the profile, factored out of Mount::SetCalibration,
 *  GetLastCalibration, SaveCalibrationDetails and LoadCalibrationDetails so
 *  it can be tested without wxWidgets or a profile (#142). Kept free of wx;
 *  tested by tests/calibration/test_calibration_codec.cpp.
 *
 *  The codec reads and writes through a Store: the keys are the names under
 *  the mount's "/<mount class>/calibration/" profile prefix, and Mount's
 *  Store puts that prefix on and talks to pConfig->Profile. The keys, their
 *  defaults and the order of the reads and writes are the ones Mount used.
 *  Strings are UTF-8.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef CALIBRATION_CODEC_H_INCLUDED
#define CALIBRATION_CODEC_H_INCLUDED

#include <functional>
#include <string>
#include <vector>

namespace calibration_codec
{

// Same values as PierSide (mount.h); mount.cpp asserts it
constexpr int PIER_SIDE_UNKNOWN = -1;
constexpr int PIER_SIDE_EAST = 0;
constexpr int PIER_SIDE_WEST = 1;

// Same values as GuideParity (mount.h); mount.cpp asserts it
constexpr int GUIDE_PARITY_EVEN = 1;
constexpr int GUIDE_PARITY_ODD = -1;
constexpr int GUIDE_PARITY_UNKNOWN = 0;

// UNKNOWN_DECLINATION (mount.h) and Rotator::POSITION_UNKNOWN (rotator.cpp, a float)
constexpr double UNKNOWN_DEC = 997.0;
constexpr double ROTATOR_POSITION_UNKNOWN = -888.f;

// The profile values under one mount's calibration prefix
class Store
{
public:
    virtual ~Store() = default;
    virtual std::string GetString(const std::string& key, const std::string& dflt) = 0;
    virtual double GetDouble(const std::string& key, double dflt) = 0;
    virtual int GetInt(const std::string& key, int dflt) = 0;
    virtual void SetString(const std::string& key, const std::string& value) = 0;
    virtual void SetDouble(const std::string& key, double value) = 0;
    virtual void SetInt(const std::string& key, int value) = 0;
};

// The fields of Calibration (mount.h); pierSide and the parities hold the enum values
struct CalibrationRecord
{
    double xRate = 0.;
    double yRate = 0.;
    double xAngle = 0.;
    double yAngle = 0.;
    double declination = UNKNOWN_DEC;
    double rotatorAngle = 0.;
    unsigned short binning = 1;
    int pierSide = PIER_SIDE_UNKNOWN;
    int raGuideParity = GUIDE_PARITY_UNKNOWN;
    int decGuideParity = GUIDE_PARITY_UNKNOWN;
    bool isValid = false;
    std::string timestamp;
};

struct StepPoint
{
    double x = 0.;
    double y = 0.;
};

// The fields of CalibrationDetails (mount.h); lastIssue holds a CalibrationIssueType
struct DetailsRecord
{
    int focalLength = 0;
    double imageScale = 0.;
    double raGuideSpeed = 0.;
    double decGuideSpeed = 0.;
    double orthoError = 0.;
    int origBinning = 1;
    std::vector<StepPoint> raSteps;
    std::vector<StepPoint> decSteps;
    int raStepCount = 0;
    int decStepCount = 0;
    int lastIssue = 0;
    std::string origTimestamp;
    int origPierSide = PIER_SIDE_UNKNOWN;
};

// A stored pier side or parity, with any value that is not one of the enum's (other than UNCHANGED) read as UNKNOWN
int PierSideFromInt(int val);
int GuideParityFromInt(int val);

// Mount::SetCalibration's write: timestamp, then each field of cal
void SaveCalibration(Store& store, const CalibrationRecord& cal, const std::string& timestamp);

// Mount::GetLastCalibration's read. With no timestamp (missing or empty) it returns false and
// leaves *cal alone; otherwise it fills every field, isValid true, and returns true.
bool LoadCalibration(Store& store, CalibrationRecord *cal);

// The "{x y}, {x y}" text a details step list is saved as, each coordinate with one decimal
std::string FormatSteps(const std::vector<StepPoint>& steps);

// Parses FormatSteps' text into *steps (cleared first), stopping at the first token that does not
// parse; *err is set then. Parsing nothing when *err is already set, as LoadCalibrationDetails reads
// the Dec steps only when the RA steps parsed.
void ParseSteps(const std::string& text, std::vector<StepPoint> *steps, bool *err);

// Mount::SaveCalibrationDetails' write. guideSpeedsOk false writes -1 for both guide speeds.
void SaveDetails(Store& store, const DetailsRecord& details, bool guideSpeedsOk);

// Mount::LoadCalibrationDetails' read. validGuideRates, when set, checks a pair of saved guide speeds
// that are both not -1; a pair it rejects is read and written back as -1, and LoadDetails returns
// true for the caller's Debug line.
bool LoadDetails(Store& store, DetailsRecord *details, const std::function<bool(double, double)>& validGuideRates);

} // namespace calibration_codec

#endif // CALIBRATION_CODEC_H_INCLUDED
