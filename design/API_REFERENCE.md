# openastro-guider — API Reference

Complete reference for the guider's control API: every JSON-RPC method and the event stream.
This is the human-readable companion to `design/API_CONTRACT.md` (the append-only change log).

> Generated from `src/event_server.cpp` (snapshot 2026-06-07). When you add/change a method,
> update this file **and** add a line to `API_CONTRACT.md`.

## Transport

The same method table is served over **two** sockets (both bind `0.0.0.0`, instance 1 ports):

- **TCP `:4400`** — the standard PHD2 *event server*: newline-delimited JSON-RPC requests, plus
  an asynchronous **event stream** pushed to every connected client. This is what **NINA**
  speaks. Port = `4400 + (instance − 1)`.
- **HTTP `:8080`** — an embedded server that (a) serves the static web UI and (b) bridges
  `POST /api/rpc` to the *same* dispatch. This is what **ARA** / browsers use. Port =
  `8080 + (instance − 1)`.

**Request** (JSON-RPC 2.0): `{"method": "<name>", "params": {...}|[...], "id": <n>}`.
Params may be an object (named) or array (positional). Over HTTP, POST that body to `/api/rpc`;
the response is wrapped `{"ok": true, "result": ...}` / `{"ok": false, "error": "..."}`.
Over `:4400` the reply is raw JSON-RPC (`{"jsonrpc":"2.0","result":...,"id":<n>}`).

**Auth:** none — the daemon is intentionally **LAN-trusted, HTTP-only** (see
`design/GUIDER_DECISIONS.md`). Do not expose it to untrusted networks.

---

## Session & state

| Method | Params | Description |
|--------|--------|-------------|
| `get_app_state` | — | Current state string (`Stopped`, `Selected`, `Calibrating`, `Guiding`, `LostLock`, `Paused`, `Looping`). |
| `get_connected` | — | `true` if all selected equipment is connected. |
| `set_connected` | `connected` (bool) | Connect/disconnect the profile's equipment. |
| `loop` | — | Start looping exposures (no guiding). |
| `stop_capture` | — | Stop looping/guiding. |
| `get_paused` | — | `true` if guiding is paused. |
| `set_paused` | `paused` (bool), `type` (`"full"` to also stop looping) | Pause/resume guiding. |
| `capture_single_frame` | `exposure`, `binning`, `gain`, `subframe`, `path`, `save` | Take one frame (optionally saved); emits `SingleFrameComplete`. |
| `get_settling` | — | `true` while a dither/guide settle is in progress. |
| `get_pixel_scale` | — | Image scale in arcsec/pixel. |
| `get_star_image` | `size` | Cropped star image around the lock position (base64). |
| `save_image` | — | Save the current frame to disk; returns the filename. |
| `export_config_settings` | — | Dump the effective config; returns a file path. |
| `shutdown` | — | Shut the daemon down. |

## Guiding actions

| Method | Params | Description |
|--------|--------|-------------|
| `guide` | `settle` (obj), `recalibrate` (bool), `roi` | Select a star (if needed), calibrate if needed, and start guiding; settles per `settle`. |
| `dither` | `amount`, `raOnly` (bool), `settle` (obj) | Dither by `amount` px and settle. |
| `guide_pulse` | `amount` (ms), `direction` (`N/S/E/W`/`Up`/…), `which` (`AO`/`Mount`) | Issue a manual guide pulse. |
| `find_star` | `roi` ([x,y,w,h]) | Auto-select the best star (optionally within ROI); returns its position. |
| `deselect_star` | — | Clear the selected star. |

## Calibration

| Method | Params | Description |
|--------|--------|-------------|
| `get_calibrated` | — | `true` if the mount is calibrated. |
| `get_calibration_data` | `which` (`Mount`/`AO`) | Calibration angles/rates/parity. |
| `clear_calibration` | `which` (`Mount`/`AO`/`Both`) | Clear calibration so the next guide recalibrates. |
| `flip_calibration` | — | Flip calibration for a meridian flip. |
| `get_calibration_files_status` | — | Whether dark-library / defect-map files exist for the profile. |
| `delete_calibration_files` | `delete_dark_library`, `delete_defect_map` (bool) | Delete the selected calibration files. |

## Guiding control (settings)

| Method | Params | Description |
|--------|--------|-------------|
| `get_dec_guide_mode` | — | Dec guide mode (`Off`/`Auto`/`North`/`South`). |
| `set_dec_guide_mode` | `mode` | Set the dec guide mode. |
| `get_algo_param_names` | `axis` (`ra`/`dec`/`x`/`y`) | Parameter names of the axis's current algorithm (incl. `algorithmName`). |
| `get_algo_param` | `axis`, `name` | Value of one algorithm parameter (`name="algorithmName"` returns the algorithm class name). |
| `set_algo_param` | `axis`, `name`, `value` | Set one algorithm parameter value. |
| `get_algos` | `axis`? (`ra`/`dec`) | **(Batch A)** Guide-algorithm names. With no params: all algorithms. With `axis`: only those valid for that axis on the current mount type (RA / Dec / AO differ — e.g. *Predictive PEC* is RA-only). |
| `get_algo` | `axis` | **(Batch A)** The algorithm selected for an axis (e.g. `Hysteresis`). |
| `set_algo` | `axis`, `name` | **(Batch A)** Switch the axis's guide algorithm. `name` must come from `get_algos(axis)`; algorithms not valid for that axis/mount — including `None`/identity, which isn't per-axis selectable — are rejected with *"algorithm not valid for this axis"*. |
| `get_guide_limits` | — | **(Batch A)** `{MaxRaDuration, MaxDecDuration}` in ms. |
| `set_guide_limits` | `MaxRaDuration`, `MaxDecDuration` (ms; either optional) | **(Batch A)** Set the max guide pulse durations. |
| `get_dec_comp` | — | **(Batch A)** Whether declination RA-rate compensation is on. |
| `set_dec_comp` | `enabled` (bool) | **(Batch A)** Enable/disable dec compensation. |
| `get_dither_settings` | — | **(Batch A)** `{ScaleFactor, RaOnly}`. |
| `set_dither_settings` | `ScaleFactor`, `RaOnly` (either optional) | **(Batch A)** Set default dither scale / RA-only. |
| `get_guide_output_enabled` | — | Whether guide output (corrections) is enabled. |
| `set_guide_output_enabled` | `enabled` (bool) | Enable/disable sending guide corrections. |

## Lock position & shift

| Method | Params | Description |
|--------|--------|-------------|
| `get_lock_position` | — | `[x, y]` of the lock position, or null. |
| `set_lock_position` | `x`, `y`, `exact` (bool) | Set the lock position. |
| `get_search_region` | — | Star search-region size (px). |
| `get_lock_shift_enabled` | — | Whether lock-position shift (for comets/spectroscopy) is on. |
| `set_lock_shift_enabled` | `enabled` (bool) | Enable/disable lock-position shift. |
| `get_lock_shift_params` | — | Shift rate/units/axes. |
| `set_lock_shift_params` | `rate`, `units`, `axes` | Set lock-shift parameters. |

## Exposure & frame timing

| Method | Params | Description |
|--------|--------|-------------|
| `get_exposure` | — | Current exposure duration (ms). |
| `set_exposure` | `exposure` (ms) | Set the exposure duration. |
| `get_exposure_durations` | — | The selectable exposure presets (ms). |
| `get_use_subframes` | — | Whether the camera uses subframes. |
| `get_limit_frame` | — | The current capture ROI/limit frame. |
| `set_limit_frame` | `roi` | Set a capture ROI/limit frame. |
| `get_variable_delay_settings` | — | `{Enabled, ShortDelaySeconds, LongDelaySeconds}`. |
| `set_variable_delay_settings` | `Enabled`, `ShortDelaySeconds`, `LongDelaySeconds` | Inter-frame variable delay (seconds). |

## Camera & cooler

| Method | Params | Description |
|--------|--------|-------------|
| `get_camera_binning` | — | Current binning. |
| `get_camera_frame_size` | — | Sensor frame size `[w, h]`. |
| `get_camera_bitdepth` | — | Camera bit depth. |
| `set_camera_bitdepth` | `bitdepth` | Set the camera bit depth. |
| `get_ccd_temperature` | — | Sensor temperature (°C). |
| `get_cooler_status` | — | Cooler on/off, setpoint, power, temperature. |
| `set_cooler_state` | `enabled` (bool) | Turn the cooler on/off. *(No setpoint — see Phase-5 gap audit row #10.)* |

## Profiles

| Method | Params | Description |
|--------|--------|-------------|
| `get_profiles` | — | All equipment profiles `[{id, name}]`. |
| `get_profile` | — | The active profile. |
| `set_profile` | `id` | Select a profile by id (disconnect first). |
| `set_profile_by_name` | `name` | Select a profile by name. |
| `create_profile` | `name`, `copy_from`/`copy_from_id`, `select` | Create a profile (optionally copying another). |
| `clone_profile` | `source_id`/`source_name`, `dest_name`, `select` | Clone an existing profile. |
| `rename_profile` | `id`/`name`, `new_name` | Rename a profile. |
| `delete_profile` | `id`/`name`, `delete_dark_files` | Delete a profile (optionally its dark files). |
| `get_profile_setup` | — | Aggregate profile setup (see fields below). |
| `set_profile_setup` | object of fields below | Set profile setup (applied **while disconnected**). |

`*_profile_setup` fields: `pixel_size`, `focal_length`, `guide_speed`, `camera_binning`,
`software_binning`, `calibration_distance`, `calibration_duration`, `high_res_encoders`,
`multistar_enabled`, `mass_change_threshold_enabled`, `auto_restore_calibration`.

## Equipment selection

| Method | Params | Description |
|--------|--------|-------------|
| `get_current_equipment` | — | The connected devices (name + connected state). |
| `get_equipment_choices` | — | The available device choices for the active profile. |
| `get_selected_camera` / `set_selected_camera` | `camera` | Get/set the selected camera (display name). |
| `get_selected_camera_id` / `set_selected_camera_id` | `camera_id` | Get/set the selected camera id. |
| `get_selected_camera_pixelsize` | — | Pixel size of the selected camera. |
| `get_selected_mount` / `set_selected_mount` | `mount` | Get/set the selected mount. |
| `get_selected_aux_mount` / `set_selected_aux_mount` | — / (choice) | Get/set the aux (pointing) mount. |
| `get_selected_ao` / `set_selected_ao` | — / (choice) | Get/set the adaptive-optics device. |
| `get_selected_rotator` / `set_selected_rotator` | `rotator` | Get/set the rotator. |

## Alpaca equipment

| Method | Params | Description |
|--------|--------|-------------|
| `get_alpaca_server` | — | Configured Alpaca host/port. |
| `set_alpaca_server` | `host`, `port`, `camera_device`, `telescope_device`, `rotator_device` | Set Alpaca server + per-device numbers (disconnect first). |
| `discover_alpaca_servers` | `num_queries`, `timeout_seconds` | Discover Alpaca servers on the LAN (mDNS/broadcast). |
| `query_alpaca_devices` | `host`, `port`, `device_type` (`CAMERA`/`TELESCOPE`/`ROTATOR`/`ALL`) | Enumerate devices on an Alpaca server. |
| `get_alpaca_camera_pixelsize` | `host`, `port`, `device_number` | Pixel size of a specific Alpaca camera. |
| `set_selected_alpaca_device` | `device_type`, `device_number`, `display` | Select a discovered Alpaca device for a slot. |

## Dark / defect-map library

| Method | Params | Description |
|--------|--------|-------------|
| `build_dark_library` | `frame_count`, `min_exposure_ms`, `max_exposure_ms`, `clear_existing`, `notes`, `load_after` | Build the dark-frame library. |
| `build_defect_map_darks` | `exposure_ms`, `frame_count`, `notes`, `load_after` | Capture the darks used to build a bad-pixel (defect) map. |
| `set_dark_library_enabled` | `enabled` (bool) | Enable/disable dark subtraction. |
| `set_defect_map_enabled` | `enabled` (bool) | Enable/disable the bad-pixel map. |

## Deprecated (INDI — removed in Phase 3)

INDI support was removed; these remain only for protocol compatibility and reference no live
backend. Do not use.

`get_indi_server`, `set_indi_server`, `get_selected_indi_camera_driver`,
`set_selected_indi_camera_driver`, `get_selected_indi_mount_driver`,
`set_selected_indi_mount_driver`.

---

## Event stream (`:4400`, pushed asynchronously)

On connect the server sends `Version` then `AppState`, then events as they occur. Each event is
`{"Event":"<name>","Timestamp":...,"Host":...,"Inst":..., ...}`.

`Version`, `AppState`, `LockPositionSet`, `StarSelected`, `StartCalibration`, `Calibrating`,
`CalibrationComplete`, `CalibrationFailed`, `CalibrationDataFlipped`, `StartGuiding`,
`GuideStep`, `GuidingDithered`, `Settling`, `SettleDone`, `StarLost`, `Paused`,
`LoopingExposures`, `SingleFrameComplete`, `ConfigurationChange`, `GuideParamChange`, `Alert`.

(See PHD2's *EventMonitoring* docs for the full per-event field shapes; the fork preserves them.)
