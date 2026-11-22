/*
 *  guide_history.h
 *  PHD Guiding
 *
 *  The guide-step history and summary stats behind the JSON-RPC
 *  get_guide_history method (#115 step 2). This is the data store that used
 *  to live inside the History graph window (GraphLogClientWindow), moved out
 *  without the display parts so the guider can run with no windows. Kept
 *  free of wx so it can be unit tested (tests/guide_history/).
 */

#ifndef GUIDE_HISTORY_H_INCLUDED
#define GUIDE_HISTORY_H_INCLUDED

#include <algorithm>
#include <cstdint>

#include "circbuf.h"
#include "guiding_stats.h"

struct FrameDroppedInfo; // guidinglog.h; only its arrival is counted

struct S_HISTORY
{
    std::int64_t timestamp; // ms UTC when the step was recorded
    double dx;
    double dy;
    double ra;
    double dec;
    double starSNR;
    double starMass;
    int raDur;
    int decDur;
    char raDir; // must stay char: get_guide_history emits "" for values below ' '
    char decDir;
    bool raLimited;
    bool decLimited;
};

struct DitherInfo
{
    std::int64_t timestamp;
    double dRa;
    double dDec;
};

struct SummaryStats
{
    S_HISTORY cur;
    unsigned int nr;
    double rms_ra;
    double rms_dec;
    double rms_tot;
    double osc_index;
    bool osc_alert;
    double ra_peak;
    double dec_peak;
    unsigned int star_lost_cnt;
    unsigned int ra_limit_cnt;
    unsigned int dec_limit_cnt;
};

// Guide-step history and summary stats behind get_guide_history.
// Not thread safe: feed and read it from the main thread only.
class GuideHistory
{
public:
    enum
    {
        DefaultMinLength = 50,
        DefaultMaxLength = 400,
    };

    // The minimum length a /graph/minLength value gives: minLength < 1 -> DefaultMinLength.
    // The default stats window (/graph/length) is twice this.
    static unsigned int ValidMinLength(unsigned int minLength);

    // minLength is validated with ValidMinLength. maxLength is the history capacity;
    // below minLength it becomes minLength. length is the stats window (n, rms, osc index,
    // peaks, limit counts); it is not clamped to the capacity. The window is fixed for the
    // life of the object, because the running counters are only valid for one window.
    GuideHistory(unsigned int minLength, unsigned int maxLength, unsigned int length, std::int64_t timeBaseMs);

    unsigned int MinLength() const { return m_minLength; }
    unsigned int Capacity() const { return m_history.capacity(); }
    unsigned int Length() const { return m_length; }

    // A guide step. Steps while settling, and the first step after a dither, are left out
    // of rms and peak but still counted in n, the osc index and the limit counts.
    void AppendData(const S_HISTORY& step, bool settling);
    // A lost star (counted in star_lost_cnt until the next ResetData).
    void AppendData(const FrameDroppedInfo&);
    // A dither: the next guide step is left out of rms and peak.
    void AppendData(const DitherInfo&);
    // Empty the history and zero the stats.
    void ResetData();

    // Index 0 is the oldest step, size() - 1 the newest.
    const circular_buffer<S_HISTORY>& History() const { return m_history; }
    const SummaryStats& Stats() const { return m_stats; }

private:
    unsigned int ItemCount() const { return std::min(m_history.size(), m_length); }
    void UpdateStats(unsigned int nr, const S_HISTORY *cur);

    unsigned int m_minLength;
    unsigned int m_length;
    circular_buffer<S_HISTORY> m_history;
    WindowedAxisStats m_noDitherRA;
    WindowedAxisStats m_noDitherDec;
    std::int64_t m_timeBase;
    bool m_ditherStarted;
    int m_raSameSides; // accumulator for the RA osc index
    SummaryStats m_stats;
};

#endif
