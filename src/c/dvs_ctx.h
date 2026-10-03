#ifndef DVS_CTX_H
#define DVS_CTX_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "dvs_variable.h"
#include "dvs_pool.h"
#include "dvs_stack.h"
#include "dvs_block_alloc.h"
#include "dvs_problem.h"
#include "dvs_trail.h"   /* TrailEntry, LevelMark */
#include "dvs_propagator.h"  /* PropQueue */
#include "dvs_search.h"      /* DecisionRecord */

/** Maximum decision depth for the embedded solver profile. The search pushes
 *  one level per decided variable, so this also caps the number of decision
 *  variables a single solve can carry natively — e.g. a symbolic array select
 *  frees every array element as a decision var, so a very large indexed array
 *  can exceed this. `_solver_solve_core` bails with DVS_SOLVE_TIMEOUT (clean defer)
 *  rather than overflowing the fixed decisions/level_marks arrays beyond this,
 *  so exceeding it degrades gracefully instead of an out-of-bounds write. */
#define MAX_DECISION_DEPTH  256u

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* dvs_ctx_t — the top-level solver context                            */
/*                                                                     */
/* Memory layout:                                                      */
/*   [ dvs_ctx_t header | dvs_pool_t header | static pool data ... ]   */
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
    uint32_t         n_clauses_at_cp;  /* learnt-clause count at checkpoint */
    TrailEntry      *trail_top;
    uint64_t         trail_count;
    dvs_stack_mark_t stack_mark;
    /* Phase B.1 step 5 (plumbing slice): SAT clause-arena position at
     * the time of the checkpoint. Zero today; populated once step 6
     * wires bbsolver to re-use a kissat instance across check-sat
     * calls. See LevelMark.sat_arena_top for the rationale. */
    size_t           sat_arena_top;
    /* The LCG had learnt nothing yet (or did not exist): a restore returns
     * it to its initial state (lcg_reset), so a context reused across
     * checkpoint/solve/restore searches exactly as a freshly compiled one. */
    uint8_t          lcg_pristine;
} CheckpointMark;

/* `r == a % b` with b a positive constant, all unsigned: the value picker
 * draws `a` with a remainder r still allows (see _pick_value). */
typedef struct {
    uint32_t a, r;
    uint32_t prop;     /* its propagator's id: dropped with it on restore */
    int64_t  b;
} DvsModLink;

typedef struct dvs_ctx_s {
    Variable          *vars;          /* pointer into static pool      */
    uint32_t           n_vars;        /* number of compiled variables  */
    uint32_t           n_vars_capacity; /* allocated size of vars array */
    uint32_t           decision_level;
    /* The decision level a solve started at: its root. Each open checkpoint
     * holds a level of its own, so a solve under checkpoints (and the pins
     * made in them) starts above 0, and nothing the search does may undo
     * what lies at or below this level. Set by the search at entry. */
    uint32_t           search_base;
    uint64_t           trail_count;
    uint64_t           conflict_count;
    uint64_t           rng_state;
    uint8_t            fair_pick;     /* dvs_solve_opts_t.fair_pick for this solve */
    /* Propagation guard (B61). A self-referential strict compare can climb one
     * value per round for 2^w rounds, growing the trail until memory runs out,
     * with no deadline check reached. dvs_solver_propagate checks prop_deadline
     * (set for the duration of a solve; 0 = none) and a trail-size cap, and on
     * either sets prop_aborted and returns PROP_CONFLICT. Whoever acts on a
     * propagation result must check prop_aborted first: that conflict proves
     * nothing, so the answer is a timeout, never unsat, and nothing is learnt
     * from it. */
    uint8_t            prop_aborted;
    double             prop_deadline;
    uint32_t           prop_ticks;
    uint8_t            bail_reason;   /* DVS_BAIL_*: why the last solve returned
                                       * DVS_SOLVE_TIMEOUT. Purely diagnostic -- a
                                       * silent `unknown` used to give no clue
                                       * whether CDCL ran out of time, ran out
                                       * of decision depth, or never compiled
                                       * the constraint at all. Read it via
                                       * DV_LOG=1 / --stats. */
    dvs_block_alloc_t *block_alloc;   /* source of dynamic blocks      */
    dvs_stack_t       *dynamic;       /* dynamic stack (trail etc.)    */
    TrailEntry        *trail_top;     /* newest trail entry, or NULL   */
    LevelMark         *level_marks;   /* array[MAX_DECISION_DEPTH]     */
    uint32_t           max_depth;     /* == MAX_DECISION_DEPTH         */
    uint32_t           n_props;       /* total propagators created     */
    uint32_t          *watcher_heads; /* array[n_vars] in static pool  */
    DecisionRecord    *decisions;     /* array[MAX_DECISION_DEPTH]     */
    int64_t           *phase_save;    /* last tried value per var      */
    /* A seeded solve's first draw per var, kept for the rest of the solve
     * (see _pick_value). keep_state: KEEP_NONE / KEEP_SET / KEEP_DEAD. */
    int64_t           *keep_val;
    uint8_t           *keep_state;
    uint32_t           keep_cap;
    uint8_t            keep_on;       /* keeping applies to this solve now */
    DvsModLink        *mod_links;
    uint32_t           n_mod_links;
    uint32_t           mod_links_cap;
    CheckpointMark     checkpoints[MAX_CHECKPOINTS];
    uint32_t           n_checkpoints;
    uint32_t          *prop_refs;     /* pool offsets of propagators   */
    uint32_t           n_prop_refs_capacity;
    uint32_t          *prop_guard_vars; /* guard var per prop, EXPR_NULL=unconditional */
    uint32_t          *prop_constraint_id; /* constraint_id for each propagator */
    PropQueue          queue;         /* 16-level priority queue       */
    uint64_t           unassigned_mask; /* bit i set = var i unassigned */
    Variable          *initial_vars;  /* saved copy at post-compile,
                                         * extended by incremental adds */
    uint32_t           initial_n_vars; /* entries of initial_vars in use */
    uint32_t           initial_vars_cap; /* entries allocated            */
    uint32_t          *assumption_var_ids;   /* var_id per assumption  */
    uint32_t          *assumption_priorities;/* priority per assumption*/
    uint32_t           n_assumptions;        /* number of assumptions  */
    uint64_t           assumption_active_mask;/* bit set = active      */
    /* Distribution constraint metadata (Sprint 7) */
    uint32_t          *dist_offsets;  /* pool offset per var -> DistMeta, 0=none */
    /* Per-variable hole list for randc exclusions (Sprint 8) */
    uint32_t          *var_holes_head; /* pool offset per var -> HoleEntry, 0=none */
    /* The size of the constant set an unconditional `x inside {...}` holds
     * x to (0 = none): a bound on its domain's size for variable selection,
     * which its bounds alone overstate by the holes between the values. */
    uint64_t          *var_n_values;
    /* Union-find alias table: var_alias[i] == representative of var i.
     * If var_alias[i] == i, the var is its own representative.
     * NULL if aliasing is not enabled. */
    uint32_t          *var_alias;
    uint32_t           incremental_capacity_hint; /* SMT2 frontend uses
                                       * to request a larger vars[] for
                                       * incremental mode (yosys-smtbmc) */
    /* LCG solver fields */
    uint32_t           current_prop_ref;  /* prop being fired (for trail) */
    uint32_t           conflict_prop_ref; /* prop that caused conflict    */
    uint32_t           conflict_clause_idx; /* learnt clause that went all-
                                             * false (EXPR_NULL if none); a
                                             * clause conflict empties no
                                             * domain, so LCG analysis must
                                             * seed from it, not from the
                                             * (stale) conflict_prop_ref. */
    uint32_t           current_trail_flags; /* TRAIL_FLAG_* bits to stamp
                                             * on the next trail entry;
                                             * caller sets, callee resets. */
    /* CDCL: heap-allocated LCG context, NULL when use_lcg=0. Lazily
     * created on first solve with use_lcg=1; freed in dvs_solver_destroy. */
    void              *lcg;               /* LCGCtx* (opaque to avoid header dep) */
    /* Bounds a checkpoint scope established that propagation cannot
     * re-derive, in order: pins (both bounds) and the compile-time bound
     * tightenings of constraints added in the scope. Each records the
     * variable, the bound (lower if is_lb), and the number of checkpoints open
     * when it was made. Inside a scope dvs_solver_reset rewinds the trail to
     * just after the innermost checkpoint and re-establishes the bounds made
     * since: their first propagation may have rested on a soft constraint the
     * solve has since relaxed. dvs_solver_restore(cp) drops those made after
     * checkpoint cp. malloc'd; freed by dvs_solver_destroy. */
    uint32_t          *scope_log_var;
    int64_t           *scope_log_bound;
    uint8_t           *scope_log_depth;
    uint8_t           *scope_log_is_lb;
    uint32_t           n_scope_log;
    uint32_t           scope_log_cap;
    /* Custom value selector hook (for cost-guided search) */
    int64_t          (*value_selector_fn)(struct dvs_ctx_s *, uint32_t, void *);
    void              *value_selector_data;
    dvs_pool_t         pool;          /* MUST be last field            */
    /* static pool data region follows immediately                      */
} dvs_ctx_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Compilation                                                         */
/* ------------------------------------------------------------------ */

/**
 * dvs_solver_compile: a variable wider than 64 bits was declared.
 *
 * The bounds/propagator engine cannot search tier-2 variables -- their bounds
 * cannot be tightened (trail_record_lb/ub refuse them) and the int64 bound
 * accessors have no tier-2 arm. Rather than return a context that reports
 * UNSAT on an unconstrained problem, or one that silently fails to enforce
 * constraints, compile declines. The bit-blasting engine (dvs_bbsolver)
 * handles these widths; escalate there.
 */
/* DVS_COMPILE_UNSUPPORTED_WIDTH is defined in dv_solve.h. */

/**
 * Install a custom value-selection callback (e.g. the cost-guided selector).
 * Passing fn=NULL restores the default selection order. (Defined in dvs_ctx.c;
 * called from dvs_costguided.c.)
 */
void dvs_solver_set_value_selector(dvs_ctx_t *ctx,
                               int64_t (*fn)(dvs_ctx_t *, uint32_t, void *),
                               void *data);

/* ------------------------------------------------------------------ */
/* Accessor wrappers (thin C functions for ctypes compatibility)       */
/*                                                                     */
/* Inline equivalents are also defined below for use in C code.       */
/* ------------------------------------------------------------------ */

/** Return the lower bound of a tier-0 variable as int32_t. */
int32_t  dvs_var_lo32(const dvs_ctx_t *ctx, uint32_t var_id);

/** Return the upper bound of a tier-0 variable as int32_t. */
int32_t  dvs_var_hi32(const dvs_ctx_t *ctx, uint32_t var_id);

/** Return the lower bound of a tier-0 or tier-1 variable as int64_t. */
int64_t  dvs_var_lo64(const dvs_ctx_t *ctx, uint32_t var_id);

/** Return the upper bound of a tier-0 or tier-1 variable as int64_t. */
int64_t  dvs_var_hi64(const dvs_ctx_t *ctx, uint32_t var_id);

/** Return a pointer to the Variable struct for var_id (for tests). */
Variable *dvs_solver_get_var(const dvs_ctx_t *ctx, uint32_t var_id);

/** Return bytes used in the static pool. */
uint32_t  dvs_ctx_pool_used(const dvs_ctx_t *ctx);

/** Return the current decision level. */
uint32_t  dvs_ctx_decision_level(const dvs_ctx_t *ctx);

/** Return the total trail entry count. */
uint64_t  dvs_ctx_trail_count(const dvs_ctx_t *ctx);

/** Return the constraint_id for propagator prop_idx, or 0 if unknown/out-of-range. */
uint32_t  dvs_prop_constraint_id(const dvs_ctx_t *ctx, uint32_t prop_idx);

/* ------------------------------------------------------------------ */
/* Inline accessors for use in C propagator code                      */
/* ------------------------------------------------------------------ */

static inline Variable *_ctx_var(const dvs_ctx_t *ctx, uint32_t var_id) {
    return &ctx->vars[var_id];
}

static inline int32_t var_lo32(const Variable *v) {
    return v->lo;
}

static inline int32_t var_hi32(const Variable *v) {
    return v->hi;
}

/* These read a TIER-1 WideBounds64 for every non-tier-0 variable. That is
 * currently exhaustive: dvs_solver_compile returns DVS_COMPILE_UNSUPPORTED_WIDTH
 * rather than creating a tier-2 variable, so no WideBoundsN reaches here.
 *
 * The assert matters because the failure mode without it is silent and
 * confusing rather than loud: WideBoundsN begins with {uint32_t n_limbs;
 * uint32_t _pad;}, so reading it as a WideBounds64 returns the LIMB COUNT as
 * the lower bound. A 65-bit variable with no constraints at all came back with
 * the domain [2, 0] -- empty -- and the solve reported UNSAT. If tier-2 search
 * is ever implemented (see _init_tier2), these need a real tier-2 arm; the
 * assert is what makes that requirement impossible to miss. */
static inline int64_t var_lo64(const dvs_ctx_t *ctx, const Variable *v) {
    if (VAR_IS_TIER0(v->flags)) {
        return (v->flags & VAR_SIGNED) ? (int64_t)v->lo
                                       : (int64_t)(uint32_t)v->lo;
    }
    assert(!VAR_IS_TIER2(v->flags) && "tier-2 bounds are WideBoundsN");
    const WideBounds64 *wb =
        (const WideBounds64 *)dvs_pool_ptr(&ctx->pool, v->holes_offset);
    return wb->lo;
}

static inline int64_t var_hi64(const dvs_ctx_t *ctx, const Variable *v) {
    if (VAR_IS_TIER0(v->flags)) {
        return (v->flags & VAR_SIGNED) ? (int64_t)v->hi
                                       : (int64_t)(uint32_t)v->hi;
    }
    assert(!VAR_IS_TIER2(v->flags) && "tier-2 bounds are WideBoundsN");
    const WideBounds64 *wb =
        (const WideBounds64 *)dvs_pool_ptr(&ctx->pool, v->holes_offset);
    return wb->hi;
}

/* ------------------------------------------------------------------ */
/* Sign-aware ordering on raw 64-bit bound patterns.                    */
/*                                                                      */
/* A bound (lo/hi/literal) is stored as int64_t but, for an *unsigned*  */
/* variable, that int64_t is the bit pattern of a uint64_t value: a     */
/* full-width unsigned-64 value > INT64_MAX appears negative. So every  */
/* ORDER-sensitive operation on bound values must interpret them per    */
/* the variable's signedness. Arithmetic on the patterns (+1/-1, the    */
/* span `hi-lo`) is already correct modulo 2^64; only the comparisons   */
/* below were signed-only, which capped unsigned vars at INT64_MAX      */
/* (the "2^63 cliff" — see dv_solve_phaseG_primary_completeness_plan).  */
/* ------------------------------------------------------------------ */
static inline int var_b_lt(const Variable *v, int64_t a, int64_t b) {
    /* Unsigned ordering is needed ONLY when the domain can exceed INT64_MAX,
     * i.e. an unsigned variable of width >= 64. For narrower unsigned vars the
     * domain is [0, 2^w-1] with 2^w-1 <= INT64_MAX, so a signed compare orders
     * them identically AND — crucially — correctly treats a *negative
     * intermediate* bound (e.g. a sum propagator's `r_lo - others_hi`, which is
     * computed in signed int64 and can go below 0) as "below the domain". An
     * unconditional unsigned compare would misread that -4 as 2^64-4 and
     * fabricate a conflict. (Wide unsigned *arithmetic* on width-64 vars, where
     * a genuine 2^64-4 and an intermediate -4 are indistinguishable bit
     * patterns, stays out of scope — see Phase G §3.1 G-1d.) */
    if (!(v->flags & VAR_SIGNED) && v->width >= 64)
        return (uint64_t)a < (uint64_t)b;
    return a < b;
}
static inline int var_b_gt(const Variable *v, int64_t a, int64_t b) {
    return var_b_lt(v, b, a);
}
static inline int64_t var_b_min(const Variable *v, int64_t a, int64_t b) {
    return var_b_lt(v, a, b) ? a : b;
}
static inline int64_t var_b_max(const Variable *v, int64_t a, int64_t b) {
    return var_b_lt(v, a, b) ? b : a;
}

/* ------------------------------------------------------------------ */
/* Representable range of a variable, from its width and signedness.    */
/* Used to guard bound tightening against edge-crossing constants       */
/* (e.g. `v < 0` on an unsigned var, or `v > INT64_MAX` on a signed     */
/* 64-bit var where `cv + 1` would overflow).                           */
/* ------------------------------------------------------------------ */

/** Smallest value representable by `v`.  Always fits in int64_t. */
static inline int64_t var_repr_min(const Variable *v) {
    if (v->flags & VAR_SIGNED) {
        uint16_t w = v->width;
        if (w >= 64) return INT64_MIN;
        return -((int64_t)1 << (w - 1));
    }
    return 0;  /* unsigned */
}

/** Largest value representable by `v`, returned as a 64-bit pattern (compare
 *  it with the sign-aware `var_b_*` helpers, never a bare signed `<`/`>`).
 *  For unsigned width 64 this is 2^64-1 (bit pattern -1); for unsigned
 *  width > 64 (tier-2) the true max exceeds 64 bits and is handled by the
 *  wide path, so INT64_MAX is kept as the int64-domain ceiling for guards. */
static inline int64_t var_repr_max(const Variable *v) {
    uint16_t w = v->width;
    if (v->flags & VAR_SIGNED) {
        if (w >= 64) return INT64_MAX;
        return ((int64_t)1 << (w - 1)) - 1;
    }
    if (w == 64) return (int64_t)UINT64_MAX;  /* 2^64-1 as a uint64 bit pattern */
    if (w > 64)  return INT64_MAX;            /* tier-2: wide path owns the bound */
    return (int64_t)(((uint64_t)1 << w) - 1); /* w == 63: 1 << 63 overflows int64 */
}

static inline const uint64_t *var_lo_wide(const dvs_ctx_t *ctx,
                                           const Variable *v) {
    const WideBoundsN *wn =
        (const WideBoundsN *)dvs_pool_ptr(&ctx->pool, v->holes_offset);
    return (const uint64_t *)(wn + 1);
}

static inline const uint64_t *var_hi_wide(const dvs_ctx_t *ctx,
                                           const Variable *v) {
    const WideBoundsN *wn =
        (const WideBoundsN *)dvs_pool_ptr(&ctx->pool, v->holes_offset);
    return (const uint64_t *)(wn + 1) + wn->n_limbs;
}

/**
 * Compute a backjump level using trail reasons.
 *
 * Returns the decision level the search should backjump to after the most
 * recent PROP_CONFLICT. The conflicting propagator is read from
 * ctx->conflict_prop_ref.  Falls back to (decision_level - 1) when no
 * useful lower-level dependency is found.  See dvs_conflict.c for the
 * algorithm.
 */
uint32_t analyze_conflict(dvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

/* Record a bound established in the current checkpoint scope (see
 * dvs_ctx_t.scope_log_*). Returns -1 when out of memory. */
int dvs_scope_log_bound(dvs_ctx_t *ctx, uint32_t var_id, int is_lb, int64_t bound);

#endif /* DVS_CTX_H */
