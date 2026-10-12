/*
 * B118: dvs_solver_add_constraint read a declared variable whose id the
 * compile had taken for an internal variable as that variable. Base
 * `x2 == x1 + 1000` makes an aux for `x1 + 1000` at id 3; an add declaring
 * "new" x3 with `x3 == x0 + 3` then constrained the aux (domain
 * [1000, 1200]) and answered unsat. Now: BAD_VAR, the context unchanged,
 * and new variables numbered from dvs_solver_n_vars() work.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "dv_solve.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static dvs_expr_t V(dvs_builder_t *b, uint32_t id) { return dvs_builder_expr_var(b, id); }
static dvs_expr_t K(dvs_builder_t *b, int64_t v) { return dvs_builder_expr_const(b, v, 0); }
static dvs_expr_t BIN(dvs_builder_t *b, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}

/* An add declaring `id` (width w) with `id == x0 + 3`. */
static int _add(dvs_ctx_t *ctx, uint32_t id, uint16_t w) {
    dvs_builder_t *ab = dvs_builder_create(0, NULL);
    dvs_builder_add_var(ab, id, w, 0, 0, 1000);
    dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_EQ, V(ab, id),
                                       BIN(ab, DVS_BIN_ADD, V(ab, 0), K(ab, 3))));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(ab, &sz);
    int rc = dvs_solver_add_constraint(ctx, p);
    dvs_builder_free_problem(ab, p, sz);
    dvs_builder_destroy(ab);
    return rc;
}

int main(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 200);
    dvs_builder_add_var(b, 1, 8, 0, 0, 200);
    dvs_builder_add_var(b, 2, 16, 0, 0, 65535);
    dvs_builder_add_constraint(b, BIN(b, DVS_BIN_EQ, V(b, 2),
                                      BIN(b, DVS_BIN_ADD, V(b, 1), K(b, 1000))));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    dvs_builder_free_problem(b, p, sz);

    uint32_t n = dvs_solver_n_vars(ctx);
    CHECK(n > 3);                            /* the compile made an aux */
    dvs_solve_opts_t opts = {0};
    opts.seed = 5;

    /* The internal id: refused, nothing changed. */
    CHECK(_add(ctx, 3, 16) == DVS_COMPILE_BAD_VAR);
    CHECK(dvs_solver_n_vars(ctx) == n);
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
    CHECK(dvs_solver_get_value(ctx, 2) == dvs_solver_get_value(ctx, 1) + 1000);

    /* A declared var re-declared at another width: refused. */
    CHECK(_add(ctx, 1, 16) == DVS_COMPILE_BAD_VAR);

    /* Numbered from dvs_solver_n_vars(): a new variable. */
    CHECK(_add(ctx, n, 16) == DVS_COMPILE_OK);
    dvs_solver_reset(ctx);
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
    CHECK(dvs_solver_get_value(ctx, n) == dvs_solver_get_value(ctx, 0) + 3);
    CHECK(dvs_solver_get_value(ctx, 2) == dvs_solver_get_value(ctx, 1) + 1000);

    /* A declared var re-declared at its width: a no-op. */
    {
        dvs_builder_t *ab = dvs_builder_create(0, NULL);
        dvs_builder_add_var(ab, 0, 8, 0, 0, 200);
        dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_LT, V(ab, 0), K(ab, 150)));
        size_t asz = 0;
        dvs_problem_t *ap = dvs_builder_finalize(ab, &asz);
        CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_OK);
        dvs_builder_free_problem(ab, ap, asz);
        dvs_builder_destroy(ab);
        dvs_solver_reset(ctx);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
        CHECK(dvs_solver_get_value(ctx, 0) < 150);
    }

    /* B119: a refused add changes nothing, and an add may not name a
     * variable no problem declared. */
    {
        uint32_t m = dvs_solver_n_vars(ctx);
        dvs_builder_t *ab = dvs_builder_create(0, NULL);
        dvs_builder_add_var(ab, m, 8, 0, 0, 255);
        dvs_builder_add_var(ab, m + 1, 65, 0, 0, INT64_MAX);
        dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_EQ, V(ab, m), K(ab, 9)));
        size_t asz = 0;
        dvs_problem_t *ap = dvs_builder_finalize(ab, &asz);
        CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_UNSUPPORTED_WIDTH);
        CHECK(dvs_solver_n_vars(ctx) == m);
        dvs_builder_free_problem(ab, ap, asz);
        dvs_builder_destroy(ab);

        /* m was refused with it: naming it is naming nothing. */
        ab = dvs_builder_create(0, NULL);
        dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_EQ, V(ab, m), K(ab, 9)));
        ap = dvs_builder_finalize(ab, &asz);
        CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_BAD_VAR);
        CHECK(dvs_solver_n_vars(ctx) == m);
        dvs_builder_free_problem(ab, ap, asz);
        dvs_builder_destroy(ab);

        /* Declaring m + 3 leaves m .. m + 2 undeclared: naming m + 1 is an
         * error, not a variable fixed to 0. */
        ab = dvs_builder_create(0, NULL);
        dvs_builder_add_var(ab, m + 3, 8, 0, 0, 255);
        dvs_builder_add_constraint(ab, BIN(ab, DVS_BIN_EQ, V(ab, m + 3),
                                           BIN(ab, DVS_BIN_ADD, V(ab, m + 1), K(ab, 1))));
        ap = dvs_builder_finalize(ab, &asz);
        CHECK(dvs_solver_add_constraint(ctx, ap) == DVS_COMPILE_BAD_VAR);
        CHECK(dvs_solver_n_vars(ctx) == m);
        dvs_builder_free_problem(ab, ap, asz);
        dvs_builder_destroy(ab);

        dvs_solver_reset(ctx);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
    }

    dvs_solver_free(ctx);
    dvs_builder_destroy(b);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("add var ids: ok\n");
    return failures ? 1 : 0;
}
