/*
 * dvs_thread — portable threading primitives (see dvs_thread.h).
 *
 * Two backends selected at compile time: Win32 when _WIN32 is defined, POSIX
 * pthreads otherwise. Each entry point is implemented once per backend; the
 * public contract (0 = success, predicate-loop condvars, non-recursive mutex)
 * is identical across both.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE   /* pthread_getattr_np */
#endif
#include "dvs_thread.h"

#if defined(_WIN32)

/* ----------------------------- Win32 backend ---------------------------- */

/* CreateThread wants an `unsigned __stdcall` entry, but we expose the POSIX
 * `void *fn(void *)` shape. Bridge through the per-thread control block: the
 * trampoline calls the user function and stashes its return value for join. */
static DWORD WINAPI dvs__thread_trampoline(LPVOID p) {
    dvs_thread_t *t = (dvs_thread_t *)p;
    t->ret = t->fn(t->arg);
    return 0;
}

int dvs_thread_create(dvs_thread_t *t, dvs_thread_fn fn, void *arg) {
    t->fn = fn;
    t->arg = arg;
    t->ret = NULL;
    t->handle = CreateThread(NULL, 0, dvs__thread_trampoline, t, 0, NULL);
    return t->handle ? 0 : -1;
}

int dvs_thread_join(dvs_thread_t *t, void **retval) {
    if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0) return -1;
    CloseHandle(t->handle);
    t->handle = NULL;
    if (retval) *retval = t->ret;
    return 0;
}

int  dvs_mutex_init(dvs_mutex_t *m)   { InitializeSRWLock(&m->lock); return 0; }
void dvs_mutex_destroy(dvs_mutex_t *m){ (void)m; /* SRWLOCK needs no teardown */ }
void dvs_mutex_lock(dvs_mutex_t *m)   { AcquireSRWLockExclusive(&m->lock); }
void dvs_mutex_unlock(dvs_mutex_t *m) { ReleaseSRWLockExclusive(&m->lock); }

int  dvs_cond_init(dvs_cond_t *c)     { InitializeConditionVariable(&c->cv); return 0; }
void dvs_cond_destroy(dvs_cond_t *c)  { (void)c; /* CONDITION_VARIABLE needs no teardown */ }
void dvs_cond_wait(dvs_cond_t *c, dvs_mutex_t *m) {
    SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
}
void dvs_cond_signal(dvs_cond_t *c)    { WakeConditionVariable(&c->cv); }
void dvs_cond_broadcast(dvs_cond_t *c) { WakeAllConditionVariable(&c->cv); }

unsigned dvs_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
}

uintptr_t dvs_stack_floor(size_t margin) {
    /* lo is the bottom of the whole reservation; the guard pages above it
     * are well inside any useful margin. */
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    if (!lo || hi - lo <= margin) return 0;
    return (uintptr_t)lo + margin;
}

#else

/* ----------------------------- POSIX backend ---------------------------- */

#include <unistd.h>

int dvs_thread_create(dvs_thread_t *t, dvs_thread_fn fn, void *arg) {
    return pthread_create(&t->handle, NULL, fn, arg);
}

int dvs_thread_join(dvs_thread_t *t, void **retval) {
    return pthread_join(t->handle, retval);
}

int  dvs_mutex_init(dvs_mutex_t *m)    { return pthread_mutex_init(&m->m, NULL); }
void dvs_mutex_destroy(dvs_mutex_t *m) { pthread_mutex_destroy(&m->m); }
void dvs_mutex_lock(dvs_mutex_t *m)    { pthread_mutex_lock(&m->m); }
void dvs_mutex_unlock(dvs_mutex_t *m)  { pthread_mutex_unlock(&m->m); }

int  dvs_cond_init(dvs_cond_t *c)      { return pthread_cond_init(&c->c, NULL); }
void dvs_cond_destroy(dvs_cond_t *c)   { pthread_cond_destroy(&c->c); }
void dvs_cond_wait(dvs_cond_t *c, dvs_mutex_t *m) { pthread_cond_wait(&c->c, &m->m); }
void dvs_cond_signal(dvs_cond_t *c)    { pthread_cond_signal(&c->c); }
void dvs_cond_broadcast(dvs_cond_t *c) { pthread_cond_broadcast(&c->c); }

unsigned dvs_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1u;
}

uintptr_t dvs_stack_floor(size_t margin) {
    uintptr_t lo = 0;
    size_t size = 0;
#if defined(__APPLE__)
    pthread_t self = pthread_self();
    size = pthread_get_stacksize_np(self);
    lo = (uintptr_t)pthread_get_stackaddr_np(self) - size;  /* addr is the top */
#elif defined(__linux__)
    /* glibc and musl; for the main thread the size follows RLIMIT_STACK. */
    pthread_attr_t a;
    void *addr = NULL;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
    if (pthread_attr_getstack(&a, &addr, &size) != 0) size = 0;
    pthread_attr_destroy(&a);
    lo = (uintptr_t)addr;
#endif
    if (!lo || size <= margin) return 0;
    return lo + margin;
}

#endif
