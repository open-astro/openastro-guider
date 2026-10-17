/*
 *  webui_rpc_guard.cpp
 *  PHD Guiding
 *
 *  See webui_rpc_guard.h.
 */

#include "webui_rpc_guard.h"

#include <set>

static bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool WebUiRpcIsRead(const std::string& method)
{
    return method.rfind("get_", 0) == 0 || EndsWith(method, "_get_status");
}

bool WebUiRpcAllowedWhileBusy(const std::string& method)
{
    static const std::set<std::string> runControl = { "stop_capture",      "set_paused", "dither", "set_exposure",
                                                      "set_lock_position", "guide",      "loop",   "shutdown" };

    // the drift-align tool guides in RA while it measures; the imaging-gear
    // devices only change what the guide log's background reader reads
    return WebUiRpcIsRead(method) || runControl.count(method) != 0 || method.rfind("driftalign_", 0) == 0 ||
        method == "set_imaging_gear_devices";
}
