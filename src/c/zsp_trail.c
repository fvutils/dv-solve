#include <string.h>
#include "zsp_trail.h"
#include "zsp_ctx.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static TrailEntry *_alloc_entry(SolveCtx *ctx) {
    return (TrailEntry *)zsp_stack_alloc(
        ctx->dynamic, sizeof(TrailEntry), _Alignof(TrailEntry));
}

static void _push_entry(SolveCtx *ctx, TrailEntry *e) {
    e->prev        = ctx->trail_top;
    ctx->trail_top = e;
    ctx->trail_count++;
}

/* ------------------------------------------------------------------ */
/* Decision-level push                                                 */
/* ------------------------------------------------------------------ */

void trail_push_level(SolveCtx *ctx) {
    uint32_t lvl = ctx->decision_level;
    LevelMark *mark = &ctx->level_marks[lvl];
    mark->stack_mark  = zsp_stack_push(ctx->dynamic);
    mark->trail_top   = ctx->trail_top;
    mark->trail_count = ctx->trail_count;
    ctx->decision_level = lvl + 1;
}

/* ------------------------------------------------------------------ */
/* Record lower-bound tightening                                       */
/* ------------------------------------------------------------------ */

int trail_record_lb(SolveCtx *ctx, uint32_t var_id, int64_t new_lb) {
    Variable *v = &ctx->vars[var_id];

    int64_t old_lb;
    uint8_t vsize;

    if (VAR_IS_TIER0(v->flags)) {
        old_lb = (int64_t)v->lo;
        vsize  = TRAIL_VALUE_32;
        v->lo  = (int32_t)new_lb;
    } else if (VAR_IS_TIER1(v->flags)) {
        WideBounds64 *wb =
            (WideBounds64 *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
        old_lb  = wb->lo;
        vsize   = TRAIL_VALUE_64;
        wb->lo  = new_lb;
    } else {
        return -1; /* tier-2: Phase 6 */
    }

    TrailEntry *e = _alloc_entry(ctx);
    if (!e) return -1;

    e->var_id         = var_id;
    e->kind           = TRAIL_LB;
    e->value_size     = vsize;
    e->decision_level = (uint16_t)ctx->decision_level;
    e->old_value      = old_lb;
    e->prop_ref       = ctx->current_prop_ref;
    e->_te_pad        = 0;
    _push_entry(ctx, e);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Record upper-bound tightening                                       */
/* ------------------------------------------------------------------ */

int trail_record_ub(SolveCtx *ctx, uint32_t var_id, int64_t new_ub) {
    Variable *v = &ctx->vars[var_id];

    int64_t old_ub;
    uint8_t vsize;

    if (VAR_IS_TIER0(v->flags)) {
        old_ub = (int64_t)v->hi;
        vsize  = TRAIL_VALUE_32;
        v->hi  = (int32_t)new_ub;
    } else if (VAR_IS_TIER1(v->flags)) {
        WideBounds64 *wb =
            (WideBounds64 *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
        old_ub  = wb->hi;
        vsize   = TRAIL_VALUE_64;
        wb->hi  = new_ub;
    } else {
        return -1; /* tier-2: Phase 6 */
    }

    TrailEntry *e = _alloc_entry(ctx);
    if (!e) return -1;

    e->var_id         = var_id;
    e->kind           = TRAIL_UB;
    e->value_size     = vsize;
    e->decision_level = (uint16_t)ctx->decision_level;
    e->old_value      = old_ub;
    e->prop_ref       = ctx->current_prop_ref;
    e->_te_pad        = 0;
    _push_entry(ctx, e);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Record hole                                                         */
/* ------------------------------------------------------------------ */

int trail_record_hole(SolveCtx *ctx, uint32_t var_id, int64_t removed_val) {
    TrailEntry *e = _alloc_entry(ctx);
    if (!e) return -1;

    e->var_id         = var_id;
    e->kind           = TRAIL_HOLE;
    e->value_size     = TRAIL_VALUE_32; /* holes always 32-bit for now */
    e->decision_level = (uint16_t)ctx->decision_level;
    e->old_value      = removed_val;
    e->prop_ref       = ctx->current_prop_ref;
    e->_te_pad        = 0;
    _push_entry(ctx, e);
    return 0;
    /* Phase 6 will remove the value from the variable's domain here */
}

/* ------------------------------------------------------------------ */
/* Backtrack                                                           */
/* ------------------------------------------------------------------ */

void trail_backtrack(SolveCtx *ctx, uint32_t target_level) {
    LevelMark *mark = &ctx->level_marks[target_level];
    TrailEntry *stop = mark->trail_top;
    TrailEntry *e    = ctx->trail_top;

    /* Walk the trail backward and undo each change */
    while (e != stop) {
        Variable *v = &ctx->vars[e->var_id];

        switch ((TrailKind)e->kind) {
        case TRAIL_LB:
            if (VAR_IS_TIER0(v->flags)) {
                v->lo = (int32_t)e->old_value;
            } else if (VAR_IS_TIER1(v->flags)) {
                WideBounds64 *wb =
                    (WideBounds64 *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
                wb->lo = e->old_value;
            }
            break;

        case TRAIL_UB:
            if (VAR_IS_TIER0(v->flags)) {
                v->hi = (int32_t)e->old_value;
            } else if (VAR_IS_TIER1(v->flags)) {
                WideBounds64 *wb =
                    (WideBounds64 *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
                wb->hi = e->old_value;
            }
            break;

        case TRAIL_HOLE:
            /* Phase 6: restore removed value to the hole list */
            break;
        }

        e = e->prev;
    }

    /* Recover all dynamic-stack memory from this level onward */
    zsp_stack_pop(ctx->dynamic, mark->stack_mark);

    /* Restore trail state */
    ctx->trail_top      = mark->trail_top;
    ctx->trail_count    = mark->trail_count;
    ctx->decision_level = target_level;

    /* Rebuild unassigned_mask from restored domains */
    if (ctx->n_vars <= 64) {
        uint64_t mask = 0;
        for (uint32_t i = 0; i < ctx->n_vars; i++) {
            if (var_lo64(ctx, &ctx->vars[i]) != var_hi64(ctx, &ctx->vars[i]))
                mask |= (1ULL << i);
        }
        ctx->unassigned_mask = mask;
    }

    /* Clear the propagation queue — any entries left from the conflicting
     * level are stale (they reference tightened domains that have been
     * restored).  Clear IN_QUEUE flags too so propagators can be re-woken. */
    /* Clear the propagation queue — any entries left from the conflicting
     * level are stale (they reference tightened domains that have been
     * restored).  Clear IN_QUEUE flags too so propagators can be re-woken. */
    for (uint32_t lvl = 0; lvl < 16; lvl++) {
        uint32_t ref = ctx->queue.heads[lvl];
        while (ref != EXPR_NULL) {
            Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
            uint32_t next = p->queue_next;
            p->flags &= (uint8_t)~PROP_FLAG_IN_QUEUE;
            p->flags &= (uint8_t)~PROP_FLAG_ENTAILED;
            p->queue_next = EXPR_NULL;
            ref = next;
        }
        ctx->queue.heads[lvl] = EXPR_NULL;
        ctx->queue.tails[lvl] = EXPR_NULL;
    }
    ctx->queue.non_empty_mask = 0;

    /* Clear ENTAILED on all propagators reachable via watcher chains.
     * Propagators that fired and returned PROP_ENTAILED during the
     * backtracked levels were dequeued, so the queue walk above did not
     * reach them.  Their entailment may no longer hold after domain
     * restoration, so they must be eligible for re-firing. */
    if (ctx->watcher_heads) {
        for (uint32_t vi = 0; vi < ctx->n_vars; vi++) {
            uint32_t ref = ctx->watcher_heads[vi];
            while (ref != EXPR_NULL) {
                Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
                p->flags &= (uint8_t)~PROP_FLAG_ENTAILED;
                uint32_t next = EXPR_NULL;
                if (p->flags & PROP_FLAG_WIDE_WATCH) {
                    uint32_t *nv_ptr = (uint32_t *)((char *)p + sizeof(Propagator));
                    uint32_t nv = nv_ptr[0];
                    uint32_t cap = nv_ptr[1];
                    uint32_t *vids = nv_ptr + 2;
                    uint32_t *wnexts = vids + cap;
                    for (uint32_t i = 0; i < nv; i++) {
                        if (vids[i] == vi) {
                            next = wnexts[i];
                            break;
                        }
                    }
                } else {
                    PropWatchSect *ws =
                        (PropWatchSect *)((char *)p + sizeof(Propagator));
                    for (uint32_t i = 0; i < ws->n_watches; i++) {
                        if (ws->var_ids[i] == vi) {
                            next = ws->next_watchers[i];
                            break;
                        }
                    }
                }
                ref = next;
            }
        }
    }

}
