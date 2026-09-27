/* make_link_geometry.c - the printed robot upper-arm link of the demonstration, as a closed STL in mm.
 *
 * The part is modelled as a signed distance field (boxes, cylinders, smooth unions) and meshed with surface nets: every
 * cell that the surface crosses gets one vertex on the surface, and every grid edge that changes sign becomes a quad.
 * That gives a closed manifold surface whose flat faces stay flat and whose bores stay round, with edges rounded by
 * about one cell.
 *
 * Frame (mm): x runs from the shoulder axis (x = 0) to the elbow axis (x = 170), y across the link, z up.
 *   - shoulder end: a yoke of two 8 mm ears, 20 mm bores, that straddles the shoulder fork of the base
 *   - middle: an I-section web (two flanges and a central plate); design B lightens it with three 16 mm holes
 *   - elbow end: a bearing boss with a 16 mm bore
 *   - motor mount: a 42 x 42 mm NEMA-17 flange on the +y side with four 4.2 mm bolt holes and a 23 mm pilot bore
 *
 * Build and run:
 *   clang -O2 demo/make_link_geometry.c -o build/linkgen -lm
 *   ./build/linkgen a demo/geometry/link_a_solid.stl 0.6
 *   ./build/linkgen b demo/geometry/link_b_lightened.stl 0.6
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y, z; } V3;

static double mind(double a, double b) { return a < b ? a : b; }
static double maxd(double a, double b) { return a > b ? a : b; }

/* smooth union (quadratic polynomial blend, radius k): rounds the joint like a printed fillet */
static double smin(double a, double b, double k) {
    double h = maxd(k - fabs(a - b), 0.0) / k;
    return mind(a, b) - h * h * k * 0.25;
}

static double sd_box(V3 p, V3 c, V3 half) {
    double qx = fabs(p.x - c.x) - half.x, qy = fabs(p.y - c.y) - half.y, qz = fabs(p.z - c.z) - half.z;
    double ox = maxd(qx, 0), oy = maxd(qy, 0), oz = maxd(qz, 0);
    return sqrt(ox * ox + oy * oy + oz * oz) + mind(maxd(qx, maxd(qy, qz)), 0.0);
}

/* cylinder of radius r along axis 0 = x, 1 = y, 2 = z, centred at c, half length h */
static double sd_cyl(V3 p, V3 c, int axis, double r, double h) {
    double a, b, l;
    if (axis == 0) l = p.x - c.x, a = p.y - c.y, b = p.z - c.z;
    else if (axis == 1) l = p.y - c.y, a = p.x - c.x, b = p.z - c.z;
    else l = p.z - c.z, a = p.x - c.x, b = p.y - c.y;
    double d = sqrt(a * a + b * b) - r, e = fabs(l) - h;
    double od = maxd(d, 0), oe = maxd(e, 0);
    return sqrt(od * od + oe * oe) + mind(maxd(d, e), 0.0);
}

static int g_design; /* 0 = A (solid web), 1 = B (I-section with lightening holes and ribs) */

/* the part: negative inside */
static double sdf(double x, double y, double z) {
    V3 p = {x, y, z};
    const double LINK = 170.0;      /* shoulder axis to elbow axis */
    const double EAR_T = 8.0, EAR_GAP = 26.0; /* yoke: two ears of EAR_T, inner gap EAR_GAP */
    double ear_y = 0.5 * (EAR_GAP + EAR_T);
    /* shoulder yoke: two rounded ears with 20 mm bores */
    double ears = 1e9;
    for (int s = -1; s <= 1; s += 2) {
        V3 c = {0, s * ear_y, 0};
        double disc = sd_cyl(p, c, 1, 17.0, EAR_T / 2);                                   /* round end around the bore */
        V3 nb = {24.0, s * ear_y, 0};
        double neck = sd_box(p, nb, (V3){24.0, EAR_T / 2, 13.0});                          /* neck towards the web */
        ears = mind(ears, smin(disc, neck, 6.0));
    }
    double part = ears;
    /* web between the yoke and the elbow boss */
    double x0 = 14.0, x1 = LINK - 16.0, xc = 0.5 * (x0 + x1), xh = 0.5 * (x1 - x0);
    V3 wc = {xc, 0, 0};
    if (g_design == 0) {
        part = smin(part, sd_box(p, wc, (V3){xh, 14.0, 13.0}), 8.0);                       /* A: solid section */
    } else {
        double top = sd_box(p, (V3){xc, 0, 10.5}, (V3){xh, 14.0, 2.5});                    /* B: I-section */
        double bot = sd_box(p, (V3){xc, 0, -10.5}, (V3){xh, 14.0, 2.5});
        double mid = sd_box(p, wc, (V3){xh, 4.0, 13.0});
        double web = smin(smin(top, bot, 3.0), mid, 3.0);
        for (int i = 0; i < 2; i++) {                                                      /* transverse ribs */
            double xr = x0 + (i + 1) * (x1 - x0) / 3.0;
            web = smin(web, sd_box(p, (V3){xr, 0, 0}, (V3){2.5, 14.0, 13.0}), 3.0);
        }
        part = smin(part, web, 8.0);
    }
    /* elbow bearing boss */
    V3 eb = {LINK, 0, 0};
    part = smin(part, sd_cyl(p, eb, 1, 15.0, 14.0), 8.0);
    /* NEMA-17 motor flange on the +y side of the elbow */
    V3 fc = {LINK, 18.5, 0};
    double flange = sd_box(p, fc, (V3){16.0, 4.5, 16.0}) - 5.0;                            /* 42 x 42 plate, reaching the boss */
    part = smin(part, flange, 4.0);
    /* holes: shoulder bores, elbow bore, motor pilot and four bolt holes, web lightening holes */
    double holes = sd_cyl(p, (V3){0, 0, 0}, 1, 10.0, 40.0);                                /* shoulder bores through both ears */
    holes = mind(holes, sd_cyl(p, eb, 1, 8.0, 40.0));                                      /* elbow bore */
    holes = mind(holes, sd_cyl(p, (V3){LINK, 22.0, 0}, 1, 11.5, 9.0));                     /* motor pilot recess */
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            holes = mind(holes, sd_cyl(p, (V3){LINK + sx * 15.5, 18.5, sz * 15.5}, 1, 2.1, 12.0)); /* bolt holes */
    if (g_design == 1)
        for (int i = 0; i < 3; i++) {
            double xh2 = x0 + 12.0 + i * (x1 - x0 - 24.0) / 2.0;
            holes = mind(holes, sd_cyl(p, (V3){xh2, 0, 0}, 1, 5.5, 20.0));                 /* lightening holes in the plate */
        }
    return maxd(part, -holes);
}

/* ------------------------------------------------------------------ surface nets */

typedef struct { float n[3], v[3][3]; uint16_t attr; } Tri;

int main(int argc, char **argv) {
    const char *design = argc > 1 ? argv[1] : "a", *out = argc > 2 ? argv[2] : "link.stl";
    double h = argc > 3 ? atof(argv[3]) : 0.6;
    g_design = design[0] == 'b' || design[0] == 'B';
    const double lo[3] = {-22, -34, -26}, hi[3] = {196, 34, 26};
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
    /* one vertex per cell the surface crosses, at the mean of the crossings on its edges */
    double *vx = malloc(nn * 3 * sizeof *vx);
    int nv = 0;
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
                    double p0[3] = {lo[0] + (i + (E[e][0] & 1)) * h, lo[1] + (j + ((E[e][0] >> 1) & 1)) * h, lo[2] + (k + ((E[e][0] >> 2) & 1)) * h};
                    double p1[3] = {lo[0] + (i + (E[e][1] & 1)) * h, lo[1] + (j + ((E[e][1] >> 1) & 1)) * h, lo[2] + (k + ((E[e][1] >> 2) & 1)) * h};
                    sx += p0[0] + t * (p1[0] - p0[0]), sy += p0[1] + t * (p1[1] - p0[1]), sz += p0[2] + t * (p1[2] - p0[2]);
                    cnt++;
                }
                vx[3 * (size_t)nv] = sx / cnt, vx[3 * (size_t)nv + 1] = sy / cnt, vx[3 * (size_t)nv + 2] = sz / cnt;
                vid[IDX(i, j, k)] = nv++;
            }
    /* every grid edge with a sign change joins the four cells around it into a quad */
    size_t cap = 1 << 16, nt = 0;
    Tri *tri = malloc(cap * sizeof *tri);
    for (int k = 0; k < n[2]; k++)
        for (int j = 0; j < n[1]; j++)
            for (int i = 0; i < n[0]; i++) {
                for (int d = 0; d < 3; d++) {
                    int i1 = i + (d == 0), j1 = j + (d == 1), k1 = k + (d == 2);
                    if (i1 >= n[0] || j1 >= n[1] || k1 >= n[2]) continue;
                    double a = f[IDX(i, j, k)], b = f[IDX(i1, j1, k1)];
                    if ((a < 0) == (b < 0)) continue;
                    /* the four cells sharing this edge */
                    int o[4][3];
                    for (int q = 0; q < 4; q++) {
                        int du = (d + 1) % 3, dv = (d + 2) % 3;
                        int su = (q == 1 || q == 2) ? -1 : 0, sv = (q >= 2) ? -1 : 0;
                        o[q][0] = i, o[q][1] = j, o[q][2] = k;
                        o[q][du] += su, o[q][dv] += sv;
                    }
                    int q[4];
                    int ok = 1;
                    for (int t = 0; t < 4 && ok; t++) {
                        if (o[t][0] < 0 || o[t][1] < 0 || o[t][2] < 0 || o[t][0] + 1 >= n[0] || o[t][1] + 1 >= n[1] || o[t][2] + 1 >= n[2]) ok = 0;
                        else if ((q[t] = vid[IDX(o[t][0], o[t][1], o[t][2])]) < 0) ok = 0;
                    }
                    if (!ok) continue;
                    int order[4] = {q[0], q[1], q[2], q[3]};
                    if (a < 0) { /* keep the outward normal consistent with the sign change */
                        order[1] = q[3], order[3] = q[1];
                    }
                    for (int t = 0; t < 2; t++) {
                        if (nt + 1 >= cap) cap *= 2, tri = realloc(tri, cap * sizeof *tri);
                        int idx[3] = {order[0], order[1 + t], order[2 + t]};
                        Tri *T = &tri[nt++];
                        for (int c2 = 0; c2 < 3; c2++)
                            for (int e = 0; e < 3; e++) T->v[c2][e] = (float)vx[3 * (size_t)idx[c2] + e];
                        double ux = T->v[1][0] - T->v[0][0], uy = T->v[1][1] - T->v[0][1], uz = T->v[1][2] - T->v[0][2];
                        double wx = T->v[2][0] - T->v[0][0], wy = T->v[2][1] - T->v[0][1], wz = T->v[2][2] - T->v[0][2];
                        double nx2 = uy * wz - uz * wy, ny2 = uz * wx - ux * wz, nz2 = ux * wy - uy * wx;
                        double len = sqrt(nx2 * nx2 + ny2 * ny2 + nz2 * nz2);
                        if (len > 0) nx2 /= len, ny2 /= len, nz2 /= len;
                        T->n[0] = (float)nx2, T->n[1] = (float)ny2, T->n[2] = (float)nz2;
                        T->attr = 0;
                    }
                }
            }
    FILE *fp = fopen(out, "wb");
    if (!fp) return fprintf(stderr, "cannot write %s\n", out), 1;
    char header[80];
    memset(header, ' ', sizeof header);
    snprintf(header, sizeof header, "NAVIER demo: printed robot upper-arm link, design %s, %.2f mm grid", g_design ? "B" : "A", h);
    header[strlen(header)] = ' ';
    fwrite(header, 1, 80, fp);
    uint32_t count = (uint32_t)nt;
    fwrite(&count, 4, 1, fp);
    for (size_t t = 0; t < nt; t++) {
        fwrite(tri[t].n, 4, 3, fp);
        fwrite(tri[t].v, 4, 9, fp);
        uint16_t a = 0;
        fwrite(&a, 2, 1, fp);
    }
    fclose(fp);
    printf("%s: design %s, %.2f mm grid, %d vertices, %zu triangles\n", out, g_design ? "B" : "A", h, nv, nt);
    free(f), free(vid), free(vx), free(tri);
    return 0;
}
