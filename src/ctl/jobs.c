/* jobs.c - job queue with a single worker thread */
#include "jobs.h"
#include "../core/paths.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { JOBS_MAX = 64, JOBS_KEEP_DATA = 3 };

struct Job {
    JobManager *jm;
    char id[64], kind[32], label[128];
    JobState state;
    double progress;
    char stage[160];
    char error_code[48], error[1024];
    char created[32], started[32], finished[32];
    double t_started, t_finished;
    atomic_bool cancel, checkpoint, pause;
    uint64_t seq;
    char run_dir[NV_PATH_MAX];
    char spec_hash[65];
    char project_id[40], project_name[64];
    uint64_t project_revision;
    void *data;
    JobRunFn run;
    JobFreeFn free_data;
    int pins;
    JsonValue *summary;
    JsonValue *warnings;
    JsonValue *error_details;
    JsonValue *checkpoint_info;
};

struct JobManager {
    pthread_mutex_t mtx;
    pthread_cond_t changed, work;
    Job *jobs[JOBS_MAX];
    int n;
    uint64_t seq;
    unsigned counter;
    pthread_t worker;
    bool worker_started, stop;
};

const char *job_state_name(JobState s) {
    static const char *const N[] = {"queued", "running", "succeeded", "failed", "cancelled", "paused"};
    return s >= JOB_QUEUED && s <= JOB_PAUSED ? N[s] : "unknown";
}

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void utc_now(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static bool terminal(JobState s) { return s == JOB_SUCCEEDED || s == JOB_FAILED || s == JOB_CANCELLED || s == JOB_PAUSED; }

JobManager *jobs_create(void) {
    JobManager *jm = calloc(1, sizeof *jm);
    if (!jm) return NULL;
    pthread_mutex_init(&jm->mtx, NULL);
    pthread_cond_init(&jm->changed, NULL);
    pthread_cond_init(&jm->work, NULL);
    return jm;
}

static void free_job(Job *j) {
    if (j->data && j->free_data) j->free_data(j->data);
    json_free(j->error_details);
    json_free(j->checkpoint_info);
    json_free(j->summary);
    json_free(j->warnings);
    free(j);
}

static void release_data_locked(Job *j) {
    if (j->data && j->pins == 0) {
        if (j->free_data) j->free_data(j->data);
        j->data = NULL;
    }
}

static Job *find_locked(JobManager *jm, const char *id) {
    for (int i = 0; id && i < jm->n; i++)
        if (!strcmp(jm->jobs[i]->id, id)) return jm->jobs[i];
    return NULL;
}

/* result data stays in memory for the newest few succeeded jobs only */
static void apply_data_policy_locked(JobManager *jm) {
    Job *list[JOBS_MAX];
    int m = 0;
    for (int i = 0; i < jm->n; i++)
        if (jm->jobs[i]->state == JOB_SUCCEEDED && jm->jobs[i]->data) list[m++] = jm->jobs[i];
    for (int a = 0; a < m; a++)
        for (int b = a + 1; b < m; b++)
            if (list[b]->seq > list[a]->seq) {
                Job *t = list[a];
                list[a] = list[b];
                list[b] = t;
            }
    for (int a = JOBS_KEEP_DATA; a < m; a++) release_data_locked(list[a]);
}

static bool evict_one_locked(JobManager *jm) {
    int victim = -1;
    for (int i = 0; i < jm->n; i++) {
        Job *j = jm->jobs[i];
        if (terminal(j->state) && j->pins == 0 && (victim < 0 || j->seq < jm->jobs[victim]->seq)) victim = i;
    }
    if (victim < 0) return false;
    free_job(jm->jobs[victim]);
    memmove(jm->jobs + victim, jm->jobs + victim + 1, (size_t)(jm->n - victim - 1) * sizeof(Job *));
    jm->n--;
    return true;
}

static void *worker_main(void *arg) {
    JobManager *jm = arg;
    pthread_mutex_lock(&jm->mtx);
    for (;;) {
        Job *next = NULL;
        for (int i = 0; i < jm->n; i++) {
            Job *j = jm->jobs[i];
            if (j->state == JOB_QUEUED && (!next || j->seq < next->seq)) next = j;
        }
        if (!next) {
            if (jm->stop) break;
            pthread_cond_wait(&jm->work, &jm->mtx);
            continue;
        }
        next->state = JOB_RUNNING;
        next->t_started = mono_now();
        utc_now(next->started, sizeof next->started);
        snprintf(next->stage, sizeof next->stage, "starting");
        next->pins++;
        pthread_cond_broadcast(&jm->changed);
        pthread_mutex_unlock(&jm->mtx);

        char code[48] = "", err[1024] = "";
        bool ok = !atomic_load(&next->cancel) && next->run(next, next->data, code, sizeof code, err, sizeof err);

        pthread_mutex_lock(&jm->mtx);
        next->pins--;
        next->t_finished = mono_now();
        utc_now(next->finished, sizeof next->finished);
        if (ok) {
            next->state = JOB_SUCCEEDED;
            next->progress = 1;
            snprintf(next->stage, sizeof next->stage, "done");
        } else if (!strcmp(code, "PAUSED")) {
            next->state = JOB_PAUSED;
            snprintf(next->error_code, sizeof next->error_code, "PAUSED");
            snprintf(next->error, sizeof next->error, "%s", err[0] ? err : "paused on request");
            snprintf(next->stage, sizeof next->stage, "paused");
        } else if (atomic_load(&next->cancel)) {
            next->state = JOB_CANCELLED;
            snprintf(next->error_code, sizeof next->error_code, "CANCELLED");
            snprintf(next->error, sizeof next->error, "cancelled on request%s%s", err[0] ? ": " : "", err);
            snprintf(next->stage, sizeof next->stage, "cancelled");
        } else {
            next->state = JOB_FAILED;
            snprintf(next->error_code, sizeof next->error_code, "%s", code[0] ? code : "SOLVER_FAILED");
            snprintf(next->error, sizeof next->error, "%s", err[0] ? err : "the job failed without a message");
            snprintf(next->stage, sizeof next->stage, "failed");
        }
        if (next->state != JOB_SUCCEEDED) release_data_locked(next);
        apply_data_policy_locked(jm);
        pthread_cond_broadcast(&jm->changed);
    }
    pthread_mutex_unlock(&jm->mtx);
    return NULL;
}

void jobs_destroy(JobManager *jm) {
    if (!jm) return;
    pthread_mutex_lock(&jm->mtx);
    jm->stop = true;
    for (int i = 0; i < jm->n; i++) {
        Job *j = jm->jobs[i];
        atomic_store(&j->cancel, true);
        if (j->state == JOB_QUEUED) j->state = JOB_CANCELLED;
    }
    pthread_cond_broadcast(&jm->work);
    pthread_cond_broadcast(&jm->changed);
    pthread_mutex_unlock(&jm->mtx);
    if (jm->worker_started) pthread_join(jm->worker, NULL);
    for (int i = 0; i < jm->n; i++) free_job(jm->jobs[i]);
    pthread_mutex_destroy(&jm->mtx);
    pthread_cond_destroy(&jm->changed);
    pthread_cond_destroy(&jm->work);
    free(jm);
}

void jobs_new_id(JobManager *jm, char *out, size_t cap) {
    /* one counter for the whole process: the private engines of a comparison study share the process with the engine
     * that started it, and a job id must identify one run */
    static atomic_uint process_counter;
    unsigned c = (unsigned)atomic_fetch_add(&process_counter, 1) + 1;
    pthread_mutex_lock(&jm->mtx);
    jm->counter = c;
    pthread_mutex_unlock(&jm->mtx);
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    snprintf(out, cap, "job-%s-%04x-%u", ts, (unsigned)getpid() & 0xffff, c);
}

bool jobs_submit(JobManager *jm, const JobSpec *s, char *err, size_t errlen) {
    Job *j = calloc(1, sizeof *j);
    if (!j) {
        if (s->free_data && s->data) s->free_data(s->data);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    j->jm = jm;
    snprintf(j->id, sizeof j->id, "%s", s->id ? s->id : "");
    snprintf(j->kind, sizeof j->kind, "%s", s->kind ? s->kind : "");
    snprintf(j->label, sizeof j->label, "%s", s->label ? s->label : "");
    snprintf(j->run_dir, sizeof j->run_dir, "%s", s->run_dir ? s->run_dir : "");
    snprintf(j->spec_hash, sizeof j->spec_hash, "%s", s->spec_hash ? s->spec_hash : "");
    snprintf(j->project_id, sizeof j->project_id, "%s", s->project_id ? s->project_id : "");
    snprintf(j->project_name, sizeof j->project_name, "%s", s->project_name ? s->project_name : "");
    j->project_revision = s->project_revision;
    j->data = s->data;
    j->run = s->run;
    j->free_data = s->free_data;
    j->state = JOB_QUEUED;
    atomic_init(&j->cancel, false);
    atomic_init(&j->checkpoint, false);
    atomic_init(&j->pause, false);
    utc_now(j->created, sizeof j->created);
    snprintf(j->stage, sizeof j->stage, "queued");
    j->warnings = json_array();
    pthread_mutex_lock(&jm->mtx);
    const char *why = NULL;
    if (!j->id[0] || find_locked(jm, j->id)) why = "duplicate or empty job id";
    else if (jm->stop) why = "the job manager is shutting down";
    else if (jm->n == JOBS_MAX && !evict_one_locked(jm)) why = "too many queued or running jobs; wait for some to finish";
    if (!why && !jm->worker_started) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        if (pthread_create(&jm->worker, &attr, worker_main, jm) != 0) why = "cannot start the job worker thread";
        else jm->worker_started = true;
        pthread_attr_destroy(&attr);
    }
    if (why) {
        pthread_mutex_unlock(&jm->mtx);
        snprintf(err, errlen, "%s", why);
        free_job(j);
        return false;
    }
    j->seq = ++jm->seq;
    jm->jobs[jm->n++] = j;
    pthread_cond_signal(&jm->work);
    pthread_mutex_unlock(&jm->mtx);
    return true;
}

bool jobs_resubmit(JobManager *jm, const JobSpec *s, char *err, size_t errlen) {
    pthread_mutex_lock(&jm->mtx);
    Job *old = find_locked(jm, s->id);
    if (old) {
        if (!(old->state == JOB_PAUSED || old->state == JOB_CANCELLED || old->state == JOB_FAILED) || old->pins > 0) {
            pthread_mutex_unlock(&jm->mtx);
            if (s->free_data && s->data) s->free_data(s->data);
            snprintf(err, errlen, "job '%s' is %s and cannot be resumed", s->id, job_state_name(old->state));
            return false;
        }
        for (int i = 0; i < jm->n; i++)
            if (jm->jobs[i] == old) {
                free_job(old);
                memmove(jm->jobs + i, jm->jobs + i + 1, (size_t)(jm->n - i - 1) * sizeof(Job *));
                jm->n--;
                break;
            }
    }
    pthread_mutex_unlock(&jm->mtx);
    return jobs_submit(jm, s, err, errlen);
}

bool jobs_request_checkpoint(JobManager *jm, const char *id, bool pause, JobState *state) {
    pthread_mutex_lock(&jm->mtx);
    Job *j = find_locked(jm, id);
    if (j) {
        if (j->state == JOB_RUNNING || j->state == JOB_QUEUED) {
            atomic_store(&j->checkpoint, true);
            if (pause) atomic_store(&j->pause, true);
            snprintf(j->stage, sizeof j->stage, "%s", pause ? "pausing at the next accepted step" : "checkpoint requested");
        }
        if (state) *state = j->state;
    }
    pthread_mutex_unlock(&jm->mtx);
    return j != NULL;
}

bool jobs_find_active(JobManager *jm, const char *hash, char *id_out, size_t cap) {
    bool found = false;
    pthread_mutex_lock(&jm->mtx);
    for (int i = 0; i < jm->n && !found; i++) {
        Job *j = jm->jobs[i];
        if (!terminal(j->state) && !atomic_load(&j->cancel) && hash && !strcmp(j->spec_hash, hash)) {
            snprintf(id_out, cap, "%s", j->id);
            found = true;
        }
    }
    pthread_mutex_unlock(&jm->mtx);
    return found;
}

static JsonValue *job_json_locked(Job *j, bool full) {
    JsonValue *o = json_object();
    json_set_string(o, "job_id", j->id);
    json_set_string(o, "kind", j->kind);
    if (j->label[0]) json_set_string(o, "label", j->label);
    json_set_string(o, "state", job_state_name(j->state));
    json_set_number(o, "progress", round(j->progress * 1000) / 1000);
    json_set_string(o, "stage", j->stage);
    json_set_string(o, "created", j->created);
    if (j->started[0]) json_set_string(o, "started", j->started);
    if (j->finished[0]) json_set_string(o, "finished", j->finished);
    if (j->t_started > 0) json_set_number(o, "elapsed_s", round(((j->t_finished > 0 ? j->t_finished : mono_now()) - j->t_started) * 1000) / 1000);
    if (!full) return o;
    if (j->run_dir[0]) json_set_string(o, "run_directory", j->run_dir);
    if (j->spec_hash[0]) json_set_string(o, "spec_hash", j->spec_hash);
    if (j->project_id[0]) {
        JsonValue *pr = json_set_object(o, "project");
        json_set_string(pr, "id", j->project_id);
        json_set_string(pr, "name", j->project_name);
        json_set_int(pr, "revision", (long long)j->project_revision);
    }
    json_set_bool(o, "cancel_requested", atomic_load(&j->cancel));
    if (j->checkpoint_info) json_set(o, "checkpoint", json_clone(j->checkpoint_info));
    if (j->state == JOB_FAILED || j->state == JOB_CANCELLED || j->state == JOB_PAUSED) {
        JsonValue *er = json_set_object(o, "error");
        json_set_string(er, "code", j->error_code);
        json_set_string(er, "message", j->error);
        if (j->error_details) json_set(er, "details", json_clone(j->error_details));
    }
    if (json_len(j->warnings)) json_set(o, "warnings", json_clone(j->warnings));
    if (j->state == JOB_SUCCEEDED && j->summary) json_set(o, "summary", json_clone(j->summary));
    json_set_bool(o, "results_in_memory", j->data != NULL);
    return o;
}

JsonValue *jobs_status_json(JobManager *jm, const char *id, double wait_s) {
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (wait_s > 0) {
        double whole = floor(wait_s);
        dl.tv_sec += (time_t)whole;
        dl.tv_nsec += (long)((wait_s - whole) * 1e9);
        if (dl.tv_nsec >= 1000000000L) dl.tv_sec++, dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&jm->mtx);
    Job *j = find_locked(jm, id);
    while (wait_s > 0 && j && !terminal(j->state) && !jm->stop) {
        int rc = pthread_cond_timedwait(&jm->changed, &jm->mtx, &dl);
        j = find_locked(jm, id);
        if (rc == ETIMEDOUT) break;
    }
    JsonValue *o = j ? job_json_locked(j, true) : NULL;
    pthread_mutex_unlock(&jm->mtx);
    return o;
}

JsonValue *jobs_list_json(JobManager *jm, int max) {
    JsonValue *arr = json_array();
    pthread_mutex_lock(&jm->mtx);
    for (int i = jm->n - 1, k = 0; i >= 0 && (max <= 0 || k < max); i--, k++) json_push(arr, job_json_locked(jm->jobs[i], false));
    pthread_mutex_unlock(&jm->mtx);
    return arr;
}

bool jobs_cancel(JobManager *jm, const char *id, JobState *state) {
    pthread_mutex_lock(&jm->mtx);
    Job *j = find_locked(jm, id);
    if (!j) {
        pthread_mutex_unlock(&jm->mtx);
        return false;
    }
    if (j->state == JOB_QUEUED) {
        atomic_store(&j->cancel, true);
        j->state = JOB_CANCELLED;
        snprintf(j->error_code, sizeof j->error_code, "CANCELLED");
        snprintf(j->error, sizeof j->error, "cancelled before it started");
        snprintf(j->stage, sizeof j->stage, "cancelled");
        utc_now(j->finished, sizeof j->finished);
        release_data_locked(j);
        pthread_cond_broadcast(&jm->changed);
    } else if (j->state == JOB_RUNNING) {
        atomic_store(&j->cancel, true);
        snprintf(j->stage, sizeof j->stage, "cancelling");
    }
    if (state) *state = j->state;
    pthread_mutex_unlock(&jm->mtx);
    return true;
}

bool jobs_acquire(JobManager *jm, const char *id, JobState *state, void **data, char *kind, size_t kindcap, char *run_dir, size_t dircap) {
    *data = NULL;
    pthread_mutex_lock(&jm->mtx);
    Job *j = find_locked(jm, id);
    if (j) {
        *state = j->state;
        if (kind) snprintf(kind, kindcap, "%s", j->kind);
        if (run_dir) snprintf(run_dir, dircap, "%s", j->run_dir);
        if (j->state == JOB_SUCCEEDED && j->data) {
            j->pins++;
            *data = j->data;
        }
    }
    pthread_mutex_unlock(&jm->mtx);
    return j != NULL;
}

void jobs_release(JobManager *jm, const char *id) {
    pthread_mutex_lock(&jm->mtx);
    Job *j = find_locked(jm, id);
    if (j && j->pins > 0 && --j->pins == 0) apply_data_policy_locked(jm);
    pthread_mutex_unlock(&jm->mtx);
}

void *jobs_attach(JobManager *jm, const char *id, const char *kind, const char *run_dir, void *data, JobFreeFn free_data, JsonValue *summary) {
    pthread_mutex_lock(&jm->mtx);
    Job *j = find_locked(jm, id);
    if (!j) {
        if (jm->n == JOBS_MAX && !evict_one_locked(jm)) goto fail;
        j = calloc(1, sizeof *j);
        if (!j) goto fail;
        j->jm = jm;
        snprintf(j->id, sizeof j->id, "%s", id);
        snprintf(j->kind, sizeof j->kind, "%s", kind ? kind : "");
        snprintf(j->run_dir, sizeof j->run_dir, "%s", run_dir ? run_dir : "");
        j->state = JOB_SUCCEEDED;
        j->progress = 1;
        snprintf(j->stage, sizeof j->stage, "loaded from the run directory");
        atomic_init(&j->cancel, false);
        utc_now(j->created, sizeof j->created);
        j->warnings = json_array();
        j->summary = summary;
        summary = NULL;
        j->seq = ++jm->seq;
        jm->jobs[jm->n++] = j;
    } else if (j->state != JOB_SUCCEEDED) {
        goto fail;
    }
    void *use;
    if (j->data) {
        use = j->data;
        free_data(data);
    } else {
        j->data = data;
        j->free_data = free_data;
        use = data;
    }
    j->pins++;
    pthread_mutex_unlock(&jm->mtx);
    json_free(summary);
    return use;
fail:
    pthread_mutex_unlock(&jm->mtx);
    free_data(data);
    json_free(summary);
    return NULL;
}

bool job_cancel_requested(Job *job) { return atomic_load(&job->cancel); }
bool job_checkpoint_requested(Job *job) { return atomic_exchange(&job->checkpoint, false); }
bool job_pause_requested(Job *job) { return atomic_load(&job->pause); }

void job_set_checkpoint_info(Job *job, JsonValue *info) {
    pthread_mutex_lock(&job->jm->mtx);
    json_free(job->checkpoint_info);
    job->checkpoint_info = info;
    pthread_mutex_unlock(&job->jm->mtx);
}

void job_progress(Job *job, double fraction, const char *stage) {
    pthread_mutex_lock(&job->jm->mtx);
    if (fraction > job->progress) job->progress = fraction < 1 ? fraction : 1;
    if (stage && strcmp(job->stage, "cancelling") != 0) snprintf(job->stage, sizeof job->stage, "%s", stage);
    pthread_mutex_unlock(&job->jm->mtx);
}

void job_set_summary(Job *job, JsonValue *summary) {
    pthread_mutex_lock(&job->jm->mtx);
    json_free(job->summary);
    job->summary = summary;
    pthread_mutex_unlock(&job->jm->mtx);
}

void job_set_error_details(Job *job, JsonValue *details) {
    pthread_mutex_lock(&job->jm->mtx);
    json_free(job->error_details);
    job->error_details = details;
    pthread_mutex_unlock(&job->jm->mtx);
}

void job_add_warning(Job *job, const char *text) {
    pthread_mutex_lock(&job->jm->mtx);
    json_push(job->warnings, json_string(text));
    pthread_mutex_unlock(&job->jm->mtx);
}

const char *job_id(const Job *job) { return job->id; }
