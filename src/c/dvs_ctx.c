#include <stdlib.h>
#include <string.h>
#include "dvs_ctx.h"
#include "dvs_oracle_hooks.h"
#include "dvs_lcg.h"
#include "dvs_costguided.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* The pool's fixed region starts after the header, 16-byte aligned. */
static inline size_t _pool_offset(void) {
    return (sizeof(dvs_ctx_t) + 15u) & ~(size_t)15u;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static dvs_ctx_t *_ctx_init(dvs_ctx_t *ctx, void *pool_buf, size_t pool_size,
                            dvs_block_alloc_t *block_alloc, dvs_alloc_t *grow);

dvs_ctx_t *dvs_solver_new(dvs_alloc_t *alloc) {
    if (!alloc) alloc = &dvs_malloc_alloc;
    dvs_ctx_t *ctx = (dvs_ctx_t *)DVS_ALLOC(alloc, sizeof(dvs_ctx_t));
    if (!ctx) return NULL;
    /* No fixed region: the pool grows from its first allocation, and the
     * trail stack gets a block allocator of its own over `alloc`. */
    dvs_block_alloc_t *ba = dvs_block_alloc_create(alloc, 64u << 10);
    if (!ba) { DVS_RELEASE(alloc, ctx, sizeof(dvs_ctx_t)); return NULL; }
    if (!_ctx_init(ctx, NULL, 0, ba, alloc)) {
        dvs_block_alloc_destroy(ba);
        DVS_RELEASE(alloc, ctx, sizeof(dvs_ctx_t));
        return NULL;
    }
    ctx->own_block_alloc = ba;
    ctx->own_header = 1;
    return ctx;
}

void dvs_solver_free(dvs_ctx_t *ctx) {
    if (!ctx) return;
    dvs_alloc_t *alloc = ctx->alloc;
    int own = ctx->own_header;
    dvs_solver_destroy(ctx);
    if (own) DVS_RELEASE(alloc, ctx, sizeof(dvs_ctx_t));
}

dvs_ctx_t *dvs_solver_create(void *static_buf, size_t static_size,
                         dvs_block_alloc_t *block_alloc) {
    if (!static_buf) return NULL;

    size_t min_size = _pool_offset() + 64u;
    if (static_size < min_size) return NULL;

    /* The header at the front of the buffer, the pool's fixed region after
     * it. With a block allocator the pool grows past the buffer through
     * the allocator behind it; without one it stays inside. */
    return _ctx_init((dvs_ctx_t *)static_buf,
                     (uint8_t *)static_buf + _pool_offset(),
                     static_size - _pool_offset(), block_alloc,
                     block_alloc ? dvs_block_alloc_backing(block_alloc) : NULL);
}

/* level_marks and decisions, one block of `cap` entries each: from
 * ctx->alloc when the pool can grow, else carved from the fixed pool. */
#define LEVEL_ROW (sizeof(LevelMark) + sizeof(DecisionRecord))

static int _alloc_levels(dvs_ctx_t *ctx, uint32_t cap) {
    uint8_t *b;
    if (ctx->pool.alloc) {
        b = (uint8_t *)DVS_ALLOC(ctx->alloc, (size_t)cap * LEVEL_ROW);
    } else {
        uint32_t ref = dvs_spool_alloc(&ctx->pool, cap * (uint32_t)LEVEL_ROW,
                                       (uint32_t)_Alignof(LevelMark));
        b = ref == EXPR_NULL ? NULL : (uint8_t *)dvs_spool_ptr(&ctx->pool, ref);
    }
    if (!b) return -1;
    memset(b, 0, (size_t)cap * LEVEL_ROW);
    ctx->level_marks = (LevelMark *)b;
    ctx->decisions   = (DecisionRecord *)(ctx->level_marks + cap);
    ctx->level_cap   = cap;
    return 0;
}

static void _free_levels(dvs_ctx_t *ctx) {
    if (ctx->pool.alloc && ctx->level_marks)
        DVS_RELEASE(ctx->alloc, ctx->level_marks, (size_t)ctx->level_cap * LEVEL_ROW);
    ctx->level_marks = NULL; ctx->decisions = NULL; ctx->level_cap = 0;
}

int dvs_ctx_reserve_levels(dvs_ctx_t *ctx, uint32_t n) {
    uint32_t old = ctx->level_cap;
    if (n <= old) return 0;
    if (!ctx->pool.alloc || n > 0xFFFFu) return -1;
    uint32_t cap = old * 2u > n ? old * 2u : n;
    if (cap > 0xFFFFu) cap = 0xFFFFu;
    uint8_t *b = (uint8_t *)DVS_ALLOC(ctx->alloc, (size_t)cap * LEVEL_ROW);
    if (!b) return -1;
    LevelMark *lm = (LevelMark *)b;
    DecisionRecord *dr = (DecisionRecord *)(lm + cap);
    memcpy(lm, ctx->level_marks, (size_t)old * sizeof *lm);
    memset(lm + old, 0, (size_t)(cap - old) * sizeof *lm);
    memcpy(dr, ctx->decisions, (size_t)old * sizeof *dr);
    memset(dr + old, 0, (size_t)(cap - old) * sizeof *dr);
    DVS_RELEASE(ctx->alloc, ctx->level_marks, (size_t)old * LEVEL_ROW);
    ctx->level_marks = lm;
    ctx->decisions   = dr;
    ctx->level_cap   = cap;
    return 0;
}

/* Set up the header at `ctx`: a pool over [pool_buf, pool_buf + pool_size)
 * growing through `grow` (NULL: fixed), the trail stack on `block_alloc`
 * (NULL: a block allocator of the context's own). */
static dvs_ctx_t *_ctx_init(dvs_ctx_t *ctx, void *pool_buf, size_t pool_size,
                            dvs_block_alloc_t *block_alloc, dvs_alloc_t *grow) {
    /* Zero every header field first: the caller's buffer may be recycled, and
     * the per-field list below had fallen behind the struct (initial_vars,
     * var_holes_head, prop_guard_vars, ... were left as whatever the buffer
     * held). Compile's set punch-out read initial_vars before setting it and
     * dereferenced garbage once dv-solve-smt2 reused a dirty buffer. */
    memset(ctx, 0, sizeof(dvs_ctx_t));
    ctx->vars            = NULL;
    ctx->n_vars          = 0;
    ctx->n_vars_capacity = 0;
    ctx->decision_level  = 0;
    ctx->trail_count     = 0;
    ctx->conflict_count  = 0;
    ctx->rng_state       = 0;
    ctx->block_alloc     = block_alloc;
    ctx->alloc           = grow ? grow : &dvs_malloc_alloc;
    ctx->dynamic         = NULL;
    ctx->trail_top       = NULL;
    ctx->entail_top      = NULL;
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
    ctx->value_fails     = NULL;
    ctx->value_fails_cap = 0;
    ctx->value_fails_on  = 0;
    ctx->keep_on         = 0;
    ctx->box_val         = NULL;
    ctx->box_cap         = 0;
    ctx->box_state       = 0;
    ctx->box_skip        = NULL;
    ctx->enum_vars       = NULL;
    ctx->enum_vals       = NULL;
    ctx->enum_nv         = 0;
    ctx->enum_nsol       = 0;
    ctx->enum_cap        = 0;
    ctx->enum_key        = 0;
    ctx->enum_state      = 0;
    ctx->enum_redo       = 0;
    ctx->box_probe_acc   = 0;
    ctx->box_probe_n     = 0;
    ctx->var_order       = NULL;
    ctx->mod_links       = NULL;
    ctx->n_mod_links     = 0;
    ctx->mod_links_cap   = 0;
    ctx->mul_links       = NULL;
    ctx->n_mul_links     = 0;
    ctx->mul_links_cap   = 0;
    ctx->ext_memo        = NULL;
    ctx->n_ext_memo      = 0;
    ctx->ext_memo_cap    = 0;
    memset(&ctx->divisors, 0, sizeof ctx->divisors);
    ctx->divisors.alloc = ctx->alloc;
    ctx->dec_tier        = NULL;
    ctx->dec_tier_n      = 0;
    ctx->mul_count_max   = 0;
    ctx->mul_count_probed = 0;
    ctx->var_n_values    = NULL;
    ctx->unassigned_mask = 0;
    ctx->assumption_var_ids = NULL;
    ctx->assumption_priorities = NULL;
    ctx->n_assumptions   = 0;
    ctx->assumption_active_mask = 0;
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

    /* A fixed pool (no `grow`): a problem that does not fit is
     * DVS_COMPILE_NOMEM. */
    if (dvs_spool_init(&ctx->pool, pool_buf, pool_size, grow) != 0)
        return NULL;
    if (!block_alloc) {
        block_alloc = ctx->own_block_alloc = dvs_block_alloc_create(NULL, 64u << 10);
        if (!block_alloc) goto fail;
        ctx->block_alloc = block_alloc;
    }

    /* Level arrays for a search's MAX_DECISION_DEPTH plus the levels a
     * fixed pool's checkpoints hold; a growable context grows them as
     * checkpoints open (dvs_ctx_reserve_levels). A fixed pool also carves
     * its checkpoint stack; a growable one allocates it on first use. */
    if (_alloc_levels(ctx, MAX_DECISION_DEPTH + DVS_FIXED_CHECKPOINTS) != 0)
        goto fail;
    if (!grow) {
        uint32_t cp_ref = dvs_spool_alloc(&ctx->pool,
            DVS_FIXED_CHECKPOINTS * (uint32_t)sizeof(CheckpointMark),
            (uint32_t)_Alignof(CheckpointMark));
        if (cp_ref == EXPR_NULL) goto fail;
        ctx->checkpoints = (CheckpointMark *)dvs_spool_ptr(&ctx->pool, cp_ref);
        ctx->checkpoints_cap = DVS_FIXED_CHECKPOINTS;
    }
    ctx->max_depth = MAX_DECISION_DEPTH;

    if (block_alloc) {
        ctx->dynamic = dvs_stack_create(block_alloc);
        if (!ctx->dynamic) goto fail;
    }

    return ctx;
fail:
    _free_levels(ctx);
    if (ctx->dynamic) dvs_stack_destroy(ctx->dynamic);
    if (ctx->own_block_alloc) dvs_block_alloc_destroy(ctx->own_block_alloc);
    dvs_spool_destroy(&ctx->pool);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Growable per-variable and per-propagator tables                     */
/* ------------------------------------------------------------------ */

/* A table block: from ctx->alloc when the pool can grow, else carved from
 * the fixed pool (an old block is then simply left behind). In fixed mode a
 * block made inside a checkpoint scope would sit above the scope's pool mark
 * and be freed by its restore, so growth is refused there. */
static void *_tbl_alloc(dvs_ctx_t *ctx, size_t bytes) {
    if (ctx->pool.alloc) return DVS_ALLOC(ctx->alloc, bytes);
    if (ctx->n_checkpoints || bytes > 0xFFFF0000u) return NULL;
    uint32_t ref = dvs_spool_alloc(&ctx->pool, (uint32_t)bytes, 16u);
    return ref == EXPR_NULL ? NULL : dvs_spool_ptr(&ctx->pool, ref);
}

static void _tbl_free(dvs_ctx_t *ctx, void *p, size_t bytes) {
    if (p && ctx->pool.alloc) DVS_RELEASE(ctx->alloc, p, bytes);
}

/* Bytes per variable in the var block: var_n_values, vars, watcher_heads,
 * var_alias, dist_offsets, var_holes_head, in that order. */
#define VAR_ROW  (sizeof(uint64_t) + sizeof(Variable) + 4u * sizeof(uint32_t))
#define PROP_ROW (3u * sizeof(uint32_t))

/* The next capacity for `n` entries. With DVS_DEBUG_ALWAYS_MOVE small
 * tables grow one entry at a time, so every new variable moves them and a
 * pointer held across its creation shows up under ASan. */
static uint32_t _next_cap(uint32_t cap, uint32_t n) {
#ifdef DVS_DEBUG_ALWAYS_MOVE
    if (n <= 4096u) return n;
#endif
    uint64_t c = (uint64_t)cap * 2u;
    if (c < 64u) c = 64u;
    if (c < n) c = n;
    if (c > 0x3FFFFFFFu) c = n;
    return (uint32_t)c;
}

int dvs_ctx_reserve_vars(dvs_ctx_t *ctx, uint32_t n) {
    uint32_t old = ctx->n_vars_capacity;
    if (n <= old) return 0;
    if (n > 0x3FFFFFFFu) goto fail;
    uint32_t cap = _next_cap(old, n);
    uint8_t *b = (uint8_t *)_tbl_alloc(ctx, (size_t)cap * VAR_ROW);
    if (!b) goto fail;

    uint64_t *nv = (uint64_t *)b;
    Variable *vars = (Variable *)(nv + cap);
    uint32_t *wh = (uint32_t *)(vars + cap);
    uint32_t *al = wh + cap, *dist = al + cap, *holes = dist + cap;
    if (old) {
        memcpy(nv, ctx->var_n_values, old * sizeof *nv);
        memcpy(vars, ctx->vars, old * sizeof *vars);
        memcpy(wh, ctx->watcher_heads, old * sizeof *wh);
        memcpy(al, ctx->var_alias, old * sizeof *al);
        if (ctx->has_dists) memcpy(dist, ctx->dist_offsets, old * sizeof *dist);
        memcpy(holes, ctx->var_holes_head, old * sizeof *holes);
    }
    memset(nv + old, 0, (size_t)(cap - old) * sizeof *nv);
    memset(vars + old, 0, (size_t)(cap - old) * sizeof *vars);
    memset(wh + old, 0xFF, (size_t)(cap - old) * sizeof *wh);
    for (uint32_t i = old; i < cap; i++) al[i] = i;
    memset(dist + (ctx->has_dists ? old : 0), 0,
           (size_t)(cap - (ctx->has_dists ? old : 0)) * sizeof *dist);
    memset(holes + old, 0, (size_t)(cap - old) * sizeof *holes);

    if (ctx->phase_save && ctx->phase_save_cap < cap) {
        int64_t *ps = (int64_t *)DVS_RESIZE(ctx->alloc, ctx->phase_save,
                                            ctx->phase_save_cap * sizeof(int64_t),
                                            cap * sizeof(int64_t));
        if (!ps) { _tbl_free(ctx, b, (size_t)cap * VAR_ROW); goto fail; }
        memset(ps + ctx->phase_save_cap, 0,
               (cap - ctx->phase_save_cap) * sizeof(int64_t));
        ctx->phase_save = ps;
        ctx->phase_save_cap = cap;
    }
    if (ctx->lcg && lcg_reserve_vars((LCGCtx *)ctx->lcg, ctx->alloc, cap) != 0) {
        _tbl_free(ctx, b, (size_t)cap * VAR_ROW);
        goto fail;
    }

    _tbl_free(ctx, ctx->var_block, (size_t)old * VAR_ROW);
    ctx->var_block       = b;
    ctx->var_n_values    = nv;
    ctx->vars            = vars;
    ctx->watcher_heads   = wh;
    ctx->var_alias       = al;
    ctx->dist_offsets    = ctx->has_dists ? dist : NULL;
    ctx->var_holes_head  = holes;
    ctx->n_vars_capacity = cap;
    return 0;
fail:
    ctx->pool.overflow = 1;          /* compile and add report NOMEM */
    return -1;
}

void dvs_ctx_enable_dists(dvs_ctx_t *ctx) {
    if (ctx->has_dists || !ctx->var_block) return;
    uint32_t cap = ctx->n_vars_capacity;
    uint32_t *d = ctx->var_alias + cap;      /* the block's dist array */
    memset(d, 0, (size_t)cap * sizeof *d);
    ctx->dist_offsets = d;
    ctx->has_dists = 1;
}

int dvs_ctx_reserve_props(dvs_ctx_t *ctx, uint32_t n) {
    uint32_t old = ctx->n_prop_refs_capacity;
    if (n <= old) return 0;
    if (n > DVS_MAX_PROPS) goto fail;
    uint32_t cap = _next_cap(old, n);
    if (cap > DVS_MAX_PROPS) cap = DVS_MAX_PROPS;
    uint32_t *b = (uint32_t *)_tbl_alloc(ctx, (size_t)cap * PROP_ROW);
    if (!b) goto fail;
    uint32_t *refs = b, *guard = b + cap, *cid = guard + cap;
    if (old) {
        memcpy(refs, ctx->prop_refs, old * sizeof *refs);
        memcpy(guard, ctx->prop_guard_vars, old * sizeof *guard);
        memcpy(cid, ctx->prop_constraint_id, old * sizeof *cid);
    }
    memset(refs + old, 0xFF, (size_t)(cap - old) * sizeof *refs);
    memset(guard + old, 0xFF, (size_t)(cap - old) * sizeof *guard);
    memset(cid + old, 0, (size_t)(cap - old) * sizeof *cid);
    _tbl_free(ctx, ctx->prop_block, (size_t)old * PROP_ROW);
    ctx->prop_block           = b;
    ctx->prop_refs            = refs;
    ctx->prop_guard_vars      = guard;
    ctx->prop_constraint_id   = cid;
    ctx->n_prop_refs_capacity = cap;
    return 0;
fail:
    ctx->pool.overflow = 1;
    return -1;
}

int dvs_ctx_alloc_phase_save(dvs_ctx_t *ctx) {
    /* Sized to n_vars_capacity for incremental variable support. */
    uint32_t n = ctx->n_vars_capacity > 0 ? ctx->n_vars_capacity : ctx->n_vars;
    int64_t *ps = (int64_t *)DVS_ALLOC(ctx->alloc, n * sizeof(int64_t));
    if (!ps) return -1;
    ctx->phase_save     = ps;
    ctx->phase_save_cap = n;
    return 0;
}

#define SCOPE_LOG_ROW (sizeof(int64_t) + 2u * sizeof(uint32_t) + 1u)

int dvs_scope_log_bound(dvs_ctx_t *ctx, uint32_t var_id, int is_lb, int64_t bound) {
    if (ctx->n_scope_log == ctx->scope_log_cap) {
        /* One block: bound[cap], var[cap], depth[cap], is_lb[cap]. */
        uint32_t old = ctx->scope_log_cap, cap = old ? 2u * old : 64u;
        int64_t *nb = (int64_t *)DVS_ALLOC(ctx->alloc, (size_t)cap * SCOPE_LOG_ROW);
        if (!nb) return -1;
        uint32_t *nv = (uint32_t *)(nb + cap), *nd = nv + cap;
        uint8_t *nl = (uint8_t *)(nd + cap);
        if (old) {
            memcpy(nb, ctx->scope_log_bound, old * sizeof *nb);
            memcpy(nv, ctx->scope_log_var, old * sizeof *nv);
            memcpy(nd, ctx->scope_log_depth, old * sizeof *nd);
            memcpy(nl, ctx->scope_log_is_lb, old);
        }
        dvs_ctx_free(ctx, ctx->scope_log_block, (size_t)old * SCOPE_LOG_ROW);
        ctx->scope_log_block = nb;
        ctx->scope_log_bound = nb;
        ctx->scope_log_var   = nv;
        ctx->scope_log_depth = nd;
        ctx->scope_log_is_lb = nl;
        ctx->scope_log_cap   = cap;
    }
    uint32_t i = ctx->n_scope_log++;
    ctx->scope_log_var[i]   = var_id;
    ctx->scope_log_bound[i] = bound;
    ctx->scope_log_depth[i] = ctx->n_checkpoints;
    ctx->scope_log_is_lb[i] = is_lb ? 1 : 0;
    return 0;
}

void dvs_solver_destroy(dvs_ctx_t *ctx) {
    if (!ctx) return;
    dvs_oracle_on_destroy(ctx);
    if (ctx->lcg) {
        lcg_destroy((LCGCtx *)ctx->lcg);
        dvs_ctx_free(ctx, ctx->lcg, sizeof(LCGCtx));
        ctx->lcg = NULL;
    }
    if (ctx->dynamic) {
        dvs_stack_destroy(ctx->dynamic);
        ctx->dynamic = NULL;
    }
    dvs_ctx_free(ctx, ctx->keep_val, (size_t)ctx->keep_cap * (sizeof(int64_t) + 1u));
    ctx->keep_val = NULL; ctx->keep_state = NULL; ctx->keep_cap = 0;
    dvs_ctx_free(ctx, ctx->box_val, (size_t)ctx->box_cap * sizeof(int64_t));
    ctx->box_val = NULL; ctx->box_cap = 0;
    dvs_ctx_free(ctx, ctx->box_skip, ctx->box_skip_n);
    ctx->box_skip = NULL; ctx->box_skip_n = 0;
    dvs_ctx_free(ctx, ctx->enum_vars, (size_t)ctx->enum_nv * sizeof(uint32_t));
    ctx->enum_vars = NULL; ctx->enum_nv = 0;
    dvs_ctx_free(ctx, ctx->enum_vals, (size_t)ctx->enum_cap * sizeof(int64_t));
    ctx->enum_vals = NULL; ctx->enum_cap = 0; ctx->enum_nsol = 0;
    dvs_ctx_free(ctx, ctx->var_order, ctx->var_order_n);
    ctx->var_order = NULL; ctx->var_order_n = 0;
    dvs_ctx_free(ctx, ctx->value_fails, ctx->value_fails_cap);
    ctx->value_fails = NULL; ctx->value_fails_cap = 0;
    dvs_ctx_free(ctx, ctx->mod_links, (size_t)ctx->mod_links_cap * sizeof(DvsModLink));
    ctx->mod_links = NULL; ctx->n_mod_links = ctx->mod_links_cap = 0;
    dvs_ctx_free(ctx, ctx->mul_links, (size_t)ctx->mul_links_cap * sizeof(DvsMulLink));
    ctx->mul_links = NULL; ctx->n_mul_links = ctx->mul_links_cap = 0;
    dvs_ctx_free(ctx, ctx->ext_memo, (size_t)ctx->ext_memo_cap * sizeof(DvsExtMemo));
    ctx->ext_memo = NULL; ctx->n_ext_memo = ctx->ext_memo_cap = 0;
    dvs_divisors_free(&ctx->divisors);
    dvs_ctx_free(ctx, ctx->dec_tier, ctx->dec_tier_n);
    ctx->dec_tier = NULL; ctx->dec_tier_n = 0;
    if (ctx->phase_save)
        DVS_RELEASE(ctx->alloc, ctx->phase_save,
                    ctx->phase_save_cap * sizeof(int64_t));
    ctx->phase_save = NULL; ctx->phase_save_cap = 0;
    if (ctx->costguided)
        DVS_RELEASE(ctx->alloc, ctx->costguided, sizeof(CostGuidedCtx));
    ctx->costguided = NULL;
    dvs_ctx_free(ctx, ctx->scope_log_block, (size_t)ctx->scope_log_cap * SCOPE_LOG_ROW);
    ctx->scope_log_block = NULL;
    ctx->scope_log_var = NULL; ctx->scope_log_bound = NULL;
    ctx->scope_log_depth = NULL; ctx->scope_log_is_lb = NULL;
    ctx->n_scope_log = ctx->scope_log_cap = 0;
    _tbl_free(ctx, ctx->var_block, (size_t)ctx->n_vars_capacity * VAR_ROW);
    _free_levels(ctx);
    if (ctx->pool.alloc)
        dvs_ctx_free(ctx, ctx->checkpoints,
                     (size_t)ctx->checkpoints_cap * sizeof(CheckpointMark));
    ctx->checkpoints = NULL; ctx->checkpoints_cap = 0;
    _tbl_free(ctx, ctx->prop_block, (size_t)ctx->n_prop_refs_capacity * PROP_ROW);
    ctx->var_block = ctx->prop_block = NULL;
    dvs_guard_ix_free(ctx);
    dvs_spool_destroy(&ctx->pool);
    if (ctx->own_block_alloc) {
        dvs_block_alloc_destroy(ctx->own_block_alloc);
        ctx->own_block_alloc = ctx->block_alloc = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Accessor wrappers                                                   */
/* ------------------------------------------------------------------ */

uint32_t dvs_solver_n_vars(const dvs_ctx_t *ctx) {
    return ctx ? ctx->n_vars : 0;
}

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
    return dvs_spool_used(&ctx->pool);
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
