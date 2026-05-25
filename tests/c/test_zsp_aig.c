/* Tests for the zsp_aig manager — verifies the Brummayer/Biere
 * rewriting rules and hash-consing. */
#include <stdio.h>
#include <stdlib.h>

#include "zsp_aig.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
    else         { printf("PASS %s\n", msg); } \
} while (0)

int main(void) {
    zsp_aig_t *m = zsp_aig_new(NULL);

    /* Constants */
    CHECK(zsp_aig_true()  == ZSP_AIG_TRUE,  "true const");
    CHECK(zsp_aig_false() == ZSP_AIG_FALSE, "false const");
    CHECK(zsp_aig_not(zsp_aig_true()) == ZSP_AIG_FALSE, "not true = false");

    zsp_aig_node_t a = zsp_aig_mk_input(m);
    zsp_aig_node_t b = zsp_aig_mk_input(m);
    zsp_aig_node_t c = zsp_aig_mk_input(m);

    /* Level 1 rules */
    CHECK(zsp_aig_mk_and(m, a, ZSP_AIG_TRUE)  == a,            "a /\\ T = a");
    CHECK(zsp_aig_mk_and(m, ZSP_AIG_TRUE, a)  == a,            "T /\\ a = a");
    CHECK(zsp_aig_mk_and(m, a, ZSP_AIG_FALSE) == ZSP_AIG_FALSE, "a /\\ F = F");
    CHECK(zsp_aig_mk_and(m, a, a) == a,                         "a /\\ a = a (idempotence)");
    CHECK(zsp_aig_mk_and(m, a, -a) == ZSP_AIG_FALSE,            "a /\\ ~a = F (contradiction)");

    /* Hash-consing */
    zsp_aig_node_t ab1 = zsp_aig_mk_and(m, a, b);
    zsp_aig_node_t ab2 = zsp_aig_mk_and(m, a, b);
    zsp_aig_node_t ba  = zsp_aig_mk_and(m, b, a);  /* commutativity via normalization */
    CHECK(ab1 == ab2, "a /\\ b consed");
    CHECK(ab1 == ba,  "b /\\ a normalized to a /\\ b");

    /* Level 2 contradiction (asymmetric): (a/\b) /\ ~a = F */
    zsp_aig_node_t ab = zsp_aig_mk_and(m, a, b);
    CHECK(zsp_aig_mk_and(m, ab, -a) == ZSP_AIG_FALSE, "(a/\\b) /\\ ~a = F");
    CHECK(zsp_aig_mk_and(m, ab, -b) == ZSP_AIG_FALSE, "(a/\\b) /\\ ~b = F");

    /* Level 2 subsumption (asymmetric): ~(a/\b) /\ ~a = ~a */
    CHECK(zsp_aig_mk_and(m, -ab, -a) == -a, "~(a/\\b) /\\ ~a = ~a");

    /* Level 2 idempotence: (a/\b) /\ a = (a/\b) */
    CHECK(zsp_aig_mk_and(m, ab, a) == ab, "(a/\\b) /\\ a = (a/\\b)");

    /* XOR / IFF round-trip on constants */
    CHECK(zsp_aig_mk_xor(m, a, ZSP_AIG_FALSE) == a,                "a XOR F = a");
    CHECK(zsp_aig_mk_xor(m, a, ZSP_AIG_TRUE)  == zsp_aig_not(a),   "a XOR T = ~a");
    CHECK(zsp_aig_mk_iff(m, a, a) == ZSP_AIG_TRUE,                 "a IFF a = T");

    /* ITE simplifications */
    CHECK(zsp_aig_mk_ite(m, ZSP_AIG_TRUE,  a, b) == a, "ITE(T,a,b) = a");
    CHECK(zsp_aig_mk_ite(m, ZSP_AIG_FALSE, a, b) == b, "ITE(F,a,b) = b");
    CHECK(zsp_aig_mk_ite(m, c, a, a)             == a, "ITE(c,a,a) = a");

    /* Stats sanity */
    printf("stats: nodes=%llu ands=%llu inputs=%llu shared=%llu\n",
           (unsigned long long)zsp_aig_num_nodes(m),
           (unsigned long long)zsp_aig_num_ands(m),
           (unsigned long long)zsp_aig_num_inputs(m),
           (unsigned long long)zsp_aig_num_shared(m));
    CHECK(zsp_aig_num_inputs(m) == 3, "3 inputs");
    CHECK(zsp_aig_num_shared(m) >= 1, "hash-consing fired at least once");

    zsp_aig_free(m);
    return failures ? 1 : 0;
}
