/* Ultra-detailed LCG trace on a minimal problem. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_placement.h"
#include "zsp_lcg.h"

static void dump_trail(SolveCtx *ctx) {
    printf("  Trail (newest first):\n");
    TrailEntry *e = ctx->trail_top;
    int count = 0;
    while (e && count < 30) {
        printf("    var%u %s old=%ld level=%u prop_ref=%s\n",
               e->var_id,
               e->kind == TRAIL_LB ? "LB" : e->kind == TRAIL_UB ? "UB" : "HOLE",
               e->old_value, e->decision_level,
               e->prop_ref == EXPR_NULL ? "DECISION" : "PROP");
        e = e->prev;
        count++;
    }
    printf("  Domains:\n");
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        int64_t lo = var_lo64(ctx, &ctx->vars[i]);
        int64_t hi = var_hi64(ctx, &ctx->vars[i]);
        if (lo != hi || lo > hi)
            printf("    var%u: [%ld, %ld]%s\n", i, lo, hi,
                   lo > hi ? " EMPTY!" : "");
    }
}

/* Hook: custom solve that stops after first conflict for tracing */
int main(void) {
    int n = 3, canvas = 25;

    /* 3 rects of sizes 10x10, 10x10, 10x10 on 25x25 canvas.
     * Total area = 300, canvas = 625. Easy. */
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

    printf("Initial domains:\n");
    for (uint32_t i = 0; i < ctx->n_vars; i++)
        printf("  var%u: [%ld, %ld]\n", i,
               var_lo64(ctx, &ctx->vars[i]),
               var_hi64(ctx, &ctx->vars[i]));

    /* Manual search: make a decision, propagate, check for conflict */
    printf("\n--- Level 0 propagation ---\n");
    solver_propagate(ctx);
    dump_trail(ctx);

    printf("\n--- Decision: var0 = 0 (x of rect 0 = 0) ---\n");
    trail_push_level(ctx);
    ctx_tighten_lb64(ctx, 0, 0);
    ctx_tighten_ub64(ctx, 0, 0);
    PropResult pr = solver_propagate(ctx);
    dump_trail(ctx);
    printf("  propagation result: %s\n", pr == PROP_OK ? "OK" : "CONFLICT");

    if (pr == PROP_OK) {
        printf("\n--- Decision: var3 = 0 (y of rect 0 = 0) ---\n");
        trail_push_level(ctx);
        ctx_tighten_lb64(ctx, 3, 0);
        ctx_tighten_ub64(ctx, 3, 0);
        pr = solver_propagate(ctx);
        dump_trail(ctx);
        printf("  propagation result: %s\n", pr == PROP_OK ? "OK" : "CONFLICT");
    }

    if (pr == PROP_OK) {
        printf("\n--- Decision: var1 = 0 (x of rect 1 = 0) ---\n");
        trail_push_level(ctx);
        ctx_tighten_lb64(ctx, 1, 0);
        ctx_tighten_ub64(ctx, 1, 0);
        pr = solver_propagate(ctx);
        dump_trail(ctx);
        printf("  propagation result: %s\n", pr == PROP_OK ? "OK" : "CONFLICT");
    }

    if (pr == PROP_CONFLICT) {
        printf("\n--- Conflict! Running LCG analysis ---\n");
        solver_enable_lcg(ctx);
        LCGCtx *lcg = (LCGCtx *)ctx->lcg_ctx;

        /* Find conflict var */
        for (uint32_t i = 0; i < ctx->n_vars; i++) {
            int64_t lo = var_lo64(ctx, &ctx->vars[i]);
            int64_t hi = var_hi64(ctx, &ctx->vars[i]);
            if (lo > hi) printf("  Conflict var: var%u [%ld, %ld]\n", i, lo, hi);
        }
        printf("  conflict_prop_ref: %s\n",
               ctx->conflict_prop_ref == EXPR_NULL ? "NULL" : "SET");

        Literal learnt[MAX_CLAUSE_LITS];
        uint32_t n_learnt = 0, bt_level = 0;
        int rc = lcg_analyze_conflict(lcg, ctx, learnt, &n_learnt, &bt_level);
        printf("  analysis rc=%d, n_learnt=%u, bt_level=%u\n", rc, n_learnt, bt_level);
        for (uint32_t i = 0; i < n_learnt; i++) {
            printf("    learnt[%u]: var%u %s %d\n", i,
                   learnt[i].var_id, learnt[i].is_lb ? ">=" : "<=",
                   learnt[i].bound);
        }
        solver_disable_lcg(ctx);
    }

    /* Now solve normally without LCG to verify feasibility */
    solver_reset(ctx);
    printf("\n--- Full solve (no LCG) ---\n");
    SolveOpts sopts = {0};
    sopts.seed = 42; sopts.max_conflicts = 200;
    sopts.max_restarts = 5000; sopts.max_shave_iters = 0;
    SolveResult sr = solver_solve(ctx, &sopts);
    printf("result=%s, conflicts=%lu\n",
           sr==0?"FEASIBLE":sr==1?"UNSAT":"TIMEOUT", ctx->conflict_count);
    if (sr == 0) {
        for (int i = 0; i < n; i++)
            printf("  rect %d: (%ld, %ld)\n", i,
                   solver_get_value(ctx, (uint32_t)i),
                   solver_get_value(ctx, (uint32_t)(n+i)));
    }

    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return 0;
}
