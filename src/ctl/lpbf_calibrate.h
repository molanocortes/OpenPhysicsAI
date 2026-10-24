/* lpbf_calibrate.h - fit our own inherent strain to measured cantilever deflections (job kind "lpbf_calibrate")
 *
 * An inherent strain is not a material property: it belongs to the model that produced it, to its element size, its
 * layer thickness and its material law. A tensor calibrated in another tool is therefore a borrowed number, useful as
 * a cross-check and not as a prediction. This operation fits our own: given the measured tip deflection of one scan
 * strategy in both build orientations, it adjusts exx and eyy (ezz is held, because the calibration that produced the
 * reference data held it too) until the two simulated deflections match, on the same layer-by-layer model that every
 * other build uses and at a fixed discretisation.
 *
 * Method: the model is linear in the inherent strain, so the residual F(exx, eyy) = (tipX - measuredX, tipY -
 * measuredY) is very nearly affine. A bounded secant (finite-difference Jacobian, then Broyden updates) therefore
 * converges in one or two steps; the number of builds is capped and reported, and the whole iteration history is in
 * the summary so the fit can be read rather than believed.
 *
 * What it does not do: it does not fit ezz, it does not fit more strategies at once, and it fits deflection only, so
 * nothing here is evidence about stress. See docs/contracts/lpbf-build.md. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../core/json.h"
#include "jobs.h"
#include "lpbf_build.h"

typedef struct LpbfCalibration {
    LpbfCase *c;              /* the case, built once; the orientation and the strain are set per trial */
    double target[2];         /* m: the measured tip deflection after the cut, X then Y */
    char target_source[192];  /* where the two numbers come from */
    Provenance target_prov;
    char target_prov_text[24];  /* the word as written: "measured" is not one of the four enum values */
    char strategy[64];
    double start[2];          /* the starting exx, eyy */
    char start_source[192];
    double lo[2], hi[2];      /* bounds on exx and eyy */
    double ezz;               /* held */
    double tol;               /* relative, on each deflection */
    int max_builds;
    /* filled by the run */
    double fit[2], got[2], got_before[2], got_spring[2];
    int builds, iterations;
    bool converged;
    double seconds;
    JsonValue *history;       /* one entry per trial: exx, eyy, tipX, tipY, errors */
} LpbfCalibration;

bool lpbf_calibrate_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
void lpbf_calibration_free(void *data);
JsonValue *lpbf_calibration_summary_json(const LpbfCalibration *cal);
