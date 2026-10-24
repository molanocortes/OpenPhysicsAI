/* mplanet.h - a planetary gear stage, loaded: a sun, N planets on a carrier's pins and an internal ring, all elastic
 * and in frictionless contact (mgearpart.h), the reduction stage of most robot joints.
 *
 * The ring and the carrier are held (the output locked, as a stage's torsional stiffness is measured), a torque acts
 * on the sun, and the stage is placed at a carrier angle by its kinematics (the sun turns 1 + z_r / z_s times the
 * carrier, each planet 1 - z_r / z_p times it about its own pin). The rigid degrees of freedom are the sun's rotation
 * and each planet's on its pin; the contact problem gives every tooth pair's force, the sun's wind-up (its rotation
 * beyond the kinematic one: the stage's compliance) and each planet's share of the torque, which a pin out of place
 * (pin_error_m) makes unequal. Each planet's teeth carry the sun on one flank and the ring on the other.
 *
 * A floating sun (sun_floating) moves sideways to wherever its contacts balance, which evens the planets' shares.
 *
 * Kept out: friction, the pins' and bearings' compliance (planets turn on rigid pins), dynamics. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gear.h"
#include "gearmesh.h"
#include "mgearpart.h"

enum { PL_MAX_PLANETS = 8 };

typedef struct PlanetarySpec {
    GearSpec sun, planet, ring, cutter; /* ring: internal, cut by the shaper `cutter` */
    GearMeshSpec mesh_sun, mesh_planet, mesh_ring;
    double E[3], nu[3];                 /* sun, planets, ring */
    int nplanets;
    double sun_torque_nm;               /* on the sun, counter-clockwise */
    double pin_error_m[PL_MAX_PLANETS][2]; /* each pin's position error: along the carrier's rotation, and outward */
    double gap_cut_m;                   /* candidate contacts: gaps up to this above a mesh's smallest; default 60 um */
    bool sun_floating;                  /* the sun free to move sideways (on a flexible coupling): its translation two
                                           more degrees of freedom with no force on them */
} PlanetarySpec;

typedef struct PlanetaryResult {
    double carrier_deg;
    double sun_windup_rad;              /* the sun's rotation beyond the kinematic one */
    double sun_float_m[2];              /* a floating sun's displacement */
    double planet_turn_rad[PL_MAX_PLANETS];
    double sun_mesh_force_n[PL_MAX_PLANETS];  /* the sun's contacts with each planet: their normal forces summed */
    double ring_mesh_force_n[PL_MAX_PLANETS];
    double load_share[PL_MAX_PLANETS];  /* each planet's share of the sun's torque, times N (1 when equal) */
    double sun_torque_check_nm;         /* the contacts' torque on the sun (balances the applied one) */
    double carrier_torque_nm, ring_torque_nm; /* the reactions of the held parts, about the stage's axis */
    int contacts, active, pivots;
    double max_mises_pa;
} PlanetaryResult;

typedef struct Planetary Planetary;

Planetary *planetary_create(const PlanetarySpec *spec, char *err, size_t errlen);
void planetary_free(Planetary *pl);
/* one position (the carrier's angle, degrees); with fields, also the parts' stress for a frame */
bool planetary_solve(Planetary *pl, double carrier_deg, PlanetaryResult *res, bool fields, char *err, size_t errlen);
/* the parts of the last solve with fields: count, and each one's name, world node positions, cells, von Mises per
 * element and per node */
int planetary_parts(const Planetary *pl);
void planetary_part(Planetary *pl, int i, const char **name, const double **xyz, int *nnodes, const int **conn, int *nelems, const float **mises,
                    const float **node_mises);
const char *planetary_summary(const Planetary *pl);
