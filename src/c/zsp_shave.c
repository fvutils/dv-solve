/*
 * zsp_shave.c -- Pre-search bounds shaving (SAC on bounds).
 *
 * For each variable, test whether the current upper/lower bound value
 * is feasible by tentatively assigning it and running propagation.
 * If propagation detects a conflict, permanently exclude that bound
 * at level 0.  Repeat until fixed point or budget exhaustion.
 */
#include <stdint.h>
#include "zsp_shave.h"
#include "zsp_ctx.h"
#include "zsp_trail.h"

PropResult bounds_shave(SolveCtx *ctx, uint32_t max_iters) {
    uint32_t total_removed = 0;
    int changed = 1;

    while (changed && total_removed < max_iters) {
        changed = 0;

        for (uint32_t x = 0; x < ctx->n_vars && total_removed < max_iters; x++) {
            /* Shave upper bound */
            for (;;) {
                int64_t hi = var_hi64(ctx, &ctx->vars[x]);
                int64_t lo = var_lo64(ctx, &ctx->vars[x]);
                if (lo >= hi) break;  /* singleton or empty */

                /* Tentatively assign x = hi at level 1 */
                trail_push_level(ctx);
                PropResult pr = ctx_tighten_lb64(ctx, x, hi);
                if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x, hi);
                if (pr == PROP_OK) pr = solver_propagate(ctx);
                trail_backtrack(ctx, 0);

                if (pr == PROP_CONFLICT) {
                    /* hi is infeasible; permanently exclude it */
                    PropResult er = ctx_tighten_ub64(ctx, x, hi - 1);
                    if (er == PROP_CONFLICT) return PROP_CONFLICT;
                    if (solver_propagate(ctx) == PROP_CONFLICT)
                        return PROP_CONFLICT;
                    changed = 1;
                    total_removed++;
                } else {
                    break;  /* bound is feasible */
                }
            }

            /* Shave lower bound */
            for (;;) {
                int64_t lo = var_lo64(ctx, &ctx->vars[x]);
                int64_t hi = var_hi64(ctx, &ctx->vars[x]);
                if (lo >= hi) break;

                trail_push_level(ctx);
                PropResult pr = ctx_tighten_lb64(ctx, x, lo);
                if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x, lo);
                if (pr == PROP_OK) pr = solver_propagate(ctx);
                trail_backtrack(ctx, 0);

                if (pr == PROP_CONFLICT) {
                    PropResult er = ctx_tighten_lb64(ctx, x, lo + 1);
                    if (er == PROP_CONFLICT) return PROP_CONFLICT;
                    if (solver_propagate(ctx) == PROP_CONFLICT)
                        return PROP_CONFLICT;
                    changed = 1;
                    total_removed++;
                } else {
                    break;
                }
            }
        }
    }

    return PROP_OK;
}
