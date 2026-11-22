/*
 *  guide_history.cpp
 *  PHD Guiding
 *
 *  The guide-step history and summary stats behind get_guide_history. See
 *  guide_history.h. The logic is lifted from GraphLogClientWindow (graph.cpp):
 *  the length validation of SetMinLength/SetMaxLength, ResetData, UpdateStats
 *  and the three AppendData overloads, in the same order. The trend lines,
 *  dither markers and stats-window updates were display only and are gone.
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

#include "guide_history.h"

#include <cmath>

const char *GuideHistoryDirection(int dir, int duration)
{
    if (duration <= 0)
        return "";
    // the order of GUIDE_DIRECTION, which this file can't include (mount.h needs wx)
    static const char *const letters[] = { "N", "S", "E", "W" };
    return dir >= 0 && dir < 4 ? letters[dir] : "";
}

unsigned int GuideHistory::ValidMinLength(unsigned int minLength)
{
    return minLength < 1 ? (unsigned int) DefaultMinLength : minLength;
}

GuideHistory::GuideHistory(unsigned int minLength, unsigned int maxLength, unsigned int length, std::int64_t timeBaseMs)
    : m_minLength(ValidMinLength(minLength)), m_length(length), m_timeBase(timeBaseMs), m_ditherStarted(false),
      m_raSameSides(0), m_stats()
{
    if (maxLength < m_minLength)
        maxLength = m_minLength;
    m_history.resize(maxLength); // sizes the ring; circular_buffer allows this only once

    m_noDitherRA.ChangeWindowSize(m_length);
    m_noDitherDec.ChangeWindowSize(m_length);

    ResetData();
}

void GuideHistory::ResetData()
{
    m_history.clear();
    m_noDitherDec.ClearAll();
    m_noDitherRA.ClearAll();
    m_raSameSides = 0;
    m_ditherStarted = false;
    UpdateStats(0, nullptr);
    // after UpdateStats, which sets the peaks from the (now empty) windowed stats
    m_stats.ra_peak = m_stats.dec_peak = 0.0;
    m_stats.star_lost_cnt = 0;
    m_stats.ra_limit_cnt = m_stats.dec_limit_cnt = 0;
}

void GuideHistory::UpdateStats(unsigned int nr, const S_HISTORY *cur)
{
    m_stats.nr = nr;
    m_stats.rms_ra = m_noDitherRA.GetPopulationSigma();
    m_stats.rms_dec = m_noDitherDec.GetPopulationSigma();
    m_stats.rms_tot = std::hypot(m_stats.rms_ra, m_stats.rms_dec);
    m_stats.ra_peak = std::max(std::fabs(m_noDitherRA.GetMaxDisplacement()), std::fabs(m_noDitherRA.GetMinDisplacement()));
    m_stats.dec_peak = std::max(std::fabs(m_noDitherDec.GetMaxDisplacement()), std::fabs(m_noDitherDec.GetMinDisplacement()));

    if (nr >= 2)
    {
        m_stats.osc_index = 1.0 - (double) m_raSameSides / (double) (nr - 1);
        m_stats.osc_alert = m_stats.osc_index > 0.6 || m_stats.osc_index < 0.15;
    }
    else
    {
        m_stats.osc_index = 0.0;
        m_stats.osc_alert = false;
    }

    m_stats.cur = cur ? *cur : S_HISTORY {};
}

void GuideHistory::AppendData(const S_HISTORY& step, bool settling)
{
    unsigned int trend_items = ItemCount();
    const int oldest_idx = m_history.size() - trend_items;

    S_HISTORY oldest {};
    if (m_history.size() > 0)
        oldest = m_history[oldest_idx];

    // update counter for osc index
    if (trend_items >= 1)
    {
        if (step.ra * m_history[m_history.size() - 1].ra > 0.0)
            ++m_raSameSides;
        if (trend_items >= m_length)
        {
            if (m_history[oldest_idx].ra * m_history[oldest_idx + 1].ra > 0.0)
                --m_raSameSides;
        }
    }

    m_stats.ra_limit_cnt += (step.raLimited ? 1 : 0) - ((trend_items >= m_length && oldest.raLimited) ? 1 : 0);
    m_stats.dec_limit_cnt += (step.decLimited ? 1 : 0) - ((trend_items >= m_length && oldest.decLimited) ? 1 : 0);

    m_history.push_front(step);

    if (m_ditherStarted)
        m_ditherStarted = false;
    else if (!settling)
    {
        // DeltaT only feeds the linear-fit sums, which nothing here reports
        double dt = (double) (step.timestamp - m_timeBase);
        m_noDitherDec.AddGuideInfo(dt, step.dec, step.decDur);
        m_noDitherRA.AddGuideInfo(dt, step.ra, step.raDur);
    }

    UpdateStats(ItemCount(), &step);
}

void GuideHistory::AppendData(const FrameDroppedInfo&)
{
    ++m_stats.star_lost_cnt;
}

void GuideHistory::AppendData(const DitherInfo&)
{
    m_ditherStarted = true;
}
