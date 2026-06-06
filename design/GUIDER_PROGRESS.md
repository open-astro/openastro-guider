# openastro-guider — Progress

Single-page status. **Read this first on resume.** See `design/PHD2_HEADLESS_PLAYBOOK.md`
for the full plan.

---

## Current phase
**Phase 3 — drop INDI.** (In flight on branch `phase/3-drop-indi`.)

## Status
- ✅ Repo created as a hard-fork of `open-astro/openastro-phd2` (full history; `upstream` remote = openastro-phd2).
- ✅ **Phase 0 complete** — tracking files + playbook (PR #1), Claude PR-review workflow (PR #2), non-compiling CI baseline (PR #3). Project *build*+GTest CI job still deferred until after Phase 3 (INDI weight).
- ✅ **Phase 1 complete (PR #4)** — dropped Windows. cppcheck CI now gates on changed lines + `--library=wxwidgets`.
- ✅ **Static-analysis cleanup (PRs #11–#15)** — drove the inherited tree to **0 cppcheck findings** (epic #10).
- ✅ **Phase 2 complete (PR #16)** — dropped macOS: dmg/mac scripts, `extra_frameworks/`, mac backends, `if(APPLE)` CMake.
- 🔄 **Phase 3** — dropping INDI (Alpaca-only): deleted the INDI backends + config/GUI/discovery, the `INDI_CAMERA`/`GUIDE_INDI`/`ROTATOR_INDI` macros, the libindi dep (find_package + thirdparty source-build + libnova/zlib), `FindINDI.cmake`, the `debian` INDI logic, and `test_indi_discovery`. Builds no longer compile INDI. Inline dead `#ifdef GUIDE_INDI` factory stubs deferred (see `GUIDER_TODO.md`).
- ⏭️ Next: the deferred inline-ifdef cleanup (Windows/macOS/INDI), then Phase 4 (headless run mode).

## Last merged
- PR #16 — Phase 2 (drop macOS).

## Next step
- Verify `./build-deb.sh` green on arm64 (no libindi needed), push `phase/3-drop-indi`, open PR, merge.
