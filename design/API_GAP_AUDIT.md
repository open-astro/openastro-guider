# Phase 5 — API Gap Audit (Advanced/"Brain" settings → JSON-RPC)

Goal of Phase 5: let **ARA** (and the web UI) fully drive `openastro-guider`. This audit maps
the Advanced Settings ("Brain") dialog + the persisted profile settings against the existing
JSON-RPC surface and lists the gaps, prioritized.

**Method:** cross-referenced the event-server dispatch table (`src/event_server.cpp`) against
the `*ConfigDialogCtrlSet` controls and the `pConfig->Profile` keys in `myframe/camera/guider/
guider_multistar/mount/scope/stepguider/rotator`. Snapshot date: 2026-06-07.

**Principle:** every method lives on the **shared dispatch** (`call_rpc_result_raw`), so adding
one exposes it over **both** TCP `:4400` (NINA) and HTTP `:8080` `/api/rpc` (ARA/web) at once.
Each new method must be logged in `design/API_CONTRACT.md`.

---

## Already covered (no work needed)

- **Lifecycle / session:** `get/set_connected`, `get_app_state`, `loop`, `stop_capture`,
  `guide`, `dither`, `guide_pulse`, `find_star`, `deselect_star`, `get/set_paused`, `shutdown`,
  `capture_single_frame`, `get_settling`, `get_pixel_scale`, `save_image`, `get_star_image`.
- **Profiles:** full CRUD — `get_profiles`, `get/set_profile`, `set_profile_by_name`,
  `create_profile`, `clone_profile`, `rename_profile`, `delete_profile`.
- **Equipment selection + discovery:** `get_equipment_choices`, `get/set_selected_{camera,
  camera_id,mount,aux_mount,ao,rotator}`, `get_current_equipment`, `get/set_alpaca_server`,
  `discover_alpaca_servers`, `query_alpaca_devices`, `set_selected_alpaca_device`,
  `get_{alpaca,selected}_camera_pixelsize`.
- **Exposure:** `get/set_exposure`, `get_exposure_durations`.
- **Calibration:** `get_calibrated`, `get_calibration_data`, `clear_calibration`,
  `flip_calibration`, `get_calibration_files_status`, `delete_calibration_files`.
- **Lock / shift:** `get/set_lock_position`, `get/set_lock_shift_enabled`,
  `get/set_lock_shift_params`.
- **Dec guide mode:** `get/set_dec_guide_mode`.
- **Algorithm parameters (values):** `get_algo_param_names`, `get/set_algo_param` — enumerate
  and get/set the *parameters of the currently selected* RA/Dec algorithm.
- **Dark / defect library:** `build_dark_library`, `build_defect_map_darks`,
  `set_dark_library_enabled`, `set_defect_map_enabled`.
- **Cooler / sensor:** `get_ccd_temperature`, `get_cooler_status`, `set_cooler_state` (on/off).
- **Camera info:** `get_camera_binning`, `get_camera_frame_size`, `get/set_camera_bitdepth`,
  `get_use_subframes` (getter), `get_search_region` (getter).
- **Guide output:** `get/set_guide_output_enabled`.
- **Misc:** `get/set_variable_delay_settings`, `get/set_limit_frame`, `export_config_settings`.
- **Profile-setup aggregate** (`get/set_profile_setup`, typically applied while disconnected):
  `pixel_size`, `focal_length`, `guide_speed`, `camera_binning`, `software_binning`,
  `calibration_distance`, `calibration_duration`, `high_res_encoders`, `multistar_enabled`,
  `mass_change_threshold_enabled`, `auto_restore_calibration`.

---

## Gaps (what ARA can't set today)

Priority: **P1** = needed for real guiding control / tuning; **P2** = useful; **P3** = niche.

| # | Pri | Area | Setting(s) | Config key | Proposed method |
|---|----|------|-----------|-----------|-----------------|
| 1 | **P1** | Mount | **Guide-algorithm selection** per axis (which algorithm, not just its params) | `/<mount>/XGuideAlgorithm`, `/YGuideAlgorithm` | `get_algo` / `set_algo` (axis), `get_algos` (list available) |
| 2 | **P1** | Mount | **Max RA / Max Dec duration** (live tuning) | `MaxRaDuration`, `MaxDecDuration` | `get/set_guide_limits` (or per-axis) |
| 3 | **P1** | Mount | **Declination compensation** on/off | `UseDecComp` | `get/set_dec_comp` |
| 4 | **P1** | Star | **Min SNR, Min/Max HFD** (star acceptance) | `/guider/StarMinSNR`, `StarMinHFD`, `StarMaxHFD` | `get/set_star_detection` |
| 5 | **P1** | Star | **Mass-change threshold value** (only the *enabled* flag is in profile_setup) | `/guider/onestar/MassChangeThreshold` | fold into `get/set_star_detection` |
| 6 | **P1** | Star | **Search region size** (setter; getter exists) | `/guider/onestar/SearchRegion` | `set_search_region` |
| 7 | **P1** | Dither | **Default dither scale / RA-only / mode** | `/DitherScaleFactor`, `/DitherRaOnly`, `/DitherMode` | `get/set_dither_settings` |
| 8 | **P1** | Camera | **Gain** | `/camera/gain` | `get/set_camera_gain` |
| 9 | **P2** | Camera | **Subframes on/off** (setter; getter exists), **read timeout** | `/camera/UseSubframes`, `/camera/TimeoutMs` | `set_use_subframes`, `get/set_camera_timeout` |
| 10 | **P2** | Camera | **Cooler setpoint** (`set_cooler_state` is on/off only) | `/camera/CoolerSetpt` | new `set_cooler_setpoint` — **don't** overload `set_cooler_state` (would break existing callers) |
| 11 | **P2** | Camera | **Saturation ADU / by-ADU**, **software binning toggle** | `/camera/SaturationADU`, `SaturationByADU`, `SoftwareBinning` | `get/set_camera_saturation` |
| 12 | **P2** | Mount | **Backlash compensation** (enabled, pulse, floor/ceiling) | `/<mount>/BacklashCompEnabled`, `DecBacklashPulse`, `DecBacklashFloor`, `DecBacklashCeiling` (persisted; keys built dynamically in `backlash_comp.cpp`) | `get/set_backlash_comp` |
| 13 | **P2** | Mount | **Mount behaviour flags**: assume-orthogonal, cal-flip-requires-dec-flip, stop-guiding-when-slewing | `AssumeOrthogonal`, `CalFlipRequiresDecFlip`, `StopGuidingWhenSlewing` | `get/set_mount_options` |
| 14 | **P2** | Star | **Tolerate jumps (+threshold)**, fast recenter, auto-select downsample | `/guider/onestar/TolerateJumps*`, `/guider/FastRecenter`, `AutoSelDownsample` | extend `get/set_star_detection` |
| 15 | **P2** | Global | **Auto-exposure** min/max/target-SNR | `/auto_exp/exposure_min`, `exposure_max`, `target_snr` | `get/set_auto_exposure` |
| 16 | **P2** | Global | **Noise-reduction method** | `/NoiseReductionMethod` | `get/set_noise_reduction` |
| 17 | **P3** | Global | Time-lapse delay, beep-for-lost-star, sticky-lock | `/frame/timeLapse`, `/BeepForLostStar`, `/StickyLockPosition` | `get/set_misc_options` |
| 18 | **P3** | Rotator | Reverse direction | `/rotator/isReversed` | `get/set_rotator_reversed` |
| 19 | **P3** | AO | StepGuider bump %, max-steps, settling boost, samples-to-average, cal steps | `/stepguider/Bump*`, `SamplesToAverage`, `CalibrationStepsPerIteration` | `get/set_ao_options` (only if AO is in scope) |

---

## Recommended Phase-5 batches

1. **Batch A — guiding control (P1):** algorithm selection (#1), max RA/Dec (#2), dec-comp (#3),
   dither defaults (#7). These are the live knobs that change guiding behaviour during a session.
2. **Batch B — star + camera (P1/P2):** star detection (#4/#5/#6/#14), camera gain/subframes/
   timeout (#8/#9). Lets ARA tune acquisition without the GUI.
3. **Batch C — remaining (P2/P3):** cooler setpoint, saturation, backlash, mount flags,
   auto-exposure, noise reduction, rotator, AO.

## Design notes / open questions
- **Live vs profile-time:** `set_profile_setup` is applied while *disconnected* (wizard-style).
  ARA needs several of these (max RA/Dec, algorithm, dither, star thresholds) to change **while
  connected/guiding**, so they want dedicated live setters even where a profile_setup field looks
  similar.
- **Granularity:** prefer a few **grouped** methods (`set_star_detection`, `set_mount_options`,
  `set_dither_settings`) over ~25 one-field methods — fewer round-trips, matches how the Brain
  tabs group them, and keeps the dispatch table manageable. Each takes only the fields present.
- **Validation + events:** setters should clamp/validate (e.g. SNR > 0, durations ≥ 0) and, where
  a change affects a running session, emit the existing config-change event so NINA/ARA stay in
  sync.
- **Back-compat:** add **new** methods rather than changing the signature/response of existing
  ones (NINA and ARA already depend on the current shapes). This is why #10 proposes a separate
  `set_cooler_setpoint` instead of overloading `set_cooler_state`.
- **AO scope:** StepGuider/AO settings (#19) are only worth doing if an AO device is actually
  targeted; defer unless ARA needs it.

See `design/GUIDER_TODO.md` (Phase 5) and append each implemented method to
`design/API_CONTRACT.md`.
