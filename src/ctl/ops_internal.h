/* ops_internal.h - helpers for operation handlers (the engine lock is held while a handler runs) */
#pragma once

#include "../core/units.h"
#include "engine_internal.h"
#include "viewrender.h"

typedef void (*OpHandlerFn)(Engine *e, JsonValue *params, OpResult *out);

typedef struct OpBinding {
    const char *name;
    OpHandlerFn fn;
} OpBinding;

extern const OpBinding OPS_PROJECT_BINDINGS[];
extern const OpBinding OPS_GEOMETRY_BINDINGS[];
extern const OpBinding OPS_SURFACE_BINDINGS[];
extern const OpBinding OPS_VIEW_BINDINGS[];
extern const OpBinding OPS_SETUP_BINDINGS[];
extern const OpBinding OPS_MESH_BINDINGS[];
extern const OpBinding OPS_ANALYSIS_BINDINGS[];
extern const OpBinding OPS_RESULTS_BINDINGS[];
extern const OpBinding OPS_CONTACT_BINDINGS[];
extern const OpBinding OPS_STUDY_BINDINGS[];
extern const OpBinding OPS_TOPOPT_BINDINGS[]; /* topology optimisation: src/ctl/ops_topopt.c */
extern const OpBinding OPS_MECH_BINDINGS[]; /* mech-integration: src/mech/mech_ops.c */

void op_fail(OpResult *r, NvErr code, const char *hint, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void op_fail_detail(OpResult *r, const char *key, JsonValue *v); /* attaches details to the current error */
void op_succeed(OpResult *r, JsonValue *value);
bool op_need_project(Engine *e, OpResult *r);
/* resolves the body named by params[key] (or the default part); fails the result when missing */
Body *op_need_body(Engine *e, const JsonValue *params, const char *key, OpResult *r);
/* quantity in SI from params[key]; *present tells whether the key was given */
bool op_quantity(OpResult *r, const JsonValue *params, const char *key, Dimension dim, const char *default_unit, double *si,
                 bool *present);
bool op_attach_image(OpResult *r, const char *mime, const char *name, unsigned char *data, size_t len); /* takes data */
/* rendered views (ops_view.c): parse camera/size options; store the view, attach its PNG and describe it in value */
bool op_parse_view(OpResult *out, const JsonValue *params, ViewOptions *o);
void op_finish_view(Engine *e, OpResult *out, ViewResult *vr, const char *kind, const char *job_id, const char *image_name, JsonValue *value);
/* transient thermal and thermomechanical analyses (ops_transient.c) */
void op_thermal_submit(Engine *e, JsonValue *params, OpResult *out, const char *analysis);
bool op_thermal_job(Engine *e, const char *job_id); /* true when the job (or its run directory) is a transient analysis */
void op_thermal_query(Engine *e, JsonValue *params, OpResult *out);
void op_thermal_probe(Engine *e, JsonValue *params, OpResult *out);
void op_thermal_render(Engine *e, JsonValue *params, OpResult *out);
void op_thermal_export(Engine *e, JsonValue *params, OpResult *out);
void op_thermal_resume(Engine *e, JsonValue *params, OpResult *out);
void op_thermal_interface_results(Engine *e, JsonValue *params, OpResult *out);
typedef struct ThermalSettings ThermalSettings;
bool op_thermal_settings(OpResult *out, const JsonValue *params, const char *analysis, ThermalSettings *s);

/* run directory runs/<job_id> of the open project, when it exists (ops_analysis.c; takes the engine lock briefly) */
bool op_project_run_dir(Engine *e, const char *id, char *out, size_t cap);
