/* ops_view.c - rendered views with known cameras, and pixel picking */
#include "ops_internal.h"
#include "selection.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool op_parse_view(OpResult *out, const JsonValue *p, ViewOptions *o) {
    memset(o, 0, sizeof *o);
    snprintf(o->preset, sizeof o->preset, "iso");
    o->fov_deg = 30;
    o->width = (int)json_get_int(p, "width", 900);
    o->height = (int)json_get_int(p, "height", 650);
    o->label_patches = json_get_bool(p, "label_patches", true);
    o->show_plate = json_get_bool(p, "show_build_plate", true);
    o->show_mesh = json_get_bool(p, "show_mesh", false);
    snprintf(o->title, sizeof o->title, "%s", json_get_str(p, "title", ""));
    o->showcase = !strcmp(json_get_str(p, "style", "default"), "showcase");
    const JsonValue *cam = json_get(p, "camera");
    if (!cam) return true;
    const JsonValue *fov = json_get(cam, "fov_deg");
    if (fov) o->fov_deg = fov->u.number;
    const JsonValue *eye = json_get(cam, "eye_mm"), *tgt = json_get(cam, "target_mm");
    if (eye || tgt) {
        double e3[3], t3[3], up[3] = {0, 0, 1};
        if (!json_get_numbers(eye, e3, 3) || !json_get_numbers(tgt, t3, 3)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, "give both camera.eye_mm and camera.target_mm, or a camera.preset", "incomplete explicit camera");
            return false;
        }
        json_get_numbers(json_get(cam, "up"), up, 3);
        for (int k = 0; k < 3; k++) o->eye[k] = 1e-3 * e3[k], o->target[k] = 1e-3 * t3[k], o->up[k] = up[k];
        double dx = o->eye[0] - o->target[0], dy = o->eye[1] - o->target[1], dz = o->eye[2] - o->target[2];
        if (!(dx * dx + dy * dy + dz * dz > 0)) {
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "camera eye and target coincide");
            return false;
        }
        o->explicit_camera = true;
        o->preset[0] = 0;
    } else {
        snprintf(o->preset, sizeof o->preset, "%s", json_get_str(cam, "preset", "iso"));
    }
    return true;
}

void op_finish_view(Engine *e, OpResult *out, ViewResult *vr, const char *kind, const char *job_id, const char *image_name, JsonValue *value) {
    const ViewEntry *ve = engine_store_view(e, &vr->cam, kind, job_id);
    json_set_string(value, "view_id", ve ? ve->id : "");
    json_set_string(value, "view_kind", kind);
    json_set(value, "camera", view_camera_json(&vr->cam));
    JsonValue *img = json_set_object(value, "image");
    json_set_string(img, "mime_type", "image/png");
    json_set_int(img, "width", vr->cam.width);
    json_set_int(img, "height", vr->cam.height);
    json_set_int(img, "bytes", (long long)vr->png_len);
    if (vr->labels && json_len(vr->labels)) {
        json_set(value, "visible_patch_labels", vr->labels);
        vr->labels = NULL;
    }
    op_attach_image(out, "image/png", image_name, vr->png, vr->png_len);
    vr->png = NULL;
    view_result_free(vr);
}

static void op_view_render(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    ViewOptions o;
    if (!op_parse_view(out, p, &o)) return;
    const JsonValue *hl = json_get(p, "highlight");
    for (size_t i = 0; i < json_len(hl); i++) {
        const char *name = json_str(json_at(hl, i));
        Selection *s = project_selection(e->proj, name);
        if (!s) {
            op_fail(out, NV_ERR_NOT_FOUND, "list selections with selection_list", "no selection named '%s'", name ? name : "");
            return;
        }
        o.highlight[o.nhighlight++] = s;
    }
    if (o.show_mesh && !e->proj->mesh.valid) {
        op_fail(out, NV_ERR_PRECONDITION, "run mesh_generate first", "there is no mesh to show");
        return;
    }
    ViewResult vr;
    char err[512];
    if (!view_render_project(e->proj, &o, &vr, err, sizeof err)) {
        op_fail(out, NV_ERR_PRECONDITION, NULL, "%s", err);
        return;
    }
    JsonValue *v = json_object();
    op_finish_view(e, out, &vr, o.show_mesh ? "mesh" : "geometry", NULL, "view.png", v);
    json_set_string(v, "how_to_use",
                    "Patch ids are drawn at visible patch centres (see visible_patch_labels). Map pixels of this image to geometry with view_pick, or "
                    "select the face under a pixel with selection_create {\"pick\": {\"view_id\": ..., \"pixel\": [x, y]}}. Photographs or screenshots from "
                    "other software have no known camera and cannot be picked; ask the user to identify faces instead.");
    op_succeed(out, v);
}

static void op_view_pick(Engine *e, JsonValue *p, OpResult *out) {
    if (!op_need_project(e, out)) return;
    const char *id = json_get_str(p, "view_id", "");
    const ViewEntry *ve = engine_find_view(e, id);
    if (!ve) {
        op_fail(out, NV_ERR_NOT_FOUND, "render a view with view_render first", "unknown view '%s' (the last %d views of this session are kept)", id, VIEW_SLOTS);
        return;
    }
    if (strcmp(ve->kind, "geometry") != 0) {
        op_fail(out, NV_ERR_UNSUPPORTED, "render a geometry view (view_render without show_mesh) to pick faces", "view '%s' shows a %s; picking works on geometry views", id,
                ve->kind);
        return;
    }
    if (ve->signature != engine_view_signature(e, false)) {
        op_fail(out, NV_ERR_STALE_REFERENCE, "render a new view", "the geometry or placement changed after view '%s' was rendered", id);
        return;
    }
    const JsonValue *pixels = json_get(p, "pixels");
    JsonValue *v = json_object();
    json_set_string(v, "view_id", id);
    JsonValue *hits = json_set_array(v, "hits");
    for (size_t i = 0; i < json_len(pixels); i++) {
        double px[2];
        JsonValue *h = json_object();
        if (!json_get_numbers(json_at(pixels, i), px, 2)) {
            json_free(h);
            json_free(v);
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "pixels[%zu] must be [x, y]", i);
            return;
        }
        JsonValue *pa = json_array();
        json_push(pa, json_number(px[0]));
        json_push(pa, json_number(px[1]));
        json_set(h, "pixel", pa);
        if (px[0] < 0 || px[1] < 0 || px[0] >= ve->cam.width || px[1] >= ve->cam.height) {
            json_set_bool(h, "hit", false);
            json_set_string(h, "reason", "outside the image");
            json_push(hits, h);
            continue;
        }
        double o[3], d[3];
        sw_ray(&ve->cam, px[0] + 0.5, px[1] + 0.5, o, d);
        int bi;
        BvhHit bh;
        if (!engine_ray_hit(e, o, d, &bi, &bh)) {
            json_set_bool(h, "hit", false);
            json_set_string(h, "reason", "background");
            json_push(hits, h);
            continue;
        }
        Body *b = e->proj->bodies[bi];
        double pt[3];
        for (int k = 0; k < 3; k++) pt[k] = o[k] + bh.t * d[k];
        json_set_bool(h, "hit", true);
        json_set_string(h, "body", b->name);
        json_set_int(h, "triangle", bh.tri);
        json_set_int(h, "patch", b->patches.tri_patch[bh.tri]);
        json_set_string(h, "patch_type", patch_type_name(b->patches.type[b->patches.tri_patch[bh.tri]]));
        json_set(h, "point_mm", json_vec3(1e3 * pt[0], 1e3 * pt[1], 1e3 * pt[2]));
        const double *n = b->build_normal + 3 * (size_t)bh.tri;
        json_set(h, "normal", json_vec3(n[0], n[1], n[2]));
        json_set_number(h, "distance_from_camera_mm", 1e3 * bh.t);
        json_push(hits, h);
    }
    op_succeed(out, v);
}

const OpBinding OPS_VIEW_BINDINGS[] = {
    {"view_render", op_view_render},
    {"view_pick", op_view_pick},
    {NULL, NULL},
};
