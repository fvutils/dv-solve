/*
 * The public C API, used through dv_solve.h alone: the header must be
 * self-contained, and its documented flow must work end to end.
 */
#include "dv_solve.h"

#include <stdlib.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

#define CTX_SIZE (1u << 20)

/* Compile p into a fresh context in buf. */
static dvs_ctx_t *compile(dvs_problem_t *p, void *buf, dvs_block_alloc_t *ba) {
    dvs_ctx_t *ctx = dvs_solver_create(buf, CTX_SIZE, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    return ctx;
}

int main(void) {
    void *buf = malloc(CTX_SIZE);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, CTX_SIZE);
    CHECK(buf != NULL && ba != NULL);

    /* 8-bit unsigned x > 10, x != 20; y == x + 1 (signed 16-bit). */
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    CHECK(b != NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 16, 1, -32768, 32767);
    dvs_expr_t x = dvs_builder_expr_var(b, 0);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT, x,
        dvs_builder_expr_const(b, 10, 1)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ, x,
        dvs_builder_expr_const(b, 20, 1)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_var(b, 1),
        dvs_builder_expr_binary(b, DVS_BIN_ADD, x,
                                dvs_builder_expr_const(b, 1, 1))));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    CHECK(p != NULL && sz > 0);

    dvs_ctx_t *ctx = compile(p, buf, ba);
    for (uint64_t seed = 1; seed <= 20; seed++) {
        dvs_solve_opts_t opts = {0};
        opts.seed = seed;
        opts.fair_pick = 1;
        dvs_solver_reset(ctx);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
        int64_t xv = dvs_solver_get_value(ctx, 0);
        int64_t yv = dvs_solver_get_value(ctx, 1);
        CHECK(xv > 10 && xv <= 255 && xv != 20);
        CHECK(yv == xv + 1);
        CHECK(dvs_solver_validate_model(ctx, p, stderr) == 0);
        uint32_t ids[2] = {0, 1};
        int64_t vals[2];
        dvs_solver_get_values(ctx, 2, ids, vals);
        CHECK(vals[0] == xv && vals[1] == yv);
    }

    /* Pin, then exclude values until none remain in a small range. */
    dvs_solver_reset(ctx);
    CHECK(dvs_solver_pin_var(ctx, 0, 42) == 0);
    CHECK(dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK);
    CHECK(dvs_solver_get_value(ctx, 1) == 43);
    dvs_solver_reset(ctx);
    CHECK(dvs_solver_pin_var(ctx, 0, 20) == -1);   /* contradicts x != 20 */
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);

    /* Unsatisfiable at compile time. */
    dvs_builder_reset(b);
    dvs_builder_add_var(b, 0, 4, 0, 0, 15);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_const(b, 20, 1)));
    p = dvs_builder_finalize(b, &sz);
    ctx = dvs_solver_create(buf, CTX_SIZE, ba);
    int rc = dvs_solver_compile(ctx, p);
    CHECK(rc == DVS_COMPILE_UNSAT ||
          (rc == DVS_COMPILE_OK && dvs_solver_solve(ctx, NULL) == DVS_SOLVE_UNSAT));
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);

    /* randc: exclude every solution of a 2-bit variable in turn. */
    dvs_builder_reset(b);
    dvs_builder_add_var(b, 0, 2, 0, 0, 3);
    p = dvs_builder_finalize(b, &sz);
    ctx = compile(p, buf, ba);
    int seen = 0, n = 0;
    for (;;) {
        dvs_solver_reset(ctx);
        if (dvs_solver_solve(ctx, NULL) != DVS_SOLVE_OK) break;
        int64_t v = dvs_solver_get_value(ctx, 0);
        CHECK(v >= 0 && v <= 3 && !(seen & (1 << v)));
        seen |= 1 << v;
        n++;
        if (dvs_solver_exclude_value(ctx, 0, v) != 0) break;
    }
    CHECK(seen == 0xF && n == 4);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);

    /* Soft constraints: x == 1 (priority 0) beats x == 2 (priority 1). */
    dvs_builder_reset(b);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    x = dvs_builder_expr_var(b, 0);
    dvs_builder_add_soft_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        x, dvs_builder_expr_const(b, 1, 1)), 0);
    dvs_builder_add_soft_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        x, dvs_builder_expr_const(b, 2, 1)), 1);
    p = dvs_builder_finalize(b, &sz);
    ctx = compile(p, buf, ba);
    CHECK(dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK);
    CHECK(dvs_solver_get_value(ctx, 0) == 1);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);

    /* An expression naming an undeclared variable is refused. */
    dvs_builder_reset(b);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 3)));
    p = dvs_builder_finalize(b, &sz);
    ctx = dvs_solver_create(buf, CTX_SIZE, ba);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_BAD_VAR);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);

    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("test_public_api: OK\n");
    return failures != 0;
}
