/* mesh.h - triangle meshes: STL import/export, normals, statistics
 *
 * Coordinate convention for the whole application ("model space" and "lattice space"):
 *   +X = streamwise (the flow travels toward +X; a vehicle's nose points toward -X)
 *   +Y = up
 *   +Z = spanwise (right-handed: X cross Y = Z)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../math3d.h"

typedef struct Mesh {
    vec3 *pos;          /* triangle soup: 3 * tri_count positions (CCW winding seen from outside) */
    vec3 *nrm;          /* 3 * tri_count unit vertex normals (may be NULL until computed) */
    uint32_t tri_count;
    uint32_t tri_cap;   /* allocated capacity in triangles */
    vec3 bmin, bmax;    /* bounding box (valid after mesh_compute_bounds) */
    char name[128];
} Mesh;

typedef struct MeshStats {
    uint32_t triangles;
    uint32_t unique_vertices;  /* after welding */
    uint32_t open_edges;       /* edges used by exactly one triangle */
    uint32_t nonmanifold_edges;/* edges used by more than two triangles */
    uint32_t degenerate_removed;
    double surface_area;
    double volume;             /* signed volume via divergence theorem (positive for outward CCW) */
    bool watertight;           /* open_edges == 0 && nonmanifold_edges == 0 */
} MeshStats;

void mesh_init(Mesh *m);
void mesh_free(Mesh *m);
bool mesh_reserve(Mesh *m, uint32_t tri_capacity);
void mesh_add_tri(Mesh *m, vec3 a, vec3 b, vec3 c);            /* grows automatically */
void mesh_add_quad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d);   /* two triangles a-b-c, a-c-d */
void mesh_append(Mesh *dst, const Mesh *src);
void mesh_compute_bounds(Mesh *m);

/* Recompute vertex normals from geometry. Faces meeting at a welded vertex are smoothed
 * together when the angle between their face normals is below crease_angle_deg. */
void mesh_compute_normals(Mesh *m, float crease_angle_deg);

/* Apply an affine transform to positions (and normals with the inverse-transpose). Updates bounds. */
void mesh_transform(Mesh *m, mat4 M);

/* Remove zero-area / non-finite triangles; returns number removed. */
uint32_t mesh_remove_degenerate(Mesh *m);

/* If the signed volume is negative (inward winding), flip all triangles. */
void mesh_fix_orientation(Mesh *m);

void mesh_compute_stats(const Mesh *m, MeshStats *out);

/* Load ASCII or binary STL (auto-detected). On success: degenerate triangles removed, bounds and
 * smooth normals (crease 35 deg) computed, name set from the file name. Returns false and fills err. */
bool mesh_load_stl(const char *path, Mesh *out, char *err, size_t errlen);

typedef struct StlInfo {
    bool binary;
    uint64_t file_bytes;
    uint32_t declared_triangles; /* binary: triangle count in the header */
    uint64_t trailing_bytes;     /* binary: bytes after the declared records (ignored) */
    char header[81];             /* binary: 80-byte header, non-printable bytes shown as '.' */
    char solid_name[81];         /* ASCII: text after "solid" on the first line */
} StlInfo;

/* Raw STL import for engineering analysis: the triangles are returned exactly as stored (nothing is removed or
 * reoriented, no normals are computed; bounds and name are set). max_bytes / max_triangles of 0 mean no limit. */
bool mesh_load_stl_ex(const char *path, Mesh *out, StlInfo *info, uint64_t max_bytes, uint32_t max_triangles, char *err,
                      size_t errlen);

/* Save as binary STL. */
bool mesh_save_stl(const char *path, const Mesh *m, char *err, size_t errlen);
