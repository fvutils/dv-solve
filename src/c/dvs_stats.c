/*
 * dvs_stats -- solve statistics. See dvs_stats.h and
 * docs/solve_stats_capture_plan.md (S0, S1).
 *
 * A solve's record lives on its thread until it closes; then, under one
 * mutex, it is added to its shape class's counters. The run-level counters
 * are the sum over classes, made when a snapshot is taken, so a solve updates
 * one set of counters.
 */
#include "dvs_stats.h"
#include "dvs_class.h"
#include "dvs_problem.h"
#include "dvs_thread.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  include <process.h>
#  define dvs__getpid _getpid
#else
#  include <unistd.h>
#  define dvs__getpid getpid
#endif

#ifndef DVS_VERSION
#  define DVS_VERSION "unknown"
#endif
#ifndef DVS_GIT_COMMIT
#  define DVS_GIT_COMMIT "unknown"
#endif

#if defined(_MSC_VER)
#  define DVS__TLS __declspec(thread)
#else
#  define DVS__TLS _Thread_local
#endif

#define N_HIST 48           /* log2 buckets: 0 for 0, k for [2^(k-1), 2^k);
                             * the last also holds everything above */
#define N_BAIL 16           /* DVS_BAIL_* values */
#define HLL_BITS 6          /* HyperLogLog registers: 2^6, ~13% error */
#define HLL_REGS (1u << HLL_BITS)
#define MAX_CLASSES 2048    /* more are counted under class 0 */
#define CLASS_CACHE 4096    /* fingerprint -> class entries */
#define ONES_BITS 512       /* per-bit one-counts kept per class, in tracked
                             * order; variables past this get none */
#define ONES_NONE 0xffffu
#define STUCK_MODELS 16     /* models before a variable that never changed is
                             * reported stuck */

volatile int dvs_stats_g_on = -1;

/* The record of the solve open on this thread. */
typedef struct {
    uint8_t  open;
    uint8_t  path;
    uint8_t  ran;           /* bit per dvs_stats_engine_t */
    int8_t   last;          /* the engine whose scope opened last, -1 none */
    int8_t   cur;           /* the engine whose scope is open, -1 none */
    uint8_t  bail;          /* last non-zero CDCL bail reason */
    uint8_t  via;           /* dvs_stats_via_t */
    uint64_t reasons;       /* bit per dvs_stats_reason_t */
    uint64_t cls;           /* shape class, 0 not known */
    uint64_t inst;          /* instance fingerprint, 0 not known */
    uint8_t  has_vals;      /* vals holds this solve's model */
    uint8_t  read;          /* an SMT2_AUX record that reads a model back */
    uint8_t  uniform;       /* the solve asked for uniform sampling */
    uint8_t  has_bind;      /* bind holds this solve's binding counts */
    dvs_stats_binding_t bind;
    uint64_t t0;
    uint64_t work[DVS_STATS_N_ENGINES];
    /* The values of the last model noted in the solve. Last, so that
     * dvs_stats_begin clears only what is before it; one thread-local, so
     * that a hook costs one TLS lookup. */
    struct {
        uint32_t n;
        uint8_t  width[DVS_CLASS_TRACK], free[DVS_CLASS_TRACK];
        uint64_t v[DVS_CLASS_TRACK];
    } vals;
} Rec;

static DVS__TLS Rec tl_rec;

/* Per thread, across records (signal 6): build time waiting for the next
 * solve, the class and instance of the last one, and whether a library step
 * is being timed (dvs_stats_step_begin). */
static DVS__TLS struct { uint64_t build_ns, cls, inst; int in_step; } tl_thr;

enum { PREV_SAME, PREV_CONSTANTS, PREV_SHAPE, N_PREV };
static const char *const k_prev[N_PREV] = { "same", "constants", "shape" };

/* The counters kept per class, and summed for the run. All uint64_t. */
typedef struct {
    uint64_t solves[DVS_STATS_N_PATHS][DVS_STATS_N_RESULTS];
    uint64_t ran[DVS_STATS_N_ENGINES];
    uint64_t answered[DVS_STATS_N_ENGINES + 1];     /* last: no engine ran */
    uint64_t fallbacks;     /* solves in which more than one engine ran */
    uint64_t work[DVS_STATS_N_ENGINES];
    uint64_t wasted[DVS_STATS_N_ENGINES];
    uint64_t bail[N_BAIL];
    uint64_t via[DVS_STATS_N_VIA];
    uint64_t reasons[DVS_STATS_N_REASONS];
    uint64_t wall_ns;
    uint64_t hist_wall_ns[N_HIST];
    uint64_t hist_work[DVS_STATS_N_ENGINES][N_HIST];  /* engines that ran */
    /* Signal 3, summed over the class's roots (dvs_stats_root_t). */
    uint64_t compiles, compile_ns;
    uint64_t roots, root_vars, root_fixed, root_tied, root_bits16, root_free_bits16;
    uint64_t root_cons, root_quiet, root_dropped;
    /* Signal 5, summed over the sampled CDCL solves (dvs_stats_binding_t). */
    uint64_t bind_solves, bind_cons, bind_narrowed, bind_conflicted;
    uint64_t bind_busy, bind_top_pm, bind_other_pm;
    /* Signal 6: time stating the problem (SMT2) and reading models back, and
     * each solve against the previous one on its thread. */
    uint64_t build_ns, read_ns;
    uint64_t uniform;       /* solves that asked for uniform sampling */
    uint64_t prev[N_PREV];
} Counts;

/* The values a class's models took (signals 4 and 7). The layout is the
 * first model's; a model with another layout (a truncated class walk) is
 * counted in `mismatched` only. The values themselves stay in memory: a
 * snapshot reports what they span, never what they are. */
typedef struct {
    uint32_t n;
    uint8_t  width[DVS_CLASS_TRACK], free[DVS_CLASS_TRACK];
    uint16_t ones_at[DVS_CLASS_TRACK];  /* into ones[], ONES_NONE if none */
    uint64_t models, mismatched;
    uint64_t v_or[DVS_CLASS_TRACK], v_and[DVS_CLASS_TRACK];
    uint8_t  hll_sol[HLL_REGS];         /* distinct models */
    uint32_t ones[ONES_BITS];
} Vals;

typedef struct {
    uint64_t cls;
    Counts   n;
    uint8_t  hll_inst[HLL_REGS];    /* distinct instances */
    Vals    *vals;                  /* NULL until a model is noted */
} ClassRec;

/* A class as a snapshot reports it. */
typedef struct {
    ClassRec r;
    int      has_f, has_v;
    dvs_class_features_t f;
    Vals     v;
} ClassSnap;

static struct {
    uint64_t compiles;
    uint64_t aux_calls;     /* DVS_STATS_PATH_SMT2_AUX records */
    uint64_t aux_work[DVS_STATS_N_ENGINES];
} g_run;
static ClassRec **g_cls;            /* open addressing on cls; NULL = empty */
static uint32_t   g_cls_cap, g_cls_n;
static uint64_t   g_cls_dropped;    /* solves counted under 0: table full */
static dvs_mutex_t g_mu = DVS_MUTEX_INIT;

typedef struct { uint64_t fp, cls; dvs_class_track_t *track; } CacheSlot;
static CacheSlot  g_cache[CLASS_CACHE];

/* What each class is made of, recorded when its hash is first computed. */
typedef struct { uint64_t cls; dvs_class_features_t f; } Info;
static Info      *g_info;           /* open addressing on cls; cls 0 = empty */
static uint32_t   g_info_cap, g_info_n;

static const char *const k_path[DVS_STATS_N_PATHS] = {
    "api", "handle", "dpi", "smt2", "verilator", "bvsat", "smt2-aux"
};
static const char *const k_engine[DVS_STATS_N_ENGINES] = {
    "cdcl", "bitblast", "cube"
};
static const char *const k_answered[DVS_STATS_N_ENGINES + 1] = {
    "cdcl", "bitblast", "cube", "none"
};
static const char *const k_result[DVS_STATS_N_RESULTS] = {
    "sat", "unsat", "unknown", "error"
};
static const char *const k_via[DVS_STATS_N_VIA] = {
    "none", "cdcl-root", "cdcl-search", "cdcl-enum", "cdcl-box",
    "bb-sat", "bb-popcount", "bb-unique", "bb-sorted", "bb-pinfast",
    "bb-divmul", "bb-enum", "bb-affine", "bb-sim-split", "bb-sim-exhaustive",
    "bb-sim-random", "bb-sim-race", "bb-sim-once", "bb-sat-fresh",
    "bb-rediversify", "cube", "hash-skip", "model-reuse", "tainted",
    "cache-unsat", "compile-unsat"
};
/* The SMT2 front end's strings for these are the same text. */
static const char *const k_reason[DVS_STATS_N_REASONS] = {
    "other",
    "wide-var", "wide-arith", "wide-const", "sext64", "sdiv-srem", "smod",
    "flush-failed",
    "multiplier", "bit-level", "seeded sample",
    "logic", "array", "engine-forced",
    "probe", "sticky", "enumeration", "opt-out", "cdcl-slow", "cdcl-declined",
    "compile-failed", "pool-overflow", "cdcl-unknown", "cdcl-error"
};
/* The record keeps the reasons as a 64-bit mask. */
typedef char dvs__reasons_fit[DVS_STATS_N_REASONS <= 64 ? 1 : -1];

/* Indexed by DVS_BAIL_* (dvs_search.h); NULL for values not in use. */
static const char *const k_bail[N_BAIL] = {
    NULL, "deadline", "max-depth", "deadline-conflict", "max-restarts",
    "propagation", "thrash", "work"
};

/* DV_STATS's file, written at exit. */
static char g_exit_path[1024];

static void _write_at_exit(void) {
    if (g_exit_path[0]) (void)dvs_stats_write(g_exit_path);
}

int dvs_stats_init_env(void) {
    const char *e = getenv("DV_STATS");
    int on = e && *e && strcmp(e, "0") != 0;
    /* A racing dvs_stats_enable() wins: only replace the unread state. */
    if (dvs_stats_g_on < 0) {
        if (on) {
            static volatile int registered;
            const char *path = strcmp(e, "1") == 0 ? "dv_solve_stats.%p.json" : e;
            if (!registered && strlen(path) < sizeof g_exit_path) {
                registered = 1;
                strcpy(g_exit_path, path);
                atexit(_write_at_exit);
            }
        }
        dvs_stats_g_on = on;
    }
    return dvs_stats_g_on > 0;
}

void dvs_stats_enable(int on) { dvs_stats_g_on = on ? 1 : 0; }

int dvs_stats_enabled(void) { return dvs_stats_on(); }

void dvs_stats_reset(void) {
    dvs_mutex_lock(&g_mu);
    memset(&g_run, 0, sizeof g_run);
    for (uint32_t i = 0; i < g_cls_cap; i++)
        if (g_cls[i]) { free(g_cls[i]->vals); free(g_cls[i]); }
    free(g_cls);
    g_cls = NULL;
    g_cls_cap = g_cls_n = 0;
    g_cls_dropped = 0;
    dvs_mutex_unlock(&g_mu);
}

static inline uint64_t _fmix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* The monotonic clock: a thread CPU clock read costs ~77 ns on Linux against
 * ~16 ns, and the two reads are most of a solve's statistics cost. */
static uint64_t _now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static unsigned _bucket(uint64_t x) {
    unsigned b = x ? 64u - (unsigned)__builtin_clzll(x) : 0u;
    return b < N_HIST ? b : N_HIST - 1;
}

/* ------------------------------------------------------------------ */
/* Classes                                                             */
/* ------------------------------------------------------------------ */

void dvs_stats_note_class(uint64_t cls, uint64_t inst) {
    if (tl_rec.open && !tl_rec.cls) {
        tl_rec.cls = cls;
        tl_rec.inst = inst;
    }
}

int dvs_stats_need_class(void) { return tl_rec.open && !tl_rec.cls; }

/* Record class `cls`'s features, once. Called with g_mu held. */
static void _info_put(uint64_t cls, const dvs_class_features_t *f) {
    if ((g_info_n + 1) * 2 > g_info_cap) {
        if (g_info_n >= 2 * MAX_CLASSES) return;
        uint32_t nc = g_info_cap ? g_info_cap * 2 : 64;
        Info *nt = (Info *)calloc(nc, sizeof *nt);
        if (!nt) return;
        for (uint32_t i = 0; i < g_info_cap; i++) {
            if (!g_info[i].cls) continue;
            uint32_t j = (uint32_t)_fmix(g_info[i].cls) & (nc - 1);
            while (nt[j].cls) j = (j + 1) & (nc - 1);
            nt[j] = g_info[i];
        }
        free(g_info);
        g_info = nt;
        g_info_cap = nc;
    }
    uint32_t i = (uint32_t)_fmix(cls) & (g_info_cap - 1);
    while (g_info[i].cls && g_info[i].cls != cls) i = (i + 1) & (g_info_cap - 1);
    if (!g_info[i].cls) {
        g_info[i].cls = cls;
        g_info[i].f = *f;
        g_info_n++;
    }
}

/* Class `cls`'s features, or NULL. Called with g_mu held. */
static const dvs_class_features_t *_info_get(uint64_t cls) {
    if (!g_info_cap || !cls) return NULL;
    uint32_t i = (uint32_t)_fmix(cls) & (g_info_cap - 1);
    while (g_info[i].cls && g_info[i].cls != cls) i = (i + 1) & (g_info_cap - 1);
    return g_info[i].cls ? &g_info[i].f : NULL;
}

uint64_t dvs_stats_class_of(const dvs_problem_t *p, uint64_t fp,
                            dvs_class_track_t *track) {
    if (track) track->n = 0;
    if (!p) return 0;
    if (!fp) fp = dvs_problem_fingerprint(p);
    CacheSlot *s = &g_cache[_fmix(fp) % CLASS_CACHE];
    dvs_mutex_lock(&g_mu);
    uint64_t cls = s->fp == fp ? s->cls : 0;
    if (cls && track && s->track) *track = *s->track;
    dvs_mutex_unlock(&g_mu);
    if (cls) return cls;
    dvs_class_t *c = (dvs_class_t *)malloc(sizeof *c);
    if (!c) return 0;
    if (dvs_class_compute(p, c) != 0) { free(c); return 0; }
    dvs_class_track_t *t = (dvs_class_track_t *)malloc(sizeof *t);
    if (t) *t = c->track;
    if (track) *track = c->track;
    cls = c->hash;
    dvs_mutex_lock(&g_mu);
    s->fp = fp;
    s->cls = cls;
    free(s->track);
    s->track = t;
    _info_put(cls, &c->f);
    dvs_mutex_unlock(&g_mu);
    free(c);
    return cls;
}

/* The record for `cls`, made if new; class 0's when the table is full. Called
 * with g_mu held. NULL only when memory ran out. */
/* The record of class `cls`, made if new. A full table puts a solve under
 * class 0 (`solve` set: counted as dropped) and anything else nowhere. */
static ClassRec *_class_rec(uint64_t cls, int solve) {
    if ((g_cls_n + 1) * 2 > g_cls_cap) {
        uint32_t nc = g_cls_cap ? g_cls_cap * 2 : 64;
        ClassRec **nt = (ClassRec **)calloc(nc, sizeof *nt);
        if (!nt) return NULL;
        for (uint32_t i = 0; i < g_cls_cap; i++) {
            if (!g_cls[i]) continue;
            uint32_t j = (uint32_t)_fmix(g_cls[i]->cls) & (nc - 1);
            while (nt[j]) j = (j + 1) & (nc - 1);
            nt[j] = g_cls[i];
        }
        free(g_cls);
        g_cls = nt;
        g_cls_cap = nc;
    }
    for (int pass = 0; pass < 2; pass++) {
        uint32_t i = (uint32_t)_fmix(cls) & (g_cls_cap - 1);
        while (g_cls[i] && g_cls[i]->cls != cls) i = (i + 1) & (g_cls_cap - 1);
        if (g_cls[i]) return g_cls[i];
        if (cls != 0 && g_cls_n >= MAX_CLASSES) {
            if (!solve) return NULL;
            g_cls_dropped++;
            cls = 0;
            continue;
        }
        ClassRec *r = (ClassRec *)calloc(1, sizeof *r);
        if (!r) return NULL;
        r->cls = cls;
        g_cls[i] = r;
        g_cls_n++;
        return r;
    }
    return NULL;
}

static void _hll_add(uint8_t *reg, uint64_t x) {
    uint64_t h = _fmix(x);
    uint32_t i = (uint32_t)(h >> (64 - HLL_BITS));
    uint64_t w = (h << HLL_BITS) | (1ULL << (HLL_BITS - 1));
    uint8_t rank = (uint8_t)(__builtin_clzll(w) + 1);
    if (rank > reg[i]) reg[i] = rank;
}

/* ln(x) for x >= 1, without libm (the library does not link it). */
static double _ln(double x) {
    int k = 0;
    while (x >= 2.0) { x *= 0.5; k++; }
    double y = (x - 1.0) / (x + 1.0), y2 = y * y, term = y, sum = 0;
    for (int n = 1; n < 40; n += 2) { sum += term / n; term *= y2; }
    return 2.0 * sum + k * 0.69314718055994530942;
}

static double _hll_estimate(const uint8_t *reg) {
    double sum = 0;
    unsigned zeros = 0;
    for (uint32_t i = 0; i < HLL_REGS; i++) {
        sum += 1.0 / (double)(1ULL << reg[i]);
        zeros += reg[i] == 0;
    }
    double m = HLL_REGS, e = 0.709 * m * m / sum;
    if (e <= 2.5 * m && zeros) e = m * _ln(m / zeros);   /* linear counting */
    return e;
}

/* ------------------------------------------------------------------ */
/* Records                                                             */
/* ------------------------------------------------------------------ */

static void _add_vals(ClassRec *c, const Rec *r);

int dvs_stats_begin(dvs_stats_path_t path) {
    if (!dvs_stats_on() || tl_rec.open) return 0;
    memset(&tl_rec, 0, offsetof(Rec, vals));
    tl_rec.open = 1;
    tl_rec.path = (uint8_t)path;
    tl_rec.last = -1;
    tl_rec.cur = -1;
    tl_rec.t0 = _now_ns();
    return 1;
}

void dvs_stats_end(dvs_stats_result_t res) {
    Rec *r = &tl_rec;
    if (!r->open) return;
    uint64_t wall = _now_ns() - r->t0;
    r->open = 0;
    if (r->path == DVS_STATS_PATH_SMT2_AUX) {
        dvs_stats_time(r->read ? DVS_STATS_TIME_READ : DVS_STATS_TIME_BUILD, 0, wall);
        if (!r->ran) return;
        dvs_mutex_lock(&g_mu);
        g_run.aux_calls++;
        for (int e = 0; e < DVS_STATS_N_ENGINES; e++) g_run.aux_work[e] += r->work[e];
        dvs_mutex_unlock(&g_mu);
        return;
    }
    unsigned prev = !tl_thr.cls || tl_thr.cls != r->cls ? PREV_SHAPE
                  : tl_thr.inst == r->inst && r->inst ? PREV_SAME : PREV_CONSTANTS;
    uint64_t build_ns = tl_thr.build_ns;
    tl_thr.build_ns = 0;
    tl_thr.cls = r->cls;
    tl_thr.inst = r->inst;
    dvs_mutex_lock(&g_mu);
    ClassRec *c = _class_rec(r->cls, 1);
    if (c) {
        Counts *n = &c->n;
        unsigned n_ran = 0;
        n->solves[r->path][res]++;
        for (int e = 0; e < DVS_STATS_N_ENGINES; e++) {
            if (!((r->ran >> e) & 1u)) continue;
            n_ran++;
            n->ran[e]++;
            n->work[e] += r->work[e];
            n->hist_work[e][_bucket(r->work[e])]++;
            if (e != r->last) n->wasted[e] += r->work[e];
        }
        n->answered[r->last < 0 ? DVS_STATS_N_ENGINES : r->last]++;
        if (n_ran > 1) n->fallbacks++;
        if (r->bail) n->bail[r->bail]++;
        n->via[r->via]++;
        for (uint64_t m = r->reasons; m; m &= m - 1) n->reasons[__builtin_ctzll(m)]++;
        n->wall_ns += wall;
        n->hist_wall_ns[_bucket(wall)]++;
        n->build_ns += build_ns;
        n->uniform += r->uniform;
        if (r->has_bind) {
            const dvs_stats_binding_t *b = &r->bind;
            n->bind_solves++;
            n->bind_cons += b->cons;
            n->bind_narrowed += b->narrowed;
            n->bind_conflicted += b->conflicted;
            n->bind_busy += b->busy;
            n->bind_top_pm += b->top_pm;
            n->bind_other_pm += b->other_pm;
        }
        if (r->cls) n->prev[prev]++;
        if (r->inst) _hll_add(c->hll_inst, r->inst);
        if (r->has_vals && res == DVS_STATS_RES_SAT) _add_vals(c, r);
    }
    dvs_mutex_unlock(&g_mu);
}

int dvs_stats_want_values(void) { return tl_rec.open; }

void dvs_stats_note_values(const dvs_class_track_t *t, const uint64_t *v) {
    Rec *r = &tl_rec;
    if (!r->open || !t) return;
    r->vals.n = t->n;
    memcpy(r->vals.width, t->width, t->n);
    memcpy(r->vals.free, t->free, t->n);
    for (uint32_t i = 0; i < t->n; i++) {
        uint32_t w = t->width[i];
        r->vals.v[i] = w >= 64 ? v[i] : v[i] & ((1ULL << w) - 1);
    }
    r->has_vals = 1;
}

/* Add the closing solve's model (r->vals) to class record `c`. Called with
 * g_mu held. */
static void _add_vals(ClassRec *c, const Rec *r) {
    Vals *q = c->vals;
    uint32_t n = r->vals.n;
    if (!q) {
        if (!(q = c->vals = (Vals *)calloc(1, sizeof *q))) return;
        q->n = n;
        uint32_t at = 0;
        for (uint32_t i = 0; i < n; i++) {
            q->width[i] = r->vals.width[i];
            q->free[i] = r->vals.free[i];
            q->v_and[i] = ~0ULL;
            if (at + q->width[i] <= ONES_BITS) {
                q->ones_at[i] = (uint16_t)at;
                at += q->width[i];
            } else {
                q->ones_at[i] = ONES_NONE;
            }
        }
    }
    if (q->n != n || memcmp(q->width, r->vals.width, n)) {
        q->mismatched++;
        return;
    }
    for (uint32_t i = 0; i < n; i++) q->free[i] |= r->vals.free[i];
    q->models++;
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t x = r->vals.v[i];
        h = (h ^ x) * 0x100000001b3ULL + (h >> 29);    /* _hll_add mixes it */
        q->v_or[i] |= x;
        q->v_and[i] &= x;
    }
    _hll_add(q->hll_sol, h);
    for (uint32_t i = 0; i < n; i++) {
        if (q->ones_at[i] == ONES_NONE) continue;
        uint32_t *ones = q->ones + q->ones_at[i];
        for (uint64_t x = r->vals.v[i]; x; x &= x - 1) ones[__builtin_ctzll(x)]++;
    }
}

int dvs_stats_engine_enter(dvs_stats_engine_t eng) {
    Rec *r = &tl_rec;
    if (!r->open || r->cur >= 0) return 0;
    static const uint8_t k_default_via[DVS_STATS_N_ENGINES] = {
        DVS_STATS_VIA_CDCL_SEARCH, DVS_STATS_VIA_BB_SAT, DVS_STATS_VIA_CUBE
    };
    r->cur = (int8_t)eng;
    r->last = (int8_t)eng;
    r->via = k_default_via[eng];
    r->ran |= (uint8_t)(1u << eng);
    return 1;
}

void dvs_stats_engine_leave(int token) {
    if (token) tl_rec.cur = -1;
}

void dvs_stats_add_work(uint64_t work) {
    Rec *r = &tl_rec;
    if (r->open && r->cur >= 0) r->work[r->cur] += work;
}

void dvs_stats_note_via(dvs_stats_via_t via) {
    if (tl_rec.open) tl_rec.via = (uint8_t)via;
}

void dvs_stats_note_reason(dvs_stats_reason_t why) {
    if (tl_rec.open) tl_rec.reasons |= 1ull << why;
}

dvs_stats_reason_t dvs_stats_reason_of(const char *why) {
    if (why)
        for (int i = 1; i < DVS_STATS_N_REASONS; i++)
            if (strcmp(why, k_reason[i]) == 0) return (dvs_stats_reason_t)i;
    return DVS_STATS_REASON_OTHER;
}

const char *dvs_stats_reason_name(dvs_stats_reason_t why) {
    return (unsigned)why < DVS_STATS_N_REASONS ? k_reason[why] : k_reason[0];
}

void dvs_stats_note_uniform(void) {
    if (tl_rec.open) tl_rec.uniform = 1;
}

void dvs_stats_note_bail(unsigned bail) {
    if (tl_rec.open && bail && bail < N_BAIL) tl_rec.bail = (uint8_t)bail;
}

uint64_t dvs_stats_clock(void) { return _now_ns(); }

uint64_t dvs_stats_bits16(int64_t lo, int64_t hi, uint32_t width) {
    uint64_t d = (uint64_t)hi - (uint64_t)lo;
    double b = d == UINT64_MAX ? 64.0 : log2((double)d + 1.0);
    if (b > (double)width) b = (double)width;
    return (uint64_t)(b * 16.0 + 0.5);
}

void dvs_stats_note_compile(uint64_t cls, uint64_t ns) {
    dvs_mutex_lock(&g_mu);
    g_run.compiles++;
    ClassRec *c = cls ? _class_rec(cls, 0) : NULL;
    if (c) {
        c->n.compiles++;
        c->n.compile_ns += ns;
    }
    dvs_mutex_unlock(&g_mu);
}

void dvs_stats_note_root(uint64_t cls, const dvs_stats_root_t *root) {
    if (!cls) return;
    dvs_mutex_lock(&g_mu);
    ClassRec *c = _class_rec(cls, 0);
    if (c) {
        Counts *n = &c->n;
        n->roots++;
        n->root_vars += root->vars;
        n->root_fixed += root->fixed;
        n->root_tied += root->tied;
        n->root_bits16 += root->bits16;
        n->root_free_bits16 += root->free_bits16;
        n->root_cons += root->cons;
        n->root_quiet += root->cons_quiet;
        n->root_dropped += root->cons_dropped;
    }
    dvs_mutex_unlock(&g_mu);
}

void dvs_stats_note_binding(const dvs_stats_binding_t *b) {
    if (!tl_rec.open) return;
    tl_rec.has_bind = 1;
    tl_rec.bind = *b;
}

void dvs_stats_time(dvs_stats_time_t kind, uint64_t cls, uint64_t ns) {
    if (kind == DVS_STATS_TIME_BUILD) {
        tl_thr.build_ns += ns;
        return;
    }
    if (!cls) cls = tl_thr.cls;
    if (!cls) return;
    dvs_mutex_lock(&g_mu);
    ClassRec *c = _class_rec(cls, 0);
    if (c) c->n.read_ns += ns;
    dvs_mutex_unlock(&g_mu);
}

void dvs_stats_add_time(dvs_stats_time_t kind, uint64_t ns) {
    if (dvs_stats_on() && (kind == DVS_STATS_TIME_BUILD || kind == DVS_STATS_TIME_READ))
        dvs_stats_time(kind, 0, ns);
}

uint64_t dvs_stats_step_begin(void) {
    if (!dvs_stats_on() || tl_rec.open || tl_thr.in_step) return 0;
    tl_thr.in_step = 1;
    return _now_ns() | 1u;          /* never 0 */
}

void dvs_stats_step_end(uint64_t t0, dvs_stats_time_t kind, uint64_t cls) {
    if (!t0) return;
    tl_thr.in_step = 0;
    dvs_stats_time(kind, cls, _now_ns() - t0);
}

void dvs_stats_note_read(void) {
    if (tl_rec.open && tl_rec.path == DVS_STATS_PATH_SMT2_AUX) tl_rec.read = 1;
}

dvs_stats_result_t dvs_stats_result_of(dvs_result_t r) {
    switch (r) {
    case DVS_SOLVE_OK:      return DVS_STATS_RES_SAT;
    case DVS_SOLVE_UNSAT:   return DVS_STATS_RES_UNSAT;
    case DVS_SOLVE_TIMEOUT: return DVS_STATS_RES_UNKNOWN;
    default:                return DVS_STATS_RES_ERROR;
    }
}

dvs_stats_result_t dvs_stats_result_of_bb(int bb_rc) {
    switch (bb_rc) {
    case 10: return DVS_STATS_RES_SAT;      /* DVS_BB_SAT */
    case 20: return DVS_STATS_RES_UNSAT;    /* DVS_BB_UNSAT */
    case 0:  return DVS_STATS_RES_UNKNOWN;  /* DVS_BB_UNKNOWN */
    default: return DVS_STATS_RES_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; size_t cap; size_t len; } Out;

static void _put(Out *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t room = o->len < o->cap ? o->cap - o->len : 0;
    int n = vsnprintf(room ? o->buf + o->len : NULL, room, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += (size_t)n;
}

/* ,"key":{"name":n,...} over `n` entries; zeros are left out unless `all`,
 * and entries with no name are summed into "other". */
static void _put_named(Out *o, const char *key, const uint64_t *v, int n,
                       const char *const *names, int all) {
    _put(o, ",\"%s\":{", key);
    int first = 1;
    uint64_t other = 0;
    for (int i = 0; i < n; i++) {
        if (!v[i] && !all) continue;
        if (!names[i]) { other += v[i]; continue; }
        _put(o, "%s\"%s\":%llu", first ? "" : ",", names[i], (unsigned long long)v[i]);
        first = 0;
    }
    if (other) _put(o, "%s\"other\":%llu", first ? "" : ",", (unsigned long long)other);
    _put(o, "}");
}

/* "name":[[bucket,count],...], the non-zero buckets; `sep` goes first. */
static void _put_hist(Out *o, const char *sep, const char *name, const uint64_t *h) {
    _put(o, "%s\"%s\":[", sep, name);
    int first = 1;
    for (int b = 0; b < N_HIST; b++) {
        if (!h[b]) continue;
        _put(o, "%s[%d,%llu]", first ? "" : ",", b, (unsigned long long)h[b]);
        first = 0;
    }
    _put(o, "]");
}

static void _add_counts(Counts *t, const Counts *c) {
    const uint64_t *s = (const uint64_t *)c;
    uint64_t *d = (uint64_t *)t;
    for (size_t i = 0; i < sizeof *c / sizeof *s; i++) d[i] += s[i];
}

/* The members for counters `n` (a class's, or the run's). */
static void _put_counts(Out *o, const Counts *n) {
    uint64_t total = 0, by_res[DVS_STATS_N_RESULTS] = { 0 };
    for (int p = 0; p < DVS_STATS_N_PATHS; p++)
        for (int r = 0; r < DVS_STATS_N_RESULTS; r++) {
            total += n->solves[p][r];
            by_res[r] += n->solves[p][r];
        }
    _put(o, ",\"solves\":%llu,\"wall_ns\":%llu", (unsigned long long)total,
         (unsigned long long)n->wall_ns);
    _put_named(o, "results", by_res, DVS_STATS_N_RESULTS, k_result, 1);
    _put(o, ",\"paths\":{");
    int first = 1;
    for (int p = 0; p < DVS_STATS_N_PATHS; p++) {
        uint64_t k = 0;
        for (int r = 0; r < DVS_STATS_N_RESULTS; r++) k += n->solves[p][r];
        if (!k) continue;
        _put(o, "%s\"%s\":{\"solves\":%llu", first ? "" : ",", k_path[p],
             (unsigned long long)k);
        for (int r = 0; r < DVS_STATS_N_RESULTS; r++)
            _put(o, ",\"%s\":%llu", k_result[r], (unsigned long long)n->solves[p][r]);
        _put(o, "}");
        first = 0;
    }
    _put(o, "}");
    _put_named(o, "engine_ran", n->ran, DVS_STATS_N_ENGINES, k_engine, 1);
    _put_named(o, "answered_by", n->answered, DVS_STATS_N_ENGINES + 1, k_answered, 1);
    _put(o, ",\"fallbacks\":%llu", (unsigned long long)n->fallbacks);
    _put_named(o, "work", n->work, DVS_STATS_N_ENGINES, k_engine, 1);
    _put_named(o, "wasted_work", n->wasted, DVS_STATS_N_ENGINES, k_engine, 1);
    _put_named(o, "bail", n->bail, N_BAIL, k_bail, 0);
    _put_named(o, "via", n->via, DVS_STATS_N_VIA, k_via, 0);
    _put_named(o, "reasons", n->reasons, DVS_STATS_N_REASONS, k_reason, 0);
    _put(o, ",\"time_ns\":{\"build\":%llu,\"compile\":%llu,\"read\":%llu}",
         (unsigned long long)n->build_ns, (unsigned long long)n->compile_ns,
         (unsigned long long)n->read_ns);
    _put_named(o, "previous", n->prev, N_PREV, k_prev, 1);
    _put(o, ",\"uniform\":%llu", (unsigned long long)n->uniform);
    if (n->roots)
        _put(o, ",\"root\":{\"count\":%llu,\"vars\":%llu,\"fixed\":%llu,"
                "\"tied\":%llu,\"bits\":%llu,\"free_bits\":%llu,\"constraints\":%llu,"
                "\"quiet\":%llu,\"dropped\":%llu}",
             (unsigned long long)n->roots, (unsigned long long)n->root_vars,
             (unsigned long long)n->root_fixed, (unsigned long long)n->root_tied,
             (unsigned long long)((n->root_bits16 + 8) / 16),
             (unsigned long long)((n->root_free_bits16 + 8) / 16),
             (unsigned long long)n->root_cons, (unsigned long long)n->root_quiet,
             (unsigned long long)n->root_dropped);
    if (n->bind_solves)
        _put(o, ",\"binding\":{\"solves\":%llu,\"constraints\":%llu,\"narrowed\":%llu,"
                "\"conflicted\":%llu,\"busy\":%llu,\"top_share\":%llu,\"other_share\":%llu}",
             (unsigned long long)n->bind_solves, (unsigned long long)n->bind_cons,
             (unsigned long long)n->bind_narrowed, (unsigned long long)n->bind_conflicted,
             (unsigned long long)n->bind_busy, (unsigned long long)n->bind_top_pm,
             (unsigned long long)n->bind_other_pm);
    _put(o, ",\"hist\":{");
    _put_hist(o, "", "wall_ns", n->hist_wall_ns);
    for (int e = 0; e < DVS_STATS_N_ENGINES; e++) {
        char name[32];
        snprintf(name, sizeof name, "work_%s", k_engine[e]);
        _put_hist(o, ",", name, n->hist_work[e]);
    }
    _put(o, "}");
}

static int _cmp_class_time(const void *a, const void *b) {
    const ClassRec *x = &((const ClassSnap *)a)->r, *y = &((const ClassSnap *)b)->r;
    if (x->n.wall_ns != y->n.wall_ns) return x->n.wall_ns < y->n.wall_ns ? 1 : -1;
    return (x->cls > y->cls) - (x->cls < y->cls);
}

static void _put_features(Out *o, const dvs_class_features_t *f) {
    static const char *const k_width[DVS_N_WIDTHS] = { "1", "8", "16", "32", "64", "wide" };
    _put(o, ",\"features\":{\"vars\":%u,\"constraints\":%u,\"softs\":%u,"
         "\"alldiffs\":%u,\"dists\":%u,\"ordered\":%u,\"components\":%u,"
         "\"max_component\":%u,\"max_depth\":%u,\"guard_depth\":%u,\"nodes\":%u,"
         "\"ops\":{\"bit\":%u,\"arith\":%u,\"mul\":%u,\"cmp\":%u,\"eq\":%u,"
         "\"bool\":%u,\"other\":%u}",
         f->n_vars, f->n_conjuncts, f->n_softs, f->n_alldiffs, f->n_dists, f->n_ordered,
         f->n_components, f->max_component, f->max_depth, f->guard_depth, f->n_nodes,
         f->n_bit, f->n_arith, f->n_mul, f->n_cmp, f->n_eq, f->n_bool, f->n_other);
    _put(o, ",\"widths\":{");
    for (int i = 0; i < DVS_N_WIDTHS; i++)
        _put(o, "%s\"%s\":%u", i ? "," : "", k_width[i], f->widths[i]);
    _put(o, "}},\"motifs\":{");
    int first = 1;
    for (int m = 0; m < DVS_N_MOTIFS; m++) {
        if (!f->motifs[m]) continue;
        _put(o, "%s\"%s\":%u", first ? "" : ",", dvs_motif_name((dvs_motif_t)m), f->motifs[m]);
        first = 0;
    }
    _put(o, "}");
}

/* What a class's models spanned, per tracked variable: the bits the values
 * used, the bits that took both values, the largest bias of a bit that
 * varied (per mille off 1/2), and whether a variable with a free declared
 * domain never changed. */
static void _put_values(Out *o, const Vals *q) {
    /* the estimate is +-13%: never more than the models themselves */
    uint64_t sol = (uint64_t)(_hll_estimate(q->hll_sol) + 0.5);
    if (sol > q->models) sol = q->models;
    _put(o, ",\"values\":{\"models\":%llu,\"mismatched\":%llu,\"solutions\":%llu,\"hll\":[",
         (unsigned long long)q->models, (unsigned long long)q->mismatched,
         (unsigned long long)sol);
    for (uint32_t k = 0; k < HLL_REGS; k++) _put(o, "%s%u", k ? "," : "", q->hll_sol[k]);
    _put(o, "],\"vars\":[");
    for (uint32_t i = 0; i < q->n; i++) {
        uint64_t vary = q->models ? q->v_or[i] ^ q->v_and[i] : 0;
        unsigned used = q->v_or[i] ? 64u - (unsigned)__builtin_clzll(q->v_or[i]) : 0u;
        unsigned bias = 0;
        if (q->ones_at[i] != ONES_NONE)
            for (uint64_t m = vary; m; m &= m - 1) {
                uint64_t ones = q->ones[q->ones_at[i] + __builtin_ctzll(m)];
                uint64_t n = q->models;
                uint64_t off = 2 * ones > n ? 2 * ones - n : n - 2 * ones;
                unsigned b = n ? (unsigned)(off * 500 / n) : 0u;
                if (b > bias) bias = b;
            }
        int stuck = q->free[i] && !vary && q->models >= STUCK_MODELS;
        _put(o, "%s{\"width\":%u,\"free\":%u,\"used\":%u,\"varies\":%u,\"max_bias\":%u,"
             "\"stuck\":%d}", i ? "," : "", q->width[i], q->free[i], used,
             (unsigned)__builtin_popcountll(vary), bias, stuck);
    }
    _put(o, "]}");
}

size_t dvs_stats_snapshot_json(char *buf, size_t cap) {
    ClassSnap *cls = NULL;
    uint32_t n_cls = 0;
    uint64_t compiles, aux_calls, aux_work[DVS_STATS_N_ENGINES], dropped;
    dvs_mutex_lock(&g_mu);
    compiles = g_run.compiles;
    aux_calls = g_run.aux_calls;
    memcpy(aux_work, g_run.aux_work, sizeof aux_work);
    dropped = g_cls_dropped;
    if (g_cls_n && (cls = (ClassSnap *)malloc((size_t)g_cls_n * sizeof *cls)) != NULL)
        for (uint32_t i = 0; i < g_cls_cap; i++) {
            if (!g_cls[i]) continue;
            ClassSnap *c = &cls[n_cls++];
            c->r = *g_cls[i];
            c->has_v = c->r.vals != NULL;
            if (c->has_v) c->v = *c->r.vals;
            const dvs_class_features_t *f = _info_get(c->r.cls);
            c->has_f = f != NULL;
            if (f) c->f = *f;
        }
    dvs_mutex_unlock(&g_mu);
    if (n_cls > 1) qsort(cls, n_cls, sizeof *cls, _cmp_class_time);

    Counts total;
    memset(&total, 0, sizeof total);
    /* Per motif: how many constraints, and the solver time of the classes
     * holding it (the plan's time-weighted motif histogram). */
    uint64_t motif_n[DVS_N_MOTIFS] = { 0 }, motif_ns[DVS_N_MOTIFS] = { 0 };
    for (uint32_t i = 0; i < n_cls; i++) {
        _add_counts(&total, &cls[i].r.n);
        if (!cls[i].has_f) continue;
        for (int m = 0; m < DVS_N_MOTIFS; m++) {
            motif_n[m] += cls[i].f.motifs[m];
            if (cls[i].f.motifs[m]) motif_ns[m] += cls[i].r.n.wall_ns;
        }
    }

    Out o = { buf, cap, 0 };
    if (cap) buf[0] = '\0';
    _put(&o, "{\"schema\":\"dv-solve-stats-snapshot/1\",\"compiles\":%llu",
         (unsigned long long)compiles);
    _put_counts(&o, &total);
    _put(&o, ",\"aux\":{\"calls\":%llu", (unsigned long long)aux_calls);
    _put_named(&o, "work", aux_work, DVS_STATS_N_ENGINES, k_engine, 1);
    _put(&o, "},\"motif_version\":%d,\"motifs\":{", DVS_MOTIF_VERSION);
    int first = 1;
    for (int m = 0; m < DVS_N_MOTIFS; m++) {
        if (!motif_n[m]) continue;
        _put(&o, "%s\"%s\":{\"count\":%llu,\"wall_ns\":%llu}", first ? "" : ",",
             dvs_motif_name((dvs_motif_t)m), (unsigned long long)motif_n[m],
             (unsigned long long)motif_ns[m]);
        first = 0;
    }
    _put(&o, "},\"classes_dropped\":%llu,\"classes\":[", (unsigned long long)dropped);
    for (uint32_t i = 0; i < n_cls; i++) {
        _put(&o, "%s{\"class\":\"%016llx\",\"instances\":%llu", i ? "," : "",
             (unsigned long long)cls[i].r.cls,
             (unsigned long long)(_hll_estimate(cls[i].r.hll_inst) + 0.5));
        _put_counts(&o, &cls[i].r.n);
        /* the registers themselves, so files merge (max per register) */
        _put(&o, ",\"hll\":[");
        for (uint32_t k = 0; k < HLL_REGS; k++)
            _put(&o, "%s%u", k ? "," : "", cls[i].r.hll_inst[k]);
        _put(&o, "]");
        if (cls[i].has_f) _put_features(&o, &cls[i].f);
        if (cls[i].has_v) _put_values(&o, &cls[i].v);
        _put(&o, "}");
    }
    _put(&o, "]}");
    free(cls);
    if (cap && o.len >= cap) buf[cap - 1] = '\0';
    return o.len;
}

/* ------------------------------------------------------------------ */
/* The stats file                                                      */
/* ------------------------------------------------------------------ */

/* `path` with %p replaced by the process id, into `out`. 0, or -1 if it does
 * not fit. */
static int _expand(const char *path, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = path; *p; p++) {
        char pid[24];
        const char *add = NULL;
        char one[2] = { *p, 0 };
        if (p[0] == '%' && p[1] == 'p') {
            snprintf(pid, sizeof pid, "%ld", (long)dvs__getpid());
            add = pid;
            p++;
        } else {
            add = one;
        }
        size_t k = strlen(add);
        if (n + k + 1 > cap) return -1;
        memcpy(out + n, add, k);
        n += k;
    }
    out[n] = '\0';
    return 0;
}

int dvs_stats_write(const char *path) {
    char file[1100], tmp[1130];
    if (!path || _expand(path, file, sizeof file) < 0) return -1;
    size_t n = dvs_stats_snapshot_json(NULL, 0);
    char *snap = (char *)malloc(n + 1);
    if (!snap) return -1;
    dvs_stats_snapshot_json(snap, n + 1);
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", file, (long)dvs__getpid());
    FILE *f = fopen(tmp, "w");
    int rc = -1;
    if (f) {
        int ok = fprintf(f, "{\"schema\":\"dv-solve-stats/1\",\"dv_solve\":\"%s\","
                            "\"commit\":\"%s\",\"cpus\":%u,\"snapshot\":%s}\n",
                         DVS_VERSION, DVS_GIT_COMMIT, dvs_cpu_count(), snap) > 0;
        ok = (fclose(f) == 0) && ok;
#if defined(_WIN32)
        if (ok) remove(file);
#endif
        if (ok && rename(tmp, file) == 0) rc = 0;
        else remove(tmp);
    }
    free(snap);
    return rc;
}
