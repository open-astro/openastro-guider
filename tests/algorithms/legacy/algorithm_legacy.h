// The frozen legacy twins of GuideAlgorithmHysteresis and GuideAlgorithmResistSwitch (#142).
//
// hysteresis_legacy.cpp and resistswitch_legacy.cpp hold verbatim LF copies of the per-frame
// code of src/guide_algorithm_hysteresis.cpp and src/guide_algorithm_resistswitch.cpp as they
// were before the math moved into guide_algorithm_kernels.{h,cpp}: the defaults, reset(),
// result() and the setters. The classes below have the members of the production headers with
// the same names and types, so the copied functions compile unchanged.
//
// Everything else here is a mechanical stand-in. The wx types are std wrappers (wxString over
// std::string with Format through snprintf, ArrayOfDbl over std::vector<double>, wxClip as wx
// defines it); pConfig drops what the setters save; Debug, THROW_INFO and ERROR_INFO append to
// the log of the twin that is running, so the tests compare the Debug lines and their order.
// The production THROW_INFO and ERROR_INFO write "Throw from <file>:<line>->" and
// "Error thrown from <file>:<line>->" before the text; the stand-ins write "Throw->" and
// "Error->", since the line numbers change with any edit.
//
// The twins pin today's behaviour so the extraction can be proven identical by differential
// testing (test_guide_algorithm_kernels).

#ifndef ALGORITHMS_LEGACY_ALGORITHM_LEGACY_H
#define ALGORITHMS_LEGACY_ALGORITHM_LEGACY_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#undef THROW_INFO
#undef ERROR_INFO
#define THROW_INFO(s) (::legacy::LogThrow("Throw->" s))
#define ERROR_INFO(s) (::legacy::LogThrow("Error->" s))

namespace legacy
{

class wxString
{
    std::string m_s;

public:
    wxString() = default;
    wxString(const char *s) : m_s(s ? s : "") { }
    explicit wxString(std::string s) : m_s(std::move(s)) { }

    const char *c_str() const { return m_s.c_str(); }
    const std::string& str() const { return m_s; }

    friend wxString operator+(const wxString& a, const wxString& b) { return wxString(a.m_s + b.m_s); }

    // wxString::Format through snprintf
    template<class... A>
    static wxString Format(const char *fmt, const A&...args);
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
template<class... A>
wxString wxString::Format(const char *fmt, const A&...args)
{
    int n = std::snprintf(nullptr, 0, fmt, args...);
    if (n <= 0)
        return wxString();
    std::string buf((size_t) n + 1, '\0');
    std::snprintf(buf.data(), buf.size(), fmt, args...);
    buf.resize((size_t) n);
    return wxString(std::move(buf));
}
#pragma GCC diagnostic pop

// WX_DEFINE_ARRAY_DOUBLE(double, ArrayOfDbl), the members the copy uses
class ArrayOfDbl
{
    std::vector<double> m_v;

public:
    void Add(double d) { m_v.push_back(d); }
    void RemoveAt(size_t i) { m_v.erase(m_v.begin() + (std::ptrdiff_t) i); }
    void Empty() { m_v.clear(); }
    size_t GetCount() const { return m_v.size(); }
    double& operator[](size_t i) { return m_v[i]; }
    const double& operator[](size_t i) const { return m_v[i]; }
};

// wx/utils.h
template<typename T1, typename T2, typename T3>
inline T1 wxClip(T1 a, T2 b, T3 c)
{
    return (a < b) ? b : ((a > c) ? c : a);
}

// The Debug log of the twin that is running
extern std::vector<std::string> *g_log;

inline wxString LogThrow(const char *s)
{
    g_log->push_back(s);
    return wxString(s);
}

struct TwinDebug
{
    void Write(const wxString& s) { g_log->push_back(s.str()); }
};
extern TwinDebug Debug;

struct TwinConfig
{
    struct
    {
        void SetDouble(const wxString&, double) { }
        void SetBoolean(const wxString&, bool) { }
    } Profile;
};
extern TwinConfig *pConfig;

// guide_algorithm_hysteresis.h, the members the copied functions use
class GuideAlgorithmHysteresis
{
public:
    double m_minMove = 0.;
    double m_hysteresis = 0.;
    double m_aggression = 0.;
    double m_lastMove = 0.;

    // the constructor as production runs it with nothing in the profile
    GuideAlgorithmHysteresis();

    wxString GetConfigPath() const { return "/scope/GuideAlgorithm/X/Hysteresis"; }

    void reset();
    double result(double input);
    bool SetMinMove(double minMove);
    bool SetHysteresis(double hysteresis);
    bool SetAggression(double aggression);
};

// guide_algorithm_resistswitch.h, the members the copied functions use
class GuideAlgorithmResistSwitch
{
public:
    static const unsigned int HISTORY_SIZE = 10;

    ArrayOfDbl m_history;
    double m_minMove = 0.;
    double m_aggression = 0.;
    bool m_fastSwitchEnabled = false;
    int m_currentSide = 0;

    // the constructor as production runs it with nothing in the profile
    GuideAlgorithmResistSwitch();

    wxString GetConfigPath() const { return "/scope/GuideAlgorithm/Y/ResistSwitch"; }

    void reset();
    double result(double input);
    bool SetMinMove(double minMove);
    bool SetAggression(double aggr);
    void SetFastSwitchEnabled(bool enable);
};

} // namespace legacy

#endif // ALGORITHMS_LEGACY_ALGORITHM_LEGACY_H
