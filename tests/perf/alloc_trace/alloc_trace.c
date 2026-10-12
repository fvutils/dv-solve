/*
 * LD_PRELOAD allocation tracer for the malloc lifecycle survey
 * (docs/malloc_lifecycle_analysis.md).
 *
 * Interposes malloc/calloc/realloc/free/posix_memalign/aligned_alloc/
 * memalign. Each allocation is keyed by its call stack (up to 8 frames);
 * per stack it counts allocations, bytes, reallocs and frees, and keeps a
 * log2 histogram of block lifetimes in nanoseconds and of request sizes.
 * At exit the table is written to $ALLOC_TRACE_OUT.<pid> with each frame
 * as "object+offset" for alloc_report.py to symbolize.
 *
 *   gcc -O2 -shared -fPIC -o alloc_trace.so alloc_trace.c -ldl
 *   ALLOC_TRACE_OUT=/tmp/at LD_PRELOAD=./alloc_trace.so dv-solve-smt2 f.smt2
 *
 * Profiling only: the backtrace on every call slows the program several
 * times; use perf for time shares.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define DEPTH 8
#define NBUCKET 48
#define SITE_BITS 16
#define LIVE_BITS 24

typedef struct {
    uint64_t key;
    void *frames[DEPTH];
    int nframes;
    uint64_t allocs, reallocs, frees, bytes, live, peak_live;
    uint64_t life[NBUCKET];   /* log2(ns) of block lifetime at free */
    uint64_t size[NBUCKET];   /* log2(bytes) of requests */
} Site;

typedef struct { uintptr_t ptr; uint32_t site; uint64_t t; } Live;

static Site *sites;
static Live *live;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int in_hook;
static int ready;

static void *(*r_malloc)(size_t);
static void *(*r_calloc)(size_t, size_t);
static void *(*r_realloc)(void *, size_t);
static void (*r_free)(void *);
static int (*r_posix_memalign)(void **, size_t, size_t);
static void *(*r_aligned_alloc)(size_t, size_t);
static void *(*r_memalign)(size_t, size_t);

/* dlsym may calloc before the real functions are known. */
static char boot[1 << 16];
static size_t boot_used;
static void *boot_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (boot_used + n > sizeof boot) return NULL;
    void *p = boot + boot_used;
    boot_used += n;
    return p;
}
static int is_boot(void *p) { return (char *)p >= boot && (char *)p < boot + sizeof boot; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
static int lg(uint64_t v) { int b = 0; while (v > 1 && b < NBUCKET - 1) { v >>= 1; b++; } return b; }
static uint64_t hptr(uintptr_t p) { p ^= p >> 33; p *= 0xff51afd7ed558ccdULL; p ^= p >> 33; return p; }

static void init(void) {
    static int busy;
    if (ready || busy) return;
    busy = 1;
    r_malloc = dlsym(RTLD_NEXT, "malloc");
    r_calloc = dlsym(RTLD_NEXT, "calloc");
    r_realloc = dlsym(RTLD_NEXT, "realloc");
    r_free = dlsym(RTLD_NEXT, "free");
    r_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    r_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    r_memalign = dlsym(RTLD_NEXT, "memalign");
    sites = mmap(NULL, sizeof(Site) << SITE_BITS, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    live = mmap(NULL, sizeof(Live) << LIVE_BITS, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    in_hook = 1;
    void *f[2];
    backtrace(f, 2);            /* loads libgcc_s outside the lock */
    in_hook = 0;
    ready = 1;
}

static uint32_t site_of(void *const *fr, int n) {
    uint64_t k = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) k = (k ^ (uintptr_t)fr[i]) * 1099511628211ULL;
    if (!k) k = 1;
    uint32_t m = (1u << SITE_BITS) - 1, i = (uint32_t)hptr(k) & m;
    for (;;) {
        if (sites[i].key == k) return i;
        if (!sites[i].key) {
            sites[i].key = k;
            memcpy(sites[i].frames, fr, sizeof(void *) * (size_t)n);
            sites[i].nframes = n;
            return i;
        }
        i = (i + 1) & m;
    }
}

static void live_put(void *p, uint32_t s) {
    uint32_t m = (1u << LIVE_BITS) - 1, i = (uint32_t)hptr((uintptr_t)p) & m;
    while (live[i].ptr) i = (i + 1) & m;
    live[i].ptr = (uintptr_t)p;
    live[i].site = s;
    live[i].t = now_ns();
}

/* Removes p (backward-shift deletion); returns 0 if p was not tracked. */
static int live_take(void *p, Live *out) {
    uint32_t m = (1u << LIVE_BITS) - 1, i = (uint32_t)hptr((uintptr_t)p) & m;
    while (live[i].ptr && live[i].ptr != (uintptr_t)p) i = (i + 1) & m;
    if (!live[i].ptr) return 0;
    *out = live[i];
    uint32_t j = i;
    for (;;) {
        j = (j + 1) & m;
        if (!live[j].ptr) break;
        uint32_t h = (uint32_t)hptr(live[j].ptr) & m;
        if ((j > i && (h <= i || h > j)) || (j < i && (h <= i && h > j))) {
            live[i] = live[j];
            i = j;
        }
    }
    live[i].ptr = 0;
    return 1;
}

static void note_free(void *p, uint64_t t) {
    Live l;
    if (!live_take(p, &l)) return;
    Site *s = &sites[l.site];
    s->frees++;
    s->live--;
    s->life[lg(t - l.t)]++;
}

static void note_alloc(void *p, size_t n, int is_realloc) {
    void *fr[DEPTH + 2];
    int nf = backtrace(fr, DEPTH + 2);
    /* drop this function and the interposed entry point */
    int skip = nf > 2 ? 2 : 0;
    uint32_t s = site_of(fr + skip, nf - skip);
    Site *st = &sites[s];
    if (is_realloc) st->reallocs++; else st->allocs++;
    st->bytes += n;
    st->size[lg(n ? n : 1)]++;
    if (++st->live > st->peak_live) st->peak_live = st->live;
    live_put(p, s);
}

#define HOOK(stmt_alloc, p, n, is_re, oldp)                         \
    do {                                                            \
        if (in_hook || !ready) { stmt_alloc; return p; }            \
        in_hook = 1;                                                \
        stmt_alloc;                                                 \
        pthread_mutex_lock(&lock);                                  \
        uint64_t t_ = now_ns();                                     \
        if (oldp) note_free(oldp, t_);                              \
        if (p) note_alloc(p, n, is_re);                             \
        pthread_mutex_unlock(&lock);                                \
        in_hook = 0;                                                \
        return p;                                                   \
    } while (0)

void *malloc(size_t n) {
    if (!ready) { init(); if (!r_malloc) return boot_alloc(n); }
    void *p;
    HOOK(p = r_malloc(n), p, n, 0, (void *)0);
}

void *calloc(size_t a, size_t b) {
    if (!ready) {
        init();
        if (!r_calloc) { void *q = boot_alloc(a * b); if (q) memset(q, 0, a * b); return q; }
    }
    void *p;
    HOOK(p = r_calloc(a, b), p, a * b, 0, (void *)0);
}

void *realloc(void *o, size_t n) {
    if (!ready) init();
    if (is_boot(o)) {
        void *q = malloc(n);
        if (q && o) memcpy(q, o, n);
        return q;
    }
    void *p;
    HOOK(p = r_realloc(o, n), p, n, o != NULL, o);
}

void free(void *p) {
    if (!p || is_boot(p)) return;
    if (!ready) init();
    if (in_hook || !ready) { r_free(p); return; }
    in_hook = 1;
    pthread_mutex_lock(&lock);
    note_free(p, now_ns());
    pthread_mutex_unlock(&lock);
    r_free(p);
    in_hook = 0;
}

int posix_memalign(void **pp, size_t a, size_t n) {
    if (!ready) init();
    int rc;
    if (in_hook) return r_posix_memalign(pp, a, n);
    in_hook = 1;
    rc = r_posix_memalign(pp, a, n);
    if (!rc) {
        pthread_mutex_lock(&lock);
        note_alloc(*pp, n, 0);
        pthread_mutex_unlock(&lock);
    }
    in_hook = 0;
    return rc;
}

void *aligned_alloc(size_t a, size_t n) {
    if (!ready) init();
    void *p;
    HOOK(p = r_aligned_alloc(a, n), p, n, 0, (void *)0);
}

void *memalign(size_t a, size_t n) {
    if (!ready) init();
    void *p;
    HOOK(p = r_memalign(a, n), p, n, 0, (void *)0);
}

__attribute__((destructor)) static void dump(void) {
    const char *base = getenv("ALLOC_TRACE_OUT");
    if (!base || !sites) return;
    in_hook = 1;
    char path[4096];
    snprintf(path, sizeof path, "%s.%d", base, (int)getpid());
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (uint32_t i = 0; i < (1u << SITE_BITS); i++) {
        Site *s = &sites[i];
        if (!s->key) continue;
        fprintf(f, "S %lu %lu %lu %lu %lu %lu\n", s->allocs, s->reallocs, s->frees,
                s->bytes, s->live, s->peak_live);
        fprintf(f, "L");
        for (int b = 0; b < NBUCKET; b++) fprintf(f, " %lu", s->life[b]);
        fprintf(f, "\nZ");
        for (int b = 0; b < NBUCKET; b++) fprintf(f, " %lu", s->size[b]);
        fprintf(f, "\n");
        for (int k = 0; k < s->nframes; k++) {
            Dl_info di;
            if (dladdr(s->frames[k], &di) && di.dli_fname)
                fprintf(f, "F %s %lx %lx\n", di.dli_fname,
                        (unsigned long)((uintptr_t)s->frames[k] - (uintptr_t)di.dli_fbase),
                        (unsigned long)(uintptr_t)di.dli_fbase);
            else
                fprintf(f, "F ? %lx 0\n", (unsigned long)(uintptr_t)s->frames[k]);
        }
    }
    fclose(f);
}
