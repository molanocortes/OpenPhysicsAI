/* mcp.c - MCP lifecycle, tools/list and tools/call over a NAVIER-AM operation backend */
#include "mcp.h"
#include "../ctl/engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const SUPPORTED[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", NULL};
enum { TOOLS_PAGE = 100 };

struct McpSession {
    McpBackend be;
    bool init_received, initialized;
    char version[16];
    char client_name[128], client_version[64];
    JsonValue *tools; /* cached operation descriptors */
};

bool mcp_version_supported(const char *v) {
    for (int i = 0; v && SUPPORTED[i]; i++)
        if (!strcmp(SUPPORTED[i], v)) return true;
    return false;
}

const char *mcp_default_instructions(void) {
    return "NAVIER-AM simulates additive manufacturing and runs FEM analyses on STL geometry. The AI plans and translates "
           "the user's request; the server performs all calculations deterministically.\n"
           "Workflow: capabilities_get (read model_limitations and analyses) -> project_create -> geometry_import -> geometry_place -> "
           "geometry_diagnostics -> surfaces_list or view_render -> selection_create -> materials_list and material_assign -> "
           "mesh_generate -> boundary_apply -> setup_validate -> analysis_run -> job_status with wait_seconds -> results_query, "
           "results_probe, results_render, results_export.\n"
           "Faces: STL files have no named faces. Select faces with geometric queries or patch ids from surfaces_list, and check "
           "each selection's area, centroid and normal before applying anything to it. To use an image, render a view and pick its "
           "pixels (view_pick, or a pick query): only views rendered by this server have a known camera. For photographs or "
           "screenshots from other software, ask the user to identify the faces instead of guessing.\n"
           "Results: quote numbers only after reading the summary checks (equilibrium_ok, energy_ok) and the warnings. Peaks at "
           "supports or sharp inside corners are singular and not design values; library materials are uncalibrated demonstration "
           "values; a colour image alone is not evidence of correctness.\n"
           "Rules: STL has no units, so always pass units; if the user did not state them, pass your best inference with "
           "units_source 'inferred' and a units_note. Numbers are in the unit documented for each field (lengths mm, "
           "angles deg, temperatures degC); strings may carry units ('0.25 in'). Mutations return the project revision: "
           "pass expected_revision to avoid overwriting concurrent edits and an idempotency_key to make retries safe. "
           "Failed calls return a stable error code with a hint: correct the input instead of retrying unchanged. "
           "Never invent dimensions, material grades, loads, contact conditions or process parameters: ask the user "
           "when a missing value materially changes the result, otherwise state the assumption and record its "
           "provenance. Report results with their units and the model limitations that apply.";
}

McpSession *mcp_session_create(const McpBackend *backend) {
    McpSession *s = calloc(1, sizeof *s);
    if (s) s->be = *backend;
    return s;
}

void mcp_session_destroy(McpSession *s) {
    if (!s) return;
    json_free(s->tools);
    free(s);
}

bool mcp_session_initialized(const McpSession *s) { return s->init_received; }
const char *mcp_session_version(const McpSession *s) { return s->version; }

/* negotiated version is at least v (revision identifiers compare as dates) */
static bool at_least(const McpSession *s, const char *v) { return s->version[0] && strcmp(s->version, v) >= 0; }

static JsonValue *valid_id(const JsonValue *id) {
    if (id && (id->type == JSON_STRING || json_is_integer(id))) return json_clone(id);
    return json_null();
}

static JsonValue *rpc_error(const JsonValue *id, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static JsonValue *rpc_error(const JsonValue *id, int code, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    JsonValue *r = json_object();
    json_set_string(r, "jsonrpc", "2.0");
    json_set(r, "id", valid_id(id));
    JsonValue *e = json_set_object(r, "error");
    json_set_int(e, "code", code);
    json_set_string(e, "message", msg);
    return r;
}

static JsonValue *rpc_result(const JsonValue *id, JsonValue *result) {
    JsonValue *r = json_object();
    json_set_string(r, "jsonrpc", "2.0");
    json_set(r, "id", json_clone(id));
    json_set(r, "result", result);
    return r;
}

static JsonValue *do_initialize(McpSession *s, const JsonValue *id, const JsonValue *params) {
    if (s->init_received) return rpc_error(id, -32600, "initialize has already been received on this session");
    const char *pv = json_get_str(params, "protocolVersion", NULL);
    if (!pv) return rpc_error(id, -32602, "initialize requires params.protocolVersion (a string)");
    const JsonValue *ci = json_get(params, "clientInfo");
    snprintf(s->client_name, sizeof s->client_name, "%s", json_get_str(ci, "name", "mcp-client"));
    snprintf(s->client_version, sizeof s->client_version, "%s", json_get_str(ci, "version", ""));
    snprintf(s->version, sizeof s->version, "%s", mcp_version_supported(pv) ? pv : MCP_LATEST_VERSION);
    s->init_received = true;
    JsonValue *res = json_object();
    json_set_string(res, "protocolVersion", s->version);
    JsonValue *caps = json_set_object(res, "capabilities");
    JsonValue *tools = json_set_object(caps, "tools");
    json_set_bool(tools, "listChanged", false);
    JsonValue *info = json_set_object(res, "serverInfo");
    json_set_string(info, "name", "navier-am");
    json_set_string(info, "version", NAVIER_AM_VERSION);
    if (at_least(s, "2025-06-18")) json_set_string(info, "title", s->be.server_title ? s->be.server_title : "NAVIER additive manufacturing simulation");
    if (at_least(s, "2025-11-25"))
        json_set_string(info, "description", "AI-controlled STL-based additive manufacturing and FEM simulation (deterministic C solvers)");
    json_set_string(res, "instructions", s->be.instructions ? s->be.instructions : mcp_default_instructions());
    return rpc_result(id, res);
}

static bool ensure_tools(McpSession *s, char *err, size_t errlen) {
    if (s->tools) return true;
    s->tools = s->be.list_ops(s->be.ctx, err, errlen);
    return s->tools != NULL;
}

static const JsonValue *find_tool(McpSession *s, const char *name) {
    for (size_t i = 0; i < json_len(s->tools); i++)
        if (!strcmp(json_get_str(json_at(s->tools, i), "name", ""), name)) return json_at(s->tools, i);
    return NULL;
}

static JsonValue *do_tools_list(McpSession *s, const JsonValue *id, const JsonValue *params) {
    char err[512];
    if (!ensure_tools(s, err, sizeof err)) return rpc_error(id, -32603, "cannot list tools: %s", err);
    size_t offset = 0;
    const JsonValue *cursor = json_get(params, "cursor");
    if (cursor && cursor->type != JSON_NULL) {
        const char *c = json_str(cursor);
        char *end = NULL;
        long v = c && !strncmp(c, "page-", 5) ? strtol(c + 5, &end, 10) : -1;
        if (!c || v < 0 || !end || *end || (size_t)v > json_len(s->tools)) return rpc_error(id, -32602, "invalid cursor");
        offset = (size_t)v;
    }
    JsonValue *res = json_object();
    JsonValue *arr = json_set_array(res, "tools");
    size_t end = offset + TOOLS_PAGE < json_len(s->tools) ? offset + TOOLS_PAGE : json_len(s->tools);
    for (size_t i = offset; i < end; i++) {
        const JsonValue *op = json_at(s->tools, i);
        JsonValue *t = json_object();
        json_set_string(t, "name", json_get_str(op, "name", ""));
        if (at_least(s, "2025-06-18")) json_set_string(t, "title", json_get_str(op, "title", ""));
        json_set_string(t, "description", json_get_str(op, "description", ""));
        json_set(t, "inputSchema", json_clone(json_get(op, "input_schema")));
        if (at_least(s, "2025-03-26")) {
            JsonValue *ann = json_set_object(t, "annotations");
            bool query = !strcmp(json_get_str(op, "kind", "query"), "query");
            json_set_string(ann, "title", json_get_str(op, "title", ""));
            json_set_bool(ann, "readOnlyHint", query);
            if (!query) {
                json_set_bool(ann, "destructiveHint", json_get_bool(op, "destructive", false));
                json_set_bool(ann, "idempotentHint", json_get_bool(op, "idempotent", false));
            }
            json_set_bool(ann, "openWorldHint", false);
        }
        json_push(arr, t);
    }
    if (end < json_len(s->tools)) {
        char next[32];
        snprintf(next, sizeof next, "page-%zu", end);
        json_set_string(res, "nextCursor", next);
    }
    return rpc_result(id, res);
}

static JsonValue *text_item(const char *text) {
    JsonValue *t = json_object();
    json_set_string(t, "type", "text");
    json_set_string(t, "text", text);
    return t;
}

static JsonValue *do_tools_call(McpSession *s, const JsonValue *id, const JsonValue *params) {
    const char *name = json_get_str(params, "name", NULL);
    if (!name) return rpc_error(id, -32602, "tools/call requires params.name");
    char err[512];
    if (!ensure_tools(s, err, sizeof err)) return rpc_error(id, -32603, "cannot list tools: %s", err);
    if (!find_tool(s, name)) return rpc_error(id, -32602, "Unknown tool: %s", name);
    const JsonValue *args = json_get(params, "arguments");
    if (args && args->type == JSON_NULL) args = NULL;
    if (args && args->type != JSON_OBJECT) return rpc_error(id, -32602, "tools/call arguments must be an object");

    JsonValue *res = s->be.call_op(s->be.ctx, name, args, s->client_name, err, sizeof err);
    JsonValue *out = json_object();
    JsonValue *content = json_set_array(out, "content");
    if (!res) {
        char text[700];
        snprintf(text, sizeof text, "BACKEND_UNAVAILABLE: %s", err);
        json_push(content, text_item(text));
        json_set_bool(out, "isError", true);
        if (at_least(s, "2025-06-18")) {
            JsonValue *sc = json_set_object(out, "structuredContent");
            json_set_bool(sc, "ok", false);
            JsonValue *e = json_set_object(sc, "error");
            json_set_string(e, "code", "BACKEND_UNAVAILABLE");
            json_set_string(e, "message", err);
        }
        return rpc_result(id, out);
    }
    bool ok = json_get_bool(res, "ok", false);
    /* the structured result mirrors the control-socket result, minus bulky image data */
    JsonValue *structured = json_object();
    json_set_bool(structured, "ok", ok);
    json_set(structured, "revision", json_clone(json_get(res, "revision")));
    if (ok) json_set(structured, "value", json_clone(json_get(res, "value")));
    else json_set(structured, "error", json_clone(json_get(res, "error")));
    if (json_get_bool(res, "replayed", false)) json_set_bool(structured, "replayed", true);
    const JsonValue *images = json_get(res, "images");
    if (json_len(images)) {
        JsonValue *list = json_set_array(structured, "images");
        for (size_t i = 0; i < json_len(images); i++) {
            JsonValue *im = json_object();
            json_set_string(im, "name", json_get_str(json_at(images, i), "name", ""));
            json_set_string(im, "mime_type", json_get_str(json_at(images, i), "mime_type", ""));
            json_push(list, im);
        }
    }
    size_t n;
    char *text = json_dump(structured, 0, &n, NULL);
    if (!ok) {
        const JsonValue *e = json_get(res, "error");
        char head[1024];
        const char *hint = json_get_str(e, "hint", NULL);
        snprintf(head, sizeof head, "%s: %s%s%s", json_get_str(e, "code", "ERROR"), json_get_str(e, "message", ""), hint ? "\nHint: " : "", hint ? hint : "");
        json_push(content, text_item(head));
    }
    json_push(content, text_item(text ? text : "{}"));
    free(text);
    for (size_t i = 0; i < json_len(images); i++) {
        const JsonValue *im = json_at(images, i);
        const char *data = json_get_str(im, "data", NULL);
        if (!data) continue;
        JsonValue *it = json_object();
        json_set_string(it, "type", "image");
        json_set_string(it, "data", data);
        json_set_string(it, "mimeType", json_get_str(im, "mime_type", "image/png"));
        json_push(content, it);
    }
    json_set_bool(out, "isError", !ok);
    if (at_least(s, "2025-06-18")) json_set(out, "structuredContent", structured);
    else json_free(structured);
    json_free(res);
    return rpc_result(id, out);
}

static JsonValue *handle_single(McpSession *s, const JsonValue *msg, bool in_batch) {
    if (msg->type != JSON_OBJECT) return rpc_error(NULL, -32600, "Invalid Request: a message must be a JSON object");
    const JsonValue *id = json_get(msg, "id");
    const char *jsonrpc = json_get_str(msg, "jsonrpc", NULL);
    if (!jsonrpc || strcmp(jsonrpc, "2.0") != 0) return rpc_error(id, -32600, "Invalid Request: jsonrpc must be \"2.0\"");
    const char *method = json_get_str(msg, "method", NULL);
    if (!method) {
        if (json_get(msg, "result") || json_get(msg, "error")) return NULL; /* responses to server requests: none are sent */
        return rpc_error(id, -32600, "Invalid Request: missing method");
    }
    if (id && !(id->type == JSON_STRING || json_is_integer(id))) return rpc_error(NULL, -32600, "Invalid Request: id must be a string or an integer");
    const JsonValue *params = json_get(msg, "params");
    if (params && params->type != JSON_OBJECT) {
        if (!id) return NULL;
        return rpc_error(id, -32602, "Invalid params: params must be an object");
    }
    if (!id) {
        if (!strcmp(method, "notifications/initialized")) s->initialized = true;
        /* notifications/cancelled: calls run to completion synchronously; long work uses jobs with their own cancel */
        return NULL;
    }
    if (!strcmp(method, "initialize")) {
        if (in_batch) return rpc_error(id, -32600, "initialize must not be part of a batch");
        return do_initialize(s, id, params);
    }
    if (!strcmp(method, "ping")) return rpc_result(id, json_object());
    if (!s->init_received) return rpc_error(id, -32600, "Server not initialized: send initialize first");
    if (!strcmp(method, "tools/list")) return do_tools_list(s, id, params);
    if (!strcmp(method, "tools/call")) return do_tools_call(s, id, params);
    return rpc_error(id, -32601, "Method not found: %s", method);
}

JsonValue *mcp_handle(McpSession *s, const JsonValue *msg) {
    if (msg->type == JSON_ARRAY) {
        if (!s->init_received || at_least(s, "2025-06-18"))
            return rpc_error(NULL, -32600, "Invalid Request: JSON-RPC batches are not supported%s%s", s->init_received ? " in protocol version " : " before initialization",
                             s->init_received ? s->version : "");
        if (json_len(msg) == 0) return rpc_error(NULL, -32600, "Invalid Request: empty batch");
        JsonValue *out = json_array();
        for (size_t i = 0; i < json_len(msg); i++) {
            JsonValue *r = handle_single(s, json_at(msg, i), true);
            if (r) json_push(out, r);
        }
        if (!json_len(out)) {
            json_free(out);
            return NULL;
        }
        return out;
    }
    return handle_single(s, msg, false);
}

char *mcp_handle_text(McpSession *s, const char *text, size_t len, size_t *out_len) {
    JsonLimits lim = {64, (size_t)64 << 20, 4096, (size_t)1 << 22};
    JsonError jerr;
    JsonValue *msg = json_parse(text, len, &lim, &jerr);
    JsonValue *resp;
    if (!msg) {
        resp = rpc_error(NULL, -32700, "Parse error: %s (byte %zu)", jerr.message, jerr.offset);
    } else {
        resp = mcp_handle(s, msg);
        json_free(msg);
    }
    if (!resp) return NULL;
    char *out = json_dump(resp, 0, out_len, NULL);
    json_free(resp);
    return out;
}
