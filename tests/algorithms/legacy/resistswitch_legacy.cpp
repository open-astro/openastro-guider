// FROZEN COPY, DO NOT EDIT: the legacy twin of GuideAlgorithmResistSwitch's per-frame math (#142).
//
// Source: src/guide_algorithm_resistswitch.cpp at origin/main 4eac9d9c (blob 563d3283), last
// changed by 9d9a08ec "format all source files with clang-format". Line ranges copied, CRLF
// converted to LF and nothing else changed:
//   42-43    the Default* constants
//   65-240   reset, sign, result, SetMinMove, SetAggression, SetFastSwitchEnabled
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
static const double DefaultAggression = 1.0;
void GuideAlgorithmResistSwitch::reset(void)
{
    m_history.Empty();

    while (m_history.GetCount() < HISTORY_SIZE)
    {
        m_history.Add(0.0);
    }

    m_currentSide = 0;
}

static int sign(double x)
{
    int iReturn = 0;

    if (x > 0.0)
    {
        iReturn = 1;
    }
    else if (x < 0.0)
    {
        iReturn = -1;
    }

    return iReturn;
}

double GuideAlgorithmResistSwitch::result(double input)
{
    double rslt = input;

    m_history.Add(input);
    m_history.RemoveAt(0);

    try
    {
        if (fabs(input) < m_minMove)
        {
            throw THROW_INFO("input < m_minMove");
        }

        if (m_fastSwitchEnabled)
        {
            double thresh = 3.0 * m_minMove;
            if (sign(input) != m_currentSide && fabs(input) > thresh)
            {
                Debug.Write(wxString::Format("resist switch: large excursion: input %.2f thresh %.2f direction from %d to %d\n",
                                             input, thresh, m_currentSide, sign(input)));
                // force switch
                m_currentSide = 0;
                unsigned int i;
                for (i = 0; i < HISTORY_SIZE - 3; i++)
                    m_history[i] = 0.0;
                for (; i < HISTORY_SIZE; i++)
                    m_history[i] = input;
            }
        }

        int decHistory = 0;

        for (unsigned int i = 0; i < m_history.GetCount(); i++)
        {
            if (fabs(m_history[i]) > m_minMove)
            {
                decHistory += sign(m_history[i]);
            }
        }

        if (m_currentSide == 0 || sign(m_currentSide) == -sign(decHistory))
        {
            if (abs(decHistory) < 3)
            {
                throw THROW_INFO("not compelling enough");
            }

            double oldest = 0.0;
            double newest = 0.0;

            for (int i = 0; i < 3; i++)
            {
                oldest += m_history[i];
                newest += m_history[m_history.GetCount() - (i + 1)];
            }

            if (fabs(newest) <= fabs(oldest))
            {
                throw THROW_INFO("Not getting worse");
            }

            Debug.Write(wxString::Format("switching direction from %d to %d - decHistory=%d oldest=%.2f newest=%.2f\n",
                                         m_currentSide, sign(decHistory), decHistory, oldest, newest));

            m_currentSide = sign(decHistory);
        }

        if (m_currentSide != sign(input))
        {
            throw THROW_INFO("must have overshot -- vetoing move");
        }
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        rslt = 0.0;
    }

    rslt *= m_aggression;

    Debug.Write(wxString::Format("GuideAlgorithmResistSwitch::result() returns %.2f from input %.2f\n", rslt, input));

    return rslt;
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
        m_currentSide = 0;
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
// END COPY

// GuideAlgorithmResistSwitch::GuideAlgorithmResistSwitch with no saved values: each
// GetDouble or GetBoolean returns the default
GuideAlgorithmResistSwitch::GuideAlgorithmResistSwitch()
{
    SetMinMove(DefaultMinMove);
    SetAggression(DefaultAggression);
    SetFastSwitchEnabled(true);
    reset();
}

} // namespace legacy
