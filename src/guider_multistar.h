/*
 *  guider_multistar.h
 *  PHD Guiding
 *
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
 *    Neither the name of Bret McKee, Dad Dog Development,
 *     Craig Stark, Stark Labs nor the names of its
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

#ifndef GUIDER_MULTISTAR_H_INCLUDED
#define GUIDER_MULTISTAR_H_INCLUDED

namespace frame_reject
{
class MassChecker;
}
class GuiderMultiStar;
class GuiderMultiStar : public Guider
{
    Star m_primaryStar;
    std::vector<GuideStar> m_guideStars;
    DescriptiveStats *m_primaryDistStats;
    frame_reject::MassChecker *m_massChecker;
    double m_lastPrimaryDistance;
    bool m_multiStarMode;
    bool m_stabilizing;
    bool m_lockPositionMoved;
    unsigned int m_starsUsed = 0;
    unsigned int m_lastStarsUsed;

    // parameters
    bool m_massChangeThresholdEnabled = false;
    double m_massChangeThreshold = 0.0;
    bool m_tolerateJumpsEnabled = false;
    double m_tolerateJumpsThreshold = 0.0;
    unsigned int m_maxStars;
    double m_stabilitySigmaX;

public:
    // Search-region bounds (px), shared by SetSearchRegion and the event-server
    // set_star_detection validation (single source of truth).
    static const int MIN_SEARCH_REGION = 7;
    static const int DEFAULT_SEARCH_REGION = 15;
    static const int MAX_SEARCH_REGION = 50;

    bool GetMassChangeThresholdEnabled() const;
    void SetMassChangeThresholdEnabled(bool enable);
    double GetMassChangeThreshold() const;
    bool SetMassChangeThreshold(double starMassChangeThreshold);
    bool SetTolerateJumps(bool enable, double threshold);
    bool GetTolerateJumpsEnabled() const { return m_tolerateJumpsEnabled; }
    double GetTolerateJumpsThreshold() const { return m_tolerateJumpsThreshold; }
    bool SetSearchRegion(int searchRegion);
    bool RefineOffset(const usImage *pImage, GuiderOffset *pOffset);

public:
    GuiderMultiStar();
    virtual ~GuiderMultiStar();

    virtual bool SetLockPosition(const PHD_Point& position) override;
    bool IsLocked() const override;
    bool AutoSelect(const wxRect& roi, bool willForceRecal) override;
    const PHD_Point& CurrentPosition() const override;
    wxRect GetBoundingBox() const override;
    int GetMaxMovePixels() const override;
    const Star& PrimaryStar() const override;
    bool GetMultiStarMode() const override;
    wxString GetStarCount() const override;
    void SetMultiStarMode(bool val) override;
    void ClearSecondaryStars();
    wxString GetSettingsSummary() const override;

    void LoadProfileSettings() override;

private:
    bool IsValidLockPosition(const PHD_Point& pt) final;
    bool IsValidSecondaryStarPosition(const PHD_Point& pt) final;
    void InvalidateCurrentPosition(bool fullReset = false) final;
    bool UpdateCurrentPosition(const usImage *pImage, GuiderOffset *ofs, FrameDroppedInfo *errorInfo) final;
    bool SetCurrentPosition(const usImage *pImage, const PHD_Point& position) final;

    void SaveStarFITS();
};

inline int GuiderMultiStar::GetMaxMovePixels() const
{
    return m_searchRegion;
}

inline const Star& GuiderMultiStar::PrimaryStar() const
{
    return m_primaryStar;
}

inline bool GuiderMultiStar::GetMultiStarMode() const
{
    return m_multiStarMode;
}

inline bool GuiderMultiStar::IsLocked() const
{
    return m_primaryStar.WasFound();
}

inline const PHD_Point& GuiderMultiStar::CurrentPosition() const
{
    return m_primaryStar;
}

#endif /* GUIDER_MULTISTAR_H_INCLUDED */
