/* thermal_participant.c - thermal integrator as an orchestrator participant (see thermal_participant.h) */
#include "thermal_participant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* before every solve of a trial: the imported data of the matching stage */
static bool tp_stage(void *ctx, ThermalModel *m, int stage, double t0, double t1, char *err, size_t errlen) {
    ThermalParticipant *tp = ctx;
    (void)m;
    int b = tp->blocks > 1 ? stage : 0, ni = tp->ninterface;
    if (tp->T_imported)
        for (int i = 0; i < ni; i++) {
            int n = tp->interface_nodes[i];
            tp->fixed[n] = 1;
            tp->fixed_T[n] = tp->T_in[b * ni + i];
        }
    if (tp->heat_imported) {
        double dt = t1 - t0;
        if (!(dt > 0)) {
            snprintf(err, errlen, "participant '%s': a solve over an empty interval cannot receive heat", tp->name);
            return false;
        }
        for (int i = 0; i < ni; i++) tp->node_power[tp->interface_nodes[i]] = tp->heat_in[b * ni + i] / dt;
    }
    return true;
}

static bool tp_trial(void *self, double t0, double t1, bool estimate, double *error, OrchFailureClass *failure, char *err, size_t errlen) {
    ThermalParticipant *tp = self;
    if (tint_time(tp->ti) != t0) {
        snprintf(err, errlen, "participant '%s' is at t = %.17g s, not at the step start %.17g s", tp->name, tint_time(tp->ti), t0);
        *failure = ORCH_FAILURE_INPUT;
        return false;
    }
    bool ok = tint_trial(tp->ti, t1, estimate && tp->estimate, &tp->trial);
    tp->pending = ok;
    if (!ok) {
        ThermalFailure f = tp->trial.failure;
        *failure = f == THERMAL_FAIL_INPUT ? ORCH_FAILURE_INPUT : (f == THERMAL_FAIL_RESOURCE ? ORCH_FAILURE_RESOURCE : ORCH_FAILURE_SOLVER);
        snprintf(err, errlen, "%s", tp->trial.err);
        return false;
    }
    *error = tp->trial.estimated ? tp->trial.norm.err : -1;
    return true;
}

static void tp_commit(void *self) {
    ThermalParticipant *tp = self;
    if (tp->pending) tint_commit_trial(tp->ti, &tp->trial);
    tp->pending = false;
}

static void tp_attempt_done(void *self, OrchAttemptOutcome outcome, StepDecision decision) {
    ThermalParticipant *tp = self;
    tint_record_trial(tp->ti, &tp->trial, decision, outcome == ORCH_ATTEMPT_ACCEPTED, outcome == ORCH_ATTEMPT_REJECTED_ERROR);
}

static int tp_size(void *self, const char *field) {
    ThermalParticipant *tp = self;
    if (!strcmp(field, "temperature")) return tp->m->nnodes;
    if (!strcmp(field, "velocity")) return tp->velocity ? 3 * tp->m->nnodes : -1;
    if (!strcmp(field, "interface_temperature") || !strcmp(field, "interface_heat_out") || !strcmp(field, "interface_heat_in"))
        return tp->ninterface > 0 ? tp->blocks * tp->ninterface : -1;
    return -1;
}

static bool tp_export(void *self, const char *field, bool trial, double *out) {
    ThermalParticipant *tp = self;
    bool from_trial = trial && tp->pending;
    int ni = tp->ninterface, B = tp->blocks;
    const double *T_acc = tint_temperature(tp->ti);
    if (!strcmp(field, "temperature")) {
        memcpy(out, from_trial ? tint_trial_temperature(tp->ti) : T_acc, (size_t)tp->m->nnodes * sizeof(double));
        return true;
    }
    if (!strcmp(field, "interface_temperature")) {
        for (int b = 0; b < B; b++) {
            const double *T = from_trial ? tint_trial_stage_temperature(tp->ti, b) : T_acc;
            /* an un-estimated trial solved stage 0 only: its end value closes the step, and the half step is
             * interpolated linearly from the accepted state */
            if (!T && b == 2) T = tint_trial_temperature(tp->ti);
            for (int i = 0; i < ni; i++) {
                int n = tp->interface_nodes[i];
                out[b * ni + i] = T ? T[n] : 0.5 * (T_acc[n] + tint_trial_temperature(tp->ti)[n]);
            }
        }
        return true;
    }
    if (!strcmp(field, "interface_heat_out")) {
        /* the reaction energy captured over a stage is heat INTO the model; from the accepted state (a prediction)
         * nothing has crossed yet in this step */
        if (!from_trial) {
            memset(out, 0, (size_t)(B * ni) * sizeof(double));
            return true;
        }
        const double t0 = tp->trial.t0, t1 = tp->trial.t1, tm = t0 + 0.5 * (t1 - t0);
        const double *e0 = tint_trial_stage_node_energy(tp->ti, 0);
        if (!e0) return false;
        for (int b = 0; b < B; b++) {
            const double *e = tint_trial_stage_node_energy(tp->ti, b);
            double share = b == 0 ? 1 : (b == 1 ? (tm - t0) / (t1 - t0) : (t1 - tm) / (t1 - t0));
            for (int i = 0; i < ni; i++) out[b * ni + i] = e ? -e[i] : -e0[i] * share;
        }
        return true;
    }
    return false;
}

static bool tp_import(void *self, const char *field, const double *in) {
    ThermalParticipant *tp = self;
    if (!strcmp(field, "velocity")) {
        if (!tp->velocity) return false;
        memcpy(tp->velocity, in, 3 * (size_t)tp->m->nnodes * sizeof(double));
        return true;
    }
    size_t bytes = (size_t)(tp->blocks * tp->ninterface) * sizeof(double);
    if (!strcmp(field, "interface_temperature")) {
        memcpy(tp->T_in, in, bytes);
        tp->T_imported = true;
        return true;
    }
    if (!strcmp(field, "interface_heat_in")) {
        if (!tp->node_power) return false;
        memcpy(tp->heat_in, in, bytes);
        tp->heat_imported = true;
        return true;
    }
    return false;
}

bool thermal_participant_bind(ThermalParticipant *tp, OrchParticipant *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    tp->blocks = tp->estimate ? THERMAL_TRIAL_STAGES : 1;
    tp->T_in = tp->heat_in = NULL;
    tp->T_imported = tp->heat_imported = tp->pending = false;
    if (tp->ninterface > 0) {
        if (!tp->interface_nodes || !tp->fixed || !tp->fixed_T) {
            snprintf(err, errlen, "participant '%s': an interface needs its node list and writable prescribed-temperature arrays", tp->name);
            return false;
        }
        for (int i = 0; i < tp->ninterface; i++)
            if (tp->interface_nodes[i] < 0 || tp->interface_nodes[i] >= tp->m->nnodes) {
                snprintf(err, errlen, "participant '%s': interface node %d does not exist", tp->name, tp->interface_nodes[i]);
                return false;
            }
        tp->T_in = calloc((size_t)(tp->blocks * tp->ninterface), sizeof(double));
        tp->heat_in = calloc((size_t)(tp->blocks * tp->ninterface), sizeof(double));
        if (!tp->T_in || !tp->heat_in || !tint_capture_nodes(tp->ti, tp->interface_nodes, tp->ninterface)) {
            thermal_participant_release(tp);
            snprintf(err, errlen, "out of memory");
            return false;
        }
        tint_set_stage_hook(tp->ti, tp_stage, tp);
    }
    out->name = tp->name;
    out->self = tp;
    out->transient = true;
    out->order = tint_order(tp->ti);
    out->trial = tp_trial;
    out->commit = tp_commit;
    out->attempt_done = tp_attempt_done;
    out->field_size = tp_size;
    out->export_field = tp_export;
    out->import_field = tp_import;
    return true;
}

void thermal_participant_release(ThermalParticipant *tp) {
    if (tp->ti) tint_set_stage_hook(tp->ti, NULL, NULL);
    free(tp->T_in), free(tp->heat_in);
    tp->T_in = tp->heat_in = NULL;
    tp->T_imported = tp->heat_imported = false;
}
