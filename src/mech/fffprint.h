/* fffprint.h - layer-by-layer fused-filament (FFF) print simulation: how a printed part heats, cools and builds up stress
 * while it grows, and how it warps when it comes off the bed.
 *
 * A prototype for NAVIER-AM milestone 5 ("FFF activation, cooling and bed release"), built on the verified core without
 * changing it: the thermal solver's element activation carries the heat balance, and the solid solver's per-element
 * modulus scaling and initial strain carry an incremental thermo-elastic stress history.
 *
 * Process model
 *   Layers. The voxel mesh is cut into simulation layers by element centroid height. A simulation layer lumps several
 *   printed layers and is deposited at once with the nozzle enthalpy; it then cools for the time the
 *   printer needs to lay it down: its volume over the volumetric deposition rate. Material is deposited only where it
 *   rests on the bed (a whole face on it) or shares a face with printed material; an element that does not yet (an
 *   overhang on the voxel staircase) waits for a later layer, and fragments that never connect are not printed (both
 *   are counted in the summary). This also keeps every printed region free of hinges and rigid-body motions.
 *   Conforming interface nodes retain their previous temperature. The missing nozzle enthalpy is integrated at the
 *   new elements' Gauss points and supplied as a nodal heat pulse in the first thermal substep. The pulse on prescribed
 *   bed nodes goes straight into the bed heat ledger. Its finite duration follows the numerical first substep, so local
 *   deposition histories still require time refinement; it does not resolve extruded roads or thermal contact resistance.
 *   Heat. Conduction in the part with k(T), rho(T) cp(T); convection h and grey radiation to the chamber air from every
 *   exposed face of the printed region; the heated bed as a prescribed temperature at the bottom nodes. After the last
 *   layer the part cools with the bed on, then with the bed switched off (the bed follows the ambient temperature).
 *   Stress. Small strain, incremental: every increment solves K du = f(d eps_th) + f_relax on the printed elements with
 *   the bed nodes held. Between an element's previous and current mean temperature the increment uses the free thermal
 *   strain integral alpha(T) dT and the alpha-weighted modulus of that path, integral E(T) alpha(T) dT / integral
 *   alpha(T) dT (E with a floor, so molten material stays in the system). That pair is exact for a fully restrained
 *   element (stress integral E alpha dT, so a coarse increment through the glass transition does not apply the hot
 *   contraction at the cold modulus) and for a free one (its thermal strain, so reheating and cooling again is
 *   reversible). The part of the path above the relaxation temperature adds neither strain nor stress: the material
 *   flows there, and material cooling through that temperature starts stress-free. The stresses accumulate. New material is born stress-free at its nominal (as-programmed) position,
 *   on a part that has already contracted and deformed: that mismatch is what makes printed parts warp. Material
 *   reheated above the relaxation temperature loses its accumulated stress (fast viscous relaxation above the glass
 *   transition), and the internal forces it held are redistributed in the next increment.
 *   Bed release. The accumulated bed reactions are applied reversed to the part on an isostatic support: the part
 *   takes its free, warped shape and keeps the residual stresses that shape cannot relieve.
 *
 * Not modelled: the toolpath inside a simulation layer, creep below the relaxation temperature, plasticity, interlayer
 * bond strength and anisotropy, crystallisation shrinkage, adhesion failure during the print, gravity. Supports can be
 * supplied as homogenised bands, with conductivity, density, stiffness and exposed pattern area stated by the caller. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../ctl/matlib.h"

typedef struct FffMaterial {
    MatTable rho, cp, k, E, alpha, emissivity; /* K and SI, as in matlib */
    double nu;
    double T_relax; /* K: accumulated stress relaxes above this temperature (e.g. a little above the glass transition) */
    double E_floor; /* Pa: lowest modulus given to hot material */
} FffMaterial;

typedef struct FffProcess {
    double layer_height;               /* m, simulation layer */
    double T_nozzle, T_bed, T_ambient; /* K */
    double h_conv;                     /* W/(m^2 K) on exposed faces (part-cooling fan and chamber air) */
    bool radiation;                    /* grey radiation with the material emissivity */
    double deposition_rate;            /* m^3/s of material laid down */
    double min_layer_time;             /* s */
    double cooldown_bed_on, cooldown_bed_off; /* s after the last layer */
    int thermal_substeps;              /* per layer, geometric in time (>= 2) */
    bool frame_every_substep;          /* a frame (and a stress increment) at every substep, not only the first and last */
    /* numerics, not process: the structural solver of an increment and the temperature change below which an increment
     * is skipped (its thermal strain is not lost, it is integrated by the next increment that runs) */
    int solver;      /* SolidSolver: 0 automatic, 1 direct, 2 iterative */
    double pcg_tol;  /* relative residual of the iterative solver (0: 1e-10) */
    double skip_dt;  /* K, 0: never skip */
} FffProcess;

typedef struct FffMesh {
    int nnodes, nelems;
    const double *xyz; /* 3 * nnodes, m; z is the build direction and the bed is the lowest node plane */
    const int *conn;   /* 8 * nelems, hex8 */
    /* optional homogenised supports (docs/contracts/supports.md): support_band[e] is the band of a support element,
     * -1 for the part; band_props holds 6 values per band: conductivity fractions along x, y, z, the capacity fraction
     * (solid fraction x relative density: it scales the density), the stiffness fraction and the pattern's surface per
     * volume (1/m). Supports are deposited and heated like the part, conduct and store heat as their band, and exchange
     * heat with the air over their own surface (convection and radiation spread over the faces of each support element,
     * in place of the voxel skin); they are removed after the bed release. */
    const int *support_band;
    int nbands;
    const double *band_props;
} FffMesh;

typedef struct FffFrame {
    int index, layer, nlayers;
    const char *stage;            /* "deposited", "layer cooled", "cool-down, bed on", "cool-down, bed off", "released" */
    double time;                  /* s since the first layer was deposited */
    const unsigned char *active;  /* nelems */
    const double *T;              /* nnodes, K */
    const double *u;              /* 3 * nnodes, m, accumulated */
    const double *vm;             /* nelems, Pa: mean of the eight Gauss-point von Mises values */
    double T_max, vm_max, u_max;
    double energy_balance;        /* worst relative thermal energy balance since the previous frame */
} FffFrame;

/* return false to stop the simulation */
typedef bool (*FffFrameFn)(const FffFrame *frame, void *ctx);
/* after every thermal step: time (s), nodal temperatures (K) and the printed elements */
typedef void (*FffStepFn)(double time, const double *T, const unsigned char *active, void *ctx);

typedef struct FffCallbacks {
    FffFrameFn frame; /* optional */
    FffStepFn step;   /* optional */
    void *ctx;
} FffCallbacks;

typedef struct FffSummary {
    int nlayers, frames, thermal_steps, mech_solves, elements, nodes;
    double print_time, total_time;          /* s */
    double peak_vm_bed, peak_vm_released;   /* Pa */
    double warp_z_max, warp_z_min;          /* m: vertical displacement range after release */
    double bed_reaction_total;              /* N: largest resultant component of the bed reactions before release */
    double release_support_reaction;        /* N: largest reaction on the isostatic support at release (self-equilibrated: ~0) */
    double equilibrium_error_last_solve;     /* recovered free-equation residual / nodal load, reaction and RHS norms */
    double equilibrium_error_at_release;     /* same diagnostic retained specifically for the bed-release solve */
    double worst_energy_balance;
    double seconds_thermal, seconds_mech;
    double printed_volume;    /* m^3 actually deposited */
    int skipped_increments;   /* increments whose largest element temperature change was below skip_dt */
    int unprintable_elements; /* never rest on the bed or on printed material: not deposited */
    int late_elements;        /* deposited after their own layer, when they first rested on printed material (overhangs) */
    double bed_heat;          /* J: heat into the bed (the prescribed-temperature nodes) over the whole simulation */
    double bed_heat_print;    /* J: the same until the last layer has cooled */
    double deposition_heat;  /* J: supplied nozzle enthalpy of all deposited material, relative to ambient */
    double deposition_correction; /* J: missing enthalpy at conforming nodes, supplied in first substeps or to the bed */
    double stored_heat;      /* J: final active part/support enthalpy relative to ambient, Gauss integrated */
    double air_heat;         /* J: net heat from material into chamber air */
    double removed_heat;     /* J: enthalpy removed with supports, relative to ambient */
    double deposition_balance; /* worst relative identity error of supplied = initial + correction */
    double global_heat_balance; /* |stored + air + bed + removed - deposition| / largest magnitude */
    int support_elements;     /* removed after the release */
    double tearoff_max, tearoff_sum; /* N: the supports' forces on the part just before removal */
} FffSummary;

bool fff_simulate(const FffMesh *mesh, const FffMaterial *mat, const FffProcess *proc, const FffCallbacks *cb, FffSummary *sum, char *err,
                  size_t errlen);

/* the deposition plan alone (geometry only): the simulation layer in which each element is deposited, -1 for never
 * (nelems; free()). Elements are cut into layers by centroid height; an element is deposited in the first layer at or
 * after its own in which it has a whole face on the bed or shares a face with deposited material. */
typedef struct FffPlanStats {
    int nlayers, unprintable, late;
} FffPlanStats;
int *fff_plan(const FffMesh *mesh, double layer_height, FffPlanStats *stats);
/* neighbour element across each of the 6 hex8 faces of every element (6 * nelems, -1: none); free() */
int *fff_face_neighbours(const FffMesh *mesh);

/* ---- the incremental thermo-elastic stress history, usable on its own (verification) ---- */
typedef struct FffMech FffMech;
/* bed_node: nnodes, 1 for nodes held by the bed */
FffMech *fff_mech_new(const FffMesh *mesh, const FffMaterial *mat, const unsigned char *bed_node, char *err, size_t errlen);
void fff_mech_free(FffMech *m);
/* deposits an element: stress-free at its nominal position, with its thermal strain measured from T_birth */
void fff_mech_activate(FffMech *m, int elem, double T_birth);
/* one increment to the nodal temperatures T (nnodes); bed held or free (after release). With skip_dt > 0 an increment
 * whose largest element temperature change is below it and that deposits nothing is skipped: skipped receives true and
 * the state is unchanged, and the next increment that runs integrates from the same previous temperatures. */
bool fff_mech_increment(FffMech *m, const double *T, char *err, size_t errlen);
bool fff_mech_increment_opt(FffMech *m, const double *T, double skip_dt, bool *skipped, char *err, size_t errlen);
/* removes the bed: applies the accumulated bed reactions reversed on an isostatic support */
bool fff_mech_release(FffMech *m, const double *T, char *err, size_t errlen);
/* a stiffness multiplier per element (homogenised supports), NULL clears it */
bool fff_mech_set_scale(FffMech *m, const double *scale);
/* removes elements after the release (the supports): what they carried is released onto the rest, the isostatic support
 * is chosen again on the part's lowest plane, and the part re-equilibrates. tear_max and tear_sum (N, may be NULL)
 * receive the largest and summed forces the removed elements exerted on the nodes they share with the rest. */
bool fff_mech_remove(FffMech *m, const int *elems, int n, const double *T, double *tear_max, double *tear_sum, char *err, size_t errlen);
const double *fff_mech_u(const FffMech *m);      /* 3 * nnodes */
const double *fff_mech_stress(const FffMech *m); /* 48 * nelems: 8 Gauss points x [xx yy zz xy yz zx] */
const unsigned char *fff_mech_active(const FffMech *m);
void fff_mech_von_mises(const FffMech *m, double *vm); /* nelems, Gauss-point mean */
double fff_mech_bed_reaction(const FffMech *m, double resultant[3]); /* largest |component| of the resultant */
double fff_mech_release_reaction(const FffMech *m);
/* The structural solver's last successful equilibrium diagnostic, preserved across skipped increments. Its
 * force scale includes individual constrained reactions and the eigenstrain RHS, not the net bed resultant. */
double fff_mech_equilibrium_error(const FffMech *m);
double fff_mech_release_equilibrium_error(const FffMech *m);
int fff_mech_solves(const FffMech *m);
