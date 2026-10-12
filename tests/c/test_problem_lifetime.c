/*
 * A context reads a problem only during dvs_solver_compile and
 * dvs_solver_add_constraint: the caller may free it right after. Every
 * problem here is freed (and its memory reused) before the context solves,
 * checkpoints, restores and resets; built with ASan any later read is a
 * use-after-free. See docs/dynamic_pool_plan.md (B117).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_ctx.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static dvs_expr_t V(dvs_builder_t *b, uint32_t id) { return dvs_builder_expr_var(b, id); }
static dvs_expr_t K(dvs_builder_t *b, int64_t v) { return dvs_builder_expr_const(b, v, 0); }
static dvs_expr_t BIN(dvs_builder_t *b, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}

/* Free a finalized problem after scribbling over it, so a stale read sees
 * garbage even without ASan. */
static void _drop(dvs_builder_t *b, dvs_problem_t *p, size_t sz) {
    memset(p, 0xA5, sz);
    dvs_builder_free_problem(b, p, sz);
}

int main(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    for (uint32_t i = 0; i < 4; i++) dvs_builder_add_var(b, i, 8, 0, 0, 200);
    dvs_builder_add_var(b, 4, 40, 0, 0, ((int64_t)1 << 40) - 1);
    dvs_builder_add_constraint(b, BIN(b, DVS_BIN_LT, V(b, 0), V(b, 1)));
    dvs_builder_add_constraint(b, BIN(b, DVS_BIN_EQ, V(b, 4),
                                      BIN(b, DVS_BIN_ADD, V(b, 2), K(b, 1000))));
    uint32_t ad[3] = {0, 1, 3};
    dvs_builder_add_all_different(b, 3, ad);
    dvs_builder_add_soft_constraint(b, BIN(b, DVS_BIN_EQ, V(b, 3), K(b, 7)), 1);
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    CHECK(p != NULL);

    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    _drop(b, p, sz);

    dvs_solve_opts_t opts = {0};
    opts.seed = 3;
    for (int round = 0; round < 50; round++) {
        int cp = dvs_solver_checkpoint(ctx);
        CHECK(cp == 0);
        /* A scope adding x0 + x1 < 100 + round and a new variable xn (the
         * compile made variables of its own: number it after them). */
        uint32_t xn = dvs_solver_n_vars(ctx);
        dvs_builder_t *ab = dvs_builder_create(0, NULL);
        dvs_builder_add_var(ab, xn, 16, 0, 0, 1000);
        dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_LT,
            BIN(ab, DVS_BIN_ADD, V(ab, 0), V(ab, 1)), K(ab, 100 + round)));
        dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_EQ, V(ab, xn),
            BIN(ab, DVS_BIN_ADD, V(ab, 0), K(ab, 3))));
        size_t asz = 0;
        dvs_problem_t *ap = dvs_builder_finalize(ab, &asz);
        CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_OK);
        _drop(ab, ap, asz);
        dvs_builder_destroy(ab);

        opts.seed = (uint64_t)round + 1;
        opts.use_lcg = (uint8_t)(round & 1);
        dvs_solver_reset(ctx);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
        int64_t x0 = dvs_solver_get_value(ctx, 0), x1 = dvs_solver_get_value(ctx, 1);
        int64_t x2 = dvs_solver_get_value(ctx, 2), x3 = dvs_solver_get_value(ctx, 3);
        CHECK(x0 < x1 && x0 + x1 < 100 + round);
        CHECK(x0 != x3 && x1 != x3);
        CHECK(dvs_solver_get_value(ctx, 4) == x2 + 1000);
        CHECK(dvs_solver_get_value(ctx, xn) == x0 + 3);
        dvs_solver_restore(ctx, (uint32_t)cp);
    }
    dvs_solver_reset(ctx);
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
    dvs_solver_free(ctx);
    dvs_builder_destroy(b);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("problem lifetime: ok\n");
    return failures ? 1 : 0;
}
