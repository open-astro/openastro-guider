# Polar Alignment — design

A design for a fast, accurate, hardware-agnostic polar-alignment (PA) routine for **ARA**,
taking the best of ASIAIR / iPolar / PoleMaster / PHD2 and — crucially — **solving the
"which bolt, which way, how much" problem** for users with any optical train (guide scope,
OAG, short or long focal length).

Status: **the guider owns the routine** (corrected 2026-11-05, Ara #1611). The in-guider Static PA
tool (`staticpa_start` / `staticpa_measure` / `staticpa_get_status` / `staticpa_stop` /
`staticpa_close`) and Polar Drift (`polardrift_*`) are the polar-alignment routines; Ara drives them
over RPC and draws their status. An earlier revision of this document gave Ara its own plate-solve
routine (two-point axis fit over `capture_single_frame`); that was never the intent — PHD2 was kept
for its guiding AND its polar alignment — and Ara's own fit reported 8″ at 3.5° off the pole
(Ara #1591). It has been removed from Ara. Owner: openastro-guider (routine) + ARA (driver/UI).

What the guider provides (see §4/§8 and `API_REFERENCE.md`):
- **Static PA over RPC:** the GUI tool's state machine + the `staticpa_geometry` kernel, headless.
  `auto` mode rotates the mount itself (the guider's own pointing source); manual mode takes three
  star positions the caller records with `staticpa_measure` after rotating RA. The status object
  carries `ref_star_target` + `current_star` (the dot-to-bullseye reticle) and `live_adjustment`
  (alt/az/total error in px and arcmin, with the on-sensor push vectors).
- **Polar Drift over RPC:** `polardrift_start/get_status/stop/close` — the drift-based check for
  the pole-less sky; the same alt/az readout from a star near the meridian/equator.
- **`capture_single_frame`, `get_star_centroids`, `set_pa_session`/`get_pa_session`** remain for
  Ara's other guide-camera uses (Smart Focus, the live view). The PA-session lease is NOT used by
  polar alignment: it blocks `loop`, which the Static PA tool needs, and the tool already refuses
  `guide` / `dither` / the Guiding Assistant while it runs.

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

The nudge must be **large enough to be detectable above solve/centroid noise** — set a minimum
expected displacement (in pixels, from the pixel scale) and, if the observed move is below it,
prompt a larger turn rather than derive a mapping from noise.

From then on the arrows are labelled with the **actual physical direction** ("Altitude: ↑
clockwise"), self-correcting for camera rotation, hemisphere, and meridian side. This is exactly
PHD2's guiding-calibration idea applied to manual bolts. (If a user prefers, the calibration
nudge is skippable — the live loop + reticle still works by trial, just without the labelled
knob direction.)

---

## 4. Architecture / responsibilities

```
        ┌──────────────────────────── ARA ────────────────────────────┐
        │  driver + UX: preflight · loop/find_star · staticpa_* RPCs    │
        │  manual-mode RA turns · reticle / arrows · session log        │
        └───────┬───────────────────────────────────┬─────────────────┘
                │ JSON-RPC (:4400 / /api/rpc)        │ ASCOM Alpaca (manual mode only)
                ▼                                      ▼
        openastro-guider (PHD2)                    Mount (RA turns when the
        capture · centroids · Static PA state      guider's own mount cannot slew)
        machine · staticpa_geometry · live
        alt/az decomposition
                │
                ▼ (its own Alpaca connection)
        Guide camera + mount (auto mode rotates RA itself)
```

- **openastro-guider owns the routine**: the capture loop, sub-pixel centroiding, the Static PA
  state machine (point 1 → rotation → point 2/3 → centre of rotation → alt/az error), the
  reticle target and the live decomposition as the user turns the bolts. Polar Drift likewise.
- **ARA drives and displays**: it checks the mount is connected and the site is set, gets the
  guider looping with a star (`set_exposure`, `loop`, `find_star`), starts the tool
  (`staticpa_start {auto: true, hemisphere}`), polls `staticpa_get_status` and publishes the
  `live_adjustment` on its own wire; Stop/Done send `staticpa_stop` + `staticpa_close` +
  `stop_capture` and log the session row.
- **Mount rotation**: auto mode uses the guider's own pointing source (`can_slew`). When the
  guider answers "mount cannot slew", Ara restarts the tool in manual mode and does the RA
  turns through its own Alpaca mount connection, re-picking the star and sending
  `staticpa_measure 2` and `3`.
- **No plate solver in the loop.** The guider is not a solver and the routine does not need one:
  the centre of rotation comes from the star track. Ara's solver stays for its own uses.

**Camera ownership:** during polar alignment the guider holds the guide camera and its tool
refuses `guide`, `dither`, `guiding_assistant_start`, `polardrift_start` and `driftalign_*`
("static polar alignment is running"). That is the single-client enforcement for PA; the
`set_pa_session` lease is for the other guide-camera borrowers (it must not be held here — it
blocks `loop`).

---

## 5. The routine (state machine)

The guider's Static PA tool, as Ara drives it (Ara states in `code`):

1. **Preflight (Ara)** — mount connected, site latitude set; a running Smart Focus loop is
   stopped; a parked mount is unparked.
2. **`seeding`** — `set_exposure` → `loop` → `find_star` (up to five frames) →
   `staticpa_start {auto: true, hemisphere}`. In auto mode the guider records point 1, rotates
   the mount west in steps (`rotation {required_deg, rotated_deg, step, required_steps,
   slewing}` in the status while it does), records point 2 and computes the centre of rotation.
   If the guider has no slewable mount ("mount cannot slew") Ara restarts with `auto: false`,
   turns RA itself (`SeedRotationDeg`, away from the meridian), re-picks the star and sends
   `staticpa_measure 2`, then again for point 3; the guider computes once it has three points.
3. **`adjusting`** — `calced=true`: the status carries `centre`, `adjustment` (at the measured
   point), `ref_star_target` and, whenever the live star is valid, `current_star` +
   `live_adjustment` — the alt/az/total error against the star's current position, which goes to
   zero as the bolts are turned. Ara relays `live_adjustment` on every poll (`polar_align.progress`).
4. **`paused`** — no `current_star` for five polls (cloud, a bumped mount); the next sighting
   resumes. Nothing is remeasured: the centre of rotation stands until the user restarts.
5. **Done / Stop (Ara)** — `staticpa_stop`, `staticpa_close`, `stop_capture`; Ara logs the last
   readout with outcome `complete` or `aborted`. `active=false` seen while polling is `failed`.

Tracking is the guider's business during the routine (the tool keeps the mount tracking so the
star stays put while the bolts move); Ara does not touch it.

---

## 6. Reusing `staticpa_geometry`

> Historical note (2026-11-05): the "plate-solve path" below was Ara's own routine, since removed; only the near-pole path is in use. Kept for the kernel's documentation.

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

> Historical note (2026-11-05): written for the removed plate-solve path. With Static PA the only FOV requirement is a usable star near the pole in the guide camera's frame; a wide-field reticle mode and an OAG's narrow field are both served by the same star track.

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
- **Differential refraction across the frame** is negligible for the narrow-FOV/OAG regime
  (≲ 0.5°) and is intentionally ignored in the centroid-track loop; only the single refraction
  term for the apparent pole position (§5) matters at v1 accuracy.

---

## 8. openastro-guider API (what Ara calls)

All in `design/API_REFERENCE.md`:

- `set_exposure`, `loop`, `find_star` — get the camera looping with a selected star (the tool
  refuses to start without one: "no star selected - start looping and select a star first").
- `staticpa_start {auto?, hemisphere?, ref_star?, hour_angle?, flip_camera?}` — Ara sends
  `auto: true` + the hemisphere from the site latitude; on "mount cannot slew" it retries with
  `auto: false`.
- `staticpa_measure {position}` — manual mode only, positions 2 and 3, after Ara's RA turns.
- `staticpa_get_status` — polled every `LoopCadenceMs` (profile, default 1 s): `calced=false`
  is Ara's `seeding`; `live_adjustment` is `adjusting`; no `current_star` for five polls is
  `paused`; `active=false` is `failed` (the tool went away underneath Ara).
- `staticpa_stop`, `staticpa_close`, `stop_capture` — the hand-back on Stop / Done / failure.
- `polardrift_*` — the drift check; Ara's DTOs exist, wiring is the follow-up (Ara #1611).

Nothing new is needed in the guider for v1. Possible later additions: a `PolarAlignment*` event
set (§9) so Ara can stop polling, and running Polar Drift alongside guiding so the guiding-time
residual (Ara #1311) comes from the same kernel.

---

## 9. Live-feedback events (optional)

The routine runs in the guider, so these would let Ara stop polling `staticpa_get_status`. Emit on the shared dispatch (both `:4400` and
`/api/rpc`), mirroring existing events:

- `PolarAlignmentState` — phase (`Setup`/`Measuring`/`Calibrating`/`Adjusting`/`Done`/`Failed`).
- `PolarAlignmentUpdate` — `{ AltError, AzError, TotalError (arcmin), AltDir, AzDir, ReticlePx? }`.

For v1 Ara polls; the status object already has the numbers.

---

## 10. Failure modes / edge cases
- **Solve fails** (clouds, too few stars, bad focus): retry with longer exposure / more stretch;
  surface a clear "couldn't solve — check focus/clouds" rather than a silent stall.
- **Meridian / hemisphere / parity:** all handled by deriving directions from the solve transform
  (no hard-coded N/S/E/W), plus the calibration nudge for knob sense.
- **Star lost during the live track loop:** fall back to a fresh solve to re-anchor.
- **Mount won't slew the requested Δ** (limits): use a smaller Δ or a different start position.
- **Meridian flip mid-routine:** a Δ that crosses the meridian flips the OTA and invalidates the
  axis fit — pick the slew direction / start position to avoid it (§5 step 2).
- **Mount not settled:** never capture the second/each measurement frame until the mount has
  settled (§5 step 2), or the solve reflects a moving target.
- **Refraction near the horizon / low pole altitude:** include a refraction term for the target
  pole position.
- **User abort / non-convergence:** the user can cancel at **any** phase. Abort (and an overall
  timeout if the error never converges) must run the **same hand-back** as normal completion —
  restore the pre-PA guiding/looping state recorded in step 1 — so a cancelled PA never leaves the
  rig in a half-configured state.

## 11. Open questions / decisions

- **Decided (2026-11-05):** the guider owns the routine; Ara has no polar-alignment math of its
  own. Any accuracy work (fit quality, rejection of a bad geometry, a verification pass before
  "Excellent") happens in `staticpa_toolwin.cpp` / `staticpa_geometry`, where the GUI tool and
  Ara both benefit.
- Should `staticpa_get_status` grow a fit-quality figure (residual of the circle fit) so Ara can
  refuse to say "Excellent" on a weak fit?
- Push events (§9) vs polling — polling at 1 Hz is fine for now.
- Polar Drift during guiding for the guiding-time residual (Ara #1311): needs the drift fit to
  run on the uncorrected star motion while guiding, or a guider-side replay of the guide log.

## 12. Phasing

1. **Done (guider):** Static PA + Polar Drift over RPC, reticle + live decomposition in the status object.
2. **Done (Ara #1611):** Ara drives Static PA (auto, with manual fallback) and shows the guider's readout; Ara's own fit removed.
3. **Next:** fit-quality figure in the status object; `PolarAlignment*` events; Polar Drift wiring in Ara.
4. **Later:** the guiding-time residual from the same kernel (Ara #1311).

---

See also: `design/API_REFERENCE.md` (guider methods), `src/staticpa_geometry.{h,cpp}` (kernel),
`design/PHD2_HEADLESS_PLAYBOOK.md` (overall architecture).
