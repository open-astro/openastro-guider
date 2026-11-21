/*
 *  dither_random.cpp
 *  PHD Guiding
 *
 *  See dither_random.h.
 */

#include "dither_random.h"

#include <cstdlib>

DitherRandom::DitherRandom() { }

DitherRandom::DitherRandom(std::uint32_t seed)
{
    (void) seed;
}

double DitherRandom::Unit()
{
    return (rand() / (double) RAND_MAX) * 2.0 - 1.0;
}

void DitherRandom::GetDither(double amount, bool raOnly, double *dRa, double *dDec)
{
    *dRa = amount * Unit();
    *dDec = raOnly ? 0.0 : amount * Unit();
}
