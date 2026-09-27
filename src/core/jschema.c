/* jschema.c - JSON Schema subset validator */
#include "jschema.h"
#include "units.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const JsonValue *root;
    bool defaults;
    JsonSchemaReport *rep;
} VCtx;

typedef struct {
    char buf[192];
    size_t len;
    int depth;
} PathBuf;

static size_t path_push(PathBuf *p, const char *seg) {
    size_t save = p->len;
    if (p->len + 2 < sizeof p->buf) p->buf[p->len++] = '/';
    for (const char *s = seg; *s && p->len + 3 < sizeof p->buf; s++) {
        if (*s == '~') p->buf[p->len++] = '~', p->buf[p->len++] = '0';
        else if (*s == '/') p->buf[p->len++] = '~', p->buf[p->len++] = '1';
        else p->buf[p->len++] = *s;
    }
    p->buf[p->len] = 0;
    p->depth++;
    return save;
}

static size_t path_push_index(PathBuf *p, size_t i) {
    char seg[24];
    snprintf(seg, sizeof seg, "%zu", i);
    return path_push(p, seg);
}

static void path_pop(PathBuf *p, size_t save) {
    p->len = save;
    p->buf[save] = 0;
    p->depth--;
}

static void issue(VCtx *c, const PathBuf *p, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void issue(VCtx *c, const PathBuf *p, const char *fmt, ...) {
    if (!c->rep || c->rep->count >= JSCHEMA_MAX_ERRORS) return;
    JsonSchemaIssue *is = &c->rep->issues[c->rep->count++];
    snprintf(is->path, sizeof is->path, "%s", p->len ? p->buf : "/");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(is->message, sizeof is->message, fmt, ap);
    va_end(ap);
}

static int edit_distance(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 48 || lb > 48) return 99;
    int prev[49], cur[49];
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = prev[j - 1] + cost;
            if (prev[j] + 1 < v) v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            cur[j] = v;
        }
        memcpy(prev, cur, sizeof cur);
    }
    return prev[lb];
}

static const JsonValue *resolve_ref(const VCtx *c, const char *ref) {
    if (!ref || strncmp(ref, "#/$defs/", 8) != 0) return NULL;
    return json_get(json_get(c->root, "$defs"), ref + 8);
}

static bool type_matches(const char *t, const JsonValue *v) {
    if (!strcmp(t, "object")) return v->type == JSON_OBJECT;
    if (!strcmp(t, "array")) return v->type == JSON_ARRAY;
    if (!strcmp(t, "string")) return v->type == JSON_STRING;
    if (!strcmp(t, "number")) return v->type == JSON_NUMBER;
    if (!strcmp(t, "integer")) return json_is_integer(v);
    if (!strcmp(t, "boolean")) return v->type == JSON_BOOL;
    if (!strcmp(t, "null")) return v->type == JSON_NULL;
    return false;
}

static void describe_value(const JsonValue *v, char *out, size_t cap) {
    switch (v->type) {
    case JSON_STRING: snprintf(out, cap, "string \"%.40s%s\"", v->u.string.ptr, v->u.string.len > 40 ? "..." : ""); break;
    case JSON_NUMBER: snprintf(out, cap, "number %.10g", v->u.number); break;
    default: snprintf(out, cap, "%s", json_type_name(v->type)); break;
    }
}

static size_t utf8_count(const char *s) {
    size_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static bool validate(VCtx *c, const JsonValue *s, JsonValue *inst, PathBuf *p, int depth);

/* Validates each branch without side effects; returns the number that match and fills best with the issues
 * of the most promising failing branch (fewest issues, deepest path). */
static int count_matches(VCtx *c, const JsonValue *branches, JsonValue *inst, PathBuf *p, int depth, int *first_match,
                         JsonSchemaReport *best) {
    int matches = 0, best_score = -1;
    *first_match = -1;
    memset(best, 0, sizeof *best);
    for (size_t i = 0; i < json_len(branches); i++) {
        JsonSchemaReport tmp = {0};
        VCtx sub = {c->root, false, &tmp};
        if (validate(&sub, json_at(branches, i), inst, p, depth + 1)) {
            if (*first_match < 0) *first_match = (int)i;
            matches++;
        } else {
            int deepest = 0;
            for (int k = 0; k < tmp.count; k++) {
                int d = 0;
                for (const char *q = tmp.issues[k].path; *q; q++) d += *q == '/';
                if (d > deepest) deepest = d;
            }
            int score = deepest * 16 - tmp.count;
            if (score > best_score) best_score = score, *best = tmp;
        }
    }
    return matches;
}

static bool validate(VCtx *c, const JsonValue *s, JsonValue *inst, PathBuf *p, int depth) {
    if (depth > 64) {
        issue(c, p, "schema recursion too deep");
        return false;
    }
    if (!s) return true;
    if (s->type == JSON_BOOL) {
        if (!s->u.boolean) issue(c, p, "is not allowed here");
        return s->u.boolean;
    }
    if (s->type != JSON_OBJECT) return true;

    const char *ref = json_get_str(s, "$ref", NULL);
    if (ref) {
        const JsonValue *target = resolve_ref(c, ref);
        if (!target) {
            issue(c, p, "internal schema error: unresolved $ref %s", ref);
            return false;
        }
        if (!validate(c, target, inst, p, depth + 1)) return false;
    }

    const JsonValue *type = json_get(s, "type");
    if (type) {
        bool ok = false;
        char expected[96] = "";
        if (type->type == JSON_STRING) {
            ok = type_matches(type->u.string.ptr, inst);
            snprintf(expected, sizeof expected, "%s", type->u.string.ptr);
        } else if (type->type == JSON_ARRAY) {
            for (size_t i = 0; i < json_len(type); i++) {
                const char *t = json_str(json_at(type, i));
                if (!t) continue;
                if (type_matches(t, inst)) ok = true;
                size_t L = strlen(expected);
                snprintf(expected + L, sizeof expected - L, "%s%s", i ? " or " : "", t);
            }
        }
        if (!ok) {
            char got[80];
            describe_value(inst, got, sizeof got);
            const char *unit = json_get_str(s, "x-unit", NULL);
            if (unit) issue(c, p, "must be %s (a number in %s, or a string with a unit) - got %s", expected, unit, got);
            else issue(c, p, "must be %s - got %s", expected, got);
            return false;
        }
    }

    bool ok = true;
    const JsonValue *en = json_get(s, "enum");
    if (en && en->type == JSON_ARRAY) {
        bool found = false;
        for (size_t i = 0; i < json_len(en) && !found; i++) found = json_equal(json_at(en, i), inst);
        if (!found) {
            char list[256] = "";
            const char *suggest = NULL;
            int best = 99;
            for (size_t i = 0; i < json_len(en); i++) {
                const char *es = json_str(json_at(en, i));
                size_t L = strlen(list);
                if (es) {
                    if (L + strlen(es) + 8 < sizeof list) snprintf(list + L, sizeof list - L, "%s\"%s\"", i ? ", " : "", es);
                    if (inst->type == JSON_STRING) {
                        int d = edit_distance(es, inst->u.string.ptr);
                        if (d < best) best = d, suggest = es;
                    }
                } else if (json_at(en, i)->type == JSON_NUMBER && L + 24 < sizeof list) {
                    snprintf(list + L, sizeof list - L, "%s%.10g", i ? ", " : "", json_at(en, i)->u.number);
                }
            }
            char got[80];
            describe_value(inst, got, sizeof got);
            if (suggest && best <= 3) issue(c, p, "must be one of %s - got %s (did you mean \"%s\"?)", list, got, suggest);
            else issue(c, p, "must be one of %s - got %s", list, got);
            ok = false;
        }
    }
    const JsonValue *cn = json_get(s, "const");
    if (cn && !json_equal(cn, inst)) {
        char want[80];
        describe_value(cn, want, sizeof want);
        issue(c, p, "must equal %s", want);
        ok = false;
    }

    if (inst->type == JSON_NUMBER) {
        double v = inst->u.number;
        const JsonValue *b;
        const char *unit = json_get_str(s, "x-unit", "");
        if ((b = json_get(s, "minimum")) && b->type == JSON_NUMBER && !(v >= b->u.number))
            issue(c, p, "must be >= %.10g %s (got %.10g)", b->u.number, unit, v), ok = false;
        if ((b = json_get(s, "maximum")) && b->type == JSON_NUMBER && !(v <= b->u.number))
            issue(c, p, "must be <= %.10g %s (got %.10g)", b->u.number, unit, v), ok = false;
        if ((b = json_get(s, "exclusiveMinimum")) && b->type == JSON_NUMBER && !(v > b->u.number))
            issue(c, p, "must be > %.10g %s (got %.10g)", b->u.number, unit, v), ok = false;
        if ((b = json_get(s, "exclusiveMaximum")) && b->type == JSON_NUMBER && !(v < b->u.number))
            issue(c, p, "must be < %.10g %s (got %.10g)", b->u.number, unit, v), ok = false;
    }

    if (inst->type == JSON_STRING) {
        size_t n = utf8_count(inst->u.string.ptr);
        const JsonValue *b;
        if ((b = json_get(s, "minLength")) && json_is_integer(b) && n < (size_t)b->u.number)
            issue(c, p, "must be at least %.0f characters", b->u.number), ok = false;
        if ((b = json_get(s, "maxLength")) && json_is_integer(b) && n > (size_t)b->u.number)
            issue(c, p, "must be at most %.0f characters", b->u.number), ok = false;
        const char *dimname = json_get_str(s, "x-dimension", NULL);
        if (dimname) {
            int d = dimension_from_name(dimname);
            char err[200];
            double si;
            if (d < 0) {
                issue(c, p, "internal schema error: unknown x-dimension %s", dimname);
                ok = false;
            } else if (!quantity_parse(inst->u.string.ptr, (Dimension)d, &si, err, sizeof err)) {
                issue(c, p, "invalid %s: %s", dimname, err);
                ok = false;
            }
        }
    }

    if (inst->type == JSON_ARRAY) {
        const JsonValue *b;
        size_t n = json_len(inst);
        if ((b = json_get(s, "minItems")) && json_is_integer(b) && n < (size_t)b->u.number)
            issue(c, p, "must have at least %.0f items (got %zu)", b->u.number, n), ok = false;
        if ((b = json_get(s, "maxItems")) && json_is_integer(b) && n > (size_t)b->u.number)
            issue(c, p, "must have at most %.0f items (got %zu)", b->u.number, n), ok = false;
        const JsonValue *items = json_get(s, "items");
        if (items) {
            for (size_t i = 0; i < n; i++) {
                size_t save = path_push_index(p, i);
                if (!validate(c, items, json_at(inst, i), p, depth + 1)) ok = false;
                path_pop(p, save);
            }
        }
    }

    if (inst->type == JSON_OBJECT) {
        const JsonValue *props = json_get(s, "properties");
        const JsonValue *addl = json_get(s, "additionalProperties");
        const JsonValue *req = json_get(s, "required");
        const JsonValue *b;
        if ((b = json_get(s, "minProperties")) && json_is_integer(b) && json_len(inst) < (size_t)b->u.number)
            issue(c, p, "must have at least %.0f properties", b->u.number), ok = false;
        if ((b = json_get(s, "maxProperties")) && json_is_integer(b) && json_len(inst) > (size_t)b->u.number)
            issue(c, p, "must have at most %.0f properties", b->u.number), ok = false;
        for (size_t i = 0; i < json_len(req); i++) {
            const char *name = json_str(json_at(req, i));
            if (name && !json_get(inst, name)) {
                const char *desc = json_get_str(json_get(props, name), "description", NULL);
                size_t save = path_push(p, name);
                if (desc) issue(c, p, "missing required property \"%s\": %.200s", name, desc);
                else issue(c, p, "missing required property \"%s\"", name);
                path_pop(p, save);
                ok = false;
            }
        }
        for (size_t i = 0; i < json_len(inst); i++) {
            const char *key = json_key_at(inst, i);
            JsonValue *val = json_value_at(inst, i);
            const JsonValue *ps = json_get(props, key);
            size_t save = path_push(p, key);
            if (ps) {
                if (!validate(c, ps, val, p, depth + 1)) ok = false;
            } else if (addl && addl->type == JSON_BOOL && !addl->u.boolean) {
                const char *best = NULL;
                int bd = 99;
                char names[200] = "";
                for (size_t k = 0; k < json_len(props); k++) {
                    const char *pn = json_key_at(props, k);
                    int d = edit_distance(pn, key);
                    if (d < bd) bd = d, best = pn;
                    size_t L = strlen(names);
                    if (L + strlen(pn) + 4 < sizeof names) snprintf(names + L, sizeof names - L, "%s%s", k ? ", " : "", pn);
                }
                if (best && bd <= 3) issue(c, p, "unknown property \"%s\" (did you mean \"%s\"?)", key, best);
                else issue(c, p, "unknown property \"%s\"; allowed: %s", key, names);
                ok = false;
            } else if (addl && addl->type == JSON_OBJECT) {
                if (!validate(c, addl, val, p, depth + 1)) ok = false;
            }
            path_pop(p, save);
        }
        if (ok && c->defaults) {
            for (size_t k = 0; k < json_len(props); k++) {
                const char *pn = json_key_at(props, k);
                const JsonValue *def = json_get(json_value_at(props, k), "default");
                if (def && !json_get(inst, pn) && json_set(inst, pn, json_clone(def))) {
                    /* a default object or array may itself have properties with defaults */
                    size_t save = path_push(p, pn);
                    validate(c, json_value_at(props, k), json_get(inst, pn), p, depth + 1);
                    path_pop(p, save);
                }
            }
        }
    }

    const JsonValue *all = json_get(s, "allOf");
    for (size_t i = 0; i < json_len(all); i++)
        if (!validate(c, json_at(all, i), inst, p, depth + 1)) ok = false;

    const JsonValue *any = json_get(s, "anyOf");
    if (any && json_len(any)) {
        JsonSchemaReport best;
        int first;
        if (count_matches(c, any, inst, p, depth, &first, &best) == 0) {
            for (int k = 0; k < best.count && c->rep && c->rep->count < JSCHEMA_MAX_ERRORS; k++) c->rep->issues[c->rep->count++] = best.issues[k];
            if (!best.count) issue(c, p, "does not match any allowed form");
            ok = false;
        } else if (c->defaults) {
            validate(c, json_at(any, (size_t)first), inst, p, depth + 1);
        }
    }

    const JsonValue *one = json_get(s, "oneOf");
    if (one && json_len(one)) {
        JsonSchemaReport best;
        int first;
        int m = count_matches(c, one, inst, p, depth, &first, &best);
        if (m == 0) {
            for (int k = 0; k < best.count && c->rep && c->rep->count < JSCHEMA_MAX_ERRORS; k++) c->rep->issues[c->rep->count++] = best.issues[k];
            if (!best.count) issue(c, p, "does not match any allowed form");
            ok = false;
        } else if (m > 1) {
            issue(c, p, "is ambiguous: matches %d alternative forms, exactly one is required", m);
            ok = false;
        } else if (c->defaults) {
            validate(c, json_at(one, (size_t)first), inst, p, depth + 1);
        }
    }
    return ok;
}

bool jschema_validate(const JsonValue *schema, const JsonValue *root, JsonValue *instance, bool apply_defaults,
                      JsonSchemaReport *report) {
    JsonSchemaReport local = {0};
    if (!report) report = &local;
    memset(report, 0, sizeof *report);
    VCtx c = {root, false, report};
    PathBuf p = {{0}, 0, 0};
    if (!instance) {
        issue(&c, &p, "missing value");
        return false;
    }
    if (!validate(&c, schema, instance, &p, 0)) return false;
    if (apply_defaults) {
        /* second pass inserts defaults only once the whole instance is known to be valid */
        c.defaults = true;
        validate(&c, schema, instance, &p, 0);
    }
    return report->count == 0;
}

void jschema_report_text(const JsonSchemaReport *r, char *out, size_t cap) {
    size_t used = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < r->count; i++) {
        int n = snprintf(out + used, cap - used, "%s%s: %s", i ? "; " : "", r->issues[i].path, r->issues[i].message);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}

/* ---- static schema check ----------------------------------------------------------------------- */

static const char *KNOWN[] = {"$schema", "$id", "$defs", "$ref", "$comment", "title", "description", "default",
                              "examples", "deprecated", "readOnly", "writeOnly", "format", "type", "enum", "const",
                              "properties", "required", "additionalProperties", "items", "minItems", "maxItems",
                              "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "minLength", "maxLength",
                              "minProperties", "maxProperties", "allOf", "anyOf", "oneOf", "x-unit", "x-dimension",
                              NULL};

static bool check_node(const JsonValue *s, const JsonValue *root, const char *where, char *err, size_t errlen, int depth) {
    if (depth > 64) {
        snprintf(err, errlen, "%s: schema nested too deeply", where);
        return false;
    }
    if (s->type == JSON_BOOL) return true;
    if (s->type != JSON_OBJECT) {
        snprintf(err, errlen, "%s: schema must be an object or boolean", where);
        return false;
    }
    for (size_t i = 0; i < json_len(s); i++) {
        const char *k = json_key_at(s, i);
        bool known = false;
        for (int j = 0; KNOWN[j] && !known; j++) known = strcmp(KNOWN[j], k) == 0;
        if (!known) {
            snprintf(err, errlen, "%s: unsupported schema keyword \"%s\"", where, k);
            return false;
        }
    }
    const char *ref = json_get_str(s, "$ref", NULL);
    if (ref) {
        VCtx c = {root, false, NULL};
        if (!resolve_ref(&c, ref)) {
            snprintf(err, errlen, "%s: $ref %s does not resolve", where, ref);
            return false;
        }
    }
    const char *dim = json_get_str(s, "x-dimension", NULL);
    if (dim && dimension_from_name(dim) < 0) {
        snprintf(err, errlen, "%s: unknown x-dimension %s", where, dim);
        return false;
    }
    const char *unit = json_get_str(s, "x-unit", NULL);
    if (unit && dim) {
        double si;
        char uerr[160];
        if (!unit_to_si(1.0, unit, (Dimension)dimension_from_name(dim), &si, uerr, sizeof uerr)) {
            snprintf(err, errlen, "%s: x-unit %s: %s", where, unit, uerr);
            return false;
        }
    }
    const JsonValue *props = json_get(s, "properties");
    const JsonValue *req = json_get(s, "required");
    for (size_t i = 0; i < json_len(req); i++) {
        const char *name = json_str(json_at(req, i));
        if (!name || !json_get(props, name)) {
            snprintf(err, errlen, "%s: required property \"%s\" is not declared in properties", where, name ? name : "?");
            return false;
        }
    }
    char sub[512];
    for (size_t i = 0; i < json_len(props); i++) {
        snprintf(sub, sizeof sub, "%s/properties/%s", where, json_key_at(props, i));
        if (!check_node(json_value_at(props, i), root, sub, err, errlen, depth + 1)) return false;
    }
    static const char *SUBS[] = {"items", "additionalProperties", NULL};
    for (int k = 0; SUBS[k]; k++) {
        const JsonValue *x = json_get(s, SUBS[k]);
        if (x && x->type == JSON_OBJECT) {
            snprintf(sub, sizeof sub, "%s/%s", where, SUBS[k]);
            if (!check_node(x, root, sub, err, errlen, depth + 1)) return false;
        }
    }
    static const char *LISTS[] = {"allOf", "anyOf", "oneOf", NULL};
    for (int k = 0; LISTS[k]; k++) {
        const JsonValue *x = json_get(s, LISTS[k]);
        for (size_t i = 0; i < json_len(x); i++) {
            snprintf(sub, sizeof sub, "%s/%s/%zu", where, LISTS[k], i);
            if (!check_node(json_at(x, i), root, sub, err, errlen, depth + 1)) return false;
        }
    }
    const JsonValue *en = json_get(s, "enum");
    if (en && (en->type != JSON_ARRAY || !json_len(en))) {
        snprintf(err, errlen, "%s: enum must be a non-empty array", where);
        return false;
    }
    const JsonValue *def = json_get(s, "default");
    if (def) {
        JsonValue *copy = json_clone(def);
        JsonSchemaReport rep;
        bool valid = jschema_validate(s, root, copy, false, &rep);
        json_free(copy);
        if (!valid) {
            char text[256];
            jschema_report_text(&rep, text, sizeof text);
            snprintf(err, errlen, "%s: default does not satisfy the schema (%s)", where, text);
            return false;
        }
    }
    return true;
}

bool jschema_check(const JsonValue *schema, const JsonValue *root, char *err, size_t errlen) {
    const JsonValue *defs = json_get(root, "$defs");
    char where[256];
    for (size_t i = 0; i < json_len(defs); i++) {
        snprintf(where, sizeof where, "#/$defs/%s", json_key_at(defs, i));
        if (!check_node(json_value_at(defs, i), root, where, err, errlen, 0)) return false;
    }
    return check_node(schema, root, "#", err, errlen, 0);
}
