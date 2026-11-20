// algo_test::Twin*: the frozen legacy copies behind the interface the typed tests use (#142).

#include "../algorithm_impls.h"
#include "algorithm_legacy.h"

namespace legacy
{
std::vector<std::string> *g_log = nullptr;
TwinDebug Debug;
static TwinConfig s_config;
TwinConfig *pConfig = &s_config;
} // namespace legacy

namespace algo_test
{

namespace
{
    // Points the Debug stand-in at one twin's log for the duration of a call
    class Bind
    {
    public:
        explicit Bind(Log *log) : m_prev(legacy::g_log) { legacy::g_log = log; }
        ~Bind() { legacy::g_log = m_prev; }
        Bind(const Bind&) = delete;
        Bind& operator=(const Bind&) = delete;

    private:
        Log *m_prev;
    };
} // namespace

TwinHysteresis::TwinHysteresis()
{
    Bind bind(&m_log);
    m_algo = std::make_unique<legacy::GuideAlgorithmHysteresis>();
}

TwinHysteresis::~TwinHysteresis() = default;

double TwinHysteresis::result(double input)
{
    Bind bind(&m_log);
    return m_algo->result(input);
}

void TwinHysteresis::reset()
{
    Bind bind(&m_log);
    m_algo->reset();
}

bool TwinHysteresis::SetMinMove(double minMove)
{
    Bind bind(&m_log);
    return m_algo->SetMinMove(minMove);
}

bool TwinHysteresis::SetHysteresis(double hysteresis)
{
    Bind bind(&m_log);
    return m_algo->SetHysteresis(hysteresis);
}

bool TwinHysteresis::SetAggression(double aggression)
{
    Bind bind(&m_log);
    return m_algo->SetAggression(aggression);
}

double TwinHysteresis::MinMove() const
{
    return m_algo->m_minMove;
}

double TwinHysteresis::Hysteresis() const
{
    return m_algo->m_hysteresis;
}

double TwinHysteresis::Aggression() const
{
    return m_algo->m_aggression;
}

double TwinHysteresis::LastMove() const
{
    return m_algo->m_lastMove;
}

TwinResistSwitch::TwinResistSwitch()
{
    Bind bind(&m_log);
    m_algo = std::make_unique<legacy::GuideAlgorithmResistSwitch>();
}

TwinResistSwitch::~TwinResistSwitch() = default;

double TwinResistSwitch::result(double input)
{
    Bind bind(&m_log);
    return m_algo->result(input);
}

void TwinResistSwitch::reset()
{
    Bind bind(&m_log);
    m_algo->reset();
}

bool TwinResistSwitch::SetMinMove(double minMove)
{
    Bind bind(&m_log);
    return m_algo->SetMinMove(minMove);
}

bool TwinResistSwitch::SetAggression(double aggression)
{
    Bind bind(&m_log);
    return m_algo->SetAggression(aggression);
}

void TwinResistSwitch::SetFastSwitchEnabled(bool enable)
{
    Bind bind(&m_log);
    m_algo->SetFastSwitchEnabled(enable);
}

double TwinResistSwitch::MinMove() const
{
    return m_algo->m_minMove;
}

double TwinResistSwitch::Aggression() const
{
    return m_algo->m_aggression;
}

bool TwinResistSwitch::FastSwitchEnabled() const
{
    return m_algo->m_fastSwitchEnabled;
}

int TwinResistSwitch::CurrentSide() const
{
    return m_algo->m_currentSide;
}

std::vector<double> TwinResistSwitch::History() const
{
    std::vector<double> h;
    for (size_t i = 0; i < m_algo->m_history.GetCount(); i++)
        h.push_back(m_algo->m_history[i]);
    return h;
}

} // namespace algo_test
