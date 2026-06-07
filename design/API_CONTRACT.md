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
- `get_algos` → result: array of selectable guide-algorithm names (untranslated).
- `get_algo` { `axis`: "ra"|"dec" } → result: the axis's selected algorithm name.
- `set_algo` { `axis`, `name` } → result: 0. Switches the axis's guide algorithm (`name` from
  `get_algos`; "None" selects the no-op/identity algorithm). Invalid name → error.
- `get_guide_limits` → result: { `MaxRaDuration`, `MaxDecDuration` } (ms).
- `set_guide_limits` { `MaxRaDuration`?, `MaxDecDuration`? } → result: 0. Either field optional.
- `get_dec_comp` → result: bool (declination RA-rate compensation enabled).
- `set_dec_comp` { `enabled`: bool } → result: 0.
- `get_dither_settings` → result: { `ScaleFactor`, `RaOnly` }.
- `set_dither_settings` { `ScaleFactor`?, `RaOnly`? } → result: 0. Either field optional.

All on the shared dispatch (served over both `:4400` and `/api/rpc`). ARA call sites: the
guiding-settings panel (algorithm pickers, RA/Dec aggression limits, dec-comp toggle, dither
defaults).

### Still to do (later batches — see `API_GAP_AUDIT.md`)
Star-detection thresholds, camera gain/subframes/timeout/cooler-setpoint, backlash comp, mount
flags, auto-exposure, noise reduction, rotator reverse. Plus the original known gap:
**dark/bad-pixel-map library management** (`build_dark_library` / `build_defect_map_darks` /
`set_dark_library_enabled` / `set_defect_map_enabled` now exist; create/select/delete coverage
to be confirmed against ARA's needs).
