/*
 * CaDiCaL backend for the zsp_sat seam (see zsp_sat_backend.h).
 *
 * Compiled only when ZSP_WITH_CADICAL is defined. Uses CaDiCaL's C API
 * (ccadical.h) so this translation unit stays C; the C++ dependency lives in
 * the linked static archive. Unlike the kissat backend, assumptions are real
 * per-solve IPASIR assumptions (cleared after each solve) and failed() reports
 * genuine unsat-core membership.
 *
 * NOTE: CaDiCaL manages its own memory; the ccadical API has no allocator
 * injection, so `alloc` is used only for this backend's own bookkeeping struct.
 * This is an intentional divergence from kissat's pool routing — CaDiCaL is not
 * part of the lightweight/embeddable build where that discipline matters.
 */

#include <stdlib.h>

#include "zsp_sat_backend.h"
#include "ccadical.h"

typedef struct {
    zsp_alloc_t *alloc;
    CCaDiCaL    *solver;
} cadical_impl_t;

static void *cd_xalloc(zsp_alloc_t *a, size_t sz) {
    if (a) return ZSP_ALLOC(a, sz);
    return malloc(sz);
}
static void cd_xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) { ZSP_RELEASE(a, p, sz); return; }
    free(p);
}

static void cd_destroy(void *impl, zsp_alloc_t *alloc) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    if (!c) return;
    if (c->solver) ccadical_release(c->solver);
    cd_xfree(alloc, c, sizeof(*c));
}

static void cd_reserve(void *impl, zsp_sat_var_t max_var) {
    /* CaDiCaL grows its variable table automatically on add(); reserve is an
     * optional hint. Declare up-front to avoid incremental regrowth. */
    cadical_impl_t *c = (cadical_impl_t *)impl;
    if (max_var > 0) ccadical_declare_more_variables(c->solver, (int)max_var);
}

static void cd_add(void *impl, zsp_sat_lit_t lit) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    ccadical_add(c->solver, (int)lit);
}

static int cd_solve(void *impl, const zsp_sat_lit_t *assumps, size_t n) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    /* Real IPASIR assumptions: valid for this solve only, cleared afterward. */
    for (size_t i = 0; i < n; i++) {
        ccadical_assume(c->solver, (int)assumps[i]);
    }
    return ccadical_solve(c->solver); /* 10 SAT / 20 UNSAT / 0 UNKNOWN */
}

static zsp_sat_lit_t cd_value(void *impl, zsp_sat_var_t var) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    /* ccadical_val(lit) returns +lit if true, -lit if false. */
    return (zsp_sat_lit_t)ccadical_val(c->solver, (int)var);
}

static int cd_failed(void *impl, zsp_sat_lit_t lit) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    return ccadical_failed(c->solver, (int)lit) ? 1 : 0;
}

static void cd_set_seed(void *impl, uint64_t seed) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    if (!c->solver || seed == 0) return;
    int cs = (int)((seed ^ (seed >> 32)) & 0x7FFFFFFF);
    if (cs == 0) cs = 1;
    ccadical_set_option(c->solver, "seed", cs);
    /* CaDiCaL has no kissat-style "lucky"; the initial decision phase is the
     * available diversity knob. Contract is per-seed determinism only, so the
     * exact distribution may differ from kissat (documented). */
    ccadical_set_option(c->solver, "phase", (int)(seed & 1u));
}

static void cd_set_conflict_limit(void *impl, uint32_t limit) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    /* wrapper contract: 0 == unlimited; CaDiCaL uses -1 for no limit. */
    ccadical_limit(c->solver, "conflicts", limit == 0 ? -1 : (int)limit);
}
static void cd_set_decision_limit(void *impl, uint32_t limit) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    ccadical_limit(c->solver, "decisions", limit == 0 ? -1 : (int)limit);
}

static void cd_set_terminate(void *impl, void *state, int (*fn)(void *)) {
    cadical_impl_t *c = (cadical_impl_t *)impl;
    if (c && c->solver) ccadical_set_terminate(c->solver, state, fn);
}

/* CaDiCaL polls the set_terminate callback during solve, so a peer merely
 * raising the shared stop flag already aborts it — no separate interrupt call
 * is needed (and ccadical exposes no synchronous terminate). No-op. */
static void cd_interrupt(void *impl) { (void)impl; }

/* CaDiCaL exposes no kissat-style clause-arena words; telemetry returns 0. */
static size_t cd_arena_zero(void *impl) { (void)impl; return 0; }

static const zsp_sat_vtbl CADICAL_VTBL = {
    .incremental        = 1,   /* real assumptions + post-solve clause add */
    .destroy            = cd_destroy,
    .reserve            = cd_reserve,
    .add                = cd_add,
    .solve              = cd_solve,
    .value              = cd_value,
    .failed             = cd_failed,
    .set_seed           = cd_set_seed,
    .set_conflict_limit = cd_set_conflict_limit,
    .set_decision_limit = cd_set_decision_limit,
    .set_terminate      = cd_set_terminate,
    .arena_size_bytes     = cd_arena_zero,
    .arena_capacity_bytes = cd_arena_zero,
    .arena_save_mark      = cd_arena_zero,
};

void *zsp_sat_cadical_create(zsp_alloc_t *alloc, const zsp_sat_vtbl **vt_out) {
    cadical_impl_t *c = (cadical_impl_t *)cd_xalloc(alloc, sizeof(*c));
    if (!c) return NULL;
    c->alloc  = alloc;
    c->solver = ccadical_init();
    if (!c->solver) {
        cd_xfree(alloc, c, sizeof(*c));
        return NULL;
    }
    ccadical_set_option(c->solver, "quiet", 1);
    /* CaDiCaL 3.x defaults to checking that every variable is declared before
     * use (factorcheck=1, active while the 'factor' technique is on). Our seam
     * uses the classic IPASIR streaming contract where add() implicitly defines
     * variables, so disable the check (this does not disable 'factor' itself). */
    ccadical_set_option(c->solver, "factorcheck", 0);
    *vt_out = &CADICAL_VTBL;
    return c;
}
