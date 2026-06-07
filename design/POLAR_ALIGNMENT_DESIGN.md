# Polar Alignment — design

A design for a fast, accurate, hardware-agnostic polar-alignment (PA) routine for **ARA**,
taking the best of ASIAIR / iPolar / PoleMaster / PHD2 and — crucially — **solving the
"which bolt, which way, how much" problem** for users with any optical train (guide scope,
OAG, short or long focal length).

Status: **proposal** (not yet built). Owner: ARA + openastro-guider.

---

## 1. Goals / non-goals

**Goals**
- Fast (seconds-to-a-couple-minutes), accurate (sub-arcmin achievable).
- **Works anywhere** — no requirement that Polaris / the celestial pole be in the field.
- Works across optical trains: guide scope, OAG, main scope; any focal length / pixel scale.
- A live-adjust experience that makes bolt-turning **unambiguous** (the headline UX win).
- No mandatory extra hardware; reuse the camera the user already has.

**Non-goals (for v1)**
- Replacing PHD2's existing GUI Static-PA / drift tools (they stay for desktop use).
- Driving third-party dedicated polar cams (PoleMaster/iPolar) — an optional later passthrough.
- Building a plate solver inside openastro-guider (the guider is not a solver; see §4).

---

## 2. Background: the two method families

| Family | Examples | Pole in FOV? | How | Speed |
|---|---|---|---|---|
| Near-pole center-of-rotation | PoleMaster, iPolar, **PHD2 Static PA** | **required** | rotate RA, fit center of star arcs, live "dot-to-bullseye" | fast |
| Plate-solve arc | **ASIAIR (2-pt)**, NINA TPPA (3-pt) | not needed | solve → derive RA axis → live error | fast (ASIAIR) / slow (TPPA) |
| Drift | PHD2 drift tools | n/a | watch star drift | slowest |

All the *good* ones share a **continuous live-adjust loop**. The slow ones re-measure between
adjustments. ASIAIR's win over TPPA is **2 points + a slick live loop**, not better math.

**Decision:** base v1 on the **plate-solve family** (ASIAIR-style), because it is hardware- and
sky-position agnostic — it removes the "is the pole in my little guide-cam FOV?" friction
entirely and works the same for a 50 mm guide scope and an OAG on a 2000 mm scope. We keep the
**near-pole center-of-rotation** path as an optional mode when the pole *is* framed (it enables
the most intuitive literal "dot-to-bullseye" UI), reusing the existing geometry kernel (§6).

---

## 3. The core problem: "which bolt, which way, how much"

This is the actual UX battle. The fix is that **a plate solve yields the full sky→image
transform (rotation + parity)**, which is the piece "turn-and-pray" tools lack. With it:

- We know where the **true refracted pole** is and where the **mount's RA axis** currently
  points (from §5), as an alt/az error vector.
- We know the on-sky directions of **+Altitude** and **+Azimuth**, so we can project them into
  **image space** as two arrows.

Two presentation modes, chosen by whether the pole is framed (§7):

- **Reticle mode (pole in frame):** the literal iPolar UX — draw the true-pole target + the
  current-axis marker; user "drives the dot into the bullseye." No numbers, no bolt names — they
  turn whatever shrinks the gap and the live view confirms instantly.
- **Decoupled mode (pole not in frame):** show **Altitude** and **Azimuth** *separately*, each as
  *direction arrow + arcmin remaining*, updating live. Decoupling is essential — chasing a single
  coupled 2-D error is what makes people feel lost. User zeroes one axis at a time.

**Bolt direction disambiguation (the differentiator).** To remove the last ambiguity (knob CW
vs CCW), do a one-time **calibration nudge** at the start of the adjust phase:
1. Prompt: "turn the **altitude** knob a little (either way)."
2. Observe how the axis marker / solved position moves in image space.
3. Derive the mapping `physical alt-knob turn → on-image displacement` (sign + magnitude per
   turn-unit, optionally).
4. Repeat for azimuth (often inferable from alt + the known transform, so step 4 may be skipped).

From then on the arrows are labelled with the **actual physical direction** ("Altitude: ↑
clockwise"), self-correcting for camera rotation, hemisphere, and meridian side. This is exactly
PHD2's guiding-calibration idea applied to manual bolts. (If a user prefers, the calibration
nudge is skippable — the live loop + reticle still works by trial, just without the labelled
knob direction.)

---

## 4. Architecture / responsibilities

```
        ┌──────────────────────────── ARA ────────────────────────────┐
        │  PA state machine · plate solver (ASTAP/astrometry) · UX     │
        │  (reticle / decoupled arrows · bolt-direction calibration)   │
        └───────┬───────────────────────────────────┬─────────────────┘
                │ JSON-RPC (:4400 / /api/rpc)        │ ASCOM Alpaca
                ▼                                      ▼
        openastro-guider (PHD2)                    Mount (RA slews)
        capture · centroids · staticpa_geometry    via AlpacaBridge
                │
                ▼ (its own Alpaca connection)
        Guide camera
```

- **ARA owns the routine**: the plate solver (ASTAP/astrometry.net — the guider is *not* a
  solver), the state machine, and all UX (reticle, arrows, calibration nudge).
- **openastro-guider provides** what it's good at: the live capture loop, **sub-pixel star
  centroiding**, and the **`staticpa_geometry`** kernel (already extracted + unit-tested).
- **Mount RA slews** come from **ARA's own Alpaca mount connection** (via AlpacaBridge), *not*
  from PHD2 — PHD2 is a guider, not a GOTO controller (its GUI Static-PA tool literally says
  "manually slew"). This avoids putting slew logic in the guider.

**Camera ownership note (important):** the guide camera can have only one client. During PA the
guider holds the guide cam, so ARA must get frames/centroids **through the guider's API**
(§8) rather than grabbing the camera itself. (Alternative: ARA disconnects the guider and drives
the cam directly — rejected for v1; it loses the guider's centroiding and complicates handoff.)

---

## 5. The routine (state machine)

1. **Setup / preflight**
   - Confirm mount connected (ARA↔Alpaca) and guide cam connected (guider).
   - **Stop any active guiding/looping** (`stop_capture`) and **record the prior state** so it can
     be restored at the end — PA drives slews and owns the capture cadence, so it must not run
     concurrently with guiding.
   - Read pixel scale from the profile (focal length + pixel size) → pick the live strategy (§7).
   - Pick an exposure that yields a solvable frame (auto-stretch / star count; longer for OAG).

2. **Axis determination (2-point, ASIAIR-style)**
   - Capture frame A (guider, full frame, saved FITS) → ARA plate-solves → (RA, Dec, PA, parity)₁.
   - ARA slews the mount in RA by Δ (default ~60°; smaller near the pole, see §7).
   - Capture frame B → solve → (RA, Dec, PA, parity)₂.
   - Compute the **RA rotation axis** on the sky from the two pointings, then the **alt/az
     offset** from the true refracted pole at the observer's location/time.
   - **Two points give a unique axis but zero redundancy** — any solve error in either point
     propagates straight into the axis. The spike (§12) must measure the real single-solve error
     budget (typical ASTAP residuals ~1–2″); add a **3rd point (over-determined least-squares
     fit)** if 2-pt can't hit sub-arcmin reliably.
   - **Refraction + precession are v1 requirements, not v2.** A routine that claims "sub-arcmin"
     cannot defer the refraction term on the pole position (it is itself arcmin-scale at typical
     pole altitudes). Precess catalog/pole positions to date (`PrecessJ2000`) and apply
     atmospheric refraction for the apparent pole. **Refraction is ARA's responsibility** (it
     owns the solver/geometry orchestration); use a standard model (e.g. Bennett / Saemundsson).

3. **Live adjust loop** (the calibration nudge happens *inside* this loop, so there is no stale
   baseline — the loop re-derives the current error every cycle, so the nudge's deliberate error
   change is simply observed, not a problem):
   - Continuously refresh the error (§7: re-solve, or centroid-track between sparse solves).
   - **Calibration nudge** (§3) as the first live action: prompt a small alt-knob turn, observe
     how the marker moves in the just-refreshed frames → derive the bolt→image mapping. Because
     the loop is already measuring live, the nudge needs no separate re-measurement of the axis.
   - Render reticle (pole framed) or decoupled alt/az arrows + arcmin (pole not framed).
   - Update until both components are under the target threshold (configurable; e.g. < 0.5–1′).

4. **Verify & hand back** — one more solve to confirm residual error; report final alt/az; then
   **restore the pre-PA state** recorded in step 1 (resume looping/guiding if it was running).

Events stream throughout so ARA's UI updates in real time (§9).

---

## 6. Reusing `staticpa_geometry`

The extracted, unit-tested kernel (`src/staticpa_geometry.{h,cpp}`) already implements the
geometry; the plate-solve path and the near-pole path both use it:

| Function | Use |
|---|---|
| `CircleFrom3Points` / `CircleFrom2PointsAndAngle` | center-of-rotation from star arcs (near-pole mode) |
| `DecomposeCoR` | CoR → Dec / cone error |
| `Radec2Px` | project the true pole (known RA/Dec) into image pixels — drives the **reticle** |
| `DecomposeAltAz` | error vector → **Altitude / Azimuth** components (the decoupling) |
| `PrecessJ2000` | precess catalog/pole positions to date (arcmin-level correctness) |

For the **plate-solve** path, the axis comes from two solved pointings rather than a pixel
circle-fit, but `DecomposeAltAz` / `Radec2Px` / `PrecessJ2000` are equally used to turn the
axis-offset into alt/az components and to place the reticle. Anything missing (e.g. an
atmospheric-refraction term for the pole position) is small and can live in ARA or be added to
the kernel.

---

## 7. FOV / setup adaptivity (the guide-scope vs OAG question)

Plate-solving **normalizes hardware differences** — the math is identical regardless of scope;
FOV only changes *how the live loop runs*, chosen automatically from the known pixel scale:

| Regime | Typical setup | Pole usually framed? | Axis determ. | Live feedback |
|---|---|---|---|---|
| Wide (≳ ~2–3°) | short guide scope | often yes | small RA Δ or near-pole CoR | **reticle** (dot-to-bullseye) on live frame |
| Medium | typical guide scope | sometimes | 2-pt plate-solve | decoupled arrows; reticle if framed |
| Narrow (≲ ~0.5°) | OAG / long FL | rarely | 2-pt plate-solve | **centroid-track between sparse solves** + arrows |

Key adaptations:
- **Exposure / star threshold** auto-scale (longer, more sensitive for small FOV).
- For tiny FOV, re-solving every frame is too slow → after the initial solve, **track stars'
  centroids** (guider's strength) for a high-refresh live readout, re-solving only periodically
  to re-anchor. **Cap the time between re-anchors (e.g. 30–60 s) even when stars are held** —
  centroid tracking accumulates drift (refraction, flexure, differential refraction across the
  FOV) that a star-lost fallback alone won't catch.
- **RA slew Δ rule:** the two measurement points must be far enough apart *on the projected sky
  plane* for a stable axis fit. Far from the pole a fixed ~60° RA Δ is fine; near the pole the
  same angular Δ collapses to a tiny projected separation. Define Δ by a **target projected
  separation** (e.g. keep the field displacement above a set arcmin threshold given the current
  Dec and pixel scale) rather than a fixed angle. Also **bound Δ against the mount's slew limits**
  — if the target Δ would exceed them, fall back to a smaller Δ or a different start position (§10).

---

## 8. openastro-guider API needs

Most pieces already exist (see `design/API_REFERENCE.md`):

- **Capture full frame for solving:** `capture_single_frame` (with `save`/`path`) → ARA solves the
  saved FITS. *Confirm it writes a solver-friendly FITS and returns the path.*
- **Centroids for live tracking:** `find_star` / `get_star_image` give star position(s); for the
  high-refresh track loop ARA needs a star (or few) centroid per frame. *Possible small addition:
  a "report centroids for the current frame / a list of stars" call if `find_star` (single best
  star) isn't enough for multi-star tracking.*
- **Exposure / subframe / gain:** `set_exposure`, `set_use_subframes`, `set_camera_gain` (Batch B)
  — already present.
- **Events:** the existing event stream; PA progress can ride a new `PolarAlignment*` event set
  (§9) or be ARA-side only.

What's **not** in the guider and stays in ARA: the plate solver and the mount RA slews.

**Net:** v1 likely needs **zero or one** new guider RPC (a multi-star centroid report), plus a
check that `capture_single_frame` produces a solvable saved FITS. The heavy lifting is ARA-side.

---

## 9. Live-feedback events (optional, if PA runs through the guider)

If any PA orchestration lands in the guider later, emit on the shared dispatch (both `:4400` and
`/api/rpc`), mirroring existing events:

- `PolarAlignmentState` — phase (`Setup`/`Measuring`/`Calibrating`/`Adjusting`/`Done`/`Failed`).
- `PolarAlignmentUpdate` — `{ AltError, AzError, TotalError (arcmin), AltDir, AzDir, ReticlePx? }`.

For v1 (ARA-orchestrated) these may be unnecessary — ARA already has the numbers.

---

## 10. Failure modes / edge cases
- **Solve fails** (clouds, too few stars, bad focus): retry with longer exposure / more stretch;
  surface a clear "couldn't solve — check focus/clouds" rather than a silent stall.
- **Meridian / hemisphere / parity:** all handled by deriving directions from the solve transform
  (no hard-coded N/S/E/W), plus the calibration nudge for knob sense.
- **Star lost during the live track loop:** fall back to a fresh solve to re-anchor.
- **Mount won't slew the requested Δ** (limits): use a smaller Δ or a different start position.
- **Refraction near the horizon / low pole altitude:** include a refraction term for the target
  pole position.

## 11. Open questions / decisions
- **Solver:** ASTAP (fast, local, good for small FOV) vs astrometry.net index files — likely ASTAP.
- Do we add the **multi-star centroid** RPC, or is `find_star` enough for the track loop?
- Is the **near-pole CoR / reticle** mode worth shipping in v1, or start decoupled-arrows-only?
- Default **target threshold** and RA slew Δ; make both configurable.
- Does `capture_single_frame` already emit a solver-ready FITS? (verify)

## 12. Phasing
1. **Spike:** *first* verify `capture_single_frame` emits a solver-ready FITS at a known path
   (the highest-risk unknown — gate everything else on it). Then ARA drives `capture_single_frame`
   → ASTAP solve → 2-pt axis → static alt/az error printout (no live loop), and **measures the
   solve error budget** to decide 2-pt vs 3-pt (§5). Proves the pipeline end-to-end.
2. **Live adjust:** continuous re-solve + decoupled arrows; target threshold; verify step.
3. **Bolt calibration nudge** → labelled physical directions.
4. **FOV adaptivity:** centroid-track-between-solves for small FOV; reticle mode for framed pole.
5. **Polish / optional:** PoleMaster/iPolar passthrough; in-guider `PolarAlignment*` events if
   any orchestration moves server-side.

---

See also: `design/API_REFERENCE.md` (guider methods), `src/staticpa_geometry.{h,cpp}` (kernel),
`design/PHD2_HEADLESS_PLAYBOOK.md` (overall architecture).
