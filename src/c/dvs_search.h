#ifndef DVS_SEARCH_H
#define DVS_SEARCH_H

#include <stdint.h>
#include "dv_solve.h"  /* dvs_ctx_t, dvs_result_t, dvs_solve_opts_t */

#ifdef __cplusplus
extern "C" {
#endif


/* ------------------------------------------------------------------ */
/* DecisionRecord                                                      */
/*                                                                     */
/* One entry per active decision level.  decisions[L] records what    */
/* was decided when entering level L+1.                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t var_id;        /* variable assigned at this decision      */
    int64_t  tried_value;   /* value tried (used on backtrack)         */
    uint8_t  is_split;      /* 1 = this level is a reversible domain split
                             *     (two-way branch that excludes tried_value)
                             *     rather than a plain value decision       */
    uint8_t  upper_first;   /* for a split: 1 = the upper half (tried_value,
                             *     dhi] was explored first, 0 = the lower half
                             *     [dlo, tried_value). Chosen at random,
                             *     size-weighted, in diversity mode so the
                             *     search does not always descend toward the
                             *     domain minimum (see _split_upper_first).  */
    uint8_t  second_phase;  /* for a split: 1 = the first half is exhausted
                             *     and the second half is being explored     */
    uint8_t  bound;         /* 0: a value decision (x == tried_value);
                             * 1: a bound decision x <= tried_value;
                             * 2: a bound decision x >= tried_value
                             *    (clause learning on a wide domain; see
                             *    _bound_decision)                        */
    uint8_t  _dec_pad[4];
} DecisionRecord;

/* Why a solve returned DVS_SOLVE_TIMEOUT. Diagnostic only; never affects the
 * verdict (DVS_SOLVE_TIMEOUT always means "unknown", i.e. correct-or-unknown). */
#define DVS_BAIL_NONE          0   /* not a timeout, or reason not recorded  */
#define DVS_BAIL_DEADLINE      1   /* wall-clock budget (decision loop)      */
#define DVS_BAIL_MAX_DEPTH     2   /* decision_level hit ctx->max_depth      */
#define DVS_BAIL_DEADLINE_CONF 3   /* wall-clock budget (conflict loop)      */
#define DVS_BAIL_MAX_RESTARTS  4   /* restart budget exhausted               */
#define DVS_BAIL_PROPAGATION   5   /* one propagation ran past the deadline or
                                    * trail cap (ctx->prop_aborted, B61)      */

const char *dvs_solver_bail_reason_str(const dvs_ctx_t *ctx);


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
 * Set the RNG seed for the next solve.
 */
void dvs_solver_set_seed(dvs_ctx_t *ctx, uint64_t seed);

/**
 * Whether a soft constraint's assumption is still active after solve.
 * Assumption indices run in REVERSE order of addition (the soft list is
 * prepended): index 0 is the soft constraint added last.
 * @return 1 if active (constraint was satisfied), 0 if relaxed, -1 on error.
 */
int dvs_solver_soft_active(const dvs_ctx_t *ctx, uint32_t assumption_idx);

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
int dvs_solver_solve_n(dvs_ctx_t *ctx, uint32_t n_solves,
                   uint32_t n_vars, const uint32_t *var_ids,
                   int64_t *out,
                   uint64_t base_seed,
                   uint32_t max_shave_iters);


/**
 * Bulk-create array element variables.
 *
 * Creates n_elems variables with IDs [elem_var_base .. elem_var_base+n_elems-1],
 * all with the same width, signedness, and initial bounds. This is more
 * efficient than building an auxiliary dvs_problem_t for the common
 * "add N identical element variables" pattern.
 *
 * @return 0 on success, -1 on capacity overflow.
 */
int dvs_solver_add_array_vars(dvs_ctx_t *ctx,
                          uint32_t elem_var_base,
                          uint32_t n_elems,
                          uint8_t  width,
                          uint8_t  is_signed,
                          int64_t  lo,
                          int64_t  hi);

#ifdef __cplusplus
}
#endif

#endif /* DVS_SEARCH_H */
