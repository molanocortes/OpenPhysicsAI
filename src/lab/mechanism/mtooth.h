/* mtooth.h - one gear tooth loaded on its flank: the single-tooth bending test.
 *
 * A sector of a gear (gearmesh.h) is held at its bore and, by choice, at its radial cut faces. A force normal to the
 * flank of the middle tooth acts at a given radius: spread uniformly across the face width and, along the profile, as
 * the Hertzian pressure of a flat anvil on the involute (or over a given half-width). This is how a tooth is loaded in
 * a single-tooth fatigue or calibration rig, and the reference case of ISO 6336-3 and AGMA 908.
 *
 * What comes out, per force: the stress along the tooth's surface from the tip down the loaded flank and fillet to the
 * root (and the same on the unloaded side), as the tangential stress sigma_t at the surface in the transverse plane,
 * which is what a strain gauge aligned with the outline measures as strain; the largest tensile and compressive values
 * and where they are, as an arc length from the tip; and the readings of gauges placed at Hofer's 30 degree tangent
 * point, at the computed maximum, at an arc length from the tip, or at a radius. A gauge reports its strain and the
 * stress a uniaxial reading of it would give (E times the strain), which is how gauge results are published. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gear.h"
#include "gearmesh.h"
#include "mbody.h"

enum { TG_HOFER30 = 0, TG_MAX = 1, TG_ARC = 2, TG_RADIUS = 3 };

typedef struct ToothGauge {
    char name[48];
    int loaded_side;   /* 1: the fillet under the loaded flank (tension); 0: the other side (compression) */
    int where;         /* TG_HOFER30, TG_MAX, TG_ARC (arc_m from the tip corner along the outline), TG_RADIUS */
    double arc_m;      /* for TG_ARC */
    double radius_m;   /* for TG_RADIUS */
    double z_m;        /* across the face from z = 0; < 0: the middle */
    double length_m;   /* grid length along the outline; 0: a point */
} ToothGauge;

typedef struct GearToothSpec {
    GearSpec gear;
    GearMeshSpec mesh;
    double E, nu;
    double load_radius_m;       /* the contact line's radius on the flank */
    double contact_halfwidth_m; /* the pressure's half-width along the profile; 0: Hertz for a flat steel anvil */
    double anvil_friction;      /* Coulomb coefficient at the anvil, sliding against the surface's motion; 0: none */
    bool hold_cut_faces;
    ToothGauge gauge[8];
    int ngauges;
} GearToothSpec;

enum { TOOTH_PROFILE_MAX = 512 };

typedef struct GearToothResult {
    double force_n;
    double halfwidth_m;                  /* the contact half-width used */
    double max_tension_pa, max_tension_arc_m;           /* loaded side, mid-face */
    double max_compression_pa, max_compression_arc_m;   /* unloaded side, mid-face (negative stress) */
    double gauge_strain[8];              /* the gauges' strain readings */
    double gauge_arc_m[8];               /* where each gauge sits, arc length from the tip */
    double max_mises_pa;
    double load_arm_check_nm;            /* the applied force's moment about the gear axis, for the record */
    double surface_slip_m;               /* the frictionless surface motion along the flank at the contact (+ toward the tip) */
    double friction_sense;               /* the friction's direction on the tooth along the flank: +1 toward the tip, -1 the root, 0 none */
} GearToothResult;

typedef struct GearTooth GearTooth;

GearTooth *geartooth_create(const GearToothSpec *spec, char *err, size_t errlen);
void geartooth_free(GearTooth *gt);
/* solves for one normal force (N) and fills res; the fields stay for geartooth_frame */
bool geartooth_solve(GearTooth *gt, double force_n, GearToothResult *res, char *err, size_t errlen);
/* the last solve: node positions displaced by `scale` times the displacement (3 * nnodes), per element von Mises (Pa),
 * per node the tangential surface stress where it is defined (Pa, 0 inside) and von Mises from the nodal strains */
void geartooth_frame(GearTooth *gt, double scale, const double **xyz, int *nnodes, const int **conn, int *nelems, const float **mises,
                     const float **surface_stress, const float **node_mises);
/* the surface profile of the last solve at mid-face: side 1 the loaded side, 0 the other; arc lengths from the tip (m)
 * and tangential stress (Pa), ordered from the tip to the root; returns the count */
int geartooth_profile(const GearTooth *gt, int loaded_side, const double **arc_m, const double **stress_pa);
/* arc length from the tip corner of the outline down to the point at radius r (right side, any side by symmetry) */
double geartooth_arc_at_radius(const GearTooth *gt, double r);
const char *geartooth_summary(const GearTooth *gt);
