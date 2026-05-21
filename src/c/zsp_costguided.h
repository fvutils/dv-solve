#ifndef ZSP_COSTGUIDED_H
#define ZSP_COSTGUIDED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolveCtx SolveCtx;

/* ================================================================== */
/* CostGuided Value Selector                                           */
/*                                                                     */
/* When the search loop needs to pick a value for a variable, the     */
/* CostGuided selector evaluates a cost function at each candidate    */
/* value in the domain and picks the one with minimum cost.           */
/*                                                                     */
/* The cost function is pluggable: the caller provides a callback     */
/* that computes the incremental cost of assigning a specific value   */
/* to a specific variable.  The first concrete cost function is       */
/* HPWL (half-perimeter wire length) for macro placement.             */
/* ================================================================== */

/**
 * Cost function callback type.
 *
 * Given a variable and a candidate value, return the incremental cost
 * of making this assignment.  Lower cost is better.
 *
 * @param ctx       Solver context (for reading other variables' bounds).
 * @param var_id    Variable being assigned.
 * @param value     Candidate value.
 * @param user_data Opaque data for the cost function.
 * @return Incremental cost (int64_t).  INT64_MAX means "infeasible".
 */
typedef int64_t (*CostFunc)(const SolveCtx *ctx, uint32_t var_id,
                             int64_t value, void *user_data);

/**
 * CostGuided context: holds the cost function and configuration.
 */
typedef struct {
    CostFunc     cost_fn;        /* cost evaluation callback           */
    void        *cost_data;      /* opaque data passed to cost_fn      */
    int32_t      max_domain_scan; /* max domain values to evaluate
                                   * (0 = unlimited, >0 = cap scan)   */
} CostGuidedCtx;

/**
 * Install a CostGuided value selector on the solver context.
 *
 * After this call, the search loop will use the cost function to
 * select values instead of random/phase-save selection.
 *
 * @param ctx       Solver context.
 * @param cost_fn   Cost evaluation function.
 * @param cost_data Opaque data for cost_fn (caller-managed lifetime).
 * @param max_scan  Maximum domain values to evaluate per decision
 *                  (0 = scan entire domain, recommended for small domains).
 */
void solver_set_cost_guided(SolveCtx *ctx, CostFunc cost_fn,
                             void *cost_data, int32_t max_scan);

/* ================================================================== */
/* HPWL Cost Function                                                  */
/*                                                                     */
/* Computes incremental half-perimeter wire length for macro placement */
/* problems.  For each net containing the current macro, the cost of  */
/* placing it at position p is:                                       */
/*   cost += max(0, net_min - (p + pin_offset))                      */
/*         + max(0, (p + pin_offset) - net_max)                      */
/* where net_min/net_max are the current bounding box of already-     */
/* placed pins in the net.                                            */
/* ================================================================== */

/** Pin descriptor: one pin on one macro in one net. */
typedef struct {
    uint32_t macro_id;   /* which macro this pin belongs to */
    int32_t  offset_x;   /* pin offset from macro origin (x) */
    int32_t  offset_y;   /* pin offset from macro origin (y) */
} CostGuidedPin;

/** Net descriptor: a set of pins connected by a net. */
typedef struct {
    uint32_t       n_pins;
    CostGuidedPin *pins;   /* array[n_pins], caller-allocated */
} CostGuidedNet;

/** Macro-to-variable mapping. */
typedef struct {
    uint32_t x_var_id;
    uint32_t y_var_id;
} CostGuidedMacro;

/**
 * HPWL cost function context.
 *
 * Holds the problem structure (macros, nets, pin mappings) needed
 * to evaluate incremental HPWL.  Caller allocates and manages
 * the arrays; this struct just references them.
 */
typedef struct {
    uint32_t          n_macros;
    uint32_t          n_nets;
    CostGuidedMacro  *macros;          /* array[n_macros] */
    CostGuidedNet    *nets;            /* array[n_nets]   */
    /* Per-macro net index (for fast lookup) */
    uint32_t         *macro_net_count; /* array[n_macros] */
    uint32_t        **macro_net_ids;   /* array[n_macros] -> net indices */
    /* Optional overlap penalty: set macro_widths != NULL to enable.
     * When enabled, positions overlapping already-placed macros get
     * a large cost penalty so the cost function steers toward feasibility. */
    int32_t          *macro_widths;    /* array[n_macros], NULL = disabled */
    int32_t          *macro_heights;   /* array[n_macros] */
} HPWLCostCtx;

/**
 * Build the per-macro net index for an HPWLCostCtx.
 *
 * Scans all nets to build macro_net_count and macro_net_ids arrays.
 * The index arrays are allocated with malloc; caller must free them
 * via hpwl_cost_ctx_destroy().
 *
 * @return 0 on success, -1 on allocation failure.
 */
int hpwl_cost_ctx_build_index(HPWLCostCtx *hctx);

/** Free the index arrays allocated by hpwl_cost_ctx_build_index(). */
void hpwl_cost_ctx_destroy(HPWLCostCtx *hctx);

/**
 * HPWL cost function (matches CostFunc signature).
 *
 * user_data must point to an HPWLCostCtx.
 */
int64_t hpwl_cost_fn(const SolveCtx *ctx, uint32_t var_id,
                      int64_t value, void *user_data);

/**
 * Convenience: set up CostGuided with HPWL on a solver context.
 *
 * Equivalent to:
 *   hpwl_cost_ctx_build_index(hctx);
 *   solver_set_cost_guided(ctx, hpwl_cost_fn, hctx, max_scan);
 */
int solver_set_cost_guided_hpwl(SolveCtx *ctx, HPWLCostCtx *hctx,
                                 int32_t max_scan);


/**
 * Run a greedy placement using HPWL cost-guided value selection.
 *
 * Places macros one at a time (most-connected first), evaluating
 * the HPWL cost function at sampled positions for each macro's x
 * and y variables.  Uses checkpoint/restore to evaluate positions
 * without permanently altering solver state.
 *
 * After completion, the best positions are stored in out_positions
 * (array of n_macros x,y pairs: [x0, y0, x1, y1, ...]).
 *
 * @param ctx         Solver context (compiled, with NoOverlap2D).
 * @param hctx        HPWL cost context with net/macro data.
 * @param max_scan    Max positions to sample per variable.
 * @param out_positions  Output array of size 2*n_macros (caller-allocated).
 * @return 0 on success (all macros placed), -1 on failure.
 */
int costguided_greedy_place(SolveCtx *ctx, HPWLCostCtx *hctx,
                             int32_t max_scan, int32_t *out_positions);

/**
 * Inject position hints into the solver's phase-save array.
 *
 * After calling this, the solver will try these values first on each
 * restart (when use_phase_save=1 in SolveOpts).
 *
 * @param ctx     Solver context.
 * @param hints   Array of (var_id, value) pairs.
 * @param n_hints Number of pairs.
 * @return 0 on success.
 */
int solver_set_phase_hints(SolveCtx *ctx, const uint32_t *var_ids,
                            const int64_t *values, uint32_t n_hints);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_COSTGUIDED_H */

/* ================================================================== */
/* LNS (Large Neighborhood Search) optimizer                           */
/*                                                                     */
/* Iteratively improves a placement by unfreezing small groups of      */
/* macros and re-solving the subproblem.  Uses the greedy placement    */
/* as the initial solution.                                            */
/* ================================================================== */

/** LNS configuration. */
typedef struct {
    uint32_t max_iterations;     /* max LNS iterations (0 = 100)     */
    double   time_limit_sec;     /* wall-clock time limit (0 = 10s)  */
    uint32_t neighborhood_size;  /* macros to unfreeze (0 = 2)       */
    uint32_t subproblem_conflicts; /* max conflicts per subproblem
                                   * (0 = 1000)                      */
    uint64_t seed;               /* RNG seed (0 = 42)                */
} LNSOpts;

/** LNS result. */
typedef struct {
    int      improved;       /* 1 if HPWL improved over initial      */
    int64_t  initial_hpwl;   /* HPWL of the initial (greedy) solution */
    int64_t  best_hpwl;      /* best HPWL found                      */
    uint32_t iterations;     /* total LNS iterations completed       */
    uint32_t improvements;   /* number of accepted improvements      */
    double   elapsed_sec;    /* total wall time                      */
} LNSResult;

/**
 * Run LNS optimization on a placement problem.
 *
 * Requires a compiled solver context with NoOverlap2D propagators.
 * Runs greedy placement internally to get the initial solution, then
 * iteratively improves by re-solving subproblems.
 *
 * On return, out_positions contains the best placement found.
 *
 * @param ctx           Solver context (compiled, with NoOverlap2D).
 * @param hctx          HPWL cost context with net/macro data.
 * @param opts          LNS options (NULL for defaults).
 * @param out_positions Output: best positions [x0,y0,x1,y1,...].
 * @param result        Output: LNS statistics.
 * @return 0 on success (feasible solution found), -1 on failure.
 */
int solver_lns_optimize(SolveCtx *ctx, HPWLCostCtx *hctx,
                         const LNSOpts *opts,
                         int32_t *out_positions, LNSResult *result);

