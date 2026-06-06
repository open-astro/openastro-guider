# openastro-guider — TODO log

Append-only list of every `// TODO(guider)` and `// GUIDER_BLOCKED` placeholder left in
code during the strip, grouped by phase. Swept before the relevant phase closes.

---

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
