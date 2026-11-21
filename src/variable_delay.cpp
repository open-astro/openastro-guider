/*
 *  variable_delay.cpp
 *  PHD Guiding
 *
 *  See variable_delay.h.
 */

#include "variable_delay.h"

namespace VariableDelay
{
int SecondsToMs(double seconds)
{
    return (int) seconds * 1000;
}

double MsToSeconds(int ms)
{
    return ms / 1000;
}
} // namespace VariableDelay
