#include "zsp_sat.h"

#include <stdlib.h>
#include <string.h>

#include "kissat.h"

struct zsp_sat_s {
    zsp_alloc_t  *alloc;
    kissat       *kissat;
    uint64_t      num_clauses;
    zsp_sat_var_t max_var;
    int           last_lit_was_zero;
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

void zsp_sat_set_seed(zsp_sat_t *s, uint64_t seed) {
    if (!s || !s->kissat) return;
    /* seed == 0 => leave kissat at its defaults (deterministic, lucky on). A
     * caller that wants a varied model passes a nonzero seed. */
    if (seed == 0) return;

    /* Fold the 64-bit seed into kissat's "seed" option ([0, INT_MAX]); this
     * feeds its random generator and our per-variable initial-phase
     * randomization in start_search (gated on a nonzero seed there too), so
     * keep it nonzero. */
    int ks = (int)((seed ^ (seed >> 32)) & 0x7FFFFFFF);
    if (ks == 0) ks = 1;
    kissat_set_option(s->kissat, "seed", ks);
    kissat_set_option(s->kissat, "phase", (int)(seed & 1u));
    /* "Lucky" assignments (all-true / all-false / propagation-forward) solve
     * trivial conflict-free instances before the decision loop, returning a
     * fixed boundary model that ignores our randomized phases. Disable them on
     * a seeded (stimulus-diversity) solve so the model comes from the
     * phase-seeded decisions. (UNSAT is unaffected — lucky never proves UNSAT.) */
    kissat_set_option(s->kissat, "lucky", 0);
}

uint64_t zsp_sat_num_clauses(const zsp_sat_t *s) {
    return s->num_clauses;
}

zsp_sat_var_t zsp_sat_max_var(const zsp_sat_t *s) {
    return s->max_var;
}
