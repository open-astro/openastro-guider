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
  optional; unspecified pulse/floor/ceiling keep current; `Floor` <= `Ceiling`; pulse within
  the backlash min/max.
- `get_auto_exposure` → result: { `Enabled`, `MinExposure`, `MaxExposure` (ms), `TargetSNR` }.
- `set_auto_exposure` { `MinExposure`?, `MaxExposure`?, `TargetSNR`? } → result: 0. All optional;
  `Min` <= `Max`. (Auto-exposure is *enabled* by selecting "Auto" via `set_exposure`.)
- `get_noise_reduction` → result: method name ("None" / "2x2 Mean" / "3x3 Median").
- `set_noise_reduction` { `method` } → result: 0.

### Still to do (Batch C2 + later — see `API_GAP_AUDIT.md`)
Camera subframes-setter / cooler-setpoint / saturation / software-binning, rotator reverse, the
niche star options (tolerate-jumps, fast-recenter, auto-select-downsample). Plus the original
known gap:
**dark/bad-pixel-map library management** (`build_dark_library` / `build_defect_map_darks` /
`set_dark_library_enabled` / `set_defect_map_enabled` now exist; create/select/delete coverage
to be confirmed against ARA's needs).
