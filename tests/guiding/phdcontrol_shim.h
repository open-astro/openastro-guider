// Mechanical stand-ins that let the production phdcontrol.cpp compile in
// test_phd_controller without wxWidgets or the app (#142, slice T2f).
//
// Force-included ahead of phdcontrol.cpp (and the test), after the test phd.h.
// Since the ControllerHost seam, phdcontrol.cpp reaches the rest of the guider
// only through its host, so all it still needs is wxString, wxRect, the
// translation macros and the Debug log, which a test does not read. Nothing
// here decides anything the controller does.

#ifndef TESTS_PHDCONTROL_SHIM_H
#define TESTS_PHDCONTROL_SHIM_H

#include <cstdio>
#include <string>
#include <utility>

#define _(s) s
#define _T(s) s

class wxString
{
    std::string m_s;

public:
    wxString() = default;
    wxString(const char *s) : m_s(s ? s : "") { }
    explicit wxString(std::string s) : m_s(std::move(s)) { }

    const char *c_str() const { return m_s.c_str(); }
    const std::string& str() const { return m_s; }
    bool empty() const { return m_s.empty(); }
    bool IsEmpty() const { return m_s.empty(); }

    friend bool operator==(const wxString& a, const wxString& b) { return a.m_s == b.m_s; }

    // wxString::Format through snprintf. A variadic template, not C varargs, so a
    // wxString argument is passed to %s as its c_str() instead of as an object.
    template<class... A>
    static wxString Format(const wxString& fmt, const A&...args);
};

namespace phdcontrol_shim
{
template<class T>
inline const T& FormatArg(const T& v)
{
    return v;
}
inline const char *FormatArg(const wxString& s)
{
    return s.c_str();
}
} // namespace phdcontrol_shim

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
template<class... A>
wxString wxString::Format(const wxString& fmt, const A&...args)
{
    int n = std::snprintf(nullptr, 0, fmt.c_str(), phdcontrol_shim::FormatArg(args)...);
    if (n <= 0)
        return wxString();
    std::string buf((size_t) n + 1, '\0');
    std::snprintf(buf.data(), buf.size(), fmt.c_str(), phdcontrol_shim::FormatArg(args)...);
    buf.resize((size_t) n);
    return wxString(std::move(buf));
}
#pragma GCC diagnostic pop

inline const wxString wxEmptyString;

struct wxRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// The Debug log: written, never read
struct PhdControlShimDebugLog
{
    void Write(const wxString&) { }
    void AddLine(const wxString&) { }
};
inline PhdControlShimDebugLog Debug;

#include "phdcontrol.h"

#endif // TESTS_PHDCONTROL_SHIM_H
