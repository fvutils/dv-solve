/*
 * CaDiCaL backend tests for the zsp_sat seam.
 *
 * Validates (a) one-shot parity with kissat and (b) the *real* incremental
 * behavior kissat cannot provide: assumptions that are local to a single solve,
 * unsat-core membership via failed(), and clause addition between solves.
 *
 * Skips cleanly (reporting success) when built with -DZSP_WITH_CADICAL=OFF, so
 * the pure-C build still runs this test binary without a CaDiCaL dependency.
 */

#include <stdio.h>

#include "zsp_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { \
    if (!(c)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
    else printf("PASS %s\n", msg); } while (0)

/* One-shot SAT / UNSAT, matching kissat semantics. */
static void test_one_shot(void) {
    zsp_sat_t *s = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    CHECK(zsp_sat_backend(s) == ZSP_SAT_BACKEND_CADICAL, "backend is cadical");
    /* (x1) & (-x1 | x2)  =>  SAT with x1=T, x2=T */
    zsp_sat_add_unit(s, 1);
    zsp_sat_add_binary(s, -1, 2);
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "one-shot SAT");
    CHECK(zsp_sat_value(s, 1) == 1, "x1 true");
    CHECK(zsp_sat_value(s, 2) == 2, "x2 true");
    zsp_sat_free(s);

    s = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    zsp_sat_add_unit(s, 1);
    zsp_sat_add_unit(s, -1);
    CHECK(zsp_sat_solve(s) == ZSP_SAT_UNSAT, "one-shot UNSAT");
    zsp_sat_free(s);
}

/* Assumptions are local to a solve: the same clause DB is SAT, then UNSAT under
 * conflicting assumptions, then SAT again once the assumptions are gone. kissat
 * cannot do this (its stub makes assumptions permanent unit clauses). */
static void test_incremental_assumptions(void) {
    zsp_sat_t *s = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    /* single clause (x1 | x2) */
    zsp_sat_add_binary(s, 1, 2);

    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "clause SAT (no assumptions)");

    /* assume x1 false => x2 forced true */
    zsp_sat_assume(s, -1);
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "SAT under assume(-x1)");
    CHECK(zsp_sat_value(s, 2) == 2, "x2 forced true under assume(-x1)");

    /* assume x1 false AND x2 false => UNSAT (clause needs one true) */
    zsp_sat_assume(s, -1);
    zsp_sat_assume(s, -2);
    CHECK(zsp_sat_solve(s) == ZSP_SAT_UNSAT, "UNSAT under assume(-x1,-x2)");
    /* both assumptions are in the core */
    CHECK(zsp_sat_failed(s, -1) == 1, "failed(-x1) reports core membership");
    CHECK(zsp_sat_failed(s, -2) == 1, "failed(-x2) reports core membership");

    /* assumptions cleared => SAT again (they were never permanent) */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "SAT again after assumptions cleared");
    zsp_sat_free(s);
}

/* Adding clauses between solves (true incrementality). */
static void test_incremental_add(void) {
    zsp_sat_t *s = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    zsp_sat_add_binary(s, 1, 2);              /* (x1 | x2) */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "add: initial SAT");

    zsp_sat_add_unit(s, -1);                  /* + (-x1) : x1 false */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "add: SAT after (-x1)");
    CHECK(zsp_sat_value(s, 2) == 2, "add: x2 true after (-x1)");

    zsp_sat_add_unit(s, -2);                  /* + (-x2) : now UNSAT */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_UNSAT, "add: UNSAT after (-x1,-x2)");
    zsp_sat_free(s);
}

/* Selector-literal push/pop: base clauses persist; per-frame clauses are added
 * on push and disabled on pop. Mirrors a BMC-style unroll (assert base, then
 * push/assert-depth/check/pop per depth). */
static void test_push_pop_scoping(void) {
    zsp_sat_t *s = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    zsp_sat_add_binary(s, 1, 2);                 /* base: (x1 | x2), permanent */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "scope: base SAT");

    /* frame that conflicts with the base */
    zsp_sat_push(s);
    zsp_sat_add_unit(s, -1);
    zsp_sat_add_unit(s, -2);                     /* x1=F, x2=F vs base => UNSAT */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_UNSAT, "scope: frame UNSAT");
    zsp_sat_pop(s);
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "scope: base restored after pop");

    /* frame that forces a value */
    zsp_sat_push(s);
    zsp_sat_add_unit(s, -1);                     /* x1=F => x2 forced true */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "scope: forcing frame SAT");
    CHECK(zsp_sat_value(s, 2) == 2, "scope: x2 forced true in frame");
    zsp_sat_pop(s);
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "scope: SAT after second pop");
    zsp_sat_free(s);
}

/* Nested frames: popping an inner frame keeps the outer frame's clauses. */
static void test_nested_scoping(void) {
    zsp_sat_t *s = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    zsp_sat_add_binary(s, 1, 2);                 /* base: (x1 | x2) */

    zsp_sat_push(s);                             /* frame A */
    zsp_sat_add_unit(s, -1);                     /* x1=F => x2=T */
    zsp_sat_push(s);                             /* frame B */
    zsp_sat_add_unit(s, -2);                     /* x2=F => with A+base UNSAT */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_UNSAT, "nested: A+B UNSAT");
    zsp_sat_pop(s);                              /* drop B only */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "nested: SAT with only A after inner pop");
    CHECK(zsp_sat_value(s, 2) == 2, "nested: x2 still forced by A");
    zsp_sat_pop(s);                              /* drop A */
    CHECK(zsp_sat_solve(s) == ZSP_SAT_SAT, "nested: base-only SAT after outer pop");
    CHECK(!zsp_sat_is_tainted(s), "nested: never tainted on incremental backend");
    zsp_sat_free(s);
}

int main(void) {
    if (!zsp_sat_has_cadical()) {
        printf("SKIP: built without ZSP_WITH_CADICAL (pure-C build)\n");
        return 0;
    }
    test_one_shot();
    test_incremental_assumptions();
    test_incremental_add();
    test_push_pop_scoping();
    test_nested_scoping();
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("all cadical backend tests passed\n");
    return 0;
}
