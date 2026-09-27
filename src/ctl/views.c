/* views.c - cache of rendered views (cameras) and ray picking against body surfaces */
#include "engine_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

uint64_t engine_view_signature(Engine *e, bool include_mesh) {
    uint64_t h = 1469598103934665603ull;
    if (!e->proj) return 0;
    for (int i = 0; i < e->proj->nbodies; i++) {
        const Body *b = e->proj->bodies[i];
        for (const char *c = b->name; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ull;
        h = (h ^ b->geom_revision) * 1099511628211ull;
    }
    if (include_mesh) h = (h ^ (e->proj->mesh.valid ? e->proj->mesh.revision + 1 : 0)) * 1099511628211ull;
    return h;
}

const ViewEntry *engine_store_view(Engine *e, const SwCamera *cam, const char *kind, const char *job_id) {
    ViewEntry *v = &e->views[e->view_next];
    e->view_next = (e->view_next + 1) % VIEW_SLOTS;
    memset(v, 0, sizeof *v);
    v->used = true;
    snprintf(v->id, sizeof v->id, "view-%u", ++e->view_counter);
    snprintf(v->kind, sizeof v->kind, "%s", kind ? kind : "geometry");
    snprintf(v->job_id, sizeof v->job_id, "%s", job_id ? job_id : "");
    v->cam = *cam;
    v->signature = engine_view_signature(e, strcmp(v->kind, "geometry") != 0);
    return v;
}

const ViewEntry *engine_find_view(Engine *e, const char *id) {
    for (int i = 0; id && i < VIEW_SLOTS; i++)
        if (e->views[i].used && !strcmp(e->views[i].id, id)) return &e->views[i];
    return NULL;
}

bool engine_ray_hit(Engine *e, const double o[3], const double d[3], int *body, BvhHit *hit) {
    bool found = false;
    double best = INFINITY;
    for (int i = 0; e->proj && i < e->proj->nbodies; i++) {
        Body *b = e->proj->bodies[i];
        if (!body_ensure_bvh(b)) continue;
        BvhHit h;
        if (bvh_ray_nearest(&b->bvh, o, d, 0.0, best, -1, &h) && h.t < best) {
            best = h.t;
            *hit = h;
            *body = i;
            found = true;
        }
    }
    return found;
}

bool engine_selection_pick(void *ctx, const char *view_id, double px, double py, Body *target, int *tri, char *err, size_t errlen) {
    Engine *e = ctx;
    const ViewEntry *ve = engine_find_view(e, view_id);
    if (!ve) {
        snprintf(err, errlen, "unknown view '%s' (the last %d rendered views of this session are kept)", view_id, VIEW_SLOTS);
        return false;
    }
    if (strcmp(ve->kind, "geometry") != 0) {
        snprintf(err, errlen, "view '%s' shows a %s; pick faces on a geometry view (view_render without show_mesh)", view_id, ve->kind);
        return false;
    }
    if (ve->signature != engine_view_signature(e, false)) {
        snprintf(err, errlen, "the geometry or placement changed after view '%s' was rendered; render a new view", view_id);
        return false;
    }
    if (px < 0 || py < 0 || px > ve->cam.width || py > ve->cam.height) {
        snprintf(err, errlen, "pixel (%.0f, %.0f) is outside the %dx%d image", px, py, ve->cam.width, ve->cam.height);
        return false;
    }
    double o[3], d[3];
    sw_ray(&ve->cam, px, py, o, d);
    int bi = -1;
    BvhHit h;
    if (!engine_ray_hit(e, o, d, &bi, &h)) {
        snprintf(err, errlen, "no geometry under pixel (%.0f, %.0f) of view '%s'", px, py, view_id);
        return false;
    }
    if (e->proj->bodies[bi] != target) {
        snprintf(err, errlen, "pixel (%.0f, %.0f) shows body '%s', not '%s'", px, py, e->proj->bodies[bi]->name, target->name);
        return false;
    }
    *tri = h.tri;
    return true;
}
