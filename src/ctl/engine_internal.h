/* engine_internal.h - Engine internals shared by the dispatcher and the operation handlers */
#pragma once

#include <pthread.h>

#include "../render/swrender.h"
#include "engine.h"
#include "jobs.h"
#include "ops.h"
#include "project.h"

enum { IDEM_SLOTS = 256, JOURNAL_SLOTS = 256, VIEW_SLOTS = 32 };

typedef struct IdemEntry {
    bool used;
    char key[129];
    char op[64];
    char params_hash[65];
    JsonValue *value;
    OpImage images[OP_MAX_IMAGES];
    int nimages;
    uint64_t revision;
} IdemEntry;

typedef struct JournalEntry {
    uint64_t revision;
    char op[64];
    char summary[256];
    char transport[16];
    char time[32];
} JournalEntry;

/* a rendered image with a known camera, kept so pixels can be mapped back to geometry */
typedef struct ViewEntry {
    bool used;
    char id[24];
    char kind[16];            /* "geometry", "mesh" or "result" */
    char job_id[40];
    SwCamera cam;
    uint64_t signature;       /* geometry (and mesh) revisions when rendered */
} ViewEntry;

struct Engine {
    pthread_mutex_t mtx;      /* serialises operations and observer access to the project */
    pthread_mutex_t stat_mtx; /* guards the counters below, so observers never wait for a long operation */
    uint64_t stat_revision, stat_changes;
    EngineConfig cfg;
    Project *proj;
    uint64_t last_revision; /* highest revision handed out in this session: revisions are never reused */
    IdemEntry idem[IDEM_SLOTS];
    int idem_next;
    JournalEntry journal[JOURNAL_SLOTS];
    int journal_len, journal_next;
    ViewEntry views[VIEW_SLOTS];
    int view_next;
    unsigned view_counter;
    const OpCaller *caller;
    JobManager *jobs; /* own locking; never touched by the worker through the engine */
};

/* all of these expect the engine lock to be held */
void engine_set_project(Engine *e, Project *p); /* takes ownership, frees the previous project */
void engine_touch(Engine *e, const char *op, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
JsonValue *engine_journal_json(Engine *e, uint64_t since_revision, int max);
bool engine_resolve_read_path(Engine *e, const char *in, char *out, size_t cap, char *err, size_t errlen);
bool engine_resolve_write_path(Engine *e, const char *base, const char *in, bool is_dir, bool mkdirs, char *out, size_t cap,
                               char *err, size_t errlen);
JsonValue *engine_roots_json(Engine *e);

/* views.c */
uint64_t engine_view_signature(Engine *e, bool include_mesh);
const ViewEntry *engine_store_view(Engine *e, const SwCamera *cam, const char *kind, const char *job_id);
const ViewEntry *engine_find_view(Engine *e, const char *id);
/* nearest body surface hit along a ray (build frame); returns the body index */
bool engine_ray_hit(Engine *e, const double origin[3], const double dir[3], int *body, BvhHit *hit);
/* SelectionPickFn for selection queries: ctx is the Engine */
bool engine_selection_pick(void *ctx, const char *view_id, double px, double py, Body *target, int *tri, char *err, size_t errlen);
