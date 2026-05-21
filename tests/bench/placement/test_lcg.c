/* Test LCG (Lazy Clause Generation) on tight placement problems.
 *
 * Compares solver performance with and without LCG enabled.
 *
 * Build: gcc -O2 -Isrc/c tests/bench/placement/test_lcg.c -Lbuild -ldv_solve -Wl,-rpath,build -o build/test_lcg
 */
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
} RunResult;

static RunResult run_test(int n, int canvas, int halo,
                           int use_lcg, int max_restarts) {
    RunResult rr = {-1, 0, 0.0};

    size_t sp_sz = 65536;
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
    uint32_t src[128];
    for (int i = 0; i < 2*n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2*n), src);

    size_t ctx_sz = 1 << 22;
    void *ctx_buf = calloc(1, ctx_sz);
    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, ctx_sz);
    SolveCtx *ctx = solver_create(ctx_buf, ctx_sz, ba);
    if (solver_compile(ctx, sp) != 0) { rr.result = -2; goto done; }

    RectSpec rects[64];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n+i);
        rects[i].width = 10 + (i % 8) * 5;
        rects[i].height = 8 + (i % 6) * 4;
        rects[i].halo_l = halo; rects[i].halo_r = halo;
        rects[i].halo_t = halo; rects[i].halo_b = halo;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    if (use_lcg == 1) {
        /* Pre-enable LCG */
        solver_enable_lcg(ctx);
    } else if (use_lcg == 2) {
        /* Let auto-activation handle it (don't pre-enable) */
        /* The search loop will enable LCG after 100 restarts */
    }

    SolveOpts sopts = {0};
    sopts.seed = 42;
    sopts.max_conflicts = 200;
    sopts.max_restarts = (uint32_t)max_restarts;
    sopts.max_shave_iters = 0;

    double t0 = now_sec();
    rr.result = solver_solve(ctx, &sopts);
    double t1 = now_sec();
    rr.conflicts = ctx->conflict_count;
    rr.time_ms = (t1 - t0) * 1000.0;

    if (use_lcg >= 1) {
        LCGCtx *lcg = (LCGCtx *)ctx->lcg_ctx;
        if (lcg) {
            printf("    LCG stats: clauses=%lu, analyses=%lu\n",
                   lcg->n_learnt, lcg->n_analyses);
        }
        solver_disable_lcg(ctx);
    }

done:
    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return rr;
}

int main(void) {
    printf("=== LCG Comparison Benchmarks ===\n\n");
    setbuf(stdout, NULL);  /* unbuffered output */
    printf("%-6s %-8s %-6s %-5s %-10s %-12s %-10s\n",
           "N", "Canvas", "Halo", "LCG", "Result", "Conflicts", "Time(ms)");
    printf("--------------------------------------------------------------------\n");

    int configs[][3] = {
        {10, 100, 0},
        {10, 80,  0},
        {10, 70,  0},
        {15, 120, 0},
        {15, 100, 0},
        {20, 150, 0},
        {20, 130, 0},
        {10, 100, 2},
        {10, 80,  2},
        {15, 120, 2},
        {20, 150, 2},
    };
    int nc = sizeof(configs) / sizeof(configs[0]);

    for (int ci = 0; ci < nc; ci++) {
        int n = configs[ci][0], canvas = configs[ci][1], halo = configs[ci][2];

        /* Without LCG */
        RunResult r0 = run_test(n, canvas, halo, 0, 5000);
        const char *s0 = r0.result==0?"FEASIBLE":r0.result==1?"UNSAT":"TIMEOUT";
        printf("%-6d %-8d %-6d %-5s %-10s %-12lu %-10.3f\n",
               n, canvas, halo, "OFF", s0, r0.conflicts, r0.time_ms);

        /* With auto-LCG (activates after 100 restarts if still unsolved) */
        RunResult r1 = run_test(n, canvas, halo, 2, 5000);
        const char *s1 = r1.result==0?"FEASIBLE":r1.result==1?"UNSAT":"TIMEOUT";
        printf("%-6d %-8d %-6d %-5s %-10s %-12lu %-10.3f\n",
               n, canvas, halo, "AUTO", s1, r1.conflicts, r1.time_ms);
        printf("\n");
    }

    return 0;
}
