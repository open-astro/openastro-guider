# openastro-guider — TODO log

Append-only list of every `// TODO(guider)` and `// GUIDER_BLOCKED` placeholder left in
code during the strip, grouped by phase. Swept before the relevant phase closes.

---

## Test-expansion follow-ups
- **Star detection: no saturation check in Peak mode.** `Star::Find` / `star_find::FindStar`
  only collect the top-3 raw pixel values (`max3[]`) in Centroid mode, so a Peak-mode call
  never returns `STAR_SATURATED` / `Result::Saturated` — a fully saturated frame comes back
  `Ok`. Pre-existing in upstream PHD2; preserved by the `star_find_core` extraction (PR #24)
  and documented at the `star_find.h` API boundary. Fixing it changes guiding detection
  behaviour (out of scope per rule #1), so it's tracked here rather than changed. Flagged by
  the PR #24 review.

## Phase 0
- _none yet_

## Phase 1 — drop Windows
- **`phase/1b` — strip leftover Windows code paths.** The `phase/1-drop-windows` PR deleted
  the Windows files/scripts/backends and the win build-target CMake, but deferred:
  - inline `#if defined(__WINDOWS__)` / `ASCOM_CAMERA` / `GUIDE_ASCOM` / `ROTATOR_ASCOM`
    blocks in `src/camera.cpp`, `src/scope.cpp`, `src/rotator.cpp`, and the macro defs in
    `src/cameras.h`, `src/scopes.h`, `src/rotators.h` (some still `#include` deleted
    `*_ascom.h` headers behind a now-always-false guard — inert on Linux, but dangling).
  - the bulk `if(WIN32)` **vcpkg** dependency-provisioning blocks in
    `thirdparty/thirdparty.cmake` (cfitsio/curl/eigen/opencv/INDI-on-Windows). Inert on
    Linux; no longer reference any deleted repo file after the `WinLibs` block was removed.
  - generic `__WINDOWS__` OS-behavior ifdefs (PATHSEP, `PHD_OSNAME`, message-catalog) in
    `src/phd.h` etc.

## Phase 2 — drop macOS
- **Leftover macOS cleanup (follow-up).** `phase/2-drop-macos` deleted the mac files/scripts/
  backends and the main `CMakeLists.txt` `if(APPLE)` blocks, but deferred (inert on Linux):
  - `if(APPLE)` blocks in `thirdparty/thirdparty.cmake` (Carbon/IOKit/Cocoa frameworks, OSX
    arch flags), `cmake_modules/compiler_options.cmake` (MACOSX_RPATH), `PHD2Packaging.cmake`
    (dmg packaging), `FindZWO.cmake` (mac SDK path).
  - inline `#ifdef __APPLE__` source paths: `serialport.cpp` (`SerialPortMac` branch),
    `phd.h` (`PHD_OSNAME`/PATHSEP), and the dead `#ifdef GUIDE_EQMAC|EQUINOX|VOYAGER`
    factory blocks + macro defs in `scope.cpp` / `scopes.h`.

## Phase 3 — drop INDI
- **Leftover INDI cleanup (follow-up).** `phase/3-drop-indi` removed the INDI files, macros,
  libindi dependency, and packaging, but deferred (dead on Linux now that the macros are gone):
  - inline `#ifdef GUIDE_INDI` / `#if defined(INDI_CAMERA)` / `#ifdef ROTATOR_INDI` factory
    branches in `scope.cpp` / `camera.cpp` / `rotator.cpp`, the guarded `OnINDIConfig`/
    `OnINDIDialog` decls + `MENU_INDICONFIG`/`MENU_INDIDIALOG`/`SCOPE_INDI` enum constants in
    `myframe.h`, and the `INDIRotatorName()` helper in `rotator.cpp`.
  - stale "shared with `INDIDiscovery`" comments in `tests/discovery/test_discovery_logic.cpp`,
    `tests/CMakeLists.txt`, and `tests/README.md` (the parse/dedupe model is now Alpaca-only).

## Phase 4 — headless run mode (deferred items)
Headless-by-default + the systemd unit landed; these are the deliberate follow-ups:
- **systemd sandboxing.** The unit runs as a dedicated `openastro-phd2` user but has no
  sandboxing directives. Add `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`,
  `PrivateTmp`, `ReadWritePaths=/var/lib/openastro-phd2` (and verify it still reaches the
  network for Alpaca + can start Xvfb). Held out of the Phase-4 PR to keep it small/safe.
- **Vestigial `plugdev`/`dialout` grants.** `debian/openastro-phd2.postinst` adds the service
  user to `plugdev`/`dialout` "for USB camera/guide port access," but this fork is Alpaca-only
  (network) and the local serial/USB guide backends were deleted in the dead-code cleanup, so
  the grants now protect nothing. Drop them (likely alongside the hardening pass).
- ~~**Dead `*_indi_*` JSON-RPC methods.**~~ **Resolved.** `get_indi_server`/`set_indi_server`,
  the `get/set_selected_indi_*_driver` methods, the dead `Contains("INDI")` branches in the
  mount/camera/rotator selection setters, and the orphaned `extract_bracketed_driver_name`
  helper were all removed from `event_server.cpp` (no live backend since the Phase 3 INDI
  drop; ARA never adopted them — `API_REFERENCE.md` had them marked "do not use").

## Phase 5 — API gap-fill (planned)
- **Advanced/"Brain" settings over RPC.** The existing methods cover equipment, profiles,
  dark library, algo params, and guide/dither/calibrate, but the Advanced Settings dialog
  surface (algorithm *selection*, max RA/Dec, dec-comp, star detection thresholds, dither
  defaults, camera gain, backlash, …) isn't fully exposed. **Audit done — see
  `design/API_GAP_AUDIT.md`** for the full gap table + prioritized batches. Implement on the
  shared dispatch (so both `:4400` and `/api/rpc` gain each), logging each in
  `design/API_CONTRACT.md`. This is what lets ARA fully drive the app.
  **Done: Batch A** (algorithm selection, max RA/Dec, dec-comp, dither defaults),
  **Batch B** (star detection thresholds, camera gain/timeout), **Batch C1** (mount flags,
  backlash comp, auto-exposure, noise reduction), and **Batch C2** (camera subframes-setter /
  cooler-setpoint / saturation, rotator reverse, niche guider options). **The A/B/C audit
  batches are complete.** Remaining known gap: explicit dark/bad-pixel-map library
  create/select/delete coverage, to be confirmed against ARA's needs (see `API_CONTRACT.md`).
- **Web-UI UX rework** (`scripts/webui/index.html`) — functional but poor UX; revisit once the
  API is complete (or let ARA supersede it).
- **NINA plugin** (future) — configure settings through a plugin while guiding runs through the
  main NINA app over `:4400`.

## Resolved — dead-code cleanup
The dead-platform cleanups are **done**, in two passes:

1. `cleanup/dead-platform-ifdefs` — the device-factory `#ifdef` stubs (ASCOM/INDI/macOS
   scopes), the platform macro blocks (collapsed to Linux/Alpaca in `scopes.h`/`cameras.h`/
   `phd.h`), the `serialport` `__WINDOWS__`/`__APPLE__` paths, and the inert
   `if(WIN32)`/`if(APPLE)` blocks in `thirdparty.cmake` / `cmake_modules`. FreeBSD support
   and the Shoestring/direct-ST4 backends (`scope_gpusb`/`gpint`/`GC_USBST4`/`parallelport`)
   were dropped too.

2. `cleanup/strip-platform-ifdefs` — the remaining **inline** `#ifdef _WIN32` /
   `__WINDOWS__` / `__WXMSW__` / `__APPLE__` / `__WXOSX__` branches across ~20 GUI/utility
   sources (`alpaca_discovery.cpp`, `phd.cpp`, `aui_controls.cpp`, `graph.cpp`, `myframe*`,
   `fitsiowrap.cpp`, …). These were dead on Linux but still present (the earlier note
   overstated their removal). Stripped with `unifdef` keeping the Linux/wxGTK branches;
   behaviour-neutral (the preprocessed Linux output is unchanged), full `phd2` build + all
   tests green.

Tree builds Linux-only and reports 0 cppcheck findings.
