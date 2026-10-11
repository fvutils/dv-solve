#include <stdint.h>
#include "dvs_lcg.h"
#include "dvs_ctx.h"
#include "dvs_propagator.h"
#include "dvs_trail.h"

/* ------------------------------------------------------------------ */
/* Helper: check a single clause, propagate if unit.                  */
/* Returns PROP_OK, PROP_CONFLICT, or 2 if clause is satisfied.       */
/* ------------------------------------------------------------------ */
static PropResult _check_clause(ClauseDB *db, dvs_ctx_t *ctx,
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
        ctx->conflict_clause_idx = ci;
        return PROP_CONFLICT;
    }

    if (n_false == n - 1) {
        /* Unit clause: enforce */
        Literal unit = lits[last_unresolved];
        if (unit.var_id >= ctx->n_vars) return PROP_OK;

        /* Sign-aware, for the same reason literal_is_true/false are: on an
         * unsigned width-64 var a bare signed compare reads hi == -1 (the
         * pattern 2^64-1) as below every bound and skips a real tightening. */
        const Variable *uv = &ctx->vars[unit.var_id];
        int64_t cur = unit.is_lb ? var_lo64(ctx, uv) : var_hi64(ctx, uv);
        int already_tight = unit.is_lb ? !var_b_lt(uv, cur, unit.bound)
                                       : !var_b_gt(uv, cur, unit.bound);
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

/* A tightening made while a clause is being checked (a unit clause
 * propagating) does not start a nested notification: it is queued in
 * db->pend and its watch list is checked once the running notification
 * finishes, until the queue is empty. Dropping it instead -- as this did
 * until 2026-10-10 -- lost every clause-to-clause chain: a clause made unit
 * or false by another clause's propagation went unnoticed until some later
 * change to one of its variables, and conflict analysis then met clauses
 * that were false below the current level (no UIP: chronological
 * backtrack) or re-learnt clauses already in the database (plan 3.1;
 * qc_satadd_w4: 271,664 + 66,478 of 413k conflicts).
 *
 * The queue holds var_id*2 + (1 for an upper-bound event). If it fills,
 * pend_over makes the drain rescan every clause instead. The flag and the
 * queue are per database (B115: a process-wide static once stayed set and
 * turned learnt-clause propagation off for every solver). */

PropResult clause_propagate(ClauseDB *db, dvs_ctx_t *ctx);

static PropResult _check_watch(ClauseDB *db, dvs_ctx_t *ctx, WatchEntry *we) {
    for (; we; we = we->next)
        if (_check_clause(db, ctx, we->clause_idx) == PROP_CONFLICT)
            return PROP_CONFLICT;
    return PROP_OK;
}

/* Check the queued events until none is left or one conflicts, then
 * release the queue. */
static PropResult _drain(ClauseDB *db, dvs_ctx_t *ctx, PropResult res) {
    while (res != PROP_CONFLICT && (db->n_pend || db->pend_over)) {
        if (db->n_pend) {
            uint32_t e = db->pend[--db->n_pend];
            uint32_t v = e >> 1;
            res = _check_watch(db, ctx, (e & 1u) ? db->watch_lb[v] : db->watch_ub[v]);
        } else {
            db->pend_over = 0;
            res = clause_propagate(db, ctx);
        }
    }
    db->n_pend = 0;          /* after a conflict the search backtracks */
    db->pend_over = 0;
    db->in_notify = 0;
    return res;
}

static PropResult _notify(ClauseDB *db, dvs_ctx_t *ctx, uint32_t var_id, int ub_event) {
    if (var_id >= db->n_watch_vars) return PROP_OK;
    if (db->in_notify) {
        if (db->n_pend < db->pend_cap)
            db->pend[db->n_pend++] = var_id * 2u + (ub_event ? 1u : 0u);
        else
            db->pend_over = 1;
        return PROP_OK;
    }
    db->in_notify = 1;
    /* An LB rise falsifies `var <= v` literals (the UB watch list); a UB
     * fall falsifies `var >= v` literals (the LB watch list). */
    return _drain(db, ctx, _check_watch(db, ctx, ub_event ? db->watch_lb[var_id]
                                                          : db->watch_ub[var_id]));
}

int clause_notify_hold(ClauseDB *db) {
    if (db->in_notify) return 0;
    db->in_notify = 1;
    return 1;
}

PropResult clause_notify_release(ClauseDB *db, dvs_ctx_t *ctx, int check) {
    if (check) return _drain(db, ctx, PROP_OK);
    db->n_pend = 0;
    db->pend_over = 0;
    db->in_notify = 0;
    return PROP_OK;
}

PropResult clause_notify_lb(ClauseDB *db, dvs_ctx_t *ctx,
                              uint32_t var_id, int64_t new_lb) {
    (void)new_lb;
    return _notify(db, ctx, var_id, 0);
}

PropResult clause_notify_ub(ClauseDB *db, dvs_ctx_t *ctx,
                              uint32_t var_id, int64_t new_ub) {
    (void)new_ub;
    return _notify(db, ctx, var_id, 1);
}

/* ------------------------------------------------------------------ */
/* Full scan propagation (fallback, used less frequently now)          */
/* ------------------------------------------------------------------ */

PropResult clause_propagate(ClauseDB *db, dvs_ctx_t *ctx) {
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
                ctx->conflict_clause_idx = ci;
                return PROP_CONFLICT;
            }

            if (n_false == n - 1) {
                /* Unit: enforce the single unresolved literal */
                Literal unit = lits[last_unresolved];
                if (unit.var_id >= ctx->n_vars) continue;
                /* Check if this would actually tighten */
                const Variable *uv = &ctx->vars[unit.var_id];
                int64_t cur_bound;
                if (unit.is_lb)
                    cur_bound = var_lo64(ctx, uv);
                else
                    cur_bound = var_hi64(ctx, uv);
                int already_tight = unit.is_lb
                    ? !var_b_lt(uv, cur_bound, unit.bound)
                    : !var_b_gt(uv, cur_bound, unit.bound);
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
