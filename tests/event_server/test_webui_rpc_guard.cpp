// Web UI settings guard tests (#81).
//
// While the guider is calibrating or guiding, POST /api/rpc refuses calls
// that change settings, equipment, profiles or calibration, so a Save in
// the web UI can't disturb a run. Calls that drive the run itself (stop,
// pause, dither, exposure) and all reads stay allowed. The :4400 JSON-RPC
// port is not affected. The rule lives in webui_rpc_guard.cpp.

#include <gtest/gtest.h>

#include "webui_rpc_guard.h"

TEST(WebUiRpcGuard, ReadsAreAllowed)
{
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("get_app_state"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("get_guide_history"));
    EXPECT_TRUE(WebUiRpcAllowedWhileBusy("get_calibration_progress"));
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
