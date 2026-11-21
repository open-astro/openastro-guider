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
| `test_guide_algorithm_math` | `src/guide_algorithm_kernels.cpp` — the Hysteresis and ResistSwitch math (#142) on fixed scenarios: the gate, the blend, aggression, the switch after a net three, the fast switch, "not getting worse", and the ResistSwitch overshoot veto pinned explicitly (a small reversal on the committed side, a large one with the fast switch off, and a veto on the frame the history switches); plus `GuideAlgorithmIdentity::result()` | Direct for Hysteresis and ResistSwitch (links the production kernel, which replaced copies of the formulas that lacked the overshoot veto and "not getting worse"); math-twin for Identity, whose `result()` returns its input and has nothing to extract |
| `test_guide_algorithm_kernels` | `src/guide_algorithm_kernels.cpp` (the per-frame math `GuideAlgorithmHysteresis::result()`, the RA default, and `GuideAlgorithmResistSwitch::result()`, the Dec default, delegate to) with the classes' `reset()` and setters (#142): the constructor defaults (Hysteresis minMove 0.2, hysteresis 0.1, aggression 0.7; ResistSwitch minMove 0.2, aggression 1.0, fast switch on), the exclusive minMove gate, the hysteresis blend and that a gated frame clears the last move, minMove 0 blending even a zero input, the setters' ranges and fallbacks (hysteresis clipped to 0-0.99, a bad aggression or minMove reset to the default, `SetAggression` clearing the last move even when rejected, ResistSwitch's minMove > 0 and `SetMinMove` clearing the side only when accepted); ResistSwitch's ten-entry history, counting only entries strictly over minMove, the switch after a net three, "not getting worse" holding the side (a steady error on the other side under 3 × minMove is not corrected until it grows), the fast switch above 3 × minMove, the overshoot veto, a switch and a veto on the same frame; NaN and infinite inputs and a NaN hysteresis; and every Debug line, in order | Characterization and differential: typed tests that run on two implementations, the frozen twin, `legacy/hysteresis_legacy.cpp` and `legacy/resistswitch_legacy.cpp`, verbatim LF copies of the source lines at 4eac9d9c before the extraction (`legacy/check_twin.sh` diffs them), compiled against mechanical wx stand-ins (`legacy/algorithm_legacy.h`) that record each Debug, `THROW_INFO` and `ERROR_INFO` line; and production as it now is over the real kernel (`extracted_algorithms.cpp`, recording the same lines). `test_algorithm_parity.cpp` drives both with the same calls, 1000 Hysteresis and 2000 ResistSwitch seeded sequences of 300 plus a grid of every parameter corner (minMove 0, aggression 0 and the maximum, hysteresis 0 and 1, fast switch on and off), with inputs at minMove and 3 × minMove and an ulp either side, sign changes, ±0, 1e300, infinities and NaN, and resets and in- and out-of-range setter calls mixed in; after every call the results and state must match bit for bit and the Debug lines exactly (except a NaN's sign and payload, which depend on the operand order GCC picks for an fadd, not on the source), and coverage counters check every veto, the fast switch, the switch and a switch-and-veto frame are reached. The target builds with `-ffp-contract=off`, so both sides compute the source arithmetic exactly: with contraction on, GCC fuses a different product of the Hysteresis blend in the twin than it did in production's `result()` |
| `test_guiding_stats` | `src/guiding_stats.cpp` — `DescriptiveStats`, `HighPassFilter`/`LowPassFilter`, `AxisStats` (count/mean/variance/sigma/median/min-max/maxDelta/move+reversal counts/linear fit), `WindowedAxisStats` (auto-window trim, `RemoveOldestEntry`, `ChangeWindowSize`, min/max recompute on age-out) | Direct: links the production source unmodified — zero wx coupling |
| `test_zfilter` | `src/zfilterfactory.cpp` — Bessel/Butterworth filter-design invariants (order, corner, name, coefficient shape, normalized `ycoeffs[0] == -1`, finiteness) plus an end-to-end recurrence (the exact `GuideAlgorithmZFilter::result()` difference equation) proving unity-DC-gain low-pass behaviour and high-frequency attenuation | Direct: links the production source unmodified |
| `test_guide_algorithm_lowpass` | `src/guide_algorithm_lowpass_math.cpp` — the Lowpass and Lowpass2 decision math (`reset`/`result`): median + slope*weight + input clamp; warm-up attenuation, outlier-dump, drift slope, sign guard, min-move gate — run against the real `WindowedAxisStats` | Direct (extract-for-testability): math factored out of the GUI-coupled algorithm `.cpp`; production `result()`/`reset()` now delegate to it |
| `test_calibration_transform` | `src/calibration_transform.cpp` — the camera↔mount coordinate transforms extracted from `Mount`: rotate by the calibration RA-axis angle, skew the Dec component by the axis-orthogonality error, the `mountTheta` sign flip for `\|yAngleError\| > π/2`, and `YAngleError`, the axis-orthogonality error `Mount::SetCalibration` stores (0 for orthogonal axes, π for a mirrored Dec axis; #142). `test_dec_compensation.cpp`: `DecideDecComp`, what entering Guiding does with the RA rate for the Dec now (#157): a calibration at Dec 80° with the Dec moved 0.001° (any move within 0.02°, at 61–87° north or south) is None, not the "too far from equator" alert; 70°→75°, 80°→79° and back toward the equator alert; above 60° a move that changes the RA rate by 4% (|cos(now)/cos(cal) − 1|) is None and by 6% alerts, toward the pole and toward the equator; the 0.02° floor holds at 89.9°; below 60° a real move compensates (also into the high-Dec range) and a move within 0.02° keeps the calibration rate, under 0.07% off; the limit itself compensates; an unknown, NaN or infinite Dec is None; and `CompensatedRaRate`'s cos(Dec) scaling with the ±89° clamp | Direct (extract-for-testability): pure math factored out of `Mount`; `Mount::Transform*` and `Mount::AdjustCalibrationForScopePointing` delegate to it, the latter keeping the alert, the Dec comp setting and the Debug lines; verified via rotation/round-trip/skew/sign-flip invariants and the Dec comp decision table |
| `test_guiding_assistant_math` | `src/guiding_assistant_math.cpp` — the Guiding Assistant's measurement and advice extracted from `GuidingAsstWin`: per-sample accumulator (high-pass RMS, low-pass RA rate, peaks, net RA drift, linear-fit Dec drift, SNR/mass averages), Barrett polar-alignment error, seeing-based min-move (incl. sliding 2-min windows, multi-star, encoder and sanity-check fallbacks), exposure range, Dec backlash classification/rounding, likely-backlash inference, PA / Lowpass2 / binning advice, the combined `Recommend()`, `guiding_assistant_apply`'s request plan (name parsing, de-duplication, dependency order), failure message and same-session check, and the non-finite guards (0/0 drift fit while samples share one wall-clock second) | Direct (extract-for-testability): wx-free kernel linked with the real `guiding_stats.cpp`; the dialog and the headless `guiding_assistant_*` RPC controller both delegate to it |
| `test_star_find` | `src/star_find_core.cpp` — the star-detection pixel math extracted from `Star::Find`: smoothed-peak search, background-annulus mean/sigma, 3σ threshold, centroid + mass, Simonetti SNR, half-flux radius, saturation heuristic | Golden-file: links the wx-free kernel only (the real `star.cpp`/`usImage.cpp` drag in ~half the app) and runs it on the real `simimage.fit` fixture loaded with cfitsio; pins centroid/mass/SNR/HFD |
| `test_imaging_gear` | `src/imaging_gear.cpp` and `src/imaging_gear_poller.cpp` — the imaging-gear guide log lines (`design/IMAGING_GEAR_LOG.md`), pixel scale, `unknown` values, Alpaca device selection and the `set_imaging_gear_devices` overrides (win over the rules, never the guide camera, take effect on the next poll, incl. while the poll thread runs), the `get_imaging_gear` JSON (`null` for unknown values, escaping), INFO lines from changes between polls (none for a switch to another device), and that the cache never waits on a 5 s server | Direct (extract-for-testability): wx-free code linked unmodified, driven by a fake read-only `Reader` |
| `test_jsonrpc_request` | `src/jsonrpc_request.cpp` — the checks an event-server (:4400) JSON-RPC request gets before dispatch (#118): a string method (else -32600) and params that are an array or object when present (a number, string, bool or `null` gets -32602), each error carrying the request's own id (null only for a non-object batch element or a request without one), and the members read as upstream PHD2 did (last duplicate wins, only a string `method`); and the number and bool param readers the method handlers share (#138): a missing param is refused like one of the wrong type, never dereferenced (`set_algo_param` without `value` crashed the guider; the handler's -32602 answer and that the guider keeps running are checked only by `scripts/phd2_rpc_smoke.py`) | Direct (extract-for-testability): wx-free rule linked unmodified with `json_parser.cpp` |
| `test_alpaca_gain` | `src/alpaca_gain.cpp` — the Alpaca camera gain support decision from `GainMin`/`GainMax` (ASCOM value mode, used by ZWO drivers) and the 0–100 % → raw gain mapping `CameraAlpaca::Capture` sends | Direct (extract-for-testability): wx-free helper linked unmodified |
| `test_alpaca_poll` | `src/alpaca_poll.cpp` — the wait `CameraAlpaca::Capture` sleeps between `imageready` polls (#120): 20 ms until the exposure is due, then a quarter of the time waited past that, 20–250 ms; monotonic, the added latency bound, and the poll count and detection delay of simulated readouts | Direct (extract-for-testability): wx-free helper linked unmodified |
| `test_alpaca_errors` | `src/alpaca_errors.cpp` — which Alpaca capture errors make `CameraAlpaca::Capture` disconnect the camera with reconnect (#103): the `CaptureFailureTracker` fed by every capture call (`binx`/`biny`, `startexposure`, `imageready`, an ImageBytes error payload, a JSON `imagearray` error) reconnects on 1031 NotConnected at once and on the 3rd consecutive 1280 acquisition failure, counted across capture attempts; a single 1280 and other errors keep the alert; a good frame, Connect, Disconnect and a reconnect start a new run, other errors neither count nor reset; and `ReadError`, the `ErrorNumber`/`ErrorMessage` reader behind the JSON `imagearray` check and `AlpacaClient`'s error handling (float coercion, missing or mistyped fields); `CallErrorReason`, the one-line reason of a failed `AlpacaClient` call (#97: Alpaca number and message, HTTP status with a short non-HTML body, no response, invalid response), `OneLine` (whitespace and control characters collapsed, invalid UTF-8 replaced, capped without splitting a UTF-8 sequence) and `CallErrorScope`, the guard that fills the call's `CallError` on every return | Direct (extract-for-testability): wx-free rule and reader linked unmodified with `json_parser.cpp` |
| `test_capture_failure` | `src/capture_failure.cpp` — the text a failed capture reports to clients (#97): `"camera error: <reason>"` from the failed Alpaca call or else the driver's alert, exactly `"camera error -- see debug log"` with no reason, the latest failed call wins and the first driver alert is kept, blank or invalid-UTF-8 input never gives an empty text, long text capped, `AlertWithReason` (`"<prefix>: <reason>"`), and the calibration frame retry (#116) and guiding frame drop (#136): `IsTransient` (Alpaca 1280 and a timed-out call; not 1031, other Alpaca errors, HTTP, parse, other transport failures, a driver alert alone or no failure), `PhaseFor` (the phase of each guider state: calibrating states retry, `STATE_GUIDING` drops, `STATE_CALIBRATED` and every other state stop), and `ShouldRetryFrame` (a decision table over phase × failure × count × capturing × connected: only while calibrating or guiding, never looping or with a star selected, capturing and connected, at most `kFrameRetries` = 2 in a row, so the 3rd 1280 in a row still reaches the #103 reconnect), and `FrameRetries`, the count `MyFrame` keeps (a good frame or a failure not retried starts a new count, a reconnect only once per run of failed frames, so a camera that fails every frame ends the calibration, or stops guiding, after 7 frames and 2 reconnects) | Direct (extract-for-testability): wx-free helper linked unmodified with `alpaca_errors.cpp` |
| `test_settle_frame` | `src/settle_frame.cpp` — what one guide frame does to a guide or dither settle (`PhdController`'s `STATE_SETTLE_WAIT`): `TotalFrames` / `DroppedFrames`, the run of in-range frames timed from its first frame (an out-of-range or lost-star frame ends it, an AO bump holds it, no settle time settles on the first in-range frame), the frame limit before the run before the timeout, and a frame dropped after a transient capture failure while guiding (#136): counted like a lost-star frame, it neither ends nor completes a run, starts none, reports the run's time so far, and still times out or reaches the frame limit | Direct (extract-for-testability): wx-free rule linked unmodified |
| `test_dither_random` | `src/dither_random.cpp` — the offsets of a random dither (`MyFrame::Dither`, `DITHER_RANDOM`) from a generator of its own (#173): two generators made from the same `rand()` state, as two daemon starts are, give different first dithers (the bug: RA +2.04, Dec −0.63 px every start); an injected seed gives the `std::mt19937` sequence mapped as `rand() / RAND_MAX * 2 - 1` was (pinned to the standard's first outputs for seed 5489); the C library's `rand`/`srand`, which the simulator camera calls, do not move it; an RA-only dither has Dec 0 and draws nothing for it; each axis uniform over [−amount, +amount] out to both ends (bins, means, no RA/Dec correlation) | Direct (extract-for-testability): wx-free rule linked unmodified |
| `test_connect_request` | `src/connect_request.cpp` — per-device `set_connected` / `get_connected` (#84): device names, and when a request is refused (camera and `all` need capture stopped; mount and rotator may join while looping but not while calibrating or guiding; nothing while the gear dialog is open) | Direct (extract-for-testability): wx-free rules linked unmodified |
| `test_connect_retry` | `src/connect_retry.cpp` — recovery of a selected device that failed to connect (#104): the retry schedule (15 s, 30 s, then every 60 s, unbounded), the per-device state (a failure schedules, a recovery reports `EquipmentReconnected` exactly once, a user disconnect stops retries but keeps the failure, re-selection forgets, "not configured" parks until the Alpaca server changes, devices independent, stale probe results dropped by token), the gates that make an attempt wait (shutdown, dark build, PA session, Guiding Assistant, a connect on the stack, and the `set_connected` refusal rules), the probe sequence against a scripted server (already connected, unreachable, refused with the server's reason, waits for connected, gives up after its bounded wait, cancel), the `guide` refusal text and the driver reason slot | Direct (extract-for-testability): wx-free rules linked unmodified with `connect_request.cpp` and `alpaca_errors.cpp` |
| `test_atomic_file_write` | `src/atomic_file_write.cpp` — how `PhdConfig::Flush` writes the config file (#142): through `<file>.tmp` in the same directory, fsynced, renamed over the file and the directory fsynced. A new file gets 0666 less the umask, a replaced one keeps its mode; empty and 3 MB contents; a temporary file a killed write left is overwritten; a write that cannot create its temporary file or cannot rename it leaves the old file and no temporary file, with the step and path in the error; a missing directory fails; a relative path is written in the current directory; and a reader thread never finds the file missing or partly written over 200 rewrites, which the delete-then-rename of wxWidgets before 3.2.8 could show. A power cut cannot be made here; `scripts/e2e_persistence.py` checks the real daemon's syscalls under strace | Direct (extract-for-testability): POSIX, wx-free, linked unmodified |
| `test_restore_calibration` | `src/restore_calibration.cpp` — when `restore_calibration` (#102) is refused (calibrating or guiding; the mount, AO or AO-rig scope not connected; a guide or dither request in flight that is not waiting on a calibration), returns `false` with nothing changed (some mount has no saved calibration, so no partial AO restore), or ends the guide request a failed calibration left waiting before it restores; when `clear_calibration` is refused (calibrating, #137) | Direct (extract-for-testability): wx-free rules linked unmodified |
| `test_pulse_wait` | `src/pulse_wait.cpp` — how long `ScopeAlpaca::Guide` waits for an Alpaca `PulseGuide` (#119), timed from just before the request: no sleep before the first `IsPulseGuiding` check when the request returned at or after the end of the pulse (a blocking driver), else the rest of the pulse plus 10 ms, and the completion timeout 2 s after the end of the pulse | Direct (extract-for-testability): wx-free rule linked unmodified |
| `test_guide_algorithm_gp_params` | `src/guide_algorithm_gp_params.cpp` — the Predictive PEC (Gaussian Process) hyperparameters `set_algo_param` `periodLength` writes (#138): the current ones with only the period changed (it used to zero the signal variances and reset the length scales) and the periodic length scale taken from the profile, since `GetGPHyperparameters()` returns it wrong once it is over half the old period; run through the real `GaussianProcessGuider` and repeated without drift. Pins the helper only: the call in the wx-coupled `SetParam` is checked by the manual live test of #138 | Direct (extract-for-testability): wx-free helper linked unmodified with the contrib `GPGuider` library |
| `test_edge_allowance` | `src/edge_allowance.cpp` — how far beyond the search region auto-select keeps a star from the frame edge (#140): the calibration distance when the mount (or AO) is not calibrated or the guide request will force a recalibration (`guide` with `recalibrate: true` selects its star before it clears the old calibration), else 0; with an AO, the larger of the AO's and the mount's; and whether a star already selected (`find_star` while looping) is far enough from the edge for a forced recalibration or `guide` selects again | Direct (extract-for-testability): wx-free rule linked unmodified |
| `test_closed_loop_calibration` | `calibration_run.cpp` (`CalibrationRun`, which `Scope::BeginCalibration` / `UpdateCalibrationState` delegate to) — the calibration state machine (#142), closed loop: a SimMount (`tests/closed_loop/sim_mount.{h,cpp}`: camera angle, Dec mirror, RA image rate × cos(Dec), Dec rate, Dec backlash as dead time on each reversal, non-orthogonal axes, seeded centroid noise, the mount's coordinates and guide rates) is pulsed by the calibration frame by frame as the guider does. Pins today's behaviour: xAngle/yAngle within 0.5° and rates within 2 % over camera angles 0/30/90/200/−135° normal and mirrored, step counts, the 61-pulse RA and Dec failures, backlash clearing (cleared before the Dec rate is measured, the last clearing move as North step 1, failure after max(8, 60000/step) pulses, going on after 3 px), DEC_NONE completing a frame after the East leg, the East/South recenter pulse sizes, the South nudge (at most 4, never over a step, none off the North line, NaN angle on the lock; #154: with the star at exact positions where the nudge cosine rounds past -1 whatever the compiler fuses, the run nudges back to the lock where the twin-only `LegacyTwin` test keeps the twin's NaN stop, and past +1 it stops with angle 0), assumeOrthogonal, RA/Dec parity, the mount reads and where they happen, the East advisory, a clear mid-run (#145), DEC_NONE switched to Auto/North/South on the completing frame failing once with `DecModeChanged` and storing nothing (#148; the twin-only `LegacyTwin` test keeps the twin's re-entry on every frame), Auto switched to None after the Dec leg still completing, and the high-Dec RA steps with the Dec unknown (75° takes 46 steps, 79° and up fail). `test_high_dec_calibration.cpp` (#141): `RaCalibrationStepMs` (step / cos(Dec), rounded up, at most 4x and at most 25 s unless the step is already longer, never less than the step, the step for no or a non-finite Dec); with the mount's Dec read at Begin, every Dec 0/45/60/70/75/80/85/−80° completes in at most 40 West steps of that pulse, xRate within 2 % of the true cos(Dec) rate, the East leg sends back exactly what West sent and leaves the star within 0.5 px in RA (fast recenter on and off), North, backlash and nudge pulses stay at the step, an unknown Dec makes exactly the twin's calls (failure at 80° included), an 85° reading at the equator stays under 9 px a pulse and completes, and ±90° pulses 4x the step and fails after 61; backlash clearing (#156): `BacklashClearingStepPx` (xRate / max(|cos(Dec)|, 0.01) × step × 0.6, the RA-rate step bit for bit for no or a non-finite Dec, finite at the pole), the "Looking for 3 moves of 1.3 px" Debug line at Dec 0/60/80/85/−80° and half of it for a Dec guide rate half the RA one, the twin's step for an unknown Dec, 20 seeds at 80° with 2500 ms backlash and 0.2 px noise accepting no clearing move for a pulse the backlash took whole and measuring yRate within −3 %/+5 % (the twin at 0° for contrast), and an 85° reading at the equator accepting no move and going on after the calibration distance, with what a wrong reading costs: at the equator with 1500 ms backlash, readings of 55/60/85/90° send 14850 ms North for 8800 ms South, take the star 51.8 px from the lock (30.4 px with the right reading) and leave it 23.5 px from it (under 2.5 px with the right reading) | Every behaviour test is a typed test that runs on two implementations: the frozen twin, `legacy/scope_calibration_legacy.cpp`, a verbatim LF copy of the `scope.cpp` lines at abe96d85 before the extraction (`legacy/check_twin.sh` diffs it), compiled against mechanical wx/app stand-ins that record every scheduled move, guide-log row, event, alert, status and mount read; and the production `CalibrationRun` driven as `Scope` now drives it (`extracted_run.{h,cpp}`, recording the same calls). `test_calibration_run_parity.cpp` runs both on identical SimMounts and requires the same calls in the same order on the same frames, the same mount reads and pulses, and the same results, doubles bit for bit: over a matrix of camera angle, mirror, backlash, Dec guide mode, assumeOrthogonal, fast recenter, coordinates, pulse guiding and noise; failure and edge cases (every Dec from −89° to 89°, guide rates, parity, the star put back or bumped before each frame); a clear or a Dec mode, step or distance change before every frame of a run; and 5000 seeded random scenarios, with coverage counters so every path is taken. The intended differences are #148: where the twin stored an RA-only calibration while the Dec guide mode needed Dec, the run must fail with `DecModeChanged` on that frame and match the twin exactly before it (a counter checks the sweeps reach it); and #154: where the twin's nudge angle was NaN for a cosine rounded past -1 or +1 and the run's clamped one is 180° or 0°, the run must match the twin exactly before it, at 0° everywhere else too, and at 180° may only add the nudge's South pulses (`NudgeCosinePastOne` reaches both with exact positions; noiseless scenarios reach it as the compiler rounds). The differential runs the extracted run with the Dec read at Begin failing (UNKNOWN_DECLINATION), and leaves out that read's two records. Plus `NudgeAngle` (#154: a cosine rounded past -1 or +1 gives 180° or 0°, NaN direction cosines stay NaN) and `CalibrationRun`'s own contract: Begin resets, Clear fails the next frame with nothing else, Complete is reported again on later frames, and a DEC_NONE run whose mode guides Dec at Complete fails with `DecModeChanged` with no effects or mount reads and never reports Complete (#148) |
| `test_guide_move_plan` | `src/guide_move_plan.cpp` — how a guide correction becomes mount pulses (#142): `PlanOffset` from `Mount::MoveOffset` (+x West, −x East, +y South, −y North; `ROUND(fabs(d / rate))`, with a zero rate giving `INT_MAX` and 0 / 0 giving 0 instead of undefined behaviour) and `PlanAxis` from `Scope::MoveAxis` (guiding disabled refuses all but manual moves, calibration moves included; only guide steps and deduced moves are filtered by the Dec guide mode and clamped to the max RA/Dec duration, recovery moves and AO bumps ignore both on purpose, calibration and manual moves pass as asked; every guide step, deduced move, recovery move and AO bump is capped at `MAX_PULSE_MS` (25 s, under the Alpaca 30 s HTTP timeout), so a zero-rate recenter no longer sends `INT_MAX` (#152); the limit-reached alert on the 5th clamp in a row in one direction (#153; it was the 6th), restarted by a clamp the other way, an unclamped or a zeroed step); and `LimitAlertThrottle` from `Scope::AlertLimitReached` on an injected clock (#164): no alert during the grace period, at most one every 30 s per axis, so an RA alert does not hold back a Dec one or the other way round, and only a raised alert restarts the 30 s, so neither a held call nor a guide step during the grace period holds back the next alert). `test_guide_sign_closed_loop.cpp` closes the loop through the real `calibration_transform.cpp`: a planar mount model calibrates as `Scope` measures, and a star moved West, North or both comes back to within 0.1 px in one step for camera angles 0/30/90/200/−135°, Dec normal or mirrored, and after a meridian flip with the flipped calibration (both Dec-flip modes); a stale calibration after a flip doubles the error; skewed axes converge; a runaway is corrected at most the max RA duration a step; Dec North-only and Dec None | Direct (extract-for-testability): wx-free planner linked unmodified; `Scope::MoveAxis` keeps the Debug lines, the alert and `Guide()` (`AlertLimitReached` the clock and the alert texts), `Mount::MoveOffset` the transform, algorithms and backlash comp |
| `test_guiding_rms_baseline` | The default guiding chain closed loop (#142): `calibration_transform.cpp` (`CameraToMount`), `guide_algorithm_kernels.cpp` (Hysteresis on RA, ResistSwitch on Dec, with the production constructor defaults) and `guide_move_plan.cpp` (`PlanOffset`, `PlanAxis` with 2500 ms max pulses, Dec mode Auto), guiding a simulated star: sinusoidal RA periodic error (480 s, ±10"), constant RA and Dec drift (0.01 and 0.02 "/s), 0.5" Gaussian seeing per image axis, a 1.93"/px camera at 30°, 2 s exposures plus 0.5 s overhead, then the RA and Dec pulses at 0.5× sidereal while the sky keeps moving. Pins the unguided RMS of that sky (19.89": RA 10.36", Dec 16.98"); guiding at under a tenth of it on every seed; the guided RMS of the exposure-averaged true error in a ±11 % band around the measured baseline (20-seed means total 0.777", RA 0.510", Dec 0.586", measured at -O0 and -O3 on GCC 14.2, where they agree to the fourth decimal); every one of 60 seeds within 0.62-0.95" and within 1.35× of each other; the band catching a detuned default (RA aggression halved or doubled, RA minMove 0.5 px, hysteresis 0.5, Dec aggression 0.5); and a calibration flipped by 180° on either axis diverging to thousands of arcsec while the other axis still guides | Closed-loop simulation, deterministic (mt19937_64 and Box-Muller, so the noise is the same on every standard library), about 30 ms. Proves signs, wiring, gains and gating; it cannot prove the defaults are well tuned for a real mount, seeing or backlash, which needs rig data |

## Daemon end-to-end (CI)

Not a ctest target: the `Daemon e2e (arm64, ASan/UBSan)` CI job builds
`phd2.bin` with `-fsanitize=address,undefined` in its own build dir and runs
`scripts/e2e_guide_scenarios.py` (#142). The script starts
`scripts/fake_alpaca_camera.py` and the daemon (headless under `xvfb-run`,
scratch `HOME`) and drives it over JSON-RPC, in two lanes at once, each with its
own fake and daemon (the physics lane on instance, fake port and display + 1, its
logs in `physics/`), so the second lane should add little wall time (locally,
under ASan on 8 cores, 289 s for both against 287 s for the main lane alone; on
the 4-vCPU CI runner that is still to be read off the first run's log):

| Scenario | Checks |
|---|---|
| smoke | `scripts/phd2_rpc_smoke.py --fake-alpaca-url` exits 0 (#155) |
| calibrate and guide | connect the Alpaca camera and mount, `loop`, `find_star`, `guide` with `recalibrate: true`: a full calibration (West, East, North, South), one `SettleDone` with `Status` 0, then `GuideStep`s; the event order Ara consumes (`StartCalibration`, `Calibrating`…, `CalibrationComplete`, `StartGuiding`, `SettleBegin`, `Settling`/`GuideStep`…, one `SettleDone`, `GuideStep`…) |
| recalibrate on a new target | `stop_capture`, slew the fake mount to Dec 50, `guide` with `recalibrate: true`: a second full calibration, made at Dec 50 |
| frame drops while guiding | 1, then 2 frames in a row failing with Alpaca 1280: one `StarLost` each, guiding goes on (#136) |
| physics: calibration truth | the fake with its mount and sky physics on (a 37° camera, RA moving cos(Dec) as far, 800 ms Dec backlash, drift and RA periodic error in simulated time, 0.15 px seeing, a new noise background each frame, sideofpier following the slews; seeded): slew to Dec 60, `loop`, `find_star`, `guide` with `recalibrate: true`; `get_calibration_data` `xAngle`/`yAngle` within 2° of `GET /fake/truth` and `xRate`/`yRate` within 5 %, plus the full calibration and the guide request's event order as above |
| physics: dither and settle | on that calibration, `dither` 3 px (random offsets, different on each run since #173) with a settle: `LockPositionSet`, `GuidingDithered`, `SettleBegin`, `Settling`/`GuideStep`…, exactly one `SettleDone` with `Status` 0; the lock moves by the dither mapped through the fake's true camera angles (as a vector, within the 2° the calibration may be off), the first `GuideStep` is the lock's move away from the new lock (as a vector) and the last before `SettleDone` within the settle pixels, and the fake's true star followed the lock to within 0.75 px |
| physics: clouds | `PUT /fake/clouds` hides the stars for 5 frames while guiding: 5 `StarLost`, each the guider finding no star (`ErrorCode` 2 "Star lost - low SNR" or 3 "Star lost - low mass"; not a mass change, not a camera error), `get_app_state` `LostLock` or `Guiding` meanwhile, guiding not stopped, and a `GuideStep` on the first clear frame |

The job fails on a failed check, on the daemon crashing or exiting non-zero
after `shutdown`, and on any sanitizer report; the daemon's debug log and
output, the fake's log and every event received are uploaded when it does.
Two checks are left out: LeakSanitizer is off (`detect_leaks=0`; the
wxWidgets/GTK and PHD2 singletons are never freed, by design), and the UBSan
vptr check is not compiled in (`-fno-sanitize=vptr`; see
`scripts/e2e_ubsan.supp`).
Locally, after an ASan build (see the job's Configure step):

```bash
python3 scripts/e2e_guide_scenarios.py --phd2 build-asan/phd2.bin \
    --instance 10 --fake-port 11150 --display 110 --work-dir /tmp/e2e
```

That also takes instance 11, fake port 11151 and display 111 for the physics
lane; `--lane main` or `--lane physics` runs one lane only. With `--display auto`
each lane takes a free display through `xvfb-run -a`, the physics lane searching
from 100 numbers higher (`--auto-display-from`), so two lanes starting at once
never take the same one. The fake's physics
options (`--camera-angle`, `--dec-mirror`, `--cos-dec`, `--dec-backlash-ms`,
`--sim-drift`, `--pe-px`, `--seeing-px`, `--frame-noise`, `--pier-flip`,
`--seed`), `/fake/clouds`, `/fake/truth` and the telescope faults
(`/fake/inject` with `device=telescope`) are described in its docstring; all are
off by default, and the main lane runs on the plain fake.

### Settings persistence

The same job then runs `scripts/e2e_persistence.py` (#142), which needs no
calibration. RC51 gets its power cut in the field, so a setting changed over
JSON-RPC has to survive both a clean stop (SIGTERM, what `systemctl stop`
sends) and a hard kill (SIGKILL, the stand-in for a power cut). Two passes run
at once, one per signal, each with its own instance, fake port and display:

| Step | Checks |
|---|---|
| set | a distinctive non-default value through each method below, read back at once. The SIGKILL pass also requires the config file on disk to have changed by the time each call returns, so a later call that writes the file cannot hide one that does not |
| stop | SIGTERM or SIGKILL to `phd2.bin` itself; after SIGTERM it must exit 0 |
| restart | the same scratch `HOME` and instance, connect again, read every value back; each one that did not survive is reported with its method name |
| config write | the first daemon of each pass runs under `strace -ff`: every rename onto the config file must be of a file fsynced first, the config file must never be deleted, and its directory must be fsynced after the rename, so a power cut at any point leaves the old or the new file whole |
| replace the config | on the config the two runs left, under `strace`: the SIGTERM pass runs `phd2.bin -s <file>`, then `-l <file>` twice (a provisioning script restoring a saved config; the second load restores what is already there), and the SIGKILL pass starts the daemon with `-R` (Reset Configuration) and requires one default profile. Each must exit 0 and leave the config file in place, never deleted, with the profiles `-l` restored |

Methods checked: before connecting, `create_profile`, `clone_profile`,
`rename_profile`, `delete_profile`, `set_profile_by_name` and `set_profile`,
`set_alpaca_server`, `set_selected_camera`, `set_selected_mount`,
`set_camera_bitdepth`, `set_profile_setup`, `set_dark_auto_load`,
`set_dither_settings`, `set_star_detection`, `set_auto_exposure`,
`set_noise_reduction`, `set_guider_options`, `set_variable_delay_settings`;
connected, `set_algo`, `set_algo_param`, `set_exposure`, `set_dec_guide_mode`,
`set_guide_limits`, `set_dec_comp`, `set_mount_options`, `set_backlash_comp`,
`set_camera_gain`, `set_camera_timeout`, `set_use_subframes`,
`set_camera_saturation`. The script's docstring lists the methods left out
and why (session state, hardware the fake lacks, or a calibration, Guiding
Assistant run or dark frames needed first). A pass takes a few seconds
locally (about 6 s under ASan). Locally (it needs `strace`):

```bash
python3 scripts/e2e_persistence.py --phd2 build-asan/phd2.bin \
    --instance 11 --fake-port 11151 --display 111 --work-dir /tmp/e2e-persistence
```

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
- **Hysteresis / ResistSwitch** — done. The per-frame math was extracted into
  the wx-free `guide_algorithm_kernels.{h,cpp}` (#142) and is covered by
  `test_guide_algorithm_math` and `test_guide_algorithm_kernels`, the latter
  against a frozen copy of the code it replaced; `test_guiding_rms_baseline`
  guides a simulated star with it.
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
  frozen copy of the code it replaced (#142). The Dec compensation decision
  `Mount::AdjustCalibrationForScopePointing` makes on each entry to Guiding is
  in `calibration_transform` too, tested there (#157). Still deferred: the effect
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
