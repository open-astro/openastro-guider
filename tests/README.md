# Tests

Unit tests for the OpenAstro PHD2 fork. Wired into CMake's `enable_testing()`
so they run via `ctest` from the build directory alongside the upstream
Gaussian-process tests under `contributions/MPI_IS_gaussian_process/tests/`.

## Running

```bash
# configure (any of the platform run_* scripts works); from the build dir:
cd tmp
ctest --output-on-failure                       # all tests
ctest -R test_alpaca_schema -V                  # single suite, verbose
ctest -E "GP|GuidePerformance"                  # exclude the slow GP tests

# build a single test binary and run it directly:
cmake --build . --target test_json_parser
./tests/test_json_parser
./tests/test_json_parser --gtest_filter='JsonParser.Parses*'
./tests/test_json_parser --gtest_list_tests     # list cases
```

To turn the test build off entirely (for example, for a packaging build that
doesn't need them), configure with `-DBUILD_TESTING=OFF`. The setup also
honours `-DPHD_BUILD_TESTS=ON` if `BUILD_TESTING` is unavailable for some
reason.

GTest is fetched automatically via `FetchContent` (pinned to v1.17.0 in
`thirdparty/thirdparty.cmake`). No system gtest install is needed.

## What's covered

| Executable | Target surface | Style |
|---|---|---|
| `test_json_parser` | `src/json_parser.cpp` — the deserializer behind every Alpaca response and event-server inbound RPC | Direct: compiles the production source into the test binary |
| `test_jsonrpc_schema` | `src/event_server.cpp`'s outgoing message shapes — `Version`, `AppState`, `SettleDone`, `Settling`, `GuideStep` (incl. the fork's `MoveTime`, absent for AO steps), `StarSelected`, JSON-RPC envelope, `get_profiles`, `get_profile`, `get_lock_shift_params`, `guiding_assistant_get_status` / `_apply`, the `GuidingAssistantProgress` / `GuidingAssistantDone` events, `get_imaging_gear` and `set_imaging_gear_devices` | Fixture-based contract: pins the documented field names so a rename in either the production formatter or a downstream consumer (NINA, KStars, Ara) becomes a visible PR diff |
| `test_alpaca_schema` | `src/alpaca_client.cpp` and `src/alpaca_discovery.cpp` JSON shapes — Alpaca standard envelope, `ErrorNumber`/`ErrorMessage` extraction (incl. lenient float→int coerce), camera/telescope `Value` shapes, the property-name fallback for non-standard servers, management API responses, the discovery UDP `{"AlpacaPort": N}` reply | Fixture-based contract |
| `test_discovery_logic` | `host:port` parsing and the `std::set` dedupe model shared by `AlpacaDiscovery::DiscoverServers` and `INDIDiscovery::DiscoverServers` | Model: reproduces the dedupe / parse semantics in `std::string` form, so the actual sockets-bound functions don't have to be runnable in a unit-test process |
| `test_guide_algorithm_math` | `result()` / `reset()` for `GuideAlgorithmIdentity`, `GuideAlgorithmHysteresis`, `GuideAlgorithmResistSwitch` | Math-twin: each algorithm's formula is reimplemented in the test alongside the production line numbers; tests pin the input/output curve on a fixed scenario |
| `test_guiding_stats` | `src/guiding_stats.cpp` — `DescriptiveStats`, `HighPassFilter`/`LowPassFilter`, `AxisStats` (count/mean/variance/sigma/median/min-max/maxDelta/move+reversal counts/linear fit), `WindowedAxisStats` (auto-window trim, `RemoveOldestEntry`, `ChangeWindowSize`, min/max recompute on age-out) | Direct: links the production source unmodified — zero wx coupling |
| `test_zfilter` | `src/zfilterfactory.cpp` — Bessel/Butterworth filter-design invariants (order, corner, name, coefficient shape, normalized `ycoeffs[0] == -1`, finiteness) plus an end-to-end recurrence (the exact `GuideAlgorithmZFilter::result()` difference equation) proving unity-DC-gain low-pass behaviour and high-frequency attenuation | Direct: links the production source unmodified |
| `test_guide_algorithm_lowpass` | `src/guide_algorithm_lowpass_math.cpp` — the Lowpass and Lowpass2 decision math (`reset`/`result`): median + slope*weight + input clamp; warm-up attenuation, outlier-dump, drift slope, sign guard, min-move gate — run against the real `WindowedAxisStats` | Direct (extract-for-testability): math factored out of the GUI-coupled algorithm `.cpp`; production `result()`/`reset()` now delegate to it |
| `test_calibration_transform` | `src/calibration_transform.cpp` — the camera↔mount coordinate transforms extracted from `Mount`: rotate by the calibration RA-axis angle, skew the Dec component by the axis-orthogonality error, the `mountTheta` sign flip for `\|yAngleError\| > π/2`, and `YAngleError`, the axis-orthogonality error `Mount::SetCalibration` stores (0 for orthogonal axes, π for a mirrored Dec axis; #142) | Direct (extract-for-testability): pure math factored out of `Mount`; `Mount::Transform*` delegate to it; verified via rotation/round-trip/skew/sign-flip invariants |
| `test_guiding_assistant_math` | `src/guiding_assistant_math.cpp` — the Guiding Assistant's measurement and advice extracted from `GuidingAsstWin`: per-sample accumulator (high-pass RMS, low-pass RA rate, peaks, net RA drift, linear-fit Dec drift, SNR/mass averages), Barrett polar-alignment error, seeing-based min-move (incl. sliding 2-min windows, multi-star, encoder and sanity-check fallbacks), exposure range, Dec backlash classification/rounding, likely-backlash inference, PA / Lowpass2 / binning advice, the combined `Recommend()`, `guiding_assistant_apply`'s request plan (name parsing, de-duplication, dependency order), failure message and same-session check, and the non-finite guards (0/0 drift fit while samples share one wall-clock second) | Direct (extract-for-testability): wx-free kernel linked with the real `guiding_stats.cpp`; the dialog and the headless `guiding_assistant_*` RPC controller both delegate to it |
| `test_star_find` | `src/star_find_core.cpp` — the star-detection pixel math extracted from `Star::Find`: smoothed-peak search, background-annulus mean/sigma, 3σ threshold, centroid + mass, Simonetti SNR, half-flux radius, saturation heuristic | Golden-file: links the wx-free kernel only (the real `star.cpp`/`usImage.cpp` drag in ~half the app) and runs it on the real `simimage.fit` fixture loaded with cfitsio; pins centroid/mass/SNR/HFD |
| `test_imaging_gear` | `src/imaging_gear.cpp` and `src/imaging_gear_poller.cpp` — the imaging-gear guide log lines (`design/IMAGING_GEAR_LOG.md`), pixel scale, `unknown` values, Alpaca device selection and the `set_imaging_gear_devices` overrides (win over the rules, never the guide camera, take effect on the next poll, incl. while the poll thread runs), the `get_imaging_gear` JSON (`null` for unknown values, escaping), INFO lines from changes between polls (none for a switch to another device), and that the cache never waits on a 5 s server | Direct (extract-for-testability): wx-free code linked unmodified, driven by a fake read-only `Reader` |
| `test_jsonrpc_request` | `src/jsonrpc_request.cpp` — the checks an event-server (:4400) JSON-RPC request gets before dispatch (#118): a string method (else -32600) and params that are an array or object when present (a number, string, bool or `null` gets -32602), each error carrying the request's own id (null only for a non-object batch element or a request without one), and the members read as upstream PHD2 did (last duplicate wins, only a string `method`); and the number and bool param readers the method handlers share (#138): a missing param is refused like one of the wrong type, never dereferenced (`set_algo_param` without `value` crashed the guider; the handler's -32602 answer and that the guider keeps running are checked only by `scripts/phd2_rpc_smoke.py`) | Direct (extract-for-testability): wx-free rule linked unmodified with `json_parser.cpp` |
| `test_alpaca_gain` | `src/alpaca_gain.cpp` — the Alpaca camera gain support decision from `GainMin`/`GainMax` (ASCOM value mode, used by ZWO drivers) and the 0–100 % → raw gain mapping `CameraAlpaca::Capture` sends | Direct (extract-for-testability): wx-free helper linked unmodified |
| `test_alpaca_poll` | `src/alpaca_poll.cpp` — the wait `CameraAlpaca::Capture` sleeps between `imageready` polls (#120): 20 ms until the exposure is due, then a quarter of the time waited past that, 20–250 ms; monotonic, the added latency bound, and the poll count and detection delay of simulated readouts | Direct (extract-for-testability): wx-free helper linked unmodified |
| `test_alpaca_errors` | `src/alpaca_errors.cpp` — which Alpaca capture errors make `CameraAlpaca::Capture` disconnect the camera with reconnect (#103): the `CaptureFailureTracker` fed by every capture call (`binx`/`biny`, `startexposure`, `imageready`, an ImageBytes error payload, a JSON `imagearray` error) reconnects on 1031 NotConnected at once and on the 3rd consecutive 1280 acquisition failure, counted across capture attempts; a single 1280 and other errors keep the alert; a good frame, Connect, Disconnect and a reconnect start a new run, other errors neither count nor reset; and `ReadError`, the `ErrorNumber`/`ErrorMessage` reader behind the JSON `imagearray` check and `AlpacaClient`'s error handling (float coercion, missing or mistyped fields); `CallErrorReason`, the one-line reason of a failed `AlpacaClient` call (#97: Alpaca number and message, HTTP status with a short non-HTML body, no response, invalid response), `OneLine` (whitespace and control characters collapsed, invalid UTF-8 replaced, capped without splitting a UTF-8 sequence) and `CallErrorScope`, the guard that fills the call's `CallError` on every return | Direct (extract-for-testability): wx-free rule and reader linked unmodified with `json_parser.cpp` |
| `test_capture_failure` | `src/capture_failure.cpp` — the text a failed capture reports to clients (#97): `"camera error: <reason>"` from the failed Alpaca call or else the driver's alert, exactly `"camera error -- see debug log"` with no reason, the latest failed call wins and the first driver alert is kept, blank or invalid-UTF-8 input never gives an empty text, long text capped, `AlertWithReason` (`"<prefix>: <reason>"`), and the calibration frame retry (#116) and guiding frame drop (#136): `IsTransient` (Alpaca 1280 and a timed-out call; not 1031, other Alpaca errors, HTTP, parse, other transport failures, a driver alert alone or no failure), `PhaseFor` (the phase of each guider state: calibrating states retry, `STATE_GUIDING` drops, `STATE_CALIBRATED` and every other state stop), and `ShouldRetryFrame` (a decision table over phase × failure × count × capturing × connected: only while calibrating or guiding, never looping or with a star selected, capturing and connected, at most `kFrameRetries` = 2 in a row, so the 3rd 1280 in a row still reaches the #103 reconnect), and `FrameRetries`, the count `MyFrame` keeps (a good frame or a failure not retried starts a new count, a reconnect only once per run of failed frames, so a camera that fails every frame ends the calibration, or stops guiding, after 7 frames and 2 reconnects) | Direct (extract-for-testability): wx-free helper linked unmodified with `alpaca_errors.cpp` |
| `test_settle_frame` | `src/settle_frame.cpp` — what one guide frame does to a guide or dither settle (`PhdController`'s `STATE_SETTLE_WAIT`): `TotalFrames` / `DroppedFrames`, the run of in-range frames timed from its first frame (an out-of-range or lost-star frame ends it, an AO bump holds it, no settle time settles on the first in-range frame), the frame limit before the run before the timeout, and a frame dropped after a transient capture failure while guiding (#136): counted like a lost-star frame, it neither ends nor completes a run, starts none, reports the run's time so far, and still times out or reaches the frame limit | Direct (extract-for-testability): wx-free rule linked unmodified |
| `test_connect_request` | `src/connect_request.cpp` — per-device `set_connected` / `get_connected` (#84): device names, and when a request is refused (camera and `all` need capture stopped; mount and rotator may join while looping but not while calibrating or guiding; nothing while the gear dialog is open) | Direct (extract-for-testability): wx-free rules linked unmodified |
| `test_connect_retry` | `src/connect_retry.cpp` — recovery of a selected device that failed to connect (#104): the retry schedule (15 s, 30 s, then every 60 s, unbounded), the per-device state (a failure schedules, a recovery reports `EquipmentReconnected` exactly once, a user disconnect stops retries but keeps the failure, re-selection forgets, "not configured" parks until the Alpaca server changes, devices independent, stale probe results dropped by token), the gates that make an attempt wait (shutdown, dark build, PA session, Guiding Assistant, a connect on the stack, and the `set_connected` refusal rules), the probe sequence against a scripted server (already connected, unreachable, refused with the server's reason, waits for connected, gives up after its bounded wait, cancel), the `guide` refusal text and the driver reason slot | Direct (extract-for-testability): wx-free rules linked unmodified with `connect_request.cpp` and `alpaca_errors.cpp` |
| `test_restore_calibration` | `src/restore_calibration.cpp` — when `restore_calibration` (#102) is refused (calibrating or guiding; the mount, AO or AO-rig scope not connected; a guide or dither request in flight that is not waiting on a calibration), returns `false` with nothing changed (some mount has no saved calibration, so no partial AO restore), or ends the guide request a failed calibration left waiting before it restores; when `clear_calibration` is refused (calibrating, #137) | Direct (extract-for-testability): wx-free rules linked unmodified |
| `test_pulse_wait` | `src/pulse_wait.cpp` — how long `ScopeAlpaca::Guide` waits for an Alpaca `PulseGuide` (#119), timed from just before the request: no sleep before the first `IsPulseGuiding` check when the request returned at or after the end of the pulse (a blocking driver), else the rest of the pulse plus 10 ms, and the completion timeout 2 s after the end of the pulse | Direct (extract-for-testability): wx-free rule linked unmodified |
| `test_guide_algorithm_gp_params` | `src/guide_algorithm_gp_params.cpp` — the Predictive PEC (Gaussian Process) hyperparameters `set_algo_param` `periodLength` writes (#138): the current ones with only the period changed (it used to zero the signal variances and reset the length scales) and the periodic length scale taken from the profile, since `GetGPHyperparameters()` returns it wrong once it is over half the old period; run through the real `GaussianProcessGuider` and repeated without drift. Pins the helper only: the call in the wx-coupled `SetParam` is checked by the manual live test of #138 | Direct (extract-for-testability): wx-free helper linked unmodified with the contrib `GPGuider` library |
| `test_edge_allowance` | `src/edge_allowance.cpp` — how far beyond the search region auto-select keeps a star from the frame edge (#140): the calibration distance when the mount (or AO) is not calibrated or the guide request will force a recalibration (`guide` with `recalibrate: true` selects its star before it clears the old calibration), else 0; with an AO, the larger of the AO's and the mount's; and whether a star already selected (`find_star` while looping) is far enough from the edge for a forced recalibration or `guide` selects again | Direct (extract-for-testability): wx-free rule linked unmodified |
| `test_closed_loop_calibration` | `calibration_run.cpp` (`CalibrationRun`, which `Scope::BeginCalibration` / `UpdateCalibrationState` delegate to) — the calibration state machine (#142), closed loop: a SimMount (`tests/closed_loop/sim_mount.{h,cpp}`: camera angle, Dec mirror, RA image rate × cos(Dec), Dec rate, Dec backlash as dead time on each reversal, non-orthogonal axes, seeded centroid noise, the mount's coordinates and guide rates) is pulsed by the calibration frame by frame as the guider does. Pins today's behaviour: xAngle/yAngle within 0.5° and rates within 2 % over camera angles 0/30/90/200/−135° normal and mirrored, step counts, the 61-pulse RA and Dec failures, backlash clearing (cleared before the Dec rate is measured, the last clearing move as North step 1, failure after max(8, 60000/step) pulses, going on after 3 px), DEC_NONE completing a frame after the East leg, the East/South recenter pulse sizes, the South nudge (at most 4, never over a step, none off the North line, NaN angle on the lock), assumeOrthogonal, RA/Dec parity, the mount reads and where they happen, the East advisory, a clear mid-run (#145), the DEC_NONE → Auto re-entry (#148), and the high-Dec RA steps with the Dec unknown (75° takes 46 steps, 79° and up fail). `test_high_dec_calibration.cpp` (#141): `RaCalibrationStepMs` (step / cos(Dec), rounded up, at most 4x, never less than the step, the step for no or a non-finite Dec); with the mount's Dec read at Begin, every Dec 0/45/60/70/75/80/85/−80° completes in at most 40 West steps of that pulse, xRate within 2 % of the true cos(Dec) rate, the East leg sends back exactly what West sent and leaves the star within 0.5 px in RA (fast recenter on and off), North, backlash and nudge pulses stay at the step, an unknown Dec makes exactly the twin's calls (failure at 80° included), an 85° reading at the equator stays under 9 px a pulse and completes, and ±90° pulses 4x the step and fails after 61 | Every behaviour test is a typed test that runs on two implementations: the frozen twin, `legacy/scope_calibration_legacy.cpp`, a verbatim LF copy of the `scope.cpp` lines at abe96d85 before the extraction (`legacy/check_twin.sh` diffs it), compiled against mechanical wx/app stand-ins that record every scheduled move, guide-log row, event, alert, status and mount read; and the production `CalibrationRun` driven as `Scope` now drives it (`extracted_run.{h,cpp}`, recording the same calls). `test_calibration_run_parity.cpp` runs both on identical SimMounts and requires the same calls in the same order on the same frames, the same mount reads and pulses, and the same results, doubles bit for bit: over a matrix of camera angle, mirror, backlash, Dec guide mode, assumeOrthogonal, fast recenter, coordinates, pulse guiding and noise; failure and edge cases (every Dec from −89° to 89°, guide rates, parity, the star put back or bumped before each frame); a clear or a Dec mode, step or distance change before every frame of a run; and 5000 seeded random scenarios, with coverage counters so every path is taken. The differential runs the extracted run with the Dec read at Begin failing (UNKNOWN_DECLINATION), and leaves out that read's two records. Plus `CalibrationRun`'s own contract: Begin resets, Clear fails the next frame with nothing else, Complete is reported again on later frames (#148) |
| `test_guide_move_plan` | `src/guide_move_plan.cpp` — how a guide correction becomes mount pulses (#142): `PlanOffset` from `Mount::MoveOffset` (+x West, −x East, +y South, −y North; `ROUND(fabs(d / rate))`, with a zero rate giving `INT_MAX` and 0 / 0 giving 0 instead of undefined behaviour) and `PlanAxis` from `Scope::MoveAxis` (guiding disabled refuses all but manual moves, calibration moves included; only guide steps and deduced moves are filtered by the Dec guide mode and clamped to the max RA/Dec duration, recovery and calibration moves pass as asked; the limit-reached alert on the 6th clamp in a row in one direction, restarted by a clamp the other way, an unclamped or a zeroed step). `test_guide_sign_closed_loop.cpp` closes the loop through the real `calibration_transform.cpp`: a planar mount model calibrates as `Scope` measures, and a star moved West, North or both comes back to within 0.1 px in one step for camera angles 0/30/90/200/−135°, Dec normal or mirrored, and after a meridian flip with the flipped calibration (both Dec-flip modes); a stale calibration after a flip doubles the error; skewed axes converge; a runaway is corrected at most the max RA duration a step; Dec North-only and Dec None | Direct (extract-for-testability): wx-free planner linked unmodified; `Scope::MoveAxis` keeps the Debug lines, the alert and `Guide()`, `Mount::MoveOffset` the transform, algorithms and backlash comp |

## How the build infra works (and why)

Almost every production `.cpp` opens with `#include "phd.h"`, which transitively
pulls in ~30 wxWidgets headers plus `mount.h`, `myframe.h`, `image_math.h`,
`guide_algorithms.h`, etc. — basically the whole project header surface.
Linking that whole graph for a unit test would require a wxApp, a stub Mount
hierarchy, a stub MyFrame, and several hundred lines of supporting scaffold.

For this fork's tests we picked a lighter approach: most production sources
either don't actually USE anything from `phd.h` (they include it
defensively, by convention, often originally for PCH benefit), or what they
DO use is fixture-shaped — JSON in, JSON out — and can be exercised at the
byte level without instantiating the surrounding class.

So `tests/CMakeLists.txt`:

1. Sits under an `add_subdirectory(tests)` placed BEFORE the root
   `include_directories(${phd_src_dir})`, so test targets DON'T inherit
   `src/` on their include path.
2. Provides a stripped-down `tests/include/phd.h` that defines
   `PHD_H_INCLUDED` and pulls in only the std/C headers production code
   expects (`<string>`, `<algorithm>`, `<math.h>`, etc.) plus the
   `POSSIBLY_UNUSED` / `ROUND` macros that show up outside GUI code.
3. Force-includes that shadow on every test target via the
   `-include ${PHD_TESTS_INCLUDE}/phd.h` compile option. This works around
   the C/C++ standard rule that `#include "phd.h"` searches the source
   file's own directory FIRST — so `-iquote` and even `target_include_
   directories(... BEFORE ...)` lose to `src/phd.h` for files compiled out
   of `src/`. The force-include trips the header guard before the source's
   own `#include` line runs.

Net effect: `json_parser.cpp` and the test fixture compile into a tiny
binary with no wx, no globals, no Mount.

For things that genuinely need the wider surface — guide-algorithm
`result()` instantiated against a real Mount, full `event_server.cpp`
formatter exercise, calibration math against a real Scope — the math-twin
or fixture-contract style is what's in tree today; see "Deferred", below.

## Deferred / follow-up

Filed as future work, with notes in the relevant test files explaining the
specific blocker.

- **Lowpass / Lowpass2** — done. The decision math was extracted into the
  wx-free `guide_algorithm_lowpass_math.{h,cpp}` (the algorithm `.cpp` files
  carry wx GUI `ConfigDialogPane`s, so they can't be linked into a test) and
  is covered by `test_guide_algorithm_lowpass` against the real
  `WindowedAxisStats`.
- **ZFilter algorithm.** The filter-design math (`ZFilterFactory`) and the
  exact `result()` difference equation are already covered by `test_zfilter`
  (the recurrence test replicates `GuideAlgorithmZFilter::result()` verbatim).
  Only the thin `m_sumCorr` correction-feedback wrapper around it is not yet
  pulled out — a small follow-up.
- **Star detection.** The core `Star::Find` pixel math is now covered by
  `test_star_find` (extracted to the wx-free `star_find_core.{h,cpp}`,
  golden-tested against `simimage.fit`). Still deferred: `Star::AutoFind`
  (whole-frame auto-select) and `star_profile.cpp`, which are wired through
  `pFrame->pGuider` and the GUI; and the `usImage`/FITS save path.
- **Calibration math.** The camera↔mount coordinate transforms are now
  covered directly by `test_calibration_transform` (extracted to the wx-free
  `calibration_transform.{h,cpp}`), `test_guide_move_plan` checks the
  guide sign through them in a closed loop, and the calibration *sequence* (the
  `scope.cpp` state machine: RA/Dec angle and rate derivation, backlash,
  recentering, nudge) is extracted to the wx-free `calibration_run.{h,cpp}`
  and tested closed loop by `test_closed_loop_calibration`, against a
  frozen copy of the code it replaced (#142). Still deferred: the effect
  replay in `Scope::UpdateCalibrationState` (translated texts, alerts,
  `SetCalibration`/profile/sanity check on completion), which is wx-bound
  and checked live against the fake Alpaca server instead, and step-size
  selection (`calstep_dialog.cpp`), which is wired through
  `Mount`/`Scope` virtuals and the GUI.
- **End-to-end test of `CameraAlpaca::Capture()`'s bounded-retry fix
  (e7a91ddc).** Lives inside the `WorkerThread` polling loop; can't be
  unit-tested without an integration harness against a fake Alpaca
  server. The constant is currently asserted to remain at 3000 ms in
  `test_alpaca_schema.cpp::AlpacaCameraAbortContract.DocumentedBoundedRetryBehavior`
  as a deliberate placeholder.
