/* engine.h - headless application core shared by every control path
 *
 * The Engine owns the active project, the idempotency cache, the change journal and (later) the job manager.
 * The terminal, the UI, the control socket and the MCP server all call ops_invoke() on the same Engine, so they
 * run the same validated operations. No OpenGL or UI state lives here; this module builds on Linux as well. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/json.h"
#include "../core/paths.h"

enum { ENGINE_MAX_ROOTS = 16 }; /* per kind; a folder inside an existing root takes no slot */

typedef struct EngineConfig {
    char workspace[NV_PATH_MAX];                      /* default parent of new projects */
    int nread_roots, nwrite_roots;
    char read_roots[ENGINE_MAX_ROOTS][NV_PATH_MAX];   /* input files must lie below one of these */
    char write_roots[ENGINE_MAX_ROOTS][NV_PATH_MAX];  /* projects and exports must lie below one of these */
    uint64_t max_stl_bytes;
    uint32_t max_triangles;
    uint64_t max_elements;
    int threads;
} EngineConfig;

/* workspace ~/NAVIER-Projects; read roots: $HOME, /tmp, /private/tmp, /Volumes and the working directory;
 * write roots: the workspace and $TMPDIR */
void engine_config_default(EngineConfig *c);
/* appends a root unless an existing root already covers it; a write root also becomes a read root. false with a message
 * naming the limit and the roots in use when the path is invalid or the limit is reached (nothing is added then) */
bool engine_config_add_root(EngineConfig *c, bool write, const char *path, char *err, size_t errlen);
bool engine_config_set_workspace(EngineConfig *c, const char *path, char *err, size_t errlen);

typedef struct Engine Engine;
typedef struct Project Project;

Engine *engine_create(const EngineConfig *cfg, char *err, size_t errlen);
void engine_destroy(Engine *e);
const EngineConfig *engine_get_config(const Engine *e);

/* Thread-safe counters for observers (e.g. the GUI): the project revision (0 without a project) and a counter that
 * changes on every modification, including switching projects. */
uint64_t engine_revision(Engine *e);
uint64_t engine_change_counter(Engine *e);

/* Direct access for observers that must read project data consistently (hold the lock briefly). */
void engine_lock(Engine *e);
void engine_unlock(Engine *e);
Project *engine_project_locked(Engine *e);

#define NAVIER_AM_VERSION "0.3.0-rc2"
