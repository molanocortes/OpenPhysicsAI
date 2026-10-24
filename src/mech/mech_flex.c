/* mech_flex.c - the flexible-body reduction job (see mech_flex.h) */
#include "mech_flex.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/sha256.h"
#include "assembly.h"
#include "flexbody.h"

static bool wr(FILE *f, const void *p, size_t size, size_t n) { return n == 0 || fwrite(p, size, n, f) == n; }

static void files_entry(JsonValue *summary, const char *path, const char *name, const char *format) {
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

bool mech_flex_job_run(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen) {
    MechFlexJob *J = data;
    StaticModel *m = &J->model;
    OrthoModel om = {m->nnodes, m->nelems, m->xyz, m->conn, J->mat.nmat, J->mat.mat, J->mat.elem_mat, J->mat.ortho ? J->mat.R : NULL, 1, HEX8_INCOMPATIBLE};
    FlexReduceInput in = {&om, J->density, J->root, J->nroot, J->ninterfaces, (const int *const *)J->itf_nodes, J->itf_count, (const double (*)[3])J->itf_point,
                          J->fixed_modes, J->zeta, {0}, {0}, J->max_frequency_hz, true};
    memcpy(in.R, J->R, sizeof in.R), memcpy(in.p, J->p, sizeof in.p);
    job_progress(job, 0.05, "constraint modes and fixed-interface modes (Craig-Bampton reduction)");
    AsmFlexible *X = calloc(1, sizeof *X);
    FlexReduceReport rep;
    memset(&rep, 0, sizeof rep);
    if (!X || !(X->interface_joint = calloc((size_t)(J->ninterfaces ? J->ninterfaces : 1), sizeof *X->interface_joint))) {
        free(X ? X->interface_joint : NULL), free(X);
        snprintf(code, codelen, "RESOURCE_LIMIT");
        snprintf(err, errlen, "out of memory");
        return false;
    }
    if (!flex_reduce(&in, &X->model, &rep, err, errlen)) {
        free(X->interface_joint), free(X);
        snprintf(code, codelen, "%s", strstr(err, "out of memory") ? "RESOURCE_LIMIT" : (strstr(err, "root region") ? "INSUFFICIENT_CONSTRAINTS" : "SOLVER_FAILED"));
        return false;
    }
    job_progress(job, 0.9, "writing the reduced model");
    const MbFlexDef *F = &X->model;
    for (int i = 0; i < J->ninterfaces; i++) snprintf(X->interface_joint[i], MB_NAME, "%s", J->itf_joint[i]);
    X->fe_mass = rep.fe_mass;
    memcpy(X->fe_com, rep.fe_com, sizeof X->fe_com), memcpy(X->fe_inertia, rep.fe_inertia, sizeof X->fe_inertia);
    X->provenance = json_clone(J->setup);
    json_set_string(X->provenance, "job_id", J->job_id);
    JsonValue *S = json_object();
    json_set_string(S, "analysis", "mech_flexible");
    json_set_string(S, "job_id", J->job_id);
    json_set(S, "setup", json_clone(J->setup));
    JsonValue *red = json_set_object(S, "reduction");
    json_set_string(red, "method", "Craig-Bampton: static constraint modes of rigid interfaces plus fixed-interface normal modes, diagonalised to mass-normalised coordinates");
    json_set_int(red, "constraint_modes", rep.constraint_modes);
    json_set_int(red, "fixed_interface_modes", rep.fixed_modes);
    json_set_int(red, "elastic_coordinates", rep.coordinates);
    json_set_int(red, "dropped_above_cutoff", rep.dropped);
    json_set_number(red, "largest_interface_compliance_loss", rep.compliance_loss);
    json_set_bool(red, "fixed_interface_modes_converged", rep.fixed_modes_converged);
    double wmax = sqrt(F->omega2[F->nmodes - 1]);
    json_set_number(red, "highest_frequency_hz", wmax / (2 * M_PI));
    json_set_number(red, "stability_step_s", 0.5 / wmax);
    JsonValue *co = json_set_array(S, "coordinates");
    for (int k = 0; k < F->nmodes; k++) {
        JsonValue *o = json_object();
        json_set_int(o, "index", k);
        json_set_number(o, "frequency_hz", sqrt(F->omega2[k]) / (2 * M_PI));
        /* effective mass of the coordinate against the reference frame: |integral rho phi|^2 (kg) */
        double lt = F->ell[6 * k + 3] * F->ell[6 * k + 3] + F->ell[6 * k + 4] * F->ell[6 * k + 4] + F->ell[6 * k + 5] * F->ell[6 * k + 5];
        json_set_number(o, "effective_mass_kg", lt);
        json_push(co, o);
    }
    JsonValue *ia = json_set_array(S, "interfaces");
    for (int i = 0; i < J->ninterfaces; i++) {
        double C[36] = {0};
        for (int k = 0; k < F->nmodes; k++) {
            double u[6];
            for (int c = 0; c < 3; c++) u[c] = F->interfaces[i].phi[3 * k + c], u[3 + c] = F->interfaces[i].psi[3 * k + c];
            for (int r = 0; r < 6; r++)
                for (int c = 0; c < 6; c++) C[6 * r + c] += u[r] * u[c] / F->omega2[k];
        }
        JsonValue *o = json_object();
        json_set_string(o, "joint", J->itf_joint[i]);
        json_set(o, "point_body_m", json_numbers(F->interfaces[i].point, 3));
        json_set_int(o, "nodes", J->itf_count[i]);
        json_set(o, "static_compliance_translation_m_per_n", json_vec3(C[0], C[7], C[14]));
        json_set(o, "static_compliance_rotation_rad_per_n_m", json_vec3(C[21], C[28], C[35]));
        if (rep.dropped) {
            const double *ls = rep.interface_loss + 6 * i;
            json_set(o, "compliance_loss_translation", json_vec3(ls[0], ls[1], ls[2]));
            json_set(o, "compliance_loss_rotation", json_vec3(ls[3], ls[4], ls[5]));
        }
        json_set_string(o, "reading", "diagonal of the 6x6 static compliance of the interface relative to the clamped root, body axes: its displacement per unit "
                                      "force and its rotation per unit moment applied at the interface point");
        json_push(ia, o);
    }
    JsonValue *mp = json_set_object(S, "mass_properties");
    json_set_number(mp, "mesh_mass_kg", rep.fe_mass);
    json_set(mp, "mesh_com_body_mm", json_vec3(rep.fe_com[0] * 1e3, rep.fe_com[1] * 1e3, rep.fe_com[2] * 1e3));
    JsonValue *w = json_set_array(S, "warnings");
    if (J->body_has_mass) {
        double dcom[3] = {rep.fe_com[0] - J->body_com[0], rep.fe_com[1] - J->body_com[1], rep.fe_com[2] - J->body_com[2]}, di = 0, isc = 0;
        for (int k = 0; k < 9; k++) di += pow(rep.fe_inertia[k] - J->body_inertia[k], 2), isc += pow(J->body_inertia[k], 2);
        json_set_number(mp, "body_mass_kg", J->body_mass);
        json_set_number(mp, "mass_difference_fraction", rep.fe_mass / J->body_mass - 1);
        json_set_number(mp, "com_offset_mm", 1e3 * sqrt(dcom[0] * dcom[0] + dcom[1] * dcom[1] + dcom[2] * dcom[2]));
        json_set_number(mp, "inertia_difference_fraction", isc > 0 ? sqrt(di / isc) : 0);
        if (fabs(rep.fe_mass / J->body_mass - 1) > 0.02)
            json_push(w, json_stringf("the mesh has %.1f%% %s mass than the body (%.6g kg against %.6g kg): the body's mass properties come from its fill model, "
                                      "the mesh is solid material; attaching the reduction replaces them with the mesh's",
                                      100 * fabs(rep.fe_mass / J->body_mass - 1), rep.fe_mass > J->body_mass ? "more" : "less", rep.fe_mass, J->body_mass));
    }
    json_set_string(mp, "reading", "attaching the reduction replaces the body's mass properties with those of the reduced mesh, so the rigid and elastic inertia "
                                   "are consistent");
    if (!rep.fixed_modes_converged) json_push(w, json_string("the fixed-interface modes did not reach their tolerance: the reduced frequencies are approximate"));
    if (rep.dropped)
        json_push(w, json_stringf("%d coordinates above %.6g Hz were dropped: up to %.2f%% of the static flexibility of an interface direction is lost (see "
                                  "compliance_loss per interface: directions with little loss keep their flexibility)", rep.dropped, J->max_frequency_hz,
                                  100 * rep.compliance_loss));
    JsonValue *as = json_set_array(S, "assumptions");
    json_push(as, json_string("first-order floating frame of reference: small linear elastic deformation superposed on large rigid motion; second-order "
                              "terms (centrifugal stiffening or softening, geometric stiffness from axial load) are neglected"));
    json_push(as, json_string("the root selection is clamped to the body's reference frame; each interface selection moves rigidly with its joint origin"));
    json_push(as, json_stringf("modal damping ratio %.4g on every coordinate (%s)", J->zeta, json_get_str(json_get(J->setup, "damping"), "source", "?")));
    json_push(as, json_string("truncated modal basis: static interface loads are exact; distributed inertia loads converge with the fixed-interface modes kept"));
    if (J->mat.ortho) mech_struct_assumptions(as, &J->mat.setup);
    char path[NV_PATH_MAX];
    JsonValue *block = asm_flexible_to_json(X);
    path_join(path, sizeof path, J->run_dir, "flexible_model.json");
    bool ok = block && json_write_file(path, block, JSON_PRETTY);
    json_free(block);
    if (ok) files_entry(S, path, "flexible_model.json", "the assembly body's \"flexible\" block (SI, body frame)");
    /* shapes for later stress recovery and display */
    path_join(path, sizeof path, J->run_dir, "mech_flex_shapes.bin");
    FILE *f = ok ? fopen(path, "wb") : NULL;
    int32_t hdr[5] = {m->nnodes, m->nelems, F->nmodes, J->mat.nmat, J->mat.ortho};
    ok = f && wr(f, "NVMFLX02", 1, 8) && wr(f, hdr, sizeof hdr[0], 5) && wr(f, m->xyz, sizeof(double), 3 * (size_t)m->nnodes) &&
         wr(f, m->conn, sizeof(int), 8 * (size_t)m->nelems) && wr(f, F->omega2, sizeof(double), (size_t)F->nmodes) &&
         wr(f, rep.shapes, sizeof(double), (size_t)F->nmodes * 3 * (size_t)m->nnodes);
    for (int k = 0; ok && k < J->mat.nmat; k++) {
        const OrthoConstants *c9 = &J->mat.mat[k];
        double v[9] = {c9->E[0], c9->E[1], c9->E[2], c9->nu12, c9->nu13, c9->nu23, c9->G12, c9->G23, c9->G13};
        ok = wr(f, v, sizeof(double), 9);
    }
    for (int e = 0; ok && e < m->nelems; e++) {
        int32_t em = J->mat.elem_mat ? J->mat.elem_mat[e] : 0;
        ok = wr(f, &em, sizeof em, 1);
    }
    ok = ok && wr(f, J->mat.R, sizeof(double), 9);
    if (f) ok &= fclose(f) == 0;
    if (ok)
        files_entry(S, path, "mech_flex_shapes.bin",
                    "NVMFLX02: int32 nodes, elements, coordinates, materials, orthotropic flag; node coordinates (m, FE frame); connectivity; omega^2; "
                    "displacement per unit coordinate (FE axes); per material E1 E2 E3 nu12 nu13 nu23 G12 G23 G13; int32 material per element; material axes (9)");
    free(rep.shapes), free(rep.interface_loss);
    asm_flexible_free(X);
    path_join(path, sizeof path, J->run_dir, "summary.json");
    if (!ok || !json_write_file(path, S, JSON_PRETTY | JSON_SORTED)) {
        json_free(S);
        snprintf(code, codelen, "IO_ERROR");
        snprintf(err, errlen, "cannot write the reduced model to %s", J->run_dir);
        return false;
    }
    job_progress(job, 1, "done");
    job_set_summary(job, S);
    return true;
}

void mech_flex_job_free(void *data) {
    MechFlexJob *J = data;
    if (!J) return;
    static_model_free(&J->model);
    vib_material_free(&J->mat);
    free(J->density), free(J->root);
    for (int i = 0; J->itf_nodes && i < J->ninterfaces; i++) free(J->itf_nodes[i]);
    free(J->itf_nodes), free(J->itf_count), free(J->itf_point), free(J->itf_joint);
    json_free(J->setup);
    free(J);
}
