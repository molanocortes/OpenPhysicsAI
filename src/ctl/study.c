/* study.c - study_check (resolve and check without solving) and study_submit (write the study directory, queue the job) */
#define _XOPEN_SOURCE 700
#include "ops_internal.h"
#include "study_internal.h"

#include <ftw.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void study_resolved_hash(const JsonValue *resolved, char out[65]) {
    JsonValue *c = json_clone(resolved);
    const JsonValue *designs = json_get(c, "designs");
    for (size_t i = 0; i < json_len(designs); i++) json_remove(json_get(json_at(designs, i), "geometry"), "path");
    study_hash_json(c, out);
    json_free(c);
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}

/* removes a temporary check folder created by study_check (and nothing else) */
static void remove_check_tree(const char *dir) {
    if (!strstr(dir, "/.study-check-")) return;
    nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

JsonValue *study_check(Engine *e, const JsonValue *def, OpResult *images, NvErr *code, char *err, size_t errlen) {
    StudyIssues is;
    si_init(&is, json_get(def, "accept"));
    *code = NV_ERR_INVALID_PARAMS;
    JsonValue *res = study_resolve(e, def, &is, code, err, errlen);
    if (!res) {
        si_free(&is);
        return NULL;
    }
    static atomic_uint counter;
    char tmp[NV_PATH_MAX], name[96], perr[700];
    snprintf(name, sizeof name, ".study-check-%ld-%u", (long)getpid(), (unsigned)atomic_fetch_add(&counter, 1) + 1);
    if (!engine_resolve_write_path(e, e->cfg.workspace, name, true, true, tmp, sizeof tmp, perr, sizeof perr)) {
        *code = NV_ERR_PERMISSION;
        snprintf(err, errlen, "cannot create a temporary folder for the check in the workspace: %s", perr);
        json_free(res);
        si_free(&is);
        return NULL;
    }
    StudyLog lg;
    memset(&lg, 0, sizeof lg);
    lg.mem = json_array();
    const JsonValue *designs = json_get(res, "designs");
    int nd = (int)json_len(designs);
    StudyDesign d[STUDY_MAX_DESIGNS];
    memset(d, 0, sizeof d);
    JsonValue *dsum = json_array();
    for (int i = 0; i < nd; i++) {
        char pdir[NV_PATH_MAX];
        path_join(pdir, sizeof pdir, tmp, json_get_str(json_at(designs, (size_t)i), "name", "design"));
        study_design_setup(&d[i], &e->cfg, res, i, pdir, &lg, &is);
        JsonValue *coarse = study_coarse_check(&d[i], res, &lg, &is);
        JsonValue *entry = d[i].inspection ? json_clone(d[i].inspection) : json_object();
        json_set_bool(entry, "setup_complete", d[i].ready);
        json_set(entry, "resolution_plan", coarse ? coarse : json_null());
        json_push(dsum, entry);
        if (images && d[i].preview_png && images->nimages < OP_MAX_IMAGES) {
            unsigned char *copy = malloc(d[i].preview_len);
            char iname[96];
            snprintf(iname, sizeof iname, "%s-regions.png", d[i].name);
            if (copy) {
                memcpy(copy, d[i].preview_png, d[i].preview_len);
                op_attach_image(images, "image/png", iname, copy, d[i].preview_len);
            }
        }
    }
    JsonValue *eq = study_equivalence(d, nd, res, &is);
    for (int i = 0; i < nd; i++) study_design_free(&d[i]);
    remove_check_tree(tmp);

    /* how heavy each design is against the load: context for the self-weight assumption */
    const JsonValue *mat = json_get(res, "material"), *ld = json_get(res, "load");
    double rho = json_get_num(mat, "density_kg_m3", NAN), F = json_get_num(ld, "force_n", NAN);
    for (int i = 0; i < nd && isfinite(rho) && F > 0; i++) {
        const JsonValue *g = json_get(json_at(dsum, (size_t)i), "geometry");
        double w = rho * json_get_num(g, "volume_mm3", NAN) * 1e-9 * json_get_num(ld, "gravity_m_s2", 9.80665);
        if (isfinite(w)) json_set_number(json_at(dsum, (size_t)i), "own_weight_to_load_ratio", w / F);
    }

    const char *status = is.not_supported ? "not_supported" : (is.blocking ? "needs_input" : "ready");
    JsonValue *r = json_object();
    json_set_string(r, "status", status);
    char hash[65];
    study_resolved_hash(res, hash);
    json_set_string(r, "study_hash", hash);
    json_set(r, "questions", is.questions), is.questions = NULL;
    json_set(r, "assumptions", is.assumptions), is.assumptions = NULL;
    json_set(r, "accepted", is.accepted), is.accepted = NULL;
    json_set(r, "unsupported", is.unsupported), is.unsupported = NULL;
    json_set(r, "not_evaluated", is.not_evaluated), is.not_evaluated = NULL;
    json_set(r, "designs", dsum);
    json_set(r, "equivalence", eq);
    const JsonValue *sens = json_get(res, "sensitivity");
    int nl = (int)json_len(json_get(json_get(res, "refinement"), "element_sizes_mm"));
    int nsens = (int)json_len(json_get(sens, "poisson_ratio")) + (int)json_len(json_get(sens, "mounting_alternatives")) +
                (json_len(json_get(sens, "youngs_modulus_relative")) == 2) + json_get_bool(sens, "self_weight", false);
    JsonValue *plan = json_set_object(r, "plan");
    json_set(plan, "element_sizes_mm", json_clone(json_get(json_get(res, "refinement"), "element_sizes_mm")));
    json_set_int(plan, "analyses", (long long)nd * (nl + nsens));
    json_set_string(plan, "order", "every design at each element size from coarse to fine, then the sensitivities at the chosen size");
    json_set(plan, "limits", json_clone(json_get(res, "limits")));
    /* preflight storage estimate against the workspace's file system (study_run checks the study directory itself) */
    double vol[STUDY_MAX_DESIGNS];
    for (int i = 0; i < nd; i++) vol[i] = json_get_num(json_get(json_at(dsum, (size_t)i), "geometry"), "volume_mm3", NAN);
    JsonValue *storage = study_storage_plan(res, vol, nd, e->cfg.workspace);
    if (!json_get_bool(storage, "sufficient", true))
        si_warn(&is, "INSUFFICIENT_STORAGE", json_clone(storage),
                "the study needs about %.0f MB at its peak, but the workspace's file system has %.0f MB free (%.0f MB kept in reserve): free space, "
                "choose a study directory on another volume, or set retain_results to none",
                json_get_num(storage, "estimated_peak_bytes", 0) / 1048576, json_get_num(storage, "free_bytes", 0) / 1048576,
                study_storage_reserve_bytes() / 1048576);
    json_set(plan, "storage", storage);
    json_set(r, "warnings", is.warnings), is.warnings = NULL;
    json_set(r, "operations_per_design", lg.mem);
    json_set(r, "resolved", res);
    json_set_string(r, "next_step",
                    !strcmp(status, "ready") ? "study_run with the same definition runs it as a background job"
                    : !strcmp(status, "needs_input")
                        ? "ask the user the blocking questions (or accept an acceptable one with their reason), update the definition and check again"
                        : "the request cannot be answered by this workflow as defined: see unsupported");
    si_free(&is);
    return r;
}

JsonValue *study_submit(Engine *e, const JsonValue *def, const char *directory, const char *reference_dir, JsonValue **report, NvErr *code, char *err,
                        size_t errlen) {
    *report = NULL;
    JsonValue *chk = study_check(e, def, NULL, code, err, errlen);
    if (!chk) return NULL;
    const char *status = json_get_str(chk, "status", "");
    if (strcmp(status, "ready") != 0) {
        *code = NV_ERR_PRECONDITION;
        size_t nb = 0;
        for (size_t i = 0; i < json_len(json_get(chk, "questions")); i++) nb += json_get_bool(json_at(json_get(chk, "questions"), i), "blocking", false);
        if (!strcmp(status, "not_supported"))
            snprintf(err, errlen, "the study cannot run: part of the request is not supported (see details.unsupported)");
        else
            snprintf(err, errlen, "the study needs input before it can run: %zu blocking question(s) (details.questions)", nb);
        *report = chk;
        return NULL;
    }
    const JsonValue *res = json_get(chk, "resolved");
    char dir[NV_PATH_MAX], perr[800], path[NV_PATH_MAX];
    if (!engine_resolve_write_path(e, e->cfg.workspace, directory && directory[0] ? directory : json_get_str(res, "name", "study"), true, false, dir, sizeof dir,
                                   perr, sizeof perr)) {
        *code = NV_ERR_PERMISSION;
        snprintf(err, errlen, "%s", perr);
        json_free(chk);
        return NULL;
    }
    double vol[STUDY_MAX_DESIGNS];
    int nd = (int)json_len(json_get(res, "designs"));
    for (int i = 0; i < nd && i < STUDY_MAX_DESIGNS; i++) vol[i] = json_get_num(json_get(json_at(json_get(chk, "designs"), (size_t)i), "geometry"), "volume_mm3", NAN);
    JsonValue *storage = study_storage_plan(res, vol, nd, dir);
    if (!json_get_bool(storage, "sufficient", true)) {
        *code = NV_ERR_RESOURCE_LIMIT;
        snprintf(err, errlen,
                 "not enough free storage for the study: about %.0f MB needed at the peak with retain_results '%s', %.0f MB free at %s and %.0f MB kept in "
                 "reserve. Free space, choose a directory on another volume, or set retain_results to none (fields are then regenerated by "
                 "study_replay)",
                 json_get_num(storage, "estimated_peak_bytes", 0) / 1048576, json_get_str(storage, "retain_results", ""),
                 json_get_num(storage, "free_bytes", 0) / 1048576, json_get_str(storage, "free_space_measured_at", ""), study_storage_reserve_bytes() / 1048576);
        json_set(chk, "storage", storage);
        *report = chk;
        return NULL;
    }
    json_set(json_get(chk, "plan"), "storage", storage);
    path_join(path, sizeof path, dir, "study.json");
    if (path_exists(path)) {
        *code = NV_ERR_ALREADY_EXISTS;
        snprintf(err, errlen, "a study already exists in %s: choose another directory, or reproduce it with study_replay", dir);
        json_free(chk);
        return NULL;
    }
    if (!engine_resolve_write_path(e, NULL, dir, true, true, dir, sizeof dir, perr, sizeof perr)) {
        *code = NV_ERR_IO;
        snprintf(err, errlen, "%s", perr);
        json_free(chk);
        return NULL;
    }
    char hash[65], now[32];
    study_resolved_hash(res, hash);
    iso_time_now(now, sizeof now);
    path_join(path, sizeof path, dir, "request.json");
    if (!json_write_file(path, def, JSON_PRETTY | JSON_SORTED)) {
        *code = NV_ERR_IO;
        snprintf(err, errlen, "cannot write %s", path);
        json_free(chk);
        return NULL;
    }
    JsonValue *sj_doc = json_object();
    json_set_string(sj_doc, "format", "navier-comparison-study-file");
    json_set_int(sj_doc, "format_version", 1);
    json_set_string(sj_doc, "study_hash", hash);
    json_set_string(sj_doc, "study_hash_covers", "the resolved study without file locations (geometry is identified by SHA-256)");
    json_set_string(sj_doc, "created", now);
    JsonValue *sw = json_set_object(sj_doc, "software");
    json_set_string(sw, "name", "NAVIER");
    json_set_string(sw, "version", NAVIER_AM_VERSION);
    json_set_string(sw, "contract_version", ops_contract_version());
    json_set_string(sw, "compiler", __VERSION__);
    json_set(sj_doc, "resolved", json_clone(res));
    JsonValue *ck = json_set_object(sj_doc, "check_at_submission");
    static const char *const KEEP[] = {"assumptions", "accepted", "questions", "unsupported", "not_evaluated", "warnings", "equivalence", "designs", "plan", NULL};
    for (int k = 0; KEEP[k]; k++) json_set(ck, KEEP[k], json_clone(json_get(chk, KEEP[k])));
    if (reference_dir && reference_dir[0]) json_set_string(sj_doc, "replay_of", reference_dir);
    path_join(path, sizeof path, dir, "study.json");
    bool wrote = json_write_file(path, sj_doc, JSON_PRETTY | JSON_SORTED);
    json_free(sj_doc);
    if (!wrote) {
        *code = NV_ERR_IO;
        snprintf(err, errlen, "cannot write %s", path);
        json_free(chk);
        return NULL;
    }
    StudyJob *sj = calloc(1, sizeof *sj);
    if (!sj) {
        *code = NV_ERR_RESOURCE_LIMIT;
        snprintf(err, errlen, "out of memory");
        json_free(chk);
        return NULL;
    }
    sj->cfg = e->cfg;
    sj->resolved = json_take(chk, "resolved");
    sj->check = chk;
    snprintf(sj->dir, sizeof sj->dir, "%s", dir);
    snprintf(sj->hash, sizeof sj->hash, "%s", hash);
    if (reference_dir) snprintf(sj->reference_dir, sizeof sj->reference_dir, "%s", reference_dir);
    char id[64];
    jobs_new_id(e->jobs, id, sizeof id);
    JsonValue *plan = json_clone(json_get(chk, "plan"));
    long long nq = (long long)json_len(json_get(chk, "assumptions"));
    JobSpec spec = {id, "comparison_study", json_get_str(sj->resolved, "name", "study"), dir, hash, "", json_get_str(sj->resolved, "name", "study"), 0,
                    sj, study_job_run, study_job_free};
    char jerr[400];
    if (!jobs_submit(e->jobs, &spec, jerr, sizeof jerr)) {
        *code = NV_ERR_BUSY;
        snprintf(err, errlen, "cannot queue the study: %s", jerr);
        json_free(plan);
        return NULL;
    }
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    json_set_string(v, "state", "queued");
    json_set_string(v, "analysis", "comparison_study");
    json_set_string(v, "directory", dir);
    json_set_string(v, "study_file", "study.json");
    json_set_string(v, "study_hash", hash);
    json_set(v, "plan", plan);
    json_set_int(v, "recorded_assumptions", nq);
    json_set_string(v, "next_step",
                    "job_status {job_id, wait_seconds} shows progress by design and mesh; when it has finished, study_evidence {job_id} returns the record");
    return v;
}
