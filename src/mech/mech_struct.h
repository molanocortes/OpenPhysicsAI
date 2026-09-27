/* mech_struct.h - printed-material records and orthotropic structural assessment jobs for the MCP layer
 *
 * Material records live in <project>/mechanics/materials.json (the shared material library is isotropic and is not
 * edited). A record holds orthotropic elastic constants with their source, an optional density and optional strengths
 * with their source. Sources are measured, datasheet, literature, calibrated, user or assumed; results repeat them.
 *
 * An orthotropic assessment solves the part with src/mech/ortho.c and stores mech_structure.bin (mesh, displacements,
 * Gauss-point stresses in global and material axes, strains, failure indices) and summary.json in the run directory. Its
 * job kind is "mech_structural": the shared result operations refuse it, and mech_structure_query reads it. */
#pragma once

#include "../ctl/ops_internal.h"
#include "../ctl/static_analysis.h"
#include "ortho.h"

typedef struct MechMaterial {
    char id[64];
    OrthoConstants k;
    double density;                 /* kg/m^3, 0 = not given */
    bool has_strength;
    OrthoStrength strength;
    char elastic_source[24], strength_source[24];
    JsonValue *record;              /* canonical record (SI values as strings with units) */
} MechMaterial;

bool mech_material_find(const Project *p, const char *id, MechMaterial *out, char *err, size_t errlen);
void mech_material_free(MechMaterial *m);

typedef struct MechStructSetup {
    MechMaterial mat;
    double R[9];                    /* material axes (columns) in the FEM build frame */
    StrengthCriterion criterion;    /* used when the material has strengths */
    JsonValue *description;         /* the resolved material model, for reports and the specification hash */
} MechStructSetup;

/* material_model {type: "orthotropic", material, build_direction, raster_reference, raster_angle, criterion}; fails the
 * operation with a hint when something is missing or inconsistent */
bool mech_struct_setup(const Project *p, const JsonValue *mm, MechStructSetup *s, OpResult *out);
void mech_struct_setup_free(MechStructSetup *s);

typedef struct MechStructJob {
    StaticModel model;              /* the assessed part with its loads and isostatic support (owned) */
    MechStructSetup setup;          /* owned */
    JsonValue *report;              /* the assessment report (owned) */
    char run_dir[NV_PATH_MAX];
    char job_id[64];
} MechStructJob;

bool mech_struct_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void mech_struct_job_free(void *data);

void op_mech_material_define(Engine *e, JsonValue *p, OpResult *out);
void op_mech_structure_query(Engine *e, JsonValue *p, OpResult *out);

/* assumptions of an orthotropic assessment, appended to the fidelity description */
void mech_struct_assumptions(JsonValue *assumptions, const MechStructSetup *s);
