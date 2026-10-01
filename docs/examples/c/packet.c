/* Randomize a bus packet with the dv-solve C API: the same constraints as
 * the SystemVerilog examples. */
#include <stdio.h>
#include <stdlib.h>
#include "dv_solve.h"

enum { ADDR, LEN, KIND };       /* variable ids: 0 to n-1 */

int main(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, ADDR, 32, 0, 0, 0xFFFFFFFF);
    dvs_builder_add_var(b, LEN,  8,  0, 0, 0xFF);
    dvs_builder_add_var(b, KIND, 4,  0, 0, 0xF);

    dvs_expr_t addr = dvs_builder_expr_var(b, ADDR);
    dvs_expr_t len  = dvs_builder_expr_var(b, LEN);
    dvs_expr_t kind = dvs_builder_expr_var(b, KIND);
#define K(v)        dvs_builder_expr_const(b, (v), 0)
#define BIN(op, l, r) dvs_builder_expr_binary(b, DVS_BIN_##op, (l), (r))

    /* addr[1:0] == 0 */
    dvs_builder_add_constraint(b, BIN(EQ, BIN(BAND, addr, K(3)), K(0)));
    /* len inside {[1:16]} */
    dvs_builder_add_constraint(b, dvs_builder_expr_in_range(b, len, K(1), K(16)));
    /* addr < 'h1000; addr + len <= 'h1000 */
    dvs_builder_add_constraint(b, BIN(LT, addr, K(0x1000)));
    dvs_builder_add_constraint(b, BIN(LTE, BIN(ADD, addr, len), K(0x1000)));
    /* kind != 0; (kind == 7) -> (len > 8) */
    dvs_builder_add_constraint(b, BIN(NEQ, kind, K(0)));
    dvs_builder_add_constraint(b, BIN(OR, BIN(NEQ, kind, K(7)), BIN(GT, len, K(8))));

    size_t size;
    dvs_problem_t *problem = dvs_builder_finalize(b, &size);
    if (!problem) return 1;

    /* The context lives in a buffer you supply, plus a block allocator. */
    size_t ctx_size = 1 << 20;
    void *ctx_buf = malloc(ctx_size);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1 << 20);
    dvs_ctx_t *ctx = dvs_solver_create(ctx_buf, ctx_size, ba);

    int rc = dvs_solver_compile(ctx, problem);
    if (rc != DVS_COMPILE_OK) {
        fprintf(stderr, "compile failed: %d\n", rc);
        return 1;
    }

    dvs_solve_opts_t opts = {0};
    opts.fair_pick = 1;                     /* spread solutions evenly */
    for (uint64_t seed = 1; seed <= 5; seed++) {
        opts.seed = seed;
        dvs_solver_reset(ctx);              /* clear the previous solution */
        if (dvs_solver_solve(ctx, &opts) != DVS_SOLVE_OK) {
            fprintf(stderr, "no solution\n");
            return 1;
        }
        printf("addr=%08llx len=%lld kind=%lld\n",
               (long long)dvs_solver_get_value(ctx, ADDR),
               (long long)dvs_solver_get_value(ctx, LEN),
               (long long)dvs_solver_get_value(ctx, KIND));
    }

    dvs_solver_destroy(ctx);
    dvs_block_alloc_destroy(ba);
    free(ctx_buf);
    dvs_builder_free_problem(b, problem, size);
    dvs_builder_destroy(b);
    return 0;
}
