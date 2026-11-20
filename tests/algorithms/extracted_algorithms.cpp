// algo_test::Extracted*: GuideAlgorithmHysteresis and GuideAlgorithmResistSwitch as production
// now has them (#142), with the profile writes dropped and Debug, THROW_INFO and ERROR_INFO
// recorded as the twin's stand-ins record them. result() and reset() are the production bodies
// over the real guide_algorithm_kernels.cpp; the constructors and setters are the production
// ones (src/guide_algorithm_hysteresis.cpp, src/guide_algorithm_resistswitch.cpp), which the
// extraction left alone apart from naming the kernel state.

#include "algorithm_impls.h"

#include <cmath>
#include <cstdio>

namespace algo_test
{

namespace
{
    template<class... A>
    std::string Format(const char *fmt, const A&...args)
    {
        int n = std::snprintf(nullptr, 0, fmt, args...);
        if (n <= 0)
            return std::string();
        std::string buf((size_t) n + 1, '\0');
        std::snprintf(buf.data(), buf.size(), fmt, args...);
        buf.resize((size_t) n);
        return buf;
    }

    const double HysteresisDefaultMinMove = 0.2;
    const double HysteresisDefaultHysteresis = 0.1;
    const double HysteresisDefaultAggression = 0.7;
    const double HysteresisMaxAggression = 2.0;
    const double HysteresisMaxHysteresis = 0.99;

    const double ResistSwitchDefaultMinMove = 0.2;
    const double ResistSwitchDefaultAggression = 1.0;
} // namespace

ExtractedHysteresis::ExtractedHysteresis()
{
    SetMinMove(HysteresisDefaultMinMove);
    SetHysteresis(HysteresisDefaultHysteresis);
    SetAggression(HysteresisDefaultAggression);
    reset();
}

void ExtractedHysteresis::reset()
{
    m_state.lastMove = 0;
}

double ExtractedHysteresis::result(double input)
{
    double dReturn = guide_algorithm_kernels::HysteresisResult(m_state, { m_minMove, m_hysteresis, m_aggression }, input);

    m_log.push_back(Format("GuideAlgorithmHysteresis::Result() returns %.2f from input %.2f\n", dReturn, input));

    return dReturn;
}

bool ExtractedHysteresis::SetMinMove(double minMove)
{
    if (minMove < 0.0)
    {
        m_log.push_back("Error->invalid minMove");
        m_minMove = HysteresisDefaultMinMove;
        return true;
    }
    m_minMove = minMove;
    return false;
}

bool ExtractedHysteresis::SetHysteresis(double hysteresis)
{
    if (hysteresis < 0.0 || hysteresis > HysteresisMaxHysteresis)
    {
        m_log.push_back("Error->invalid hysteresis");
        // wxClip(hysteresis, 0., MaxHysteresis)
        m_hysteresis = hysteresis < 0. ? 0. : (hysteresis > HysteresisMaxHysteresis ? HysteresisMaxHysteresis : hysteresis);
        return true;
    }
    m_hysteresis = hysteresis;
    return false;
}

bool ExtractedHysteresis::SetAggression(double aggression)
{
    bool bError = false;
    if (aggression < 0.0 || aggression > HysteresisMaxAggression)
    {
        m_log.push_back("Error->invalid aggression");
        bError = true;
        m_aggression = HysteresisDefaultAggression;
    }
    else
        m_aggression = aggression;

    m_state.lastMove = 0.0;
    return bError;
}

ExtractedResistSwitch::ExtractedResistSwitch()
{
    SetMinMove(ResistSwitchDefaultMinMove);
    SetAggression(ResistSwitchDefaultAggression);
    SetFastSwitchEnabled(true);
    reset();
}

void ExtractedResistSwitch::reset()
{
    guide_algorithm_kernels::ResistSwitchReset(m_state);
}

double ExtractedResistSwitch::result(double input)
{
    using guide_algorithm_kernels::ResistSwitchVeto;

    guide_algorithm_kernels::ResistSwitchStep step =
        guide_algorithm_kernels::ResistSwitchResult(m_state, { m_minMove, m_aggression, m_fastSwitchEnabled }, input);

    if (step.fastSwitched)
    {
        m_log.push_back(Format("resist switch: large excursion: input %.2f thresh %.2f direction from %d to %d\n", input,
                               step.thresh, step.fastSwitchFrom, step.fastSwitchTo));
    }

    if (step.switched)
    {
        m_log.push_back(Format("switching direction from %d to %d - decHistory=%d oldest=%.2f newest=%.2f\n", step.switchFrom,
                               step.switchTo, step.decHistory, step.oldest, step.newest));
    }

    switch (step.veto)
    {
    case ResistSwitchVeto::None:
        break;
    case ResistSwitchVeto::BelowMinMove:
        m_log.push_back("Throw->input < m_minMove");
        break;
    case ResistSwitchVeto::NotCompelling:
        m_log.push_back("Throw->not compelling enough");
        break;
    case ResistSwitchVeto::NotGettingWorse:
        m_log.push_back("Throw->Not getting worse");
        break;
    case ResistSwitchVeto::Overshoot:
        m_log.push_back("Throw->must have overshot -- vetoing move");
        break;
    }

    m_log.push_back(Format("GuideAlgorithmResistSwitch::result() returns %.2f from input %.2f\n", step.result, input));

    return step.result;
}

bool ExtractedResistSwitch::SetMinMove(double minMove)
{
    bool bError = false;
    if (minMove <= 0.0)
    {
        m_log.push_back("Error->invalid minMove");
        bError = true;
        m_minMove = ResistSwitchDefaultMinMove;
    }
    else
    {
        m_minMove = minMove;
        m_state.currentSide = 0;
    }

    m_log.push_back(Format("GuideAlgorithmResistSwitch::SetMinMove() returns %d, m_minMove=%.2f\n", bError, m_minMove));
    return bError;
}

bool ExtractedResistSwitch::SetAggression(double aggr)
{
    bool bError = false;
    if (aggr < 0.0 || aggr > 1.0)
    {
        m_log.push_back("Error->invalid aggression");
        bError = true;
        m_aggression = ResistSwitchDefaultAggression;
    }
    else
        m_aggression = aggr;

    m_log.push_back(
        Format("GuideAlgorithmResistSwitch::SetAggression() returns %d, m_aggression=%.2f\n", bError, m_aggression));
    return bError;
}

void ExtractedResistSwitch::SetFastSwitchEnabled(bool enable)
{
    m_fastSwitchEnabled = enable;
    m_log.push_back(Format("GuideAlgorithmResistSwitch::SetFastSwitchEnabled(%d)\n", m_fastSwitchEnabled));
}

} // namespace algo_test
