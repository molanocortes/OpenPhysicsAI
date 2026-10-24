/* common.c - time, memory, logging and string helpers */
#include "common.h"

#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void *mem_alloc_aligned(size_t size) {
    if (size == 0) size = 1;
    size_t align = size >= (1u << 20) ? 16384 : 64;
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) return NULL;
    memset(p, 0, size);
    return p;
}

void mem_free_aligned(void *p) { free(p); }

static LogSink g_sink = NULL;
static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

void log_set_sink(LogSink sink) {
    pthread_mutex_lock(&g_log_mtx);
    g_sink = sink;
    pthread_mutex_unlock(&g_log_mtx);
}

void log_msg(LogLevel level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_mtx);
    LogSink sink = g_sink;
    pthread_mutex_unlock(&g_log_mtx);
    if (sink) {
        sink(level, buf);
    } else {
        static const char *tag[] = {"info", " ok ", "warn", "err!", "data", " >> "};
        fprintf(stderr, "[%s] %s\n", tag[level < 6 ? level : 0], buf);
    }
}

void str_copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++, b++;
    }
    return *a == *b;
}

bool str_starts_with_i(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++, prefix++;
    }
    return true;
}
