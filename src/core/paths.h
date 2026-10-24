/* paths.h - file system helpers: user expansion, resolution under allowed roots, atomic writes */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define NV_PATH_MAX 4096

/* "~/x" -> $HOME/x; relative paths are made absolute against the current directory (no normalisation). */
bool path_expand(const char *in, char *out, size_t cap);
/* realpath of an existing file or directory */
bool path_real(const char *in, char *out, size_t cap);
/* Resolves a path that may not exist yet: the deepest existing ancestor is canonicalised with realpath and the
 * remaining components are appended; "." and ".." are rejected in the part that does not exist. With mkdirs the
 * missing directories are created (0755), including the final component when final_is_dir. */
bool path_resolve_new(const char *in, char *out, size_t cap, bool mkdirs, bool final_is_dir, char *err, size_t errlen);
/* true when path equals root or lies below it (both canonical absolute paths) */
bool path_within(const char *path, const char *root);
bool path_mkdirs(const char *dir);
bool path_exists(const char *p);
bool path_is_dir(const char *p);
bool path_is_file(const char *p);
long long path_file_size(const char *p); /* -1 if it cannot be read */
const char *path_basename(const char *p);
bool path_join(char *out, size_t cap, const char *a, const char *b);
/* Replaces characters outside [A-Za-z0-9._-] with '_' (for names that become file names). */
void path_sanitize_name(char *s);

char *path_read_file(const char *p, size_t max_bytes, size_t *len, char *err, size_t errlen);
/* write to <p>.tmp<pid> then rename */
bool path_write_file_atomic(const char *p, const void *data, size_t n, char *err, size_t errlen);
bool path_copy_file(const char *src, const char *dst, char *err, size_t errlen);

/* OpenPhysicsAI's own per-user places and settings. Until 2026-10-21 the project was called NAVIER; for one release its
 * names are read as fallbacks: an environment variable NAVIER_X, the folders ~/.navier and ~/NAVIER-Projects. Below
 * ~/.openphysicsai the code touches only its own named subfolders (run, agent, app) and never lists the folder. */
/* $OPENPHYSICS_<name>, else $NAVIER_<name>, else NULL (a variable set to the empty string counts as set) */
const char *op_getenv(const char *name);
/* ~/.openphysicsai/<sub>; false when there is no home */
bool op_state_dir(char *out, size_t cap, const char *sub);
/* ~/.navier/<sub>, the place before the rename, when it exists; false otherwise (read it, never write it) */
bool op_legacy_state_path(char *out, size_t cap, const char *sub);
/* the projects folder: ~/OpenPhysicsAI-Projects, or ~/NAVIER-Projects when only that one exists (*legacy says which,
 * may be NULL); false when there is no home */
bool op_projects_dir(char *out, size_t cap, bool *legacy);
