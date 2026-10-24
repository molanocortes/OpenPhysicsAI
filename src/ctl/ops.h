/* ops.h - the typed operation registry and dispatcher
 *
 * An operation is a named, schema-validated function on the Engine. The schemas live in ops_schema.json (embedded
 * at build time); ops_invoke validates parameters (applying defaults), enforces expected_revision, replays
 * idempotent retries, serialises access to the engine, runs the handler and journals the change. Terminal
 * commands, UI actions, the control socket and MCP tools are thin adapters over ops_invoke. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/errors.h"
#include "../core/json.h"
#include "engine.h"

enum { OP_MAX_IMAGES = 4 };

typedef struct OpImage {
    char mime[32];
    unsigned char *data;
    size_t len;
    char name[96];
} OpImage;

typedef struct OpResult {
    bool ok;
    JsonValue *value; /* result object on success */
    JsonValue *error; /* {code, message, hint?, details?} on failure */
    OpImage images[OP_MAX_IMAGES];
    int nimages;
    bool replayed;    /* returned from the idempotency cache */
    uint64_t revision;
} OpResult;

typedef struct OpCaller {
    const char *transport; /* "console", "ui", "socket", "mcp", "test" */
    const char *client;    /* free text, e.g. the MCP clientInfo name */
} OpCaller;

typedef struct OpInfo {
    const char *name, *title, *description;
    const JsonValue *input;  /* input schema (owned by the registry) */
    bool mutating, idempotent, destructive;
    bool engine_lock; /* false: the handler does its own locking (jobs, results) */
} OpInfo;

/* Parses and checks the embedded schema and binds handlers (fails if any operation lacks one). Thread-safe. */
bool ops_registry_init(char *err, size_t errlen);
int ops_count(void);
const OpInfo *ops_at(int i);
const OpInfo *ops_find(const char *name);
const char *ops_contract_version(void);
/* Stand-alone input schema for an operation: its input plus the $defs it references (caller frees). */
JsonValue *ops_input_schema(const OpInfo *op);

void ops_invoke(Engine *e, const char *name, const JsonValue *params, const OpCaller *caller, OpResult *out);
void op_result_free(OpResult *r);
/* {"ok":..., "revision":..., "value" | "error":..., "replayed":?, "images":[{"mime_type","name","data"(base64)}]} */
JsonValue *op_result_json(const OpResult *r, bool include_image_data);
