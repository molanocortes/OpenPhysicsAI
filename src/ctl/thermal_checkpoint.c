/* thermal_checkpoint.c - durable checkpoints and the stored-frame stream of transient thermal runs
 * (format and guarantees: transient_analysis.h) */
#include "../core/sha256.h"
#include "engine.h"
#include "project.h"
#include "transient_analysis.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char CK_MAGIC[8] = {'N', 'V', 'C', 'K', 'P', 'T', '0', '1'};
static const char FR_MAGIC[8] = {'N', 'V', 'F', 'R', 'A', 'M', 'E', 'S'};
/* version 2: per-group energy budgets; 3: advection budgets; 4: several thermal participants, the mapped flow and the
 * coupling statistics */
enum { CK_VERSION = 4, FR_HEADER_BYTES = 32 };

/* ---- hashes of the resolved solver inputs ---------------------------------------------------------------------- */

static void h_d(Sha256 *h, double v) { sha256_update(h, &v, sizeof v); }
static void h_i(Sha256 *h, long long v) { sha256_update(h, &v, sizeof v); }
static void h_table(Sha256 *h, const ThermalTable *t) {
    h_i(h, t->n);
    if (t->n > 0) sha256_update(h, t->t, (size_t)t->n * sizeof(double)), sha256_update(h, t->v, (size_t)t->n * sizeof(double));
}
static void h_mtable(Sha256 *h, const MatTable *t) {
    h_i(h, t->n);
    if (t->n > 0) sha256_update(h, t->t, (size_t)t->n * sizeof(double)), sha256_update(h, t->v, (size_t)t->n * sizeof(double));
}
static void h_done(Sha256 *h, char out[65]) {
    unsigned char d[32];
    sha256_final(h, d);
    sha256_hex(d, out);
}

void thermal_case_hashes(const ThermalCase *c, ThermalCaseHashes *hs) {
    Sha256 h;
    size_t nn = (size_t)c->nnodes, ne = (size_t)c->nelems;
    /* mesh: coordinates, connectivity and the body of every element */
    sha256_init(&h);
    h_i(&h, c->nnodes), h_i(&h, c->nelems);
    sha256_update(&h, c->xyz, 3 * nn * sizeof(double));
    sha256_update(&h, c->conn, 8 * ne * sizeof(int));
    sha256_update(&h, c->elem_body, ne);
    h_done(&h, hs->mesh);
    /* materials: the tables the solvers read, per slot, and which slot every element uses */
    sha256_init(&h);
    h_i(&h, c->nmat);
    sha256_update(&h, c->elem_mat, ne * sizeof(int));
    for (int i = 0; i < c->nmat; i++) {
        const ThermalMaterial *m = &c->tmat[i];
        h_table(&h, &m->k), h_table(&h, &m->cp), h_table(&h, &m->rho), h_table(&h, &m->k2), h_table(&h, &m->k3);
        sha256_update(&h, m->axes, sizeof m->axes);
        h_d(&h, m->latent_heat), h_d(&h, m->solidus), h_d(&h, m->liquidus);
        if (c->settings.mechanical)
            for (int p = MATP_E; p <= MATP_ALPHA; p++) h_mtable(&h, &c->rec[i].prop[p]);
        if (c->settings.cht) h_mtable(&h, &c->rec[i].prop[MATP_VISCOSITY]);
    }
    h_done(&h, hs->materials);
    /* thermal conditions as resolved onto the mesh, with their schedules */
    sha256_init(&h);
    sha256_update(&h, c->fixed_base ? c->fixed_base : c->fixed, nn);
    sha256_update(&h, c->fixed_T, nn * sizeof(double));
    h_i(&h, c->ntfaces);
    for (int i = 0; i < c->ntfaces; i++) {
        const ThermalFace *f = &c->tfaces[i];
        h_i(&h, f->elem), h_i(&h, f->face), h_i(&h, f->kind);
        h_d(&h, c->tface_base ? c->tface_base[i] : f->value), h_d(&h, f->ambient);
        h_i(&h, c->tface_bc ? c->tface_bc[i] : -1);
    }
    h_i(&h, c->nsources);
    for (int k = 0; k < c->nsources; k++) h_i(&h, c->source_bc[k]), h_i(&h, c->source_body[k]), h_d(&h, c->source_magnitude[k]);
    h_i(&h, c->nbc);
    for (int b = 0; b < c->nbc; b++) {
        h_i(&h, c->nsched[b]);
        for (int k = 0; k < c->nsched[b]; k++) h_d(&h, c->sched_t[b][k]), h_d(&h, c->sched_f[b][k]);
    }
    h_i(&h, c->ninterface);
    if (c->ninterface > 0) sha256_update(&h, c->interfaces, (size_t)c->ninterface * sizeof(ThermalInterfaceNode));
    if (c->advect) {
        sha256_update(&h, c->advect, ne);
        h_i(&h, c->fluid_body);
        sha256_update(&h, c->flow_n, sizeof c->flow_n);
        h_d(&h, c->flow_dx);
        sha256_update(&h, c->flow_origin, sizeof c->flow_origin);
    }
    if (c->has_mech) {
        h_i(&h, c->mech.nnodes);
        sha256_update(&h, c->mech.fixed, 3 * nn);
        sha256_update(&h, c->mech.fixed_value, 3 * nn * sizeof(double));
        sha256_update(&h, c->mech.nodal_force, 3 * nn * sizeof(double));
        sha256_update(&h, c->mech.gravity, sizeof c->mech.gravity);
    }
    h_done(&h, hs->conditions);
    /* physics and output settings; run control (checkpoint cadence, max_steps) is deliberately left out */
    const ThermalSettings *s = &c->settings;
    sha256_init(&h);
    h_d(&h, s->end_time), h_d(&h, s->time_step), h_i(&h, s->stepping), h_d(&h, s->theta), h_i(&h, s->consistent_capacity);
    h_d(&h, s->initial_temperature), h_d(&h, s->reference_temperature), h_i(&h, s->output_every), h_i(&h, s->max_picard), h_d(&h, s->picard_tol);
    h_i(&h, s->property_eval), h_i(&h, s->element_capacity), h_i(&h, s->phase_change), h_i(&h, s->mechanical), h_i(&h, s->formulation);
    h_i(&h, s->solver), h_d(&h, s->pcg_tol);
    h_d(&h, s->dt_min), h_d(&h, s->dt_max), h_d(&h, s->step_safety), h_d(&h, s->max_step_growth), h_d(&h, s->max_step_shrink);
    h_i(&h, s->max_rejections), h_d(&h, s->temporal_relative), h_d(&h, s->temporal_temperature), h_d(&h, s->temporal_enthalpy);
    h_i(&h, s->temporal_temperature_only);
    if (s->cht) {
        h_i(&h, s->cht);
        sha256_update(&h, s->fluid_body, strlen(s->fluid_body));
        h_d(&h, s->inlet_velocity), h_d(&h, s->inlet_temperature);
        for (int w = 0; w < 4; w++) h_i(&h, s->flow_wall[w]);
        h_d(&h, s->flow_steady_tolerance);
        h_i(&h, s->coupling_monolithic), h_d(&h, s->coupling_relative), h_i(&h, s->coupling_max_iterations), h_d(&h, s->coupling_relaxation);
    }
    h_i(&h, s->noutput_times);
    if (s->noutput_times > 0) sha256_update(&h, s->output_times, (size_t)s->noutput_times * sizeof(double));
    h_d(&h, s->output_interval);
    h_done(&h, hs->settings);
}

/* ---- state serialisation: explicit field order, exact binary values ---------------------------------------------- */

enum { STATE_D = 40 + 7 * THERMAL_MAX_GROUPS, STATE_I = 32 };

typedef struct {
    double d[STATE_D];
    int64_t i[STATE_I];
    int nd, ni;
} Packed;

static void pd(Packed *p, double v) { p->d[p->nd++] = v; }
static void pi(Packed *p, long long v) { p->i[p->ni++] = v; }

static void pack_state(const ThermalIntegratorState *st, Packed *p) {
    memset(p, 0, sizeof *p);
    const StepController *c = &st->controller;
    const StepControllerSettings *cs = &c->set;
    const ThermalBudget *b = &st->budget;
    const ThermalWork *w = &st->work;
    pd(p, st->t);
    pd(p, cs->dt_initial), pd(p, cs->dt_min), pd(p, cs->dt_max), pd(p, cs->safety), pd(p, cs->max_growth), pd(p, cs->max_shrink), pd(p, cs->solver_shrink);
    pd(p, c->dt), pd(p, c->err_prev), pd(p, c->last_factor);
    pd(p, b->source), pd(p, b->boundary), pd(p, b->prescribed), pd(p, b->interface_net), pd(p, b->stored), pd(p, b->enthalpy);
    pd(p, b->worst_balance), pd(p, b->worst_mismatch), pd(p, b->liquid_volume_max);
    pd(p, w->seconds_solve), pd(p, w->seconds_estimate), pd(p, w->error_max), pd(p, w->error_sum), pd(p, w->dt_min), pd(p, w->dt_max);
    for (int g = 0; g < THERMAL_MAX_GROUPS; g++)
        pd(p, b->group_stored[g]), pd(p, b->group_source[g]), pd(p, b->group_boundary[g]), pd(p, b->group_prescribed[g]), pd(p, b->interface_heat[g]);
    pd(p, b->advection), pd(p, b->inflow), pd(p, b->enthalpy_inflow), pd(p, b->enthalpy_outflow), pd(p, b->divergence);
    for (int g = 0; g < THERMAL_MAX_GROUPS; g++) pd(p, b->group_advection[g]), pd(p, b->group_inflow[g]);
    pi(p, st->fixed_index);
    pi(p, cs->max_rejections), pi(p, cs->max_solver_failures), pi(p, cs->max_steps), pi(p, cs->order);
    pi(p, c->after_failure), pi(p, c->rejections), pi(p, c->solver_failures), pi(p, c->accepted), pi(p, c->rejected_total), pi(p, c->solver_failures_total);
    pi(p, b->picard_max);
    pi(p, w->attempts), pi(p, w->accepted), pi(p, w->rejected), pi(p, w->nonlinear_failures), pi(p, w->linear_failures), pi(p, w->state_failures);
    pi(p, w->solves), pi(p, w->assemblies), pi(p, w->picard_iterations), pi(p, w->linear_iterations);
}

static void unpack_state(const Packed *p, ThermalIntegratorState *st) {
    memset(st, 0, sizeof *st);
    int d = 0, i = 0;
    StepController *c = &st->controller;
    StepControllerSettings *cs = &c->set;
    ThermalBudget *b = &st->budget;
    ThermalWork *w = &st->work;
    st->t = p->d[d++];
    cs->dt_initial = p->d[d++], cs->dt_min = p->d[d++], cs->dt_max = p->d[d++], cs->safety = p->d[d++], cs->max_growth = p->d[d++];
    cs->max_shrink = p->d[d++], cs->solver_shrink = p->d[d++];
    c->dt = p->d[d++], c->err_prev = p->d[d++], c->last_factor = p->d[d++];
    b->source = p->d[d++], b->boundary = p->d[d++], b->prescribed = p->d[d++], b->interface_net = p->d[d++], b->stored = p->d[d++];
    b->enthalpy = p->d[d++], b->worst_balance = p->d[d++], b->worst_mismatch = p->d[d++], b->liquid_volume_max = p->d[d++];
    w->seconds_solve = p->d[d++], w->seconds_estimate = p->d[d++], w->error_max = p->d[d++], w->error_sum = p->d[d++];
    w->dt_min = p->d[d++], w->dt_max = p->d[d++];
    for (int g = 0; g < THERMAL_MAX_GROUPS; g++) {
        b->group_stored[g] = p->d[d++], b->group_source[g] = p->d[d++], b->group_boundary[g] = p->d[d++];
        b->group_prescribed[g] = p->d[d++], b->interface_heat[g] = p->d[d++];
    }
    b->advection = p->d[d++], b->inflow = p->d[d++], b->enthalpy_inflow = p->d[d++], b->enthalpy_outflow = p->d[d++], b->divergence = p->d[d++];
    for (int g = 0; g < THERMAL_MAX_GROUPS; g++) b->group_advection[g] = p->d[d++], b->group_inflow[g] = p->d[d++];
    st->fixed_index = (long)p->i[i++];
    cs->max_rejections = (int)p->i[i++], cs->max_solver_failures = (int)p->i[i++], cs->max_steps = (long)p->i[i++], cs->order = (int)p->i[i++];
    c->after_failure = p->i[i++] != 0, c->rejections = (int)p->i[i++], c->solver_failures = (int)p->i[i++], c->accepted = (long)p->i[i++];
    c->rejected_total = (long)p->i[i++], c->solver_failures_total = (long)p->i[i++];
    b->picard_max = (int)p->i[i++];
    w->attempts = (long)p->i[i++], w->accepted = (long)p->i[i++], w->rejected = (long)p->i[i++], w->nonlinear_failures = (long)p->i[i++];
    w->linear_failures = (long)p->i[i++], w->state_failures = (long)p->i[i++], w->solves = (long)p->i[i++], w->assemblies = (long)p->i[i++];
    w->picard_iterations = (long)p->i[i++], w->linear_iterations = (long)p->i[i++];
}

/* ---- file helpers ------------------------------------------------------------------------------------------------ */

static bool write_all(FILE *f, const void *p, size_t n) { return n == 0 || fwrite(p, 1, n, f) == n; }

static bool sync_file(FILE *f) { return fflush(f) == 0 && fsync(fileno(f)) == 0; }

static void sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/* ---- checkpoint -------------------------------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    char type; /* 'd' double, 'q' int64 */
    size_t count;
    const void *data;
} CkArr;

enum { CK_MAX_ARRAYS = 16, ORCH_WORK_FIELDS = 9 };

static void pack_orch_work(const OrchWork *w, double out[ORCH_WORK_FIELDS]) {
    out[0] = (double)w->attempts, out[1] = (double)w->accepted, out[2] = (double)w->rejected_error, out[3] = (double)w->rejected_coupling;
    out[4] = (double)w->solver_failures, out[5] = (double)w->coupling_iterations, out[6] = w->coupling_residual_max, out[7] = w->error_max;
    out[8] = (double)w->evaluations;
}

static void unpack_orch_work(const double in[ORCH_WORK_FIELDS], OrchWork *w) {
    w->attempts = (long)in[0], w->accepted = (long)in[1], w->rejected_error = (long)in[2], w->rejected_coupling = (long)in[3];
    w->solver_failures = (long)in[4], w->coupling_iterations = (long)in[5], w->coupling_residual_max = in[6], w->error_max = in[7];
    w->evaluations = (long)in[8];
}

bool thermal_checkpoint_write(ThermalCase *c, const ThermalIntegrator *const *ti, int nti, int frames, char *err, size_t errlen) {
    if (!c->run_dir[0]) {
        snprintf(err, errlen, "the run has no directory to write a checkpoint into");
        return false;
    }
    if (nti < 1 || nti > 2) {
        snprintf(err, errlen, "a checkpoint holds one or two thermal participants (got %d)", nti);
        return false;
    }
    char path[NV_PATH_MAX], tmp[NV_PATH_MAX + 8], prev[NV_PATH_MAX];
    path_join(path, sizeof path, c->run_dir, "checkpoint.nvc");
    path_join(prev, sizeof prev, c->run_dir, "checkpoint.prev.nvc");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    /* stored frames the checkpoint refers to must be on disk first */
    if (!thermal_frames_sync(c, err, errlen)) return false;
    ThermalIntegratorState st, st2;
    tint_get_state(ti[0], &st);
    Packed pk, pk2;
    pack_state(&st, &pk);
    if (nti == 2) {
        tint_get_state(ti[1], &st2);
        pack_state(&st2, &pk2);
    }
    if (frames < 1 || frames > c->noutputs) {
        snprintf(err, errlen, "a checkpoint needs between 1 and %d stored frames (got %d)", c->noutputs, frames);
        return false;
    }
    double ow[ORCH_WORK_FIELDS];
    pack_orch_work(&c->orch_work, ow);
    size_t nvel = c->velocity && c->flow.done ? 3 * (size_t)c->nnodes : 0;
    CkArr arr[CK_MAX_ARRAYS];
    int na = 0;
    arr[na++] = (CkArr){"state_d", 'd', (size_t)pk.nd, pk.d};
    arr[na++] = (CkArr){"state_i", 'q', (size_t)pk.ni, pk.i};
    arr[na++] = (CkArr){"T", 'd', (size_t)tint_node_count(ti[0]), tint_temperature(ti[0])};
    arr[na++] = (CkArr){"frame_times", 'd', (size_t)frames, c->times};
    arr[na++] = (CkArr){"step_t", 'd', (size_t)c->nhistory, c->step_t};
    arr[na++] = (CkArr){"step_dt", 'd', (size_t)c->nhistory, c->step_dt};
    arr[na++] = (CkArr){"step_err", 'd', (size_t)c->nhistory, c->step_err};
    arr[na++] = (CkArr){"resume_times", 'd', (size_t)c->nresumes, c->resume_times};
    arr[na++] = (CkArr){"orch_work", 'd', ORCH_WORK_FIELDS, ow};
    if (nti == 2) {
        arr[na++] = (CkArr){"state_d.1", 'd', (size_t)pk2.nd, pk2.d};
        arr[na++] = (CkArr){"state_i.1", 'q', (size_t)pk2.ni, pk2.i};
        arr[na++] = (CkArr){"T.1", 'd', (size_t)tint_node_count(ti[1]), tint_temperature(ti[1])};
    }
    if (nvel) arr[na++] = (CkArr){"velocity", 'd', nvel, c->velocity};
    Sha256 h;
    sha256_init(&h);
    uint64_t payload = 0;
    for (int i = 0; i < na; i++) {
        size_t bytes = arr[i].count * 8;
        if (bytes) sha256_update(&h, arr[i].data, bytes);
        payload += bytes;
    }
    char digest[65];
    h_done(&h, digest);
    long seq = c->checkpoint_sequence + 1;
    JsonValue *hd = json_object();
    json_set_string(hd, "format", "navier-thermal-checkpoint");
    json_set_int(hd, "version", CK_VERSION);
    json_set_string(hd, "job_id", c->job_id);
    json_set_int(hd, "sequence", seq);
    char now[32];
    iso_time_now(now, sizeof now);
    json_set_string(hd, "created", now);
    JsonValue *sw = json_set_object(hd, "software");
    json_set_string(sw, "version", NAVIER_AM_VERSION);
    JsonValue *hh = json_set_object(hd, "hashes");
    json_set_string(hh, "mesh", c->hashes.mesh);
    json_set_string(hh, "materials", c->hashes.materials);
    json_set_string(hh, "conditions", c->hashes.conditions);
    json_set_string(hh, "settings", c->hashes.settings);
    json_set_number(hd, "time_s", st.t);
    json_set_number(hd, "end_time_s", c->settings.end_time);
    json_set_int(hd, "accepted_steps", st.work.accepted);
    json_set_int(hd, "nnodes", tint_node_count(ti[0]));
    json_set_int(hd, "participants", nti);
    if (nti == 2) json_set_int(hd, "nnodes_2", tint_node_count(ti[1]));
    json_set_int(hd, "velocity_count", (long long)nvel);
    json_set_int(hd, "frames", frames);
    json_set_int(hd, "nhistory", c->nhistory);
    json_set_int(hd, "nresumes", c->nresumes);
    json_set_bool(hd, "has_mechanical", c->has_mech);
    if (nvel) {
        JsonValue *fl = json_set_object(hd, "flow");
        json_set_number(fl, "tau", c->flow.tau), json_set_number(fl, "u_lattice", c->flow.u_lattice), json_set_number(fl, "mach", c->flow.mach);
        json_set_number(fl, "reynolds_cell", c->flow.reynolds_cell), json_set_number(fl, "reynolds_hydraulic", c->flow.reynolds_hydraulic);
        json_set_number(fl, "dt", c->flow.dt), json_set_number(fl, "physical_time", c->flow.physical_time), json_set_number(fl, "change", c->flow.change);
        json_set_number(fl, "density_min", c->flow.density_min), json_set_number(fl, "density_max", c->flow.density_max);
        json_set_number(fl, "inlet_flux", c->flow.inlet_flux), json_set_number(fl, "outlet_flux", c->flow.outlet_flux);
        json_set_number(fl, "scale_min", c->flow.scale_min), json_set_number(fl, "scale_max", c->flow.scale_max);
        json_set_number(fl, "seconds", c->flow.seconds), json_set_int(fl, "steps", c->flow.steps);

    }
    JsonValue *al = json_set_array(hd, "arrays");
    for (int i = 0; i < na; i++) {
        JsonValue *o = json_object();
        json_set_string(o, "name", arr[i].name);
        char t[2] = {arr[i].type, 0};
        json_set_string(o, "type", t);
        json_set_int(o, "count", (long long)arr[i].count);
        json_push(al, o);
    }
    json_set_int(hd, "payload_bytes", (long long)payload);
    json_set_string(hd, "payload_sha256", digest);
    json_set_string(hd, "history_variables",
                    "none besides temperature: the phase state is the equilibrium liquid fraction of the temperature, and schedules are "
                    "functions of time. A flow study also stores the steady velocity it computed once, so that a resumed run does not "
                    "depend on recomputing it");
    size_t hlen = 0;
    char *text = json_dump(hd, 0, &hlen, NULL);
    json_free(hd);
    FILE *f = text ? fopen(tmp, "wb") : NULL;
    if (!f) {
        free(text);
        snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno));
        return false;
    }
    uint64_t hl = hlen;
    bool ok = write_all(f, CK_MAGIC, 8) && write_all(f, &hl, 8) && write_all(f, text, hlen);
    free(text);
    for (int i = 0; ok && i < na; i++) ok = write_all(f, arr[i].data, arr[i].count * 8);
    ok = ok && sync_file(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(tmp); /* the previous checkpoint stays untouched */
        snprintf(err, errlen, "writing the checkpoint failed (disk full?): %s", strerror(errno));
        return false;
    }
    if (path_is_file(path) && rename(path, prev) != 0) {
        remove(tmp);
        snprintf(err, errlen, "cannot keep the previous checkpoint: %s", strerror(errno));
        return false;
    }
    if (rename(tmp, path) != 0) {
        snprintf(err, errlen, "cannot publish the checkpoint: %s", strerror(errno));
        return false;
    }
    sync_dir(c->run_dir);
    c->checkpoint_sequence = seq;
    c->last_checkpoint_time = st.t;
    return true;
}

void thermal_checkpoint_free(ThermalCheckpoint *ck) {
    if (!ck) return;
    free(ck->T), free(ck->T2), free(ck->velocity), free(ck->frame_times), free(ck->step_t), free(ck->step_dt), free(ck->step_err);
    json_free(ck->flow_info);
    free(ck);
}

static ThermalCheckpoint *read_one(const char *path, bool header_only, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return NULL;
    }
    ThermalCheckpoint *ck = calloc(1, sizeof *ck);
    char magic[8], *text = NULL;
    uint64_t hl = 0;
    JsonValue *h = NULL;
    struct stat sb;
    if (!ck || fstat(fileno(f), &sb) != 0 || fread(magic, 1, 8, f) != 8 || memcmp(magic, CK_MAGIC, 8) != 0 || fread(&hl, 8, 1, f) != 1 || hl == 0 ||
        hl > (8u << 20)) {
        snprintf(err, errlen, "%s: not a checkpoint file (bad magic or header length)", path);
        goto bad;
    }
    text = malloc((size_t)hl);
    JsonError je;
    if (!text || fread(text, 1, (size_t)hl, f) != hl || !(h = json_parse(text, (size_t)hl, NULL, &je))) {
        snprintf(err, errlen, "%s: the checkpoint header is truncated or unreadable", path);
        goto bad;
    }
    if (strcmp(json_get_str(h, "format", ""), "navier-thermal-checkpoint") || json_get_int(h, "version", 0) != CK_VERSION) {
        snprintf(err, errlen, "%s: unsupported checkpoint format or version %lld (this build reads version %d; a run checkpointed by another version "
                              "must be started again)",
                 path, json_get_int(h, "version", 0), CK_VERSION);
        goto bad;
    }
    long long payload = json_get_int(h, "payload_bytes", -1);
    if (payload < 0 || (long long)sb.st_size != 16 + (long long)hl + payload) {
        snprintf(err, errlen, "%s: the file is %lld bytes but its header declares %lld: truncated or damaged", path, (long long)sb.st_size,
                 16 + (long long)hl + payload);
        goto bad;
    }
    snprintf(ck->file, sizeof ck->file, "%s", path);
    snprintf(ck->job_id, sizeof ck->job_id, "%s", json_get_str(h, "job_id", ""));
    snprintf(ck->created, sizeof ck->created, "%s", json_get_str(h, "created", ""));
    snprintf(ck->software, sizeof ck->software, "%s", json_get_str(json_get(h, "software"), "version", ""));
    const JsonValue *hh = json_get(h, "hashes");
    snprintf(ck->hashes.mesh, 65, "%s", json_get_str(hh, "mesh", ""));
    snprintf(ck->hashes.materials, 65, "%s", json_get_str(hh, "materials", ""));
    snprintf(ck->hashes.conditions, 65, "%s", json_get_str(hh, "conditions", ""));
    snprintf(ck->hashes.settings, 65, "%s", json_get_str(hh, "settings", ""));
    ck->sequence = (long)json_get_int(h, "sequence", 0);
    ck->nnodes = (int)json_get_int(h, "nnodes", -1);
    ck->frames = (int)json_get_int(h, "frames", -1);
    ck->nhistory = (int)json_get_int(h, "nhistory", -1);
    ck->nresumes = (int)json_get_int(h, "nresumes", 0);
    ck->has_mech = json_get_bool(h, "has_mechanical", false);
    if (ck->nnodes <= 0 || ck->frames < 1 || ck->frames > TC_MAX_OUTPUTS || ck->nhistory < 0 || ck->nresumes < 0 || ck->nresumes > 16) {
        snprintf(err, errlen, "%s: implausible sizes in the checkpoint header", path);
        goto bad;
    }
    ck->nparticipants = (int)json_get_int(h, "participants", 1);
    ck->nnodes2 = (int)json_get_int(h, "nnodes_2", 0);
    ck->nvelocity = (int)json_get_int(h, "velocity_count", 0);
    if (ck->nparticipants < 1 || ck->nparticipants > 2 || (ck->nparticipants == 2 && ck->nnodes2 <= 0) || ck->nvelocity < 0 || ck->nvelocity % 3) {
        snprintf(err, errlen, "%s: implausible participant sizes in the checkpoint header", path);
        goto bad;
    }
    const JsonValue *al = json_get(h, "arrays");
    enum { BASE = 9 };
    const char *names[CK_MAX_ARRAYS] = {"state_d", "state_i", "T", "frame_times", "step_t", "step_dt", "step_err", "resume_times", "orch_work"};
    size_t counts[CK_MAX_ARRAYS] = {0, 0, (size_t)ck->nnodes, (size_t)ck->frames, (size_t)ck->nhistory, (size_t)ck->nhistory, (size_t)ck->nhistory,
                                    (size_t)ck->nresumes, ORCH_WORK_FIELDS};
    int nexp = BASE;
    if (ck->nparticipants == 2) {
        names[nexp] = "state_d.1", counts[nexp++] = 0;
        names[nexp] = "state_i.1", counts[nexp++] = 0;
        names[nexp] = "T.1", counts[nexp++] = (size_t)ck->nnodes2;
    }
    if (ck->nvelocity > 0) names[nexp] = "velocity", counts[nexp++] = (size_t)ck->nvelocity;
    if ((int)json_len(al) != nexp) {
        snprintf(err, errlen, "%s: unexpected array table", path);
        goto bad;
    }
    long long total = 0;
    for (int i = 0; i < nexp; i++) {
        const JsonValue *o = json_at(al, (size_t)i);
        long long cnt = json_get_int(o, "count", -1);
        bool state_d = !strcmp(names[i], "state_d") || !strcmp(names[i], "state_d.1"), state_i = !strcmp(names[i], "state_i") || !strcmp(names[i], "state_i.1");
        if (strcmp(json_get_str(o, "name", ""), names[i]) || cnt < 0 || (!state_d && !state_i && (size_t)cnt != counts[i]) || (state_d && cnt > STATE_D) ||
            (state_i && cnt > STATE_I)) {
            snprintf(err, errlen, "%s: the array table does not match the header", path);
            goto bad;
        }
        counts[i] = (size_t)cnt;
        total += cnt * 8;
    }
    if (total != payload) {
        snprintf(err, errlen, "%s: the arrays do not add up to the declared payload", path);
        goto bad;
    }
    /* read the payload and verify its hash, even for a header-only read */
    Packed pk, pk2;
    memset(&pk, 0, sizeof pk), memset(&pk2, 0, sizeof pk2);
    pk.nd = (int)counts[0], pk.ni = (int)counts[1];
    ck->T = malloc((size_t)ck->nnodes * sizeof(double));
    ck->frame_times = malloc((size_t)ck->frames * sizeof(double));
    size_t nh = counts[4] ? counts[4] : 1;
    ck->step_t = malloc(nh * sizeof(double)), ck->step_dt = malloc(nh * sizeof(double)), ck->step_err = malloc(nh * sizeof(double));
    if (ck->nparticipants == 2) ck->T2 = malloc((size_t)ck->nnodes2 * sizeof(double));
    if (ck->nvelocity > 0) ck->velocity = malloc((size_t)ck->nvelocity * sizeof(double));
    if (!ck->T || !ck->frame_times || !ck->step_t || !ck->step_dt || !ck->step_err || (ck->nparticipants == 2 && !ck->T2) || (ck->nvelocity > 0 && !ck->velocity)) {
        snprintf(err, errlen, "out of memory reading the checkpoint");
        goto bad;
    }
    double ow[ORCH_WORK_FIELDS];
    void *dst[CK_MAX_ARRAYS] = {pk.d, pk.i, ck->T, ck->frame_times, ck->step_t, ck->step_dt, ck->step_err, ck->resume_times, ow};
    int k = BASE;
    if (ck->nparticipants == 2) {
        pk2.nd = (int)counts[k], dst[k++] = pk2.d;
        pk2.ni = (int)counts[k], dst[k++] = pk2.i;
        dst[k++] = ck->T2;
    }
    if (ck->nvelocity > 0) dst[k++] = ck->velocity;
    Sha256 hs;
    sha256_init(&hs);
    for (int i = 0; i < nexp; i++) {
        if (!counts[i]) continue;
        if (fread(dst[i], 8, counts[i], f) != counts[i]) {
            snprintf(err, errlen, "%s: the checkpoint payload is truncated", path);
            goto bad;
        }
        sha256_update(&hs, dst[i], counts[i] * 8);
    }
    char digest[65];
    h_done(&hs, digest);
    if (strcmp(digest, json_get_str(h, "payload_sha256", ""))) {
        snprintf(err, errlen, "%s: the checkpoint payload does not match its SHA-256: the file is damaged", path);
        goto bad;
    }
    Packed shape;
    ThermalIntegratorState zero;
    memset(&zero, 0, sizeof zero);
    pack_state(&zero, &shape); /* the field counts this build writes */
    if (pk.nd != shape.nd || pk.ni != shape.ni || (ck->nparticipants == 2 && (pk2.nd != shape.nd || pk2.ni != shape.ni))) {
        snprintf(err, errlen, "%s: the saved integrator state has %d + %d fields, this build expects %d + %d", path, pk.nd, pk.ni, shape.nd, shape.ni);
        goto bad;
    }
    unpack_state(&pk, &ck->state);
    if (ck->nparticipants == 2) unpack_state(&pk2, &ck->state2);
    unpack_orch_work(ow, &ck->orch_work);
    if (json_get(h, "flow")) ck->flow_info = json_clone(json_get(h, "flow"));
    (void)header_only;
    fclose(f);
    free(text);
    json_free(h);
    return ck;
bad:
    fclose(f);
    free(text);
    json_free(h);
    thermal_checkpoint_free(ck);
    return NULL;
}

ThermalCheckpoint *thermal_checkpoint_read(const char *run_dir, bool header_only, char *err, size_t errlen) {
    char path[NV_PATH_MAX], prev[NV_PATH_MAX], e1[512] = "", e2[512] = "";
    path_join(path, sizeof path, run_dir, "checkpoint.nvc");
    path_join(prev, sizeof prev, run_dir, "checkpoint.prev.nvc");
    ThermalCheckpoint *ck = path_is_file(path) ? read_one(path, header_only, e1, sizeof e1) : NULL;
    if (ck) return ck;
    if (!path_is_file(path)) snprintf(e1, sizeof e1, "no checkpoint.nvc");
    ck = path_is_file(prev) ? read_one(prev, header_only, e2, sizeof e2) : NULL;
    if (ck) {
        snprintf(ck->fallback_reason, sizeof ck->fallback_reason, "%s", e1);
        return ck;
    }
    snprintf(err, errlen, "no usable checkpoint in %s (%s%s%s)", run_dir, e1, e2[0] ? "; previous: " : "", e2);
    return NULL;
}

JsonValue *thermal_checkpoint_info_json(const ThermalCheckpoint *ck) {
    JsonValue *o = json_object();
    json_set_bool(o, "available", true);
    json_set_int(o, "sequence", ck->sequence);
    json_set_number(o, "time_s", ck->state.t);
    json_set_int(o, "accepted_steps", ck->state.work.accepted);
    json_set_int(o, "stored_frames", ck->frames);
    json_set_int(o, "thermal_participants", ck->nparticipants);
    json_set_bool(o, "holds_flow_field", ck->nvelocity > 0);
    json_set_string(o, "created", ck->created);
    json_set_string(o, "file", ck->file);
    if (ck->fallback_reason[0]) json_set_string(o, "newest_checkpoint_rejected", ck->fallback_reason);
    return o;
}

/* ---- stored frames ----------------------------------------------------------------------------------------------- */

static size_t frame_record_bytes(const ThermalCase *c) { return 16 + (size_t)c->nnodes * 8 * (c->has_mech ? 5 : 1); }

bool thermal_frames_create(ThermalCase *c, char *err, size_t errlen) {
    if (!c->run_dir[0]) return true; /* runs without a directory (tests) keep frames in memory only */
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, c->run_dir, "frames.nvf");
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errlen, "cannot create %s: %s", path, strerror(errno));
        return false;
    }
    unsigned char hdr[FR_HEADER_BYTES] = {0};
    memcpy(hdr, FR_MAGIC, 8);
    uint64_t nn = (uint64_t)c->nnodes, rec = frame_record_bytes(c);
    memcpy(hdr + 8, &nn, 8);
    memcpy(hdr + 16, &rec, 8);
    hdr[24] = c->has_mech;
    if (!write_all(f, hdr, sizeof hdr)) {
        fclose(f);
        snprintf(err, errlen, "cannot write %s", path);
        return false;
    }
    c->frames_file = f;
    return thermal_frames_append(c, 0, err, errlen);
}

bool thermal_frames_append(ThermalCase *c, int index, char *err, size_t errlen) {
    FILE *f = c->frames_file;
    if (!f) return true;
    uint64_t idx = (uint64_t)index;
    size_t nn = (size_t)c->nnodes, i = (size_t)index;
    bool ok = write_all(f, &idx, 8) && write_all(f, &c->times[i], 8) && write_all(f, c->T + i * nn, nn * 8);
    if (ok && c->has_mech) ok = write_all(f, c->mech_u + i * 3 * nn, 3 * nn * 8) && write_all(f, c->mech_vm + i * nn, nn * 8);
    if (!ok) snprintf(err, errlen, "writing stored frame %d failed (disk full?)", index);
    return ok;
}

bool thermal_frames_sync(ThermalCase *c, char *err, size_t errlen) {
    if (c->frames_file && !sync_file(c->frames_file)) {
        snprintf(err, errlen, "cannot flush the stored frames to disk: %s", strerror(errno));
        return false;
    }
    return true;
}

void thermal_frames_close(ThermalCase *c) {
    if (c->frames_file) fclose(c->frames_file);
    c->frames_file = NULL;
}

bool thermal_frames_restore(ThermalCase *c, const ThermalCheckpoint *ck, char *err, size_t errlen) {
    char path[NV_PATH_MAX];
    path_join(path, sizeof path, c->run_dir, "frames.nvf");
    FILE *f = fopen(path, "r+b");
    if (!f) {
        snprintf(err, errlen, "the checkpoint refers to %d stored frames but %s cannot be opened: %s", ck->frames, path, strerror(errno));
        return false;
    }
    unsigned char hdr[FR_HEADER_BYTES];
    uint64_t nn = 0, rec = 0;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, FR_MAGIC, 8) != 0) {
        fclose(f);
        snprintf(err, errlen, "%s is not a stored-frame file", path);
        return false;
    }
    memcpy(&nn, hdr + 8, 8), memcpy(&rec, hdr + 16, 8);
    if (nn != (uint64_t)c->nnodes || rec != frame_record_bytes(c) || (hdr[24] != 0) != c->has_mech) {
        fclose(f);
        snprintf(err, errlen, "%s was written for a different mesh or analysis", path);
        return false;
    }
    if (ck->frames > c->noutputs) {
        fclose(f);
        snprintf(err, errlen, "the checkpoint has %d stored frames but this run stores %d", ck->frames, c->noutputs);
        return false;
    }
    size_t n = (size_t)c->nnodes;
    for (int i = 0; i < ck->frames; i++) {
        uint64_t idx;
        double t;
        size_t k = (size_t)i;
        bool ok = fread(&idx, 8, 1, f) == 1 && fread(&t, 8, 1, f) == 1 && fread(c->T + k * n, 8, n, f) == n;
        if (ok && c->has_mech) ok = fread(c->mech_u + k * 3 * n, 8, 3 * n, f) == 3 * n && fread(c->mech_vm + k * n, 8, n, f) == n;
        if (!ok || idx != (uint64_t)i || memcmp(&t, &ck->frame_times[i], 8) != 0) {
            fclose(f);
            snprintf(err, errlen, "stored frame %d in %s is missing or does not match the checkpoint (index %llu, time %.17g against %.17g)", i, path,
                     (unsigned long long)idx, t, ck->frame_times[i]);
            return false;
        }
        c->times[i] = t;
        double lo = INFINITY, hi = -INFINITY;
        for (size_t q = 0; q < n; q++) lo = fmin(lo, c->T[k * n + q]), hi = fmax(hi, c->T[k * n + q]);
        c->tmin[i] = lo, c->tmax[i] = hi;
        if (c->has_mech) {
            double peak = 0, umax = 0;
            for (size_t q = 0; q < n; q++) {
                const double *u = c->mech_u + k * 3 * n + 3 * q;
                peak = fmax(peak, c->mech_vm[k * n + q]);
                umax = fmax(umax, sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]));
            }
            c->mech_peak[i] = peak, c->mech_umax[i] = umax;
        }
    }
    /* frames written after the checkpoint are discarded: the resumed run writes them again */
    long keep = (long)FR_HEADER_BYTES + (long)ck->frames * (long)rec;
    if (fflush(f) != 0 || ftruncate(fileno(f), keep) != 0 || fseek(f, keep, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errlen, "cannot truncate %s after frame %d: %s", path, ck->frames - 1, strerror(errno));
        return false;
    }
    c->frames_file = f;
    return true;
}
