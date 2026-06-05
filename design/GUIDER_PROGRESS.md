# openastro-guider — Progress

Single-page status. **Read this first on resume.** See `design/PHD2_HEADLESS_PLAYBOOK.md`
for the full plan.

---

## Current phase
**Phase 1 — drop Windows.** (In flight on branch `phase/1-drop-windows`.)

## Status
- ✅ Repo created as a hard-fork of `open-astro/openastro-phd2` (full history, 3,590 commits; `upstream` remote = openastro-phd2).
- ✅ **Phase 0 complete** — `design/` tracking files + playbook (PR #1), Claude PR-review workflow (PR #2), non-compiling CI baseline (PR #3: sanity/format/cppcheck/unicode/shellcheck/zizmor). Note: the project *build*+GTest job is intentionally deferred until after Phase 3 (INDI/GUI weight) per `ci.yml`.
- 🔄 **Phase 1** — deleting Windows files, `WinLibs/`, ASCOM-COM + win32 backends, and the `if(WIN32)`/MSVC blocks in `CMakeLists.txt`. Leftover inline `__WINDOWS__`/`ASCOM_*` code paths + `thirdparty.cmake` vcpkg blocks deferred to `phase/1b` (see `GUIDER_TODO.md`).
- ⏭️ Next: `phase/1b` (strip leftover Windows ifdefs/vcpkg), then Phase 2 (drop macOS).

## Last merged
- PR #3 — CI baseline (non-compiling check suite).

## Next step
- Verify `./build-deb.sh` green on arm64, push `phase/1-drop-windows`, open PR, merge.
