#ifndef DVS_CTX_H
#define DVS_CTX_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "dvs_variable.h"
#include "dvs_pool.h"
#include "dvs_spool.h"
#include "dvs_stack.h"
#include "dvs_block_alloc.h"
#include "dvs_problem.h"
#include "dvs_trail.h"   /* TrailEntry, LevelMark */
#include "dvs_propagator.h"  /* PropQueue */
#include "dvs_search.h"      /* DecisionRecord */
#include "dvs_stackinfo.h"   /* dvs_stack_guard_t */
#include "dvs_numth.h"       /* DvsDivisors */

/** Decision depth a search may use beyond one level per variable, counted
 *  from its root (the level the open checkpoints hold). The search pushes
 *  one level per decided variable, and splits push more. A growable context
 *  sizes its level arrays at the start of each search to the root plus
 *  n_vars plus this (up to the 16-bit level limit); a fixed pool keeps the
 *  root plus this. Past the limit the search bails with DVS_SOLVE_TIMEOUT. */
#define MAX_DECISION_DEPTH  256u

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* dvs_ctx_t — the top-level solver context                            */
/*                                                                     */
/* Memory layout:                                                      */
/*   [ dvs_ctx_t header | pool's fixed region ... ]  (caller's buffer) */
/*   + pool slots from ctx->alloc as the pool grows (dvs_spool.h)      */
/*                                                                     */
/* The pool (ctx->pool, offsets via dvs_spool_ptr) is used for:       */
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
/* A context with a fixed pool holds at most DVS_FIXED_CHECKPOINTS open
 * checkpoints; a growable one grows its checkpoint stack (and its level
 * arrays) up to DVS_MAX_CHECKPOINTS. Each open checkpoint holds a decision
 * level, and trail entries record their level in 16 bits. */
#define DVS_FIXED_CHECKPOINTS 32u
#define DVS_MAX_CHECKPOINTS   (0xFFFFu - MAX_DECISION_DEPTH - 1u)

typedef struct {
    uint32_t         decision_level;
    uint32_t         n_vars_at_cp;
    uint32_t         n_props_at_cp;
    uint32_t         n_clauses_at_cp;  /* learnt-clause count at checkpoint */
    TrailEntry      *trail_top;
    uint64_t         trail_count;
    EntailEntry     *entail_top;
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
    /* The pool position: dvs_solver_restore frees everything the scope
     * allocated (its propagators, its variables' wide bounds, a regrown
     * reset snapshot), so push/pop sessions hold pool use flat. */
    dvs_spool_mark_t pool_mark;
    /* The reset snapshot as it stood: an add in the scope extends it, and
     * may regrow it into memory the restore frees. */
    Variable        *initial_vars;
    uint32_t         initial_n_vars;
    uint32_t         initial_vars_cap;
} CheckpointMark;

/* `r == a % b` with b a positive constant, all unsigned: the value picker
 * draws `a` with a remainder r still allows (see _pick_value). */
typedef struct {
    uint32_t a, r;
    uint32_t prop;     /* its propagator's id: dropped with it on restore */
    int64_t  b;
} DvsModLink;

/* `r == a * b` (a == b: a square), recorded for the value picker and the
 * decision order (see DvsMulLink in dvs_prop_templates.c). */
typedef struct {
    uint32_t r, a, b;
    uint32_t prop;     /* its propagator's id: dropped with it on restore */
    uint8_t  width;
    uint8_t  _pad[3];
} DvsMulLink;

/* A variable compile made for a value-preserving extension of `src`: an
 * aux var that IS src's value at width `to` (see _value_to_var). Kept so a
 * second extension of the same value reuses it, which is what lets `c * c`
 * over extended operands be seen as a square. */
typedef struct {
    uint32_t src, r;
    uint32_t to;
    uint8_t  sgn;
    uint8_t  _pad;
} DvsExtMemo;

typedef struct dvs_ctx_s {
    Variable          *vars;          /* pointer into static pool      */
    uint32_t           n_vars;        /* number of compiled variables  */
    uint32_t           n_vars_capacity; /* entries in every per-variable
                                       * table; grown by
                                       * dvs_ctx_reserve_vars        */
    uint32_t           decision_level;
    /* The decision level a solve started at: its root. Each open checkpoint
     * holds a level of its own, so a solve under checkpoints (and the pins
     * made in them) starts above 0, and nothing the search does may undo
     * what lies at or below this level. Set by the search at entry. */
    uint32_t           search_base;
    /* A solve sealed search_base and left its search on the trail: the
     * decisions of a model (kept for get_value), or of a search that timed
     * out. That state is not the caller's: the next call that builds on the
     * caller's state (solve, pin, checkpoint, add, exclude, propagate_only)
     * first backtracks to search_base (dvs_search_discard). Cleared by reset
     * and restore, which undo it themselves. Left in place, a solve after a
     * timed-out solve sealed the dead search as its root and answered unsat
     * on a satisfiable problem (B109). */
    uint8_t            search_sealed;
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
    /* Thrash memory for this compiled problem (see _solver_solve). A reset
     * keeps it, so a randomize() loop that re-solves the problem pays a
     * search that cannot finish once, not on every call; a compile clears it. */
    uint64_t           prop_fires;    /* propagator firings, ever: the work count */
    uint64_t           ok_fires_max;  /* most firings a solve that answered
                                       * (sat or unsat) needed               */
    uint64_t           thrash_limit;  /* prop_fires past which this solve
                                       * gives up, 0: none                   */
    uint8_t            thrashed;      /* a default-budget solve gave up       */
    uint8_t            thrash_hit;    /* this solve reached thrash_limit      */
    uint64_t           problem_fp;    /* the compiled problem's fingerprint, the
                                       * key of the process-wide memory; 0 once
                                       * an incremental add changed it        */
    dvs_block_alloc_t *block_alloc;   /* source of dynamic blocks      */
    uint8_t            own_header;    /* made by dvs_solver_new: the
                                       * header is from ctx->alloc      */
    dvs_block_alloc_t *own_block_alloc; /* made by dvs_solver_create when
                                       * given none (fixed pool): the
                                       * trail stack still needs blocks */
    dvs_alloc_t       *alloc;         /* source of the context's heap
                                       * tables: block_alloc's backing
                                       * allocator, else dvs_malloc_alloc */
    dvs_spool_mark_t   pool_floor;    /* pool position after compile: a
                                       * restore never frees below it   */
    dvs_stack_t       *dynamic;       /* dynamic stack (trail etc.)    */
    TrailEntry        *trail_top;     /* newest trail entry, or NULL   */
    EntailEntry       *entail_top;    /* newest entailment record (dvs_trail.h) */
    LevelMark         *level_marks;   /* array[level_cap]              */
    uint32_t           max_depth;     /* the search's level limit: its
                                       * root + MAX_DECISION_DEPTH     */
    uint32_t           level_cap;     /* entries in level_marks and
                                       * decisions; grown by
                                       * dvs_ctx_reserve_levels       */
    uint32_t           n_props;       /* total propagators created     */
    uint32_t          *watcher_heads; /* per variable: first watcher  */
    DecisionRecord    *decisions;     /* array[level_cap]              */
    int64_t           *phase_save;    /* last tried value per var;
                                       * from ctx->alloc, created lazily */
    uint32_t           phase_save_cap;
    /* Item 3c: each var's compile-time-merged congruence fact
     * `v % cong_m[v] == cong_r[v]` (0 <= cong_r[v] < cong_m[v]), the same
     * (R,M) `_compile_congruence_facts` (dvs_compile.c) CRT-merges per var
     * to catch a contradiction -- persisted here so _pick_value can draw
     * `R + M*k` directly instead of the per-link fixup (_pick_mod_aligned).
     * cong_m[v] == 0: no fact for v. Same lazy-alloc/grow pattern as
     * phase_save (from ctx->alloc, NULL until first use); unlike
     * dist_offsets/var_holes_head it is not part of the shared var_block
     * since it is populated once at compile and never touched afterward. */
    uint64_t          *cong_m;
    int64_t           *cong_r;
    uint32_t           cong_cap;
    /* Item 4a: each var's compile-time `$countones` chain fact -- the set
     * of bit positions `countones_bits[v]` (mask) a left-deep
     * extract+zero_extend+add chain sums, equated to a constant popcount
     * `countones_n[v]`, as `_compile_countones_facts` (dvs_compile.c)
     * detects from the constraint AST (the accept-criteria benches never
     * use EXPR_COUNTONES/prop_add_countones_32 -- see that function's
     * header comment). countones_bits[v] == 0: no fact for v. Same
     * lazy-alloc/grow pattern as cong_m/cong_r above. */
    uint64_t          *countones_bits;
    uint32_t          *countones_n;
    uint32_t           countones_cap;
    /* Item 4a: each var's compile-time "which bits are pinned, and to
     * what" fact from a top-level `EQ(EXTRACT(v,hi,lo), CONST)` conjunct
     * (independent of countones_bits/n above -- `pop32`'s nibble pin is a
     * second, separate conjunct, not part of the add chain). Bit `i` of
     * known_bits_mask[v] set means bit `i` of known_bits_val[v] is that
     * bit's pinned value; known_bits_mask[v] == 0: no fact for v. Same
     * lazy-alloc/grow pattern as cong_m/cong_r above. */
    uint64_t          *known_bits_mask;
    uint64_t          *known_bits_val;
    uint32_t           known_bits_cap;
    /* A seeded solve's first draw per var, kept for the rest of the solve
     * (see _pick_value). keep_state: KEEP_NONE / KEEP_SET / KEEP_DEAD. */
    int64_t           *keep_val;
    uint8_t           *keep_state;
    uint32_t           keep_cap;
    uint8_t            keep_on;       /* keeping applies to this solve now */
    /* A seeded solve's count of failed value decisions per var (saturating),
     * which gates bound decisions (see _bound_decision). */
    uint8_t           *value_fails;
    uint32_t           value_fails_cap;
    uint8_t            value_fails_on; /* counting applies to this solve now */
    /* Box-rejection tier (see _box_sample): drawn values, and whether the
     * tier suits this problem (0 not yet probed, 1 yes, 2 no). */
    int64_t           *box_val;
    uint32_t           box_cap;
    uint8_t            box_state;
    uint8_t           *box_skip;      /* [n_decl_vars] 1: defined by an
                                       * equality (`v == <expr>`): not drawn
                                       * by the box tier, and decided by the
                                       * search only after every other var */
    uint32_t           box_skip_n;    /* its length                    */
    /* Enumeration tier (see _enum_sample): every solution of a problem the
     * box tier gives up on, when it has few, so a solve can draw one
     * uniformly. Valid for the root state hashed in enum_key. */
    uint32_t          *enum_vars;     /* [enum_nv] the enumerated vars */
    int64_t           *enum_vals;     /* [enum_nsol][enum_nv]          */
    uint32_t           enum_nv;
    uint32_t           enum_nsol;
    uint32_t           enum_cap;      /* int64 slots in enum_vals      */
    uint64_t           enum_key;
    uint8_t            enum_state;    /* 0 not yet tried, 1 on, 2 off  */
    uint8_t            box_probe_acc; /* the box probe's accepts, and   */
    uint8_t            box_probe_n;   /* tries: an estimate of how many
                                       * solutions there are            */
    uint8_t            enum_redo;     /* rebuilds left on a new root   */
    /* The list's root, for a later root that can only have lost solutions
     * (_enum_covers): every var's root bounds [enum_root_n][2], the
     * propagator count and restore_gen then. enum_root_n == 0: not kept. */
    int64_t           *enum_root;
    uint32_t           enum_root_n;
    uint32_t           enum_root_cap; /* vars enum_root has room for   */
    uint32_t           enum_n_props;
    uint32_t           enum_n_decl;
    uint32_t           enum_restore_gen;
    uint32_t           restore_gen;   /* dvs_solver_restore calls: each
                                       * removes propagators            */
    uint32_t           n_decl_vars;   /* vars the problem declared; compile
                                       * adds its own after them         */
    uint8_t           *var_order;     /* [n_decl_vars] solve-order stage
                                       * (VarSpec.order), NULL: none     */
    uint32_t           var_order_n;   /* its length                    */
    DvsModLink        *mod_links;
    uint32_t           n_mod_links;
    uint32_t           mod_links_cap;
    DvsMulLink        *mul_links;
    uint32_t           n_mul_links;
    uint32_t           mul_links_cap;
    DvsExtMemo        *ext_memo;
    uint32_t           n_ext_memo;
    uint32_t           ext_memo_cap;
    DvsDivisors        divisors;      /* last fixed product factored     */
    uint8_t           *dec_tier;      /* [dec_tier_n] decision tier from the
                                       * multiply links; NULL: all equal  */
    uint32_t           dec_tier_n;
    uint64_t           mul_count_max; /* largest leftover-product count
                                       * seen (_mul_count_accept); kept
                                       * across solves of the problem    */
    uint8_t            mul_count_probed;
    CheckpointMark    *checkpoints;   /* array[checkpoints_cap]: from
                                       * the fixed pool, else ctx->alloc */
    uint32_t           n_checkpoints;
    uint32_t           checkpoints_cap;
    uint32_t          *prop_refs;     /* pool offsets of propagators   */
    uint32_t           n_prop_refs_capacity;
    uint32_t          *prop_guard_vars; /* guard var per prop, EXPR_NULL=unconditional */
    /* prop_guard_vars inverted, for waking a guard's propagators without a
     * scan of every propagator (_wake_var): guard_ix[v]..guard_ix[v+1] index
     * guard_ix_props, the prop ids guarded by v in id order. Built lazily
     * from ctx->alloc; rebuilt when the var or prop count it was built for
     * changes or a guard is written (guard_ix_dirty). */
    uint32_t          *guard_ix;
    uint32_t          *guard_ix_props;
    uint32_t           guard_ix_nvars;
    uint32_t           guard_ix_nprops;
    size_t             guard_ix_bytes;
    uint8_t            guard_ix_dirty;
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
    /* The per-variable tables (vars, var_n_values, watcher_heads,
     * var_alias, dist_offsets, var_holes_head) share one block, and the
     * per-propagator ones (prop_refs, prop_guard_vars, prop_constraint_id)
     * another, so a resize is all-or-nothing. From ctx->alloc when the pool
     * can grow; carved from the pool in fixed mode. */
    void              *var_block;
    void              *prop_block;
    uint8_t            has_dists;     /* dist_offsets is in use        */
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
    uint32_t          *scope_log_depth;
    uint8_t           *scope_log_is_lb;
    uint32_t           n_scope_log;
    uint32_t           scope_log_cap;
    void              *scope_log_block; /* the four arrays, one block    */
    /* Custom value selector hook (for cost-guided search) */
    int64_t          (*value_selector_fn)(struct dvs_ctx_s *, uint32_t, void *);
    void              *value_selector_data;
    /* dvs_solver_set_cost_guided's selector state (from ctx->alloc). */
    void              *costguided;
    /* The oracle check attached by dvs_solver_set_oracle(), or NULL
     * (dvs_oracle.c). */
    struct dvs_octx_s *oracle;
    /* Compile's recursive passes stop (decline the constraint) when the
     * stack runs low (dvs_stackinfo.h). A cache; any value is safe. */
    dvs_stack_guard_t  stack_guard;
    dvs_spool_t        pool;          /* offset-addressed, growable    */
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

/* The first hole (excluded value) of var_id, or NULL. */
static inline HoleEntry *dvs_hole_first(const dvs_ctx_t *ctx, uint32_t var_id) {
    uint32_t off = ctx->var_holes_head[var_id];
    return off ? (HoleEntry *)dvs_spool_ptr(&ctx->pool, off) : NULL;
}

/* The variable a compile-made, value-preserving extension stands for
 * (DvsExtMemo), or id itself. Both are the same value. */
static inline uint32_t dvs_value_src(const dvs_ctx_t *ctx, uint32_t id) {
    for (int hop = 0; hop < 8; hop++) {
        uint32_t i;
        for (i = 0; i < ctx->n_ext_memo; i++)
            if (ctx->ext_memo[i].r == id) break;
        if (i == ctx->n_ext_memo) return id;
        id = ctx->ext_memo[i].src;
    }
    return id;
}

/* The context's heap memory comes from ctx->alloc (dvs_alloc.h). */
static inline void *dvs_ctx_resize(dvs_ctx_t *ctx, void *p, size_t old_sz,
                                   size_t new_sz) {
    return DVS_RESIZE(ctx->alloc, p, old_sz, new_sz);
}
static inline void dvs_ctx_free(dvs_ctx_t *ctx, void *p, size_t sz) {
    if (p) DVS_RELEASE(ctx->alloc, p, sz ? sz : 1);
}

/** Make level_marks and decisions hold at least `n` levels. A fixed-pool
 *  context cannot grow them. 0, or -1 (unchanged). */
int dvs_ctx_reserve_levels(dvs_ctx_t *ctx, uint32_t n);
/** Free the guard index (guard_ix); it is rebuilt on next use. */
void dvs_guard_ix_free(dvs_ctx_t *ctx);

/** Allocate ctx->phase_save (uninitialised) from ctx->alloc; 0 or -1. */
int dvs_ctx_alloc_phase_save(dvs_ctx_t *ctx);

/** Allocate ctx->cong_m/cong_r (zeroed: no fact on any var) from ctx->alloc,
 *  sized to n_vars_capacity; a no-op if already allocated. 0, or -1 on
 *  allocation failure (ctx->cong_m/cong_r left NULL/untouched). */
int dvs_ctx_alloc_cong_facts(dvs_ctx_t *ctx);

/** Allocate ctx->countones_bits/countones_n (zeroed: no fact on any var)
 *  from ctx->alloc, sized to n_vars_capacity; a no-op if already allocated.
 *  0, or -1 on allocation failure (left NULL/untouched). */
int dvs_ctx_alloc_countones_facts(dvs_ctx_t *ctx);

/** Allocate ctx->known_bits_mask/known_bits_val (zeroed: no fact on any
 *  var) from ctx->alloc, sized to n_vars_capacity; a no-op if already
 *  allocated. 0, or -1 on allocation failure (left NULL/untouched). */
int dvs_ctx_alloc_known_bits_facts(dvs_ctx_t *ctx);

/**
 * Make every per-variable table hold at least `n` variables, growing them
 * together (and the phase-save array and the clause-learning state). New
 * entries read as "nothing": a zeroed Variable, no watcher, its own alias,
 * no dist, no holes. Pointers into the tables (`Variable *`, ...) are
 * invalid after a call that grows them.
 *
 * @return 0, or -1 when memory runs out; the pool's overflow flag is then
 *         set, so compile reports DVS_COMPILE_NOMEM. In fixed mode a table
 *         cannot grow inside a checkpoint scope (restore would free it).
 */
int dvs_ctx_reserve_vars(dvs_ctx_t *ctx, uint32_t n);

/** Start using dist_offsets (all 0: no dist on any variable). */
void dvs_ctx_enable_dists(dvs_ctx_t *ctx);

/** The most propagators a context holds: Propagator.prop_id is 16 bits. */
#define DVS_MAX_PROPS 0xFFFFu

/** As dvs_ctx_reserve_vars, for the per-propagator tables. Fails past
 *  DVS_MAX_PROPS. */
int dvs_ctx_reserve_props(dvs_ctx_t *ctx, uint32_t n);

/** Room for `k` more variables: the per-variable tables can take ids up to
 *  n_vars + k - 1. */
static inline int dvs_ctx_room(dvs_ctx_t *ctx, uint32_t k) {
    return ctx->n_vars + k <= ctx->n_vars_capacity ||
           dvs_ctx_reserve_vars(ctx, ctx->n_vars + k) == 0;
}

/** Reserve the prop_refs slot of the next propagator; 0 or -1. */
static inline int dvs_ctx_room_prop(dvs_ctx_t *ctx) {
    return ctx->n_props < ctx->n_prop_refs_capacity ||
           dvs_ctx_reserve_props(ctx, ctx->n_props + 1) == 0 ? 0 : -1;
}

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
        (const WideBounds64 *)dvs_spool_ptr(&ctx->pool, v->holes_offset);
    return wb->lo;
}

static inline int64_t var_hi64(const dvs_ctx_t *ctx, const Variable *v) {
    if (VAR_IS_TIER0(v->flags)) {
        return (v->flags & VAR_SIGNED) ? (int64_t)v->hi
                                       : (int64_t)(uint32_t)v->hi;
    }
    assert(!VAR_IS_TIER2(v->flags) && "tier-2 bounds are WideBoundsN");
    const WideBounds64 *wb =
        (const WideBounds64 *)dvs_spool_ptr(&ctx->pool, v->holes_offset);
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
        uint32_t w = v->width;
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
    uint32_t w = v->width;
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
        (const WideBoundsN *)dvs_spool_ptr(&ctx->pool, v->holes_offset);
    return (const uint64_t *)(wn + 1);
}

static inline const uint64_t *var_hi_wide(const dvs_ctx_t *ctx,
                                           const Variable *v) {
    const WideBoundsN *wn =
        (const WideBoundsN *)dvs_spool_ptr(&ctx->pool, v->holes_offset);
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
