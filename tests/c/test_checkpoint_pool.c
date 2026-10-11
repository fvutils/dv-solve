/*
 * Push/pop sessions hold the context pool flat: dvs_solver_restore frees
 * what the scope allocated (its propagators, its variables' wide bounds, a
 * regrown reset snapshot), and an exclusion made in a scope survives the
 * restore. See docs/dynamic_pool_plan.md, P0.3.
 */
#include <stdio.h>
#include <stdlib.h>

#include "dv_solve.h"
#include "dvs_ctx.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

#define CTX_SIZE (1u << 20)
#define N_CYCLES 10000

static dvs_expr_t V(dvs_builder_t *b, uint32_t id) { return dvs_builder_expr_var(b, id); }
static dvs_expr_t K(dvs_builder_t *b, int64_t v) { return dvs_builder_expr_const(b, v, 0); }
static dvs_expr_t BIN(dvs_builder_t *b, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}

int main(void) {
    void *buf = malloc(CTX_SIZE);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);

    /* x0..x3 8-bit, x4 40-bit (tier-1: its bounds live in the pool). */
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    for (uint32_t i = 0; i < 4; i++) dvs_builder_add_var(b, i, 8, 0, 0, 255);
    dvs_builder_add_var(b, 4, 40, 0, 0, ((int64_t)1 << 40) - 1);
    dvs_builder_add_constraint(b, BIN(b, DVS_BIN_LT, V(b, 0), V(b, 1)));
    dvs_builder_add_constraint(b, BIN(b, DVS_BIN_GT, V(b, 4), K(b, 1000)));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);

    /* The scope's additions: a new 40-bit variable x5 (wide bounds, reset
     * snapshot growth), x5 == x4 + 3, x0 + x1 < 120, x2 != x3, and an
     * all-different. Built once: an added problem must outlive the
     * context. */
    dvs_builder_t *ab = dvs_builder_create(0, NULL);
    dvs_builder_add_var(ab, 5, 40, 0, 0, ((int64_t)1 << 40) - 1);
    dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_EQ, V(ab, 5),
                                       BIN(ab, DVS_BIN_ADD, V(ab, 4), K(ab, 3))));
    dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_LT,
                                       BIN(ab, DVS_BIN_ADD, V(ab, 0), V(ab, 1)), K(ab, 120)));
    dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_NEQ, V(ab, 2), V(ab, 3)));
    uint32_t ad[3] = {0, 1, 2};
    dvs_builder_add_all_different(ab, 3, ad);
    size_t asz = 0;
    dvs_problem_t *ap = dvs_builder_finalize(ab, &asz);
    CHECK(p != NULL && ap != NULL);

    dvs_ctx_t *ctx = dvs_solver_create(buf, CTX_SIZE, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);

    uint32_t used0 = 0, used20 = 0;
    for (uint32_t i = 0; i < N_CYCLES; i++) {
        int cp = dvs_solver_checkpoint(ctx);
        CHECK(cp == 0);
        CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_OK);
        /* The first 20 scopes each exclude a value of x2; it must hold in
         * every later scope. */
        if (i < 20) CHECK(dvs_solver_exclude_value(ctx, 2, 100 + i) == 0);
        dvs_solve_opts_t opts = {0};
        opts.seed = i + 1;
        opts.fair_pick = 1;
        opts.use_lcg = (uint8_t)(i & 1);
        dvs_solver_reset(ctx);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
        int64_t x0 = dvs_solver_get_value(ctx, 0), x1 = dvs_solver_get_value(ctx, 1);
        int64_t x2 = dvs_solver_get_value(ctx, 2), x3 = dvs_solver_get_value(ctx, 3);
        int64_t x4 = dvs_solver_get_value(ctx, 4), x5 = dvs_solver_get_value(ctx, 5);
        CHECK(x0 < x1 && x0 + x1 < 120 && x2 != x3 && x2 != x0 && x2 != x1);
        CHECK(x4 > 1000 && x5 == x4 + 3);
        CHECK(!(x2 >= 100 && x2 < 100 + (int64_t)(i < 20 ? i + 1 : 20)));
        dvs_solver_restore(ctx, (uint32_t)cp);
        if (i == 0)  used0  = dvs_ctx_pool_used(ctx);
        if (i == 19) used20 = dvs_ctx_pool_used(ctx);
        if (i > 19 && dvs_ctx_pool_used(ctx) != used20) {
            CHECK(dvs_ctx_pool_used(ctx) == used20);
            break;
        }
    }
    printf("pool after first pop %u, after 20 exclusions %u, after %u pops %u\n",
           used0, used20, N_CYCLES, dvs_ctx_pool_used(ctx));
    /* The 20 exclusions are all the scopes kept. */
    CHECK(used20 - used0 <= 20u * 32u);

    /* Nested scopes, restored from the outer one. */
    uint32_t before = dvs_ctx_pool_used(ctx);
    for (uint32_t i = 0; i < 100; i++) {
        int c0 = dvs_solver_checkpoint(ctx);
        CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_OK);
        int c1 = dvs_solver_checkpoint(ctx);
        CHECK(c1 == c0 + 1);
        CHECK(dvs_solver_pin_var(ctx, 0, 3) == 0);
        dvs_solver_reset(ctx);
        CHECK(dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK);
        CHECK(dvs_solver_get_value(ctx, 0) == 3);
        dvs_solver_restore(ctx, (uint32_t)c0);
    }
    CHECK(dvs_ctx_pool_used(ctx) == before);

    /* After every scope is gone the base problem solves as compiled. */
    dvs_solver_reset(ctx);
    CHECK(dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK);

    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(ab, ap, asz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(ab);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
