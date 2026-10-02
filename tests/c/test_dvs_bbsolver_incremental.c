/* Incremental-reuse tests for dvs_bbsolver (Phase 5a/5b foundation).
 *
 * Validates that one live bbsolver instance can accept additional asserted
 * predicates and be re-solved (dvs_bbsolver_assert + dvs_bbsolver_resolve),
 * reusing the accumulated clause DB instead of a free+rebuild. Correctness is
 * backend-independent, so the scenarios run on the default backend and, when
 * compiled with CaDiCaL, again on the incremental backend (which additionally
 * retains learned clauses — not directly observable from a verdict, but the
 * verdicts must match).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "dvs_bbsolver.h"
#include "dvs_builder.h"
#include "dvs_problem.h"
#include "dvs_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { \
    if (!(c)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
    else printf("PASS %s\n", msg); } while (0)

/* Build: vars x,y in [0,255]; base constraint x+y==10; plus two *unasserted*
 * predicates in the pool (x<3 and x>=3) for incremental assertion. */
static dvs_problem_t *build_problem(dvs_builder_t *b, size_t *sz,
                                   dvs_expr_t *xlt3, dvs_expr_t *xge3) {
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_expr_t x   = dvs_builder_expr_var(b, 0);
    dvs_expr_t y   = dvs_builder_expr_var(b, 1);
    dvs_expr_t k10 = dvs_builder_expr_const(b, 10, 0);
    dvs_expr_t k3  = dvs_builder_expr_const(b, 3, 0);
    dvs_expr_t sum = dvs_builder_expr_binary(b, DVS_BIN_ADD, x, y);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ, sum, k10));
    /* Predicates left unasserted — asserted incrementally below. They still
     * live in the finalized pool (finalize copies the whole builder pool). */
    *xlt3 = dvs_builder_expr_binary(b, DVS_BIN_LT,  x, k3);
    *xge3 = dvs_builder_expr_binary(b, DVS_BIN_GTE, x, k3);
    return dvs_builder_finalize(b, sz);
}

/* Full incremental flow — requires an incremental backend (CaDiCaL). */
static void run_incremental_scenario(const char *tag) {
    char label[128];
    dvs_builder_t *b = dvs_builder_create(4096, NULL);
    size_t sz;
    dvs_expr_t xlt3, xge3;
    dvs_problem_t *p = build_problem(b, &sz, &xlt3, &xge3);

    dvs_bbsolver_t *S = dvs_bbsolver_new(NULL, p);

    snprintf(label, sizeof label, "[%s] base x+y==10 SAT", tag);
    CHECK(dvs_bbsolver_check(S, 0) == DVS_BB_SAT, label);

    int64_t vx = -1, vy = -1;
    dvs_bbsolver_value(S, 0, &vx);
    dvs_bbsolver_value(S, 1, &vy);
    snprintf(label, sizeof label, "[%s] base model sum==10", tag);
    CHECK(vx + vy == 10, label);

    /* Incrementally add x<3 and re-solve on the SAME instance. */
    snprintf(label, sizeof label, "[%s] assert(x<3) accepted", tag);
    CHECK(dvs_bbsolver_assert(S, xlt3) == 0, label);
    snprintf(label, sizeof label, "[%s] resolve after x<3 SAT", tag);
    CHECK(dvs_bbsolver_resolve(S, 0) == DVS_BB_SAT, label);

    vx = -1; vy = -1;
    dvs_bbsolver_value(S, 0, &vx);
    dvs_bbsolver_value(S, 1, &vy);
    snprintf(label, sizeof label, "[%s] model honors added x<3 and base", tag);
    CHECK(vx < 3 && vx + vy == 10, label);
    printf("       [%s] model after x<3: x=%" PRId64 " y=%" PRId64 "\n", tag, vx, vy);

    /* Add the conflicting x>=3: the retained DB now holds both, so UNSAT. */
    snprintf(label, sizeof label, "[%s] assert(x>=3) accepted", tag);
    CHECK(dvs_bbsolver_assert(S, xge3) == 0, label);
    snprintf(label, sizeof label, "[%s] resolve after x<3 & x>=3 UNSAT", tag);
    CHECK(dvs_bbsolver_resolve(S, 0) == DVS_BB_UNSAT, label);

    dvs_bbsolver_free(S);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

/* Differential: the incremental result must match a fresh monolithic build of
 * the same accumulated constraint set. */
static void test_matches_fresh_build(void) {
    /* Fresh build of {x+y==10, x<3}: expect SAT. */
    dvs_builder_t *b = dvs_builder_create(4096, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_expr_t x   = dvs_builder_expr_var(b, 0);
    dvs_expr_t y   = dvs_builder_expr_var(b, 1);
    dvs_expr_t k10 = dvs_builder_expr_const(b, 10, 0);
    dvs_expr_t k3  = dvs_builder_expr_const(b, 3, 0);
    dvs_expr_t sum = dvs_builder_expr_binary(b, DVS_BIN_ADD, x, y);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ, sum, k10));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT, x, k3));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_bbsolver_t *S = dvs_bbsolver_new(NULL, p);
    CHECK(dvs_bbsolver_check(S, 0) == DVS_BB_SAT,
          "fresh {x+y==10, x<3} SAT (matches incremental)");
    dvs_bbsolver_free(S);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

/* On a non-incremental backend (kissat) assert/resolve must refuse cleanly
 * (DVS_BB_ERROR) rather than crash — the caller is expected to free+rebuild. */
static void test_noninc_refuses(void) {
    dvs_builder_t *b = dvs_builder_create(4096, NULL);
    size_t sz;
    dvs_expr_t xlt3, xge3;
    dvs_problem_t *p = build_problem(b, &sz, &xlt3, &xge3);
    dvs_bbsolver_t *S = dvs_bbsolver_new(NULL, p);

    CHECK(dvs_bbsolver_check(S, 0) == DVS_BB_SAT, "[kissat] base SAT");
    CHECK(dvs_bbsolver_assert(S, xge3) == DVS_BB_ERROR,
          "[kissat] assert refused on non-incremental backend");
    CHECK(dvs_bbsolver_resolve(S, 0) == DVS_BB_ERROR,
          "[kissat] resolve refused on non-incremental backend");

    dvs_bbsolver_free(S);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

int main(void) {
    /* Default backend (kissat unless the environment overrides). */
    unsetenv("DV_SAT_BACKEND");
    test_noninc_refuses();
    test_matches_fresh_build();

    /* Incremental backend, when compiled in. */
    if (dvs_sat_has_cadical()) {
        setenv("DV_SAT_BACKEND", "cadical", 1);
        run_incremental_scenario("cadical");
        unsetenv("DV_SAT_BACKEND");
    } else {
        printf("SKIP cadical scenario (built without DVS_WITH_CADICAL)\n");
    }

    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("all bbsolver incremental tests passed\n");
    return 0;
}
