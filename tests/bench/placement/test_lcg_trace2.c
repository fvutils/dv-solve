/* Trace LCG on the N=10, canvas=70 problem that falsely reports UNSAT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dvs_problem.h"
#include "dvs_ctx.h"
#include "dvs_search.h"
#include "dvs_placement.h"
#include "dvs_lcg.h"

static void print_literal(Literal lit) {
    printf("var%u %s %d", lit.var_id, lit.is_lb ? ">=" : "<=", lit.bound);
}

int main(void) {
    int n = 10, canvas = 70;

    size_t sp_sz = 131072;
    void *sp_buf = calloc(1, sp_sz);
    dvs_problem_t *sp = solve_problem_init(sp_buf, sp_sz);

    for (int i = 0; i < n; i++) {
        int w = 10 + (i % 8) * 5;
        int h = 8 + (i % 6) * 4;
        int xl = canvas - w; if (xl < 0) xl = 0;
        int yl = canvas - h; if (yl < 0) yl = 0;
        problem_add_var(sp, (uint32_t)i, 32, 0, 0, xl);
        problem_add_var(sp, (uint32_t)(n+i), 32, 0, 0, yl);
    }
    uint32_t src[128];
    for (int i = 0; i < 2*n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2*n), src);

    size_t ctx_sz = 1 << 23;
    void *ctx_buf = calloc(1, ctx_sz);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, ctx_sz);
    dvs_ctx_t *ctx = dvs_solver_create(ctx_buf, ctx_sz, ba);
    dvs_solver_compile(ctx, sp);

    RectSpec rects[10];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n+i);
        rects[i].width = 10 + (i % 8) * 5;
        rects[i].height = 8 + (i % 6) * 4;
        rects[i].halo_l = 0; rects[i].halo_r = 0;
        rects[i].halo_t = 0; rects[i].halo_b = 0;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    printf("Rects:\n");
    for (int i = 0; i < n; i++) {
        printf("  rect %d: %dx%d, x=var%d[0,%d], y=var%d[0,%d]\n",
               i, rects[i].width, rects[i].height,
               i, canvas - rects[i].width,
               n+i, canvas - rects[i].height);
    }

    /* First verify feasibility without LCG */
    printf("\n=== Without LCG ===\n");
    {
        dvs_solve_opts_t sopts = {0};
        sopts.seed = 42; sopts.max_conflicts = 200;
        sopts.max_restarts = 10000; sopts.max_shave_iters = 0;
        dvs_result_t sr = dvs_solver_solve(ctx, &sopts);
        printf("result=%s, conflicts=%lu\n",
               sr==0?"FEASIBLE":sr==1?"UNSAT":"TIMEOUT", ctx->conflict_count);
        if (sr == 0) {
            for (int i = 0; i < n; i++)
                printf("  rect %d: (%ld, %ld)\n", i,
                       dvs_solver_get_value(ctx, (uint32_t)i),
                       dvs_solver_get_value(ctx, (uint32_t)(n+i)));
        }
    }

    dvs_solver_reset(ctx);

    /* Now with LCG */
    printf("\n=== With LCG ===\n");
    dvs_solver_enable_lcg(ctx);
    {
        dvs_solve_opts_t sopts = {0};
        sopts.seed = 42; sopts.max_conflicts = 200;
        sopts.max_restarts = 10000; sopts.max_shave_iters = 0;
        dvs_result_t sr = dvs_solver_solve(ctx, &sopts);
        printf("result=%s, conflicts=%lu\n",
               sr==0?"FEASIBLE":sr==1?"UNSAT":"TIMEOUT", ctx->conflict_count);

        LCGCtx *lcg = (LCGCtx *)ctx->lcg_ctx;
        printf("clauses_learnt=%lu, analyses=%lu\n", lcg->n_learnt, lcg->n_analyses);

        printf("\nLearned clauses (%u total):\n", lcg->clause_db.n_clauses);
        for (uint32_t ci = 0; ci < lcg->clause_db.n_clauses && ci < 20; ci++) {
            Clause *cl = lcg->clause_db.clauses[ci];
            if (!cl) continue;
            Literal *lits = (Literal *)(cl + 1);
            printf("  C%u (lbd=%u, %u lits):", ci, cl->lbd, cl->n_lits);
            for (uint32_t li = 0; li < cl->n_lits; li++) {
                printf(" ");
                print_literal(lits[li]);
            }
            printf("\n");
        }

        if (sr == 0) {
            for (int i = 0; i < n; i++)
                printf("  rect %d: (%ld, %ld)\n", i,
                       dvs_solver_get_value(ctx, (uint32_t)i),
                       dvs_solver_get_value(ctx, (uint32_t)(n+i)));
        }
    }
    dvs_solver_disable_lcg(ctx);

    dvs_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return 0;
}
