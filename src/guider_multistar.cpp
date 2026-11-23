/*
 *  guider_multistar.cpp
 *  PHD Guiding

 *  Original guider_onestar Created by Craig Stark.
 *  Copyright (c) 2006-2010 Craig Stark.
 *  All rights reserved.
 *
 *  guider_onestar completely refactored by Bret McKee
 *  Copyright (c) 2012 Bret McKee
 *  All rights reserved.
 *
 *  guider_multistar extensions created by Bruce Waddington
 *  Copyright (c) 2020 Bruce Waddington
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "phd.h"
#include "edge_allowance.h"
#include "frame_reject.h"
#include "monotonic_timers.h"
#include "multistar_refine.h"

#include <wx/dir.h>
#include <algorithm>

#if ((wxMAJOR_VERSION < 3) && (wxMINOR_VERSION < 9))
# define wxPENSTYLE_DOT wxDOT
#endif

static const double DefaultMassChangeThreshold = 0.5;

// MIN/DEFAULT/MAX_SEARCH_REGION are public GuiderMultiStar statics (guider_multistar.h).
enum
{
    DEFAULT_MAX_STAR_COUNT = 9,
    DEFAULT_STABILITY_SIGMAX = 5,
    MAX_LIST_SIZE = 12
};

// Define a constructor for the guider
GuiderMultiStar::GuiderMultiStar()
    : Guider(), m_massChecker(new frame_reject::MassChecker()), m_stabilizing(false), m_multiStarMode(true),
      m_lastPrimaryDistance(0), m_lockPositionMoved(false), m_maxStars(DEFAULT_MAX_STAR_COUNT),
      m_stabilitySigmaX(DEFAULT_STABILITY_SIGMAX), m_lastStarsUsed(0)
{
    SetState(STATE_UNINITIALIZED);
    m_primaryDistStats = new DescriptiveStats();
}

GuiderMultiStar::~GuiderMultiStar()
{
    delete m_massChecker;
    delete m_primaryDistStats;
}

void GuiderMultiStar::SetMultiStarMode(bool val)
{
    bool oldVal = m_multiStarMode;
    bool autoFindForced = false;
    m_multiStarMode = val;
    if (val && !oldVal)
    {
        m_primaryDistStats->ClearAll();
        if (IsCalibratingOrGuiding())
        {
            // Keep calibrating or guiding on the current star and lock position (#187): stopping, auto-selecting
            // and starting again moved the lock to wherever the star was, with no StarSelected, LockPositionSet or
            // settle for the client. Secondary stars from the selection are found again at their offsets from the
            // primary once it has stabilized, as after a lock position change; without any, guiding goes on with
            // the single star until the next selection. Their references are taken again once the primary has
            // stabilized, the same refresh PHD2 makes after a dither. The lock did not move here, so any offset
            // left when stabilization ends is carried into them; keeping the selection's references would avoid it.
            if (m_guideStars.size() > 1)
            {
                m_lockPositionMoved = true;
                m_stabilizing = true;
            }
        }
        else if (GetState() >= STATE_SELECTED) // If we have a single star, need to force an auto-find to be sure we
                                               // have the right secondary stars
        {
            StopGuiding();
            InvalidateCurrentPosition(true);
            if (!AutoSelect(wxRect(0, 0, 0, 0), false))
                autoFindForced = true;
        }
    }
    if (!val)
        m_stabilizing = false;
    pConfig->Profile.SetBoolean("/guider/multistar/enabled", m_multiStarMode);
    wxString msg = wxString::Format("MultiStar mode %s", val ? "enabled" : "disabled");
    if (autoFindForced)
        msg += ", AutoFind forced\n";
    else
        msg += "\n";
    Debug.Write(msg);
    pFrame->NotifyGuidingParam("MultiStar", m_multiStarMode ? "true" : "false", true);
}

void GuiderMultiStar::ClearSecondaryStars()
{
    if (m_guideStars.size() > 1)
    {
        m_guideStars.erase(m_guideStars.begin() + 1, m_guideStars.end());
        Debug.Write("MultiStar: secondary guide stars cleared");
    }
}
void GuiderMultiStar::LoadProfileSettings()
{
    Guider::LoadProfileSettings();

    double massChangeThreshold = pConfig->Profile.GetDouble("/guider/onestar/MassChangeThreshold", DefaultMassChangeThreshold);
    SetMassChangeThreshold(massChangeThreshold);

    bool massChangeThreshEnabled =
        pConfig->Profile.GetBoolean("/guider/onestar/MassChangeThresholdEnabled", massChangeThreshold != 1.0);
    SetMassChangeThresholdEnabled(massChangeThreshEnabled);

    bool tolerateJumps = pConfig->Profile.GetBoolean("/guider/onestar/TolerateJumpsEnabled", false);
    double tolerateJumpsThresh = pConfig->Profile.GetDouble("/guider/onestar/TolerateJumpsThreshold", 4.0);
    SetTolerateJumps(tolerateJumps, tolerateJumpsThresh);

    int searchRegion = pConfig->Profile.GetInt("/guider/onestar/SearchRegion", DEFAULT_SEARCH_REGION);
    SetSearchRegion(searchRegion);

    SetMultiStarMode(pConfig->Profile.GetBoolean("/guider/multistar/enabled", false));
}

bool GuiderMultiStar::GetMassChangeThresholdEnabled() const
{
    return m_massChangeThresholdEnabled;
}

void GuiderMultiStar::SetMassChangeThresholdEnabled(bool enable)
{
    m_massChangeThresholdEnabled = enable;
    pConfig->Profile.SetBoolean("/guider/onestar/MassChangeThresholdEnabled", enable);
}

double GuiderMultiStar::GetMassChangeThreshold() const
{
    return m_massChangeThreshold;
}

bool GuiderMultiStar::SetMassChangeThreshold(double massChangeThreshold)
{
    bool bError = false;

    try
    {
        if (massChangeThreshold < 0.0)
        {
            throw ERROR_INFO("massChangeThreshold < 0");
        }

        m_massChangeThreshold = massChangeThreshold;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);

        bError = true;
        m_massChangeThreshold = DefaultMassChangeThreshold;
    }

    pConfig->Profile.SetDouble("/guider/onestar/MassChangeThreshold", m_massChangeThreshold);

    return bError;
}

bool GuiderMultiStar::SetTolerateJumps(bool enable, double threshold)
{
    m_tolerateJumpsEnabled = enable;
    pConfig->Profile.SetBoolean("/guider/onestar/TolerateJumpsEnabled", enable);

    m_tolerateJumpsThreshold = threshold;
    pConfig->Profile.SetDouble("/guider/onestar/TolerateJumpsThreshold", threshold);

    return false;
}

bool GuiderMultiStar::SetSearchRegion(int searchRegion)
{
    bool bError = false;

    try
    {
        if (searchRegion < MIN_SEARCH_REGION)
        {
            m_searchRegion = MIN_SEARCH_REGION;
            throw ERROR_INFO("searchRegion < MIN_SEARCH_REGION");
        }
        else if (searchRegion > MAX_SEARCH_REGION)
        {
            m_searchRegion = MAX_SEARCH_REGION;
            throw ERROR_INFO("searchRegion > MAX_SEARCH_REGION");
        }
        m_searchRegion = searchRegion;
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
    }

    pConfig->Profile.SetInt("/guider/onestar/SearchRegion", m_searchRegion);

    return bError;
}

bool GuiderMultiStar::SetCurrentPosition(const usImage *pImage, const PHD_Point& position)
{
    bool bError = true;

    try
    {
        if (!position.IsValid())
        {
            throw ERROR_INFO("position is invalid");
        }

        double x = position.X;
        double y = position.Y;

        Debug.Write(wxString::Format("SetCurrentPosition(%.2f,%.2f)\n", x, y));

        if ((x <= 0) || (x >= pImage->Size.x))
        {
            throw ERROR_INFO("invalid x value");
        }

        if ((y <= 0) || (y >= pImage->Size.y))
        {
            throw ERROR_INFO("invalid y value");
        }

        m_massChecker->Reset();
        bError = !m_primaryStar.Find(pImage, m_searchRegion, x, y, pFrame->GetStarFindMode(), GetMinStarHFD(), GetMaxStarHFD(),
                                     pCamera->GetSaturationADU(), Star::FIND_LOGGING_VERBOSE);
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
    }

    return bError;
}

static wxString StarStatusStr(const Star& star)
{
    if (!star.IsValid())
        return _("No star selected");

    switch (star.GetError())
    {
    case Star::STAR_LOWSNR:
        return _("Star lost - low SNR");
    case Star::STAR_LOWMASS:
        return _("Star lost - low mass");
    case Star::STAR_LOWHFD:
        return _("Star lost - low HFD");
    case Star::STAR_TOO_NEAR_EDGE:
        return _("Star too near edge");
    case Star::STAR_MASSCHANGE:
        return _("Star lost - mass changed");
    default:
        return _("No star found");
    }
}

static wxString StarStatus(const Star& star)
{
    wxString status = wxString::Format(_("m=%.0f SNR=%.1f"), star.Mass, star.SNR);

    if (star.GetError() == Star::STAR_SATURATED)
        status += _T(" ") + _("Saturated");

    int exp;
    bool auto_exp;
    pFrame->GetExposureInfo(&exp, &auto_exp);

    if (auto_exp)
    {
        status += _T(" ");
        if (exp >= 1)
            status += wxString::Format(_("Exp=%0.1f s"), (double) exp / 1000.);
        else
            status += wxString::Format(_("Exp=%d ms"), exp);
    }

    return status;
}

bool GuiderMultiStar::AutoSelect(const wxRect& roi, bool willForceRecal)
{
    Debug.Write("GuiderMultiStar::AutoSelect enter\n");

    bool error = false;

    usImage *image = CurrentImage();

    try
    {
        if (!image->ImageData)
        {
            throw ERROR_INFO("No Current Image");
        }

        // If the mount will calibrate after this selection (it is not calibrated, or the guide request
        // will clear its calibration first, #140), we need to choose a star a bit farther from the edge
        // to allow for the motion of the star during calibration
        //
        int edgeAllowance = CalibrationEdgeAllowance(willForceRecal);

        GuideStar newStar;
        if (!newStar.AutoFind(*image, edgeAllowance, m_searchRegion, roi, m_guideStars,
                              (pCamera->UseSubframes || !m_multiStarMode) ? 1 : MAX_LIST_SIZE))
        {
            throw ERROR_INFO("Unable to AutoFind");
        }

        m_massChecker->Reset();

        if (!m_primaryStar.Find(image, m_searchRegion, newStar.X, newStar.Y, Star::FIND_CENTROID, GetMinStarHFD(),
                                GetMaxStarHFD(), pCamera->GetSaturationADU(), Star::FIND_LOGGING_VERBOSE))
        {
            throw ERROR_INFO("Unable to find");
        }

        // DEBUG OUTPUT
        wxString buff = wxString::Format("MultiStar: List (%d): ", (int) m_guideStars.size());
        for (auto pGS = m_guideStars.begin(); pGS != m_guideStars.end(); ++pGS)
        {
            buff += wxString::Format("{%0.2f, %0.2f}(%0.1f), ", pGS->X, pGS->Y, pGS->SNR);
        }
        Debug.Write(buff + "\n");

        m_primaryDistStats->ClearAll();

        if (SetLockPosition(m_primaryStar))
        {
            throw ERROR_INFO("Unable to set Lock Position");
        }

        if (GetState() == STATE_SELECTING)
        {
            // immediately advance the state machine now, rather than waiting for
            // the next exposure to complete. Socket server clients are going to
            // try to start guiding after selecting the star, but guiding will fail
            // to start if state is still STATE_SELECTING
            Debug.Write(wxString::Format("AutoSelect: state = %d, call UpdateGuideState\n", GetState()));
            UpdateGuideState(NULL, false);
        }

        UpdateImageDisplay();

        pFrame->StatusMsg(wxString::Format(_("Auto-selected star at (%.1f, %.1f)"), m_primaryStar.X, m_primaryStar.Y));
        pFrame->UpdateStatusBarStarInfo(m_primaryStar.SNR, m_primaryStar.GetError() == Star::STAR_SATURATED);
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        error = true;
    }

    if (image->ImageData)
    {
        if (error)
            Debug.Write("GuiderMultiStar::AutoSelect failed.\n");

        ImageLogger::LogAutoSelectImage(image, !error);
    }

    return error;
}

inline static wxRect SubframeRect(const PHD_Point& pos, int halfwidth)
{
    return wxRect(ROUND(pos.X) - halfwidth, ROUND(pos.Y) - halfwidth, 2 * halfwidth + 1, 2 * halfwidth + 1);
}

wxRect GuiderMultiStar::GetBoundingBox() const
{
    enum
    {
        SUBFRAME_BOUNDARY_PX = 0
    };

    GUIDER_STATE state = GetState();

    bool subframe;
    PHD_Point pos;

    switch (state)
    {
    case STATE_SELECTED:
    case STATE_CALIBRATING_PRIMARY:
    case STATE_CALIBRATING_SECONDARY:
        subframe = m_primaryStar.WasFound();
        pos = CurrentPosition();
        break;
    case STATE_GUIDING:
    {
        subframe = m_primaryStar.WasFound(); // true;
        // As long as the star is close to the lock position, keep the subframe
        // at the lock position. Otherwise, follow the star.
        double dist = CurrentPosition().Distance(LockPosition());
        if ((int) dist > m_searchRegion / 3)
            pos = CurrentPosition();
        else
            pos = LockPosition();
        break;
    }
    default:
        subframe = false;
    }

    if (m_forceFullFrame)
    {
        subframe = false;
    }

    if (subframe)
    {
        wxRect box(SubframeRect(pos, m_searchRegion + SUBFRAME_BOUNDARY_PX));
        box.Intersect(wxRect(CurrentImage()->Size));
        return box;
    }
    else
    {
        return wxRect(0, 0, 0, 0);
    }
}

void GuiderMultiStar::InvalidateCurrentPosition(bool fullReset)
{
    m_primaryStar.Invalidate();

    if (fullReset)
    {
        m_primaryStar.X = m_primaryStar.Y = 0.0;
    }
}

wxString GuiderMultiStar::GetStarCount() const
{
    // no weird displays if stars are being removed from list
    return wxString::Format("%u/%u", wxMin(m_starsUsed, static_cast<unsigned int>(m_guideStars.size())),
                            static_cast<unsigned int>(m_guideStars.size()));
}

static void DebugWrite(const std::string& line)
{
    Debug.Write(wxString(line));
}

// Use secondary stars to refine Offset value if appropriate.  Return of true means offset has been adjusted
// (multistar_refine::Refine)
bool GuiderMultiStar::RefineOffset(const usImage *pImage, GuiderOffset *pOffset)
{
    m_starsUsed = 1;
    bool refined = false;

    // Primary star is in position 0 of the list
    try
    {
        if (IsGuiding() && m_guideStars.size() > 1 && pMount->GetGuidingEnabled() && !PhdController::IsSettling())
        {
            multistar_refine::Frame frame;
            frame.primaryX = m_primaryStar.X;
            frame.primaryY = m_primaryStar.Y;
            frame.primarySNR = m_primaryStar.SNR;
            frame.offsetX = pOffset->cameraOfs.X;
            frame.offsetY = pOffset->cameraOfs.Y;
            frame.maxStars = m_maxStars;
            frame.stabilitySigmaX = m_stabilitySigmaX;

            multistar_refine::State state;
            state.stabilizing = m_stabilizing;
            state.lockPositionMoved = m_lockPositionMoved;

            auto find = [this, pImage](GuideStar& star, double x, double y, bool verbose)
            {
                return star.Find(pImage, m_searchRegion, x, y, pFrame->GetStarFindMode(), GetMinStarHFD(), GetMaxStarHFD(),
                                 pCamera->GetSaturationADU(),
                                 verbose ? Star::FIND_LOGGING_VERBOSE : Star::FIND_LOGGING_MINIMAL);
            };
            auto validPosition = [this](double x, double y) { return IsValidSecondaryStarPosition(PHD_Point(x, y)); };

            multistar_refine::Result res = multistar_refine::Refine(m_guideStars, frame, state, *m_primaryDistStats, find,
                                                                    validPosition, multistar_refine::LogFn(DebugWrite));

            m_stabilizing = state.stabilizing;
            m_lockPositionMoved = state.lockPositionMoved;
            m_starsUsed = res.starsUsed;
            if (res.refined)
            {
                pOffset->cameraOfs.X = res.offsetX;
                pOffset->cameraOfs.Y = res.offsetY;
                refined = true;
            }
        }
    }
    catch (const wxString& msg)
    {
        Debug.Write(wxString::Format("MultiStar fault: exception at %d, %s, reverting to single-star mode\n", __LINE__, msg));
        m_multiStarMode = false;
    }

    return refined;
}

static frame_reject::DistanceChecker s_distanceChecker(DebugWrite);

bool GuiderMultiStar::UpdateCurrentPosition(const usImage *pImage, GuiderOffset *ofs, FrameDroppedInfo *errorInfo)
{
    if (!m_primaryStar.IsValid() && m_primaryStar.X == 0.0 && m_primaryStar.Y == 0.0)
    {
        Debug.Write("UpdateCurrentPosition: no star selected\n");
        errorInfo->starError = Star::STAR_ERROR;
        errorInfo->starMass = 0.0;
        errorInfo->starSNR = 0.0;
        errorInfo->starHFD = 0.0;
        errorInfo->status = _("No star selected");
        ImageLogger::LogImageStarDeselected(pImage);
        return true;
    }

    bool bError = false;

    try
    {
        Star newStar(m_primaryStar);

        if (!newStar.Find(pImage, m_searchRegion, pFrame->GetStarFindMode(), GetMinStarHFD(), GetMaxStarHFD(),
                          pCamera->GetSaturationADU(), Star::FIND_LOGGING_VERBOSE))
        {
            errorInfo->starError = newStar.GetError();
            errorInfo->starMass = 0.0;
            errorInfo->starSNR = 0.0;
            errorInfo->starHFD = 0.0;
            errorInfo->status = StarStatusStr(newStar);
            m_primaryStar.SetError(newStar.GetError());

            s_distanceChecker.Activate(monotonic_timers::ClockMs());
            ImageLogger::LogImage(pImage, *errorInfo);

            throw ERROR_INFO("UpdateCurrentPosition():newStar not found");
        }

        // check to see if it seems like the star we just found was the
        // same as the original star by comparing the mass
        if (m_massChangeThresholdEnabled)
        {
            int exposure;
            bool isAutoExp;
            pFrame->GetExposureInfo(&exposure, &isAutoExp);
            m_massChecker->SetExposure(exposure, isAutoExp);
            double limits[4];
            if (m_massChecker->CheckMass(newStar.Mass, m_massChangeThreshold, limits))
            {
                m_primaryStar.SetError(Star::STAR_MASSCHANGE);
                errorInfo->starError = Star::STAR_MASSCHANGE;
                errorInfo->starMass = newStar.Mass;
                errorInfo->starSNR = newStar.SNR;
                errorInfo->starHFD = newStar.HFD;
                errorInfo->status = StarStatusStr(m_primaryStar);
                pFrame->StatusMsg(wxString::Format(_("Mass: %.f vs %.f"), newStar.Mass, limits[1]));

                Debug.Write(wxString::Format(
                    "UpdateCurrentPosition: star mass new=%.1f exp=%.1f thresh=%.0f%% limits=(%.1f, %.1f, %.1f)\n",
                    newStar.Mass, limits[1], m_massChangeThreshold * 100., limits[0], limits[2], limits[3]));

                m_massChecker->AppendData(newStar.Mass, monotonic_timers::ClockMs());

                s_distanceChecker.Activate(monotonic_timers::ClockMs());
                ImageLogger::LogImage(pImage, *errorInfo);

                throw THROW_INFO("massChangeThreshold error");
            }
        }

        const PHD_Point& lockPos = LockPosition();
        double distance;
        bool raOnly = MyFrame::GuidingRAOnly();
        if (lockPos.IsValid())
        {
            if (raOnly)
                distance = fabs(newStar.X - lockPos.X);
            else
                distance = newStar.Distance(lockPos);
        }
        else
            distance = 0.;

        double tolerance = m_tolerateJumpsEnabled ? m_tolerateJumpsThreshold : 9e99;

        frame_reject::GuiderStatus guiderStatus;
        guiderStatus.guiding = IsGuiding();
        guiderStatus.paused = IsPaused();
        guiderStatus.settling = PhdController::IsSettling();
        guiderStatus.frameCount = CurrentErrorFrameCount();
        guiderStatus.avgDist = CurrentErrorSmoothed(raOnly);
        if (!s_distanceChecker.CheckDistance(distance, tolerance, guiderStatus, monotonic_timers::ClockMs()))
        {
            m_primaryStar.SetError(Star::STAR_ERROR);
            errorInfo->starError = Star::STAR_ERROR;
            errorInfo->starMass = newStar.Mass;
            errorInfo->starSNR = newStar.SNR;
            errorInfo->starHFD = newStar.HFD;
            errorInfo->status = StarStatusStr(m_primaryStar);
            pFrame->StatusMsg(_("Recovering"));

            ImageLogger::LogImage(pImage, *errorInfo);

            throw THROW_INFO("CheckDistance error");
        }

        ImageLogger::LogImage(pImage, distance);

        // update the star position, mass, etc.
        m_primaryStar = newStar;
        m_massChecker->AppendData(newStar.Mass, monotonic_timers::ClockMs());

        if (lockPos.IsValid())
        {
            ofs->cameraOfs = m_primaryStar - lockPos;
            if (m_multiStarMode && m_guideStars.size() > 1)
            {
                if (RefineOffset(pImage, ofs))
                    distance = hypot(ofs->cameraOfs.X, ofs->cameraOfs.Y); // Distance is reported to server clients
            }
            else
                m_starsUsed = 1;

            if (pMount && pMount->IsCalibrated())
                pMount->TransformCameraCoordinatesToMountCoordinates(ofs->cameraOfs, ofs->mountOfs, true);
            double distanceRA = ofs->mountOfs.IsValid() ? fabs(ofs->mountOfs.X) : 0.;
            UpdateCurrentDistance(distance, distanceRA);
        }

        pFrame->AdjustAutoExposure(m_primaryStar.SNR);
        pFrame->UpdateStatusBarStarInfo(m_primaryStar.SNR, m_primaryStar.GetError() == Star::STAR_SATURATED);
        errorInfo->status = StarStatus(m_primaryStar);
    }
    catch (const wxString& Msg)
    {
        POSSIBLY_UNUSED(Msg);
        bError = true;
        pFrame->ResetAutoExposure(); // use max exposure duration
    }

    return bError;
}

bool GuiderMultiStar::SetLockPosition(const PHD_Point& position)
{
    if (!Guider::SetLockPosition(position))
    {
        if (m_multiStarMode)
        {
            m_lockPositionMoved = true;
            m_stabilizing = true;
            Debug.Write("MultiStar: stabilizing after lock position change\n");
        }
        return false;
    }
    else
        return true;
}

bool GuiderMultiStar::IsValidLockPosition(const PHD_Point& pt)
{
    const usImage *pImage = CurrentImage();
    // this is a bit ugly as it is tightly coupled to Star::Find (edge_allowance.cpp)
    return ValidLockPosition(pt.X, pt.Y, pImage->Size.GetX(), pImage->Size.GetY(), m_searchRegion);
}

bool GuiderMultiStar::IsValidSecondaryStarPosition(const PHD_Point& pt)
{
    const usImage *pImage = CurrentImage();
    // As above, tightly coupled to Star::Find but with somewhat relaxed constraints. Find handles cases where search region is
    // only partly within image
    return pt.X >= 5 && pt.X + 5 < pImage->Size.GetX() && pt.Y >= 5 && pt.Y + 5 < pImage->Size.GetY();
}

void GuiderMultiStar::SaveStarFITS()
{
    double StarX = m_primaryStar.X;
    double StarY = m_primaryStar.Y;
    usImage *pImage = CurrentImage();
    usImage tmpimg;
    wxString imgLogDirectory;

    tmpimg.Init(60, 60);
    int start_x = ROUND(StarX) - 30;
    int start_y = ROUND(StarY) - 30;
    if ((start_x + 60) > pImage->Size.GetWidth())
        start_x = pImage->Size.GetWidth() - 60;
    if ((start_y + 60) > pImage->Size.GetHeight())
        start_y = pImage->Size.GetHeight() - 60;
    int x, y, width;
    width = pImage->Size.GetWidth();
    unsigned short *usptr = tmpimg.ImageData;
    for (y = 0; y < 60; y++)
    {
        for (x = 0; x < 60; x++, usptr++)
            *usptr = *(pImage->ImageData + (y + start_y) * width + (x + start_x));
    }

    imgLogDirectory = Debug.GetLogDir() + PATHSEPSTR + "PHD2_Stars";
    if (!wxDirExists(imgLogDirectory))
        wxFileName::Mkdir(imgLogDirectory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxString fname = imgLogDirectory + PATHSEPSTR + "PHD_GuideStar" + wxDateTime::Now().Format(_T("_%j_%H%M%S")) + ".fit";

    fitsfile *fptr; // FITS file pointer
    int status = 0; // CFITSIO status value MUST be initialized to zero!

    PHD_fits_create_file(&fptr, fname, false, &status);

    if (!status)
    {
        long fsize[] = { 60, 60 };
        fits_create_img(fptr, USHORT_IMG, 2, fsize, &status);

        FITSHdrWriter hdr(fptr, &status);

        hdr.write("DATE", wxDateTime::UNow(), wxDateTime::UTC, "file creation time, UTC");
        hdr.write("DATE-OBS", pImage->ImgStartTime, wxDateTime::UTC, "image capture start time, UTC");
        hdr.write("EXPOSURE", (float) pImage->ImgExpDur / 1000.0f, "Exposure time [s]");
        auto binning = pCamera->GetBinning();
        hdr.write("XBINNING", binning, "Camera X binning");
        hdr.write("YBINNING", binning, "Camera Y binning");
        hdr.write("XORGSUB", start_x, "Subframe x position in binned pixels");
        hdr.write("YORGSUB", start_y, "Subframe y position in binned pixels");

        if (!status)
        {
            long fpixel[] = { 1, 1, 1 };
            fits_write_pix(fptr, TUSHORT, fpixel, tmpimg.NPixels, tmpimg.ImageData, &status);
        }
    }

    PHD_fits_close_file(fptr);
}

wxString GuiderMultiStar::GetSettingsSummary() const
{
    // return a loggable summary of guider configs
    wxString s = wxString::Format(_T("Search region = %d px, Star mass tolerance "), GetSearchRegion());

    if (GetMassChangeThresholdEnabled())
        s += wxString::Format(_T("= %.1f%%"), GetMassChangeThreshold() * 100.0);
    else
        s += _T("disabled");

    if (m_multiStarMode)
        s += wxString::Format(_T(", Multi-star mode, list size = %d\n "), (int) m_guideStars.size());
    else
        s += ", Single-star mode\n";
    return s;
}
