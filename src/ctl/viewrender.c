/* viewrender.c - software-rendered project views and FEM field images */
#include "viewrender.h"
#include "../fem/hex8.h"
#include "../fem/tet.h"
#include "selection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char SHOW_TOP[3] = {20, 22, 27}, SHOW_BOTTOM[3] = {13, 14, 18}; /* style "showcase" */
static const unsigned char BG_TOP[3] = {24, 28, 38}, BG_BOTTOM[3] = {52, 58, 72}, INK[3] = {232, 236, 244}, DIM[3] = {150, 158, 175};

void view_result_free(ViewResult *r) {
    free(r->png);
    json_free(r->labels);
    memset(r, 0, sizeof *r);
}

JsonValue *view_camera_json(const SwCamera *c) {
    JsonValue *o = json_object();
    json_set(o, "eye_mm", json_vec3(1e3 * c->eye[0], 1e3 * c->eye[1], 1e3 * c->eye[2]));
    json_set(o, "target_mm", json_vec3(1e3 * c->target[0], 1e3 * c->target[1], 1e3 * c->target[2]));
    json_set(o, "up", json_vec3(c->up[0], c->up[1], c->up[2]));
    json_set(o, "right", json_vec3(c->right[0], c->right[1], c->right[2]));
    json_set(o, "forward", json_vec3(c->forward[0], c->forward[1], c->forward[2]));
    json_set_string(o, "projection", c->fov_deg > 0 ? "perspective" : "orthographic");
    if (c->fov_deg > 0) json_set_number(o, "vertical_fov_deg", c->fov_deg);
    else json_set_number(o, "ortho_height_mm", 1e3 * c->ortho_height);
    json_set_int(o, "width", c->width);
    json_set_int(o, "height", c->height);
    json_set_string(o, "pixel_convention", "origin at the top-left corner, x right, y down; pixel (i, j) covers [i, i+1) x [j, j+1)");
    return o;
}

static double nice_step(double range) {
    double raw = range / 8, p = pow(10, floor(log10(raw))), m = raw / p;
    return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
}

static void draw_plate(SwImage *im, const SwCamera *cam, double lo[3], double hi[3]) {
    double ext = fmax(hi[0] - lo[0], hi[1] - lo[1]);
    double margin = 0.25 * ext + 1e-3;
    double x0 = lo[0] - margin, x1 = hi[0] + margin, y0 = lo[1] - margin, y1 = hi[1] + margin;
    double step = nice_step(fmax(x1 - x0, y1 - y0));
    x0 = floor(x0 / step) * step, y0 = floor(y0 / step) * step, x1 = ceil(x1 / step) * step, y1 = ceil(y1 / step) * step;
    static const unsigned char GRID[3] = {84, 92, 110}, AXIS[3] = {118, 128, 150};
    for (double x = x0; x <= x1 + 1e-12; x += step) {
        double a[3] = {x, y0, 0}, b[3] = {x, y1, 0};
        sw_line(im, cam, a, b, fabs(x) < 0.5 * step ? AXIS : GRID, 0, 1);
    }
    for (double y = y0; y <= y1 + 1e-12; y += step) {
        double a[3] = {x0, y, 0}, b[3] = {x1, y, 0};
        sw_line(im, cam, a, b, fabs(y) < 0.5 * step ? AXIS : GRID, 0, 1);
    }
    char label[64];
    snprintf(label, sizeof label, "plate grid %.3g mm", step * 1e3);
    sw_text(im, 12, im->h - 22, 1, label, DIM);
}

static void setup_camera(SwCamera *cam, const ViewOptions *o, double lo[3], double hi[3], int w, int h) {
    memset(cam, 0, sizeof *cam);
    if (o->explicit_camera) sw_camera_look(cam, o->eye, o->target, o->up, o->fov_deg, w, h);
    else sw_camera_preset(cam, o->preset[0] ? o->preset : "iso", lo, hi, o->fov_deg, w, h);
}


/* the nodes of a boundary face: 4 corners of a hexahedral face, or 3 corners and (TET10) 3 mid-edge nodes of a
 * tetrahedral one; returns the number of corners */
static int face_nodes6(const int *conn, int elem_type, int e, int local, int out[6]) {
    if (!elem_type) {
        for (int q = 0; q < 4; q++) out[q] = conn[8 * (size_t)e + HEX8_FACE_NODES[local][q]];
        return 4;
    }
    int npe = elem_type == SOLID_ELEM_TET10 ? 10 : 4;
    for (int q = 0; q < 3; q++) out[q] = conn[(size_t)npe * e + TET_FACE[local][q]];
    for (int q = 0; q < 3; q++) out[3 + q] = elem_type == SOLID_ELEM_TET10 ? conn[(size_t)npe * e + TET10_FACE_MID[local][q]] : -1;
    return 3;
}

/* the triangles that draw a face: two for a quadrilateral, one for a flat triangle, four through the mid-edge nodes of
 * a six-node triangle (so a curved TET10 face is drawn curved) */
int view_faces_of_alive(const int *conn, int elem_type, int nelems, const unsigned char *alive, int **face_elem, unsigned char **face_local) {
    typedef struct { int k[4]; int elem; short local, count; } Slot;
    int nlocal = elem_type ? 4 : 6, nalive = 0;
    for (int e = 0; e < nelems; e++) nalive += alive[e] ? 1 : 0;
    size_t cap = 64;
    while (cap < 2 * (size_t)nlocal * (size_t)(nalive ? nalive : 1)) cap <<= 1;
    Slot *tab = malloc(cap * sizeof *tab);
    if (!tab) return -1;
    for (size_t i = 0; i < cap; i++) tab[i].count = 0;
    for (int e = 0; e < nelems; e++) {
        if (!alive[e]) continue;
        for (int l = 0; l < nlocal; l++) {
            int fn[6], k[4] = {-1, -1, -1, -1};
            int nc = face_nodes6(conn, elem_type, e, l, fn);
            for (int q = 0; q < nc && q < 4; q++) k[q] = fn[q];
            for (int a2 = 1; a2 < 4; a2++) /* the corner nodes, sorted: the same face of the neighbour gives the same key */
                for (int b2 = a2; b2 > 0 && k[b2 - 1] > k[b2]; b2--) {
                    int t2 = k[b2];
                    k[b2] = k[b2 - 1], k[b2 - 1] = t2;
                }
            size_t h = ((size_t)(unsigned)k[0] * 73856093u ^ (size_t)(unsigned)k[1] * 19349663u ^ (size_t)(unsigned)k[2] * 83492791u ^
                        (size_t)(unsigned)k[3] * 2654435761u) & (cap - 1);
            while (tab[h].count && (tab[h].k[0] != k[0] || tab[h].k[1] != k[1] || tab[h].k[2] != k[2] || tab[h].k[3] != k[3])) h = (h + 1) & (cap - 1);
            if (!tab[h].count) tab[h] = (Slot){{k[0], k[1], k[2], k[3]}, e, (short)l, 1};
            else tab[h].count++;
        }
    }
    int n = 0;
    for (size_t i = 0; i < cap; i++) n += tab[i].count == 1;
    *face_elem = malloc((size_t)(n ? n : 1) * sizeof(int)), *face_local = malloc((size_t)(n ? n : 1));
    if (!*face_elem || !*face_local) {
        free(*face_elem), free(*face_local), free(tab);
        return -1;
    }
    n = 0;
    for (size_t i = 0; i < cap; i++)
        if (tab[i].count == 1) (*face_elem)[n] = tab[i].elem, (*face_local)[n] = (unsigned char)tab[i].local, n++;
    free(tab);
    return n;
}

static int face_draw_tris(const int *n, int ncorner, int tri[4][3]) {
    if (ncorner == 4) {
        int t[2][3] = {{n[0], n[1], n[2]}, {n[0], n[2], n[3]}};
        memcpy(tri, t, sizeof t);
        return 2;
    }
    if (n[3] < 0) {
        tri[0][0] = n[0], tri[0][1] = n[1], tri[0][2] = n[2];
        return 1;
    }
    int t[4][3] = {{n[0], n[3], n[5]}, {n[3], n[1], n[4]}, {n[5], n[4], n[2]}, {n[3], n[4], n[5]}};
    memcpy(tri, t, sizeof t);
    return 4;
}

bool view_render_project(Project *p, const ViewOptions *o, ViewResult *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    int W = o->width, H = o->height;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < p->nbodies; i++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], p->bodies[i]->bmin[k]), hi[k] = fmax(hi[k], p->bodies[i]->bmax[k]);
    if (!p->nbodies) {
        snprintf(err, errlen, "the project has no geometry to render");
        return false;
    }
    SwImage big;
    if (!sw_image_init(&big, 2 * W, 2 * H)) {
        snprintf(err, errlen, "image size %dx%d not supported", W, H);
        return false;
    }
    if (o->showcase) sw_clear(&big, SHOW_TOP, SHOW_BOTTOM);
    else sw_clear(&big, BG_TOP, BG_BOTTOM);
    SwCamera cam2;
    setup_camera(&cam2, o, lo, hi, 2 * W, 2 * H);
    if (o->show_plate) draw_plate(&big, &cam2, lo, hi);
    static const float BODY_COL[4][3] = {{0.70f, 0.74f, 0.80f}, {0.62f, 0.72f, 0.66f}, {0.76f, 0.68f, 0.60f}, {0.66f, 0.64f, 0.78f}};
    static const float HL_COL[VIEW_MAX_HIGHLIGHT][3] = {{1.0f, 0.55f, 0.10f}, {0.20f, 0.75f, 1.0f}, {0.95f, 0.30f, 0.55f}, {0.45f, 0.90f, 0.35f},
                                                        {0.95f, 0.85f, 0.20f}, {0.65f, 0.45f, 1.0f}, {0.30f, 0.95f, 0.85f}, {1.0f, 0.40f, 0.30f}};
    if (o->show_mesh && p->mesh.valid) {
        const HexMesh *hm = &p->mesh.hm;
        for (int f = 0; f < hm->nfaces; f++) {
            int e = hm->face_elem[f], fnod[6], ftri[4][3];
            int nc = face_nodes6(hm->conn, hm->elem_type, e, hm->face_local[f], fnod), nt = face_draw_tris(fnod, nc, ftri);
            float col[3];
            memcpy(col, hm->region[e] == HEX_REGION_PLATE ? (float[3]){0.48f, 0.50f, 0.55f} : BODY_COL[0], sizeof col);
            int b = hm->body[e];
            for (int s = 0; s < o->nhighlight && b >= 0 && hm->face_tri[f] >= 0; s++)
                if (!strcmp(o->highlight[s]->body, p->mesh.body_name[b]) && selection_has_triangle(o->highlight[s], hm->face_tri[f])) memcpy(col, HL_COL[s], sizeof col);
            for (int t = 0; t < nt; t++)
                sw_triangle(&big, &cam2, hm->xyz + 3 * (size_t)ftri[t][0], hm->xyz + 3 * (size_t)ftri[t][1], hm->xyz + 3 * (size_t)ftri[t][2], col, col, col, true, e);
        }
        if (hm->nfaces < 60000) {
            static const unsigned char EDGE[3] = {40, 44, 54};
            for (int f = 0; f < hm->nfaces; f++) {
                int e = hm->face_elem[f], fnod[6];
                int nc = face_nodes6(hm->conn, hm->elem_type, e, hm->face_local[f], fnod);
                for (int q = 0; q < nc; q++)
                    sw_line(&big, &cam2, hm->xyz + 3 * (size_t)fnod[q], hm->xyz + 3 * (size_t)fnod[(q + 1) % nc], EDGE, 1e-6 * (hi[2] - lo[2] + hi[0] - lo[0]), 1);
            }
        }
    } else {
        for (int i = 0; i < p->nbodies; i++) {
            Body *b = p->bodies[i];
            for (int t = 0; t < b->surf.nt; t++) {
                float col[3];
                memcpy(col, BODY_COL[i % 4], sizeof col);
                for (int s = 0; s < o->nhighlight; s++)
                    if (!strcmp(o->highlight[s]->body, b->name) && selection_has_triangle(o->highlight[s], t)) memcpy(col, HL_COL[s], sizeof col);
                const int *tr = b->surf.tri + 3 * (size_t)t;
                sw_triangle(&big, &cam2, b->build_v + 3 * (size_t)tr[0], b->build_v + 3 * (size_t)tr[1], b->build_v + 3 * (size_t)tr[2], col, col, col, true, i);
            }
            /* patch outlines: edges between different patches and open edges */
            static const unsigned char OUTLINE[3] = {22, 24, 30};
            double bias = 2e-3 * sqrt((b->bmax[0] - b->bmin[0]) * (b->bmax[0] - b->bmin[0]) + (b->bmax[1] - b->bmin[1]) * (b->bmax[1] - b->bmin[1]) +
                                      (b->bmax[2] - b->bmin[2]) * (b->bmax[2] - b->bmin[2]));
            for (int t = 0; t < b->surf.nt; t++)
                for (int e = 0; e < 3; e++) {
                    int nb = b->surf.nbr[3 * t + e];
                    if (nb >= 0 && (nb < t || b->patches.tri_patch[nb] == b->patches.tri_patch[t])) continue;
                    sw_line(&big, &cam2, b->build_v + 3 * (size_t)b->surf.tri[3 * t + e], b->build_v + 3 * (size_t)b->surf.tri[3 * t + (e + 1) % 3], OUTLINE, bias, 2);
                }
        }
    }
    SwImage img;
    if (!sw_downsample(&big, &img)) {
        sw_image_free(&big);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    sw_image_free(&big);
    SwCamera cam;
    setup_camera(&cam, o, lo, hi, W, H);
    out->labels = json_array();
    if (o->label_patches && !o->show_mesh) {
        int budget = 120;
        for (int i = 0; i < p->nbodies && budget > 0; i++) {
            Body *b = p->bodies[i];
            for (int pid = 0; pid < b->patches.n && budget > 0; pid++) {
                PatchGeom g;
                patch_geometry(&b->surf, b->build_v, b->place.R, &b->patches, pid, &g);
                /* label anchor: the patch triangle nearest to the area centroid, lifted slightly along its normal */
                int best = b->patches.order[b->patches.start[pid]];
                double bd = INFINITY;
                for (int k = b->patches.start[pid]; k < b->patches.start[pid + 1]; k++) {
                    int t = b->patches.order[k];
                    const double *c = b->build_centroid + 3 * (size_t)t;
                    double d = (c[0] - g.centroid[0]) * (c[0] - g.centroid[0]) + (c[1] - g.centroid[1]) * (c[1] - g.centroid[1]) + (c[2] - g.centroid[2]) * (c[2] - g.centroid[2]);
                    if (d < bd) bd = d, best = t;
                }
                double anchor[3];
                for (int k = 0; k < 3; k++) anchor[k] = b->build_centroid[3 * (size_t)best + k];
                double px, py, depth;
                if (!sw_project(&cam, anchor, &px, &py, &depth)) continue;
                int ix = (int)px, iy = (int)py;
                if (ix < 2 || iy < 2 || ix >= W - 2 || iy >= H - 2) continue;
                /* visible when the depth buffer at the anchor is close to the anchor depth */
                double scene = img.depth[(size_t)iy * (size_t)W + (size_t)ix];
                double size = fmax(b->bmax[0] - b->bmin[0], fmax(b->bmax[1] - b->bmin[1], b->bmax[2] - b->bmin[2]));
                if (!(fabs(scene - depth) <= 0.02 * size)) continue;
                const double *n = b->build_normal + 3 * (size_t)best;
                if (fabs(n[0] * cam.forward[0] + n[1] * cam.forward[1] + n[2] * cam.forward[2]) < 0.15) continue; /* seen edge-on */
                char txt[16];
                snprintf(txt, sizeof txt, "%d", pid);
                int tw = sw_text_width(txt, 1);
                static const unsigned char BOX[3] = {16, 18, 24};
                sw_rect(&img, ix - tw / 2 - 2, iy - 6, tw + 4, 11, BOX);
                sw_text(&img, ix - tw / 2, iy - 4, 1, txt, INK);
                JsonValue *l = json_object();
                json_set_string(l, "body", b->name);
                json_set_int(l, "patch", pid);
                json_set(l, "pixel", json_vec3(ix, iy, 0));
                json_remove(l, "pixel");
                JsonValue *pix = json_array();
                json_push(pix, json_number(ix));
                json_push(pix, json_number(iy));
                json_set(l, "pixel", pix);
                json_push(out->labels, l);
                budget--;
            }
        }
    }
    if (o->title[0] && !o->showcase) sw_text(&img, 12, 10, W >= 1200 ? 2 : 1, o->title, INK);
    for (int s = 0; s < o->nhighlight; s++) {
        unsigned char c[3] = {(unsigned char)(HL_COL[s][0] * 255), (unsigned char)(HL_COL[s][1] * 255), (unsigned char)(HL_COL[s][2] * 255)};
        sw_rect(&img, 12, 30 + 14 * s, 10, 10, c);
        char label[128];
        snprintf(label, sizeof label, "%s (%.4g mm2)", o->highlight[s]->name, o->highlight[s]->area * 1e6);
        sw_text(&img, 28, 31 + 14 * s, 1, label, INK);
    }
    if (!o->showcase) sw_axes(&img, &cam, W - 50, H - 50, 28);
    out->cam = cam;
    bool ok = sw_png(&img, &out->png, &out->png_len);
    sw_image_free(&img);
    if (!ok) {
        view_result_free(out);
        snprintf(err, errlen, "PNG encoding failed");
    }
    return ok;
}

bool view_render_field(const FieldView *f, const ViewOptions *o, ViewResult *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    int W = o->width, H = o->height;
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    /* frame the deformed shape together with the undeformed one */
    int fnpe = f->elem_type == SOLID_ELEM_TET10 ? 10 : (f->elem_type ? 4 : 8);
    for (int fc = 0; fc < f->nfaces; fc++) {
        int e = f->face_elem[fc];
        for (int a = 0; a < fnpe; a++) {
            int nd = f->conn[(size_t)fnpe * e + a];
            for (int k = 0; k < 3; k++) {
                double x = f->xyz[3 * (size_t)nd + k];
                double xd = x + (f->u ? f->deformation_scale * f->u[3 * (size_t)nd + k] : 0);
                lo[k] = fmin(lo[k], fmin(x, xd)), hi[k] = fmax(hi[k], fmax(x, xd));
            }
        }
    }
    if (!f->nfaces) {
        snprintf(err, errlen, "nothing to render");
        return false;
    }
    SwImage big;
    if (!sw_image_init(&big, 2 * W, 2 * H)) {
        snprintf(err, errlen, "image size %dx%d not supported", W, H);
        return false;
    }
    /* lighter ground than geometry views, so the dark low end of the colour maps stays distinguishable */
    static const unsigned char FIELD_BG_TOP[3] = {70, 76, 90}, FIELD_BG_BOTTOM[3] = {108, 114, 128};
    if (o->showcase) sw_clear(&big, SHOW_TOP, SHOW_BOTTOM);
    else sw_clear(&big, FIELD_BG_TOP, FIELD_BG_BOTTOM);
    SwCamera cam2;
    setup_camera(&cam2, o, lo, hi, 2 * W, 2 * H);
    /* solid faces first, then the glass ones from the back forwards, so a body behind glass is still drawn */
    int *order = NULL, nglass = 0;
    double *gdepth = NULL;
    if (f->body_opacity && f->elem_body && f->nbodies > 0) {
        order = malloc((size_t)(f->nfaces ? f->nfaces : 1) * sizeof(int));
        gdepth = malloc((size_t)(f->nfaces ? f->nfaces : 1) * sizeof(double));
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int fc = 0; fc < f->nfaces; fc++) {
            int e = f->face_elem[fc], fnod[6], ftri[4][3];
            int bi = f->elem_body ? f->elem_body[e] : -1;
            double alpha = f->body_opacity && bi >= 0 && bi < f->nbodies ? f->body_opacity[bi] : 1.0;
            if (pass == 0 && alpha < 0.999) { /* collect the glass faces and how far away they are */
                if (order && gdepth) {
                    double c[3] = {0, 0, 0};
                    int ncf = face_nodes6(f->conn, f->elem_type, e, f->face_local[fc], fnod);
                    for (int q = 0; q < ncf; q++)
                        for (int k = 0; k < 3; k++) c[k] += f->xyz[3 * (size_t)fnod[q] + k] / ncf;
                    double dx = c[0] - cam2.eye[0], dy = c[1] - cam2.eye[1], dz = c[2] - cam2.eye[2];
                    order[nglass] = fc, gdepth[nglass] = dx * dx + dy * dy + dz * dz;
                    nglass++;
                }
                continue;
            }
            if (pass == 1) continue;
            int nt = face_draw_tris(fnod, face_nodes6(f->conn, f->elem_type, e, f->face_local[fc], fnod), ftri);
            for (int t = 0; t < nt; t++) {
                double P[3][3], S[3];
                for (int q = 0; q < 3; q++) {
                    int nd = ftri[t][q];
                    for (int k = 0; k < 3; k++) P[q][k] = f->xyz[3 * (size_t)nd + k] + (f->u ? f->deformation_scale * f->u[3 * (size_t)nd + k] : 0);
                    S[q] = f->node_value[nd];
                }
                sw_triangle_scalar(&big, &cam2, P[0], P[1], P[2], S[0], S[1], S[2], f->lo, f->hi, f->cmap, true, 0);
            }
        }
    }
    for (int a = 0; a < nglass; a++) /* farthest first */
        for (int b = a + 1; b < nglass; b++)
            if (gdepth[b] > gdepth[a]) {
                double dt = gdepth[a];
                int it = order[a];
                gdepth[a] = gdepth[b], gdepth[b] = dt;
                order[a] = order[b], order[b] = it;
            }
    for (int a = 0; a < nglass; a++) {
        int fc = order[a], e = f->face_elem[fc], fnod[6], ftri[4][3];
        int bi = f->elem_body ? f->elem_body[e] : -1;
        double alpha = f->body_opacity && bi >= 0 && bi < f->nbodies ? f->body_opacity[bi] : 1.0;
        int nt = face_draw_tris(fnod, face_nodes6(f->conn, f->elem_type, e, f->face_local[fc], fnod), ftri);
        for (int t = 0; t < nt; t++) {
            double P[3][3], S[3];
            for (int q = 0; q < 3; q++) {
                int nd = ftri[t][q];
                for (int k = 0; k < 3; k++) P[q][k] = f->xyz[3 * (size_t)nd + k] + (f->u ? f->deformation_scale * f->u[3 * (size_t)nd + k] : 0);
                S[q] = f->node_value[nd];
            }
            sw_triangle_scalar_alpha(&big, &cam2, P[0], P[1], P[2], S[0], S[1], S[2], f->lo, f->hi, f->cmap, true, 0, alpha);
        }
    }
    free(order), free(gdepth);
    if (f->show_undeformed && f->u && f->nfaces < 4000000) {
        /* outline of the undeformed shape: only crease edges, where the boundary faces meeting at an edge face different
         * directions (voxel faces carry their direction in face_local); drawn over the surface, without depth test */
        static const unsigned char GHOST[3] = {226, 229, 237};
        typedef struct {
            int a, b;
            signed char dir;
            bool crease;
            int face; /* tetrahedral faces: the first face seen, whose normal decides the crease */
        } EdgeSlot;
        size_t cap = 16;
        while (cap < 8 * (size_t)f->nfaces) cap <<= 1;
        EdgeSlot *tab = malloc(cap * sizeof *tab);
        if (tab) {
            for (size_t i = 0; i < cap; i++) tab[i].a = -1;
            for (int fc = 0; fc < f->nfaces; fc++) {
                int e = f->face_elem[fc], fnod[6];
                int nc = face_nodes6(f->conn, f->elem_type, e, f->face_local[fc], fnod);
                for (int q = 0; q < nc; q++) {
                    int a = fnod[q], b = fnod[(q + 1) % nc];
                    if (a > b) {
                        int t = a;
                        a = b, b = t;
                    }
                    size_t hsh = ((size_t)a * 73856093u ^ (size_t)b * 19349663u) & (cap - 1);
                    while (tab[hsh].a >= 0 && (tab[hsh].a != a || tab[hsh].b != b)) hsh = (hsh + 1) & (cap - 1);
                    if (tab[hsh].a < 0) tab[hsh] = (EdgeSlot){a, b, (signed char)f->face_local[fc], false, fc};
                    else if (!f->elem_type && tab[hsh].dir != (signed char)f->face_local[fc]) tab[hsh].crease = true;
                    else if (f->elem_type) {
                        /* a crease where the two faces' normals differ by more than 30 degrees */
                        double nrm[2][3];
                        int fcs[2] = {tab[hsh].face, fc};
                        for (int s2 = 0; s2 < 2; s2++) {
                            int fn2[6];
                            face_nodes6(f->conn, f->elem_type, f->face_elem[fcs[s2]], f->face_local[fcs[s2]], fn2);
                            const double *p0 = f->xyz + 3 * (size_t)fn2[0], *p1 = f->xyz + 3 * (size_t)fn2[1], *p2 = f->xyz + 3 * (size_t)fn2[2];
                            double u1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, u2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                            nrm[s2][0] = u1[1] * u2[2] - u1[2] * u2[1], nrm[s2][1] = u1[2] * u2[0] - u1[0] * u2[2], nrm[s2][2] = u1[0] * u2[1] - u1[1] * u2[0];
                        }
                        double d = nrm[0][0] * nrm[1][0] + nrm[0][1] * nrm[1][1] + nrm[0][2] * nrm[1][2];
                        double l = sqrt((nrm[0][0] * nrm[0][0] + nrm[0][1] * nrm[0][1] + nrm[0][2] * nrm[0][2]) *
                                        (nrm[1][0] * nrm[1][0] + nrm[1][1] * nrm[1][1] + nrm[1][2] * nrm[1][2]));
                        if (l > 0 && d < 0.866 * l) tab[hsh].crease = true;
                    }
                }
            }
            for (size_t i = 0; i < cap; i++)
                if (tab[i].a >= 0 && tab[i].crease) sw_line(&big, &cam2, f->xyz + 3 * (size_t)tab[i].a, f->xyz + 3 * (size_t)tab[i].b, GHOST, INFINITY, 1);
            free(tab);
        }
    }
    SwImage img;
    if (!sw_downsample(&big, &img)) {
        sw_image_free(&big);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    sw_image_free(&big);
    SwCamera cam;
    setup_camera(&cam, o, lo, hi, W, H);
    int bar_h = H * 45 / 100, bar_x = W - 110, bar_y = 70;
    if (!o->showcase) sw_colorbar(&img, bar_x, bar_y, 16, bar_h, f->cmap, f->lo, f->hi, f->legend_title, f->unit);
    if (f->title && !o->showcase) sw_text(&img, 12, 10, W >= 1200 ? 2 : 1, f->title, INK);
    if (f->marker && !o->showcase) {
        double m[3];
        for (int k = 0; k < 3; k++) m[k] = f->marker[k];
        double px, py, d;
        if (sw_project(&cam, m, &px, &py, &d)) {
            static const unsigned char MK[3] = {255, 255, 255};
            sw_rect(&img, (int)px - 5, (int)py, 11, 1, MK);
            sw_rect(&img, (int)px, (int)py - 5, 1, 11, MK);
            if (f->marker_label) sw_text(&img, (int)px + 7, (int)py - 10, 1, f->marker_label, MK);
        }
    }
    if (!o->showcase) sw_axes(&img, &cam, W - 50, H - 50, 28);
    out->cam = cam;
    out->labels = json_array();
    bool ok = sw_png(&img, &out->png, &out->png_len);
    sw_image_free(&img);
    if (!ok) {
        view_result_free(out);
        snprintf(err, errlen, "PNG encoding failed");
    }
    return ok;
}
