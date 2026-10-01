/* Tests for the dvs_aig manager — verifies the Brummayer/Biere
 * rewriting rules and hash-consing. */
#include <stdio.h>
#include <stdlib.h>

#include "dvs_aig.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
    else         { printf("PASS %s\n", msg); } \
} while (0)

int main(void) {
    dvs_aig_t *m = dvs_aig_new(NULL);

    /* Constants */
    CHECK(dvs_aig_true()  == DVS_AIG_TRUE,  "true const");
    CHECK(dvs_aig_false() == DVS_AIG_FALSE, "false const");
    CHECK(dvs_aig_not(dvs_aig_true()) == DVS_AIG_FALSE, "not true = false");

    dvs_aig_node_t a = dvs_aig_mk_input(m);
    dvs_aig_node_t b = dvs_aig_mk_input(m);
    dvs_aig_node_t c = dvs_aig_mk_input(m);

    /* Level 1 rules */
    CHECK(dvs_aig_mk_and(m, a, DVS_AIG_TRUE)  == a,            "a /\\ T = a");
    CHECK(dvs_aig_mk_and(m, DVS_AIG_TRUE, a)  == a,            "T /\\ a = a");
    CHECK(dvs_aig_mk_and(m, a, DVS_AIG_FALSE) == DVS_AIG_FALSE, "a /\\ F = F");
    CHECK(dvs_aig_mk_and(m, a, a) == a,                         "a /\\ a = a (idempotence)");
    CHECK(dvs_aig_mk_and(m, a, -a) == DVS_AIG_FALSE,            "a /\\ ~a = F (contradiction)");

    /* Hash-consing */
    dvs_aig_node_t ab1 = dvs_aig_mk_and(m, a, b);
    dvs_aig_node_t ab2 = dvs_aig_mk_and(m, a, b);
    dvs_aig_node_t ba  = dvs_aig_mk_and(m, b, a);  /* commutativity via normalization */
    CHECK(ab1 == ab2, "a /\\ b consed");
    CHECK(ab1 == ba,  "b /\\ a normalized to a /\\ b");

    /* Level 2 contradiction (asymmetric): (a/\b) /\ ~a = F */
    dvs_aig_node_t ab = dvs_aig_mk_and(m, a, b);
    CHECK(dvs_aig_mk_and(m, ab, -a) == DVS_AIG_FALSE, "(a/\\b) /\\ ~a = F");
    CHECK(dvs_aig_mk_and(m, ab, -b) == DVS_AIG_FALSE, "(a/\\b) /\\ ~b = F");

    /* Level 2 subsumption (asymmetric): ~(a/\b) /\ ~a = ~a */
    CHECK(dvs_aig_mk_and(m, -ab, -a) == -a, "~(a/\\b) /\\ ~a = ~a");

    /* Level 2 idempotence: (a/\b) /\ a = (a/\b) */
    CHECK(dvs_aig_mk_and(m, ab, a) == ab, "(a/\\b) /\\ a = (a/\\b)");

    /* XOR / IFF round-trip on constants */
    CHECK(dvs_aig_mk_xor(m, a, DVS_AIG_FALSE) == a,                "a XOR F = a");
    CHECK(dvs_aig_mk_xor(m, a, DVS_AIG_TRUE)  == dvs_aig_not(a),   "a XOR T = ~a");
    CHECK(dvs_aig_mk_iff(m, a, a) == DVS_AIG_TRUE,                 "a IFF a = T");

    /* ITE simplifications */
    CHECK(dvs_aig_mk_ite(m, DVS_AIG_TRUE,  a, b) == a, "ITE(T,a,b) = a");
    CHECK(dvs_aig_mk_ite(m, DVS_AIG_FALSE, a, b) == b, "ITE(F,a,b) = b");
    CHECK(dvs_aig_mk_ite(m, c, a, a)             == a, "ITE(c,a,a) = a");

    /* Stats sanity */
    printf("stats: nodes=%llu ands=%llu inputs=%llu shared=%llu\n",
           (unsigned long long)dvs_aig_num_nodes(m),
           (unsigned long long)dvs_aig_num_ands(m),
           (unsigned long long)dvs_aig_num_inputs(m),
           (unsigned long long)dvs_aig_num_shared(m));
    CHECK(dvs_aig_num_inputs(m) == 3, "3 inputs");
    CHECK(dvs_aig_num_shared(m) >= 1, "hash-consing fired at least once");

    dvs_aig_free(m);
    return failures ? 1 : 0;
}
