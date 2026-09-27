/* project.h - the persisted simulation specification: bodies with units and placement, surface patches, selections,
 * materials, boundary conditions, mesh settings, provenance of every input, assumptions, deterministic save/load.
 *
 * Frames and units
 *   file frame   coordinates as stored in the STL, in the body's declared length unit
 *   build frame  right-handed, metres, +Z = build direction, origin at the centre of the plate's top surface
 *   x_build = R * (unit_scale * x_file) + t
 * The fluid viewer uses a Y-up world; the conversion between the two is explicit, never implied. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../core/json.h"
#include "../core/paths.h"
#include "../geom/bvh.h"
#include "../geom/mesh.h"
#include "../geom/patches.h"
#include "../geom/surface.h"
#include "setup.h"

typedef enum { ROLE_PART = 0, ROLE_SUPPORT, ROLE_BUILD_PLATE, ROLE_COUNT } BodyRole;
const char *body_role_name(BodyRole r);
int body_role_from_name(const char *s);

enum { PLACEMENT_MAX_ROTATIONS = 8 };

typedef struct Placement {
    char up_axis[3];                         /* "+Z", "-Y", ... */
    Provenance up_source;
    double rotate_z_deg;
    int nrot;
    char rot_axis[PLACEMENT_MAX_ROTATIONS];  /* 'x', 'y' or 'z' */
    double rot_deg[PLACEMENT_MAX_ROTATIONS];
    double position_m[2];                    /* footprint centre on the plate */
    double z_offset_m;                       /* lowest point above the plate top */
    /* derived */
    double R[9];                             /* row-major rotation, file axes -> build axes */
    double t[3];                             /* metres */
} Placement;

enum { PATCH_PLANAR_TOL_DEG = 2, PATCH_FEATURE_ANGLE_DEG = 30 };

typedef struct Body {
    char name[64];
    BodyRole role;
    char source_path[NV_PATH_MAX];
    char stored_file[256];
    char sha256[65];
    uint64_t file_bytes;
    StlInfo stl;
    char units[8];
    double unit_scale;             /* metres per file unit */
    Provenance units_source;
    char units_note[512];
    SurfaceRepairOptions repair;
    Placement place;
    Mesh file_mesh;                /* triangles exactly as stored in the file */
    Surface surf;                  /* repaired, welded analysis surface (file units) */
    SurfaceComponent *comps;
    SurfaceDiagnostics diag;
    PatchSet patches;              /* face-like patches of surf (independent of placement) */
    /* build-frame caches, refreshed by placement_apply */
    double *build_v;               /* 3*surf.nv (m) */
    double *build_normal;          /* 3*surf.nt unit normals */
    double *build_centroid;        /* 3*surf.nt (m) */
    double *build_area;            /* surf.nt (m^2) */
    double bmin[3], bmax[3];       /* placed bounds (m) */
    Bvh bvh;                       /* over build_v, built on demand */
    bool bvh_ready;
    uint64_t geom_revision;        /* changes whenever geometry or placement changes */
} Body;

typedef struct Assumption {
    char subject[96];
    char text[512];
    Provenance source;
    uint64_t revision;
} Assumption;

typedef struct Project {
    char id[33];
    char name[64];
    char description[2048];
    char dir[NV_PATH_MAX];
    char created[32], modified[32];
    uint64_t revision;
    uint64_t saved_revision;
    Body **bodies;
    int nbodies;
    Assumption *assumptions;
    int nassumptions, cap_assumptions;
    uint64_t geom_counter;
    /* analysis setup */
    Selection *selections;
    int nselections, cap_selections;
    MaterialAssignment *materials;
    int nmaterials, cap_materials;
    JsonValue *user_materials; /* array of material records defined in this project */
    BoundaryCondition *bcs;
    int nbcs, cap_bcs;
    ThermalContact *contacts;
    int ncontacts, cap_contacts;
    MeshState mesh;
} Project;

Project *project_new(const char *name, const char *dir, const char *description);
void project_free(Project *p);
Body *project_body(Project *p, const char *name); /* NULL name: first part (or first body) */
bool project_add_body(Project *p, Body *b);
bool project_remove_body(Project *p, const char *name);
void project_note_assumption(Project *p, const char *subject, Provenance src, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void project_clear_assumptions(Project *p, const char *subject);

bool body_name_valid(const char *s);
void body_free(Body *b);
Body *body_load(const char *path, const char *name, const char *units, const SurfaceRepairOptions *repair,
                uint64_t max_bytes, uint32_t max_triangles, char *err, size_t errlen);
double length_unit_scale(const char *unit);
/* recomputes R, t and the build-frame caches from the placement settings */
bool placement_apply(Body *b, char *err, size_t errlen);
bool body_ensure_bvh(Body *b);
/* file-frame point -> build frame (m) */
void body_file_to_build(const Body *b, const double *x, double *out);

/* setup entries */
Selection *project_selection(Project *p, const char *name);
Selection *project_add_selection(Project *p); /* zeroed slot, NULL on OOM */
bool project_remove_selection(Project *p, const char *name);
MaterialAssignment *project_material_for(Project *p, const char *target);
BoundaryCondition *project_bc(Project *p, const char *name);
BoundaryCondition *project_add_bc(Project *p);
bool project_remove_bc(Project *p, const char *name);
ThermalContact *project_contact(Project *p, const char *name);
ThermalContact *project_add_contact(Project *p);
bool project_remove_contact(Project *p, const char *name);
JsonValue *thermal_contact_json(const ThermalContact *c);
/* marks the mesh stale when a meshed body changed; returns true when the mesh is current */
bool project_mesh_current(Project *p, char *why, size_t cap);
/* the same for any mesh, voxel or tetrahedral (the static structural chain accepts both) */
bool project_mesh_current_any(Project *p, char *why, size_t cap);

/* JSON views */
JsonValue *body_json(const Body *b, bool detailed);
JsonValue *body_diagnostics_json(const Body *b);
JsonValue *units_plausibility_json(const Body *b);
JsonValue *project_summary_json(const Project *p);
JsonValue *assumptions_json(const Project *p);
JsonValue *boundary_condition_json(const BoundaryCondition *b);

/* project.json persistence; load re-imports stored inputs, verifies their hashes and re-resolves selections */
JsonValue *project_to_json(const Project *p);
bool project_save_file(Project *p, char *err, size_t errlen);
Project *project_load_file(const char *project_json_path, uint64_t max_bytes, uint32_t max_triangles, char *err, size_t errlen);

void iso_time_now(char *buf, size_t cap);
