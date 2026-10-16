/*
 *  webui_rpc_guard.h
 *  PHD Guiding
 *
 *  Which JSON-RPC methods the web UI (POST /api/rpc) may call while the
 *  guider is calibrating or guiding. Kept free of wx so it can be unit
 *  tested (tests/event_server/test_webui_rpc_guard.cpp).
 */

#ifndef WEBUI_RPC_GUARD_H_INCLUDED
#define WEBUI_RPC_GUARD_H_INCLUDED

#include <string>

// false for calls that change settings, equipment, profiles, calibration or
// darks; true for reads and for calls that drive the run (stop, pause, dither)
bool WebUiRpcAllowedWhileBusy(const std::string& method);

#endif
