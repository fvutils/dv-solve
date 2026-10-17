/* End-to-end test: build an AIG, Tseitin-encode it, solve via kissat,
 * verify the model satisfies the original constraints. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvs_alloc.h"

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

/* Allocator that counts bytes, to check every release passes the size that
 * was allocated. */
typedef struct {
    dvs_alloc_t base;
    size_t      bytes_alloced;
    size_t      bytes_freed;
} counting_alloc_t;

static void *count_alloc(dvs_alloc_t *self, size_t sz) {
    ((counting_alloc_t *)self)->bytes_alloced += sz;
    return malloc(sz);
}
static void count_release(dvs_alloc_t *self, void *p, size_t sz) {
    ((counting_alloc_t *)self)->bytes_freed += sz;
    free(p);
}

/* Test 5: many top-level encodes over a growing AIG, then free: the encoder's
 * bytes balance. */
static void test_alloc_balance(void) {
    counting_alloc_t ca;
    memset(&ca, 0, sizeof(ca));
    ca.base.alloc = count_alloc;
    ca.base.release = count_release;

    dvs_aig_t *aig = dvs_aig_new(NULL);
    dvs_sat_t *sat = dvs_sat_new(NULL);
    dvs_aig_cnf_t *enc = dvs_aig_cnf_new(&ca.base, aig, sat);

    dvs_aig_node_t acc = dvs_aig_mk_input(aig);
    for (int i = 0; i < 2000; i++) {
        dvs_aig_node_t x = dvs_aig_mk_input(aig);
        dvs_aig_node_t y = dvs_aig_mk_input(aig);
        acc = dvs_aig_mk_and(aig, acc, dvs_aig_mk_or(aig, x, y));
        if (i % 100 == 0) dvs_aig_cnf_encode(enc, acc, /*top_level=*/1);
    }
    CHECK(dvs_sat_solve(sat) == DVS_SAT_SAT, "growing conjunction is SAT");

    dvs_aig_cnf_free(enc);
    CHECK(ca.bytes_alloced == ca.bytes_freed, "encoder bytes balance after free");
    if (ca.bytes_alloced != ca.bytes_freed)
        fprintf(stderr, "  alloced %zu freed %zu\n", ca.bytes_alloced, ca.bytes_freed);
    dvs_sat_free(sat);
    dvs_aig_free(aig);
}

/* Test 6: a node visited in one top-level pass is asserted again by a later
 * pass. The flatten's visit marks must not leak from pass to pass. */
static void test_marks_per_pass(void) {
    dvs_aig_t *aig = dvs_aig_new(NULL);
    dvs_sat_t *sat = dvs_sat_new(NULL);
    dvs_aig_cnf_t *enc = dvs_aig_cnf_new(NULL, aig, sat);

    dvs_aig_node_t a = dvs_aig_mk_input(aig);
    dvs_aig_node_t b = dvs_aig_mk_input(aig);
    dvs_aig_node_t c = dvs_aig_mk_input(aig);
    dvs_aig_node_t ab = dvs_aig_mk_and(aig, a, b);

    dvs_aig_cnf_encode(enc, ab, 1);
    uint64_t n1 = dvs_aig_cnf_num_clauses(enc);
    dvs_aig_cnf_encode(enc, ab, 1);
    uint64_t n2 = dvs_aig_cnf_num_clauses(enc);
    /* Each pass asserts the two leaves a and b as units. */
    CHECK(n2 - n1 == 2, "second pass over the same AND asserts its leaves again");

    /* Pass 3 shares a with pass 1 in the other polarity: UNSAT. */
    dvs_aig_cnf_encode(enc, dvs_aig_mk_and(aig, dvs_aig_not(a), c), 1);
    CHECK(dvs_sat_solve(sat) == DVS_SAT_UNSAT, "a /\\ b, then ~a /\\ c is UNSAT");

    dvs_aig_cnf_free(enc);
    dvs_sat_free(sat);
    dvs_aig_free(aig);
}

int main(void) {
    test_and();
    test_contradiction();
    test_xor();
    test_ite();
    test_alloc_balance();
    test_marks_per_pass();
    return failures ? 1 : 0;
}
