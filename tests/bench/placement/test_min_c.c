/* Minimal C test for MinOfN propagator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvs_problem.h"
#include "dvs_ctx.h"
#include "dvs_search.h"
#include "dvs_placement.h"

int main(void) {
    /* Build problem: 4 variables */
    size_t sp_sz = 65536;
    void *sp_buf = calloc(1, sp_sz);
    dvs_problem_t *sp = solve_problem_init(sp_buf, sp_sz);
    if (!sp) { fprintf(stderr, "sp init fail\n"); return 1; }

    problem_add_var(sp, 0, 32, 0, 0, 10);  /* r */
    problem_add_var(sp, 1, 32, 0, 2, 8);   /* a */
    problem_add_var(sp, 2, 32, 0, 4, 10);  /* b */
    problem_add_var(sp, 3, 32, 0, 1, 5);   /* c */

    uint32_t src_ids[] = {0, 1, 2, 3};
    problem_add_source(sp, 4, src_ids);

    /* Create solver context */
    size_t ctx_sz = 1 << 20;
    void *ctx_buf = calloc(1, ctx_sz);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, ctx_sz);
    dvs_ctx_t *ctx = dvs_solver_create(ctx_buf, ctx_sz, ba);
    if (!ctx) { fprintf(stderr, "ctx create fail\n"); return 1; }

    int rc = dvs_solver_compile(ctx, sp);
    printf("compile rc=%d, n_vars=%u, n_props=%u, n_prop_refs_cap=%u\n",
           rc, ctx->n_vars, ctx->n_props, ctx->n_prop_refs_capacity);

    if (rc != 0) { fprintf(stderr, "compile fail %d\n", rc); return 1; }

    /* Add MinOfN: r = min(a, b, c) */
    uint32_t ops[] = {1, 2, 3};
    printf("Adding MinOfN...\n");
    uint32_t ref = prop_add_min_of_n_32(ctx, 0, 3, ops, 1);
    printf("MinOfN ref=%u\n", ref);

    if (ref == 0xFFFFFFFF) {
        fprintf(stderr, "MinOfN alloc fail\n");
        return 1;
    }

    /* Solve */
    dvs_solve_opts_t sopts = {0};
    sopts.seed = 42;
    sopts.max_conflicts = 100;
    sopts.max_restarts = 1000;

    printf("Solving...\n");
    dvs_result_t sr = dvs_solver_solve(ctx, &sopts);
    printf("solve result=%d\n", sr);

    if (sr == DVS_SOLVE_OK) {
        int64_t r = dvs_solver_get_value(ctx, 0);
        int64_t a = dvs_solver_get_value(ctx, 1);
        int64_t b = dvs_solver_get_value(ctx, 2);
        int64_t c = dvs_solver_get_value(ctx, 3);
        printf("r=%ld, a=%ld, b=%ld, c=%ld\n", r, a, b, c);
        printf("min(a,b,c)=%ld\n",
               a < b ? (a < c ? a : c) : (b < c ? b : c));
    }

    dvs_block_alloc_destroy(ba);
    free(ctx_buf);
    free(sp_buf);
    return 0;
}
