# openastro-guider — Progress

Single-page status. **Read this first on resume.** See `design/PHD2_HEADLESS_PLAYBOOK.md`
for the full plan.

---

## Current phase
**Phase 4 complete (headless run mode + systemd).** Next up: **Phase 5 — API gap-fill**.

## Status
- ✅ Repo created as a hard-fork of `open-astro/openastro-phd2` (full history; `upstream` remote = openastro-phd2).
- ✅ **Phase 0** — tracking files + playbook, Claude PR-review workflow, non-compiling CI baseline.
- ✅ **Phase 1** — dropped Windows (build scripts, WinLibs, ASCOM/win32 backends, win CMake).
- ✅ **Phase 2** — dropped macOS (dmg/mac scripts, frameworks, mac backends, `if(APPLE)` CMake).
- ✅ **Phase 3** — dropped INDI (Alpaca-only): backends + config/GUI/discovery, INDI macros, the
  libindi dependency, `FindINDI.cmake`, the `debian` INDI logic.
- ✅ **Dead-code + platform cleanup** — drove the tree to **0 cppcheck findings**; stripped all
  inline platform `#ifdef`s (incl. the late `_WINDOWS`/`_WIN64` spellings) so `src/` has **zero**
  platform `#if`s; dropped FreeBSD + the Shoestring/direct-ST4 backends; removed help + all
  translation catalogs (~49 MB); restricted the build to **arm64 only**; scrubbed the last
  historical Windows/macOS code comments.
- ✅ **Test expansion** — extracted PA geometry, calibration transforms, lowpass guide math, and
  star detection into wx-free kernels with unit tests; suite grew 9 → 15 binaries.
- ✅ **Phase 4 (headless run mode)** — headless is now the **default** (`--gui` opts into the
  window; `--auto-connect` added; `--headless`/`--headless-auto-connect` kept as back-compat).
  systemd unit (`debian/openastro-phd2.service`) runs the daemon as a dedicated `openastro-phd2`
  user under Xvfb with the event/JSON-RPC server forced on; maintainer scripts create the user +
  state dir. Verified end-to-end: `:4400` (NINA) and `:8080` (`/api/rpc` + web UI for ARA) both
  reachable, `get_app_state` answers over both.
- ⏭️ Next: **Phase 5 — API gap-fill** (expose the Advanced/"Brain" settings over the shared
  JSON-RPC dispatch so ARA can fully control the app; log each in `API_CONTRACT.md`).

## Last merged
- See `git log` / the GitHub PR list for the latest (the headless/Phase-4 PR is the most recent
  daemon work; #37 was the comment scrub before it).

## Next step
- Phase 5: audit the Advanced Settings dialog → map each control to an existing or new RPC method
  (see the Phase 5 list in `GUIDER_TODO.md`). Also pending: systemd hardening + dropping the
  vestigial `plugdev`/`dialout` grants, and removing the dead `*_indi_*` RPC methods.
