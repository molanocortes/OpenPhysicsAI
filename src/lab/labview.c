/* labview.c - pictures of any physics-lab result (labio.h) on the software renderer: the drawing behind
 * tools/labfilm.c and the app's lab view. See labview.h for the two ways of drawing. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/json.h"
#include "labview.h"
#include "style.h"




void labview_cmap_rgb(int cmap, double t, float rgb[3]) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (cmap == 100) {
        rgb[0] = rgb[1] = rgb[2] = (float)t;
    } else if (cmap == 101) { /* ink: black on white, for schlieren */
        rgb[0] = rgb[1] = rgb[2] = (float)(1.0 - t);
    } else if (cmap == 102) { /* ember: metal grey at zero, then yellow, orange, red, dark red */
        static const float K[5][3] = {{0.80f, 0.81f, 0.83f}, {0.98f, 0.86f, 0.35f}, {0.96f, 0.55f, 0.16f}, {0.80f, 0.18f, 0.12f}, {0.40f, 0.04f, 0.08f}};
        double u = t * 4;
        int i = (int)u;
        if (i > 3) i = 3;
        double f = u - i;
        for (int k = 0; k < 3; k++) rgb[k] = (float)((1 - f) * K[i][k] + f * K[i + 1][k]);
    } else if (cmap == 103) { /* glass: bottle green where intact, a pale crack white where broken */
        static const float K[3][3] = {{0.36f, 0.58f, 0.46f}, {0.72f, 0.86f, 0.78f}, {0.98f, 0.99f, 0.96f}};
        double u = t * 2;
        int i = (int)u;
        if (i > 1) i = 1;
        double f = u - i;
        for (int k = 0; k < 3; k++) rgb[k] = (float)((1 - f) * K[i][k] + f * K[i + 1][k]);
    } else if (cmap == 104) { /* fire: soot black, deep red, orange, yellow, the white of the hottest flame */
        static const float K[5][3] = {{0.05f, 0.03f, 0.03f}, {0.55f, 0.06f, 0.02f}, {0.95f, 0.38f, 0.04f}, {1.00f, 0.80f, 0.25f}, {1.00f, 0.97f, 0.85f}};
        double u = t * 4;
        int i = (int)u;
        if (i > 3) i = 3;
        double f = u - i;
        for (int k = 0; k < 3; k++) rgb[k] = (float)((1 - f) * K[i][k] + f * K[i + 1][k]);
    } else {
        sw_colormap((SwColormap)cmap, t, rgb);
    }
}

static double transform(const LabViewOpts *o, double v) {
    if (o->absval) v = fabs(v);
    if (o->logscale) v = v > 0 ? log10(v) : -30;
    return v;
}

/* ---- plan view of 2D blocks ---------------------------------------------------------------------------------- */

typedef struct Plan {
    const LabPart *part;
    const float *val;     /* the field, all blocks' cells in order */
    const float *solid;   /* optional */
    size_t *first;        /* first cell of each block */
    double x0, y0, x1, y1; /* world bounds of the blocks */
    /* lookup grid: for each bin, the blocks overlapping it, finest first */
    int gx, gy;
    int *bin_start, *bin_list;
} Plan;

static int cmp_level_desc(const void *a, const void *b, void *ctx) { return 0; }

static void plan_free(Plan *p) {
    free(p->first);
    free(p->bin_start);
    free(p->bin_list);
}

static bool plan_build(Plan *p, const LabPart *part) {
    memset(p, 0, sizeof *p);
    p->part = part;
    int nb = part->nblocks;
    p->first = malloc((size_t)(nb + 1) * sizeof(size_t));
    if (!p->first) return false;
    p->first[0] = 0;
    p->x0 = p->y0 = INFINITY;
    p->x1 = p->y1 = -INFINITY;
    for (int b = 0; b < nb; b++) {
        const LabBlock *bl = &part->blocks[b];
        p->first[b + 1] = p->first[b] + lab_block_cells(bl);
        p->x0 = fmin(p->x0, bl->origin[0]);
        p->y0 = fmin(p->y0, bl->origin[1]);
        p->x1 = fmax(p->x1, bl->origin[0] + bl->n[0] * bl->dx[0]);
        p->y1 = fmax(p->y1, bl->origin[1] + bl->n[1] * bl->dx[1]);
    }
    p->gx = p->gy = 128;
    int ng = p->gx * p->gy;
    int *count = calloc((size_t)ng + 1, sizeof(int));
    if (!count) return false;
    double bw = (p->x1 - p->x0) / p->gx, bh = (p->y1 - p->y0) / p->gy;
    /* two passes: count, then fill; blocks are visited finest level first so each bin's list is ordered */
    int maxlev = 0;
    for (int b = 0; b < nb; b++) maxlev = part->blocks[b].level > maxlev ? part->blocks[b].level : maxlev;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            p->bin_start = malloc(((size_t)ng + 1) * sizeof(int));
            if (!p->bin_start) {
                free(count);
                return false;
            }
            p->bin_start[0] = 0;
            for (int i = 0; i < ng; i++) p->bin_start[i + 1] = p->bin_start[i] + count[i];
            p->bin_list = malloc((size_t)(p->bin_start[ng] ? p->bin_start[ng] : 1) * sizeof(int));
            if (!p->bin_list) {
                free(count);
                return false;
            }
            memset(count, 0, (size_t)ng * sizeof(int));
        }
        for (int lev = maxlev; lev >= 0; lev--)
            for (int b = 0; b < nb; b++) {
                const LabBlock *bl = &part->blocks[b];
                if (bl->level != lev) continue;
                int i0 = (int)floor((bl->origin[0] - p->x0) / bw), i1 = (int)floor((bl->origin[0] + bl->n[0] * bl->dx[0] - p->x0) / bw - 1e-9);
                int j0 = (int)floor((bl->origin[1] - p->y0) / bh), j1 = (int)floor((bl->origin[1] + bl->n[1] * bl->dx[1] - p->y0) / bh - 1e-9);
                if (i0 < 0) i0 = 0;
                if (j0 < 0) j0 = 0;
                if (i1 >= p->gx) i1 = p->gx - 1;
                if (j1 >= p->gy) j1 = p->gy - 1;
                for (int j = j0; j <= j1; j++)
                    for (int i = i0; i <= i1; i++) {
                        int g = j * p->gx + i;
                        if (pass == 1) p->bin_list[p->bin_start[g] + count[g]] = b;
                        count[g]++;
                    }
            }
    }
    free(count);
    (void)cmp_level_desc;
    return true;
}

/* the finest block containing (x, y) and the continuous cell coordinates there; -1 outside every block */
static int plan_locate(const Plan *p, double x, double y, double *fx, double *fy) {
    int i = (int)floor((x - p->x0) / (p->x1 - p->x0) * p->gx), j = (int)floor((y - p->y0) / (p->y1 - p->y0) * p->gy);
    if (i < 0 || j < 0 || i >= p->gx || j >= p->gy) return -1;
    int g = j * p->gx + i;
    for (int k = p->bin_start[g]; k < p->bin_start[g + 1]; k++) {
        const LabBlock *bl = &p->part->blocks[p->bin_list[k]];
        double u = (x - bl->origin[0]) / bl->dx[0], v = (y - bl->origin[1]) / bl->dx[1];
        if (u >= 0 && v >= 0 && u < bl->n[0] && v < bl->n[1]) {
            *fx = u, *fy = v;
            return p->bin_list[k];
        }
    }
    return -1;
}

static double plan_value(const Plan *p, const float *f, int b, double u, double v, bool nearest) {
    const LabBlock *bl = &p->part->blocks[b];
    const float *c = f + p->first[b];
    int nx = bl->n[0], ny = bl->n[1];
    if (nearest) {
        int i = (int)u, j = (int)v;
        return c[(size_t)j * nx + i];
    }
    double a = u - 0.5, bb = v - 0.5;
    int i0 = (int)floor(a), j0 = (int)floor(bb);
    double tx = a - i0, ty = bb - j0;
    int i1 = i0 + 1, j1 = j0 + 1;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > nx - 1) i1 = nx - 1;
    if (j1 > ny - 1) j1 = ny - 1;
    if (i0 > nx - 1) i0 = nx - 1;
    if (j0 > ny - 1) j0 = ny - 1;
    double v00 = c[(size_t)j0 * nx + i0], v10 = c[(size_t)j0 * nx + i1], v01 = c[(size_t)j1 * nx + i0], v11 = c[(size_t)j1 * nx + i1];
    return (1 - ty) * ((1 - tx) * v00 + tx * v10) + ty * ((1 - tx) * v01 + tx * v11);
}

/* |grad f| per cell by central differences inside each block (one-sided at block edges) */
static float *gradient_magnitude(const LabPart *part, const float *f) {
    size_t n = lab_part_ncells(part), at = 0;
    float *g = malloc((n ? n : 1) * sizeof(float));
    if (!g) return NULL;
    for (int b = 0; b < part->nblocks; b++) {
        const LabBlock *bl = &part->blocks[b];
        int nx = bl->n[0], ny = bl->n[1];
        const float *c = f + at;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                int il = i > 0 ? i - 1 : i, ir = i < nx - 1 ? i + 1 : i, jl = j > 0 ? j - 1 : j, jr = j < ny - 1 ? j + 1 : j;
                double gx = (ir > il) ? (c[(size_t)j * nx + ir] - c[(size_t)j * nx + il]) / ((ir - il) * bl->dx[0]) : 0;
                double gy = (jr > jl) ? (c[(size_t)jr * nx + i] - c[(size_t)jl * nx + i]) / ((jr - jl) * bl->dx[1]) : 0;
                g[at + (size_t)j * nx + i] = (float)sqrt(gx * gx + gy * gy);
            }
        at += lab_block_cells(bl);
    }
    return g;
}

static const unsigned char LEVEL_TINT[6][3] = {{255, 255, 255}, {120, 200, 255}, {120, 255, 160}, {255, 220, 90}, {255, 130, 90}, {230, 110, 255}};

/* what a pixel of a 2D field shows: the field through the colour map, bodies, contour lines, the mesh; shared by the
 * flat view and the world view so that the two differ only in where the camera stands */
typedef struct PixCtx {
    const LabViewOpts *o;
    const LabPart *part;
    Plan *p;
    const LabField *sol, *lf;
    const float *vals;
    const float *grad;
    double gmax, lstep, lmin, lo, hi;
} PixCtx;

/* -1 outside the blocks, 1 a body (rgb its solid colour), 0 the field; scale is pixels per metre at this pixel */
static int pix_color(const PixCtx *C, double wx, double wy, double scale, int x, int y, float rgb[3]) {
    const LabViewOpts *o = C->o;
    double qy = o->mirror ? fabs(wy) : wy, u, v;
    int b = plan_locate(C->p, wx, qy, &u, &v);
    if (b < 0) return -1;
    const LabBlock *blk = &C->part->blocks[b];
    if (o->mesh_below && wy < 0) {
        bool solid_here = C->sol && plan_value(C->p, C->sol->data, b, u, v, true) > 0.5f;
        static const float LV[6][3] = {{0.96f, 0.97f, 0.98f}, {0.80f, 0.90f, 0.99f}, {0.78f, 0.95f, 0.80f},
                                       {0.99f, 0.93f, 0.70f}, {0.99f, 0.78f, 0.66f}, {0.93f, 0.74f, 0.96f}};
        int lv = blk->level < 6 ? blk->level : 5;
        for (int k = 0; k < 3; k++) rgb[k] = solid_here ? STYLE_SOLID[k] : LV[lv][k];
        if (solid_here) return 1;
        double cpx = blk->dx[0] * scale;
        double du = fabs(u - floor(u + 0.5)) * cpx, dv = fabs(v - floor(v + 0.5)) * cpx;
        double bu = fabs(u / blk->n[0] - floor(u / blk->n[0] + 0.5)) * cpx * blk->n[0];
        double bv = fabs(v / blk->n[1] - floor(v / blk->n[1] + 0.5)) * cpx * blk->n[1];
        float ink = 1.0f;
        if (cpx >= 2.5 && (du < 0.55 || dv < 0.55)) ink = cpx >= 5 ? 0.45f : 0.62f;
        else if (cpx < 2.5) ink = 0.80f;
        if (bu < 0.7 || bv < 0.7) ink = 0.30f;
        if (cpx < 2.5 && !(bu < 0.7 || bv < 0.7)) {
            double dens = fmin(1.0, 2.5 / cpx);
            int hx = (x * 73856093) ^ (y * 19349663);
            if ((hx & 1023) < (int)(1023 * 0.5 * dens)) ink = 0.55f;
        }
        for (int k = 0; k < 3; k++) rgb[k] *= ink;
        return 0;
    }
    if (C->sol && plan_value(C->p, C->sol->data, b, u, v, true) > 0.5f) {
        for (int k = 0; k < 3; k++) rgb[k] = STYLE_SOLID[k];
        return 1;
    }
    if (C->vals) {
        double sv = plan_value(C->p, C->vals, b, u, v, o->nearest), t;
        if (C->grad) t = 1.0 - exp(-12.0 * sv / C->gmax);
        else t = (transform(o, sv) - C->lo) / (C->hi - C->lo);
        labview_cmap_rgb(o->cmap, t, rgb);
    } else {
        rgb[0] = rgb[1] = rgb[2] = 0.5f;
    }
    if (o->levels) {
        int lv = blk->level < 6 ? blk->level : 5;
        for (int k = 0; k < 3; k++) rgb[k] = (float)(0.8 * rgb[k] + 0.2 * LEVEL_TINT[lv][k] / 255.0);
    }
    if (C->lf && C->lstep > 0) { /* contour lines: distance to the nearest level in pixels, from the pixel-scale gradient */
        double v0 = plan_value(C->p, C->lf->data, b, u, v, false), u1, v1, u2, v2;
        int b1 = plan_locate(C->p, wx + 1 / scale, qy, &u1, &v1), b2 = plan_locate(C->p, wx, qy + 1 / scale, &u2, &v2);
        if (b1 >= 0 && b2 >= 0) {
            double gx = plan_value(C->p, C->lf->data, b1, u1, v1, false) - v0, gy = plan_value(C->p, C->lf->data, b2, u2, v2, false) - v0;
            double g = sqrt(gx * gx + gy * gy), f = (v0 - C->lmin) / C->lstep;
            if (g > 0) {
                double d = fabs(f - floor(f + 0.5)) * C->lstep / g;
                if (d < 0.9) { /* dark lines over a bright field, pale ones over a dark field: always legible */
                    double a = 0.7 * (1 - d / 0.9), lum = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
                    static const float PALE[3] = {0.78f, 0.84f, 0.92f};
                    for (int k = 0; k < 3; k++) rgb[k] = (float)(lum < 0.3 ? (1 - 0.8 * a) * rgb[k] + 0.8 * a * PALE[k] : (1 - a) * rgb[k]);
                }
            }
        }
    }
    if (o->mesh) {
        double cpx = blk->dx[0] * scale, cpy = blk->dx[1] * scale;
        double du = fabs(u - floor(u + 0.5)) * cpx, dv = fabs(v - floor(v + 0.5)) * cpy;
        if ((cpx >= 3 && du < 0.6) || (cpy >= 3 && dv < 0.6)) {
            double a = cpx >= 6 ? 0.45 : 0.3;
            for (int k = 0; k < 3; k++) rgb[k] = (float)((1 - a) * rgb[k]);
        }
    }
    return 0;
}

static void pix_ctx_init(PixCtx *C, const LabViewOpts *o, const LabPart *part, Plan *p, double lo, double hi, float **grad_out) {
    memset(C, 0, sizeof *C);
    C->o = o, C->part = part, C->p = p, C->lo = lo, C->hi = hi;
    const LabField *fld = lab_find_field(part, o->field);
    C->sol = o->solid ? lab_find_field(part, o->solid) : NULL;
    C->vals = fld ? fld->data : NULL;
    *grad_out = NULL;
    if (C->vals && o->schlieren) {
        *grad_out = gradient_magnitude(part, C->vals);
        C->vals = C->grad = *grad_out;
        size_t n = lab_part_ncells(part);
        for (size_t i = 0; i < n; i++) C->gmax = fmax(C->gmax, C->grad[i]);
        if (C->gmax <= 0) C->gmax = 1;
    }
    C->lf = o->lines ? lab_find_field(part, o->lines) : NULL;
    if (C->lf) {
        size_t n = lab_part_ncells(part);
        double a = INFINITY, bm = -INFINITY;
        for (size_t i = 0; i < n; i++)
            if (isfinite(C->lf->data[i])) a = fmin(a, C->lf->data[i]), bm = fmax(bm, C->lf->data[i]);
        C->lmin = a, C->lstep = (bm - a) / (o->nlines > 0 ? o->nlines : 24);
    }
}

/* the flat view: the field fills the plot area, seen straight from above */
static void draw_plan(SwImage *im, const LabViewOpts *o, const LabFrame *fr, const LabPart *part, double lo, double hi, int px0, int py0,
                      int pw, int ph) {
    Plan p;
    if (!plan_build(&p, part)) return;
    float *grad;
    PixCtx C;
    pix_ctx_init(&C, o, part, &p, lo, hi, &grad);
    double wx0 = p.x0, wx1 = p.x1, wy0 = o->mirror ? -p.y1 : p.y0, wy1 = p.y1;
    double cx = o->center_given ? o->cx : 0.5 * (wx0 + wx1), cy = o->center_given ? o->cy : 0.5 * (wy0 + wy1);
    double sw = (wx1 - wx0) / o->zoom, sh = (wy1 - wy0) / o->zoom;
    double scale = fmin(pw / sw, ph / sh);
    double ox = px0 + 0.5 * (pw - sw * scale), oy = py0 + 0.5 * (ph - sh * scale);
    double left = cx - 0.5 * sw, top = cy + 0.5 * sh;
    for (int y = py0; y < py0 + ph; y++)
        for (int x = px0; x < px0 + pw; x++) {
            double wx = left + (x + 0.5 - ox) / scale, wy = top - (y + 0.5 - oy) / scale;
            float rgb[3];
            if (pix_color(&C, wx, wy, scale, x, y, rgb) < 0) continue;
            unsigned char *px = im->rgb + 3 * ((size_t)y * im->w + x);
            for (int k = 0; k < 3; k++) px[k] = (unsigned char)lround(255.0 * fmin(1.0, rgb[k]));
        }
    free(grad);
    plan_free(&p);
    (void)fr;
}

static inline double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* the lab's floor at height zf: a grid at a round spacing that fades into the room, and a soft shadow of the footprint
 * [bmin, bmax] cast along the key light; writes depth so that everything drawn after stands on it */
static void draw_floor(SwImage *im, const SwCamera *cam, double zf, double cx, double cy, double L, const double *bmin, const double *bmax) {
    if (cam->eye[2] <= zf) return; /* a view from below: no floor between the camera and the scene */
    double lz[3] = {STYLE_LIGHT[0], STYLE_LIGHT[1], STYLE_LIGHT[2]}, ln = sqrt(dot3(lz, lz));
    for (int k = 0; k < 3; k++) lz[k] /= ln;
    double grid = pow(10, floor(log10(L / 4))), tanh_ = tan(0.5 * cam->fov_deg * M_PI / 180) * 2 / im->h;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            double org[3], dir[3];
            sw_ray(cam, x + 0.5, y + 0.5, org, dir);
            if (fabs(dir[2]) < 1e-12) continue;
            double tf = (zf - org[2]) / dir[2];
            if (!(tf > 0)) continue;
            double F[3] = {org[0] + tf * dir[0], org[1] + tf * dir[1], zf};
            double gx = fabs(F[0] / grid - floor(F[0] / grid + 0.5)), gy = fabs(F[1] / grid - floor(F[1] / grid + 0.5));
            double zc = tf * dot3(dir, cam->forward), wpp = zc * tanh_ / grid;
            double line = fmax(0, 1 - fmin(gx, gy) / fmax(wpp, 1e-9)) * 0.9;
            double h = bmax[2] - zf, sx = F[0] - lz[0] / lz[2] * h * 0.5, sy = F[1] - lz[1] / lz[2] * h * 0.5;
            double dx_ = fmax(fmax(bmin[0] - sx, sx - bmax[0]), 0), dy_ = fmax(fmax(bmin[1] - sy, sy - bmax[1]), 0);
            double shadow = 1 - 0.5 * exp(-hypot(dx_, dy_) / (0.04 * L));
            double fade = exp(-hypot(F[0] - cx, F[1] - cy) / (1.2 * L));
            size_t i = (size_t)y * im->w + x;
            unsigned char *px = im->rgb + 3 * i;
            for (int k = 0; k < 3; k++) {
                double c = (STYLE_FLOOR[k] + line * (STYLE_GRID[k] - STYLE_FLOOR[k])) * shadow;
                px[k] = (unsigned char)lround(fade * 255.0 * c + (1 - fade) * px[k]);
            }
            im->depth[i] = zc;
        }
}

/* the world view of a 2D field: a slab in the lab's room, its field on the top face, bodies standing on it as metal
 * (a planar section's bodies extruded, since they run through the depth), a floor with a grid and the slab's shadow;
 * seen by the given camera, with every surface in one depth buffer so points drawn after it sit in the same space */
static void draw_world(SwImage *im, const SwCamera *cam, const LabViewOpts *o, const LabPart *part, double lo, double hi, double L,
                       double Hb, double T) {
    Plan p;
    if (!plan_build(&p, part)) return;
    float *grad;
    PixCtx C;
    pix_ctx_init(&C, o, part, &p, lo, hi, &grad);
    double x0 = p.x0, x1 = p.x1, y0 = o->mirror ? -p.y1 : p.y0, y1 = p.y1;
    double zf = -T - 0.02 * L; /* the floor, a little below the slab */
    double lz[3] = {STYLE_LIGHT[0], STYLE_LIGHT[1], STYLE_LIGHT[2]}, ln = sqrt(dot3(lz, lz));
    for (int k = 0; k < 3; k++) lz[k] /= ln;
    double grid = pow(10, floor(log10(L / 4))); /* grid lines at a round spacing, four to forty across */
    double tanh_ = tan(0.5 * cam->fov_deg * M_PI / 180) * 2 / im->h;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            double org[3], dir[3];
            sw_ray(cam, x + 0.5, y + 0.5, org, dir);
            if (fabs(dir[2]) < 1e-12) continue;
            size_t i = (size_t)y * im->w + x;
            float rgb[3];
            double depth = INFINITY;
            /* the slab's top face */
            double t0 = -org[2] / dir[2];
            if (t0 > 0) {
                double P[3] = {org[0] + t0 * dir[0], org[1] + t0 * dir[1], 0};
                if (P[0] >= x0 && P[0] <= x1 && P[1] >= y0 && P[1] <= y1) {
                    double zc = t0 * dot3(dir, cam->forward), scale = 1 / (zc * tanh_);
                    int r = pix_color(&C, P[0], P[1], scale, x, y, rgb);
                    if (r == 0) {
                        double sh = 0.88 + 0.12 * lz[2];
                        for (int k = 0; k < 3; k++) rgb[k] *= (float)sh;
                        depth = zc;
                    } else if (r == 1 && Hb > 0) { /* over a body: its top cap, if the ray meets it */
                        double t1 = (Hb - org[2]) / dir[2];
                        double Q[3] = {org[0] + t1 * dir[0], org[1] + t1 * dir[1], Hb};
                        float c2[3];
                        if (t1 > 0 && pix_color(&C, Q[0], Q[1], scale, x, y, c2) == 1) {
                            double sh = 0.50 + 0.50 * lz[2];
                            for (int k = 0; k < 3; k++) rgb[k] = (float)(STYLE_SOLID[k] * sh);
                            depth = t1 * dot3(dir, cam->forward);
                        }
                    } else if (r == 1) {
                        depth = zc;
                    }
                }
            }
            /* the floor, where the slab does not cover it */
            if (!isfinite(depth)) {
                double tf = (zf - org[2]) / dir[2];
                if (tf > 0) {
                    double F[3] = {org[0] + tf * dir[0], org[1] + tf * dir[1], zf};
                    double gx = fabs(F[0] / grid - floor(F[0] / grid + 0.5)), gy = fabs(F[1] / grid - floor(F[1] / grid + 0.5));
                    double zc = tf * dot3(dir, cam->forward), wpp = zc * tanh_ / grid;
                    double line = fmax(0, 1 - fmin(gx, gy) / fmax(wpp, 1e-9)) * 0.9;
                    /* the slab's soft shadow, cast along the light */
                    double sx = F[0] - lz[0] / lz[2] * (0 - zf), sy = F[1] - lz[1] / lz[2] * (0 - zf);
                    double dx_ = fmax(fmax(x0 - sx, sx - x1), 0), dy_ = fmax(fmax(y0 - sy, sy - y1), 0);
                    double shadow = 1 - 0.55 * exp(-hypot(dx_, dy_) / (0.03 * L));
                    double fade = exp(-hypot(F[0] - 0.5 * (x0 + x1), F[1] - 0.5 * (y0 + y1)) / (1.2 * L));
                    for (int k = 0; k < 3; k++) rgb[k] = (float)((STYLE_FLOOR[k] + line * (STYLE_GRID[k] - STYLE_FLOOR[k])) * shadow);
                    unsigned char *px = im->rgb + 3 * i;
                    for (int k = 0; k < 3; k++) /* the floor fades into the room's background with distance */
                        px[k] = (unsigned char)lround(fade * 255.0 * rgb[k] + (1 - fade) * px[k]);
                    im->depth[i] = zc;
                }
                continue;
            }
            unsigned char *px = im->rgb + 3 * i;
            for (int k = 0; k < 3; k++) px[k] = (unsigned char)lround(255.0 * fmin(1.0, rgb[k]));
            im->depth[i] = depth;
        }
    /* the slab's four sides, down to its thickness */
    double cs[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int e = 0; e < 4; e++) {
        const double *a = cs[e], *b = cs[(e + 1) % 4];
        double n[3] = {b[1] - a[1], -(b[0] - a[0]), 0}, nn = hypot(n[0], n[1]);
        n[0] /= nn, n[1] /= nn;
        float sh = (float)(0.45 + 0.55 * fmax(0, dot3(n, lz)));
        float c[3] = {STYLE_SLAB_SIDE[0] * sh, STYLE_SLAB_SIDE[1] * sh, STYLE_SLAB_SIDE[2] * sh};
        double A[3] = {a[0], a[1], 0}, B[3] = {b[0], b[1], 0}, Bd[3] = {b[0], b[1], -T}, Ad[3] = {a[0], a[1], -T};
        sw_triangle(im, cam, A, B, Bd, c, c, c, false, 2), sw_triangle(im, cam, A, Bd, Ad, c, c, c, false, 2);
    }
    /* the walls of the bodies: every edge between a body cell and a field cell, raised to the bodies' height */
    if (Hb > 0 && C.sol) {
        size_t at = 0;
        for (int bi = 0; bi < part->nblocks; bi++) {
            const LabBlock *bl = &part->blocks[bi];
            for (int j = 0; j < bl->n[1]; j++)
                for (int ii = 0; ii < bl->n[0]; ii++) {
                    if (C.sol->data[at + (size_t)j * bl->n[0] + ii] <= 0.5f) continue;
                    double cx = bl->origin[0] + (ii + 0.5) * bl->dx[0], cy = bl->origin[1] + (j + 0.5) * bl->dx[1];
                    static const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (int d = 0; d < 4; d++) {
                        double nx = cx + D[d][0] * bl->dx[0], ny = cy + D[d][1] * bl->dx[1], u, v;
                        int nb = plan_locate(&p, nx, ny, &u, &v);
                        if (nb >= 0 && plan_value(&p, C.sol->data, nb, u, v, true) > 0.5f) continue;
                        double ex = cx + 0.5 * D[d][0] * bl->dx[0], ey = cy + 0.5 * D[d][1] * bl->dx[1];
                        double tx = -D[d][1] * 0.5 * bl->dx[0], ty = D[d][0] * 0.5 * bl->dx[1];
                        double n[3] = {D[d][0], D[d][1], 0};
                        float sh = (float)(0.40 + 0.60 * fmax(0, dot3(n, lz)));
                        float c[3] = {STYLE_SOLID[0] * sh, STYLE_SOLID[1] * sh, STYLE_SOLID[2] * sh};
                        for (int m = 0; m < (o->mirror ? 2 : 1); m++) {
                            double s = m ? -1 : 1;
                            double A[3] = {ex - tx, s * (ey - ty), 0}, B[3] = {ex + tx, s * (ey + ty), 0};
                            double Bu[3] = {B[0], B[1], Hb}, Au[3] = {A[0], A[1], Hb};
                            sw_triangle(im, cam, A, B, Bu, c, c, c, false, 3), sw_triangle(im, cam, A, Bu, Au, c, c, c, false, 3);
                        }
                    }
                }
            at += lab_block_cells(bl);
        }
    }
    /* an axisymmetric section (drawn mirrored): its bodies revolved about the axis into solids that rise out of the cut
     * plane, the upper half of each, lit like every other solid in the room */
    if (o->mirror && C.sol) {
        const int NX = 480, NPHI = 40;
        double *rx = calloc(NX, sizeof(double));
        double dmin = INFINITY;
        for (int bi = 0; bi < part->nblocks; bi++) dmin = fmin(dmin, part->blocks[bi].dx[1]);
        for (int k = 0; k < NX; k++) { /* the body's radius at each x: the outermost body cell */
            double xx = p.x0 + (k + 0.5) * (p.x1 - p.x0) / NX, u, v;
            for (double r = p.y1 - 0.5 * dmin; r > 0; r -= 0.5 * dmin) {
                int b = plan_locate(&p, xx, r, &u, &v);
                if (b >= 0 && plan_value(&p, C.sol->data, b, u, v, true) > 0.5f) {
                    rx[k] = r;
                    break;
                }
            }
        }
        for (int pass = 0; pass < 3; pass++) { /* smooth the stepped section where the body is continuous */
            double prev = rx[0];
            for (int k = 1; k + 1 < NX; k++) {
                double cur = rx[k];
                if (prev > 0 && cur > 0 && rx[k + 1] > 0) rx[k] = 0.25 * prev + 0.5 * cur + 0.25 * rx[k + 1];
                prev = cur;
            }
        }
        double dxs = (p.x1 - p.x0) / NX;
        for (int k = 0; k + 1 < NX; k++) {
            if (rx[k] <= 0 && rx[k + 1] <= 0) continue;
            double xa = p.x0 + (k + 0.5) * dxs, xb = xa + dxs, ra = rx[k], rb = rx[k + 1];
            double slope = (rb - ra) / dxs; /* the surface normal in the meridian: (-slope, 1) normalised */
            double nm = sqrt(1 + slope * slope);
            for (int m = 0; m < NPHI; m++) {
                double f0 = M_PI * m / NPHI, f1 = M_PI * (m + 1) / NPHI;
                double P00[3] = {xa, ra * cos(f0), ra * sin(f0)}, P01[3] = {xa, ra * cos(f1), ra * sin(f1)};
                double P10[3] = {xb, rb * cos(f0), rb * sin(f0)}, P11[3] = {xb, rb * cos(f1), rb * sin(f1)};
                double fm = 0.5 * (f0 + f1), n[3] = {-slope / nm, cos(fm) / nm, sin(fm) / nm};
                float sh = (float)(0.35 + 0.65 * fmax(0, dot3(n, lz)));
                /* a highlight where the surface faces halfway between the light and the camera */
                double hv[3] = {lz[0] - cam->forward[0], lz[1] - cam->forward[1], lz[2] - cam->forward[2]}, hn = sqrt(dot3(hv, hv));
                double spec = pow(fmax(0, dot3(n, hv) / hn), 40) * 0.5;
                float c[3];
                for (int q = 0; q < 3; q++) c[q] = (float)fmin(1.0, STYLE_SOLID[q] * sh + spec);
                sw_triangle(im, cam, P00, P10, P11, c, c, c, false, 4), sw_triangle(im, cam, P00, P11, P01, c, c, c, false, 4);
            }
        }
        free(rx);
    }
    free(grad);
    plan_free(&p);
}

/* ---- 3D view ------------------------------------------------------------------------------------------------- */

static void splat(SwImage *im, const SwCamera *cam, const double *p, double r, const float rgb[3]) {
    double px, py, d;
    if (!sw_project(cam, p, &px, &py, &d) || d <= 0) return;
    int x0 = (int)floor(px - r), x1 = (int)ceil(px + r), y0 = (int)floor(py - r), y1 = (int)ceil(py + r);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || y < 0 || x >= im->w || y >= im->h) continue;
            double dx = x + 0.5 - px, dy = y + 0.5 - py, q = (dx * dx + dy * dy) / (r * r);
            if (q > 1) continue;
            size_t i = (size_t)y * im->w + x;
            double z = d - 1e-6 * d * sqrt(1 - q); /* a sphere's front */
            if (z >= im->depth[i]) continue;
            im->depth[i] = z;
            double shade = 0.55 + 0.45 * sqrt(1 - q);
            for (int k = 0; k < 3; k++) im->rgb[3 * i + k] = (unsigned char)lround(255.0 * fmin(1.0, rgb[k] * shade));
        }
}

static void part_bounds(const LabPart *pt, double bmin[3], double bmax[3]) {
    if (pt->kind == LAB_BLOCKS) {
        for (int b = 0; b < pt->nblocks; b++) {
            const LabBlock *bl = &pt->blocks[b];
            double lo[3] = {bl->origin[0], bl->origin[1], bl->origin[2]}, hi[3];
            int ext[3] = {bl->n[0], bl->n[1], bl->n[2]};
            if (bl->n[2] <= 1) {
                /* a 2D block: map its two axes onto its plane */
                ext[2] = 0;
                if (bl->plane == LAB_PLANE_XZ) ext[2] = bl->n[1], ext[1] = 0;
                if (bl->plane == LAB_PLANE_YZ) ext[2] = bl->n[1], ext[1] = bl->n[0], ext[0] = 0;
            }
            double d[3] = {bl->dx[0], bl->dx[1], bl->dx[2]};
            if (bl->n[2] <= 1 && bl->plane == LAB_PLANE_XZ) d[2] = bl->dx[1];
            if (bl->n[2] <= 1 && bl->plane == LAB_PLANE_YZ) d[1] = bl->dx[0], d[2] = bl->dx[1];
            for (int k = 0; k < 3; k++) hi[k] = lo[k] + ext[k] * d[k];
            for (int k = 0; k < 3; k++) bmin[k] = fmin(bmin[k], lo[k]), bmax[k] = fmax(bmax[k], hi[k]);
        }
    } else {
        for (int i = 0; i < pt->npoints; i++)
            for (int k = 0; k < 3; k++) bmin[k] = fmin(bmin[k], pt->xyz[3 * i + k]), bmax[k] = fmax(bmax[k], pt->xyz[3 * i + k]);
    }
}

/* the world point of a 2D block's cell corner (i, j) */
static void block_corner(const LabBlock *bl, double i, double j, double out[3]) {
    out[0] = bl->origin[0], out[1] = bl->origin[1], out[2] = bl->origin[2];
    if (bl->plane == LAB_PLANE_XY) out[0] += i * bl->dx[0], out[1] += j * bl->dx[1];
    else if (bl->plane == LAB_PLANE_XZ) out[0] += i * bl->dx[0], out[2] += j * bl->dx[1];
    else out[1] += i * bl->dx[0], out[2] += j * bl->dx[1];
}

typedef struct Face {
    int n[4];
    int key[4];
    int cell;
} Face;

static int cmp_face(const void *a, const void *b) {
    const Face *x = a, *y = b;
    for (int k = 0; k < 4; k++)
        if (x->key[k] != y->key[k]) return x->key[k] < y->key[k] ? -1 : 1;
    return 0;
}

static void sort4(int *k) {
    for (int i = 1; i < 4; i++)
        for (int j = i; j > 0 && k[j - 1] > k[j]; j--) {
            int t = k[j];
            k[j] = k[j - 1];
            k[j - 1] = t;
        }
}

static void draw_3d_part(SwImage *im, const SwCamera *cam, const LabViewOpts *o, const LabPart *pt, double lo, double hi, double pix_per_m) {
    const LabField *fld = lab_find_field(pt, o->field);
    static const float NEUTRAL[3] = {0.80f, 0.82f, 0.86f};
    static const unsigned char EDGE[3] = {25, 28, 36};
    if (pt->kind == LAB_BLOCKS) {
        size_t at = 0;
        for (int b = 0; b < pt->nblocks; b++) {
            const LabBlock *bl = &pt->blocks[b];
            if (bl->n[2] > 1) {
                at += lab_block_cells(bl);
                continue; /* 3D blocks are stored for probing; slices are what is drawn */
            }
            for (int j = 0; j < bl->n[1]; j++)
                for (int i = 0; i < bl->n[0]; i++) {
                    double a[3], bb[3], c[3], d[3];
                    block_corner(bl, i, j, a);
                    block_corner(bl, i + 1, j, bb);
                    block_corner(bl, i + 1, j + 1, c);
                    block_corner(bl, i, j + 1, d);
                    double s = fld ? transform(o, fld->data[at + (size_t)j * bl->n[0] + i]) : lo;
                    sw_triangle_scalar(im, cam, a, bb, c, s, s, s, lo, hi, (SwColormap)(o->cmap < 100 ? o->cmap : 0), false, 1);
                    sw_triangle_scalar(im, cam, a, c, d, s, s, s, lo, hi, (SwColormap)(o->cmap < 100 ? o->cmap : 0), false, 1);
                }
            at += lab_block_cells(bl);
        }
    } else if (pt->kind == LAB_POINTS) {
        LabViewOpts op = *o; /* particles of a solid wear the solid palette, like a solid's surface */
        if (op.solid_points && op.cmap == SW_CMAP_LAB) op.cmap = SW_CMAP_LAB_SOLID;
        o = &op;
        const LabField *rad = o->true_size ? lab_find_field(pt, "radius") : NULL;
        for (int i = 0; i < pt->npoints; i++) {
            float rgb[3] = {NEUTRAL[0], NEUTRAL[1], NEUTRAL[2]};
            if (fld && (size_t)i < fld->count) labview_cmap_rgb(o->cmap, (transform(o, fld->data[i]) - lo) / (hi - lo), rgb);
            double r = o->radius;
            if (o->point_radius_m > 0) { /* a particle of a liquid at its own size on screen */
                double px, py, d, px2, py2, d2, q[3] = {pt->xyz[3 * i] + cam->right[0] * o->point_radius_m, pt->xyz[3 * i + 1] + cam->right[1] * o->point_radius_m,
                                                       pt->xyz[3 * i + 2] + cam->right[2] * o->point_radius_m};
                if (sw_project(cam, &pt->xyz[3 * i], &px, &py, &d) && sw_project(cam, q, &px2, &py2, &d2)) r = fmax(r, hypot(px2 - px, py2 - py));
            }
            if (rad && (size_t)i < rad->count) { /* the body's own radius on screen, never smaller than the default */
                double px, py, d, px2, py2, d2, q[3] = {pt->xyz[3 * i] + cam->right[0] * rad->data[i], pt->xyz[3 * i + 1] + cam->right[1] * rad->data[i],
                                                       pt->xyz[3 * i + 2] + cam->right[2] * rad->data[i]};
                if (sw_project(cam, &pt->xyz[3 * i], &px, &py, &d) && sw_project(cam, q, &px2, &py2, &d2)) r = fmax(r, hypot(px2 - px, py2 - py));
            }
            splat(im, cam, &pt->xyz[3 * i], r, rgb);
        }
    } else if (pt->kind == LAB_CELLS) {
        /* a solid's surface: the sequential palette starts at the material's own metal */
        LabViewOpts os = *o;
        if (os.cmap == SW_CMAP_LAB) os.cmap = SW_CMAP_LAB_SOLID;
        o = &os;
        const double *X = pt->xyz;
        bool node = fld && fld->location == LAB_AT_NODE && fld->count == (size_t)pt->npoints;
        bool cellf = fld && fld->location == LAB_AT_CELL && fld->count == (size_t)pt->ncells;
        if (pt->cell_type == LAB_LINE) {
            for (int c = 0; c < pt->ncells; c++) {
                const int *e = &pt->conn[2 * c];
                float rgb[3] = {NEUTRAL[0], NEUTRAL[1], NEUTRAL[2]};
                if (cellf) labview_cmap_rgb(o->cmap, (transform(o, fld->data[c]) - lo) / (hi - lo), rgb);
                else if (node) labview_cmap_rgb(o->cmap, (transform(o, fld->data[e[0]]) - lo) / (hi - lo), rgb);
                unsigned char col[3] = {(unsigned char)(255 * rgb[0]), (unsigned char)(255 * rgb[1]), (unsigned char)(255 * rgb[2])};
                sw_line(im, cam, &X[3 * e[0]], &X[3 * e[1]], col, 0, (int)fmax(1, o->radius * 0.5));
            }
            return;
        }
        /* the faces to draw: triangles and quads as given; for hexahedra the faces used by exactly one cell */
        int nf = 0;
        Face *faces = NULL;
        if (pt->cell_type == LAB_HEX) {
            static const int HF[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
            faces = malloc((size_t)pt->ncells * 6 * sizeof *faces);
            if (!faces) return;
            for (int c = 0; c < pt->ncells; c++)
                for (int f = 0; f < 6; f++) {
                    Face *F = &faces[nf++];
                    for (int k = 0; k < 4; k++) F->n[k] = F->key[k] = pt->conn[8 * c + HF[f][k]];
                    sort4(F->key);
                    F->cell = c;
                }
            qsort(faces, (size_t)nf, sizeof *faces, cmp_face);
            int m = 0;
            for (int i = 0; i < nf;) {
                int j = i + 1;
                while (j < nf && cmp_face(&faces[i], &faces[j]) == 0) j++;
                if (j - i == 1) faces[m++] = faces[i];
                i = j;
            }
            nf = m;
        } else {
            int per = pt->cell_type;
            faces = malloc((size_t)pt->ncells * sizeof *faces);
            if (!faces) return;
            for (int c = 0; c < pt->ncells; c++) {
                Face *F = &faces[nf++];
                for (int k = 0; k < 4; k++) F->n[k] = pt->conn[per * c + (k < per ? k : per - 1)];
                F->cell = c;
            }
        }
        for (int i = 0; i < nf; i++) {
            const Face *F = &faces[i];
            const double *p0 = &X[3 * F->n[0]], *p1 = &X[3 * F->n[1]], *p2 = &X[3 * F->n[2]], *p3 = &X[3 * F->n[3]];
            if (fld && (node || cellf)) {
                double s0, s1, s2, s3;
                if (node) {
                    s0 = transform(o, fld->data[F->n[0]]), s1 = transform(o, fld->data[F->n[1]]);
                    s2 = transform(o, fld->data[F->n[2]]), s3 = transform(o, fld->data[F->n[3]]);
                } else {
                    s0 = s1 = s2 = s3 = transform(o, fld->data[F->cell]);
                }
                if (o->cmap < 100) {
                    SwColormap cm = (SwColormap)o->cmap;
                    sw_triangle_scalar(im, cam, p0, p1, p2, s0, s1, s2, lo, hi, cm, true, 1);
                    if (F->n[3] != F->n[2]) sw_triangle_scalar(im, cam, p0, p2, p3, s0, s2, s3, lo, hi, cm, true, 1);
                } else { /* labfilm's own maps: colours at the vertices */
                    float c0[3], c1[3], c2[3], c3[3];
                    labview_cmap_rgb(o->cmap, (s0 - lo) / (hi - lo), c0), labview_cmap_rgb(o->cmap, (s1 - lo) / (hi - lo), c1);
                    labview_cmap_rgb(o->cmap, (s2 - lo) / (hi - lo), c2), labview_cmap_rgb(o->cmap, (s3 - lo) / (hi - lo), c3);
                    sw_triangle(im, cam, p0, p1, p2, c0, c1, c2, true, 1);
                    if (F->n[3] != F->n[2]) sw_triangle(im, cam, p0, p2, p3, c0, c2, c3, true, 1);
                }
            } else {
                sw_triangle(im, cam, p0, p1, p2, NEUTRAL, NEUTRAL, NEUTRAL, true, 1);
                if (F->n[3] != F->n[2]) sw_triangle(im, cam, p0, p2, p3, NEUTRAL, NEUTRAL, NEUTRAL, true, 1);
            }
        }
        if (o->mesh) {
            double bias = 2.0 / pix_per_m;
            for (int i = 0; i < nf; i++) {
                const Face *F = &faces[i];
                int nv = (F->n[3] != F->n[2]) ? 4 : 3;
                for (int k = 0; k < nv; k++) {
                    /* an edge shorter than a few pixels on screen would only darken the surface */
                    double ax, ay, az, bx, by, bz;
                    if (!sw_project(cam, &X[3 * F->n[k]], &ax, &ay, &az) || !sw_project(cam, &X[3 * F->n[(k + 1) % nv]], &bx, &by, &bz)) continue;
                    if (hypot(ax - bx, ay - by) < 5.0) continue;
                    sw_line(im, cam, &X[3 * F->n[k]], &X[3 * F->n[(k + 1) % nv]], EDGE, bias, 1);
                }
            }
        }
        free(faces);
    }
}

/* a vertical colour bar drawn with labfilm's own maps (so gray, ink and ember have one too), ink chosen for the background */
/* dark ink over a light background and light ink over a dark one: the mean luminance of the box the text will cover */
static const unsigned char *ink_for(const SwImage *im, int x, int y, int w, int h) {
    static const unsigned char LIGHT[3] = {235, 238, 245}, DARK[3] = {30, 34, 44};
    double s = 0;
    long n = 0;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            if (i < 0 || j < 0 || i >= im->w || j >= im->h) continue;
            const unsigned char *p = im->rgb + 3 * ((size_t)j * im->w + i);
            s += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2], n++;
        }
    return (n && s / n > 140) ? DARK : LIGHT;
}

static void colorbar(SwImage *im, const LabViewOpts *o, int x, int y, int w, int h, double lo, double hi, const char *title, const char *unit) {
    static const unsigned char LIGHT[3] = {235, 238, 245}, DARK[3] = {30, 34, 44}, FRAME[3] = {150, 155, 170};
    const unsigned char *ink = o->light ? DARK : LIGHT;
    int sc = im->h >= 900 ? 2 : 1;
    for (int j = 0; j < h; j++) {
        float rgb[3];
        labview_cmap_rgb(o->cmap, 1.0 - (double)j / (h > 1 ? h - 1 : 1), rgb);
        unsigned char c[3] = {(unsigned char)(255 * rgb[0]), (unsigned char)(255 * rgb[1]), (unsigned char)(255 * rgb[2])};
        sw_rect(im, x, y + j, w, 1, c);
    }
    sw_rect(im, x - 1, y - 1, w + 2, 1, FRAME), sw_rect(im, x - 1, y + h, w + 2, 1, FRAME);
    sw_rect(im, x - 1, y, 1, h, FRAME), sw_rect(im, x + w, y, 1, h, FRAME);
    (void)ink;
    if (title) sw_text(im, x, y - 10 * sc - 6, sc, title, ink_for(im, x, y - 10 * sc - 6, sw_text_width(title, sc), 8 * sc));
    for (int k = 0; k <= 4; k++) {
        double v = hi - (hi - lo) * k / 4.0;
        char lab[48];
        if (v == 0) snprintf(lab, sizeof lab, "0");
        else if (fabs(v) >= 1e5 || fabs(v) < 1e-3) snprintf(lab, sizeof lab, "%.3e", v);
        else snprintf(lab, sizeof lab, "%.4g", v);
        int ty = y + (int)lround((double)(h - 1) * k / 4.0);
        sw_rect(im, x + w, ty, 4, 1, FRAME);
        int tx = x + w + 7;
        if (tx + sw_text_width(lab, sc) > im->w - 2) tx = x - 6 - sw_text_width(lab, sc);
        sw_text(im, tx, ty - 3 * sc, sc, lab, ink_for(im, tx, ty - 3 * sc, sw_text_width(lab, sc), 8 * sc));
    }
    if (unit) sw_text(im, x, y + h + 6 * sc, sc, unit, ink_for(im, x, y + h + 6 * sc, sw_text_width(unit, sc), 8 * sc));
}

/* ---- driver -------------------------------------------------------------------------------------------------- */

static bool all_plan(const LabFrame *fr) {
    if (fr->nparts == 0) return false;
    for (int i = 0; i < fr->nparts; i++) {
        const LabPart *p = &fr->parts[i];
        if (p->kind != LAB_BLOCKS) return false;
        for (int b = 0; b < p->nblocks; b++)
            if (p->blocks[b].n[2] > 1 || p->blocks[b].plane != LAB_PLANE_XY) return false;
    }
    return true;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* the colour range of a field: robust (the 0.5 and 99.5 percentiles over the selected frames, bodies left out, so one
 * wild cell cannot wash out the picture), and symmetric about zero for signed data, so that the diverging palette's
 * dark middle is zero */
void labview_range(LabFile *lf, const LabViewOpts *o, double *lo, double *hi) {
    size_t cap = 1 << 16, n = 0;
    double *v = malloc(cap * sizeof(double));
    int nf = lab_frame_count(lf);
    for (int i = o->first; i < nf && i <= o->last; i += o->every) {
        LabFrame fr;
        char err[128];
        if (!lab_read_frame(lf, i, &fr, err, sizeof err)) continue;
        for (int p = 0; p < fr.nparts; p++) {
            const LabField *f = lab_find_field(&fr.parts[p], o->field);
            if (!f) continue;
            const LabField *sol = o->solid ? lab_find_field(&fr.parts[p], o->solid) : NULL;
            if (sol && sol->count != f->count) sol = NULL;
            size_t stride = f->count > 200000 ? f->count / 200000 : 1;
            for (size_t k = 0; k < f->count; k += stride) {
                if (sol && sol->data[k] > 0.5f) continue;
                double x = transform(o, f->data[k]);
                if (!isfinite(x) || (o->logscale && x <= -30)) continue;
                if (n == cap) {
                    double *nv = realloc(v, 2 * cap * sizeof(double));
                    if (!nv) break;
                    v = nv, cap *= 2;
                }
                v[n++] = x;
            }
        }
        lab_frame_free(&fr);
    }
    if (n == 0) {
        *lo = 0, *hi = 1;
        free(v);
        return;
    }
    qsort(v, n, sizeof(double), cmp_double);
    *lo = v[(size_t)(0.005 * (n - 1))], *hi = v[(size_t)(0.995 * (n - 1))];
    if (!(*hi > *lo)) *lo = v[0], *hi = v[n - 1]; /* a field that is mostly one value: its whole range */
    free(v);
    if (style_pick(*lo, *hi) == STYLE_SIGNED) {
        double m = fmax(fabs(*lo), fabs(*hi));
        *lo = -m, *hi = m;
    }
    if (!(*hi > *lo)) *hi = *lo + 1;
}

int labview_info(LabFile *lf) {
    printf("header: %s\n", lab_header(lf));
    int n = lab_frame_count(lf);
    printf("frames: %d", n);
    if (n) printf(", time %.6g to %.6g s", lab_frame_time(lf, 0), lab_frame_time(lf, n - 1));
    printf("\n");
    if (!n) return 0;
    LabFrame fr;
    char err[128];
    if (!lab_read_frame(lf, n - 1, &fr, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    for (int i = 0; i < fr.nparts; i++) {
        const LabPart *p = &fr.parts[i];
        const char *kind = p->kind == LAB_BLOCKS ? "blocks" : p->kind == LAB_POINTS ? "points" : "cells";
        printf("part %s: %s", p->name, kind);
        if (p->kind == LAB_BLOCKS) {
            int maxlev = 0;
            for (int b = 0; b < p->nblocks; b++) maxlev = p->blocks[b].level > maxlev ? p->blocks[b].level : maxlev;
            printf(", %d blocks, %zu cells, levels 0..%d", p->nblocks, lab_part_ncells(p), maxlev);
        } else if (p->kind == LAB_POINTS) {
            printf(", %d points", p->npoints);
        } else {
            printf(", %d nodes, %d cells of %d nodes", p->npoints, p->ncells, p->cell_type);
        }
        printf("\n");
        for (int k = 0; k < p->nfields; k++) {
            const LabField *f = &p->fields[k];
            double lo = INFINITY, hi = -INFINITY;
            for (size_t m = 0; m < f->count; m++) lo = fmin(lo, f->data[m]), hi = fmax(hi, f->data[m]);
            printf("  field %s: %zu values at %s, %.6g to %.6g\n", f->name, f->count, f->location == LAB_AT_CELL ? "cells" : "nodes", lo, hi);
        }
    }
    lab_frame_free(&fr);
    return 0;
}


/* the first 2D part in the xy plane (the section a planar or axisymmetric solver writes), or -1 */
static int world_part(const LabFrame *fr) {
    for (int i = 0; i < fr->nparts; i++) /* a result with 3D solid cells (a room, a body) is a 3D scene, not a section */
        if (fr->parts[i].kind == LAB_CELLS && fr->parts[i].cell_type == LAB_HEX) return -1;
    for (int i = 0; i < fr->nparts; i++) {
        const LabPart *p = &fr->parts[i];
        if (p->kind != LAB_BLOCKS || p->nblocks == 0) continue;
        bool ok = true;
        for (int b = 0; b < p->nblocks; b++)
            if (p->blocks[b].n[2] > 1 || p->blocks[b].plane != LAB_PLANE_XY) ok = false;
        if (ok) return i;
    }
    return -1;
}

bool labview_render(LabFile *lf, const LabFrame *fr, const LabViewOpts *o_in, double lo, double hi, SwImage *out) {
    LabViewOpts oo = *o_in; /* the lab's palette: signed data diverging, magnitudes sequential, unless a map was asked for */
    if (oo.cmap == STYLE_AUTO) oo.cmap = oo.schlieren ? 101 : style_pick(lo, hi);
    if (strstr(lab_header(lf), "\"domain\":\"impact\"")) oo.solid_points = true;
    if (strstr(lab_header(lf), "\"domain\":\"water\"") && !(oo.point_radius_m > 0)) {
        const char *k = strstr(lab_header(lf), "\"particle_spacing_m\":");
        double sp = 0;
        if (k && sscanf(k + 21, "%lf", &sp) == 1 && sp > 0) oo.point_radius_m = 0.62 * sp;
    }
    const LabViewOpts *o = &oo;
    const int ss = o->supersample > 0 ? o->supersample : 2;
    const int W = o->w * ss, H = o->h * ss;
    SwImage im;
    if (!sw_image_init(&im, W, H)) return false;
    static const unsigned char LT[3] = {250, 250, 252}, LB[3] = {228, 231, 238};
    sw_clear(&im, o->light ? LT : STYLE_BG_TOP, o->light ? LB : STYLE_BG_BOTTOM);
    int margin_r = o->field[0] && !o->schlieren && !o->no_colorbar ? 190 * ss / 2 : 20 * ss;
    int top = 70 * ss / 2 + 10;
    bool flat = o->view && !strcmp(o->view, "flat");
    int wp = flat ? -1 : world_part(fr);
    const LabPart *imgp = NULL; /* a picture (r, g, b fields): seen, not placed in the room, so it fills the frame */
    for (int p = 0; p < fr->nparts && !imgp; p++)
        if (fr->parts[p].kind == LAB_BLOCKS && fr->parts[p].nblocks == 1 && lab_find_field(&fr->parts[p], "r") &&
            lab_find_field(&fr->parts[p], "g") && lab_find_field(&fr->parts[p], "b"))
            imgp = &fr->parts[p];
    if (imgp) {
        const LabBlock *bl = &imgp->blocks[0];
        const float *R = lab_find_field(imgp, "r")->data, *G = lab_find_field(imgp, "g")->data, *B = lab_find_field(imgp, "b")->data;
        int y0 = o->no_chrome ? 0 : 40 * ss;
        double sc = fmin((double)W / bl->n[0], (double)(H - y0) / bl->n[1]);
        int ox = (int)((W - bl->n[0] * sc) / 2), oy = y0 + (int)((H - y0 - bl->n[1] * sc) / 2);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double u = (x + 0.5 - ox) / sc - 0.5, v = bl->n[1] - (y + 0.5 - oy) / sc - 0.5; /* the block's rows go up */
                if (u < -0.5 || v < -0.5 || u > bl->n[0] - 0.5 || v > bl->n[1] - 0.5) continue;
                int i0 = (int)floor(u), j0 = (int)floor(v);
                double tx = u - i0, ty = v - j0;
                int i1 = i0 + 1, j1 = j0 + 1;
                i0 = i0 < 0 ? 0 : i0, j0 = j0 < 0 ? 0 : j0, i1 = i1 >= bl->n[0] ? bl->n[0] - 1 : i1, j1 = j1 >= bl->n[1] ? bl->n[1] - 1 : j1;
                const float *ch[3] = {R, G, B};
                unsigned char *px = im.rgb + 3 * ((size_t)y * W + x);
                for (int k = 0; k < 3; k++) {
                    const float *c = ch[k];
                    double a = (1 - ty) * ((1 - tx) * c[(size_t)j0 * bl->n[0] + i0] + tx * c[(size_t)j0 * bl->n[0] + i1]) +
                               ty * ((1 - tx) * c[(size_t)j1 * bl->n[0] + i0] + tx * c[(size_t)j1 * bl->n[0] + i1]);
                    px[k] = (unsigned char)lround(255.0 * fmin(1.0, fmax(0.0, a)));
                }
            }
        oo.no_colorbar = true, margin_r = 0;
    } else if (flat && all_plan(fr)) {
        for (int p = 0; p < fr->nparts; p++) draw_plan(&im, o, fr, &fr->parts[p], lo, hi, 16 * ss, top, W - margin_r - 16 * ss, H - top - 16 * ss);
    } else if (wp >= 0) {
        /* the world view: the section as a slab in the room, and every other part in the same camera */
        const LabPart *part = &fr->parts[wp];
        double bmin[3] = {INFINITY, INFINITY, 0}, bmax[3] = {-INFINITY, -INFINITY, 0};
        part_bounds(part, bmin, bmax);
        if (o->mirror) bmin[1] = -bmax[1];
        double L = fmax(bmax[0] - bmin[0], bmax[1] - bmin[1]), T = 0.03 * L, Hb = o->mirror ? 0 : 0.06 * L;
        { /* a body that covers much of the section (a wedge, a wall) is extruded thin, so that it does not hide the field */
            const LabField *sf = o->solid ? lab_find_field(part, o->solid) : NULL;
            if (sf) {
                size_t n = lab_part_ncells(part), ns = 0;
                for (size_t i = 0; i < n; i++) ns += sf->data[i] > 0.5f;
                if (n && (double)ns / n > 0.10) Hb = 0.012 * L;
                /* a small body (an aerofoil in a long tunnel) is extruded in proportion to itself, not to the tunnel */
                const LabBlock *bl = part->nblocks == 1 ? &part->blocks[0] : NULL;
                if (bl && bl->n[2] <= 1 && ns) {
                    int i0 = bl->n[0], i1 = -1, j0 = bl->n[1], j1 = -1;
                    for (int j = 0; j < bl->n[1]; j++)
                        for (int i = 0; i < bl->n[0]; i++)
                            if (sf->data[(size_t)j * bl->n[0] + i] > 0.5f) i0 = i < i0 ? i : i0, i1 = i > i1 ? i : i1, j0 = j < j0 ? j : j0, j1 = j > j1 ? j : j1;
                    double ext = fmax((i1 - i0 + 1) * bl->dx[0], (j1 - j0 + 1) * bl->dx[1]);
                    if (i1 >= i0 && (double)ns / n <= 0.10) Hb = fmin(Hb, 0.35 * ext);
                }
            }
        }
        double cx = o->center_given ? o->cx : 0.5 * (bmin[0] + bmax[0]), cy = o->center_given ? o->cy : 0.5 * (bmin[1] + bmax[1]);
        double half = 0.5 * fmax(bmax[0] - bmin[0], (bmax[1] - bmin[1]) * (double)(W - margin_r) / H) / o->zoom;
        bool topv = o->view && !strcmp(o->view, "top");
        double az = (o->angles ? o->azim : -62.0) * M_PI / 180, el = (o->angles ? o->elev : (topv ? 89.0 : 38.0)) * M_PI / 180;
        double dist = 1.05 * half / tan(15.0 * M_PI / 180);
        double target[3] = {cx, cy - (topv ? 0 : 0.06 * half), 0};
        double eye[3] = {target[0] + dist * cos(el) * cos(az), target[1] + dist * cos(el) * sin(az), target[2] + dist * sin(el)};
        double up[3] = {0, 0, 1};
        if (el > 80 * M_PI / 180) up[0] = 0, up[1] = 1, up[2] = 0;
        SwCamera cam;
        sw_camera_look(&cam, eye, target, up, 30, W - margin_r, H);
        draw_world(&im, &cam, o, part, lo, hi, L, Hb, T);
        LabViewOpts o2 = *o;
        o2.radius = o->radius * ss;
        for (int p = 0; p < fr->nparts; p++)
            if (p != wp) draw_3d_part(&im, &cam, &o2, &fr->parts[p], lo, hi, 1.0);
    } else {
        double bmin[3] = {INFINITY, INFINITY, INFINITY}, bmax[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int p = 0; p < fr->nparts; p++) part_bounds(&fr->parts[p], bmin, bmax);
        double ext = 0; /* a flat axis (a 2D block, a point) gets a thickness relative to the scene, not an absolute one */
        for (int k = 0; k < 3; k++)
            if (bmax[k] > bmin[k]) ext = fmax(ext, bmax[k] - bmin[k]);
        if (!(ext > 0)) ext = 1;
        for (int k = 0; k < 3; k++)
            if (!(bmax[k] > bmin[k])) bmin[k] -= 0.01 * ext, bmax[k] += 0.01 * ext;
        if (o->focus >= 0) { /* a view centred on one body */
            for (int p = 0; p < fr->nparts; p++)
                if (fr->parts[p].kind == LAB_POINTS && o->focus < fr->parts[p].npoints) {
                    for (int k = 0; k < 3; k++) {
                        double c = fr->parts[p].xyz[3 * o->focus + k];
                        bmin[k] = c - o->span, bmax[k] = c + o->span;
                    }
                    break;
                }
        }
        if (o->zoom != 1 || o->center_given) { /* about the given centre (x, y), or the box's own */
            for (int k = 0; k < 3; k++) {
                double c = (o->center_given && k < 2) ? (k == 0 ? o->cx : o->cy) : 0.5 * (bmin[k] + bmax[k]), h2 = 0.5 * (bmax[k] - bmin[k]) / o->zoom;
                bmin[k] = c - h2, bmax[k] = c + h2;
            }
        }
        SwCamera cam;
        if (o->angles) {
            double c[3] = {0.5 * (bmin[0] + bmax[0]), 0.5 * (bmin[1] + bmax[1]), 0.5 * (bmin[2] + bmax[2])};
            double rad = 0.5 * sqrt((bmax[0] - bmin[0]) * (bmax[0] - bmin[0]) + (bmax[1] - bmin[1]) * (bmax[1] - bmin[1]) + (bmax[2] - bmin[2]) * (bmax[2] - bmin[2]));
            double az = o->azim * 3.14159265358979 / 180, el = o->elev * 3.14159265358979 / 180, dist = rad / sin(15 * 3.14159265358979 / 180);
            double eye[3] = {c[0] + dist * cos(el) * cos(az), c[1] + dist * cos(el) * sin(az), c[2] + dist * sin(el)}, up[3] = {0, 0, 1};
            sw_camera_look(&cam, eye, c, up, 30, W - margin_r, H);
        } else {
            sw_camera_preset(&cam, o->view ? o->view : "iso", bmin, bmax, 30, W - margin_r, H);
        }
        double diag = sqrt((bmax[0] - bmin[0]) * (bmax[0] - bmin[0]) + (bmax[1] - bmin[1]) * (bmax[1] - bmin[1]) +
                           (bmax[2] - bmin[2]) * (bmax[2] - bmin[2]));
        double ppm = H / (diag > 0 ? diag : 1);
        /* the lab's floor under every bench-top scene (not in space: an orbit has no floor) */
        const char *hd = lab_header(lf);
        if (!strstr(hd, "\"domain\":\"orbit\"")) {
            double L = fmax(fmax(bmax[0] - bmin[0], bmax[1] - bmin[1]), bmax[2] - bmin[2]);
            draw_floor(&im, &cam, bmin[2] - 0.02 * L, 0.5 * (bmin[0] + bmax[0]), 0.5 * (bmin[1] + bmax[1]), L, bmin, bmax);
        }
        LabViewOpts o2 = *o;
        o2.radius = o->radius * ss;
        for (int p = 0; p < fr->nparts; p++) draw_3d_part(&im, &cam, &o2, &fr->parts[p], lo, hi, ppm);
        if (o->labels) { /* the names the header lists under "bodies", beside the first points part */
            JsonValue *h = json_parse(lab_header(lf), strlen(lab_header(lf)), NULL, NULL);
            const JsonValue *names = h ? json_get(h, "bodies") : NULL;
            for (int p = 0; p < fr->nparts && names; p++) {
                if (fr->parts[p].kind != LAB_POINTS) continue;
                for (int i = 0; i < fr->parts[p].npoints && i < (int)json_len(names); i++) {
                    double px, py, d;
                    if (!sw_project(&cam, &fr->parts[p].xyz[3 * i], &px, &py, &d) || d <= 0) continue;
                    if (px < 0 || py < 0 || px >= im.w || py >= im.h) continue;
                    static const unsigned char LI[3] = {225, 230, 240}, DI[3] = {40, 44, 56};
                    sw_text(&im, (int)px + 6 * ss, (int)py - 4 * ss, ss + 1, json_str(json_at(names, (size_t)i)), o->light ? DI : LI);
                }
                break;
            }
            json_free(h);
        }
    }
    static const unsigned char INK[3] = {235, 238, 245}, DARK[3] = {30, 34, 44};
    const unsigned char *ink = o->light ? DARK : INK;
    if (!o->no_chrome) { /* a band behind the title and the time, so they read over a field that fills the frame */
        int bh = 40 * ss;
        for (int y = 0; y < bh && y < H; y++)
            for (int x = 0; x < W; x++) {
                unsigned char *px = im.rgb + 3 * ((size_t)y * W + x);
                for (int k = 0; k < 3; k++) px[k] = o->light ? (unsigned char)(px[k] + (255 - px[k]) * 0.75) : (unsigned char)(px[k] * 0.3);
            }
    }
    if (o->title && !o->no_chrome) sw_text(&im, 16 * ss, 14 * ss, 2 * ss, o->title, ink);
    char tl[64];
    double t = fr->time;
    const char *u = "s";
    if (fabs(t) > 0 && fabs(t) < 1e-6) t *= 1e9, u = "ns";
    else if (fabs(t) > 0 && fabs(t) < 1e-3) t *= 1e6, u = "us";
    else if (fabs(t) >= 1e-3 && fabs(t) < 1) t *= 1e3, u = "ms";
    else if (fabs(t) >= 86400 * 3) t /= 86400, u = "days";
    else if (fabs(t) >= 3600) t /= 3600, u = "h";
    char tu[16] = "";
    const char *tuk = strstr(lab_header(lf), "\"time_unit\":\""); /* a domain with its own unit of time (relativity: M) */
    if (tuk && sscanf(tuk + 13, "%15[^\"]", tu) == 1) t = fr->time, u = tu;
    snprintf(tl, sizeof tl, "t = %.4g %s", t, u);
    if (!o->no_chrome) sw_text(&im, W - sw_text_width(tl, 2 * ss) - 16 * ss, 14 * ss, 2 * ss, tl, ink);
    if (o->field[0] && !o->schlieren && !o->no_colorbar) {
        char unit[96];
        snprintf(unit, sizeof unit, "%s%s", o->logscale ? "log10 " : "", o->unit ? o->unit : "");
        LabViewOpts ob = *o; /* a field shown only on solid surfaces: the bar shows the palette as the surfaces wear it */
        bool solid_only = true;
        for (int p = 0; p < fr->nparts; p++)
            if (fr->parts[p].kind != LAB_CELLS && !(fr->parts[p].kind == LAB_POINTS && o->solid_points) &&
                lab_find_field(&fr->parts[p], o->field))
                solid_only = false;
        if (solid_only && ob.cmap == SW_CMAP_LAB) ob.cmap = SW_CMAP_LAB_SOLID;
        colorbar(&im, &ob, W - margin_r + 26 * ss, top + 30 * ss, 22 * ss, H - top - 110 * ss, lo, hi, o->field, unit);
    }
    if (ss == 1) {
        *out = im;
        return true;
    }
    bool ok = sw_downsample(&im, out);
    sw_image_free(&im);
    return ok;
}

void labview_defaults(LabViewOpts *o) {
    memset(o, 0, sizeof *o);
    o->field = "";
    o->cmap = STYLE_AUTO;
    o->w = 1280, o->h = 720, o->every = 1, o->last = 1 << 30;
    o->radius = 2.5;
    o->zoom = 1;
    o->focus = -1;
    o->supersample = 2;
}
