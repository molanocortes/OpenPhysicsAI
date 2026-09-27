/* static_analysis.h - static structural analysis of a project
 *
 * static_model_build turns the persisted setup (meshed bodies, material assignments, boundary conditions on named
 * selections) into a self-contained model: owned copies of the mesh, materials evaluated at the reference temperature,
 * prescribed displacements and consistent nodal loads. It reports every problem it finds (stale references,
 * conflicts, rigid-body modes, unmapped selections) instead of repairing it. The model is solved on the job worker
 * without touching the project, and the results are persisted in the run directory. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "../core/paths.h"
#include "../fem/solid.h"
#include "jobs.h"
#include "project.h"

typedef struct StaticSettings {
    Hex8Formulation formulation;
    SolidSolver solver;
    double pcg_tol;
    double reference_temperature_k; /* temperature at which temperature-dependent properties are evaluated */
    /* one bit per mesh body without stiffness (a fluid domain of a conjugate study): its material is not read and the
     * caller leaves its elements out of the structural solve */
    unsigned exclude_bodies;
} StaticSettings;

enum { SA_MAX_BCS = 64, SA_MAX_MATERIALS = 16, SA_MAX_SETS = 64 };

typedef struct SaBcInfo {
    char name[64], selection[64];
    BcKind kind;
    int faces, nodes;
    double mesh_area, stl_area; /* m^2 */
    double requested[3];        /* resultant on the STL surface (N); gravity: weight of the meshed parts */
    double applied[3];          /* resultant of the nodal loads (N) */
    /* moments about the build-frame origin (N m): of the load as specified on the STL surface (a force or traction acts
     * uniformly over the selected STL area, so at its area centroid; a pressure triangle by triangle) and of the nodal
     * loads that represent it on the mesh. Their difference divided by the force is how far the staircase moved the
     * line of action. Gravity: both are the moment of the meshed weight. NAN in results written before they existed. */
    double requested_moment[3];
    double applied_moment[3];
    int constrained_nodes;      /* loaded nodes that also carry a prescribed displacement */
} SaBcInfo;

typedef struct SaSet { /* mesh nodes and boundary faces of a selection, for result queries */
    char name[64];
    int nnodes, nfaces;
    int *nodes; /* sorted */
    int *faces;
} SaSet;

typedef struct StaticModel {
    int nnodes, nelems, nfaces;
    double *xyz;           /* 3*nnodes (m) */
    int *conn;             /* 8*nelems */
    int *elem_mat;         /* nelems */
    signed char *elem_body;
    int *face_elem;
    unsigned char *face_local;
    double h[3];
    int nbodies;
    char body_name[MESH_MAX_BODIES][64];
    double body_volume_stl[MESH_MAX_BODIES], body_volume_mesh[MESH_MAX_BODIES]; /* m^3 (NAN when unknown) */
    char mesh_hash[65];
    int nmat;
    SolidMaterial mat[SA_MAX_MATERIALS];
    char mat_id[SA_MAX_MATERIALS][64], mat_status[SA_MAX_MATERIALS][24];
    unsigned char *fixed;  /* 3*nnodes */
    double *fixed_value;   /* 3*nnodes (m) */
    double *nodal_force;   /* 3*nnodes (N) */
    double gravity[3];
    int *node_bc;          /* nnodes: first constraint condition (index into bc) on the node, -1 */
    int nbc;
    SaBcInfo bc[SA_MAX_BCS];
    int nsets;
    SaSet set[SA_MAX_SETS];
    int nconcave;
    double *concave_seg;   /* 6*nconcave: sharp re-entrant STL edges (m), where linear elastic stresses are singular */
    StaticSettings settings;
    int elem_type;         /* SOLID_ELEM_HEX8 (0, voxel meshes; conn holds 8 per element), SOLID_ELEM_TET4 or SOLID_ELEM_TET10 */
} StaticModel;

/* element access that does not assume hexahedra (static_model.c) */
int sa_npe(const StaticModel *m);                                 /* nodes per element: 8, 4 or 10 */
int sa_ngp(const StaticModel *m);                                 /* stress points per element: 8, 1 or 4 */
int sa_face_nodes_of(const StaticModel *m, int face, int *nodes); /* every node of a boundary face: 4, 3 or 6 */
void sa_elem_coords(const StaticModel *m, int e, double (*X)[3]); /* sa_npe rows */
double sa_elem_volume(const StaticModel *m, int e);
void sa_elem_centroid(const StaticModel *m, int e, double c[3]);
/* consistent nodal forces (sa_npe x 3) of a uniform traction on a boundary face, its area and outward normal */
void sa_face_load(const StaticModel *m, int face, const double t[3], double *fe, double *area, double normal[3]);
const char *sa_element_name(const StaticModel *m);                /* "hex8", "tet4", "tet10" */

/* errors and warnings receive {code, message, hint?, details?} objects; returns true when the model can be solved */
bool static_model_build(Project *p, const StaticSettings *s, StaticModel *m, JsonValue *errors, JsonValue *warnings);
void static_model_free(StaticModel *m);
JsonValue *static_model_summary_json(const StaticModel *m);
const char *static_formulation_name(Hex8Formulation f);

typedef struct StaticResults {
    StaticModel model;
    SolidResult sol;         /* gp_strain is not kept (strain follows from the stress for linear elasticity) */
    double *node_vm;         /* nnodes: von Mises stress of the averaged nodal stress tensor (Pa) */
    double *elem_vm;         /* nelems: largest Gauss-point von Mises stress (Pa) */
    JsonValue *build_warnings;
    JsonValue *summary;
    char job_id[64];
    char run_dir[NV_PATH_MAX];
} StaticResults;

bool static_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void static_results_free(void *data);
void static_results_derive(StaticResults *r); /* node_vm and elem_vm from the stresses */
JsonValue *static_summary_json(const StaticResults *r);

/* results.nvr: a JSON header followed by little-endian arrays */
bool static_results_save(const StaticResults *r, const char *path, char *err, size_t errlen);
StaticResults *static_results_load(const char *path, char *err, size_t errlen);
/* VTK XML unstructured grid (appended raw data, UInt64 headers), SI units in the array names */
bool static_export_vtu(const StaticResults *r, const char *path, char *err, size_t errlen);
/* one row per node in mm, MPa and N */
bool static_export_csv(const StaticResults *r, const char *path, char *err, size_t errlen);

/* engineering quantities of a static result (static_quantities.c). Each has one stated definition:
 *   region displacement  area-weighted mean over the selection's boundary faces of u . d (d a unit direction); the
 *                        displacement is bilinear on each voxel face, so the mean over a face is the mean of its
 *                        four corner values, exactly
 *   load work            W = sum over the condition's nodes of f . u, with f the consistent nodal forces of that load
 *   conjugate displacement  W / |F|: for a uniform traction this equals the area-weighted mean displacement along F
 *   stiffness            k = |F|^2 / W = |F| / conjugate displacement: the load divided by the displacement that does
 *                        work with it, so it follows from an integral over the loaded region, not from one node
 *   mass                 density x volume per body, of the mesh (what the analysis used) and of the STL (the geometry)
 *   balance              applied loads, body forces and reactions: force and moment sums about a point */
typedef struct StaticRegionDisplacement {
    int faces;
    double area;                  /* m^2 of mesh faces */
    double mean[3];               /* m: area-weighted mean displacement vector */
    double along;                 /* m: area-weighted mean of u . d */
    double min_along, max_along;  /* m: extremes of u . d at the region's nodes */
    double centroid[3];           /* m: area centroid of the region's mesh faces */
} StaticRegionDisplacement;
bool static_region_displacement(const StaticResults *r, const char *selection, const double dir[3], StaticRegionDisplacement *out, char *err,
                                size_t errlen);

typedef struct StaticLoadWork {
    double force[3];              /* N: resultant of the condition's nodal forces */
    double work;                  /* J */
    double conjugate_displacement;/* m */
    double stiffness;             /* N/m */
} StaticLoadWork;
/* force, traction and pressure conditions; bc indexes model.bc */
bool static_load_work(const StaticResults *r, int bc, StaticLoadWork *out, char *err, size_t errlen);
/* index of the condition in model.bc, -1 when absent */
int static_find_bc(const StaticModel *m, const char *name);
/* per body and in total: volume and mass of the mesh and of the STL geometry */
JsonValue *static_mass_json(const StaticModel *m);
/* force and moment balance about `about` (m; NULL: the model's bounding-box centre), reactions per support with their
 * moments, and for each load the line-of-action shift between the specified and the applied load */
JsonValue *static_balance_json(const StaticResults *r, const double *about);
/* small-deformation indicators against the stated criteria: largest displacement over the model size, largest
 * infinitesimal rotation, largest strain indicator (von Mises stress / E) */
JsonValue *static_deformation_json(const StaticResults *r);

/* strain at a Gauss point from its stress (isotropic linear elasticity, engineering shear) */
void static_strain_from_stress(const SolidMaterial *m, const double sig[6], double eps[6]);
/* distance (m) from a point to the nearest sharp re-entrant edge of the geometry, INFINITY when there is none */
double static_distance_to_reentrant_edge(const StaticModel *m, const double p[3]);
