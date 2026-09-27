/* am_print.c - prints a part layer by layer in the fused-filament print simulation (src/mech/fffprint.c) and exports the
 * build for an interactive player: temperature, von Mises stress and displacement on the growing part at every layer,
 * the cool-down and the release from the bed, and temperature histories at probe points.
 *
 * The part goes through the NAVIER engine exactly as through MCP (project, STL import with its unit, material from the
 * library, voxel mesh); the finished mesh is then printed. Output directory:
 *   meta.json            frames (stage, layer, time, peaks), probe histories, process, material, results, assumptions
 *   mesh.bin             node positions and every face that is exposed at some point of the build, with its frame range
 *   frames/NNN.bin       per frame: temperature, von Mises and displacement at the nodes (quantised)
 *   png/                 rendered key frames and film strips (software renderer)
 *
 *   make build/amprint
 *   ./build/amprint --stl demo/geometry/truss_bridge.stl --out demo/workspace/print_bridge --element 3 --layer 3
 *   ./build/amprint --stl demo/geometry/truss_bridge.stl --plan --element 3 --layer 3     (deposition plan only)
 *
 * mesh.bin (little endian): "NVPRNT01", uint32 nodes, faces, frames, 0; float32 xyz[3 nodes] (mm: x, y centred, z up from
 * the bed); uint32 quad[4 faces] (counter-clockwise seen from outside); uint16 range[2 faces] (the face is exposed in
 * frames first <= i < end; 65535: to the end).
 * frames/NNN.bin: uint8 T[nodes] over [T_air, T_nozzle]; uint8 von_mises[nodes] over [0, the largest of the build] (mean of
 * the printed elements at the node); int8 u[3 nodes] times the frame's u_scale_mm. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/core/json.h"
#include "../src/core/paths.h"
#include "../src/ctl/engine.h"
#include "../src/ctl/matlib.h"
#include "../src/ctl/ops.h"
#include "../src/ctl/static_analysis.h"
#include "../src/fem/hex8.h"
#include "../src/mech/fffprint.h"
#include "../src/render/swrender.h"

typedef struct Frame {
    int layer, printed;
    char stage[40];
    double time, T_max, vm_max, u_max, balance;
    unsigned char *active; /* nelems */
    float *T, *u, *vm;     /* nnodes, 3 nnodes, nelems */
} Frame;

typedef struct Probe {
    char label[80];
    double at[3]; /* mm, part (STL) frame */
    int node;
    int elems[8], nelems;
} Probe;

typedef struct Store {
    const FffMesh *mesh;
    Frame *frames;
    int n, cap;
    double t0;
    Probe *probes;
    int nprobes;
    double *ht;        /* s */
    float *hT, *hTmax; /* degC: nprobes per sample (NaN before the probe is printed), hottest printed node */
    int hn, hcap;
    unsigned char *node_on;
} Store;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static bool keep_frame(const FffFrame *f, void *ctx) {
    Store *s = ctx;
    const FffMesh *M = s->mesh;
    if (s->n == s->cap) {
        s->cap = s->cap ? 2 * s->cap : 64;
        Frame *nf = realloc(s->frames, (size_t)s->cap * sizeof *nf);
        if (!nf) return false;
        s->frames = nf;
    }
    Frame *F = &s->frames[s->n];
    memset(F, 0, sizeof *F);
    F->layer = f->layer, F->time = f->time, F->T_max = f->T_max, F->vm_max = f->vm_max, F->u_max = f->u_max, F->balance = f->energy_balance;
    snprintf(F->stage, sizeof F->stage, "%s", f->stage);
    size_t nn = (size_t)M->nnodes, ne = (size_t)M->nelems;
    F->active = malloc(ne), F->T = malloc(nn * sizeof(float)), F->u = malloc(3 * nn * sizeof(float)), F->vm = malloc(ne * sizeof(float));
    if (!F->active || !F->T || !F->u || !F->vm) return false;
    memcpy(F->active, f->active, ne);
    for (size_t n = 0; n < nn; n++) {
        F->T[n] = (float)f->T[n];
        for (int k = 0; k < 3; k++) F->u[3 * n + (size_t)k] = (float)f->u[3 * n + (size_t)k];
    }
    for (size_t e = 0; e < ne; e++) F->vm[e] = (float)f->vm[e], F->printed += f->active[e] != 0;
    s->n++;
    printf("  %3d  layer %2d/%d  %-20s t = %6.2f h  elements %6d  T max %6.1f C  von Mises max %6.2f MPa  |u| max %.3f mm  (%.1f s)\n", s->n - 1,
           f->layer + 1, f->nlayers, f->stage, f->time / 3600, F->printed, f->T_max - 273.15, f->vm_max / 1e6, f->u_max * 1e3, now_s() - s->t0);
    fflush(stdout);
    return true;
}

static void keep_step(double time, const double *T, const unsigned char *active, void *ctx) {
    Store *s = ctx;
    const FffMesh *M = s->mesh;
    if (s->hn == s->hcap) {
        s->hcap = s->hcap ? 2 * s->hcap : 256;
        s->ht = realloc(s->ht, (size_t)s->hcap * sizeof(double));
        s->hT = realloc(s->hT, (size_t)(s->hcap * (s->nprobes ? s->nprobes : 1)) * sizeof(float));
        s->hTmax = realloc(s->hTmax, (size_t)s->hcap * sizeof(float));
        if (!s->ht || !s->hT || !s->hTmax) exit(1);
    }
    s->ht[s->hn] = time;
    for (int p = 0; p < s->nprobes; p++) {
        const Probe *pr = &s->probes[p];
        bool on = false;
        for (int i = 0; i < pr->nelems && !on; i++) on = active[pr->elems[i]] != 0;
        s->hT[s->hn * s->nprobes + p] = on ? (float)(T[pr->node] - 273.15) : NAN;
    }
    memset(s->node_on, 0, (size_t)M->nnodes);
    for (int e = 0; e < M->nelems; e++)
        if (active[e])
            for (int a = 0; a < 8; a++) s->node_on[M->conn[8 * (size_t)e + (size_t)a]] = 1;
    double tmax = -INFINITY;
    for (int n = 0; n < M->nnodes; n++)
        if (s->node_on[n]) tmax = fmax(tmax, T[n]);
    s->hTmax[s->hn] = (float)(tmax - 273.15);
    s->hn++;
}

static bool op(Engine *e, const char *name, const char *json, JsonValue **value) {
    JsonValue *p = json_parse(json, strlen(json), NULL, NULL);
    OpResult r;
    OpCaller caller = {"test", "amprint"};
    ops_invoke(e, name, p, &caller, &r);
    json_free(p);
    if (!r.ok) {
        char *t = json_dump(r.error, 0, NULL, NULL);
        fprintf(stderr, "%s failed: %s\n", name, t ? t : "?");
        free(t);
        op_result_free(&r);
        return false;
    }
    if (value) *value = json_clone(r.value);
    op_result_free(&r);
    return true;
}

/* bounding box of an STL (binary or ASCII), in the file's unit */
static bool stl_bounds(const char *path, double lo[3], double hi[3]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    for (int k = 0; k < 3; k++) lo[k] = INFINITY, hi[k] = -INFINITY;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 80, SEEK_SET);
    uint32_t count = 0;
    bool binary = fread(&count, 4, 1, fp) == 1 && size == 84 + 50 * (long)count;
    if (binary) {
        unsigned char rec[50];
        for (uint32_t t = 0; t < count && fread(rec, 50, 1, fp) == 1; t++)
            for (int v = 0; v < 3; v++)
                for (int k = 0; k < 3; k++) {
                    float x;
                    memcpy(&x, rec + 12 + 12 * v + 4 * k, 4);
                    lo[k] = fmin(lo[k], x), hi[k] = fmax(hi[k], x);
                }
    } else {
        fseek(fp, 0, SEEK_SET);
        char word[64];
        while (fscanf(fp, "%63s", word) == 1) {
            if (strcmp(word, "vertex")) continue;
            double x[3];
            if (fscanf(fp, "%lf %lf %lf", &x[0], &x[1], &x[2]) != 3) break;
            for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], x[k]), hi[k] = fmax(hi[k], x[k]);
        }
    }
    fclose(fp);
    return lo[0] <= hi[0];
}

/* ------------------------------------------------------------------------------------------------ rendering */

typedef struct View {
    const FffMesh *M;
    const int *nb;
    SwCamera cam;
    double bmin[3], bmax[3];
    double T_lo, T_hi, vm_hi, exaggeration;
    int nlayers;
    const char *title;
    float *node_vm;
    int *node_cnt;
} View;

/* field 0 temperature, 1 von Mises; compact panels carry a short caption */
static void render_frame(View *v, const Frame *F, int field, int W, int H, bool compact, SwImage *out) {
    const FffMesh *M = v->M;
    SwImage big;
    sw_image_init(&big, 2 * W, 2 * H);
    const unsigned char top[3] = {24, 30, 42}, bottom[3] = {54, 62, 78};
    sw_clear(&big, top, bottom);
    SwCamera cam;
    sw_camera_look(&cam, v->cam.eye, v->cam.target, v->cam.up_hint, v->cam.fov_deg, 2 * W, 2 * H);
    size_t nn = (size_t)M->nnodes, ne = (size_t)M->nelems;
    const unsigned char plate[3] = {88, 100, 120};
    double pad = 0.03 * (v->bmax[0] - v->bmin[0]);
    for (int i = 0; i <= 12; i++) {
        double x = v->bmin[0] - pad + i * (v->bmax[0] - v->bmin[0] + 2 * pad) / 12;
        double a[3] = {x, v->bmin[1] - pad, v->bmin[2]}, b[3] = {x, v->bmax[1] + pad, v->bmin[2]};
        sw_line(&big, &cam, a, b, plate, 0, 1);
    }
    for (int j = 0; j <= 4; j++) {
        double y = v->bmin[1] - pad + j * (v->bmax[1] - v->bmin[1] + 2 * pad) / 4;
        double a[3] = {v->bmin[0] - pad, y, v->bmin[2]}, b[3] = {v->bmax[0] + pad, y, v->bmin[2]};
        sw_line(&big, &cam, a, b, plate, 0, 1);
    }
    if (field == 1) {
        memset(v->node_vm, 0, nn * sizeof(float)), memset(v->node_cnt, 0, nn * sizeof(int));
        for (size_t e = 0; e < ne; e++) {
            if (!F->active[e]) continue;
            for (int a = 0; a < 8; a++) {
                int n = M->conn[8 * e + (size_t)a];
                v->node_vm[n] += F->vm[e], v->node_cnt[n]++;
            }
        }
        for (size_t n = 0; n < nn; n++)
            if (v->node_cnt[n]) v->node_vm[n] /= (float)v->node_cnt[n];
    }
    double lo = field == 0 ? v->T_lo : 0, hi = field == 0 ? v->T_hi : v->vm_hi;
    SwColormap cm = field == 0 ? SW_CMAP_TURBO : SW_CMAP_VIRIDIS;
    for (size_t e = 0; e < ne; e++) {
        if (!F->active[e]) continue;
        for (int f = 0; f < 6; f++) {
            int o = v->nb[6 * e + (size_t)f];
            if (o >= 0 && F->active[o]) continue;
            double p[4][3], s[4];
            for (int i = 0; i < 4; i++) {
                int n = M->conn[8 * e + (size_t)HEX8_FACE_NODES[f][i]];
                for (int k = 0; k < 3; k++) p[i][k] = M->xyz[3 * (size_t)n + (size_t)k] + v->exaggeration * F->u[3 * (size_t)n + (size_t)k];
                s[i] = field == 0 ? F->T[n] - 273.15 : v->node_vm[n] / 1e6;
            }
            sw_triangle_scalar(&big, &cam, p[0], p[1], p[2], s[0], s[1], s[2], lo, hi, cm, true, 0);
            sw_triangle_scalar(&big, &cam, p[0], p[2], p[3], s[0], s[2], s[3], lo, hi, cm, true, 0);
        }
    }
    sw_image_init(out, W, H);
    sw_downsample(&big, out);
    sw_image_free(&big);
    const unsigned char white[3] = {236, 240, 246}, grey[3] = {170, 180, 196}, accent[3] = {255, 196, 92};
    char line[200];
    int hours = (int)(F->time / 3600), mins = (int)fmod(F->time / 60, 60);
    if (compact) {
        snprintf(line, sizeof line, "layer %d/%d  %s  %dh%02d", F->layer + 1, v->nlayers, F->stage, hours, mins);
        sw_text(out, 10, 8, 1, line, accent);
        if (field == 0) snprintf(line, sizeof line, "hottest %.0f C", F->T_max - 273.15);
        else snprintf(line, sizeof line, "peak %.2f MPa", F->vm_max / 1e6);
        sw_text(out, 10, 24, 1, line, grey);
        return;
    }
    sw_text(out, 18, 16, 2, v->title, white);
    snprintf(line, sizeof line, "layer %d of %d   |   %s   |   %d h %02d min", F->layer + 1, v->nlayers, F->stage, hours, mins);
    sw_text(out, 18, 44, 1, line, accent);
    if (field == 0) snprintf(line, sizeof line, "hottest %.0f C", F->T_max - 273.15);
    else snprintf(line, sizeof line, "peak von Mises %.2f MPa   |   largest displacement %.3f mm (drawn x%.0f)", F->vm_max / 1e6, F->u_max * 1e3, v->exaggeration);
    sw_text(out, 18, 62, 1, line, grey);
    sw_colorbar(out, W - 110, 90, 22, H - 200, cm, lo, hi, field == 0 ? "temperature" : "von Mises", field == 0 ? "C" : "MPa");
}

static void write_png(const SwImage *im, const char *path) {
    unsigned char *png = NULL;
    size_t len = 0;
    if (sw_png(im, &png, &len)) {
        FILE *fp = fopen(path, "wb");
        if (fp) fwrite(png, 1, len, fp), fclose(fp);
    }
    free(png);
}

static void blit(SwImage *dst, const SwImage *src, int x0, int y0) {
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) {
            int X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= dst->w || Y >= dst->h) continue;
            memcpy(dst->rgb + 3 * ((size_t)Y * (size_t)dst->w + (size_t)X), src->rgb + 3 * ((size_t)y * (size_t)src->w + (size_t)x), 3);
        }
}

/* ------------------------------------------------------------------------------------------------ export */

static JsonValue *float_array(const float *v, int n, double scale) {
    JsonValue *a = json_array();
    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) json_push(a, json_null());
        else json_push(a, json_number(round(v[i] * scale) / scale));
    }
    return a;
}

static JsonValue *pair(double a, double b) {
    JsonValue *v = json_array();
    json_push(v, json_number(a)), json_push(v, json_number(b));
    return v;
}

static JsonValue *colormap_json(SwColormap cm) {
    JsonValue *a = json_array();
    for (int i = 0; i < 256; i++) {
        float rgb[3];
        sw_colormap(cm, i / 255.0, rgb);
        JsonValue *c = json_array();
        for (int k = 0; k < 3; k++) json_push(c, json_number(round(255 * rgb[k])));
        json_push(a, c);
    }
    return a;
}

int main(int argc, char **argv) {
    const char *stl = NULL, *out = NULL, *material = "pla_generic_demo", *title = "printing the truss bridge", *pngs = "key";
    double element_mm = 3, layer_mm = 3, nozzle_c = 210, bed_c = 60, air_c = 30, h_conv = 15, rate_mm3s = 15, skip_dt = 0, pcg_tol = 0;
    int solver = 0;
    int W = 1100, H = 640, substeps = 8;
    bool plan_only = false;
    Probe probes[16];
    int nprobes = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stl") && i + 1 < argc) stl = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--element") && i + 1 < argc) element_mm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--layer") && i + 1 < argc) layer_mm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--material") && i + 1 < argc) material = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate_mm3s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--substeps") && i + 1 < argc) substeps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) pngs = argv[++i];
        else if (!strcmp(argv[i], "--plan")) plan_only = true;
        else if (!strcmp(argv[i], "--skip-dt") && i + 1 < argc) skip_dt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pcg-tol") && i + 1 < argc) pcg_tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "--solver") && i + 1 < argc) {
            const char *sv = argv[++i];
            solver = !strcmp(sv, "direct") ? 1 : (!strcmp(sv, "iterative") ? 2 : 0);
        }
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc && nprobes < 16) {
            const char *spec = argv[++i], *at = strrchr(spec, '@');
            Probe *p = &probes[nprobes];
            memset(p, 0, sizeof *p);
            if (!at || sscanf(at + 1, "%lf,%lf,%lf", &p->at[0], &p->at[1], &p->at[2]) != 3) {
                fprintf(stderr, "amprint: --probe takes \"label@x,y,z\" in mm of the STL\n");
                return 2;
            }
            snprintf(p->label, sizeof p->label, "%.*s", (int)(at - spec), spec);
            nprobes++;
        } else {
            fprintf(stderr, "usage: amprint --stl FILE (--out DIR | --plan) [--element mm] [--layer mm] [--material id] [--rate mm3/s] [--substeps n]\n"
                            "               [--probe label@x,y,z]... [--png none|key] [--size WxH] [--title text]\n");
            return 2;
        }
    }
    if (!stl || (!out && !plan_only)) {
        fprintf(stderr, "amprint: --stl and --out (or --plan) are required\n");
        return 2;
    }
    char absout[NV_PATH_MAX], abstl[NV_PATH_MAX], ws[NV_PATH_MAX], path[NV_PATH_MAX], err[512];
    if (!realpath(stl, abstl)) {
        fprintf(stderr, "amprint: cannot resolve %s\n", stl);
        return 2;
    }
    if (plan_only) {
        const char *tmp = getenv("TMPDIR");
        snprintf(path, sizeof path, "%s/amprint_plan_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
        mkdir(path, 0755);
        if (!realpath(path, absout)) return 2;
    } else {
        mkdir(out, 0755);
        if (!realpath(out, absout)) {
            fprintf(stderr, "amprint: cannot resolve %s\n", out);
            return 2;
        }
    }
    snprintf(ws, sizeof ws, "%s/ws", absout);
    mkdir(ws, 0755);
    double t_start = now_s();
    EngineConfig cfg;
    engine_config_default(&cfg);
    if (!engine_config_set_workspace(&cfg, ws, err, sizeof err) || !engine_config_add_root(&cfg, true, absout, err, sizeof err)) {
        fprintf(stderr, "amprint: %s\n", err);
        return 1;
    }
    char dir[NV_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", abstl);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    engine_config_add_root(&cfg, false, dir, err, sizeof err);
    Engine *e = engine_create(&cfg, err, sizeof err);
    if (!e) {
        fprintf(stderr, "amprint: %s\n", err);
        return 1;
    }
    char js[2 * NV_PATH_MAX];
    printf("== part: import, material, voxel mesh\n");
    snprintf(js, sizeof js, "{\"name\": \"print\", \"description\": \"layer-by-layer print simulation\"}");
    bool ok = op(e, "project_create", js, NULL);
    snprintf(js, sizeof js, "{\"path\": \"%s\", \"units\": \"mm\", \"name\": \"part\"}", abstl);
    ok = ok && op(e, "geometry_import", js, NULL);
    snprintf(js, sizeof js, "{\"body\": \"part\", \"material\": \"%s\", \"source\": \"user\"}", material);
    ok = ok && op(e, "material_assign", js, NULL);
    snprintf(js, sizeof js, "{\"element_size\": \"%g mm\"}", element_mm);
    ok = ok && op(e, "mesh_generate", js, NULL);
    if (!ok) return 1;
    StaticModel sm;
    memset(&sm, 0, sizeof sm);
    JsonValue *errors = json_array(), *warnings = json_array();
    engine_lock(e);
    StaticSettings sset = {HEX8_INCOMPATIBLE, SOLID_SOLVER_AUTO, 1e-10, 293.15};
    bool built = static_model_build(engine_project_locked(e), &sset, &sm, errors, warnings);
    engine_unlock(e);
    if (!built && !sm.nelems) {
        char *t = json_dump(errors, 0, NULL, NULL);
        fprintf(stderr, "amprint: the mesh cannot be built: %s\n", t ? t : "?");
        return 1;
    }
    MaterialRecord rec;
    if (!material_lookup(NULL, material, &rec, err, sizeof err)) {
        fprintf(stderr, "amprint: %s\n", err);
        return 1;
    }
    FffMesh M = {sm.nnodes, sm.nelems, sm.xyz, sm.conn};
    double bmin[3] = {INFINITY, INFINITY, INFINITY}, bmax[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int n = 0; n < sm.nnodes; n++)
        for (int k = 0; k < 3; k++) bmin[k] = fmin(bmin[k], sm.xyz[3 * n + k]), bmax[k] = fmax(bmax[k], sm.xyz[3 * n + k]);
    printf("  %s: %d hex elements, %d nodes, %.0f x %.0f x %.0f mm; %s (%s)\n", abstl, sm.nelems, sm.nnodes, 1e3 * (bmax[0] - bmin[0]), 1e3 * (bmax[1] - bmin[1]),
           1e3 * (bmax[2] - bmin[2]), rec.name, rec.status);
    size_t nn = (size_t)sm.nnodes, ne = (size_t)sm.nelems;

    /* deposition plan */
    FffPlanStats plan;
    int *dep = fff_plan(&M, layer_mm * 1e-3, &plan);
    if (!dep) {
        fprintf(stderr, "amprint: out of memory for the deposition plan\n");
        return 1;
    }
    printf("== deposition plan: %d simulation layers of %.1f mm; %d elements deposited after their own layer (overhangs); %d never deposited\n", plan.nlayers,
           layer_mm, plan.late, plan.unprintable);
    if (plan.unprintable) {
        double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (size_t q = 0; q < ne; q++) {
            if (dep[q] >= 0) continue;
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) {
                    double x = sm.xyz[3 * (size_t)sm.conn[8 * q + (size_t)a] + (size_t)k];
                    lo[k] = fmin(lo[k], x), hi[k] = fmax(hi[k], x);
                }
        }
        printf("   never deposited (not face-connected to the bed through printed material): within x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f mm (mesh frame)\n",
               1e3 * lo[0], 1e3 * hi[0], 1e3 * lo[1], 1e3 * hi[1], 1e3 * lo[2], 1e3 * hi[2]);
    }
    if (plan_only) {
        int *count = calloc((size_t)plan.nlayers, sizeof(int));
        for (size_t q = 0; q < ne; q++)
            if (dep[q] >= 0) count[dep[q]]++;
        for (int k = 0; k < plan.nlayers; k++) printf("   layer %2d  z %5.1f..%5.1f mm  %6d elements\n", k + 1, k * layer_mm, (k + 1) * layer_mm, count[k]);
        free(count);
        engine_destroy(e);
        return plan.unprintable ? 3 : 0;
    }

    /* probes: the nearest printed node to each point, with the STL placed on the mesh by its bounding box */
    double slo[3], shi[3], offset[3] = {0, 0, 0};
    if (nprobes && stl_bounds(abstl, slo, shi))
        for (int k = 0; k < 3; k++) offset[k] = k < 2 ? 0.5 * (bmin[k] + bmax[k]) - 0.5e-3 * (slo[k] + shi[k]) : bmin[k] - 1e-3 * slo[k];
    int *node_elems = malloc(8 * nn * sizeof(int)), *node_ne = calloc(nn, sizeof(int));
    unsigned char *printable_node = calloc(nn, 1);
    for (size_t q = 0; q < ne; q++)
        for (int a = 0; a < 8; a++) {
            int n = sm.conn[8 * q + (size_t)a];
            if (dep[q] >= 0) printable_node[n] = 1;
            if (node_ne[n] < 8) node_elems[8 * (size_t)n + (size_t)node_ne[n]++] = (int)q;
        }
    for (int p = 0; p < nprobes; p++) {
        double x[3], best = INFINITY;
        for (int k = 0; k < 3; k++) x[k] = 1e-3 * probes[p].at[k] + offset[k];
        probes[p].node = -1;
        for (size_t n = 0; n < nn; n++) {
            if (!printable_node[n]) continue;
            double d = 0;
            for (int k = 0; k < 3; k++) d += (sm.xyz[3 * n + (size_t)k] - x[k]) * (sm.xyz[3 * n + (size_t)k] - x[k]);
            if (d < best) best = d, probes[p].node = (int)n;
        }
        int n = probes[p].node;
        probes[p].nelems = 0;
        for (int i = 0; n >= 0 && i < node_ne[n]; i++)
            if (dep[node_elems[8 * (size_t)n + (size_t)i]] >= 0) probes[p].elems[probes[p].nelems++] = node_elems[8 * (size_t)n + (size_t)i];
        printf("  probe \"%s\": node %d, %.1f mm from the requested point\n", probes[p].label, n, 1e3 * sqrt(best));
    }
    free(node_elems), free(node_ne), free(printable_node);

    FffMaterial mat;
    memset(&mat, 0, sizeof mat);
    mat.rho = rec.prop[MATP_DENSITY], mat.cp = rec.prop[MATP_CP], mat.k = rec.prop[MATP_K], mat.E = rec.prop[MATP_E];
    mat.alpha = rec.prop[MATP_ALPHA], mat.emissivity = rec.prop[MATP_EMISSIVITY];
    mat.nu = mat_eval(&rec.prop[MATP_NU], 293.15);
    mat.T_relax = (isfinite(rec.glass_transition_k) ? rec.glass_transition_k : 333.15) + 10;
    mat.E_floor = 1e6;
    FffProcess P = {layer_mm * 1e-3, nozzle_c + 273.15, bed_c + 273.15, air_c + 273.15, h_conv,  true,   rate_mm3s * 1e-9,
                    60.0,              1200.0,            3600.0,        substeps,        false,  solver, pcg_tol,          skip_dt};
    printf("== printing: %.1f mm simulation layers, nozzle %.0f C, bed %.0f C, air %.0f C, h %.0f W/m2K, %.1f mm3/s\n", layer_mm, nozzle_c, bed_c, air_c, h_conv,
           rate_mm3s);
    Store store;
    memset(&store, 0, sizeof store);
    store.mesh = &M, store.t0 = now_s(), store.probes = probes, store.nprobes = nprobes, store.node_on = malloc(nn);
    FffSummary S;
    FffCallbacks cb = {keep_frame, keep_step, &store};
    if (!fff_simulate(&M, &mat, &P, &cb, &S, err, sizeof err)) {
        fprintf(stderr, "amprint: %s\n", err);
        return 1;
    }
    double t_sim = now_s() - t_start;
    printf("== printed in %.1f h of machine time (%d layers); simulated in %.1f s (thermal %.1f s, stress %.1f s, %d increments, %d skipped)\n",
           S.print_time / 3600, S.nlayers, t_sim, S.seconds_thermal, S.seconds_mech, S.mech_solves, S.skipped_increments);
    printf("   peak von Mises on the bed %.2f MPa, after release %.2f MPa; vertical warp %.3f to %.3f mm; worst energy balance %.1e; release support reaction %.1e N\n",
           S.peak_vm_bed / 1e6, S.peak_vm_released / 1e6, 1e3 * S.warp_z_min, 1e3 * S.warp_z_max, S.worst_energy_balance, S.release_support_reaction);

    /* ---------------------------------------------------------------- export for the player */
    int nframes = store.n;
    printf("== exporting %d frames\n", nframes);
    double vm_hi = 0;
    for (int i = 0; i < nframes; i++) vm_hi = fmax(vm_hi, store.frames[i].vm_max);
    vm_hi = vm_hi > 0 ? vm_hi : 1;
    int *nb = fff_face_neighbours(&M);
    uint16_t *act = malloc(ne * sizeof(uint16_t));
    double *evol = malloc(ne * sizeof(double));
    for (size_t q = 0; q < ne; q++) {
        act[q] = 65535;
        for (int i = 0; i < nframes; i++)
            if (store.frames[i].active[q]) {
                act[q] = (uint16_t)i;
                break;
            }
        double X[8][3];
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) X[a][k] = sm.xyz[3 * (size_t)sm.conn[8 * q + (size_t)a] + (size_t)k];
        evol[q] = hex8_volume(X);
    }
    double c0[3] = {0.5 * (bmin[0] + bmax[0]), 0.5 * (bmin[1] + bmax[1]), bmin[2]};
    size_t nfaces = 0, fcap = 1 << 16;
    uint32_t *quads = malloc(fcap * 4 * sizeof(uint32_t));
    uint16_t *ranges = malloc(fcap * 2 * sizeof(uint16_t));
    for (size_t q = 0; q < ne; q++) {
        if (act[q] == 65535) continue;
        double ec[3] = {0, 0, 0};
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) ec[k] += sm.xyz[3 * (size_t)sm.conn[8 * q + (size_t)a] + (size_t)k] / 8;
        for (int f = 0; f < 6; f++) {
            int o = nb[6 * q + (size_t)f];
            uint16_t end = o >= 0 ? act[o] : 65535;
            if (!(act[q] < end)) continue;
            if (nfaces == fcap) {
                fcap *= 2;
                quads = realloc(quads, fcap * 4 * sizeof(uint32_t)), ranges = realloc(ranges, fcap * 2 * sizeof(uint16_t));
            }
            int nd[4];
            double fc[3] = {0, 0, 0};
            for (int i = 0; i < 4; i++) {
                nd[i] = sm.conn[8 * q + (size_t)HEX8_FACE_NODES[f][i]];
                for (int k = 0; k < 3; k++) fc[k] += sm.xyz[3 * (size_t)nd[i] + (size_t)k] / 4;
            }
            const double *p0 = sm.xyz + 3 * (size_t)nd[0], *p1 = sm.xyz + 3 * (size_t)nd[1], *p3 = sm.xyz + 3 * (size_t)nd[3];
            double a1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, a3[3] = {p3[0] - p0[0], p3[1] - p0[1], p3[2] - p0[2]};
            double nrm[3] = {a1[1] * a3[2] - a1[2] * a3[1], a1[2] * a3[0] - a1[0] * a3[2], a1[0] * a3[1] - a1[1] * a3[0]};
            if (nrm[0] * (fc[0] - ec[0]) + nrm[1] * (fc[1] - ec[1]) + nrm[2] * (fc[2] - ec[2]) < 0) {
                int t = nd[1];
                nd[1] = nd[3], nd[3] = t;
            }
            for (int i = 0; i < 4; i++) quads[4 * nfaces + (size_t)i] = (uint32_t)nd[i];
            ranges[2 * nfaces] = act[q], ranges[2 * nfaces + 1] = end;
            nfaces++;
        }
    }
    snprintf(path, sizeof path, "%s/mesh.bin", absout);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "amprint: cannot write %s\n", path);
        return 1;
    }
    uint32_t hdr[4] = {(uint32_t)nn, (uint32_t)nfaces, (uint32_t)nframes, 0};
    fwrite("NVPRNT01", 1, 8, fp), fwrite(hdr, 4, 4, fp);
    for (size_t n = 0; n < nn; n++) {
        float x[3];
        for (int k = 0; k < 3; k++) x[k] = (float)(1e3 * (sm.xyz[3 * n + (size_t)k] - c0[k]));
        fwrite(x, 4, 3, fp);
    }
    fwrite(quads, 4, 4 * nfaces, fp), fwrite(ranges, 2, 2 * nfaces, fp);
    fclose(fp);
    snprintf(path, sizeof path, "%s/frames", absout);
    mkdir(path, 0755);
    unsigned char *qbuf = malloc(5 * nn);
    float *node_vm = malloc(nn * sizeof(float));
    int *node_cnt = malloc(nn * sizeof(int));
    double vol_all = 0;
    for (size_t q = 0; q < ne; q++) vol_all += dep[q] >= 0 ? evol[q] : 0;
    JsonValue *meta = json_object();
    json_set_int(meta, "format", 1);
    json_set_string(meta, "title", title);
    JsonValue *fr = json_set_array(meta, "frames");
    for (int i = 0; i < nframes; i++) {
        Frame *F = &store.frames[i];
        memset(node_vm, 0, nn * sizeof(float)), memset(node_cnt, 0, nn * sizeof(int));
        double vol = 0;
        for (size_t q = 0; q < ne; q++) {
            if (!F->active[q]) continue;
            vol += evol[q];
            for (int a = 0; a < 8; a++) {
                int n = sm.conn[8 * q + (size_t)a];
                node_vm[n] += F->vm[q], node_cnt[n]++;
            }
        }
        double umax = 0, uz_lo = INFINITY, uz_hi = -INFINITY, ztop = 0;
        for (size_t n = 0; n < nn; n++) {
            if (!node_cnt[n]) continue;
            node_vm[n] /= (float)node_cnt[n];
            for (int k = 0; k < 3; k++) umax = fmax(umax, fabs(F->u[3 * n + (size_t)k]));
            uz_lo = fmin(uz_lo, F->u[3 * n + 2]), uz_hi = fmax(uz_hi, F->u[3 * n + 2]);
            ztop = fmax(ztop, sm.xyz[3 * n + 2] - bmin[2]);
        }
        double uscale = umax > 0 ? umax / 127 : 1;
        for (size_t n = 0; n < nn; n++) {
            double t = node_cnt[n] ? (F->T[n] - 273.15 - air_c) / (nozzle_c - air_c) : 0;
            qbuf[n] = (unsigned char)lround(fmin(fmax(t, 0), 1) * 255);
            qbuf[nn + n] = (unsigned char)lround(fmin(fmax(node_vm[n] / vm_hi, 0), 1) * 255);
            for (int k = 0; k < 3; k++) {
                long v = node_cnt[n] ? lround(F->u[3 * n + (size_t)k] / uscale) : 0;
                qbuf[2 * nn + 3 * n + (size_t)k] = (unsigned char)(int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
            }
        }
        char name[64];
        snprintf(name, sizeof name, "frames/%03d.bin", i);
        snprintf(path, sizeof path, "%s/%s", absout, name);
        fp = fopen(path, "wb");
        if (!fp || fwrite(qbuf, 1, 5 * nn, fp) != 5 * nn) {
            fprintf(stderr, "amprint: cannot write %s\n", path);
            return 1;
        }
        fclose(fp);
        JsonValue *o = json_object();
        json_set_int(o, "index", i), json_set_int(o, "layer", F->layer + 1), json_set_string(o, "stage", F->stage);
        json_set_number(o, "time_h", F->time / 3600), json_set_number(o, "T_max_c", F->T_max - 273.15);
        json_set_number(o, "von_mises_max_mpa", F->vm_max / 1e6), json_set_number(o, "displacement_max_mm", F->u_max * 1e3);
        json_set_number(o, "uz_min_mm", 1e3 * uz_lo), json_set_number(o, "uz_max_mm", 1e3 * uz_hi);
        json_set_number(o, "u_scale_mm", 1e3 * uscale), json_set_int(o, "printed_elements", F->printed);
        json_set_number(o, "printed_fraction", vol_all > 0 ? vol / vol_all : 0), json_set_number(o, "top_mm", 1e3 * ztop);
        json_set_number(o, "energy_balance", F->balance), json_set_string(o, "file", name);
        json_push(fr, o);
    }
    const char *base = strrchr(abstl, '/');
    json_set_string(meta, "part", base ? base + 1 : abstl);
    JsonValue *mj = json_set_object(meta, "mesh");
    json_set_int(mj, "elements", sm.nelems), json_set_int(mj, "nodes", sm.nnodes), json_set_int(mj, "faces", (long long)nfaces);
    json_set_number(mj, "element_size_mm", element_mm);
    json_set_int(mj, "elements_never_deposited", plan.unprintable), json_set_int(mj, "elements_deposited_late", plan.late);
    json_set_number(mj, "printed_volume_cm3", vol_all * 1e6);
    json_set(mj, "size_mm", json_vec3(1e3 * (bmax[0] - bmin[0]), 1e3 * (bmax[1] - bmin[1]), 1e3 * (bmax[2] - bmin[2])));
    JsonValue *bj = json_set_array(mj, "bounds_mm");
    json_push(bj, json_vec3(1e3 * (bmin[0] - c0[0]), 1e3 * (bmin[1] - c0[1]), 0));
    json_push(bj, json_vec3(1e3 * (bmax[0] - c0[0]), 1e3 * (bmax[1] - c0[1]), 1e3 * (bmax[2] - bmin[2])));
    JsonValue *matj = json_set_object(meta, "material");
    json_set_string(matj, "id", rec.id), json_set_string(matj, "name", rec.name), json_set_string(matj, "status", rec.status);
    json_set_number(matj, "glass_transition_c", isfinite(rec.glass_transition_k) ? rec.glass_transition_k - 273.15 : NAN);
    json_set_number(matj, "relaxation_c", mat.T_relax - 273.15);
    json_set_number(matj, "youngs_modulus_20c_gpa", mat_eval(&mat.E, 293.15) / 1e9);
    json_set_number(matj, "density_kg_m3", mat_eval(&mat.rho, 293.15));
    JsonValue *pj = json_set_object(meta, "process");
    json_set_number(pj, "simulation_layer_mm", layer_mm), json_set_number(pj, "nozzle_c", nozzle_c), json_set_number(pj, "bed_c", bed_c);
    json_set_number(pj, "air_c", air_c), json_set_number(pj, "convection_w_m2k", h_conv), json_set_number(pj, "deposition_rate_mm3_s", rate_mm3s);
    json_set_number(pj, "min_layer_time_s", P.min_layer_time);
    json_set_number(pj, "cooldown_bed_on_min", P.cooldown_bed_on / 60), json_set_number(pj, "cooldown_bed_off_min", P.cooldown_bed_off / 60);
    json_set_int(pj, "thermal_substeps_per_layer", substeps);
    JsonValue *ranges_j = json_set_object(meta, "ranges");
    json_set(ranges_j, "temperature_c", pair(air_c, nozzle_c));
    json_set(ranges_j, "von_mises_mpa", pair(0, vm_hi / 1e6));
    JsonValue *rj = json_set_object(meta, "results");
    json_set_int(rj, "layers", S.nlayers), json_set_number(rj, "print_time_h", S.print_time / 3600), json_set_number(rj, "total_time_h", S.total_time / 3600);
    json_set_number(rj, "peak_von_mises_on_bed_mpa", S.peak_vm_bed / 1e6), json_set_number(rj, "peak_von_mises_released_mpa", S.peak_vm_released / 1e6);
    json_set_number(rj, "warp_z_min_mm", 1e3 * S.warp_z_min), json_set_number(rj, "warp_z_max_mm", 1e3 * S.warp_z_max);
    json_set_number(rj, "bed_reaction_largest_n", S.bed_reaction_total);
    json_set_number(rj, "worst_thermal_energy_balance", S.worst_energy_balance), json_set_number(rj, "release_support_reaction_n", S.release_support_reaction);
    json_set_int(rj, "thermal_steps", S.thermal_steps), json_set_int(rj, "stress_increments", S.mech_solves);
    json_set_number(rj, "seconds_simulation", t_sim), json_set_number(rj, "seconds_thermal", S.seconds_thermal);
    json_set_number(rj, "seconds_stress", S.seconds_mech);
    JsonValue *hj = json_set_object(meta, "history");
    {
        size_t hn = (size_t)(store.hn ? store.hn : 1);
        float *th = malloc(hn * sizeof(float)), *col = malloc(hn * sizeof(float));
        for (int i = 0; i < store.hn; i++) th[i] = (float)(store.ht[i] / 3600);
        json_set(hj, "time_h", float_array(th, store.hn, 1e5));
        json_set(hj, "T_max_c", float_array(store.hTmax, store.hn, 10));
        JsonValue *pa = json_set_array(hj, "probes");
        for (int p = 0; p < nprobes; p++) {
            JsonValue *po = json_object();
            const int n = probes[p].node;
            json_set_string(po, "label", probes[p].label);
            json_set(po, "at_mm", json_vec3(probes[p].at[0], probes[p].at[1], probes[p].at[2]));
            json_set(po, "node_mm", json_vec3(1e3 * (sm.xyz[3 * (size_t)n] - c0[0]), 1e3 * (sm.xyz[3 * (size_t)n + 1] - c0[1]),
                                              1e3 * (sm.xyz[3 * (size_t)n + 2] - c0[2])));
            json_set_int(po, "node", n);
            for (int i = 0; i < store.hn; i++) col[i] = store.hT[i * nprobes + p];
            json_set(po, "T_c", float_array(col, store.hn, 10));
            json_push(pa, po);
        }
        free(th), free(col);
    }
    JsonValue *aj = json_set_array(meta, "assumptions");
    json_push(aj, json_stringf("each %.1f mm simulation layer lumps about %.0f printed layers of 0.2 mm and is deposited at once at the nozzle temperature; it "
                               "then cools for the time the printer needs to lay down its volume at %.0f mm3/s (at least %.0f s)",
                               layer_mm, layer_mm / 0.2, rate_mm3s, P.min_layer_time));
    json_push(aj, json_string("solid part (100 % infill); material is deposited only where it rests on the bed or on printed material, and an overhang waits "
                              "for the layer that supports it"));
    json_push(aj, json_string("heat: conduction with the library's k, cp(T) and density; convection and grey radiation to the chamber air from every exposed "
                              "face; the heated bed as a fixed temperature, switched off for the last hour"));
    json_push(aj, json_string("stress: small-strain thermo-elasticity with E(T) and alpha(T); new material born stress-free at its programmed position; "
                              "stress relaxes fully above the relaxation temperature (glass transition + 10 C); the bed holds the bottom face rigidly until release"));
    json_push(aj, json_string("release: the accumulated bed reactions removed on an isostatic support, so the part takes its free, warped shape"));
    json_push(aj, json_string("not modelled: the toolpath inside a layer, creep below the relaxation temperature, plasticity, raster anisotropy and interlayer "
                              "strength, crystallisation, supports, gravity, adhesion failure"));
    json_push(aj, json_string("material values are demonstration data from the library (typical magnitudes for PLA), not measured for a filament"));
    JsonValue *cmj = json_set_object(meta, "colormaps");
    json_set(cmj, "turbo", colormap_json(SW_CMAP_TURBO)), json_set(cmj, "viridis", colormap_json(SW_CMAP_VIRIDIS));
    json_set(cmj, "coolwarm", colormap_json(SW_CMAP_COOLWARM));
    snprintf(path, sizeof path, "%s/meta.json", absout);
    json_write_file(path, meta, 0);
    printf("   mesh.bin: %zu nodes, %zu faces; %d frame files of %.0f kB each\n", nn, nfaces, nframes, 5.0 * (double)nn / 1024);

    /* ---------------------------------------------------------------- key frames and film strips (software renderer) */
    if (strcmp(pngs, "none")) {
        printf("== rendering key frames (%.1f s)\n", now_s() - t_start);
        snprintf(path, sizeof path, "%s/png", absout);
        mkdir(path, 0755);
        View v;
        memset(&v, 0, sizeof v);
        v.M = &M, v.nb = nb, v.T_lo = air_c, v.T_hi = nozzle_c, v.vm_hi = vm_hi / 1e6, v.nlayers = S.nlayers, v.title = title;
        memcpy(v.bmin, bmin, sizeof bmin), memcpy(v.bmax, bmax, sizeof bmax);
        v.node_vm = node_vm, v.node_cnt = node_cnt;
        double u_last = nframes ? store.frames[nframes - 1].u_max : 0, size = fmax(bmax[0] - bmin[0], fmax(bmax[1] - bmin[1], bmax[2] - bmin[2]));
        v.exaggeration = u_last > 0 ? fmin(50, fmax(1, round(0.03 * size / u_last))) : 1;
        double Lx = bmax[0] - bmin[0];
        double eye[3] = {bmin[0] - 0.35 * Lx, bmin[1] - 1.25 * Lx, bmax[2] + 0.62 * Lx};
        double target[3] = {0.5 * (bmin[0] + bmax[0]), 0.5 * (bmin[1] + bmax[1]), bmin[2] + 0.38 * (bmax[2] - bmin[2])}, up[3] = {0, 0, 1};
        sw_camera_look(&v.cam, eye, target, up, 26, 2 * W, 2 * H);
        /* the deposited frame of the layers at 1/6 .. 6/6 of the build */
        int pick[6], npick = 0;
        for (int s = 1; s <= 6; s++) {
            int want = (int)lround(s * S.nlayers / 6.0) - 1, best = -1;
            for (int i = 0; i < nframes; i++)
                if (store.frames[i].layer == want && !strcmp(store.frames[i].stage, "deposited")) best = i;
            if (best >= 0) pick[npick++] = best;
        }
        for (int field = 0; field < 2; field++) {
            int pw = W / 2, ph = H / 2, cols = 3;
            SwImage strip;
            sw_image_init(&strip, cols * pw, 2 * ph + 40);
            const unsigned char bg[3] = {20, 25, 34}, white[3] = {236, 240, 246};
            sw_clear(&strip, bg, bg);
            char cap[200];
            snprintf(cap, sizeof cap, "%s: %s, layer by layer", title, field == 0 ? "temperature" : "accumulated von Mises stress");
            sw_text(&strip, 12, 12, 2, cap, white);
            for (int s = 0; s < npick; s++) {
                SwImage panel;
                render_frame(&v, &store.frames[pick[s]], field, pw, ph, true, &panel);
                blit(&strip, &panel, (s % cols) * pw, 40 + (s / cols) * ph);
                sw_image_free(&panel);
            }
            snprintf(path, sizeof path, "%s/png/filmstrip_%s.png", absout, field == 0 ? "temperature" : "stress");
            write_png(&strip, path);
            sw_image_free(&strip);
        }
        int key[4] = {npick > 2 ? pick[2] : 0, npick ? pick[npick - 1] : 0, nframes - 2, nframes - 1};
        const char *key_name[4] = {"mid_print", "last_layer", "cooled_on_bed", "released"};
        for (int k = 0; k < 4; k++) {
            if (key[k] < 0 || key[k] >= nframes) continue;
            for (int field = 0; field < 2; field++) {
                SwImage im;
                render_frame(&v, &store.frames[key[k]], field, W, H, false, &im);
                snprintf(path, sizeof path, "%s/png/%s_%s.png", absout, key_name[k], field == 0 ? "temperature" : "stress");
                write_png(&im, path);
                sw_image_free(&im);
            }
        }
    }
    printf("== done in %.1f s: %s\n", now_s() - t_start, absout);
    engine_destroy(e);
    return 0;
}
