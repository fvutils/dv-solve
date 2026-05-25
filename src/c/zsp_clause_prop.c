#include <stdint.h>
#include "zsp_lcg.h"
#include "zsp_ctx.h"
#include "zsp_propagator.h"
#include "zsp_trail.h"

/* ------------------------------------------------------------------ */
/* Helper: check a single clause, propagate if unit.                  */
/* Returns PROP_OK, PROP_CONFLICT, or 2 if clause is satisfied.       */
/* ------------------------------------------------------------------ */
static PropResult _check_clause(ClauseDB *db, SolveCtx *ctx,
                                 uint32_t ci) {
    Clause *cl = db->clauses[ci];
    if (!cl) return PROP_OK;

    Literal *lits = (Literal *)(cl + 1);
    uint32_t n = cl->n_lits;
    if (n == 0) return PROP_OK;

    uint32_t n_false = 0;
    uint32_t last_unresolved = 0;

    for (uint32_t i = 0; i < n; i++) {
        if (lits[i].var_id >= ctx->n_vars) return PROP_OK;
        if (literal_is_true(ctx, lits[i])) return PROP_OK; /* satisfied */
        if (literal_is_false(ctx, lits[i])) {
            n_false++;
        } else {
            last_unresolved = i;
        }
    }

    if (n_false == n) {
        db->n_conflicts++;
        return PROP_CONFLICT;
    }

    if (n_false == n - 1) {
        /* Unit clause: enforce */
        Literal unit = lits[last_unresolved];
        if (unit.var_id >= ctx->n_vars) return PROP_OK;

        int64_t cur = unit.is_lb ? var_lo64(ctx, &ctx->vars[unit.var_id])
                                 : var_hi64(ctx, &ctx->vars[unit.var_id]);
        int already_tight = unit.is_lb ? (cur >= unit.bound)
                                       : (cur <= unit.bound);
        if (already_tight) return PROP_OK;

        /* Stamp clause-reason on the trail entry so lcg_analyze_conflict
         * can resolve through this clause instead of treating the
         * tightening as a level-N decision. prop_ref is repurposed as
         * the clause index when TRAIL_FLAG_FROM_CLAUSE is set. */
        uint32_t saved_pr   = ctx->current_prop_ref;
        uint32_t saved_flag = ctx->current_trail_flags;
        ctx->current_prop_ref    = ci;
        ctx->current_trail_flags = saved_flag | TRAIL_FLAG_FROM_CLAUSE;
        PropResult pr;
        if (unit.is_lb)
            pr = ctx_tighten_lb64(ctx, unit.var_id, (int64_t)unit.bound);
        else
            pr = ctx_tighten_ub64(ctx, unit.var_id, (int64_t)unit.bound);
        ctx->current_prop_ref    = saved_pr;
        ctx->current_trail_flags = saved_flag;
        if (pr == PROP_CONFLICT) return PROP_CONFLICT;
        db->n_propagations++;
    }

    return PROP_OK;
}

/* ------------------------------------------------------------------ */
/* Event-driven notifications                                          */
/* ------------------------------------------------------------------ */

/* Reentrancy guard: prevent recursive clause notifications */
static int _in_clause_prop = 0;

PropResult clause_notify_lb(ClauseDB *db, SolveCtx *ctx,
                              uint32_t var_id, int64_t new_lb) {
    if (_in_clause_prop) return PROP_OK;  /* skip recursive calls */
    _in_clause_prop = 1;
    /* When LB of var_id rises, literals "var_id <= v" with v < new_lb
     * become false.  These are UB literals (is_lb=0) with bound < new_lb.
     * Check clauses on the UB watch list for this variable. */
    if (var_id >= db->n_watch_vars) return PROP_OK;
    WatchEntry *we = db->watch_ub[var_id];
    while (we) {
        PropResult pr = _check_clause(db, ctx, we->clause_idx);
        if (pr == PROP_CONFLICT) { _in_clause_prop = 0; return PROP_CONFLICT; }
        we = we->next;
    }
    _in_clause_prop = 0;
    return PROP_OK;
}

PropResult clause_notify_ub(ClauseDB *db, SolveCtx *ctx,
                              uint32_t var_id, int64_t new_ub) {
    if (_in_clause_prop) return PROP_OK;
    _in_clause_prop = 1;
    /* When UB of var_id falls, literals "var_id >= v" with v > new_ub
     * become false.  These are LB literals (is_lb=1) with bound > new_ub.
     * Check clauses on the LB watch list for this variable. */
    if (var_id >= db->n_watch_vars) return PROP_OK;
    WatchEntry *we = db->watch_lb[var_id];
    while (we) {
        PropResult pr = _check_clause(db, ctx, we->clause_idx);
        if (pr == PROP_CONFLICT) { _in_clause_prop = 0; return PROP_CONFLICT; }
        we = we->next;
    }
    _in_clause_prop = 0;
    return PROP_OK;
}

/* ------------------------------------------------------------------ */
/* Full scan propagation (fallback, used less frequently now)          */
/* ------------------------------------------------------------------ */

PropResult clause_propagate(ClauseDB *db, SolveCtx *ctx) {
    /* Fixed-point loop: keep scanning until no new propagations */
    int changed = 1;
    int iterations = 0;
    const int MAX_ITERS = 10;  /* limit iterations; most value is in first pass */

    while (changed && iterations < MAX_ITERS) {
        changed = 0;
        iterations++;

        for (uint32_t ci = 0; ci < db->n_clauses; ci++) {
            Clause *cl = db->clauses[ci];
            if (!cl) continue;

            Literal *lits = (Literal *)(cl + 1);
            uint32_t n = cl->n_lits;
            if (n == 0) continue;

            uint32_t n_false = 0;
            uint32_t n_true = 0;
            uint32_t last_unresolved = 0;

            for (uint32_t i = 0; i < n; i++) {
                if (lits[i].var_id >= ctx->n_vars) { n_true = 1; break; }
                if (literal_is_true(ctx, lits[i])) {
                    n_true = 1;
                    break;
                } else if (literal_is_false(ctx, lits[i])) {
                    n_false++;
                } else {
                    last_unresolved = i;
                }
            }

            if (n_true) continue;

            if (n_false == n) {
                db->n_conflicts++;
                return PROP_CONFLICT;
            }

            if (n_false == n - 1) {
                /* Unit: enforce the single unresolved literal */
                Literal unit = lits[last_unresolved];
                if (unit.var_id >= ctx->n_vars) continue;
                /* Check if this would actually tighten */
                int64_t cur_bound;
                if (unit.is_lb)
                    cur_bound = var_lo64(ctx, &ctx->vars[unit.var_id]);
                else
                    cur_bound = var_hi64(ctx, &ctx->vars[unit.var_id]);
                int already_tight = unit.is_lb ? (cur_bound >= unit.bound)
                                               : (cur_bound <= unit.bound);
                if (already_tight) continue;

                uint32_t saved_pr   = ctx->current_prop_ref;
                uint32_t saved_flag = ctx->current_trail_flags;
                ctx->current_prop_ref    = ci;
                ctx->current_trail_flags = saved_flag | TRAIL_FLAG_FROM_CLAUSE;
                PropResult pr;
                if (unit.is_lb) {
                    pr = ctx_tighten_lb64(ctx, unit.var_id,
                                           (int64_t)unit.bound);
                } else {
                    pr = ctx_tighten_ub64(ctx, unit.var_id,
                                           (int64_t)unit.bound);
                }
                ctx->current_prop_ref    = saved_pr;
                ctx->current_trail_flags = saved_flag;
                if (pr == PROP_CONFLICT) return PROP_CONFLICT;
                changed = 1;
                db->n_propagations++;
            }
        }
    }
    return PROP_OK;
}
