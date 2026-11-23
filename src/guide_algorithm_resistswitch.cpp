/*
 *  guide_algorithm_resistswitch.cpp
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

static const double DefaultMinMove = 0.2;
static const double DefaultAggression = 1.0;

GuideAlgorithmResistSwitch::GuideAlgorithmResistSwitch(Mount *pMount, GuideAxis axis) : GuideAlgorithm(pMount, axis)
{
    double minMove = pConfig->Profile.GetDouble(GetConfigPath() + "/minMove", DefaultMinMove);
    SetMinMove(minMove);

    double aggr = pConfig->Profile.GetDouble(GetConfigPath() + "/aggression", DefaultAggression);
    SetAggression(aggr);

    bool enable = pConfig->Profile.GetBoolean(GetConfigPath() + "/fastSwitch", true);
    SetFastSwitchEnabled(enable);

    reset();
}

GuideAlgorithmResistSwitch::~GuideAlgorithmResistSwitch(void) { }

GUIDE_ALGORITHM GuideAlgorithmResistSwitch::Algorithm() const
{
    return GUIDE_ALGORITHM_RESIST_SWITCH;
}
void GuideAlgorithmResistSwitch::reset(void)
{
    guide_algorithm_kernels::ResistSwitchReset(m_state);
}

double GuideAlgorithmResistSwitch::result(double input)
{
    using guide_algorithm_kernels::ResistSwitchVeto;

    // the math is in guide_algorithm_kernels.cpp (#142); the Debug lines are written here in
    // the order the step made them
    guide_algorithm_kernels::ResistSwitchStep step =
        guide_algorithm_kernels::ResistSwitchResult(m_state, { m_minMove, m_aggression, m_fastSwitchEnabled }, input);

    if (step.fastSwitched)
    {
        Debug.Write(wxString::Format("resist switch: large excursion: input %.2f thresh %.2f direction from %d to %d\n", input,
                                     step.thresh, step.fastSwitchFrom, step.fastSwitchTo));
    }

    if (step.switched)
    {
        Debug.Write(wxString::Format("switching direction from %d to %d - decHistory=%d oldest=%.2f newest=%.2f\n",
                                     step.switchFrom, step.switchTo, step.decHistory, step.oldest, step.newest));
    }

    switch (step.veto)
    {
    case ResistSwitchVeto::None:
        break;
    case ResistSwitchVeto::BelowMinMove:
        THROW_INFO("input < m_minMove");
        break;
    case ResistSwitchVeto::NotCompelling:
        THROW_INFO("not compelling enough");
        break;
    case ResistSwitchVeto::NotGettingWorse:
        THROW_INFO("Not getting worse");
        break;
    case ResistSwitchVeto::Overshoot:
        THROW_INFO("must have overshot -- vetoing move");
        break;
    }

    Debug.Write(wxString::Format("GuideAlgorithmResistSwitch::result() returns %.2f from input %.2f\n", step.result, input));

    return step.result;
}

bool GuideAlgorithmResistSwitch::SetMinMove(double minMove)
{
    bool bError = false;

    try
    {
        if (minMove <= 0.0)
        {
            throw ERROR_INFO("invalid minMove");
        }

        m_minMove = minMove;
        m_state.currentSide = 0;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_minMove = DefaultMinMove;
    }

    pConfig->Profile.SetDouble(GetConfigPath() + "/minMove", m_minMove);

    Debug.Write(wxString::Format("GuideAlgorithmResistSwitch::SetMinMove() returns %d, m_minMove=%.2f\n", bError, m_minMove));

    return bError;
}

bool GuideAlgorithmResistSwitch::SetAggression(double aggr)
{
    bool bError = false;

    try
    {
        if (aggr < 0.0 || aggr > 1.0)
        {
            throw ERROR_INFO("invalid aggression");
        }

        m_aggression = aggr;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_aggression = DefaultAggression;
    }

    pConfig->Profile.SetDouble(GetConfigPath() + "/aggression", m_aggression);

    Debug.Write(
        wxString::Format("GuideAlgorithmResistSwitch::SetAggression() returns %d, m_aggression=%.2f\n", bError, m_aggression));

    return bError;
}

void GuideAlgorithmResistSwitch::SetFastSwitchEnabled(bool enable)
{
    m_fastSwitchEnabled = enable;
    pConfig->Profile.SetBoolean(GetConfigPath() + "/fastSwitch", m_fastSwitchEnabled);
    Debug.Write(wxString::Format("GuideAlgorithmResistSwitch::SetFastSwitchEnabled(%d)\n", m_fastSwitchEnabled));
}

void GuideAlgorithmResistSwitch::GetParamNames(wxArrayString& names) const
{
    names.push_back("minMove");
    names.push_back("fastSwitch");
    names.push_back("aggression");
}

bool GuideAlgorithmResistSwitch::GetParam(const wxString& name, double *val) const
{
    bool ok = true;

    if (name == "minMove")
        *val = GetMinMove();
    else if (name == "fastSwitch")
        *val = GetFastSwitchEnabled() ? 1.0 : 0.0;
    else if (name == "aggression")
        *val = GetAggression();
    else
        ok = false;

    return ok;
}

bool GuideAlgorithmResistSwitch::SetParam(const wxString& name, double val)
{
    bool err;

    if (name == "minMove")
        err = SetMinMove(val);
    else if (name == "fastSwitch")
    {
        SetFastSwitchEnabled(val != 0.0);
        err = false;
    }
    else if (name == "aggression")
        err = SetAggression(val);
    else
        err = true;

    return !err;
}

wxString GuideAlgorithmResistSwitch::GetSettingsSummary() const
{
    // return a loggable summary of current mount settings
    return wxString::Format("Minimum move = %.3f Aggression = %.f%% FastSwitch = %s\n", GetMinMove(), GetAggression() * 100.0,
                            GetFastSwitchEnabled() ? "enabled" : "disabled");
}
