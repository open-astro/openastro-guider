/*
 *  staticpa_geometry.cpp
 *  PHD Guiding
 *
 *  Pure geometry / astrometry kernels for the Static Polar Alignment tool.
 *  See staticpa_geometry.h for the rationale. Each function is a verbatim
 *  lift of the math previously inline in StaticPaToolWin, with variable names
 *  preserved so the diff against the originals is easy to audit.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of openphdguiding.org nor the names of its
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

#include "staticpa_geometry.h"

#include <cmath>

namespace
{

// File-local copies of the angle helpers used by the PA math. These mirror
// the definitions in image_math.h, duplicated here only so this module stays
// free of image_math.h's wxWidgets coupling.
double norm(double val, double start, double end)
{
    double const range = end - start;
    double const ofs = val - start;
    return val - std::floor(ofs / range) * range;
}

double degrees(double radians)
{
    return radians * 180. / M_PI;
}

double radians(double degrees)
{
    return degrees * M_PI / 180.;
}

} // namespace

namespace staticpa_geom
{

Circle CircleFrom3Points(const Px& p1, const Px& p2, const Px& p3)
{
    double x1 = p1.x, y1 = p1.y;
    double x2 = p2.x, y2 = p2.y;
    double x3 = p3.x, y3 = p3.y;

    double a, b, c, e, f, g, i, j, k;
    double m11, m12, m13, m14;
    // |A| = aei + bfg + cdh -ceg -bdi -afh
    // a b c
    // d e f
    // g h i
    // a= x1^2+y1^2; b=x1; c=y1; d=1
    // e= x2^2+y2^2; f=x2; g=y2; h=1
    // i= x3^2+y3^2; j=x3; k=y3; l=1
    //
    // x0 = 1/2.|M12|/|M11|
    // y0 = -1/2.|M13|/|M11|
    // r = x0^2 + y0^2 + |M14| / |M11|
    //
    a = x1 * x1 + y1 * y1;
    b = x1;
    c = y1;
    e = x2 * x2 + y2 * y2;
    f = x2;
    g = y2;
    i = x3 * x3 + y3 * y3;
    j = x3;
    k = y3;
    m11 = b * g + c * j + f * k - g * j - c * f - b * k;
    m12 = a * g + c * i + e * k - g * i - c * e - a * k;
    m13 = a * f + b * i + e * j - f * i - b * e - a * j;
    m14 = a * f * k + b * g * i + c * e * j - c * f * i - b * e * k - a * g * j;

    Circle out;
    out.cx = (1. / 2.) * m12 / m11;
    out.cy = (-1. / 2.) * m13 / m11;
    out.r = std::sqrt(out.cx * out.cx + out.cy * out.cy + m14 / m11);
    return out;
}

Circle CircleFrom2PointsAndAngle(const Px& p1, const Px& p2, double radiff, int hemi, double *slopebaseRadOut)
{
    double x1 = p1.x, y1 = p1.y;
    double x2 = p2.x, y2 = p2.y;

    // Alternative algorithm based on two points and angle rotated
    double theta2 = radiff / 2.0; // Half the image rotation for midpoint of chord
    double lenchord = std::hypot(x1 - x2, y1 - y2);
    double cr = std::fabs(lenchord / 2.0 / std::sin(theta2));
    double lenbase = std::fabs(cr * std::cos(theta2));
    // Calculate the slope of the chord in pixels
    // We know the image is moving clockwise in NH and anti-clockwise in SH
    // So subtract PI/2 in NH or add PI/2 in SH to get the slope to the CoR
    // Invert y values as pixels are +ve downwards
    double slopebase = std::atan2(y1 - y2, x2 - x1) - hemi * M_PI / 2.0;

    Circle out;
    out.cx = (x1 + x2) / 2.0 + lenbase * std::cos(slopebase);
    out.cy = (y1 + y2) / 2.0 - lenbase * std::sin(slopebase); // subtract for pixels
    out.r = cr;
    if (slopebaseRadOut)
        *slopebaseRadOut = slopebase;
    return out;
}

CorrVectors DecomposeCoR(const Circle& cor, int widthPx, int heightPx, double camAngleDeg)
{
    double cx = cor.cx, cy = cor.cy;
    int xpx = widthPx, ypx = heightPx;

    // Distance and angle of CoR from centre of sensor
    double cor_r = std::hypot(xpx / 2 - cx, ypx / 2 - cy);
    double cor_a = degrees(std::atan2(ypx / 2 - cy, xpx / 2 - cx));
    double rarot = -camAngleDeg;
    // Cone and Dec components of CoR vector
    double dec_r = cor_r * std::sin(radians(cor_a - rarot));
    double cone_r = cor_r * std::cos(radians(cor_a - rarot));

    CorrVectors out;
    out.a.x = -dec_r * std::sin(radians(rarot)); // Dec
    out.a.y = dec_r * std::cos(radians(rarot));
    out.b.x = cone_r * std::cos(radians(rarot)); // Cone
    out.b.y = cone_r * std::sin(radians(rarot));
    return out;
}

Px Radec2Px(double starRaDeg, double starDecDeg, double pxScale, double ra_deg, double camAngleDeg, bool flip, int hemi)
{
    // Convert dec to pixel radius
    double r = (90.0 - std::fabs(starDecDeg)) * 3600 / pxScale;

    // Target hour angle - or rather the rotation needed to correct.
    // HA = LST - RA
    // In NH HA decreases clockwise; RA increases clockwise
    // "Up" is HA=0
    // Sensor "up" is 90deg counterclockwise from mount RA plus rotation
    // Star rotation is RAstar - RAmount
    double a1 = starRaDeg - (ra_deg - 90.0);
    a1 = norm(a1, 0, 360);

    double l_camAngle;
    l_camAngle = norm(flip ? camAngleDeg + 180.0 : camAngleDeg, 0, 360);
    double a = l_camAngle - a1 * hemi;

    Px px;
    px.x = r * std::cos(radians(a));
    px.y = -r * std::sin(radians(a));
    return px;
}

AltAzResult DecomposeAltAz(const Px& target, const Px& measured, double camAngleDeg, double ha_deg)
{
    double xt = target.x, yt = target.y;
    double xs = measured.x, ys = measured.y;

    double hcor_r = std::hypot(xt - xs, yt - ys); // xt,yt: target, xs,ys: measured
    double hcor_a = degrees(std::atan2(yt - ys, xt - xs));
    double rarot = -camAngleDeg;
    double harot = norm(rarot - (90 + ha_deg), 0, 360);
    double hrot = norm(hcor_a - harot, 0, 360);

    double az_r = hcor_r * std::sin(radians(hrot));
    double alt_r = hcor_r * std::cos(radians(hrot));

    AltAzResult out;
    out.vec.a.x = -az_r * std::sin(radians(harot)); // Az
    out.vec.a.y = az_r * std::cos(radians(harot));
    out.vec.b.x = alt_r * std::cos(radians(harot)); // Alt
    out.vec.b.y = alt_r * std::sin(radians(harot));
    out.altErrPx = alt_r;
    out.azErrPx = az_r;
    out.totErrPx = hcor_r;
    out.rarot = rarot;
    out.harot = harot;
    out.hcorAngle = hcor_a;
    return out;
}

Px PrecessJ2000(double JDnow, double raDeg, double decDeg)
{
    double tnow = JDnow / 36525; // JDNow is days since J2000.0 so no need to subtract JD2000
    double t2 = std::pow(tnow, 2);
    double t3 = std::pow(tnow, 3);
    double zed, zeta, theta; // arcseconds
    double zedrad, zetarad, thetarad;
    zeta = 2.5976176 + 2306.0809506 * tnow + 0.3019015 * t2 + 0.0179663 * t3;
    zetarad = radians(zeta / 3600);
    zed = -2.5976176 + 2306.0803226 * tnow + 1.0947790 * t2 + 0.0182273 * t3;
    zedrad = radians(zed / 3600);
    theta = 2004.1917476 * tnow - 0.4269353 * t2 - 0.0418251 * t3;
    thetarad = radians(theta / 3600);

    //  Build the transformation matrix
    double Xx, Xy, Xz, Yx, Yy, Yz, Zx, Zy, Zz;
    Xx = std::cos(zedrad) * std::cos(thetarad) * std::cos(zetarad) - std::sin(zedrad) * std::sin(zetarad);
    Yx = -std::cos(zedrad) * std::cos(thetarad) * std::sin(zetarad) - std::sin(zedrad) * std::cos(zetarad);
    Zx = -std::cos(zedrad) * std::sin(thetarad);
    Xy = std::sin(zedrad) * std::cos(thetarad) * std::cos(zetarad) + std::cos(zedrad) * std::sin(zetarad);
    Yy = -std::sin(zedrad) * std::cos(thetarad) * std::sin(zetarad) + std::cos(zedrad) * std::cos(zetarad);
    Zy = -std::sin(zedrad) * std::sin(thetarad);
    Xz = std::sin(thetarad) * std::cos(zetarad);
    Yz = -std::sin(thetarad) * std::sin(zetarad);
    Zz = std::cos(thetarad);

    // Transform coordinates;
    double x0, y0, z0;
    double x, y, z;
    x0 = std::cos(radians(decDeg)) * std::cos(radians(raDeg));
    y0 = std::cos(radians(decDeg)) * std::sin(radians(raDeg));
    z0 = std::sin(radians(decDeg));
    x = Xx * x0 + Yx * y0 + Zx * z0;
    y = Xy * x0 + Yy * y0 + Zy * z0;
    z = Xz * x0 + Yz * y0 + Zz * z0;
    double radeg, decdeg;
    radeg = norm(degrees(std::atan2(y, x)), 0, 360);
    decdeg = degrees(std::atan2(z, std::sqrt(1 - z * z)));

    Px out;
    out.x = radeg;
    out.y = decdeg;
    return out;
}

} // namespace staticpa_geom
