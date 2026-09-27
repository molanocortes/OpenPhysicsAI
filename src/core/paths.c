/* paths.c - file system helpers */
#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <limits.h> /* PATH_MAX; on Linux it lives here, not in <sys/param.h> */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool path_expand(const char *in, char *out, size_t cap) {
    if (!in || !*in || !cap) return false;
    int n;
    if (in[0] == '~' && (in[1] == '/' || in[1] == 0)) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        n = snprintf(out, cap, "%s%s", home, in + 1);
    } else if (in[0] == '/') {
        n = snprintf(out, cap, "%s", in);
    } else {
        char cwd[NV_PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return false;
        n = snprintf(out, cap, "%s/%s", strcmp(cwd, "/") ? cwd : "", in);
    }
    return n > 0 && (size_t)n < cap;
}

bool path_real(const char *in, char *out, size_t cap) {
    char exp[NV_PATH_MAX], res[PATH_MAX];
    if (!path_expand(in, exp, sizeof exp) || !realpath(exp, res)) return false;
    int n = snprintf(out, cap, "%s", res);
    return n > 0 && (size_t)n < cap;
}

bool path_resolve_new(const char *in, char *out, size_t cap, bool mkdirs, bool final_is_dir, char *err, size_t errlen) {
    char abs[NV_PATH_MAX];
    if (!path_expand(in, abs, sizeof abs)) {
        if (err) snprintf(err, errlen, "invalid path '%s'", in ? in : "");
        return false;
    }
    /* strip trailing slashes */
    size_t L = strlen(abs);
    while (L > 1 && abs[L - 1] == '/') abs[--L] = 0;

    char prefix[NV_PATH_MAX];
    memcpy(prefix, abs, L + 1);
    const char *rest[256];
    char restbuf[NV_PATH_MAX];
    int nrest = 0;
    char real[PATH_MAX];
    size_t used = 0;
    for (;;) {
        if (realpath(prefix, real)) break;
        if (errno != ENOENT) {
            if (err) snprintf(err, errlen, "cannot resolve '%s': %s", prefix, strerror(errno));
            return false;
        }
        char *slash = strrchr(prefix, '/');
        if (!slash || nrest >= 255) {
            if (err) snprintf(err, errlen, "cannot resolve '%s'", abs);
            return false;
        }
        const char *comp = slash + 1;
        size_t cl = strlen(comp);
        if (!cl || !strcmp(comp, ".") || !strcmp(comp, "..") || used + cl + 1 > sizeof restbuf) {
            if (err) snprintf(err, errlen, "path '%s' contains '.' or '..' below a directory that does not exist", abs);
            return false;
        }
        memcpy(restbuf + used, comp, cl + 1);
        rest[nrest++] = restbuf + used;
        used += cl + 1;
        if (slash == prefix) {
            strcpy(prefix, "/");
        } else {
            *slash = 0;
        }
    }
    struct stat st;
    if (nrest && (stat(real, &st) != 0 || !S_ISDIR(st.st_mode))) {
        if (err) snprintf(err, errlen, "'%s' is not a directory", real);
        return false;
    }
    char result[NV_PATH_MAX];
    int n = snprintf(result, sizeof result, "%s", real);
    for (int i = nrest - 1; i >= 0; i--) {
        size_t cur = strlen(result);
        n = snprintf(result + cur, sizeof result - cur, "%s%s", strcmp(result, "/") ? "/" : "", rest[i]);
        if (n < 0 || (size_t)n >= sizeof result - cur) {
            if (err) snprintf(err, errlen, "path too long");
            return false;
        }
        bool is_last = i == 0;
        if (mkdirs && (!is_last || final_is_dir)) {
            if (mkdir(result, 0755) != 0 && errno != EEXIST) {
                if (err) snprintf(err, errlen, "cannot create directory '%s': %s", result, strerror(errno));
                return false;
            }
        }
    }
    n = snprintf(out, cap, "%s", result);
    if (n < 0 || (size_t)n >= cap) {
        if (err) snprintf(err, errlen, "path too long");
        return false;
    }
    return true;
}

bool path_within(const char *path, const char *root) {
    if (!path || !root || path[0] != '/' || root[0] != '/') return false;
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (n == 1) return true; /* "/" */
    return strncmp(path, root, n) == 0 && (path[n] == 0 || path[n] == '/');
}

bool path_mkdirs(const char *dir) {
    char tmp[NV_PATH_MAX];
    return path_resolve_new(dir, tmp, sizeof tmp, true, true, NULL, 0);
}

bool path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

bool path_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

long long path_file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

const char *path_basename(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/') b = s + 1;
    return b;
}

bool path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t la = strlen(a);
    int n = snprintf(out, cap, "%s%s%s", a, la && a[la - 1] == '/' ? "" : "/", b);
    return n > 0 && (size_t)n < cap;
}

void path_sanitize_name(char *s) {
    for (; *s; s++) {
        char c = *s;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) *s = '_';
    }
}

char *path_read_file(const char *p, size_t max_bytes, size_t *len, char *err, size_t errlen) {
    FILE *f = fopen(p, "rb");
    if (!f) {
        if (err) snprintf(err, errlen, "cannot open '%s': %s", p, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        if (err) snprintf(err, errlen, "'%s' is not a regular file", p);
        fclose(f);
        return NULL;
    }
    if ((unsigned long long)st.st_size > max_bytes) {
        if (err) snprintf(err, errlen, "'%s' is %lld bytes, above the %zu byte limit", p, (long long)st.st_size, max_bytes);
        fclose(f);
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    char *buf = malloc(n + 1);
    if (!buf) {
        if (err) snprintf(err, errlen, "out of memory reading '%s'", p);
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    if (got != n) {
        if (err) snprintf(err, errlen, "read error on '%s'", p);
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

bool path_write_file_atomic(const char *p, const void *data, size_t n, char *err, size_t errlen) {
    char tmp[NV_PATH_MAX];
    int w = snprintf(tmp, sizeof tmp, "%s.tmp%ld", p, (long)getpid());
    if (w < 0 || (size_t)w >= sizeof tmp) {
        if (err) snprintf(err, errlen, "path too long");
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        if (err) snprintf(err, errlen, "cannot create '%s': %s", tmp, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, p) != 0) ok = false;
    if (!ok) {
        if (err) snprintf(err, errlen, "write error on '%s': %s", p, strerror(errno));
        remove(tmp);
    }
    return ok;
}

bool path_copy_file(const char *src, const char *dst, char *err, size_t errlen) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        if (err) snprintf(err, errlen, "cannot open '%s': %s", src, strerror(errno));
        return false;
    }
    char tmp[NV_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", dst, (long)getpid());
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        if (err) snprintf(err, errlen, "cannot create '%s': %s", tmp, strerror(errno));
        fclose(in);
        return false;
    }
    char buf[1 << 16];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (ok && rename(tmp, dst) != 0) ok = false;
    if (!ok) {
        if (err) snprintf(err, errlen, "copy '%s' -> '%s' failed: %s", src, dst, strerror(errno));
        remove(tmp);
    }
    return ok;
}
