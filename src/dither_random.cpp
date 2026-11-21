/*
 *  dither_random.cpp
 *  PHD Guiding
 *
 *  See dither_random.h.
 */

#include "dither_random.h"

#include <chrono>
#include <exception>
#include <iterator>

DitherRandom::DitherRandom()
{
    // std::random_device can throw where it has no entropy source, and on some platforms it is a
    // fixed sequence; the clock mixed in still gives each start its own seed then
    std::uint32_t words[4] = {};
    try
    {
        std::random_device rd;
        words[0] = rd();
        words[1] = rd();
    }
    catch (const std::exception&)
    {
    }
    auto now = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    words[2] = static_cast<std::uint32_t>(now);
    words[3] = static_cast<std::uint32_t>(now >> 32);
    std::seed_seq seq(std::begin(words), std::end(words));
    m_gen.seed(seq);
}

DitherRandom::DitherRandom(std::uint32_t seed) : m_gen(seed) { }

double DitherRandom::Unit()
{
    // as rand() / (double) RAND_MAX * 2 - 1 was
    constexpr double range = static_cast<double>(std::mt19937::max() - std::mt19937::min());
    return static_cast<double>(m_gen() - std::mt19937::min()) / range * 2.0 - 1.0;
}

void DitherRandom::GetDither(double amount, bool raOnly, double *dRa, double *dDec)
{
    *dRa = amount * Unit();
    *dDec = raOnly ? 0.0 : amount * Unit();
}
