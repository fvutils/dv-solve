/* Phase B.1 step 3 (incremental API surface — stub semantics):
 *
 * Exercise zsp_sat_push/pop/assume/failed/is_tainted on the stub
 * implementation. The stub guarantees:
 *   - push/pop with no clauses added in the frame leaves the solver clean.
 *   - clauses added in a popped frame mark the solver tainted; next solve
 *     returns ZSP_SAT_UNKNOWN.
 *   - assumed literals are replayed as unit clauses at solve time and can
 *     force UNSAT or be consistent with the problem.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsp_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

static void test_push_pop_clean(void) {
    zsp_sat_t *s = zsp_sat_new(NULL);
    zsp_sat_add_binary(s, 1, 2);
    zsp_sat_add_binary(s, -1, -2);

    zsp_sat_push(s);
    CHECK(zsp_sat_push_depth(s) == 1, "push_depth==1 after one push");
    zsp_sat_pop(s);
    CHECK(zsp_sat_push_depth(s) == 0, "push_depth==0 after matching pop");
    CHECK(!zsp_sat_is_tainted(s), "clean push/pop does not taint");

    int rc = zsp_sat_solve(s);
    CHECK(rc == ZSP_SAT_SAT, "solve after clean push/pop still SAT");
    zsp_sat_free(s);
}

static void test_push_add_pop_taints(void) {
    zsp_sat_t *s = zsp_sat_new(NULL);
    zsp_sat_add_binary(s, 1, 2);
    zsp_sat_push(s);
    zsp_sat_add_unit(s, 1); /* clause added inside the popped frame */
    zsp_sat_pop(s);
    CHECK(zsp_sat_is_tainted(s), "pop with added clauses taints the solver");
    int rc = zsp_sat_solve(s);
    CHECK(rc == ZSP_SAT_UNKNOWN, "tainted solver returns UNKNOWN");
    zsp_sat_free(s);
}

static void test_assume_forces_unsat(void) {
    zsp_sat_t *s = zsp_sat_new(NULL);
    /* Problem: (a v b) — SAT on its own. Assume ¬a ∧ ¬b → UNSAT. */
    zsp_sat_add_binary(s, 1, 2);
    zsp_sat_assume(s, -1);
    zsp_sat_assume(s, -2);
    int rc = zsp_sat_solve(s);
    CHECK(rc == ZSP_SAT_UNSAT, "conflicting assumptions yield UNSAT");
    /* failed() is stubbed to 0; just exercise the call surface. */
    CHECK(zsp_sat_failed(s, -1) == 0, "failed() is 0 in stub mode");
    zsp_sat_free(s);
}

static void test_assume_consistent(void) {
    zsp_sat_t *s = zsp_sat_new(NULL);
    /* (a v b) ∧ (¬a v ¬b), assume a — forces b=false, SAT. */
    zsp_sat_add_binary(s, 1, 2);
    zsp_sat_add_binary(s, -1, -2);
    zsp_sat_assume(s, 1);
    int rc = zsp_sat_solve(s);
    CHECK(rc == ZSP_SAT_SAT, "consistent assumption yields SAT");
    CHECK(zsp_sat_value(s, 1) == 1,  "assumed var=1 is true in model");
    CHECK(zsp_sat_value(s, 2) == -2, "implied var=2 is false in model");
    zsp_sat_free(s);
}

static void test_arena_mark_monotonic(void) {
    /* Phase B.1 step 5 (plumbing slice): the arena mark is just the
     * arena top in `ward` units. It must be monotonically non-decreasing
     * as clauses are added (the deep refactor that would let it shrink
     * via rewind is intentionally not in this slice). Note: small
     * clauses (binary, ternary) are stored inline in watch lists, not
     * in the clause arena, so we need a longer clause to actually grow
     * the arena. */
    zsp_sat_t *s = zsp_sat_new(NULL);
    zsp_sat_arena_mark_t m0 = zsp_sat_arena_save_mark(s);
    /* A 5-literal clause goes into the arena. */
    for (int i = 1; i <= 5; i++) zsp_sat_add(s, i);
    zsp_sat_add(s, 0);
    zsp_sat_arena_mark_t m1 = zsp_sat_arena_save_mark(s);
    CHECK(m1 >= m0, "arena mark non-decreasing after adding a 5-lit clause");
    /* Another 5-lit clause — mark should grow strictly. */
    for (int i = 1; i <= 5; i++) zsp_sat_add(s, -i);
    zsp_sat_add(s, 0);
    zsp_sat_arena_mark_t m2 = zsp_sat_arena_save_mark(s);
    CHECK(m2 > m1, "arena mark strictly grows on additional general clause");
    printf("       arena marks: m0=%zu m1=%zu m2=%zu\n",
           (size_t)m0, (size_t)m1, (size_t)m2);
    zsp_sat_free(s);
}

int main(void) {
    test_push_pop_clean();
    test_push_add_pop_taints();
    test_assume_forces_unsat();
    test_assume_consistent();
    test_arena_mark_monotonic();
    return failures ? 1 : 0;
}
