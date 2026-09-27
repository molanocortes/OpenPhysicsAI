/* mech_flex.h - flexible bodies for the MCP layer: the Craig-Bampton reduction of a body's meshed part as a job
 *
 * mech_flexible_reduce clamps the part at a root selection (where the body's own joint holds it), makes each named child
 * joint's selection a rigid interface at that joint's origin, and reduces the part with flexbody.c. The job writes
 * flexible_model.json (the assembly's "flexible" block, see assembly.h) and summary.json (frequencies, interface static
 * compliance, mass properties of the mesh against the body's, the stability step, assumptions). mech_flexible_attach
 * copies the block into the assembly; the dynamics runtime then integrates the elastic coordinates with the rigid motion.
 * Job kind "mech_flexible". */
#pragma once

#include "mech_vib.h"
#include "multibody.h"

typedef struct MechFlexJob {
    StaticModel model;             /* the extracted part */
    VibMaterial mat;
    double *density;
    int *root, nroot;
    int ninterfaces;
    int **itf_nodes, *itf_count;
    double (*itf_point)[3];        /* FE coordinates */
    char (*itf_joint)[MB_NAME];
    int fixed_modes;
    double zeta, max_frequency_hz;
    double R[9], p[3];             /* body -> FE placement */
    double body_mass, body_com[3], body_inertia[9]; /* the body's mass properties before the reduction (compared, not used) */
    bool body_has_mass;
    JsonValue *setup;              /* provenance: body, part, selections, damping, material model, mesh hash */
    char run_dir[NV_PATH_MAX], job_id[64];
} MechFlexJob;

bool mech_flex_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void mech_flex_job_free(void *data);
