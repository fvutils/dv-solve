/*
 * dvs_sat dispatcher.
 *
 * Public entry points (dvs_sat.h) forward to a backend vtable
 * (dvs_sat_backend.h). This file owns the backend-independent bookkeeping:
 * clause/variable counters, the queued-assumption buffer, and the push/pop
 * taint state. Backends (kissat, CaDiCaL) are thin and live in their own TUs.
 */

#include "dvs_sat.h"
#include "dvs_sat_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DVS_SAT_MAX_PUSH 64

/* Selector variables for incremental push/pop. A clause added inside frame f
 * gets `(C | ~sel_f)`; solving assumes every open frame's sel true (clause
 * active); pop permanently asserts `~sel_f` (clause satisfied → disabled).
 *
 * Selector ids are allocated DENSELY, just above the highest variable seen so
 * far — SAT solvers size their tables by max variable id, so a sparse/high
 * base would blow up memory. The caller contract is therefore: do not
 * introduce a NEW user variable id above the current max while a frame is open
 * (it could alias a live selector). Bit-blast var ids are contiguous, so
 * allocating a selector above the running max at push time is safe; the
 * eventual bit-blast integration coordinates id allocation explicitly. */

struct dvs_sat_s {
    const dvs_sat_vtbl *vt;
    void               *impl;
    dvs_alloc_t        *alloc;
    dvs_sat_backend_t   backend;
    int                 incremental;   /* backend supports selector push/pop */

    uint64_t            num_clauses;
    dvs_sat_var_t       max_var;
    int                 last_lit_was_zero;

    /* Incremental-API state. */
    int                 push_depth;
    int                 tainted;
    /* Non-incremental (kissat) path: clause-count snapshot per frame; pop
     * taints if the frame added clauses. */
    uint64_t            push_num_clauses[DVS_SAT_MAX_PUSH];
    /* Incremental (selector) path: the selector var id assigned to each open
     * frame, and the next free selector id. */
    uint32_t            sel_stack[DVS_SAT_MAX_PUSH];
    uint32_t            sel_next;

    /* Assumptions queued for the next solve. Applied by the backend's solve(). */
    dvs_sat_lit_t      *assume_buf;
    size_t              assume_n;
    size_t              assume_cap;

    /* Clause-DB recording (opt-in). When `recording`, every literal handed to
     * dvs_sat_add (0 terminators included) is appended to rec_buf, an exact
     * replay log used to clone the instance into per-worker cube solvers. */
    int                 recording;
    dvs_sat_lit_t      *rec_buf;
    size_t              rec_n;
    size_t              rec_cap;

    /* Lazy instance (dvs_sat_new_lazy): impl stays NULL until
     * dvs_sat_materialize. Meanwhile lz_ops logs the calls bound for the
     * backend, in order; an LZ_ADD op stands for its next `v` literals of
     * lz_lits. lz_failed: the log or the backend could not be made. */
    int                 lz_failed;
    /* dvs_sat_set_prepare: run once, just before the backend is made. */
    void              (*prep_fn)(void *);
    void               *prep_ctx;
    struct lz_op { uint32_t op; uint64_t v; } *lz_ops;
    size_t              lz_nops, lz_capops;
    dvs_sat_lit_t      *lz_lits;
    size_t              lz_nlits, lz_caplits;
};

enum { LZ_ADD, LZ_RESERVE, LZ_SEED, LZ_CONFLICTS, LZ_DECISIONS, LZ_LIGHT,
       LZ_SAMPLING, LZ_PHASE };

/* Append a literal to the assumption buffer without touching var bookkeeping
 * (used for both user assumptions and internal selector assumptions). Returns
 * 0 on success, -1 on allocation failure. */
static int assume_append_raw(dvs_sat_t *s, dvs_sat_lit_t lit);

static void *xalloc(dvs_alloc_t *a, size_t sz) {
    if (a) return DVS_ALLOC(a, sz);
    return malloc(sz);
}

static void xfree(dvs_alloc_t *a, void *p, size_t sz) {
    if (a) { DVS_RELEASE(a, p, sz); return; }
    free(p);
}

int dvs_sat_has_cadical(void) {
#ifdef DVS_WITH_CADICAL
    return 1;
#else
    return 0;
#endif
}

static dvs_sat_t *sat_alloc(dvs_alloc_t *alloc) {
    dvs_sat_t *s = (dvs_sat_t *)xalloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->last_lit_was_zero = 1; /* no clause in progress */
    s->sel_next = 0;          /* set to max_var+1 lazily at first push */
    /* DV_DUMP_CNF=path: record the clause log and write it as DIMACS at the
     * first solve (a diagnostic for comparing SAT backends offline). */
    if (getenv("DV_DUMP_CNF")) s->recording = 1;
    return s;
}

/* Create the backend impl into s: `backend`, or kissat when that is not
 * compiled in (or, when `fallback`, fails). Returns 0 on success. */
static int sat_create_impl(dvs_sat_t *s, dvs_sat_backend_t backend, int fallback) {
    void *impl = NULL;
    const dvs_sat_vtbl *vt = NULL;
    dvs_sat_backend_t got = DVS_SAT_BACKEND_KISSAT;
#ifdef DVS_WITH_CADICAL
    if (backend == DVS_SAT_BACKEND_CADICAL) {
        impl = dvs_sat_cadical_create(s->alloc, &vt);
        got = DVS_SAT_BACKEND_CADICAL;
        if (!impl && !fallback) return -1;
    }
#else
    (void)backend; (void)fallback;
#endif
    if (!impl) {
        /* Default, or fallback when the requested backend is not compiled in. */
        impl = dvs_sat_kissat_create(s->alloc, &vt);
        got = DVS_SAT_BACKEND_KISSAT;
    }
    if (!impl) return -1;
    s->impl        = impl;
    s->vt          = vt;
    s->backend     = got;
    s->incremental = vt->incremental;
    return 0;
}

dvs_sat_t *dvs_sat_new_backend(dvs_alloc_t *alloc, dvs_sat_backend_t backend) {
    dvs_sat_t *s = sat_alloc(alloc);
    if (!s) return NULL;
    if (sat_create_impl(s, backend, 1) != 0) {
        xfree(alloc, s, sizeof(*s));
        return NULL;
    }
    return s;
}

dvs_sat_t *dvs_sat_new_lazy(dvs_alloc_t *alloc, dvs_sat_backend_t backend) {
    dvs_sat_t *s = sat_alloc(alloc);
    if (!s) return NULL;
    /* Fix the backend now: the dispatcher's push/pop scheme depends on it. */
    s->backend = backend == DVS_SAT_BACKEND_CADICAL && dvs_sat_has_cadical()
               ? DVS_SAT_BACKEND_CADICAL : DVS_SAT_BACKEND_KISSAT;
    s->incremental = s->backend == DVS_SAT_BACKEND_CADICAL;
    return s;
}

/* Grow a log array to hold one more element; 0 on success. */
static int lz_grow(dvs_sat_t *s, void **buf, size_t *cap, size_t n, size_t elem) {
    if (n < *cap) return 0;
    size_t nc = *cap ? *cap * 2 : 64;
    void *nb = xalloc(s->alloc, nc * elem);
    if (!nb) { s->lz_failed = 1; return -1; }
    if (*buf) {
        memcpy(nb, *buf, n * elem);
        xfree(s->alloc, *buf, *cap * elem);
    }
    *buf = nb;
    *cap = nc;
    return 0;
}

static void lz_log(dvs_sat_t *s, uint32_t op, uint64_t v) {
    if (s->lz_failed) return;
    if (s->lz_nops >= s->lz_capops
            && lz_grow(s, (void **)&s->lz_ops, &s->lz_capops, s->lz_nops, sizeof(*s->lz_ops)))
        return;
    s->lz_ops[s->lz_nops].op = op;
    s->lz_ops[s->lz_nops].v = v;
    s->lz_nops++;
}

/* A literal (0 ends the clause) for the backend's add(). */
static void vt_add(dvs_sat_t *s, dvs_sat_lit_t lit) {
    if (s->impl) { s->vt->add(s->impl, lit); return; }
    if (s->lz_failed) return;
    if (s->lz_nlits >= s->lz_caplits
            && lz_grow(s, (void **)&s->lz_lits, &s->lz_caplits, s->lz_nlits, sizeof(*s->lz_lits)))
        return;
    s->lz_lits[s->lz_nlits++] = lit;
    if (s->lz_nops && s->lz_ops[s->lz_nops - 1].op == LZ_ADD) s->lz_ops[s->lz_nops - 1].v++;
    else lz_log(s, LZ_ADD, 1);
}

static void lz_release(dvs_sat_t *s) {
    if (s->lz_ops) xfree(s->alloc, s->lz_ops, s->lz_capops * sizeof(*s->lz_ops));
    if (s->lz_lits) xfree(s->alloc, s->lz_lits, s->lz_caplits * sizeof(*s->lz_lits));
    s->lz_ops = NULL;
    s->lz_lits = NULL;
    s->lz_nops = s->lz_capops = s->lz_nlits = s->lz_caplits = 0;
}

int dvs_sat_materialize(dvs_sat_t *s) {
    if (!s) return -1;
    if (s->impl) return 0;
    if (s->prep_fn) {
        void (*fn)(void *) = s->prep_fn;
        s->prep_fn = NULL;
        fn(s->prep_ctx);
    }
    if (s->lz_failed || sat_create_impl(s, s->backend, 0) != 0) {
        s->lz_failed = 1;
        lz_release(s);
        return -1;
    }
    const dvs_sat_vtbl *vt = s->vt;
    size_t li = 0;
    for (size_t i = 0; i < s->lz_nops; i++) {
        uint64_t v = s->lz_ops[i].v;
        switch (s->lz_ops[i].op) {
        case LZ_ADD:
            for (uint64_t k = 0; k < v; k++) vt->add(s->impl, s->lz_lits[li++]);
            break;
        case LZ_RESERVE:   vt->reserve(s->impl, (dvs_sat_var_t)v); break;
        case LZ_SEED:      vt->set_seed(s->impl, v); break;
        case LZ_CONFLICTS: vt->set_conflict_limit(s->impl, (uint32_t)v); break;
        case LZ_DECISIONS: vt->set_decision_limit(s->impl, (uint32_t)v); break;
        case LZ_LIGHT:
            if (vt->set_light_search) vt->set_light_search(s->impl, (int)v);
            break;
        case LZ_SAMPLING:
            if (vt->set_sampling) vt->set_sampling(s->impl, (int)v);
            break;
        case LZ_PHASE:
            if (vt->phase) vt->phase(s->impl, (dvs_sat_lit_t)(int64_t)v);
            break;
        }
    }
    lz_release(s);
    return 0;
}

void dvs_sat_set_prepare(dvs_sat_t *s, void (*fn)(void *), void *ctx) {
    if (!s) return;
    s->prep_fn = fn;
    s->prep_ctx = ctx;
}

int dvs_sat_is_lazy(const dvs_sat_t *s) {
    return s && !s->impl && !s->lz_failed;
}

dvs_sat_t *dvs_sat_new(dvs_alloc_t *alloc) {
    return dvs_sat_new_backend(alloc, DVS_SAT_BACKEND_KISSAT);
}

dvs_sat_backend_t dvs_sat_backend(const dvs_sat_t *s) {
    return s ? s->backend : DVS_SAT_BACKEND_KISSAT;
}

int dvs_sat_is_incremental(const dvs_sat_t *s) {
    return s ? s->incremental : 0;
}

void dvs_sat_free(dvs_sat_t *s) {
    if (!s) return;
    if (s->vt && s->impl) s->vt->destroy(s->impl, s->alloc);
    lz_release(s);
    if (s->assume_buf) {
        xfree(s->alloc, s->assume_buf, s->assume_cap * sizeof(dvs_sat_lit_t));
    }
    if (s->rec_buf) {
        xfree(s->alloc, s->rec_buf, s->rec_cap * sizeof(dvs_sat_lit_t));
    }
    xfree(s->alloc, s, sizeof(*s));
}

void dvs_sat_reserve(dvs_sat_t *s, dvs_sat_var_t max_var) {
    if (max_var > 0) {
        if (s->impl) s->vt->reserve(s->impl, max_var);
        else lz_log(s, LZ_RESERVE, (uint64_t)max_var);
        if (max_var > s->max_var) s->max_var = max_var;
    }
}

/* Append one literal to the recording log (grows geometrically). On OOM we
 * silently disable recording — the caller detects the short/empty log via
 * dvs_sat_recorded and falls back to a non-parallel solve, never a wrong answer. */
static void rec_append(dvs_sat_t *s, dvs_sat_lit_t lit) {
    if (s->rec_n == s->rec_cap) {
        size_t nc = s->rec_cap ? s->rec_cap * 2 : 4096;
        size_t old_bytes = s->rec_cap * sizeof(dvs_sat_lit_t);
        dvs_sat_lit_t *nb = (dvs_sat_lit_t *)xalloc(s->alloc, nc * sizeof(dvs_sat_lit_t));
        if (!nb) { s->recording = 0; return; }
        if (s->rec_buf) {
            memcpy(nb, s->rec_buf, s->rec_n * sizeof(dvs_sat_lit_t));
            xfree(s->alloc, s->rec_buf, old_bytes);
        }
        s->rec_buf = nb;
        s->rec_cap = nc;
    }
    s->rec_buf[s->rec_n++] = lit;
}

void dvs_sat_record_start(dvs_sat_t *s) {
    if (s) s->recording = 1;
}

const dvs_sat_lit_t *dvs_sat_recorded(const dvs_sat_t *s, size_t *n) {
    if (n) *n = s ? s->rec_n : 0;
    return s ? s->rec_buf : NULL;
}

void dvs_sat_add(dvs_sat_t *s, dvs_sat_lit_t lit) {
    if (s->recording) rec_append(s, lit);
    if (lit == 0) {
        /* Closing a non-empty clause inside an open frame: tag it with the
         * innermost frame's selector, `(C | ~sel)`, so pop can disable it.
         * Empty clauses (last_lit_was_zero) pass through untagged so they keep
         * their UNSAT meaning. */
        if (!s->last_lit_was_zero) {
            if (s->incremental && s->push_depth > 0) {
                vt_add(s, -(dvs_sat_lit_t)s->sel_stack[s->push_depth - 1]);
            }
            s->num_clauses++;
        }
        vt_add(s, 0);
        s->last_lit_was_zero = 1;
    } else {
        vt_add(s, lit);
        s->last_lit_was_zero = 0;
        dvs_sat_var_t v = lit < 0 ? -lit : lit;
        if (v > s->max_var) s->max_var = v;
    }
}

void dvs_sat_add_unit(dvs_sat_t *s, dvs_sat_lit_t a) {
    dvs_sat_add(s, a);
    dvs_sat_add(s, 0);
}

void dvs_sat_add_binary(dvs_sat_t *s, dvs_sat_lit_t a, dvs_sat_lit_t b) {
    dvs_sat_add(s, a);
    dvs_sat_add(s, b);
    dvs_sat_add(s, 0);
}

void dvs_sat_add_ternary(dvs_sat_t *s, dvs_sat_lit_t a, dvs_sat_lit_t b, dvs_sat_lit_t c) {
    dvs_sat_add(s, a);
    dvs_sat_add(s, b);
    dvs_sat_add(s, c);
    dvs_sat_add(s, 0);
}

int dvs_sat_solve(dvs_sat_t *s) {
    /* Tainted instance: a popped frame added clauses the stub cannot roll
     * back. Caller must rebuild. */
    if (s->tainted) return DVS_SAT_UNKNOWN;
    if (dvs_sat_materialize(s) != 0) { s->assume_n = 0; return DVS_SAT_UNKNOWN; }

    /* Incremental path: assume every open frame's selector true so that
     * frame's clauses `(C | ~sel)` are active for this solve. Appended after
     * any user assumptions; both are cleared below. */
    if (s->incremental) {
        for (int i = 0; i < s->push_depth; i++) {
            if (assume_append_raw(s, (dvs_sat_lit_t)s->sel_stack[i]) != 0) {
                return DVS_SAT_UNKNOWN; /* OOM: fail safe */
            }
        }
    }

    const char *dump = getenv("DV_DUMP_CNF");
    if (dump && s->recording && s->rec_n) {
        FILE *f = fopen(dump, "w");
        if (f) {
            fprintf(f, "p cnf %u %llu\n", (unsigned)s->max_var,
                    (unsigned long long)(s->num_clauses + s->assume_n));
            int open = 0;
            for (size_t i = 0; i < s->rec_n; i++) {
                if (s->rec_buf[i] == 0 && !open) continue;
                fprintf(f, s->rec_buf[i] ? "%d " : "%d\n", (int)s->rec_buf[i]);
                open = s->rec_buf[i] != 0;
            }
            for (size_t i = 0; i < s->assume_n; i++)
                fprintf(f, "%d 0\n", (int)s->assume_buf[i]);
            fclose(f);
        }
    }

    int rc = s->vt->solve(s->impl, s->assume_buf, s->assume_n);
    s->assume_n = 0; /* user + selector assumptions consumed by this solve */
    return rc;
}

dvs_sat_lit_t dvs_sat_value(dvs_sat_t *s, dvs_sat_var_t var) {
    if (dvs_sat_materialize(s) != 0) return 0;
    return s->vt->value(s->impl, var);
}

void dvs_sat_set_conflict_limit(dvs_sat_t *s, uint32_t limit) {
    if (s->impl) s->vt->set_conflict_limit(s->impl, limit);
    else lz_log(s, LZ_CONFLICTS, limit);
}

void dvs_sat_set_decision_limit(dvs_sat_t *s, uint32_t limit) {
    if (s->impl) s->vt->set_decision_limit(s->impl, limit);
    else lz_log(s, LZ_DECISIONS, limit);
}

void dvs_sat_set_light_search(dvs_sat_t *s, int on) {
    if (!s) return;
    if (!s->impl) lz_log(s, LZ_LIGHT, (uint64_t)on);
    else if (s->vt->set_light_search) s->vt->set_light_search(s->impl, on);
}

void dvs_sat_phase(dvs_sat_t *s, dvs_sat_lit_t lit) {
    if (!s) return;
    if (!s->impl) lz_log(s, LZ_PHASE, (uint64_t)(int64_t)lit);
    else if (s->vt->phase) s->vt->phase(s->impl, lit);
}

void dvs_sat_set_sampling(dvs_sat_t *s, int on) {
    if (!s) return;
    if (!s->impl) lz_log(s, LZ_SAMPLING, (uint64_t)on);
    else if (s->vt->set_sampling) s->vt->set_sampling(s->impl, on);
}

void dvs_sat_set_seed(dvs_sat_t *s, uint64_t seed) {
    if (!s) return;
    if (!s->impl) lz_log(s, LZ_SEED, seed);
    else s->vt->set_seed(s->impl, seed);
}

void dvs_sat_set_terminate(dvs_sat_t *s, void *state, int (*fn)(void *)) {
    if (!s || dvs_sat_materialize(s) != 0 || !s->vt->set_terminate) return;
    s->vt->set_terminate(s->impl, state, fn);
}

void dvs_sat_interrupt(dvs_sat_t *s) {
    if (!s || !s->vt || !s->vt->interrupt) return;
    s->vt->interrupt(s->impl);
}

uint64_t dvs_sat_work(const dvs_sat_t *s) {
    return s && s->impl && s->vt->work ? s->vt->work(s->impl) : 0;
}

uint64_t dvs_sat_num_clauses(const dvs_sat_t *s) {
    return s->num_clauses;
}

dvs_sat_var_t dvs_sat_max_var(const dvs_sat_t *s) {
    return s->max_var;
}

size_t dvs_sat_arena_size_bytes(dvs_sat_t *s) {
    return s && dvs_sat_materialize(s) == 0 ? s->vt->arena_size_bytes(s->impl) : 0;
}

size_t dvs_sat_arena_capacity_bytes(dvs_sat_t *s) {
    return s && dvs_sat_materialize(s) == 0 ? s->vt->arena_capacity_bytes(s->impl) : 0;
}

dvs_sat_arena_mark_t dvs_sat_arena_save_mark(dvs_sat_t *s) {
    return s && dvs_sat_materialize(s) == 0 ? s->vt->arena_save_mark(s->impl) : 0;
}

void dvs_sat_push(dvs_sat_t *s) {
    if (!s) return;
    if (s->push_depth >= DVS_SAT_MAX_PUSH) {
        s->tainted = 1;
        return;
    }
    if (s->incremental) {
        /* Assign a fresh selector for this frame, dense just above the current
         * max var. Never reused, so a push/pop/push sequence cannot reactivate
         * stale clauses. */
        if (s->sel_next <= (uint32_t)s->max_var) {
            s->sel_next = (uint32_t)s->max_var + 1;
        }
        s->sel_stack[s->push_depth++] = s->sel_next++;
    } else {
        s->push_num_clauses[s->push_depth++] = s->num_clauses;
    }
}

void dvs_sat_pop(dvs_sat_t *s) {
    if (!s || s->push_depth == 0) return;
    if (s->incremental) {
        /* Permanently falsify the frame's selector: `(~sel)` unit forces
         * every `(C | ~sel)` clause of this frame to true → disabled. */
        uint32_t sel = s->sel_stack[--s->push_depth];
        vt_add(s, -(dvs_sat_lit_t)sel);
        vt_add(s, 0);
    } else {
        uint64_t snap = s->push_num_clauses[--s->push_depth];
        /* Clauses added in the popped frame cannot be removed in stub mode. */
        if (s->num_clauses != snap) s->tainted = 1;
    }
}

int dvs_sat_push_depth(const dvs_sat_t *s) {
    return s ? s->push_depth : 0;
}

int dvs_sat_is_tainted(const dvs_sat_t *s) {
    return s ? s->tainted : 0;
}

static int assume_append_raw(dvs_sat_t *s, dvs_sat_lit_t lit) {
    if (s->assume_n == s->assume_cap) {
        size_t new_cap = s->assume_cap ? s->assume_cap * 2 : 8;
        size_t new_bytes = new_cap * sizeof(dvs_sat_lit_t);
        size_t old_bytes = s->assume_cap * sizeof(dvs_sat_lit_t);
        dvs_sat_lit_t *nb = (dvs_sat_lit_t *)xalloc(s->alloc, new_bytes);
        if (!nb) { s->tainted = 1; return -1; }
        if (s->assume_buf) {
            memcpy(nb, s->assume_buf, s->assume_n * sizeof(dvs_sat_lit_t));
            xfree(s->alloc, s->assume_buf, old_bytes);
        }
        s->assume_buf = nb;
        s->assume_cap = new_cap;
    }
    s->assume_buf[s->assume_n++] = lit;
    return 0;
}

void dvs_sat_assume(dvs_sat_t *s, dvs_sat_lit_t lit) {
    if (!s || lit == 0) return;
    if (assume_append_raw(s, lit) != 0) return;
    dvs_sat_var_t v = lit < 0 ? -lit : lit;
    if (v > s->max_var) s->max_var = v;
}

int dvs_sat_failed(dvs_sat_t *s, dvs_sat_lit_t lit) {
    if (!s || dvs_sat_materialize(s) != 0) return 0;
    return s->vt->failed(s->impl, lit);
}
