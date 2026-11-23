// FROZEN COPY, DO NOT EDIT: the legacy twin of GuiderMultiStar's frame rejection and refine step
// (#142).
//
// Source: src/guider_multistar.cpp at origin/main 7755af25 (blob b93eeaf0), last changed by the
// #231 work that put MassChecker and DistanceChecker on monotonic_timers::ClockMs. Line ranges
// copied, LF as in the source, nothing changed:
//   53-181   class MassChecker
//   605-708  struct DistanceChecker
//   717-925  AppendStarUse, GuiderMultiStar::RefineOffset
//
// Check it against the source with
//   bash tests/guiding/legacy/check_twin.sh
// which diffs these ranges of src/guider_multistar.cpp at 7755af25 against the copy below and
// must print nothing. Everything the copy calls outside these lines is a mechanical stand-in in
// multistar_legacy.h.

#include "multistar_legacy.h"

#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"

namespace legacy
{

TwinDebug Debug;
int64_t monotonic_timers::nowMs = 0;
static Guider s_guider;
static TwinFrame s_frame { &s_guider };
TwinFrame *pFrame = &s_frame;
bool PhdController::settling = false;
static TwinMount s_mount;
TwinMount *pMount = &s_mount;
static TwinCamera s_camera;
TwinCamera *pCamera = &s_camera;
FindHook g_find;

// BEGIN COPY
class MassChecker
{
    enum
    {
        DefaultTimeWindowMs = 22500
    };

    struct Entry
    {
        wxLongLong_t time;
        double mass;
    };

    std::deque<Entry> m_data;
    double m_highMass; // high-water mark
    double m_lowMass; // low-water mark
    unsigned long m_timeWindow;
    double *m_tmp;
    size_t m_tmpSize;
    int m_exposure;
    bool m_isAutoExposure;

public:
    MassChecker() : m_highMass(0.), m_lowMass(9e99), m_tmp(0), m_tmpSize(0), m_exposure(0), m_isAutoExposure(false)
    {
        SetTimeWindow(DefaultTimeWindowMs);
    }

    ~MassChecker() { delete[] m_tmp; }

    void SetTimeWindow(unsigned int milliseconds)
    {
        // an abrupt change in mass will affect the median after approx m_timeWindow/2
        m_timeWindow = milliseconds * 2;
    }

    void SetExposure(int exposure, bool isAutoExp)
    {
        if (isAutoExp != m_isAutoExposure)
        {
            m_isAutoExposure = isAutoExp;
            m_exposure = exposure;
            Reset();
        }
        else if (exposure != m_exposure)
        {
            m_exposure = exposure;
            if (!m_isAutoExposure)
            {
                Reset();
            }
        }
    }

    double AdjustedMass(double mass) const { return m_isAutoExposure ? mass / (double) m_exposure : mass; }

    void AppendData(double mass)
    {
        wxLongLong_t now = monotonic_timers::ClockMs();
        wxLongLong_t oldest = now - m_timeWindow;

        while (m_data.size() > 0 && m_data.front().time < oldest)
            m_data.pop_front();

        Entry entry;
        entry.time = now;
        entry.mass = AdjustedMass(mass);
        m_data.push_back(entry);
    }

    bool CheckMass(double mass, double threshold, double limits[4])
    {
        if (m_data.size() < 5)
            return false;

        if (m_tmpSize < m_data.size())
        {
            delete[] m_tmp;
            m_tmpSize = m_data.size() + 10;
            m_tmp = new double[m_tmpSize];
        }

        std::deque<Entry>::const_iterator it = m_data.begin();
        std::deque<Entry>::const_iterator end = m_data.end();
        double *p = &m_tmp[0];
        for (; it != end; ++it)
            *p++ = it->mass;

        size_t mid = m_data.size() / 2;
        std::nth_element(&m_tmp[0], &m_tmp[mid], &m_tmp[m_data.size()]);
        double med = m_tmp[mid];

        if (med > m_highMass)
            m_highMass = med;
        if (med < m_lowMass)
            m_lowMass = med;

        // let the low water mark drift to follow the median so that it moves back up after a
        // period of intermittent clouds has brought it down
        m_lowMass += .05 * (med - m_lowMass);

        limits[0] = m_lowMass * (1. - threshold);
        limits[1] = med;
        limits[2] = m_highMass * (1. + threshold);
        // when mass is depressed by sky conditions, we still want to trigger a rejection when
        // there is a large spike in mass, even if it is still below the high water mark-based
        // threshold
        limits[3] = med * (1. + 2.0 * threshold);

        double adjmass = AdjustedMass(mass);
        bool reject = adjmass < limits[0] || adjmass > limits[2] || adjmass > limits[3];

        if (reject && m_isAutoExposure)
        {
            // convert back to mass-like numbers for logging by caller
            for (int i = 0; i < 4; i++)
                limits[i] *= (double) m_exposure;
        }

        return reject;
    }

    void Reset()
    {
        m_data.clear();
        m_highMass = 0.;
        m_lowMass = 9e99;
    }
};
struct DistanceChecker
{
    enum State
    {
        ST_GUIDING,
        ST_WAITING,
        ST_RECOVERING,
    };
    State m_state;
    wxLongLong_t m_expires = 0;
    double m_forceTolerance = 0.;

    enum
    {
        WAIT_INTERVAL_MS = 5000
    };

    DistanceChecker() : m_state(ST_GUIDING) { }

    void Activate()
    {
        if (m_state == ST_GUIDING)
        {
            Debug.Write("DistanceChecker: activated\n");
            m_state = ST_WAITING;
            m_expires = monotonic_timers::ClockMs() + WAIT_INTERVAL_MS;
            m_forceTolerance = 2.0;
        }
    }

    static bool _CheckDistance(double distance, bool raOnly, double tolerance)
    {
        enum
        {
            MIN_FRAMES_FOR_STATS = 10
        };
        Guider *guider = pFrame->pGuider;
        if (!guider->IsGuiding() || guider->IsPaused() || PhdController::IsSettling() ||
            guider->CurrentErrorFrameCount() < MIN_FRAMES_FOR_STATS)
        {
            return true;
        }
        double avgDist = guider->CurrentErrorSmoothed(raOnly);
        double threshold = tolerance * avgDist;
        if (distance > threshold)
        {
            Debug.Write(wxString::Format("DistanceChecker: reject for large offset (%.2f > %.2f) avgDist = %.2f count = %u\n",
                                         distance, threshold, avgDist, guider->CurrentErrorFrameCount()));
            return false;
        }
        return true;
    }

    bool CheckDistance(double distance, bool raOnly, double tolerance)
    {
        if (m_forceTolerance != 0.)
            tolerance = m_forceTolerance;

        bool small_offset = _CheckDistance(distance, raOnly, tolerance);

        switch (m_state)
        {
        default:
        case ST_GUIDING:
            if (small_offset)
                return true;

            Debug.Write("DistanceChecker: activated\n");
            m_state = ST_WAITING;
            m_expires = monotonic_timers::ClockMs() + WAIT_INTERVAL_MS;
            return false;

        case ST_WAITING:
        {
            if (small_offset)
            {
                Debug.Write("DistanceChecker: deactivated\n");
                m_state = ST_GUIDING;
                m_forceTolerance = 0.;
                return true;
            }
            // large distance
            wxLongLong_t now = monotonic_timers::ClockMs();
            if (now < m_expires)
            {
                // reject frame
                return false;
            }
            // timed-out
            Debug.Write("DistanceChecker: begin recovering\n");
            m_state = ST_RECOVERING;
            // fall through
        }

        case ST_RECOVERING:
            if (small_offset)
            {
                Debug.Write("DistanceChecker: deactivated\n");
                m_state = ST_GUIDING;
            }
            return true;
        }
    }
};
// Private method to build compact logging string for how secondary stars were used
static void AppendStarUse(wxString& secondaryInfo, int starNum, double dX, double dY, double weight, const wxString& flag)
{
    secondaryInfo += wxString::Format("[#%d %0.2f,%0.2f,%0.2f,%s] ", starNum, dX, dY, weight, flag);
}

// Use secondary stars to refine Offset value if appropriate.  Return of true means offset has been adjusted
bool GuiderMultiStar::RefineOffset(const usImage *pImage, GuiderOffset *pOffset)
{
    double primaryDistance;
    double secondaryDistance;
    double primarySigma = 0;
    bool averaged = false;
    int validStars = 0;
    GuiderOffset origOffset = *pOffset;
    m_starsUsed = 1;
    bool erasures = false;
    bool refined = false;

    // Primary star is in position 0 of the list
    try
    {
        if (IsGuiding() && m_guideStars.size() > 1 && pMount->GetGuidingEnabled() && !PhdController::IsSettling())
        {
            double sumWeights = 1;
            double sumX = origOffset.cameraOfs.X;
            double sumY = origOffset.cameraOfs.Y;
            primaryDistance = hypot(sumX, sumY);

            m_primaryDistStats->AddValue(primaryDistance);

#define Iter_Inx(p) (p - m_guideStars.begin())

            if (m_primaryDistStats->GetCount() > 5)
            {
                primarySigma = m_primaryDistStats->GetSigma();
                if (!m_stabilizing && primaryDistance > m_stabilitySigmaX * primarySigma)
                {
                    m_stabilizing = true;
                    Debug.Write("MultiStar: large primary error, entering stabilization period\n");
                }
                else if (m_stabilizing)
                {
                    if (primaryDistance <= 2 * primarySigma)
                    {
                        m_stabilizing = false;
                        Debug.Write("MultiStar: exiting stabilization period\n");
                        if (m_lockPositionMoved)
                        {
                            m_lockPositionMoved = false;
                            Debug.Write("MultiStar: updating star positions after lock position change\n");
                            for (auto pGS = m_guideStars.begin() + 1; pGS != m_guideStars.end();)
                            {
                                PHD_Point expectedLoc = m_primaryStar + pGS->offsetFromPrimary;
                                bool found;
                                if (IsValidSecondaryStarPosition(expectedLoc))
                                    found = pGS->Find(pImage, m_searchRegion, expectedLoc.X, expectedLoc.Y,
                                                      pFrame->GetStarFindMode(), GetMinStarHFD(), GetMaxStarHFD(),
                                                      pCamera->GetSaturationADU(), Star::FIND_LOGGING_VERBOSE);
                                else
                                    found = pGS->Find(pImage, m_searchRegion, pGS->X, pGS->Y, pFrame->GetStarFindMode(),
                                                      GetMinStarHFD(), GetMaxStarHFD(), pCamera->GetSaturationADU(),
                                                      Star::FIND_LOGGING_VERBOSE);
                                if (found)
                                {
                                    pGS->referencePoint.X = pGS->X;
                                    pGS->referencePoint.Y = pGS->Y;
                                    pGS->wasLost = false;
                                    ++pGS;
                                }
                                else
                                {
                                    // Don't need to update reference point, lost star will continue to use the
                                    // offsetFromPrimary location for possible recovery
                                    pGS->wasLost = true;
                                    ++pGS;
                                }
                            }
                            return false; // All the secondary stars reference points reflect current positions
                        }
                    }
                }
            }
            else
                m_stabilizing = true; // get some data for primary star movement

            if (!m_stabilizing && m_guideStars.size() > 1 && (sumX != 0 || sumY != 0))
            {
                wxString secondaryInfo = "MultiStar: ";
                for (auto pGS = m_guideStars.begin() + 1; pGS != m_guideStars.end();)
                {
                    if (m_starsUsed >= m_maxStars || m_guideStars.size() == 1)
                        break;
                    bool found = false;
                    if (pGS->wasLost)
                    {
                        // Look for it based on its original offset from the primary star
                        PHD_Point expectedLoc = m_primaryStar + pGS->offsetFromPrimary;
                        found = pGS->Find(pImage, m_searchRegion, expectedLoc.X, expectedLoc.Y, pFrame->GetStarFindMode(),
                                          GetMinStarHFD(), GetMaxStarHFD(), pCamera->GetSaturationADU(),
                                          Star::FIND_LOGGING_MINIMAL);
                    }
                    else
                        // Look for it where we last found it
                        found = pGS->Find(pImage, m_searchRegion, pGS->X, pGS->Y, pFrame->GetStarFindMode(), GetMinStarHFD(),
                                          GetMaxStarHFD(), pCamera->GetSaturationADU(), Star::FIND_LOGGING_MINIMAL);
                    if (found)
                    {
                        double dX = pGS->X - pGS->referencePoint.X;
                        double dY = pGS->Y - pGS->referencePoint.Y;

                        pGS->wasLost = false;
                        m_starsUsed++;

                        if (dX != 0. || dY != 0.)
                        {
                            // Handle zero-counting - suspect results of exactly zero movement
                            if (dX == 0. || dY == 0.)
                                ++pGS->zeroCount;
                            else if (pGS->zeroCount > 0)
                                --pGS->zeroCount;

                            if (pGS->zeroCount == 5)
                            {
                                AppendStarUse(secondaryInfo, Iter_Inx(pGS), 0, 0, 0, "DZ");
                                pGS = m_guideStars.erase(pGS);
                                erasures = true;
                                continue;
                            }

                            // Handle suspicious excursions - counted as "misses"
                            secondaryDistance = hypot(dX, dY);
                            if (secondaryDistance > 2.5 * primarySigma)
                            {
                                if (++pGS->missCount > 10)
                                {
                                    // Reset the reference point to wherever it is now
                                    pGS->referencePoint.X = pGS->X;
                                    pGS->referencePoint.Y = pGS->Y;
                                    pGS->missCount = 0;
                                    AppendStarUse(secondaryInfo, Iter_Inx(pGS), dX, dY, 0, "R");
                                }
                                else
                                    AppendStarUse(secondaryInfo, Iter_Inx(pGS), dX, dY, 0,
                                                  "M" + std::to_string(pGS->missCount));
                                ++pGS;
                                continue;
                            }
                            else if (pGS->missCount > 0)
                            {
                                --pGS->missCount;
                            }

                            // At this point we have usable data from the secondary star
                            double wt = (pGS->SNR / m_primaryStar.SNR);
                            sumX += wt * dX;
                            sumY += wt * dY;
                            sumWeights += wt;
                            averaged = true;
                            validStars++;

                            AppendStarUse(secondaryInfo, Iter_Inx(pGS), dX, dY, wt, "U");
                        }
                        else // exactly zero on both axes, probably a hot pixel, drop it
                        {
                            AppendStarUse(secondaryInfo, Iter_Inx(pGS), 0, 0, 0, "DZ");
                            pGS = m_guideStars.erase(pGS);
                            erasures = true;
                        }
                    }
                    else
                    {
                        // star not found in its search region
                        AppendStarUse(secondaryInfo, Iter_Inx(pGS), 0, 0, 0, "L");
                        pGS->wasLost = true;
                    }
                    if (!erasures)
                        ++pGS;
                    else
                        erasures = false;
                } // End of looping through secondary stars
                Debug.Write(secondaryInfo + "\n");

                if (averaged)
                {
                    sumX = sumX / sumWeights;
                    sumY = sumY / sumWeights;
                    if (hypot(sumX, sumY) < primaryDistance) // Apply average only if its smaller than single-star delta
                    {
                        pOffset->cameraOfs.X = sumX;
                        pOffset->cameraOfs.Y = sumY;
                        refined = true;
                    }
                    Debug.Write(wxString::Format("%s, %d included, MultiStar: {%0.2f, %0.2f}, one-star: {%0.2f, %0.2f}\n",
                                                 refined ? "refined" : "single-star", validStars, sumX, sumY,
                                                 origOffset.cameraOfs.X, origOffset.cameraOfs.Y));
                }
            }
        }
    }
    catch (const wxString& msg)
    {
        Debug.Write(wxString::Format("MultiStar fault: exception at %d, %s, reverting to single-star mode\n", __LINE__, msg));
        m_multiStarMode = false;
    }

    return refined;
#undef Iter_Inx
}
// END COPY

} // namespace legacy

namespace legacy_api
{

MassChecker::MassChecker() : m_impl(new legacy::MassChecker()) { }
MassChecker::~MassChecker()
{
    delete m_impl;
}
void MassChecker::SetTimeWindow(unsigned int ms)
{
    m_impl->SetTimeWindow(ms);
}
void MassChecker::SetExposure(int exposure, bool isAutoExp)
{
    m_impl->SetExposure(exposure, isAutoExp);
}
void MassChecker::AppendData(double mass)
{
    m_impl->AppendData(mass);
}
bool MassChecker::CheckMass(double mass, double threshold, double limits[4])
{
    return m_impl->CheckMass(mass, threshold, limits);
}
void MassChecker::Reset()
{
    m_impl->Reset();
}

DistanceChecker::DistanceChecker() : m_impl(new legacy::DistanceChecker()) { }
DistanceChecker::~DistanceChecker()
{
    delete m_impl;
}
void DistanceChecker::Activate()
{
    m_impl->Activate();
}
bool DistanceChecker::CheckDistance(double distance, bool raOnly, double tolerance)
{
    return m_impl->CheckDistance(distance, raOnly, tolerance);
}
int DistanceChecker::State() const
{
    return m_impl->m_state;
}
double DistanceChecker::ForceTolerance() const
{
    return m_impl->m_forceTolerance;
}

} // namespace legacy_api
