/* C benchmark: measure scaling of NoOverlap2D solver across problem sizes.
 *
 * Compile: gcc -O2 -I src/c bench_c_scaling.c -L build -ldv_solve -Wl,-rpath,build -o bench_c_scaling
 * Run: ./bench_c_scaling
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_placement.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct {
    int n_rects;
    int canvas;
    int halo;
    int result;
    uint64_t conflicts;
    double time_sec;
} BenchResult;

static BenchResult run_one(int n, int canvas, int halo, int max_restarts) {
    BenchResult br = {n, canvas, halo, -1, 0, 0.0};

    size_t sp_sz = 65536;
    void *sp_buf = calloc(1, sp_sz);
    SolveProblem *sp = solve_problem_init(sp_buf, sp_sz);
    if (!sp) { free(sp_buf); return br; }

    for (int i = 0; i < n; i++) {
        int w = 10 + (i % 8) * 5;
        int h = 8 + (i % 6) * 4;
        int xlim = canvas - w - 2 * halo;
        int ylim = canvas - h - 2 * halo;
        if (xlim < 0) xlim = 0;
        if (ylim < 0) ylim = 0;
        problem_add_var(sp, (uint32_t)i, 32, 0, 0, xlim);
        problem_add_var(sp, (uint32_t)(n + i), 32, 0, 0, ylim);
    }
    uint32_t src[256];
    for (int i = 0; i < 2 * n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2 * n), src);

    size_t ctx_sz = 1 << 22;
    void *ctx_buf = calloc(1, ctx_sz);
    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, ctx_sz);
    SolveCtx *ctx = solver_create(ctx_buf, ctx_sz, ba);
    if (solver_compile(ctx, sp) != 0) {
        br.result = -2;
        goto cleanup;
    }

    RectSpec rects[64];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n + i);
        rects[i].width = 10 + (i % 8) * 5;
        rects[i].height = 8 + (i % 6) * 4;
        rects[i].halo_l = halo;
        rects[i].halo_r = halo;
        rects[i].halo_t = halo;
        rects[i].halo_b = halo;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    SolveOpts sopts = {0};
    sopts.seed = 42;
    sopts.max_conflicts = 200;
    sopts.max_restarts = (uint32_t)max_restarts;
    sopts.max_shave_iters = 0;

    double t0 = now_sec();
    SolveResult sr = solver_solve(ctx, &sopts);
    double t1 = now_sec();

    br.result = sr;
    br.conflicts = ctx->conflict_count;
    br.time_sec = t1 - t0;

cleanup:
    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return br;
}

int main(void) {
    printf("%-6s %-8s %-6s %-10s %-12s %-10s\n",
           "N", "Canvas", "Halo", "Result", "Conflicts", "Time(ms)");
    printf("--------------------------------------------------------------\n");

    /* Scaling with generous canvas */
    int sizes[] = {2, 5, 10, 15, 20, 30, 40, 50, 64};
    for (int si = 0; si < 9; si++) {
        int n = sizes[si];
        if (n > 64) continue;
        int canvas = 50 + n * 20;  /* generous */
        BenchResult r = run_one(n, canvas, 0, 10000);
        const char *status = r.result == 0 ? "FEASIBLE" :
                             r.result == 1 ? "UNSAT" :
                             r.result == 2 ? "TIMEOUT" : "ERROR";
        printf("%-6d %-8d %-6d %-10s %-12lu %-10.3f\n",
               n, canvas, 0, status, r.conflicts, r.time_sec * 1000);
    }

    printf("\n--- With halos ---\n");
    int sizes2[] = {5, 10, 15, 20, 30};
    for (int si = 0; si < 5; si++) {
        int n = sizes2[si];
        int canvas = 50 + n * 25;
        BenchResult r = run_one(n, canvas, 2, 10000);
        const char *status = r.result == 0 ? "FEASIBLE" :
                             r.result == 1 ? "UNSAT" :
                             r.result == 2 ? "TIMEOUT" : "ERROR";
        printf("%-6d %-8d %-6d %-10s %-12lu %-10.3f\n",
               n, canvas, 2, status, r.conflicts, r.time_sec * 1000);
    }

    printf("\n--- Tight canvas (ratio ~2x area) ---\n");
    int sizes3[] = {5, 10, 15, 20};
    for (int si = 0; si < 4; si++) {
        int n = sizes3[si];
        int canvas = 30 + n * 10;
        BenchResult r = run_one(n, canvas, 0, 50000);
        const char *status = r.result == 0 ? "FEASIBLE" :
                             r.result == 1 ? "UNSAT" :
                             r.result == 2 ? "TIMEOUT" : "ERROR";
        printf("%-6d %-8d %-6d %-10s %-12lu %-10.3f\n",
               n, canvas, 0, status, r.conflicts, r.time_sec * 1000);
    }

    return 0;
}
