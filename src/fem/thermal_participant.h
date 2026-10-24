/* thermal_participant.h - a thermal model with its transactional integrator as an orchestrator participant
 *
 * Fields:
 *   "temperature"            nnodes      export: nodal temperature (K) of the trial candidate or of the accepted state
 *   "interface_temperature"  B * ni      export: temperature at the interface nodes (K), intensive
 *                                        import: holds those nodes at the given temperatures (Dirichlet)
 *   "interface_heat_out"     B * ni      export: heat that LEFT the model through each interface node (J), from the
 *                                        reactions of the held nodes: a conservative quantity
 *   "interface_heat_in"      B * ni      import: heat that ENTERS each interface node (J), applied as a constant nodal
 *                                        power E / duration (Neumann)
 *   "velocity"               3 * nnodes  import: the advecting velocity (m/s) at every node, when the adapter was given
 *                                        writable velocity storage (the model's velocity array)
 * ni is the number of interface nodes. Interface fields are resolved per trial STAGE (thermal_integrator.h): B = 3
 * blocks [full step | first half step | second half step] for a participant that estimates its temporal error, B = 1
 * otherwise. Each solve of a step-doubling trial then uses the data of the matching solve of its partner, so a converged
 * partitioned trial reproduces both solutions of the monolithic estimate and the estimate itself. When a trial did not
 * estimate (fixed stepping) its three blocks are derived from the single solve: temperatures at the end of the step,
 * at the half step by linear interpolation from the accepted state; heat split in proportion to duration.
 *
 * The interface node lists of two coupled participants must correspond one to one (matching meshes); the transfer is
 * then the identity and a conservative transfer preserves the sum exactly. Nodes that import a temperature must be
 * marked fixed in the model from the start (the set of prescribed nodes must not change during a run). */
#pragma once

#include <stdbool.h>

#include "orchestrator.h"
#include "thermal_integrator.h"

typedef struct ThermalParticipant {
    /* set by the caller */
    const char *name;
    ThermalIntegrator *ti;
    ThermalModel *m;             /* the model the integrator steps */
    unsigned char *fixed;        /* writable m->fixed storage (nnodes) */
    double *fixed_T;             /* writable m->fixed_T storage (nnodes) */
    double *node_power;          /* writable m->node_power storage (nnodes), needed to import heat */
    double *velocity;            /* writable m->velocity storage (3 nnodes), needed to import a velocity; NULL: none */
    int ninterface;
    const int *interface_nodes;  /* local node ids */
    bool estimate;               /* adaptive: estimate the temporal error of trials */
    /* owned by the adapter (thermal_participant_bind / thermal_participant_release) */
    int blocks;
    double *T_in, *heat_in;      /* blocks * ninterface */
    bool T_imported, heat_imported;
    ThermalTrial trial;
    bool pending;
} ThermalParticipant;

/* allocates the import buffers, installs the stage hook and fills the OrchParticipant callbacks */
bool thermal_participant_bind(ThermalParticipant *tp, OrchParticipant *out, char *err, size_t errlen);
void thermal_participant_release(ThermalParticipant *tp);
