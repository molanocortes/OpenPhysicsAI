/* mech_vib.c - natural modes and transient elastic response of parts as jobs, and their query (see mech_vib.h) */
#include "mech_vib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/sha256.h"
#include "../fem/dense.h"
#include "loads.h"

/* ------------------------------------------------------------------------------------------------ material */

bool vib_material_from(const StaticModel *sub, MechStructSetup *ortho, VibMaterial *vm, char *err, size_t errlen) {
    memset(vm, 0, sizeof *vm);
    for (int k = 0; k < 9; k++) vm->R[k] = k % 4 == 0;
    if (ortho) {
        vm->ortho = true;
        vm->nmat = 1;
        vm->mat = malloc(sizeof *vm->mat);
        if (!vm->mat) {
            snprintf(err, errlen, "out of memory");
            return false;
        }
        vm->mat[0] = ortho->mat.k;
        memcpy(vm->R, ortho->R, sizeof vm->R);
        vm->setup = *ortho;
        memset(ortho, 0, sizeof *ortho);
        return true;
    }
    vm->nmat = sub->nmat;
    vm->mat = malloc((size_t)(sub->nmat ? sub->nmat : 1) * sizeof *vm->mat);
    vm->elem_mat = malloc((size_t)(sub->nelems ? sub->nelems : 1) * sizeof(int));
    if (!vm->mat || !vm->elem_mat) {
        vib_material_free(vm);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (int i = 0; i < sub->nmat; i++) {
        if (!(sub->mat[i].E > 0) || !(sub->mat[i].nu > -1 && sub->mat[i].nu < 0.5)) {
            snprintf(err, errlen, "material '%s' has no valid elastic constants (E = %g Pa, nu = %g)", sub->mat_id[i], sub->mat[i].E, sub->mat[i].nu);
            vib_material_free(vm);
            return false;
        }
        ortho_isotropic(sub->mat[i].E, sub->mat[i].nu, &vm->mat[i]);
    }
    memcpy(vm->elem_mat, sub->elem_mat, (size_t)sub->nelems * sizeof(int));
    return true;
}

void vib_material_free(VibMaterial *vm) {
    free(vm->mat), free(vm->elem_mat);
    mech_struct_setup_free(&vm->setup);
    memset(vm, 0, sizeof *vm);
}

static OrthoModel vib_model(const StaticModel *m, const VibMaterial *vm) {
    return (OrthoModel){m->nnodes, m->nelems, m->xyz, m->conn, vm->nmat, vm->mat, vm->elem_mat, vm->ortho ? vm->R : NULL, 1, HEX8_INCOMPATIBLE};
}

static void write_files_entry(JsonValue *summary, const char *path, const char *name, const char *format) {
    JsonValue *files = json_get(summary, "files");
    if (!files) files = json_set_array(summary, "files");
    char hex[65];
    uint64_t bytes = 0;
    if (!sha256_file(path, hex, &bytes)) return;
    JsonValue *fo = json_object();
    json_set_string(fo, "name", name);
    json_set_string(fo, "format", format);
    json_set_int(fo, "bytes", (long long)bytes);
    json_set_string(fo, "sha256", hex);
    json_push(files, fo);
}

static bool wr(FILE *f, const void *p, size_t size, size_t n) { return n == 0 || fwrite(p, size, n, f) == n; }

/* ------------------------------------------------------------------------------------------------ modal job */

bool mech_modal_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    MechModalJob *J = data;
    StaticModel *m = &J->model;
    OrthoModel om = vib_model(m, &J->mat);
    StructDyn sd = {&om, J->density, J->fixed};
    int rigid_expected = J->fixed ? 0 : 6;
    ModalOptions mo = {J->elastic_modes + rigid_expected, 0, 1e-10, 400};
    job_progress(job, 0.05, "computing natural modes (subspace iteration)");
    ModalResult r;
    if (!modal_solve(&sd, &mo, &r, err, errlen)) {
        snprintf(code, codelen, "%s", strstr(err, "out of memory") ? "RESOURCE_LIMIT" : (strstr(err, "rigid-body") ? "INSUFFICIENT_CONSTRAINTS" : "SOLVER_FAILED"));
        return false;
    }
    job_progress(job, 0.9, "writing modes");
    JsonValue *S = json_object();
    json_set_string(S, "analysis", "mech_modal");
    json_set_string(S, "job_id", J->job_id);
    json_set(S, "setup", json_clone(J->setup));
    json_set_number(S, "mass_kg", r.mass);
    json_set(S, "mass_centre_mm", json_vec3(r.com[0] * 1e3, r.com[1] * 1e3, r.com[2] * 1e3));
    JsonValue *chk = json_set_object(S, "checks");
    json_set_bool(chk, "converged", r.converged);
    json_set_int(chk, "iterations", r.iterations);
    json_set_int(chk, "rigid_body_modes_found", r.nrigid);
    json_set_int(chk, "rigid_body_modes_expected", rigid_expected);
    double resmax = 0;
    for (int i = r.nrigid; i < r.nmodes; i++) resmax = fmax(resmax, r.residual[i]);
    json_set_number(chk, "largest_elastic_residual", resmax);
    json_set_number(chk, "mass_orthogonality", r.orthogonality);
    JsonValue *modes = json_set_array(S, "modes");
    double sum[6] = {0};
    for (int i = 0; i < r.nmodes; i++) {
        JsonValue *o = json_object();
        bool rigid = i < r.nrigid;
        json_set_int(o, "mode", i + 1);
        json_set_string(o, "kind", rigid ? "rigid_body" : "elastic");
        json_set_number(o, "frequency_hz", rigid ? 0 : sqrt(fmax(r.omega2[i], 0)) / (2 * M_PI));
        double f[6];
        for (int d = 0; d < 6; d++) {
            double tot = d < 3 ? r.mass : r.inertia[d - 3];
            f[d] = tot > 0 ? r.eff[6 * i + d] / tot : 0;
            if (!rigid) sum[d] += f[d];
        }
        json_set(o, "effective_mass_fraction", json_vec3(f[0], f[1], f[2]));
        json_set(o, "effective_inertia_fraction", json_vec3(f[3], f[4], f[5]));
        json_set_number(o, "residual", r.residual[i]);
        json_push(modes, o);
    }
    JsonValue *sm = json_set_object(S, "elastic_mode_sums");
    json_set(sm, "effective_mass_fraction", json_vec3(sum[0], sum[1], sum[2]));
    json_set(sm, "effective_inertia_fraction", json_vec3(sum[3], sum[4], sum[5]));
    json_set_string(sm, "reading",
                    J->fixed ? "fractions of the supported mass carried by the computed elastic modes; response sums need most of it in the loaded directions"
                             : "free-free: the rigid-body modes carry the mass and inertia, so elastic sums are small by construction");
    JsonValue *w = json_set_array(S, "warnings");
    if (!r.converged) json_push(w, json_string("the eigenvalue iteration did not reach its tolerance: frequencies are approximate"));
    if (r.nrigid != rigid_expected)
        json_push(w, json_stringf("%d rigid-body modes found where %d were expected: check the supports", r.nrigid, rigid_expected));
    JsonValue *as = json_set_array(S, "assumptions");
    json_push(as, json_string("linear elastic vibration about the undeformed configuration, consistent mass, no damping"));
    json_push(as, json_string("no prestress, rotation (centrifugal stiffening, gyroscopic coupling) or contact in the modes"));
    if (J->mat.ortho) mech_struct_assumptions(as, &J->mat.setup);
    /* modes file */
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, J->run_dir, "mech_modes.bin");
    FILE *f = fopen(path, "wb");
    int32_t hdr[5] = {m->nnodes, m->nelems, r.neq, r.nmodes, r.nrigid};
    bool ok = f && wr(f, "NVMMOD01", 1, 8) && wr(f, hdr, sizeof hdr[0], 5) && wr(f, m->xyz, sizeof(double), 3 * (size_t)m->nnodes) &&
              wr(f, m->conn, sizeof(int), 8 * (size_t)m->nelems) && wr(f, r.eq, sizeof(int), 3 * (size_t)m->nnodes) && wr(f, r.omega2, sizeof(double), (size_t)r.nmodes) &&
              wr(f, r.phi, sizeof(double), (size_t)r.nmodes * (size_t)r.neq);
    if (f) ok &= fclose(f) == 0;
    modal_result_free(&r);
    if (!ok) {
        json_free(S);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    write_files_entry(S, path, "mech_modes.bin", "NVMMOD01: counts, node coordinates (m), connectivity, equation numbers, omega^2, mass-normalised mode shapes");
    path_join(path, sizeof path, J->run_dir, "summary.json");
    if (!json_write_file(path, S, JSON_PRETTY | JSON_SORTED)) {
        json_free(S);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    job_progress(job, 1, "done");
    job_set_summary(job, S);
    return true;
}

void mech_modal_job_free(void *data) {
    MechModalJob *J = data;
    if (!J) return;
    static_model_free(&J->model);
    vib_material_free(&J->mat);
    free(J->density), free(J->fixed);
    json_free(J->setup);
    free(J);
}

/* ------------------------------------------------------------------------------------------------ transient job */

typedef struct Peak {
    double value, t;
    int e, g;
} Peak;

static void gp_pos(const StaticModel *m, int e, int g, double x[3]) {
    double N[8], dN[8][3];
    hex8_shape(HEX8_XI[g][0] / sqrt(3), HEX8_XI[g][1] / sqrt(3), HEX8_XI[g][2] / sqrt(3), N, dN);
    x[0] = x[1] = x[2] = 0;
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) x[k] += N[a] * m->xyz[3 * (size_t)m->conn[8 * (size_t)e + a] + (size_t)k];
}

static JsonValue *peak_json(const StaticModel *m, const Peak *p, double scale, const char *unit) {
    JsonValue *o = json_object();
    char key[32];
    snprintf(key, sizeof key, "value_%s", unit);
    json_set_number(o, key, p->value * scale);
    json_set_number(o, "time_s", p->t);
    if (p->e >= 0) {
        double x[3];
        gp_pos(m, p->e, p->g, x);
        json_set(o, "location_mm", json_vec3(x[0] * 1e3, x[1] * 1e3, x[2] * 1e3));
        json_set_int(o, "element", p->e);
        json_set_int(o, "gauss_point", p->g);
    }
    return o;
}

bool mech_transient_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    MechTransientJob *J = data;
    StaticModel *m = &J->model;
    OrthoModel om = vib_model(m, &J->mat);
    size_t NN3 = 3 * (size_t)m->nnodes, NE = (size_t)m->nelems, P = (size_t)J->npat, S = (size_t)J->nsteps + 1;
    bool ok = false, have_modes = false, haveF = false;
    ModalResult r;
    CholFactor F;
    double *Spat = NULL, *Smode = NULL, *u = NULL, *b = NULL, *x = NULL, *Peq = NULL, *q = NULL, *qd = NULL, *gen = NULL, *zeta = NULL, *hist_out = NULL;
    unsigned char *fixed = NULL;
    int *eqi = NULL;
    JsonValue *Sum = NULL;
    /* 1. free-free modes */
    job_progress(job, 0.02, "free-free natural modes");
    StructDyn sd = {&om, J->density, NULL};
    ModalOptions mo = {6 + J->elastic_modes, 0, 1e-10, 400};
    if (!modal_solve(&sd, &mo, &r, err, errlen)) {
        snprintf(code, codelen, "SOLVER_FAILED");
        return false;
    }
    have_modes = true;
    int nel = r.nmodes - r.nrigid;
    size_t NEQ = (size_t)r.neq;
    /* 2. quasi-static response of every load pattern with an isostatic support */
    job_progress(job, 0.35, "quasi-static response of each load pattern");
    FemMeshView mv = {m->nnodes, m->nelems, m->xyz, m->conn, J->density};
    IsostaticSupport sup;
    fixed = calloc(NN3, 1);
    eqi = malloc(NN3 * sizeof(int));
    if (!fixed || !eqi) goto oom;
    if (!loads_isostatic_choose(&mv, NULL, 0, &sup)) {
        snprintf(code, codelen, "MESH_INVALID");
        snprintf(err, errlen, "cannot choose three non-collinear support nodes");
        goto fail;
    }
    loads_isostatic_fixed(&sup, fixed);
    int neqi = 0;
    SolveStats st;
    memset(&st, 0, sizeof st);
    if (!ortho_assemble_factor(&om, fixed, eqi, &neqi, &F, &st, err, errlen)) {
        snprintf(code, codelen, "SOLVER_FAILED");
        goto fail;
    }
    haveF = true;
    Spat = malloc(P * 48 * NE * sizeof(double));
    u = malloc(NN3 * sizeof(double));
    b = malloc((size_t)(neqi ? neqi : 1) * sizeof(double));
    x = malloc((size_t)(neqi ? neqi : 1) * sizeof(double));
    if (!Spat || !u || !b || !x) goto oom;
    for (size_t j = 0; j < P; j++) {
        if (job_cancel_requested(job)) goto cancelled;
        for (size_t k = 0; k < NN3; k++)
            if (eqi[k] >= 0) b[eqi[k]] = J->patterns[j * NN3 + k];
        chol_solve(&F, b, x);
        for (size_t k = 0; k < NN3; k++) u[k] = eqi[k] >= 0 ? x[eqi[k]] : 0;
        ortho_gauss_stress(&om, u, Spat + j * 48 * NE);
    }
    chol_free(&F);
    haveF = false;
    /* 3. stresses of the elastic modes */
    job_progress(job, 0.55, "modal stresses");
    Smode = malloc((size_t)(nel ? nel : 1) * 48 * NE * sizeof(double));
    if (!Smode) goto oom;
    for (int i = 0; i < nel; i++) {
        const double *ph = r.phi + (size_t)(r.nrigid + i) * NEQ;
        for (size_t k = 0; k < NN3; k++) u[k] = r.eq[k] >= 0 ? ph[r.eq[k]] : 0;
        ortho_gauss_stress(&om, u, Smode + (size_t)i * 48 * NE);
    }
    /* 4. modal integration and the quasi-static modal coordinates */
    job_progress(job, 0.65, "modal integration");
    Peq = calloc(P * NEQ, sizeof(double));
    q = malloc((size_t)r.nmodes * S * sizeof(double));
    qd = malloc((size_t)r.nmodes * S * sizeof(double));
    gen = malloc((size_t)r.nmodes * P * sizeof(double));
    zeta = malloc((size_t)r.nmodes * sizeof(double));
    if (!Peq || !q || !qd || !gen || !zeta) goto oom;
    for (size_t j = 0; j < P; j++)
        for (size_t k = 0; k < NN3; k++)
            if (r.eq[k] >= 0) Peq[j * NEQ + (size_t)r.eq[k]] = J->patterns[j * NN3 + k];
    for (int i = 0; i < r.nmodes; i++) {
        zeta[i] = J->zeta;
        for (size_t j = 0; j < P; j++) {
            double s = 0;
            const double *ph = r.phi + (size_t)i * NEQ, *pj = Peq + j * NEQ;
            for (size_t k = 0; k < NEQ; k++) s += ph[k] * pj[k];
            gen[(size_t)i * P + j] = s;
        }
    }
    ModalLoad ml = {J->npat, Peq, J->nsteps, J->dt, J->hist};
    double rigid_share = 0;
    if (!modal_transient(&r, &ml, zeta, q, qd, &rigid_share, err, errlen)) {
        snprintf(code, codelen, "SOLVER_FAILED");
        goto fail;
    }
    /* 5. stress histories: sigma = sum_j g_j S_pat,j + sum_i (q_i - q_i,static) S_mode,i */
    job_progress(job, 0.75, "stress histories");
    bool strength = J->mat.ortho && J->mat.setup.mat.has_strength;
    Peak vm_dyn = {0, 0, -1, -1}, vm_qs = {0, 0, -1, -1}, fi_dyn = {0, 0, -1, -1}, fi_qs = {0, 0, -1, -1};
    hist_out = malloc(S * 5 * sizeof(double));
    double *cdyn = malloc((size_t)(nel ? nel : 1) * sizeof(double));
    if (!hist_out || !cdyn) {
        free(cdyn);
        goto oom;
    }
    size_t stride = 1;
    double work = (double)S * 8.0 * (double)NE * (double)(P + (size_t)nel);
    while (work / (double)stride > 4e9) stride++;
    size_t nout = 0;
    for (size_t n = 0; n < S; n += stride) {
        if (job_cancel_requested(job)) {
            free(cdyn);
            goto cancelled;
        }
        double t = J->t0 + (double)n * J->dt;
        for (int i = 0; i < nel; i++) {
            int mi = r.nrigid + i;
            double f = 0;
            for (size_t j = 0; j < P; j++) f += gen[(size_t)mi * P + j] * J->hist[j * S + n];
            cdyn[i] = q[(size_t)mi * S + n] - f / r.omega2[mi];
        }
        double mx_dyn = 0, mx_qs = 0, fx_dyn = 0, fx_qs = 0;
        for (size_t e = 0; e < NE; e++)
            for (int g = 0; g < 8; g++) {
                size_t off = 48 * e + 6 * (size_t)g;
                double sq[6] = {0}, sdn[6];
                for (size_t j = 0; j < P; j++) {
                    double gj = J->hist[j * S + n];
                    if (gj == 0) continue;
                    const double *sp = Spat + j * 48 * NE + off;
                    for (int c = 0; c < 6; c++) sq[c] += gj * sp[c];
                }
                memcpy(sdn, sq, sizeof sdn);
                for (int i = 0; i < nel; i++) {
                    const double *smd = Smode + (size_t)i * 48 * NE + off;
                    for (int c = 0; c < 6; c++) sdn[c] += cdyn[i] * smd[c];
                }
                double vd = von_mises(sdn), vq = von_mises(sq);
                if (vd > mx_dyn) mx_dyn = vd;
                if (vq > mx_qs) mx_qs = vq;
                if (vd > vm_dyn.value) vm_dyn = (Peak){vd, t, (int)e, g};
                if (vq > vm_qs.value) vm_qs = (Peak){vq, t, (int)e, g};
                if (strength) {
                    double smat[6];
                    StrengthResult sr;
                    voigt_stress_to_material(J->mat.R, sdn, smat);
                    strength_evaluate(J->mat.setup.criterion, &J->mat.setup.mat.strength, smat, &sr);
                    if (sr.index > fx_dyn) fx_dyn = sr.index;
                    if (sr.index > fi_dyn.value) fi_dyn = (Peak){sr.index, t, (int)e, g};
                    voigt_stress_to_material(J->mat.R, sq, smat);
                    strength_evaluate(J->mat.setup.criterion, &J->mat.setup.mat.strength, smat, &sr);
                    if (sr.index > fx_qs) fx_qs = sr.index;
                    if (sr.index > fi_qs.value) fi_qs = (Peak){sr.index, t, (int)e, g};
                }
            }
        hist_out[5 * nout] = t, hist_out[5 * nout + 1] = mx_dyn, hist_out[5 * nout + 2] = mx_qs, hist_out[5 * nout + 3] = fx_dyn, hist_out[5 * nout + 4] = fx_qs;
        nout++;
        if (n % 64 == 0) job_progress(job, 0.75 + 0.2 * (double)n / (double)S, "stress histories");
    }
    free(cdyn);
    /* 6. summary and history */
    job_progress(job, 0.96, "writing results");
    Sum = json_object();
    json_set_string(Sum, "analysis", "mech_transient");
    json_set_string(Sum, "job_id", J->job_id);
    json_set(Sum, "mechanical_transient", json_clone(J->report));
    JsonValue *md = json_set_object(Sum, "modes");
    json_set_int(md, "rigid_body_modes_found", r.nrigid);
    json_set_bool(md, "converged", r.converged);
    JsonValue *fq = json_set_array(md, "elastic_frequencies_hz");
    for (int i = r.nrigid; i < r.nmodes; i++) json_push(fq, json_number(sqrt(r.omega2[i]) / (2 * M_PI)));
    double fmax_mode = nel ? sqrt(r.omega2[r.nmodes - 1]) / (2 * M_PI) : 0;
    json_set_number(md, "highest_mode_hz", fmax_mode);
    json_set_number(md, "load_sampling_nyquist_hz", 0.5 / J->dt);
    json_set_number(md, "damping_ratio", J->zeta);
    JsonValue *chk = json_set_object(Sum, "checks");
    json_set_number(chk, "rigid_body_load_share", rigid_share);
    json_set_string(chk, "rigid_body_load_share_reading",
                    rigid_share < 1e-6 ? "negligible: the sampled loads are self-equilibrated, as the rigid dynamics requires"
                                       : "not negligible: the loads do not balance the rigid motion (mass or load placement differs between the rigid and FEM models)");
    json_set_int(chk, "stress_evaluation_stride", (long long)stride);
    JsonValue *res = json_set_object(Sum, "response");
    json_set(res, "peak_von_mises_dynamic", peak_json(m, &vm_dyn, 1e-6, "mpa"));
    json_set(res, "peak_von_mises_quasi_static", peak_json(m, &vm_qs, 1e-6, "mpa"));
    json_set_number(res, "dynamic_amplification_von_mises", vm_qs.value > 0 ? vm_dyn.value / vm_qs.value : 0);
    if (strength) {
        json_set_string(res, "criterion", strength_criterion_name(J->mat.setup.criterion));
        json_set(res, "peak_failure_index_dynamic", peak_json(m, &fi_dyn, 1, "index"));
        json_set(res, "peak_failure_index_quasi_static", peak_json(m, &fi_qs, 1, "index"));
        json_set_number(res, "smallest_strength_ratio_dynamic", fi_dyn.value > 0 ? 1 / fi_dyn.value : 0);
    }
    json_set_string(res, "reading",
                    "dynamic = quasi-static solution plus the modal dynamic correction (mode-acceleration method); quasi-static = the same loads without "
                    "the elastic inertia; the amplification compares the two peaks, which may occur at different times and places");
    JsonValue *as = json_set_array(Sum, "assumptions");
    json_push(as, json_string("linear elastic response about the rigid motion (small deformation; the deformation does not feed back into the rigid dynamics: one-way)"));
    json_push(as, json_string("loads vary linearly between samples; joint and contact loads enter over their selections as resultant-preserving tractions"));
    json_push(as, json_stringf("modal damping ratio %.4g for every elastic mode (%s)", J->zeta, json_get_str(J->report, "damping_source", "?")));
    json_push(as, json_string("no centrifugal stiffening, gyroscopic coupling or geometric nonlinearity of the spinning part"));
    if (J->mat.ortho) mech_struct_assumptions(as, &J->mat.setup);
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, J->run_dir, "mech_transient.csv");
    FILE *f = fopen(path, "wb");
    bool wok = f != NULL;
    if (f) {
        fprintf(f, "time [s],peak von Mises dynamic [Pa],peak von Mises quasi-static [Pa]%s\n", strength ? ",peak failure index dynamic [1],peak failure index quasi-static [1]" : "");
        for (size_t k = 0; k < nout; k++) {
            fprintf(f, "%.17g,%.17g,%.17g", hist_out[5 * k], hist_out[5 * k + 1], hist_out[5 * k + 2]);
            if (strength) fprintf(f, ",%.17g,%.17g", hist_out[5 * k + 3], hist_out[5 * k + 4]);
            fputc('\n', f);
        }
        wok = !ferror(f);
        wok &= fclose(f) == 0;
    }
    if (!wok) {
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write %s", path);
        goto fail;
    }
    write_files_entry(Sum, path, "mech_transient.csv", "per sample: time, peak von Mises stress (dynamic, quasi-static) and failure index when strengths exist");
    path_join(path, sizeof path, J->run_dir, "summary.json");
    if (!json_write_file(path, Sum, JSON_PRETTY | JSON_SORTED)) {
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write %s", path);
        goto fail;
    }
    job_set_summary(job, Sum);
    Sum = NULL;
    ok = true;
    goto done;
cancelled:
    snprintf(code, codelen, "CANCELLED");
    snprintf(err, errlen, "cancelled");
    goto fail;
oom:
    snprintf(code, codelen, "RESOURCE_LIMIT");
    snprintf(err, errlen, "out of memory in the transient assessment (%d nodes, %d elements, %d patterns)", m->nnodes, m->nelems, J->npat);
fail:
    ok = false;
done:
    if (haveF) chol_free(&F);
    if (have_modes) modal_result_free(&r);
    free(Spat), free(Smode), free(u), free(b), free(x), free(Peq), free(q), free(qd), free(gen), free(zeta), free(hist_out), free(fixed), free(eqi);
    json_free(Sum);
    return ok;
}

void mech_transient_job_free(void *data) {
    MechTransientJob *J = data;
    if (!J) return;
    static_model_free(&J->model);
    vib_material_free(&J->mat);
    free(J->density), free(J->patterns), free(J->hist);
    json_free(J->report);
    free(J);
}

/* ------------------------------------------------------------------------------------------------ coupled flexible stress */

/* the reduction's mesh, material and coordinate shapes (NVMFLX02) */
typedef struct FlexShapes {
    int nn, ne, nm, nmat, ortho;
    double *xyz, *omega2, *shapes, R[9];
    int *conn, *elem_mat;
    OrthoConstants *mat;
} FlexShapes;

static void flex_shapes_free(FlexShapes *s) {
    free(s->xyz), free(s->omega2), free(s->shapes), free(s->conn), free(s->elem_mat), free(s->mat);
    memset(s, 0, sizeof *s);
}

static bool flex_shapes_read(const char *path, FlexShapes *s, bool with_shapes) {
    memset(s, 0, sizeof *s);
    FILE *f = fopen(path, "rb");
    char magic[8];
    int32_t hdr[5];
    bool ok = f && fread(magic, 1, 8, f) == 8 && !memcmp(magic, "NVMFLX02", 8) && fread(hdr, sizeof hdr[0], 5, f) == 5 && hdr[0] > 0 && hdr[1] > 0 && hdr[2] > 0 &&
              hdr[3] > 0 && hdr[3] <= 64;
    if (ok) {
        s->nn = hdr[0], s->ne = hdr[1], s->nm = hdr[2], s->nmat = hdr[3], s->ortho = hdr[4];
        size_t nn = (size_t)s->nn, ne = (size_t)s->ne, nm = (size_t)s->nm;
        s->xyz = malloc(3 * nn * sizeof(double)), s->conn = malloc(8 * ne * sizeof(int)), s->omega2 = malloc(nm * sizeof(double));
        s->mat = malloc((size_t)s->nmat * sizeof *s->mat), s->elem_mat = malloc(ne * sizeof(int));
        s->shapes = with_shapes ? malloc(nm * 3 * nn * sizeof(double)) : NULL;
        ok = s->xyz && s->conn && s->omega2 && s->mat && s->elem_mat && (!with_shapes || s->shapes) && fread(s->xyz, sizeof(double), 3 * nn, f) == 3 * nn &&
             fread(s->conn, sizeof(int), 8 * ne, f) == 8 * ne && fread(s->omega2, sizeof(double), nm, f) == nm &&
             (with_shapes ? fread(s->shapes, sizeof(double), nm * 3 * nn, f) == nm * 3 * nn : fseek(f, (long)(nm * 3 * nn * sizeof(double)), SEEK_CUR) == 0);
        for (int k = 0; ok && k < s->nmat; k++) {
            double v[9];
            ok = fread(v, sizeof(double), 9, f) == 9;
            OrthoConstants c9 = {{v[0], v[1], v[2]}, v[3], v[4], v[5], v[6], v[7], v[8]};
            s->mat[k] = c9;
        }
        ok = ok && fread(s->elem_mat, sizeof(int), ne, f) == ne && fread(s->R, sizeof(double), 9, f) == 9;
        for (size_t e = 0; ok && e < ne; e++) ok = s->elem_mat[e] >= 0 && s->elem_mat[e] < s->nmat;
        for (size_t k = 0; ok && k < 8 * ne; k++) ok = s->conn[k] >= 0 && s->conn[k] < s->nn;
    }
    if (f) fclose(f);
    if (!ok) flex_shapes_free(s);
    return ok;
}

static JsonValue *gp_peak_json(const FlexShapes *s, const Peak *p) {
    JsonValue *o = json_object();
    json_set_number(o, "value_mpa", p->value * 1e-6);
    json_set_number(o, "time_s", p->t);
    if (p->e >= 0) {
        double N[8], dN[8][3], x[3] = {0, 0, 0};
        hex8_shape(HEX8_XI[p->g][0] / sqrt(3), HEX8_XI[p->g][1] / sqrt(3), HEX8_XI[p->g][2] / sqrt(3), N, dN);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) x[k] += N[a] * s->xyz[3 * (size_t)s->conn[8 * (size_t)p->e + a] + (size_t)k];
        json_set(o, "location_mm", json_vec3(x[0] * 1e3, x[1] * 1e3, x[2] * 1e3));
        json_set_int(o, "element", p->e);
        json_set_int(o, "gauss_point", p->g);
    }
    return o;
}

bool mech_flex_stress_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    MechFlexStressJob *J = data;
    FlexShapes fs;
    job_progress(job, 0.02, "reading the coordinate shapes of the reduction");
    if (!flex_shapes_read(J->shapes_path, &fs, true)) {
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot read the coordinate shapes %s", J->shapes_path);
        return false;
    }
    if (fs.nm != J->ncoord) {
        flex_shapes_free(&fs);
        snprintf(code, codelen, "PRECONDITION_FAILED");
        snprintf(err, errlen, "the reduction has %d coordinates but the run recorded %d", fs.nm, J->ncoord);
        return false;
    }
    size_t NE = (size_t)fs.ne, NM = (size_t)fs.nm, R = (size_t)J->nrows, G48 = 48 * NE;
    OrthoModel om = {fs.nn, fs.ne, fs.xyz, fs.conn, fs.nmat, fs.mat, fs.elem_mat, fs.ortho ? fs.R : NULL, 1, HEX8_INCOMPATIBLE};
    double *gpk = malloc(NM * G48 * sizeof(double)), *hist = malloc(2 * (R ? R : 1) * sizeof(double));
    if (!gpk || !hist) {
        free(gpk), free(hist), flex_shapes_free(&fs);
        snprintf(code, codelen, "RESOURCE_LIMIT");
        snprintf(err, errlen, "out of memory for %zu Gauss-point stress fields", NM);
        return false;
    }
    job_progress(job, 0.1, "Gauss-point stresses of every coordinate shape");
    for (size_t k = 0; k < NM; k++) ortho_gauss_stress(&om, fs.shapes + k * 3 * (size_t)fs.nn, gpk + k * G48);
    job_progress(job, 0.3, "stress history from the recorded elastic coordinates");
    Peak pk = {0, J->nrows ? J->t[0] : 0, -1, -1};
    for (size_t r = 0; r < R; r++) {
        const double *eta = J->eta + r * NM;
        double rowmax = 0;
        int re = -1, rg = -1;
        for (size_t e = 0; e < NE; e++)
            for (int g = 0; g < 8; g++) {
                double s6[6] = {0, 0, 0, 0, 0, 0};
                for (size_t k = 0; k < NM; k++) {
                    const double *src = gpk + k * G48 + 48 * e + 6 * (size_t)g;
                    for (int c = 0; c < 6; c++) s6[c] += eta[k] * src[c];
                }
                double vm = von_mises(s6);
                if (vm > rowmax) rowmax = vm, re = (int)e, rg = g;
            }
        hist[2 * r] = J->t[r], hist[2 * r + 1] = rowmax;
        if (re >= 0 && (pk.e < 0 || rowmax > pk.value)) pk.value = rowmax, pk.t = J->t[r], pk.e = re, pk.g = rg;
        if ((r & 63) == 0) job_progress(job, 0.3 + 0.6 * (double)r / (double)R, "stress history from the recorded elastic coordinates");
    }
    JsonValue *S = json_object();
    json_set_string(S, "analysis", "mech_flexible_stress");
    json_set_string(S, "job_id", J->job_id);
    json_set(S, "setup", json_clone(J->report));
    JsonValue *res = json_set_object(S, "response");
    json_set(res, "peak_von_mises", gp_peak_json(&fs, &pk));
    json_set_int(res, "samples", J->nrows);
    json_set_string(res, "reading", "von Mises stress at the Gauss points of the reduction's mesh, from sigma = sum over coordinates of eta_k(t) sigma_k: the "
                                    "deformation of the coupled flexible dynamics relative to the reference frame held at the root region");
    JsonValue *as = json_set_array(S, "assumptions");
    json_push(as, json_string("linear elastic stresses of the small deformation about the undeformed part, from the elastic coordinates of the coupled run "
                              "(first-order floating frame; second-order stiffness effects of the motion are not included)"));
    json_push(as, json_stringf("evaluated at the recorded instants (record period %.4g s): peaks between records are not seen", J->record_period));
    json_push(as, json_string("the root selection is clamped and the interface selections move rigidly in the reduction: stresses next to them are those of "
                              "rigid regions, so local concentrations there depend on that idealisation"));
    json_push(as, json_string("coordinates dropped by the reduction's frequency cutoff carry no stress (the reduction reports the flexibility they held per "
                              "interface direction)"));
    if (fs.ortho)
        json_push(as, json_string("orthotropic printed material: von Mises is an indicator only; failure indices with strengths come from mech_fem_assess or "
                                  "mech_transient_assess"));
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, J->run_dir, "mech_flexible_stress.csv");
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) {
        fprintf(f, "time [s],peak von Mises coupled [Pa]\n");
        for (size_t r = 0; r < R; r++) fprintf(f, "%.17g,%.17g\n", hist[2 * r], hist[2 * r + 1]);
        ok = fclose(f) == 0;
    }
    if (ok) write_files_entry(S, path, "mech_flexible_stress.csv", "time and the largest Gauss-point von Mises stress at each recorded instant");
    free(gpk), free(hist);
    flex_shapes_free(&fs);
    path_join(path, sizeof path, J->run_dir, "summary.json");
    if (!ok || !json_write_file(path, S, JSON_PRETTY | JSON_SORTED)) {
        json_free(S);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write the stress results to %s", J->run_dir);
        return false;
    }
    job_progress(job, 1, "done");
    job_set_summary(job, S);
    return true;
}

void mech_flex_stress_job_free(void *data) {
    MechFlexStressJob *J = data;
    if (!J) return;
    free(J->t), free(J->eta);
    json_free(J->report);
    free(J);
}

/* ------------------------------------------------------------------------------------------------ query */

void op_mech_vibration_query(Engine *e, JsonValue *p, OpResult *out) {
    const char *id = json_get_str(p, "job_id", ""), *what = json_get_str(p, "what", "summary");
    char dir[NV_PATH_MAX], path[NV_PATH_MAX];
    if (!op_project_run_dir(e, id, dir, sizeof dir)) {
        op_fail(out, NV_ERR_NOT_FOUND, "use job_list for the jobs of the open project", "no run directory for job '%s'", id);
        return;
    }
    path_join(path, sizeof path, dir, "summary.json");
    JsonError je;
    JsonValue *summary = json_read_file(path, 64 << 20, &je);
    const char *kind = json_get_str(summary, "analysis", "");
    if (!summary || (strcmp(kind, "mech_modal") && strcmp(kind, "mech_transient") && strcmp(kind, "mech_flexible") && strcmp(kind, "mech_flexible_stress"))) {
        json_free(summary);
        op_fail(out, NV_ERR_PRECONDITION,
                "wait with job_status; mech_vibration_query reads mech_modal_run, mech_transient_assess, mech_flexible_reduce and mech_flexible_stress jobs",
                "job '%s' has no modal, transient, reduction or flexible stress results", id);
        return;
    }
    /* kind points into summary, which is freed below */
    bool transient = !strcmp(kind, "mech_transient"), flexible = !strcmp(kind, "mech_flexible"), fstress = !strcmp(kind, "mech_flexible_stress");
    JsonValue *v = json_object();
    json_set_string(v, "job_id", id);
    if (!strcmp(what, "summary")) {
        json_set(v, "summary", summary);
        op_succeed(out, v);
        return;
    }
    if (!strcmp(what, "history")) {
        json_free(summary);
        if (!transient && !fstress) {
            json_free(v);
            op_fail(out, NV_ERR_INVALID_PARAMS, "history belongs to transient assessments and flexible stress jobs", "job '%s' is a %s", id,
                    flexible ? "flexible-body reduction" : "modal analysis");
            return;
        }
        path_join(path, sizeof path, dir, fstress ? "mech_flexible_stress.csv" : "mech_transient.csv");
        FILE *f = fopen(path, "rb");
        if (!f) {
            json_free(v);
            op_fail(out, NV_ERR_IO, NULL, "cannot read %s", path);
            return;
        }
        int maxp = (int)json_get_int(p, "max_points", 500);
        if (maxp < 2) maxp = 2;
        char line[512];
        long rows = -1;
        while (fgets(line, sizeof line, f)) rows++;
        rewind(f);
        long stride = rows > maxp ? (rows + maxp - 1) / maxp : 1;
        JsonValue *cols = json_set_array(v, "columns");
        if (fgets(line, sizeof line, f)) {
            char *tok = strtok(line, ",\n");
            while (tok) json_push(cols, json_string(tok)), tok = strtok(NULL, ",\n");
        }
        JsonValue *data = json_set_array(v, "rows");
        long k = 0;
        while (fgets(line, sizeof line, f)) {
            if (k++ % stride) continue;
            JsonValue *row = json_array();
            char *tok = strtok(line, ",\n");
            while (tok) json_push(row, json_number(strtod(tok, NULL))), tok = strtok(NULL, ",\n");
            json_push(data, row);
        }
        fclose(f);
        op_succeed(out, v);
        return;
    }
    if (!strcmp(what, "mode_shape") && flexible) { /* the displacement shape of one elastic coordinate */
        json_free(summary);
        path_join(path, sizeof path, dir, "mech_flex_shapes.bin");
        FlexShapes fs;
        int mode = (int)json_get_int(p, "mode", 1);
        if (!flex_shapes_read(path, &fs, true)) {
            json_free(v);
            op_fail(out, NV_ERR_IO, NULL, "cannot read the coordinate shapes of job '%s'", id);
            return;
        }
        if (mode < 1 || mode > fs.nm) {
            json_free(v);
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "coordinate %d is not between 1 and %d", mode, fs.nm);
            flex_shapes_free(&fs);
            return;
        }
        const double *ph = fs.shapes + (size_t)(mode - 1) * 3 * (size_t)fs.nn;
        double amax = 0;
        int best[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        double bv[10] = {0};
        for (size_t n = 0; n < (size_t)fs.nn; n++) {
            const double *d = ph + 3 * n;
            double a = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            amax = fmax(amax, a);
            for (int sl = 0; sl < 10; sl++)
                if (best[sl] < 0 || a > bv[sl]) {
                    for (int t = 9; t > sl; t--) best[t] = best[t - 1], bv[t] = bv[t - 1];
                    best[sl] = (int)n, bv[sl] = a;
                    break;
                }
        }
        json_set_int(v, "mode", mode);
        json_set_number(v, "frequency_hz", sqrt(fmax(fs.omega2[mode - 1], 0)) / (2 * M_PI));
        json_set_string(v, "kind", "elastic coordinate of a flexible-body reduction (root clamped, interfaces free and rigid)");
        json_set_string(v, "normalisation", "displacements scaled so the largest nodal amplitude is 1 (FE axes)");
        JsonValue *arr = json_set_array(v, "largest_nodes");
        for (int sl = 0; sl < 10 && best[sl] >= 0 && amax > 0; sl++) {
            size_t n = (size_t)best[sl];
            JsonValue *o = json_object();
            json_set(o, "location_mm", json_vec3(fs.xyz[3 * n] * 1e3, fs.xyz[3 * n + 1] * 1e3, fs.xyz[3 * n + 2] * 1e3));
            json_set(o, "displacement", json_vec3(ph[3 * n] / amax, ph[3 * n + 1] / amax, ph[3 * n + 2] / amax));
            json_push(arr, o);
        }
        flex_shapes_free(&fs);
        op_succeed(out, v);
        return;
    }
    if (!strcmp(what, "mode_shape")) {
        json_free(summary);
        path_join(path, sizeof path, dir, "mech_modes.bin");
        FILE *f = fopen(path, "rb");
        char magic[8];
        int32_t hdr[5];
        int mode = (int)json_get_int(p, "mode", 1);
        bool ok = f && fread(magic, 1, 8, f) == 8 && !memcmp(magic, "NVMMOD01", 8) && fread(hdr, sizeof hdr[0], 5, f) == 5 && hdr[0] > 0 && hdr[1] > 0 && hdr[2] > 0 &&
                  hdr[3] > 0;
        if (!ok) {
            if (f) fclose(f);
            json_free(v);
            op_fail(out, NV_ERR_IO, "mode shapes belong to mech_modal_run jobs", "cannot read the modes of job '%s'", id);
            return;
        }
        if (mode < 1 || mode > hdr[3]) {
            fclose(f);
            json_free(v);
            op_fail(out, NV_ERR_INVALID_PARAMS, NULL, "mode %d is not between 1 and %d", mode, hdr[3]);
            return;
        }
        size_t nn = (size_t)hdr[0], ne = (size_t)hdr[1], neq = (size_t)hdr[2], nm = (size_t)hdr[3];
        double *xyz = malloc(3 * nn * sizeof(double)), *w2 = malloc(nm * sizeof(double)), *ph = malloc(neq * sizeof(double));
        int *eq = malloc(3 * nn * sizeof(int));
        ok = xyz && w2 && ph && eq && fread(xyz, sizeof(double), 3 * nn, f) == 3 * nn && fseek(f, (long)(8 * ne * sizeof(int)), SEEK_CUR) == 0 &&
             fread(eq, sizeof(int), 3 * nn, f) == 3 * nn && fread(w2, sizeof(double), nm, f) == nm && fseek(f, (long)((size_t)(mode - 1) * neq * sizeof(double)), SEEK_CUR) == 0 &&
             fread(ph, sizeof(double), neq, f) == neq;
        fclose(f);
        if (ok) {
            double amax = 0;
            int best[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
            double bv[10] = {0};
            for (size_t n = 0; n < nn; n++) {
                double d[3];
                for (int k = 0; k < 3; k++) d[k] = eq[3 * n + (size_t)k] >= 0 ? ph[eq[3 * n + (size_t)k]] : 0;
                double a = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                amax = fmax(amax, a);
                for (int s = 0; s < 10; s++)
                    if (best[s] < 0 || a > bv[s]) {
                        for (int t = 9; t > s; t--) best[t] = best[t - 1], bv[t] = bv[t - 1];
                        best[s] = (int)n, bv[s] = a;
                        break;
                    }
            }
            json_set_int(v, "mode", mode);
            json_set_number(v, "frequency_hz", sqrt(fmax(w2[mode - 1], 0)) / (2 * M_PI));
            json_set_string(v, "normalisation", "displacements scaled so the largest nodal amplitude is 1");
            JsonValue *arr = json_set_array(v, "largest_nodes");
            for (int s = 0; s < 10 && best[s] >= 0; s++) {
                size_t n = (size_t)best[s];
                JsonValue *o = json_object();
                double d[3];
                for (int k = 0; k < 3; k++) d[k] = eq[3 * n + (size_t)k] >= 0 ? ph[eq[3 * n + (size_t)k]] / amax : 0;
                json_set(o, "location_mm", json_vec3(xyz[3 * n] * 1e3, xyz[3 * n + 1] * 1e3, xyz[3 * n + 2] * 1e3));
                json_set(o, "displacement", json_vec3(d[0], d[1], d[2]));
                json_push(arr, o);
            }
        }
        free(xyz), free(w2), free(ph), free(eq);
        if (!ok) {
            json_free(v);
            op_fail(out, NV_ERR_IO, NULL, "the modes file of job '%s' is truncated", id);
            return;
        }
        op_succeed(out, v);
        return;
    }
    json_free(summary), json_free(v);
    op_fail(out, NV_ERR_INVALID_PARAMS, "what is summary, mode_shape or history", "unknown what '%s'", what);
}
