/* mtrain.h - a gear train of any layout, loaded: gears (spur or helical, external or internal) on shafts, shafts on
 * axes fixed in the housing or carried round by carriers, every gear elastic and in frictionless contact with the gears
 * it meshes with (mgearpart.h). Planetary stages in series, compound planets (a Wolfrom stage), parallel-shaft
 * gearboxes with intermediate shafts, and their combinations.
 *
 * Shafts. A shaft is a rigid body turning about its axis; every gear on it turns with it (a compound planet, an
 * intermediate shaft's wheel and pinion, a carrier and the sun of the next stage). A shaft on a carrier has its axis
 * carried round by the carrier's rotation (a planet). A shaft is fixed (part of the housing: a fixed ring), locked
 * (it turns with the train, but is held where the train puts it while the load is applied: an output held to measure
 * the train's stiffness), floating (free to move sideways in the plane: a floating sun) or free; a torque may act on
 * it.
 *
 * Kinematics. A mesh between gears A and B on shafts a and b whose axes are fixed in the frame of carrier c (or of the
 * housing) ties their turns: z_A (theta_a - theta_c) = -/+ z_B (theta_b - theta_c), minus for two external gears, plus
 * for a gear inside a ring. With the fixed shafts at rest and the input turned by 1, the ties give every shaft's ratio;
 * the train must have exactly one degree of freedom and the ties must agree (a planetary's planets do). The train is
 * placed at an input angle by these ratios.
 *
 * Phasing. At the reference pose each gear's teeth are turned so that, along the line of centres of every mesh, one
 * gear's tooth space is centred on the other's tooth (the backlash split evenly). Gears are phased one mesh at a time
 * from a gear whose phase is given (or the first gear); a mesh whose two gears are both phased already is checked, and
 * a train that cannot be assembled (planets that cannot be spaced as asked, a compound planet whose two gears cannot
 * both mesh) is refused with the mesh and its misfit. The gears on one shaft are phased independently: each compound
 * planet is timed to fit, as such planets are assembled.
 *
 * Loads. The contact problem's rigid degrees of freedom are every shaft's rotation (absolute; a planet's axis moves
 * with its carrier's), a floating or elastically supported shaft's two translations (a planet's relative to its pin,
 * against its bearing's stiffness); fixed and locked shafts are held. Each free one is in
 * equilibrium between its torque and its gears' contacts; a carrier's through its planets' pins. Both flanks of every
 * mesh are candidates, so a train finds which side of each mesh carries the load (and closes the backlash on it).
 * Reported: every shaft's wind-up (its rotation beyond the rigid closure of the backlash), the contacts' torque on it
 * (on a held one, the reaction), the contacts' axial force on it (helical gears), every mesh's force.
 *
 * Pose. The contacts are found, and their gaps and their changes with the rigid motions taken, at a pose: the
 * kinematic one moved by every rigid degree of freedom's offset. A solve gives the rigid motions from that pose; the
 * pose moves by them and the contacts are found again there, until a solve moves no gear's teeth by more than a
 * nanometre, or, where a contact switches at the pose, until the steps, halved, close in on it (twice: under a
 * thousandth of the loads, the backlash's rigid closure, and under the loads, each from the last solve's pose). A
 * train of high ratio turns its input degrees beyond its kinematic angle before it carries its load, because the
 * backlash and compliance of its far meshes are seen through the ratio (about 5 degrees for the 69:1 Wolfrom stage of
 * examples/lab/robot_wolfrom_stage.json); solved at the kinematic pose, the gaps of tooth corners,
 * linear in the turns only near where they were found, closed where the teeth do not touch, and such a train jammed or
 * lost most of its torque (2026-10-22, geartest M22).
 *
 * Kept out: friction, the compliance of shafts, bearings, pins and carriers (all rigid), dynamics, gravity. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gear.h"
#include "gearmesh.h"
#include "mgearpart.h"

enum { TR_MAX_MODELS = 16, TR_MAX_SHAFTS = 32, TR_MAX_GEARS = 40, TR_MAX_MESHES = 64 };

typedef struct TrainModelSpec {
    char name[32];
    GearSpec gear;
    bool internal;           /* a ring, cut by `cutter` */
    GearSpec cutter;
    GearMeshSpec mesh;       /* teeth_modelled: the whole gear, or a sector */
    GearHold hold;           /* where its bore (a ring's outer rim) is held; lugs 0: all round */
    double E, nu;
} TrainModelSpec;

typedef struct TrainShaftSpec {
    char name[32];
    double x, y;             /* the axis at the reference pose, m (on a carrier: with the carrier at 0) */
    int carrier;             /* the shaft that carries this axis round, or -1 */
    bool fixed, locked, floating;
    double support_n_per_m;  /* > 0: its axis held by a spring of this stiffness in every direction of the plane (a
                                planet's bearing on its pin, a shaft's bearings), two more rigid degrees of freedom */
    double torque_nm;        /* applied about its axis, counter-clockwise seen from +z */
    double pin_error_m[2];   /* a carried axis out of place: along the carrier's rotation, and outward */
    bool plate_front;        /* pictures only: a carrier's plate on the +z side of its planets (default behind them) */
} TrainShaftSpec;

typedef struct TrainGearSpec {
    char name[32];
    int model, shaft;
    double z0_m;             /* the face's start along the axis */
    double phase_deg;        /* the polar angle of the middle tooth's centre line at the reference pose (z0); NAN:
                                phased by its meshes */
} TrainGearSpec;

typedef struct TrainMeshSpec {
    int a, b;                /* gears */
} TrainMeshSpec;

typedef struct TrainSpec {
    TrainModelSpec model[TR_MAX_MODELS];
    TrainShaftSpec shaft[TR_MAX_SHAFTS];
    TrainGearSpec gear[TR_MAX_GEARS];
    TrainMeshSpec mesh[TR_MAX_MESHES];
    int nmodels, nshafts, ngears, nmeshes;
    int input;               /* the shaft whose turn places the train */
    double gap_cut_m;        /* candidate contacts: gaps up to this above a flank pairing's smallest; default 60 um */
    double range_deg[2];     /* the input's turns the solves will use: a gear held at lugs has the compliance of only
                                the teeth that come into mesh over them (solving elsewhere is refused) */
} TrainSpec;

typedef struct TrainResult {
    double input_deg;
    int contacts, active, pivots;
    int poses;                            /* the contact searches it took to settle (closure and load together) */
    double settle_m;                      /* how far the last solve would still move the teeth: under 1 nm, more where
                                             the pose sits on a contact that switches there (mtrain.c settle) */
    double turn_rad[TR_MAX_SHAFTS];       /* each shaft's kinematic turn */
    double offset_rad[TR_MAX_SHAFTS];     /* its rotation beyond that where the train settled under its loads (the
                                             backlash's closure and the wind-up) */
    double energy_j;                      /* the elastic energy then stored in the contacts' compliance (the parts' and
                                             the near field's, taken at its load) and in the soft springs and bearings */
    double windup_rad[TR_MAX_SHAFTS];     /* its rotation beyond the rigid closure of the backlash */
    double torque_nm[TR_MAX_SHAFTS];      /* the contacts' torque on it about its axis (a carrier's through its pins);
                                             a free shaft's balances its applied torque, a held one's is the reaction */
    double axial_n[TR_MAX_SHAFTS];        /* the contacts' force on its gears along +z */
    double force_n[TR_MAX_SHAFTS][2];     /* the contacts' force on its gears in the plane (what its bearings carry) */
    double float_m[TR_MAX_SHAFTS][2];     /* a floating or supported shaft's displacement (a planet's from its pin) */
    double mesh_force_n[TR_MAX_MESHES];   /* each mesh's normal forces summed */
    double mesh_axial_n[TR_MAX_MESHES];   /* their components along +z on the mesh's gear a */
    int mesh_side[TR_MAX_MESHES];         /* the flank pairing that carries it: 0 left (of gear a), 1 right, -1 none */
    double mesh_side_force_n[TR_MAX_MESHES][2]; /* its normal forces on each pairing: gear a's left, right flank */
    double max_mises_pa, gear_max_mises_pa[TR_MAX_GEARS];
    double closure_seconds, solve_seconds, search_seconds;
} TrainResult;

typedef struct Train Train;

Train *train_create(const TrainSpec *spec, char *err, size_t errlen);
void train_free(Train *t);
/* a shaft's kinematic turn per turn of the input */
double train_ratio(const Train *t, int shaft);
/* the train at an input angle (degrees), loaded; with fields, also every gear's stress for a frame */
bool train_solve(Train *t, double input_deg, TrainResult *res, bool fields, char *err, size_t errlen);
/* the gears of the last solve with fields: each one's name, world node positions, cells, von Mises per element and
 * per node */
int train_gears(const Train *t);
void train_gear(Train *t, int i, const char **name, const double **xyz, int *nnodes, const int **conn, int *nelems, const float **mises,
                const float **node_mises);
/* the rigid parts that are not gears, for pictures (carrier plates and pins, shaft hubs), at the last solve's pose */
int train_visuals(const Train *t);
void train_visual(Train *t, int i, const char **name, const double **xyz, int *nnodes, const int **conn, int *nelems);
const char *train_summary(const Train *t);
const TrainSpec *train_spec(const Train *t);
