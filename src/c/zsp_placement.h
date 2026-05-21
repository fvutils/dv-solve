#ifndef ZSP_PLACEMENT_H
#define ZSP_PLACEMENT_H

#include <stdint.h>
#include "zsp_propagator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* forward declaration */
typedef struct SolveCtx SolveCtx;

/* ================================================================== */
/* MinOf_N / MaxOf_N propagators                                       */
/*                                                                     */
/* MinOf_N: r == min(x0, x1, ..., x_{n-1})                            */
/* MaxOf_N: r == max(x0, x1, ..., x_{n-1})                            */
/*                                                                     */
/* Used for HPWL: net_xmin = min(pin_x_0, ..., pin_x_k)              */
/*                net_xmax = max(pin_x_0, ..., pin_x_k)              */
/*                net_hpwl_x = net_xmax - net_xmin                   */
/* ================================================================== */

#define MAX_MINMAX_VARS 32u

typedef struct {
    Propagator    hdr;
    uint32_t      n_vars;       /* total watches: [0]=result, [1..n-1]=operands */
    uint32_t      _capacity;
    uint32_t      var_ids[MAX_MINMAX_VARS + 1];
    uint32_t      watcher_nexts[MAX_MINMAX_VARS + 1];
} MinOfN_32_t;

typedef struct {
    Propagator    hdr;
    uint32_t      n_vars;
    uint32_t      _capacity;
    uint32_t      var_ids[MAX_MINMAX_VARS + 1];
    uint32_t      watcher_nexts[MAX_MINMAX_VARS + 1];
} MaxOfN_32_t;

/**
 * MinOf_N: r == min(operands[0], ..., operands[n-1]).
 * var_ids[0]=result, var_ids[1..n]=operands.
 */
uint32_t prop_add_min_of_n_32(SolveCtx *ctx, uint32_t result_id,
                               uint32_t n_operands, const uint32_t *operand_ids,
                               uint8_t priority);

/**
 * MaxOf_N: r == max(operands[0], ..., operands[n-1]).
 * var_ids[0]=result, var_ids[1..n]=operands.
 */
uint32_t prop_add_max_of_n_32(SolveCtx *ctx, uint32_t result_id,
                               uint32_t n_operands, const uint32_t *operand_ids,
                               uint8_t priority);

/* ================================================================== */
/* NoOverlap2D: pairwise rectangle non-overlap propagator             */
/*                                                                     */
/* For N rectangles with position variables (x_i, y_i) and fixed      */
/* dimensions (w_i, h_i), ensures no pair of rectangles overlaps.     */
/*                                                                     */
/* Each rectangle i occupies [x_i, x_i+w_i) x [y_i, y_i+h_i).       */
/* For each pair (i,j), at least one of these must hold:              */
/*   x_i + w_i <= x_j   (i left of j)                                */
/*   x_j + w_j <= x_i   (j left of i)                                */
/*   y_i + h_i <= y_j   (i above j)                                  */
/*   y_j + h_j <= y_i   (j above i)                                  */
/* ================================================================== */

#define MAX_NOOVERLAP2D_RECTS 64u

typedef struct {
    uint32_t x_id;    /* variable for x position */
    uint32_t y_id;    /* variable for y position */
    int32_t  width;   /* fixed rectangle width   */
    int32_t  height;  /* fixed rectangle height  */
    int32_t  halo_l;  /* halo: extra spacing left   */
    int32_t  halo_r;  /* halo: extra spacing right  */
    int32_t  halo_t;  /* halo: extra spacing top    */
    int32_t  halo_b;  /* halo: extra spacing bottom */
} RectSpec;

typedef struct {
    Propagator    hdr;
    uint32_t      n_vars;       /* 2 * n_rects (for wide-watch compat) */
    uint32_t      _capacity;
    uint32_t      var_ids[MAX_NOOVERLAP2D_RECTS * 2];
    uint32_t      watcher_nexts[MAX_NOOVERLAP2D_RECTS * 2];
    uint32_t      n_rects;      /* actual rectangle count */
    uint32_t      _rpad;
    RectSpec      rects[MAX_NOOVERLAP2D_RECTS];
    /* LCG pair tracking: records which pair (i,j) last tightened
     * each variable's bound. Index = rect_idx * 2 + (0=LB, 1=UB).
     * Each entry: (pair_i << 8) | pair_j, or 0xFFFF if unknown. */
    uint16_t      last_pair[MAX_NOOVERLAP2D_RECTS * 2 * 2];
} NoOverlap2D_t;

/**
 * NoOverlap2D: ensure all rectangles are non-overlapping.
 *
 * @param n_rects   Number of rectangles (2 <= n <= MAX_NOOVERLAP2D_RECTS).
 * @param rects     Array of RectSpec (positions are var IDs, dimensions fixed).
 * @param priority  Queue priority level.
 * @return Pool offset, or EXPR_NULL on failure.
 */
uint32_t prop_add_no_overlap_2d(SolveCtx *ctx, uint32_t n_rects,
                                 const RectSpec *rects, uint8_t priority);

/* ================================================================== */
/* Optimization: branch-and-bound wrapper                              */
/*                                                                     */
/* solver_optimize() minimizes an objective variable by iteratively    */
/* solving with tighter upper bounds.                                  */
/* ================================================================== */

typedef struct {
    uint64_t seed;
    uint32_t max_conflicts;
    uint32_t max_restarts;
    uint32_t max_rounds;        /* max B&B iterations (0=unlimited)   */
    double   time_limit_sec;    /* wall-clock limit (0=unlimited)     */
    uint8_t  use_phase_save;
    uint8_t  _pad[3];
    uint32_t max_shave_iters;
} OptimizeOpts;

typedef struct {
    int      found;          /* 1 if any feasible solution found */
    int64_t  best_objective; /* best objective value found       */
    uint32_t n_rounds;       /* number of B&B rounds completed   */
    double   elapsed_sec;    /* total wall time                  */
} OptimizeResult;

/**
 * Minimize the value of objective_var_id using branch-and-bound.
 *
 * The solver finds successive solutions with tighter upper bounds on
 * the objective variable. The best solution's variable assignments
 * are left in the context after return.
 *
 * @param ctx             Compiled solver context.
 * @param objective_var_id Variable to minimize.
 * @param opts            Options (NULL for defaults).
 * @param result          Output: best solution info.
 * @return 0 on success, -1 on error.
 */
int solver_optimize(SolveCtx *ctx, uint32_t objective_var_id,
                    const OptimizeOpts *opts, OptimizeResult *result);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_PLACEMENT_H */
