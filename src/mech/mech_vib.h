/* mech_vib.h - vibration of parts for the MCP layer
 *
 * mech_modal_run: natural modes of a meshed part, free-free or clamped on selections, with the part's isotropic project
 * material or an orthotropic printed-material record. Job kind "mech_modal".
 *
 * mech_transient_assess: the elastic response of a part while it moves with a rigid dynamics run. The loads on the part
 * (joint wrenches, persistent contact forces and d'Alembert body forces of the rigid motion) are sampled over a time window
 * and written as fixed spatial patterns times histories: 6 patterns per attachment (unit force and moment about the
 * selection centroid) and 12 inertial patterns (specific force, angular acceleration and the six products of angular
 * velocity components). The response uses the mode-acceleration method: the exact quasi-static solution of each pattern
 * (isostatic 3-2-1 support, one factorisation) plus the dynamic correction sum phi_i (q_i - q_i,static) of the free-free
 * elastic modes, integrated exactly for piecewise-linear loads. Stresses therefore converge with the static solution, and
 * the modes only carry the dynamic part. Job kind "mech_transient". */
#pragma once

#include "mech_struct.h"
#include "modal.h"

typedef struct VibMaterial {
    int nmat;
    OrthoConstants *mat;       /* owned */
    int *elem_mat;             /* owned */
    double R[9];
    bool ortho;
    MechStructSetup setup;     /* orthotropic record and criterion (owned), when ortho */
} VibMaterial;

/* the elastic model of an extracted part: its project materials, or the orthotropic record (takes ownership of *ortho) */
bool vib_material_from(const StaticModel *sub, MechStructSetup *ortho, VibMaterial *vm, char *err, size_t errlen);
void vib_material_free(VibMaterial *vm);

typedef struct MechModalJob {
    StaticModel model;
    VibMaterial mat;
    double *density;
    unsigned char *fixed;      /* NULL = free-free */
    int elastic_modes;
    JsonValue *setup;
    char run_dir[NV_PATH_MAX], job_id[64];
} MechModalJob;

typedef struct MechTransientJob {
    StaticModel model;
    VibMaterial mat;
    double *density;
    int elastic_modes;
    double zeta;
    int npat, nsteps;
    double dt, t0;
    double *patterns;          /* npat * 3 * nnodes */
    double *hist;              /* npat * (nsteps + 1) */
    JsonValue *report;
    char run_dir[NV_PATH_MAX], job_id[64];
} MechTransientJob;

/* stresses of a flexible body from the elastic coordinates recorded by a dynamics run: sigma(t) = sum eta_k(t) sigma_k with the
 * coordinate shapes of its reduction (mech_flex_shapes.bin). Job kind "mech_flexible_stress". */
typedef struct MechFlexStressJob {
    char shapes_path[NV_PATH_MAX];
    int ncoord, nrows;
    double *t;                 /* nrows */
    double *eta;               /* nrows * ncoord */
    double record_period;
    JsonValue *report;         /* setup: run, body, reduction, window, reduction losses */
    char run_dir[NV_PATH_MAX], job_id[64];
} MechFlexStressJob;

bool mech_flex_stress_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void mech_flex_stress_job_free(void *data);

bool mech_modal_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void mech_modal_job_free(void *data);
bool mech_transient_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void mech_transient_job_free(void *data);

void op_mech_vibration_query(Engine *e, JsonValue *p, OpResult *out);
