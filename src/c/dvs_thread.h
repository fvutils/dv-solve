#ifndef DVS_THREAD_H
#define DVS_THREAD_H

/*
 * dvs_thread — a thin, portable threading abstraction.
 *
 * The cube-and-conquer parallel engine (dvs_cube.c) needs threads, mutexes and
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
 *     recovered via dvs_thread_join's `retval` out-param (may be NULL).
 *   - Mutexes are non-recursive. Condition variables follow the usual
 *     predicate-loop protocol: wait atomically releases the mutex and re-locks
 *     on wake; spurious wakeups are possible, so always wait in a while-loop.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*dvs_thread_fn)(void *);

#if defined(_WIN32)

typedef struct { HANDLE handle; dvs_thread_fn fn; void *arg; void *ret; } dvs_thread_t;
typedef struct { SRWLOCK lock; } dvs_mutex_t;
typedef struct { CONDITION_VARIABLE cv; } dvs_cond_t;

#else

typedef struct { pthread_t handle; } dvs_thread_t;
typedef struct { pthread_mutex_t m; } dvs_mutex_t;
typedef struct { pthread_cond_t c; } dvs_cond_t;

#endif

/* Spawn a thread running `fn(arg)`. Returns 0 on success. */
int  dvs_thread_create(dvs_thread_t *t, dvs_thread_fn fn, void *arg);
/* Join a thread; if `retval` is non-NULL it receives the thread's return value.
 * Returns 0 on success. */
int  dvs_thread_join(dvs_thread_t *t, void **retval);

int  dvs_mutex_init(dvs_mutex_t *m);
void dvs_mutex_destroy(dvs_mutex_t *m);
void dvs_mutex_lock(dvs_mutex_t *m);
void dvs_mutex_unlock(dvs_mutex_t *m);

int  dvs_cond_init(dvs_cond_t *c);
void dvs_cond_destroy(dvs_cond_t *c);
/* Atomically release `m` and block until signalled; re-locks `m` before return.
 * Must be called with `m` held. Spurious wakeups possible — loop on a predicate. */
void dvs_cond_wait(dvs_cond_t *c, dvs_mutex_t *m);
void dvs_cond_signal(dvs_cond_t *c);
void dvs_cond_broadcast(dvs_cond_t *c);

/* Number of logical CPUs (>= 1). Best-effort; returns 1 if undeterminable. */
unsigned dvs_cpu_count(void);

/* The lowest address the calling thread's stack may grow down to while still
 * leaving `margin` bytes free, or 0 if the platform cannot tell. Recursive
 * passes compare the address of a local against it (dvs_stack_exhausted) and
 * refuse the input rather than overflow: a fixed depth limit cannot be right
 * for every stack -- 1 MB on a Windows thread, 512 KB on a macOS secondary
 * thread, whatever the caller gave its own threads. */
uintptr_t dvs_stack_floor(size_t margin);

/* Nonzero when the caller's stack has reached `floor` (0 never has). */
static inline int dvs_stack_exhausted(uintptr_t floor) {
    char here;
    return floor && (uintptr_t)&here < floor;
}

#ifdef __cplusplus
}
#endif

#endif /* DVS_THREAD_H */
