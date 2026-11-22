// Mechanical stand-ins that let the frozen copy of GuiderMultiStar's frame
// rejection and refine step (multistar_legacy.cpp) compile without wxWidgets or
// the app (#142).
//
// Nothing here decides anything the copied code depends on. wxString is a
// std::string wrapper with Format through snprintf; Debug records each line;
// the clock, the guider's state (pFrame->pGuider, PhdController::IsSettling,
// pMount, pCamera) are plain values the test sets; GuideStar::Find calls the
// test's star finder. GuiderMultiStar has only the members the copied
// RefineOffset uses, with the same names.
//
// Everything lives in namespace legacy, so the copied lines compile verbatim.

#ifndef GUIDING_LEGACY_MULTISTAR_LEGACY_H
#define GUIDING_LEGACY_MULTISTAR_LEGACY_H

#include "guiding_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace legacy
{

typedef long long wxLongLong_t;

class wxString
{
    std::string m_s;

public:
    wxString() = default;
    wxString(const char *s) : m_s(s ? s : "") { }
    wxString(std::string s) : m_s(std::move(s)) { }

    const char *c_str() const { return m_s.c_str(); }
    const std::string& str() const { return m_s; }

    wxString& operator+=(const wxString& o)
    {
        m_s += o.m_s;
        return *this;
    }
    friend wxString operator+(const wxString& a, const wxString& b) { return wxString(a.m_s + b.m_s); }

    template<class... A>
    static wxString Format(const wxString& fmt, const A&...args);
};

namespace detail
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
} // namespace detail

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
template<class... A>
wxString wxString::Format(const wxString& fmt, const A&...args)
{
    int n = std::snprintf(nullptr, 0, fmt.c_str(), detail::FormatArg(args)...);
    if (n <= 0)
        return wxString();
    std::string buf((size_t) n + 1, '\0');
    std::snprintf(buf.data(), buf.size(), fmt.c_str(), detail::FormatArg(args)...);
    buf.resize((size_t) n);
    return wxString(std::move(buf));
}
#pragma GCC diagnostic pop

// The Debug log: every line the copied code writes, in order
struct TwinDebug
{
    std::vector<std::string> lines;
    void Write(const wxString& s) { lines.push_back(s.str()); }
};
extern TwinDebug Debug;

namespace monotonic_timers
{
    extern int64_t nowMs; // what ClockMs returns, set by the test
    inline int64_t ClockMs()
    {
        return nowMs;
    }
} // namespace monotonic_timers

// pFrame->pGuider, as DistanceChecker::_CheckDistance reads it
struct Guider
{
    bool guiding = false;
    bool paused = false;
    unsigned int frameCount = 0;
    double avgDist = 0.;
    double avgDistRA = 0.;

    bool IsGuiding() const { return guiding; }
    bool IsPaused() const { return paused; }
    unsigned int CurrentErrorFrameCount() const { return frameCount; }
    double CurrentErrorSmoothed(bool raOnly) const { return raOnly ? avgDistRA : avgDist; }
};

enum FindMode
{
    FIND_CENTROID,
    FIND_PEAK,
};

struct TwinFrame
{
    Guider *pGuider;
    FindMode GetStarFindMode() const { return FIND_CENTROID; }
};
extern TwinFrame *pFrame;

struct PhdController
{
    static bool settling;
    static bool IsSettling() { return settling; }
};

struct TwinMount
{
    bool guidingEnabled = true;
    bool GetGuidingEnabled() const { return guidingEnabled; }
};
extern TwinMount *pMount;

struct TwinCamera
{
    unsigned short GetSaturationADU() const { return 65535; }
};
extern TwinCamera *pCamera;

struct usImage
{
};

// point.h's PHD_Point, the arithmetic the copied code uses
struct PHD_Point
{
    double X = 0.;
    double Y = 0.;
    PHD_Point() = default;
    PHD_Point(double x, double y) : X(x), Y(y) { }
    PHD_Point operator+(const PHD_Point& a) const { return PHD_Point(X + a.X, Y + a.Y); }
};

class Star : public PHD_Point
{
public:
    enum StarFindLogType
    {
        FIND_LOGGING_MINIMAL,
        FIND_LOGGING_VERBOSE,
    };
    double SNR = 0.;
};

class GuideStar;
// GuideStar::Find: the test's star finder, given the star and Find's base_x, base_y and whether
// it logs verbosely
using FindHook = std::function<bool(GuideStar& star, int baseX, int baseY, bool verbose)>;
extern FindHook g_find;

// star.h's GuideStar, plus an id for the test
class GuideStar : public Star
{
public:
    PHD_Point referencePoint;
    unsigned int missCount = 0;
    unsigned int zeroCount = 0;
    PHD_Point offsetFromPrimary;
    bool wasLost = false;
    int id = 0;

    bool Find(const usImage *, int, int base_x, int base_y, FindMode, double, double, unsigned short,
              StarFindLogType loggingControl)
    {
        return g_find(*this, base_x, base_y, loggingControl == FIND_LOGGING_VERBOSE);
    }
};

struct GuiderOffset
{
    PHD_Point cameraOfs;
};

// The GuiderMultiStar members the copied RefineOffset reads and writes
class GuiderMultiStar
{
public:
    Star m_primaryStar;
    std::vector<GuideStar> m_guideStars;
    DescriptiveStats *m_primaryDistStats = nullptr;
    bool m_multiStarMode = true;
    bool m_stabilizing = false;
    bool m_lockPositionMoved = false;
    unsigned int m_starsUsed = 0;
    unsigned int m_maxStars = 9;
    double m_stabilitySigmaX = 5;
    int m_searchRegion = 15;
    bool guiding = true; // IsGuiding
    // IsValidSecondaryStarPosition: the copied rule on an image of this size
    int imageWidth = 1280;
    int imageHeight = 960;

    bool IsGuiding() const { return guiding; }
    double GetMinStarHFD() const { return 1.5; }
    double GetMaxStarHFD() const { return 10.; }
    bool IsValidSecondaryStarPosition(const PHD_Point& pt) const
    {
        return pt.X >= 5 && pt.X + 5 < imageWidth && pt.Y >= 5 && pt.Y + 5 < imageHeight;
    }

    bool RefineOffset(const usImage *pImage, GuiderOffset *pOffset);
};

class MassChecker;
struct DistanceChecker;

} // namespace legacy

// The frozen MassChecker and DistanceChecker are defined in multistar_legacy.cpp; these give the
// test their whole interface
namespace legacy_api
{
struct MassChecker
{
    MassChecker();
    ~MassChecker();
    MassChecker(const MassChecker&) = delete;
    MassChecker& operator=(const MassChecker&) = delete;
    void SetTimeWindow(unsigned int ms);
    void SetExposure(int exposure, bool isAutoExp);
    void AppendData(double mass);
    bool CheckMass(double mass, double threshold, double limits[4]);
    void Reset();

private:
    legacy::MassChecker *m_impl;
};

struct DistanceChecker
{
    DistanceChecker();
    ~DistanceChecker();
    DistanceChecker(const DistanceChecker&) = delete;
    DistanceChecker& operator=(const DistanceChecker&) = delete;
    void Activate();
    bool CheckDistance(double distance, bool raOnly, double tolerance);
    int State() const;
    double ForceTolerance() const;

private:
    legacy::DistanceChecker *m_impl;
};
} // namespace legacy_api

#endif // GUIDING_LEGACY_MULTISTAR_LEGACY_H
