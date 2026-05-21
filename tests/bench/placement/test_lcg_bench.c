/* Comprehensive LCG benchmark: tight placement problems. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_placement.h"
#include "zsp_lcg.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct {
    int result;
    uint64_t conflicts;
    double time_ms;
    uint64_t clauses;
} RunResult;

static RunResult run_test(int n, int canvas, int halo,
                           int use_lcg, uint32_t max_restarts) {
    RunResult rr = {-1, 0, 0.0, 0};

    size_t sp_sz = 131072;
    void *sp_buf = calloc(1, sp_sz);
    SolveProblem *sp = solve_problem_init(sp_buf, sp_sz);
    if (!sp) { free(sp_buf); return rr; }

    for (int i = 0; i < n; i++) {
        int w = 10 + (i % 8) * 5;
        int h = 8 + (i % 6) * 4;
        int xl = canvas - w - 2*halo; if (xl < 0) xl = 0;
        int yl = canvas - h - 2*halo; if (yl < 0) yl = 0;
        problem_add_var(sp, (uint32_t)i, 32, 0, 0, xl);
        problem_add_var(sp, (uint32_t)(n+i), 32, 0, 0, yl);
    }
    uint32_t src[256];
    for (int i = 0; i < 2*n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2*n), src);

    size_t ctx_sz = 1 << 23;
    void *ctx_buf = calloc(1, ctx_sz);
    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, ctx_sz);
    SolveCtx *ctx = solver_create(ctx_buf, ctx_sz, ba);
    if (solver_compile(ctx, sp) != 0) { rr.result = -2; goto done; }

    RectSpec rects[128];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n+i);
        rects[i].width = 10 + (i % 8) * 5;
        rects[i].height = 8 + (i % 6) * 4;
        rects[i].halo_l = halo; rects[i].halo_r = halo;
        rects[i].halo_t = halo; rects[i].halo_b = halo;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    if (use_lcg) solver_enable_lcg(ctx);

    SolveOpts sopts = {0};
    sopts.seed = 42;
    sopts.max_conflicts = 200;
    sopts.max_restarts = max_restarts;
    sopts.max_shave_iters = 0;

    double t0 = now_sec();
    rr.result = solver_solve(ctx, &sopts);
    double t1 = now_sec();
    rr.conflicts = ctx->conflict_count;
    rr.time_ms = (t1 - t0) * 1000.0;

    if (ctx->lcg_ctx) {
        LCGCtx *lcg = (LCGCtx *)ctx->lcg_ctx;
        rr.clauses = lcg->n_learnt;
        solver_disable_lcg(ctx);
    }

done:
    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return rr;
}

int main(void) {
    setbuf(stdout, NULL);
    printf("=== LCG Comprehensive Benchmark ===\n\n");
    printf("%-5s %-6s %-5s | %-10s %-10s %-10s | %-10s %-10s %-10s %-6s | %-8s\n",
           "N", "Canvas", "Halo",
           "OFF_Result", "OFF_Confl", "OFF_ms",
           "LCG_Result", "LCG_Confl", "LCG_ms", "Claus",
           "Speedup");
    printf("------------------------------------------------------------------------------------\n");

    struct { int n, canvas, halo; uint32_t mr; } C[] = {
        /* Easy */
        {10, 100, 0, 5000},
        {15, 120, 0, 5000},
        {20, 150, 0, 5000},
        /* Medium */
        {10, 80,  0, 10000},
        {15, 100, 0, 10000},
        {20, 130, 0, 10000},
        /* Tight */
        {10, 70,  0, 20000},
        {10, 80,  2, 20000},
        {15, 100, 2, 20000},
        /* Very tight */
        {10, 65,  0, 20000},
        {15, 90,  0, 20000},
    };
    int nc = sizeof(C) / sizeof(C[0]);

    for (int ci = 0; ci < nc; ci++) {
        RunResult r0 = run_test(C[ci].n, C[ci].canvas, C[ci].halo, 0, C[ci].mr);
        RunResult r1 = run_test(C[ci].n, C[ci].canvas, C[ci].halo, 1, C[ci].mr);
        const char *s0 = r0.result==0?"OK":r0.result==1?"UNSAT":"TIMEOUT";
        const char *s1 = r1.result==0?"OK":r1.result==1?"UNSAT":"TIMEOUT";
        double speedup = r1.time_ms > 0 ? r0.time_ms / r1.time_ms : 0;
        printf("%-5d %-6d %-5d | %-10s %-10lu %-10.1f | %-10s %-10lu %-10.1f %-6lu | %-8.1fx\n",
               C[ci].n, C[ci].canvas, C[ci].halo,
               s0, r0.conflicts, r0.time_ms,
               s1, r1.conflicts, r1.time_ms, r1.clauses,
               speedup);
    }

    printf("\n");
    return 0;
}
