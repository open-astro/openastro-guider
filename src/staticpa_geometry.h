/*
 *  staticpa_geometry.h
 *  PHD Guiding
 *
 *  Pure geometry / astrometry kernels for the Static Polar Alignment tool,
 *  factored out of StaticPaToolWin (a wxFrame) so they can be unit tested
 *  without a GUI, a mount connection, or the wall clock.
 *
 *  Everything here is a free function over plain numbers: no wxWidgets, no
 *  Debug log, no member state, no I/O. The StaticPaToolWin methods
 *  (CalcRotationCentre / CalcAdjustments / Radec2Px / J2000Now) read their
 *  member/instrument/time inputs and then delegate the math to these.
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

#ifndef STATICPA_GEOMETRY_H_INCLUDED
#define STATICPA_GEOMETRY_H_INCLUDED

namespace staticpa_geom
{

// Minimal value type so these kernels stay free of the wx-coupled PHD_Point
// (point.h also declares ShiftPoint, which pulls in wxLongLong). Callers in
// StaticPaToolWin convert to/from PHD_Point at the boundary.
struct Px
{
    double x = 0.;
    double y = 0.;
};

// Centre + radius of the rotation circle, in sensor pixels. `valid` is false
// for degenerate inputs (see the CircleFrom* docs); when false cx/cy/r are
// left at 0 rather than NaN/inf, and callers must reject the result.
//
// Note: a valid result can still have a very large (but finite) radius for
// near-degenerate geometry (near-collinear points, or a tiny rotation). This
// is intentional and matches upstream PHD2: in polar alignment a well-aligned
// mount or an off-sensor pole legitimately yields a large radius, so the
// magnitude of r is NOT an error condition and is deliberately not capped.
struct Circle
{
    double cx = 0.;
    double cy = 0.;
    double r = 0.;
    bool valid = true;
};

// A pair of correction vectors (each an offset in sensor pixels).
struct CorrVectors
{
    Px a;
    Px b;
};

// --- Centre of rotation -----------------------------------------------------

// Manual mode: the unique circle through three measured star positions, via
// the determinant (matrix-minor) construction. Mirrors the !m_auto branch of
// CalcRotationCentre(). If the three points are collinear (or coincident) no
// finite circle exists; the result has valid == false and cx/cy/r == 0.
Circle CircleFrom3Points(const Px& p1, const Px& p2, const Px& p3);

// Auto mode: the circle implied by two measured positions on the arc plus the
// RA rotation between them. raDiffRad is the (already hemisphere-signed,
// sign-inverted, normalised) image rotation in radians; hemi is +1 (north) or
// -1 (south). Mirrors the m_auto branch of CalcRotationCentre(). If
// slopebaseRadOut is non-null it receives the chord-to-CoR slope in radians
// (used only for the diagnostic log). If raDiffRad is ~0 (no rotation between
// the two shots) the radius is undefined; the result has valid == false and
// cx/cy/r == 0.
Circle CircleFrom2PointsAndAngle(const Px& p1, const Px& p2, double raDiffRad, int hemi, double *slopebaseRadOut = nullptr);

// Decompose the offset of the centre of rotation from the sensor centre into
// Dec and Cone (orthogonal axis) correction vectors. Mirrors the tail of
// CalcRotationCentre(). Returns { Dec, Cone }.
CorrVectors DecomposeCoR(const Circle& cor, int widthPx, int heightPx, double camAngleDeg);

// --- Reference-star projection / adjustments --------------------------------

// Project a star's (RA, Dec) in degrees to a pixel offset from the centre of
// rotation. raMountDeg is the mount RA reference already corrected for hour
// angle (the caller resolves it from the mount or the clock). Mirrors the
// tail of Radec2Px() (everything after ra_deg is known).
Px Radec2Px(double starRaDeg, double starDecDeg, double pxScale, double raMountDeg, double camAngleDeg, bool flip, int hemi);

// Decompose the residual between a reference star's predicted and measured
// pixel positions into Alt and Az correction vectors. camAngleDeg is the
// camera rotation; haDeg is the hour angle in degrees. Mirrors the math core
// of CalcAdjustments(). Returns { Az, Alt } plus the scalar pixel errors.
struct AltAzResult
{
    CorrVectors vec; // vec.a = Az, vec.b = Alt
    double altErrPx = 0.;
    double azErrPx = 0.;
    double totErrPx = 0.;
    // Diagnostic intermediates (degrees), exposed only for the debug log.
    double rarot = 0.;
    double harot = 0.;
    double hcorAngle = 0.;
};
AltAzResult DecomposeAltAz(const Px& target, const Px& measured, double camAngleDeg, double haDeg);

// --- Precession -------------------------------------------------------------

// Precess J2000 (RA, Dec) in degrees forward by daysSinceJ2000 days, using the
// Capitaine/Wallace/Chapront series (terms to t^3). The caller supplies the
// elapsed days so this stays clock-free. Mirrors J2000Now() after JDnow.
Px PrecessJ2000(double daysSinceJ2000, double raDeg, double decDeg);

} // namespace staticpa_geom

#endif // STATICPA_GEOMETRY_H_INCLUDED
