# openastro-guider — Decisions log

Append-only log of every non-obvious decision made during the strip + headless-enable.
Each entry: date, decision, reason, and a file/ref where it's encoded. **Do not edit prior
entries; add new ones at the bottom.**

---

## 2026-06-02 — Repo created

- **Hard-fork of `open-astro/openastro-phd2`.** GitHub forbids forking a repo into the org
  that already owns it, so `openastro-guider` was seeded by cloning `openastro-phd2` and
  pushing its full history (3,590 commits + tags) to a fresh repo. This yields a fully
  independent repo (own issues/PRs, not in PHD2's fork network). `openastro-phd2` is kept as
  the `upstream` git remote for selectively pulling future fixes.
- **Scope set:** Linux-only, headless, Alpaca-only guiding daemon; keep wxWidgets; drop
  macOS, Windows, and INDI. ARA is the UI and drives the daemon over the event-server API
  (JSON-RPC :4400). See `design/PHD2_HEADLESS_PLAYBOOK.md`.
- **Workflow:** direct-to-master GitHub Flow with `phase/<N>-<name>` branches and the same
  merge-gate used across the OpenAstro org (adopted 2026-06-02).

## 2026-06-05 — Default branch is `main` (not `master`)

- The repo's default branch, `origin/HEAD`, and the CI `push:` trigger all use **`main`**.
  The 2026-06-02 entry above and earlier playbook wording said "master"; that was loose
  wording, not a second branch. The playbook has been corrected to say `main`; this
  append-only log keeps the original entry as written. **Branch from / merge to `main`.**

## 2026-06-05 — Phase 1: drop Windows

- **What:** Removed the Windows build system + backends (branch `phase/1-drop-windows`):
  win build scripts (`build-exe.ps1`, `run_exe.bat`), installer (`phd2.iss.in`), resources
  (`phd.rc`), vendored `WinLibs/` (~1.2 MB), `cmake_modules/FindASCOM_INTERFACE.cmake`, and
  the ASCOM-COM + win32 source backends (`cam_ascom`, `scope_ascom`, `rotator_ascom`,
  `comdispatch`, `serialport_win32`, `parallelport_win32`). Stripped the `if(WIN32)`/MSVC
  blocks from `CMakeLists.txt` (win exe target, MSVC PCH macro, win doc/webui bundling,
  `iphlpapi` link) and the `WinLibs` DLL block from `thirdparty/thirdparty.cmake`. Removed
  the now-unconditional `#include`s of deleted headers from the aggregator headers
  (`scopes.h`, `rotators.h`, `serialports.h`, `parallelports.h`) and `rotator.cpp`.
- **Why:** `openastro-guider` is Linux-only/headless/Alpaca-only (playbook §2). Equipment
  comes via the kept Alpaca path (`src/*_alpaca.*`); ASCOM is a Windows-COM backend with no
  role here. All removed code was already dead on Linux (behind `WIN32`/`ASCOM_*` guards),
  so this is a surface-shrink, not a behavior change.
- **Deferred to `phase/1b`:** the remaining inline `#if defined(__WINDOWS__)` /
  `ASCOM_CAMERA` / `GUIDE_ASCOM` / `ROTATOR_ASCOM` code-path blocks in `camera.cpp`,
  `scope.cpp`, `rotator.cpp`, `cameras.h`, `scopes.h`, `rotators.h`, etc. (inert on Linux,
  some still `#include` deleted headers behind a false guard), plus the bulk
  `if(WIN32)` **vcpkg** dependency-provisioning blocks in `thirdparty/thirdparty.cmake`.
  Kept this PR a focused, reviewable deletion that does not risk the Debian build. See
  `design/GUIDER_TODO.md`.
- **Verified:** `./build-deb.sh --force` built `openastro-phd2_2.0.0_arm64.deb` on arm64
  after the strip, with ctest 10/10 green (source-built INDI 2.2.1.1 since system libindi is
  1.9.9). `--force` only skips the script's curl-variant precheck (`libcurl4-gnutls-dev`); the
  build links the installed `libcurl4-openssl-dev`, which `debian/control` accepts.

## 2026-06-05 — Phase 2: drop macOS

- **What:** Removed the macOS build + backends (branch `phase/2-drop-macos`): dmg/run
  scripts (`build-dmg.sh`, `run_dmg.sh`), `Info.plist.in`, `run_phd2_macos`, the vendored
  `extra_frameworks/` (~1.2 MB) and `packaging/macos/`, and the mac-only source backends
  (`serialport_mac`, and the AppleScript/Cocoa scope backends `scope_eqmac`, `scope_equinox`,
  `scope_voyager`). Stripped the `if(APPLE)` blocks from `CMakeLists.txt` (the `MACOSX_BUNDLE`
  executable target, OSX deployment target, Carbon/Frameworks, Apple doc + webui bundling,
  the `-Wno-inconsistent-missing-override` flag) and removed the unconditional `#include`s of
  the deleted headers from `scopes.h` / `serialports.h`. Also dropped a leftover dead
  `if(WINDOWS)` generator block missed in Phase 1.
- **Why:** Linux-only/headless/Alpaca-only (playbook §2). All removed code was dead on Linux
  (behind `__APPLE__` / `GUIDE_EQMAC|EQUINOX|VOYAGER` guards).
- **Deferred** (same precedent as the Windows vcpkg deferral): the inert `if(APPLE)` blocks
  still in `thirdparty/thirdparty.cmake`, `cmake_modules/compiler_options.cmake`,
  `cmake_modules/PHD2Packaging.cmake`, `FindZWO.cmake`, and the inline `#ifdef __APPLE__`
  source paths (`serialport.cpp` mac branch, `phd.h` PATHSEP/OSNAME, the dead
  `#ifdef GUIDE_EQMAC|EQUINOX|VOYAGER` factory blocks in `scope.cpp`). All inert on Linux;
  tracked in `GUIDER_TODO.md`.
- **Verified:** `./build-deb.sh --force` builds the `.deb` + ctest green on arm64.

## 2026-06-05 — Phase 3: drop INDI (Alpaca-only)

- **What:** Removed INDI end to end (branch `phase/3-drop-indi`). Deleted the INDI device
  backends (`cam_indi`, `scope_indi`, `rotator_indi`) and the shared INDI config/GUI/discovery
  (`config_indi`, `indi_gui`, `indi_discovery`); removed the `INDI_CAMERA` / `GUIDE_INDI` /
  `ROTATOR_INDI` feature macros (so the factory `#ifdef` branches in `camera.cpp` / `scope.cpp` /
  `rotator.cpp` compile out, like the ASCOM/macOS ones) and the unconditional `config_indi.h`
  include + `INDIConfig::LoadProfileSettings()` call in `myframe.cpp`. Removed the libindi
  dependency from `thirdparty.cmake` (the `find_package(INDI)` path **and** the INDI 2.2.1.1
  `ExternalProject` source-build, plus the libnova/zlib links that existed only for INDI —
  nothing in our own code uses libnova), deleted `cmake_modules/FindINDI.cmake`, and dropped
  the `USE_SYSTEM_LIBINDI` auto-detect + INDI runtime-bundling from `debian/rules` and
  `libindi-dev`/`libnova-dev`/`indi-bin` from `debian/control`. Removed the
  `test_indi_discovery` unit test.
- **Why:** `openastro-guider` is Alpaca-only (playbook §1-§2); INDI was the last non-Alpaca
  equipment backend. Unlike Windows/macOS, INDI was *live* on Linux, so this rewired the
  device factories (via the macros) rather than just deleting dead-guarded code.
- **Payoff:** builds no longer compile INDI — the slow INDI 2.2.1.1 source-build (Debian Trixie
  only ships 1.9.9) and the `libindi-dev`/`libgsl`/`libfftw` build-dep burden are gone, so
  `./build-deb.sh` is substantially faster.
- **Deferred:** the dead inline `#ifdef GUIDE_INDI`/`INDI_CAMERA`/`ROTATOR_INDI` factory stubs
  and stale "shared with INDIDiscovery" test comments — folded into the same ifdef-cleanup
  follow-up as Windows/macOS (see `GUIDER_TODO.md`).
- **Verified:** `./build-deb.sh --force` builds the `.deb` + ctest 9/9 green on arm64 with no
  libindi present.
