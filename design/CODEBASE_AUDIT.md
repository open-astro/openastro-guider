# openastro-guider — Codebase Audit

**Date:** 2026-07-11
**Version audited:** 2.0.0 (`main` @ `f45f0723`)
**Scope:** Full-tree senior engineering audit covering the OpenAstro-authored Alpaca
network layer, the event-server / JSON-RPC surface, the inherited PHD2 core engine,
build/CI/packaging, and the Python tooling + test suite.

> **Remediation status:** The High/Medium correctness, parser, and daemon-robustness
> findings, plus the build/CI cleanup, were fixed in PR #68 (merged to `main`). The
> web-UI authentication / CSRF items (#4) and the deeper async-HTTP rework (#8) were
> intentionally deferred. This document is retained as the original findings record.

## Context

`openastro-guider` is a Linux-only, headless, Alpaca-only guiding engine — a *strip +
headless-enable* of PHD2, not a rewrite. The inherited guiding engine, calibration math,
guide algorithms, and event-server protocol are kept intact by design; OpenAstro's own code
is concentrated in the Alpaca backends, the headless run mode, the event-server hardening,
the systemd packaging, and the Python tooling. This audit weighs findings accordingly: the
network-facing surface (event server, Alpaca client, web UI) gets the most scrutiny, and the
inherited engine is spot-checked for issues that specifically bite an **unattended daemon**.

Overall the fork is in good shape and has clearly hardened the right places (path
confinement, request size caps, RFC-correct JSON escaping, CSRF checks, an HTTP send queue,
Alpaca binary-payload bounds checks). The residual risk sits in two bands: (1) the inherited
vjson parser, which still assumes trusted input but now sits on the primary attack surface,
and (2) the GUI-heritage seams that a headless daemon exposes (no signal handling, modal
dialogs on daemon paths, synchronous blocking HTTP on the event-loop thread).

---

## Priority summary

| # | Severity | Area | Finding |
|---|----------|------|---------|
| 1 | **High** | Event server | Unauthenticated heap/stack over-read in JSON number scanner (missing NUL guard) |
| 2 | **High** | Event server | Unauthenticated CPU-DoS freezes the guiding thread via uncapped float-exponent expansion |
| 3 | **High** | Alpaca | JSON `ImageArray` axis-swap path transposes/truncates frames from spec-compliant servers |
| 4 | **High** | Web UI | Fallback web server is an unauthenticated full-control RPC proxy (CSRF / DNS-rebinding) |
| 5 | **High** | CI | CI never compiles or runs tests; the entire 131-case suite runs only on a developer's Pi |
| 6 | **High** | Build | No compiler warnings enabled anywhere (`-Wall`/`-Wextra`/`-Werror` absent) |
| 7 | Medium | Alpaca | Blanket retry of non-idempotent PUTs can double-fire guide pulses |
| 8 | Medium | Alpaca | Synchronous HTTP with generous timeouts blocks event-loop / GUI / connect threads |
| 9 | Medium | Core | OOM during capture aborts the daemon (dead non-nothrow `new[]` null-check) |
| 10 | Medium | Core | No SIGTERM handling — `systemctl stop` kills without graceful shutdown |
| 11 | Medium | Core | Modal `wxMessageBox` reachable from daemon log paths can hang the service |
| 12 | Medium | Event server / Core | Event socket silently truncates writes to slow clients, corrupting the JSON stream |
| — | Low | Various | See detail sections |

---

## 1. Event server / JSON-RPC (`event_server.cpp`, `json_parser.cpp`)

The network-facing control surface. All socket events run on the wx main event-loop thread,
which also runs guide-step/mount-move processing — so anything that blocks the parse blocks
guiding. The recently added application-layer hardening is well executed; the exposure sits
one layer down in the inherited vjson parser.

### High

- **Heap/stack over-read in the number scanner — `json_parser.cpp:608`.** The number-token
  loop terminates on whitespace/`,`/`]`/`}` but never on `\0`, so a trailing number at
  end-of-buffer walks past the terminator. An unauthenticated client sending `[1` triggers a
  heap over-read on the `POST /api/rpc` path (buffer is `malloc(length+1)`) or a stack
  over-read on the `:4400` line path — worst case an unmapped-page read crashes the daemon.
  **Fix:** add `*it != '\0'` to the loop condition.

- **CPU-DoS via exponent expansion — `json_parser.cpp:281-297`.** `atof` expands the exponent
  with an O(exponent) loop over an unbounded client value. `[1e999999999]` measured at ~2.9 s
  of pure CPU on the test box; running on the shared event-loop thread, this stalls guiding
  for the full window, and an array of such tokens multiplies it into minutes. Large digit
  strings also overflow the `int exponent` (UB) en route. **Fix:** clamp the exponent
  magnitude (beyond ~±38 is already `inf`/0 for `float`).

### Medium

- **Lossy write to slow clients — `event_server.cpp:512-522`.** The `:4400` `send_buf` writes
  once and only logs a short write; a stalled reader gets a truncated JSON line and every
  subsequent message is mid-stream garbage. The HTTP path got a proper queue+backpressure fix
  (`send_buf_http`, `:1094`); the main event socket did not. (Also reported by the core-engine
  pass — same root cause.)

- **No connection cap / no idle timeout — `event_server.cpp:7569`, `:7617`.** Both accept
  loops insert every client with no ceiling and no read timeout; a slowloris partial-header
  client holds a connection (and up to a 1 MB `recvbuf`) open forever. Even under the stated
  "LAN-trusted" model, one host can exhaust FDs/memory and take guiding down.

### Low

- Signed-overflow UB in `atoi`/`atof` accumulation (`json_parser.cpp:180,239,277`).
- `GET /api/discover/alpaca` (`event_server.cpp:7326`) is a side-effecting GET outside the
  CSRF guard (which covers only `POST /api/rpc`); commit-`27706ba` clamps bound the cost.

### Done well

`capture_single_frame` path confinement (`realpath` on parent + allowed root, trailing-slash
compare) defeats `../` and planted-symlink escapes; 1 KB line cap and 1 MB HTTP body cap with
pre-addition `Content-Length` rejection; batch cap of 100; RFC 8259 control-char escaping in
`json_escape`; refcounted `ClientData` reentrancy safety; correct non-blocking HTTP
backpressure. The JSON *string* decoder correctly stops at NUL and decodes in place.

---

## 2. Alpaca network layer (`alpaca_client.*`, `cam_alpaca.*`, `scope_alpaca.*`, `rotator_alpaca.*`, `config_alpaca.*`, `alpaca_discovery.*`)

Functional and unusually defensive at the byte level, but carrying visible scar tissue from
debugging a misbehaving server — some workarounds are now correctness hazards.

### High

- **`ImageArray` JSON axis-swap transposes/truncates frames — `cam_alpaca.cpp:1161-1267`.**
  The swap heuristic swaps `imageWidth`/`imageHeight` but the copy loop is not adjusted: rows
  are bounded by `height` when JSON rows are the sensor *width*, and pixels land at
  `img[xcoord*W + ycoord]`. A spec-compliant server serving JSON (no ImageBytes) yields a
  garbled, transposed H×H corner with the rest of the buffer uninitialized — every guide
  frame unusable. The ImageBytes decoder in the same function gets this right; the fix pattern
  already exists locally.

### Medium

- **Blanket PUT retry double-fires guide pulses — `alpaca_client.cpp:585-612`, `1257-1285`.**
  Retrying on `CURLE_GOT_NOTHING`/`CURLE_RECV_ERROR` re-sends requests the server may have
  already executed. `PutAction` carries `pulseguide` and `startexposure`; a dropped connection
  after a completed 1000 ms pulse causes a double move and an unexplained guide overshoot.
  Retry is safe for GETs — make it opt-in per endpoint for PUTs.
- **One transient error permanently disables coordinate reporting — `scope_alpaca.cpp:675-679`.**
  `GetDeclinationRadians()` clears `m_canGetCoordinates` on *any* throw, including a single
  timeout — one Wi-Fi hiccup silently degrades dec-compensated guiding for the session. Clear
  only on a definitive "not implemented" Alpaca error.
- **Discovery RPC blocks the main loop up to ~10 min — `event_server.cpp:1400-1431` +
  `alpaca_discovery.cpp:180-317`.** Discovery always waits the full per-query timeout even
  after responses arrive; `{"num_queries":20,"timeout_seconds":30}` freezes the event loop for
  600 s. Even defaults freeze it ~4 s.
- **Long synchronous connect chains, no cancellation — `cam_alpaca.cpp:163-594`.** ~15
  sequential HTTP calls (10 s connect / 30 s transfer each) plus a 30 s poll on the RPC
  thread; against a black-holing host, connect takes minutes uninterruptibly. Same in
  `ScopeAlpaca::Connect` and `AlpacaConfig::QueryDevices` (on the GUI thread — discover was
  moved to `std::async`, query was not).
- **Cooler-status reads serialize behind image downloads — `alpaca_client.h:54`.** One CURL
  handle + one mutex per camera means `get_cooler_status` stalls behind a 30 s `imagearray`
  download. Correct locking, wrong granularity — use a second handle for light status reads.

### Low

- `CURLOPT_POSTFIELDSIZE` passed `int 0` instead of `0L` (varargs UB) — `alpaca_client.cpp:206,416`.
- `PutAction`'s `action` param is dead; callers pass values that are never sent (`:1223`).
- Broken/dead property-name capitalization dance triplicated across `GetDouble/Int/String` (`:769-791` etc.); `GetBool` inconsistently lacks it.
- `errorCode` out-param conflates transport / HTTP / Alpaca error domains; `Connect` misreads `200` as "auth enabled" for any malformed body.
- Discovered `AlpacaPort` not range-checked (`alpaca_discovery.cpp:248-265`) — clamp to 1..65535.
- Very verbose hot-path logging (~150 lines/s while exposing; 20 ms `imageready` poll).
- `Slewing()` fails open (guiding continues through a slew on status error); `SaveSettings` can keep old port with new host.

### Done well

The ImageBytes binary decoder is genuinely careful (LE reads with offset bounds checks,
payload/type validation, clamped subframe writes — no overflow path found). Resource
discipline is clean: one CURL handle created/destroyed once, `curl_slist`s freed on every
path, discovery socket closed on all paths. Client thread-safety is correct (single mutex,
atomic transaction IDs, response copied out under lock). Headless constraints respected — no
modal dialogs from `Connect()`.

---

## 3. Core engine (inherited PHD2, spot-check)

Mature, deliberately structured (clean worker-thread request/complete protocol, consistent
main-thread marshalling for wx calls). The fork hardened the right places. Residual risk is
concentrated at the GUI-heritage seams a headless daemon exposes.

### Medium

- **OOM aborts the daemon — `usImage.cpp:129`.** Non-nothrow `new unsigned short[NPixels]`
  never returns null, so the null-check and the entire `CAPT_FAIL_MEMORY` →
  `DisconnectWithAlert` recovery path is dead code. `bad_alloc` propagates out of
  `WorkerThread::Entry` (which catches only `const wxString&`) → `std::terminate`. Memory
  pressure on a small SBC after hours of per-frame allocation aborts mid-session. systemd
  `Restart=on-failure` masks it but guiding state is lost.
- **No signal handling — `phd.cpp`.** No `sigaction` for SIGTERM/SIGINT anywhere; the service
  is `Type=simple` with no `ExecStop`, so `systemctl stop` kills with default disposition —
  `MyFrame::OnClose` never runs, so no camera/mount disconnect, no guide-log summary, no
  config flush. Graceful shutdown works only via the JSON-RPC `shutdown` command.
- **Modal dialogs on daemon paths — `debuglog.cpp:120,135`.** Log-open failure and log-dir
  change open a modal `wxMessageBox` on the Xvfb display that nobody can dismiss; an unwritable
  or full log dir at startup or daily rollover hangs the relevant path.
- **Lossy event-socket writes** — same as Event Server finding above (`event_server.cpp:512-522`).

### Low

- `RefineOffset` double-processes a star after a zero-count eviction (`guider_multistar.cpp:836-839`) — small guiding-accuracy defect, no crash.
- `SaveStarFITS` negative-index over-read near top/left edge (`guider_multistar.cpp:1281-1294`) — **dead code** (no callers).
- Cross-thread flags are `volatile`, not atomic (`worker_thread.h:114-115`) — formal data race, benign on Linux ARM/x86.
- Forced `Kill()` of a stuck worker mid-HTTP-call abandons locks/heap (`myframe.cpp:1599`) — last-resort path before shutdown.
- Star-lost "red flash" `wxMilliSleep(100)` runs headless too, blocking the event loop per lost frame (`guider.cpp:1351`).
- Failed auto-select always writes a full-frame FITS regardless of logging settings (`imagelogger.cpp:254`) — bounded disk-fill vector on SD cards.

### Checked clean

`circbuf.h` (correct, no growth); log growth is bounded (per-line flush, daily rollover,
30-day retention); image ownership through the pipeline is coherent (no per-frame leak found);
`star_find_core.cpp` bounds-clamps every pass and the fork *added* an `nbg == 0` NaN guard;
`fitsiowrap.cpp` correct.

---

## 4. Build / CI / Packaging

Packaging and CI *security hygiene* are well above typical hobby-project standard. Two
structural gaps dominate.

### High

- **CI never compiles or runs tests — `.github/workflows/ci.yml`.** Only lint/sanity jobs
  (clang-format, cppcheck-on-changed-lines, unicode, line-endings, shellcheck, zizmor). The
  stated blocker ("once INDI is stripped") is stale — INDI is already gone and the tree
  builds. The 15-binary / 131-case suite runs only on a developer's local Pi; a compile break
  or test regression merges green. Free `ubuntu-24.04-arm` runners make the arm64 target
  CI-buildable today. **This is the single highest-value fix.**
- **No compiler warnings enabled — `cmake_modules/compiler_options.cmake`.** No
  `-Wall`/`-Wextra`/`-Werror` anywhere, no sanitizer/debug presets. On a large inherited C++20
  tree this forfeits the cheapest bug-detection layer (uninitialized reads, sign issues,
  ignored results).

### Medium / Low

- `debian/control:7` allows wx 3.0 alternatives that contradict `find_package(wxWidgets 3.2 REQUIRED)`.
- GTest FetchContent has no `URL_HASH` (`thirdparty.cmake:101-104`) — unverified download on the default configure path.
- `pre-commit.py` is dead: Python-2-only `unicode()` (crashes on modern interpreters) and a newline policy that contradicts the repo's intentional CRLF/LF mix. Delete or rewrite.
- `run_deb.sh` dependency hint omits required `libcfitsio-dev` while listing no-longer-needed gettext/i18n.
- Dead `-DOPENSOURCE_ONLY=1` flag consumed by no CMake file; Build-Depends drift (`libv4l-dev`/`V4L_CAMERA` dead; `libx11-dev` only transitive); `CMakeLists.txt` reads `USE_SYSTEM_GTEST` before it's declared and calls `enable_language()` before `project()`.
- Reproducibility: builder git identity + timestamps stamped into the changelog; no `SOURCE_DATE_EPOCH`.

### Done well

All actions SHA-pinned, `permissions: contents: read`, `persist-credentials: false`, a
checksum-verified zizmor audit, Trojan-Source Unicode scan, ShellCheck, and a CRLF-flip guard
born from a real incident. Changed-lines scoping keeps lint meaningful on an inherited tree.
Packaging is careful: hardened systemd unit (`NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome`, `PrivateTmp`, dedicated user), postinst that revokes obsolete group grants,
versioned `Breaks`/`Replaces`, dpkg hardening flags, ctest forced even under `nocheck`.
Single-source versioning (`version.md` → CMake/deb/changelog). Minimal vendoring — essentially
everything from system packages; GTest current (1.17.0). No stale/vulnerable vendored libs.

---

## 5. Python tooling, web UI, and tests

### High

- **Fallback web server is an unauthenticated full-control RPC proxy —
  `scripts/phd2_web_ui_server.py:313-327`.** `POST /api/rpc` forwards any method to the daemon
  with no auth token, no `Origin`/`Host` validation, no Content-Type requirement; the UI sends
  a CORS "simple request", so any malicious page the operator visits can fire blind
  cross-origin POSTs (`shutdown`, `delete_profile`, `set_alpaca_server` to an attacker host),
  and DNS rebinding makes responses readable. With `--listen 0.0.0.0` anyone on the LAN gets
  full control. Mitigations present: loopback default, and the script self-identifies as a
  deprecated dev fallback. **Add an origin check + a loopback-only or token guard, or retire
  it in favor of the embedded C++ portal.**

### Medium / Low

- `/api/discover/indi` is an unauthenticated network-scan proxy with an uncapped `max_hosts` (`:300-309`) — CSRF-driven internal port scanning.
- The Python fallback can't serve `/api/frame.jpg` (only the C++ server implements it), so the fallback UI shows a permanently blank camera panel + a 404 every 400 ms — the two servers have drifted apart.
- Daemon-originated strings rendered via `innerHTML` (`webui/assets/app.js:461,471,649`) — latent XSS if any status/param string ever echoes user text.
- Dead event-subscription machinery (no SSE/WebSocket consumer); misleading "bridge connected" startup message; pending RPC callers not failed on disconnect; unvalidated client `timeout_s`.
- `fake_alpaca_camera.py` reads scope fields without the state lock and doesn't URL-decode PUT bodies — benign for a test tool.

### Test suite

11 gtest suites, ~2,700 lines, **131 real-assertion cases, all hermetic** (no network/hardware).
Genuinely high-quality where production code is linked in: `json_parser`, `guiding_stats`,
`zfilter`, polar-alignment geometry, calibration transforms, lowpass guide math, star-find on
a real FITS fixture. The header-substitution scheme that links wx-free kernels is clever and
documented. **Two tiers:** the above execute real code; a second tier (`event_server` /
`alpaca` schema fixtures, reimplemented `ParseServerString`, "math twin" algorithm tests) pins
wire shapes and honestly documents that it does *not* exercise the emitters.

**Coverage gaps:** the highest-risk code is the least tested — the guiding loop
(`guider.cpp` 1,957 lines, `guider_multistar.cpp`), event-server RPC dispatch
(`event_server.cpp` 8,117 lines), and the Alpaca client (1,348 lines) have only fixture-level
coverage; `backlash_comp` and the Gaussian-process algorithm are untested; there are no Python
tests. `fake_alpaca_camera.py` would make a daemon integration test cheap to wire up.

---

## Recommended remediation order

1. **`json_parser.cpp` NUL guard + exponent clamp** (findings 1, 2) — small, unauthenticated, network-triggerable; the two most serious defects.
2. **Add a CI build + `ctest` job on an arm64 runner** (finding 5) — the stated blocker is stale; unlocks every other regression defense and is low effort.
3. **`ImageArray` axis-swap fix** (finding 3) — one High correctness bug; the correct pattern already exists in the same function.
4. **Web UI origin/loopback guard** (finding 4) — or retire the Python fallback in favor of the embedded portal.
5. **Daemon-hardening seams** (findings 9–11): make `usImage` allocation nothrow-aware (or catch `bad_alloc` in the worker), add SIGTERM → graceful shutdown, and replace daemon-path `wxMessageBox` calls with logged errors.
6. **Enable `-Wall -Wextra`** (finding 6) and triage; make PUT retry opt-in (finding 7); move blocking Alpaca HTTP off the event-loop/GUI threads (finding 8).
7. Prune stale tooling (`pre-commit.py`, dead `OPENSOURCE_ONLY`, control-file drift) and the Low-severity polish items.

## Bottom line

This is a competently maintained fork that has already hardened its most exposed surfaces and
carries an honest, well-documented design trail. No memory-corruption *write* primitive or
authentication-bypass-to-RCE was found. The concrete risks that remain are (a) two
unauthenticated parser bugs a few bytes can trigger, (b) a handful of frame-decode and
guide-pulse correctness bugs, and (c) daemon-robustness seams inherited from PHD2's GUI
origins. The most important process gap is that a substantial, good test suite never runs in
CI — fixing that first makes every subsequent fix safe to land.
