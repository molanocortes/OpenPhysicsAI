/* motor3d.c - a permanent-magnet motor in three dimensions (docs/lab/magnet.md, "The motor in 3D").
 *
 * The motor is the one its 2D scenario draws (regions painted in order, the rotor's turning with it, radially
 * magnetised magnets, slots carrying phase currents), given a stack length and ends: the cross-section is extruded over
 * the stack, and beyond it, in air, the windings close. Each coil runs from a slot carrying its phase forwards to the
 * nearest slot carrying the same phase backwards, along the stack in both, and round the end turns between them. A coil
 * is given as a current vector potential T = -K chi e_r (mag3d.h), K = J r dtheta_slot, chi rising from 0 to 1 across
 * the forward slot, 1 over the teeth between, falling across the return slot and across the end turns' thickness beyond
 * the stack: its curl is the coil's current, closed and divergence-free by construction. The field is solved by
 * mag3d (MFEM, Nedelec elements) on a cylindrical grid whose rings and sectors follow the regions' edges.
 *
 * The torque on the rotor is Arkkio's: the Maxwell stress averaged through the air gap's volume,
 * T = 1 / (mu0 (r2 - r1)) int r B_r B_theta dV, over the whole axial length (the end walls carry no stress: B_n = 0).
 *
 * Not modelled (as in 2D): saturation, eddy currents, the magnets' temperature. The end windings' own shape (their
 * bend and spread) is idealised as a band of the slots' radial depth. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "drive.h"
#include "mag3d.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { MAXREG = 64, MAXMAT = 12, MAXR = 256 };
static const double MU0 = 4e-7 * M_PI;

typedef struct {
    int shape; /* 0 box, 1 circle, 2 sector */
    double a[6];
    int mat, phase;
    bool rotor, radial;
    double sign, mag_sign;
} Reg;

typedef struct {
    char name[32];
    double mu, Br;
} Mat;

typedef struct {
    int nmat, nreg;
    Mat mat[MAXMAT];
    Reg reg[MAXREG];
    double cx, cy;
} Design;

static double wrap360(double a) {
    a = fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

/* which region paints the point (x, y) with the rotor turned by ang degrees; -1 is air */
static int paint(const Design *D, double x, double y, double ang) {
    int hit = -1;
    for (int k = 0; k < D->nreg; k++) {
        const Reg *R = &D->reg[k];
        double px = x, py = y;
        if (R->rotor) { /* the point in the rotor's own frame */
            double c = cos(-ang * M_PI / 180), s = sin(-ang * M_PI / 180), u = x - D->cx, v = y - D->cy;
            px = D->cx + c * u - s * v, py = D->cy + s * u + c * v;
        }
        bool in = false;
        if (R->shape == 0) in = px >= R->a[0] && px <= R->a[2] && py >= R->a[1] && py <= R->a[3];
        else if (R->shape == 1) in = hypot(px - R->a[0], py - R->a[1]) <= R->a[2];
        else {
            double rr = hypot(px - R->a[0], py - R->a[1]), th = atan2(py - R->a[1], px - R->a[0]) * 180 / M_PI;
            in = rr >= R->a[2] && rr <= R->a[3] && wrap360(th - R->a[4]) <= R->a[5] - R->a[4];
        }
        if (in) hit = k;
    }
    return hit;
}

/* chi across a coil's span in angle: rising over the forward slot [a0, a1], 1 to the return slot [b0, b1], falling over it */
static double chi_theta(double th, double a0, double a1, double b0, double b1) {
    double d = wrap360(th - a0), w = a1 - a0, s = wrap360(b0 - a0), e = s + (b1 - b0);
    if (d < w) return d / w;
    if (d < s) return 1;
    if (d < e) return 1 - (d - s) / (b1 - b0);
    return 0;
}

/* the tabulated torque over one period, periodic and linear between its points (drive.h's MgTorqueFn) */
static double table_torque3d(double theta, void *ctx) {
    const struct { const double *t; int n; double step; } *T = ctx;
    double x = theta / T->step, f = floor(x);
    long i = (long)f;
    int a = (int)(((i % T->n) + T->n) % T->n), b = (a + 1) % T->n;
    return T->t[a] + (x - f) * (T->t[b] - T->t[a]);
}

static void add_breaks(double *b, int *n, double r) {
    for (int i = 0; i < *n; i++)
        if (fabs(b[i] - r) < 1e-9) return;
    if (*n < MAXR) b[(*n)++] = r;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

bool lab_run_magnet3d(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    static Design D;
    memset(&D, 0, sizeof D);
    /* materials and regions, as in 2D (mg_scenario.c) */
    D.nmat = 1;
    snprintf(D.mat[0].name, sizeof D.mat[0].name, "air"), D.mat[0].mu = 1;
    const JsonValue *mats = json_get(root, "materials");
    for (size_t k = 0; mats && k < json_len(mats) && D.nmat < MAXMAT; k++) {
        const JsonValue *M = json_at(mats, k);
        if (!json_get_str(M, "source", NULL)) {
            snprintf(err, errlen, "materials[%zu]: every material needs a source for its properties", k);
            return false;
        }
        Mat *m = &D.mat[D.nmat++];
        snprintf(m->name, sizeof m->name, "%s", json_get_str(M, "name", "material"));
        m->mu = json_get_num(M, "relative_permeability", 1), m->Br = json_get_num(M, "remanence_t", 0);
    }
    const JsonValue *regs = json_get(root, "regions");
    for (size_t k = 0; regs && k < json_len(regs) && D.nreg < MAXREG; k++) {
        const JsonValue *J = json_at(regs, k);
        Reg *R = &D.reg[D.nreg];
        if (json_get_numbers(json_get(J, "box_m"), R->a, 4)) R->shape = 0;
        else if (json_get_numbers(json_get(J, "circle_m"), R->a, 3)) R->shape = 1;
        else if (json_get_numbers(json_get(J, "sector_m_deg"), R->a, 6)) R->shape = 2;
        else {
            snprintf(err, errlen, "regions[%zu]: box_m, circle_m or sector_m_deg", k);
            return false;
        }
        const char *mn = json_get_str(J, "material", "air");
        R->mat = -1;
        for (int m = 0; m < D.nmat; m++)
            if (!strcmp(D.mat[m].name, mn)) R->mat = m;
        if (R->mat < 0) {
            snprintf(err, errlen, "regions[%zu]: unknown material \"%s\"", k, mn);
            return false;
        }
        R->rotor = json_get_bool(J, "rotor", false);
        R->radial = !strcmp(json_get_str(J, "magnetisation", "none"), "radial");
        if (!R->radial && strcmp(json_get_str(J, "magnetisation", "none"), "none")) {
            snprintf(err, errlen, "regions[%zu]: the 3D motor takes radial magnetisation only", k);
            return false;
        }
        R->mag_sign = json_get_num(J, "magnetisation_angle_deg", 1) >= 0 ? 1 : -1;
        const char *ph = json_get_str(J, "phase", "");
        R->phase = ph[0] == 'A' ? 0 : ph[0] == 'B' ? 1 : ph[0] == 'C' ? 2 : -1;
        R->sign = json_get_num(J, "phase_sign", 1);
        if (R->phase >= 0 && R->shape != 2) {
            snprintf(err, errlen, "regions[%zu]: a winding must be a sector (slot)", k);
            return false;
        }
        D.nreg++;
    }
    const JsonValue *rot = json_get(root, "rotor"), *cur = json_get(root, "currents"), *run = json_get(root, "run"), *t3 = json_get(root, "three_d");
    double c2[2] = {0, 0};
    json_get_numbers(json_get(rot, "centre_m"), c2, 2);
    D.cx = c2[0], D.cy = c2[1];
    int pp = (int)json_get_int(rot, "pole_pairs", 1);
    double Jpk = json_get_num(cur, "peak_density_a_m2", 0), e0 = json_get_num(cur, "electrical_angle_at_zero_deg", 0);
    double a0 = json_get_num(run, "angle_from_deg", 0), a1 = json_get_num(run, "angle_to_deg", 0);
    int frames = (int)json_get_int(run, "frames", 1);
    double L = json_get_num(t3, "stack_length_m", -1), E = json_get_num(t3, "end_space_m", 0.025), et = json_get_num(t3, "end_turn_m", 0.010);
    double Rout = json_get_num(t3, "outer_radius_m", -1), hr = json_get_num(t3, "radial_cell_m", 0.002);
    int nt = (int)json_get_int(t3, "sectors", 144), nstack = (int)json_get_int(t3, "stack_layers", 10), gap_cells = (int)json_get_int(t3, "gap_cells", 2);
    bool periodic = json_get_bool(t3, "periodic", false);
    if (!(L > 0) || !(Rout > 0) || nt < 12 || nstack < 3 || frames < 1 || !(et > 0 && et < E) || gap_cells < 1) {
        snprintf(err, errlen, "three_d: stack_length_m > 0, outer_radius_m > 0, sectors >= 12, stack_layers >= 3, 0 < end_turn_m < end_space_m");
        return false;
    }

    /* the air gap for the torque: the rings between the rotor's outermost radius and the stator's bore */
    double gap_in = 0, gap_out = 1e9;
    for (int k = 0; k < D.nreg; k++) { /* the rotor's outermost solid radius */
        const Reg *R = &D.reg[k];
        if (R->rotor && R->mat > 0 && R->shape > 0) gap_in = fmax(gap_in, R->shape == 1 ? R->a[2] : R->a[3]);
    }
    for (int k = 0; k < D.nreg; k++) { /* the bore: the smallest inner radius of a solid stator sector beyond it */
        const Reg *R = &D.reg[k];
        if (!R->rotor && R->mat > 0 && R->shape == 2 && R->a[2] >= gap_in - 1e-12) gap_out = fmin(gap_out, R->a[2]);
    }
    if (!(gap_out > gap_in)) {
        snprintf(err, errlen, "motor3d: cannot find the air gap between rotor (%.4g m) and stator", gap_in);
        return false;
    }

    /* rings on every radius a region names about the rotor's centre, split to about radial_cell_m (the air gap into
     * gap_cells, since the torque lives there) */
    double brk[MAXR];
    int nb = 0;
    for (int k = 0; k < D.nreg; k++) {
        const Reg *R = &D.reg[k];
        if (R->shape == 0 || fabs(R->a[0] - D.cx) > 1e-9 || fabs(R->a[1] - D.cy) > 1e-9) continue;
        if (R->shape == 1) add_breaks(brk, &nb, R->a[2]);
        else add_breaks(brk, &nb, R->a[2]), add_breaks(brk, &nb, R->a[3]);
    }
    add_breaks(brk, &nb, Rout);
    qsort(brk, (size_t)nb, sizeof(double), cmp_d);
    while (nb > 0 && brk[nb - 1] > Rout + 1e-12) nb--;
    if (nb < 2) {
        snprintf(err, errlen, "three_d: the regions give no rings about the rotor's centre");
        return false;
    }
    static double r[MAXR * 8], z[512];
    int nr = 0;
    r[0] = brk[0]; /* the innermost circle (the shaft) is the grid's hole: a magnetic wall (mag3d.h) */
    for (int i = 0; i + 1 < nb; i++) {
        int n = (int)ceil((brk[i + 1] - brk[i]) / hr - 1e-9);
        if (n < 2) n = 2;
        if (brk[i] >= gap_in - 1e-12 && brk[i + 1] <= gap_out + 1e-12) n = gap_cells;
        for (int j = 1; j <= n && nr + 1 < MAXR * 8; j++) r[++nr] = brk[i] + (brk[i + 1] - brk[i]) * j / n;
    }
    int nz = 0;
    if (periodic) {
        for (int i = 0; i <= 3; i++) z[i] = -L / 2 + L * i / 3;
        nz = 3;
    } else {
        int nend = (int)ceil(et / (L / nstack) - 1e-9), nair = 2;
        double zz = -L / 2 - E;
        z[nz] = zz;
        for (int i = 1; i <= nair; i++) z[++nz] = -L / 2 - E + (E - et) * i / nair;
        for (int i = 1; i <= nend; i++) z[++nz] = -L / 2 - et + et * i / nend;
        for (int i = 1; i <= nstack; i++) z[++nz] = -L / 2 + L * i / nstack;
        for (int i = 1; i <= nend; i++) z[++nz] = L / 2 + et * i / nend;
        for (int i = 1; i <= nair; i++) z[++nz] = L / 2 + et + (E - et) * i / nair;
    }
    Mag3DGrid g = {nr, nt, nz, r, z, 0.0, periodic};
    size_t n = mag3d_count(&g);
    double *nu = malloc(n * sizeof *nu), *T = calloc(3 * n, sizeof *T), *Br = calloc(3 * n, sizeof *Br), *B = malloc(3 * n * sizeof *B);
    int *mat = malloc(n * sizeof *mat), *kind = malloc(n * sizeof *kind);
    if (!nu || !T || !Br || !B || !mat || !kind) {
        free(nu), free(T), free(Br), free(B), free(mat), free(kind);
        snprintf(err, errlen, "motor3d: out of memory");
        return false;
    }

    /* coils: each forward slot with the nearest return slot of its phase, counter-clockwise */
    int fwd[MAXREG], ret[MAXREG], ncoil = 0;
    for (int k = 0; k < D.nreg; k++) {
        if (D.reg[k].phase < 0 || D.reg[k].sign <= 0) continue;
        int best = -1;
        double bd = 1e9;
        for (int q = 0; q < D.nreg; q++) {
            if (D.reg[q].phase != D.reg[k].phase || D.reg[q].sign >= 0) continue;
            double d = wrap360(D.reg[q].a[4] - D.reg[k].a[4]);
            if (d > 0 && d < bd) bd = d, best = q;
        }
        if (best >= 0) fwd[ncoil] = k, ret[ncoil++] = best;
    }

    /* the result's header */
    JsonValue *meta = json_object(), *fields = NULL;
    bool mok = meta && json_set_string(meta, "domain", "magnet") && json_set_string(meta, "title", json_get_str(root, "title", "3D motor")) &&
               json_set_string(meta, "model", "3D magnetostatics, MFEM Nedelec elements; torque by Arkkio's method") &&
               json_set_string(meta, "time_unit", !periodic && json_get(root, "drive") ? "s" : "deg") && json_set(meta, "scenario", json_clone(root));
    fields = mok ? json_set_object(meta, "fields") : NULL;
    mok = mok && fields && json_set_string(fields, "B", "T");
    JsonValue *looks = mok ? json_set_object(meta, "looks") : NULL; /* copper windings, magnets by pole; iron by |B| */
    mok = mok && looks && json_set_string(looks, "windings", "copper") && json_set_string(looks, "magnets N", "magnet_north") &&
          json_set_string(looks, "magnets S", "magnet_south");
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    if (!w) {
        if (!hdr) snprintf(err, errlen, "motor3d: cannot build the header");
        free(nu), free(T), free(Br), free(B), free(mat), free(kind);
        return false;
    }

    /* vertices of the grid, once; each part takes the ones its cells use */
    size_t nv = (size_t)(nr + 1) * nt * (nz + 1);
    double *xyz = malloc(3 * nv * sizeof *xyz), *pxyz = malloc(3 * nv * sizeof *pxyz), *pB = malloc(n * sizeof *pB);
    int *conn = malloc(8 * n * sizeof *conn), *remap = malloc(nv * sizeof *remap);
    /* a drive keeps every solved field of the sweep (one period of the torque) to show the rotor turning */
    const JsonValue *drv = periodic ? NULL : json_get(root, "drive");
    double *Btab = drv ? malloc((size_t)frames * 3 * n * sizeof *Btab) : NULL, *tqtab = malloc((size_t)frames * sizeof *tqtab);
    int *ktab = drv ? malloc((size_t)frames * n * sizeof *ktab) : NULL;
    if (!xyz || !pxyz || !pB || !conn || !remap || !tqtab || (drv && (!Btab || !ktab))) {
        snprintf(err, errlen, "motor3d: out of memory");
        lab_close(w);
        free(xyz), free(pxyz), free(pB), free(conn), free(remap), free(nu), free(T), free(Br), free(B), free(mat), free(kind), free(Btab), free(ktab), free(tqtab);
        return false;
    }
    for (int iz = 0; iz <= nz; iz++)
        for (int it = 0; it < nt; it++)
            for (int ir = 0; ir <= nr; ir++) {
                size_t v = (size_t)ir + (size_t)(nr + 1) * ((size_t)it + (size_t)nt * iz);
                double th = 2 * M_PI * it / nt;
                xyz[3 * v] = D.cx + r[ir] * cos(th), xyz[3 * v + 1] = D.cy + r[ir] * sin(th), xyz[3 * v + 2] = z[iz];
            }
    double gap_mid = 0.5 * (gap_in + gap_out);
    /* one frame: the parts as hexahedra coloured by |B|; the solution may be shown turned by `shift` sectors (a whole
     * number of periods), and the rotor's own parts turned on by `extra` degrees (between two solved angles) */
    #define WRITE_FRAME(TIME, KIND, BF, SHIFT, EXTRA)                                                                                       \
        do {                                                                                                                               \
            lab_frame_begin(w, (TIME));                                                                                                    \
            static const char *NAMES[6] = {NULL, "stator", "rotor", "magnets N", "windings", "magnets S"};                                 \
            double ce = cos((EXTRA) * M_PI / 180), se = sin((EXTRA) * M_PI / 180);                                                         \
            for (int part = 1; part <= 5; part++) {                                                                                        \
                bool turns = part == 2 || part == 3 || part == 5;                                                                          \
                for (size_t v = 0; v < nv; v++) remap[v] = -1;                                                                             \
                int np = 0, nc = 0;                                                                                                        \
                for (int iz = 0; iz < nz; iz++)                                                                                            \
                    for (int it = 0; it < nt; it++)                                                                                        \
                        for (int ir = 0; ir < nr; ir++) {                                                                                  \
                            size_t e = mag3d_index(&g, ir, ((it - (SHIFT)) % nt + nt) % nt, iz);                                         \
                            if ((KIND)[e] != part) continue;                                                                               \
                            static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}; \
                            for (int q = 0; q < 8; q++) {                                                                                  \
                                size_t v = (size_t)(ir + corner[q][0]) +                                                                   \
                                           (size_t)(nr + 1) * ((size_t)((it + corner[q][1]) % nt) + (size_t)nt * (iz + corner[q][2]));     \
                                if (remap[v] < 0) {                                                                                        \
                                    remap[v] = np;                                                                                         \
                                    double u = xyz[3 * v] - D.cx, vv = xyz[3 * v + 1] - D.cy;                                              \
                                    bool rot = turns && hypot(u, vv) < gap_mid;                                                            \
                                    pxyz[3 * np] = D.cx + (rot ? ce * u - se * vv : u), pxyz[3 * np + 1] = D.cy + (rot ? se * u + ce * vv : vv); \
                                    pxyz[3 * np + 2] = xyz[3 * v + 2];                                                                     \
                                    np++;                                                                                                  \
                                }                                                                                                          \
                                conn[8 * nc + q] = remap[v];                                                                               \
                            }                                                                                                              \
                            const double *b3 = &(BF)[3 * e];                                                                               \
                            pB[nc++] = sqrt(b3[0] * b3[0] + b3[1] * b3[1] + b3[2] * b3[2]);                                                \
                        }                                                                                                                  \
                if (!nc) continue;                                                                                                         \
                lab_part_cells(w, NAMES[part], np, pxyz, nc, LAB_HEX, conn);                                                               \
                lab_field_d(w, "B", LAB_AT_CELL, (size_t)nc, pB);                                                                          \
            }                                                                                                                              \
            if (!lab_frame_end(w)) {                                                                                                       \
                snprintf(err, errlen, "cannot write a frame (disk full?)");                                                                \
                ok = false;                                                                                                                \
            }                                                                                                                              \
        } while (0)

    bool ok = true;
    double tsum = 0, tmin = INFINITY, tmax = -INFINITY, factor_mb = 0, solve_s = 0;
    int dofs = 0;
    int nd = snprintf(info->diagnostics, sizeof info->diagnostics, "{\"torque_n_m\":[");
    clock_t c0 = clock();
    for (int f = 0; f < frames && ok; f++) {
        double ang = frames > 1 ? a0 + (a1 - a0) * f / (frames - 1) : a0;
        double el = (e0 + pp * ang) * M_PI / 180;
        memset(T, 0, 3 * n * sizeof *T), memset(Br, 0, 3 * n * sizeof *Br);
        for (int iz = 0; iz < nz; iz++)
            for (int it = 0; it < nt; it++)
                for (int ir = 0; ir < nr; ir++) {
                    size_t e = mag3d_index(&g, ir, it, iz);
                    double c[3];
                    mag3d_centre(&g, ir, it, iz, c);
                    double x = D.cx + c[0], y = D.cy + c[1], rc = hypot(c[0], c[1]), th = atan2(c[1], c[0]) * 180 / M_PI;
                    double ex = c[0] / rc, ey = c[1] / rc;
                    bool stack = periodic || fabs(c[2]) < L / 2;
                    int k = stack ? paint(&D, x, y, ang) : -1, m = k >= 0 ? D.reg[k].mat : 0;
                    mat[e] = m;
                    kind[e] = k < 0 ? 0 : D.reg[k].phase >= 0 ? 4 : D.mat[m].Br > 0 ? (D.reg[k].mag_sign > 0 ? 3 : 5) : D.reg[k].rotor && D.mat[m].mu > 1 ? 2 : D.mat[m].mu > 1 ? 1 : 0;
                    nu[e] = 1 / (MU0 * D.mat[m].mu);
                    if (k >= 0 && D.mat[m].Br > 0 && D.reg[k].radial) {
                        double sgn = D.reg[k].mag_sign * D.mat[m].Br;
                        Br[3 * e] = sgn * ex, Br[3 * e + 1] = sgn * ey;
                    }
                    for (int q = 0; q < ncoil; q++) {
                        const Reg *F = &D.reg[fwd[q]], *Rt = &D.reg[ret[q]];
                        if (rc < F->a[2] || rc > F->a[3]) continue;
                        double az = fabs(c[2]), cz = periodic || az <= L / 2 ? 1 : az < L / 2 + et ? 1 - (az - L / 2) / et : 0;
                        double ch = chi_theta(th, F->a[4], F->a[5], Rt->a[4], Rt->a[5]) * cz;
                        if (ch == 0) continue;
                        if (!stack && cz > 0) kind[e] = 4; /* the end turns */
                        double Js = F->sign * Jpk * cos(el - F->phase * 2 * M_PI / 3), K = Js * rc * (F->a[5] - F->a[4]) * M_PI / 180;
                        T[3 * e] -= K * ch * ex, T[3 * e + 1] -= K * ch * ey;
                    }
                }
        Mag3DStats st;
        if (!mag3d_solve(&g, nu, NULL, T, Br, B, &st, err, errlen)) {
            ok = false;
            break;
        }
        factor_mb = st.factor_mb, solve_s += st.assemble_s + st.solve_s, dofs = st.dofs;
        /* torque, Arkkio */
        double tq = 0;
        for (int iz = 0; iz < nz; iz++)
            for (int it = 0; it < nt; it++)
                for (int ir = 0; ir < nr; ir++) {
                    if (r[ir] < gap_in - 1e-12 || r[ir + 1] > gap_out + 1e-12) continue;
                    size_t e = mag3d_index(&g, ir, it, iz);
                    double c[3];
                    mag3d_centre(&g, ir, it, iz, c);
                    double rc = hypot(c[0], c[1]), ex = c[0] / rc, ey = c[1] / rc;
                    double br = B[3 * e] * ex + B[3 * e + 1] * ey, bt = -B[3 * e] * ey + B[3 * e + 1] * ex;
                    tq += rc * br * bt * mag3d_volume(&g, ir, it, iz);
                }
        tq /= MU0 * (gap_out - gap_in);
        tqtab[f] = tq;
        tsum += tq, tmin = fmin(tmin, tq), tmax = fmax(tmax, tq);
        if (nd < (int)sizeof info->diagnostics - 64) nd += snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd, "%s%.6g", f ? "," : "", tq);
        if (drv) memcpy(&Btab[(size_t)f * 3 * n], B, 3 * n * sizeof *B), memcpy(&ktab[(size_t)f * n], kind, n * sizeof *kind);
        else WRITE_FRAME(ang, kind, B, 0, 0.0);
        if (!quiet)
            fprintf(stderr, "  angle %7.2f deg  %d unknowns  factor %.0f MB  torque %.5g N m%s  %.1f s cpu\n", ang, st.dofs, st.factor_mb, tq,
                    periodic ? " (periodic slab)" : "", (double)(clock() - c0) / CLOCKS_PER_SEC);
    }
    double span = z[nz] - z[0];
    nd += snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd,
                   "],\"mean_torque_n_m\":%.6g,\"ripple_n_m\":%.6g,\"%s\":%.6g,\"unknowns\":%d,\"factor_mb\":%.0f,\"field_solve_s\":%.3g,"
                   "\"coils\":%d,\"gap_m\":[%.6g,%.6g]",
                   frames ? tsum / frames : 0, tmax - tmin, periodic ? "periodic_slab_m" : "axial_length_m", span, dofs, factor_mb, solve_s, ncoil, gap_in,
                   gap_out);
    if (ok && drv) {
        /* the drive: the torque table over one period turns the rotor (drive.h); the solved fields turn with it, a
         * whole period being the same solution turned by one period's sectors (the currents follow the rotor) */
        const JsonValue *ld = json_get(drv, "load"), *su = json_get(drv, "spin_up"), *dens = json_get(drv, "density_kg_m3");
        int nper = frames - 1;
        double step = nper > 0 ? (a1 - a0) / nper : 0, per_sectors = (a1 - a0) * nt / 360.0, step_sectors = step * nt / 360.0;
        double dur = json_get_num(su, "duration_s", -1), h = json_get_num(su, "step_s", 2e-5);
        int sframes = (int)json_get_int(su, "frames", 40);
        if (fabs(a0) > 1e-12 || nper < 2 || fabs(per_sectors - round(per_sectors)) > 1e-9 || fabs(step_sectors - round(step_sectors)) > 1e-9 ||
            !(dur > 0) || !(h > 0) || sframes < 2 || !json_get_str(ld, "source", NULL) || !json_get_str(dens, "source", NULL)) {
            snprintf(err, errlen, "drive: the sweep must start at 0 and cover one period in whole sectors; spin_up {duration_s, frames, step_s}, "
                                  "load {torque_n_m, fan_n_m_s2, source} and density_kg_m3 {material: value, source} are required");
            ok = false;
        }
        /* the rotor's inertia from its own elements */
        double J = 0;
        for (int iz = 0; ok && iz < nz; iz++)
            for (int it = 0; it < nt; it++)
                for (int ir = 0; ir < nr; ir++) {
                    size_t e = mag3d_index(&g, ir, it, iz);
                    int kd = ktab[e];
                    if (kd != 2 && kd != 3 && kd != 5) continue;
                    double c[3];
                    mag3d_centre(&g, ir, it, iz, c);
                    int k = paint(&D, D.cx + c[0], D.cy + c[1], 0.0);
                    double rho = k >= 0 ? json_get_num(dens, D.mat[D.reg[k].mat].name, -1) : -1;
                    if (!(rho > 0)) {
                        snprintf(err, errlen, "drive: density_kg_m3 has no value for the rotor's material \"%s\"", k >= 0 ? D.mat[D.reg[k].mat].name : "air");
                        ok = false;
                        break;
                    }
                    J += rho * (c[0] * c[0] + c[1] * c[1]) * mag3d_volume(&g, ir, it, iz);
                }
        MgLoad load = {J, json_get_num(ld, "torque_n_m", 0), json_get_num(ld, "fan_n_m_s2", 0)};
        struct Tab { const double *t; int n; double step; } tab = {tqtab, nper, step * M_PI / 180};
        double theta = 0, omega = 0, t = 0, speed_max = 0;
        long steps_per = (long)ceil(dur / (sframes - 1) / h);
        if (ok) nd += snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd, ",\"rotor_inertia_kg_m2\":%.5g,\"speed_rpm\":[", J);
        for (int f = 0; ok && f < sframes; f++) {
            if (f > 0) {
                double hh = dur / (sframes - 1) / steps_per;
                for (long q = 0; q < steps_per; q++) mg_rotor_rk4(&theta, &omega, hh, &load, table_torque3d, &tab), t += hh;
            }
            double deg = theta * 180 / M_PI;
            long idx = lround(deg / step), k = ((idx % nper) + nper) % nper, per = (idx - k) / nper;
            int shift = (int)lround(per * per_sectors);
            WRITE_FRAME(t, &ktab[(size_t)k * n], &Btab[(size_t)k * 3 * n], shift, deg - idx * step);
            double rpm = omega * 60 / (2 * M_PI);
            speed_max = fmax(speed_max, rpm);
            if (nd < (int)sizeof info->diagnostics - 40) nd += snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd, "%s%.1f", f ? "," : "", rpm);
            if (!quiet) fprintf(stderr, "  t %.4f s  rotor %.1f deg  %.0f rpm\n", t, deg, rpm);
        }
        double tm = tsum / frames, wss = load.fan > 0 && tm > load.torque ? sqrt((tm - load.torque) / load.fan) : NAN;
        if (ok && nd < (int)sizeof info->diagnostics - 80)
            nd += snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd, "],\"steady_speed_rpm\":%.5g,\"speed_reached_rpm\":%.5g",
                           isfinite(wss) ? wss * 60 / (2 * M_PI) : -1, speed_max);
    }
    snprintf(info->diagnostics + nd, sizeof info->diagnostics - (size_t)nd, "}");
    info->frames = lab_frames_written(w);
    info->steps = frames;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(xyz), free(pxyz), free(pB), free(conn), free(remap), free(nu), free(T), free(Br), free(B), free(mat), free(kind), free(Btab), free(ktab), free(tqtab);
    return ok;
}
