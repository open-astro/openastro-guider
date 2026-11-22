/*
 *  multistar_refine.h
 *  PHD Guiding
 *
 *  The multi-star refine step (#142), moved unchanged from
 *  GuiderMultiStar::RefineOffset: the primary star's offset is replaced by the
 *  SNR-weighted mean of its own and the secondary stars' offsets from their
 *  reference points, when that mean is the smaller. A stabilisation period
 *  after a jump of the primary (over stabilitySigmaX sigma of its distances,
 *  until back at 2 sigma) suppresses it; secondaries that do not move at all
 *  are dropped as hot pixels, and those off by over 2.5 sigma are counted as
 *  misses and re-referenced after 10. A lock position move re-references them
 *  all when the stabilisation ends.
 *
 *  Refine works in place on the caller's star list (the primary at index 0) and
 *  is given the star finder, the position check and the Debug log, so it is
 *  tested on plain star records. Kept free of wx so it can be unit tested
 *  (tests/guiding/test_multistar_refine.cpp).
 */

#ifndef MULTISTAR_REFINE_H_INCLUDED
#define MULTISTAR_REFINE_H_INCLUDED

#include "guiding_stats.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace multistar_refine
{

using LogFn = std::function<void(const std::string&)>;

// The frame being refined
struct Frame
{
    double primaryX = 0.; // where the primary star was found
    double primaryY = 0.;
    double primarySNR = 0.;
    double offsetX = 0.; // the primary's camera offset from the lock position
    double offsetY = 0.;
    unsigned int maxStars = 0; // the most stars used, the primary included
    double stabilitySigmaX = 0.; // a primary distance over this times its sigma starts a stabilisation
};

// GuiderMultiStar's state carried from frame to frame
struct State
{
    bool stabilizing = false;
    bool lockPositionMoved = false;
};

struct Result
{
    bool refined = false; // offsetX/Y replace the primary's offset
    double offsetX = 0.;
    double offsetY = 0.;
    unsigned int starsUsed = 1; // GuiderMultiStar::m_starsUsed
};

// The compact per-star Debug entry "[#n dX,dY,weight,flag] "
std::string StarUse(int starNum, double dX, double dY, double weight, const std::string& flag);
// The closing Debug line when a secondary was averaged
std::string Summary(bool refined, int validStars, double sumX, double sumY, double oneStarX, double oneStarY);

// Star: a GuideStar-like record (X, Y, SNR, referencePoint.X/.Y, offsetFromPrimary.X/.Y,
// missCount, zeroCount, wasLost). find(star, x, y, verbose) runs Star::Find around (x, y) and
// updates the star; validPosition(x, y) is GuiderMultiStar::IsValidSecondaryStarPosition.
// distStats holds the primary's distances (GuiderMultiStar::m_primaryDistStats).
template<class Star, class FindFn, class ValidFn>
Result Refine(std::vector<Star>& stars, const Frame& frame, State& state, DescriptiveStats& distStats, FindFn find,
              ValidFn validPosition, const LogFn& log)
{
    Result res;
    double primaryDistance;
    double secondaryDistance;
    double primarySigma = 0;
    bool averaged = false;
    int validStars = 0;
    bool erasures = false;

    double sumWeights = 1;
    double sumX = frame.offsetX;
    double sumY = frame.offsetY;
    primaryDistance = hypot(sumX, sumY);

    distStats.AddValue(primaryDistance);

    auto index = [&stars](typename std::vector<Star>::iterator p) { return static_cast<int>(p - stars.begin()); };

    if (distStats.GetCount() > 5)
    {
        primarySigma = distStats.GetSigma();
        if (!state.stabilizing && primaryDistance > frame.stabilitySigmaX * primarySigma)
        {
            state.stabilizing = true;
            log("MultiStar: large primary error, entering stabilization period\n");
        }
        else if (state.stabilizing)
        {
            if (primaryDistance <= 2 * primarySigma)
            {
                state.stabilizing = false;
                log("MultiStar: exiting stabilization period\n");
                if (state.lockPositionMoved)
                {
                    state.lockPositionMoved = false;
                    log("MultiStar: updating star positions after lock position change\n");
                    for (auto pGS = stars.begin() + 1; pGS != stars.end();)
                    {
                        double expectedX = frame.primaryX + pGS->offsetFromPrimary.X;
                        double expectedY = frame.primaryY + pGS->offsetFromPrimary.Y;
                        bool found;
                        if (validPosition(expectedX, expectedY))
                            found = find(*pGS, expectedX, expectedY, true);
                        else
                            found = find(*pGS, pGS->X, pGS->Y, true);
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
                    return res; // All the secondary stars reference points reflect current positions
                }
            }
        }
    }
    else
        state.stabilizing = true; // get some data for primary star movement

    if (!state.stabilizing && stars.size() > 1 && (sumX != 0 || sumY != 0))
    {
        std::string secondaryInfo = "MultiStar: ";
        for (auto pGS = stars.begin() + 1; pGS != stars.end();)
        {
            if (res.starsUsed >= frame.maxStars || stars.size() == 1)
                break;
            bool found = false;
            if (pGS->wasLost)
            {
                // Look for it based on its original offset from the primary star
                found = find(*pGS, frame.primaryX + pGS->offsetFromPrimary.X, frame.primaryY + pGS->offsetFromPrimary.Y, false);
            }
            else
                // Look for it where we last found it
                found = find(*pGS, pGS->X, pGS->Y, false);
            if (found)
            {
                double dX = pGS->X - pGS->referencePoint.X;
                double dY = pGS->Y - pGS->referencePoint.Y;

                pGS->wasLost = false;
                res.starsUsed++;

                if (dX != 0. || dY != 0.)
                {
                    // Handle zero-counting - suspect results of exactly zero movement
                    if (dX == 0. || dY == 0.)
                        ++pGS->zeroCount;
                    else if (pGS->zeroCount > 0)
                        --pGS->zeroCount;

                    if (pGS->zeroCount == 5)
                    {
                        // Known bug, kept as is here (#142, pinned by test_multistar_refine's
                        // ZeroCountDropCountsTheNextStarTwice): the continue skips the loop tail
                        // below, so erasures is still true at the end of the next star's pass,
                        // which then does not advance pGS. The next star is found and averaged a
                        // second time (twice its weight, starsUsed counted twice); if that star is
                        // a miss, its ++pGS; continue passes the flag on to the star after it.
                        // Upstream PHD2 has the same code.
                        secondaryInfo += StarUse(index(pGS), 0, 0, 0, "DZ");
                        pGS = stars.erase(pGS);
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
                            secondaryInfo += StarUse(index(pGS), dX, dY, 0, "R");
                        }
                        else
                            secondaryInfo += StarUse(index(pGS), dX, dY, 0, "M" + std::to_string(pGS->missCount));
                        ++pGS;
                        continue;
                    }
                    else if (pGS->missCount > 0)
                    {
                        --pGS->missCount;
                    }

                    // At this point we have usable data from the secondary star
                    double wt = (pGS->SNR / frame.primarySNR);
                    sumX += wt * dX;
                    sumY += wt * dY;
                    sumWeights += wt;
                    averaged = true;
                    validStars++;

                    secondaryInfo += StarUse(index(pGS), dX, dY, wt, "U");
                }
                else // exactly zero on both axes, probably a hot pixel, drop it
                {
                    secondaryInfo += StarUse(index(pGS), 0, 0, 0, "DZ");
                    pGS = stars.erase(pGS);
                    erasures = true;
                }
            }
            else
            {
                // star not found in its search region
                secondaryInfo += StarUse(index(pGS), 0, 0, 0, "L");
                pGS->wasLost = true;
            }
            if (!erasures)
                ++pGS;
            else
                erasures = false;
        } // End of looping through secondary stars
        log(secondaryInfo + "\n");

        if (averaged)
        {
            sumX = sumX / sumWeights;
            sumY = sumY / sumWeights;
            if (hypot(sumX, sumY) < primaryDistance) // Apply average only if its smaller than single-star delta
            {
                res.offsetX = sumX;
                res.offsetY = sumY;
                res.refined = true;
            }
            log(Summary(res.refined, validStars, sumX, sumY, frame.offsetX, frame.offsetY));
        }
    }

    return res;
}

} // namespace multistar_refine

#endif // MULTISTAR_REFINE_H_INCLUDED
