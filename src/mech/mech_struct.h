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

/* walls and infill of a printed part (GOALS.md G17 step 3): solid printed walls of the given thicknesses around an
 * infill core whose constants and strengths follow from its pattern and density (src/mech/infill.h) */
typedef struct PrintLayout {
    bool on;
    int perimeters, top_layers, bottom_layers;
    double line_width, layer_height;  /* m */
    double density;                   /* infill: the core's solid fraction, (0, 1] */
    char pattern[24];
    OrthoConstants core;              /* in the material axes of the part */
    OrthoStrength core_strength;      /* valid when the material has strengths */
    JsonValue *rules;                 /* how the core follows from the pattern and density, repeated in every result */
} PrintLayout;

typedef struct MechStructSetup {
    MechMaterial mat;
    double R[9];                    /* material axes (columns) in the FEM build frame */
    StrengthCriterion criterion;    /* used when the material has strengths */
    JsonValue *description;         /* the resolved material model, for reports and the specification hash */
    PrintLayout layout;             /* walls and infill; off for a solid part */
    double raster_ref[3], raster_angle; /* as given, so that other build directions can be solved (rad) */
    bool compare_orientations;      /* also solve the three principal build directions (GOALS.md G17 step 4) */
    JsonValue *inputs;              /* the supports and loads, each with its kind and who set it: the answer's provenance */
} MechStructSetup;

/* Walls and infill (GOALS.md G17 step 3): the fraction of each voxel element that lies in the printed walls, measured
 * along the grid axes from the part's boundary: within t_side of a face across the layers (perimeters x line width),
 * within t_top of an upward face and t_bottom of a downward one (top and bottom layers x layer height). build_axis is
 * 0, 1 or 2 and build_sign +1 or -1 (the build direction is that axis); NULL with a reason for a mesh that is not a
 * voxel mesh. The caller frees the array (nelems values in [0, 1]). */
double *mech_wall_fractions(const StaticModel *m, int build_axis, int build_sign, double t_side, double t_top, double t_bottom, char *err,
                            size_t errlen);

/* material_model {type: "orthotropic", material, build_direction, raster_reference, raster_angle, criterion,
 * compare_orientations}; fails the
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

/* the printed part in a static analysis (GOALS.md G17): resolves material_model {type: "orthotropic", ...} for a voxel
 * static model and lets static_job_run solve it with the orthotropic elements and the strength criterion (failure
 * index per Gauss point, the largest around each node, and a strength summary). Fails the operation with a hint. */
bool mech_static_printed_attach(const Project *p, const JsonValue *mm, StaticResults *r, OpResult *out);
const JsonValue *mech_static_printed_description(const StaticResults *r); /* the resolved model, for the run's hash */
void mech_struct_job_free(void *data);

void op_mech_material_define(Engine *e, JsonValue *p, OpResult *out);
void op_mech_structure_query(Engine *e, JsonValue *p, OpResult *out);

/* assumptions of an orthotropic assessment, appended to the fidelity description */
void mech_struct_assumptions(JsonValue *assumptions, const MechStructSetup *s);
