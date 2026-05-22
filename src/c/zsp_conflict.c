/*
 * zsp_conflict.c — Conflict analysis
 *
 * Phase 6.1b: replaces the previous chronological-only stub with a
 * reason-aware backjump-level computation.
 *
 * Algorithm (conservative pre-UIP "look-back"):
 *
 *   1. Start from the conflicting propagator (ctx->conflict_prop_ref) and
 *      walk its watched variables.  For each, find the latest trail entry
 *      with decision_level < current_level.  These are the lower-level
 *      facts the conflict directly depends on.
 *
 *   2. For each trail entry at the current level that we encounter during
 *      the walk (i.e. variables the conflicting propagator watched that
 *      were tightened at the current level), recurse one step into the
 *      causing propagator (e->prop_ref) and inspect ITS watched vars.
 *      This yields the second-tier dependencies — still trail entries at
 *      lower levels.
 *
 *   3. Backjump level = the highest such lower-level decision level.
 *      If none is found, fall back to chronological (current_level - 1).
 *
 * This is not a full first-UIP analysis (no learnt-clause materialisation,
 * no full resolution chain), but it does enable non-chronological
 * backjumping when the conflict skips over irrelevant intermediate
 * decision levels.  Sound — over-approximates the dependency set, so any
 * level it backjumps to has actually been touched.
 *
 * The search loop in zsp_search.c may consult `analyze_conflict` to decide
 * how far to unwind; the current loop is structured around bisection of
 * the failing decision's domain, so this analysis is wired but advisory.
 */

#include <stdint.h>
#include "zsp_ctx.h"
#include "zsp_propagator.h"
#include "zsp_trail.h"
#include "zsp_pool.h"

#define PROP_WS(p) ((PropWatchSect *)((char *)(p) + sizeof(Propagator)))

/* Find the most recent trail entry for var_id and update *out_max_lower
 * if its decision_level is below current_level and is the new maximum.
 * Returns 1 if the entry was at the current level (caller may want to
 * recurse into its reason), 0 otherwise (or if no entry found). */
static int _scan_var(SolveCtx *ctx, uint32_t var_id,
                     uint32_t current_level, uint32_t *out_max_lower,
                     TrailEntry **out_entry) {
    for (TrailEntry *e = ctx->trail_top; e; e = e->prev) {
        if (e->var_id == var_id) {
            if (out_entry) *out_entry = e;
            if (e->decision_level < current_level) {
                if (e->decision_level > *out_max_lower)
                    *out_max_lower = e->decision_level;
                return 0;
            }
            return 1;
        }
    }
    if (out_entry) *out_entry = NULL;
    return 0;
}

uint32_t analyze_conflict(SolveCtx *ctx) {
    uint32_t current_level = ctx->decision_level;
    if (current_level == 0) return 0;

    uint32_t conflict_prop = ctx->conflict_prop_ref;
    if (conflict_prop == EXPR_NULL) return current_level - 1;

    Propagator    *p  = (Propagator *)zsp_pool_ptr(&ctx->pool, conflict_prop);
    PropWatchSect *ws = PROP_WS(p);
    uint32_t       max_lower = 0;
    int            found_lower = 0;

    /* First tier: conflicting propagator's watched variables. */
    for (uint32_t i = 0; i < ws->n_watches; i++) {
        TrailEntry *e = NULL;
        uint32_t before = max_lower;
        int at_current = _scan_var(ctx, ws->var_ids[i], current_level,
                                    &max_lower, &e);
        if (max_lower != before) found_lower = 1;

        /* Second tier: if the var was tightened at current level by a
         * propagator, follow that propagator's antecedents too. */
        if (at_current && e && e->prop_ref != EXPR_NULL) {
            Propagator    *ap  = (Propagator *)zsp_pool_ptr(&ctx->pool,
                                                             e->prop_ref);
            PropWatchSect *aws = PROP_WS(ap);
            for (uint32_t j = 0; j < aws->n_watches; j++) {
                uint32_t b2 = max_lower;
                (void)_scan_var(ctx, aws->var_ids[j], current_level,
                                &max_lower, NULL);
                if (max_lower != b2) found_lower = 1;
            }
        }
    }

    if (!found_lower) return current_level - 1;
    return max_lower;
}
