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

## Resolved — dead-code cleanup (branch `cleanup/dead-platform-ifdefs`)
The deferred inline-ifdef / inert-CMake cleanups logged under the Phase 1/2/3 sections
above are **done**: the dead `#ifdef` factory stubs (ASCOM/INDI/macOS scopes), the
platform macro blocks (collapsed to Linux/Alpaca), the `phd.h`/`serialport` `__WINDOWS__`/
`__APPLE__` paths, and the inert `if(WIN32)`/`if(APPLE)` blocks in `thirdparty.cmake` /
`cmake_modules` have all been removed. FreeBSD support and the Shoestring/direct-ST4
backends (`scope_gpusb`/`gpint`/`GC_USBST4`/`parallelport`) were dropped too. Tree builds
Linux-only and reports 0 cppcheck findings.
