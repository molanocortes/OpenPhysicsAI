/* common.h - shared utilities for NAVIER (real-time 3D LBM water tunnel) */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MINI(a, b) ((a) < (b) ? (a) : (b))
#define MAXI(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

/* Monotonic wall clock in seconds. */
double now_seconds(void);

/* Page-aligned, zero-initialised allocation for large numeric arrays. */
void *mem_alloc_aligned(size_t size);
void mem_free_aligned(void *p);

/* Logging. Messages go to stderr until a sink is installed (the in-app terminal). */
typedef enum { LOG_INFO = 0, LOG_OK, LOG_WARN, LOG_ERROR, LOG_DATA, LOG_ECHO } LogLevel;
typedef void (*LogSink)(LogLevel level, const char *msg);
void log_set_sink(LogSink sink);
void log_msg(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGOK(...) log_msg(LOG_OK, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define LOGD(...) log_msg(LOG_DATA, __VA_ARGS__)

/* Bit-exact finiteness test (safe under -ffast-math). */
static inline bool is_finite_f32(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    return ((b >> 23) & 0xFF) != 0xFF;
}

/* Small string helpers. */
void str_copy(char *dst, size_t cap, const char *src);
bool str_ieq(const char *a, const char *b);
bool str_starts_with_i(const char *s, const char *prefix);
