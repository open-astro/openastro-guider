# PHD2 JSON-RPC API Reference

This document describes the JSON-RPC API implemented in `src/event_server.cpp`.
It is intended for automation clients and headless integrations.

## Scope And Source Of Truth

- Source of truth: the dispatch table and handlers in `src/event_server.cpp`.
- This document covers:
  - all currently implemented RPC methods
  - parameter shapes, aliases, and validation behavior
  - runtime preconditions/state constraints
  - asynchronous event stream behavior
  - the HTTP data endpoints (see "HTTP Data Endpoints")

## Transport

- Protocol: JSON-RPC 2.0 style objects over TCP
- Framing: one JSON object per line
- Responses are `\r\n` terminated
- Events and method responses share the same socket stream
- Default event server port for instance 1: `4400`

## Request/Response Basics

Request:

```json
{"jsonrpc":"2.0","method":"get_app_state","id":1}
```

Success:

```json
{"jsonrpc":"2.0","result":"Guiding","id":1}
```

Error:

```json
{"jsonrpc":"2.0","error":{"code":-32602,"message":"expected camera param"},"id":1}
```

## Params Rules

Most handlers accept both params styles:

- object form: `{"params":{"name":value}}`
- positional array form: `{"params":[value0,value1,...]}`

For positional arrays, names map in declared handler order. Example:

- `set_lock_position` declares `(x, y, exact)`, so `[120, 240, true]` maps to those fields.

`params` must be an array or an object, or be left out. Anything else (a number, a string, a bool, or `null`) is refused before the method runs with `-32602` "invalid params - params must be an array or object", for example `{"method":"set_exposure","params":2000,"id":42}` (instead of `[2000]`) gets `{"jsonrpc":"2.0","error":{"code":-32602,"message":"invalid params - params must be an array or object"},"id":42}`. JSON-RPC 2.0 would also allow `-32600` here (upstream PHD2 used it); this fork uses `-32602` so a client can tell bad params from a malformed request.

## Error Codes

Standard JSON-RPC codes:

- `-32700` parse error (including a line that is not a JSON object or array)
- `-32600` invalid request (no string `method`, a batch element that is not an object, an empty batch `[]`, a batch of more than 100 calls)
- `-32601` method not found
- `-32602` invalid params (including `params` that is not an array or object)
- `-32603` internal error (including a request line too long to read)

An error reply carries the request's `id`, including the errors raised before the method runs (missing method, bad `params`), so a client that matches replies by `id` sees it. The `id` is `null` only when none can be read: a parse error, a batch element that is not an object, an empty or oversized batch, a line too long to read, or a request sent without an `id`. A request without an `id` (a notification) gets no reply once its method runs, but one refused before that is still answered, with `"id":null`.

Application/runtime errors:

- usually `code: 1` for runtime/state/device failures
- a few imaging methods also use app codes `2`/`3` for specific failures

## Global Runtime Contracts

- Selection/config changes are rejected while equipment is connected:
  - `set_selected_*` methods
  - `set_indi_server`
  - `set_alpaca_server`
  - `set_selected_alpaca_device`
  - `set_profile_setup`
- `get_connected` is strict: true means all selected active components are connected.
- If `id` is omitted (notification), the server does not send a response.

## Method Groups

### Session, Profiles, Connection

- `get_profiles`
  - result: `[{"id":int,"name":string,"selected"?:true}]`

- `get_profile`
  - result: `{"id":int,"name":string}`

- `set_profile`
  - params: `{"id":int}`

- `set_profile_by_name`
  - params: `{"name":string}`

- `create_profile`
  - params:
    - required: `name`
    - optional clone source: `copy_from` or `copy_from_id` (mutually exclusive)
    - optional `select` (bool, default true)
  - result: `{"id":int,"name":string,"selected":bool}`

- `clone_profile`
  - params:
    - source: `source_id` or `source_name`
    - destination: `dest_name`
    - optional `select` (bool, default true)
  - result: `{"id":int,"name":string,"selected":bool}`

- `rename_profile`
  - params: target selector (`id` or `name`) + `new_name`
  - precondition: no equipment connected
  - result: `{"id":int,"name":string,"selected":bool}`

- `delete_profile`
  - params:
    - target selector: `id` or `name`
    - optional `delete_dark_files` (bool, default true)
  - precondition: no equipment connected
  - result: `{"deleted_id":int,"deleted_name":string,"current_profile":{"id":int,"name":string}}`
  - deleting the current profile makes another profile current and loads its camera, mount and rotator selections
    and settings, as `set_profile` does

- `get_profile_setup`
  - result fields:
    - `focal_length`
    - `pixel_size`
    - `camera_binning`
    - `software_binning`
    - `guide_speed`
    - `calibration_duration`
    - `calibration_distance`
    - `high_res_encoders`
    - `auto_restore_calibration`
    - `multistar_enabled`
    - `mass_change_threshold_enabled`

- `set_profile_setup`
  - params: object containing any subset of writable setup fields above
  - precondition: no equipment connected
  - validation:
    - `focal_length`: `0..50000`
    - `pixel_size`: `(0,100]`
    - `camera_binning`: `1..64`
    - `software_binning`: `1..MAX_SOFTWARE_BINNING`
    - `guide_speed`: `(0,10]`
    - `calibration_duration`: `50..60000`
    - `calibration_distance`: `5..200`
  - result: full setup object (same shape as `get_profile_setup`)
  - every field applies at once, with no restart: the next connect and the next calibration use the new values
    (before #117, `calibration_duration`, `calibration_distance`, `high_res_encoders`, `pixel_size`,
    `camera_binning` and `software_binning` reached the selected mount and camera only after a guider restart or
    a profile or device change). What the equipment and calibration may still change afterwards:
    - at connect, a camera that reports its pixel size replaces `pixel_size` with it (and saves it in the profile),
      a hardware binning above the camera's maximum is lowered to it, and a binning the camera cannot offer at all is
      reset to 1, with `calibration_duration` divided by the old binning
    - at the start of a calibration (PHD2 behaviour), when the binning differs from the one the saved calibration
      was made with (1 when there is none), `calibration_distance` is recomputed for the new binning; with a saved
      calibration, `calibration_duration` is recomputed too, and also when the mount's guide speed differs by more
      than 5% from the saved calibration's

- `get_connected`
  - result: bool

- `set_connected`
  - params: `{"connected":true|false}`
  - a failed connect's error text ends with the reason the driver got, when it has one:
    `"mount failed to connect (<reason>)"`, `"equipment failed to connect: mount (<reason>)"` (reasons are worded as
    in the capture failure reasons below, e.g. `"The mount did not reply (Alpaca 1280)"`,
    `"no response from the Alpaca server (Couldn't connect to server)"`)
  - connect recovery (#104, headless daemon only; the desktop GUI keeps PHD2's behaviour): a selected camera, mount
    or rotator the guider was asked to connect (`--auto-connect` at startup, `set_connected` true, `guide`, or the
    camera auto-reconnect ending in `EquipmentReconnectFailed`) that is still not connected is retried in the
    background 15 s, 30 s and 60 s after each failure, then every 60 s, with no limit. An attempt first asks the
    Alpaca server on a thread of its own, with 1.5 s connect / 3 s request timeouts: `GET <type>/<n>/connected`
    and, if false, `PUT Connected=true` (10 s, since a server that connects synchronously answers it only once the
    device is open), then up to 5 s more for the device to read connected. Only when the server reports the device
    connected does the guider connect it, the same way `set_connected` does (including the calibration auto-load),
    so a failing attempt never holds up the JSON-RPC or event servers. When it connects, `EquipmentReconnected` is
    sent (below); no exposure starts. Failed attempts send no `Alert`, not even when the driver fails after the
    server reported the device connected (that failure goes to the debug log, and its reason to the next error
    text); a connect a client asks for keeps its `Alert`.
  - an attempt waits while a dark-library or defect-map build runs, a `set_pa_session` lease is held, or the
    Guiding Assistant runs, and under the same rules as `set_connected` (the camera not while capture is active;
    the mount and rotator not while calibrating or guiding). It runs once the gate opens.
  - `set_connected` false (for that device or for all) stops the retries for the devices it covers, even one that
    was not connected: the guider never connects a device the user disconnected behind their back. A refused
    disconnect changes nothing. Selecting another device or another profile forgets the state, and the device is then
    connected only when a client asks again (`set_connected`, `guide`); choosing the same device again keeps it. A
    device that failed because the Alpaca server is not configured is not retried until `set_alpaca_server` changes
    the server settings. An attempt already asking the server when the user disconnects the device, selects
    another one or changes the Alpaca server is stopped before its `PUT Connected=true` if that has not been sent
    yet, and its result is dropped.

- `shutdown`
  - requests app termination

### Guiding Lifecycle

- `get_app_state`
  - result enum: `Stopped|Selected|Calibrating|Guiding|LostLock|Paused|Looping|Unknown`

- `loop`
  - start looping exposures

- `stop_capture`
  - stop capture/guiding loop

- `guide`
  - params:
    - required `settle` object: `pixels`, `time`, `timeout`
    - optional `recalibrate` (bool)
    - optional `roi` as `[x,y,width,height]`
  - notes:
    - if `/server/guide_allow_recalibrate` is false, requested recalibration is ignored
    - headless daemon (#104): a selected camera or mount that is not connected is connected first, with the same
      probe of the Alpaca server as the background retry (above, under `set_connected`; it waits at most 2 s, not
      5 s, for the device to read connected after its `PUT`). The probe runs on the main thread, so the daemon
      answers no other RPC and sends no event until `guide` returns. The camera and the mount therefore share one
      budget of about 4 s, which every request gets what is left of (so a stalled `PUT Connected=true` gets the
      rest, not 10 s): the call takes as long as the server takes to refuse or finish `Connected=true` (the test
      server simulates AlpacaBridge timing out on a powered-off mount in about 2.3 s), about 1.5 s per device for
      a server that is not reachable (about 3 s for a camera and a mount on the same server), and at worst about
      4 s. The budget stays below a 5 s per-call or ping timeout such as Ara's (the #100 note in
      `design/API_CONTRACT.md`). A device the server reports connected is then connected through its driver (a
      `GET connected` and the property reads, normally well under a second), which the budget does not bound, so
      that starts only with at least 1 s of the budget left; with less, the device is refused as `"...: the mount
      is still connecting"` and the background retry connects it at once (only a driver that stalls after the
      server answered can take `guide` past the budget). A device whose request was sent but had not connected
      when the budget ran out is refused (`"...: the mount failed to connect: no response from the Alpaca server
      (Timeout was reached)"`) and left to the background retry, which keeps its longer timeouts on its own
      thread. A device the budget had run out for before any request for it was sent (the camera used it up, or
      the server read it not connected with no time left for the `PUT`) is refused as `"...: the mount was not
      tried in time"`; that counts as no attempt, so its retry schedule and last reason stay as they were (a
      device that was not being retried yet is tried by the background retry at once). When a
      background attempt for the device is already asking the server, `guide` sends no second request: it waits
      for that attempt's answer within the same budget and connects the device if the server reports it connected;
      if the attempt has not ended by then, `guide` is refused with `"...: the mount is still connecting"` and the
      attempt finishes on its own (`EquipmentReconnected` follows when it connects). `guide` is refused only
      when the connect fails, with error code `1` and the old text as a prefix, one clause per device joined by
      `"; "`:
      `"all equipment must be connected first: the mount failed to connect: <reason>"`;
      `"...: the mount was disconnected by the user"` for a device `set_connected` false disconnected (it is not
      connected by `guide`); `"...: the camera is not connected: <refusal>"` when the connect itself is refused
      (for example while capture is active). A device that had failed to connect (see `EquipmentReconnected`) sends
      `EquipmentReconnected` before the calibration or guiding events; a device connected for the first time (never
      tried since the daemon started without `--auto-connect`, or forgotten by another selection) sends none. A
      rotator does not gate `guide`.

- `dither`
  - params:
    - required `amount` (number)
    - optional `raOnly` (bool, default false)
    - required `settle` object: `pixels`, `time`, `timeout`

- `get_settling`
  - result: bool

- `get_paused`
  - result: bool

- `set_paused`
  - params:
    - `paused` (bool/int)
    - optional `type`: `"full"` for full pause, otherwise guiding pause

### Exposure And Image Flow

- `get_exposure`
  - result: requested exposure duration (ms)

- `set_exposure`
  - params: `{"exposure":int_ms}`

- `get_exposure_durations`
  - result: supported exposure list (ms)

- `capture_single_frame`
  - params:
    - optional `exposure` (`1..600000` ms)
    - optional `binning` (must be valid for camera)
    - optional `gain` (`0..100`)
    - optional `subframe` as `[x,y,width,height]`
    - optional `path` (absolute path only; must not already exist)
    - optional `save` (bool)
  - rules:
    - rejected if capture is active
    - requires connected camera
    - if `save=false`, `path` is not allowed
  - result: `0` when exposure has been started
  - completion is signaled asynchronously by `SingleFrameComplete` (payload includes `Path` and `Filename` when `save=true`)
  - the saved file can be fetched remotely via `GET /api/capture/<Filename>` on the embedded HTTP server
  - auto-named files (no `path`) are temporary: the daemon keeps only the newest 10 `save_image_XXXXXX` frames per directory, for at most 15 minutes, and prunes the rest on each new save and at startup — fetch the frame when `SingleFrameComplete` arrives. Files saved to a caller-supplied `path` are never pruned.

- `save_image`
  - saves current guider image to temp file
  - result: `{"filename":"..."}`
  - the file is temporary and pruned like `capture_single_frame`'s auto-named frames (newest 10, at most 15 minutes) — read it right away

- `get_star_image`
  - params: optional `size` (int, minimum `15`)
  - requires a selected star and valid current image
  - result:
    - `frame`
    - `width`
    - `height`
    - `star_pos` `[x,y]` within returned crop
    - `pixels` base64-encoded 16-bit pixel bytes

- `find_star`
  - params: optional `roi` `[x,y,width,height]`
  - result: lock position point on success

- `deselect_star`
  - clears selected lock position

- `get_lock_position`
  - result: `[x,y]` or `null`

- `set_lock_position`
  - params: `x`, `y`, optional `exact` (default true)
  - if `exact=false`, lock point snaps using star-at-position logic

- `get_use_subframes`
  - result: bool

- `get_search_region`
  - result: search region radius/setting used by guider

- `get_camera_binning`
  - requires connected camera
  - result: int

- `get_camera_frame_size`
  - requires connected camera
  - result: `[width,height]`

### Equipment Inventory, Selection, INDI, Alpaca

- `get_current_equipment`
  - result object may include: `camera`, `mount`, `aux_mount`, `AO`, `rotator`
  - each present device field has `{ "name": string, "connected": bool }`

- `get_equipment_choices`
  - result:
    - `camera`: string[]
    - `mount`: string[]
    - `aux_mount`: string[]
    - `AO`: string[]
    - `rotator`: string[]

Selection getters:

- `get_selected_mount`
- `get_selected_indi_mount_driver`
- `get_selected_camera`
- `get_selected_camera_id`
- `get_selected_indi_camera_driver`
- `get_camera_bitdepth`
- `get_selected_aux_mount`
- `get_selected_ao`
- `get_selected_rotator`

Selection setters (all blocked while equipment is connected):

- `set_selected_mount`
  - params: `{"mount":"<choice>"}`

- `set_selected_indi_mount_driver`
  - params aliases: `mount_driver` or `mount` or `driver`

- `set_selected_camera`
  - params: `{"camera":"<choice>"}`

- `set_selected_camera_id`
  - params: `{"camera_id":"<driver-camera-id>"}`

- `set_selected_indi_camera_driver`
  - params aliases: `camera_driver` or `camera` or `driver`

- `set_camera_bitdepth`
  - params: `{"bitdepth":int}`
  - range: `0..32`

- `set_selected_aux_mount`
  - params: `{"aux_mount":"<choice>"}`

- `set_selected_ao`
  - params: `{"ao":"<choice>"}`

- `set_selected_rotator`
  - params: `{"rotator":"<choice>"}`

INDI server config (blocked while equipment is connected):

- `get_indi_server`
  - result: `{"host":string,"port":int}`

- `set_indi_server`
  - params: any of `host` and/or `port`
  - host must be non-empty string, port range `1..65535`

Alpaca server config (blocked while equipment is connected):

- `get_alpaca_server`
  - result:
    - `host`
    - `port`
    - `camera_device`
    - `telescope_device`
    - `rotator_device`

- `set_alpaca_server`
  - params: any subset of:
    - `host`, `port`
    - `camera_device`, `telescope_device`, `rotator_device`
  - port range `1..65535`
  - device ids must be `>= 0`
  - when matching Alpaca devices are currently selected, menu selections are auto-updated
  - headless daemon (#104): a device still to be connected (one that failed and is being retried, or one that failed
    because the server was not configured) stays to be connected across that re-selection and is tried again at
    once, so `set_alpaca_server` alone connects a device `--auto-connect` could not connect; `EquipmentReconnected`
    follows when it connects. A device the user disconnected stays disconnected.

Alpaca discovery/query:

- `discover_alpaca_servers`
  - optional params:
    - `num_queries` or alias `queries` (`1..20`, default `2`)
    - `timeout_seconds` or alias `timeout` (`1..30`, default `2`)
  - result: array like `["192.168.1.154:6800"]`
  - returns app error if build lacks Alpaca support

- `query_alpaca_devices`
  - params:
    - optional `host` and `port` (defaults from profile)
    - optional `device_type` or alias `type`:
      - `all|camera|telescope|mount|rotator`
  - result: array of objects:
    - `device_number`
    - `device_type`
    - `device_name`
    - `display_name`
    - `display` (`"Device <n>: <name>"`)

- `get_alpaca_camera_pixelsize`
  - params:
    - optional `host`, `port`
    - optional camera selector: `device_number` or alias `device` (`>= 0`)
  - result: `{"host":string,"port":int,"device_number":int,"pixel_size":number}`
  - notes:
    - reads `PixelSizeX`, falls back to `PixelSizeY`
    - returns app error if Alpaca support is not compiled in

- `get_selected_camera_pixelsize`
  - params: none
  - result: `{"camera":string,"pixel_size":number,"source":string}`
  - notes:
    - uses selected camera/ID, connects driver if needed, and queries driver-reported pixel size
    - `source` values include `driver`, `connected_camera_device`, `connected_camera_profile`, or `profile`

- `set_selected_alpaca_device`
  - params:
    - device type via `device_type` or alias `type`:
      - `camera|telescope|mount|rotator`
    - device number via either:
      - `device_number` or alias `device` (int >= 0)
      - `display` in form `"Device <n>: ..."`
  - updates per-type Alpaca device id and applies matching current selection choice when possible

### Guiding Control, Calibration, Algorithms

- `get_calibrated`
  - result: bool

- `clear_calibration`
  - params: optional `which` = `mount|ao|both` (default both)

- `flip_calibration`
  - flips calibration data for pier-side style inversion

- `restore_calibration`
  - puts back the last calibration saved in the profile (for the mount, and for the AO when one is used), after a `guide` with `recalibrate: true` erased it and the calibration failed
  - after `CalibrationFailed` the guider keeps looping and the `guide` request stays open, waiting for the calibration, so another `guide` is refused as in progress; `restore_calibration` ends that request before it restores: the request's `SettleDone` arrives with `Status: 1`, `Error: "calibration failed; saved calibration restored"` and `TotalFrames`/`DroppedFrames` `0` (it never settled), and the sticky-lock setting the calibration forced on is put back to the profile's. So `CalibrationFailed` → `restore_calibration` → `guide` works without `stop_capture`
  - result `true`: restored, and every mount is calibrated
  - result `false`: either some mount has no saved calibration in the profile, and nothing was changed (no partial AO restore, and a waiting `guide` request is not ended; `stop_capture` ends it); or the calibration was restored (and a waiting request ended) but is not usable with the current Dec guide mode (an RA-only calibration while the Dec guide mode is `Auto`, `North` or `South`), which `get_calibrated` also reports as `false`
  - error `1` (nothing changed): calibrating or guiding; the mount or AO not connected; a guide or dither request in progress that is not waiting on a calibration

- `guide_pulse`
  - params:
    - `amount` (int ms; negative amount flips direction)
    - `direction` (`n|s|e|w|north|south|east|west|up|down|left|right`)
    - optional `which` (`mount|ao`, default mount)
  - restrictions:
    - target device must be connected
    - rejected while calibrating/guiding or if device busy

- `get_calibration_data`
  - params: optional `which` (`mount|ao`, default mount)
  - requires selected device connected
  - result:
    - always `calibrated`
    - when calibrated also includes `xAngle`, `xRate`, `xParity`, `yAngle`, `yRate`, `yParity`, `declination`

- `get_guide_output_enabled`
- `set_guide_output_enabled`
  - setter params: `{"enabled":bool}`

- `get_algo_param_names`
  - params: `{"axis":"ra|dec|x|y"}`
  - includes `algorithmName`

- `get_algo_param`
  - params: `{"axis":"...","name":"..."}`

- `set_algo_param`
  - params: `{"axis":"...","name":"...","value":number}`

- `get_dec_guide_mode`
- `set_dec_guide_mode`
  - setter params: `{"mode":"<valid dec mode string>"}`

- `get_pixel_scale`
  - result: number or `null` if unknown

### Lock Shift, Variable Delay, Frame Limit

- `get_lock_shift_enabled`
- `set_lock_shift_enabled`
  - setter params: `{"enabled":bool}`

- `get_lock_shift_params`
  - default result uses mount coordinates (`RA/Dec`) and units (`arcsec/hr`)
  - if params request camera axes (`{"axes":"x/y"}` or `{"axes":"camera"}`), result is converted to `X/Y` and `pixels/hr`

- `set_lock_shift_params`
  - params:
    - `rate`: `[x,y]`
    - `units`: `arcsec/hr|arc-sec/hr|pixels/hr`
    - `axes`: `RA/Dec|X/Y`

- `get_variable_delay_settings`
  - result: `{"Enabled":bool,"ShortDelaySeconds":number,"LongDelaySeconds":number}`

- `set_variable_delay_settings`
  - params (required): `Enabled`, `ShortDelaySeconds`, `LongDelaySeconds`

- `get_limit_frame`
  - result: `{"roi":null}` or `{"roi":[x,y,width,height]}`

- `set_limit_frame`
  - params: required `roi`
    - `roi = null` clears frame limit
    - `roi = [x,y,width,height]` sets frame limit
  - requires camera support for frame limiting

### Calibration Files, Darks, Defect Map, Cooler, Export

- `get_calibration_files_status`
  - result fields include:
    - `profile_id`
    - `dark_library_path`, `defect_map_path`
    - `dark_library_exists`, `defect_map_exists`
    - `dark_library_compatible`, `defect_map_compatible`
    - `dark_library_loaded`, `defect_map_loaded`
    - `auto_load_darks`, `auto_load_defect_map`
    - if camera present: `dark_count_loaded`, `dark_min_exposure_seconds_loaded`, `dark_max_exposure_seconds_loaded`

- `set_dark_library_enabled`
  - params: `{"enabled":bool}`
  - enabling requires connected camera
  - result: same object as `get_calibration_files_status`

- `set_defect_map_enabled`
  - params: `{"enabled":bool}`
  - enabling requires connected camera
  - result: same object as `get_calibration_files_status`

- `build_dark_library`
  - params:
    - `frame_count` (`1..50`, default `5`)
    - `min_exposure_ms` (`1..600000`, optional)
    - `max_exposure_ms` (`1..600000`, optional)
    - `clear_existing` (bool, default false)
    - `notes` (string, optional)
    - `load_after` (bool, default true)
  - requires connected camera and no active capture
  - result includes:
    - `profile_id`
    - `dark_library_path`
    - `frame_count`
    - `exposure_count`
    - `exposures_ms`

- `build_defect_map_darks`
  - params:
    - `exposure_ms` (`1..600000`, default `3000`)
    - `frame_count` (`1..50`, default `10`)
    - `notes` (string, optional)
    - `load_after` (bool, default true)
  - requires connected camera and no active capture
  - result includes:
    - `profile_id`
    - `defect_map_path`
    - `defect_count`
    - `exposure_ms`
    - `frame_count`

- `delete_calibration_files`
  - params (optional booleans):
    - `delete_dark_library` (default true)
    - `delete_defect_map` (default true)
  - result: same object as `get_calibration_files_status`

- `get_cooler_status`
  - requires connected camera
  - result always includes `coolerOn`, `temperature`
  - if cooler on, also includes `setpoint`, `power`

- `set_cooler_state`
  - params: `{"enabled":bool}`
  - requires connected camera with cooler support
  - when enabling, configured profile setpoint is applied

- `get_ccd_temperature`
  - requires connected camera
  - result: `{"temperature":number}`

- `export_config_settings`
  - exports configuration to default data directory as `phd2_settings.txt`
  - result: `{"filename":"..."}`

## Async Events

All events include base fields:

- `Event`
- `Timestamp`
- `Host`
- `Inst`

Event names emitted:

- `Version`
- `AppState`
- `StartCalibration`
- `Calibrating`
- `CalibrationFailed`
- `CalibrationComplete`
- `CalibrationDataFlipped`
- `LoopingExposures`
- `LoopingExposuresStopped`
- `SingleFrameComplete`
- `StarSelected`
- `StarLost`
- `StartGuiding`
- `GuidingStopped`
- `Paused`
- `Resumed`
- `GuideStep`
- `GuidingDithered`
- `LockPositionSet`
- `LockPositionLost`
- `LockPositionShiftLimitReached`
- `SettleBegin`
- `Settling`
- `SettleDone`
- `Alert`
- `GuideParamChange`
- `ConfigurationChange`
- `DarkLibraryBuildStarted` / `DarkLibraryFrameComplete` / `DarkLibraryBuildComplete` / `DarkLibraryBuildFailed`
- `DefectMapBuildStarted` / `DefectMapFrameComplete` / `DefectMapBuildComplete` / `DefectMapBuildFailed`
- `EquipmentDisconnected` / `EquipmentReconnected` / `EquipmentReconnectFailed`

Payload notes for downstream clients:

- `Alert` — `{ Msg: string, Type: "info"|"question"|"warning"|"error" }`. `Alert` is a human-readable surface;
  clients that need to REACT to a fault should route the structured events below instead of parsing `Msg`.
- `Version` — `{ PHDVersion, PHDSubver, OverlapSupport, MsgVersion, Fork }`. `Fork` is `"openastro-guider"` on this
  daemon (absent on stock PHD2), so a client can detect which server it reached. The synchronous `get_version` RPC
  returns the same information without waiting for the catch-up event.
- Calibration-build events (`DarkLibraryBuild*` / `DefectMapBuild*`):
  - `*BuildStarted` — `{ profile_id, exposure_count, frames_per_exposure, planned_exposures_ms: [int] }`
  - `*FrameComplete` — `{ exposure_index, exposure_count, frame, frame_count, exposure_ms }` (1-based indices;
    one event per captured frame, emitted while the synchronous build RPC is still running)
  - `*BuildComplete` — `{ profile_id, built_count }`. For a **synchronous** build the RPC response carries the full
    result payload (paths, counts); for an **async** build (`async: true`, where the RPC returns `0` immediately)
    this event is the completion signal — the artifact paths are `DarkLibFileName(profile_id)` /
    `DefectMapFileName(profile_id)`, or read them from `get_calibration_files_status`.
  - `DarkLibraryBuildFailed` — `{ error, partial_groups_completed }` (completed exposure GROUPS; each group is
    `frame_count` frames). `DefectMapBuildFailed` — `{ error, partial_frames_completed }` (completed FRAMES of
    the single group). The field names differ **on purpose**: the dark-library build fails between exposure
    *groups*, the defect-map build between *frames* of its single group, so one field name never carries two
    units. Clients keying off "partial progress" should read whichever key matches the event (`DarkLibrary*` →
    `partial_groups_completed`; `DefectMap*` → `partial_frames_completed`).
- `GuideStep` — PHD2's fields, plus `MoveTime` (fork addition, #119): the milliseconds, as an integer, the guider
  spent sending this step's guide pulses (RA, then Dec) and waiting for the mount to report them complete. A mount
  guide step is sent before the next exposure starts, so on a rig without an AO this is the time the correction added
  to the guide cycle; on an Alpaca mount it includes the driver's `IsPulseGuiding` tail after the pulse
  (AlpacaBridge's iOptron driver reports a pulse as running for 500 ms after it ends). Absent for AO steps; on an AO
  rig the scope's bump moves carry it but run beside the exposures. The guide log CSV is unchanged.
- `LoopingExposures` — `{ Frame }`, plus `StarMass`, `SNR`, `HFD` when a star was measured, `ErrorCode` when the
  star was not found and `Status` when there is a reason to show.
- `EquipmentDisconnected` — `{ device_type: "camera", reason: string, reconnecting: bool }`. Emitted (in addition
  to `Alert`) whenever the daemon force-disconnects a device on a fault: the capture timeout, an Alpaca camera the
  server reports as not connected (Alpaca error 1031 from any capture call: `binx`/`biny`, `startexposure`,
  `imageready`, `imagearray` as ImageBytes or JSON), an Alpaca camera that fails 3 image acquisitions in a row
  (Alpaca error 1280 from those calls, counted across capture attempts; a good frame, a connect or disconnect and
  the reconnect itself start a new count), or a memory error. For the two Alpaca triggers `reason` carries the
  server's message: `"The Alpaca server reports the camera as not connected: <message>"` and
  `"The camera failed 3 image acquisitions in a row (Alpaca error 1280: <message>)"` (without `: <message>` when
  the server sent none). A single 1280, and any other capture error, only raises `Alert`. While looping or guiding,
  like every capture error it fails the frame, which stops looping and guiding (`LoopingExposuresStopped`), so 3
  consecutive 1280s are reached when the client restarts after each failure. While calibrating, a 1280 frame is
  taken again (#116, see Capture failure reasons below), so 3 consecutive 1280s are reached within one calibration,
  with no client restart: `EquipmentDisconnected` can then arrive mid-calibration, and once the camera is back the
  calibration carries on. `reconnecting: true` means the daemon WILL ATTEMPT automatic
  reconnection — best-effort, throttled to 3 attempts per minute (a memory error sends `reconnecting: false`). The
  disconnect state machine is fully observable: `reconnecting: true` is always followed by either
  `EquipmentReconnected` (recovered) or `EquipmentReconnectFailed` (the reconnect failed or the throttle gave up —
  terminal). `device_type` is `"camera"` today — a mount fault surfaces as RPC errors, not an auto-disconnect.
- `EquipmentReconnected` — `{ device_type: "camera"|"mount"|"rotator" }`. The recovered twin: automatic
  reconnection succeeded. If the failed frame was part of looping or guiding, exposures resume; a reconnect deferred
  until a dark-library or defect-map build has ended starts no exposure. A client that raised a fault on
  `EquipmentDisconnected` clears it here. On the headless daemon (#104) it is also sent, once, when a device that
  had failed to connect (at startup, on a connect request, or after `EquipmentReconnectFailed`) connects by any
  path: the background retry, `guide`, `set_connected` or the Gear dialog. It then follows the calibration
  auto-load and comes before the response of the RPC that connected it; it can arrive with no
  `EquipmentDisconnected` before it (a mount that was off at startup), or after the terminal
  `EquipmentReconnectFailed`; a recovery by the retry starts no exposure.
- `EquipmentReconnectFailed` — `{ device_type: "camera", attempts: int, reason: string }`. Terminal event: a client
  that keyed off `reconnecting: true` clears the pending state here instead of waiting indefinitely. Sent when the
  reconnect attempt itself fails (`reason: "the camera did not reconnect"`, for example because the server refuses
  `Connected=true`; `attempts` counts the attempts made in the last 60 s, this one included), or when the
  auto-reconnect throttle is exhausted (`reason: "auto-reconnect throttle exhausted (3 attempts in 60 s)"`;
  `attempts` is the number of *completed* prior attempts in the window, i.e. the throttle limit, 3 — the blocked
  attempt is not counted). It ends the automatic reconnection only: on the headless daemon (#104) the camera joins
  the slow background retry (15 s, 30 s, then every 60 s; see `set_connected`), and `EquipmentReconnected` follows
  when it connects, by the retry or by `set_connected`.
- Capture failure reasons (#97). When the guide camera fails a frame, every report of that failure carries the
  camera's own reason, one line of UTF-8 of at most 200 bytes:
  - for an Alpaca error, `"<ErrorMessage> (Alpaca <ErrorNumber>)"`, or `"Alpaca error <ErrorNumber>"` when the
    server sent no message;
  - for an HTTP status other than 200, `"HTTP <status>"`, plus `": <body>"` when the whole body, whitespace
    collapsed, is at most 120 bytes and has no `<` (a longer body or an HTML error page is left out, never cut);
  - for no response, `"no response from the Alpaca server (<curl error>)"`; for a response that is not Alpaca
    JSON, `"invalid response from the Alpaca server"`;
  - otherwise the alert the camera driver raised for the frame, such as the capture-timeout text.

  Where it appears:
  - `SingleFrameComplete` — `{ Success, Error?, Path?, Filename? }`. On a capture failure `Error` is
    `"camera error: <reason>"`, for example `"camera error: Camera not connected (Alpaca 1031)"`, or exactly
    `"camera error -- see debug log"` when the camera gave no reason. This includes a single frame whose camera the
    daemon tried to reconnect (1031) and could not.
  - `SettleDone` of a `guide` or `dither` request that a capture failure ended: `Error` is the same
    `"camera error: <reason>"` text. It used to be `"Stopped capturing"` (failure while looping or selecting a star)
    or `"Guiding stopped"` (failure while calibrating or guiding). A request ended by `stop_capture` still says
    `"Stopped capturing"` / `"Guiding stopped"`. The event order is unchanged from earlier releases:
    - failure while calibrating or guiding: `CalibrationFailed` (when calibrating), `GuidingStopped`, `SettleDone`,
      then `LoopingExposuresStopped` (it can arrive twice);
    - failure while looping or selecting a star: `LoopingExposuresStopped`, `SettleDone`, then a second
      `LoopingExposuresStopped`;
    - while a guide pulse is still in progress, `GuidingStopped` is held until the pulse ends, so it can follow
      `SettleDone` and `LoopingExposuresStopped`.

    Treat `SettleDone` as the end of the request and a `LoopingExposuresStopped` around it as part of the same stop.
  - `CalibrationFailed` of a calibration a capture failure interrupted: `Reason` is the same text instead of
    `"Calibration manually stopped"`.

    A transient failure does not interrupt a calibration at once (#116). While calibrating, a frame that fails with
    Alpaca error 1280 (the driver's image acquisition failed) or with no response because the call timed out
    (`"no response from the Alpaca server (Timeout was reached)"`) is taken again, up to 2 times in a row for the
    same calibration step, before the calibration fails with that frame's reason as above. Each failed frame still
    raises its `Alert`; the retry is written to the debug log and sends no other event, and the mount is not moved
    again for the retried frame. A good frame starts a new count. Since a 3rd 1280 in a row reconnects the camera
    (see `EquipmentDisconnected`), 3 consecutive 1280s during a calibration send `EquipmentDisconnected{reconnecting:
    true}`, and the calibration resumes after `EquipmentReconnected` (or fails after `EquipmentReconnectFailed`).
    The first such reconnect in a run of failed frames also starts a new count; a second one does not, so a camera
    that keeps failing every frame fails the calibration after at most 2 reconnects instead of retrying for ever. Every other failure (1031, any other Alpaca error, an HTTP status, an invalid
    response, another transport failure, the capture timeout), a failure after `stop_capture`, and every failure
    while looping or guiding behave as before.
  - The `build_dark_library` / `build_defect_map_darks` error and `DarkLibraryBuildFailed` /
    `DefectMapBuildFailed` `error`: `"failed to capture dark frame <i>/<n>: <reason>"` (without `: <reason>` when
    there is none).
  - `Alert` `Msg` for a failed `startexposure`, `imageready`, `imagearray` or binning call:
    `"<prefix>: <reason>"`, for example
    `"Alpaca error -- Cannot start exposure: Duration out of range (Alpaca 1025)"` (the old prefix
    `"... with given parameters"` is gone).

  The reason's server text is passed through untranslated; the driver-alert fallback is the daemon's own message
  and can be localised. The reason is cleared whenever an exposure is scheduled or starts, so a frame never reports
  an earlier frame's failure. Looping and guiding with no `guide` / `dither` request in flight still end with
  `LoopingExposuresStopped` / `GuidingStopped`, which carry no reason; the `Alert` does. A failed gain write is
  still logged and does not fail the frame.

### Startup Catch-Up Events

Catch-up events are sent on EVERY new socket connection (not just the first client of a server's lifetime) —
a reconnecting client always receives a fresh `Version` first, so reconnect handling can re-validate the
server identity unconditionally.

When a client connects, server sends catch-up context including:

- `Version`
- current lock position (`LockPositionSet`) if valid
- current selected star (`StarSelected`) if valid
- calibration complete events for calibrated mounts
- one of in-progress state indicators (`StartGuiding`, `StartCalibration`, or `Paused`), when applicable
- final `AppState`

## Example Workflows

### Headless Bootstrap

1. `get_profiles`
2. `set_profile` or `set_profile_by_name`
3. `get_equipment_choices`
4. `set_selected_camera` / `set_selected_mount` / optional aux/AO/rotator
5. optional INDI/Alpaca config (`set_indi_server`, `set_alpaca_server`)
6. `set_connected` true
7. `loop` or `guide`

### Dither Between Exposures

1. call `dither` with settle object
2. watch `Settling`
3. wait for `SettleDone` success before resuming imaging exposure

### Single Frame Capture With Save

1. `capture_single_frame` with `save=true` and absolute `path` (must be inside the daemon's data directory — or the directory configured via the `/server/capture_frame_dir` config entry — because the server transports are unauthenticated). Omitting `path` saves to an auto-generated filename inside that same directory.
2. watch `SingleFrameComplete`
3. inspect `Success`, `Path`, and `Filename` in the event payload
4. a remote client retrieves the FITS bytes with `GET http://<daemon>:<httpport>/api/capture/<Filename>` on the embedded HTTP server (files are served only from the capture directory; traversal attempts return `404`)
5. the auto-named file is pruned once 10 newer frames exist or after 15 minutes, so fetch it promptly; a later fetch may return `404`

## Complete Method Name List

`clear_calibration`, `deselect_star`, `get_exposure`, `set_exposure`, `get_exposure_durations`, `get_profiles`, `get_profile`, `set_profile`, `set_profile_by_name`, `create_profile`, `clone_profile`, `rename_profile`, `delete_profile`, `get_profile_setup`, `set_profile_setup`, `get_connected`, `set_connected`, `get_calibrated`, `get_paused`, `set_paused`, `get_lock_position`, `set_lock_position`, `loop`, `stop_capture`, `guide`, `dither`, `find_star`, `get_pixel_scale`, `get_app_state`, `flip_calibration`, `restore_calibration`, `get_lock_shift_enabled`, `set_lock_shift_enabled`, `get_lock_shift_params`, `set_lock_shift_params`, `save_image`, `get_star_image`, `get_use_subframes`, `get_search_region`, `shutdown`, `get_camera_binning`, `get_camera_frame_size`, `get_current_equipment`, `get_indi_server`, `set_indi_server`, `get_alpaca_server`, `set_alpaca_server`, `discover_alpaca_servers`, `query_alpaca_devices`, `get_alpaca_camera_pixelsize`, `get_selected_camera_pixelsize`, `set_selected_alpaca_device`, `get_equipment_choices`, `get_selected_mount`, `get_selected_indi_mount_driver`, `set_selected_mount`, `set_selected_indi_mount_driver`, `get_selected_camera`, `get_selected_camera_id`, `get_selected_indi_camera_driver`, `set_selected_camera`, `set_selected_camera_id`, `set_selected_indi_camera_driver`, `get_camera_bitdepth`, `set_camera_bitdepth`, `get_selected_aux_mount`, `set_selected_aux_mount`, `get_selected_ao`, `set_selected_ao`, `get_selected_rotator`, `set_selected_rotator`, `get_guide_output_enabled`, `set_guide_output_enabled`, `get_algo_param_names`, `get_algo_param`, `set_algo_param`, `get_dec_guide_mode`, `set_dec_guide_mode`, `get_settling`, `guide_pulse`, `get_calibration_data`, `get_version`, `capture_single_frame`, `get_calibration_files_status`, `set_dark_library_enabled`, `set_defect_map_enabled`, `build_dark_library`, `build_defect_map_darks`, `delete_calibration_files`, `get_cooler_status`, `set_cooler_state`, `get_ccd_temperature`, `export_config_settings`, `get_variable_delay_settings`, `set_variable_delay_settings`, `get_limit_frame`, `set_limit_frame`.

## Method Spec Sheets

This section is normative for integrator behavior. Each method entry describes:

- request contract (parameter names and accepted aliases)
- preconditions and runtime state gates
- result shape
- common failure modes and error code patterns
- observable side effects/events

### A) Profile, Session, Connection

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_profiles` | none | none | array of `{id,name,selected?}` | none expected | none |
| `get_profile` | none | none | `{id,name}` | none expected | none |
| `set_profile` | `id:int` | guider context must exist | `0` | `-32602` bad `id`; `1` profile switch failure | active profile changes |
| `set_profile_by_name` | `name:string` | guider context must exist | `0` | `-32602` bad/missing name; `1` profile switch failure | active profile changes |
| `create_profile` | `name`; optional `copy_from` or `copy_from_id`; optional `select:bool` | none; if `select=true`, guider context required | `{id,name,selected}` | `-32602` invalid/mutually-exclusive source args; `1` creation/select failure | new profile persisted; may become active |
| `clone_profile` | source via `source_id` or `source_name`; `dest_name`; optional `select:bool` | none; if `select=true`, guider context required | `{id,name,selected}` | `-32602` invalid source/destination args; `1` clone/select failure | new profile persisted; may become active |
| `rename_profile` | target via `id` or `name`; `new_name` | no equipment connected | `{id,name,selected}` or `0` if unchanged | `-32602` invalid profile args/name collision; `1` cannot rename while connected or rename failure | profile name updated; title updates when active profile renamed |
| `delete_profile` | target via `id` or `name`; optional `delete_dark_files:bool` | no equipment connected | `{deleted_id,deleted_name,current_profile:{id,name}}` | `-32602` invalid selector/boolean; `1` cannot delete while connected | profile removed; if deleting current profile, the new current profile's gear and settings are loaded |
| `get_profile_setup` | none | none | setup object with 11 fields | none expected | none |
| `set_profile_setup` | object with any subset of setup fields | no equipment connected | full setup object (post-apply) | `-32602` type/range violations; `1` blocked while connected | profile settings flushed; runtime setup reloaded, including the selected mount and camera |
| `get_connected` | none | none | `bool` | none expected | none |
| `set_connected` | `connected:bool` | guider context must exist | `0` | `-32602` invalid boolean; `1` connect/disconnect failure (a connect failure ends with ` (<reason>)` when the driver has one) | device connection state changes; corresponding events may follow. Headless (#104): a failed connect is retried in the background (15/30/60 s, then every 60 s) until it connects; `connected:false` stops that for the devices it covers; `EquipmentReconnected` when a failed device connects |
| `shutdown` | none | none | `0` | none in normal path | app termination requested. Semantics: the daemon exits CLEANLY (exit code 0) — under systemd `Restart=on-failure` this is a deliberate stop and the unit is NOT restarted; use `Restart=always` if a supervisor should bring it back |
| `get_version` | none | none | `{version,phd_version,phd_subver,msg_version,overlap_support,fork}` | none expected | none. Synchronous twin of the `Version` catch-up event (previously implemented but undocumented); `version` is the user-facing FULLVER string and `fork` is `"openastro-guider"` for daemon-vs-stock-PHD2 detection. **Casing:** this RPC *result* is snake_case while the `Version` *event* is PascalCase for the same data — `phd_version`↔`PHDVersion`, `phd_subver`↔`PHDSubver`, `overlap_support`↔`OverlapSupport`, `msg_version`↔`MsgVersion`, `fork`↔`Fork` (the split is deliberate: RPC results follow the snake_case method convention, events follow PHD2's PascalCase event convention — a client reading both must map the keys). **Back-compat:** `fork` is `"openastro-guider"` post-rename; older builds/docs used `"openastro-phd2"`, so clients matching on it should accept both |

### B) Guiding Lifecycle, Star Selection, Dither

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_app_state` | none | none | state string | none expected | none |
| `loop` | none | none | `0` | `1` cannot start looping | transitions toward `Looping` state |
| `stop_capture` | none | none | `0` | none expected | stops looping/guiding capture path |
| `guide` | `settle:{pixels,time,timeout}`; optional `recalibrate:bool`; optional `roi:[x,y,w,h]` | guiding must be possible (`CanGuide`); headless (#104): a selected camera or mount that is not connected is connected first | `0` | `-32602` bad settle/roi/recalibrate type; `1` cannot guide or guide failed; `1` `"all equipment must be connected first: the mount failed to connect: <reason>"` (or `"...: the mount was disconnected by the user"`) when that connect fails | guiding/calibration/settle event flow starts; `EquipmentReconnected` first when a failed device connected |
| `dither` | `amount:number`; optional `raOnly:bool`; `settle:{pixels,time,timeout}` | guiding controller available | `0` | `-32602` missing/invalid params; `1` dither failure | `GuidingDithered` + settling events |
| `get_settling` | none | none | `bool` | none expected | none |
| `get_paused` | none | guider context must exist | `bool` | `1` internal guider error | none |
| `set_paused` | `paused:bool`; optional `type:"full"` | none | `0` | `-32602` invalid param types | pause state changes; `Paused`/`Resumed` events |
| `find_star` | optional `roi:[x,y,w,h]` | guider context must exist | lock point `[x,y]` | `-32602` invalid roi; `1` cannot find star | selected star/lock changes; `StarSelected` possible |
| `deselect_star` | none | guider context must exist | `0` | `1` internal guider error | lock/star reset |
| `get_lock_position` | none | guider context must exist | `[x,y]` or `null` | `1` internal guider error | none |
| `set_lock_position` | `x:number`,`y:number`, optional `exact:bool` | guider context must exist | `0` | `-32602` invalid args; `-32600`/`-32602` invalid request shape; `-32600` method-level if cannot set lock | lock position changes; `LockPositionSet` event |

### C) Exposure, Imaging, Frame Geometry

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_exposure` | none | none | exposure ms (int) | none expected | none |
| `set_exposure` | `exposure:int` | none | `0` | `-32602` invalid/missing exposure; `1` set failure | requested exposure changes |
| `get_exposure_durations` | none | none | int[] (ms) | none expected | none |
| `capture_single_frame` | optional `exposure`,`binning`,`gain`,`subframe`,`path`,`save` | no active capture; connected camera required; `path` must be absolute, in an existing directory inside the data directory (or the `/server/capture_frame_dir` config override); symlinks are resolved before the check | `0` (capture queued) | `-32602` invalid params/path rules; `1` camera/capture state failure; `2` exposure start failure | completion via `SingleFrameComplete` event (a capture failure gives `Success:false`, `Error:"camera error: <reason>"`; see Capture failure reasons) |
| `save_image` | none | guider context; current image must exist | `{filename}` | `2` no image; `3` save failure | writes temporary image file |
| `get_star_image` | optional `size:int>=15` | guider context; selected star and valid image | `{frame,width,height,star_pos,pixels}` | `-32602` invalid size; `2` no star selected | none |
| `get_use_subframes` | none | none | `bool` | none expected | none |
| `get_search_region` | none | guider context | numeric search region | `1` internal guider error | none |
| `get_camera_binning` | none | connected camera | `int` | `1` camera not connected | none |
| `get_camera_frame_size` | none | connected camera | `[width,height]` | `1` camera not connected | none |

### D) Equipment Inventory And Selection

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_current_equipment` | none | none | object with present device blocks (`camera`,`mount`,`aux_mount`,`AO`,`rotator`) | none expected | none |
| `get_equipment_choices` | none | none | object with string arrays for each device class | none expected | none |
| `get_selected_mount` | none | none | string | none expected | none |
| `set_selected_mount` | `mount:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted mount selection; may update INDI mount driver |
| `get_selected_indi_mount_driver` | none | none | string | none expected | none |
| `set_selected_indi_mount_driver` | `mount_driver` (aliases: `mount`,`driver`) | no equipment connected | `0` | `-32602` missing/invalid param; `1` blocked while connected | persisted INDI mount driver; may rewrite selected mount string |
| `get_selected_camera` | none | none | string | none expected | none |
| `set_selected_camera` | `camera:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted camera selection; may update INDI camera driver |
| `get_selected_camera_id` | none | none | camera id string | none expected | none |
| `set_selected_camera_id` | `camera_id:string` | none | `0` | `-32602` missing/invalid param | persisted id for current camera selection key |
| `get_selected_indi_camera_driver` | none | none | string | none expected | none |
| `set_selected_indi_camera_driver` | `camera_driver` (aliases: `camera`,`driver`) | no equipment connected | `0` | `-32602` missing/invalid param; `1` blocked while connected | persisted INDI camera driver; may rewrite selected camera string |
| `get_camera_bitdepth` | none | none | bitdepth int (profile value or camera fallback) | none expected | none |
| `set_camera_bitdepth` | `bitdepth:int` | none | `0` | `-32602` missing/non-int/out of `0..32` | persisted profile bitdepth |
| `get_selected_aux_mount` | none | none | string | none expected | none |
| `set_selected_aux_mount` | `aux_mount:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted aux selection; may update INDI mount driver |
| `get_selected_ao` | none | none | string | none expected | none |
| `set_selected_ao` | `ao:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted AO selection |
| `get_selected_rotator` | none | none | string | none expected | none |
| `set_selected_rotator` | `rotator:string` | no equipment connected | `0` | `-32602` missing/invalid/unknown selection; `1` blocked while connected | persisted rotator selection; may update INDI rotator driver |

### E) INDI And Alpaca Configuration / Discovery

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_indi_server` | none | none | `{host,port}` | none expected | none |
| `set_indi_server` | any of `host:string`,`port:int` | no equipment connected | `0` | `-32602` missing both fields/invalid host/port out of `1..65535`; `1` blocked while connected | persisted INDI host/port |
| `get_alpaca_server` | none | none | `{host,port,camera_device,telescope_device,rotator_device}` | none expected | none |
| `set_alpaca_server` | any subset of `host`,`port`,`camera_device`,`telescope_device`,`rotator_device` | no equipment connected | `0` | `-32602` invalid/missing all fields/port range/device < 0; `1` blocked while connected | persisted Alpaca endpoint/device ids; may auto-adjust selected Alpaca choices; headless (#104): a device still to be connected is tried again at once |
| `discover_alpaca_servers` | optional `num_queries|queries` (`1..20`), `timeout_seconds|timeout` (`1..30`) | Alpaca support compiled in | `string[]` host:port list | `-32602` invalid numeric ranges; `1` Alpaca disabled in build | network discovery queries |
| `query_alpaca_devices` | optional `host`,`port`,`device_type|type` | Alpaca support compiled in | array of `{device_number,device_type,device_name,display_name,display}` | `-32602` invalid args/type enum; `1` server query/response failure or Alpaca disabled | HTTP query to Alpaca management endpoint |
| `get_alpaca_camera_pixelsize` | optional `host`,`port`,`device_number|device` | Alpaca support compiled in | `{host,port,device_number,pixel_size}` | `-32602` invalid host/port/device args; `1` query failure or Alpaca disabled | reads Alpaca camera `pixelsizex`/`pixelsizey` |
| `get_selected_camera_pixelsize` | none | guider context and selected camera required | `{camera,pixel_size,source}` | `1` no camera selected, camera creation/connect/query failure, or driver does not report pixel size | may connect camera driver temporarily and update profile pixel size |
| `set_selected_alpaca_device` | type via `device_type|type`; device via `device_number|device` or parseable `display` | no equipment connected | `0` | `-32602` invalid type/device/display parse; `1` blocked while connected | persists selected Alpaca device number and attempts to sync active selection string |

### F) Guiding Output, Algorithms, Calibration Data

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_guide_output_enabled` | none | mount object should exist | `bool` | `1` mount not defined | none |
| `set_guide_output_enabled` | `enabled:bool` | mount object should exist | `0` | `-32602` invalid boolean; `1` mount not defined | guide output enable state changes |
| `get_algo_param_names` | `axis:string` (`ra|dec|x|y`) | none | string[] incl. `algorithmName` | `1` invalid axis | none. `get_algo_param_names` stays the authoritative per-profile list (names depend on the selected algorithm); as a static reference, the common writable names are — Hysteresis: `hysteresis`,`aggression`,`minMove`; ResistSwitch: `aggression`,`minMove`,`fastSwitch`; LowPass: `minMove`,`slopeWeight`; LowPass2: `aggressiveness`,`minMove`; ZFilter: `expFactor`,`minMove`; GaussianProcess: `minMove` + the GP hyperparameters. Verify against the live list before writing |
| `get_algo_param` | `axis:string`, `name:string` | none | number or string (`algorithmName`) | `1` invalid args or unknown param | none |
| `set_algo_param` | `axis:string`, `name:string`, `value:number` | none | `0` | `1` invalid args or set failure | algorithm parameter updated; graph controls refreshed |
| `get_dec_guide_mode` | none | none | mode string (`Scope::DecGuideModeStr`) | none expected | none |
| `set_dec_guide_mode` | `mode:string` | none | `0` | `1` invalid mode | DEC guide mode updated; graph controls refreshed |
| `get_calibrated` | none | none | `bool` | none expected | none |
| `clear_calibration` | optional `which:string` (`mount|ao|both`) | none | `0` | `-32602` invalid `which` | mount/AO calibration cleared |
| `flip_calibration` | none | none | `0` | `1` flip failure | calibration parity/geometry flipped; `CalibrationDataFlipped` event |
| `restore_calibration` | none | guider context must exist; not calibrating or guiding; mount (and AO, if used) connected; no guide/dither request in progress other than one waiting on a failed calibration | `true` restored and every mount calibrated; `false` some mount has no saved calibration (nothing changed) or the restored calibration is not usable with the current Dec guide mode (`get_calibrated` agrees) | `1` calibrating or guiding, mount/AO not connected, or a guide/dither request in progress | saved calibration loaded on every mount; a `guide` request still waiting on the failed calibration is ended first with `SettleDone` `Status:1`, `Error:"calibration failed; saved calibration restored"`, `TotalFrames:0`, `DroppedFrames:0`, and the sticky-lock setting it forced on for the calibration put back |
| `guide_pulse` | `amount:int`, `direction:string`, optional `which` (`mount|ao`) | target device connected; guider must not be calibrating/guiding; target not busy | `0` | `1` invalid args, invalid `which`, not connected, busy/calibrating/guiding | manual pulse scheduled |
| `get_calibration_data` | optional `which` (`mount|ao`) | selected device must be connected | `{calibrated,...}` | `1` invalid `which` or device not connected | none |

### G) Lock Shift, Delay, ROI Frame Limiting

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_lock_shift_enabled` | none | guider context must exist | `bool` | `1` internal guider error | none |
| `set_lock_shift_enabled` | `enabled:bool` | guider context must exist | `0` | `-32602` invalid boolean | lock-shift enable toggled |
| `get_lock_shift_params` | optional `axes` (`x/y` or `camera` for camera-space projection) | guider context must exist | `{enabled,rate,units,axes}` | `1` internal guider error | none |
| `set_lock_shift_params` | `rate:[x,y]`, `units`, `axes` | guider context must exist | `0` | `-32602` invalid rate/units/axes | lock-shift rate model changed |
| `get_variable_delay_settings` | none | none | `{Enabled,ShortDelaySeconds,LongDelaySeconds}` | none expected | none |
| `set_variable_delay_settings` | required `Enabled`,`ShortDelaySeconds`,`LongDelaySeconds` | none | `0` | `-32602` missing/invalid types | variable delay config changed |
| `get_limit_frame` | none | none | `{roi:null}` or `{roi:[x,y,w,h]}` | none expected | none |
| `set_limit_frame` | required `roi` (`null` or `[x,y,w,h]`) | camera must exist and support frame limiting; guider context required | `0` | `-32602` missing/invalid roi; `1` no camera/unsupported/failure | frame limiting region updated |

### H) Calibration Files, Darks, Defect Map

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_calibration_files_status` | none | none | status object (paths, exists/loaded/compatible/autoload flags, optional loaded dark stats) | none expected | none |
| `set_dark_library_enabled` | `enabled:bool` | enabling requires connected camera | status object | `-32602` invalid bool; `1` camera not connected/load/unload failure | dark library load state toggled |
| `set_defect_map_enabled` | `enabled:bool` | enabling requires connected camera | status object | `-32602` invalid bool; `1` camera not connected/load/unload failure | defect map load state toggled |
| `build_dark_library` | optional `frame_count`,`min_exposure_ms`,`max_exposure_ms`,`clear_existing`,`notes`,`load_after`,`async` | connected camera; capture inactive | sync: `{profile_id,dark_library_path,frame_count,exposure_count,exposures_ms}`; async: `0` | `-32602` invalid ranges/types; `1` no camera/capture active/no matched exposure/capture save failure | captures dark stacks for selected exposures, writes dark library, optional load. Default is SYNCHRONOUS: blocks for roughly `frame_count x sum(matched exposures)` (2–3 min), which can trip a client's per-call receive timeout. Pass `async:true` to return `0` immediately and get the result via the `DarkLibraryBuildComplete`/`DarkLibraryBuildFailed` event instead. Either way, subscribe to `DarkLibraryBuildStarted`/`DarkLibraryFrameComplete`/`DarkLibraryBuildComplete`/`DarkLibraryBuildFailed` for live progress (poll `get_calibration_files_status.build` as the fallback) |
| `build_defect_map_darks` | optional `exposure_ms`,`frame_count`,`notes`,`load_after`,`async` | connected camera; capture inactive | sync: `{profile_id,defect_map_path,defect_count,exposure_ms,frame_count}`; async: `0` | `-32602` invalid ranges/types; `1` no camera/capture active/capture failure | captures master dark, builds defect map, saves files, optional load. Default SYNCHRONOUS like `build_dark_library`; `async:true` returns `0` immediately with the result riding `DefectMapBuildComplete`/`DefectMapBuildFailed`. Emits the `DefectMapBuildStarted`/`DefectMapFrameComplete`/`DefectMapBuildComplete`/`DefectMapBuildFailed` twins |
| `delete_calibration_files` | optional `delete_dark_library:bool`,`delete_defect_map:bool` | none | status object | `-32602` invalid bools | deletes selected files for current profile, clears loaded artifacts where applicable |

### I) Cooler, Temperature, Config Export

| Method | Parameters | Preconditions | Success Result | Common Errors | Side Effects / Events |
|---|---|---|---|---|---|
| `get_cooler_status` | none | connected camera | `{coolerOn,temperature}` plus `{setpoint,power}` when on | `1` camera not connected/status failure | none |
| `set_cooler_state` | `enabled:bool` | connected camera with cooler support | `0` | `-32602` invalid bool; `1` camera not connected/no cooler/set failure | toggles cooler; on enable applies configured cooler setpoint |
| `get_ccd_temperature` | none | connected camera | `{temperature}` | `1` camera not connected/read failure | none |
| `export_config_settings` | none | none | `{filename}` | `1` export failure | writes `phd2_settings.txt` to default file directory |

## Parameter Schemas

### Common Structural Types

- `roi`: `[x:int, y:int, width:int, height:int]`
- `point`: `[x:number, y:number]`
- `settle`:
  - `pixels:number`
  - `time:number` (seconds, floored to int)
  - `timeout:number` (seconds, floored to int)

### `guide` Request Schema

```json
{
  "method": "guide",
  "params": {
    "settle": {"pixels": 0.5, "time": 6, "timeout": 30},
    "recalibrate": false,
    "roi": [100, 120, 800, 600]
  },
  "id": 42
}
```

### `dither` Request Schema

```json
{
  "method": "dither",
  "params": {
    "amount": 10,
    "raOnly": false,
    "settle": {"pixels": 1.5, "time": 8, "timeout": 30}
  },
  "id": 43
}
```

### `set_lock_shift_params` Schema

```json
{
  "method": "set_lock_shift_params",
  "params": {
    "rate": [3.3, 1.1],
    "units": "arcsec/hr",
    "axes": "RA/Dec"
  },
  "id": 44
}
```

## Response Schemas For Frequently Consumed APIs

### `get_current_equipment`

```json
{
  "camera": {"name": "CameraName", "connected": true},
  "mount": {"name": "MountName", "connected": true},
  "aux_mount": {"name": "AuxMountName", "connected": false},
  "AO": {"name": "AOName", "connected": false},
  "rotator": {"name": "RotatorName", "connected": true}
}
```

### `query_alpaca_devices`

```json
[
  {
    "device_number": 0,
    "device_type": "Camera",
    "device_name": "ASI2600MM",
    "display_name": "ASI2600MM",
    "display": "Device 0: ASI2600MM"
  }
]
```

### `get_calibration_files_status`

```json
{
  "profile_id": 3,
  "dark_library_path": "/path/to/darks.fits",
  "defect_map_path": "/path/to/defectmap.fit",
  "dark_library_exists": true,
  "defect_map_exists": true,
  "dark_library_compatible": true,
  "defect_map_compatible": true,
  "dark_library_loaded": true,
  "defect_map_loaded": false,
  "auto_load_darks": true,
  "auto_load_defect_map": true,
  "dark_count_loaded": 8,
  "dark_min_exposure_seconds_loaded": 1.0,
  "dark_max_exposure_seconds_loaded": 5.0
}
```

## HTTP Data Endpoints

When server mode is enabled, the daemon also listens for HTTP on all interfaces (`0.0.0.0`), port `8080 + instance - 1`. That is always the JSON-RPC port plus `3680`, so instance 1 uses `4400` and `8080`, and instance 2 uses `4401` and `8081`. Clients may derive the HTTP port from the JSON-RPC port. If the HTTP port cannot be bound, server mode does not start.

The HTTP port carries image data only. Everything else, including setup, discovery and control, goes over JSON-RPC.

- `GET /api/capture/<filename>`: raw FITS bytes of a file saved by `capture_single_frame` (see "Single Frame Capture With Save"). `<filename>` is the `Filename` field of the `SingleFrameComplete` event, URL-encoded. Files are served only from the capture directory; an empty name, `..`, a leading `/` or `\`, or a symlink that resolves outside the capture directory returns `404`. Response: `200` with `Content-Type: application/fits`.
- `GET /api/frame.jpg`: the current guide frame as a JPEG, display-stretched the same way the guider display stretches it. The response carries an `ETag` taken from the frame number; a request whose `If-None-Match` matches it gets `304 Not Modified` without re-encoding the frame. When there is no frame yet it returns `404` with the body `no image available`.

Every response sets `Connection: close`. Every response except the `304` also sets `Cache-Control: no-store`.

### HTTP Status Codes

- `200 OK`: the file or frame is in the body.
- `304 Not Modified`: `GET /api/frame.jpg` with an `If-None-Match` that matches the current frame.
- `400 Bad Request`: the request could not be parsed, or it was larger than 1 MB.
- `404 Not Found`: any other method or path, a capture file that is missing or outside the capture directory, or no guide frame yet.
- `500 Internal Server Error`: the guide frame could not be encoded as JPEG.

## Error Semantics By Category

- Contract violation (`-32602`): wrong type, missing required field, out-of-range numeric value, invalid enum string.
- Request-shape violation (`-32600`): malformed request object or `params` not array/object.
- Runtime/state violation (`1`): camera/mount not connected, operation blocked by active capture or connected-equipment guard, unsupported build capability.
- Imaging pipeline specific:
  - `2`: no image / capture start issue class
  - `3`: save failure class

## Client Implementation Recommendations

- Treat response `result:0` as operation accepted/completed, depending on method semantics.
- For async operations (`capture_single_frame`, guiding/dither), use events as authoritative completion/state source.
- Read/modify/write profile-affecting settings only while disconnected to avoid state-gate failures.
- Parse device selections case-insensitively on client side but preserve exact strings returned by `get_equipment_choices` for setter calls.
- For Alpaca, prefer numeric `device_number` in `set_selected_alpaca_device` for deterministic behavior.
