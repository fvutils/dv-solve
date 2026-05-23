#ifndef ZSP_CTX_H
#define ZSP_CTX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "zsp_variable.h"
#include "zsp_pool.h"
#include "zsp_stack.h"
#include "zsp_block_alloc.h"
#include "zsp_problem.h"
#include "zsp_trail.h"   /* TrailEntry, LevelMark */
#include "zsp_propagator.h"  /* PropQueue */
#include "zsp_search.h"      /* DecisionRecord */

/** Maximum decision depth for the embedded solver profile. */
#define MAX_DECISION_DEPTH  256u

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* SolveCtx — the top-level solver context                            */
/*                                                                     */
/* Memory layout:                                                      */
/*   [ SolveCtx header | zsp_pool_t header | static pool data ... ]   */
/*                      ^-- &ctx->pool                                */
/*                                                                     */
/* The static pool is used for:                                        */
/*   - Variable[] array                                               */
/*   - WideBounds64/WideBoundsN for tier-1/2 variables                */
/*   - Propagators (Phase 6)                                          */
/*                                                                     */
/* The dynamic stack is backed by block_alloc and holds:              */
/*   - Trail entries (Phase 5)                                        */
/*   - Decision-level markers (Phase 5)                               */
/*   - Scratch allocations during search (Phase 7)                    */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* CheckpointMark -- saved state for incremental checkpoint/restore   */
/* ------------------------------------------------------------------ */
#define MAX_CHECKPOINTS 32u

typedef struct {
    uint32_t         decision_level;
    uint32_t         n_vars_at_cp;
    uint32_t         n_props_at_cp;
    uint32_t         _cp_pad;
    TrailEntry      *trail_top;
    uint64_t         trail_count;
    zsp_stack_mark_t stack_mark;
} CheckpointMark;

typedef struct SolveCtx {
    Variable          *vars;          /* pointer into static pool      */
    uint32_t           n_vars;        /* number of compiled variables  */
    uint32_t           n_vars_capacity; /* allocated size of vars array */
    uint32_t           decision_level;
    uint64_t           trail_count;
    uint64_t           conflict_count;
    uint64_t           rng_state;
    zsp_block_alloc_t *block_alloc;   /* source of dynamic blocks      */
    zsp_stack_t       *dynamic;       /* dynamic stack (trail etc.)    */
    TrailEntry        *trail_top;     /* newest trail entry, or NULL   */
    LevelMark         *level_marks;   /* array[MAX_DECISION_DEPTH]     */
    uint32_t           max_depth;     /* == MAX_DECISION_DEPTH         */
    uint32_t           n_props;       /* total propagators created     */
    uint32_t          *watcher_heads; /* array[n_vars] in static pool  */
    DecisionRecord    *decisions;     /* array[MAX_DECISION_DEPTH]     */
    int64_t           *phase_save;    /* last tried value per var      */
    CheckpointMark     checkpoints[MAX_CHECKPOINTS];
    uint32_t           n_checkpoints;
    uint32_t          *prop_refs;     /* pool offsets of propagators   */
    uint32_t           n_prop_refs_capacity;
    uint32_t          *prop_guard_vars; /* guard var per prop, EXPR_NULL=unconditional */
    uint32_t          *prop_constraint_id; /* constraint_id for each propagator */
    PropQueue          queue;         /* 16-level priority queue       */
    uint64_t           unassigned_mask; /* bit i set = var i unassigned */
    Variable          *initial_vars;  /* saved copy at post-compile     */
    uint32_t           initial_n_vars; /* n_vars at compile time        */
    uint32_t          *assumption_var_ids;   /* var_id per assumption  */
    uint32_t          *assumption_priorities;/* priority per assumption*/
    uint32_t           n_assumptions;        /* number of assumptions  */
    uint64_t           assumption_active_mask;/* bit set = active      */
    /* Distribution constraint metadata (Sprint 7) */
    uint32_t          *dist_offsets;  /* pool offset per var -> DistMeta, 0=none */
    /* Per-variable hole list for randc exclusions (Sprint 8) */
    uint32_t          *var_holes_head; /* pool offset per var -> HoleEntry, 0=none */
    /* Union-find alias table: var_alias[i] == representative of var i.
     * If var_alias[i] == i, the var is its own representative.
     * NULL if aliasing is not enabled. */
    uint32_t          *var_alias;
    uint32_t           _pad;          /* keep pool 16-byte aligned     */
    /* LCG solver fields */
    uint32_t           current_prop_ref;  /* prop being fired (for trail) */
    uint32_t           conflict_prop_ref; /* prop that caused conflict    */
    /* CDCL: heap-allocated LCG context, NULL when use_lcg=0. Lazily
     * created on first solve with use_lcg=1; freed in solver_destroy. */
    void              *lcg;               /* LCGCtx* (opaque to avoid header dep) */
    /* Custom value selector hook (for cost-guided search) */
    int64_t          (*value_selector_fn)(struct SolveCtx *, uint32_t, void *);
    void              *value_selector_data;
    zsp_pool_t         pool;          /* MUST be last field            */
    /* static pool data region follows immediately                      */
} SolveCtx;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * Create a solver context in a caller-supplied static buffer.
 *
 * @param static_buf   Buffer for the static segment (variables, propagators…).
 *                     Must be suitably aligned.
 * @param static_size  Size of static_buf in bytes.
 * @param block_alloc  Block allocator for the dynamic stack.  Must outlive
 *                     the context.  May be NULL (dynamic stack disabled).
 * @return  Pointer to the initialised context (== static_buf), or NULL.
 */
SolveCtx *solver_create(void *static_buf, size_t static_size,
                         zsp_block_alloc_t *block_alloc);

/**
 * Destroy the context: tear down the dynamic stack and zero the struct.
 * The static buffer is caller-managed; solver_destroy does not free it.
 */
void solver_destroy(SolveCtx *ctx);

/* ------------------------------------------------------------------ */
/* Compilation                                                         */
/* ------------------------------------------------------------------ */

/**
 * Compile a SolveProblem into the context's static pool.
 *
 * Walks the VarSpec linked list in `sp`, allocates Variable[] in the
 * static pool, and initialises each variable's domain according to its
 * tier (0/1/2).
 *
 * The SolveProblem `sp` is consumed but not freed; the caller should
 * reset or free the problem's buffer after this call.
 *
 * @return  0 on success, -1 if the static pool is too small.
 */
int solver_compile(SolveCtx *ctx, SolveProblem *sp);

/**
 * Add constraints from an auxiliary SolveProblem to an already-compiled context.
 *
 * @return 0 on success, -1 if a new variable exceeds capacity,
 *         -2 if UNSAT detected during propagation.
 */
int solver_add_constraint(SolveCtx *ctx, SolveProblem *aux_sp);

/**
 * Validate that the current variable assignment satisfies every
 * top-level constraint in `sp`. Intended to be called after solver_solve
 * returns SOLVE_OK as a sanity check against silently-dropped or
 * incorrectly-compiled constraints.
 *
 * Evaluates each ConstraintSpec root expression under the assignment;
 * a 0 value is treated as a violation. Constraints involving constructs
 * the evaluator can't handle (arrays, sums, countones, clog2, in_set,
 * in_range) are skipped — they don't count as violations or successes.
 *
 * @return 0 if every evaluated constraint was satisfied; positive count
 *         of violations otherwise. Diagnostic messages for each violation
 *         are written to `err` (skipped when err is NULL).
 */
int solver_validate_model(SolveCtx *ctx, SolveProblem *sp, FILE *err);

/**
 * Save a checkpoint of the current solver state.
 * @return Checkpoint index (0-based), or -1 if MAX_CHECKPOINTS exceeded.
 */
int solver_checkpoint(SolveCtx *ctx);

/**
 * Restore solver state to a previously saved checkpoint.
 * Undoes domain changes, deactivates propagators added after checkpoint.
 */
void solver_restore(SolveCtx *ctx, uint32_t cp);

/* ------------------------------------------------------------------ */
/* Accessor wrappers (thin C functions for ctypes compatibility)       */
/*                                                                     */
/* Inline equivalents are also defined below for use in C code.       */
/* ------------------------------------------------------------------ */

/** Return the lower bound of a tier-0 variable as int32_t. */
int32_t  zsp_var_lo32(const SolveCtx *ctx, uint32_t var_id);

/** Return the upper bound of a tier-0 variable as int32_t. */
int32_t  zsp_var_hi32(const SolveCtx *ctx, uint32_t var_id);

/** Return the lower bound of a tier-0 or tier-1 variable as int64_t. */
int64_t  zsp_var_lo64(const SolveCtx *ctx, uint32_t var_id);

/** Return the upper bound of a tier-0 or tier-1 variable as int64_t. */
int64_t  zsp_var_hi64(const SolveCtx *ctx, uint32_t var_id);

/** Return a pointer to the Variable struct for var_id (for tests). */
Variable *solver_get_var(const SolveCtx *ctx, uint32_t var_id);

/** Return bytes used in the static pool. */
uint32_t  zsp_ctx_pool_used(const SolveCtx *ctx);

/** Return the current decision level. */
uint32_t  zsp_ctx_decision_level(const SolveCtx *ctx);

/** Return the total trail entry count. */
uint64_t  zsp_ctx_trail_count(const SolveCtx *ctx);

/** Return the constraint_id for propagator prop_idx, or 0 if unknown/out-of-range. */
uint32_t  zsp_prop_constraint_id(const SolveCtx *ctx, uint32_t prop_idx);

/* ------------------------------------------------------------------ */
/* Inline accessors for use in C propagator code                      */
/* ------------------------------------------------------------------ */

static inline Variable *_ctx_var(const SolveCtx *ctx, uint32_t var_id) {
    return &ctx->vars[var_id];
}

static inline int32_t var_lo32(const Variable *v) {
    return v->lo;
}

static inline int32_t var_hi32(const Variable *v) {
    return v->hi;
}

static inline int64_t var_lo64(const SolveCtx *ctx, const Variable *v) {
    if (VAR_IS_TIER0(v->flags)) {
        return (v->flags & VAR_SIGNED) ? (int64_t)v->lo
                                       : (int64_t)(uint32_t)v->lo;
    }
    const WideBounds64 *wb =
        (const WideBounds64 *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
    return wb->lo;
}

static inline int64_t var_hi64(const SolveCtx *ctx, const Variable *v) {
    if (VAR_IS_TIER0(v->flags)) {
        return (v->flags & VAR_SIGNED) ? (int64_t)v->hi
                                       : (int64_t)(uint32_t)v->hi;
    }
    const WideBounds64 *wb =
        (const WideBounds64 *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
    return wb->hi;
}

static inline const uint64_t *var_lo_wide(const SolveCtx *ctx,
                                           const Variable *v) {
    const WideBoundsN *wn =
        (const WideBoundsN *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
    return (const uint64_t *)(wn + 1);
}

static inline const uint64_t *var_hi_wide(const SolveCtx *ctx,
                                           const Variable *v) {
    const WideBoundsN *wn =
        (const WideBoundsN *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
    return (const uint64_t *)(wn + 1) + wn->n_limbs;
}

/**
 * Compute a backjump level using trail reasons.
 *
 * Returns the decision level the search should backjump to after the most
 * recent PROP_CONFLICT. The conflicting propagator is read from
 * ctx->conflict_prop_ref.  Falls back to (decision_level - 1) when no
 * useful lower-level dependency is found.  See zsp_conflict.c for the
 * algorithm.
 */
uint32_t analyze_conflict(SolveCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_CTX_H */
