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
