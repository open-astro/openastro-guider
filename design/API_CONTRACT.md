# openastro-guider — API contract

Append-only design log for the control API that OpenAstro ARA uses to drive the headless
guider. The transport is PHD2's **event server (JSON-RPC over TCP :4400)** plus its event
stream. One entry per method ARA depends on or that we add during the port (request/response
shape + the ARA call site). Equipment (guide camera + mount) is reached separately via ASCOM
Alpaca through AlpacaBridge.

See `design/PHD2_HEADLESS_PLAYBOOK.md` §4.

---

## Inherited (already in the PHD2 event server)
Covered by upstream; catalogued here as ARA wiring lands. Examples: `guide`, `dither`,
`set_exposure` / `get_exposure`, `get_algo_param_names` / `get_algo_param` / `set_algo_param`,
`set_lock_position`, `flip_calibration`, `get_connected` / `set_connected`, and the event
stream (`GuideStep`, `SettleDone`, `StarLost`, `CalibrationComplete`, …).

## Added for headless (Phase 5 gap-fill)
- _none yet._ Known gap to design first: **dark-frame / bad-pixel-map library management**
  (create / select / delete / status) — currently GUI-only, no RPC equivalent.
