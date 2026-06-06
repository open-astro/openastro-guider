# openastro-guider — Progress

Single-page status. **Read this first on resume.** See `design/PHD2_HEADLESS_PLAYBOOK.md`
for the full plan.

---

## Current phase
**Dead-code cleanup (post-strip).** (In flight on branch `cleanup/dead-platform-ifdefs`.)

## Status
- ✅ Repo created as a hard-fork of `open-astro/openastro-phd2` (full history; `upstream` remote = openastro-phd2).
- ✅ **Phase 0 complete** — tracking files + playbook (PR #1), Claude PR-review workflow (PR #2), non-compiling CI baseline (PR #3). Project *build*+GTest CI job still deferred until after Phase 3 (INDI weight).
- ✅ **Phase 1 complete (PR #4)** — dropped Windows. cppcheck CI now gates on changed lines + `--library=wxwidgets`.
- ✅ **Static-analysis cleanup (PRs #11–#15)** — drove the inherited tree to **0 cppcheck findings** (epic #10).
- ✅ **Phase 2 complete (PR #16)** — dropped macOS: dmg/mac scripts, `extra_frameworks/`, mac backends, `if(APPLE)` CMake.
- 🔄 **Phase 3 complete (PR #17)** — dropped INDI (Alpaca-only): backends + config/GUI/discovery, the INDI macros, the libindi dep (find_package + thirdparty source-build + libnova/zlib), `FindINDI.cmake`, the `debian` INDI logic, `test_indi_discovery`. Builds no longer compile INDI.
- 🔄 **Dead-code cleanup** — swept the deferred dead `#ifdef` stubs the strips left (ASCOM/INDI/macOS-scope factory branches), collapsed the platform macro blocks + `phd.h`/`serialport` to Linux-only, **dropped FreeBSD** + the Shoestring/direct-ST4 backends (`scope_gpusb`/`gpint`/`GC_USBST4`/`parallelport`), removed the inert `if(WIN32)`/`if(APPLE)` blocks from `thirdparty.cmake`/`cmake_modules`, and cleared the Phase-3 review nit (`FindNova.cmake`). Tree at **0 cppcheck findings**, ctest 9/9.
- ⏭️ Next: Phase 4 (headless run mode).

## Last merged
- PR #17 — Phase 3 (drop INDI).

## Next step
- Land the dead-code cleanup PR, then start Phase 4 (headless `--daemon` run mode + systemd).
