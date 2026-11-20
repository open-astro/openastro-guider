// FROZEN COPY, DO NOT EDIT: the legacy twin of GuideAlgorithmHysteresis's per-frame math (#142).
//
// Source: src/guide_algorithm_hysteresis.cpp at origin/main 4eac9d9c (blob f59bf193), last
// changed by 1bf9ce60 "code cleanup: use wxClip (#1395)". Line ranges copied, CRLF converted to
// LF and nothing else changed:
//   42-46    the Default* and Max* constants
//   70-167   reset, result, SetMinMove, SetHysteresis, SetAggression
//
// Check it against the source with
//   bash tests/algorithms/legacy/check_twin.sh
// which diffs these ranges of the source at 4eac9d9c against the copy below and must print
// nothing. Everything the copy calls outside these lines is a mechanical stand-in in
// algorithm_legacy.h.

#include "algorithm_legacy.h"

namespace legacy
{

// BEGIN COPY
static const double DefaultMinMove = 0.2;
static const double DefaultHysteresis = 0.1;
static const double DefaultAggression = 0.7;
static const double MaxAggression = 2.0;
static const double MaxHysteresis = 0.99;
void GuideAlgorithmHysteresis::reset(void)
{
    m_lastMove = 0;
}

double GuideAlgorithmHysteresis::result(double input)
{
    double dReturn = (1.0 - m_hysteresis) * input + m_hysteresis * m_lastMove;

    dReturn *= m_aggression;

    if (fabs(input) < m_minMove)
    {
        dReturn = 0.0;
    }

    m_lastMove = dReturn;

    Debug.Write(wxString::Format("GuideAlgorithmHysteresis::Result() returns %.2f from input %.2f\n", dReturn, input));

    return dReturn;
}

bool GuideAlgorithmHysteresis::SetMinMove(double minMove)
{
    bool bError = false;

    try
    {
        if (minMove < 0.0)
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

bool GuideAlgorithmHysteresis::SetHysteresis(double hysteresis)
{
    bool bError = false;

    try
    {
        if (hysteresis < 0.0 || hysteresis > MaxHysteresis)
        {
            throw ERROR_INFO("invalid hysteresis");
        }

        m_hysteresis = hysteresis;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_hysteresis = wxClip(hysteresis, 0., MaxHysteresis);
    }

    pConfig->Profile.SetDouble(GetConfigPath() + "/hysteresis", m_hysteresis);

    return bError;
}

bool GuideAlgorithmHysteresis::SetAggression(double aggression)
{
    bool bError = false;

    try
    {
        if (aggression < 0.0 || aggression > MaxAggression)
        {
            throw ERROR_INFO("invalid aggression");
        }

        m_aggression = aggression;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        m_aggression = DefaultAggression;
    }

    m_lastMove = 0.0;
    pConfig->Profile.SetDouble(GetConfigPath() + "/aggression", m_aggression);

    return bError;
}
// END COPY

// GuideAlgorithmHysteresis::GuideAlgorithmHysteresis with no saved values: each GetDouble
// returns the default
GuideAlgorithmHysteresis::GuideAlgorithmHysteresis()
{
    SetMinMove(DefaultMinMove);
    SetHysteresis(DefaultHysteresis);
    SetAggression(DefaultAggression);
    reset();
}

} // namespace legacy
