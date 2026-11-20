// The implementations the guide algorithm characterization tests run on (#142).
//
// Twin*: the frozen copies of the production code before the extraction
// (legacy/hysteresis_legacy.cpp, legacy/resistswitch_legacy.cpp), compiled against mechanical
// stand-ins. Each object is built as the production constructor builds it with nothing in the
// profile.
//
// The typed tests call the members production has (result, reset, the setters) and read the
// state through accessors. DebugLog() is every Debug line the object wrote, THROW_INFO and
// ERROR_INFO lines included as "Throw->text" and "Error->text", in order.

#ifndef ALGORITHMS_ALGORITHM_IMPLS_H
#define ALGORITHMS_ALGORITHM_IMPLS_H

#include <memory>
#include <string>
#include <vector>

namespace legacy
{
class GuideAlgorithmHysteresis;
class GuideAlgorithmResistSwitch;
} // namespace legacy

namespace algo_test
{

using Log = std::vector<std::string>;

class TwinHysteresis
{
public:
    TwinHysteresis();
    ~TwinHysteresis();
    TwinHysteresis(const TwinHysteresis&) = delete;
    TwinHysteresis& operator=(const TwinHysteresis&) = delete;

    double result(double input);
    void reset();
    bool SetMinMove(double minMove);
    bool SetHysteresis(double hysteresis);
    bool SetAggression(double aggression);

    double MinMove() const;
    double Hysteresis() const;
    double Aggression() const;
    double LastMove() const;

    const Log& DebugLog() const { return m_log; }
    void ClearLog() { m_log.clear(); }

private:
    Log m_log;
    std::unique_ptr<legacy::GuideAlgorithmHysteresis> m_algo;
};

class TwinResistSwitch
{
public:
    TwinResistSwitch();
    ~TwinResistSwitch();
    TwinResistSwitch(const TwinResistSwitch&) = delete;
    TwinResistSwitch& operator=(const TwinResistSwitch&) = delete;

    double result(double input);
    void reset();
    bool SetMinMove(double minMove);
    bool SetAggression(double aggression);
    void SetFastSwitchEnabled(bool enable);

    double MinMove() const;
    double Aggression() const;
    bool FastSwitchEnabled() const;
    int CurrentSide() const;
    std::vector<double> History() const; // oldest first

    const Log& DebugLog() const { return m_log; }
    void ClearLog() { m_log.clear(); }

private:
    Log m_log;
    std::unique_ptr<legacy::GuideAlgorithmResistSwitch> m_algo;
};

} // namespace algo_test

#endif // ALGORITHMS_ALGORITHM_IMPLS_H
