#include "lpbf_calibrate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "../core/paths.h"
#include "setup.h"
#include "transient_analysis.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static JsonValue *json_pair(double a, double b) {
    JsonValue *v = json_array();
    json_push(v, json_number(a)), json_push(v, json_number(b));
    return v;
}

/* One build in one orientation. The case is the same for both: the mesh lays the part along its own x and the build
 * orientation only decides which component of the machine-frame tensor lies along that axis. */
static bool trial(Job *job, LpbfCalibration *cal, const double eps[2], int orientation, double lo, double hi,
                  double *tip, char *code, size_t codelen, char *err, size_t errlen) {
    cal->c->s.eps[0] = eps[0], cal->c->s.eps[1] = eps[1], cal->c->s.eps[2] = cal->ezz;
    cal->c->s.orientation = orientation;
    cal->c->eps_model[0] = orientation ? eps[1] : eps[0];
    cal->c->eps_model[1] = orientation ? eps[0] : eps[1];
    cal->c->eps_model[2] = cal->ezz;
    if (!lpbf_build_once(job, cal->c, false, lo, hi, code, codelen, err, errlen)) return false;
    *tip = cal->c->tip_after;
    cal->got_before[orientation] = cal->c->tip_before;
    cal->got_spring[orientation] = cal->c->springback;
    cal->builds++;
    return true;
}

/* Both orientations at one strain: the residual of the fit. */
static bool residual(Job *job, LpbfCalibration *cal, const double eps[2], double tip[2], double res[2],
                     const char *what, char *code, size_t codelen, char *err, size_t errlen) {
    double span = 0.9 / (double)(cal->max_builds ? cal->max_builds : 12);
    double base = fmin(0.9, span * cal->builds);
    char stage[96];
    snprintf(stage, sizeof stage, "%s: exx %.6g, eyy %.6g, build X", what, eps[0], eps[1]);
    job_progress(job, base, stage);
    if (!trial(job, cal, eps, 0, base, base + span, &tip[0], code, codelen, err, errlen)) return false;
    base = fmin(0.9, span * cal->builds);
    snprintf(stage, sizeof stage, "%s: exx %.6g, eyy %.6g, build Y", what, eps[0], eps[1]);
    job_progress(job, base, stage);
    if (!trial(job, cal, eps, 1, base, base + span, &tip[1], code, codelen, err, errlen)) return false;
    res[0] = tip[0] - cal->target[0];
    res[1] = tip[1] - cal->target[1];
    JsonValue *h = json_object();
    json_set_string(h, "what", what);
    json_set_number(h, "exx", eps[0]);
    json_set_number(h, "eyy", eps[1]);
    json_set_number(h, "tip_x_mm", 1e3 * tip[0]);
    json_set_number(h, "tip_y_mm", 1e3 * tip[1]);
    json_set_number(h, "error_x_pct", 100.0 * res[0] / cal->target[0]);
    json_set_number(h, "error_y_pct", 100.0 * res[1] / cal->target[1]);
    json_set_int(h, "builds_so_far", cal->builds);
    json_push(cal->history, h);
    return true;
}

static void clamp(const LpbfCalibration *cal, double e[2]) {
    for (int i = 0; i < 2; i++) e[i] = fmin(cal->hi[i], fmax(cal->lo[i], e[i]));
}

static bool converged(const LpbfCalibration *cal, const double res[2]) {
    return fabs(res[0]) <= cal->tol * fabs(cal->target[0]) && fabs(res[1]) <= cal->tol * fabs(cal->target[1]);
}

/* 2x2 solve of J dx = -F; false when the Jacobian is singular to working precision */
static bool step(const double J[2][2], const double F[2], double dx[2]) {
    double det = J[0][0] * J[1][1] - J[0][1] * J[1][0];
    double scale = fmax(fabs(J[0][0] * J[1][1]), fabs(J[0][1] * J[1][0]));
    if (!(fabs(det) > 1e-12 * fmax(scale, 1e-300))) return false;
    dx[0] = -(J[1][1] * F[0] - J[0][1] * F[1]) / det;
    dx[1] = -(-J[1][0] * F[0] + J[0][0] * F[1]) / det;
    return true;
}

bool lpbf_calibrate_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    LpbfCalibration *cal = data;
    double t0 = now_s();
    cal->history = json_array();
    double e[2] = {cal->start[0], cal->start[1]}, tip[2], F[2];
    clamp(cal, e);
    if (!residual(job, cal, e, tip, F, "start", code, codelen, err, errlen)) return false;
    /* the Jacobian by forward differences, once; the model is linear in the strain, so it stays right */
    double J[2][2];
    if (!converged(cal, F)) {
        for (int j = 0; j < 2 && cal->builds + 2 <= cal->max_builds; j++) {
            double h = fmax(2e-4, 0.05 * fabs(e[j]));
            if (e[j] + h > cal->hi[j]) h = -h;
            double ej[2] = {e[0], e[1]}, tj[2], Fj[2];
            ej[j] += h;
            char what[48];
            snprintf(what, sizeof what, "jacobian column %d", j + 1);
            if (!residual(job, cal, ej, tj, Fj, what, code, codelen, err, errlen)) return false;
            J[0][j] = (Fj[0] - F[0]) / h;
            J[1][j] = (Fj[1] - F[1]) / h;
        }
    }
    while (!converged(cal, F) && cal->builds + 2 <= cal->max_builds) {
        double dx[2];
        if (!step(J, F, dx)) {
            snprintf(code, codelen, "SOLVER_FAILED");
            snprintf(err, errlen, "the two deflections do not separate exx from eyy: the fit's Jacobian is singular");
            return false;
        }
        double en[2] = {e[0] + dx[0], e[1] + dx[1]};
        clamp(cal, en);
        double dxc[2] = {en[0] - e[0], en[1] - e[1]}, tn[2], Fn[2];
        char what[48];
        snprintf(what, sizeof what, "secant step %d", cal->iterations + 1);
        if (!residual(job, cal, en, tn, Fn, what, code, codelen, err, errlen)) return false;
        cal->iterations++;
        /* Broyden: correct the Jacobian with what this step actually did */
        double dn = dxc[0] * dxc[0] + dxc[1] * dxc[1];
        if (dn > 0) {
            for (int i = 0; i < 2; i++) {
                double pred = J[i][0] * dxc[0] + J[i][1] * dxc[1];
                double corr = (Fn[i] - F[i] - pred) / dn;
                J[i][0] += corr * dxc[0], J[i][1] += corr * dxc[1];
            }
        }
        e[0] = en[0], e[1] = en[1], F[0] = Fn[0], F[1] = Fn[1], tip[0] = tn[0], tip[1] = tn[1];
    }
    cal->fit[0] = e[0], cal->fit[1] = e[1];
    cal->got[0] = tip[0], cal->got[1] = tip[1];
    cal->converged = converged(cal, F);
    cal->seconds = now_s() - t0;
    JsonValue *summary = lpbf_calibration_summary_json(cal);
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, cal->c->c.run_dir, "summary.json");
    if (!json_write_file(path, summary, JSON_PRETTY)) {
        json_free(summary);
        snprintf(code, codelen, "IO");
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    job_set_summary(job, summary);
    job_progress(job, 1.0, cal->converged ? "done" : "stopped at the build limit");
    return true;
}

void lpbf_calibration_free(void *data) {
    LpbfCalibration *cal = data;
    if (!cal) return;
    thermal_case_free(cal->c);
    json_free(cal->history);
    free(cal);
}

JsonValue *lpbf_calibration_summary_json(const LpbfCalibration *cal) {
    JsonValue *o = json_object();
    json_set_string(o, "analysis", "lpbf_calibrate");
    json_set_string(o, "strategy", cal->strategy);
    json_set_bool(o, "converged", cal->converged);
    JsonValue *fit = json_set_object(o, "inherent_strain");
    json_set_number(fit, "exx", cal->fit[0]);
    json_set_number(fit, "eyy", cal->fit[1]);
    json_set_number(fit, "ezz", cal->ezz);
    json_set_string(fit, "frame", "machine frame: a Y build takes eyy along the part's axis");
    json_set_string(fit, "ezz_note", "held, not fitted: two deflections cannot separate three components, and the "
                                     "calibration that produced the reference tensors held it at the same value");
    char src[320];
    snprintf(src, sizeof src, "fitted by navier lpbf_calibrate on %s, against %s", cal->strategy, cal->target_source);
    json_set_string(fit, "source", src);
    json_set_string(fit, "provenance", "calibrated");
    JsonValue *t = json_set_object(o, "target");
    json_set_number(t, "tip_uz_after_cut_x_mm", 1e3 * cal->target[0]);
    json_set_number(t, "tip_uz_after_cut_y_mm", 1e3 * cal->target[1]);
    json_set_string(t, "source", cal->target_source);
    json_set_string(t, "provenance", cal->target_prov_text[0] ? cal->target_prov_text : provenance_name(cal->target_prov));
    JsonValue *g = json_set_object(o, "achieved");
    json_set_number(g, "tip_uz_after_cut_x_mm", 1e3 * cal->got[0]);
    json_set_number(g, "tip_uz_after_cut_y_mm", 1e3 * cal->got[1]);
    json_set_number(g, "tip_uz_before_cut_x_mm", 1e3 * cal->got_before[0]);
    json_set_number(g, "tip_uz_before_cut_y_mm", 1e3 * cal->got_before[1]);
    json_set_number(g, "springback_x_mm", 1e3 * cal->got_spring[0]);
    json_set_number(g, "springback_y_mm", 1e3 * cal->got_spring[1]);
    json_set_number(g, "error_x_pct", 100.0 * (cal->got[0] - cal->target[0]) / cal->target[0]);
    json_set_number(g, "error_y_pct", 100.0 * (cal->got[1] - cal->target[1]) / cal->target[1]);
    json_set_number(g, "tolerance_pct", 100.0 * cal->tol);
    JsonValue *f = json_set_object(o, "fit");
    json_set_string(f, "method", "bounded secant: forward-difference Jacobian once, then Broyden updates; the model is "
                                 "linear in the inherent strain, so the first step is usually the last");
    json_set_int(f, "builds", cal->builds);
    json_set_int(f, "build_limit", cal->max_builds);
    json_set_int(f, "secant_steps", cal->iterations);
    json_set_number(f, "start_exx", cal->start[0]);
    json_set_number(f, "start_eyy", cal->start[1]);
    json_set_string(f, "start_source", cal->start_source);
    json_set(f, "bounds_exx", json_pair(cal->lo[0], cal->hi[0]));
    json_set(f, "bounds_eyy", json_pair(cal->lo[1], cal->hi[1]));
    json_set_number(f, "seconds", cal->seconds);
    json_set(o, "history", json_clone(cal->history));
    JsonValue *m = json_set_object(o, "discretisation");
    json_set_number(m, "layer_thickness_mm", 1e3 * cal->c->s.layer_thickness);
    json_set_int(m, "elements", cal->c->c.nelems);
    json_set_int(m, "nodes", cal->c->c.nnodes);
    json_set(m, "element_size_mm", json_vec3(1e3 * cal->c->c.h[0], 1e3 * cal->c->c.h[1], 1e3 * cal->c->c.h[2]));
    json_set_string(m, "note", "a fitted inherent strain belongs to this discretisation and to this material law; "
                               "using it at another element size or layer thickness is a new assumption");
    json_set_string(o, "scope", "the fit matches tip deflection only. It is not evidence about stress, and it absorbs "
                                "every error of the model it was fitted with");
    return o;
}
