#ifndef ZSP_THREAD_H
#define ZSP_THREAD_H

/*
 * zsp_thread — a thin, portable threading abstraction.
 *
 * The cube-and-conquer parallel engine (zsp_cube.c) needs threads, mutexes and
 * condition variables. This layer wraps them so the rest of dv-solve is free of
 * platform #ifdefs and so a native-Windows backend can be dropped in without
 * touching callers.
 *
 * Backends:
 *   - POSIX  (pthreads)                — the default, built everywhere else.
 *   - Win32  (CreateThread / SRWLOCK / CONDITION_VARIABLE) — used when _WIN32.
 *
 * Contract common to both:
 *   - Every *_init returns 0 on success, non-zero on failure.
 *   - A thread function has signature `void *fn(void *arg)`; its return value is
 *     recovered via zsp_thread_join's `retval` out-param (may be NULL).
 *   - Mutexes are non-recursive. Condition variables follow the usual
 *     predicate-loop protocol: wait atomically releases the mutex and re-locks
 *     on wake; spurious wakeups are possible, so always wait in a while-loop.
 */

#include <stddef.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*zsp_thread_fn)(void *);

#if defined(_WIN32)

typedef struct { HANDLE handle; zsp_thread_fn fn; void *arg; void *ret; } zsp_thread_t;
typedef struct { SRWLOCK lock; } zsp_mutex_t;
typedef struct { CONDITION_VARIABLE cv; } zsp_cond_t;

#else

typedef struct { pthread_t handle; } zsp_thread_t;
typedef struct { pthread_mutex_t m; } zsp_mutex_t;
typedef struct { pthread_cond_t c; } zsp_cond_t;

#endif

/* Spawn a thread running `fn(arg)`. Returns 0 on success. */
int  zsp_thread_create(zsp_thread_t *t, zsp_thread_fn fn, void *arg);
/* Join a thread; if `retval` is non-NULL it receives the thread's return value.
 * Returns 0 on success. */
int  zsp_thread_join(zsp_thread_t *t, void **retval);

int  zsp_mutex_init(zsp_mutex_t *m);
void zsp_mutex_destroy(zsp_mutex_t *m);
void zsp_mutex_lock(zsp_mutex_t *m);
void zsp_mutex_unlock(zsp_mutex_t *m);

int  zsp_cond_init(zsp_cond_t *c);
void zsp_cond_destroy(zsp_cond_t *c);
/* Atomically release `m` and block until signalled; re-locks `m` before return.
 * Must be called with `m` held. Spurious wakeups possible — loop on a predicate. */
void zsp_cond_wait(zsp_cond_t *c, zsp_mutex_t *m);
void zsp_cond_signal(zsp_cond_t *c);
void zsp_cond_broadcast(zsp_cond_t *c);

/* Number of logical CPUs (>= 1). Best-effort; returns 1 if undeterminable. */
unsigned zsp_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_THREAD_H */
