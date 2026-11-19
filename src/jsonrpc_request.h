/*
 *  jsonrpc_request.h
 *  PHD Guiding
 *
 *  The checks a JSON-RPC request on the event server (:4400) gets before it
 *  is dispatched (#118): a method, and params that are an array or an object
 *  when present. A rejected request is answered with its own id, so a client
 *  that matches replies by id sees the error. Kept free of wx so it can be
 *  unit tested (tests/event_server/test_jsonrpc_request.cpp).
 */

#ifndef JSONRPC_REQUEST_H_INCLUDED
#define JSONRPC_REQUEST_H_INCLUDED

struct json_value;

namespace JsonRpcRequest
{
// the JSON-RPC 2.0 codes the checks return; event_server.cpp static_asserts
// that its JSONRPC_* codes agree
constexpr int kInvalidRequest = -32600;
constexpr int kInvalidParams = -32602;
} // namespace JsonRpcRequest

struct JsonRpcRequestCheck
{
    const json_value *method; // the "method" member when it is a string, else null
    const json_value *params; // the "params" member of any type, null when absent
    const json_value *id; // the "id" member of any type, null when absent (or no id can be read)
    int error; // 0 when the request can be dispatched, else a JsonRpcRequest code
    const char *message; // the error message when error != 0, else null
};

// Checks one request (a whole line, or one element of a batch). In order:
//  - not an object: kInvalidRequest, id null (only a batch element can be one;
//    a line that is not an object or array is a parse error)
//  - the members are read as upstream PHD2 did: "method" only when it is a
//    string, "params" and "id" of any type, the last duplicate wins
//  - no string method: kInvalidRequest, with the request's id
//  - params present and not an array or object (number, string, bool or
//    null): kInvalidParams, with the request's id
//  - otherwise error 0
JsonRpcRequestCheck CheckJsonRpcRequest(const json_value *req);

#endif
