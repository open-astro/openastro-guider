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
