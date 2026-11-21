# openastro-guider — API contract

Append-only design log for the control API that OpenAstro ARA uses to drive the headless
guider. The transport is PHD2's **event server (JSON-RPC over TCP :4400)** plus its event
stream. The HTTP port `:8080` carries image data only (`GET /api/capture/<filename>`,
`GET /api/frame.jpg`); its `/api/rpc` bridge was removed in #114. One entry
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
`SingleFrameComplete` event carries `Success` + `Path` (on a failed capture `Success:false` and
`Error:"camera error: <reason>"`, e.g. `"camera error: Camera not connected (Alpaca 1031)"`, or
`"camera error -- see debug log"` when the camera gave no reason, #97). No RA/Dec in the header — ARA seeds
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
  decomposition vs the live star — poll this during bolt adjustment; it converges to zero),
  and while adjusting (calced from this run's points, not aligning) `live_tracking` { `state` tracking|searching|lost, `frames_since_fix`,
  `seconds_since_fix`, `confirmed_by`, `ambiguous` }. `current_star` / `live_adjustment` come
  from the tool's own field tracker (#99), only while `state` is `tracking` }.
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

## 2026-06-10 — dark-build progress (web app progress bar)

- `get_dark_build_progress` {} → { `active` (bool), `exposure_index`, `exposure_count`,
  `exposure_ms`, `frame`, `frame_count` (all zero/false when idle; indices 1-based) } —
  progress of an in-flight `build_dark_library` / `build_defect_map_darks`. The build RPCs
  run on the main thread, but each dark frame is captured on a thread of its own while the
  main thread pumps the event loop (#100; until 2.2.0 the loop ran only between frames, so an
  RPC sent during a frame waited for the whole exposure plus the camera's retries, and a
  client with a 5 s per-call timeout declared the guider down), so both servers answer within
  milliseconds while a build runs and clients can poll this method to drive a progress bar.
  To keep that pump safe, while a build is active the capture-starting RPCs (`guide`, `loop`,
  `dither`, `guide_pulse`, `capture_single_frame`, `set_connected`, both build methods,
  `rebuild_defect_map`), the profile switches (`set_profile`, `set_profile_by_name`,
  `delete_profile`), the camera setters the capture reads (`set_exposure`, `set_camera_gain`,
  `set_camera_timeout`, `set_use_subframes`, `set_camera_saturation`, `set_cooler_state`,
  `set_cooler_setpoint`) and `shutdown` are rejected with "dark-frame capture in progress";
  the Gear dialog's camera connect, disconnect, setup and selection refuse the same way, and a camera
  auto-reconnect that a capture error raises mid-frame (the capture timeout, or an Alpaca camera reporting
  not connected or failing acquisitions, #103) runs once the build has ended and starts no exposure.
  ARA call sites: none yet (added for the web app's Darks tab; available to ARA's dark
  flows if useful).

## 2026-06-10 — get_version (web app header pill)

- `get_version` {} → { `version` (user-facing FULLVER from version.md), `phd_version`,
  `phd_subver`, `msg_version` } — the `Version` event's fields as a pollable RPC for HTTP
  clients that never see the :4400 event stream. Added so the web app can show the daemon
  version in its header (matching AlpacaBridge's web face).
  ARA call sites: none yet (useful for ARA's diagnostics/about surface).

## 2026-10-16 — headless Guiding Assistant (#83)

PHD2's Guiding Assistant was GUI-only (`GuidingAsstWin`), so a headless install couldn't measure
unguided drift, seeing and Dec backlash or get the min-move / exposure / backlash suggestions. Its
statistics and recommendation logic were extracted into the wx-free
`src/guiding_assistant_math.{h,cpp}` (unit-tested by `test_guiding_assistant_math`); the dialog now
delegates to it, and a headless controller in `guiding_assistant.cpp` drives the same measurement
from the same guider hooks without the dialog.

- `guiding_assistant_start` { `measure_backlash`: bool, `duration_s`? (10–3600, default 120) } →
  status. Requires guiding, or looping + selected star + calibrated mount (the run then starts
  guiding itself and returns to looping at the end). Disables guide output, multi-star and variable
  delay for the run and restores them afterwards (and subframes after the backlash test).
- `guiding_assistant_stop` → status. Finishes early with the samples so far (backlash test skipped),
  or aborts the backlash test.
- `guiding_assistant_get_status` → { `state`: idle|measuring|measuring_backlash|done|failed,
  `duration_s`, `measure_backlash`, `elapsed_s`, `samples`, `pixel_scale`, after ≥2 samples
  `ra_drift_px_min`, `dec_drift_px_min`, `ra_rms_px`, `dec_rms_px`, `high_freq_rms_px`,
  `ra_peak_px`, `dec_peak_px`, `ra_peak_peak_px`, `ra_max_drift_px_s`, `snr`, `star_mass`,
  `polar_align_error_arcmin`, `polar_align_error_is_lower_bound`; `backlash_status`?,
  `backlash_result`? (`valid`/`impaired`/`aborted`), `backlash_ms`?, `backlash_sigma_ms`?,
  `backlash_px`?; when done `recommendations` {
  `min_move_ra_px`, `min_move_dec_px`, `min_move_seeing_based`, `exposure_s`, `exposure_max_s`,
  `drift_limiting_exposure_s`?, `dec_backlash_ms`?, `dec_guide_mode`?, `dec_algorithm`?,
  `messages` [{`id`, `text`}] }; `error`? }.
- `guiding_assistant_apply` { `which`: ["exposure", "min_move_ra", "min_move_dec", "dec_backlash",
  "dec_guide_mode", "dec_algorithm"] } → { `applied`: [...] }. All-or-nothing: every item is
  checked against the live mount/camera before anything changes ("<item>: <reason>; nothing was
  applied"); applied in dependency order and saved to the current profile. If a setter still
  refuses, the error names it and what was already applied and saved. Refused if the profile
  changed or the camera/mount was disconnected or reconnected since the run.
- Status numbers are always finite (undefined → 0); after the run ends `pixel_scale` and the
  polar-alignment fields are frozen at the values the results used. Closing the guider mid-run
  restores the settings the run changed.
- Events: `GuidingAssistantProgress` (per sample / backlash step) and `GuidingAssistantDone`
  (same shape as get_status, recommendations included).
- `guide`, `dither`, `staticpa_start`, `polardrift_start`, `driftalign_start` and
  `driftalign_drift` are rejected while a run is active.

Deviations from the issue: `exposure_s` is the start of the recommended range (`exposure_max_s`
is its end) because the GA recommends a range, not a single value; `dec_algorithm` was added
because the GA also recommends Lowpass2 for Dec. Shapes pinned by fixtures in
`test_jsonrpc_schema`. ARA call sites: the Setup flow's guiding-assistant step.

## 2026-10-16 — imaging-gear devices and snapshot (#89)

The headless guider reads the imaging train from AlpacaBridge for the guide log (#82,
`design/IMAGING_GEAR_LOG.md`). These two methods let ARA name the devices instead of the guider
guessing, and read what the guider sees. The override-aware device selection, the overrides'
validation and the JSON are in the wx-free `src/imaging_gear.{h,cpp}` shared with openastro-phd2
(unit-tested by `test_imaging_gear`); overrides reach the poll thread through
`ImagingGear::Poller::SetContext` (under the poller's mutex) and a change triggers a rediscovery
on the next poll.

- `set_imaging_gear_devices` { `camera`?, `filterwheel`?, `focuser`?, `rotator`?, `telescope`?,
  `observingconditions`? } (Alpaca device numbers) → { the same six keys: number or null } — the
  overrides now in effect. Omitted keys keep their override, `-1`/`null` clears one, no params
  clear all; unknown keys, non-integers and `camera` equal to the guide camera's device number are
  refused (`-32602`) and nothing changes. An override wins over the selection rules (also over the
  telescope/rotator the guider is connected to); a camera override is never ambiguous; an
  override naming an unconfigured device reads nothing for that type. Not persisted to the
  profile: ARA sets them each session (a saved device number would silently pick the wrong device
  on another rig or server). Allowed over `/api/rpc` while guiding.
- `get_imaging_gear` {} → { `source`: alpaca|unavailable|not_alpaca, `server`, `telescope` {
  `device`, `name`, `focal_length_mm`, `aperture_mm`, `focal_ratio` }, `camera` { `device`, `name`,
  `sensor`, `sensor_type`, `pixel_size_x_um`, `pixel_size_y_um`, `width_px`, `height_px`, `bin_x`,
  `bin_y`, `pixel_scale_arcsec`, `gain`, `offset`, `readout_mode`, `cooler_on`, `sensor_temp_c` },
  `filter_wheel` { `device`, `name`, `filter`, `position` (1-based), `count`, `focus_offset` },
  `rotator` { `device`, `name`, `position_deg`, `mechanical_position_deg`, `reverse` }, `focuser` {
  `device`, `name`, `position`, `temperature_c` }, `conditions` { `device`, `name`, `seeing_arcsec`,
  `wind_speed_m_s`, `wind_gust_m_s`, `temperature_c`, `humidity_pct`, `dew_point_c`,
  `pressure_hpa`, `cloud_cover_pct`, `sky_quality_mag_arcsec2` }, `guide_train` { `type`:
  off_axis_guider|separate_guide_scope|null, `ratio` }, `ambiguous_cameras`, `overrides` {...} }.
  Every key always present; `null` for the log's `unknown` and for absent devices. Reads only the
  poller's cache (`Poller::Latest()`), never the network.

Additions beyond the issue: each device carries its Alpaca `device` number (so ARA can see that an
override took effect); switching wheel/rotator/focuser by override writes no `INFO: EQUIPMENT`
line, and switching cameras doesn't end an exposure in the log; an unreported device name is now
empty in the snapshot (`unknown` in the log as before, `null` in JSON). Shapes pinned by fixtures
in `test_jsonrpc_schema`. ARA call sites: none yet.

## 2026-11-17 — restore_calibration (#102)

`guide` with `recalibrate: true` erases the calibration before it calibrates; when that calibration
fails the mount is left uncalibrated, and the `guide` request stays waiting for the calibration
(PhdController's calibration wait has no failure check) while the guider keeps looping, so the
next `guide` is refused as in progress. The rules are in the wx-free `src/restore_calibration.cpp`
(unit-tested by `test_restore_calibration`).

- `restore_calibration` {} → bool. Puts back the calibration saved in the profile for every mount
  (the mount, and the AO when one is used). A `guide` request still waiting on the failed
  calibration is ended first (`PhdController::AbortController`): its `SettleDone` has `Status` 1
  and `Error` "calibration failed; saved calibration restored" (`TotalFrames`/`DroppedFrames` 0),
  the sticky-lock setting the calibration forced on is put back, and the next `guide` is accepted,
  so ARA can go `CalibrationFailed` → `restore_calibration` → `guide` without `stop_capture`.
  `true`: restored and every mount calibrated. `false`: some mount has no saved calibration and
  nothing changed (no partial AO restore, a waiting request is not ended), or the restored
  calibration is not usable with the current Dec guide mode (RA-only calibration with Dec
  Auto/North/South; `get_calibrated` agrees). Error 1, nothing changed: calibrating or guiding;
  the mount or AO not connected; a guide or dither request in progress that is not waiting on a
  calibration. Refused over `/api/rpc` while calibrating or guiding, like every other setter.

ARA call sites: none yet (meant for the recovery after `CalibrationFailed` on a forced recalibration).

## 2026-11-18 — connect recovery (#104)

A mount that did not answer when the guider started (the Pi up before the rig's 12 V) was never
tried again, so every `guide` was refused with "all equipment must be connected first" until
someone sent `set_connected` by hand. Headless daemon only; the desktop GUI is unchanged. The
schedule, per-device state, gates, probe sequence and texts are in the wx-free
`src/connect_retry.cpp` (unit-tested by `test_connect_retry`); GearDialog drives them.

- Background retry: a selected camera, mount or rotator the guider was asked to connect
  (`--auto-connect`, `set_connected` true, `guide`, or the camera auto-reconnect ending in
  `EquipmentReconnectFailed`) that is still not connected is retried 15 s, 30 s and 60 s after
  each failure, then every 60 s, with no limit. An attempt probes the Alpaca server on its own
  thread with short timeouts (GET connected, PUT Connected=true with 10 s, wait for connected);
  only a device the server reports connected is connected on the main thread, through the
  `set_connected` path. It waits during a dark build, a PA lease, an open static PA tool (any
  phase, until `staticpa_close`), polar drift / drift align, the Guiding Assistant, and the `set_connected` refusal rules. No `Alert` per attempt, also
  when the driver fails after the probe saw the device connected (debug log only).
- `set_connected` false (one device or all) stops the retry for the devices it covers;
  choosing another device or profile forgets the state (choosing the same device again keeps it);
  either, a server change, or a retry gate that closes while the probe runs (checked every 1 s tick)
  stops a probe in flight before its PUT if not yet sent and drops its result;
  "not configured" is not retried until `set_alpaca_server`. `set_alpaca_server` re-selects the
  Alpaca devices for the new settings but keeps a device still to be connected (retrying or not
  configured) and tries it at once, so it alone connects a device `--auto-connect` could not.
- `guide`: connects a selected camera or mount that is not connected first (same probe, on the
  main thread, so the daemon answers no other RPC meanwhile; the camera and the mount share a
  budget of about 4 s, every request getting what is left of it; about 1.5 s per device, ~3 s for
  both, when the server is not reachable). The driver connect of a device the server reports
  connected is not bounded by the budget, so it starts only with at least 1 s left, else the device
  is refused as "...: the mount is still connecting" and the background retry connects it at once.
  A device whose request had not ended when the budget ran out is refused ("...: the mount failed
  to connect: no response from the Alpaca server (Timeout was reached)") and left to the
  background retry; one the budget ran out for before any request was sent is refused as "...: the
  mount was not tried in time" and counts as no attempt (its schedule and reason stay). A background attempt already
  in flight for the device is waited for within the budget (no second PUT) and its result used;
  if it has not ended in time: "...: the mount is still connecting". Refused only if that fails,
  code 1, the old text kept as the prefix:
  "all equipment must be connected first: the mount failed to connect: <reason>", or
  "...: the mount was disconnected by the user".
- `set_connected` connect errors gain " (<reason>)": "mount failed to connect (<reason>)",
  "equipment failed to connect: mount (<reason>)".
- `EquipmentReconnected{device_type}`: device_type can now be "mount" or "rotator" as well as
  "camera". Also sent once when a device that had failed to connect connects by any path (not for a
  device's first connect),
  flushed at the end of `GearDialog::EndModal` (after the calibration auto-load, before the RPC
  response). This fixes the RC51 finding: `set_connected` after `EquipmentReconnectFailed` now
  sends `EquipmentReconnected{camera}`. `EquipmentReconnectFailed` is still terminal for the
  automatic reconnection, but the camera then joins the slow retry.

ARA: does not parse "all equipment must be connected first" or "equipment failed to connect";
its `EquipmentReconnected` handler resolves only a camera-drop fault and ignores other device
types and unsolicited events (`GuiderService.FaultReaction.cs`, `AffectsGuiding`).

## 2026-11-19 — calibration frame retry (#116)

One failed exposure ended a calibration that was otherwise going well (RC51: four calibrations in a
row lost after up to 4 minutes of good steps to a single Alpaca 1280). The decision is in the wx-free
`src/capture_failure.cpp` (`IsTransient`, `ShouldRetryFrame`, unit-tested by
`test_capture_failure`); `MyFrame::OnExposeComplete` applies it.

- While calibrating, a frame that fails with Alpaca 1280 or a timed-out call ("no response from the
  Alpaca server (Timeout was reached)") is exposed again, up to 2 times in a row per calibration
  step, without moving the mount again, before `CalibrationFailed` with the #97 reason as before.
  Each failed frame still raises its `Alert`; the retry itself is in the debug log only. A good
  frame starts a new count, and so does the first camera reconnect in a run of failed frames (not a
  second one, so a camera that fails every frame ends the calibration after at most 2 reconnects).
- A 3rd 1280 in a row still reconnects the camera (#103), so `EquipmentDisconnected{reconnecting:
  true}` can now arrive mid-calibration with no client restart; the calibration resumes after
  `EquipmentReconnected`.
- Unchanged: 1031 and every other failure, a failure after `stop_capture`, and every failure
  while looping (guiding: see #136 below).

ARA: no code change. `CalibrationFailed.Reason` and the event order are unchanged, and the
`Alert` of a retried frame is not consumed. A 3rd consecutive 1280 during a calibration now
reaches its `EquipmentDisconnected` handler (`PHD2Guider.cs`, the `on_guider_lost` policy, default
`pause_and_retry`), which before this was reached mid-calibration only by a 1031.

## 2026-11-20 — drop transient guiding frames (#136)

One failed exposure stopped guiding: while guiding or settling, any failed frame ran
`StopCapturing` and failed the `guide` / `dither` request. RC51's ASI120 fails 10-30% of its
exposures through AlpacaBridge, so guiding stopped every few minutes and the sub was lost. The
decision is #116's (`ShouldRetryFrame` in `src/capture_failure.cpp`, unit-tested by
`test_capture_failure`), now also applied while guiding.

- While guiding (guider state `Guiding`, including a guide or dither settle and a paused
  guider), a frame that fails with Alpaca 1280 or a timed-out call is dropped, up to 2 in a row,
  with the same count as a calibration step: a good frame, a failure that is not dropped and the
  first camera reconnect in a run of failed frames start a new count.
- A dropped frame sends `StarLost` with a new `Frame` number (the next `GuideStep` numbers show
  the gap), `Time`, `AvgDist`, `StarMass`/`SNR`/`HFD` 0, no `ErrorCode`, and `Status` the #97
  text, e.g. `"camera error: ZWO image acquisition failed: … (Alpaca 1280)"`, the same string
  `SettleDone.Error` carries. The camera's `Alert` for it goes out as before, and the guide log
  gets a `DROP` line. No `GuidingStopped`, `LoopingExposuresStopped` or `SettleDone` error.
- No guide pulse and no deduced (dead-reckoning) move for a dropped frame; the star, the lock
  position and `get_app_state` (`Guiding`) are untouched, and the next exposure starts at once.
- A settle in progress counts a dropped frame like a lost-star frame (`TotalFrames` and
  `DroppedFrames` both go up, and a `Settling` event follows with `StarLocked` false). The settle
  timeout keeps running and is checked on that frame too. A dropped frame does not end a run of
  in-range frames, nor complete one: the next in-range frame measures the settle time from the
  first in-range frame. The settle rule is in the wx-free `src/settle_frame.cpp`, unit-tested by
  `test_settle_frame`.
- A 3rd 1280 in a row now reaches the #103 camera reconnect mid-guide:
  `EquipmentDisconnected{reconnecting: true}`, `EquipmentReconnected`, then guiding resumes on
  the next exposure (a slow reconnect can time a settle out, which fails the request while
  guiding continues).
- Unchanged: a 1031 is not dropped. It reconnects the camera at once (#103,
  `EquipmentDisconnected{reconnecting: true}`) and, when the reconnect succeeds,
  `EquipmentReconnected` follows and guiding resumes on the next exposure with no
  `GuidingStopped`; guiding stops only when that reconnect fails or is throttled
  (`EquipmentReconnectFailed`).
- Unchanged, guiding stops as before (`GuidingStopped`, `SettleDone{Error: "camera error: …"}`
  when a request is settling, `LoopingExposuresStopped`): every non-transient failure that does
  not reconnect the camera (another Alpaca error, an HTTP status, a transport or parse failure),
  a 1280 or timeout once the count is used up (a camera that fails every frame stops guiding at
  the 7th, after 2 reconnects), a camera reconnect that fails or is throttled, and any failure
  after `stop_capture`. Every failure while looping, with a star selected (including a `guide`
  request still selecting its star) or in `STATE_CALIBRATED` stops capture as before.

ARA: no code change. `StarLost` already raises a `star_lost` marker and sets the local state to
`LostLock` until the next `GuideStep`; `Dither` and `StartGuiding` read `get_app_state` over
RPC, which stays `Guiding`, so a dither is not skipped and guiding is not restarted. With guiding
no longer stopped by a single 1280, the `on_guider_lost` policy (default `pause_and_retry`) can
now run mid-guide, on the `EquipmentDisconnected{reconnecting: true}` of a 3rd 1280 in a row.

## 2026-11-19 — HTTP port is data-only (#114)

ARA is the user interface, so the embedded web UI and its control routes are gone. The HTTP
listener stays on `8080 + (instance − 1)` (the JSON-RPC port + 3680) and serves two routes,
unchanged: `GET /api/capture/<filename>` and `GET /api/frame.jpg`. Every other method or path is
`404`, including the old `405` for a method other than GET or POST.

- Removed: `GET /` and `/index.html`, `GET /assets/*`, `GET /api/setup`, `GET /api/discover/alpaca`
  and `POST /api/rpc`, with the `/api/rpc` busy guard (`src/webui_rpc_guard.cpp`, #81), the web
  app (`scripts/webui/`, installed to `share/phd2/webui`) and `scripts/phd2_web_ui_server.py`.
- Every method the removed routes reached is still on `:4400`, including
  `discover_alpaca_servers`. The busy guard did not apply to `:4400` and still does not.

ARA: no code change. It uses `:4400` for control and only the two kept routes on `:8080`
(`PolarAlignFrameFetcher` / `PHD2Guider.CaptureFrameUri`, `PHD2Guider.LiveFrameUri`).

## 2026-11-19 — Static PA removed (#113)

ARA's polar alignment is the only polar-alignment routine, so the in-guider Static PA tool
(inherited from PHD2) is gone. Its auto mode slewed the mount through upstream motion code with
no meridian, pier-side or hour-angle guard; removing it supersedes #112.

- `staticpa_start`, `staticpa_measure`, `staticpa_get_status`, `staticpa_stop` and
  `staticpa_close` now return JSON-RPC `-32601` "method not found". The entries above that
  describe them (2026-06-10) are history.
- `guiding_assistant_start` no longer checks for a running Static PA tool; it still refuses
  while polar drift or drift alignment is running. The connect retry (#104) no longer waits on
  an open Static PA tool; it still waits while polar drift is drifting or the drift tool is open.
- `LoopingExposures` no longer carries `Status: "Static PA: waiting for the RA step to finish"`.
- Polar drift (`polardrift_*`) and drift align (`driftalign_*`) are unchanged.

ARA: no code change. Nothing calls the `staticpa_*` methods since openastro-ara#1686; the unused
classes in `PHD2Methods.PolarAlign.cs` go in an ARA follow-up.

## 2026-11-19 — Polar Drift and Drift Align removed (#129)

ARA's polar alignment is the only polar-alignment routine, so the in-guider Polar Drift and
Drift Align tools (inherited from PHD2) are gone. The Drift Align window had a "Slew To" button
that drove the mount; no RPC method reached it.

- `polardrift_start`, `polardrift_get_status`, `polardrift_stop`, `polardrift_close`,
  `driftalign_start`, `driftalign_set_phase`, `driftalign_drift`, `driftalign_adjust`,
  `driftalign_get_status` and `driftalign_close` now return JSON-RPC `-32601` "method not
  found". The entries above that describe them (2026-06-10) are history.
- `guiding_assistant_start` no longer refuses while polar drift or drift alignment is running.
  The connect retry (#104) no longer waits on those tools, and its "polar or drift alignment in
  progress" reason is gone; it still waits on a dark build, a `set_pa_session` lease and a
  Guiding Assistant run.
- The Guiding Assistant's > 10 arc-min polar-alignment recommendation (`id` `"PA"`) now reads
  "redo the polar alignment to improve it" instead of naming the Drift Align tool. Its
  polar-alignment estimate is unchanged.
- The mount slew calls only Drift Align used are gone too, so the guider has no path that slews
  the mount. Its Alpaca mount driver no longer reads `canslew` / `canslewasync` when it connects.
- `get_pa_session` / `set_pa_session` are unchanged.

ARA: no code change. Nothing calls the `polardrift_*` or `driftalign_*` methods; the unused
classes in `PHD2Methods.PolarAlign.cs` go in an ARA follow-up.

## 2026-11-20 — clear_calibration refused while calibrating (#137)

`clear_calibration` during a calibration erased the state the calibration was stepping through,
so the next frame hit `assert(false)` in `Scope::UpdateCalibrationState` (the packaged guider,
built with asserts, aborted) or, with `NDEBUG`, left the guider in `Calibrating` forever.

- `clear_calibration` → error 1 "cannot clear calibration while calibrating", nothing changed,
  while the guider is calibrating. Still allowed while guiding, as in PHD2: guide corrections keep
  the calibration guiding started with, but `dither` and `flip_calibration` fail until the next
  `guide` calibrates again. The rule is in the wx-free
  `src/restore_calibration.cpp` (`test_restore_calibration`).
- A calibration cleared some other way while it runs (the legacy socket server's CLEARCAL) now
  ends as a failed calibration: `CalibrationFailed` with `Reason` "Calibration failed: the
  calibration was cleared while calibrating" ("AO calibration failed: ..." for an AO), and the
  guider goes back to looping, as after any calibration failure.
- `guide_pulse` refuses an `amount` outside `-60000..60000` with `-32602` before the sign flip;
  `-2147483648` used to pass the cap as a negative duration (a no-op that returned `0`; for an
  AO, an abort on `assert(steps >= 0)` in the packaged guider).
- The `.deb` is now built as Release (`NDEBUG`), like CI and dev builds.

ARA: no code change. `ClearCalibrationAsync` (the live clear button) already throws on an error
response; `ClearCalibration` (mediator) returns false.

## 2026-11-20 — set_algo_param value check, Predictive PEC period (#138)

- `set_algo_param` without `value` crashed the guider (the missing param was dereferenced). A
  missing or non-numeric `value` (a string, a bool, `null`) now gets JSON-RPC `-32602` "expected
  numeric value param"; a non-numeric one got error `1` "expected param value param" before. A bad
  axis or name and a failed set keep error `1`. The other handlers already checked their params
  before reading them; the readers they share (`float_param`, `bool_param`) now refuse a missing
  param themselves too, in the wx-free `src/jsonrpc_request.cpp`.
- `set_algo_param` `periodLength` on Predictive PEC (Gaussian Process) now changes only the
  period. It wrote every other GP hyperparameter as 0: the three signal variances were saved to
  the profile as 0, which turned the PEC prediction off across restarts, the three length scales
  went back to their defaults, and the call answered "could not set param" although the period was
  set. The periodic length scale is the one saved in the profile, not the GP guider's own read of
  it, which is wrong once the length scale is over half the old period. A period below 1 is still
  refused with error `1` (the period goes back to its default).

ARA: no code change. It always sends a numeric `value` and does not set `periodLength`.

## 2026-11-20 — pixel-scale change acted on once per request (#139)

A camera connect that reads a pixel size 1% or more off the profile's makes the guider adjust for
the new image scale (PHD2 behaviour): it recomputes `/scope/CalibrationDuration`, sets both axes'
`minMove` to `SmartDefaultMinMove`, clears the calibration and raises the "Binning or camera pixel
size changed unexpectedly" `Alert`. The Gear dialog reset what triggers it only when the dialog
opened, which the headless daemon never does, so after one such connect every later
`set_connected` (one device or all, connect or disconnect, the #104 retry and `guide`'s connect
included) and `set_profile` did it again, wiping the `calibration_duration` and `minMove` the client
had set since, and a connect of all equipment raised the `Alert` twice.

- Each gear request (`set_connected`, `set_profile`, and the #104 retry and `guide` connects) now
  begins afresh, as a run of the dialog does, and the adjustment is made once, by the request whose
  camera connect saw the change.
- The camera change itself carries over to later requests: the mount's calibration is cleared once,
  by the first request that has the mount connected (the camera's own, or a later one when the mount
  was off), and the calibration saved in the profile is not auto-loaded for the rest of the process,
  as before, because it was made at the old image scale. A calibration made since is kept.
- A request refused because the GUI's Connect Equipment dialog is open leaves the dialog's latch alone.
- The latch is the wx-free `CameraChangeLatch` in `src/connect_request.cpp`, unit-tested by
  `test_connect_request`; that the gear requests drive it is checked live against the fake Alpaca
  camera.

ARA: no code change. Its `set_profile_setup` `pixel_size` and its `set_algo_param` `minMove`
now survive later connects and the #104 automatic reconnects.

## 2026-11-20 — high-Dec calibration RA step (#141)

No API change. A calibration above about Dec 79° failed with `CalibrationFailed` "RA Calibration
Failed: star did not move enough": the West leg pulsed the Dec 0 step, which moves the star only
cos(Dec) as far, and ran out of its 61 pulses.

- The West and East legs now pulse `ceil(step / max(|cos(Dec)|, 1/4))` ms, at most 25 000 ms
  (a step already longer is not grown), for the Dec the guider reads from the mount once at the
  start of each calibration. The 25 s ceiling keeps a grown pulse under the 30 s HTTP timeout the
  Alpaca client gives a `PulseGuide` that blocks while it runs; it only matters for a step over
  6250 ms. The Dec leg, backlash clearing and the
  South nudge keep the step. An unknown Dec (no coordinates, a failed read) keeps the step for every
  leg, as before.
- Not changed: `calibration_duration` (`get_profile_setup`/`set_profile_setup`, the profile's
  `/scope/CalibrationDuration`), the guide-log header's "Calibration Step", the `Calibrating` event
  fields and their order, the `CalibrationFailed` reasons, `get_calibration_data`. The RA leg takes
  as long as before in fewer `Calibrating` events (RA pulses up to 4x the step, 2.2 s for a 550 ms
  step), so a client's calibration timeout is not affected.
- Every calibration still starts from scratch; the scaled step is not stored anywhere.

ARA: no code change.

## 2026-11-20 — Dec guide mode switched from Off during a calibration (#148)

A calibration started with Dec guide mode Off (`set_dec_guide_mode` "Off", `DEC_NONE`) skips the
Dec leg and completes on the frame after the East leg with no Dec rate. A `set_dec_guide_mode`
to Auto, North or South in that one frame left the mount uncalibrated for the new mode, so the
guider ran the completion again on every frame, sending `CalibrationComplete` each time, and never
left `Calibrating`.

- That calibration now fails, before anything is stored: `CalibrationFailed` with `Reason`
  "Calibration failed: the Dec guide mode was changed from Off during calibration", an `Alert`
  with the same text, and the guider goes back to looping, as after any calibration failure. No
  `CalibrationComplete` is sent and the RA-only calibration is not saved.
- Not changed: `set_dec_guide_mode` is still accepted during a calibration. A mode set before the
  East leg ends is used for the Dec leg, as before; a calibration that ran the Dec leg completes
  whatever the mode is when it completes, Off included; Off throughout completes as before (an
  RA-only calibration). The other `CalibrationFailed` reasons and the event order are unchanged.
- The check is in the wx-free `CalibrationRun` (`src/calibration_run.cpp`,
  `Fail::DecModeChanged`), so it holds whatever changed the mode.

ARA: no code change. The new `Reason` is one more failed calibration; a client that waits for
`CalibrationComplete` or `CalibrationFailed` gets the failure instead of waiting forever.

## 2026-11-20 — 25 s guide pulse ceiling (#152)

No new method or field. A dither's fast recenter and an AO bump of the mount were sent at
whatever length they were computed, so one computed with a rate of 0 asked the mount for
`INT_MAX` ms, and a guide step was held only to the max RA/Dec duration, which
`set_guide_limits` accepts up to 1 000 000 ms.

- Every pulse sized from a star offset (guide step, deduced move while the star is lost,
  fast-recenter step after a `dither`, AO bump) is now at most 25 000 ms, under the 30 s HTTP
  timeout the Alpaca client gives a `PulseGuide` that blocks while it runs, the ceiling #141 gave
  the calibration's grown RA step. A `GuideStep` whose RA or Dec pulse was cut to 25 000 ms has
  `RALimited` / `DecLimited` set, as for the max duration.
- `set_guide_limits` still accepts and stores up to 1 000 000 ms and `get_guide_limits` returns
  what was set; a value over 25 000 now guides as 25 000.
- Not changed: the fast-recenter step and the AO bump are still not held to the max RA/Dec
  duration and still ignore `set_dec_guide_mode` (a `dither` with a North or South Dec mode and a
  settle runs the settle in Auto, as before). Calibration pulses and `guide_pulse` (its own
  `-60000..60000`) are not capped.

ARA: no code change.

## 2026-11-20 — "Calibration was too far from equator" Alert on Dec jitter (#157)

No API change. Every entry to `Guiding` (each `guide`, after its calibration when one runs)
checks the Dec the mount reports against the calibration's Dec. For a calibration above Dec 60°,
any difference at all raised an `Alert` "Calibration was too far from equator, recalibration is
needed.", so a calibration that had just completed at Dec 70° or 80° alerted as soon as the
reported Dec moved by a driver's rounding.

- A Dec within 0.02° (72") of the calibration's is now unchanged. Above 60° the `Alert` is raised
  only when the Dec moved enough to change the RA rate by more than 5%,
  |cos(Dec now) / cos(calibration Dec) − 1| > 0.05: about 0.5° at Dec 80°, and a move from 70° to
  75° still raises it. A smaller move raises no `Alert`, and the guider guides with the
  calibration's RA rate as measured, as it did when the Dec had not changed.
- Below 60° a Dec that moved more than 0.02° scales the RA rate by cos(Dec now) / cos(calibration
  Dec) when Dec compensation is on, as before; a smaller move keeps the calibration's RA rate
  (under 0.07% different).
- A Dec that is not finite is treated like an unknown Dec: no `Alert` and no scaling.
- Not changed: the `Alert` text, the 60° limit, `get_calibration_data`, the `Guiding`/`GuideStep`
  events, and every other `Alert` of the same check (guide speeds, side of pier, pixel size,
  rotator).

ARA: no code change. It recalibrates on every new target, so the `Alert` it showed after a fresh
high-Dec calibration goes away; it still appears for a calibration reused after a real Dec move.

## 2026-11-20 — backlash clearing at high Dec (#156)

No API change. Before the Dec leg, backlash clearing pulses North until three pulses in a row each
move the star at least 0.6 of a pulse's expected move. That move came from the RA rate the West
leg measured, which shrinks with cos(Dec) while a Dec pulse does not: 1.3 px at the equator on the
FL 400 mm / 3.75 µm / 0.5x profile, 0.2 px at Dec 80°, where centroid noise passed for a move and
the Dec rate could be measured with backlash still in it.

- With the Dec the guider reads from the mount at the start of the calibration (#141), clearing
  now expects the Dec leg's move at every Dec (the RA rate divided by cos(Dec), times the Dec/RA
  guide-rate ratio as before). An unknown Dec keeps the old step.
- A client may see more `Calibrating` steps with `dir` "Backlash" at high Dec, while the backlash
  is taken up; clearing still sends at most max(8, 60000 / step) pulses and still goes on once the
  star has moved the calibration distance. A wrong Dec reading (85° reported at the equator) makes
  clearing accept no move and go on after the calibration distance; the calibration completes.
- What a wrong Dec reading costs: one whose cos(Dec) is under 0.6 of the true one (55° reported at
  the equator already, or a mount in a false Home that reports 90°) makes clearing use up the
  calibration distance North before the North leg starts. The star then travels up to about twice
  the calibration distance North, beyond the edge margin auto-select keeps (#140: the calibration
  distance plus the search region), so a star picked near that margin can be lost and the
  calibration fail. The South leg sends back only the North leg's pulses and the nudge is at most 4
  pulses of one step, so a completed calibration leaves the star about one calibration distance from
  the lock instead of about 2 px; the `Calibrating` steps show it, and guiding brings it back.
- Not changed: the clearing pulse (the calibration step), the `Calibrating` event fields and their
  order, the `CalibrationFailed` reasons, `get_calibration_data`.

ARA: no code change.

## 2026-11-20 — max-duration alert one guide step earlier (#153)

No API change. The `Alert` event the guider sends when its guide steps keep hitting the max RA or
Dec duration ("PHD2 is not able to make sufficient corrections in RA/Dec..." and its variants)
now comes on the 5th guide step in a row clamped in the same direction, instead of the 6th.

- Not changed: the alert text and flags, the 30 s minimum between two of them, what starts a new
  run (a clamp the other way, an unclamped step, a step the Dec guide mode zeroes), and the
  `GuideStep` event's `RALimited` / `DecLimited`, which are set on every clamped step as before.

ARA: no code change.

## 2026-11-20 — the event order of a guide request, checked in CI (#142)

No API change. The order below is what the daemon sends today (inherited from PHD2's
`PhdController`); the `Daemon e2e (arm64, ASan/UBSan)` CI job now checks it against the real
daemon and the fake Alpaca server (`scripts/e2e_guide_scenarios.py`), so a change to it fails CI.

For one `guide` request with `recalibrate: true` that calibrates and settles:

- `StartCalibration`, one or more `Calibrating`, `CalibrationComplete`;
- `StartGuiding`, then `SettleBegin` at once, before the first `GuideStep`;
- `Settling` and `GuideStep` events, interleaved, while the settle runs;
- exactly one `SettleDone` (`Status` 0 on success), then `GuideStep` events.

`LockPositionSet` and `StarSelected` come before `StartCalibration` when the request selects
the star, and `ConfigurationChange` / `AppState` can arrive in between; the check ignores those.
A request that fails still ends with exactly one `SettleDone` (see `doc/jsonrpc_api.md` for the
failure orders). A dropped frame while guiding (#136) is a `StarLost` and the `GuideStep`s go on.

The job's second lane (a fake with a camera angle, cos(Dec), backlash, drift, periodic error and
seeing) also pins, with no API change:

- `get_calibration_data` `xAngle`/`yAngle` within 2° and `xRate`/`yRate` within 5 % of the fake's
  true values (`xRate`/`yRate` in px/s, the RA rate at the calibration's Dec);
- a `dither` while guiding: `LockPositionSet`, `GuidingDithered` (`dx`/`dy`, the dither in RA/Dec
  px), `SettleBegin`, `Settling` and `GuideStep` events, exactly one `SettleDone` (`Status` 0),
  then `GuideStep`s; `get_lock_position` moves by `dx` along `xAngle` plus `dy` along `yAngle`
  (camera px, the dither's length kept);
- frames with no star while guiding (clouds): one `StarLost` each, the guider finding no star:
  `ErrorCode` 2 with `Status` "Star lost - low SNR", or `ErrorCode` 3 with "Star lost - low mass"
  (not 7, "Star lost - mass changed", which a star found but too faint gives, and not a camera
  error, which has no `ErrorCode`); `get_app_state` `LostLock` (or `Guiding`) meanwhile, no
  `GuidingStopped`, and a `GuideStep` on the first frame with the star again.

ARA: no code change. It waits for `SettleDone` as the end of the request, which this pins.

## 2026-11-20 — settings on disk when the call returns (#142)

A setting changed over JSON-RPC is now in the config file on disk by the time the response is
sent, for every method, unless the write fails: the event server writes the config after each
request, when the request changed it. Before, most setters (`set_dither_settings`,
`set_star_detection`, `set_auto_exposure`, `set_noise_reduction`, `set_guider_options`,
`set_variable_delay_settings`, `set_algo`, `set_algo_param`, `set_exposure`, `set_dec_guide_mode`,
`set_guide_limits`, `set_dec_comp`, `set_mount_options`, `set_backlash_comp`, `set_camera_gain`,
`set_camera_timeout`, `set_use_subframes`, `set_camera_saturation`, and the profile create, clone,
rename, delete and select methods) changed it in memory only, and it reached the disk at the next
method that wrote the file or at a clean exit, so a power cut or kill lost it.

- The write leaves the old file or the new one whole whatever the moment of a power cut
  (temporary file, fsync, rename, directory fsync); a request that changed nothing writes nothing.
- A daemon stopped with SIGTERM (`systemctl stop`) or `shutdown` with a camera and mount in its
  profile no longer reads freed memory on the way out.
- A write that fails (a full disk, an SD card remounted read-only) does not change the response:
  the setting is changed in memory and the request succeeds as before, the debug log gets
  `PhdConfig flush failed: <step and error>`, and the write is tried again after every later
  request, and at exit, until one succeeds. A client cannot tell from the response that the
  setting is not on disk yet.
- No change to any method's parameters, results or errors. A request that changes a setting takes
  the time of the write longer to answer: about 1 ms on an NVMe disk, more on an SD card.
- `scripts/e2e_persistence.py` (CI) checks it for 29 settings across SIGTERM and SIGKILL.

ARA: no code change. A setting it sent is kept across a power cut once it has the answer, unless
the guider's disk refused the write.

## 2026-11-20 — random dither seeded at startup (#173)

No new method or field. With the dither mode Random (the default; Spiral is unchanged), the
offsets a `dither` moves the lock by, `GuidingDithered` `dx`/`dy`, came from a sequence that only
the simulator camera ever seeded, so every start of the guider gave the same dithers in the same
order (the first 3 px one RA +2.04, Dec −0.63 px). They now come from a generator seeded once at
startup, so each start gives its own sequence.

- Same distribution as before: RA, and Dec unless the dither is RA only (the request's `raOnly`,
  `set_dither_settings` `RaOnly`, or Dec guide mode Off), each uniform over [−amount, +amount],
  `amount` being the request's `amount` times `ScaleFactor`; an RA-only dither has `dy` 0.
- A client that expected a given offset from a fresh start, as nothing should, gets a different
  one each start.

ARA: no code change.

## 2026-11-20 — max-duration alert throttled per axis (#164)

No API change. The `Alert` event the guider sends when its guide steps keep hitting the max RA or
Dec duration ("PHD2 is not able to make sufficient corrections in RA/Dec..." and its variants)
had one 30 s throttle for both axes, restarted by calls that raised nothing.

- An RA alert no longer holds a Dec alert back for 30 s, or the other way round: each axis has its
  own 30 s minimum between two of its alerts, so a client can get an RA and a Dec alert together.
- The 30 s starts only when an alert is sent. A clamped guide step during the 120 s grace period
  after a calibration or a guide start sends nothing and no longer restarts it, so the first alert
  after the grace period is no longer held back up to 30 s more.
- Not changed: the alert texts and flags, the grace period, the 5 clamped steps in a row that
  raise it (#153), and `GuideStep`'s `RALimited` / `DecLimited`.

ARA: no code change.

## 2026-11-20 — fast recenter counts what the mount moved (#165)

No API change. After a `dither` with fast recenter on (`/guider/FastRecenter`, the default) the
guider moves the star back to the new lock in recovery steps that bypass the guide algorithm,
then the guide algorithm takes over and the settle runs as before.

- Each step now counts only what the mount moved of it. A step cut to the 25 s pulse ceiling
  (#152) or by an AO's travel is made up by another recenter step, so a client can see one or more
  extra frames before the guide algorithm takes over, each a `GuideStep` with the pulse sent, as
  before. So is a step that moved nothing because its move failed once. Three steps in a row that
  moved nothing (the move keeps failing, or guiding is disabled) end the recenter, and the guide
  algorithm corrects the rest, instead of the guider sending recenter steps until it had counted
  the whole dither.
- When every pulse is sent whole the recenter takes the same steps as before, and the variable
  exposure delay counts the frames as recentering up to the step that moves the rest of the dither,
  not the frame after it, as before. After a last step that was cut or failed, that frame is
  followed by the step that makes it up.
- Not changed: `dither`'s parameters and result, `GuidingDithered`, the settle and `SettleDone`, and
  recenter steps still ignoring the Dec guide mode (Off included) and the max RA/Dec duration.

ARA: no code change.

## 2026-11-20 — variable delay in fractional seconds (#174)

`set_variable_delay_settings` cast `ShortDelaySeconds` and `LongDelaySeconds` to an integer before
multiplying by 1000, so 2.5 s was stored as 2000 ms, and `get_variable_delay_settings` reported the
stored ms divided by 1000 in integers.

- `set_variable_delay_settings` stores each delay rounded to the nearest ms (2.5 s is 2500 ms).
  Params, result and errors unchanged.
- `get_variable_delay_settings` reports the stored delays in seconds with their fraction:
  `{"Enabled":true,"ShortDelaySeconds":2.5,"LongDelaySeconds":7.25}`. A whole number of seconds
  is still a JSON integer (`3`, not `3.0`), the type PHD2 documents, so a client that only sends
  whole seconds reads exactly what it read before. The value is printed with 6 significant digits,
  so it is exact to the ms below 1000 s (PHD2's own dialog allows up to 10 s short, 120 s long).
- A profile saved before this keeps its delays: they are stored in ms (`/frame/var_delay/*`), and
  only a fractional delay sent over JSON-RPC was cut.
- The inherited Advanced Settings dialog still shows whole seconds and writes both delays back when
  it is closed with OK, so on a desktop run that cuts a fractional delay to whole seconds (2.5 s
  becomes 2 s) even if its delay fields were not touched. The packaged service runs headless under
  `xvfb-run`, where nobody can open the dialog.

ARA: read `ShortDelaySeconds` and `LongDelaySeconds` as numbers, not integers, if it shows them.

## 2026-11-21 — `guiding_assistant_apply` answers with valid JSON (#184)

No new method or field. The success result's `applied` array listed the applied names without quotes,
`{"applied":[min_move_ra,min_move_dec]}`, which is not JSON: a client's parser failed on the whole response line.

- `applied` is now an array of strings: `{"applied":["min_move_ra","min_move_dec"]}`, in the order the names were
  applied (RA, then Dec, then backlash), as before.
- Not changed: which names `which` accepts, what is applied, the refusals (error 1) and the parameter errors.

ARA: no code change needed if it already expected strings; until now any call that applied something failed to
parse.

## 2026-11-20 — get_exposure while the camera is disconnected (#176)

`get_exposure` returned `0` whenever the camera was not connected, even right after a
`set_exposure` that succeeded.

- `get_exposure` now returns the stored exposure in ms whatever the connection state: what the
  last `set_exposure` (or the profile) selected; with Auto (`set_exposure` `-1`) the exposure auto
  exposure is at, as with a camera connected. With a camera connected it returns what it did
  before.
- `set_exposure`, `get_exposure_durations` and the other exposure methods are unchanged.

ARA: no code change needed. It can read `get_exposure` before connecting and show the selected
exposure; it no longer has to treat `0` as "not connected".

## 2026-11-20 — setters answered without the 40 ms delayed-ACK wait (#175)

A setter that sends an event before its response (`ConfigurationChange` for any profile write)
took about 45 ms to answer, against about 0.1 ms for a getter: the event and the response are two
small writes, and without `TCP_NODELAY` the response waited for the client's delayed ACK of the
event. The event server now sets `TCP_NODELAY` on every accepted JSON-RPC (`:4400`) and HTTP
(`:8080`) connection, and a setter is answered in well under a millisecond on a Release build.

- No change to any method, event, or the order of the bytes on the wire: the `ConfigurationChange`
  event still arrives before the response, only without the wait between them.

ARA: no code change. Its connect and profile-change setter sequences finish about 45 ms per call
sooner.

## 2026-11-21 — one `LoopingExposuresStopped` per failed capture frame (#186)

No new method or field. A capture frame that failed for good (not one of the #116 / #136 retried
frames) while the guider was looping, calibrating or guiding sent `LoopingExposuresStopped` twice in
a row: a 1280 while looping, the Alpaca server going away while guiding, and the camera reconnect
throttle running out (`EquipmentReconnectFailed`) each ended with two.

- It is now sent once, in the same place as the first one was, so the order is otherwise unchanged:
  `Alert`, then `GuidingStopped` when guiding, `LoopingExposuresStopped`, then `LockPositionLost`
  when the lock is lost; and `EquipmentDisconnected`, `EquipmentReconnectFailed`,
  `LoopingExposuresStopped` for the throttle.
- With a `guide` request in flight, its `SettleDone` follows the one `LoopingExposuresStopped` when
  looping or selecting a star, and no event comes after it (the second stop used to come after it);
  when calibrating or guiding the order is `GuidingStopped`, `SettleDone`, `LoopingExposuresStopped`.
  `doc/jsonrpc_api.md` gives the same order.
- Not changed: a frame that fails after a `stop_capture`, or a `capture_single_frame` that fails,
  still sends one; the failure reasons in the `Alert` and in a failed guide request's `SettleDone`;
  `get_app_state` reading `Stopped`.

ARA: no code change. A client that counted the stops, or took the second one as a new stop after it
had already started looping again, now sees one.

## 2026-11-21 — PulseGuide-failed alert throttled (#188)

No new method, field or event. The `Alert` event "PulseGuide command to mount has failed - guiding is
likely to be ineffective." was sent for every guide pulse the Alpaca mount failed, so a mount that
stops taking pulses sent two identical alerts a frame when both axes corrected.

- The first failed pulse still sends the alert at once; after it, at most one is sent every 30 s per
  mount, whichever axis failed. The 30 s is timed to the whole second, so two alerts can be just
  over 29 s apart. It starts only when an alert is sent, and runs on a steady clock, so a system clock change does not hold it back.
- Not changed: the alert text and flags, its "don't show again" setting
  (`/Confirm/<profile>/PulseGuideFailedAlertEnabled`), the failed pulse counting as a failed move,
  guiding going on, and the "Guiding stopped: the scope started slewing." alert.

ARA: no code change; it gets one alert for a run of failed pulses instead of one per pulse, so it
should not count `Alert` events to judge how many pulses failed.

## 2026-11-21 — multi-star turned back on while guiding keeps the star (#187)

No new method, field or event. Turning multi-star back on while calibrating or guiding stopped guiding, auto-selected
a star and started guiding again: `GuidingStopped`, then `StartGuiding` with no `StarSelected`, `LockPositionSet`,
`SettleBegin` or `SettleDone`, and a lock position moved to wherever the star was, or onto another star. Over the API
that happened at the end of every Guiding Assistant run started while guiding with multi-star on (the run turns
multi-star off and puts it back), and on `set_lock_shift_enabled` `false` while guiding with multi-star on (lock shift
turns it off the same way).

- Guiding (or calibration) now goes on with the same star and lock position. The secondary stars found when the star
  was selected are found again around the primary once it is back on the lock, as after a lock position change; if
  the selection had none, guiding goes on with the one star until the next star selection.
- A GA run's stop or end while guiding now sends only its `GuideParamChange` events (`MountGuidingEnabled` `true`,
  `MultiStar` `true`) and `GuidingAssistantDone`; `get_lock_position` reads what it did before the run.
- Not changed: turning multi-star on while looping with a star selected still auto-selects a star to find the
  secondary stars (a GA run started from looping ends this way), and the setting is restored as before.

ARA: no code change needed. It no longer sees guiding stop and start again without a settle at the end of a GA run;
a handler that re-armed its guiding state on that `StartGuiding` will simply not run.
