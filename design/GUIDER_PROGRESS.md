# openastro-guider — Progress

Single-page status. **Read this first on resume.** See `design/PHD2_HEADLESS_PLAYBOOK.md`
for the full plan.

---

## Current phase
**Phase 5 (API gap-fill) complete**, including the dark/bad-pixel-map tail. Phase-4 deferred
items (systemd sandboxing, group grants, dead INDI RPCs) closed. Polar-alignment **guider-side
enablers** landed (spike gate verified + `get_star_centroids` + PA session lease); the routine
itself is ARA-side (see `POLAR_ALIGNMENT_DESIGN.md`). Next up: **Phase 6 — ARA integration
validation** (driven from the ARA repo).

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
- ✅ **Phase 5 (API gap-fill)** — the Advanced/"Brain" settings audit (`API_GAP_AUDIT.md`)
  implemented in Batches A/B/C1/C2 (#42–#45) plus the dark/bad-pixel-map tail
  (`rebuild_defect_map`, `add_bad_pixel`, `set_dark_auto_load`), all on the shared dispatch and
  logged in `API_CONTRACT.md` / `API_REFERENCE.md`.
- ✅ **Phase-4 deferred items closed** — systemd sandboxing (`NoNewPrivileges`,
  `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `ReadWritePaths`), the vestigial
  `plugdev`/`dialout` grants revoked, and the dead `*_indi_*` RPC methods removed.
- ✅ **Polar-alignment guider-side enablers** — the §12.1 spike gate verified
  (`capture_single_frame` saves a solver-ready FITS; `SingleFrameComplete` carries the path),
  `get_star_centroids` (multi-star sub-pixel centroid report), and the `set/get_pa_session`
  ownership lease. The PA routine itself (plate solver, state machine, UX) is ARA-side — see
  `POLAR_ALIGNMENT_DESIGN.md`.
- ⏭️ Next: **Phase 6 — ARA integration validation** (ARA ↔ headless guider end-to-end against
  Alpaca simulators; driven from the ARA repo).

## Last merged
- See `git log` / the GitHub PR list for the latest.

## Next step
- Phase 6 (ARA-side): wire ARA to the guider API and run the §6 integration smoke (connect,
  calibrate, guide, dither/settle, dark library, algorithm change, star-lost recovery), then
  the ARA polar-alignment spike (ASTAP solve of `capture_single_frame` output, 2-pt axis).
- Guider housekeeping (low priority): web-UI UX rework or let ARA supersede it; NINA plugin
  (future).
