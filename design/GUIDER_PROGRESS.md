# openastro-guider — Progress

Single-page status. **Read this first on resume.** See `design/PHD2_HEADLESS_PLAYBOOK.md`
for the full plan.

---

## Current phase
**Phase 2 — drop macOS.** (In flight on branch `phase/2-drop-macos`.)

## Status
- ✅ Repo created as a hard-fork of `open-astro/openastro-phd2` (full history; `upstream` remote = openastro-phd2).
- ✅ **Phase 0 complete** — tracking files + playbook (PR #1), Claude PR-review workflow (PR #2), non-compiling CI baseline (PR #3). Project *build*+GTest CI job still deferred until after Phase 3 (INDI weight).
- ✅ **Phase 1 complete (PR #4)** — dropped Windows: build scripts, `WinLibs/`, ASCOM-COM + win32 backends, `if(WIN32)`/MSVC CMake. cppcheck CI now gates on changed lines + `--library=wxwidgets`.
- ✅ **Static-analysis cleanup (PRs #11–#15)** — drove the inherited tree to **0 cppcheck findings** (epic #10): real bugs, uninit members, copy-control, ignored-returns, printf/perf/misc.
- 🔄 **Phase 2** — deleting macOS: dmg/mac scripts, `Info.plist.in`, `run_phd2_macos`, `extra_frameworks/`, `packaging/macos/`, mac backends (`serialport_mac`, `scope_eqmac`/`equinox`/`voyager`), and the `if(APPLE)` blocks in `CMakeLists.txt`. Inert `if(APPLE)` blocks in `thirdparty.cmake`/`cmake_modules` + inline `#ifdef __APPLE__` paths deferred (see `GUIDER_TODO.md`), same precedent as the Windows vcpkg deferral.
- ⏭️ Next: Phase 3 (drop INDI) — removes the libindi dep + thirdparty INDI source-build.

## Last merged
- PR #15 — final cppcheck cleanup (tree at 0 findings).

## Next step
- Phase 2 PR (#16) is open and green; once merged, start Phase 3 (drop INDI).
