#ifndef ZSP_WIREMASK_H
#define ZSP_WIREMASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolveCtx SolveCtx;

/* ================================================================== */
/* Wire Mask Value Selector                                            */
/*                                                                     */
/* For macro placement: when branching on a position variable, compute */
/* incremental HPWL for each candidate value and pick the minimum.    */
/*                                                                     */
/* The wire mask concept from WireMask-BBO:                           */
/*   For each net containing the current macro, for each candidate    */
/*   position p on the relevant axis:                                 */
/*     cost += max(0, net_min - p) + max(0, p - net_max)             */
/*   Place at argmin(cost).                                           */
/* ================================================================== */

/* Net descriptor: a net connects pin offsets on macros. */
typedef struct {
    uint32_t  n_pins;            /* number of pins in this net        */
    uint32_t *macro_ids;         /* macro index per pin               */
    int32_t  *pin_x_offsets;     /* x offset of pin from macro origin */
    int32_t  *pin_y_offsets;     /* y offset of pin from macro origin */
} WireMaskNet;

/* Macro descriptor: maps macro index to solver variables. */
typedef struct {
    uint32_t x_var_id;           /* solver variable for x position   */
    uint32_t y_var_id;           /* solver variable for y position   */
} WireMaskMacro;

/* Wire mask context: holds net/macro metadata for value selection. */
typedef struct {
    uint32_t        n_macros;
    uint32_t        n_nets;
    WireMaskMacro  *macros;      /* array[n_macros] */
    WireMaskNet    *nets;        /* array[n_nets]   */
    /* Per-net index: for each macro, which nets does it belong to? */
    uint32_t       *macro_net_count;  /* array[n_macros] */
    uint32_t      **macro_net_ids;    /* array[n_macros] -> net indices */
} WireMaskCtx;

/**
 * Compute the best value for a position variable using wire mask scoring.
 *
 * @param ctx       Solver context (for reading current variable bounds).
 * @param wm        Wire mask context with net/macro data.
 * @param var_id    The variable being assigned.
 * @return Best value in [lo, hi] that minimizes incremental HPWL.
 *         Returns lo if no wire mask info is available for this variable.
 */
int32_t wiremask_select_value(const SolveCtx *ctx, const WireMaskCtx *wm,
                               uint32_t var_id);

/**
 * Run a greedy placement using wire mask scoring.
 *
 * Places macros one at a time in the given order. For each macro, evaluates
 * all feasible positions and picks the one minimizing incremental HPWL.
 *
 * @param ctx       Solver context (must be compiled with position variables).
 * @param wm        Wire mask context.
 * @param order     Array of macro indices defining placement order.
 * @param n_macros  Number of macros to place.
 * @return 0 on success, -1 if any macro cannot be legally placed.
 */
int wiremask_greedy_place(SolveCtx *ctx, const WireMaskCtx *wm,
                           const uint32_t *order, uint32_t n_macros);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_WIREMASK_H */
