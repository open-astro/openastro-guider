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

// true only for reads, calls that drive the run (stop, pause, dither) and the
// drift-align tool; everything else is refused, including methods added later
bool WebUiRpcAllowedWhileBusy(const std::string& method);

// get_* and *_get_status: reads that don't need to go in the debug log
bool WebUiRpcIsRead(const std::string& method);

#endif
