/* ops_study.c - comparison-study operations: study_check, study_run, study_evidence, study_replay */
#include "../core/jschema.h"
#include "../core/sha256.h"
#include "ops_internal.h"
#include "study.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the definition from params.definition, or read from params.file and validated like an inline one; NULL when failed */
static JsonValue *definition_of(Engine *e, const char *op, const JsonValue *p, OpResult *out) {
    const JsonValue *inl = json_get(p, "definition");
    const char *file = json_get_str(p, "file", NULL);
    if ((inl != NULL) == (file != NULL)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "pass the study as definition (an object) or as file (a JSON file path)", "give exactly one of definition and file");
        return NULL;
    }
    if (inl) return json_clone(inl);
    char path[NV_PATH_MAX], err[800];
    if (!engine_resolve_read_path(e, file, path, sizeof path, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
        return NULL;
    }
    JsonError je;
    JsonValue *doc = json_read_file(path, (size_t)16 << 20, &je);
    if (!doc) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "the file must hold one JSON object: the study definition", "cannot read %s: %s (line %d)", path, je.message, je.line);
        return NULL;
    }
    const OpInfo *info = ops_find(op);
    JsonValue *schema = info ? ops_input_schema(info) : NULL;
    JsonValue *wrapped = json_object();
    json_set(wrapped, "definition", doc);
    JsonSchemaReport rep;
    memset(&rep, 0, sizeof rep);
    bool ok = schema && jschema_validate(schema, schema, wrapped, true, &rep);
    json_free(schema);
    if (!ok) {
        char text[1200];
        jschema_report_text(&rep, text, sizeof text);
        op_fail(out, NV_ERR_INVALID_PARAMS, "correct the listed fields; the definition schema is in the study_check input schema", "%s: %s", path, text);
        json_free(wrapped);
        return NULL;
    }
    JsonValue *def = json_take(wrapped, "definition");
    json_free(wrapped);
    /* geometry paths in a definition file are relative to the file, so a study folder can be moved or shipped */
    char base[NV_PATH_MAX];
    snprintf(base, sizeof base, "%s", path);
    char *slash = strrchr(base, '/');
    if (slash) *slash = 0;
    const JsonValue *designs = json_get(def, "designs");
    for (size_t i = 0; i < json_len(designs); i++) {
        JsonValue *g = json_get(json_at(designs, i), "geometry");
        const char *gp = json_get_str(g, "path", "");
        if (gp[0] && gp[0] != '/' && gp[0] != '~') {
            char joined[NV_PATH_MAX];
            path_join(joined, sizeof joined, base, gp);
            json_set_string(g, "path", joined);
        }
    }
    return def;
}

static void op_study_check(Engine *e, JsonValue *p, OpResult *out) {
    JsonValue *def = definition_of(e, "study_check", p, out);
    if (!def) return;
    NvErr code = NV_ERR_INVALID_PARAMS;
    char err[1200];
    JsonValue *r = study_check(e, def, out, &code, err, sizeof err);
    json_free(def);
    if (!r) {
        op_fail(out, code, "fix the definition and check again", "%s", err);
        return;
    }
    op_succeed(out, r);
}

static void op_study_run(Engine *e, JsonValue *p, OpResult *out) {
    JsonValue *def = definition_of(e, "study_run", p, out);
    if (!def) return;
    NvErr code = NV_ERR_INVALID_PARAMS;
    char err[1200];
    JsonValue *report = NULL;
    JsonValue *v = study_submit(e, def, json_get_str(p, "directory", NULL), NULL, &report, &code, err, sizeof err);
    json_free(def);
    if (!v) {
        op_fail(out, code,
                report ? "answer the blocking questions (details.questions) with the user, or accept an acceptable one with the user's reason; then run again"
                       : NULL,
                "%s", err);
        if (report) {
            static const char *const KEYS[] = {"status", "questions", "unsupported", "assumptions", "warnings", "equivalence", NULL};
            for (int k = 0; KEYS[k]; k++) op_fail_detail(out, KEYS[k], json_take(report, KEYS[k]));
            json_free(report);
        }
        return;
    }
    op_succeed(out, v);
}

/* the study directory named by params.directory, or the directory of the comparison_study job params.job_id */
static bool study_dir_of(Engine *e, const JsonValue *p, OpResult *out, char *dir, size_t cap) {
    const char *id = json_get_str(p, "job_id", NULL), *d = json_get_str(p, "directory", NULL);
    char err[800];
    if ((id != NULL) == (d != NULL)) {
        op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "give exactly one of job_id and directory");
        return false;
    }
    if (d) {
        if (!engine_resolve_read_path(e, d, dir, cap, err, sizeof err)) {
            op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
            return false;
        }
        return true;
    }
    JsonValue *st = jobs_status_json(e->jobs, id, 0);
    if (!st) {
        op_fail(out, NV_ERR_NOT_FOUND, "job_list shows this session's jobs; for an earlier session pass the study directory", "no job '%s' in this session", id);
        return false;
    }
    if (strcmp(json_get_str(st, "kind", ""), "comparison_study") != 0) {
        op_fail(out, NV_ERR_INVALID_PARAMS, "use results_query or results_quantities for analysis jobs", "job '%s' is a %s job, not a comparison study", id,
                json_get_str(st, "kind", ""));
        json_free(st);
        return false;
    }
    const char *state = json_get_str(st, "state", "");
    if (!strcmp(state, "queued") || !strcmp(state, "running")) {
        op_fail(out, NV_ERR_PRECONDITION, "wait with job_status {\"job_id\": ..., \"wait_seconds\": 60}", "study job '%s' is %s (%s)", id, state,
                json_get_str(st, "stage", ""));
        json_free(st);
        return false;
    }
    snprintf(dir, cap, "%s", json_get_str(st, "run_directory", ""));
    json_free(st);
    return true;
}

static void op_study_evidence(Engine *e, JsonValue *p, OpResult *out) {
    char dir[NV_PATH_MAX], err[800];
    if (!study_dir_of(e, p, out, dir, sizeof dir)) return;
    JsonValue *ev = study_read_evidence(dir, err, sizeof err);
    if (!ev) {
        op_fail(out, NV_ERR_NOT_FOUND, "the study may have failed before writing its record: job_status shows the failure and its recovery options", "%s", err);
        return;
    }
    const char *detail = json_get_str(p, "detail", "summary");
    JsonValue *v;
    if (!strcmp(detail, "full")) {
        v = ev;
    } else {
        v = json_object();
        static const char *const KEYS[] = {"format", "format_version", "status", "nature", "question", "study", "comparison", "numerical_evidence", "interpretation",
                                           "important_uncertainties", "not_evaluated", "unsupported_requests", "failures", "reproduction", NULL};
        for (int k = 0; KEYS[k]; k++)
            if (json_get(ev, KEYS[k])) json_set(v, KEYS[k], json_take(ev, KEYS[k]));
        JsonValue *assumptions = json_take(ev, "assumptions");
        json_set_int(v, "assumptions_count", (long long)json_len(assumptions));
        json_free(assumptions);
        json_set_string(v, "detail_note", "detail: full adds modeled_conditions, assumptions, and every level record with its checks, costs and run identities");
        json_free(ev);
    }
    json_set_string(v, "directory", dir);
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, dir, "report.md");
    json_set_string(v, "report_file", path);
    char hex[65];
    uint64_t bytes = 0;
    if (sha256_file(path, hex, &bytes)) json_set_string(v, "report_sha256", hex);
    path_join(path, sizeof path, dir, "evidence.json");
    if (sha256_file(path, hex, &bytes)) json_set_string(v, "evidence_sha256", hex);
    if (json_get_bool(p, "include_report", false)) {
        path_join(path, sizeof path, dir, "report.md");
        size_t len = 0;
        char *md = path_read_file(path, (size_t)8 << 20, &len, err, sizeof err);
        if (md) json_set_string(v, "report_markdown", md);
        free(md);
    }
    path_join(path, sizeof path, dir, "replay.json");
    if (path_exists(path)) {
        JsonError je;
        JsonValue *rp = json_read_file(path, (size_t)32 << 20, &je);
        if (rp) json_set(v, "replay", rp);
    }
    op_succeed(out, v);
}

static void op_study_replay(Engine *e, JsonValue *p, OpResult *out) {
    char dir[NV_PATH_MAX], err[1200], path[NV_PATH_MAX];
    const char *d = json_get_str(p, "directory", "");
    if (!engine_resolve_read_path(e, d, dir, sizeof dir, err, sizeof err)) {
        op_fail(out, NV_ERR_PERMISSION, NULL, "%s", err);
        return;
    }
    path_join(path, sizeof path, dir, "study.json");
    JsonError je;
    JsonValue *doc = json_read_file(path, (size_t)64 << 20, &je);
    path_join(path, sizeof path, dir, "request.json");
    JsonValue *req = doc ? json_read_file(path, (size_t)16 << 20, &je) : NULL;
    if (!doc || !req) {
        json_free(doc);
        op_fail(out, NV_ERR_NOT_FOUND, "pass the directory of a study written by study_run", "%s: %s", dir, je.message);
        return;
    }
    /* the recorded hash must still describe the resolved study */
    char hash[65];
    study_resolved_hash(json_get(doc, "resolved"), hash);
    if (strcmp(hash, json_get_str(doc, "study_hash", "")) != 0) {
        json_free(doc), json_free(req);
        op_fail(out, NV_ERR_STALE_REFERENCE, "study.json was modified after it was written; a replay must start from the original file",
                "the content of %s/study.json no longer matches its recorded hash", dir);
        return;
    }
    /* geometry from the stored copies, verified against the recorded hashes */
    const JsonValue *rdesigns = json_get(json_get(doc, "resolved"), "designs");
    JsonValue *designs = json_get(req, "designs");
    const JsonValue *checked = json_get(json_get(doc, "check_at_submission"), "designs");
    for (size_t i = 0; i < json_len(designs); i++) {
        JsonValue *dd = json_at(designs, i);
        const char *name = json_get_str(dd, "name", "");
        const char *want = json_get_str(json_get(json_at(rdesigns, i), "geometry"), "sha256", "");
        const char *stored = json_get_str(json_get(json_at(checked, i), "geometry"), "stored_copy", "");
        char copy[NV_PATH_MAX], sub[600], hex[65];
        uint64_t bytes = 0;
        snprintf(sub, sizeof sub, "designs/%s/%s", name, stored);
        path_join(copy, sizeof copy, dir, sub);
        if (!stored[0] || !sha256_file(copy, hex, &bytes) || strcmp(hex, want) != 0) {
            json_free(doc), json_free(req);
            op_fail(out, NV_ERR_STALE_REFERENCE, "the stored input copies are part of the study; restore them from a backup",
                    "design '%s': the stored geometry copy %s is missing or does not match the recorded SHA-256", name, copy);
            return;
        }
        json_set_string(json_get(dd, "geometry"), "path", copy);
    }
    char into[NV_PATH_MAX], now[32];
    const char *given = json_get_str(p, "into", NULL);
    if (given) {
        snprintf(into, sizeof into, "%s", given);
    } else {
        iso_time_now(now, sizeof now);
        for (char *c = now; *c; c++)
            if (*c == ':') *c = '-';
        snprintf(into, sizeof into, "%s/replays/%s", dir, now);
    }
    NvErr code = NV_ERR_INVALID_PARAMS;
    JsonValue *report = NULL;
    JsonValue *v = study_submit(e, req, into, dir, &report, &code, err, sizeof err);
    json_free(doc), json_free(req);
    if (!v) {
        op_fail(out, code, report ? "the replayed definition no longer resolves as it did: see details" : NULL, "%s", err);
        if (report) {
            op_fail_detail(out, "questions", json_take(report, "questions"));
            op_fail_detail(out, "unsupported", json_take(report, "unsupported"));
            json_free(report);
        }
        return;
    }
    json_set_string(v, "replay_of", dir);
    json_set_string(v, "next_step", "when the job has finished, study_evidence {job_id} returns the replayed record with a replay section comparing every quantity");
    op_succeed(out, v);
}

const OpBinding OPS_STUDY_BINDINGS[] = {
    {"study_check", op_study_check},
    {"study_run", op_study_run},
    {"study_evidence", op_study_evidence},
    {"study_replay", op_study_replay},
    {NULL, NULL},
};
