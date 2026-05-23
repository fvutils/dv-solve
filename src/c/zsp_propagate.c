#include <stdint.h>
#include "zsp_propagator.h"
#include "zsp_ctx.h"
#include "zsp_trail.h"
#include "zsp_lcg.h"

/* ------------------------------------------------------------------ */
/* Internal: wake all watchers on a variable                          */
/* ------------------------------------------------------------------ */

static void _wake_var(SolveCtx *ctx, uint32_t var_id) {
    uint32_t prop_ref = ctx->watcher_heads[var_id];
    while (prop_ref != EXPR_NULL) {
        Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, prop_ref);

        /* Find next in chain for this variable before potentially re-queuing */
        uint32_t next = EXPR_NULL;

        if (p->flags & PROP_FLAG_WIDE_WATCH) {
            /* Wide-watch propagator (AllDifferent, SumEq, etc.):
             * Layout: hdr, n_vars, _capacity, var_ids[cap], watcher_nexts[cap].
             * Use _capacity to locate watcher_nexts correctly. */
            uint32_t *nv_ptr = (uint32_t *)((char *)p + sizeof(Propagator));
            uint32_t nv = nv_ptr[0];
            uint32_t cap = nv_ptr[1];
            uint32_t *vids = nv_ptr + 2;
            uint32_t *wnexts = vids + cap;
            for (uint32_t i = 0; i < nv; i++) {
                if (vids[i] == var_id) {
                    next = wnexts[i];
                    break;
                }
            }
        } else {
            PropWatchSect *ws = (PropWatchSect *)((char *)p + sizeof(Propagator));
            for (uint32_t i = 0; i < ws->n_watches; i++) {
                if (ws->var_ids[i] == var_id) {
                    next = ws->next_watchers[i];
                    break;
                }
            }
        }

        prop_enqueue(ctx, prop_ref);
        prop_ref = next;
    }

    /* Also re-enqueue any propagator whose guard is this variable */
    if (ctx->prop_guard_vars && ctx->prop_refs) {
        for (uint32_t i = 0; i < ctx->n_props; i++) {
            if (ctx->prop_guard_vars[i] == var_id &&
                ctx->prop_refs[i] != EXPR_NULL) {
                prop_enqueue(ctx, ctx->prop_refs[i]);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Queue management                                                    */
/* ------------------------------------------------------------------ */

void prop_enqueue(SolveCtx *ctx, uint32_t prop_ref) {
    Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, prop_ref);
    if (p->flags & (PROP_FLAG_IN_QUEUE | PROP_FLAG_ENTAILED)) return;

    p->flags    |= PROP_FLAG_IN_QUEUE;
    p->queue_next = EXPR_NULL;

    uint32_t lvl = p->priority;
    if (ctx->queue.heads[lvl] == EXPR_NULL) {
        ctx->queue.heads[lvl] = prop_ref;
        ctx->queue.tails[lvl] = prop_ref;
        ctx->queue.non_empty_mask |= (uint16_t)(1u << lvl);
    } else {
        Propagator *tail = (Propagator *)zsp_pool_ptr(&ctx->pool,
                                                       ctx->queue.tails[lvl]);
        tail->queue_next      = prop_ref;
        ctx->queue.tails[lvl] = prop_ref;
    }
}

/* ------------------------------------------------------------------ */
/* Guard management                                                    */
/* ------------------------------------------------------------------ */

void prop_set_guard(SolveCtx *ctx, uint32_t prop_ref, uint32_t guard_var_id) {
    Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, prop_ref);
    if (ctx->prop_guard_vars && p->prop_id < ctx->n_prop_refs_capacity) {
        ctx->prop_guard_vars[p->prop_id] = guard_var_id;
    }
}

/* ------------------------------------------------------------------ */
/* Domain-tightening                                                   */
/* ------------------------------------------------------------------ */

/* When LCG is active, notify the clause DB so that learnt clauses
 * containing literals on this variable can propagate (or detect
 * conflict).  Returns PROP_OK or PROP_CONFLICT. */
static inline PropResult _lcg_notify_lb(SolveCtx *ctx, uint32_t var_id,
                                         int64_t new_lb) {
    if (!ctx->lcg) return PROP_OK;
    LCGCtx *lcg = (LCGCtx *)ctx->lcg;
    if (!lcg->enabled) return PROP_OK;
    return clause_notify_lb(&lcg->clause_db, ctx, var_id, new_lb);
}

static inline PropResult _lcg_notify_ub(SolveCtx *ctx, uint32_t var_id,
                                         int64_t new_ub) {
    if (!ctx->lcg) return PROP_OK;
    LCGCtx *lcg = (LCGCtx *)ctx->lcg;
    if (!lcg->enabled) return PROP_OK;
    return clause_notify_ub(&lcg->clause_db, ctx, var_id, new_ub);
}

PropResult ctx_tighten_lb32(SolveCtx *ctx, uint32_t var_id, int32_t new_lb) {
    Variable *v = &ctx->vars[var_id];
    if (new_lb <= v->lo) return PROP_OK;

    trail_record_lb(ctx, var_id, (int64_t)new_lb);  /* applies change */

    if (v->lo > v->hi) return PROP_CONFLICT;
    /* Update unassigned_mask: clear bit when variable becomes singleton */
    if (v->lo == v->hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_lb(ctx, var_id, (int64_t)new_lb);
}

PropResult ctx_tighten_ub32(SolveCtx *ctx, uint32_t var_id, int32_t new_ub) {
    Variable *v = &ctx->vars[var_id];
    if (new_ub >= v->hi) return PROP_OK;

    trail_record_ub(ctx, var_id, (int64_t)new_ub);  /* applies change */

    if (v->lo > v->hi) return PROP_CONFLICT;
    if (v->lo == v->hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_ub(ctx, var_id, (int64_t)new_ub);
}

PropResult ctx_tighten_lb64(SolveCtx *ctx, uint32_t var_id, int64_t new_lb) {
    Variable *v = &ctx->vars[var_id];
    int64_t curr = var_lo64(ctx, v);
    if (new_lb <= curr) return PROP_OK;

    trail_record_lb(ctx, var_id, new_lb);

    /* check conflict */
    int64_t lo = var_lo64(ctx, v);
    int64_t hi = var_hi64(ctx, v);
    if (lo > hi) return PROP_CONFLICT;
    if (lo == hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_lb(ctx, var_id, new_lb);
}

PropResult ctx_tighten_ub64(SolveCtx *ctx, uint32_t var_id, int64_t new_ub) {
    Variable *v = &ctx->vars[var_id];
    int64_t curr = var_hi64(ctx, v);
    if (new_ub >= curr) return PROP_OK;

    trail_record_ub(ctx, var_id, new_ub);

    int64_t lo = var_lo64(ctx, v);
    int64_t hi = var_hi64(ctx, v);
    if (lo > hi) return PROP_CONFLICT;
    if (lo == hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_ub(ctx, var_id, new_ub);
}

/* ------------------------------------------------------------------ */
/* Propagation loop                                                    */
/* ------------------------------------------------------------------ */

PropResult solver_propagate(SolveCtx *ctx) {
    while (ctx->queue.non_empty_mask) {
        /* Lowest set bit = highest priority level with entries */
        uint32_t lvl = (uint32_t)__builtin_ctz(ctx->queue.non_empty_mask);

        /* Dequeue head */
        uint32_t    prop_ref = ctx->queue.heads[lvl];
        Propagator *p        = (Propagator *)zsp_pool_ptr(&ctx->pool, prop_ref);

        ctx->queue.heads[lvl] = p->queue_next;
        if (ctx->queue.heads[lvl] == EXPR_NULL) {
            ctx->queue.tails[lvl]      = EXPR_NULL;
            ctx->queue.non_empty_mask &= (uint16_t)~(1u << lvl);
        }
        p->queue_next  = EXPR_NULL;
        p->flags      &= (uint8_t)~PROP_FLAG_IN_QUEUE;

        if (p->flags & PROP_FLAG_ENTAILED) continue;

        /* Guard-gated check: skip or entail based on guard variable */
        if (ctx->prop_guard_vars && p->prop_id < ctx->n_prop_refs_capacity) {
            uint32_t gid = ctx->prop_guard_vars[p->prop_id];
            if (gid != EXPR_NULL) {
                int64_t glo = var_lo64(ctx, &ctx->vars[gid]);
                int64_t ghi = var_hi64(ctx, &ctx->vars[gid]);
                if (glo == 0 && ghi == 0) {
                    /* Guard is false: propagator is entailed */
                    p->flags |= PROP_FLAG_ENTAILED;
                    continue;
                }
                if (glo != ghi) {
                    /* Guard is undecided: skip this firing */
                    continue;
                }
                /* Guard is true (glo == ghi == 1): fire normally */
            }
        }

        /* Tag the firing propagator so trail_record_* attributes the
         * resulting bound tightenings to it. Cleared after fire so
         * non-propagation tightenings (e.g. decision pushes, level-0
         * compile-time clamps) record reason=EXPR_NULL. */
        ctx->current_prop_ref = prop_ref;
        PropResult r = p->fire(p, ctx);
        ctx->current_prop_ref = EXPR_NULL;
        if (r == PROP_CONFLICT) {
            /* Stash the conflicting propagator for downstream conflict
             * analysis (zsp_analyze_conflict). */
            ctx->conflict_prop_ref = prop_ref;
            return PROP_CONFLICT;
        }
        if (r == PROP_ENTAILED) p->flags |= PROP_FLAG_ENTAILED;
    }
    return PROP_OK;
}

/* ------------------------------------------------------------------ */
/* solver_propagate_only -- public propagation-only feasibility check */
/* ------------------------------------------------------------------ */

PropResult solver_propagate_only(SolveCtx *ctx) {
    return solver_propagate(ctx);
}
