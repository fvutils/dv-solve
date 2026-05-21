#include <string.h>
#include "zsp_ctx.h"
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
    m->trail_top      = ctx->trail_top;
    m->trail_count    = ctx->trail_count;
    m->_cp_pad        = 0;

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

    /* Deactivate propagators added after checkpoint by setting ENTAILED.
       They remain in watcher chains but fire as no-ops. */
    if (ctx->prop_refs) {
        for (uint32_t i = m->n_props_at_cp; i < ctx->n_props; i++) {
            uint32_t pref = ctx->prop_refs[i];
            if (pref != EXPR_NULL) {
                Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, pref);
                p->flags |= PROP_FLAG_ENTAILED;
            }
        }
    }

    /* Restore variable count */
    ctx->n_vars = m->n_vars_at_cp;

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
