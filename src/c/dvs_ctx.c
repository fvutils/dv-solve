#include <stdlib.h>
#include <string.h>
#include "dvs_ctx.h"
#include "dvs_lcg.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline size_t _pool_offset(void) {
    return offsetof(dvs_ctx_t, pool);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

dvs_ctx_t *dvs_solver_create(void *static_buf, size_t static_size,
                         dvs_block_alloc_t *block_alloc) {
    if (!static_buf) return NULL;

    size_t min_size = offsetof(dvs_ctx_t, pool) + sizeof(dvs_pool_t) + 1;
    if (static_size < min_size) return NULL;

    dvs_ctx_t *ctx        = (dvs_ctx_t *)static_buf;
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
    ctx->keep_val        = NULL;
    ctx->keep_state      = NULL;
    ctx->keep_cap        = 0;
    ctx->keep_on         = 0;
    ctx->mod_links       = NULL;
    ctx->n_mod_links     = 0;
    ctx->mod_links_cap   = 0;
    ctx->var_n_values    = NULL;
    ctx->unassigned_mask = 0;
    ctx->assumption_var_ids = NULL;
    ctx->assumption_priorities = NULL;
    ctx->n_assumptions   = 0;
    ctx->assumption_active_mask = 0;
    ctx->incremental_capacity_hint = 0;
    ctx->var_alias       = NULL;
    ctx->current_prop_ref  = EXPR_NULL;
    ctx->prop_aborted      = 0;
    ctx->prop_deadline     = 0.0;
    ctx->prop_ticks        = 0;
    ctx->conflict_prop_ref = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
    ctx->current_trail_flags = 0;
    ctx->lcg               = NULL;
    ctx->scope_log_var     = NULL;
    ctx->scope_log_bound   = NULL;
    ctx->scope_log_depth   = NULL;
    ctx->scope_log_is_lb   = NULL;
    ctx->n_scope_log       = 0;
    ctx->scope_log_cap     = 0;

    /* Init PropQueue — all levels empty */
    ctx->queue.non_empty_mask = 0;
    ctx->queue._pad[0] = ctx->queue._pad[1] = ctx->queue._pad[2] = 0;
    for (int i = 0; i < 16; i++) {
        ctx->queue.heads[i] = EXPR_NULL;
        ctx->queue.tails[i] = EXPR_NULL;
    }

    size_t pool_buf_size = static_size - _pool_offset();
    if (!dvs_pool_init(&ctx->pool, pool_buf_size)) return NULL;

    /* Pre-allocate LevelMark[MAX_DECISION_DEPTH] in the static pool */
    uint32_t marks_ref = dvs_pool_alloc(
        &ctx->pool,
        MAX_DECISION_DEPTH * (uint32_t)sizeof(LevelMark),
        (uint32_t)_Alignof(LevelMark));
    if (marks_ref == EXPR_NULL) return NULL;
    ctx->level_marks = (LevelMark *)dvs_pool_ptr(&ctx->pool, marks_ref);
    ctx->max_depth   = MAX_DECISION_DEPTH;
    memset(ctx->level_marks, 0, MAX_DECISION_DEPTH * sizeof(LevelMark));

    /* Pre-allocate DecisionRecord[MAX_DECISION_DEPTH] in the static pool */
    uint32_t dec_ref = dvs_pool_alloc(
        &ctx->pool,
        MAX_DECISION_DEPTH * (uint32_t)sizeof(DecisionRecord),
        (uint32_t)_Alignof(DecisionRecord));
    if (dec_ref == EXPR_NULL) return NULL;
    ctx->decisions = (DecisionRecord *)dvs_pool_ptr(&ctx->pool, dec_ref);
    memset(ctx->decisions, 0, MAX_DECISION_DEPTH * sizeof(DecisionRecord));

    if (block_alloc) {
        ctx->dynamic = dvs_stack_create(block_alloc);
        if (!ctx->dynamic) return NULL;
    }

    return ctx;
}

int dvs_scope_log_bound(dvs_ctx_t *ctx, uint32_t var_id, int is_lb, int64_t bound) {
    if (ctx->n_scope_log == ctx->scope_log_cap) {
        uint32_t cap = ctx->scope_log_cap ? 2u * ctx->scope_log_cap : 64u;
        uint32_t *nv = (uint32_t *)realloc(ctx->scope_log_var, cap * sizeof *nv);
        if (nv) ctx->scope_log_var = nv;
        int64_t *nb = (int64_t *)realloc(ctx->scope_log_bound, cap * sizeof *nb);
        if (nb) ctx->scope_log_bound = nb;
        uint8_t *nd = (uint8_t *)realloc(ctx->scope_log_depth, cap * sizeof *nd);
        if (nd) ctx->scope_log_depth = nd;
        uint8_t *nl = (uint8_t *)realloc(ctx->scope_log_is_lb, cap * sizeof *nl);
        if (nl) ctx->scope_log_is_lb = nl;
        if (!nv || !nb || !nd || !nl) return -1;
        ctx->scope_log_cap = cap;
    }
    uint32_t i = ctx->n_scope_log++;
    ctx->scope_log_var[i]   = var_id;
    ctx->scope_log_bound[i] = bound;
    ctx->scope_log_depth[i] = (uint8_t)ctx->n_checkpoints;
    ctx->scope_log_is_lb[i] = is_lb ? 1 : 0;
    return 0;
}

void dvs_solver_destroy(dvs_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->lcg) {
        lcg_destroy((LCGCtx *)ctx->lcg);
        free(ctx->lcg);
        ctx->lcg = NULL;
    }
    if (ctx->dynamic) {
        dvs_stack_destroy(ctx->dynamic);
        ctx->dynamic = NULL;
    }
    free(ctx->keep_val);
    free(ctx->keep_state);
    ctx->keep_val = NULL; ctx->keep_state = NULL; ctx->keep_cap = 0;
    free(ctx->mod_links);
    ctx->mod_links = NULL; ctx->n_mod_links = ctx->mod_links_cap = 0;
    free(ctx->scope_log_var);
    free(ctx->scope_log_bound);
    free(ctx->scope_log_depth);
    free(ctx->scope_log_is_lb);
    ctx->scope_log_var = NULL; ctx->scope_log_bound = NULL;
    ctx->scope_log_depth = NULL; ctx->scope_log_is_lb = NULL;
    ctx->n_scope_log = ctx->scope_log_cap = 0;
}

/* ------------------------------------------------------------------ */
/* Accessor wrappers                                                   */
/* ------------------------------------------------------------------ */

int32_t dvs_var_lo32(const dvs_ctx_t *ctx, uint32_t var_id) {
    return var_lo32(_ctx_var(ctx, var_id));
}

int32_t dvs_var_hi32(const dvs_ctx_t *ctx, uint32_t var_id) {
    return var_hi32(_ctx_var(ctx, var_id));
}

int64_t dvs_var_lo64(const dvs_ctx_t *ctx, uint32_t var_id) {
    return var_lo64(ctx, _ctx_var(ctx, var_id));
}

int64_t dvs_var_hi64(const dvs_ctx_t *ctx, uint32_t var_id) {
    return var_hi64(ctx, _ctx_var(ctx, var_id));
}

Variable *dvs_solver_get_var(const dvs_ctx_t *ctx, uint32_t var_id) {
    if (!ctx->vars || var_id >= ctx->n_vars) return NULL;
    return &ctx->vars[var_id];
}

uint32_t dvs_ctx_pool_used(const dvs_ctx_t *ctx) {
    return dvs_pool_used(&ctx->pool);
}

uint32_t dvs_ctx_decision_level(const dvs_ctx_t *ctx) {
    return ctx->decision_level;
}

uint64_t dvs_ctx_trail_count(const dvs_ctx_t *ctx) {
    return ctx->trail_count;
}

uint32_t dvs_prop_constraint_id(const dvs_ctx_t *ctx, uint32_t prop_idx) {
    if (!ctx->prop_constraint_id || prop_idx >= ctx->n_prop_refs_capacity)
        return 0;
    return ctx->prop_constraint_id[prop_idx];
}

void dvs_solver_set_value_selector(dvs_ctx_t *ctx,
                               int64_t (*fn)(dvs_ctx_t *, uint32_t, void *),
                               void *data) {
    ctx->value_selector_fn   = fn;
    ctx->value_selector_data = data;
}
