/* make_bridge_geometry.c - a printed pedestrian truss bridge (scale model), as a closed STL in mm.
 *
 * Two Pratt trusses (bottom chord, top chord, inclined end posts, verticals, diagonals sloping down towards mid-span) on a
 * deck plate with transverse floor beams, a top lateral system (struts and X-bracing between the top chords), portal
 * struts over the end posts, and bearing blocks with pin bores at both abutments. The model is printed deck-down, so every
 * member rises from the deck: end posts and diagonals at 56.6 degrees, and the top lateral members bridge between the
 * top chords.
 *
 * The part is a signed distance field (boxes, oriented bars, smooth unions that act as gusset fillets) meshed with surface
 * nets, which gives a closed manifold surface with flat faces kept flat and edges rounded by about one cell.
 *
 * Frame (mm): x along the span (bearing centres at x = 0 and x = 480), y across (trusses at y = +-40), z up (deck on the
 * build plate at z = 0).
 *
 *   clang -O2 demo/make_bridge_geometry.c -o build/bridgegen -lm
 *   ./build/bridgegen demo/geometry/truss_bridge.stl 1.0
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y, z; } V3;

static double mind(double a, double b) { return a < b ? a : b; }
static double maxd(double a, double b) { return a > b ? a : b; }
static double smin(double a, double b, double k) {
    double h = maxd(k - fabs(a - b), 0.0) / k;
    return mind(a, b) - h * h * k * 0.25;
}

static double sd_box(V3 p, V3 c, V3 half) {
    double qx = fabs(p.x - c.x) - half.x, qy = fabs(p.y - c.y) - half.y, qz = fabs(p.z - c.z) - half.z;
    double ox = maxd(qx, 0), oy = maxd(qy, 0), oz = maxd(qz, 0);
    return sqrt(ox * ox + oy * oy + oz * oz) + mind(maxd(qx, maxd(qy, qz)), 0.0);
}

/* square bar of half width w between a and b (the bar's own axes: along a->b, and the two directions orthogonal to it
 * chosen with the second one in the vertical plane when possible) */
static double sd_bar(V3 p, V3 a, V3 b, double w) {
    double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, L = sqrt(dx * dx + dy * dy + dz * dz);
    double ux = dx / L, uy = dy / L, uz = dz / L;
    /* v: horizontal normal to the bar (or y for vertical bars); n = u x v */
    double vx = -uy, vy = ux, vz = 0, vl = sqrt(vx * vx + vy * vy);
    if (vl < 1e-9) vx = 0, vy = 1, vz = 0, vl = 1;
    vx /= vl, vy /= vl, vz /= vl;
    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    double px = p.x - 0.5 * (a.x + b.x), py = p.y - 0.5 * (a.y + b.y), pz = p.z - 0.5 * (a.z + b.z);
    V3 q = {px * ux + py * uy + pz * uz, px * vx + py * vy + pz * vz, px * nx + py * ny + pz * nz};
    return sd_box(q, (V3){0, 0, 0}, (V3){0.5 * L, w, w});
}

static double sd_cyl_y(V3 p, double cx, double cz, double r, double y0, double y1) {
    double d = sqrt((p.x - cx) * (p.x - cx) + (p.z - cz) * (p.z - cz)) - r;
    double e = fabs(p.y - 0.5 * (y0 + y1)) - 0.5 * (y1 - y0);
    return sqrt(maxd(d, 0) * maxd(d, 0) + maxd(e, 0) * maxd(e, 0)) + mind(maxd(d, e), 0.0);
}

enum { PANELS = 8 };
static const double SPAN = 480.0, PANEL = 60.0, TRUSS_Y = 40.0, BAR = 4.0; /* members 8 x 8 mm */
static const double Z_BOT = 9.0, Z_TOP = 100.0, DECK_T = 5.0;

static double sdf(double x, double y, double z) {
    V3 p = {x, y, z};
    const double K = 2.5; /* gusset fillet */
    double part = sd_box(p, (V3){SPAN / 2, 0, DECK_T / 2}, (V3){SPAN / 2 + 18, 45, DECK_T / 2}); /* deck plate on the plate */
    for (int s = -1; s <= 1; s += 2) {
        double ty = s * TRUSS_Y, t = 1e9;
        t = mind(t, sd_bar(p, (V3){-8, ty, Z_BOT}, (V3){SPAN + 8, ty, Z_BOT}, BAR));                       /* bottom chord */
        t = mind(t, sd_bar(p, (V3){PANEL, ty, Z_TOP}, (V3){SPAN - PANEL, ty, Z_TOP}, BAR));                 /* top chord */
        t = smin(t, sd_bar(p, (V3){0, ty, Z_BOT}, (V3){PANEL, ty, Z_TOP}, BAR), K);                         /* end posts */
        t = smin(t, sd_bar(p, (V3){SPAN, ty, Z_BOT}, (V3){SPAN - PANEL, ty, Z_TOP}, BAR), K);
        for (int i = 1; i < PANELS; i++) t = smin(t, sd_bar(p, (V3){i * PANEL, ty, Z_BOT}, (V3){i * PANEL, ty, Z_TOP}, BAR), K); /* verticals */
        for (int i = 1; i < PANELS - 1; i++) { /* Pratt diagonals: down towards mid-span */
            double xt = i < PANELS / 2 ? i * PANEL : (i + 1) * PANEL, xb = i < PANELS / 2 ? (i + 1) * PANEL : i * PANEL;
            t = smin(t, sd_bar(p, (V3){xt, ty, Z_TOP}, (V3){xb, ty, Z_BOT}, 3.0), K);
        }
        part = smin(part, t, K);
    }
    for (int i = 0; i <= PANELS; i++) /* floor beams on the deck */
        part = smin(part, sd_box(p, (V3){i * PANEL, 0, DECK_T + 3.0}, (V3){3.0, TRUSS_Y, 3.0}), K);
    for (int i = 1; i < PANELS; i++) /* top struts */
        part = smin(part, sd_box(p, (V3){i * PANEL, 0, Z_TOP}, (V3){3.0, TRUSS_Y, 3.0}), K);
    for (int i = 1; i < PANELS - 1; i++) { /* top X-bracing in each inner panel */
        double x0 = i * PANEL, x1 = (i + 1) * PANEL;
        part = smin(part, sd_bar(p, (V3){x0, -TRUSS_Y, Z_TOP}, (V3){x1, TRUSS_Y, Z_TOP}, 3.0), K);
        part = smin(part, sd_bar(p, (V3){x0, TRUSS_Y, Z_TOP}, (V3){x1, -TRUSS_Y, Z_TOP}, 3.0), K);
    }
    /* bearing blocks with pin bores along y at both abutments */
    for (int e = 0; e < 2; e++) {
        double xc = e ? SPAN : 0;
        double blk = sd_box(p, (V3){xc, 0, 11.0}, (V3){13.0, 47.0, 11.0});
        part = smin(part, blk, 3.0);
    }
    double holes = 1e9;
    for (int e = 0; e < 2; e++) {
        double xc = e ? SPAN : 0;
        holes = mind(holes, sd_cyl_y(p, xc, 13.0, 4.0, -60, 60)); /* 8 mm pin bores */
    }
    return maxd(part, -holes);
}

/* ------------------------------------------------------------------ surface nets */

typedef struct { float n[3], v[3][3]; } Tri;

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "truss_bridge.stl";
    double h = argc > 2 ? atof(argv[2]) : 0.8;
    const double lo[3] = {-40, -60, -4}, hi[3] = {SPAN + 40, 60, Z_TOP + 12};
    int n[3];
    for (int d = 0; d < 3; d++) n[d] = (int)ceil((hi[d] - lo[d]) / h) + 1;
    size_t nn = (size_t)n[0] * n[1] * n[2];
    float *f = malloc(nn * sizeof *f);
    int *vid = malloc(nn * sizeof *vid);
    if (!f || !vid) return fprintf(stderr, "out of memory\n"), 1;
#define IDX(i, j, k) (((size_t)(k) * n[1] + (j)) * n[0] + (i))
    for (int k = 0; k < n[2]; k++)
        for (int j = 0; j < n[1]; j++)
            for (int i = 0; i < n[0]; i++) f[IDX(i, j, k)] = (float)sdf(lo[0] + i * h, lo[1] + j * h, lo[2] + k * h);
    size_t vcap = 1 << 20, nv = 0;
    double *vx = malloc(vcap * 3 * sizeof *vx);
    static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (size_t i = 0; i < nn; i++) vid[i] = -1;
    for (int k = 0; k + 1 < n[2]; k++)
        for (int j = 0; j + 1 < n[1]; j++)
            for (int i = 0; i + 1 < n[0]; i++) {
                double c[8], sx = 0, sy = 0, sz = 0;
                int cnt = 0, neg = 0;
                for (int b = 0; b < 8; b++) c[b] = f[IDX(i + (b & 1), j + ((b >> 1) & 1), k + ((b >> 2) & 1))], neg += c[b] < 0;
                if (neg == 0 || neg == 8) continue;
                for (int e = 0; e < 12; e++) {
                    double a = c[E[e][0]], b2 = c[E[e][1]];
                    if ((a < 0) == (b2 < 0)) continue;
                    double t = a / (a - b2);
                    int a0 = E[e][0], a1 = E[e][1];
                    sx += lo[0] + (i + (a0 & 1) + t * ((a1 & 1) - (a0 & 1))) * h;
                    sy += lo[1] + (j + ((a0 >> 1) & 1) + t * (((a1 >> 1) & 1) - ((a0 >> 1) & 1))) * h;
                    sz += lo[2] + (k + ((a0 >> 2) & 1) + t * (((a1 >> 2) & 1) - ((a0 >> 2) & 1))) * h;
                    cnt++;
                }
                if (nv == vcap) vcap *= 2, vx = realloc(vx, vcap * 3 * sizeof *vx);
                vx[3 * nv] = sx / cnt, vx[3 * nv + 1] = sy / cnt, vx[3 * nv + 2] = sz / cnt;
                vid[IDX(i, j, k)] = (int)nv++;
            }
    size_t tcap = 1 << 20, nt = 0;
    Tri *tri = malloc(tcap * sizeof *tri);
    for (int k = 0; k < n[2]; k++)
        for (int j = 0; j < n[1]; j++)
            for (int i = 0; i < n[0]; i++)
                for (int d = 0; d < 3; d++) {
                    int i1 = i + (d == 0), j1 = j + (d == 1), k1 = k + (d == 2);
                    if (i1 >= n[0] || j1 >= n[1] || k1 >= n[2]) continue;
                    double a = f[IDX(i, j, k)], b = f[IDX(i1, j1, k1)];
                    if ((a < 0) == (b < 0)) continue;
                    int o[4][3], q[4], ok = 1;
                    for (int t = 0; t < 4; t++) {
                        int du = (d + 1) % 3, dv = (d + 2) % 3;
                        o[t][0] = i, o[t][1] = j, o[t][2] = k;
                        o[t][du] += (t == 1 || t == 2) ? -1 : 0;
                        o[t][dv] += (t >= 2) ? -1 : 0;
                    }
                    for (int t = 0; t < 4 && ok; t++) {
                        if (o[t][0] < 0 || o[t][1] < 0 || o[t][2] < 0 || o[t][0] + 1 >= n[0] || o[t][1] + 1 >= n[1] || o[t][2] + 1 >= n[2]) ok = 0;
                        else if ((q[t] = vid[IDX(o[t][0], o[t][1], o[t][2])]) < 0) ok = 0;
                    }
                    if (!ok) continue;
                    int order[4] = {q[0], q[1], q[2], q[3]};
                    if (a < 0) order[1] = q[3], order[3] = q[1];
                    for (int t = 0; t < 2; t++) {
                        if (nt == tcap) tcap *= 2, tri = realloc(tri, tcap * sizeof *tri);
                        int idx[3] = {order[0], order[1 + t], order[2 + t]};
                        Tri *T = &tri[nt++];
                        for (int c2 = 0; c2 < 3; c2++)
                            for (int e = 0; e < 3; e++) T->v[c2][e] = (float)vx[3 * (size_t)idx[c2] + e];
                        double ux = T->v[1][0] - T->v[0][0], uy = T->v[1][1] - T->v[0][1], uz = T->v[1][2] - T->v[0][2];
                        double wx = T->v[2][0] - T->v[0][0], wy = T->v[2][1] - T->v[0][1], wz = T->v[2][2] - T->v[0][2];
                        double cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx, len = sqrt(cx * cx + cy * cy + cz * cz);
                        if (len > 0) cx /= len, cy /= len, cz /= len;
                        T->n[0] = (float)cx, T->n[1] = (float)cy, T->n[2] = (float)cz;
                    }
                }
    FILE *fp = fopen(out, "wb");
    if (!fp) return fprintf(stderr, "cannot write %s\n", out), 1;
    char header[80];
    memset(header, ' ', sizeof header);
    int hl = snprintf(header, sizeof header, "NAVIER demo: printed Pratt truss bridge, %.2f mm grid", h);
    if (hl > 0 && hl < 80) header[hl] = ' ';
    fwrite(header, 1, 80, fp);
    uint32_t count = (uint32_t)nt;
    fwrite(&count, 4, 1, fp);
    for (size_t t = 0; t < nt; t++) {
        uint16_t attr = 0;
        fwrite(tri[t].n, 4, 3, fp), fwrite(tri[t].v, 4, 9, fp), fwrite(&attr, 2, 1, fp);
    }
    fclose(fp);
    printf("%s: %.2f mm grid, %zu vertices, %zu triangles\n", out, h, nv, nt);
    free(f), free(vid), free(vx), free(tri);
    return 0;
}
