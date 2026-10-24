/* mgearpair.h - a pair of spur gears in mesh, loaded: loaded tooth contact analysis by finite elements.
 *
 * Both gears are elastic sectors of teeth (gearmesh.h), each held at its bore (and, by choice, at the sector's cut
 * faces), factorised once in its own frame (mbody.h). For each position of the driving gear through the mesh cycle,
 * the gears are placed by perfect conjugate action (the gear turns -z1/z2 times the pinion), and the gap between
 * every node of the pinion's driving flanks and the mating flank of the gear is measured on the exact outlines
 * (gear.h), not on the faceted mesh, so a gap is right to nanometres. Then the contact problem is solved (mcontact.h)
 * with the driving torque on the pinion and the gear held, which gives:
 *
 *   - the loaded transmission error: the pinion's rotation beyond conjugate action, and its part that is geometry
 *     alone (the first contact's closure: tip relief, backlash) so the elastic part, and the mesh stiffness, follow;
 *   - the force on every contacting node, and so the load each tooth pair carries;
 *   - the stress and strain fields of both gears (one solve each), and what a strain gauge bonded to a fillet reads:
 *     the strain along the outline's tangent in the transverse plane, averaged over the gauge's length.
 *
 * What is left out, and said so in every result: friction (mu ~ 0.05 in lubricated steel gears), dynamics (the
 * analysis is quasi-static: the speed at which a measurement was taken matters only through dynamic load), the
 * shafts' and bearings' compliance (the bores are held rigidly), and edge contact at a tip corner that would touch
 * the mating flank between its nodes. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gear.h"
#include "gearmesh.h"
#include "mbody.h"
#include "mcontact.h"

typedef struct GearGauge {
    char name[48];
    int body;          /* 0 the pinion, 1 the gear */
    int tooth;         /* relative to the middle tooth of the sector: 0 the middle, +1 the next counter-clockwise */
    int loaded_side;   /* 1: the fillet on the side of the tooth that carries the contact (tension); 0: the other (compression) */
    double radius_m;   /* the gauge's centre on the outline, as a radius; 0: Hofer's 30 degree tangent point */
    double z_m;        /* across the face, from the z = 0 face; < 0: the middle */
    double length_m;   /* the gauge grid's length along the outline; 0: a point */
} GearGauge;

typedef struct GearPairSpec {
    GearSpec gear[2];        /* 0 the pinion (driver), 1 the gear */
    GearMeshSpec mesh[2];
    double E[2], nu[2];      /* Pa, - */
    double centre_distance_m; /* 0: the standard distance for the shifts */
    double torque_nm;        /* on the pinion */
    double roll_from_deg, roll_to_deg; /* the pinion's positions, from conjugate start with the middle teeth facing */
    int positions;
    bool hold_cut_faces;     /* hold the sectors' radial cut faces as well as the bores */
    bool single_tooth;       /* only the pinion's middle tooth carries load (a rig whose other teeth are ground away) */
    double gap_cut_m;        /* candidate contacts: gaps up to this; default 60 um */
    GearGauge gauge[8];
    int ngauges;
    double gauge_shift_deg;  /* added to every position for the gauge's tooth: 0 */
} GearPairSpec;

typedef struct GearPairPosition {
    double roll_deg;          /* pinion position: its turn from the reference, degrees */
    double gauged_roll_deg;   /* the roll angle of the unloaded contact point on the gear's middle tooth (its involute's
                                 parameter: r = r_b sqrt(1 + theta^2)); NaN when that tooth is not in mesh */
    double theta_rad;         /* loaded rotation beyond conjugate action */
    double theta_geom_rad;    /* the same with no load (geometry only) */
    double te_um;             /* loaded transmission error along the line of action, theta * r_b1 */
    double te_elastic_um;     /* its elastic part */
    double load_by_tooth[8];  /* N, by pinion tooth of the sector */
    double force_total_n;
    int contacts, active;
    double gauge_strain[8];   /* the gauges' readings, 1 */
    double max_mises_pa[2];
} GearPairPosition;

typedef struct GearPair GearPair;

/* builds both gears, meshes and factorises them, and their contact zones */
GearPair *gearpair_create(const GearPairSpec *spec, char *err, size_t errlen);
void gearpair_free(GearPair *gp);
/* one position: fills pos; with fields, also the world node positions and fields of both gears for a frame */
bool gearpair_solve(GearPair *gp, double roll_deg, GearPairPosition *pos, bool fields, char *err, size_t errlen);
/* the frame of the last solve with fields: world node positions (3 * nnodes), per element von Mises (Pa), per node the
 * contact force it carries (N) and von Mises from the nodal strains (Pa, smooth for pictures) */
void gearpair_frame(GearPair *gp, int body, const double **xyz_world, int *nnodes, const int **conn, int *nelems, const float **mises,
                    const float **contact, const float **node_mises);
const char *gearpair_summary(GearPair *gp); /* sizes, times and methods, as JSON text */
/* the roll angle of the unloaded contact on the gear's middle tooth at a pinion turn (geometry only), and the turn
 * that puts it at a given roll angle */
bool gearpair_gauged_roll(GearPair *gp, double turn_deg, double *roll_deg);
bool gearpair_turn_for_roll(GearPair *gp, double roll_deg, double *turn_deg, char *err, size_t errlen);
double gearpair_base_radius(const GearPair *gp, int body);
/* the torque on the pinion for the solves that follow (the parts stay factorised) */
void gearpair_set_torque(GearPair *gp, double torque_nm);
