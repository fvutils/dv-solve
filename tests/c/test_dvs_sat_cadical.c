/*
 * CaDiCaL backend tests for the dvs_sat seam.
 *
 * Validates (a) one-shot parity with kissat and (b) the *real* incremental
 * behavior kissat cannot provide: assumptions that are local to a single solve,
 * unsat-core membership via failed(), and clause addition between solves.
 *
 * Skips cleanly (reporting success) when built with -DDVS_WITH_CADICAL=OFF, so
 * the pure-C build still runs this test binary without a CaDiCaL dependency.
 */

#include <stdio.h>

#include "dvs_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { \
    if (!(c)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
    else printf("PASS %s\n", msg); } while (0)

/* One-shot SAT / UNSAT, matching kissat semantics. */
static void test_one_shot(void) {
    dvs_sat_t *s = dvs_sat_new_backend(NULL, DVS_SAT_BACKEND_CADICAL);
    CHECK(dvs_sat_backend(s) == DVS_SAT_BACKEND_CADICAL, "backend is cadical");
    /* (x1) & (-x1 | x2)  =>  SAT with x1=T, x2=T */
    dvs_sat_add_unit(s, 1);
    dvs_sat_add_binary(s, -1, 2);
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "one-shot SAT");
    CHECK(dvs_sat_value(s, 1) == 1, "x1 true");
    CHECK(dvs_sat_value(s, 2) == 2, "x2 true");
    dvs_sat_free(s);

    s = dvs_sat_new_backend(NULL, DVS_SAT_BACKEND_CADICAL);
    dvs_sat_add_unit(s, 1);
    dvs_sat_add_unit(s, -1);
    CHECK(dvs_sat_solve(s) == DVS_SAT_UNSAT, "one-shot UNSAT");
    dvs_sat_free(s);
}

/* Assumptions are local to a solve: the same clause DB is SAT, then UNSAT under
 * conflicting assumptions, then SAT again once the assumptions are gone. kissat
 * cannot do this (its stub makes assumptions permanent unit clauses). */
static void test_incremental_assumptions(void) {
    dvs_sat_t *s = dvs_sat_new_backend(NULL, DVS_SAT_BACKEND_CADICAL);
    /* single clause (x1 | x2) */
    dvs_sat_add_binary(s, 1, 2);

    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "clause SAT (no assumptions)");

    /* assume x1 false => x2 forced true */
    dvs_sat_assume(s, -1);
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "SAT under assume(-x1)");
    CHECK(dvs_sat_value(s, 2) == 2, "x2 forced true under assume(-x1)");

    /* assume x1 false AND x2 false => UNSAT (clause needs one true) */
    dvs_sat_assume(s, -1);
    dvs_sat_assume(s, -2);
    CHECK(dvs_sat_solve(s) == DVS_SAT_UNSAT, "UNSAT under assume(-x1,-x2)");
    /* both assumptions are in the core */
    CHECK(dvs_sat_failed(s, -1) == 1, "failed(-x1) reports core membership");
    CHECK(dvs_sat_failed(s, -2) == 1, "failed(-x2) reports core membership");

    /* assumptions cleared => SAT again (they were never permanent) */
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "SAT again after assumptions cleared");
    dvs_sat_free(s);
}

/* Adding clauses between solves (true incrementality). */
static void test_incremental_add(void) {
    dvs_sat_t *s = dvs_sat_new_backend(NULL, DVS_SAT_BACKEND_CADICAL);
    dvs_sat_add_binary(s, 1, 2);              /* (x1 | x2) */
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "add: initial SAT");

    dvs_sat_add_unit(s, -1);                  /* + (-x1) : x1 false */
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "add: SAT after (-x1)");
    CHECK(dvs_sat_value(s, 2) == 2, "add: x2 true after (-x1)");

    dvs_sat_add_unit(s, -2);                  /* + (-x2) : now UNSAT */
    CHECK(dvs_sat_solve(s) == DVS_SAT_UNSAT, "add: UNSAT after (-x1,-x2)");
    dvs_sat_free(s);
}

/* Selector-literal push/pop: base clauses persist; per-frame clauses are added
 * on push and disabled on pop. Mirrors a BMC-style unroll (assert base, then
 * push/assert-depth/check/pop per depth). */
static void test_push_pop_scoping(void) {
    dvs_sat_t *s = dvs_sat_new_backend(NULL, DVS_SAT_BACKEND_CADICAL);
    dvs_sat_add_binary(s, 1, 2);                 /* base: (x1 | x2), permanent */
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "scope: base SAT");

    /* frame that conflicts with the base */
    dvs_sat_push(s);
    dvs_sat_add_unit(s, -1);
    dvs_sat_add_unit(s, -2);                     /* x1=F, x2=F vs base => UNSAT */
    CHECK(dvs_sat_solve(s) == DVS_SAT_UNSAT, "scope: frame UNSAT");
    dvs_sat_pop(s);
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "scope: base restored after pop");

    /* frame that forces a value */
    dvs_sat_push(s);
    dvs_sat_add_unit(s, -1);                     /* x1=F => x2 forced true */
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "scope: forcing frame SAT");
    CHECK(dvs_sat_value(s, 2) == 2, "scope: x2 forced true in frame");
    dvs_sat_pop(s);
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "scope: SAT after second pop");
    dvs_sat_free(s);
}

/* Nested frames: popping an inner frame keeps the outer frame's clauses. */
static void test_nested_scoping(void) {
    dvs_sat_t *s = dvs_sat_new_backend(NULL, DVS_SAT_BACKEND_CADICAL);
    dvs_sat_add_binary(s, 1, 2);                 /* base: (x1 | x2) */

    dvs_sat_push(s);                             /* frame A */
    dvs_sat_add_unit(s, -1);                     /* x1=F => x2=T */
    dvs_sat_push(s);                             /* frame B */
    dvs_sat_add_unit(s, -2);                     /* x2=F => with A+base UNSAT */
    CHECK(dvs_sat_solve(s) == DVS_SAT_UNSAT, "nested: A+B UNSAT");
    dvs_sat_pop(s);                              /* drop B only */
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "nested: SAT with only A after inner pop");
    CHECK(dvs_sat_value(s, 2) == 2, "nested: x2 still forced by A");
    dvs_sat_pop(s);                              /* drop A */
    CHECK(dvs_sat_solve(s) == DVS_SAT_SAT, "nested: base-only SAT after outer pop");
    CHECK(!dvs_sat_is_tainted(s), "nested: never tainted on incremental backend");
    dvs_sat_free(s);
}

int main(void) {
    if (!dvs_sat_has_cadical()) {
        printf("SKIP: built without DVS_WITH_CADICAL (pure-C build)\n");
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
