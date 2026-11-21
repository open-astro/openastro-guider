/*
 *  variable_delay.cpp
 *  PHD Guiding
 *
 *  See variable_delay.h.
 */

#include "variable_delay.h"

#include <cmath>

namespace VariableDelay
{
int SecondsToMs(double seconds)
{
    // multiply first, then round: 2.5 s is 2500 ms, and 1.001 s, which comes to 1000.9999999999999 ms in binary, is 1001
    return (int) std::lround(seconds * 1000.0);
}

double MsToSeconds(int ms)
{
    return ms / 1000.0;
}
} // namespace VariableDelay
