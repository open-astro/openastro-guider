# Unit tests for PHD2 fork
#
# Strategy: most production .cpp files include "phd.h", which transitively
# pulls in wxFrame/Mount/MyFrame and most of the project. To keep test
# binaries small and the test surface focused, we use header substitution:
# tests/include/ comes first on the include path, so production .cpp files
# doing `#include "phd.h"` land on tests/include/phd.h — a minimal header
# that brings in the std/C headers production code expects but skips the
# wx/Mount/MyFrame mountain. The real production .cpp files compile
# unmodified.
#
# Pure-data files (json_parser.cpp) just need the std headers and that's
# the end of the story. Heavier tests (guide algorithms) include
# test_phd_full.h on top, which layers on stub Mount/MyFrame/Debug.

set(PHD_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PHD_TESTS_INCLUDE ${PHD_TESTS_DIR}/include)
set(PHD_TESTS_STUBS ${PHD_TESTS_DIR}/stubs)

# Common interface for every test target: gtest + std headers.
add_library(phd_test_common INTERFACE)
target_link_libraries(phd_test_common INTERFACE GTest::gtest GTest::gtest_main)
target_compile_features(phd_test_common INTERFACE cxx_std_20)

# C/C++ standard says #include "phd.h" finds the source file's own
# directory first - so for json_parser.cpp in src/, the real src/phd.h
# always wins over our shadow no matter what -iquote does. Force-include
# the shadow first so PHD_H_INCLUDED is already set when the production
# source's #include "phd.h" line runs, making it a no-op.
#
# Force-include flag is compiler-specific: GCC/Clang use `-include FILE`,
# MSVC uses `/FI FILE`. MSVC silently ignores `-include` with warning
# D9002, so passing the wrong flag produces a confusing missing-header
# error rather than a clear failure.
if(MSVC)
  target_compile_options(phd_test_common INTERFACE /FI${PHD_TESTS_INCLUDE}/phd.h)
else()
  target_compile_options(phd_test_common INTERFACE -include ${PHD_TESTS_INCLUDE}/phd.h)
endif()
target_include_directories(phd_test_common INTERFACE
  ${PHD_TESTS_INCLUDE}
  ${phd_src_dir}
)

############################################################################
# test_json_parser  --  json_parser.cpp is std-only, so it just needs the
# minimal phd.h substitute.
############################################################################
add_executable(test_json_parser
  ${PHD_TESTS_DIR}/json/test_json_parser.cpp
  ${phd_src_dir}/json_parser.cpp
)
target_link_libraries(test_json_parser PRIVATE phd_test_common)
set_property(TARGET test_json_parser PROPERTY FOLDER "Unit tests")
add_test(NAME test_json_parser COMMAND test_json_parser)

############################################################################
# test_jsonrpc_schema  --  pins the canonical event-server JSON-RPC
# message shapes from the consumer's POV. event_server.cpp has too much
# Mount/Camera coupling to call its formatters directly; this fixture-
# based contract test keeps documented field names from silently rotting.
############################################################################
add_executable(test_jsonrpc_schema
  ${PHD_TESTS_DIR}/event_server/test_jsonrpc_schema.cpp
  ${phd_src_dir}/json_parser.cpp
)
target_link_libraries(test_jsonrpc_schema PRIVATE phd_test_common)
set_property(TARGET test_jsonrpc_schema PROPERTY FOLDER "Unit tests")
add_test(NAME test_jsonrpc_schema COMMAND test_jsonrpc_schema)

############################################################################
# test_alpaca_schema  --  pins the Alpaca request/response shapes the
# alpaca_client / alpaca_discovery modules read and write. Same pattern as
# test_jsonrpc_schema: full round-trip would need wxApp + curl + a mock
# server; here we contract-test the documented JSON shapes via JsonParser.
############################################################################
add_executable(test_alpaca_schema
  ${PHD_TESTS_DIR}/alpaca/test_alpaca_schema.cpp
  ${phd_src_dir}/json_parser.cpp
)
target_link_libraries(test_alpaca_schema PRIVATE phd_test_common)
set_property(TARGET test_alpaca_schema PROPERTY FOLDER "Unit tests")
add_test(NAME test_alpaca_schema COMMAND test_alpaca_schema)

############################################################################
# test_discovery_logic  --  models the host:port parse and dedupe-set
# semantics shared by AlpacaDiscovery and INDIDiscovery, without needing
# real sockets.
############################################################################
add_executable(test_discovery_logic
  ${PHD_TESTS_DIR}/discovery/test_discovery_logic.cpp
)
target_link_libraries(test_discovery_logic PRIVATE phd_test_common)
set_property(TARGET test_discovery_logic PROPERTY FOLDER "Unit tests")
add_test(NAME test_discovery_logic COMMAND test_discovery_logic)

############################################################################
# test_guide_algorithm_math  --  links the REAL guide_algorithm_kernels.cpp:
# the Hysteresis and ResistSwitch math on fixed scenarios, the ResistSwitch
# overshoot veto pinned explicitly (#142). Identity's one-line result() is
# still mirrored in the test.
############################################################################
add_executable(test_guide_algorithm_math
  ${PHD_TESTS_DIR}/algorithms/test_guide_algorithm_math.cpp
  ${phd_src_dir}/guide_algorithm_kernels.cpp
)
target_link_libraries(test_guide_algorithm_math PRIVATE phd_test_common)
set_property(TARGET test_guide_algorithm_math PROPERTY FOLDER "Unit tests")
add_test(NAME test_guide_algorithm_math COMMAND test_guide_algorithm_math)

############################################################################
# test_guide_algorithm_kernels  --  links the REAL guide_algorithm_kernels.cpp,
# the per-frame math of the Hysteresis and ResistSwitch guide algorithms
# (#142), the RA and Dec defaults. Every characterization test is a typed
# test that runs on a frozen copy of the production result(), reset() and
# setters before the extraction (tests/algorithms/legacy, compiled against
# mechanical wx stand-ins; check_twin.sh diffs it against the source) and on
# production as it now is over the kernel (extracted_algorithms.cpp); a
# differential test drives both with seeded call sequences and requires
# identical results, state and Debug lines, doubles bit for bit.
############################################################################
add_executable(test_guide_algorithm_kernels
  ${PHD_TESTS_DIR}/algorithms/test_algorithm_characterization.cpp
  ${PHD_TESTS_DIR}/algorithms/test_algorithm_parity.cpp
  ${PHD_TESTS_DIR}/algorithms/extracted_algorithms.cpp
  ${PHD_TESTS_DIR}/algorithms/legacy/algorithm_twin.cpp
  ${PHD_TESTS_DIR}/algorithms/legacy/hysteresis_legacy.cpp
  ${PHD_TESTS_DIR}/algorithms/legacy/resistswitch_legacy.cpp
  ${phd_src_dir}/guide_algorithm_kernels.cpp
)
target_link_libraries(test_guide_algorithm_kernels PRIVATE phd_test_common)
# GCC in GNU mode (gnu++20) and Clang contract a * b + c * d into an FMA, and which product
# they fuse depends on the code around it: the frozen twin's copy of the Hysteresis blend is
# fused on the other product than production's result() was, so the two would differ by an
# ulp for reasons that are not in the source. With contraction off, both sides compute the
# source arithmetic exactly and the differential compares that, bit for bit.
if(NOT MSVC)
  target_compile_options(test_guide_algorithm_kernels PRIVATE -ffp-contract=off)
endif()
set_property(TARGET test_guide_algorithm_kernels PROPERTY FOLDER "Unit tests")
add_test(NAME test_guide_algorithm_kernels COMMAND test_guide_algorithm_kernels)

############################################################################
# test_guiding_stats  --  links the REAL production guiding_stats.cpp, which
# has zero wx/Mount coupling. Exercises DescriptiveStats, the HPF/LPF
# filters, AxisStats (incl. linear fit) and WindowedAxisStats windowing.
############################################################################
add_executable(test_guiding_stats
  ${PHD_TESTS_DIR}/guiding_stats/test_guiding_stats.cpp
  ${phd_src_dir}/guiding_stats.cpp
)
target_link_libraries(test_guiding_stats PRIVATE phd_test_common)
set_property(TARGET test_guiding_stats PROPERTY FOLDER "Unit tests")
add_test(NAME test_guiding_stats COMMAND test_guiding_stats)

############################################################################
# test_guide_history  --  links the REAL production guide_history.cpp (the
# guide-step history and summary stats behind get_guide_history, moved out
# of the History graph window in #115 step 2) against the REAL
# guiding_stats.cpp. No wx coupling.
############################################################################
add_executable(test_guide_history
  ${PHD_TESTS_DIR}/guide_history/test_guide_history.cpp
  ${phd_src_dir}/guide_history.cpp
  ${phd_src_dir}/guiding_stats.cpp
)
target_link_libraries(test_guide_history PRIVATE phd_test_common)
set_property(TARGET test_guide_history PROPERTY FOLDER "Unit tests")
add_test(NAME test_guide_history COMMAND test_guide_history)

############################################################################
# test_zfilter  --  links the REAL production zfilterfactory.cpp (pure DSP
# math, no wx coupling). Verifies filter-design invariants plus an
# end-to-end recurrence proving unity-DC-gain low-pass behaviour.
############################################################################
add_executable(test_zfilter
  ${PHD_TESTS_DIR}/zfilter/test_zfilter.cpp
  ${phd_src_dir}/zfilterfactory.cpp
)
target_link_libraries(test_zfilter PRIVATE phd_test_common)
set_property(TARGET test_zfilter PROPERTY FOLDER "Unit tests")
add_test(NAME test_zfilter COMMAND test_zfilter)

############################################################################
# test_guide_algorithm_lowpass  --  links the REAL Lowpass/Lowpass2 decision
# math (guide_algorithm_lowpass_math.cpp) against the REAL WindowedAxisStats
# (guiding_stats.cpp). The algorithm .cpp files themselves carry wx GUI
# ConfigDialogPanes, so the math was factored into a wx-free helper the
# production result()/reset() now delegate to.
############################################################################
add_executable(test_guide_algorithm_lowpass
  ${PHD_TESTS_DIR}/algorithms/test_guide_algorithm_lowpass.cpp
  ${phd_src_dir}/guide_algorithm_lowpass_math.cpp
  ${phd_src_dir}/guiding_stats.cpp
)
target_link_libraries(test_guide_algorithm_lowpass PRIVATE phd_test_common)
set_property(TARGET test_guide_algorithm_lowpass PROPERTY FOLDER "Unit tests")
add_test(NAME test_guide_algorithm_lowpass COMMAND test_guide_algorithm_lowpass)

############################################################################
# test_calibration_transform  --  links the REAL production
# calibration_transform.cpp, the camera<->mount coordinate transforms
# extracted from Mount. No wx coupling. Verifies the rotation/skew math via
# round-trip, length-preservation, known-rotation and sign-flip invariants.
# test_dec_compensation.cpp: what entering Guiding does with the RA rate for
# the Dec now, compensate, alert or nothing (#157).
############################################################################
add_executable(test_calibration_transform
  ${PHD_TESTS_DIR}/calibration/test_calibration_transform.cpp
  ${PHD_TESTS_DIR}/calibration/test_dec_compensation.cpp
  ${phd_src_dir}/calibration_transform.cpp
)
target_link_libraries(test_calibration_transform PRIVATE phd_test_common)
set_property(TARGET test_calibration_transform PROPERTY FOLDER "Unit tests")
add_test(NAME test_calibration_transform COMMAND test_calibration_transform)

############################################################################
# test_calibration_adapt  --  links the REAL production calibration_codec.cpp
# (the calibration's profile keys, defaults and step-list text, from
# Mount::SetCalibration / GetLastCalibration / Save- and
# LoadCalibrationDetails), calibration_adjust.cpp (what entering Guiding does
# to the calibration: Dec compensation, the meridian flip, the rotator and
# the binning rescale, from Mount::AdjustCalibrationForScopePointing and
# FlipCalibration) and calibration_sanity.cpp (Scope::SanityCheckCalibration's
# advisory checks) (#142). std-only.
############################################################################
add_executable(test_calibration_adapt
  ${PHD_TESTS_DIR}/calibration/test_calibration_codec.cpp
  ${PHD_TESTS_DIR}/calibration/test_calibration_adjust.cpp
  ${PHD_TESTS_DIR}/calibration/test_calibration_sanity.cpp
  ${phd_src_dir}/calibration_codec.cpp
  ${phd_src_dir}/calibration_adjust.cpp
  ${phd_src_dir}/calibration_sanity.cpp
  ${phd_src_dir}/calibration_transform.cpp
)
target_link_libraries(test_calibration_adapt PRIVATE phd_test_common)
set_property(TARGET test_calibration_adapt PROPERTY FOLDER "Unit tests")
add_test(NAME test_calibration_adapt COMMAND test_calibration_adapt)

############################################################################
# test_guiding_assistant_math  --  links the REAL production
# guiding_assistant_math.cpp (the Guiding Assistant's sample statistics,
# polar-alignment estimate and recommendation logic, extracted from
# GuidingAsstWin) against the REAL guiding_stats.cpp. No wx coupling; the
# dialog and the headless guiding_assistant_* RPC controller both delegate
# to it.
############################################################################
add_executable(test_guiding_assistant_math
  ${PHD_TESTS_DIR}/guiding_assistant/test_guiding_assistant_math.cpp
  ${phd_src_dir}/guiding_assistant_math.cpp
  ${phd_src_dir}/guiding_stats.cpp
)
target_link_libraries(test_guiding_assistant_math PRIVATE phd_test_common)
set_property(TARGET test_guiding_assistant_math PROPERTY FOLDER "Unit tests")
add_test(NAME test_guiding_assistant_math COMMAND test_guiding_assistant_math)

############################################################################
# test_star_find  --  links the REAL production star_find_core.cpp (the star
# detection pixel math extracted from Star::Find) and feeds it a real FITS
# fixture loaded directly with cfitsio. Unlike the other suites this needs
# cfitsio (not wx); the production star.cpp/usImage.cpp are too app-coupled to
# link, which is exactly why the math was extracted.
############################################################################
find_library(CFITSIO_LIB NAMES cfitsio)
find_path(CFITSIO_INC NAMES fitsio.h)
if(CFITSIO_LIB AND CFITSIO_INC)
  add_executable(test_star_find
    ${PHD_TESTS_DIR}/star/test_star_find.cpp
    ${phd_src_dir}/star_find_core.cpp
  )
  target_link_libraries(test_star_find PRIVATE phd_test_common ${CFITSIO_LIB})
  target_include_directories(test_star_find PRIVATE ${CFITSIO_INC})
  target_compile_definitions(test_star_find PRIVATE PHD_FIXTURE_DIR="${PHD_PROJECT_ROOT_DIR}")
  set_property(TARGET test_star_find PROPERTY FOLDER "Unit tests")
  add_test(NAME test_star_find COMMAND test_star_find)
else()
  message(STATUS "cfitsio not found - skipping test_star_find")
endif()

############################################################################
# test_star_find_synthetic  --  the same REAL star_find_core.cpp on synthetic
# frames (#142): pixel-integrated Gaussian stars with seeded noise
# (star/synthetic_star.h). Centroid accuracy and precision, HFD, SNR,
# saturation and the rejection branches. std-only, no cfitsio.
############################################################################
add_executable(test_star_find_synthetic
  ${PHD_TESTS_DIR}/star/test_star_find_synthetic.cpp
  ${phd_src_dir}/star_find_core.cpp
)
target_link_libraries(test_star_find_synthetic PRIVATE phd_test_common)
set_property(TARGET test_star_find_synthetic PROPERTY FOLDER "Unit tests")
add_test(NAME test_star_find_synthetic COMMAND test_star_find_synthetic)

############################################################################
# test_imaging_gear  --  imaging_gear.cpp / imaging_gear_poller.cpp: the
# imaging-train guide log lines (design/IMAGING_GEAR_LOG.md), pixel scale,
# unknown values, Alpaca device selection, and the background poller
# (driven by a fake read-only Reader). std-only.
############################################################################
add_executable(test_imaging_gear
  ${PHD_TESTS_DIR}/imaging_gear/test_imaging_gear.cpp
  ${PHD_TESTS_DIR}/imaging_gear/test_imaging_gear_poller.cpp
  ${phd_src_dir}/imaging_gear.cpp
  ${phd_src_dir}/imaging_gear_poller.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(test_imaging_gear PRIVATE phd_test_common Threads::Threads)
set_property(TARGET test_imaging_gear PROPERTY FOLDER "Unit tests")
add_test(NAME test_imaging_gear COMMAND test_imaging_gear)

############################################################################
# test_jsonrpc_request  --  jsonrpc_request.cpp: the checks an event-server
# JSON-RPC request gets before dispatch, and the id its error echoes (#118).
# std-only.
############################################################################
add_executable(test_jsonrpc_request
  ${PHD_TESTS_DIR}/event_server/test_jsonrpc_request.cpp
  ${phd_src_dir}/jsonrpc_request.cpp
  ${phd_src_dir}/json_parser.cpp
)
target_link_libraries(test_jsonrpc_request PRIVATE phd_test_common)
set_property(TARGET test_jsonrpc_request PROPERTY FOLDER "Unit tests")
add_test(NAME test_jsonrpc_request COMMAND test_jsonrpc_request)

############################################################################
# test_variable_delay  --  variable_delay.cpp: the seconds <-> ms conversion
# of get_ and set_variable_delay_settings (#174). std-only.
############################################################################
add_executable(test_variable_delay
  ${PHD_TESTS_DIR}/event_server/test_variable_delay.cpp
  ${phd_src_dir}/variable_delay.cpp
)
target_link_libraries(test_variable_delay PRIVATE phd_test_common)
set_property(TARGET test_variable_delay PROPERTY FOLDER "Unit tests")
add_test(NAME test_variable_delay COMMAND test_variable_delay)

############################################################################
# test_profile_defaults  --  reads src/*.cpp: every read of a profile key
# passes the same default (#191). std-only, links no production code.
############################################################################
add_executable(test_profile_defaults
  ${PHD_TESTS_DIR}/event_server/test_profile_defaults.cpp
)
target_compile_definitions(test_profile_defaults PRIVATE PHD_SRC_DIR="${phd_src_dir}")
target_link_libraries(test_profile_defaults PRIVATE phd_test_common)
set_property(TARGET test_profile_defaults PROPERTY FOLDER "Unit tests")
add_test(NAME test_profile_defaults COMMAND test_profile_defaults)

############################################################################
# test_alpaca_gain  --  alpaca_gain.cpp: the GainMin/GainMax support
# decision and the 0-100 % -> raw gain mapping CameraAlpaca uses. std-only.
############################################################################
add_executable(test_alpaca_gain
  ${PHD_TESTS_DIR}/camera/test_alpaca_gain.cpp
  ${phd_src_dir}/alpaca_gain.cpp
)
target_link_libraries(test_alpaca_gain PRIVATE phd_test_common)
set_property(TARGET test_alpaca_gain PROPERTY FOLDER "Unit tests")
add_test(NAME test_alpaca_gain COMMAND test_alpaca_gain)

############################################################################
# test_alpaca_poll  --  alpaca_poll.cpp: the ImageReady poll interval
# CameraAlpaca waits between polls during the readout (#120). std-only.
############################################################################
add_executable(test_alpaca_poll
  ${PHD_TESTS_DIR}/camera/test_alpaca_poll.cpp
  ${phd_src_dir}/alpaca_poll.cpp
)
target_link_libraries(test_alpaca_poll PRIVATE phd_test_common)
set_property(TARGET test_alpaca_poll PROPERTY FOLDER "Unit tests")
add_test(NAME test_alpaca_poll COMMAND test_alpaca_poll)

############################################################################
# test_alpaca_errors  --  alpaca_errors.cpp: which Alpaca capture errors
# disconnect the camera with reconnect (1031 at once, 1280 three in a row)
# and reading an Alpaca error out of a JSON response (#103). std-only.
############################################################################
add_executable(test_alpaca_errors
  ${PHD_TESTS_DIR}/camera/test_alpaca_errors.cpp
  ${phd_src_dir}/alpaca_errors.cpp
  ${phd_src_dir}/json_parser.cpp
)
target_link_libraries(test_alpaca_errors PRIVATE phd_test_common)
set_property(TARGET test_alpaca_errors PROPERTY FOLDER "Unit tests")
add_test(NAME test_alpaca_errors COMMAND test_alpaca_errors)

############################################################################
# test_connect_request  --  connect_request.cpp: device names and the
# refuse-while-busy rules for per-device set_connected (#84), and the
# per-request camera-change latch (#139). std-only.
############################################################################
add_executable(test_connect_request
  ${PHD_TESTS_DIR}/equipment/test_connect_request.cpp
  ${phd_src_dir}/connect_request.cpp
)
target_link_libraries(test_connect_request PRIVATE phd_test_common)
set_property(TARGET test_connect_request PROPERTY FOLDER "Unit tests")
add_test(NAME test_connect_request COMMAND test_connect_request)

############################################################################
# test_gear_selection  --  gear_selection.cpp: which camera, mount and
# rotator a profile's saved choices select (#115 step 3): the list match
# without case, the INDI rule, the stale-choice fallbacks, the camera's
# saved-name chain, the camera-change test and the camera-id key. std-only.
############################################################################
add_executable(test_gear_selection
  ${PHD_TESTS_DIR}/equipment/test_gear_selection.cpp
  ${phd_src_dir}/gear_selection.cpp
)
target_link_libraries(test_gear_selection PRIVATE phd_test_common)
set_property(TARGET test_gear_selection PROPERTY FOLDER "Unit tests")
add_test(NAME test_gear_selection COMMAND test_gear_selection)

############################################################################
# test_gear_session  --  gear_session.cpp: the sequencing of a gear request
# (#115 step 3) over a recording fake GearPort: the leaf order of connect
# all and disconnect all, every error text, no-ops and refusals, the
# request depth and latch session (#139), and the end of request, run once
# per accepted request. std-only.
############################################################################
add_executable(test_gear_session
  ${PHD_TESTS_DIR}/equipment/test_gear_session.cpp
  ${phd_src_dir}/gear_session.cpp
  ${phd_src_dir}/connect_request.cpp
)
target_link_libraries(test_gear_session PRIVATE phd_test_common)
set_property(TARGET test_gear_session PROPERTY FOLDER "Unit tests")
add_test(NAME test_gear_session COMMAND test_gear_session)

############################################################################
# test_connect_retry  --  connect_retry.cpp: the background retry of a
# selected device that failed to connect, its gates, the Alpaca probe run
# before a connect, and the guide refusal text (#104). std-only.
############################################################################
add_executable(test_connect_retry
  ${PHD_TESTS_DIR}/equipment/test_connect_retry.cpp
  ${phd_src_dir}/connect_retry.cpp
  ${phd_src_dir}/connect_request.cpp
  ${phd_src_dir}/alpaca_errors.cpp
  ${phd_src_dir}/json_parser.cpp
)
target_link_libraries(test_connect_retry PRIVATE phd_test_common Threads::Threads)
set_property(TARGET test_connect_retry PROPERTY FOLDER "Unit tests")
add_test(NAME test_connect_retry COMMAND test_connect_retry)

############################################################################
# test_pulse_wait  --  pulse_wait.cpp: how long ScopeAlpaca::Guide waits
# for an Alpaca PulseGuide, timed from the request (#119). std-only.
############################################################################
add_executable(test_pulse_wait
  ${PHD_TESTS_DIR}/equipment/test_pulse_wait.cpp
  ${phd_src_dir}/pulse_wait.cpp
)
target_link_libraries(test_pulse_wait PRIVATE phd_test_common)
set_property(TARGET test_pulse_wait PROPERTY FOLDER "Unit tests")
add_test(NAME test_pulse_wait COMMAND test_pulse_wait)

############################################################################
# test_capture_cleanup  --  capture_cleanup.cpp: pruning of the
# save_image_XXXXXX frames the daemon names itself (#93). std-only.
############################################################################
add_executable(test_capture_cleanup
  ${PHD_TESTS_DIR}/capture/test_capture_cleanup.cpp
  ${phd_src_dir}/capture_cleanup.cpp
)
target_link_libraries(test_capture_cleanup PRIVATE phd_test_common)
set_property(TARGET test_capture_cleanup PROPERTY FOLDER "Unit tests")
add_test(NAME test_capture_cleanup COMMAND test_capture_cleanup)

############################################################################
# test_capture_failure  --  capture_failure.cpp: the reason a failed capture
# reports to clients (SingleFrameComplete / SettleDone / dark-build error /
# Alert), built from the failed Alpaca call or the driver's alert (#97), and
# whether a failed calibration frame is exposed again (#116) or a failed
# guiding frame is dropped (#136), and the phase of each guider state.
# std-only.
############################################################################
add_executable(test_capture_failure
  ${PHD_TESTS_DIR}/capture/test_capture_failure.cpp
  ${phd_src_dir}/capture_failure.cpp
  ${phd_src_dir}/alpaca_errors.cpp
  ${phd_src_dir}/json_parser.cpp
)
target_link_libraries(test_capture_failure PRIVATE phd_test_common)
set_property(TARGET test_capture_failure PROPERTY FOLDER "Unit tests")
add_test(NAME test_capture_failure COMMAND test_capture_failure)

############################################################################
# test_settle_frame  --  settle_frame.cpp: what one guide frame does to a
# guide or dither settle, including a frame dropped after a transient
# capture failure (#136). std-only.
############################################################################
add_executable(test_settle_frame
  ${PHD_TESTS_DIR}/guiding/test_settle_frame.cpp
  ${phd_src_dir}/settle_frame.cpp
)
target_link_libraries(test_settle_frame PRIVATE phd_test_common)
set_property(TARGET test_settle_frame PROPERTY FOLDER "Unit tests")
add_test(NAME test_settle_frame COMMAND test_settle_frame)

############################################################################
# test_settle_tracker  --  settle_tracker.cpp: PhdController's settle timers
# (the timeout from the settle's start, the in-range run from its first
# frame) and frame counts, on an injected clock, with steps of the system
# time of +-1 h, and the clock PhdController gives it, the steady one (#142,
# #257). std-only.
############################################################################
add_executable(test_settle_tracker
  ${PHD_TESTS_DIR}/guiding/test_settle_tracker.cpp
  ${phd_src_dir}/settle_tracker.cpp
  ${phd_src_dir}/settle_frame.cpp
  ${phd_src_dir}/monotonic_timers.cpp
)
target_link_libraries(test_settle_tracker PRIVATE phd_test_common)
set_property(TARGET test_settle_tracker PROPERTY FOLDER "Unit tests")
add_test(NAME test_settle_tracker COMMAND test_settle_tracker)

############################################################################
# test_guide_distance_tracker  --  guide_distance_tracker.cpp: the guider's
# averaged distance behind CurrentError and CurrentErrorSmoothed (the short
# and long averages, the dither bump, the re-seed, the 100 px after 20 s with
# no star, RA only), on an injected clock (#142). std-only.
############################################################################
add_executable(test_guide_distance_tracker
  ${PHD_TESTS_DIR}/guiding/test_guide_distance_tracker.cpp
  ${phd_src_dir}/guide_distance_tracker.cpp
  ${phd_src_dir}/monotonic_timers.cpp
)
target_link_libraries(test_guide_distance_tracker PRIVATE phd_test_common)
set_property(TARGET test_guide_distance_tracker PROPERTY FOLDER "Unit tests")
add_test(NAME test_guide_distance_tracker COMMAND test_guide_distance_tracker)

############################################################################
# test_dither_random  --  dither_random.cpp: the offsets of a random dither,
# from a generator of its own seeded at startup so a daemon start does not
# repeat the last start's dithers, with the old distribution and range
# (#173). std-only.
############################################################################
add_executable(test_dither_random
  ${PHD_TESTS_DIR}/guiding/test_dither_random.cpp
  ${phd_src_dir}/dither_random.cpp
)
target_link_libraries(test_dither_random PRIVATE phd_test_common)
set_property(TARGET test_dither_random PROPERTY FOLDER "Unit tests")
add_test(NAME test_dither_random COMMAND test_dither_random)

############################################################################
# test_restore_calibration  --  restore_calibration.cpp: when
# restore_calibration is refused, returns false or ends a stale guide
# request before restoring (#102); clear_calibration is refused while
# calibrating (#137). std-only.
############################################################################
add_executable(test_restore_calibration
  ${PHD_TESTS_DIR}/calibration/test_restore_calibration.cpp
  ${phd_src_dir}/restore_calibration.cpp
)
target_link_libraries(test_restore_calibration PRIVATE phd_test_common)
set_property(TARGET test_restore_calibration PROPERTY FOLDER "Unit tests")
add_test(NAME test_restore_calibration COMMAND test_restore_calibration)

############################################################################
# test_calstep_math  --  calstep_math.cpp: the recommended calibration
# distance and step (#142), with golden values, the guard for an unknown
# image scale or guide speed, and a differential against the frozen copy of
# the CalstepDialog lines it came from (calibration/legacy, check_twin.sh
# diffs it against the source). std-only.
############################################################################
add_executable(test_calstep_math
  ${PHD_TESTS_DIR}/calibration/test_calstep_math.cpp
  ${PHD_TESTS_DIR}/calibration/legacy/calstep_legacy.cpp
  ${phd_src_dir}/calstep_math.cpp
)
target_link_libraries(test_calstep_math PRIVATE phd_test_common)
set_property(TARGET test_calstep_math PROPERTY FOLDER "Unit tests")
add_test(NAME test_calstep_math COMMAND test_calstep_math)

############################################################################
# test_image_scale  --  image_scale.h: PercentChange, the 5% test that flags
# an image-scale change, and SiderealRateFromGuideSpeed, moved out of
# AdvancedDialog with MakeImageScaleAdjustments (#115 step 4, S4.1).
# std-only, header-only.
############################################################################
add_executable(test_image_scale
  ${PHD_TESTS_DIR}/calibration/test_image_scale.cpp
)
target_link_libraries(test_image_scale PRIVATE phd_test_common)
set_property(TARGET test_image_scale PROPERTY FOLDER "Unit tests")
add_test(NAME test_image_scale COMMAND test_image_scale)

############################################################################
# test_instance_lock  --  instance_lock.cpp: where the single-instance lock
# goes, $RUNTIME_DIRECTORY under systemd or wx's default (#135). std-only.
############################################################################
add_executable(test_instance_lock
  ${PHD_TESTS_DIR}/startup/test_instance_lock.cpp
  ${phd_src_dir}/instance_lock.cpp
)
target_link_libraries(test_instance_lock PRIVATE phd_test_common)
set_property(TARGET test_instance_lock PROPERTY FOLDER "Unit tests")
add_test(NAME test_instance_lock COMMAND test_instance_lock)

############################################################################
# test_atomic_file_write  --  atomic_file_write.cpp: the config file write
# that a kill or power cut cannot leave empty, torn or missing (#142).
# POSIX, std-only.
############################################################################
add_executable(test_atomic_file_write
  ${PHD_TESTS_DIR}/startup/test_atomic_file_write.cpp
  ${phd_src_dir}/atomic_file_write.cpp
)
target_link_libraries(test_atomic_file_write PRIVATE phd_test_common)
set_property(TARGET test_atomic_file_write PROPERTY FOLDER "Unit tests")
add_test(NAME test_atomic_file_write COMMAND test_atomic_file_write)

############################################################################
# test_guide_algorithm_gp_params  --  guide_algorithm_gp_params.cpp: the
# Gaussian Process hyperparameters set_algo_param "periodLength" writes,
# the current ones with only the period changed (#138), run through the
# real GaussianProcessGuider (contrib GPGuider target). No wx coupling.
############################################################################
add_executable(test_guide_algorithm_gp_params
  ${PHD_TESTS_DIR}/algorithms/test_guide_algorithm_gp_params.cpp
  ${phd_src_dir}/guide_algorithm_gp_params.cpp
)
target_link_libraries(test_guide_algorithm_gp_params PRIVATE phd_test_common GPGuider)
set_property(TARGET test_guide_algorithm_gp_params PROPERTY FOLDER "Unit tests")
add_test(NAME test_guide_algorithm_gp_params COMMAND test_guide_algorithm_gp_params)

############################################################################
# test_edge_allowance  --  edge_allowance.cpp: how far beyond the search
# region auto-select keeps a star from the frame edge, including a guide
# with recalibrate: true on a calibrated mount (#140). std-only.
############################################################################
add_executable(test_edge_allowance
  ${PHD_TESTS_DIR}/star/test_edge_allowance.cpp
  ${phd_src_dir}/edge_allowance.cpp
)
target_link_libraries(test_edge_allowance PRIVATE phd_test_common)
set_property(TARGET test_edge_allowance PROPERTY FOLDER "Unit tests")
add_test(NAME test_edge_allowance COMMAND test_edge_allowance)

############################################################################
# test_monotonic_timers  --  monotonic_timers.cpp: how long ago Guider last
# found the star (the 100 px large distance after 20 s) and MyFrame's camera
# reconnect throttle (3 attempts in 60 s), on an injected clock, and the
# clock their callers read (#226); the set_pa_session lease on an injected
# clock, and that ClockMs is the same steady clock in milliseconds (#231);
# RunTimer, the GA sampling's and the backlash measurement's timer, on an
# injected clock and on the clock it reads itself (#235).
# std-only.
############################################################################
add_executable(test_monotonic_timers
  ${PHD_TESTS_DIR}/guiding/test_monotonic_timers.cpp
  ${phd_src_dir}/monotonic_timers.cpp
)
target_link_libraries(test_monotonic_timers PRIVATE phd_test_common)
set_property(TARGET test_monotonic_timers PROPERTY FOLDER "Unit tests")
add_test(NAME test_monotonic_timers COMMAND test_monotonic_timers)

############################################################################
# test_frame_reject  --  frame_reject.cpp (MassChecker and DistanceChecker,
# the frame rejection GuiderMultiStar::UpdateCurrentPosition runs) and
# multistar_refine.h (the multi-star refine step GuiderMultiStar::RefineOffset
# delegates to), on an injected clock and a scripted star finder (#142),
# against the REAL guiding_stats.cpp. test_multistar_parity.cpp compares them
# with a frozen copy of the guider_multistar.cpp code they came from (the
# legacy twin, compiled against mechanical wx stand-ins; legacy/check_twin.sh
# diffs it against the source) over seeded call sequences.
############################################################################
add_executable(test_frame_reject
  ${PHD_TESTS_DIR}/guiding/test_frame_reject.cpp
  ${PHD_TESTS_DIR}/guiding/test_multistar_refine.cpp
  ${PHD_TESTS_DIR}/guiding/test_multistar_parity.cpp
  ${PHD_TESTS_DIR}/guiding/legacy/multistar_legacy.cpp
  ${phd_src_dir}/frame_reject.cpp
  ${phd_src_dir}/multistar_refine.cpp
  ${phd_src_dir}/guiding_stats.cpp
)
target_include_directories(test_frame_reject PRIVATE ${PHD_TESTS_DIR}/guiding)
target_link_libraries(test_frame_reject PRIVATE phd_test_common)
set_property(TARGET test_frame_reject PROPERTY FOLDER "Unit tests")
add_test(NAME test_frame_reject COMMAND test_frame_reject)

############################################################################
# test_closed_loop_calibration  --  closed-loop tests of the mount
# calibration state machine (#142): a SimMount (camera angle, Dec mirror,
# cos(Dec) RA rate, Dec backlash, non-orthogonal axes, seeded noise) drives
# it frame by frame. Links the REAL production calibration_run.cpp (the
# state machine Scope delegates to) and runs every behaviour test on it and
# on a frozen copy of the scope.cpp code it replaced (the legacy twin,
# compiled against mechanical wx stand-ins); a differential test compares
# the two effect by effect over a scenario matrix and 5000 seeded scenarios.
# test_high_dec_calibration.cpp: the RA step sized for the Dec (#141).
############################################################################
add_executable(test_closed_loop_calibration
  ${PHD_TESTS_DIR}/closed_loop/test_closed_loop_calibration.cpp
  ${PHD_TESTS_DIR}/closed_loop/test_calibration_run_parity.cpp
  ${PHD_TESTS_DIR}/closed_loop/test_high_dec_calibration.cpp
  ${PHD_TESTS_DIR}/closed_loop/sim_mount.cpp
  ${PHD_TESTS_DIR}/closed_loop/extracted_run.cpp
  ${PHD_TESTS_DIR}/closed_loop/legacy/legacy_twin.cpp
  ${PHD_TESTS_DIR}/closed_loop/legacy/scope_calibration_legacy.cpp
  ${phd_src_dir}/calibration_run.cpp
  ${phd_src_dir}/calibration_transform.cpp
)
target_link_libraries(test_closed_loop_calibration PRIVATE phd_test_common)
set_property(TARGET test_closed_loop_calibration PROPERTY FOLDER "Unit tests")
add_test(NAME test_closed_loop_calibration COMMAND test_closed_loop_calibration)

############################################################################
# test_guide_move_plan  --  guide_move_plan.cpp: the pulse per axis
# Mount::MoveOffset plans for a guide correction and what Scope::MoveAxis
# does to it (guiding disabled, Dec guide mode, max-duration clamp,
# limit-reached count), plus the guide sign in a closed loop through the
# real calibration_transform.cpp across camera angle, Dec mirror and a
# meridian flip (#142). std-only.
############################################################################
add_executable(test_guide_move_plan
  ${PHD_TESTS_DIR}/guiding/test_guide_move_plan.cpp
  ${PHD_TESTS_DIR}/guiding/test_guide_sign_closed_loop.cpp
  ${phd_src_dir}/guide_move_plan.cpp
  ${phd_src_dir}/calibration_transform.cpp
)
target_link_libraries(test_guide_move_plan PRIVATE phd_test_common)
set_property(TARGET test_guide_move_plan PROPERTY FOLDER "Unit tests")
add_test(NAME test_guide_move_plan COMMAND test_guide_move_plan)

############################################################################
# test_guiding_rms_baseline  --  a seeded closed-loop guiding simulation
# (#142): sinusoidal PE, drift, seeing, the mount's guide rate and the
# camera's exposure cadence, guided through the REAL calibration_transform,
# guide_algorithm_kernels (Hysteresis on RA, ResistSwitch on Dec, with the
# production defaults) and guide_move_plan. Pins the guided RMS against a
# measured baseline and the unguided RMS, and a flipped calibration
# diverging. std-only.
############################################################################
add_executable(test_guiding_rms_baseline
  ${PHD_TESTS_DIR}/guiding/test_guiding_rms_baseline.cpp
  ${phd_src_dir}/guide_algorithm_kernels.cpp
  ${phd_src_dir}/guide_move_plan.cpp
  ${phd_src_dir}/calibration_transform.cpp
)
target_link_libraries(test_guiding_rms_baseline PRIVATE phd_test_common)
set_property(TARGET test_guiding_rms_baseline PROPERTY FOLDER "Unit tests")
add_test(NAME test_guiding_rms_baseline COMMAND test_guiding_rms_baseline)
