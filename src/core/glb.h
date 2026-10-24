/* glb.h - a glTF 2.0 binary (GLB) writer for the headset's scenes (docs/xr.md): one buffer; views, accessors, meshes of
 * primitives, materials, PNG images as textures, nodes with extras. The JSON is built as a DOM (json.h), so every name
 * and extra is escaped; bytes go into the binary chunk 4-byte aligned. Nothing here reads a GLB.
 *
 *     Glb g; glb_init(&g, "OpenPhysicsAI");
 *     int pos = glb_floats(&g, xyz, n, 3, true);            a VEC3 accessor with its min and max (POSITION needs them)
 *     JsonValue *prims = glb_mesh(&g, "result", &mesh);
 *     JsonValue *attr = glb_primitive(prims, GLB_TRIANGLES, glb_indices(&g, idx, ni), material);
 *     json_set_int(attr, "POSITION", pos);
 *     glb_node(&g, "result", mesh, extras);
 *     glb_finish(&g, &bytes, &len);                          malloc'd; glb_free afterwards in any case
 *
 * A failed allocation anywhere makes glb_finish fail; the calls before it need no checks of their own. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"

enum { GLB_UBYTE = 5121, GLB_USHORT = 5123, GLB_UINT = 5125, GLB_FLOAT = 5126 };
enum { GLB_POINTS = 0, GLB_LINES = 1, GLB_TRIANGLES = 4 };

typedef struct Glb {
    JsonValue *root;
    unsigned char *bin;
    size_t len, cap;
    bool failed;
} Glb;

bool glb_init(Glb *g, const char *generator);
void glb_free(Glb *g);
/* a buffer view over a copy of the bytes (no accessor): a volume, an image; -1 on failure */
int glb_bytes(Glb *g, const void *data, size_t bytes);
/* count elements of comps floats (1 SCALAR, 2 VEC2, 3 VEC3) as an accessor over a new view; minmax adds the bounds */
int glb_floats(Glb *g, const float *v, size_t count, int comps, bool minmax);
int glb_indices(Glb *g, const uint32_t *idx, size_t count);
/* an embedded PNG as a texture sampled linearly and clamped at its edges; returns the texture index */
int glb_texture_png(Glb *g, const unsigned char *png, size_t len);
/* a material (the glTF object, taken over); returns its index */
int glb_material(Glb *g, JsonValue *material);
/* a new mesh; returns its primitives array (owned by the glTF) and its index in *mesh */
JsonValue *glb_mesh(Glb *g, const char *name, int *mesh);
/* a primitive of that mesh (indices < 0: none, material < 0: none); returns its attributes object to fill */
JsonValue *glb_primitive(JsonValue *primitives, int mode, int indices, int material);
/* a node of the scene, with a mesh (or -1) and extras (taken over, may be NULL); returns its index */
int glb_node(Glb *g, const char *name, int mesh, JsonValue *extras);
/* the file: header, JSON chunk (padded with spaces), binary chunk (padded with zeros) */
bool glb_finish(Glb *g, unsigned char **out, size_t *len);

/* IEEE half precision of a float, rounded to nearest even; infinities and NaN kept */
uint16_t glb_half(float f);
float glb_half_to_float(uint16_t h);
