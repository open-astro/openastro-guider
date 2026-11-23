/*
 *  guide_algorithm_lowpass.cpp
 *  PHD Guiding
 *
 *  Created by Bret McKee
 *  Copyright (c) 2012 Bret McKee
 *  All rights reserved.
 *
 *  Based upon work by Craig Stark.
 *  Copyright (c) 2006-2010 Craig Stark.
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
 *    Neither the name of Bret McKee, Dad Dog Development,
 *     Craig Stark, Stark Labs nor the names of its
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

#include "phd.h"

#include "guide_algorithm_lowpass_math.h"

static const double DefaultMinMove = 0.2;
static const double DefaultSlopeWeight = 5.0;

GuideAlgorithmLowpass::GuideAlgorithmLowpass(Mount *pMount, GuideAxis axis) : GuideAlgorithm(pMount, axis)
{
    double minMove = pConfig->Profile.GetDouble(GetConfigPath() + "/minMove", DefaultMinMove);
    SetMinMove(minMove);

    double slopeWeight = pConfig->Profile.GetDouble(GetConfigPath() + "/SlopeWeight", DefaultSlopeWeight);
    SetSlopeWeight(slopeWeight);

    m_axisStats = WindowedAxisStats(0); // self-managed window
    m_timeBase = 0;

    reset();
}

GuideAlgorithmLowpass::~GuideAlgorithmLowpass(void) { }

GUIDE_ALGORITHM GuideAlgorithmLowpass::Algorithm() const
{
    return GUIDE_ALGORITHM_LOWPASS;
}
void GuideAlgorithmLowpass::reset(void)
{
    guide_lowpass::Reset(m_axisStats, m_timeBase, HISTORY_SIZE);
}

double GuideAlgorithmLowpass::result(double input)
{
    guide_lowpass::Result r = guide_lowpass::Compute(m_axisStats, m_timeBase, m_slopeWeight, m_minMove, input);

    if (r.clampedToInput)
    {
        Debug.Write(wxString::Format("GuideAlgorithmLowpass::Result() input %.2f is < calculated value %.2f, using input\n",
                                     input, r.computed));
    }

    Debug.Write(wxString::Format("GuideAlgorithmLowpass::Result() returns %.2f from input %.2f\n", r.value, input));

    return r.value;
}

bool GuideAlgorithmLowpass::SetMinMove(double minMove)
{
    bool bError = false;

    try
    {
        if (minMove < 0)
        {
            throw ERROR_INFO("invalid minMove");
        }

        m_minMove = minMove;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_minMove = DefaultMinMove;
    }

    pConfig->Profile.SetDouble(GetConfigPath() + "/minMove", m_minMove);

    return bError;
}

bool GuideAlgorithmLowpass::SetSlopeWeight(double slopeWeight)
{
    bool bError = false;

    try
    {
        if (slopeWeight < 0.0)
        {
            throw ERROR_INFO("invalid slopeWeight");
        }

        m_slopeWeight = slopeWeight;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_slopeWeight = DefaultSlopeWeight;
    }

    pConfig->Profile.SetDouble(GetConfigPath() + "/SlopeWeight", m_slopeWeight);

    return bError;
}

void GuideAlgorithmLowpass::GetParamNames(wxArrayString& names) const
{
    names.push_back("minMove");
    names.push_back("slopeWeight");
}

bool GuideAlgorithmLowpass::GetParam(const wxString& name, double *val) const
{
    bool ok = true;

    if (name == "minMove")
        *val = GetMinMove();
    else if (name == "slopeWeight")
        *val = GetSlopeWeight();
    else
        ok = false;

    return ok;
}

bool GuideAlgorithmLowpass::SetParam(const wxString& name, double val)
{
    bool err;

    if (name == "minMove")
        err = SetMinMove(val);
    else if (name == "slopeWeight")
        err = SetSlopeWeight(val);
    else
        err = true;

    return !err;
}

wxString GuideAlgorithmLowpass::GetSettingsSummary() const
{
    // return a loggable summary of current mount settings
    return wxString::Format("Slope weight = %.3f, Minimum move = %.3f\n", GetSlopeWeight(), GetMinMove());
}
