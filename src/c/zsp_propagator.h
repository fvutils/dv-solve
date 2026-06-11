#ifndef ZSP_PROPAGATOR_H
#define ZSP_PROPAGATOR_H

#include <stdint.h>
#include "zsp_pool.h"   /* EXPR_NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* forward declarations */
typedef struct Propagator Propagator;
typedef struct SolveCtx   SolveCtx;

/* ------------------------------------------------------------------ */
/* PropResult                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    PROP_OK       = 0,   /* propagated; may have tightened some bounds  */
    PROP_CONFLICT = 1,   /* empty domain detected                       */
    PROP_ENTAILED = 2,   /* constraint permanently satisfied            */
} PropResult;

/* ------------------------------------------------------------------ */
/* Propagator flags                                                    */
/* ------------------------------------------------------------------ */
#define PROP_FLAG_ENTAILED  0x01u
#define PROP_FLAG_IN_QUEUE  0x02u
#define PROP_FLAG_WIDE_WATCH 0x04u  /* watcher layout differs from PropWatchSect */

/* ------------------------------------------------------------------ */
/* Event types (for watcher registration — Phase 7+)                  */
/* ------------------------------------------------------------------ */
#define EVT_LB   0x01u
#define EVT_UB   0x02u
#define EVT_SING 0x04u
#define EVT_HOLE 0x08u
#define EVT_ANY  (EVT_LB | EVT_UB | EVT_SING | EVT_HOLE)

/* ------------------------------------------------------------------ */
/* PropWatchSect                                                       */
/*                                                                     */
/* Fixed-size block immediately after the 16-byte Propagator header.  */
/* Contains watched-variable IDs and per-variable linked-list chains. */
/*                                                                     */
/* MAX_PROP_WATCHES = 4 supports up to 4-variable propagators.        */
/* (BoundsAdd needs 3: r, a, b; BitSlice/Extend need 2.)             */
/* ------------------------------------------------------------------ */
#define MAX_PROP_WATCHES 4u

typedef struct {
    uint32_t n_watches;                     /* number of watched variables */
    uint32_t var_ids[MAX_PROP_WATCHES];     /* watched variable IDs        */
    uint32_t next_watchers[MAX_PROP_WATCHES]; /* per-var watcher chain      */
} PropWatchSect;  /* 4 + 16 + 16 = 36 bytes */

/* ------------------------------------------------------------------ */
/* Propagator header — 16 bytes                                        */
/*                                                                     */
/* Immediately after the header lies a PropWatchSect (36 bytes),      */
/* followed by template-specific data.                                 */
/* ------------------------------------------------------------------ */
/* Forward declaration for LCG explanation */
struct Explanation;

struct Propagator {
    PropResult (*fire)(Propagator *self, SolveCtx *ctx);  /* 8 bytes */
    /* Explanation callback for LCG (NULL if propagator doesn't support LCG).
     * Called during conflict analysis to explain why a bound was tightened. */
    int (*explain)(Propagator *self, SolveCtx *ctx,
                   uint32_t var_id, uint8_t is_lb,
                   int64_t new_bound, struct Explanation *out);  /* 8 bytes */
    uint32_t    queue_next;  /* pool offset to next in FIFO queue       */
    uint16_t    prop_id;
    uint8_t     priority;    /* 0=high (fires first), 15=low            */
    uint8_t     flags;       /* PROP_FLAG_* bits                        */
};  /* 24 bytes (16 without explain) */

/* ------------------------------------------------------------------ */
/* PropQueue — 16-level priority FIFO                                  */
/*                                                                     */
/* Priority 0 fires first (cheap bounds propagators).                 */
/* non_empty_mask bit i set ↔ level i has at least one entry.        */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t  non_empty_mask;
    uint16_t  _pad[3];
    uint32_t  heads[16];    /* pool offsets to queue head per level */
    uint32_t  tails[16];    /* pool offsets to queue tail per level */
} PropQueue;  /* 2 + 6 + 64 + 64 = 136 bytes */

/* ------------------------------------------------------------------ */
/* Template structs                                                    */
/*                                                                     */
/* Layout (all templates):                                            */
/*   [Propagator hdr (16)] [PropWatchSect ws (36)] [template data]   */
/*                                                                     */
/* var_ids[0..n-1] are the primary variable references.  For binary   */
/* templates: var_ids[0]=lhs, var_ids[1]=rhs.  For ternary (Add etc.) */
/* var_ids[0]=result, var_ids[1]=operand_a, var_ids[2]=operand_b.    */
/* ------------------------------------------------------------------ */

typedef struct { Propagator hdr; PropWatchSect ws; } BoundsLE_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsLT_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsEQ_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsNE_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsAdd_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsMul_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsDiv_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsMod_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } UnaryNeg_32_t;

/* BoundsLE/LT/EQ/NE/Add/Mul/Div/Mod/UnaryNeg _64 variants */
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsLE_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsLT_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsEQ_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsNE_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsAdd_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsMul_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsDiv_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsMod_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } UnaryNeg_64_t;

/* ITE value: r = cond ? a : b */
typedef struct { Propagator hdr; PropWatchSect ws; } ITEValue_64_t;

/* Bitwise propagators: r = a OP b */
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsBAND_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsBOR_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsBXOR_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsBNOT_64_t;

/* Shift propagators */
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsSHL_64_t;
typedef struct { Propagator hdr; PropWatchSect ws; } BoundsLSHR_64_t;


/* Concat propagator: r = {hi, lo} where lo is lo_width bits wide.
   var_ids[0]=r, [1]=hi, [2]=lo */
typedef struct {
    Propagator    hdr;
    PropWatchSect ws;
    uint8_t       lo_width;
    uint8_t       _cpad[3];
} BoundsConcat_64_t;

/* InSet_32: x ∈ {elems[0], …, elems[n_elems-1]}
   Template data: n_elems (4), _pad (4), then int32_t elems[] */
typedef struct {
    Propagator  hdr;
    PropWatchSect ws;
    uint32_t    n_elems;
    uint32_t    _pad;
    /* int32_t elems[n_elems] follow immediately */
} InSet_32_t;

/* InSet_64 */
typedef struct {
    Propagator  hdr;
    PropWatchSect ws;
    uint32_t    n_elems;
    uint32_t    _pad;
    /* int64_t elems[n_elems] follow immediately */
} InSet_64_t;

/* Implication_32: guard → (var ≤/≥ bound)
   var_ids[0]=guard, var_ids[1]=var
   is_ub=1 → enforce UB (var ≤ bound); is_ub=0 → enforce LB (var ≥ bound) */
typedef struct {
    Propagator  hdr;
    PropWatchSect ws;
    int32_t     bound;
    uint8_t     is_ub;
    uint8_t     _pad[3];
} Implication_32_t;

/* Reification_32: guard ↔ (x ≤ y) */
typedef struct { Propagator hdr; PropWatchSect ws; } Reification_32_t;
typedef struct { Propagator hdr; PropWatchSect ws; } Reification_64_t;

/* ReificationEq: guard ↔ (x == y), var_ids[0]=guard, [1]=x, [2]=y */
typedef struct { Propagator hdr; PropWatchSect ws; } ReificationEq_32_t;


/* BvAddConst_64: r = (x + c) mod 2^width  (BV modular add with constant).
   var_ids[0]=r, var_ids[1]=x.  Width is 1..64. */
typedef struct {
    Propagator    hdr;
    PropWatchSect ws;
    uint64_t      c;        /* reduced mod 2^width */
    uint8_t       width;    /* 1..64 */
    uint8_t       _cpad[7];
} BvAddConst_64_t;

/* BvBin_64: modular fixed-width 2's-complement var-var arithmetic.
   r = (a op b) mod 2^width  for op in {ADD, SUB, MUL, SHL}.
   var_ids[0]=r, var_ids[1]=a, var_ids[2]=b.  Width is 1..63.
   Operand and result signedness is read from the Variable flags. */
typedef struct {
    Propagator    hdr;
    PropWatchSect ws;
    uint8_t       width;    /* 1..63 */
    uint8_t       _bpad[7];
} BvBin_64_t;

/* BitSlice_32: r = a[hi_bit:lo_bit]  var_ids[0]=r, var_ids[1]=a */
typedef struct {
    Propagator  hdr;
    PropWatchSect ws;
    uint8_t     hi_bit;
    uint8_t     lo_bit;
    uint8_t     _pad[2];
} BitSlice_32_t;
typedef struct {
    Propagator  hdr;
    PropWatchSect ws;
    uint8_t     hi_bit;
    uint8_t     lo_bit;
    uint8_t     _pad[2];
} BitSlice_64_t;

/* ------------------------------------------------------------------ */
/* Queue management                                                    */
/* ------------------------------------------------------------------ */

/** Enqueue a propagator (no-op if already queued or entailed). */
void prop_enqueue(SolveCtx *ctx, uint32_t prop_ref);

/** Set a guard variable on a propagator. When guard is 0, propagator is
 *  entailed; when guard is undecided (lo!=hi), propagator is skipped;
 *  when guard is 1, propagator fires normally. */
void prop_set_guard(SolveCtx *ctx, uint32_t prop_ref, uint32_t guard_var_id);

/* ------------------------------------------------------------------ */
/* Domain-tightening functions                                        */
/*                                                                     */
/* These are the canonical way for propagators to modify variable     */
/* domains.  Each function:                                           */
/*   1. No-ops if the new bound is not tighter.                       */
/*   2. Records the old bound in the trail and applies the change.    */
/*   3. Returns PROP_CONFLICT if the domain becomes empty.            */
/*   4. Wakes all watchers on the variable.                           */
/* ------------------------------------------------------------------ */

PropResult ctx_tighten_lb32(SolveCtx *ctx, uint32_t var_id, int32_t new_lb);
PropResult ctx_tighten_ub32(SolveCtx *ctx, uint32_t var_id, int32_t new_ub);
PropResult ctx_tighten_lb64(SolveCtx *ctx, uint32_t var_id, int64_t new_lb);
PropResult ctx_tighten_ub64(SolveCtx *ctx, uint32_t var_id, int64_t new_ub);

/* ------------------------------------------------------------------ */
/* Propagation loop                                                    */
/* ------------------------------------------------------------------ */

/**
 * Run the propagation loop until fixedpoint or conflict.
 *
 * @return PROP_OK on fixedpoint, PROP_CONFLICT if any domain empties.
 */
PropResult solver_propagate(SolveCtx *ctx);

/**
 * Run propagation only (no search).  Returns PROP_OK or PROP_CONFLICT.
 * Useful for fast feasibility checks.
 */
PropResult solver_propagate_only(SolveCtx *ctx);

/* ------------------------------------------------------------------ */
/* Propagator constructors                                             */
/*                                                                     */
/* Each constructor:                                                   */
/*   - Allocates the propagator in ctx's static pool.                 */
/*   - Registers watchers for all variable IDs.                       */
/*   - Enqueues the propagator for an initial firing.                 */
/*   - Returns the pool offset (PropRef), or EXPR_NULL on failure.    */
/* ------------------------------------------------------------------ */

uint32_t prop_add_bounds_le_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_lt_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_eq_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_ne_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_add_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
uint32_t prop_add_bounds_mul_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
uint32_t prop_add_bounds_div_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
uint32_t prop_add_bounds_mod_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
uint32_t prop_add_unary_neg_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint8_t priority);

uint32_t prop_add_bounds_le_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_lt_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_eq_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_ne_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority);
uint32_t prop_add_bounds_add_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
/** Modular BV add with constant: r = (x + c) mod 2^width. width is 1..64. */
uint32_t prop_add_bvadd_const_64(SolveCtx *ctx, uint32_t r_id, uint32_t x_id,
                                  uint64_t c, uint8_t width, uint8_t priority);
/** Modular fixed-width 2's-complement var-var arithmetic (width 1..63):
 *  r = (a + b) mod 2^width / (a - b) / (a * b) / (a << b). */
uint32_t prop_add_bvadd_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority);
uint32_t prop_add_bvsub_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority);
uint32_t prop_add_bvmul_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority);
uint32_t prop_add_bvshl_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority);
uint32_t prop_add_bounds_mul_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
uint32_t prop_add_bounds_div_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
uint32_t prop_add_bounds_mod_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority);
uint32_t prop_add_unary_neg_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint8_t priority);

/** ITE value propagator: r = cond ? a : b.
 *  var_ids[0]=r, var_ids[1]=cond, var_ids[2]=a, var_ids[3]=b */
uint32_t prop_add_ite_value_64(SolveCtx *ctx, uint32_t r_id,
                                uint32_t cond_id, uint32_t a_id,
                                uint32_t b_id, uint8_t priority);

/** Bitwise AND: r = a & b.  var_ids[0]=r, [1]=a, [2]=b */
uint32_t prop_add_bounds_band_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint32_t b_id,
                                  uint8_t priority);
/** Bitwise OR: r = a | b */
uint32_t prop_add_bounds_bor_64(SolveCtx *ctx, uint32_t r_id,
                                 uint32_t a_id, uint32_t b_id,
                                 uint8_t priority);
/** Bitwise XOR: r = a ^ b */
uint32_t prop_add_bounds_bxor_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint32_t b_id,
                                  uint8_t priority);
/** Bitwise NOT: r = ~a.  var_ids[0]=r, [1]=a */
uint32_t prop_add_bounds_bnot_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint8_t priority);

/** Left shift: r = a << b.  var_ids[0]=r, [1]=a, [2]=b */
uint32_t prop_add_bounds_shl_64(SolveCtx *ctx, uint32_t r_id,
                                 uint32_t a_id, uint32_t b_id,
                                 uint8_t priority);
/** Logical right shift: r = a >> b */
uint32_t prop_add_bounds_lshr_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint32_t b_id,
                                  uint8_t priority);


/** Concat: r = {hi, lo}. lo_width is the bit width of lo.
 *  var_ids[0]=r, [1]=hi, [2]=lo */
uint32_t prop_add_bounds_concat_64(SolveCtx *ctx, uint32_t r_id,
                                    uint32_t hi_id, uint32_t lo_id,
                                    uint8_t lo_width, uint8_t priority);

/**
 * InSet: x ∈ {elems[0], …, elems[n_elems-1]}.
 * @param elems  Array of int32_t allowed values.
 */
uint32_t prop_add_in_set_32(SolveCtx *ctx, uint32_t x_id,
                             uint32_t n_elems, const int32_t *elems,
                             uint8_t priority);
uint32_t prop_add_in_set_64(SolveCtx *ctx, uint32_t x_id,
                             uint32_t n_elems, const int64_t *elems,
                             uint8_t priority);

/**
 * Implication: guard=true → (var ≤ bound) if is_ub, or (var ≥ bound) otherwise.
 * var_ids[0]=guard, var_ids[1]=var.
 */
uint32_t prop_add_implication_32(SolveCtx *ctx,
                                  uint32_t guard_id, uint32_t var_id,
                                  int32_t bound, uint8_t is_ub,
                                  uint8_t priority);

uint32_t prop_add_reification_32(SolveCtx *ctx, uint32_t guard_id,
                                   uint32_t x_id, uint32_t y_id,
                                   uint8_t priority);
uint32_t prop_add_reification_64(SolveCtx *ctx, uint32_t guard_id,
                                   uint32_t x_id, uint32_t y_id,
                                   uint8_t priority);

uint32_t prop_add_reification_eq_32(SolveCtx *ctx, uint32_t guard_id,
                                     uint32_t x_id, uint32_t y_id,
                                     uint8_t priority);

uint32_t prop_add_bit_slice_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                uint8_t hi_bit, uint8_t lo_bit, uint8_t priority);
uint32_t prop_add_bit_slice_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                uint8_t hi_bit, uint8_t lo_bit, uint8_t priority);


/* ------------------------------------------------------------------ */
/* AllDifferent: x0, x1, ..., xN are all distinct (up to 16 vars)    */
/*                                                                     */
/* Uses PROP_FLAG_WIDE_WATCH because it watches more variables than   */
/* PropWatchSect supports (>4).  _wake_var checks this flag to use    */
/* the custom watcher_nexts array instead of PropWatchSect.           */
/* ------------------------------------------------------------------ */
#define MAX_ALLDIFF_VARS 16u

typedef struct {
    Propagator    hdr;
    uint32_t      n_vars;
    uint32_t      _capacity;   /* max var_ids/watcher_nexts entries */
    uint32_t      var_ids[MAX_ALLDIFF_VARS];
    uint32_t      watcher_nexts[MAX_ALLDIFF_VARS];
} AllDifferent_t;

/**
 * AllDifferent constraint: all watched variables must have distinct values.
 *
 * @param n_vars   Number of variables (2 <= n_vars <= MAX_ALLDIFF_VARS).
 * @param var_ids  Array of variable IDs.
 * @param priority Queue priority level.
 * @return Pool offset of the propagator, or EXPR_NULL on failure.
 */
uint32_t prop_add_all_different(SolveCtx *ctx, uint32_t n_vars,
                                 const uint32_t *var_ids, uint8_t priority);


/* ------------------------------------------------------------------ */
/* DisjClause: (v0 op0 c0) OR (v1 op1 c1) OR ... (up to 4 clauses)  */
/*                                                                     */
/* Each clause is var_id op constant.  When all but one clause are    */
/* falsified by the current bounds, the surviving clause is enforced. */
/* Uses uint32_t for op to avoid including zsp_problem.h.             */
/* ------------------------------------------------------------------ */
#define MAX_DISJ_CLAUSES 16u

typedef struct {
    Propagator    hdr;
    PropWatchSect ws;
    uint32_t      n_clauses;
    struct {
        uint32_t var_id;
        uint32_t op;       /* BIN_EQ / BIN_NEQ / BIN_LT / BIN_LTE / BIN_GT / BIN_GTE */
        int64_t  constant;
        uint32_t rhs_var_id;  /* UINT32_MAX = use constant; else = var-var */
    } clauses[MAX_DISJ_CLAUSES];
} DisjClause_t;

uint32_t prop_add_disj_clause(SolveCtx *ctx,
                               uint32_t n_clauses,
                               const uint32_t *var_ids,
                               const uint32_t *ops,
                               const int64_t *constants,
                               uint8_t priority,
                               const uint32_t *rhs_var_ids);

/* ------------------------------------------------------------------ */
/* SumEq: result == var_ids[0] + var_ids[1] + ... + var_ids[n-1]      */
/*                                                                     */
/* Uses PROP_FLAG_WIDE_WATCH for variable-length watch list.           */
/* var_ids[0] = result, var_ids[1..n] = summands.                     */
/* ------------------------------------------------------------------ */
#define MAX_SUM_VARS 64u  /* max summands (not counting result) */

typedef struct {
    Propagator    hdr;
    uint32_t      n_vars;      /* total watched vars (1 result + N summands) */
    uint32_t      _capacity;   /* max var_ids/watcher_nexts entries */
    uint32_t      var_ids[MAX_SUM_VARS + 1];
    uint32_t      watcher_nexts[MAX_SUM_VARS + 1];
} SumEq_32_t;

/**
 * Sum-equality constraint: result == sum of summand variables.
 *
 * @param result_id   Variable ID for the result.
 * @param n_summands  Number of summand variables (1 <= n <= MAX_SUM_VARS).
 * @param summand_ids Array of summand variable IDs.
 * @param priority    Queue priority level.
 * @return Pool offset of the propagator, or EXPR_NULL on failure.
 */
uint32_t prop_add_sum_eq_32(SolveCtx *ctx, uint32_t result_id,
                             uint32_t n_summands, const uint32_t *summand_ids,
                             uint8_t priority);

/* ------------------------------------------------------------------ */
/* Countones_32: result == popcount(operand)                           */
/* ------------------------------------------------------------------ */
typedef struct { Propagator hdr; PropWatchSect ws; } Countones_32_t;

uint32_t prop_add_countones_32(SolveCtx *ctx, uint32_t result_id,
                                uint32_t operand_id, uint8_t priority);

/* ------------------------------------------------------------------ */
/* Clog2_32: result == ceil(log2(operand))                             */
/* ------------------------------------------------------------------ */
typedef struct { Propagator hdr; PropWatchSect ws; } Clog2_32_t;

uint32_t prop_add_clog2_32(SolveCtx *ctx, uint32_t result_id,
                            uint32_t operand_id, uint8_t priority);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_PROPAGATOR_H */
