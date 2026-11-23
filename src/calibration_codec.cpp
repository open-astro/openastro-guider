/*
 *  calibration_codec.cpp
 *  PHD Guiding
 *
 *  The calibration's profile codec. See calibration_codec.h. A lift of the
 *  profile reads and writes of Mount::SetCalibration, GetLastCalibration,
 *  SaveCalibrationDetails and LoadCalibrationDetails, with the same keys,
 *  defaults and order; the step-list text is formatted and parsed the way
 *  wxString::Format, wxStringTokenizer (wxTOKEN_STRTOK), wxString::Trim,
 *  Mid and ToDouble did it.
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

#include "calibration_codec.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace calibration_codec
{

int PierSideFromInt(int val)
{
    return val == PIER_SIDE_EAST ? PIER_SIDE_EAST : val == PIER_SIDE_WEST ? PIER_SIDE_WEST : PIER_SIDE_UNKNOWN;
}

int GuideParityFromInt(int val)
{
    switch (val)
    {
    case GUIDE_PARITY_EVEN:
        return GUIDE_PARITY_EVEN;
    case GUIDE_PARITY_ODD:
        return GUIDE_PARITY_ODD;
    default:
        return GUIDE_PARITY_UNKNOWN;
    }
}

void SaveCalibration(Store& store, const CalibrationRecord& cal, const std::string& timestamp)
{
    store.SetString("timestamp", timestamp);
    store.SetDouble("xAngle", cal.xAngle);
    store.SetDouble("yAngle", cal.yAngle);
    store.SetDouble("xRate", cal.xRate);
    store.SetDouble("yRate", cal.yRate);
    store.SetInt("binning", cal.binning);
    store.SetDouble("declination", cal.declination);
    store.SetInt("pierSide", cal.pierSide);
    store.SetInt("raGuideParity", cal.raGuideParity);
    store.SetInt("decGuideParity", cal.decGuideParity);
    store.SetDouble("rotatorAngle", cal.rotatorAngle);
}

bool LoadCalibration(Store& store, CalibrationRecord *cal)
{
    std::string timestamp = store.GetString("timestamp", std::string());
    if (timestamp.empty())
        return false;

    cal->xRate = store.GetDouble("xRate", 1.0);
    cal->yRate = store.GetDouble("yRate", 1.0);
    cal->binning = (unsigned short) store.GetInt("binning", 1);
    cal->xAngle = store.GetDouble("xAngle", 0.0);
    cal->yAngle = store.GetDouble("yAngle", 0.0);
    cal->declination = store.GetDouble("declination", 0.0);
    cal->pierSide = PierSideFromInt(store.GetInt("pierSide", PIER_SIDE_UNKNOWN));
    cal->raGuideParity = GuideParityFromInt(store.GetInt("raGuideParity", GUIDE_PARITY_UNKNOWN));
    cal->decGuideParity = GuideParityFromInt(store.GetInt("decGuideParity", GUIDE_PARITY_UNKNOWN));
    cal->rotatorAngle = store.GetDouble("rotatorAngle", ROTATOR_POSITION_UNKNOWN);
    cal->timestamp = timestamp;
    cal->isValid = true;
    return true;
}

std::string FormatSteps(const std::vector<StepPoint>& steps)
{
    std::string s;
    for (const StepPoint& p : steps)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{%0.1f %0.1f}, ", p.x, p.y);
        s += buf;
    }
    // wxString::Left(length - 2): the whole (empty) string when there are no steps
    if (s.size() >= 2)
        s.resize(s.size() - 2);
    return s;
}

namespace
{

    // wxSafeIsspace: the ASCII white space wxString::Trim removes
    bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    }

    // wxString::Mid(first, count) with count npos (or past the end) meaning the rest
    std::string Mid(const std::string& s, size_t first, size_t count = std::string::npos)
    {
        if (first > s.size())
            return std::string();
        return s.substr(first, count);
    }

    // wxString::ToDouble: the whole, non-empty string is a number in range
    bool ToDouble(const std::string& s, double *val)
    {
        errno = 0;
        const char *start = s.c_str();
        char *end;
        *val = std::strtod(start, &end);
        return *end == '\0' && end != start && errno != ERANGE;
    }

} // namespace

void ParseSteps(const std::string& text, std::vector<StepPoint> *steps, bool *err)
{
    steps->clear();

    size_t pos = 0;
    while (!*err)
    {
        // wxStringTokenizer(text, "},", wxTOKEN_STRTOK): runs of characters between delimiters, empty runs skipped
        pos = text.find_first_not_of("},", pos);
        if (pos == std::string::npos)
            break;
        size_t endPos = text.find_first_of("},", pos);
        std::string tk = text.substr(pos, endPos == std::string::npos ? std::string::npos : endPos - pos);
        pos = endPos;

        // looks like {x y, left-trimmed
        size_t first = 0;
        while (first < tk.size() && IsSpace(tk[first]))
            ++first;
        tk.erase(0, first);

        // int blankLoc = tk.find(" "): -1 when there is no blank, so Mid(1, -2) is the rest and Mid(0) all of it
        int blankLoc = (int) tk.find(' ');
        double x;
        double y;
        if (!ToDouble(Mid(tk, 1, (size_t) (blankLoc - 1)), &x))
            *err = true;
        tk = Mid(tk, (size_t) (blankLoc + 1));
        if (!ToDouble(tk, &y))
            *err = true;
        if (!*err)
            steps->push_back(StepPoint { x, y });

        if (pos == std::string::npos)
            break;
    }
}

void SaveDetails(Store& store, const DetailsRecord& details, bool guideSpeedsOk)
{
    store.SetInt("focal_length", details.focalLength);
    store.SetDouble("image_scale", details.imageScale);
    if (guideSpeedsOk)
    {
        store.SetDouble("ra_guide_rate", details.raGuideSpeed);
        store.SetDouble("dec_guide_rate", details.decGuideSpeed);
    }
    else
    {
        store.SetDouble("ra_guide_rate", -1.0);
        store.SetDouble("dec_guide_rate", -1.0);
    }
    store.SetDouble("ortho_error", details.orthoError);
    store.SetInt("orig_binning", details.origBinning);
    store.SetString("orig_timestamp", details.origTimestamp);
    store.SetInt("orig_pierside", details.origPierSide);

    store.SetString("ra_steps", FormatSteps(details.raSteps));
    store.SetString("dec_steps", FormatSteps(details.decSteps));

    store.SetInt("ra_step_count", details.raStepCount);
    store.SetInt("dec_step_count", details.decStepCount);
    store.SetInt("last_issue", details.lastIssue);
}

bool LoadDetails(Store& store, DetailsRecord *details, const std::function<bool(double, double)>& validGuideRates)
{
    details->focalLength = store.GetInt("focal_length", 0);
    details->imageScale = store.GetDouble("image_scale", 1.0);
    details->raGuideSpeed = store.GetDouble("ra_guide_rate", -1.0);
    details->decGuideSpeed = store.GetDouble("dec_guide_rate", -1.0);
    details->orthoError = store.GetDouble("ortho_error", 0.0);
    details->raStepCount = store.GetInt("ra_step_count", 0);
    details->decStepCount = store.GetInt("dec_step_count", 0);
    details->origBinning = store.GetInt("orig_binning", 1);
    details->lastIssue = store.GetInt("last_issue", 0);
    details->origTimestamp = store.GetString("orig_timestamp", "Unknown");
    details->origPierSide = PierSideFromInt(store.GetInt("orig_pierside", PIER_SIDE_UNKNOWN));

    bool cleared = false;
    if (validGuideRates)
    {
        if (!(details->raGuideSpeed == -1.0 || details->decGuideSpeed == -1.0))
        {
            if (!validGuideRates(details->raGuideSpeed, details->decGuideSpeed))
            {
                details->raGuideSpeed = -1.0;
                details->decGuideSpeed = -1.0;
                store.SetDouble("ra_guide_rate", -1.0);
                store.SetDouble("dec_guide_rate", -1.0);
                // Need to prevent old bogus values in registry from being propagated
                cleared = true;
            }
        }
    }

    bool err = false;
    ParseSteps(store.GetString("ra_steps", ""), &details->raSteps, &err);
    ParseSteps(store.GetString("dec_steps", ""), &details->decSteps, &err);

    return cleared;
}

} // namespace calibration_codec
