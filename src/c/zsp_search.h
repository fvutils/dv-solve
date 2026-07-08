#ifndef ZSP_SEARCH_H
#define ZSP_SEARCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* forward declaration */
typedef struct SolveCtx SolveCtx;

/* ------------------------------------------------------------------ */
/* SolveResult                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    SOLVE_OK      = 0,   /* satisfying assignment found               */
    SOLVE_UNSAT   = 1,   /* problem is unsatisfiable                  */
    SOLVE_TIMEOUT = 2,   /* conflict/restart budget exhausted         */
} SolveResult;

/* ------------------------------------------------------------------ */
/* DecisionRecord                                                      */
/*                                                                     */
/* One entry per active decision level.  decisions[L] records what    */
/* was decided when entering level L+1.                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t var_id;        /* variable assigned at this decision      */
    int64_t  tried_value;   /* value tried (used on backtrack)         */
    uint8_t  tried_lower;   /* for a split: 0 = upper half being tried,
                             *              1 = lower half being tried    */
    uint8_t  is_split;      /* 1 = this level is a reversible domain split
                             *     (two-way branch that excludes tried_value)
                             *     rather than a plain value decision       */
    uint8_t  _dec_pad[6];
} DecisionRecord;

/* ------------------------------------------------------------------ */
/* SolveOpts — tuning knobs                                           */
/*                                                                     */
/* Pass NULL for all-default behaviour.                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t seed;              /* RNG seed (0 = use ctx->rng_state)     */
    uint32_t max_conflicts;     /* max conflicts per restart (0=unlimited)*/
    uint32_t max_restarts;      /* max total restarts (0=unlimited)       */
    uint8_t  use_phase_save;    /* 1 = remember last assigned value       */
    uint8_t  use_lcg;           /* 1 = enable lazy clause generation (CDCL) */
    uint8_t  fair_pick;         /* decision-variable tie-break:
                                 *   0 = fast    — deterministic MRV (lowest
                                 *       index); finds *a* solution quickly.
                                 *   1 = uniform — random tie-break among
                                 *       smallest-domain vars; gives uniform
                                 *       marginals / full coverage (the right
                                 *       mode for constrained-random stimulus). */
    uint8_t  _pad[1];
    uint32_t max_shave_iters;   /* pre-search bounds shaving budget (0=use default 1000) */
} SolveOpts;

/* ------------------------------------------------------------------ */
/* DistMeta -- compiled distribution metadata for a single variable   */
/*                                                                     */
/* Stored in the static pool; dist_offsets[var_id] points here.       */
/* n_entries DistMetaEntry values follow immediately after the struct. */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t  lo;           /* range lower bound                      */
    int64_t  hi;           /* range upper bound                      */
    uint64_t cum_weight;   /* cumulative effective weight up to and including this entry */
    uint32_t weight;       /* raw weight                             */
    uint8_t  is_per_value; /* 1 = per-value (:=), 0 = per-range (:/)*/
    uint8_t  _dmpad[3];
} DistMetaEntry;

typedef struct {
    uint32_t n_entries;
    uint32_t _pad;
    /* DistMetaEntry entries[n_entries] follow immediately */
} DistMeta;

/* ------------------------------------------------------------------ */
/* HoleEntry -- linked list node for excluded values (randc support)   */
/*                                                                     */
/* Stored in the static pool; var_holes_head[var_id] points to the    */
/* first entry.  Values are kept sorted ascending for efficient skip. */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t  value;    /* excluded value                              */
    uint32_t next;     /* pool offset to next HoleEntry, or 0 = end  */
    uint32_t _hpad;
} HoleEntry;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * Run the search until a satisfying assignment is found, the problem is
 * proved unsatisfiable, or the budget is exhausted.
 *
 * Requires that solver_compile() has already been called on ctx.
 *
 * @return SOLVE_OK, SOLVE_UNSAT, or SOLVE_TIMEOUT.
 */
SolveResult solver_solve(SolveCtx *ctx, const SolveOpts *opts);

/**
 * Return the assigned value of variable var_id after a successful solve.
 *
 * Valid only after solver_solve() returns SOLVE_OK.
 * Returns the lower bound (== upper bound if fully assigned).
 */
int64_t solver_get_value(const SolveCtx *ctx, uint32_t var_id);

/**
 * Reset the solver to its post-compile state.
 * Restores all variable domains, clears trail and decisions,
 * and re-enqueues all propagators.
 */
void solver_reset(SolveCtx *ctx);

/**
 * Pin a variable to a specific value.
 * Tightens both lb and ub, then runs propagation.
 * @return 0 on success, -1 if the pin causes a conflict.
 */
int solver_pin_var(SolveCtx *ctx, uint32_t var_id, int64_t value);

/**
 * Set the RNG seed for the next solve.
 */
void solver_set_seed(SolveCtx *ctx, uint64_t seed);

/**
 * Read values of multiple variables in one call.
 * @param n       Number of variables to read.
 * @param var_ids Array of variable IDs.
 * @param out     Output array (caller-allocated, size >= n).
 */
void solver_get_values(const SolveCtx *ctx, uint32_t n,
                       const uint32_t *var_ids, int64_t *out);

/**
 * Batch solve: reset + solve + read values, repeated n_solves times.
 *
 * Keeps the entire loop in C to avoid per-solve FFI overhead.
 * Seeds are base_seed, base_seed+1, ..., base_seed+n_solves-1.
 *
 * @param out  Output matrix: n_solves * n_vars int64 values (row-major).
 *             Only the first n_ok rows are filled.
 * @return Number of successful solves (n_ok).
 */
int solver_solve_n(SolveCtx *ctx, uint32_t n_solves,
                   uint32_t n_vars, const uint32_t *var_ids,
                   int64_t *out,
                   uint64_t base_seed,
                   uint32_t max_shave_iters);


/**
 * Query whether a soft constraint's assumption is still active after solve.
 * @param assumption_idx  0-based index into the soft constraint list.
 * @return 1 if active (constraint was satisfied), 0 if relaxed, -1 on error.
 */
int solver_soft_active(const SolveCtx *ctx, uint32_t assumption_idx);

/**
 * Exclude a specific value from a variable's domain.
 *
 * Used for randc (cyclic-random) semantics: after solving, the caller
 * excludes the obtained value so the next solve cannot pick it again.
 *
 * - If value == current lb: tightens lb to lb+1.
 * - If value == current ub: tightens ub to ub-1.
 * - Otherwise: adds the value to a per-variable hole list.
 *
 * @return 0 on success, -1 if the exclusion empties the domain.
 */
int solver_exclude_value(SolveCtx *ctx, uint32_t var_id, int64_t value);

/**
 * Bulk-create array element variables.
 *
 * Creates n_elems variables with IDs [elem_var_base .. elem_var_base+n_elems-1],
 * all with the same width, signedness, and initial bounds. This is more
 * efficient than building an auxiliary SolveProblem for the common
 * "add N identical element variables" pattern.
 *
 * @return 0 on success, -1 on capacity overflow.
 */
int solver_add_array_vars(SolveCtx *ctx,
                          uint32_t elem_var_base,
                          uint32_t n_elems,
                          uint8_t  width,
                          uint8_t  is_signed,
                          int64_t  lo,
                          int64_t  hi);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_SEARCH_H */
