#include <string.h>
#include "zsp_ctx.h"
#include "zsp_lcg.h"
#include "zsp_propagator.h"
#include "zsp_trail.h"

/* ------------------------------------------------------------------ */
/* solver_checkpoint -- save solver state                              */
/* ------------------------------------------------------------------ */

int solver_checkpoint(SolveCtx *ctx) {
    if (ctx->n_checkpoints >= MAX_CHECKPOINTS) return -1;

    uint32_t cp = ctx->n_checkpoints++;
    CheckpointMark *m = &ctx->checkpoints[cp];

    m->decision_level = ctx->decision_level;
    m->n_vars_at_cp   = ctx->n_vars;
    m->n_props_at_cp  = ctx->n_props;
    m->n_clauses_at_cp = ctx->lcg ? ((LCGCtx *)ctx->lcg)->clause_db.n_clauses : 0;
    m->trail_top      = ctx->trail_top;
    m->trail_count    = ctx->trail_count;

    if (ctx->dynamic)
        m->stack_mark = zsp_stack_push(ctx->dynamic);

    /* Push a trail level to isolate subsequent domain changes */
    trail_push_level(ctx);

    return (int)cp;
}

/* ------------------------------------------------------------------ */
/* solver_restore -- restore solver state to checkpoint               */
/* ------------------------------------------------------------------ */

void solver_restore(SolveCtx *ctx, uint32_t cp) {
    if (cp >= ctx->n_checkpoints) return;

    CheckpointMark *m = &ctx->checkpoints[cp];

    /* Backtrack trail to undo all domain changes since checkpoint */
    trail_backtrack(ctx, m->decision_level);

    /* Remove propagators added after checkpoint. NULL'ing the
     * prop_refs[] slot is enough — watcher chains may still point
     * at the (now-dead) propagator memory, but neither _wake_var nor
     * solver_propagate iterate prop_refs slots that are NULL.
     * Marking ENTAILED alone wasn't sufficient: solver_reset()
     * clears the ENTAILED bit on every slot in prop_refs, which
     * silently re-activated post-checkpoint props after a pop +
     * subsequent check-sat. The new vars at reused IDs would then
     * be constrained by stale propagators from the popped scope
     * and search returned spurious unsat. */
    if (ctx->prop_refs) {
        uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                       ? ctx->n_props : ctx->n_prop_refs_capacity;
        for (uint32_t i = m->n_props_at_cp; i < lim; i++) {
            uint32_t pref = ctx->prop_refs[i];
            if (pref != EXPR_NULL) {
                Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, pref);
                /* Mark entailed too in case the watcher chain still
                 * iterates the propagator before its slot is checked. */
                p->flags |= PROP_FLAG_ENTAILED;
                ctx->prop_refs[i] = EXPR_NULL;
            }
        }
    }
    /* Roll back n_props as well so future allocations get fresh
     * slots rather than colliding with the dead entries. */
    ctx->n_props = m->n_props_at_cp;

    /* Clear the propagator queue. Stale entries from inside-push
     * propagators (now NULL'd in prop_refs but possibly queued by
     * watcher-chain side effects) could otherwise be dequeued and
     * deref'd as ENTAILED check happens after dequeue — but the
     * pool memory may be in an inconsistent state. */
    ctx->queue.non_empty_mask = 0;
    for (int i = 0; i < 16; i++) {
        ctx->queue.heads[i] = EXPR_NULL;
        ctx->queue.tails[i] = EXPR_NULL;
    }
    /* Re-enqueue pre-checkpoint props (capped at side-table size). */
    if (ctx->prop_refs) {
        uint32_t lim = m->n_props_at_cp < ctx->n_prop_refs_capacity
                       ? m->n_props_at_cp : ctx->n_prop_refs_capacity;
        for (uint32_t i = 0; i < lim; i++) {
            uint32_t pref = ctx->prop_refs[i];
            if (pref != EXPR_NULL) {
                Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, pref);
                p->flags &= (uint8_t)~(PROP_FLAG_ENTAILED | PROP_FLAG_IN_QUEUE);
                p->queue_next = EXPR_NULL;
                prop_enqueue(ctx, pref);
            }
        }
    }

    /* Restore variable count */
    ctx->n_vars = m->n_vars_at_cp;

    /* Drop learnt clauses added between checkpoint and now. Their
     * literals reference var IDs that may be reused for fresh aux
     * vars in the post-pop scope — keeping the clauses around would
     * mis-interpret them against the new vars and produce spurious
     * unsat results. Set to NULL (the arena memory stays; clause_db
     * scans skip NULL entries). */
    if (ctx->lcg) {
        ClauseDB *db = &((LCGCtx *)ctx->lcg)->clause_db;
        for (uint32_t i = m->n_clauses_at_cp; i < db->n_clauses; i++) {
            db->clauses[i] = NULL;
        }
        db->n_clauses = m->n_clauses_at_cp;
    }

    /* Pop this and all later checkpoints */
    ctx->n_checkpoints = cp;

    /* Clear entailed flag on pre-checkpoint propagators and re-enqueue.
     * During solving, propagators may have been marked ENTAILED; this
     * flag must be cleared so they fire again after domain restoration. */
    ctx->queue.non_empty_mask = 0;
    for (int i = 0; i < 16; i++) {
        ctx->queue.heads[i] = EXPR_NULL;
        ctx->queue.tails[i] = EXPR_NULL;
    }
    if (ctx->prop_refs) {
        for (uint32_t i = 0; i < m->n_props_at_cp; i++) {
            uint32_t pref = ctx->prop_refs[i];
            if (pref != EXPR_NULL) {
                Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, pref);
                p->flags &= (uint8_t)~(PROP_FLAG_ENTAILED | PROP_FLAG_IN_QUEUE);
                prop_enqueue(ctx, pref);
            }
        }
    }
}
