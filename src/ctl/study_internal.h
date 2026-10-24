/* study_internal.h - shared pieces of the comparison-study implementation (study_*.c) */
#pragma once

#include <stdio.h>

#include "engine_internal.h"
#include "jobs.h"
#include "study.h"

/* ---- findings of a check: questions (blocking or not), assumptions, unsupported requests, exclusions, warnings ---- */
typedef struct StudyIssues {
    JsonValue *questions, *assumptions, *unsupported, *not_evaluated, *warnings, *accepted;
    const JsonValue *accept; /* definition.accept: [{id, reason}] */
    int blocking;
    bool not_supported;
} StudyIssues;

void si_init(StudyIssues *is, const JsonValue *accept);
void si_free(StudyIssues *is);
/* a question for the user. acceptable questions can be answered by accepting them (definition.accept) with a reason; an
 * accepted question becomes a user-sourced assumption instead */
void si_question(StudyIssues *is, const char *id, bool blocking, bool acceptable, const char *why, JsonValue *details, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));
void si_assume(StudyIssues *is, const char *subject, const char *source, const char *effect, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void si_unsupported(StudyIssues *is, const char *id, bool blocking, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void si_not_evaluated(StudyIssues *is, const char *item, const char *reason);
void si_warn(StudyIssues *is, const char *code, JsonValue *details, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* ---- operations on private engines, logged ---- */
typedef struct StudyLog {
    FILE *f;         /* operations.jsonl, or NULL */
    JsonValue *mem;  /* array collecting {design, operation, params}, or NULL */
    char design[64];
} StudyLog;

/* runs op on eng with params (consumed); false with r->error set when the operation fails. Free r with op_result_free. */
bool study_op(Engine *eng, StudyLog *lg, const char *op, JsonValue *params, OpResult *r);
/* "code: message" of a failed operation */
void study_op_error(const OpResult *r, char *out, size_t cap);

/* ---- resolution ---- */
/* definition -> resolved study; path problems (not found, outside the read roots) fail with code/err, everything else
 * becomes issues */
JsonValue *study_resolve(Engine *e, const JsonValue *def, StudyIssues *is, NvErr *code, char *err, size_t errlen);
const char *study_project_source(const char *study_source); /* user | inferred | default for project entries */

/* ---- one design on its private engine ---- */
typedef struct StudyDesign {
    char name[64];
    Engine *eng;
    bool ready;                 /* project, geometry, material, regions and conditions are set up */
    double t_mm[3];             /* build frame = design frame + t */
    double volume_mm3, bbox_min[3], bbox_max[3]; /* design frame */
    double thickness_min_mm, thickness_p05_mm;
    /* regions in the design frame */
    double mount_area, mount_c[3], mount_n[3];
    double load_area, load_c[3], load_n[3];
    JsonValue *inspection;      /* geometry and regions summary for the report */
    unsigned char *preview_png;
    size_t preview_len;
} StudyDesign;

/* creates the engine, the project in project_dir and the complete setup; questions go to is */
bool study_design_setup(StudyDesign *d, const EngineConfig *cfg, const JsonValue *resolved, int index, const char *project_dir, StudyLog *lg,
                        StudyIssues *is);
void study_design_free(StudyDesign *d);
/* equivalence of the mounting and load conditions of every design against the first */
JsonValue *study_equivalence(const StudyDesign *d, int n, const JsonValue *resolved, StudyIssues *is);
/* meshes the coarsest level and validates the setup; returns the mesh summary (NULL when meshing failed) */
JsonValue *study_coarse_check(StudyDesign *d, const JsonValue *resolved, StudyLog *lg, StudyIssues *is);

/* ---- execution ---- */
typedef struct StudyJob {
    EngineConfig cfg;
    JsonValue *resolved;        /* owned */
    JsonValue *check;           /* the check report at submission (owned) */
    char dir[NV_PATH_MAX];
    char hash[65];
    char reference_dir[NV_PATH_MAX]; /* replay: the study whose evidence to reproduce ("" otherwise) */
} StudyJob;

bool study_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void study_job_free(void *data);

/* ---- evidence ---- */
/* refinement of one quantity over valid levels (element sizes descending): changes, the convergence criterion and, when its
 * conditions hold, a discretisation-error estimate. verr_percent: each mesh's volume error against the geometry (may be NULL) */
JsonValue *study_refinement(const double *h_mm, const double *q, const double *verr_percent, int n, double criterion);
/* the evidence record from the per-design level and sensitivity records */
JsonValue *study_build_evidence(const StudyJob *sj, StudyDesign *d, int nd, JsonValue **levels, JsonValue **sensitivity, JsonValue *failures,
                                const char *status, double wall_seconds);
/* compares the quantities of two evidence records level by level */
JsonValue *study_compare_replay(const JsonValue *reference, const JsonValue *replay);

/* ---- storage ---- */
/* estimated bytes of one analysis record (results.nvr, spec.json, summary.json) for a mesh of about `elements` elements */
double study_run_bytes(double elements);
/* free space kept in reserve by the free-space checks */
double study_storage_reserve_bytes(void);
/* free bytes on the file system of path, or of its nearest existing ancestor (reported in measured_at); -1 when unknown */
double study_free_bytes(const char *path, char *measured_at, size_t cap);
/* preflight storage plan: estimated retained and peak bytes for the retention setting, free space at dir, sufficiency */
JsonValue *study_storage_plan(const JsonValue *resolved, const double *volume_mm3, int nd, const char *dir);

double study_now(void);
void study_hash_json(const JsonValue *v, char out[65]);
