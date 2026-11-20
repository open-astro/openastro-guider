/*
 *  jsonrpc_request.cpp
 *  PHD Guiding
 *
 *  See jsonrpc_request.h.
 */

#include "jsonrpc_request.h"

#include <cstring>
#include <string> // json_parser.h uses std::string without including it

#include "json_parser.h"

JsonRpcRequestCheck CheckJsonRpcRequest(const json_value *req)
{
    JsonRpcRequestCheck chk = { nullptr, nullptr, nullptr, 0, nullptr };

    if (!req || req->type != JSON_OBJECT)
    {
        chk.error = JsonRpcRequest::kInvalidRequest;
        chk.message = "invalid request - request must be an object";
        return chk;
    }

    json_for_each(t, req)
    {
        if (!t->name)
            continue;
        if (t->type == JSON_STRING && std::strcmp(t->name, "method") == 0)
            chk.method = t;
        else if (std::strcmp(t->name, "params") == 0)
            chk.params = t;
        else if (std::strcmp(t->name, "id") == 0)
            chk.id = t;
    }

    if (!chk.method)
    {
        chk.error = JsonRpcRequest::kInvalidRequest;
        chk.message = "invalid request - missing method";
        return chk;
    }

    if (chk.params && chk.params->type != JSON_ARRAY && chk.params->type != JSON_OBJECT)
    {
        chk.error = JsonRpcRequest::kInvalidParams;
        chk.message = "invalid params - params must be an array or object";
        return chk;
    }

    return chk;
}

bool JsonRpcFloatParam(const json_value *v, double *val)
{
    if (!v)
        return false;
    if (v->type == JSON_INT)
    {
        *val = (double) v->int_value;
        return true;
    }
    if (v->type == JSON_FLOAT)
    {
        *val = v->float_value;
        return true;
    }
    return false;
}

bool JsonRpcBoolParam(const json_value *v, bool *val)
{
    if (!v || (v->type != JSON_BOOL && v->type != JSON_INT))
        return false;
    *val = v->int_value != 0;
    return true;
}
