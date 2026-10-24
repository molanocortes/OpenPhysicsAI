/* threads.c - thread pool implementation */
#include "threads.h"
#include "common.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

typedef struct {
    struct ThreadPool *pool;
    int id;
} WorkerArg;

struct ThreadPool {
    int nworkers; /* background threads; caller is an extra participant */
    pthread_t *threads;
    WorkerArg *args;
    pthread_mutex_t mtx;
    pthread_cond_t cv_work;
    pthread_cond_t cv_done;
    pthread_mutex_t job_mtx; /* serialises pool_for callers */

    ParallelFn fn;
    void *ctx;
    int count, grain;
    atomic_int next;
    int active;
    unsigned long job_id;
    bool quit;
};

static void run_chunks(struct ThreadPool *p, ParallelFn fn, void *ctx, int count, int grain, int tid) {
    for (;;) {
        int b = atomic_fetch_add_explicit(&p->next, grain, memory_order_relaxed);
        if (b >= count) break;
        int e = b + grain;
        if (e > count) e = count;
        fn(ctx, b, e, tid);
    }
}

static void *worker_main(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    struct ThreadPool *p = wa->pool;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    unsigned long seen = 0;
    for (;;) {
        pthread_mutex_lock(&p->mtx);
        while (!p->quit && p->job_id == seen) pthread_cond_wait(&p->cv_work, &p->mtx);
        if (p->quit) {
            pthread_mutex_unlock(&p->mtx);
            break;
        }
        seen = p->job_id;
        ParallelFn fn = p->fn;
        void *ctx = p->ctx;
        int count = p->count, grain = p->grain;
        pthread_mutex_unlock(&p->mtx);

        run_chunks(p, fn, ctx, count, grain, wa->id);

        pthread_mutex_lock(&p->mtx);
        if (--p->active == 0) pthread_cond_signal(&p->cv_done);
        pthread_mutex_unlock(&p->mtx);
    }
    return NULL;
}

ThreadPool *pool_create(int nthreads) {
    if (nthreads < 1) nthreads = 1;
    struct ThreadPool *p = calloc(1, sizeof *p);
    p->nworkers = nthreads - 1;
    pthread_mutex_init(&p->mtx, NULL);
    pthread_mutex_init(&p->job_mtx, NULL);
    pthread_cond_init(&p->cv_work, NULL);
    pthread_cond_init(&p->cv_done, NULL);
    if (p->nworkers > 0) {
        p->threads = calloc((size_t)p->nworkers, sizeof(pthread_t));
        p->args = calloc((size_t)p->nworkers, sizeof(WorkerArg));
        for (int i = 0; i < p->nworkers; i++) {
            p->args[i].pool = p;
            p->args[i].id = i;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 8u << 20);
            pthread_create(&p->threads[i], &attr, worker_main, &p->args[i]);
            pthread_attr_destroy(&attr);
        }
    }
    return p;
}

void pool_destroy(ThreadPool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mtx);
    p->quit = true;
    pthread_cond_broadcast(&p->cv_work);
    pthread_mutex_unlock(&p->mtx);
    for (int i = 0; i < p->nworkers; i++) pthread_join(p->threads[i], NULL);
    pthread_mutex_destroy(&p->mtx);
    pthread_mutex_destroy(&p->job_mtx);
    pthread_cond_destroy(&p->cv_work);
    pthread_cond_destroy(&p->cv_done);
    free(p->threads);
    free(p->args);
    free(p);
}

int pool_size(const ThreadPool *p) { return p->nworkers + 1; }

void pool_for(ThreadPool *p, int count, int grain, ParallelFn fn, void *ctx) {
    if (count <= 0) return;
    if (grain < 1) grain = 1;
    if (p->nworkers == 0 || count <= grain) {
        fn(ctx, 0, count, p->nworkers);
        return;
    }
    pthread_mutex_lock(&p->job_mtx);
    pthread_mutex_lock(&p->mtx);
    p->fn = fn;
    p->ctx = ctx;
    p->count = count;
    p->grain = grain;
    atomic_store(&p->next, 0);
    p->active = p->nworkers;
    p->job_id++;
    pthread_cond_broadcast(&p->cv_work);
    pthread_mutex_unlock(&p->mtx);

    run_chunks(p, fn, ctx, count, grain, p->nworkers);

    pthread_mutex_lock(&p->mtx);
    while (p->active > 0) pthread_cond_wait(&p->cv_done, &p->mtx);
    pthread_mutex_unlock(&p->mtx);
    pthread_mutex_unlock(&p->job_mtx);
}

int cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

int cpu_perf_count(void) {
#ifdef __APPLE__
    int v = 0;
    size_t len = sizeof v;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &v, &len, NULL, 0) == 0 && v > 0) return v;
#endif
    return cpu_count();
}
