/*
 *  webui_rpc_guard.cpp
 *  PHD Guiding
 *
 *  See webui_rpc_guard.h.
 */

#include "webui_rpc_guard.h"

#include <set>

bool WebUiRpcAllowedWhileBusy(const std::string& method)
{
    // setters that drive the run rather than change its settings
    static const std::set<std::string> runSetters = { "set_paused", "set_exposure", "set_lock_position" };
    static const std::set<std::string> refused = { "clear_calibration",  "flip_calibration",       "delete_calibration_files",
                                                   "create_profile",     "delete_profile",         "rename_profile",
                                                   "build_dark_library", "build_defect_map_darks", "rebuild_defect_map" };

    if (method.rfind("set_", 0) == 0)
        return runSetters.count(method) != 0;
    return refused.count(method) == 0;
}
