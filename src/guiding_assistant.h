/*
 *  guiding_assistant.h
 *  PHD Guiding
 *
 *  Created by Andy Galasso
 *  Copyright (c) 2015 Andy Galasso
 *  All rights reserved.
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

#ifndef GUIDING_ASSISTANT_INCLUDED
#define GUIDING_ASSISTANT_INCLUDED

class GuidingAssistant
{
    GuidingAssistant(); // not implemented
public:
    static wxWindow *CreateDialogBox();
    static void NotifyGuideStep(const GuideStepInfo& info);
    static void NotifyFrameDropped(const FrameDroppedInfo& info);
    static void NotifyBacklashStep(const PHD_Point& camLoc);
    static void NotifyBacklashError();
    static void UpdateUIControls();

    // Headless Guiding Assistant, driven over JSON-RPC (guiding_assistant_*).
    // Runs the same measurement as the dialog without showing it: guide
    // output is disabled while the star drifts, the unguided samples feed the
    // shared ga_math statistics, an optional Dec backlash test follows, and
    // the dialog's recommendations are computed at the end.
    enum ApiState
    {
        API_IDLE,
        API_MEASURING,
        API_MEASURING_BACKLASH,
        API_DONE,
        API_FAILED
    };

    struct ApiMessage
    {
        wxString id; // "Exp", "Bin", "Cal", "Star", "PA", "StarHFD", "RAMinMove", "DecMinMove", "BLT", "DecAlgo"
        wxString text;
    };

    struct ApiRecommendations
    {
        double minMoveRaPx = 0.;
        double minMoveDecPx = 0.;
        bool minMoveSeeingBased = false;
        double exposureMinS = 0.;
        double exposureMaxS = 0.;
        double driftLimitingExposureS = 0.; // 0 = n/a
        bool hasDecBacklashMs = false;
        int decBacklashMs = 0;
        wxString decGuideMode; // empty unless the backlash is too large to compensate
        wxString decAlgorithm; // empty unless Lowpass2 is recommended for Dec
        std::vector<ApiMessage> messages; // the dialog's recommendation texts, in display order
    };

    struct ApiStatus
    {
        ApiState state = API_IDLE;
        double durationS = 0.;
        bool measureBacklash = false;
        double elapsedS = 0.;
        unsigned int samples = 0;
        double pixelScale = 0.;
        bool hasStats = false; // >= 2 samples
        double raDriftPxMin = 0.;
        double decDriftPxMin = 0.;
        double raRmsPx = 0.;
        double decRmsPx = 0.;
        double highFreqRmsPx = 0.;
        double raPeakPx = 0.;
        double decPeakPx = 0.;
        double raPeakPeakPx = 0.;
        double raMaxDriftPxSec = 0.;
        double snr = 0.;
        double starMass = 0.;
        double polarAlignErrorArcmin = 0.;
        bool polarAlignErrorIsLowerBound = false; // declination unknown
        wxString backlashStatus; // backlash-test progress text
        wxString backlashResult; // "valid" / "impaired" / "aborted" once the backlash test has ended
        bool hasBacklash = false;
        int backlashMs = 0;
        double backlashSigmaMs = 0.;
        double backlashPx = 0.;
        bool hasRecommendations = false;
        ApiRecommendations recommendations;
        wxString error;
    };

    static const char *ApiStateName(ApiState state);
    // durationS: seconds of unguided sampling before results (and the backlash test)
    static bool ApiStart(double durationS, bool measureBacklash, wxString *error);
    static bool ApiStop(wxString *error);
    static void ApiGetStatus(ApiStatus *status);
    static bool ApiIsRunning();
    // which: "min_move_ra", "min_move_dec", "exposure", "dec_backlash", "dec_guide_mode", "dec_algorithm"
    static bool ApiApply(const std::vector<wxString>& which, std::vector<wxString> *applied, wxString *error);
    // App closing: restore what a running headless run changed, without driving the guider or gear
    static void ApiShutdown();
    // A camera or mount connected (GearDialog); results from before can no longer be applied
    static void NotifyGearConnected();
};

#endif
