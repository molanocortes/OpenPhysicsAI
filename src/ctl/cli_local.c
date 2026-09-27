/* cli_local.c - navier-ctl without a server: operations, comparison studies and diagnostics on an engine inside the
 * command-line process (cli_local.h) */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "cli_local.h"

#include "../core/paths.h"
#include "../net/ctlclient.h"
#include "../net/ctlserver.h"
#include "matlib.h"
#include "ops.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

static const OpCaller CLI_CALLER = {"cli", "navier-ctl --embedded"};

static void print_json(const JsonValue *v) {
    char *t = json_dump(v, JSON_PRETTY, NULL, NULL);
    if (t) printf("%s\n", t);
    free(t);
}

static char *read_text(const char *arg) {
    FILE *f = !strcmp(arg, "-") ? stdin : fopen(arg[0] == '@' ? arg + 1 : arg, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0, n;
    char *buf = malloc(cap);
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 2) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb, cap *= 2;
        }
    }
    if (buf) buf[len] = 0;
    if (f != stdin) fclose(f);
    return buf;
}

/* runs an operation; the value on success (caller frees), NULL after printing the error */
static JsonValue *op(Engine *e, const char *name, JsonValue *params, bool quiet) {
    OpResult r;
    ops_invoke(e, name, params, &CLI_CALLER, &r);
    json_free(params);
    JsonValue *v = NULL;
    if (r.ok) {
        v = json_clone(r.value);
    } else if (!quiet) {
        fprintf(stderr, "navier-ctl: %s failed\n", name);
        char *t = json_dump(r.error, JSON_PRETTY, NULL, NULL);
        if (t) fprintf(stderr, "%s\n", t);
        free(t);
    }
    op_result_free(&r);
    return v;
}

/* waits for a job, reporting stage changes on stderr; the final status (caller frees) */
static JsonValue *wait_job(Engine *e, const char *id, bool progress) {
    char last[200] = "";
    for (;;) {
        JsonValue *p = json_object();
        json_set_string(p, "job_id", id);
        json_set_number(p, "wait_seconds", 5);
        JsonValue *st = op(e, "job_status", p, false);
        if (!st) return NULL;
        const char *state = json_get_str(st, "state", ""), *stage = json_get_str(st, "stage", "");
        if (progress && strcmp(stage, last) != 0) {
            fprintf(stderr, "  %3.0f%%  %s\n", 100 * json_get_num(st, "progress", 0), stage);
            snprintf(last, sizeof last, "%s", stage);
        }
        if (strcmp(state, "queued") != 0 && strcmp(state, "running") != 0) return st;
        json_free(st);
    }
}

static void print_questions(const JsonValue *report) {
    const JsonValue *qs = json_get(report, "questions");
    for (size_t i = 0; i < json_len(qs); i++) {
        const JsonValue *q = json_at(qs, i);
        printf("  [%s%s] %s\n      %s\n", json_get_bool(q, "blocking", false) ? "blocking" : "question", json_get_bool(q, "acceptable", false) ? ", acceptable" : "",
               json_get_str(q, "id", ""), json_get_str(q, "question", ""));
    }
    const JsonValue *un = json_get(report, "unsupported");
    for (size_t i = 0; i < json_len(un); i++) printf("  [unsupported] %s\n", json_get_str(json_at(un, i), "text", ""));
}

static int study_cmd(Engine *e, int argc, char **argv, int i) {
    if (i >= argc) {
        fprintf(stderr, "usage: navier-ctl --embedded study check FILE | run FILE [--dir DIR] | replay DIR [--into DIR] | evidence DIR [--full] | report DIR\n");
        return 2;
    }
    const char *sub = argv[i++];
    const char *arg = i < argc ? argv[i++] : NULL;
    if (!arg) {
        fprintf(stderr, "navier-ctl: study %s needs a file or directory\n", sub);
        return 2;
    }
    char abs[NV_PATH_MAX];
    if (!path_expand(arg, abs, sizeof abs)) snprintf(abs, sizeof abs, "%s", arg);
    const char *dir_opt = NULL, *into = NULL;
    bool full = false;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir_opt = argv[++i];
        else if (!strcmp(argv[i], "--into") && i + 1 < argc) into = argv[++i];
        else if (!strcmp(argv[i], "--full")) full = true;
    }
    if (!strcmp(sub, "check")) {
        JsonValue *p = json_object();
        json_set_string(p, "file", abs);
        JsonValue *v = op(e, "study_check", p, false);
        if (!v) return 1;
        const char *status = json_get_str(v, "status", "");
        printf("status: %s (study hash %s)\n", status, json_get_str(v, "study_hash", ""));
        print_questions(v);
        const JsonValue *w = json_get(v, "warnings");
        for (size_t k = 0; k < json_len(w); k++) printf("  [warning %s] %s\n", json_get_str(json_at(w, k), "code", ""), json_get_str(json_at(w, k), "message", ""));
        printf("equivalence of mounting and load between designs: %s\n", json_get_str(json_get(v, "equivalence"), "status", ""));
        printf("assumptions recorded: %zu; analyses planned: %lld\n", json_len(json_get(v, "assumptions")), json_get_int(json_get(v, "plan"), "analyses", 0));
        int code = !strcmp(status, "ready") ? 0 : (!strcmp(status, "needs_input") ? 4 : 5);
        json_free(v);
        return code;
    }
    if (!strcmp(sub, "run") || !strcmp(sub, "replay")) {
        JsonValue *p = json_object();
        OpResult r;
        if (!strcmp(sub, "run")) {
            json_set_string(p, "file", abs);
            if (dir_opt) {
                char d[NV_PATH_MAX];
                json_set_string(p, "directory", path_expand(dir_opt, d, sizeof d) ? d : dir_opt);
            }
        } else {
            json_set_string(p, "directory", abs);
            if (into) {
                char d[NV_PATH_MAX];
                json_set_string(p, "into", path_expand(into, d, sizeof d) ? d : into);
            }
        }
        ops_invoke(e, !strcmp(sub, "run") ? "study_run" : "study_replay", p, &CLI_CALLER, &r);
        json_free(p);
        if (!r.ok) {
            fprintf(stderr, "navier-ctl: %s\n", json_get_str(r.error, "message", "refused"));
            const JsonValue *det = json_get(r.error, "details");
            if (json_get(det, "questions") || json_get(det, "unsupported")) print_questions(det);
            int code = json_get(det, "questions") ? 4 : 1;
            op_result_free(&r);
            return code;
        }
        char id[64], dir[NV_PATH_MAX];
        snprintf(id, sizeof id, "%s", json_get_str(r.value, "job_id", ""));
        snprintf(dir, sizeof dir, "%s", json_get_str(r.value, "directory", ""));
        op_result_free(&r);
        fprintf(stderr, "study job %s in %s\n", id, dir);
        JsonValue *st = wait_job(e, id, true);
        const char *state = json_get_str(st, "state", "failed");
        JsonValue *ep = json_object();
        json_set_string(ep, "directory", dir);
        JsonValue *ev = op(e, "study_evidence", ep, true);
        int code = 0;
        if (ev) {
            printf("status: %s\noutcome: %s\n%s\n", json_get_str(ev, "status", ""), json_get_str(json_get(ev, "comparison"), "outcome", ""),
                   json_get_str(json_get(ev, "comparison"), "statement", ""));
            printf("report: %s\nevidence: %s/evidence.json\n", json_get_str(ev, "report_file", ""), dir);
            const JsonValue *rp = json_get(ev, "replay");
            if (rp) {
                printf("replay: %s (%lld of %lld quantities bitwise identical, largest relative difference %.3g)\n", json_get_str(rp, "outcome", ""),
                       json_get_int(rp, "bitwise_identical", 0), json_get_int(rp, "compared", 0), json_get_num(rp, "largest_relative_difference", NAN));
                if (strcmp(json_get_str(rp, "outcome", ""), "reproduced") != 0) code = 6;
            }
        }
        if (strcmp(state, "succeeded") != 0) {
            fprintf(stderr, "navier-ctl: the study job %s: %s\n", state, json_get_str(json_get(st, "error"), "message", ""));
            code = 3;
        }
        json_free(st), json_free(ev);
        return code;
    }
    if (!strcmp(sub, "evidence")) {
        JsonValue *p = json_object();
        json_set_string(p, "directory", abs);
        json_set_string(p, "detail", full ? "full" : "summary");
        JsonValue *v = op(e, "study_evidence", p, false);
        if (!v) return 1;
        print_json(v);
        json_free(v);
        return 0;
    }
    if (!strcmp(sub, "report")) {
        char path[NV_PATH_MAX], err[300];
        path_join(path, sizeof path, abs, "report.md");
        size_t n = 0;
        char *t = path_read_file(path, (size_t)16 << 20, &n, err, sizeof err);
        if (!t) {
            fprintf(stderr, "navier-ctl: %s\n", err);
            return 1;
        }
        fwrite(t, 1, n, stdout);
        free(t);
        return 0;
    }
    fprintf(stderr, "navier-ctl: unknown study command %s\n", sub);
    return 2;
}

/* ------------------------------------------------------------------------------------------------ doctor */

typedef struct Diag {
    JsonValue *checks;
    int failed;
} Diag;

static void diag(Diag *d, const char *name, const char *status, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void diag(Diag *d, const char *name, const char *status, const char *fmt, ...) {
    char text[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    JsonValue *o = json_object();
    json_set_string(o, "check", name);
    json_set_string(o, "status", status);
    json_set_string(o, "detail", text);
    json_push(d->checks, o);
    if (!strcmp(status, "fail")) d->failed++;
}

static bool self_path(char *out, size_t cap) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)cap;
    char tmp[NV_PATH_MAX];
    uint32_t tsize = sizeof tmp;
    if (_NSGetExecutablePath(tmp, &tsize) != 0) return false;
    (void)size;
    return path_real(tmp, out, cap);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
#endif
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}

static void write_box(const char *path, double lx, double ly, double lz) {
    double p[8][3] = {{0, 0, 0}, {lx, 0, 0}, {lx, ly, 0}, {0, ly, 0}, {0, 0, lz}, {lx, 0, lz}, {lx, ly, lz}, {0, ly, lz}};
    int q[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    FILE *f = fopen(path, "wb");
    if (!f) return;
    char header[80] = "navier doctor reference box";
    fwrite(header, 1, 80, f);
    uint32_t n = 12;
    fwrite(&n, 4, 1, f);
    for (int k = 0; k < 6; k++) {
        int t[2][3] = {{q[k][0], q[k][1], q[k][2]}, {q[k][0], q[k][2], q[k][3]}};
        for (int s = 0; s < 2; s++) {
            float z[3] = {0, 0, 0};
            fwrite(z, 4, 3, f);
            for (int v = 0; v < 3; v++) {
                float c[3] = {(float)p[t[s][v]][0], (float)p[t[s][v]][1], (float)p[t[s][v]][2]};
                fwrite(c, 4, 3, f);
            }
            uint16_t attr = 0;
            fwrite(&attr, 2, 1, f);
        }
    }
    fclose(f);
}

/* MCP stdio handshake with navier-mcp next to this executable: initialize, then tools/list */
static void check_mcp(Diag *d, const char *mcp_path, const char *workspace) {
    int in[2], outp[2];
    if (pipe(in) != 0 || pipe(outp) != 0) {
        diag(d, "mcp_stdio_transport", "fail", "cannot create pipes");
        return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, outp[0]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) posix_spawn_file_actions_adddup2(&fa, devnull, 2);
    char *args[] = {(char *)mcp_path, "--embedded", "--workspace", (char *)workspace, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, mcp_path, &fa, NULL, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]), close(outp[1]);
    if (devnull >= 0) close(devnull);
    if (rc != 0) {
        close(in[1]), close(outp[0]);
        diag(d, "mcp_stdio_transport", "fail", "cannot start %s: %s", mcp_path, strerror(rc));
        return;
    }
    const char *msgs = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"navier-doctor\",\"version\":\"1\"}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";
    ssize_t wr = write(in[1], msgs, strlen(msgs));
    (void)wr;
    char *buf = malloc(4 << 20);
    size_t len = 0;
    int responses = 0;
    long long tools = -1;
    char proto[40] = "";
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    while (buf && responses < 2) {
        struct pollfd pf = {outp[0], POLLIN, 0};
        gettimeofday(&t1, NULL);
        int left = 15000 - (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
        if (left <= 0 || poll(&pf, 1, left) <= 0) break;
        ssize_t n = read(outp[0], buf + len, (4 << 20) - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = 0;
        char *nl;
        while ((nl = memchr(buf, '\n', len)) != NULL) {
            *nl = 0;
            JsonError je;
            JsonValue *msg = json_parse(buf, (size_t)(nl - buf), NULL, &je);
            if (msg) {
                const JsonValue *res = json_get(msg, "result");
                if (json_get_int(msg, "id", 0) == 1) snprintf(proto, sizeof proto, "%s", json_get_str(res, "protocolVersion", ""));
                if (json_get_int(msg, "id", 0) == 2) tools = (long long)json_len(json_get(res, "tools"));
                responses++;
                json_free(msg);
            }
            size_t used = (size_t)(nl - buf) + 1;
            memmove(buf, nl + 1, len - used);
            len -= used;
        }
    }
    free(buf);
    close(in[1]);
    close(outp[0]);
    int status = 0;
    for (int k = 0; k < 50 && waitpid(pid, &status, WNOHANG) == 0; k++) usleep(100000);
    if (waitpid(pid, &status, WNOHANG) == 0) kill(pid, SIGKILL), waitpid(pid, &status, 0);
    if (tools == ops_count() && proto[0])
        diag(d, "mcp_stdio_transport", "pass", "%s answered initialize (protocol %s) and listed %lld tools", mcp_path, proto, tools);
    else
        diag(d, "mcp_stdio_transport", "fail", "%s: %d responses, protocol '%s', %lld tools (expected %d)", mcp_path, responses, proto, tools, ops_count());
}

static void check_reference_solve(Diag *d, EngineConfig *cfg) {
    char tmpl[NV_PATH_MAX], err[600];
    const char *tmpdir = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/navier-doctor-XXXXXX", tmpdir && tmpdir[0] ? tmpdir : "/tmp");
    if (!mkdtemp(tmpl)) {
        diag(d, "reference_solve", "fail", "cannot create a temporary folder: %s", strerror(errno));
        return;
    }
    char dir[NV_PATH_MAX];
    if (!path_real(tmpl, dir, sizeof dir)) snprintf(dir, sizeof dir, "%s", tmpl);
    EngineConfig c = *cfg;
    engine_config_add_root(&c, false, dir, err, sizeof err);
    engine_config_add_root(&c, true, dir, err, sizeof err);
    Engine *e = engine_create(&c, err, sizeof err);
    if (!e) {
        diag(d, "reference_solve", "fail", "cannot start an engine: %s", err);
        return;
    }
    char c10[NV_PATH_MAX], c20[NV_PATH_MAX], study[NV_PATH_MAX];
    path_join(c10, sizeof c10, dir, "c10.stl");
    path_join(c20, sizeof c20, dir, "c20.stl");
    path_join(study, sizeof study, dir, "study");
    write_box(c10, 100, 10, 10);
    write_box(c20, 100, 10, 5);
    char text[4096];
    snprintf(text, sizeof text,
             "{\"name\":\"doctor_cantilevers\",\"question\":\"reference solve of the diagnostic\",\"designs\":["
             "{\"name\":\"C10\",\"geometry\":{\"path\":\"%s\",\"units\":\"mm\"},\"mounting_region\":{\"query\":{\"plane\":{\"axis\":\"x\",\"at\":\"min\"}},\"description\":\"root\"},"
             "\"load_region\":{\"query\":{\"plane\":{\"axis\":\"x\",\"at\":\"max\"}},\"description\":\"tip\"}},"
             "{\"name\":\"C20\",\"geometry\":{\"path\":\"%s\",\"units\":\"mm\"},\"mounting_region\":{\"query\":{\"plane\":{\"axis\":\"x\",\"at\":\"min\"}},\"description\":\"root\"},"
             "\"load_region\":{\"query\":{\"plane\":{\"axis\":\"x\",\"at\":\"max\"}},\"description\":\"tip\"}}],"
             "\"material\":{\"record\":{\"id\":\"steel_test\",\"name\":\"steel test values\",\"family\":\"metal\",\"status\":\"user_supplied\",\"provenance\":\"diagnostic\","
             "\"youngs_modulus_pa\":{\"value\":200e9},\"poisson_ratio\":{\"value\":0.3},\"density_kg_m3\":{\"value\":7850}},\"source\":\"user\"},"
             "\"mounting\":{\"idealization\":\"fixed\",\"source\":\"user\"},\"load\":{\"kind\":\"force\",\"force\":\"10 N\",\"direction\":\"-z\",\"source\":\"user\"},"
             "\"refinement\":{\"element_sizes\":[\"2.5 mm\",\"1.25 mm\"]},\"accept\":[{\"id\":\"equivalence\",\"reason\":\"the cross-section is the difference\"}]}",
             c10, c20);
    JsonError je;
    JsonValue *def = json_parse(text, strlen(text), NULL, &je);
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    JsonValue *p = json_object();
    json_set(p, "definition", def);
    json_set_string(p, "directory", study);
    JsonValue *v = op(e, "study_run", p, true);
    JsonValue *st = v ? wait_job(e, json_get_str(v, "job_id", ""), false) : NULL;
    JsonValue *ep = json_object();
    json_set_string(ep, "directory", study);
    JsonValue *ev = st ? op(e, "study_evidence", ep, true) : (json_free(ep), NULL);
    gettimeofday(&t1, NULL);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + 1e-6 * (double)(t1.tv_usec - t0.tv_usec);
    const JsonValue *vals = json_get(json_get(ev, "comparison"), "values");
    double u10 = NAN;
    for (size_t k = 0; k < json_len(vals); k++)
        if (!strcmp(json_get_str(json_at(vals, k), "design", ""), "C10")) u10 = json_get_num(json_at(vals, k), "load_region_displacement_mm", NAN);
    const double ref = 0.0201560; /* Timoshenko beam theory: F L^3 / 3EI + F L / kGA, 10 N, 100 x 10 x 10 mm, steel */
    bool checks_pass = true;
    const JsonValue *ne = json_get(ev, "numerical_evidence");
    for (size_t k = 0; k < json_len(ne); k++) {
        const char *chk = json_get_str(json_at(ne, k), "check", "");
        if (strstr(chk, "discretisation") || strstr(chk, "small deformation")) continue;
        checks_pass &= !strcmp(json_get_str(json_at(ne, k), "outcome", ""), "pass");
    }
    double rel = u10 / ref - 1;
    if (ev && isfinite(u10) && fabs(rel) <= 0.02 && checks_pass)
        diag(d, "reference_solve", "pass",
             "cantilever study (two designs, two meshes, %.1f s): tip-region deflection %.6f mm against beam theory %.6f mm (%+.2f%%, criterion 2%%); balance, energy "
             "and mapping checks pass",
             secs, u10, ref, 100 * rel);
    else
        diag(d, "reference_solve", "fail", "cantilever study: state %s, deflection %.6g mm against %.6f mm, checks %s", json_get_str(st, "state", "not started"), u10, ref,
             checks_pass ? "pass" : "fail");
    json_free(v), json_free(st), json_free(ev);
    engine_destroy(e);
    nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static int doctor(EngineConfig *cfg, bool as_json, bool skip_solve) {
    Diag d = {json_array(), 0};
    char rerr[600];
    bool registry = ops_registry_init(rerr, sizeof rerr);
    diag(&d, "version", "info", "NAVIER %s, operation contract %s, built with %s (IEEE double precision, no fast-math, no FP contraction in the core)",
         NAVIER_AM_VERSION, ops_contract_version(), __VERSION__);
    char self[NV_PATH_MAX] = "", dirn[NV_PATH_MAX] = "", mcp[NV_PATH_MAX] = "";
    if (self_path(self, sizeof self)) {
        snprintf(dirn, sizeof dirn, "%s", self);
        char *slash = strrchr(dirn, '/');
        if (slash) *slash = 0;
        char server[NV_PATH_MAX];
        path_join(mcp, sizeof mcp, dirn, "navier-mcp");
        path_join(server, sizeof server, dirn, "navier-server");
        bool m = access(mcp, X_OK) == 0, s = access(server, X_OK) == 0;
        diag(&d, "executables", m && s ? "pass" : "fail", "%s: navier-mcp %s, navier-server %s", dirn, m ? "present" : "MISSING", s ? "present" : "MISSING");
    } else {
        diag(&d, "executables", "warn", "cannot determine the location of this executable");
    }
#ifdef __APPLE__
    {
        char foreign[1200] = "";
        uint32_t n = _dyld_image_count();
        int count = 0;
        for (uint32_t k = 0; k < n; k++) {
            const char *img = _dyld_get_image_name(k);
            if (!img || (self[0] && !strcmp(img, self)) || !strncmp(img, "/usr/lib/", 9) || !strncmp(img, "/System/", 8)) continue;
            char r[NV_PATH_MAX];
            if (self[0] && path_real(img, r, sizeof r) && !strcmp(r, self)) continue;
            size_t L = strlen(foreign);
            snprintf(foreign + L, sizeof foreign - L, "%s%s", count++ ? ", " : "", img);
        }
        if (count) diag(&d, "runtime_libraries", "fail", "non-system libraries loaded: %s", foreign);
        else diag(&d, "runtime_libraries", "pass", "only macOS system libraries are loaded (%u images)", n);
    }
#else
    diag(&d, "runtime_libraries", "info", "linked against the C library, libm and pthreads only");
#endif
    char err[600];
    snprintf(err, sizeof err, "%s", rerr);
    if (registry)
        diag(&d, "operation_contract", ops_find("study_run") ? "pass" : "fail", "%d operations, contract %s, comparison-study operations %s", ops_count(),
             ops_contract_version(), ops_find("study_run") ? "present" : "MISSING");
    else
        diag(&d, "operation_contract", "fail", "the embedded operation schema is invalid: %s", err);
    const JsonValue *lib = matlib_builtin(err, sizeof err);
    if (lib) diag(&d, "material_library", "pass", "%zu embedded records, each labelled with its status (the library holds demonstration values)", json_len(json_get(lib, "materials")));
    else diag(&d, "material_library", "fail", "%s", err);
    /* writable locations */
    const char *tmpdir = getenv("TMPDIR");
    const char *places[2] = {cfg->workspace, tmpdir && tmpdir[0] ? tmpdir : "/tmp"};
    const char *names[2] = {"workspace_writable", "temp_writable"};
    for (int k = 0; k < 2; k++) {
        char probe[NV_PATH_MAX];
        bool ok = path_mkdirs(places[k]);
        snprintf(probe, sizeof probe, "%s/.navier-doctor-probe-%ld", places[k], (long)getpid());
        FILE *f = ok ? fopen(probe, "w") : NULL;
        ok = f && fputs("probe\n", f) >= 0;
        if (f) fclose(f);
        remove(probe);
        diag(&d, names[k], ok ? "pass" : "fail", "%s %s", places[k], ok ? "can be created and written" : "is NOT writable");
    }
    JsonValue *roots = json_array();
    for (int k = 0; k < cfg->nread_roots; k++) json_push(roots, json_string(cfg->read_roots[k]));
    char *rt = json_dump(roots, 0, NULL, NULL);
    diag(&d, "file_access", "info", "read roots %s; projects and exports are written below the workspace and the temporary folder, or roots added with --allow-write",
         rt ? rt : "");
    free(rt);
    json_free(roots);
    if (mcp[0]) check_mcp(&d, mcp, places[1]);
    char sock[512];
    ctl_default_socket_path(sock, sizeof sock);
    CtlClient *cc = ctl_connect_unix(sock, err, sizeof err);
    diag(&d, "control_socket", "info", cc ? "a NAVIER server answers at %s (navier-mcp would bridge to it by default; pass --embedded to avoid that)"
                                          : "no server at %s: navier-mcp runs its engine in-process (embedded)",
         sock);
    if (cc) ctl_close(cc);
    if (!skip_solve) check_reference_solve(&d, cfg);
    if (as_json) {
        JsonValue *o = json_object();
        json_set(o, "checks", d.checks);
        json_set_bool(o, "ok", d.failed == 0);
        print_json(o);
        json_free(o);
    } else {
        for (size_t k = 0; k < json_len(d.checks); k++) {
            const JsonValue *c = json_at(d.checks, k);
            printf("%-5s %-22s %s\n", json_get_str(c, "status", ""), json_get_str(c, "check", ""), json_get_str(c, "detail", ""));
        }
        printf("\n%s\n", d.failed ? "DIAGNOSTICS FAILED" : "ALL DIAGNOSTICS PASSED");
        json_free(d.checks);
    }
    return d.failed ? 7 : 0;
}

int cli_local_main(int argc, char **argv, int i, EngineConfig *cfg) {
    const char *cmd = argv[i++];
    if (!strcmp(cmd, "doctor")) {
        bool js = false, skip = false;
        for (; i < argc; i++) js |= !strcmp(argv[i], "--json"), skip |= !strcmp(argv[i], "--no-solve");
        return doctor(cfg, js, skip);
    }
    char err[600];
    Engine *e = engine_create(cfg, err, sizeof err);
    if (!e) {
        fprintf(stderr, "navier-ctl: %s\n", err);
        return 2;
    }
    int status = 2;
    if (!strcmp(cmd, "study")) {
        status = study_cmd(e, argc, argv, i);
    } else if (!strcmp(cmd, "call")) {
        if (i >= argc) {
            fprintf(stderr, "usage: navier-ctl --embedded call OP [JSON | @FILE | -] [--wait]\n");
        } else {
            const char *name = argv[i++];
            bool wait = false;
            JsonValue *params = NULL;
            for (; i < argc; i++) {
                if (!strcmp(argv[i], "--wait")) {
                    wait = true;
                    continue;
                }
                char *text = (argv[i][0] == '@' || !strcmp(argv[i], "-")) ? read_text(argv[i]) : strdup(argv[i]);
                JsonError je;
                params = text ? json_parse(text, strlen(text), NULL, &je) : NULL;
                free(text);
                if (!params) {
                    fprintf(stderr, "navier-ctl: invalid JSON parameters\n");
                    engine_destroy(e);
                    return 2;
                }
            }
            JsonValue *v = op(e, name, params ? params : json_object(), false);
            status = v ? 0 : 1;
            if (v && wait && json_get(v, "job_id")) {
                JsonValue *st = wait_job(e, json_get_str(v, "job_id", ""), true);
                print_json(st);
                status = st && !strcmp(json_get_str(st, "state", ""), "succeeded") ? 0 : 3;
                json_free(st);
            } else if (v) {
                print_json(v);
            }
            json_free(v);
        }
    } else {
        fprintf(stderr, "navier-ctl: unknown command %s in embedded mode (call, study, doctor)\n", cmd);
    }
    engine_destroy(e);
    return status;
}
