/*
 *  multistar_refine.cpp
 *  PHD Guiding
 *
 *  See multistar_refine.h. The Debug lines of the refine step, in the formats
 *  GuiderMultiStar::RefineOffset wrote them with wxString::Format (#142).
 */

#include "multistar_refine.h"

#include <cstdio>

namespace
{
// snprintf into a std::string of the size the text needs, as wxString::Format gives
template<class... A>
std::string Format(const char *fmt, A... args)
{
    int n = snprintf(nullptr, 0, fmt, args...);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0)
        snprintf(s.data(), s.size() + 1, fmt, args...);
    return s;
}
} // namespace

namespace multistar_refine
{

std::string StarUse(int starNum, double dX, double dY, double weight, const std::string& flag)
{
    return Format("[#%d %0.2f,%0.2f,%0.2f,%s] ", starNum, dX, dY, weight, flag.c_str());
}

std::string Summary(bool refined, int validStars, double sumX, double sumY, double oneStarX, double oneStarY)
{
    return Format("%s, %d included, MultiStar: {%0.2f, %0.2f}, one-star: {%0.2f, %0.2f}\n", refined ? "refined" : "single-star",
                  validStars, sumX, sumY, oneStarX, oneStarY);
}

} // namespace multistar_refine
