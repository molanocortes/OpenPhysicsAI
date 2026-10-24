/* errors.h - stable, machine-readable error codes shared by every control transport
 *
 * Every failed operation reports {code, message, hint?, details?}. `code` is one of the names below and never
 * changes meaning; `message` is for humans; `hint` says how to fix the request; `details` carries structured data
 * (the offending field path, expected and actual values, the current project revision, ...). */
#pragma once

#include "json.h"

typedef enum {
    NV_OK = 0,
    NV_ERR_INVALID_REQUEST,      /* malformed envelope (not an object, missing method, bad id) */
    NV_ERR_UNKNOWN_OPERATION,    /* no such operation / tool */
    NV_ERR_INVALID_PARAMS,       /* parameters do not match the operation schema */
    NV_ERR_INVALID_UNIT,         /* unit string not understood or wrong physical dimension */
    NV_ERR_OUT_OF_RANGE,         /* value outside its physically or numerically valid range */
    NV_ERR_UNITS_REQUIRED,       /* a length unit (or an explicit, recorded assumption) is required */
    NV_ERR_NOT_FOUND,            /* named body, selection, material, job or file does not exist */
    NV_ERR_ALREADY_EXISTS,       /* name collision without overwrite */
    NV_ERR_NO_PROJECT,           /* operation needs an open project */
    NV_ERR_PRECONDITION,         /* a prerequisite step is missing (e.g. no mesh yet) */
    NV_ERR_REVISION_CONFLICT,    /* expected_revision does not match the project revision */
    NV_ERR_IDEMPOTENCY_KEY_REUSED, /* same idempotency_key sent with different parameters */
    NV_ERR_STALE_REFERENCE,      /* selection/mesh/boundary condition built on an older geometry revision */
    NV_ERR_GEOMETRY_INVALID,     /* geometry cannot be used (e.g. not a closed solid for volume meshing) */
    NV_ERR_MESH_INVALID,         /* inverted or degenerate elements, empty mesh */
    NV_ERR_INSUFFICIENT_CONSTRAINTS, /* rigid-body motion not prevented */
    NV_ERR_CONFLICTING_BC,       /* incompatible boundary conditions on the same entities */
    NV_ERR_SOLVER_FAILED,        /* linear solver or Newton iteration did not converge */
    NV_ERR_CANCELLED,            /* job cancelled on request */
    NV_ERR_RESOURCE_LIMIT,       /* file, triangle, element, memory or message size limit */
    NV_ERR_PERMISSION,           /* path outside the allowed roots, peer not allowed, bad token */
    NV_ERR_IO,                   /* file system error */
    NV_ERR_BUSY,                 /* too many queued requests or running jobs */
    NV_ERR_TIMEOUT,              /* operation or wait timed out */
    NV_ERR_UNSUPPORTED,          /* capability not implemented; see capabilities_get for model limits */
    NV_ERR_INTERNAL,             /* bug: please report with the request */
    NV_ERR_COUNT
} NvErr;

const char *nv_err_name(NvErr code);
/* JSON-RPC error code used by the control socket for this error (-32602 for parameter errors, -320xx otherwise) */
int nv_err_rpc_code(NvErr code);
NvErr nv_err_from_name(const char *name);

/* {"code": "...", "message": "..."} (+ hint when non-NULL) */
JsonValue *nv_error_json(NvErr code, const char *hint, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
