/*
 * dvs_solver_create() must not depend on the caller's buffer being zeroed.
 *
 * It initialised the context header field by field, and the list had fallen
 * behind the struct: initial_vars, var_holes_head, prop_guard_vars, ... kept
 * whatever bytes the buffer held. Compile's punch-out of the gaps in a value
 * set (x == a || x == b || ...) calls dvs_solver_exclude_value() before compile
 * records initial_vars, so it dereferenced that garbage. dv-solve-smt2 recycles
 * its context buffer, so this crashed riscv-dv rv64gc riscv_amo_test only after
 * tens of thousands of earlier commands had dirtied the buffer.
 */
#include "dv_solve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

#define CTX_SIZE (1u << 20)

int main(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    CHECK(b != NULL);
    /* x in {3, 7, 11, 30} via a disjunction of equalities: a gapped set the
     * compile punches holes into. */
    dvs_builder_add_var(b, 0, 5, 0, 0, 31);
    dvs_expr_t x = dvs_builder_expr_var(b, 0);
    static const int64_t vals[] = {3, 7, 11, 30};
    dvs_expr_t any = 0; int have = 0;
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        dvs_expr_t eq = dvs_builder_expr_binary(b, DVS_BIN_EQ, x,
                                                dvs_builder_expr_const(b, vals[i], 0));
        any = have ? dvs_builder_expr_binary(b, DVS_BIN_OR, any, eq) : eq;
        have = 1;
    }
    dvs_builder_add_constraint(b, any);
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    CHECK(p != NULL);

    void *buf = malloc(CTX_SIZE);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, CTX_SIZE);
    CHECK(buf != NULL && ba != NULL);
    /* A recycled buffer: every byte non-zero, pointers far outside the heap. */
    memset(buf, 0xBE, CTX_SIZE);

    dvs_ctx_t *ctx = dvs_solver_create(buf, CTX_SIZE, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    for (uint64_t seed = 1; seed <= 20; seed++) {
        dvs_solve_opts_t opts = {0};
        opts.seed = seed;
        dvs_solver_reset(ctx);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
        int64_t v = dvs_solver_get_value(ctx, 0);
        CHECK(v == 3 || v == 7 || v == 11 || v == 30);
    }
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);

    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    else printf("test_ctx_dirty_buffer: OK\n");
    return failures ? 1 : 0;
}
