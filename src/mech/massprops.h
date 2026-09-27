/* massprops.h - mass properties of rigid parts from closed triangle meshes, with explicit fill models for printed parts
 *
 * Geometry requirements (checked, never assumed)
 *   - closed: every edge is shared by exactly two triangles
 *   - consistently oriented: the two triangles traverse a shared edge in opposite directions
 *   - outward: the total signed volume is positive (counter-clockwise triangles seen from outside)
 *   Closed components with negative signed volume are cavities (inward-facing inner shells) and subtract volume: they are
 *   explicit internal geometry. A closed outer shell alone says nothing about what is inside a printed part.
 *
 * Fill models (the choice is an input with provenance; there is no default)
 *   solid          uniform density over the enclosed volume
 *   shell_infill   homogenised print: a wall of thickness t on every surface at full density, the remaining volume at
 *                  infill_fraction * density. Thin-shell approximation: wall volume = area * t with the wall mass placed on
 *                  the surface; the error grows with t/R (curvature) and is reported through wall_fraction.
 *   measured_mass  a weighed mass distributed uniformly over the enclosed volume (centre of mass and inertia then assume a
 *                  uniform distribution, which is wrong for sparse infill; reported as an assumption)
 *
 * Integrals follow Eberly, "Polyhedral Mass Properties (Revisited)", computed about the bounding-box centre to limit
 * cancellation, then shifted. Units are those of the vertices (callers pass metres) and kg/m^3. */
#pragma once

#include <stdbool.h>

#include "mechdiag.h"

typedef struct MeshTopology {
    int ntri, nvert;
    int boundary_edges, nonmanifold_edges, inconsistent_edges, degenerate_triangles;
    int components;
    int positive_shells, negative_shells, open_components;
    bool closed, oriented;
    double signed_volume; /* m^3 */
    double area;          /* m^2 */
    double bmin[3], bmax[3];
} MeshTopology;

typedef struct MomentIntegrals {
    double zeroth;    /* V (m^3) or A (m^2) */
    double first[3];  /* integral of x, y, z */
    double second[6]; /* integral of xx, yy, zz, xy, yz, zx */
} MomentIntegrals;

typedef enum { FILL_SOLID = 0, FILL_SHELL_INFILL, FILL_MEASURED_MASS, FILL_MODEL_COUNT } FillModel;
const char *fill_model_name(FillModel f);
int fill_model_from_name(const char *s);

typedef struct FillSpec {
    FillModel model;
    double density;         /* kg/m^3 of the solid material (solid, shell_infill; optional for measured_mass) */
    double shell_thickness; /* m (shell_infill) */
    double infill_fraction; /* 0..1 (shell_infill) */
    double measured_mass;   /* kg (measured_mass) */
} FillSpec;

typedef struct MassProperties {
    double mass;            /* kg */
    double com[3];          /* m, mesh coordinates */
    double inertia_com[9];  /* kg m^2 about the centre of mass, mesh axes */
    double principal[3];    /* principal moments, descending */
    double axes[9];         /* principal axes in the columns, right-handed */
    double volume;          /* enclosed volume (m^3), cavities subtracted */
    double area;            /* surface area (m^2) */
    double wall_fraction;   /* shell_infill: wall volume / enclosed volume (thin-shell approximation quality) */
    double effective_density; /* mass / volume */
} MassProperties;

/* topology and orientation checks; v: 3*nv coordinates, tri: 3*nt indices */
bool mesh_topology(const double *v, int nv, const int *tri, int nt, MeshTopology *out);
void mesh_volume_integrals(const double *v, const int *tri, int nt, MomentIntegrals *out);
void mesh_surface_integrals(const double *v, const int *tri, int nt, MomentIntegrals *out);

/* validates the mesh (errors in diag) and computes mass properties for the fill model */
bool mass_properties_from_mesh(const double *v, int nv, const int *tri, int nt, const FillSpec *fill, MassProperties *mp, MechDiag *diag,
                               const char *subject);

/* I_point = I_com + m (|d|^2 E - d d^T), d = com - point */
void inertia_about_point(const double *I_com, double mass, const double *com, const double *point, double *I_point);
/* I_com from the inertia about another point */
void inertia_to_com(const double *I_point, double mass, const double *com, const double *point, double *I_com);
/* R I R^T */
void inertia_rotate(const double *R, const double *I, double *out);
/* symmetric, positive principal moments, triangle inequality (relative tolerance tol); why receives the reason */
bool inertia_admissible(const double *I, double tol, char *why, int whylen);
/* principal moments (descending) and right-handed axes (columns) */
void inertia_principal(const double *I, double moments[3], double axes[9]);
