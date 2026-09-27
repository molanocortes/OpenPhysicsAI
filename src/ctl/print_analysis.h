/* print_analysis.h - the layer-by-layer fused-filament print as an engine analysis (job kind "fff_print")
 *
 * print_case_build turns the persisted setup (meshed body, material assignment, process inputs) into a self-contained
 * case and refuses what it cannot answer for: a stale mesh, a material without provenance, a part that does not rest on
 * the build plate, several materials or filaments, a material record without the tables the print needs. The case is
 * printed on the job worker by src/mech/fffprint.c and the stored times are written into the ordinary time-indexed
 * result file (results.nvt), so every result operation and the application's stored-time slider read a print like a
 * transient thermal run: temperature, displacement and von Mises per stored time, the state after release from the bed
 * as the last one, and elem_birth saying at which stored time each element first exists.
 *
 * Scope: this is a process simulation on library material data. The summary states that in machine-readable form
 * (summary.scope); see docs/contracts/print-results.md. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "jobs.h"
#include "lpbf_build.h"
#include "project.h"
#include "transient_analysis.h"

enum { PRINT_MAX_PROBES = 16 };

typedef struct PrintSettings {
    char body[64];                            /* the printed body ("" = the only one) */
    double layer_height;                      /* m: the simulation layer, which lumps several printed layers */
    double nozzle_t, bed_t, ambient_t;        /* K */
    double h_conv;                            /* W/(m^2 K) on every exposed face */
    bool radiation;                           /* grey radiation with the material's emissivity */
    double deposition_rate;                   /* m^3/s */
    double min_layer_time;                    /* s */
    double cooldown_bed_on, cooldown_bed_off; /* s after the last layer */
    double relaxation_offset;                 /* K above the glass transition: stress relaxes above it */
    double printed_layer_height;              /* m: the printer's own layer, for the lumping factor in the summary */
    int thermal_substeps;                     /* per simulation layer (>= 2) */
    bool store_substeps;                      /* store every thermal substep instead of the key times */
    Provenance provenance;                    /* where the process values come from; refused when absent */
    Hex8Formulation formulation;
    SolidSolver solver;   /* structural solver of every increment (automatic by default) */
    double pcg_tol;       /* relative residual of the iterative solver */
    double skip_below_k;  /* K: skip an increment whose largest element temperature change is below this (0: never) */
    int nprobes;
    double probe[PRINT_MAX_PROBES][3]; /* m, build frame */
    char probe_label[PRINT_MAX_PROBES][64];
    /* supports (docs/contracts/supports.md): generated and homogenised as for the LPBF build, from the support fields
     * of sup; the part may then stand above the plate */
    bool supports;
    LpbfSettings sup;
} PrintSettings;

/* The job data. ThermalCase is the first member, so a PrintCase * is a ThermalCase *: the result operations get the
 * case they expect, and thermal_case_free frees the whole allocation (the extra members own no memory). */
typedef struct PrintCase {
    ThermalCase c;
    PrintSettings s;
    char material_id[64], material_status[24], material_name[160];
    double glass_transition_k, relaxation_k, strength_pa;
    /* filled by the run */
    int layers, layers_deposited, late_elements, never_deposited, printed_elements, thermal_steps, stress_increments, skipped_increments;
    double print_time, total_time;                  /* s of machine time */
    double peak_bed, peak_released;                 /* Pa */
    double warp_min, warp_max;                      /* m */
    double bed_reaction, release_reaction;          /* N */
    double worst_balance;                           /* relative */
    double seconds_thermal, seconds_stress, seconds_total;
    double volume;                                  /* m^3 of printed material */
    JsonValue *probes;                              /* histories, attached to the summary */
    LpbfCase *sup_stats;                            /* the supports as generated (NULL without) */
    double bed_heat, bed_heat_print;                /* J into the bed: whole run, and until the last layer cooled */
    double tearoff_max, tearoff_sum;                /* N: the supports' forces on the part before removal */
    int supports_removed;
} PrintCase;

/* errors and warnings receive {code, message, hint?} objects; true when the case can be printed */
bool print_case_build(Project *p, const PrintSettings *s, PrintCase **out, JsonValue *errors, JsonValue *warnings);
/* job entry point: prints the case, stores the times and writes results.nvt and summary.json into the run directory */
bool print_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void print_case_free(void *pc); /* JobFreeFn: frees the probe histories, then the case */
JsonValue *print_summary_json(const PrintCase *pc);
JsonValue *print_model_json(const PrintCase *pc);
/* "15 mm3/s", "0.9 cm3/min" or a plain number in m^3/s */
bool print_parse_volume_rate(const JsonValue *v, double *si, char *err, size_t errlen);
