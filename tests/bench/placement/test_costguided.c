/* Test CostGuided HPWL value selector on placement problems.
 *
 * Compares random vs cost-guided value selection on problems
 * with nets, measuring both solve time and HPWL quality.
 *
 * Build: gcc -O2 -Isrc/c tests/bench/placement/test_costguided.c \
 *        -Lbuild -lzsp_solver -Wl,-rpath,build -o build/test_costguided
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_placement.h"
#include "zsp_costguided.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Compute HPWL from placed positions */
static int64_t compute_hpwl(const SolveCtx *ctx, const HPWLCostCtx *hctx) {
    int64_t total = 0;
    for (uint32_t ni = 0; ni < hctx->n_nets; ni++) {
        const CostGuidedNet *net = &hctx->nets[ni];
        int32_t xmin = INT32_MAX, xmax = INT32_MIN;
        int32_t ymin = INT32_MAX, ymax = INT32_MIN;
        for (uint32_t pi = 0; pi < net->n_pins; pi++) {
            uint32_t mid = net->pins[pi].macro_id;
            int32_t x = (int32_t)solver_get_value(ctx, hctx->macros[mid].x_var_id)
                        + net->pins[pi].offset_x;
            int32_t y = (int32_t)solver_get_value(ctx, hctx->macros[mid].y_var_id)
                        + net->pins[pi].offset_y;
            if (x < xmin) xmin = x;
            if (x > xmax) xmax = x;
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
        total += (int64_t)(xmax - xmin) + (int64_t)(ymax - ymin);
    }
    return total;
}

typedef struct {
    int       result;
    uint64_t  conflicts;
    double    time_ms;
    int64_t   hpwl;
} RunResult;

static RunResult run_placement(int n_macros, int canvas, int n_nets,
                                int use_costguided, uint64_t seed) {
    RunResult rr = {-1, 0, 0.0, 0};
    int n = n_macros;

    /* Build problem */
    size_t sp_sz = 65536;
    void *sp_buf = calloc(1, sp_sz);
    SolveProblem *sp = solve_problem_init(sp_buf, sp_sz);
    if (!sp) { free(sp_buf); return rr; }

    int widths[64], heights[64];
    for (int i = 0; i < n; i++) {
        widths[i]  = 10 + (i % 8) * 5;
        heights[i] = 8  + (i % 6) * 4;
        problem_add_var(sp, (uint32_t)i,     32, 0, 0, canvas - widths[i]);
        problem_add_var(sp, (uint32_t)(n+i), 32, 0, 0, canvas - heights[i]);
    }
    uint32_t src[128];
    for (int i = 0; i < 2*n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2*n), src);

    /* Create context */
    size_t ctx_sz = 1 << 22;
    void *ctx_buf = calloc(1, ctx_sz);
    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, ctx_sz);
    SolveCtx *ctx = solver_create(ctx_buf, ctx_sz, ba);
    if (solver_compile(ctx, sp) != 0) { rr.result = -2; goto done; }

    /* Add NoOverlap2D */
    RectSpec rects[64];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n+i);
        rects[i].width = widths[i];
        rects[i].height = heights[i];
        rects[i].halo_l = 0; rects[i].halo_r = 0;
        rects[i].halo_t = 0; rects[i].halo_b = 0;
    }
    prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);

    /* Build nets: random connectivity */
    srand((unsigned)seed);
    CostGuidedPin *all_pins = (CostGuidedPin *)calloc(
        (size_t)n_nets * 4, sizeof(CostGuidedPin));
    CostGuidedNet *nets = (CostGuidedNet *)calloc(
        (size_t)n_nets, sizeof(CostGuidedNet));
    CostGuidedMacro *macros = (CostGuidedMacro *)calloc(
        (size_t)n, sizeof(CostGuidedMacro));

    for (int i = 0; i < n; i++) {
        macros[i].x_var_id = (uint32_t)i;
        macros[i].y_var_id = (uint32_t)(n+i);
    }

    uint32_t pin_offset = 0;
    for (int ni = 0; ni < n_nets; ni++) {
        int degree = 2 + (rand() % 3);  /* 2-4 pins per net */
        if (degree > n) degree = n;
        nets[ni].n_pins = (uint32_t)degree;
        nets[ni].pins = &all_pins[pin_offset];
        /* Pick random distinct macros */
        for (int pi = 0; pi < degree; pi++) {
            uint32_t mid;
            int unique;
            do {
                mid = (uint32_t)(rand() % n);
                unique = 1;
                for (int k = 0; k < pi; k++)
                    if (nets[ni].pins[k].macro_id == mid) { unique = 0; break; }
            } while (!unique);
            nets[ni].pins[pi].macro_id = mid;
            nets[ni].pins[pi].offset_x = (int32_t)(rand() % widths[mid]);
            nets[ni].pins[pi].offset_y = (int32_t)(rand() % heights[mid]);
        }
        pin_offset += (uint32_t)degree;
    }

    /* Set up HPWL cost context */
    /* Build dimension arrays for overlap penalty */
    int32_t *macro_ws = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    int32_t *macro_hs = (int32_t *)calloc((size_t)n, sizeof(int32_t));
    for (int i = 0; i < n; i++) {
        macro_ws[i] = widths[i];
        macro_hs[i] = heights[i];
    }

    HPWLCostCtx hctx;
    memset(&hctx, 0, sizeof(hctx));
    hctx.n_macros = (uint32_t)n;
    hctx.n_nets = (uint32_t)n_nets;
    hctx.macros = macros;
    hctx.nets = nets;
    hctx.macro_widths = macro_ws;
    hctx.macro_heights = macro_hs;

    if (use_costguided) {
        solver_set_cost_guided_hpwl(ctx, &hctx, 500);
    } else {
        /* Build the index anyway so we can compute HPWL after solve */
        hpwl_cost_ctx_build_index(&hctx);
    }

    /* Solve */
    SolveOpts sopts = {0};
    sopts.seed = seed;
    sopts.max_conflicts = 200;
    sopts.max_restarts = 5000;
    sopts.max_shave_iters = 0;

    double t0 = now_sec();
    rr.result = solver_solve(ctx, &sopts);
    rr.time_ms = (now_sec() - t0) * 1000.0;
    rr.conflicts = ctx->conflict_count;

    if (rr.result == 0) {
        rr.hpwl = compute_hpwl(ctx, &hctx);
    }

    hpwl_cost_ctx_destroy(&hctx);
    free(macro_ws);
    free(macro_hs);
    free(all_pins);
    free(nets);
    free(macros);

done:
    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return rr;
}

int main(void) {
    setbuf(stdout, NULL);

    printf("=== CostGuided HPWL Benchmark ===\n\n");
    printf("%-4s %-6s %-5s %-5s %-8s %-10s %-8s %-10s\n",
           "N", "Canvas", "Nets", "CG", "Result", "Conflicts", "HPWL", "Time(ms)");
    printf("--------------------------------------------------------------\n");

    int configs[][3] = {
        /* n_macros, canvas, n_nets */
        {10, 200, 20},
        {10, 200, 50},
        {20, 400, 40},
        {20, 400, 100},
        {30, 600, 60},
        {30, 600, 150},
    };
    int nc = sizeof(configs) / sizeof(configs[0]);

    for (int ci = 0; ci < nc; ci++) {
        int n = configs[ci][0], canvas = configs[ci][1], nets = configs[ci][2];

        /* Run 3 seeds and average */
        int64_t hpwl_sum[2] = {0, 0};
        double  time_sum[2] = {0, 0};
        uint64_t conf_sum[2] = {0, 0};
        int      ok_count[2] = {0, 0};

        for (int s = 0; s < 3; s++) {
            uint64_t seed = 42 + (uint64_t)s * 1000003;
            for (int cg = 0; cg < 2; cg++) {
                RunResult r = run_placement(n, canvas, nets, cg, seed);
                if (r.result == 0) {
                    hpwl_sum[cg] += r.hpwl;
                    ok_count[cg]++;
                }
                time_sum[cg] += r.time_ms;
                conf_sum[cg] += r.conflicts;
            }
        }

        for (int cg = 0; cg < 2; cg++) {
            const char *label = cg ? "ON" : "OFF";
            if (ok_count[cg] > 0) {
                printf("%-4d %-6d %-5d %-5s %-8s %-10lu %-8ld %-10.1f\n",
                       n, canvas, nets, label, "OK",
                       conf_sum[cg] / 3, hpwl_sum[cg] / ok_count[cg],
                       time_sum[cg] / 3.0);
            } else {
                printf("%-4d %-6d %-5d %-5s %-8s %-10lu %-8s %-10.1f\n",
                       n, canvas, nets, label, "FAIL",
                       conf_sum[cg] / 3, "-", time_sum[cg] / 3.0);
            }
        }
        printf("\n");
    }

    return 0;
}
