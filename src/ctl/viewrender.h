/* viewrender.h - rendered views of a project for AI clients: geometry with highlighted selections and patch labels,
 * the build plate, and FEM fields on the (optionally deformed) mesh. Every view records its camera so pixels can be
 * mapped back to geometry (view_pick, selection "pick"). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../render/swrender.h"
#include "project.h"

enum { VIEW_MAX_HIGHLIGHT = 8 };

typedef struct ViewOptions {
    char preset[16];     /* iso, iso_back, front, back, left, right, top, bottom; "" when the camera is explicit */
    bool explicit_camera;
    double eye[3], target[3], up[3]; /* build frame, m */
    double fov_deg;      /* 0 orthographic */
    int width, height;
    const Selection *highlight[VIEW_MAX_HIGHLIGHT];
    int nhighlight;
    bool label_patches;
    bool show_plate;
    bool show_mesh;      /* draw the voxel mesh boundary instead of the STL surface */
    char title[256];
    /* style "showcase": the capture preset of the repository's media (docs/images/SHOWCASE.md). A dark neutral
     * background and none of the renderer's own annotations (title, colour bar, maximum marker, axis triad): the
     * compositor draws those, identically on every picture, from the legend and camera this operation returns. */
    bool showcase;
} ViewOptions;

typedef struct ViewResult {
    SwCamera cam;
    unsigned char *png;
    size_t png_len;
    JsonValue *labels; /* [{"body", "patch", "pixel": [x, y]}] for labels that were visible */
} ViewResult;

/* The boundary faces of the elements that exist (alive[e] != 0), for a stored time of a build: a face is drawn when
 * exactly one existing element owns it. Hexahedra (elem_type 0) and tetrahedra. The arrays are malloc'ed; returns the
 * number of faces, or -1 when out of memory. Across the coarse and fine sides of an adaptive mesh the faces do not
 * pair, so a few interior faces are kept: they are hidden by the surface in front of them. */
int view_faces_of_alive(const int *conn, int elem_type, int nelems, const unsigned char *alive, int **face_elem, unsigned char **face_local);

bool view_render_project(Project *p, const ViewOptions *o, ViewResult *out, char *err, size_t errlen);

typedef struct FieldView {
    const double *xyz;          /* 3*nnodes (m) */
    const double *u;            /* 3*nnodes displacement (m) or NULL */
    const int *conn;            /* 8*nelems */
    const int *face_elem;       /* boundary faces */
    const unsigned char *face_local;
    int nfaces;
    const double *node_value;   /* nnodes: value to colour by (already in display units) */
    double lo, hi;              /* colour range */
    double deformation_scale;
    bool show_undeformed;
    SwColormap cmap;
    const char *title;
    const char *legend_title;
    const char *unit;
    const double *marker;       /* optional point to mark (m, undeformed) e.g. the location of a maximum */
    const char *marker_label;
    int elem_type;              /* 0: conn holds hexahedra (8 per element); SOLID_ELEM_TET4 / TET10: 4 / 10, tetrahedral faces */
    /* per-body opacity: a body below 1 is drawn as glass, after the solid ones and from the back forwards */
    const signed char *elem_body;
    const float *body_opacity;  /* nbodies entries, NULL = everything solid */
    int nbodies;
} FieldView;

bool view_render_field(const FieldView *f, const ViewOptions *o, ViewResult *out, char *err, size_t errlen);
void view_result_free(ViewResult *r);
JsonValue *view_camera_json(const SwCamera *c);
