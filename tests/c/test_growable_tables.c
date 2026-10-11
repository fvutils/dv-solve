/*
 * The per-variable and per-propagator tables grow on demand
 * (docs/dynamic_pool_plan.md P2): no slack guess, no 8192-variable
 * incremental cap. A fixed-mode context still fails cleanly with
 * DVS_COMPILE_NOMEM, and more propagators than a 16-bit id can name is
 * NOMEM, not ids that wrap.
 */
#include <stdio.h>
#include <stdlib.h>

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

/* x[i] in [0,255], x[i] + x[i+1] < 300 + i%7: compile lifts each sum
 * into an aux variable, so the context ends up with about twice the
 * declared variables and grows its tables to hold them. */
static dvs_problem_t *sums(dvs_builder_t *b, uint32_t n, size_t *sz) {
    for (uint32_t i = 0; i < n; i++) dvs_builder_add_var(b, i, 16, 0, 0, 255);
    for (uint32_t i = 0; i + 1 < n; i++)
        dvs_builder_add_constraint(b, BIN(b, DVS_BIN_LT,
            BIN(b, DVS_BIN_ADD, V(b, i), V(b, i + 1)), K(b, 300 + (int64_t)(i % 7))));
    return dvs_builder_finalize(b, sz);
}

static void test_compile_grows(void) {
    void *buf = malloc(64u << 10);          /* far too small to hold it all */
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    size_t sz = 0;
    dvs_problem_t *p = sums(b, 200, &sz);   /* decisions stay under 256 */
    dvs_ctx_t *ctx = dvs_solver_create(buf, 64u << 10, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    CHECK(ctx->n_vars > 300);               /* the sums' aux variables */
    CHECK(ctx->n_vars_capacity >= ctx->n_vars);
    dvs_solve_opts_t opts = {0};
    opts.seed = 7;
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
    CHECK(dvs_solver_validate_model(ctx, p, stderr) == 0);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
}

static void test_incremental_past_8192(void) {
    /* yosys-smtbmc shape: every step adds fresh variables. The context used
     * to stop at its 8192-variable capacity hint. */
    void *buf = malloc(1u << 16);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 16, 0, 0, 65535);
    dvs_builder_add_constraint(b, BIN(b, DVS_BIN_GT, V(b, 0), K(b, 5)));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_ctx_t *ctx = dvs_solver_create(buf, 1u << 16, ba);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);

    enum { STEPS = 40, PER = 300 };          /* 12000 new variables */
    static dvs_builder_t *sb[STEPS];
    static dvs_problem_t *sp[STEPS];
    static size_t ssz[STEPS];
    uint32_t next = ctx->n_vars;
    for (int s = 0; s < STEPS; s++) {
        sb[s] = dvs_builder_create(0, NULL);
        uint32_t base = next;
        for (uint32_t i = 0; i < PER; i++)
            dvs_builder_add_var(sb[s], base + i, 16, 0, 0, 65535);
        /* chain each step to the previous one: v[base] == v[base-1] + 1 */
        dvs_builder_add_constraint(sb[s], BIN(sb[s], DVS_BIN_EQ, V(sb[s], base),
            BIN(sb[s], DVS_BIN_ADD, V(sb[s], base - 1), K(sb[s], 1))));
        for (uint32_t i = 1; i < PER; i++)
            dvs_builder_add_constraint(sb[s], BIN(sb[s], DVS_BIN_EQ, V(sb[s], base + i),
                V(sb[s], base + i - 1)));
        sp[s] = dvs_builder_finalize(sb[s], &ssz[s]);
        int rc = dvs_solver_add_constraint(ctx, sp[s]);
        CHECK(rc == DVS_COMPILE_OK);
        if (rc != DVS_COMPILE_OK) break;
        next = ctx->n_vars;
    }
    CHECK(ctx->n_vars > 12000);
    dvs_solver_reset(ctx);
    CHECK(dvs_solver_pin_var(ctx, 0, 100) == 0);
    CHECK(dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK);
    CHECK(dvs_solver_get_value(ctx, 0) == 100);
    CHECK(dvs_solver_get_value(ctx, ctx->n_vars - 1) > 100);
    dvs_solver_destroy(ctx);
    for (int s = 0; s < STEPS; s++)
        if (sp[s]) { dvs_builder_free_problem(sb[s], sp[s], ssz[s]); dvs_builder_destroy(sb[s]); }
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
}

static void test_fixed_mode(void) {
    /* No block allocator: the buffer is all there is. Too small -> NOMEM. */
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    size_t sz = 0;
    dvs_problem_t *p = sums(b, 200, &sz);
    void *small = malloc(48u << 10);         /* holds the context, not this */
    dvs_ctx_t *ctx = dvs_solver_create(small, 48u << 10, NULL);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_NOMEM);
    dvs_solver_destroy(ctx);
    free(small);

    /* Big enough: compiles, and its tables live in the buffer. */
    void *big = malloc(16u << 20);
    ctx = dvs_solver_create(big, 16u << 20, NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    CHECK(dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK);
    CHECK(dvs_solver_validate_model(ctx, p, stderr) == 0);

    /* Inside a checkpoint a fixed-mode table cannot grow (restore would
     * free it): an add that needs more variables is NOMEM, and the
     * context is whole again after the restore. */
    dvs_builder_t *ab = dvs_builder_create(0, NULL);
    uint32_t base = ctx->n_vars_capacity;   /* past the capacity */
    for (uint32_t i = 0; i < 10; i++) dvs_builder_add_var(ab, base + i, 8, 0, 0, 255);
    dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_LT, V(ab, base), V(ab, base + 1)));
    size_t asz = 0;
    dvs_problem_t *ap = dvs_builder_finalize(ab, &asz);
    int cp = dvs_solver_checkpoint(ctx);
    CHECK(cp == 0);
    CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_NOMEM);
    dvs_solver_restore(ctx, (uint32_t)cp);
    dvs_solver_reset(ctx);
    CHECK(dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK);
    CHECK(dvs_solver_validate_model(ctx, p, stderr) == 0);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(ab, ap, asz);
    dvs_builder_destroy(ab);
    free(big);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

static void test_prop_id_limit(void) {
    /* More propagators than Propagator.prop_id (16 bits) can name. */
    enum { N = 70000 };
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    for (uint32_t i = 0; i < N; i++) dvs_builder_add_var(b, i, 32, 1, 0, 1000000);
    for (uint32_t i = 0; i + 1 < N; i++)
        dvs_builder_add_constraint(b, BIN(b, DVS_BIN_LTE, V(b, i), V(b, i + 1)));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    void *buf = malloc(1u << 16);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);
    dvs_ctx_t *ctx = dvs_solver_create(buf, 1u << 16, ba);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_NOMEM);
    CHECK(ctx->n_props <= DVS_MAX_PROPS);
    dvs_solver_destroy(ctx);
    dvs_block_alloc_destroy(ba);
    free(buf);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

int main(void) {
    test_compile_grows();
    test_incremental_past_8192();
    test_fixed_mode();
    test_prop_id_limit();
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
