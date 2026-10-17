// Web UI settings guard tests (#81).
//
// While the guider is calibrating or guiding, POST /api/rpc allows only
// reads, calls that drive the run itself (stop, pause, dither, exposure)
// and the drift-align tool (which runs while guiding); everything else,
// including methods added later, is refused, so a Save in the web UI can't
// disturb a run. The :4400 JSON-RPC port is not affected. The rule lives
// in webui_rpc_guard.cpp.

#include <gtest/gtest.h>

#include "webui_rpc_guard.h"

TEST(WebUiRpcGuard, ReadsAreAllowed)
{
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("get_app_state"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("get_guide_history"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("get_calibration_progress"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("staticpa_get_status"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("polardrift_get_status"));
}

TEST(WebUiRpcGuard, DriftAlignRunsWhileGuiding)
{
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("driftalign_drift"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("driftalign_adjust"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("driftalign_close"));
}

TEST(WebUiRpcGuard, AnythingElseIsRefused)
{
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("clone_profile"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("add_bad_pixel"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("staticpa_start"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("polardrift_start"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("capture_single_frame"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("find_star"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("some_method_added_later"));
}

TEST(WebUiRpcGuard, OnlyStateChangingCallsAreLogged)
{
    EXPECT_FALSE(WebUiRpcIsRead("set_dec_comp"));
    EXPECT_FALSE(WebUiRpcIsRead("guide"));
    EXPECT_TRUE(WebUiRpcIsRead("get_guide_history"));
    EXPECT_TRUE(WebUiRpcIsRead("driftalign_get_status"));
}

TEST(WebUiRpcGuard, RunControlIsAllowed)
{
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("stop_capture"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("set_paused"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("dither"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("set_exposure"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("set_lock_position"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("guide"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("loop"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("shutdown"));
}

TEST(WebUiRpcGuard, SettingsSettersAreRefused)
{
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("set_guide_output_enabled"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("set_guide_limits"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("set_dec_comp"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("set_dither_settings"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("set_algo_param"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("set_camera_gain"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("set_connected"));
}

TEST(WebUiRpcGuard, CalibrationProfileAndDarkChangesAreRefused)
{
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("clear_calibration"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("flip_calibration"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("delete_calibration_files"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("create_profile"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("delete_profile"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("rename_profile"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("build_dark_library"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("build_defect_map_darks"));
    EXPECT_FALSE(WebUiRpcAllowedWhileBusy("rebuild_defect_map"));
}

TEST(WebUiRpcGuard, NamingTheImagingGearIsAllowed)
{
    // Ara may (re)connect mid-run; the imaging-gear devices only change what
    // the guide log's background reader reads, never the guiding (#89)
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("set_imaging_gear_devices"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("get_imaging_gear"));
}
