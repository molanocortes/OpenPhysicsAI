/* shapes.c - procedural built-in geometries (closed components, outward CCW; flow +X, Y up, Z span) */
#include "shapes.h"
#include "../common.h"

#define PI 3.14159265358979323846
#define RAD(d) ((d) * PI / 180.0)

/* ---------------------------------------------------------------------------------------------- */
/* building blocks */

static bool same_pt(vec3 a, vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static void tri(Mesh *m, vec3 a, vec3 b, vec3 c) {
    if (!same_pt(a, b) && !same_pt(b, c) && !same_pt(a, c)) mesh_add_tri(m, a, b, c);
}
static void quad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d) {
    tri(m, a, b, c);
    tri(m, a, c, d);
}

static vec3 vd(double x, double y, double z) { return v3((float)x, (float)y, (float)z); }

static vec3 *alloc_pts(size_t n) {
    vec3 *p = malloc(n * sizeof(vec3));
    if (!p) LOGE("shapes: out of memory");
    return p;
}

/* Append a closed, consistently wound component; flipped if its winding came out inward. */
static void add_component(Mesh *m, Mesh *c) {
    mesh_fix_orientation(c);
    mesh_append(m, c);
    mesh_free(c);
}

enum { CAP_NONE, CAP_FAN, CAP_STRIP };

/* CAP_FAN: fan to the ring centroid (convex rings). CAP_STRIP: airfoil loop (index 0 = TE, np/2 = LE),
 * closed by quads between upper and lower points at matching chordwise stations. */
static void cap_ring(Mesh *m, const vec3 *ring, int np, int type, bool end) {
    if (type == CAP_FAN) {
        double c[3] = {0, 0, 0};
        for (int k = 0; k < np; k++) c[0] += ring[k].x, c[1] += ring[k].y, c[2] += ring[k].z;
        vec3 ctr = vd(c[0] / np, c[1] / np, c[2] / np);
        for (int k = 0; k < np; k++) {
            vec3 a = ring[k], b = ring[(k + 1) % np];
            if (end) tri(m, ctr, a, b);
            else tri(m, ctr, b, a);
        }
    } else if (type == CAP_STRIP) {
        int n = np / 2;
        for (int i = 0; i < n; i++) {
            vec3 a = ring[n - i], b = ring[n - i - 1], c = ring[(n + i + 1) % np], d = ring[n + i];
            if (end) quad(m, d, c, b, a);
            else quad(m, a, b, c, d);
        }
    }
}

/* Closed loft through nr rings of np points each (pts[r * np + k]). */
static void loft(Mesh *m, const vec3 *pts, int nr, int np, int cap0, int cap1) {
    Mesh c;
    mesh_init(&c);
    for (int r = 0; r + 1 < nr; r++)
        for (int k = 0; k < np; k++) {
            int k1 = (k + 1) % np;
            quad(&c, pts[r * np + k], pts[r * np + k1], pts[(r + 1) * np + k1], pts[(r + 1) * np + k]);
        }
    cap_ring(&c, pts, np, cap0, false);
    cap_ring(&c, pts + (nr - 1) * np, np, cap1, true);
    add_component(m, &c);
}

/* Body with elliptical sections along X; stations with ry = rz = 0 become poles. */
typedef struct { double x, yc, ry, rz; } Station;

static void body_rev(Mesh *m, const Station *st, int ns, int nseg) {
    vec3 *pts = alloc_pts((size_t)ns * (size_t)nseg);
    if (!pts) return;
    for (int i = 0; i < ns; i++)
        for (int k = 0; k < nseg; k++) {
            double ph = 2 * PI * k / nseg;
            bool pole = st[i].ry == 0 && st[i].rz == 0;
            pts[i * nseg + k] = pole ? vd(st[i].x, st[i].yc, 0)
                                     : vd(st[i].x, st[i].yc + st[i].ry * cos(ph), st[i].rz * sin(ph));
        }
    int c0 = st[0].ry == 0 && st[0].rz == 0 ? CAP_NONE : CAP_FAN;
    int c1 = st[ns - 1].ry == 0 && st[ns - 1].rz == 0 ? CAP_NONE : CAP_FAN;
    loft(m, pts, ns, nseg, c0, c1);
    free(pts);
}

/* NACA 4-digit loop with closed trailing edge and cosine spacing: 2n points, index 0 = TE (1,0),
 * 1..n-1 upper surface toward the LE, n = LE (0,0), n+1..2n-1 lower surface. Chord units. */
enum { SEC_MAX = 512 };
typedef struct { int np; double x[SEC_MAX], y[SEC_MAX]; } Section;

static void naca4(Section *s, double m, double p, double t, int n) {
    if (n > SEC_MAX / 2) n = SEC_MAX / 2;
    s->np = 2 * n;
    s->x[0] = 1, s->y[0] = 0;
    s->x[n] = 0, s->y[n] = 0;
    for (int i = 1; i < n; i++) {
        double x = 0.5 * (1 - cos(PI * i / n));
        double yt = 5 * t * (0.2969 * sqrt(x) - 0.1260 * x - 0.3516 * x * x + 0.2843 * x * x * x - 0.1036 * x * x * x * x);
        double yc = 0, dy = 0;
        if (m > 0) {
            if (x < p) yc = m / (p * p) * (2 * p * x - x * x), dy = 2 * m / (p * p) * (p - x);
            else yc = m / ((1 - p) * (1 - p)) * (1 - 2 * p + 2 * p * x - x * x), dy = 2 * m / ((1 - p) * (1 - p)) * (p - x);
        }
        double th = atan(dy);
        s->x[n - i] = x - yt * sin(th), s->y[n - i] = yc + yt * cos(th);
        s->x[n + i] = x + yt * sin(th), s->y[n + i] = yc - yt * cos(th);
    }
}

/* ring[k] = le + chord * (x' X + y' T): section point with thickness scaled by tscale, then rotated by
 * `twist` (radians, positive = nose toward -T) about the quarter chord. */
static void place_section(vec3 *ring, const Section *s, vec3 le, double chord, vec3 tdir, double twist, double tscale) {
    double ca = cos(twist), sa = sin(twist);
    for (int k = 0; k < s->np; k++) {
        double x = s->x[k] - 0.25, y = s->y[k] * tscale;
        double xr = (x * ca - y * sa + 0.25) * chord, yr = (x * sa + y * ca) * chord;
        ring[k] = vd(le.x + xr + tdir.x * yr, le.y + tdir.y * yr, le.z + tdir.z * yr);
    }
}

static void center_bbox(Mesh *m) {
    mesh_compute_bounds(m);
    vec3 c = v3_scale(v3_add(m->bmin, m->bmax), -0.5f);
    mesh_transform(m, m4_translate(c));
}

static double smoothstep01(double t) {
    t = CLAMP(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

/* ---------------------------------------------------------------------------------------------- */
/* simple shapes */

static void gen_sphere(Mesh *m) {
    const double g = (1 + sqrt(5.0)) / 2;
    const double V[12][3] = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g},
                             {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
    static const int F[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9},  {5, 11, 4},
                                 {11, 10, 2}, {10, 7, 6}, {7, 1, 8},   {3, 9, 4},  {3, 4, 2},   {3, 2, 6},  {3, 6, 8},
                                 {3, 8, 9},  {4, 9, 5},  {2, 4, 11},  {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    Mesh a, b;
    mesh_init(&a);
    for (int f = 0; f < 20; f++) {
        vec3 p[3];
        for (int k = 0; k < 3; k++) p[k] = v3_norm(vd(V[F[f][k]][0], V[F[f][k]][1], V[F[f][k]][2]));
        mesh_add_tri(&a, p[0], p[1], p[2]);
    }
    for (int level = 0; level < 4; level++) {
        mesh_init(&b);
        mesh_reserve(&b, a.tri_count * 4);
        for (uint32_t t = 0; t < a.tri_count; t++) {
            vec3 p0 = a.pos[3 * t], p1 = a.pos[3 * t + 1], p2 = a.pos[3 * t + 2];
            vec3 m01 = v3_norm(v3_add(p0, p1)), m12 = v3_norm(v3_add(p1, p2)), m20 = v3_norm(v3_add(p2, p0));
            mesh_add_tri(&b, p0, m01, m20);
            mesh_add_tri(&b, m01, p1, m12);
            mesh_add_tri(&b, m20, m12, p2);
            mesh_add_tri(&b, m01, m12, m20);
        }
        mesh_free(&a);
        a = b;
    }
    for (uint32_t i = 0; i < 3 * a.tri_count; i++) a.pos[i] = v3_scale(a.pos[i], 0.5f);
    add_component(m, &a);
}

static void gen_cylinder(Mesh *m) {
    enum { N = 96 };
    vec3 pts[2 * N];
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < N; k++) pts[r * N + k] = vd(0.5 * cos(2 * PI * k / N), 0.5 * sin(2 * PI * k / N), r ? 2.0 : -2.0);
    loft(m, pts, 2, N, CAP_FAN, CAP_FAN);
}

static void add_box(Mesh *m, vec3 lo, vec3 hi) {
    static const int F[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    vec3 p[8];
    for (int i = 0; i < 8; i++) p[i] = v3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
    Mesh c;
    mesh_init(&c);
    for (int f = 0; f < 6; f++) quad(&c, p[F[f][0]], p[F[f][1]], p[F[f][2]], p[F[f][3]]);
    add_component(m, &c);
}

static void gen_cube(Mesh *m) { add_box(m, v3(-0.5f, -0.5f, -0.5f), v3(0.5f, 0.5f, 0.5f)); }
static void gen_plate(Mesh *m) { add_box(m, v3(-0.01f, -0.5f, -0.5f), v3(0.01f, 0.5f, 0.5f)); }

static void gen_torus(Mesh *m) {
    enum { NU = 96, NV = 48 };
    const double R = 0.5, r = 0.15;
    vec3 *g = alloc_pts(NU * NV);
    if (!g) return;
    for (int u = 0; u < NU; u++)
        for (int v = 0; v < NV; v++) {
            double pu = 2 * PI * u / NU, pv = 2 * PI * v / NV, rr = R + r * cos(pv);
            g[u * NV + v] = vd(r * sin(pv), rr * cos(pu), rr * sin(pu));
        }
    Mesh c;
    mesh_init(&c);
    for (int u = 0; u < NU; u++)
        for (int v = 0; v < NV; v++) {
            int u1 = (u + 1) % NU, v1 = (v + 1) % NV;
            quad(&c, g[u * NV + v], g[u1 * NV + v], g[u1 * NV + v1], g[u * NV + v1]);
        }
    free(g);
    add_component(m, &c);
}

static void gen_airfoil(Mesh *m) {
    static Section s;
    naca4(&s, 0, 0, 0.12, 100);
    vec3 *pts = alloc_pts(2 * (size_t)s.np);
    if (!pts) return;
    for (int r = 0; r < 2; r++) place_section(pts + r * s.np, &s, v3(-0.5f, 0, r ? 0.5f : -0.5f), 1.0, v3(0, 1, 0), 0, 1);
    loft(m, pts, 2, s.np, CAP_STRIP, CAP_STRIP);
    free(pts);
}

static void gen_wing(Mesh *m) {
    static Section s;
    naca4(&s, 0.02, 0.4, 0.12, 80);
    enum { HALF = 24, NR = 2 * HALF + 1 };
    vec3 *pts = alloc_pts((size_t)NR * (size_t)s.np);
    if (!pts) return;
    for (int r = 0; r < NR; r++) {
        double z = -2.5 + 5.0 * r / (NR - 1), az = fabs(z), chord = 1.0 - 0.5 * az / 2.5;
        double xle = tan(RAD(5.0)) * az - 0.25 * chord, y = tan(RAD(3.0)) * az;
        place_section(pts + r * s.np, &s, vd(xle, y, z), chord, v3(0, 1, 0), 0, 1);
    }
    loft(m, pts, NR, s.np, CAP_STRIP, CAP_STRIP);
    free(pts);
}

/* ---------------------------------------------------------------------------------------------- */
/* Ahmed body: loft of axis-aligned rectangles (rounded front via inset, sharp longitudinal edges) */

static void gen_ahmed(Mesh *m) {
    const double L = 1.044, W = 0.389, H = 0.288, R = 0.100, slant = 0.222, ang = RAD(25.0);
    const double sdx = slant * cos(ang);
    enum { NF = 16, NS = NF + 3 };
    vec3 pts[NS * 4];
    for (int i = 0; i < NS; i++) {
        double x, inset = 0;
        if (i <= NF) {
            double th = 0.5 * PI * i / NF;
            x = R - R * cos(th), inset = R * (1 - sin(th));
            if (i == NF) inset = 0, x = R;
        } else {
            x = i == NF + 1 ? L - sdx : L;
        }
        double drop = x > L - sdx ? (x - (L - sdx)) * tan(ang) : 0;
        double ylo = -H / 2 + inset, yhi = H / 2 - inset - drop, zlo = -W / 2 + inset, zhi = W / 2 - inset;
        double xx = x - L / 2;
        pts[i * 4 + 0] = vd(xx, ylo, zlo);
        pts[i * 4 + 1] = vd(xx, ylo, zhi);
        pts[i * 4 + 2] = vd(xx, yhi, zhi);
        pts[i * 4 + 3] = vd(xx, yhi, zlo);
    }
    loft(m, pts, NS, 4, CAP_FAN, CAP_FAN);
}

/* ---------------------------------------------------------------------------------------------- */
/* SUBOFF-like submarine */

static double sub_radius(double x) {
    const double R = 0.117 / 2, re = 0.1 * R;
    if (x <= 0) return 0;
    if (x < 0.2) return R * sqrt(1 - (0.2 - x) * (0.2 - x) / 0.04);
    if (x <= 0.7) return R;
    if (x < 0.985) return re + (R - re) * 0.5 * (1 + cos(PI * (x - 0.7) / 0.285));
    if (x >= 1) return 0;
    double u = (x - 0.985) / 0.015;
    return re * sqrt(1 - u * u);
}

static void gen_submarine(Mesh *m) {
    const double R = 0.117 / 2;
    enum { NB = 20, NM = 5, NT = 28, NC = 8 };
    Station st[NB + NM + NT + NC + 1];
    int ns = 0;
    for (int i = 0; i < NB; i++) { /* bow: x = 0.2 (1 - cos th), r = R sin th */
        double th = 0.5 * PI * i / NB, x = 0.2 * (1 - cos(th));
        st[ns++] = (Station){x, 0, R * sin(th), R * sin(th)};
    }
    for (int i = 0; i < NM; i++) {
        double x = 0.2 + 0.5 * i / NM;
        st[ns++] = (Station){x, 0, R, R};
    }
    for (int i = 0; i < NT; i++) {
        double x = 0.7 + 0.285 * i / NT, r = sub_radius(x);
        st[ns++] = (Station){x, 0, r, r};
    }
    for (int i = 0; i <= NC; i++) { /* elliptic tail cap */
        double th = 0.5 * PI * i / NC, x = 0.985 + 0.015 * sin(th), r = i == NC ? 0 : sub_radius(x);
        if (i == NC) x = 1.0;
        st[ns++] = (Station){x, 0, r, r};
    }
    body_rev(m, st, ns, 64);

    /* sail (fairwater): NACA 0020, rounded top */
    static Section s;
    naca4(&s, 0, 0, 0.20, 40);
    const double chord = 0.07, xle = 0.22, y0 = R - 0.012, ytop = R + 0.055, rr = 0.010;
    enum { NSAIL = 12 };
    vec3 *pts = alloc_pts((size_t)NSAIL * (size_t)s.np);
    if (!pts) return;
    for (int i = 0; i < NSAIL; i++) {
        double y, ts = 1;
        if (i < 5) {
            y = y0 + (ytop - rr - y0) * i / 5.0;
        } else {
            double th = 0.5 * PI * (i - 5) / (NSAIL - 6);
            y = ytop - rr + rr * sin(th);
            ts = MAXI(0.25, cos(th));
        }
        place_section(pts + i * s.np, &s, vd(xle, y, 0), chord, v3(0, 0, 1), 0, ts);
    }
    loft(m, pts, NSAIL, s.np, CAP_STRIP, CAP_STRIP);

    /* four cruciform stern fins at 0.88 L */
    naca4(&s, 0, 0, 0.20, 24);
    const double fx = 0.865, fc = 0.03, r0 = 0.4 * sub_radius(0.88), r1 = sub_radius(0.88) + 0.04;
    const vec3 er[4] = {{0, 1, 0}, {0, 0, 1}, {0, -1, 0}, {0, 0, -1}}, et[4] = {{0, 0, 1}, {0, 1, 0}, {0, 0, 1}, {0, 1, 0}};
    for (int f = 0; f < 4; f++) {
        for (int i = 0; i < 4; i++) {
            double r = r0 + (r1 - r0) * i / 3.0;
            place_section(pts + i * s.np, &s, vd(fx, er[f].y * r, er[f].z * r), fc, et[f], 0, 1);
        }
        loft(m, pts, 4, s.np, CAP_STRIP, CAP_STRIP);
    }
    free(pts);
    center_bbox(m);
}

/* ---------------------------------------------------------------------------------------------- */
/* Sailplane (Discus-like, 1:15): pod-and-boom fuselage, bubble canopy, shoulder wing, T-tail.
 * Built with the nose at x = 0 and the fuselage axis near y = 0, then centred. */

#define GL_L 0.45    /* fuselage length */
#define GL_RM 0.021  /* max fuselage half-width (height = 1.1 x width) */
#define GL_HW 1.1

/* centreline height and half-width of the fuselage at xi = x / L */
static void gl_fuselage(double xi, double *yc, double *rw) {
    double r;
    if (xi <= 0 || xi >= 1) r = 0;
    else if (xi < 0.24) r = GL_RM * sqrt(1 - pow(1 - xi / 0.24, 2.6));
    else if (xi < 0.38) r = GL_RM;
    else if (xi < 0.64) r = 0.0085 + (GL_RM - 0.0085) * (1 - smoothstep01((xi - 0.38) / 0.26));
    else if (xi < 0.975) r = 0.0085 + (0.006 - 0.0085) * (xi - 0.64) / 0.335;
    else r = 0.006 * sqrt(1 - ((xi - 0.975) / 0.025) * ((xi - 0.975) / 0.025));
    double droop = xi < 0.3 ? -0.004 * (1 - xi / 0.3) * (1 - xi / 0.3) : 0;
    *yc = droop + 0.0075 * smoothstep01((xi - 0.36) / 0.34);
    *rw = r;
}

static double gl_top(double xi) {
    double yc, rw;
    gl_fuselage(xi, &yc, &rw);
    return yc + GL_HW * rw;
}

static void gl_body(Mesh *m) {
    Station st[80];
    int ns = 0;
    double xi[80];
    for (int i = 0; i <= 18; i++) xi[ns++] = 0.24 * (1 - cos(0.5 * PI * i / 18));
    for (int i = 1; i <= 4; i++) xi[ns++] = 0.24 + 0.14 * i / 4;
    for (int i = 1; i <= 12; i++) xi[ns++] = 0.38 + 0.26 * i / 12;
    for (int i = 1; i <= 10; i++) xi[ns++] = 0.64 + 0.335 * i / 10;
    for (int i = 1; i <= 8; i++) xi[ns++] = i == 8 ? 1.0 : 0.975 + 0.025 * sin(0.5 * PI * i / 8);
    for (int i = 0; i < ns; i++) {
        double yc, rw;
        gl_fuselage(xi[i], &yc, &rw);
        st[i] = (Station){xi[i] * GL_L, yc, GL_HW * rw, rw};
    }
    body_rev(m, st, ns, 48);

    /* canopy: elliptic bulge whose centreline follows the fuselage top, buried at both ends */
    const double xa = 0.06, xb = 0.30, xp = xa + 0.4 * (xb - xa), Hc = 0.024, Wc = 0.016;
    enum { NCAN = 26 };
    Station cs[NCAN];
    for (int i = 0; i < NCAN; i++) {
        double t = 0.5 * (1 - cos(PI * i / (NCAN - 1))), x = xa + (xb - xa) * t;
        double u = x < xp ? (xp - x) / (xp - xa) : (x - xp) / (xb - xp);
        double e = i == 0 || i == NCAN - 1 ? 0 : x < xp ? sqrt(MAXI(0.0, 1 - u * u)) : MAXI(0.0, 1 - u * u);
        double yc = gl_top(x) - 0.42 * Hc * e - 0.004 * (1 - e);
        cs[i] = (Station){x * GL_L, yc, Hc * e, Wc * e};
    }
    body_rev(m, cs, NCAN, 40);
}

static void gl_wing(Mesh *m) {
    static Section s;
    naca4(&s, 0.04, 0.4, 0.15, 56);
    double h[40];
    int nh = 0;
    static const double hs[] = {0, 0.04, 0.08, 0.12, 0.16, 0.2, 0.25, 0.3, 0.35, 0.4, 0.44, 0.465};
    for (size_t i = 0; i < ARRAY_LEN(hs); i++) h[nh++] = hs[i];
    for (int j = 0; j <= 6; j++) h[nh++] = 0.475 + 0.025 * sin(0.5 * PI * j / 6);
    int nr = 2 * nh - 1;
    vec3 *pts = alloc_pts((size_t)nr * (size_t)s.np);
    if (!pts) return;
    const double xle0 = 0.26 * GL_L, y0 = gl_top(0.3) - 0.007;
    for (int r = 0; r < nr; r++) {
        int i = r < nh ? nh - 1 - r : r - nh + 1;
        double a = h[i], z = r < nh ? -a : a;
        double cn = a <= 0.3 ? 0.070 + (0.060 - 0.070) * a / 0.3 : 0.060 + (0.028 - 0.060) * (a - 0.3) / 0.2;
        double rf = a > 0.475 ? MAXI(0.3, sqrt(MAXI(0.0, 1 - ((a - 0.475) / 0.025) * ((a - 0.475) / 0.025)))) : 1;
        double c = cn * rf, xle = xle0 + 0.75 * (0.070 - cn) + 0.7 * (cn - c);
        place_section(pts + r * s.np, &s, vd(xle, y0 + tan(RAD(3.0)) * a, z), c, v3(0, 1, 0), RAD(1.5) * a / 0.5, 1);
    }
    loft(m, pts, nr, s.np, CAP_STRIP, CAP_STRIP);
    free(pts);
}

static void gl_tail(Mesh *m) {
    static Section s;
    naca4(&s, 0, 0, 0.10, 32);
    double ycb, rwb;
    gl_fuselage(0.93, &ycb, &rwb);
    const double btop = ycb + GL_HW * rwb, ys = btop + 0.075, fxle = 0.448 - 0.060, sw = tan(RAD(25.0));
    enum { NF = 9 };
    vec3 *pts = alloc_pts(40 * (size_t)s.np);
    if (!pts) return;
    /* vertical fin (thickness along Z), root buried in the boom, tip just under the stabiliser mid-plane */
    for (int i = 0; i < NF; i++) {
        double y = (ycb - 0.002) + ((ys - 0.0001) - (ycb - 0.002)) * i / (NF - 1);
        double hf = CLAMP((y - btop) / 0.075, 0.0, 1.0), c = 0.060 + (0.034 - 0.060) * hf;
        place_section(pts + i * s.np, &s, vd(fxle + sw * (y - btop), y, 0), c, v3(0, 0, 1), 0, 1);
    }
    loft(m, pts, NF, s.np, CAP_STRIP, CAP_STRIP);

    /* horizontal stabiliser on top (T-tail) */
    static const double hs[] = {0, 0.015, 0.03, 0.045, 0.06, 0.07, 0.078, 0.083, 0.087, 0.0895, 0.09};
    const int nh = (int)ARRAY_LEN(hs), nr = 2 * nh - 1;
    const double sxle = fxle + sw * 0.075 - 0.002;
    for (int r = 0; r < nr; r++) {
        int i = r < nh ? nh - 1 - r : r - nh + 1;
        double a = hs[i], z = r < nh ? -a : a, cn = 0.038 + (0.024 - 0.038) * a / 0.09;
        double rf = a > 0.078 ? MAXI(0.35, sqrt(MAXI(0.0, 1 - ((a - 0.078) / 0.012) * ((a - 0.078) / 0.012)))) : 1;
        double c = cn * rf;
        place_section(pts + r * s.np, &s, vd(sxle + 0.6 * (0.038 - cn) + 0.7 * (cn - c), ys, z), c, v3(0, 1, 0), 0, 1);
    }
    loft(m, pts, nr, s.np, CAP_STRIP, CAP_STRIP);
    free(pts);
}

static void gen_glider(Mesh *m) {
    gl_body(m);
    gl_wing(m);
    gl_tail(m);
    center_bbox(m);
}

/* ---------------------------------------------------------------------------------------------- */

static const struct {
    ShapeInfo info;
    void (*gen)(Mesh *m);
} g_shapes[] = {
    {{"sphere", "icosphere, diameter 1"}, gen_sphere},
    {{"cylinder", "circular cylinder across the flow (axis Z), D 1, length 4"}, gen_cylinder},
    {{"cube", "1 x 1 x 1 box"}, gen_cube},
    {{"plate", "thin flat plate facing the flow, 1 x 1, thickness 0.02"}, gen_plate},
    {{"torus", "ring with its axis along the flow, R 0.5, r 0.15"}, gen_torus},
    {{"airfoil", "extruded NACA 0012, chord 1, span 1"}, gen_airfoil},
    {{"wing", "tapered NACA 2412 wing, span 5, taper 0.5, 5 deg sweep, 3 deg dihedral"}, gen_wing},
    {{"glider", "Discus-like sailplane at 1:15, span 1, T-tail"}, gen_glider},
    {{"ahmed", "Ahmed reference body, 25 deg rear slant"}, gen_ahmed},
    {{"submarine", "DARPA SUBOFF-like hull with sail and cruciform stern fins"}, gen_submarine},
};

int shape_count(void) { return (int)ARRAY_LEN(g_shapes); }

const ShapeInfo *shape_info(int index) { return index >= 0 && index < shape_count() ? &g_shapes[index].info : NULL; }

bool shape_generate(const char *name, Mesh *out) {
    for (int i = 0; name && i < shape_count(); i++) {
        if (!str_ieq(name, g_shapes[i].info.name)) continue;
        mesh_init(out);
        g_shapes[i].gen(out);
        mesh_compute_bounds(out);
        mesh_compute_normals(out, 40.0f);
        str_copy(out->name, sizeof out->name, g_shapes[i].info.name);
        return true;
    }
    return false;
}
