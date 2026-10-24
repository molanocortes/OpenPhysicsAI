/* errors.c - error code names and JSON construction */
#include "errors.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *NAMES[NV_ERR_COUNT] = {
    [NV_OK] = "OK",
    [NV_ERR_INVALID_REQUEST] = "INVALID_REQUEST",
    [NV_ERR_UNKNOWN_OPERATION] = "UNKNOWN_OPERATION",
    [NV_ERR_INVALID_PARAMS] = "INVALID_PARAMS",
    [NV_ERR_INVALID_UNIT] = "INVALID_UNIT",
    [NV_ERR_OUT_OF_RANGE] = "OUT_OF_RANGE",
    [NV_ERR_UNITS_REQUIRED] = "UNITS_REQUIRED",
    [NV_ERR_NOT_FOUND] = "NOT_FOUND",
    [NV_ERR_ALREADY_EXISTS] = "ALREADY_EXISTS",
    [NV_ERR_NO_PROJECT] = "NO_PROJECT",
    [NV_ERR_PRECONDITION] = "PRECONDITION_FAILED",
    [NV_ERR_REVISION_CONFLICT] = "REVISION_CONFLICT",
    [NV_ERR_IDEMPOTENCY_KEY_REUSED] = "IDEMPOTENCY_KEY_REUSED",
    [NV_ERR_STALE_REFERENCE] = "STALE_REFERENCE",
    [NV_ERR_GEOMETRY_INVALID] = "GEOMETRY_INVALID",
    [NV_ERR_MESH_INVALID] = "MESH_INVALID",
    [NV_ERR_INSUFFICIENT_CONSTRAINTS] = "INSUFFICIENT_CONSTRAINTS",
    [NV_ERR_CONFLICTING_BC] = "CONFLICTING_BOUNDARY_CONDITIONS",
    [NV_ERR_SOLVER_FAILED] = "SOLVER_FAILED",
    [NV_ERR_CANCELLED] = "CANCELLED",
    [NV_ERR_RESOURCE_LIMIT] = "RESOURCE_LIMIT",
    [NV_ERR_PERMISSION] = "PERMISSION_DENIED",
    [NV_ERR_IO] = "IO_ERROR",
    [NV_ERR_BUSY] = "BUSY",
    [NV_ERR_TIMEOUT] = "TIMEOUT",
    [NV_ERR_UNSUPPORTED] = "UNSUPPORTED",
    [NV_ERR_INTERNAL] = "INTERNAL_ERROR",
};

const char *nv_err_name(NvErr code) { return code >= 0 && code < NV_ERR_COUNT && NAMES[code] ? NAMES[code] : "INTERNAL_ERROR"; }

NvErr nv_err_from_name(const char *name) {
    for (int i = 0; i < NV_ERR_COUNT; i++)
        if (NAMES[i] && name && strcmp(NAMES[i], name) == 0) return (NvErr)i;
    return NV_ERR_INTERNAL;
}

int nv_err_rpc_code(NvErr code) {
    switch (code) {
    case NV_OK: return 0;
    case NV_ERR_INVALID_REQUEST: return -32600;
    case NV_ERR_UNKNOWN_OPERATION: return -32601;
    case NV_ERR_INVALID_PARAMS: return -32602;
    case NV_ERR_INTERNAL: return -32603;
    default: return -32000 - (int)code; /* implementation-defined server error range -32000..-32099 */
    }
}

JsonValue *nv_error_json(NvErr code, const char *hint, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    JsonValue *e = json_object();
    json_set_string(e, "code", nv_err_name(code));
    json_set_string(e, "message", msg);
    if (hint && *hint) json_set_string(e, "hint", hint);
    return e;
}
