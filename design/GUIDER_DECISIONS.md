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

## 2026-06-06 — Phase 4: headless run mode + systemd

- **What:** Made **headless the default run mode**. Previously the app launched the GUI unless
  `--headless` was passed; now it starts headless (window hidden, event/JSON-RPC server forced
  on) and `--gui` is the opt-in to show the window for local debugging. The constructor default
  and `OnCmdLineParsed` both default `m_headless = true`. Added a clean `--auto-connect` switch
  (auto-connect the selected equipment on startup); `--headless` is now an accepted no-op and
  `--headless-auto-connect` is a back-compat alias for `--auto-connect`, so existing
  scripts/units/NINA invocations keep working. The systemd unit's `ExecStart` drops `--headless`
  and uses `--auto-connect`.
- **Why:** `openastro-guider` is a guiding *daemon*, not an app (playbook §2). It should "just
  start headless" under systemd with no flag; the GUI is only a local-diagnosis affordance.
- **Display strategy — Xvfb (documented, not Xvfb-free):** the daemon runs the wxGTK core under
  `xvfb-run` (a virtual X server) with the window hidden. wxGTK genuinely needs a display to
  initialize even when nothing is shown, so a truly Xvfb-free build would mean decoupling the
  guide engine + event server from `wxApp`/GTK startup — a large, higher-risk refactor of app
  init that we explicitly chose **not** to do (rule #1: no scope creep). Xvfb is lightweight
  and is the same approach KStars/Ekos-style headless guiders use. `xvfb` is a runtime `Depends`
  in `debian/control`. Encoded in `debian/openastro-phd2.service` (with a comment pointing here).
- **Two API surfaces, one method table:** the event server exposes the same ~95 JSON-RPC methods
  over **two** sockets — raw TCP `:4400` (the standard PHD2 event-server protocol, used by
  **NINA** and the future NINA plugin) and an embedded HTTP server `:8080` (`8080+instance-1`)
  that serves the static web UI and bridges `POST /api/rpc` → `call_rpc_result_raw` (the surface
  **ARA**/browsers drive). Phase 5 (API gap-fill) adds methods to that shared table so both
  surfaces gain them at once. Recorded so the protocol-compat constraint on `:4400` is explicit.
- **Packaging:** verified the daemon end-to-end (starts headless under Xvfb, `:4400` + `:8080`
  both listen, `get_app_state` answers over both). Fixed the unit's `Documentation=` URL
  (`phd2-alpaca-indi` → `openastro-guider`) and deleted the stale duplicate
  `packaging/systemd/phd2-headless.service` (old `phd2`/`phd2.bin`/`/var/lib/phd2` names) — the
  installed `debian/openastro-phd2.service` is canonical.
- **Deferred** (tracked in `GUIDER_TODO.md`): systemd sandboxing hardening; the now-vestigial
  `plugdev`/`dialout` group grants in `postinst` (Alpaca-only ⇒ no local USB/serial backends);
  the dead `*_indi_*` JSON-RPC methods left after the INDI drop; the web-UI UX rework; and the
  NINA plugin. Phase 5 = the API gap-fill (Advanced/"Brain" settings).
- **Verified:** full `phd2` target builds; headless smoke test green (both sockets reachable,
  RPC answers).

## 2026-06-07 — No authentication (LAN-trusted, HTTP-only)

- **What:** The event server (`:4400`) and the embedded HTTP `/api/rpc` (`:8080`) have **no
  authentication, no TLS, no CORS**, and bind `0.0.0.0`. We are **keeping it that way.**
- **Why:** This is an astro LAN appliance, not an internet service. `:4400` is the *standard*
  PHD2 protocol — **NINA has no way to authenticate to it**, so adding auth there would break
  the primary client. Deploy on a trusted local network (the same assumption upstream PHD2 and
  every ASIAIR-style guider makes). Flagged repeatedly by the code-review bot; recorded here so
  it stops being re-litigated.
- **If that ever changes:** auth would go on the `:8080` HTTP surface only (a static bearer
  token + CORS), leaving `:4400` for NINA, or bind `:4400` to localhost behind a proxy.

## 2026-06-07 — Phase 5 Batch A: guiding-control API + reference doc

- **What:** Added the first Phase-5 gap-fill methods to the shared event-server dispatch (so
  they serve both `:4400` and `/api/rpc`): guide-algorithm **selection** (`get_algos`,
  `get_algo`, `set_algo` — params were already exposed; *switching* the algorithm was the gap),
  max RA/Dec pulse limits (`get/set_guide_limits`), dec compensation (`get/set_dec_comp`), and
  dither defaults (`get/set_dither_settings`). Exposed the previously file-local
  `GuideAlgorithmName`/`GuideAlgorithmFromName` as public `Mount` statics so the GUI choice
  list and the API share one name↔enum mapping.
- **Why:** Batch A of the `API_GAP_AUDIT.md` plan — the live knobs that make ARA a real guiding
  controller. Dither *mode* (vs scale/RA-only) was left out of this batch (no public accessor;
  deferred).
- **Docs:** created `design/API_REFERENCE.md` — the full human-readable reference for **all**
  ~104 methods + the event stream (companion to the append-only `API_CONTRACT.md`).
- **Verified:** full `phd2` target builds; all 9 new methods present in the dispatch table /
  binary. (Runtime RPC round-trip couldn't be exercised in the sandbox — wxGTK needs Xvfb,
  which the harness kills — but the handlers mirror the existing proven ones.)

## 2026-07-31 — Optional multi-star GuideStep telemetry for ARA auto-tune

- **What:** Added optional `MultiStarCount` and `RejectedStarCount` integer fields to `GuideStep`.
  The event server emits them only when the active guider reports a valid `used/available` multi-star
  count. Legacy and single-star payloads remain unchanged.
- **Why:** ARA's deterministic auto-tuner needs guide-star quality evidence when selecting short
  exposures. Optional fields preserve compatibility with older guider clients and profiles.
- **Verified:** `cmake --build build -j2`; CTest unit suites excluding the existing long-running
  `GuidePerformanceTest`, `GaussianProcessTest`, and `GPGuiderTest`; legacy and extended JSON schema
  cases pass.
- **Deferred:** ARA remains tolerant of omitted fields. No guide-loop behavior or pulse calculation
  changes are made here.
