/*
 *  drift_tool.h
 *  PHD Guiding
 *
 *  Created by Andy Galasso
 *  Copyright (c) 2013 Andy Galasso
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
#ifndef DRIFT_TOOL_H
#define DRIFT_TOOL_H

class DriftTool
{
    DriftTool(); // not implemented
public:
    static wxWindow *CreateDriftToolWindow();

    // Headless/API access (event_server): drive the tool without showing it.
    // DriftToolWin lives entirely in drift_tool.cpp, so the RPC layer goes
    // through these accessors; JSON marshalling stays in event_server.
    enum ApiPhase
    {
        API_PHASE_AZIMUTH = 0,
        API_PHASE_ALTITUDE = 1,
    };
    enum ApiMode
    {
        API_MODE_IDLE = 0,
        API_MODE_DRIFT = 1,
        API_MODE_ADJUST = 2,
    };
    struct ApiStatus
    {
        bool active = false;
        bool drifting = false;
        int phase = API_PHASE_AZIMUTH;
        int mode = API_MODE_IDLE;
        bool canSlew = false;
        bool slewing = false;
        wxString statusMessage;
    };
    static bool ApiStart(wxString *error); // create the hidden tool (no-op if already active)
    static bool ApiSetPhase(int phase, wxString *error);
    static bool ApiSetMode(int mode, wxString *error);
    static bool ApiGetStatus(ApiStatus *status); // returns false when the tool is not active
    static bool ApiClose(wxString *error); // full restore + teardown (same as the GUI close)
};

#endif
