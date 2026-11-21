/*
 *  dither_random.h
 *  PHD Guiding
 *
 *  The offsets of a random dither (DITHER_RANDOM in MyFrame::Dither): RA, and
 *  Dec unless the dither is RA only, each uniform in [-amount, +amount].
 *  Each generator has its own std::mt19937, seeded from std::random_device
 *  when default-constructed, so a daemon start does not repeat the last
 *  start's dithers (#173); tests inject a seed for a fixed sequence.
 *  Kept free of wx so it can be unit tested (tests/guiding/test_dither_random.cpp).
 */

#ifndef DITHER_RANDOM_H_INCLUDED
#define DITHER_RANDOM_H_INCLUDED

#include <cstdint>
#include <random>

class DitherRandom
{
public:
    DitherRandom(); // seeded from std::random_device
    explicit DitherRandom(std::uint32_t seed); // a fixed sequence, for tests

    // Draws RA, then Dec unless raOnly (then *dDec is 0 and nothing is drawn for it)
    void GetDither(double amount, bool raOnly, double *dRa, double *dDec);

private:
    double Unit(); // uniform in [-1, +1], both ends included

    std::mt19937 m_gen;
};

#endif // DITHER_RANDOM_H_INCLUDED
