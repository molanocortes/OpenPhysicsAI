/* swrender.c - triangle and line rasterisation with a depth buffer, 5x7 bitmap text, colour maps, legends */
#include "swrender.h"
#include "../core/png.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void normalize(double *v) {
    double l = sqrt(dot(v, v));
    if (l > 0)
        for (int k = 0; k < 3; k++) v[k] /= l;
}

static void cross(const double *a, const double *b, double *r) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

bool sw_image_init(SwImage *im, int w, int h) {
    memset(im, 0, sizeof *im);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    im->w = w, im->h = h;
    im->rgb = malloc((size_t)w * (size_t)h * 3);
    im->depth = malloc((size_t)w * (size_t)h * sizeof(double));
    im->mask = calloc((size_t)w * (size_t)h, 1);
    if (!im->rgb || !im->depth || !im->mask) {
        sw_image_free(im);
        return false;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) im->depth[i] = INFINITY;
    return true;
}

void sw_image_free(SwImage *im) {
    free(im->rgb);
    free(im->depth);
    free(im->mask);
    memset(im, 0, sizeof *im);
}

void sw_clear(SwImage *im, const unsigned char top[3], const unsigned char bottom[3]) {
    for (int y = 0; y < im->h; y++) {
        double t = im->h > 1 ? (double)y / (im->h - 1) : 0;
        unsigned char c[3];
        for (int k = 0; k < 3; k++) c[k] = (unsigned char)lround(top[k] + t * (bottom[k] - top[k]));
        for (int x = 0; x < im->w; x++) memcpy(im->rgb + 3 * ((size_t)y * (size_t)im->w + (size_t)x), c, 3);
    }
    for (size_t i = 0; i < (size_t)im->w * (size_t)im->h; i++) im->depth[i] = INFINITY, im->mask[i] = 0;
}

bool sw_camera_look(SwCamera *c, const double eye[3], const double target[3], const double up[3], double fov_deg, int w, int h) {
    memcpy(c->eye, eye, sizeof c->eye);
    memcpy(c->target, target, sizeof c->target);
    memcpy(c->up_hint, up, sizeof c->up_hint);
    c->fov_deg = fov_deg;
    c->width = w, c->height = h;
    for (int k = 0; k < 3; k++) c->forward[k] = target[k] - eye[k];
    if (!(dot(c->forward, c->forward) > 0)) return false;
    normalize(c->forward);
    cross(c->forward, up, c->right);
    if (!(dot(c->right, c->right) > 1e-12)) {
        /* up parallel to the view direction: pick another hint */
        double alt[3] = {0, 1, 0};
        if (fabs(c->forward[1]) > 0.9) alt[1] = 0, alt[2] = 1;
        cross(c->forward, alt, c->right);
    }
    normalize(c->right);
    cross(c->right, c->forward, c->up);
    normalize(c->up);
    if (c->ortho_height <= 0) {
        double d = sqrt((target[0] - eye[0]) * (target[0] - eye[0]) + (target[1] - eye[1]) * (target[1] - eye[1]) + (target[2] - eye[2]) * (target[2] - eye[2]));
        c->ortho_height = d * 0.5;
    }
    return true;
}

bool sw_camera_preset(SwCamera *c, const char *preset, const double bmin[3], const double bmax[3], double fov_deg, int w, int h) {
    double center[3], ext[3];
    for (int k = 0; k < 3; k++) center[k] = 0.5 * (bmin[k] + bmax[k]), ext[k] = bmax[k] - bmin[k];
    double radius = 0.5 * sqrt(dot(ext, ext));
    if (!(radius > 0)) radius = 1e-3;
    double dir[3], up[3] = {0, 0, 1};
    if (!preset || !strcmp(preset, "iso")) dir[0] = 1, dir[1] = -1.3, dir[2] = 0.9;
    else if (!strcmp(preset, "iso_back")) dir[0] = -1, dir[1] = 1.3, dir[2] = 0.9;
    else if (!strcmp(preset, "front")) dir[0] = 0, dir[1] = -1, dir[2] = 0;
    else if (!strcmp(preset, "back")) dir[0] = 0, dir[1] = 1, dir[2] = 0;
    else if (!strcmp(preset, "left")) dir[0] = -1, dir[1] = 0, dir[2] = 0;
    else if (!strcmp(preset, "right")) dir[0] = 1, dir[1] = 0, dir[2] = 0;
    else if (!strcmp(preset, "top")) dir[0] = 0, dir[1] = 0, dir[2] = 1, up[0] = 0, up[1] = 1, up[2] = 0;
    else if (!strcmp(preset, "bottom")) dir[0] = 0, dir[1] = 0, dir[2] = -1, up[0] = 0, up[1] = 1, up[2] = 0;
    else return false;
    normalize(dir);
    double aspect = (double)w / h;
    double fov = fov_deg > 0 ? fov_deg : 30;
    double half = fov * M_PI / 360.0;
    double half_h = aspect < 1 ? atan(tan(half) * aspect) : half;
    double dist = radius / sin(half_h) * 1.08;
    double eye[3];
    for (int k = 0; k < 3; k++) eye[k] = center[k] + dir[k] * dist;
    c->ortho_height = 2.2 * radius * (aspect < 1 ? 1 / aspect : 1);
    return sw_camera_look(c, eye, center, up, fov_deg, w, h);
}

bool sw_project(const SwCamera *c, const double p[3], double *px, double *py, double *depth) {
    double d[3] = {p[0] - c->eye[0], p[1] - c->eye[1], p[2] - c->eye[2]};
    double xc = dot(d, c->right), yc = dot(d, c->up), zc = dot(d, c->forward);
    if (depth) *depth = zc;
    if (c->fov_deg > 0) {
        if (!(zc > 1e-12)) return false;
        double f = 0.5 * c->height / tan(c->fov_deg * M_PI / 360.0);
        *px = 0.5 * c->width + f * xc / zc;
        *py = 0.5 * c->height - f * yc / zc;
    } else {
        double f = c->height / c->ortho_height;
        *px = 0.5 * c->width + f * xc;
        *py = 0.5 * c->height - f * yc;
    }
    return true;
}

void sw_ray(const SwCamera *c, double px, double py, double o[3], double dir[3]) {
    if (c->fov_deg > 0) {
        double f = 0.5 * c->height / tan(c->fov_deg * M_PI / 360.0);
        double xc = (px - 0.5 * c->width) / f, yc = (0.5 * c->height - py) / f;
        for (int k = 0; k < 3; k++) {
            o[k] = c->eye[k];
            dir[k] = c->forward[k] + xc * c->right[k] + yc * c->up[k];
        }
        normalize(dir);
    } else {
        double f = c->height / c->ortho_height;
        double xc = (px - 0.5 * c->width) / f, yc = (0.5 * c->height - py) / f;
        for (int k = 0; k < 3; k++) {
            o[k] = c->eye[k] + xc * c->right[k] + yc * c->up[k];
            dir[k] = c->forward[k];
        }
    }
}

static void rasterize(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, const float *c0, const float *c1,
                      const float *c2, const double *sv, double lo, double hi, SwColormap cmap, bool shade, int id, double alpha);

void sw_triangle(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, const float *c0, const float *c1,
                 const float *c2, bool shade, int id) {
    rasterize(im, c, p0, p1, p2, c0, c1, c2, NULL, 0, 1, SW_CMAP_TURBO, shade, id, 1.0);
}

void sw_triangle_scalar(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, double s0, double s1,
                        double s2, double lo, double hi, SwColormap cmap, bool shade, int id) {
    double sv[3] = {s0, s1, s2};
    static const float none[3] = {0, 0, 0};
    rasterize(im, c, p0, p1, p2, none, none, none, sv, lo, hi, cmap, shade, id, 1.0);
}

void sw_triangle_scalar_alpha(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, double s0, double s1,
                              double s2, double lo, double hi, SwColormap cmap, bool shade, int id, double alpha) {
    double sv[3] = {s0, s1, s2};
    static const float none[3] = {0, 0, 0};
    rasterize(im, c, p0, p1, p2, none, none, none, sv, lo, hi, cmap, shade, id, alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha));
}

static void rasterize(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const double *p2, const float *c0, const float *c1,
                      const float *c2, const double *sv, double lo, double hi, SwColormap cmap, bool shade, int id, double alpha) {
    double X[3], Y[3], Z[3];
    const double *P[3] = {p0, p1, p2};
    for (int i = 0; i < 3; i++)
        if (!sw_project(c, P[i], &X[i], &Y[i], &Z[i])) return;
    double area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (fabs(area) < 1e-12) return;
    double light = 1;
    if (shade) {
        double e1[3], e2[3], n[3];
        for (int k = 0; k < 3; k++) e1[k] = p1[k] - p0[k], e2[k] = p2[k] - p0[k];
        cross(e1, e2, n);
        normalize(n);
        double ndl = fabs(dot(n, c->forward));
        light = 0.30 + 0.70 * ndl;
    }
    int x0 = (int)floor(fmin(X[0], fmin(X[1], X[2]))), x1 = (int)ceil(fmax(X[0], fmax(X[1], X[2])));
    int y0 = (int)floor(fmin(Y[0], fmin(Y[1], Y[2]))), y1 = (int)ceil(fmax(Y[0], fmax(Y[1], Y[2])));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > im->w - 1) x1 = im->w - 1;
    if (y1 > im->h - 1) y1 = im->h - 1;
    bool persp = c->fov_deg > 0;
    double iz[3] = {1 / Z[0], 1 / Z[1], 1 / Z[2]};
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double sx = x + 0.5, sy = y + 0.5;
            double w0 = ((X[1] - sx) * (Y[2] - sy) - (X[2] - sx) * (Y[1] - sy)) / area;
            double w1 = ((X[2] - sx) * (Y[0] - sy) - (X[0] - sx) * (Y[2] - sy)) / area;
            double w2 = 1 - w0 - w1;
            if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
            double depth, b0 = w0, b1 = w1, b2 = w2;
            if (persp) {
                double inv = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
                depth = 1 / inv;
                b0 = w0 * iz[0] * depth, b1 = w1 * iz[1] * depth, b2 = w2 * iz[2] * depth; /* perspective-correct */
            } else {
                depth = w0 * Z[0] + w1 * Z[1] + w2 * Z[2];
            }
            size_t idx = (size_t)y * (size_t)im->w + (size_t)x;
            if (!(depth < im->depth[idx])) continue;
            if (alpha >= 1.0) { /* glass leaves the depth buffer alone: what is behind it must still be drawn */
                im->depth[idx] = depth;
                im->mask[idx] = (unsigned char)(id < 0 ? 0 : (id % 255) + 1);
            }
            float pc[3];
            if (sv) {
                double s = b0 * sv[0] + b1 * sv[1] + b2 * sv[2];
                sw_colormap(cmap, hi > lo ? (s - lo) / (hi - lo) : 0.5, pc);
            }
            for (int k = 0; k < 3; k++) {
                double v = (sv ? pc[k] : b0 * c0[k] + b1 * c1[k] + b2 * c2[k]) * light;
                double src = fmin(1, fmax(0, v)) * 255;
                double dst = im->rgb[3 * idx + k];
                im->rgb[3 * idx + k] = (unsigned char)lround(alpha >= 1.0 ? src : alpha * src + (1 - alpha) * dst);
            }
        }
}

static void plot(SwImage *im, int x, int y, double depth, const unsigned char rgb[3]) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    size_t idx = (size_t)y * (size_t)im->w + (size_t)x;
    if (isfinite(depth) && !(depth <= im->depth[idx])) return;
    memcpy(im->rgb + 3 * idx, rgb, 3);
}

void sw_line(SwImage *im, const SwCamera *c, const double *p0, const double *p1, const unsigned char rgb[3], double bias, int thickness) {
    double x0, y0, z0, x1, y1, z1;
    if (!sw_project(c, p0, &x0, &y0, &z0) || !sw_project(c, p1, &x1, &y1, &z1)) return;
    double len = fmax(fabs(x1 - x0), fabs(y1 - y0));
    int steps = (int)ceil(len) + 1;
    if (steps > 20000) return;
    int r = thickness > 1 ? thickness / 2 : 0;
    for (int s = 0; s <= steps; s++) {
        double t = (double)s / steps;
        double x = x0 + t * (x1 - x0), y = y0 + t * (y1 - y0), z = z0 + t * (z1 - z0) - bias;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) plot(im, (int)floor(x) + dx, (int)floor(y) + dy, z, rgb);
    }
}

void sw_rect(SwImage *im, int x, int y, int w, int h, const unsigned char rgb[3]) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < im->w && j < im->h) memcpy(im->rgb + 3 * ((size_t)j * (size_t)im->w + (size_t)i), rgb, 3);
}

/* ---- 5x7 font ---------------------------------------------------------------------------------- */

static const unsigned char FONT[101][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00},
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}, {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}, {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},
    {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}, {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}, {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}, {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}, {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}, {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E},
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    {0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}, {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}, {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}, {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E}, {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}, {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F},
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}, {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08}, {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}, {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11},
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}, {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},
    {0x00, 0x00, 0x0D, 0x13, 0x0F, 0x01, 0x01}, {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E},
    {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06}, {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}, {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}, {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}, {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}, {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02}, {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08}, {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00},
    /* 95 degree sign, 96 multiplication sign, 97 micro, 98 superscript 2, 99 superscript 3, 100 plus-minus */
    {0x0C, 0x12, 0x12, 0x0C, 0x00, 0x00, 0x00}, {0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00}, {0x00, 0x00, 0x11, 0x11, 0x13, 0x1D, 0x10},
    {0x0C, 0x02, 0x04, 0x08, 0x0E, 0x00, 0x00}, {0x0E, 0x02, 0x06, 0x02, 0x0E, 0x00, 0x00}, {0x04, 0x04, 0x1F, 0x04, 0x04, 0x00, 0x1F},
};

static int glyph_index(const unsigned char **s) {
    unsigned char c = **s;
    if (c < 0x80) {
        (*s)++;
        return c >= 32 && c <= 126 ? c - 32 : 31; /* '?' for control characters */
    }
    uint32_t cp = 0;
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    cp = n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int i = 1; i < n; i++) {
        if (((*s)[i] & 0xC0) != 0x80) {
            n = i;
            break;
        }
        cp = (cp << 6) | ((*s)[i] & 0x3F);
    }
    *s += n;
    switch (cp) {
    case 0xB0: return 95;
    case 0xD7: return 96;
    case 0xB5: case 0x3BC: return 97;
    case 0xB2: return 98;
    case 0xB3: return 99;
    case 0xB1: return 100;
    default: return 31;
    }
}

void sw_text(SwImage *im, int x, int y, int scale, const char *utf8, const unsigned char rgb[3]) {
    const unsigned char *s = (const unsigned char *)utf8;
    int pen = x;
    while (*s) {
        int g = glyph_index(&s);
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (FONT[g][row] & (0x10 >> col)) sw_rect(im, pen + col * scale, y + row * scale, scale, scale, rgb);
        pen += 6 * scale;
    }
}

int sw_text_width(const char *utf8, int scale) {
    const unsigned char *s = (const unsigned char *)utf8;
    int n = 0;
    while (*s) glyph_index(&s), n++;
    return n * 6 * scale - (n ? scale : 0);
}

/* ---- colour maps ------------------------------------------------------------------------------- */

void sw_colormap(SwColormap cmap, double t, float rgb[3]) {
    if (!(t >= 0)) t = 0;
    if (t > 1) t = 1;
    double r, g, b;
    switch (cmap) {
    case SW_CMAP_VIRIDIS: {
        /* polynomial fit of matplotlib's viridis */
        static const double C[7][3] = {{0.2777273272234177, 0.005407344544966578, 0.3340998053353061},
                                       {0.1050930431085774, 1.404613529898575, 1.384590162594685},
                                       {-0.3308618287255563, 0.214847559468213, 0.09509516302823659},
                                       {-4.634230498983486, -5.799100973351585, -19.33244095627987},
                                       {6.228269936347081, 14.17993336680509, 56.69055260068105},
                                       {4.776384997670288, -13.74514537774601, -65.35303263337234},
                                       {-5.435455855934631, 4.645852612178535, 26.3124352495832}};
        double v[3];
        for (int k = 0; k < 3; k++) {
            double acc = C[6][k];
            for (int i = 5; i >= 0; i--) acc = C[i][k] + t * acc;
            v[k] = acc;
        }
        r = v[0], g = v[1], b = v[2];
        break;
    }
    case SW_CMAP_HEAT: {
        /* a sequential warm map for temperature: five anchors, linear between them, luminance rising all the way
         * (0.14, 0.27, 0.46, 0.66, 0.90), and a low end that stays visible on a dark background */
        static const double HEAT_T[5] = {0.00, 0.35, 0.60, 0.80, 1.00};
        static const double HEAT_C[5][3] = {{0.23, 0.09, 0.36}, {0.62, 0.16, 0.38}, {0.90, 0.35, 0.22}, {0.98, 0.62, 0.18}, {0.99, 0.91, 0.55}};
        int i = 0;
        while (i < 3 && t > HEAT_T[i + 1]) i++;
        double u = (t - HEAT_T[i]) / (HEAT_T[i + 1] - HEAT_T[i]);
        r = HEAT_C[i][0] + u * (HEAT_C[i + 1][0] - HEAT_C[i][0]);
        g = HEAT_C[i][1] + u * (HEAT_C[i + 1][1] - HEAT_C[i][1]);
        b = HEAT_C[i][2] + u * (HEAT_C[i + 1][2] - HEAT_C[i][2]);
        break;
    }
    case SW_CMAP_LAB:
    case SW_CMAP_LAB_SIGNED:
    case SW_CMAP_LAB_SOLID: {
        /* the lab's palette. LAB: deep indigo, blue, teal, lime, warm light, luminance rising all the way (about 0.12,
         * 0.33, 0.55, 0.80, 0.93), its low end still distinct from the room's background. LAB_SIGNED: blue through the
         * dark of the room to orange-red, so that a signed quantity (vorticity, a pressure deviation, a scattered field)
         * glows out of the background on both sides of zero. */
        static const double LT[5] = {0.00, 0.25, 0.50, 0.75, 1.00};
        static const double LC[5][3] = {{0.13, 0.13, 0.36}, {0.15, 0.38, 0.70}, {0.12, 0.68, 0.70}, {0.74, 0.87, 0.38}, {1.00, 0.94, 0.66}};
        static const double SC[5][3] = {{0.35, 0.62, 1.00}, {0.16, 0.36, 0.78}, {0.10, 0.12, 0.18}, {0.86, 0.36, 0.14}, {1.00, 0.72, 0.36}};
        static const double MC[5][3] = {{0.80, 0.80, 0.78}, {0.52, 0.66, 0.84}, {0.14, 0.62, 0.72}, {0.74, 0.87, 0.38}, {1.00, 0.86, 0.42}};
        const double(*C)[3] = cmap == SW_CMAP_LAB ? LC : cmap == SW_CMAP_LAB_SOLID ? MC : SC;
        int i = 0;
        while (i < 3 && t > LT[i + 1]) i++;
        double u = (t - LT[i]) / (LT[i + 1] - LT[i]);
        r = C[i][0] + u * (C[i + 1][0] - C[i][0]), g = C[i][1] + u * (C[i + 1][1] - C[i][1]), b = C[i][2] + u * (C[i + 1][2] - C[i][2]);
        break;
    }
    case SW_CMAP_COOLWARM: {
        static const double A[3] = {0.230, 0.299, 0.754}, M[3] = {0.865, 0.865, 0.865}, B[3] = {0.706, 0.016, 0.150};
        double u = t < 0.5 ? t * 2 : (t - 0.5) * 2;
        const double *p = t < 0.5 ? A : M, *q = t < 0.5 ? M : B;
        r = p[0] + u * (q[0] - p[0]), g = p[1] + u * (q[1] - p[1]), b = p[2] + u * (q[2] - p[2]);
        break;
    }
    default:
        /* Turbo polynomial approximation (Mikhailov, 2019) */
        r = 0.13572138 + t * (4.61539260 + t * (-42.66032258 + t * (130.58871182 + t * (-150.56663583 + t * 58.13745683))));
        g = 0.09140261 + t * (2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604))));
        b = 0.10667330 + t * (12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973))));
        break;
    }
    rgb[0] = (float)fmin(1, fmax(0, r));
    rgb[1] = (float)fmin(1, fmax(0, g));
    rgb[2] = (float)fmin(1, fmax(0, b));
}

int sw_colormap_find(const char *name) {
    for (int i = 0; i < SW_CMAP_COUNT; i++)
        if (name && !strcmp(name, sw_colormap_name((SwColormap)i))) return i;
    return -1;
}

const char *sw_colormap_name(SwColormap cmap) {
    switch (cmap) {
    case SW_CMAP_VIRIDIS: return "viridis";
    case SW_CMAP_COOLWARM: return "coolwarm";
    case SW_CMAP_HEAT: return "heat";
    case SW_CMAP_LAB: return "lab";
    case SW_CMAP_LAB_SIGNED: return "lab-signed";
    case SW_CMAP_LAB_SOLID: return "lab-solid";
    default: return "turbo";
    }
}

static void format_tick(double v, char *out, size_t cap) {
    double a = fabs(v);
    if (v == 0) snprintf(out, cap, "0");
    else if (a >= 1e5 || a < 1e-3) snprintf(out, cap, "%.3e", v);
    else snprintf(out, cap, "%.4g", v);
}

/* text placed so that it stays inside the image: the caller puts the bar close to the right edge, and at the larger
 * text scale a title or a tick label is wider than what is left there */
static int fit_text_x(const SwImage *im, int x, const char *s, int scale) {
    int w = sw_text_width(s, scale);
    if (x + w > im->w - 2) x = im->w - 2 - w;
    return x < 2 ? 2 : x;
}

void sw_colorbar(SwImage *im, int x, int y, int w, int h, SwColormap cmap, double lo, double hi, const char *title, const char *unit) {
    static const unsigned char LIGHT_INK[3] = {235, 238, 245}, DARK_INK[3] = {30, 34, 44}, FRAME[3] = {160, 165, 180};
    /* the text takes the colour that reads on the background beside the bar */
    const unsigned char *INK = LIGHT_INK;
    int sx = x - 8 < 0 ? 0 : x - 8, sy = y + h / 2 < im->h ? y + h / 2 : im->h - 1;
    if (sx < im->w && sy >= 0) {
        const unsigned char *px = im->rgb + 3 * ((size_t)sy * (size_t)im->w + (size_t)sx);
        if (0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2] > 140) INK = DARK_INK;
    }
    int scale = im->h >= 900 ? 2 : 1;
    if (title) {
        int ts = scale;
        if (sw_text_width(title, ts) > im->w - 4 && ts > 1) ts = 1; /* a very long title drops to the small font */
        sw_text(im, fit_text_x(im, x, title, ts), y - 12 * ts - 4, ts, title, INK);
    }
    for (int j = 0; j < h; j++) {
        float rgb[3];
        sw_colormap(cmap, 1.0 - (double)j / (h > 1 ? h - 1 : 1), rgb);
        unsigned char c[3] = {(unsigned char)(rgb[0] * 255), (unsigned char)(rgb[1] * 255), (unsigned char)(rgb[2] * 255)};
        sw_rect(im, x, y + j, w, 1, c);
    }
    sw_rect(im, x - 1, y - 1, w + 2, 1, FRAME);
    sw_rect(im, x - 1, y + h, w + 2, 1, FRAME);
    sw_rect(im, x - 1, y, 1, h, FRAME);
    sw_rect(im, x + w, y, 1, h, FRAME);
    for (int k = 0; k <= 4; k++) {
        double v = hi - (hi - lo) * k / 4.0;
        char label[48];
        format_tick(v, label, sizeof label);
        int ty = y + (int)lround((double)(h - 1) * k / 4.0);
        sw_rect(im, x + w, ty, 4, 1, FRAME);
        int tx = x + w + 7;
        if (tx + sw_text_width(label, scale) > im->w - 2) tx = x - 6 - sw_text_width(label, scale); /* to the left instead */
        sw_text(im, tx < 2 ? 2 : tx, ty - 3 * scale, scale, label, INK);
    }
    /* below the lowest tick label, which is 7 rows of glyph starting 3 rows above the tick */
    if (unit) {
        int uy = y + h - 1 + 4 * scale + 4;
        if (uy + 7 * scale > im->h - 2) uy = im->h - 2 - 7 * scale;
        sw_text(im, fit_text_x(im, x, unit, scale), uy, scale, unit, INK);
    }
}

void sw_axes(SwImage *im, const SwCamera *c, int cx, int cy, int size) {
    static const unsigned char COL[3][3] = {{230, 80, 70}, {90, 200, 90}, {80, 140, 240}};
    static const char *LAB[3] = {"X", "Y", "Z"};
    for (int k = 0; k < 3; k++) {
        double axis[3] = {k == 0, k == 1, k == 2};
        double sx = dot(axis, c->right), sy = dot(axis, c->up);
        int ex = cx + (int)lround(sx * size), ey = cy - (int)lround(sy * size);
        int steps = size * 2;
        for (int s = 0; s <= steps; s++) {
            int px = cx + (ex - cx) * s / steps, py = cy + (ey - cy) * s / steps;
            sw_rect(im, px - 1, py - 1, 2, 2, COL[k]);
        }
        sw_text(im, ex + 3, ey - 3, 1, LAB[k], COL[k]);
    }
}

bool sw_downsample(const SwImage *src, SwImage *dst) {
    if (!sw_image_init(dst, src->w / 2, src->h / 2)) return false;
    for (int y = 0; y < dst->h; y++)
        for (int x = 0; x < dst->w; x++) {
            size_t o = 3 * ((size_t)y * (size_t)dst->w + (size_t)x);
            for (int k = 0; k < 3; k++) {
                int s = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) s += src->rgb[3 * ((size_t)(2 * y + dy) * (size_t)src->w + (size_t)(2 * x + dx)) + k];
                dst->rgb[o + k] = (unsigned char)((s + 2) / 4);
            }
            size_t si = (size_t)(2 * y) * (size_t)src->w + (size_t)(2 * x);
            dst->depth[(size_t)y * (size_t)dst->w + (size_t)x] = src->depth[si];
            dst->mask[(size_t)y * (size_t)dst->w + (size_t)x] = src->mask[si];
        }
    return true;
}

bool sw_png(const SwImage *im, unsigned char **png, size_t *len) { return png_encode(im->rgb, im->w, im->h, 3, png, len); }
