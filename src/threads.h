/* threads.h - minimal thread pool with dynamic chunk scheduling (P/E-core friendly) */
#pragma once

#include <stdbool.h>

/* Called for the half-open index range [begin, end). thread_id is in [0, pool_size(p)]
 * (the calling thread participates with id == number of worker threads). */
typedef void (*ParallelFn)(void *ctx, int begin, int end, int thread_id);

typedef struct ThreadPool ThreadPool;

/* nthreads = total threads that do work, including the caller (>= 1). */
ThreadPool *pool_create(int nthreads);
void pool_destroy(ThreadPool *p);

/* Number of distinct thread ids that fn may see (worker threads + caller). */
int pool_size(const ThreadPool *p);

/* Split [0, count) into chunks of `grain` items; threads pull chunks until exhausted.
 * Blocks until all chunks are processed. Safe to call from multiple threads (calls are
 * serialised per pool). Not re-entrant: never call pool_for on the same pool from inside fn. */
void pool_for(ThreadPool *p, int count, int grain, ParallelFn fn, void *ctx);

/* Logical CPU count. */
int cpu_count(void);
int cpu_perf_count(void); /* performance cores (macOS), or cpu_count() elsewhere */
