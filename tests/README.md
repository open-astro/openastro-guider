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
| `test_jsonrpc_schema` | `src/event_server.cpp`'s outgoing message shapes — `Version`, `AppState`, `SettleDone`, `Settling`, `StarSelected`, JSON-RPC envelope, `get_profiles`, `get_profile`, `get_lock_shift_params` | Fixture-based contract: pins the documented field names so a rename in either the production formatter or a downstream consumer (NINA, KStars, web UI) becomes a visible PR diff |
| `test_alpaca_schema` | `src/alpaca_client.cpp` and `src/alpaca_discovery.cpp` JSON shapes — Alpaca standard envelope, `ErrorNumber`/`ErrorMessage` extraction (incl. lenient float→int coerce), camera/telescope `Value` shapes, the property-name fallback for non-standard servers, management API responses, the discovery UDP `{"AlpacaPort": N}` reply | Fixture-based contract |
| `test_discovery_logic` | `host:port` parsing and the `std::set` dedupe model shared by `AlpacaDiscovery::DiscoverServers` and `INDIDiscovery::DiscoverServers` | Model: reproduces the dedupe / parse semantics in `std::string` form, so the actual sockets-bound functions don't have to be runnable in a unit-test process |
| `test_guide_algorithm_math` | `result()` / `reset()` for `GuideAlgorithmIdentity`, `GuideAlgorithmHysteresis`, `GuideAlgorithmResistSwitch` | Math-twin: each algorithm's formula is reimplemented in the test alongside the production line numbers; tests pin the input/output curve on a fixed scenario |
| `test_guiding_stats` | `src/guiding_stats.cpp` — `DescriptiveStats`, `HighPassFilter`/`LowPassFilter`, `AxisStats` (count/mean/variance/sigma/median/min-max/maxDelta/move+reversal counts/linear fit), `WindowedAxisStats` (auto-window trim, `RemoveOldestEntry`, `ChangeWindowSize`, min/max recompute on age-out) | Direct: links the production source unmodified — zero wx coupling |
| `test_zfilter` | `src/zfilterfactory.cpp` — Bessel/Butterworth filter-design invariants (order, corner, name, coefficient shape, normalized `ycoeffs[0] == -1`, finiteness) plus an end-to-end recurrence (the exact `GuideAlgorithmZFilter::result()` difference equation) proving unity-DC-gain low-pass behaviour and high-frequency attenuation | Direct: links the production source unmodified |
| `test_polar_alignment` | `src/staticpa_geometry.cpp` — the Static PA geometry/astrometry extracted from `StaticPaToolWin`: circle-from-3-points, circle-from-2-points-plus-rotation, CoR Dec/Cone decomposition, reference-star projection (`Radec2Px`), Alt/Az error decomposition, and IAU-2000 precession (`J2000Now`) | Direct (extract-for-testability): pure kernels factored out of the wxFrame and linked unmodified; verified via geometric invariants |
| `test_guide_algorithm_lowpass` | `src/guide_algorithm_lowpass_math.cpp` — the Lowpass and Lowpass2 decision math (`reset`/`result`): median + slope*weight + input clamp; warm-up attenuation, outlier-dump, drift slope, sign guard, min-move gate — run against the real `WindowedAxisStats` | Direct (extract-for-testability): math factored out of the GUI-coupled algorithm `.cpp`; production `result()`/`reset()` now delegate to it |

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
- **Star detection (`star.cpp`, `star_profile.cpp`).** Uses `usImage`
  which is wx + cfitsio coupled. The repo already ships `savetest.fit`
  and `simimage.fit` as fixtures — the right shape for these is a
  golden-file harness that drives a built `phd2.bin`, probably as a small
  Python script invoked from `ctest`.
- **Calibration math (`calibration_assistant.cpp`, `backlash_comp.cpp`).**
  Sign conventions, RA/Dec angle, and step-size selection — high-value
  surface but the math is wired through `Mount` and `Scope` virtuals.
  Either revisit the full stub-layer approach or extract the pure math
  into a header in a follow-up PR.
- **End-to-end test of `CameraAlpaca::Capture()`'s bounded-retry fix
  (e7a91ddc).** Lives inside the `WorkerThread` polling loop; can't be
  unit-tested without an integration harness against a fake Alpaca
  server. The constant is currently asserted to remain at 3000 ms in
  `test_alpaca_schema.cpp::AlpacaCameraAbortContract.DocumentedBoundedRetryBehavior`
  as a deliberate placeholder.
