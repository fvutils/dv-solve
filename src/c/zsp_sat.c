#include "zsp_sat.h"

#include <stdlib.h>
#include <string.h>

#include "kissat.h"

/* Phase B.1 step 3 (incremental stub): per-frame snapshot of the
 * clause count at push time; pop flips `tainted` if the count grew. */
#define ZSP_SAT_MAX_PUSH 64

struct zsp_sat_s {
    zsp_alloc_t  *alloc;
    kissat       *kissat;
    uint64_t      num_clauses;
    zsp_sat_var_t max_var;
    int           last_lit_was_zero;

    /* Incremental-API state. */
    uint64_t      push_num_clauses[ZSP_SAT_MAX_PUSH];
    int           push_depth;
    int           tainted;

    /* Assumptions queued for the next solve. Replayed as unit clauses.
     * Dynamic buffer routed through the same allocator as the solver. */
    zsp_sat_lit_t *assume_buf;
    size_t         assume_n;
    size_t         assume_cap;
};

static void *xalloc(zsp_alloc_t *a, size_t sz) {
    if (a) return ZSP_ALLOC(a, sz);
    return malloc(sz);
}

static void xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) { ZSP_RELEASE(a, p, sz); return; }
    free(p);
}

zsp_sat_t *zsp_sat_new(zsp_alloc_t *alloc) {
    zsp_sat_t *s = (zsp_sat_t *)xalloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    /* Phase B.1: route kissat's internal allocations through our allocator
     * when one is provided. NULL means upstream behavior (libc). */
    s->kissat = kissat_init_with_alloc(alloc);
    if (!s->kissat) {
        xfree(alloc, s, sizeof(*s));
        return NULL;
    }
    s->last_lit_was_zero = 1; /* no clause in progress */
    return s;
}

void zsp_sat_free(zsp_sat_t *s) {
    if (!s) return;
    if (s->kissat) kissat_release(s->kissat);
    if (s->assume_buf) {
        xfree(s->alloc, s->assume_buf,
              s->assume_cap * sizeof(zsp_sat_lit_t));
    }
    xfree(s->alloc, s, sizeof(*s));
}

void zsp_sat_reserve(zsp_sat_t *s, zsp_sat_var_t max_var) {
    if (max_var > 0) {
        kissat_reserve(s->kissat, (int)max_var);
        if (max_var > s->max_var) s->max_var = max_var;
    }
}

void zsp_sat_add(zsp_sat_t *s, zsp_sat_lit_t lit) {
    kissat_add(s->kissat, (int)lit);
    if (lit == 0) {
        if (!s->last_lit_was_zero) s->num_clauses++;
        s->last_lit_was_zero = 1;
    } else {
        s->last_lit_was_zero = 0;
        zsp_sat_var_t v = lit < 0 ? -lit : lit;
        if (v > s->max_var) s->max_var = v;
    }
}

void zsp_sat_add_unit(zsp_sat_t *s, zsp_sat_lit_t a) {
    zsp_sat_add(s, a);
    zsp_sat_add(s, 0);
}

void zsp_sat_add_binary(zsp_sat_t *s, zsp_sat_lit_t a, zsp_sat_lit_t b) {
    zsp_sat_add(s, a);
    zsp_sat_add(s, b);
    zsp_sat_add(s, 0);
}

void zsp_sat_add_ternary(zsp_sat_t *s, zsp_sat_lit_t a, zsp_sat_lit_t b, zsp_sat_lit_t c) {
    zsp_sat_add(s, a);
    zsp_sat_add(s, b);
    zsp_sat_add(s, c);
    zsp_sat_add(s, 0);
}

int zsp_sat_solve(zsp_sat_t *s) {
    /* Tainted instance: a popped frame had added clauses that the stub
     * cannot roll back. Caller must rebuild. */
    if (s->tainted) return ZSP_SAT_UNKNOWN;

    /* Replay queued assumptions as unit clauses. Stub semantics: these
     * become permanent in the kissat instance. */
    for (size_t i = 0; i < s->assume_n; i++) {
        zsp_sat_add(s, s->assume_buf[i]);
        zsp_sat_add(s, 0);
    }
    s->assume_n = 0;

    return kissat_solve(s->kissat);
}

zsp_sat_lit_t zsp_sat_value(zsp_sat_t *s, zsp_sat_var_t var) {
    return (zsp_sat_lit_t)kissat_value(s->kissat, (int)var);
}

void zsp_sat_set_conflict_limit(zsp_sat_t *s, uint32_t limit) {
    kissat_set_conflict_limit(s->kissat, (unsigned)limit);
}

void zsp_sat_set_decision_limit(zsp_sat_t *s, uint32_t limit) {
    kissat_set_decision_limit(s->kissat, (unsigned)limit);
}

uint64_t zsp_sat_num_clauses(const zsp_sat_t *s) {
    return s->num_clauses;
}

zsp_sat_var_t zsp_sat_max_var(const zsp_sat_t *s) {
    return s->max_var;
}

size_t zsp_sat_arena_size_bytes(zsp_sat_t *s) {
    return s && s->kissat ? kissat_arena_size_bytes(s->kissat) : 0;
}

size_t zsp_sat_arena_capacity_bytes(zsp_sat_t *s) {
    return s && s->kissat ? kissat_arena_capacity_bytes(s->kissat) : 0;
}

zsp_sat_arena_mark_t zsp_sat_arena_save_mark(zsp_sat_t *s) {
    return s && s->kissat ? kissat_arena_size_words(s->kissat) : 0;
}

void zsp_sat_push(zsp_sat_t *s) {
    if (!s) return;
    if (s->push_depth >= ZSP_SAT_MAX_PUSH) {
        s->tainted = 1;
        return;
    }
    s->push_num_clauses[s->push_depth++] = s->num_clauses;
}

void zsp_sat_pop(zsp_sat_t *s) {
    if (!s || s->push_depth == 0) return;
    uint64_t snap = s->push_num_clauses[--s->push_depth];
    /* Clauses added in the popped frame cannot be removed in stub mode. */
    if (s->num_clauses != snap) s->tainted = 1;
}

int zsp_sat_push_depth(const zsp_sat_t *s) {
    return s ? s->push_depth : 0;
}

int zsp_sat_is_tainted(const zsp_sat_t *s) {
    return s ? s->tainted : 0;
}

void zsp_sat_assume(zsp_sat_t *s, zsp_sat_lit_t lit) {
    if (!s || lit == 0) return;
    if (s->assume_n == s->assume_cap) {
        size_t new_cap = s->assume_cap ? s->assume_cap * 2 : 8;
        size_t new_bytes = new_cap * sizeof(zsp_sat_lit_t);
        size_t old_bytes = s->assume_cap * sizeof(zsp_sat_lit_t);
        zsp_sat_lit_t *nb = (zsp_sat_lit_t *)xalloc(s->alloc, new_bytes);
        if (!nb) { s->tainted = 1; return; }
        if (s->assume_buf) {
            memcpy(nb, s->assume_buf, s->assume_n * sizeof(zsp_sat_lit_t));
            xfree(s->alloc, s->assume_buf, old_bytes);
        }
        s->assume_buf = nb;
        s->assume_cap = new_cap;
    }
    s->assume_buf[s->assume_n++] = lit;
    zsp_sat_var_t v = lit < 0 ? -lit : lit;
    if (v > s->max_var) s->max_var = v;
}

int zsp_sat_failed(zsp_sat_t *s, zsp_sat_lit_t lit) {
    (void)s; (void)lit;
    /* Reserved for the real implementation that tracks unsat-core
     * membership of assumed literals. Stub returns 0. */
    return 0;
}
