/* jobs.h - asynchronous jobs: one worker thread runs queued jobs in submission order (each solve already uses all
 * performance cores), with progress, cooperative cancellation and results that stay available after the client that
 * started them disconnects. Result data of the newest few succeeded jobs stays in memory; older results are reloaded
 * from their run directories on demand. No engine or project state is touched by the worker. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/json.h"

/* PAUSED: stopped on request after writing a checkpoint; resumable, not a failure and not a cancellation */
typedef enum { JOB_QUEUED = 0, JOB_RUNNING, JOB_SUCCEEDED, JOB_FAILED, JOB_CANCELLED, JOB_PAUSED } JobState;
const char *job_state_name(JobState s);

typedef struct Job Job;
typedef struct JobManager JobManager;

/* Runs on the worker thread. Returns false with an error code name (e.g. "SOLVER_FAILED") and a message. */
typedef bool (*JobRunFn)(Job *job, void *data, char *code, size_t codelen, char *err, size_t errlen);
typedef void (*JobFreeFn)(void *data);

typedef struct JobSpec {
    const char *id;   /* from jobs_new_id */
    const char *kind; /* "static_structural" */
    const char *label;
    const char *run_dir;
    const char *spec_hash;
    const char *project_id, *project_name;
    uint64_t project_revision;
    void *data;
    JobRunFn run;
    JobFreeFn free_data;
} JobSpec;

JobManager *jobs_create(void);
void jobs_destroy(JobManager *jm); /* cancels queued and running jobs and joins the worker */

void jobs_new_id(JobManager *jm, char *out, size_t cap);
/* queues a job; the manager owns spec->data from here on, also when submission fails */
bool jobs_submit(JobManager *jm, const JobSpec *spec, char *err, size_t errlen);
/* a queued or running job with this specification hash */
bool jobs_find_active(JobManager *jm, const char *spec_hash, char *id_out, size_t cap);
/* status (with the summary when succeeded) after waiting up to wait_s seconds for a final state; NULL if unknown */
JsonValue *jobs_status_json(JobManager *jm, const char *id, double wait_s);
JsonValue *jobs_list_json(JobManager *jm, int max);
/* requests cancellation (queued jobs are cancelled at once); false if unknown */
bool jobs_cancel(JobManager *jm, const char *id, JobState *state);
/* asks a running job to write a checkpoint at its next accepted step boundary and, with pause, to stop there in the
 * PAUSED state. A checkpoint request is not a cancellation: without pause the job continues. False if unknown. */
bool jobs_request_checkpoint(JobManager *jm, const char *id, bool pause, JobState *state);
/* queues a job under an id whose existing entry is paused, cancelled or failed (a resumed run keeps its id and run
 * directory). Fails for an active or succeeded entry. */
bool jobs_resubmit(JobManager *jm, const JobSpec *spec, char *err, size_t errlen);
/* looks a job up; a succeeded job's in-memory data is returned pinned (unpin with jobs_release) */
bool jobs_acquire(JobManager *jm, const char *id, JobState *state, void **data, char *kind, size_t kindcap, char *run_dir, size_t dircap);
void jobs_release(JobManager *jm, const char *id);
/* attaches results loaded from disk to a succeeded job (registering it when unknown, e.g. after a restart) and returns
 * the pinned data to use; data (and summary) are freed when the job already has data or on failure */
void *jobs_attach(JobManager *jm, const char *id, const char *kind, const char *run_dir, void *data, JobFreeFn free_data, JsonValue *summary);

/* for run functions */
bool job_cancel_requested(Job *job);
/* true once per request: the run function writes a checkpoint when it sees it */
bool job_checkpoint_requested(Job *job);
bool job_pause_requested(Job *job);
/* the last checkpoint written (time, path, step), reported by job_status; takes ownership */
void job_set_checkpoint_info(Job *job, JsonValue *info);
void job_progress(Job *job, double fraction, const char *stage);
void job_set_summary(Job *job, JsonValue *summary); /* takes ownership */
/* structured details of a failure (failure class, last accepted time, cause, checkpoint, recovery options), reported by
 * job_status under error.details; takes ownership. Also kept for cancelled jobs. */
void job_set_error_details(Job *job, JsonValue *details);
void job_add_warning(Job *job, const char *text);
const char *job_id(const Job *job);
