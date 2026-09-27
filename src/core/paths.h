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
