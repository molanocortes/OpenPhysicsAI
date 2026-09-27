/* engine.c - engine lifetime, configuration, access roots, revisions and the change journal */
#include "engine_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ROOT_ADDED, ROOT_COVERED, ROOT_INVALID, ROOT_FULL };

/* A root inside an existing root adds no access and takes no slot (compared on canonical paths, so /tmp/x is covered by
 * /private/tmp). Skipping it can only narrow access if a symbolic link changes later, never widen it. */
static int add_root(char roots[][NV_PATH_MAX], int *n, const char *path) {
    char exp[NV_PATH_MAX], canon[NV_PATH_MAX];
    if (!path_expand(path, exp, sizeof exp)) return ROOT_INVALID;
    size_t L = strlen(exp);
    while (L > 1 && exp[L - 1] == '/') exp[--L] = 0;
    bool have_canon = path_resolve_new(exp, canon, sizeof canon, false, true, NULL, 0);
    for (int i = 0; i < *n; i++) {
        if (strcmp(roots[i], exp) == 0) return ROOT_COVERED;
        char cr[NV_PATH_MAX];
        if (have_canon && path_resolve_new(roots[i], cr, sizeof cr, false, true, NULL, 0) && path_within(canon, cr)) return ROOT_COVERED;
    }
    if (*n >= ENGINE_MAX_ROOTS) return ROOT_FULL;
    snprintf(roots[(*n)++], NV_PATH_MAX, "%s", exp);
    return ROOT_ADDED;
}

void engine_config_default(EngineConfig *c) {
    memset(c, 0, sizeof *c);
    const char *home = getenv("HOME");
    snprintf(c->workspace, sizeof c->workspace, "%s/NAVIER-Projects", home && *home ? home : "/tmp");
    c->max_stl_bytes = (uint64_t)1 << 30;
    c->max_triangles = 5000000;
    c->max_elements = 2000000;
    c->threads = 0;
    if (home && *home) add_root(c->read_roots, &c->nread_roots, home);
    add_root(c->read_roots, &c->nread_roots, "/tmp");
    add_root(c->read_roots, &c->nread_roots, "/private/tmp");
    add_root(c->read_roots, &c->nread_roots, "/Volumes");
    char cwd[NV_PATH_MAX];
    if (getcwd(cwd, sizeof cwd) && strcmp(cwd, "/") != 0) add_root(c->read_roots, &c->nread_roots, cwd);
    add_root(c->write_roots, &c->nwrite_roots, c->workspace);
    const char *tmp = getenv("TMPDIR");
    if (tmp && *tmp) add_root(c->write_roots, &c->nwrite_roots, tmp);
}

static void full_message(char *err, size_t errlen, const char *path, const char *kind, char roots[][NV_PATH_MAX], int n) {
    if (!err) return;
    int w = snprintf(err, errlen,
                     "cannot add %s root '%s': the limit of %d %s roots is reached. Roots in use (defaults included): ", kind, path, ENGINE_MAX_ROOTS, kind);
    for (int i = 0; i < n && w > 0 && (size_t)w < errlen; i++) w += snprintf(err + w, errlen - (size_t)w, "%s%s", i ? ", " : "", roots[i]);
    if (w > 0 && (size_t)w < errlen)
        snprintf(err + w, errlen - (size_t)w, ". A folder inside a root needs no slot of its own: allow a common parent folder instead");
}

bool engine_config_add_root(EngineConfig *c, bool write, const char *path, char *err, size_t errlen) {
    int nw0 = c->nwrite_roots;
    int rc = write ? add_root(c->write_roots, &c->nwrite_roots, path) : add_root(c->read_roots, &c->nread_roots, path);
    if (rc == ROOT_INVALID) {
        if (err) snprintf(err, errlen, "cannot add %s root '%s': not a valid path", write ? "write" : "read", path ? path : "");
        return false;
    }
    if (rc == ROOT_FULL) {
        if (write) full_message(err, errlen, path, "write", c->write_roots, c->nwrite_roots);
        else full_message(err, errlen, path, "read", c->read_roots, c->nread_roots);
        return false;
    }
    /* anything writable must also be readable (to reopen projects): if that cannot be granted, the write root is withdrawn */
    if (write && add_root(c->read_roots, &c->nread_roots, path) == ROOT_FULL) {
        c->nwrite_roots = nw0;
        full_message(err, errlen, path, "read (implied by a write root)", c->read_roots, c->nread_roots);
        return false;
    }
    return true;
}

bool engine_config_set_workspace(EngineConfig *c, const char *path, char *err, size_t errlen) {
    char exp[NV_PATH_MAX];
    if (!path_expand(path, exp, sizeof exp)) {
        if (err) snprintf(err, errlen, "invalid workspace path '%s'", path);
        return false;
    }
    snprintf(c->workspace, sizeof c->workspace, "%s", exp);
    return engine_config_add_root(c, true, exp, err, errlen);
}

Engine *engine_create(const EngineConfig *cfg, char *err, size_t errlen) {
    Engine *e = calloc(1, sizeof *e);
    if (!e) {
        if (err) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (cfg) e->cfg = *cfg;
    else engine_config_default(&e->cfg);
    pthread_mutex_init(&e->mtx, NULL);
    pthread_mutex_init(&e->stat_mtx, NULL);
    char rerr[512];
    e->jobs = jobs_create();
    if (!e->jobs || !ops_registry_init(rerr, sizeof rerr)) {
        if (err) snprintf(err, errlen, "%s", e->jobs ? rerr : "out of memory");
        jobs_destroy(e->jobs);
        pthread_mutex_destroy(&e->mtx);
        pthread_mutex_destroy(&e->stat_mtx);
        free(e);
        return NULL;
    }
    return e;
}

void engine_destroy(Engine *e) {
    if (!e) return;
    jobs_destroy(e->jobs); /* cancels and joins running jobs first */
    project_free(e->proj);
    for (int i = 0; i < IDEM_SLOTS; i++) {
        json_free(e->idem[i].value);
        for (int k = 0; k < e->idem[i].nimages; k++) free(e->idem[i].images[k].data);
    }
    pthread_mutex_destroy(&e->mtx);
    pthread_mutex_destroy(&e->stat_mtx);
    free(e);
}

const EngineConfig *engine_get_config(const Engine *e) { return &e->cfg; }

uint64_t engine_revision(Engine *e) {
    pthread_mutex_lock(&e->stat_mtx);
    uint64_t r = e->stat_revision;
    pthread_mutex_unlock(&e->stat_mtx);
    return r;
}

uint64_t engine_change_counter(Engine *e) {
    pthread_mutex_lock(&e->stat_mtx);
    uint64_t r = e->stat_changes;
    pthread_mutex_unlock(&e->stat_mtx);
    return r;
}

void engine_lock(Engine *e) { pthread_mutex_lock(&e->mtx); }
void engine_unlock(Engine *e) { pthread_mutex_unlock(&e->mtx); }
Project *engine_project_locked(Engine *e) { return e->proj; }

static void publish_stats(Engine *e) {
    pthread_mutex_lock(&e->stat_mtx);
    e->stat_revision = e->proj ? e->proj->revision : 0;
    e->stat_changes++;
    pthread_mutex_unlock(&e->stat_mtx);
}

static void journal_add(Engine *e, const char *op, const char *summary) {
    JournalEntry *j = &e->journal[e->journal_next];
    j->revision = e->proj ? e->proj->revision : e->last_revision;
    snprintf(j->op, sizeof j->op, "%s", op);
    snprintf(j->summary, sizeof j->summary, "%s", summary);
    snprintf(j->transport, sizeof j->transport, "%s", e->caller && e->caller->transport ? e->caller->transport : "internal");
    iso_time_now(j->time, sizeof j->time);
    e->journal_next = (e->journal_next + 1) % JOURNAL_SLOTS;
    if (e->journal_len < JOURNAL_SLOTS) e->journal_len++;
}

void engine_set_project(Engine *e, Project *p) {
    if (e->proj && e->proj != p) project_free(e->proj);
    e->proj = p;
    if (p) {
        uint64_t rev = e->last_revision + 1;
        if (p->revision >= rev) rev = p->revision + 1;
        p->revision = rev;
        e->last_revision = rev;
    }
    publish_stats(e);
}

void engine_touch(Engine *e, const char *op, const char *fmt, ...) {
    char summary[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(summary, sizeof summary, fmt, ap);
    va_end(ap);
    uint64_t rev = e->last_revision + 1;
    if (e->proj) {
        if (e->proj->revision >= rev) rev = e->proj->revision + 1;
        e->proj->revision = rev;
        iso_time_now(e->proj->modified, sizeof e->proj->modified);
    }
    e->last_revision = rev;
    journal_add(e, op, summary);
    publish_stats(e);
}

JsonValue *engine_journal_json(Engine *e, uint64_t since, int max) {
    JsonValue *arr = json_array();
    int n = e->journal_len, start = (e->journal_next - n + JOURNAL_SLOTS) % JOURNAL_SLOTS;
    int first = 0;
    if (max > 0 && n > max) first = n - max;
    for (int i = first; i < n; i++) {
        const JournalEntry *j = &e->journal[(start + i) % JOURNAL_SLOTS];
        if (j->revision <= since) continue;
        JsonValue *o = json_object();
        json_set_int(o, "revision", (long long)j->revision);
        json_set_string(o, "operation", j->op);
        json_set_string(o, "summary", j->summary);
        json_set_string(o, "transport", j->transport);
        json_set_string(o, "time", j->time);
        json_push(arr, o);
    }
    return arr;
}

static bool within_roots(const char roots[][NV_PATH_MAX], int n, const char *canonical) {
    for (int i = 0; i < n; i++) {
        char canon_root[NV_PATH_MAX];
        if (path_resolve_new(roots[i], canon_root, sizeof canon_root, false, true, NULL, 0) && path_within(canonical, canon_root))
            return true;
    }
    return false;
}

static void roots_text(const char roots[][NV_PATH_MAX], int n, char *out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < n && used < cap; i++) {
        int w = snprintf(out + used, cap - used, "%s%s", i ? ", " : "", roots[i]);
        if (w < 0) break;
        used += (size_t)w;
    }
}

bool engine_resolve_read_path(Engine *e, const char *in, char *out, size_t cap, char *err, size_t errlen) {
    if (!path_real(in, out, cap)) {
        if (err) snprintf(err, errlen, "file not found: %s", in);
        return false;
    }
    if (!within_roots(e->cfg.read_roots, e->cfg.nread_roots, out)) {
        char roots[1024];
        roots_text(e->cfg.read_roots, e->cfg.nread_roots, roots, sizeof roots);
        if (err) snprintf(err, errlen, "'%s' is outside the allowed read roots (%s)", out, roots);
        return false;
    }
    return true;
}

bool engine_resolve_write_path(Engine *e, const char *base, const char *in, bool is_dir, bool mkdirs, char *out, size_t cap,
                               char *err, size_t errlen) {
    char joined[NV_PATH_MAX];
    if (in[0] != '/' && in[0] != '~' && base) {
        if (!path_join(joined, sizeof joined, base, in)) {
            if (err) snprintf(err, errlen, "path too long");
            return false;
        }
    } else {
        snprintf(joined, sizeof joined, "%s", in);
    }
    if (!path_resolve_new(joined, out, cap, false, is_dir, err, errlen)) return false;
    if (!within_roots(e->cfg.write_roots, e->cfg.nwrite_roots, out)) {
        char roots[1024];
        roots_text(e->cfg.write_roots, e->cfg.nwrite_roots, roots, sizeof roots);
        if (err) snprintf(err, errlen, "'%s' is outside the allowed write roots (%s); the server operator can add roots with --allow-write", out, roots);
        return false;
    }
    if (mkdirs) {
        char tmp[NV_PATH_MAX];
        if (!path_resolve_new(out, tmp, sizeof tmp, true, is_dir, err, errlen)) return false;
    }
    return true;
}

JsonValue *engine_roots_json(Engine *e) {
    JsonValue *o = json_object();
    json_set_string(o, "workspace", e->cfg.workspace);
    JsonValue *r = json_set_array(o, "read_roots");
    for (int i = 0; i < e->cfg.nread_roots; i++) json_push(r, json_string(e->cfg.read_roots[i]));
    JsonValue *w = json_set_array(o, "write_roots");
    for (int i = 0; i < e->cfg.nwrite_roots; i++) json_push(w, json_string(e->cfg.write_roots[i]));
    return o;
}
