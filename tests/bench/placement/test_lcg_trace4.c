/* Force a conflict and trace LCG analysis step by step. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_placement.h"
#include "zsp_lcg.h"

int main(void) {
    /* 3 rects of 10x10 on 25x25 canvas.
     * Force: all at x=0, y=0 -> rect1 y must be >= 10.
     * Then force rect1 y=0 -> CONFLICT (rects 0 and 1 overlap). */
    int n = 3, canvas = 25;

    size_t sp_sz = 65536;
    void *sp_buf = calloc(1, sp_sz);
    SolveProblem *sp = solve_problem_init(sp_buf, sp_sz);

    for (int i = 0; i < n; i++) {
        problem_add_var(sp, (uint32_t)i, 32, 0, 0, canvas - 10);
        problem_add_var(sp, (uint32_t)(n+i), 32, 0, 0, canvas - 10);
    }
    uint32_t src[6];
    for (int i = 0; i < 2*n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2*n), src);

    size_t ctx_sz = 1 << 22;
    void *ctx_buf = calloc(1, ctx_sz);
    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, ctx_sz);
    SolveCtx *ctx = solver_create(ctx_buf, ctx_sz, ba);
    solver_compile(ctx, sp);

    RectSpec rects[3];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n+i);
        rects[i].width = 10; rects[i].height = 10;
        rects[i].halo_l = 0; rects[i].halo_r = 0;
        rects[i].halo_t = 0; rects[i].halo_b = 0;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    solver_enable_lcg(ctx);

    printf("=== Setup: rect0=(0,0), rect1=(0,*) forcing x overlap ===\n\n");

    /* Level 1: rect0 at (0, 0) */
    trail_push_level(ctx);
    ctx_tighten_lb64(ctx, 0, 0); ctx_tighten_ub64(ctx, 0, 0);  /* x0=0 */
    ctx_tighten_lb64(ctx, 3, 0); ctx_tighten_ub64(ctx, 3, 0);  /* y0=0 */
    PropResult pr = solver_propagate(ctx);
    printf("Level 1 (rect0 at 0,0): %s\n", pr==PROP_OK?"OK":"CONFLICT");

    /* Print propagated domains */
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        int64_t lo = var_lo64(ctx, &ctx->vars[i]);
        int64_t hi = var_hi64(ctx, &ctx->vars[i]);
        if (lo != hi) printf("  var%u: [%ld, %ld]\n", i, lo, hi);
    }

    /* Level 2: rect1 x=0 */
    trail_push_level(ctx);
    ctx_tighten_lb64(ctx, 1, 0); ctx_tighten_ub64(ctx, 1, 0);  /* x1=0 */
    pr = solver_propagate(ctx);
    printf("\nLevel 2 (rect1 x=0): %s\n", pr==PROP_OK?"OK":"CONFLICT");
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        int64_t lo = var_lo64(ctx, &ctx->vars[i]);
        int64_t hi = var_hi64(ctx, &ctx->vars[i]);
        if (lo != hi) printf("  var%u: [%ld, %ld]\n", i, lo, hi);
    }

    /* Level 3: rect1 y=0 -> should conflict with rect0 */
    trail_push_level(ctx);
    pr = ctx_tighten_lb64(ctx, 4, 0);
    printf("\nLevel 3: set y1 lb=0: %s\n", pr==PROP_OK?"OK":"CONFLICT");
    if (pr == PROP_OK) {
        pr = ctx_tighten_ub64(ctx, 4, 0);  /* y1=0 -> overlap with rect0! */
        printf("  set y1 ub=0: %s\n", pr==PROP_OK?"OK":"CONFLICT");
    }
    if (pr == PROP_OK)
        pr = solver_propagate(ctx);

    if (pr == PROP_CONFLICT) {
        printf("\nCONFLICT at level %u\n", ctx->decision_level);

        /* Print trail */
        printf("Trail:\n");
        TrailEntry *te = ctx->trail_top;
        while (te) {
            printf("  var%u %s old=%ld level=%u prop=%s\n",
                   te->var_id,
                   te->kind==TRAIL_LB?"LB":"UB",
                   te->old_value, te->decision_level,
                   te->prop_ref==EXPR_NULL?"DEC":"PROP");
            te = te->prev;
        }

        /* Find conflict var */
        for (uint32_t i = 0; i < ctx->n_vars; i++) {
            int64_t lo = var_lo64(ctx, &ctx->vars[i]);
            int64_t hi = var_hi64(ctx, &ctx->vars[i]);
            if (lo > hi) printf("Conflict var: var%u [%ld,%ld]\n", i, lo, hi);
        }
        printf("conflict_prop_ref: %s\n",
               ctx->conflict_prop_ref==EXPR_NULL?"NULL":"SET");

        /* Run analysis */
        LCGCtx *lcg = (LCGCtx *)ctx->lcg_ctx;
        Literal learnt[MAX_CLAUSE_LITS];
        uint32_t n_learnt = 0, bt_level = 0;
        int rc = lcg_analyze_conflict(lcg, ctx, learnt, &n_learnt, &bt_level);
        printf("\nAnalysis: rc=%d, n_learnt=%u, bt_level=%u\n",
               rc, n_learnt, bt_level);
        for (uint32_t i = 0; i < n_learnt; i++)
            printf("  learnt[%u]: var%u %s %d\n", i,
                   learnt[i].var_id, learnt[i].is_lb ? ">=" : "<=",
                   learnt[i].bound);
    }

    solver_disable_lcg(ctx);
    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return 0;
}
