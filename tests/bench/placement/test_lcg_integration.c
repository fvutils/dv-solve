/* Test LCG integration: compare OFF vs PRE-ENABLED on tight problems.
 *
 * Build: gcc -O2 -Isrc/c tests/bench/placement/test_lcg_integration.c \
 *        -Lbuild -ldv_solve -Wl,-rpath,build -o build/test_lcg_integration
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
    uint64_t clauses_learnt;
    uint64_t analyses;
    uint64_t db_props;
} RunResult;

static RunResult run_test(int n, int canvas, int halo,
                           int pre_lcg, uint32_t max_restarts) {
    RunResult rr = {-1, 0, 0.0, 0, 0, 0};

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

    size_t ctx_sz = 1 << 23;  /* 8 MiB */
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

    if (pre_lcg)
        solver_enable_lcg(ctx);

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
        rr.clauses_learnt = lcg->n_learnt;
        rr.analyses = lcg->n_analyses;
        rr.db_props = lcg->clause_db.n_propagations;
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
    printf("=== LCG Integration Test: OFF vs PRE-ENABLED ===\n\n");
    printf("%-6s %-8s %-6s %-5s %-10s %-12s %-10s %-8s %-8s %-8s\n",
           "N", "Canvas", "Halo", "LCG", "Result", "Conflicts", "Time(ms)",
           "Clauses", "Analyses", "DBProps");
    printf("--------------------------------------------------------------------------------------------\n");

    struct { int n, canvas, halo; uint32_t restarts; } configs[] = {
        /* Tight problems where LCG should help */
        {10, 70, 0, 10000},
        {10, 80, 2, 10000},
        {15, 100, 0, 10000},
        {15, 120, 2, 10000},
        {20, 130, 0, 10000},
        /* Medium problems */
        {10, 100, 0, 5000},
        {15, 120, 0, 5000},
        {20, 150, 0, 5000},
    };
    int nc = sizeof(configs) / sizeof(configs[0]);

    for (int ci = 0; ci < nc; ci++) {
        int n = configs[ci].n, canvas = configs[ci].canvas, halo = configs[ci].halo;
        uint32_t mr = configs[ci].restarts;

        /* Without LCG */
        RunResult r0 = run_test(n, canvas, halo, 0, mr);
        const char *s0 = r0.result==0?"FEASIBLE":r0.result==1?"UNSAT":"TIMEOUT";
        printf("%-6d %-8d %-6d %-5s %-10s %-12lu %-10.1f %-8s %-8s %-8s\n",
               n, canvas, halo, "OFF", s0, r0.conflicts, r0.time_ms, "-", "-", "-");

        /* With LCG pre-enabled */
        RunResult r1 = run_test(n, canvas, halo, 1, mr);
        const char *s1 = r1.result==0?"FEASIBLE":r1.result==1?"UNSAT":"TIMEOUT";
        printf("%-6d %-8d %-6d %-5s %-10s %-12lu %-10.1f %-8lu %-8lu %-8lu\n",
               n, canvas, halo, "ON", s1, r1.conflicts, r1.time_ms,
               r1.clauses_learnt, r1.analyses, r1.db_props);

        /* Compute speedup */
        if (r0.conflicts > 0 && r1.conflicts > 0) {
            double ratio = (double)r0.conflicts / (double)r1.conflicts;
            printf("  -> conflict ratio: %.2fx (%s)\n",
                   ratio, ratio > 1.1 ? "LCG BETTER" :
                          ratio < 0.9 ? "LCG WORSE" : "SIMILAR");
        }
        printf("\n");
    }

    return 0;
}
