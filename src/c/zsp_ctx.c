#include <stdlib.h>
#include <string.h>
#include "zsp_ctx.h"
#include "zsp_lcg.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline size_t _pool_offset(void) {
    return offsetof(SolveCtx, pool);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SolveCtx *solver_create(void *static_buf, size_t static_size,
                         zsp_block_alloc_t *block_alloc) {
    if (!static_buf) return NULL;

    size_t min_size = offsetof(SolveCtx, pool) + sizeof(zsp_pool_t) + 1;
    if (static_size < min_size) return NULL;

    SolveCtx *ctx        = (SolveCtx *)static_buf;
    ctx->vars            = NULL;
    ctx->n_vars          = 0;
    ctx->n_vars_capacity = 0;
    ctx->decision_level  = 0;
    ctx->trail_count     = 0;
    ctx->conflict_count  = 0;
    ctx->rng_state       = 0;
    ctx->block_alloc     = block_alloc;
    ctx->dynamic         = NULL;
    ctx->trail_top       = NULL;
    ctx->level_marks     = NULL;
    ctx->max_depth       = 0;
    ctx->n_props         = 0;
    ctx->watcher_heads   = NULL;
    ctx->n_checkpoints   = 0;
    ctx->prop_refs       = NULL;
    ctx->n_prop_refs_capacity = 0;
    ctx->decisions       = NULL;
    ctx->phase_save      = NULL;
    ctx->unassigned_mask = 0;
    ctx->assumption_var_ids = NULL;
    ctx->assumption_priorities = NULL;
    ctx->n_assumptions   = 0;
    ctx->assumption_active_mask = 0;
    ctx->_pad            = 0;
    ctx->var_alias       = NULL;
    ctx->current_prop_ref  = EXPR_NULL;
    ctx->conflict_prop_ref = EXPR_NULL;
    ctx->lcg               = NULL;

    /* Init PropQueue — all levels empty */
    ctx->queue.non_empty_mask = 0;
    ctx->queue._pad[0] = ctx->queue._pad[1] = ctx->queue._pad[2] = 0;
    for (int i = 0; i < 16; i++) {
        ctx->queue.heads[i] = EXPR_NULL;
        ctx->queue.tails[i] = EXPR_NULL;
    }

    size_t pool_buf_size = static_size - _pool_offset();
    if (!zsp_pool_init(&ctx->pool, pool_buf_size)) return NULL;

    /* Pre-allocate LevelMark[MAX_DECISION_DEPTH] in the static pool */
    uint32_t marks_ref = zsp_pool_alloc(
        &ctx->pool,
        MAX_DECISION_DEPTH * (uint32_t)sizeof(LevelMark),
        (uint32_t)_Alignof(LevelMark));
    if (marks_ref == EXPR_NULL) return NULL;
    ctx->level_marks = (LevelMark *)zsp_pool_ptr(&ctx->pool, marks_ref);
    ctx->max_depth   = MAX_DECISION_DEPTH;
    memset(ctx->level_marks, 0, MAX_DECISION_DEPTH * sizeof(LevelMark));

    /* Pre-allocate DecisionRecord[MAX_DECISION_DEPTH] in the static pool */
    uint32_t dec_ref = zsp_pool_alloc(
        &ctx->pool,
        MAX_DECISION_DEPTH * (uint32_t)sizeof(DecisionRecord),
        (uint32_t)_Alignof(DecisionRecord));
    if (dec_ref == EXPR_NULL) return NULL;
    ctx->decisions = (DecisionRecord *)zsp_pool_ptr(&ctx->pool, dec_ref);
    memset(ctx->decisions, 0, MAX_DECISION_DEPTH * sizeof(DecisionRecord));

    if (block_alloc) {
        ctx->dynamic = zsp_stack_create(block_alloc);
        if (!ctx->dynamic) return NULL;
    }

    return ctx;
}

void solver_destroy(SolveCtx *ctx) {
    if (!ctx) return;
    if (ctx->lcg) {
        lcg_destroy((LCGCtx *)ctx->lcg);
        free(ctx->lcg);
        ctx->lcg = NULL;
    }
    if (ctx->dynamic) {
        zsp_stack_destroy(ctx->dynamic);
        ctx->dynamic = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Accessor wrappers                                                   */
/* ------------------------------------------------------------------ */

int32_t zsp_var_lo32(const SolveCtx *ctx, uint32_t var_id) {
    return var_lo32(_ctx_var(ctx, var_id));
}

int32_t zsp_var_hi32(const SolveCtx *ctx, uint32_t var_id) {
    return var_hi32(_ctx_var(ctx, var_id));
}

int64_t zsp_var_lo64(const SolveCtx *ctx, uint32_t var_id) {
    return var_lo64(ctx, _ctx_var(ctx, var_id));
}

int64_t zsp_var_hi64(const SolveCtx *ctx, uint32_t var_id) {
    return var_hi64(ctx, _ctx_var(ctx, var_id));
}

Variable *solver_get_var(const SolveCtx *ctx, uint32_t var_id) {
    if (!ctx->vars || var_id >= ctx->n_vars) return NULL;
    return &ctx->vars[var_id];
}

uint32_t zsp_ctx_pool_used(const SolveCtx *ctx) {
    return zsp_pool_used(&ctx->pool);
}

uint32_t zsp_ctx_decision_level(const SolveCtx *ctx) {
    return ctx->decision_level;
}

uint64_t zsp_ctx_trail_count(const SolveCtx *ctx) {
    return ctx->trail_count;
}

uint32_t zsp_prop_constraint_id(const SolveCtx *ctx, uint32_t prop_idx) {
    if (!ctx->prop_constraint_id || prop_idx >= ctx->n_prop_refs_capacity)
        return 0;
    return ctx->prop_constraint_id[prop_idx];
}

void solver_set_value_selector(SolveCtx *ctx,
                               int64_t (*fn)(SolveCtx *, uint32_t, void *),
                               void *data) {
    ctx->value_selector_fn   = fn;
    ctx->value_selector_data = data;
}
