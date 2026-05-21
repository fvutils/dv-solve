/* Trace LCG on a minimal problem to debug unsound clauses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_placement.h"
#include "zsp_lcg.h"

static void print_literal(const char *prefix, Literal lit) {
    printf("%s var%u %s %d", prefix, lit.var_id,
           lit.is_lb ? ">=" : "<=", lit.bound);
}

int main(void) {
    /* 4 rects on a 30x30 canvas. Feasible but tight. */
    int n = 4, canvas = 30;

    size_t sp_sz = 65536;
    void *sp_buf = calloc(1, sp_sz);
    SolveProblem *sp = solve_problem_init(sp_buf, sp_sz);

    /* Rects: 10x10, 15x12, 10x10, 15x12 */
    int widths[]  = {10, 15, 10, 15};
    int heights[] = {10, 12, 10, 12};

    for (int i = 0; i < n; i++) {
        problem_add_var(sp, (uint32_t)i,     32, 0, 0, canvas - widths[i]);
        problem_add_var(sp, (uint32_t)(n+i), 32, 0, 0, canvas - heights[i]);
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
        rects[i].width = widths[i]; rects[i].height = heights[i];
        rects[i].halo_l = 0; rects[i].halo_r = 0;
        rects[i].halo_t = 0; rects[i].halo_b = 0;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    printf("Variables:\n");
    for (int i = 0; i < 2*n; i++) {
        printf("  var%d: [%ld, %ld]  (%s of rect %d)\n",
               i, var_lo64(ctx, &ctx->vars[i]),
               var_hi64(ctx, &ctx->vars[i]),
               i < n ? "x" : "y", i < n ? i : i-n);
    }

    /* Test WITHOUT LCG */
    printf("\n=== Without LCG ===\n");
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

    solver_reset(ctx);

    /* Test WITH LCG - trace learned clauses */
    printf("\n=== With LCG (tracing) ===\n");
    solver_enable_lcg(ctx);

    SolveOpts sopts = {0};
    sopts.seed = 42; sopts.max_conflicts = 200;
    sopts.max_restarts = 100; sopts.max_shave_iters = 0;
    SolveResult sr = solver_solve(ctx, &sopts);
    printf("result=%d, conflicts=%lu\n", sr, ctx->conflict_count);

    LCGCtx *lcg = (LCGCtx *)ctx->lcg_ctx;
    printf("clauses=%lu, analyses=%lu\n", lcg->n_learnt, lcg->n_analyses);

    /* Print learned clauses */
    printf("\nLearned clauses:\n");
    for (uint32_t ci = 0; ci < lcg->clause_db.n_clauses; ci++) {
        Clause *cl = lcg->clause_db.clauses[ci];
        if (!cl) continue;
        Literal *lits = (Literal *)(cl + 1);
        printf("  clause %u (lbd=%u, %u lits):", ci, cl->lbd, cl->n_lits);
        for (uint32_t li = 0; li < cl->n_lits; li++) {
            print_literal(" ", lits[li]);
            /* Check if literal is currently true or false */
            int t = literal_is_true(ctx, lits[li]);
            int f = literal_is_false(ctx, lits[li]);
            printf("[%s]", t ? "T" : f ? "F" : "?");
        }
        printf("\n");
    }

    if (sr == 0) {
        for (int i = 0; i < n; i++)
            printf("  rect %d: (%ld, %ld)\n", i,
                   solver_get_value(ctx, (uint32_t)i),
                   solver_get_value(ctx, (uint32_t)(n+i)));
    }

    /* Verify: check if the known feasible solution violates any learned clause */
    if (sr != 0) {
        printf("\nValidation: checking if known solution violates clauses...\n");
        /* Try the solution from the non-LCG run manually */
        /* We'll just reset and try without LCG to get positions */
        solver_disable_lcg(ctx);
        solver_reset(ctx);
        SolveOpts sopts2 = {0};
        sopts2.seed = 42; sopts2.max_conflicts = 200;
        sopts2.max_restarts = 5000; sopts2.max_shave_iters = 0;
        SolveResult sr2 = solver_solve(ctx, &sopts2);
        if (sr2 == 0) {
            printf("  Known feasible solution:\n");
            int64_t vals[8];
            for (int i = 0; i < 2*n; i++)
                vals[i] = solver_get_value(ctx, (uint32_t)i);
            for (int i = 0; i < n; i++)
                printf("    rect %d: (%ld, %ld)\n", i, vals[i], vals[n+i]);
        }
    }

    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return 0;
}
