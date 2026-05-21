/* Diagnostic test: trace LCG conflict analysis on a small problem.
 *
 * 4 rects on a tight canvas to force conflicts, then print what
 * the conflict analysis produces.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_placement.h"
#include "zsp_lcg.h"

int main(void) {
    /* 4 rects of size 10x10 on a 25x25 canvas, no halo.
     * Total area = 400, canvas area = 625. Tight but feasible. */
    int n = 4, canvas = 25;

    size_t sp_sz = 65536;
    void *sp_buf = calloc(1, sp_sz);
    SolveProblem *sp = solve_problem_init(sp_buf, sp_sz);

    for (int i = 0; i < n; i++) {
        problem_add_var(sp, (uint32_t)i,     32, 0, 0, canvas - 10);
        problem_add_var(sp, (uint32_t)(n+i), 32, 0, 0, canvas - 10);
    }
    uint32_t src[8];
    for (int i = 0; i < 2*n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2*n), src);

    size_t ctx_sz = 1 << 22;
    void *ctx_buf = calloc(1, ctx_sz);
    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, ctx_sz);
    SolveCtx *ctx = solver_create(ctx_buf, ctx_sz, ba);
    solver_compile(ctx, sp);

    RectSpec rects[4];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n+i);
        rects[i].width = 10; rects[i].height = 10;
        rects[i].halo_l = 0; rects[i].halo_r = 0;
        rects[i].halo_t = 0; rects[i].halo_b = 0;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    printf("=== Without LCG ===\n");
    {
        SolveOpts sopts = {0};
        sopts.seed = 42; sopts.max_conflicts = 200;
        sopts.max_restarts = 5000; sopts.max_shave_iters = 0;
        SolveResult sr = solver_solve(ctx, &sopts);
        printf("result=%d, conflicts=%lu\n", sr, ctx->conflict_count);
        if (sr == 0) {
            for (int i = 0; i < n; i++)
                printf("  rect %d: (%ld, %ld)\n", i,
                       solver_get_value(ctx, (uint32_t)i),
                       solver_get_value(ctx, (uint32_t)(n+i)));
        }
    }

    /* Reset and try with LCG */
    solver_reset(ctx);
    /* Re-add the propagator after reset since it was cleared */
    /* Actually, solver_reset preserves propagators. Re-enqueue them. */

    printf("\n=== With LCG ===\n");
    solver_enable_lcg(ctx);
    {
        SolveOpts sopts = {0};
        sopts.seed = 42; sopts.max_conflicts = 200;
        sopts.max_restarts = 5000; sopts.max_shave_iters = 0;
        SolveResult sr = solver_solve(ctx, &sopts);
        printf("result=%d, conflicts=%lu\n", sr, ctx->conflict_count);
        if (sr == 0) {
            for (int i = 0; i < n; i++)
                printf("  rect %d: (%ld, %ld)\n", i,
                       solver_get_value(ctx, (uint32_t)i),
                       solver_get_value(ctx, (uint32_t)(n+i)));
        }
        LCGCtx *lcg = (LCGCtx *)ctx->lcg_ctx;
        printf("clauses=%lu, analyses=%lu, db_props=%lu, db_conflicts=%lu\n",
               lcg->n_learnt, lcg->n_analyses,
               lcg->clause_db.n_propagations, lcg->clause_db.n_conflicts);
    }
    solver_disable_lcg(ctx);

    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return 0;
}
