/* gear.c - see gear.h. The tooth space is generated as the envelope of the rack: a point of the rack's profile
 * touches the gear when its normal passes through the pitch point (the law of gearing for a rack rolling on the
 * pitch circle), so each rack point maps to exactly one gear point, and the straight flank, the tip fillet and the
 * tip line of the rack give the involute, the trochoidal fillet and the root circle. */
#include "gear.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gear_spec_defaults(GearSpec *s) {
    memset(s, 0, sizeof *s);
    s->pressure_deg = 20;
    s->addendum = 1;
    s->rack_addendum = 1.25;
    s->rack_tip_radius = 0.38;
}

double gear_inv(double a) { return tan(a) - a; }

double gear_flank_half_angle(const GearGeometry *g, double R) {
    const GearSpec *s = &g->spec;
    double an = s->pressure_deg * M_PI / 180, mt = s->module_m / cos(g->helix);
    double s_ref = mt * (M_PI / 2 + 2 * s->shift * tan(an)); /* transverse, before thinning */
    double aR = acos(fmin(1, g->r_b / R));
    /* a flank moved by t along its normal moves by t / cos(beta_b) along its transverse normal: a turn of that over r_b */
    return s_ref / (2 * g->r) + gear_inv(g->alpha) - gear_inv(aR) - s->thinning_m / (g->r_b * cos(g->beta_b));
}

/* the helix: from the spec, the transverse pressure angle and module, and the twist of a section with reference radius r */
static void helix_of(const GearSpec *s, double *beta, double *alpha_t, double *mt) {
    double an = s->pressure_deg * M_PI / 180;
    *beta = s->helix_deg * M_PI / 180;
    *mt = s->module_m / cos(*beta);
    *alpha_t = atan(tan(an) / cos(*beta));
}

bool gear_involute_point(const GearGeometry *g, double R, GearPoint *out) {
    if (R < g->r_b || R > g->r_a * (1 + 1e-12)) return false;
    double psi = gear_flank_half_angle(g, R), aR = acos(fmin(1, g->r_b / R));
    /* the point at polar angle pi/2 - psi; the involute's normal is tangent to the base circle: on the right flank
     * it points away from the tooth at the polar angle minus (pi/2 - aR), aR the pressure angle at this radius */
    double th = M_PI / 2 - psi;
    out->x = R * cos(th), out->y = R * sin(th);
    double nth = th - M_PI / 2 + aR;
    out->nx = cos(nth), out->ny = sin(nth);
    out->kind = GEAR_INVOLUTE;
    out->roll = sqrt(fmax(0, R * R - g->r_b * g->r_b));
    return true;
}

/* one point of the rack's profile (u, v from the datum line, outward unit normal n of the rack tooth) mapped onto
 * the gear, in the frame where the rack tooth centred on u = 0 cuts the space centred on +y. The rack tooth's outward
 * normal points into the gear's material, so the gear's own outward normal is its opposite. */
static void envelope(double r, double xm, double u, double v, double nu, double nv, double *gx, double *gy, double *gnx, double *gny) {
    double phi = (u - (xm + v) * nu / nv) / r; /* the gear's turn at which this rack point touches it */
    double px = u - r * phi, py = r + xm + v;  /* the rack has moved by -r phi while the gear turned by phi */
    double c = cos(phi), s = sin(phi);
    *gx = px * c + py * s, *gy = -px * s + py * c;
    *gnx = -(nu * c + nv * s), *gny = -(-nu * s + nv * c);
}

typedef struct Buf {
    GearPoint *p;
    int n, cap;
} Buf;

static bool push(Buf *b, double x, double y, double nx, double ny, int kind, double roll) {
    if (b->n == b->cap) {
        int cap = b->cap ? 2 * b->cap : 256;
        GearPoint *q = realloc(b->p, (size_t)cap * sizeof *q);
        if (!q) return false;
        b->p = q, b->cap = cap;
    }
    b->p[b->n++] = (GearPoint){x, y, nx, ny, kind, roll};
    return true;
}

bool gear_build(const GearSpec *spec, int samples, GearGeometry *g, char *err, size_t errlen) {
    memset(g, 0, sizeof *g);
    g->spec = *spec;
    const GearSpec *s = &g->spec;
    if (s->teeth < 6 || s->module_m <= 0 || s->pressure_deg <= 0 || s->pressure_deg >= 45 || s->face_width_m <= 0) {
        snprintf(err, errlen, "gear: teeth >= 6, a module, a pressure angle in (0, 45) degrees and a face width are needed");
        return false;
    }
    if (samples < 40) samples = 40;
    double m = s->module_m, z = s->teeth, beta, mt;
    helix_of(s, &beta, &g->alpha, &mt);
    if (fabs(beta) >= 45 * M_PI / 180) {
        snprintf(err, errlen, "gear: a helix angle below 45 degrees is needed");
        return false;
    }
    g->helix = beta;
    g->r = mt * z / 2;
    g->r_b = g->r * cos(g->alpha);
    g->twist = tan(beta) / g->r;
    g->beta_b = atan(fabs(tan(beta)) * cos(g->alpha));
    double cb = cos(beta);
    double xm = s->shift * m;
    g->r_a = s->tip_radius_m > 0 ? s->tip_radius_m : g->r + (s->addendum + s->shift) * m;
    g->r_f = g->r + xm - s->rack_addendum * m;
    /* the rack in its normal section (the hob's), the profile mapped to the transverse plane by u_t = u_n / cos(beta),
     * normals by (n_u cos(beta), n_v) */
    double an = s->pressure_deg * M_PI / 180, ta = tan(an), ca = cos(an), sa = sin(an);
    g->tooth_thickness_ref_m = mt * (M_PI / 2 + 2 * s->shift * tan(an)) - 2 * s->thinning_m / (ca * cb);
    if (s->bore_radius_m <= 0 || s->bore_radius_m >= g->r_f - 1.5 * m) {
        snprintf(err, errlen, "gear: the bore radius must be positive and at least 1.5 modules inside the root circle (%.4g m)", g->r_f);
        return false;
    }
    /* the rack: half-width on the datum line w0 (widened by the thinning), tip line at v = -h m, tip fillet rho */
    double w0 = M_PI * m / 4 + s->thinning_m / ca;
    double h = s->rack_addendum * m, rho = s->rack_tip_radius * m;
    double vc = -h + rho;
    double uc = w0 + vc * ta - rho / ca;
    if (uc < 0) { /* the fillets meet: a full round tip, the largest radius that fits (u_c = 0) */
        rho = (w0 - h * ta) / (1 / ca - ta);
        vc = -h + rho, uc = 0;
    }
#define ENV(U, V, NU, NV) do { \
        double nu_ = (NU) * cb, nv_ = (NV), L_ = hypot(nu_, nv_); \
        envelope(g->r, xm, (U) / cb, (V), nu_ / L_, nv_ / L_, &gx, &gy, &gnx, &gny); \
    } while (0)
    Buf b = {0};
    bool ok = true;
    int nroot = samples / 10 + 4, nfil = samples / 2, ninv = samples;
    double gx, gy, gnx, gny;
    /* root circle: the rack's tip line from the space's middle (u = 0) to the fillet (u = uc) */
    for (int i = 0; i < nroot && ok && uc > 0; i++) {
        ENV(uc * i / nroot, -h, 0, -1);
        ok = push(&b, gx, gy, gnx, gny, GEAR_ROOT, -1);
    }
    /* fillet: its normal from straight down (on the tip line) to the flank's */
    for (int i = 0; i < nfil && ok; i++) {
        double bb = -M_PI / 2 + (M_PI / 2 - an) * i / nfil;
        ENV(uc + rho * cos(bb), vc + rho * sin(bb), cos(bb), sin(bb));
        ok = push(&b, gx, gy, gnx, gny, GEAR_FILLET, -1);
    }
    int form = b.n;
    /* flank: v from the fillet's tangency up until the generated point reaches the tip circle; the radius grows
     * monotonically along a flank that is not undercut, so the end is found by bisection */
    double vt = vc - rho * sa, nu = ca, nv = -sa;
    double v_hi = vt + 0.1 * m;
    for (int it = 0; it < 200; it++) {
        ENV(w0 + v_hi * ta, v_hi, nu, nv);
        if (hypot(gx, gy) >= g->r_a) break;
        v_hi += 0.1 * m;
    }
    double lo = vt, hi = v_hi;
    for (int it = 0; it < 200; it++) {
        double mid = 0.5 * (lo + hi);
        ENV(w0 + mid * ta, mid, nu, nv);
        if (hypot(gx, gy) < g->r_a) lo = mid;
        else hi = mid;
    }
    double v_tip = 0.5 * (lo + hi);
    for (int i = 0; i <= ninv && ok; i++) {
        double v = vt + (v_tip - vt) * i / ninv;
        ENV(w0 + v * ta, v, nu, nv);
        double R = hypot(gx, gy);
        ok = push(&b, gx, gy, gnx, gny, GEAR_INVOLUTE, sqrt(fmax(0, R * R - g->r_b * g->r_b)));
    }
#undef ENV
    if (!ok) {
        free(b.p);
        snprintf(err, errlen, "gear: out of memory");
        return false;
    }
    /* undercut: along a side that is not undercut the radius only grows; the fillet cutting back into the involute
     * makes it fall somewhere */
    for (int i = 1; i < b.n; i++)
        if (hypot(b.p[i].x, b.p[i].y) < hypot(b.p[i - 1].x, b.p[i - 1].y) - 1e-12 * g->r) {
            free(b.p);
            snprintf(err, errlen, "gear: the tooth is undercut (z = %d, x = %.3g): the generating fillet cuts into the involute; use a profile shift", s->teeth, s->shift);
            return false;
        }
    /* the side generated by the rack's right half is the left flank of the tooth on the right of the space: turn it
     * by +pi/z to centre that tooth on +y, then mirror it (x -> -x) to describe the tooth's right side */
    double rot = M_PI / z, c = cos(rot), sn = sin(rot);
    for (int i = 0; i < b.n; i++) {
        GearPoint *p = &b.p[i];
        double x = p->x * c - p->y * sn, y = p->x * sn + p->y * c;
        double nx = p->nx * c - p->ny * sn, ny = p->nx * sn + p->ny * c;
        p->x = -x, p->y = y, p->nx = -nx, p->ny = ny;
    }
    /* tip relief: a linear modification in roll length, along the normal */
    if (s->tip_relief_m > 0) {
        double rs = s->relief_start_m > g->r_b ? s->relief_start_m : g->r_b;
        double Ls = sqrt(fmax(0, rs * rs - g->r_b * g->r_b)), La = sqrt(g->r_a * g->r_a - g->r_b * g->r_b);
        if (La <= Ls) {
            free(b.p);
            snprintf(err, errlen, "gear: the tip relief must start inside the tip circle");
            return false;
        }
        for (int i = form; i < b.n; i++) {
            GearPoint *p = &b.p[i];
            if (p->roll > Ls) {
                double d = s->tip_relief_m * (p->roll - Ls) / (La - Ls);
                p->x -= d * p->nx, p->y -= d * p->ny, p->kind = GEAR_RELIEF;
            }
        }
    }
    g->right.p = b.p, g->right.n = b.n, g->right.form = form;
    g->right.r_form = hypot(b.p[form].x, b.p[form].y);
    GearPoint *tip = &b.p[b.n - 1];
    g->tip_thickness_m = 2 * hypot(tip->x, tip->y) * atan2(tip->x, tip->y);
    if (g->tip_thickness_m <= 0) {
        gear_free(g);
        snprintf(err, errlen, "gear: the tooth comes to a point below the tip circle; reduce the tip radius or the shift");
        return false;
    }
    return true;
}

/* The cutter rolls inside the ring (the ring held, its centre at the origin): with its centre at polar angle beta on
 * the cutting centre distance a, the instantaneous centre is the pitch point at r_w along the same ray, and pure
 * rolling turns the cutter by -(a / r_cw) (beta - pi/2) from its reference pose (beta = pi/2: centre on +y, the tooth
 * pointing +y, cutting the space centred on +y). A cutter point touches the ring when its normal passes through the
 * pitch point. */
typedef struct Shaper {
    double a, rw, rcw; /* cutting centre distance, the ring's and the cutter's cutting pitch radii */
} Shaper;

static void shaper_pose(const Shaper *k, double beta, double px, double py, double nx, double ny, double *wx, double *wy, double *wnx,
                        double *wny) {
    double chi = -(k->a / k->rcw) * (beta - M_PI / 2), c = cos(chi), s = sin(chi);
    *wx = k->a * cos(beta) + c * px - s * py, *wy = k->a * sin(beta) + s * px + c * py;
    *wnx = c * nx - s * ny, *wny = s * nx + c * ny;
}

/* the meshing function: the normal's moment about the pitch point (zero when it passes through it) */
static double shaper_f(const Shaper *k, double beta, double px, double py, double nx, double ny) {
    double wx, wy, wnx, wny, ix = k->rw * cos(beta), iy = k->rw * sin(beta);
    shaper_pose(k, beta, px, py, nx, ny, &wx, &wy, &wnx, &wny);
    return (wx - ix) * wny - (wy - iy) * wnx;
}

/* the ring point a cutter point (px, py, outward normal nx, ny, in the cutter's frame) cuts, with the ring's outward
 * normal; false if it never touches within a pitch and a half */
static bool shaper_cut(const Shaper *k, double span, double px, double py, double nx, double ny, double *gx, double *gy, double *gnx, double *gny) {
    double b0 = M_PI / 2 - span, b1 = M_PI / 2 + span, f0 = shaper_f(k, b0, px, py, nx, ny);
    int n = 400;
    double prev = b0, fp = f0;
    for (int i = 1; i <= n; i++) {
        double b = b0 + (b1 - b0) * i / n, f = shaper_f(k, b, px, py, nx, ny);
        if ((fp <= 0 && f >= 0) || (fp >= 0 && f <= 0)) {
            double lo = prev, hi = b, flo = fp;
            for (int it = 0; it < 80; it++) {
                double mid = 0.5 * (lo + hi), fm = shaper_f(k, mid, px, py, nx, ny);
                if ((flo <= 0 && fm <= 0) || (flo >= 0 && fm >= 0)) lo = mid, flo = fm;
                else hi = mid;
            }
            double wnx, wny;
            shaper_pose(k, 0.5 * (lo + hi), px, py, nx, ny, gx, gy, &wnx, &wny);
            *gnx = -wnx, *gny = -wny; /* the cutter's outward normal points into the ring's material */
            return true;
        }
        prev = b, fp = f;
    }
    return false;
}

bool gear_build_internal(const GearSpec *ring, const GearSpec *cutter, int samples, GearGeometry *g, char *err, size_t errlen) {
    memset(g, 0, sizeof *g);
    g->spec = *ring;
    g->internal = true;
    const GearSpec *s = &g->spec;
    if (s->teeth < 12 || s->module_m <= 0 || s->pressure_deg <= 0 || s->pressure_deg >= 45 || s->face_width_m <= 0) {
        snprintf(err, errlen, "ring: teeth >= 12, a module, a pressure angle in (0, 45) degrees and a face width are needed");
        return false;
    }
    if (fabs(cutter->module_m - s->module_m) > 1e-12 * s->module_m || fabs(cutter->pressure_deg - s->pressure_deg) > 1e-9) {
        snprintf(err, errlen, "ring: the shaper cutter needs the ring's module and pressure angle");
        return false;
    }
    if (cutter->teeth < 8 || s->teeth - cutter->teeth < 8) {
        snprintf(err, errlen, "ring: the shaper needs at least 8 teeth and 8 fewer than the ring (%d)", s->teeth);
        return false;
    }
    if (samples < 40) samples = 40;
    double m = s->module_m, z = s->teeth, zc = cutter->teeth, beta, mt;
    helix_of(s, &beta, &g->alpha, &mt);
    g->helix = beta;
    g->r = mt * z / 2;
    g->r_b = g->r * cos(g->alpha);
    g->twist = tan(beta) / g->r;
    g->beta_b = atan(fabs(tan(beta)) * cos(g->alpha));
    /* the cutter: an external gear (a helical shaper of the ring's helix, whose transverse section cuts it); the ring's
     * thinning is the cutter's thickening, and the shifts set the cutting centre distance
     * (inv alpha_c = inv alpha_t + 2 tan alpha_n (x_ring - x_cutter) / (z_ring - z_cutter)) */
    GearSpec cs = *cutter;
    cs.thinning_m = cutter->thinning_m - s->thinning_m;
    cs.tip_relief_m = 0;
    cs.face_width_m = s->face_width_m;
    cs.helix_deg = s->helix_deg;
    if (cs.bore_radius_m <= 0) cs.bore_radius_m = 0.25 * cs.module_m / cos(beta) * cs.teeth;
    GearGeometry cg;
    if (!gear_build(&cs, samples, &cg, err, errlen)) {
        snprintf(err + strlen(err), errlen - strlen(err), " (the ring's shaper cutter)");
        return false;
    }
    double target = gear_inv(g->alpha) + 2 * tan(s->pressure_deg * M_PI / 180) * (s->shift - cs.shift) / (z - zc), lo = 0.01, hi = 1.5;
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (gear_inv(mid) < target) lo = mid;
        else hi = mid;
    }
    double ac = 0.5 * (lo + hi);
    Shaper k;
    k.a = mt * (z - zc) / 2 * cos(g->alpha) / cos(ac);
    k.rw = k.a * z / (z - zc), k.rcw = k.a * zc / (z - zc);
    g->r_f = k.a + cg.r_a;
    g->r_a = s->tip_radius_m > 0 ? s->tip_radius_m : g->r - (s->addendum - s->shift) * m;
    if (g->r_a <= g->r_b * 1.0001) {
        gear_free(&cg);
        snprintf(err, errlen, "ring: its tip (minor) radius %.4g m must lie outside the base circle %.4g m", g->r_a, g->r_b);
        return false;
    }
    if (s->bore_radius_m <= g->r_f + 1.5 * m) {
        gear_free(&cg);
        snprintf(err, errlen, "ring: its outer radius (bore_radius_m) must be at least 1.5 modules outside the root circle (%.4g m)", g->r_f);
        return false;
    }
    /* the cutter's tooth, right half, in its own frame: the tip land from the centre line to the corner, the corner
     * (its normal sweeping from the tip land's to the flank's), then the flank from the corner down */
    const GearFlank *cf = &cg.right;
    const GearPoint *corner = &cf->p[cf->n - 1];
    double rc_tip = hypot(corner->x, corner->y), th_c = atan2(corner->x, corner->y); /* polar angle from +y */
    Buf b = {0};
    bool ok = true;
    double span = 1.5 * 2 * M_PI / z, gx, gy, gnx, gny;
    int nland = samples / 10 + 4, ncorner = samples / 2;
    for (int i = 0; i < nland && ok; i++) { /* tip land: the ring's root circle */
        double th = th_c * i / nland, px = rc_tip * sin(th), py = rc_tip * cos(th);
        if (shaper_cut(&k, span, px, py, sin(th), cos(th), &gx, &gy, &gnx, &gny)) ok = push(&b, gx, gy, gnx, gny, GEAR_ROOT, -1);
    }
    double a_land = atan2(cos(th_c), sin(th_c)), a_flank = atan2(corner->ny, corner->nx); /* the land's normal is (sin th, cos th) */
    while (a_flank > a_land) a_flank -= 2 * M_PI;
    for (int i = 0; i < ncorner && ok; i++) { /* the corner: the fillet */
        double a = a_land + (a_flank - a_land) * i / ncorner;
        if (shaper_cut(&k, span, corner->x, corner->y, cos(a), sin(a), &gx, &gy, &gnx, &gny)) ok = push(&b, gx, gy, gnx, gny, GEAR_FILLET, -1);
    }
    int form = b.n;
    for (int i = cf->n - 1; i >= cf->form && ok; i--) { /* the flank, from the corner down: the ring's involute */
        const GearPoint *p = &cf->p[i];
        if (!shaper_cut(&k, span, p->x, p->y, p->nx, p->ny, &gx, &gy, &gnx, &gny)) continue;
        double R = hypot(gx, gy);
        if (R < g->r_a) { /* past the ring's minor circle: end exactly on it */
            if (b.n > form) {
                GearPoint *q = &b.p[b.n - 1];
                double Rq = hypot(q->x, q->y), w = (Rq - g->r_a) / (Rq - R);
                ok = push(&b, q->x + w * (gx - q->x), q->y + w * (gy - q->y), q->nx + w * (gnx - q->nx), q->ny + w * (gny - q->ny), GEAR_INVOLUTE, 0);
            }
            break;
        }
        ok = push(&b, gx, gy, gnx, gny, GEAR_INVOLUTE, 0);
    }
    gear_free(&cg);
    if (!ok || b.n - form < 4) {
        free(b.p);
        snprintf(err, errlen, ok ? "ring: the cutter did not generate the flank (teeth or shifts out of range)" : "ring: out of memory");
        return false;
    }
    /* along a side the radius falls from the root to the tip; a rise means the cutter's corner cuts into the flank */
    for (int i = 1; i < b.n; i++)
        if (hypot(b.p[i].x, b.p[i].y) > hypot(b.p[i - 1].x, b.p[i - 1].y) + 1e-12 * g->r) {
            free(b.p);
            snprintf(err, errlen, "ring: the shaper's corner cuts into the involute (z %d with a %d-tooth cutter); change the cutter", s->teeth, cutter->teeth);
            return false;
        }
    /* the space centred on +y was cut; its right edge is the left side of the tooth on its right: turn by +pi/z to
     * centre that tooth on +y and mirror to describe its right side */
    double rot = M_PI / z, c = cos(rot), sn = sin(rot);
    for (int i = 0; i < b.n; i++) {
        GearPoint *p = &b.p[i];
        double x = p->x * c - p->y * sn, y = p->x * sn + p->y * c;
        double nx = p->nx * c - p->ny * sn, ny = p->nx * sn + p->ny * c;
        p->x = -x, p->y = y, p->nx = -nx, p->ny = ny;
        double R = hypot(p->x, p->y);
        if (p->kind == GEAR_INVOLUTE) p->roll = sqrt(fmax(0, R * R - g->r_b * g->r_b));
    }
    g->right.p = b.p, g->right.n = b.n, g->right.form = form;
    g->right.r_form = hypot(b.p[form].x, b.p[form].y);
    GearPoint *tip = &b.p[b.n - 1];
    g->tip_thickness_m = 2 * hypot(tip->x, tip->y) * atan2(tip->x, tip->y);
    g->tooth_thickness_ref_m = mt * (M_PI / 2 - 2 * s->shift * tan(s->pressure_deg * M_PI / 180)) - 2 * s->thinning_m / (cos(s->pressure_deg * M_PI / 180) * cos(beta));
    if (g->tip_thickness_m <= 0) {
        gear_free(g);
        snprintf(err, errlen, "ring: the tooth comes to a point before its minor circle");
        return false;
    }
    return true;
}

void gear_free(GearGeometry *g) {
    free(g->right.p);
    g->right.p = NULL, g->right.n = 0;
}

double gear_working_pressure_angle(const GearSpec *a, const GearSpec *b) {
    double beta, alpha, mt;
    helix_of(a, &beta, &alpha, &mt); /* transverse */
    double target = gear_inv(alpha) + 2 * tan(a->pressure_deg * M_PI / 180) * (a->shift + b->shift) / (a->teeth + b->teeth);
    double lo = 0.01, hi = 1.5; /* inv is increasing */
    for (int i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (gear_inv(mid) < target) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

double gear_centre_distance(const GearSpec *a, const GearSpec *b) {
    double beta, alpha, mt, aw = gear_working_pressure_angle(a, b);
    helix_of(a, &beta, &alpha, &mt);
    return mt * (a->teeth + b->teeth) / 2 * cos(alpha) / cos(aw);
}
