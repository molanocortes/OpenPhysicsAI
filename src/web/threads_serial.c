/* threads_serial.c - src/threads.h without threads, for a build that has none (the lab in a web page, src/web/README.md).
 * pool_for runs every chunk on the caller, in order, with thread id 0, so a solver computes what it computes on a
 * one-thread pool of the native build. Linked instead of src/threads.c; nothing else changes. */
#include <stdlib.h>

#include "../threads.h"

struct ThreadPool {
    int unused;
};

ThreadPool *pool_create(int nthreads) {
    (void)nthreads;
    return calloc(1, sizeof(ThreadPool));
}

void pool_destroy(ThreadPool *p) { free(p); }

int pool_size(const ThreadPool *p) {
    (void)p;
    return 1;
}

void pool_for(ThreadPool *p, int count, int grain, ParallelFn fn, void *ctx) {
    (void)p;
    if (count <= 0) return;
    if (grain < 1) grain = 1;
    for (int begin = 0; begin < count; begin += grain) {
        int end = count - begin > grain ? begin + grain : count;
        fn(ctx, begin, end, 0);
    }
}

int cpu_count(void) { return 1; }
int cpu_perf_count(void) { return 1; }
