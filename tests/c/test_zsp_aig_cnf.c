/* End-to-end test: build an AIG, Tseitin-encode it, solve via kissat,
 * verify the model satisfies the original constraints. */
#include <stdio.h>
#include <stdlib.h>

#include "zsp_aig.h"
#include "zsp_aig_cnf.h"
#include "zsp_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

/* Test 1: a /\ b is SAT, models must have a=true, b=true. */
static void test_and(void) {
    zsp_aig_t *aig = zsp_aig_new(NULL);
    zsp_sat_t *sat = zsp_sat_new(NULL);
    zsp_aig_cnf_t *enc = zsp_aig_cnf_new(NULL, aig, sat);

    zsp_aig_node_t a = zsp_aig_mk_input(aig);
    zsp_aig_node_t b = zsp_aig_mk_input(aig);
    zsp_aig_node_t ab = zsp_aig_mk_and(aig, a, b);

    zsp_aig_cnf_encode(enc, ab, /*top_level=*/1);

    int rc = zsp_sat_solve(sat);
    CHECK(rc == ZSP_SAT_SAT, "a /\\ b is SAT");
    if (rc == ZSP_SAT_SAT) {
        CHECK(zsp_aig_cnf_value(enc, a) == 1, "model a=true");
        CHECK(zsp_aig_cnf_value(enc, b) == 1, "model b=true");
    }

    zsp_aig_cnf_free(enc);
    zsp_sat_free(sat);
    zsp_aig_free(aig);
}

/* Test 2: a /\ ~a is UNSAT. */
static void test_contradiction(void) {
    zsp_aig_t *aig = zsp_aig_new(NULL);
    zsp_sat_t *sat = zsp_sat_new(NULL);
    zsp_aig_cnf_t *enc = zsp_aig_cnf_new(NULL, aig, sat);

    zsp_aig_node_t a = zsp_aig_mk_input(aig);
    /* a /\ ~a will be folded to FALSE by the AIG rewriter — so this tests
     * the FALSE -> unit clause path. */
    zsp_aig_node_t f = zsp_aig_mk_and(aig, a, zsp_aig_not(a));
    CHECK(f == ZSP_AIG_FALSE, "a /\\ ~a rewrites to FALSE");

    /* Asserting FALSE at top-level: top_level=1 will assert leaf=FALSE as
     * unit clause {-1}, plus encode_subtree on FALSE asserts {1} (TRUE unit).
     * Together they're unsat. */
    zsp_aig_cnf_encode(enc, f, /*top_level=*/1);
    int rc = zsp_sat_solve(sat);
    CHECK(rc == ZSP_SAT_UNSAT, "asserting FALSE is UNSAT");

    zsp_aig_cnf_free(enc);
    zsp_sat_free(sat);
    zsp_aig_free(aig);
}

/* Test 3: (a XOR b) is SAT, model must have a != b. */
static void test_xor(void) {
    zsp_aig_t *aig = zsp_aig_new(NULL);
    zsp_sat_t *sat = zsp_sat_new(NULL);
    zsp_aig_cnf_t *enc = zsp_aig_cnf_new(NULL, aig, sat);

    zsp_aig_node_t a = zsp_aig_mk_input(aig);
    zsp_aig_node_t b = zsp_aig_mk_input(aig);
    zsp_aig_node_t xor_ab = zsp_aig_mk_xor(aig, a, b);

    zsp_aig_cnf_encode(enc, xor_ab, /*top_level=*/1);

    int rc = zsp_sat_solve(sat);
    CHECK(rc == ZSP_SAT_SAT, "a XOR b is SAT");
    if (rc == ZSP_SAT_SAT) {
        int va = zsp_aig_cnf_value(enc, a);
        int vb = zsp_aig_cnf_value(enc, b);
        CHECK(va != vb && va != 0 && vb != 0, "model: a != b");
    }
    printf("xor stats: vars=%llu clauses=%llu literals=%llu\n",
           (unsigned long long)zsp_aig_cnf_num_vars(enc),
           (unsigned long long)zsp_aig_cnf_num_clauses(enc),
           (unsigned long long)zsp_aig_cnf_num_literals(enc));

    zsp_aig_cnf_free(enc);
    zsp_sat_free(sat);
    zsp_aig_free(aig);
}

/* Test 4: ITE encoding — ite(c, a, b) with c true forces output=a. */
static void test_ite(void) {
    zsp_aig_t *aig = zsp_aig_new(NULL);
    zsp_sat_t *sat = zsp_sat_new(NULL);
    zsp_aig_cnf_t *enc = zsp_aig_cnf_new(NULL, aig, sat);

    zsp_aig_node_t c = zsp_aig_mk_input(aig);
    zsp_aig_node_t a = zsp_aig_mk_input(aig);
    zsp_aig_node_t b = zsp_aig_mk_input(aig);

    /* (c) /\ ite(c, a, b) /\ ~a  should be UNSAT (c true ⇒ ite = a; ~a ⇒ contradiction) */
    zsp_aig_node_t ite = zsp_aig_mk_ite(aig, c, a, b);
    zsp_aig_node_t goal = zsp_aig_mk_and(aig, c, zsp_aig_mk_and(aig, ite, zsp_aig_not(a)));

    zsp_aig_cnf_encode(enc, goal, /*top_level=*/1);
    int rc = zsp_sat_solve(sat);
    CHECK(rc == ZSP_SAT_UNSAT, "c /\\ ite(c,a,b) /\\ ~a is UNSAT");

    zsp_aig_cnf_free(enc);
    zsp_sat_free(sat);
    zsp_aig_free(aig);
}

int main(void) {
    test_and();
    test_contradiction();
    test_xor();
    test_ite();
    return failures ? 1 : 0;
}
