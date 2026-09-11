/*
 * zsp_thread — portable threading primitives (see zsp_thread.h).
 *
 * Two backends selected at compile time: Win32 when _WIN32 is defined, POSIX
 * pthreads otherwise. Each entry point is implemented once per backend; the
 * public contract (0 = success, predicate-loop condvars, non-recursive mutex)
 * is identical across both.
 */

#include "zsp_thread.h"

#if defined(_WIN32)

/* ----------------------------- Win32 backend ---------------------------- */

/* CreateThread wants an `unsigned __stdcall` entry, but we expose the POSIX
 * `void *fn(void *)` shape. Bridge through the per-thread control block: the
 * trampoline calls the user function and stashes its return value for join. */
static DWORD WINAPI zsp__thread_trampoline(LPVOID p) {
    zsp_thread_t *t = (zsp_thread_t *)p;
    t->ret = t->fn(t->arg);
    return 0;
}

int zsp_thread_create(zsp_thread_t *t, zsp_thread_fn fn, void *arg) {
    t->fn = fn;
    t->arg = arg;
    t->ret = NULL;
    t->handle = CreateThread(NULL, 0, zsp__thread_trampoline, t, 0, NULL);
    return t->handle ? 0 : -1;
}

int zsp_thread_join(zsp_thread_t *t, void **retval) {
    if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0) return -1;
    CloseHandle(t->handle);
    t->handle = NULL;
    if (retval) *retval = t->ret;
    return 0;
}

int  zsp_mutex_init(zsp_mutex_t *m)   { InitializeSRWLock(&m->lock); return 0; }
void zsp_mutex_destroy(zsp_mutex_t *m){ (void)m; /* SRWLOCK needs no teardown */ }
void zsp_mutex_lock(zsp_mutex_t *m)   { AcquireSRWLockExclusive(&m->lock); }
void zsp_mutex_unlock(zsp_mutex_t *m) { ReleaseSRWLockExclusive(&m->lock); }

int  zsp_cond_init(zsp_cond_t *c)     { InitializeConditionVariable(&c->cv); return 0; }
void zsp_cond_destroy(zsp_cond_t *c)  { (void)c; /* CONDITION_VARIABLE needs no teardown */ }
void zsp_cond_wait(zsp_cond_t *c, zsp_mutex_t *m) {
    SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0);
}
void zsp_cond_signal(zsp_cond_t *c)    { WakeConditionVariable(&c->cv); }
void zsp_cond_broadcast(zsp_cond_t *c) { WakeAllConditionVariable(&c->cv); }

unsigned zsp_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
}

#else

/* ----------------------------- POSIX backend ---------------------------- */

#include <unistd.h>

int zsp_thread_create(zsp_thread_t *t, zsp_thread_fn fn, void *arg) {
    return pthread_create(&t->handle, NULL, fn, arg);
}

int zsp_thread_join(zsp_thread_t *t, void **retval) {
    return pthread_join(t->handle, retval);
}

int  zsp_mutex_init(zsp_mutex_t *m)    { return pthread_mutex_init(&m->m, NULL); }
void zsp_mutex_destroy(zsp_mutex_t *m) { pthread_mutex_destroy(&m->m); }
void zsp_mutex_lock(zsp_mutex_t *m)    { pthread_mutex_lock(&m->m); }
void zsp_mutex_unlock(zsp_mutex_t *m)  { pthread_mutex_unlock(&m->m); }

int  zsp_cond_init(zsp_cond_t *c)      { return pthread_cond_init(&c->c, NULL); }
void zsp_cond_destroy(zsp_cond_t *c)   { pthread_cond_destroy(&c->c); }
void zsp_cond_wait(zsp_cond_t *c, zsp_mutex_t *m) { pthread_cond_wait(&c->c, &m->m); }
void zsp_cond_signal(zsp_cond_t *c)    { pthread_cond_signal(&c->c); }
void zsp_cond_broadcast(zsp_cond_t *c) { pthread_cond_broadcast(&c->c); }

unsigned zsp_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1u;
}

#endif
