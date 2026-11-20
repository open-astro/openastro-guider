// Mechanical stand-ins that let the frozen copy of Scope's calibration code
// (scope_calibration_legacy.cpp) compile without wxWidgets or the app (#142).
//
// Nothing here decides anything the calibration depends on. The wx types are
// std::string wrappers, the translation macros are the identity, and the app
// globals the copied code calls (pFrame, pPointingSource, pCamera, pConfig,
// GuideLog, EvtServer, Debug, Rotator) are recorders: each call appends a
// closed_loop::Event to the current twin's log, and the mount reads are
// forwarded to its SimMount. The PHD types the code uses are copied from
// mount.h, scope.h and guidinglog.h with the same names and values.
//
// Everything lives in namespace legacy, so the copied lines compile verbatim.

#ifndef CLOSED_LOOP_LEGACY_SHIM_H
#define CLOSED_LOOP_LEGACY_SHIM_H

#include "../harness.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// point.h's ShiftPoint names these; the calibration never uses ShiftPoint
typedef long long wxLongLong_t;
struct wxLongLong
{
    long long GetValue() const { return 0; }
};
inline wxLongLong wxGetUTCTimeMillis()
{
    return wxLongLong();
}

#include "point.h"

// The test phd.h makes ERROR_INFO a bare string literal. The production one
// yields a wxString, which is what UpdateCalibrationState catches.
#undef ERROR_INFO
#define ERROR_INFO(s) (wxString(s))

#define _(s) s
#define _T(s) s
#define wxTRANSLATE(s) s
#define wxMin(a, b) (((a) < (b)) ? (a) : (b))
#define wxMax(a, b) (((a) > (b)) ? (a) : (b))

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
    bool IsEmpty() const { return m_s.empty(); }

    wxString& operator+=(const wxString& o)
    {
        m_s += o.m_s;
        return *this;
    }
    friend wxString operator+(const wxString& a, const wxString& b) { return wxString(a.m_s + b.m_s); }
    friend bool operator==(const wxString& a, const wxString& b) { return a.m_s == b.m_s; }

    // wxString::Format through snprintf. A variadic template, not C varargs, so a
    // wxString argument is passed to %s as its c_str() instead of as an object.
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

inline const wxString wxEmptyString;

inline wxString wxGetTranslation(const wxString& s)
{
    return s;
}

struct wxRealPoint
{
    double x = 0.;
    double y = 0.;
    wxRealPoint() = default;
    wxRealPoint(double x_, double y_) : x(x_), y(y_) { }
};

// image_math.h
inline static double norm(double val, double start, double end)
{
    double const range = end - start;
    double const ofs = val - start;
    return val - floor(ofs / range) * range;
}
inline static double norm_angle(double val)
{
    return norm(val, -M_PI, M_PI);
}
inline static double degrees(double radians)
{
    return radians * 180. / M_PI;
}
inline static double radians(double degrees)
{
    return degrees * M_PI / 180.;
}

// mount.h
enum GUIDE_DIRECTION
{
    NONE = -1,
    UP = 0,
    NORTH = UP,
    DOWN,
    SOUTH = DOWN,
    RIGHT,
    EAST = RIGHT,
    LEFT,
    WEST = LEFT
};

enum PierSide
{
    PIER_SIDE_UNKNOWN = -1,
    PIER_SIDE_EAST = 0,
    PIER_SIDE_WEST = 1,
};

enum GuideParity
{
    GUIDE_PARITY_EVEN = 1,
    GUIDE_PARITY_ODD = -1,
    GUIDE_PARITY_UNKNOWN = 0,
    GUIDE_PARITY_UNCHANGED = -5,
};

#define UNKNOWN_DECLINATION 997.0

struct Calibration
{
    double xRate = 0.;
    double yRate = 0.;
    double xAngle = 0.;
    double yAngle = 0.;
    double declination = UNKNOWN_DECLINATION;
    double rotatorAngle = 0.;
    unsigned short binning = 1;
    PierSide pierSide = PIER_SIDE_UNKNOWN;
    GuideParity raGuideParity = GUIDE_PARITY_UNKNOWN;
    GuideParity decGuideParity = GUIDE_PARITY_UNKNOWN;
    bool isValid = false;
    wxString timestamp;
};

enum CalibrationIssueType
{
    CI_None,
    CI_Steps,
    CI_Angle,
    CI_Rates,
    CI_Different
};

struct CalibrationDetails
{
    int focalLength = 0;
    double imageScale = 0.;
    double raGuideSpeed = 0.;
    double decGuideSpeed = 0.;
    double orthoError = 0.;
    int origBinning = 1;
    std::vector<wxRealPoint> raSteps;
    std::vector<wxRealPoint> decSteps;
    int raStepCount = 0;
    int decStepCount = 0;
    CalibrationIssueType lastIssue = CI_None;
    wxString origTimestamp;
    PierSide origPierSide = PIER_SIDE_UNKNOWN;
};

enum MountMoveOptionBits
{
    MOVEOPTS_CALIBRATION_MOVE = 0,
};

// scope.h
#define CALIBRATION_RATE_UNCALIBRATED 123e4

enum DEC_GUIDE_MODE
{
    DEC_NONE = 0,
    DEC_AUTO,
    DEC_NORTH,
    DEC_SOUTH
};

class Mount;

// guidinglog.h
struct CalibrationStepInfo
{
    Mount *mount;
    wxString direction;
    int stepNumber;
    double dx;
    double dy;
    PHD_Point pos;
    double dist;
    wxString msg;

    CalibrationStepInfo(Mount *mount_, const wxString& dir_, int stepNumber_, double dx_, double dy_, const PHD_Point& pos_,
                        double dist_, const wxString& msg_ = wxEmptyString)
        : mount(mount_), direction(dir_), stepNumber(stepNumber_), dx(dx_), dy(dy_), pos(pos_), dist(dist_), msg(msg_)
    {
    }
};

// The twin that is running: where the recorders write and what they read
struct TwinContext
{
    std::vector<closed_loop::Event> events;
    closed_loop::SimMount *sim = nullptr;
    int frame = -1;
    bool fastRecenter = true;
    int maxMovePx = 15;

    closed_loop::Event& Add(closed_loop::Event::Kind kind, const wxString& text = wxEmptyString)
    {
        closed_loop::Event e;
        e.kind = kind;
        e.frame = frame;
        e.text = text.str();
        events.push_back(e);
        return events.back();
    }
};
extern TwinContext *g_twin;

typedef void alert_fn(long);

struct TwinGuider
{
    bool IsFastRecenterEnabled() const { return g_twin->fastRecenter; }
    int GetMaxMovePixels() const { return g_twin->maxMovePx; }
};

struct TwinFrame
{
    TwinGuider *pGuider;

    void StatusMsg(const wxString& text) { g_twin->Add(closed_loop::Event::Status, text); }
    void Alert(const wxString& msg, int = 0) { g_twin->Add(closed_loop::Event::Alert, msg); }
    void Alert(const wxString& msg, alert_fn *, const wxString&, alert_fn *, intptr_t, bool = false, int = 0)
    {
        g_twin->Add(closed_loop::Event::Alert, msg);
    }
    void ScheduleAxisMove(Mount *, const GUIDE_DIRECTION direction, int duration, unsigned int moveOptions)
    {
        closed_loop::Event& e = g_twin->Add(closed_loop::Event::Move);
        e.dir = (closed_loop::Dir) direction;
        e.ms = duration;
        e.moveOptions = moveOptions;
    }
};
extern TwinFrame *pFrame;

struct TwinPointingSource
{
    // true = error, as in Scope
    bool GetCoordinates(double *ra, double *dec, double *siderealTime)
    {
        *siderealTime = 0.;
        return !g_twin->sim->Coordinates(ra, dec);
    }
    bool GetGuideRates(double *pRAGuideRate, double *pDecGuideRate)
    {
        return !g_twin->sim->GuideRates(pRAGuideRate, pDecGuideRate);
    }
    double GetDeclinationRadians()
    {
        std::optional<double> d = g_twin->sim->ReportedDecRad();
        return d ? *d : UNKNOWN_DECLINATION;
    }
    PierSide SideOfPier() { return PIER_SIDE_UNKNOWN; }
};
extern TwinPointingSource *pPointingSource;

struct TwinCamera
{
    unsigned short GetBinning() const { return 1; }
};
extern TwinCamera *pCamera;

struct TwinConfig
{
    struct
    {
        void SetBoolean(const wxString&, bool) { }
    } Global;
    int GetCurrentProfileId() const { return 1; }
    void Flush() { }
};
extern TwinConfig *pConfig;

struct TwinGuideLog
{
    void CalibrationStep(const CalibrationStepInfo& info)
    {
        closed_loop::Event& e = g_twin->Add(closed_loop::Event::LogStep, info.direction);
        e.step = info.stepNumber;
        e.dx = info.dx;
        e.dy = info.dy;
        e.x = info.pos.X;
        e.y = info.pos.Y;
        e.dist = info.dist;
    }
    void CalibrationFailed(const Mount *, const wxString& msg) { g_twin->Add(closed_loop::Event::LogFailed, msg); }
    void CalibrationDirectComplete(const Mount *, const wxString& direction, double angle, double rate, GuideParity parity)
    {
        closed_loop::Event& e = g_twin->Add(closed_loop::Event::DirectComplete, direction);
        e.angle = angle;
        e.rate = rate;
        e.parity = parity;
    }
    void CalibrationComplete(const Mount *) { g_twin->Add(closed_loop::Event::LogComplete); }
};
extern TwinGuideLog GuideLog;

struct TwinEvtServer
{
    void NotifyCalibrationStep(const CalibrationStepInfo& info)
    {
        closed_loop::Event& e = g_twin->Add(closed_loop::Event::NotifyStep, info.msg);
        e.step = info.stepNumber;
    }
    void NotifyCalibrationFailed(const Mount *, const wxString& msg) { g_twin->Add(closed_loop::Event::NotifyFailed, msg); }
    void NotifyCalibrationComplete(const Mount *) { g_twin->Add(closed_loop::Event::NotifyComplete); }
};
extern TwinEvtServer EvtServer;

struct TwinDebug
{
    void Write(const wxString& s) { g_twin->Add(closed_loop::Event::Debug, s); }
};
extern TwinDebug Debug;

struct Rotator
{
    static double RotatorPosition() { return 0.; }
};

} // namespace legacy

#endif // CLOSED_LOOP_LEGACY_SHIM_H
