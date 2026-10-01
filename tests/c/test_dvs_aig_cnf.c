/* End-to-end test: build an AIG, Tseitin-encode it, solve via kissat,
 * verify the model satisfies the original constraints. */
#include <stdio.h>
#include <stdlib.h>

#include "dvs_aig.h"
#include "dvs_aig_cnf.h"
#include "dvs_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

/* Test 1: a /\ b is SAT, models must have a=true, b=true. */
static void test_and(void) {
    dvs_aig_t *aig = dvs_aig_new(NULL);
    dvs_sat_t *sat = dvs_sat_new(NULL);
    dvs_aig_cnf_t *enc = dvs_aig_cnf_new(NULL, aig, sat);

    dvs_aig_node_t a = dvs_aig_mk_input(aig);
    dvs_aig_node_t b = dvs_aig_mk_input(aig);
    dvs_aig_node_t ab = dvs_aig_mk_and(aig, a, b);

    dvs_aig_cnf_encode(enc, ab, /*top_level=*/1);

    int rc = dvs_sat_solve(sat);
    CHECK(rc == DVS_SAT_SAT, "a /\\ b is SAT");
    if (rc == DVS_SAT_SAT) {
        CHECK(dvs_aig_cnf_value(enc, a) == 1, "model a=true");
        CHECK(dvs_aig_cnf_value(enc, b) == 1, "model b=true");
    }

    dvs_aig_cnf_free(enc);
    dvs_sat_free(sat);
    dvs_aig_free(aig);
}

/* Test 2: a /\ ~a is UNSAT. */
static void test_contradiction(void) {
    dvs_aig_t *aig = dvs_aig_new(NULL);
    dvs_sat_t *sat = dvs_sat_new(NULL);
    dvs_aig_cnf_t *enc = dvs_aig_cnf_new(NULL, aig, sat);

    dvs_aig_node_t a = dvs_aig_mk_input(aig);
    /* a /\ ~a will be folded to FALSE by the AIG rewriter — so this tests
     * the FALSE -> unit clause path. */
    dvs_aig_node_t f = dvs_aig_mk_and(aig, a, dvs_aig_not(a));
    CHECK(f == DVS_AIG_FALSE, "a /\\ ~a rewrites to FALSE");

    /* Asserting FALSE at top-level: top_level=1 will assert leaf=FALSE as
     * unit clause {-1}, plus encode_subtree on FALSE asserts {1} (TRUE unit).
     * Together they're unsat. */
    dvs_aig_cnf_encode(enc, f, /*top_level=*/1);
    int rc = dvs_sat_solve(sat);
    CHECK(rc == DVS_SAT_UNSAT, "asserting FALSE is UNSAT");

    dvs_aig_cnf_free(enc);
    dvs_sat_free(sat);
    dvs_aig_free(aig);
}

/* Test 3: (a XOR b) is SAT, model must have a != b. */
static void test_xor(void) {
    dvs_aig_t *aig = dvs_aig_new(NULL);
    dvs_sat_t *sat = dvs_sat_new(NULL);
    dvs_aig_cnf_t *enc = dvs_aig_cnf_new(NULL, aig, sat);

    dvs_aig_node_t a = dvs_aig_mk_input(aig);
    dvs_aig_node_t b = dvs_aig_mk_input(aig);
    dvs_aig_node_t xor_ab = dvs_aig_mk_xor(aig, a, b);

    dvs_aig_cnf_encode(enc, xor_ab, /*top_level=*/1);

    int rc = dvs_sat_solve(sat);
    CHECK(rc == DVS_SAT_SAT, "a XOR b is SAT");
    if (rc == DVS_SAT_SAT) {
        int va = dvs_aig_cnf_value(enc, a);
        int vb = dvs_aig_cnf_value(enc, b);
        CHECK(va != vb && va != 0 && vb != 0, "model: a != b");
    }
    printf("xor stats: vars=%llu clauses=%llu literals=%llu\n",
           (unsigned long long)dvs_aig_cnf_num_vars(enc),
           (unsigned long long)dvs_aig_cnf_num_clauses(enc),
           (unsigned long long)dvs_aig_cnf_num_literals(enc));

    dvs_aig_cnf_free(enc);
    dvs_sat_free(sat);
    dvs_aig_free(aig);
}

/* Test 4: ITE encoding — ite(c, a, b) with c true forces output=a. */
static void test_ite(void) {
    dvs_aig_t *aig = dvs_aig_new(NULL);
    dvs_sat_t *sat = dvs_sat_new(NULL);
    dvs_aig_cnf_t *enc = dvs_aig_cnf_new(NULL, aig, sat);

    dvs_aig_node_t c = dvs_aig_mk_input(aig);
    dvs_aig_node_t a = dvs_aig_mk_input(aig);
    dvs_aig_node_t b = dvs_aig_mk_input(aig);

    /* (c) /\ ite(c, a, b) /\ ~a  should be UNSAT (c true ⇒ ite = a; ~a ⇒ contradiction) */
    dvs_aig_node_t ite = dvs_aig_mk_ite(aig, c, a, b);
    dvs_aig_node_t goal = dvs_aig_mk_and(aig, c, dvs_aig_mk_and(aig, ite, dvs_aig_not(a)));

    dvs_aig_cnf_encode(enc, goal, /*top_level=*/1);
    int rc = dvs_sat_solve(sat);
    CHECK(rc == DVS_SAT_UNSAT, "c /\\ ite(c,a,b) /\\ ~a is UNSAT");

    dvs_aig_cnf_free(enc);
    dvs_sat_free(sat);
    dvs_aig_free(aig);
}

int main(void) {
    test_and();
    test_contradiction();
    test_xor();
    test_ite();
    return failures ? 1 : 0;
}
