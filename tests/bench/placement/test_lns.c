/* Test LNS optimizer on placement benchmarks.
 *
 * Build: gcc -O2 -Isrc/c tests/bench/placement/test_lns.c \
 *        -Lbuild -ldv_solve -Wl,-rpath,build -o build/test_lns
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

static void test_lns(int n, int canvas, int n_nets, double time_lim) {
    size_t sp_sz = 65536;
    void *sp_buf = calloc(1, sp_sz);
    SolveProblem *sp = solve_problem_init(sp_buf, sp_sz);

    int widths[256], heights[256];
    srand(42);
    for (int i = 0; i < n; i++) {
        widths[i]  = 10 + (rand() % 40);
        heights[i] = 8  + (rand() % 30);
        problem_add_var(sp, (uint32_t)i,     32, 0, 0, canvas - widths[i]);
        problem_add_var(sp, (uint32_t)(n+i), 32, 0, 0, canvas - heights[i]);
    }
    uint32_t src[512];
    for (int i = 0; i < 2*n; i++) src[i] = (uint32_t)i;
    problem_add_source(sp, (uint32_t)(2*n), src);

    size_t ctx_sz = 1 << 24;
    void *ctx_buf = calloc(1, ctx_sz);
    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, ctx_sz);
    SolveCtx *ctx = solver_create(ctx_buf, ctx_sz, ba);
    solver_compile(ctx, sp);

    /* Add NoOverlap2D */
    RectSpec rects[256];
    for (int i = 0; i < n; i++) {
        rects[i].x_id = (uint32_t)i;
        rects[i].y_id = (uint32_t)(n+i);
        rects[i].width = widths[i];
        rects[i].height = heights[i];
        rects[i].halo_l = 0; rects[i].halo_r = 0;
        rects[i].halo_t = 0; rects[i].halo_b = 0;
    }
    if (n <= 64) {
        prop_add_no_overlap_2d(ctx, (uint32_t)n, rects, 2);
    } else {
        /* Chunk into blocks of 32 for N>64 */
        for (int bi = 0; bi < n; bi += 32) {
            for (int bj = bi; bj < n; bj += 32) {
                int ni_end = bi + 32 < n ? bi + 32 : n;
                int nj_end = bj + 32 < n ? bj + 32 : n;
                int cnt = 0;
                RectSpec chunk[64];
                for (int i = bi; i < ni_end; i++) chunk[cnt++] = rects[i];
                if (bj != bi)
                    for (int j = bj; j < nj_end; j++) chunk[cnt++] = rects[j];
                if (cnt >= 2)
                    prop_add_no_overlap_2d(ctx, (uint32_t)cnt, chunk, 2);
            }
        }
    }

    /* Build nets */
    CostGuidedPin *all_pins = calloc((size_t)n_nets * 4, sizeof(CostGuidedPin));
    CostGuidedNet *nets = calloc((size_t)n_nets, sizeof(CostGuidedNet));
    CostGuidedMacro *macros = calloc((size_t)n, sizeof(CostGuidedMacro));

    for (int i = 0; i < n; i++) {
        macros[i].x_var_id = (uint32_t)i;
        macros[i].y_var_id = (uint32_t)(n+i);
    }

    uint32_t po = 0;
    for (int ni = 0; ni < n_nets; ni++) {
        int deg = 2 + (rand() % 3);
        if (deg > n) deg = n;
        nets[ni].n_pins = (uint32_t)deg;
        nets[ni].pins = &all_pins[po];
        for (int pi = 0; pi < deg; pi++) {
            uint32_t mid;
            int unique;
            do { mid = (uint32_t)(rand() % n); unique = 1;
                for (int k=0;k<pi;k++) if (nets[ni].pins[k].macro_id==mid){unique=0;break;}
            } while(!unique);
            nets[ni].pins[pi].macro_id = mid;
            nets[ni].pins[pi].offset_x = (int32_t)(rand() % widths[mid]);
            nets[ni].pins[pi].offset_y = (int32_t)(rand() % heights[mid]);
        }
        po += (uint32_t)deg;
    }

    int32_t *macro_ws = calloc((size_t)n, sizeof(int32_t));
    int32_t *macro_hs = calloc((size_t)n, sizeof(int32_t));
    for (int i = 0; i < n; i++) {
        macro_ws[i] = widths[i]; macro_hs[i] = heights[i];
    }

    HPWLCostCtx hctx;
    memset(&hctx, 0, sizeof(hctx));
    hctx.n_macros = (uint32_t)n;
    hctx.n_nets = (uint32_t)n_nets;
    hctx.macros = macros;
    hctx.nets = nets;
    hctx.macro_widths = macro_ws;
    hctx.macro_heights = macro_hs;

    int32_t *positions = calloc((size_t)n * 2, sizeof(int32_t));
    LNSResult lr;

    LNSOpts lopts;
    memset(&lopts, 0, sizeof(lopts));
    lopts.max_iterations = 200;
    lopts.time_limit_sec = time_lim;
    lopts.neighborhood_size = 2;
    lopts.subproblem_conflicts = 500;
    lopts.seed = 42;

    int rc = solver_lns_optimize(ctx, &hctx, &lopts, positions, &lr);

    if (rc == 0) {
        printf("N=%-3d canvas=%-4d nets=%-4d | greedy=%ld  best=%ld  impr=%.1f%%"
               "  iters=%u  accepted=%u  time=%.2fs\n",
               n, canvas, n_nets,
               lr.initial_hpwl, lr.best_hpwl,
               lr.initial_hpwl > 0 ?
                   (1.0 - (double)lr.best_hpwl / lr.initial_hpwl) * 100.0 : 0.0,
               lr.iterations, lr.improvements, lr.elapsed_sec);
    } else {
        printf("N=%-3d canvas=%-4d nets=%-4d | FAILED\n", n, canvas, n_nets);
    }

    free(positions); free(macro_ws); free(macro_hs);
    hpwl_cost_ctx_destroy(&hctx);
    free(all_pins); free(nets); free(macros);
    zsp_block_alloc_destroy(ba);
    free(ctx_buf); free(sp_buf);
}

int main(void) {
    setbuf(stdout, NULL);
    printf("=== LNS Optimizer Test ===\n\n");
    test_lns(20,  400, 40,  2.0);
    test_lns(20,  400, 100, 2.0);
    test_lns(50,  600, 100, 5.0);
    test_lns(50,  600, 200, 5.0);
    test_lns(100, 800, 200, 10.0);
    return 0;
}
