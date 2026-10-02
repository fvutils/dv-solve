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
};

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

dvs_sat_t *dvs_sat_new_backend(dvs_alloc_t *alloc, dvs_sat_backend_t backend) {
    dvs_sat_t *s = (dvs_sat_t *)xalloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->last_lit_was_zero = 1; /* no clause in progress */

    void *impl = NULL;
    const dvs_sat_vtbl *vt = NULL;

#ifdef DVS_WITH_CADICAL
    if (backend == DVS_SAT_BACKEND_CADICAL) {
        impl = dvs_sat_cadical_create(alloc, &vt);
        s->backend = DVS_SAT_BACKEND_CADICAL;
    }
#endif
    if (!impl) {
        /* Default, or fallback when the requested backend is not compiled in. */
        impl = dvs_sat_kissat_create(alloc, &vt);
        s->backend = DVS_SAT_BACKEND_KISSAT;
    }
    if (!impl) {
        xfree(alloc, s, sizeof(*s));
        return NULL;
    }
    s->impl        = impl;
    s->vt          = vt;
    s->incremental = vt->incremental;
    s->sel_next    = 0; /* set to max_var+1 lazily at first push */
    return s;
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
        s->vt->reserve(s->impl, max_var);
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
                s->vt->add(s->impl, -(dvs_sat_lit_t)s->sel_stack[s->push_depth - 1]);
            }
            s->num_clauses++;
        }
        s->vt->add(s->impl, 0);
        s->last_lit_was_zero = 1;
    } else {
        s->vt->add(s->impl, lit);
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

    int rc = s->vt->solve(s->impl, s->assume_buf, s->assume_n);
    s->assume_n = 0; /* user + selector assumptions consumed by this solve */
    return rc;
}

dvs_sat_lit_t dvs_sat_value(dvs_sat_t *s, dvs_sat_var_t var) {
    return s->vt->value(s->impl, var);
}

void dvs_sat_set_conflict_limit(dvs_sat_t *s, uint32_t limit) {
    s->vt->set_conflict_limit(s->impl, limit);
}

void dvs_sat_set_decision_limit(dvs_sat_t *s, uint32_t limit) {
    s->vt->set_decision_limit(s->impl, limit);
}

void dvs_sat_set_light_search(dvs_sat_t *s, int on) {
    if (s && s->vt->set_light_search) s->vt->set_light_search(s->impl, on);
}

void dvs_sat_set_seed(dvs_sat_t *s, uint64_t seed) {
    if (!s) return;
    s->vt->set_seed(s->impl, seed);
}

void dvs_sat_set_terminate(dvs_sat_t *s, void *state, int (*fn)(void *)) {
    if (!s || !s->vt || !s->vt->set_terminate) return;
    s->vt->set_terminate(s->impl, state, fn);
}

void dvs_sat_interrupt(dvs_sat_t *s) {
    if (!s || !s->vt || !s->vt->interrupt) return;
    s->vt->interrupt(s->impl);
}

uint64_t dvs_sat_num_clauses(const dvs_sat_t *s) {
    return s->num_clauses;
}

dvs_sat_var_t dvs_sat_max_var(const dvs_sat_t *s) {
    return s->max_var;
}

size_t dvs_sat_arena_size_bytes(dvs_sat_t *s) {
    return s ? s->vt->arena_size_bytes(s->impl) : 0;
}

size_t dvs_sat_arena_capacity_bytes(dvs_sat_t *s) {
    return s ? s->vt->arena_capacity_bytes(s->impl) : 0;
}

dvs_sat_arena_mark_t dvs_sat_arena_save_mark(dvs_sat_t *s) {
    return s ? s->vt->arena_save_mark(s->impl) : 0;
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
        s->vt->add(s->impl, -(dvs_sat_lit_t)sel);
        s->vt->add(s->impl, 0);
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
    if (!s) return 0;
    return s->vt->failed(s->impl, lit);
}
