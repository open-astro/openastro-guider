# openastro-guider — API contract

Append-only design log for the control API that OpenAstro ARA uses to drive the headless
guider. The transport is PHD2's **event server (JSON-RPC over TCP :4400)** plus its event
stream, and the embedded **HTTP `/api/rpc`** bridge on `:8080` (same method table). One entry
per method ARA depends on or that we add during the port. Equipment (guide camera + mount) is
reached separately via ASCOM Alpaca through AlpacaBridge.

**Full per-method reference: `design/API_REFERENCE.md`** (every method + the event stream).
Gap analysis driving the work: `design/API_GAP_AUDIT.md`. See also `PHD2_HEADLESS_PLAYBOOK.md` §4.

---

## Inherited (already in the PHD2 event server)
Covered by upstream; catalogued here as ARA wiring lands. Examples: `guide`, `dither`,
`set_exposure` / `get_exposure`, `get_algo_param_names` / `get_algo_param` / `set_algo_param`,
`set_lock_position`, `flip_calibration`, `get_connected` / `set_connected`, and the event
stream (`GuideStep`, `SettleDone`, `StarLost`, `CalibrationComplete`, …).

## Added for headless (Phase 5 gap-fill)

### Batch A — guiding control (2026-06-07)
- `get_algos` { `axis`?: "ra"|"dec" } → result: array of guide-algorithm names (untranslated).
  No `axis` → all algorithms; with `axis` → only those valid for that axis/mount (requires a
  connected mount).
- `get_algo` { `axis`: "ra"|"dec" } → result: the axis's selected algorithm name.
- `set_algo` { `axis`, `name` } → result: 0. Switches the axis's guide algorithm (`name` from
  `get_algos(axis)`). Names not valid for the axis/mount — including "None"/identity, which
  isn't per-axis selectable — are rejected with an error.
- `get_guide_limits` → result: { `MaxRaDuration`, `MaxDecDuration` } (ms).
- `set_guide_limits` { `MaxRaDuration`?, `MaxDecDuration`? } → result: 0. Either field optional.
- `get_dec_comp` → result: bool (declination RA-rate compensation enabled).
- `set_dec_comp` { `enabled`: bool } → result: 0.
- `get_dither_settings` → result: { `ScaleFactor`, `RaOnly` }.
- `set_dither_settings` { `ScaleFactor`?, `RaOnly`? } → result: 0. Either field optional.

All on the shared dispatch (served over both `:4400` and `/api/rpc`). ARA call sites: the
guiding-settings panel (algorithm pickers, RA/Dec aggression limits, dec-comp toggle, dither
defaults).

### Batch B — star detection + camera (2026-06-07)
- `get_star_detection` → result: { `MinStarSNR`, `MinStarHFD`, `MaxStarHFD`, `SearchRegion` }, plus
  { `MassChangeThreshold`, `MassChangeThresholdEnabled` } when the multi-star guider is active.
  Requires a guider.
- `set_star_detection` { `MinStarSNR`?, `MinStarHFD`?, `MaxStarHFD`?, `SearchRegion`? (7..50),
  `MassChangeThreshold`?, `MassChangeThresholdEnabled`? } → result: 0. All optional, validated
  before applying; `SearchRegion` and `MassChange*` require the multi-star guider.
- `get_camera_gain` → result: { `Gain` (0..100), `HasGainControl` }. Requires a connected camera.
- `set_camera_gain` { `gain`: 0..100 } → result: 0. Errors if the camera has no gain control.
- `get_camera_timeout` → result: download/read timeout ms.
- `set_camera_timeout` { `timeout_ms`: 5000..600000 } → result: 0. Values below the 5000 ms
  floor are rejected (not silently raised).

ARA call sites: the star-detection / camera panels.

### Batch C1 — mount options, backlash, exposure (2026-06-07)
- `get_mount_options` → result: { `AssumeOrthogonal`, `CalFlipRequiresDecFlip`,
  `StopGuidingWhenSlewing` } (bool). Requires a scope.
- `set_mount_options` { any of the above (bool) } → result: 0. All optional.
- `get_backlash_comp` → result: { `Enabled`, `PulseWidth`, `Floor`, `Ceiling` } (ms).
- `set_backlash_comp` { `Enabled`?, `PulseWidth`?, `Floor`?, `Ceiling`? } → result: 0. All
  optional; unspecified pulse/floor/ceiling keep current. Constraints (rejected, not clamped):
  `PulseWidth` in [min, max], `Floor` in [min, PulseWidth], `Ceiling` in [PulseWidth, max]
  (min/max = backlash pulse min/max = 20/8000 ms).
- `get_auto_exposure` → result: { `Enabled`, `MinExposure`, `MaxExposure` (ms), `TargetSNR` }.
- `set_auto_exposure` { `MinExposure`?, `MaxExposure`?, `TargetSNR`? } → result: 0. All optional;
  `Min` <= `Max`. (Auto-exposure is *enabled* by selecting "Auto" via `set_exposure`.)
- `get_noise_reduction` → result: method name ("None" / "2x2 Mean" / "3x3 Median").
- `set_noise_reduction` { `method` } → result: 0.

### Batch C2 — camera extras, rotator, guider options (2026-06-07)
- `set_use_subframes` { `enabled`: bool } → result: 0 (get_use_subframes already existed;
  enabling requires camera subframe support).
- `set_cooler_setpoint` { `setpoint`: °C, -100..50 } → result: 0. Requires a cooler.
- `get_camera_saturation` → result: { `ByADU`, `SaturationADU` }.
- `set_camera_saturation` { `ByADU`?, `SaturationADU`? (0..65535) } → result: 0 (applied together).
- `get_rotator_reversed` / `set_rotator_reversed` { `reversed`: bool } → result: 0 / bool.
- `get_guider_options` → result: { `FastRecenter`, `AutoSelDownsample` }, plus
  { `TolerateJumps`, `TolerateJumpsThreshold` } with the multi-star guider.
- `set_guider_options` { `FastRecenter`?, `AutoSelDownsample`? (0..4), `TolerateJumps`?,
  `TolerateJumpsThreshold`? } → result: 0. `TolerateJumps*` require the multi-star guider.

This completes the `API_GAP_AUDIT.md` batches (A/B/C). Software binning is already covered by
`set_profile_setup` (`software_binning`).

### Dark / bad-pixel-map tail (2026-06-10)
Closes the last known Phase-5 gap. The audit found create/select/delete were already covered
(`build_dark_library`, `build_defect_map_darks`, `set_dark_library_enabled`,
`set_defect_map_enabled`, `get_calibration_files_status`, `delete_calibration_files`); what was
missing was the **Refine Bad-pixel Map** surface (rebuild with custom aggressiveness, without
recapturing) and the auto-load flags:

- `rebuild_defect_map` { `aggressiveness_hot`? (0..100), `aggressiveness_cold`? (0..100),
  `save`? (default true), `load_after`? (default true) } → result: { `profile_id`,
  `defect_map_path`, `aggressiveness_hot/cold`, `hot_pixel_count`, `cold_pixel_count`,
  `defect_count`, `master_dark_exposure_ms`, `master_dark_frame_count`,
  `image_mean/stdev/median/mad`, `saved`, `loaded` }. Rebuilds the bad-pixel map from the
  **existing** master dark (errors if `build_defect_map_darks` hasn't produced one).
  Aggressiveness defaults to the profile's last-used values (GUI default 75). `save:false` is a
  dry run — returns the predicted counts without touching the map file (mirrors the GUI's
  slider preview). Saving persists the aggressiveness as the new profile defaults. Requires a
  connected camera (the map info embeds the camera name). Manual pixels are discarded on
  rebuild, same as the GUI.
- `add_bad_pixel` { `x`, `y` } → result: { `x`, `y`, `added` }. Adds one pixel to the
  **currently loaded** defect map (in-memory + disk, same as the GUI's "Add Bad Pixel");
  `added:false` if already present. Requires a connected camera and a loaded map. Bounds are
  checked against the frame size when known.
- `set_dark_auto_load` { `auto_load_darks`?, `auto_load_defect_map`? (bool, at least one) } →
  result: the `get_calibration_files_status` object. Controls whether the dark library /
  defect map auto-load on camera connect (the flags `get_calibration_files_status` reports).

ARA call sites: the dark-library / bad-pixel-map panel.

### Polar-alignment guider-side enablers (2026-06-10)
The PA routine is ARA-orchestrated (`POLAR_ALIGNMENT_DESIGN.md`); these are the guider's
contributions, on the shared dispatch:

- `get_star_centroids` { `roi`? ([x,y,w,h]), `max_stars`? (1..50, default 12) } → result:
  `[{ x, y, snr, mass, hfd }, …]` — sub-pixel centroids of stars detected on the **current**
  frame, with no star selection / guider-state change (unlike `find_star`). Errors with no
  current image, on subframe images, or when no stars are found. This is the §8/§11 multi-star
  centroid report for the narrow-FOV centroid-track loop.
- `set_pa_session` { `active` (bool), `timeout_s`? (10..3600, default 600) } → result:
  { `active`, `expires_in_s`? }. Starts/renews (or ends) the §4 camera-ownership lease: while
  active, `guide` / `loop` / `dither` / `guide_pulse` / `build_dark_library` /
  `build_defect_map_darks` / `set_connected` are rejected ("polar-alignment session in
  progress"); `capture_single_frame`, `find_star`, `get_star_centroids`, and `stop_capture`
  stay available. Auto-expires so a crashed orchestrator can't wedge the daemon. Starting is
  rejected while calibrating/guiding.
- `get_pa_session` → result: { `active`, `expires_in_s`? }.

Also verified (the §12.1 spike gate): `capture_single_frame` with `save` + absolute `path`
writes a solver-ready FITS (16-bit unsigned; `EXPOSURE`, `DATE-OBS`, `XBINNING`,
`XPIXSZ`/`YPIXSZ`, `SCALE`/`PIXSCALE` arcsec/px, `INSTRUME`, `SATURATE`) and the
`SingleFrameComplete` event carries `Success` + `Path`. No RA/Dec in the header — ARA seeds
the solver from the mount pointing.

ARA call sites: the polar-alignment routine (all phases).

### Static Polar Alignment over RPC (2026-06-10)
The in-guider **Static PA** (center-of-rotation) tool is now drivable headlessly. The RPC layer
constructs the existing `StaticPaToolWin` without showing it and the alignment state machine
runs from the guider's per-frame hook exactly as for the desktop GUI (no logic duplicated; the
adjustment math was factored into `StaticPaToolWin::CalcAdjustmentsFor` so the GUI chart and
the API status share one code path).

- `staticpa_start` { `auto`?, `hemisphere`? ("north"/"south"), `ref_star`? (index),
  `hour_angle`? (0..24 h), `flip_camera`? } → result: the status object. Requires camera,
  known pixel scale, looping + selected star; rejects auto without an async-slew mount.
- `staticpa_measure` { `position`: 2|3 } → status. Manual mode: record point 2/3 on the next
  frame (client rotates RA between points).
- `staticpa_get_status` → { `active`, `aligning`, `auto`, `can_slew`, `hemisphere`,
  `hour_angle`, `flip_camera`, `pixel_scale`, `camera_angle`, `ref_star`, `ref_stars` (8
  near-pole catalog stars, RA/Dec of date), `measured_points`, `rotation`? (auto progress),
  `calced`, and when calced: `centre {x,y,radius_px}`, `adjustment` (alt/az/total error px +
  arcmin, correction vectors), `ref_star_target`, `current_star`, `live_adjustment` (same
  decomposition vs the live star — poll this during bolt adjustment; it converges to zero) }.
- `staticpa_stop` → status. Auto: aborts slew + clears points; manual: keeps points.
- `staticpa_close` → 0. Tears the tool down.

Verified live against `scripts/fake_alpaca_camera.py` with its new `--rotate-deg-per-sec` /
`--pole` star-field rotation: the manual 3-point flow recovered the simulated pole to ~0.2 px
(centre (149.84, 200.02) vs true (150, 200), radius 150.15 px). ARA call sites: the
polar-alignment routine's near-pole mode.

### Polar Drift Alignment over RPC (2026-06-10)
Same hidden-window pattern as the Static PA methods: the existing `PolarDriftToolWin` is
constructed unshown and its per-frame drift accumulation runs from the guider hook.

- `polardrift_start` { `hemisphere`? ("north"/"south"), `mirrored`? } → status. Disables guide
  output (saving the prior enabled state) so the star drifts freely. Requires camera, known
  pixel scale, looping + selected star.
- `polardrift_get_status` → { `active`, `drifting`, `hemisphere`, `mirrored`, `pixel_scale`,
  `num_samples`, `elapsed_s`?, and after ≥2 samples: `offset_px`, `error_arcmin`,
  `pole_direction_deg`, `current_star`, `target` }. The fit improves with time; adjust alt/az
  to put the star on `target`, then restart to re-measure.
- `polardrift_stop` → status. Restores guide output; result stays readable.
- `polardrift_close` → 0.

Verified live against the rotating fake camera: the fitted offset matched the analytic value
for the simulated rotation rate (28.7 kpx ≈ r·ω·13751 s/rad) and with the hemisphere matching
the simulated rotation sense the `target` direction pointed at the fake pole (97.9° vs 89.9°
geometric; the 8° residual is the arc-curvature shift of the 20 s fit window). ARA call sites:
the polar-alignment routine's drift mode.

### Drift Alignment over RPC + Alpaca coordinate-units fix (2026-06-10)
The classic drift-align tool (meridian/horizon dec-drift method) completes the set: all three
in-guider polar-alignment tools are now drivable headlessly. The tool window lives entirely in
`drift_tool.cpp`, so the RPC layer goes through new `DriftTool::Api*` accessors; the
drift/adjust state machine runs from the existing `APPSTATE_NOTIFY` pump. The polar-error
number comes from a new `GraphLogWindow::GetPolarAlignmentError()` — the same Frank-Barrett
dec-trendline estimate the graph overlays, factored out of the paint code so the API can read
it without painting.

- `driftalign_start` → status. Requires camera, mount, known pixel scale.
- `driftalign_set_phase` { `phase`: "azimuth"|"altitude" } → status.
- `driftalign_drift` → status. Starts guiding if needed (mount must be calibrated), then
  disables dec output and watches the dec trendline.
- `driftalign_adjust` → status. Stops guiding (keeps looping), locks the lock position at the
  drifted star for bolt adjustment.
- `driftalign_get_status` → { `active`, `phase`, `mode`, `drifting`, `can_slew`, `slewing`,
  `calibrated`, `guiding`, `status_message`?, `polar_alignment_error`? { `error_arcmin`,
  `dec_drift_arcsec_per_min`, `samples` }, `current_star`?, `lock_position`?, `scope`?
  { `ra_hours`, `dec_degrees`, `lst_hours` } }.
- `driftalign_close` → 0. Full GUI-equivalent restore.

**Bug fix surfaced by this work:** `ScopeAlpaca::GetCoordinates` returned RA/Dec in *radians*
and `SlewToCoordinates`/`Async` expected radians, while the PHD2-wide convention (manual
pointing scope, drift tool, static PA, calibration assistant — `hourAngle = lst − ra`) is
**hours/degrees**. Since Alpaca is this fork's only mount backend, every hour-angle computation
in those tools was wrong with a connected mount. Now hours/degrees pass through unchanged
(Alpaca's native units). `GetDeclinationRadians` (explicitly radians) is unaffected.

Verified live against the fake Alpaca rig (`scripts/fake_alpaca_camera.py` now serves a
telescope at device 0 whose pulseguide requests shift the star field, plus an injected
`--dec-drift-px-per-min`): the guider calibrated and guided against the fake mount end-to-end,
and the drift fit reported −66.95′ vs the analytic 68.2′ (3.82 × 15.46″/min ÷ cos 30°) after
only 9 samples. ARA call sites: the polar-alignment routine's drift-align mode.

### Web app + guide-history/frame endpoints (2026-06-10)
The `:8080` web UI was rebuilt from the setup wizard into a full PHD2-like single-page app
(`scripts/webui/`: vanilla JS/CSS, no build step, no CDN — works offline on the Pi). Tabs:
Guide (live frame + star/lock overlays, PHD2-style guide graph, loop/select/guide/dither/pause),
Polar Align (all three tool flows), Equipment, Settings (the Batch A–C2 surface), Darks.

Server-side additions backing it:
- `get_guide_history` { `max_points`? (1..400, default 200) } → { `pixel_scale`,
  `stats` { `n`, `rms_ra`, `rms_dec`, `rms_tot`, `osc_index`, `ra_peak`, `dec_peak`,
  `star_lost`, `ra_limited`, `dec_limited` }, `points` [{ `t` (ms epoch), `dx`, `dy`, `ra`,
  `dec` (px), `ra_dur`, `dec_dur` (ms), `ra_dir`, `dec_dir` ("E"/"W"/"N"/"S" or ""), `mass`,
  `snr` }] } — the graph window's data store, for HTTP clients that have no event stream.
  (Direction chars are "" when no correction was issued; emitting the raw 0 char corrupted
  the JSON — caught by the contract test.)
- `GET /api/frame.jpg` — the current guide frame stretched exactly as the GUI displays it
  (FiltMin/FiltMax + gamma), JPEG q85; 404 when no frame. The web app polls it for the live
  view (the GUI's own displayed image only refreshes on paint, so the endpoint stretches the
  raw frame on demand).

Verified by a 49-check contract test against the live daemon + fake Alpaca rig: every RPC the
app issues (all five tabs), the static routes, frame.jpg JPEG validity, and guide history
populating during a real calibrate→guide→dither→pause session (14 pts, 0.101 px RMS).
ARA call sites: none (the web app is an alternative client; ARA supersedes it on mobile).
