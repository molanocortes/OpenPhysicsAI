/* ops.c - operation registry (embedded schema), validation, concurrency guard, idempotent replay, dispatch */
#include "ops_internal.h"
#include "../core/base64.h"
#include "../core/jschema.h"
#include "../core/sha256.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mech-integration: the mechanics operation fragment (src/mech/mech_ops_schema.json) */
extern const char NAVIER_MECH_OPS_SCHEMA[];
extern const size_t NAVIER_MECH_OPS_SCHEMA_len;

extern const char NAVIER_OPS_SCHEMA[];
extern const size_t NAVIER_OPS_SCHEMA_len;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static bool g_ready;
static char g_err[512];
static JsonValue *g_root;
static OpInfo *g_ops;
static OpHandlerFn *g_fns;
static int g_nops;

static const OpBinding *const ALL_BINDINGS[] = {OPS_PROJECT_BINDINGS, OPS_GEOMETRY_BINDINGS, OPS_SURFACE_BINDINGS, OPS_VIEW_BINDINGS,
                                               OPS_SETUP_BINDINGS,   OPS_MESH_BINDINGS,     OPS_ANALYSIS_BINDINGS, OPS_RESULTS_BINDINGS,
                                               OPS_CONTACT_BINDINGS, OPS_STUDY_BINDINGS,  OPS_TOPOPT_BINDINGS,
                                               OPS_MECH_BINDINGS, /* mech-integration */
                                               NULL};

static bool tool_name_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 128) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

static void registry_init_once(void) {
    JsonError jerr;
    g_root = json_parse(NAVIER_OPS_SCHEMA, NAVIER_OPS_SCHEMA_len, NULL, &jerr);
    if (!g_root) {
        snprintf(g_err, sizeof g_err, "ops_schema.json: %s (line %d, column %d)", jerr.message, jerr.line, jerr.column);
        return;
    }
    { /* mech-integration: merge the fragment's $defs and operations; a name collision is a registry error */
        JsonValue *frag = json_parse(NAVIER_MECH_OPS_SCHEMA, NAVIER_MECH_OPS_SCHEMA_len, NULL, &jerr);
        if (!frag) {
            snprintf(g_err, sizeof g_err, "mech_ops_schema.json: %s (line %d, column %d)", jerr.message, jerr.line, jerr.column);
            return;
        }
        JsonValue *defs = json_get(g_root, "$defs"), *fdefs = json_get(frag, "$defs"), *rops = json_get(g_root, "operations");
        for (size_t i = 0; i < json_len(fdefs); i++) {
            if (json_get(defs, json_key_at(fdefs, i))) {
                snprintf(g_err, sizeof g_err, "mech_ops_schema.json: $defs/%s already exists", json_key_at(fdefs, i));
                json_free(frag);
                return;
            }
            json_set(defs, json_key_at(fdefs, i), json_clone(json_value_at(fdefs, i)));
        }
        const JsonValue *fops = json_get(frag, "operations");
        for (size_t i = 0; i < json_len(fops); i++) json_push(rops, json_clone(json_at(fops, i)));
        JsonValue *ext = json_set_object(g_root, "extensions");
        json_set_string(ext, json_get_str(frag, "contract", "navier-mech"), json_get_str(frag, "contract_version", "?"));
        json_free(frag);
    }
    const JsonValue *ops = json_get(g_root, "operations");
    int n = (int)json_len(ops);
    g_ops = calloc((size_t)(n ? n : 1), sizeof *g_ops);
    g_fns = calloc((size_t)(n ? n : 1), sizeof *g_fns);
    if (!g_ops || !g_fns) {
        snprintf(g_err, sizeof g_err, "out of memory");
        return;
    }
    for (int i = 0; i < n; i++) {
        const JsonValue *o = json_at(ops, (size_t)i);
        OpInfo *info = &g_ops[i];
        info->name = json_get_str(o, "name", NULL);
        info->title = json_get_str(o, "title", "");
        info->description = json_get_str(o, "description", "");
        info->input = json_get(o, "input");
        const char *kind = json_get_str(o, "kind", "");
        if (!info->name || !tool_name_ok(info->name)) {
            snprintf(g_err, sizeof g_err, "operation %d has an invalid name", i);
            return;
        }
        if (strcmp(kind, "query") && strcmp(kind, "mutation")) {
            snprintf(g_err, sizeof g_err, "operation %s: kind must be query or mutation", info->name);
            return;
        }
        info->mutating = !strcmp(kind, "mutation");
        info->idempotent = json_get_bool(o, "idempotent", !info->mutating);
        info->destructive = json_get_bool(o, "destructive", false);
        info->engine_lock = json_get_bool(o, "engine_lock", true);
        const JsonValue *props = json_get(info->input, "properties");
        if (!info->engine_lock && (json_get(props, "idempotency_key") || json_get(props, "expected_revision"))) {
            snprintf(g_err, sizeof g_err, "operation %s runs without the engine lock and cannot take idempotency_key or expected_revision", info->name);
            return;
        }
        if (!info->input || strcmp(json_get_str(info->input, "type", ""), "object") != 0) {
            snprintf(g_err, sizeof g_err, "operation %s: input schema must be an object schema", info->name);
            return;
        }
        char serr[400];
        if (!jschema_check(info->input, g_root, serr, sizeof serr)) {
            snprintf(g_err, sizeof g_err, "operation %s: %s", info->name, serr);
            return;
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(g_ops[j].name, info->name)) {
                snprintf(g_err, sizeof g_err, "duplicate operation %s", info->name);
                return;
            }
        for (int b = 0; ALL_BINDINGS[b] && !g_fns[i]; b++)
            for (const OpBinding *ob = ALL_BINDINGS[b]; ob->name; ob++)
                if (!strcmp(ob->name, info->name)) g_fns[i] = ob->fn;
        if (!g_fns[i]) {
            snprintf(g_err, sizeof g_err, "operation %s has no handler", info->name);
            return;
        }
    }
    for (int b = 0; ALL_BINDINGS[b]; b++)
        for (const OpBinding *ob = ALL_BINDINGS[b]; ob->name; ob++) {
            bool declared = false;
            for (int i = 0; i < n && !declared; i++) declared = !strcmp(g_ops[i].name, ob->name);
            if (!declared) {
                snprintf(g_err, sizeof g_err, "handler %s has no schema in ops_schema.json", ob->name);
                return;
            }
        }
    g_nops = n;
    g_ready = true;
}

bool ops_registry_init(char *err, size_t errlen) {
    pthread_once(&g_once, registry_init_once);
    if (!g_ready && err) snprintf(err, errlen, "%s", g_err);
    return g_ready;
}

int ops_count(void) { return g_ready ? g_nops : 0; }
const OpInfo *ops_at(int i) { return g_ready && i >= 0 && i < g_nops ? &g_ops[i] : NULL; }

const OpInfo *ops_find(const char *name) {
    if (!g_ready || !name) return NULL;
    for (int i = 0; i < g_nops; i++)
        if (!strcmp(g_ops[i].name, name)) return &g_ops[i];
    return NULL;
}

const char *ops_contract_version(void) { return g_ready ? json_get_str(g_root, "contract_version", "?") : "?"; }

/* ---- stand-alone schemas: inline $refs ------------------------------------------------------------ */
/* Tool schemas are served without $ref/$defs, which several MCP clients do not resolve. A recursive reference (a
 * selection query nested inside "all") becomes a plain schema of the definition's type whose description points to the
 * enclosing definition; requests are still validated against the full recursive grammar, with precise errors. */

static JsonValue *inline_refs(const JsonValue *v, const char **stack, int depth) {
    if (!v) return NULL;
    if (v->type == JSON_ARRAY) {
        JsonValue *a = json_array();
        for (size_t i = 0; i < json_len(v); i++) json_push(a, inline_refs(json_at(v, i), stack, depth));
        return a;
    }
    if (v->type != JSON_OBJECT) return json_clone(v);
    const char *ref = json_get_str(v, "$ref", NULL);
    JsonValue *o;
    if (ref && !strncmp(ref, "#/$defs/", 8)) {
        const char *name = ref + 8;
        bool recursive = false;
        for (int i = 0; i < depth; i++) recursive |= !strcmp(stack[i], name);
        const JsonValue *def = json_get(json_get(g_root, "$defs"), name);
        if (recursive || depth >= 16 || !def) {
            o = json_object();
            const JsonValue *type = def ? json_get(def, "type") : NULL;
            json_set(o, "type", type ? json_clone(type) : json_string("object"));
            json_set(o, "description", json_stringf("Nested %s: same structure as the enclosing %s.", name, name));
        } else {
            stack[depth] = name;
            o = inline_refs(def, stack, depth + 1);
        }
    } else {
        o = json_object();
    }
    for (size_t i = 0; i < json_len(v); i++) {
        const char *k = json_key_at(v, i);
        if (strcmp(k, "$ref") != 0) json_set(o, k, inline_refs(json_value_at(v, i), stack, depth));
    }
    return o;
}

JsonValue *ops_input_schema(const OpInfo *op) {
    const char *stack[32];
    return inline_refs(op->input, stack, 0);
}

/* ---- results ----------------------------------------------------------------------------------- */

void op_fail(OpResult *r, NvErr code, const char *hint, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    json_free(r->error);
    json_free(r->value);
    r->value = NULL;
    r->ok = false;
    r->error = nv_error_json(code, hint, "%s", msg);
}

void op_fail_detail(OpResult *r, const char *key, JsonValue *v) {
    if (!r->error) {
        json_free(v);
        return;
    }
    JsonValue *d = json_get(r->error, "details");
    if (!d) d = json_set_object(r->error, "details");
    json_set(d, key, v);
}

void op_succeed(OpResult *r, JsonValue *value) {
    json_free(r->value);
    json_free(r->error);
    r->error = NULL;
    r->value = value ? value : json_object();
    r->ok = true;
}

bool op_need_project(Engine *e, OpResult *r) {
    if (e->proj) return true;
    op_fail(r, NV_ERR_NO_PROJECT, "call project_create (or project_open) first", "no project is open");
    return false;
}

Body *op_need_body(Engine *e, const JsonValue *params, const char *key, OpResult *r) {
    if (!op_need_project(e, r)) return NULL;
    const char *name = json_get_str(params, key, NULL);
    Body *b = project_body(e->proj, name);
    if (b) return b;
    if (name) {
        op_fail(r, NV_ERR_NOT_FOUND, "use project_inspect to list bodies", "no body named '%s'", name);
        JsonValue *names = json_array();
        for (int i = 0; i < e->proj->nbodies; i++) json_push(names, json_string(e->proj->bodies[i]->name));
        op_fail_detail(r, "available_bodies", names);
    } else {
        op_fail(r, NV_ERR_PRECONDITION, "import geometry with geometry_import first", "the project has no bodies");
    }
    return NULL;
}

bool op_quantity(OpResult *r, const JsonValue *params, const char *key, Dimension dim, const char *default_unit, double *si, bool *present) {
    const JsonValue *v = json_get(params, key);
    if (present) *present = v != NULL;
    if (!v) return true;
    char err[256];
    if (!quantity_from_json(v, dim, default_unit, si, err, sizeof err)) {
        op_fail(r, NV_ERR_INVALID_UNIT, NULL, "%s: %s", key, err);
        return false;
    }
    return true;
}

bool op_attach_image(OpResult *r, const char *mime, const char *name, unsigned char *data, size_t len) {
    if (r->nimages >= OP_MAX_IMAGES) {
        free(data);
        return false;
    }
    OpImage *im = &r->images[r->nimages++];
    snprintf(im->mime, sizeof im->mime, "%s", mime);
    snprintf(im->name, sizeof im->name, "%s", name ? name : "image");
    im->data = data;
    im->len = len;
    return true;
}

void op_result_free(OpResult *r) {
    json_free(r->value);
    json_free(r->error);
    for (int i = 0; i < r->nimages; i++) free(r->images[i].data);
    memset(r, 0, sizeof *r);
}

JsonValue *op_result_json(const OpResult *r, bool include_image_data) {
    JsonValue *o = json_object();
    json_set_bool(o, "ok", r->ok);
    json_set_int(o, "revision", (long long)r->revision);
    if (r->ok) json_set(o, "value", json_clone(r->value));
    else json_set(o, "error", json_clone(r->error));
    if (r->replayed) json_set_bool(o, "replayed", true);
    if (r->nimages) {
        JsonValue *imgs = json_set_array(o, "images");
        for (int i = 0; i < r->nimages; i++) {
            JsonValue *im = json_object();
            json_set_string(im, "mime_type", r->images[i].mime);
            json_set_string(im, "name", r->images[i].name);
            json_set_int(im, "bytes", (long long)r->images[i].len);
            if (include_image_data) {
                char *b64 = base64_encode(r->images[i].data, r->images[i].len, NULL);
                json_set(im, "data", json_string(b64));
                free(b64);
            }
            json_push(imgs, im);
        }
    }
    return o;
}

/* ---- dispatch ---------------------------------------------------------------------------------- */

static int edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 63 || lb > 63) return 99;
    int prev[64], cur[64];
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int v = prev[j - 1] + (a[i - 1] != b[j - 1]);
            if (prev[j] + 1 < v) v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            cur[j] = v;
        }
        memcpy(prev, cur, sizeof cur);
    }
    return prev[lb];
}

static void copy_images(OpImage *dst, int *ndst, const OpImage *src, int nsrc) {
    *ndst = 0;
    for (int i = 0; i < nsrc; i++) {
        unsigned char *d = malloc(src[i].len ? src[i].len : 1);
        if (!d) continue;
        memcpy(d, src[i].data, src[i].len);
        dst[*ndst] = src[i];
        dst[*ndst].data = d;
        (*ndst)++;
    }
}

void ops_invoke(Engine *e, const char *name, const JsonValue *params, const OpCaller *caller, OpResult *out) {
    memset(out, 0, sizeof *out);
    char err[512];
    if (!ops_registry_init(err, sizeof err)) {
        op_fail(out, NV_ERR_INTERNAL, NULL, "operation registry unavailable: %s", err);
        return;
    }
    int idx = -1;
    for (int i = 0; name && i < g_nops && idx < 0; i++)
        if (!strcmp(g_ops[i].name, name)) idx = i;
    if (idx < 0) {
        const char *best = NULL;
        int bd = 99;
        for (int i = 0; name && i < g_nops; i++) {
            int d = edit_distance(name, g_ops[i].name);
            if (d < bd) bd = d, best = g_ops[i].name;
        }
        char hint[160];
        if (best && bd <= 4) snprintf(hint, sizeof hint, "did you mean '%s'?", best);
        else snprintf(hint, sizeof hint, "list operations with capabilities_get (MCP: tools/list)");
        op_fail(out, NV_ERR_UNKNOWN_OPERATION, hint, "unknown operation '%s'", name ? name : "(none)");
        return;
    }
    const OpInfo *op = &g_ops[idx];
    if (params && params->type != JSON_OBJECT) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "pass the parameters as a JSON object", "parameters for %s must be an object, not %s", op->name,
                json_type_name(params->type));
        return;
    }
    JsonValue *p = params ? json_clone(params) : json_object();
    if (!p) {
        op_fail(out, NV_ERR_RESOURCE_LIMIT, NULL, "out of memory copying parameters");
        return;
    }
    JsonSchemaReport rep;
    if (!jschema_validate(op->input, g_root, p, true, &rep)) {
        char text[2048];
        jschema_report_text(&rep, text, sizeof text);
        op_fail(out, NV_ERR_INVALID_PARAMS, "correct the listed fields and retry; the input schema documents units, ranges and defaults",
                "invalid parameters for %s: %s", op->name, text);
        JsonValue *issues = json_array();
        for (int i = 0; i < rep.count; i++) {
            JsonValue *is = json_object();
            json_set_string(is, "path", rep.issues[i].path);
            json_set_string(is, "message", rep.issues[i].message);
            json_push(issues, is);
        }
        op_fail_detail(out, "issues", issues);
        json_free(p);
        return;
    }

    if (!op->engine_lock) {
        /* job and result operations lock what they touch, so they neither wait for nor block a long operation */
        g_fns[idx](e, p, out);
        if (!out->ok && !out->error) op_fail(out, NV_ERR_INTERNAL, NULL, "%s returned no result", op->name);
        if (out->ok && !out->value) out->value = json_object();
        out->revision = engine_revision(e);
        json_free(p);
        return;
    }
    pthread_mutex_lock(&e->mtx);
    e->caller = caller;
    const char *key = json_get_str(p, "idempotency_key", NULL);
    char phash[65] = "";
    if (key) {
        JsonValue *canon = json_clone(p);
        json_remove(canon, "idempotency_key");
        json_remove(canon, "expected_revision");
        size_t n = 0;
        char *text = json_dump(canon, JSON_SORTED, &n, NULL);
        sha256_hex_of(text ? text : "", n, phash);
        free(text);
        json_free(canon);
        for (int i = 0; i < IDEM_SLOTS; i++) {
            IdemEntry *ie = &e->idem[i];
            if (!ie->used || strcmp(ie->key, key) != 0 || strcmp(ie->op, op->name) != 0) continue;
            if (strcmp(ie->params_hash, phash) != 0) {
                op_fail(out, NV_ERR_IDEMPOTENCY_KEY_REUSED, "use a new idempotency_key for a different request",
                        "idempotency_key '%s' was already used for %s with different parameters", key, op->name);
            } else {
                out->ok = true;
                out->value = json_clone(ie->value);
                copy_images(out->images, &out->nimages, ie->images, ie->nimages);
                out->replayed = true;
                out->revision = e->proj ? e->proj->revision : 0;
            }
            e->caller = NULL;
            pthread_mutex_unlock(&e->mtx);
            json_free(p);
            return;
        }
    }
    const JsonValue *exp = json_get(p, "expected_revision");
    uint64_t current = e->proj ? e->proj->revision : 0;
    if (op->mutating && exp && (uint64_t)exp->u.number != current) {
        op_fail(out, NV_ERR_REVISION_CONFLICT, "re-read the project (project_inspect), check the changes listed in details, then retry with the current revision",
                "the project is at revision %llu, but the request expected %llu", (unsigned long long)current, (unsigned long long)exp->u.number);
        op_fail_detail(out, "current_revision", json_number((double)current));
        op_fail_detail(out, "expected_revision", json_number(exp->u.number));
        op_fail_detail(out, "changes_since", engine_journal_json(e, (uint64_t)exp->u.number, 32));
    } else {
        g_fns[idx](e, p, out);
        if (!out->ok && !out->error) op_fail(out, NV_ERR_INTERNAL, NULL, "%s returned no result", op->name);
        if (out->ok && !out->value) out->value = json_object();
        if (out->ok && key) {
            IdemEntry *ie = &e->idem[e->idem_next];
            e->idem_next = (e->idem_next + 1) % IDEM_SLOTS;
            json_free(ie->value);
            for (int k = 0; k < ie->nimages; k++) free(ie->images[k].data);
            memset(ie, 0, sizeof *ie);
            ie->used = true;
            snprintf(ie->key, sizeof ie->key, "%s", key);
            snprintf(ie->op, sizeof ie->op, "%s", op->name);
            memcpy(ie->params_hash, phash, sizeof phash);
            ie->value = json_clone(out->value);
            copy_images(ie->images, &ie->nimages, out->images, out->nimages);
        }
    }
    out->revision = e->proj ? e->proj->revision : 0;
    e->caller = NULL;
    pthread_mutex_unlock(&e->mtx);
    json_free(p);
}
