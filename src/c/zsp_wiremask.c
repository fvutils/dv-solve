#include <stdint.h>
#include <limits.h>
#include "zsp_wiremask.h"
#include "zsp_ctx.h"

/* ================================================================== */
/* Wire Mask Value Selection                                           */
/*                                                                     */
/* For a given position variable (x or y), compute the incremental    */
/* HPWL cost at each feasible value in the domain and return the      */
/* value with minimum cost.                                           */
/* ================================================================== */

/* Find which macro and axis (0=x, 1=y) a variable corresponds to. */
static int _find_macro_axis(const WireMaskCtx *wm, uint32_t var_id,
                             uint32_t *out_macro, int *out_is_y) {
    for (uint32_t m = 0; m < wm->n_macros; m++) {
        if (wm->macros[m].x_var_id == var_id) {
            *out_macro = m;
            *out_is_y = 0;
            return 1;
        }
        if (wm->macros[m].y_var_id == var_id) {
            *out_macro = m;
            *out_is_y = 1;
            return 1;
        }
    }
    return 0;
}

int32_t wiremask_select_value(const SolveCtx *ctx, const WireMaskCtx *wm,
                               uint32_t var_id) {
    if (!wm || !ctx) {
        return ctx->vars[var_id].lo;
    }

    uint32_t macro_idx;
    int is_y;
    if (!_find_macro_axis(wm, var_id, &macro_idx, &is_y)) {
        return ctx->vars[var_id].lo;
    }

    const Variable *v = &ctx->vars[var_id];
    int32_t lo = v->lo;
    int32_t hi = v->hi;

    /* For very large domains, fall back to midpoint to avoid O(n) scan */
    if ((int64_t)hi - (int64_t)lo > 10000) {
        return lo + (hi - lo) / 2;
    }

    uint32_t n_nets_for_macro = wm->macro_net_count[macro_idx];
    if (n_nets_for_macro == 0) {
        return lo + (hi - lo) / 2;
    }

    int64_t best_cost = INT64_MAX;
    int32_t best_val = lo;

    for (int32_t p = lo; p <= hi; p++) {
        int64_t cost = 0;

        for (uint32_t ni = 0; ni < n_nets_for_macro; ni++) {
            uint32_t net_idx = wm->macro_net_ids[macro_idx][ni];
            const WireMaskNet *net = &wm->nets[net_idx];

            /* Compute current bounding box of placed pins in this net
             * (excluding the current macro) */
            int32_t net_min = INT32_MAX;
            int32_t net_max = INT32_MIN;
            int32_t my_pin_offset = 0;

            for (uint32_t pi = 0; pi < net->n_pins; pi++) {
                uint32_t mid = net->macro_ids[pi];
                if (mid == macro_idx) {
                    my_pin_offset = is_y ? net->pin_y_offsets[pi]
                                         : net->pin_x_offsets[pi];
                    continue;
                }

                /* Check if this macro's position is determined (singleton) */
                uint32_t other_var = is_y ? wm->macros[mid].y_var_id
                                          : wm->macros[mid].x_var_id;
                const Variable *ov = &ctx->vars[other_var];
                if (ov->lo == ov->hi) {
                    int32_t pin_pos = ov->lo + (is_y ? net->pin_y_offsets[pi]
                                                      : net->pin_x_offsets[pi]);
                    if (pin_pos < net_min) net_min = pin_pos;
                    if (pin_pos > net_max) net_max = pin_pos;
                }
            }

            if (net_min == INT32_MAX) continue;  /* no placed pins */

            /* Cost of placing this macro's pin at p + my_pin_offset */
            int32_t my_pin_pos = p + my_pin_offset;
            if (my_pin_pos < net_min) {
                cost += (int64_t)(net_min - my_pin_pos);
            } else if (my_pin_pos > net_max) {
                cost += (int64_t)(my_pin_pos - net_max);
            }
            /* If within bounding box, cost contribution is 0 */
        }

        if (cost < best_cost) {
            best_cost = cost;
            best_val = p;
        }
    }

    return best_val;
}

/* ================================================================== */
/* Greedy placement using wire mask                                    */
/* ================================================================== */

int wiremask_greedy_place(SolveCtx *ctx, const WireMaskCtx *wm,
                           const uint32_t *order, uint32_t n_macros) {
    if (!ctx || !wm || !order) return -1;

    for (uint32_t step = 0; step < n_macros; step++) {
        uint32_t m = order[step];
        if (m >= wm->n_macros) return -1;

        uint32_t x_var = wm->macros[m].x_var_id;
        uint32_t y_var = wm->macros[m].y_var_id;

        /* Select best x and y values using wire mask */
        int32_t best_x = wiremask_select_value(ctx, wm, x_var);
        int32_t best_y = wiremask_select_value(ctx, wm, y_var);

        /* Pin the macro to the selected position */
        if (solver_pin_var(ctx, x_var, best_x) != 0) return -1;
        if (solver_pin_var(ctx, y_var, best_y) != 0) return -1;
    }

    return 0;
}
