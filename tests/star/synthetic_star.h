// Synthetic guide frames for the star-detection tests (#142): pixel-integrated Gaussian stars
// on a flat or sloped background, seeded Gaussian read noise, and 16-bit quantisation with a
// clip level. Pixel (x, y) covers [x - 0.5, x + 0.5] x [y - 0.5, y + 0.5], so a star centred at
// (10, 20) peaks on pixel (10, 20) and star_find::FindStar's coordinates are the truth's.
//
// The noise is Box-Muller over std::mt19937, whose output the standard fixes, so a seed gives the
// same frame on every standard library (std::normal_distribution does not).

#ifndef TESTS_STAR_SYNTHETIC_STAR_H
#define TESTS_STAR_SYNTHETIC_STAR_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace synth
{
constexpr double kPi = 3.14159265358979323846;

struct Frame
{
    int width = 0;
    int height = 0;
    std::vector<double> value; // ADU, before quantisation

    Frame(int w, int h, double background = 0.0) : width(w), height(h), value((size_t) w * h, background) { }

    double& at(int x, int y) { return value[(size_t) y * width + x]; }
};

// The fraction of a unit Gaussian of sigma centred at c that falls on pixel i
inline double PixelFraction(int i, double c, double sigma)
{
    double const k = 1.0 / (sigma * std::sqrt(2.0));
    return 0.5 * (std::erf((i + 0.5 - c) * k) - std::erf((i - 0.5 - c) * k));
}

// Adds a star of total flux (ADU summed over the plane) centred at (cx, cy)
inline void AddStar(Frame& f, double cx, double cy, double sigma, double flux)
{
    for (int y = 0; y < f.height; ++y)
    {
        double const fy = PixelFraction(y, cy, sigma);
        if (fy == 0.0)
            continue;
        for (int x = 0; x < f.width; ++x)
            f.at(x, y) += flux * fy * PixelFraction(x, cx, sigma);
    }
}

// The total flux of a star of sigma whose point-sampled peak would be peak ADU
inline double FluxForPeak(double peak, double sigma)
{
    return peak * 2.0 * kPi * sigma * sigma;
}

// Adds slope ADU per pixel along x (0 at x = 0)
inline void AddGradientX(Frame& f, double slope)
{
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x)
            f.at(x, y) += slope * x;
}

// Adds seeded Gaussian noise of sigma ADU to every pixel
inline void AddNoise(Frame& f, double sigma, uint32_t seed)
{
    std::mt19937 rng(seed);
    auto uniform = [&rng]() { return ((double) rng() + 1.0) / 4294967297.0; }; // (0, 1)
    for (size_t i = 0; i < f.value.size(); i += 2)
    {
        double const r = std::sqrt(-2.0 * std::log(uniform()));
        double const t = 2.0 * kPi * uniform();
        f.value[i] += sigma * r * std::cos(t);
        if (i + 1 < f.value.size())
            f.value[i + 1] += sigma * r * std::sin(t);
    }
}

// Rounds to unsigned 16-bit, clipped to [0, clip]
inline std::vector<unsigned short> Quantize(const Frame& f, unsigned clip = 65535)
{
    std::vector<unsigned short> px(f.value.size());
    for (size_t i = 0; i < px.size(); ++i)
    {
        double const v = std::floor(f.value[i] + 0.5);
        px[i] = (unsigned short) std::clamp(v, 0.0, (double) clip);
    }
    return px;
}
} // namespace synth

#endif
