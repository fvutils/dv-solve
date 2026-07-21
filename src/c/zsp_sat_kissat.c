/*
 * kissat backend for the zsp_sat seam (see zsp_sat_backend.h).
 *
 * kissat is non-incremental by design. This backend keeps the historical
 * "Phase B.0" semantics: assumptions are replayed as permanent unit clauses at
 * solve time, and there is no unsat-core (failed() is handled by the
 * dispatcher's stub, so it is not part of this vtable's contract beyond
 * returning 0). It is pure C and is the lightweight/embeddable default.
 */

#include <stdlib.h>

#include "zsp_sat_backend.h"
#include "kissat.h"

typedef struct {
    zsp_alloc_t *alloc;
    kissat      *kissat;
} kissat_impl_t;

static void *ki_xalloc(zsp_alloc_t *a, size_t sz) {
    if (a) return ZSP_ALLOC(a, sz);
    return malloc(sz);
}
static void ki_xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) { ZSP_RELEASE(a, p, sz); return; }
    free(p);
}

static void ki_destroy(void *impl, zsp_alloc_t *alloc) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    if (!k) return;
    if (k->kissat) kissat_release(k->kissat);
    ki_xfree(alloc, k, sizeof(*k));
}

static void ki_reserve(void *impl, zsp_sat_var_t max_var) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    if (max_var > 0) kissat_reserve(k->kissat, (int)max_var);
}

static void ki_add(void *impl, zsp_sat_lit_t lit) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    kissat_add(k->kissat, (int)lit);
}

static int ki_solve(void *impl, const zsp_sat_lit_t *assumps, size_t n) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    /* Stub incremental semantics: assumptions become permanent unit clauses. */
    for (size_t i = 0; i < n; i++) {
        kissat_add(k->kissat, (int)assumps[i]);
        kissat_add(k->kissat, 0);
    }
    return kissat_solve(k->kissat);
}

static zsp_sat_lit_t ki_value(void *impl, zsp_sat_var_t var) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    return (zsp_sat_lit_t)kissat_value(k->kissat, (int)var);
}

static int ki_failed(void *impl, zsp_sat_lit_t lit) {
    (void)impl; (void)lit;
    return 0; /* kissat has no unsat-core; caller sees the dispatcher stub. */
}

static void ki_set_seed(void *impl, uint64_t seed) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    if (!k->kissat) return;
    if (seed == 0) return; /* defaults: deterministic, lucky on */
    /* Fold the 64-bit seed into kissat's "seed" option ([0, INT_MAX]); feeds
     * its RNG and per-variable initial-phase randomization in start_search. */
    int ks = (int)((seed ^ (seed >> 32)) & 0x7FFFFFFF);
    if (ks == 0) ks = 1;
    kissat_set_option(k->kissat, "seed", ks);
    kissat_set_option(k->kissat, "phase", (int)(seed & 1u));
    /* Disable "lucky" boundary assignments on a seeded diversity solve so the
     * model comes from the phase-seeded decisions. UNSAT is unaffected. */
    kissat_set_option(k->kissat, "lucky", 0);
}

static void ki_set_terminate(void *impl, void *state, int (*fn)(void *)) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    if (k && k->kissat) kissat_set_terminate(k->kissat, state, fn);
}

/* Thread-safe async abort: raise kissat's termination flag. kissat_search()
 * polls solver->termination.flagged and unwinds with an UNKNOWN result. This
 * build does NOT poll the set_terminate callback, so this explicit poke is the
 * only way to interrupt a running kissat solve from a peer thread. */
static void ki_interrupt(void *impl) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    if (k && k->kissat) kissat_terminate(k->kissat);
}

static void ki_set_conflict_limit(void *impl, uint32_t limit) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    kissat_set_conflict_limit(k->kissat, (unsigned)limit);
}
static void ki_set_decision_limit(void *impl, uint32_t limit) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    kissat_set_decision_limit(k->kissat, (unsigned)limit);
}

static size_t ki_arena_size_bytes(void *impl) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    return k && k->kissat ? kissat_arena_size_bytes(k->kissat) : 0;
}
static size_t ki_arena_capacity_bytes(void *impl) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    return k && k->kissat ? kissat_arena_capacity_bytes(k->kissat) : 0;
}
static size_t ki_arena_save_mark(void *impl) {
    kissat_impl_t *k = (kissat_impl_t *)impl;
    return k && k->kissat ? kissat_arena_size_words(k->kissat) : 0;
}

static const zsp_sat_vtbl KISSAT_VTBL = {
    .incremental        = 0,   /* one-shot: dispatcher uses rebuild-on-taint */
    .destroy            = ki_destroy,
    .reserve            = ki_reserve,
    .add                = ki_add,
    .solve              = ki_solve,
    .value              = ki_value,
    .failed             = ki_failed,
    .set_seed           = ki_set_seed,
    .set_conflict_limit = ki_set_conflict_limit,
    .set_decision_limit = ki_set_decision_limit,
    .set_terminate      = ki_set_terminate,
    .interrupt          = ki_interrupt,
    .arena_size_bytes     = ki_arena_size_bytes,
    .arena_capacity_bytes = ki_arena_capacity_bytes,
    .arena_save_mark      = ki_arena_save_mark,
};

void *zsp_sat_kissat_create(zsp_alloc_t *alloc, const zsp_sat_vtbl **vt_out) {
    kissat_impl_t *k = (kissat_impl_t *)ki_xalloc(alloc, sizeof(*k));
    if (!k) return NULL;
    k->alloc = alloc;
    /* Route kissat's internal allocations through our allocator when provided. */
    k->kissat = kissat_init_with_alloc(alloc);
    if (!k->kissat) {
        ki_xfree(alloc, k, sizeof(*k));
        return NULL;
    }
    /* Disable kissat's SAT-sweeping (kitten-based equivalence sweeping).
     * Profiling (docs/perf_sweep_easy_band_2026-07-21.md) showed `kitten_solve`
     * + sweep_refine consuming 30-50% of solve time on small/medium BMC and CSP
     * instances — where a plain CDCL search cracks them in <1ms — for a 3-13x
     * SAT-time win (2.7x aggregate on the SAT-bound easy band), 0 correctness
     * changes. It is NEUTRAL on the hard corpus we win on (vlsat3): those wins
     * come from `factor`/`congruence`, which stay ON. Opt back in with
     * DV_KISSAT_SWEEP=1 for A/B or if a future hard instance needs it. */
    if (!getenv("DV_KISSAT_SWEEP")) {
        kissat_set_option(k->kissat, "sweep", 0);
    }
    *vt_out = &KISSAT_VTBL;
    return k;
}
