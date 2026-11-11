/*
 * dvs_solver_reset() must release the trail. The trail and entailment records
 * live on the context's dynamic stack; a full reset dropped the pointers to
 * them but kept the stack's blocks, so every solve after a reset took more
 * blocks and a long run (one randomize() call per solve, a state variable
 * pinned each time) grew without bound.
 */
#include "dv_solve.h"
#include "dvs_alloc.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

/* malloc, counting the bytes held */
static size_t live = 0;
static void *count_alloc(dvs_alloc_t *self, size_t size) {
    (void)self;
    live += size;
    return malloc(size);
}
static void count_release(dvs_alloc_t *self, void *ptr, size_t size) {
    (void)self;
    live -= size;
    free(ptr);
}

int main(void) {
    /* x == s || x == s + 1 || x == 5, s a state variable pinned per solve */
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    CHECK(b != NULL);
    dvs_builder_add_var(b, 0, 32, 1, INT32_MIN, INT32_MAX);
    dvs_builder_add_var(b, 1, 32, 1, INT32_MIN, INT32_MAX);
    dvs_builder_set_var_kind(b, 1, DVS_VAR_STATE);
    dvs_expr_t x = dvs_builder_expr_var(b, 0);
    dvs_expr_t s = dvs_builder_expr_var(b, 1);
    dvs_expr_t s1 = dvs_builder_expr_binary(b, DVS_BIN_ADD, s, dvs_builder_expr_const(b, 1, 1));
    dvs_expr_t c = dvs_builder_expr_binary(
        b, DVS_BIN_OR,
        dvs_builder_expr_binary(b, DVS_BIN_OR, dvs_builder_expr_binary(b, DVS_BIN_EQ, x, s),
                                dvs_builder_expr_binary(b, DVS_BIN_EQ, x, s1)),
        dvs_builder_expr_binary(b, DVS_BIN_EQ, x, dvs_builder_expr_const(b, 5, 1)));
    dvs_builder_add_constraint(b, c);
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    CHECK(p != NULL);

    dvs_alloc_t counting = {count_alloc, count_release, NULL};
    /* Small blocks, so a few solves' records take a new one */
    dvs_block_alloc_t *ba = dvs_block_alloc_create(&counting, 4096);
    CHECK(ba != NULL);
    void *buf = malloc(1u << 20);
    dvs_ctx_t *ctx = dvs_solver_create(buf, 1u << 20, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    size_t after_warmup = 0;
    for (int64_t i = 0; i < 5000; i++) {
        dvs_solve_opts_t opts = {0};
        opts.seed = (uint64_t)i + 1;
        dvs_solver_reset(ctx);
        CHECK(dvs_solver_pin_var(ctx, 1, i * 1000) == 0);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
        int64_t v = dvs_solver_get_value(ctx, 0);
        CHECK(v == i * 1000 || v == i * 1000 + 1 || v == 5);
        if (i == 100) after_warmup = live;
    }
    CHECK(live <= after_warmup);
    if (live > after_warmup)
        fprintf(stderr, "held %zu bytes after 100 solves, %zu after 5000\n", after_warmup, live);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);

    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    else printf("test_reset_memory: OK\n");
    return failures ? 1 : 0;
}
