/* Typed operators (DVS_BIN_ULT .. DVS_BIN_LSHR) on bit-blast, in an EXPLICIT
 * problem, the form the SMT-LIB front end builds (SV elaboration lowers them
 * otherwise, so the builder door never shows them to bit-blast).
 *
 * `c == a op k` with `a`'s sign bit set (by `(a LSHR (w-1)) == 1`) and
 * `c != 0`: bit-blast encodes the typed operator, and every model is checked
 * here against SMT-LIB's semantics, at 8 and 64 bits, for a positive and a
 * negative divisor. A compare is asserted directly: `a op k`. With
 * DV_BB_STATS=1 the engine also re-evaluates each assert on the model with
 * bb_eval and prints `[bb-eval] mismatch=0 ... declined=0`.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "dvs_bbsolver.h"
#include "dvs_builder.h"
#include "dvs_problem.h"

#define DRAWS 30

static int failures = 0;

static uint64_t mask(uint32_t w) { return w >= 64 ? ~0ull : (1ull << w) - 1; }
static int neg(uint64_t x, uint32_t w) { return (int)((x >> (w - 1)) & 1); }
static uint64_t sneg(uint64_t x, uint32_t w) { return (0 - x) & mask(w); }

/* SMT-LIB semantics of `op` on w-bit patterns. */
static uint64_t ref(int op, uint64_t x, uint64_t y, uint32_t w) {
    uint64_t M = mask(w), sb = 1ull << (w - 1);
    int xn = neg(x, w), yn = neg(y, w);
    uint64_t ax = xn ? sneg(x, w) : x, ay = yn ? sneg(y, w) : y;
    switch (op) {
    case DVS_BIN_ULT: return x < y;
    case DVS_BIN_ULE: return x <= y;
    case DVS_BIN_UGT: return x > y;
    case DVS_BIN_UGE: return x >= y;
    case DVS_BIN_SLT: return (x ^ sb) < (y ^ sb);
    case DVS_BIN_SLE: return (x ^ sb) <= (y ^ sb);
    case DVS_BIN_SGT: return (x ^ sb) > (y ^ sb);
    case DVS_BIN_SGE: return (x ^ sb) >= (y ^ sb);
    case DVS_BIN_UDIV: return y ? x / y : M;
    case DVS_BIN_UREM: return y ? x % y : x;
    case DVS_BIN_LSHR: return y >= w ? 0 : x >> y;
    case DVS_BIN_SDIV: {
        uint64_t q = ay ? ax / ay : M;
        return xn != yn ? sneg(q, w) : q;
    }
    default: {                                   /* SREM, SMOD */
        uint64_t m = ay ? ax % ay : ax;
        uint64_t r = xn ? sneg(m, w) : m;
        if (op == DVS_BIN_SMOD && r && neg(r, w) != yn) r = (r + y) & M;
        return r;
    }
    }
}

static int is_cmp(int op) { return op >= DVS_BIN_ULT && op <= DVS_BIN_SGE; }

static void run(int op, uint32_t w, int64_t kv) {
    uint64_t k = (uint64_t)kv & mask(w);
    dvs_builder_t *b = dvs_builder_create(4096, NULL);
    dvs_builder_add_var(b, 0, w, 0, 0, w == 64 ? -1 : (int64_t)mask(w));   /* a */
    dvs_builder_add_var(b, 1, w, 0, 0, w == 64 ? -1 : (int64_t)mask(w));   /* c */
    dvs_expr_t a = dvs_builder_expr_var(b, 0), c = dvs_builder_expr_var(b, 1);
    dvs_expr_t K = dvs_builder_expr_const_sized(b, (int64_t)k, 0, w);
    dvs_expr_t top = dvs_builder_expr_binary(b, DVS_BIN_LSHR, a,
                         dvs_builder_expr_const_sized(b, w - 1, 0, w));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ, top,
                                  dvs_builder_expr_const_sized(b, 1, 0, w)));
    if (is_cmp(op)) {
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, op, a, K));
    } else {
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ, c,
                                      dvs_builder_expr_binary(b, op, a, K)));
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ, c,
                                      dvs_builder_expr_const_sized(b, 0, 0, w)));
    }
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    p->flags |= DVS_PROBLEM_F_EXPLICIT;          /* as the SMT-LIB front end */
    dvs_bbsolver_t *S = dvs_bbsolver_new_backend(NULL, p, 1);
    if (S) dvs_bbsolver_set_resample(S, 1);
    int bad = 0, nsat = 0;
    for (uint64_t seed = 1; seed <= DRAWS && S; seed++) {
        int rc = seed == 1 ? dvs_bbsolver_check(S, seed)
                           : dvs_bbsolver_resample(S, seed) == 0 ? DVS_BB_SAT : -1;
        if (rc != DVS_BB_SAT) {
            fprintf(stderr, "  op %d w %u k %" PRId64 " seed %" PRIu64 ": rc %d\n", op, w, kv, seed, rc);
            bad++;
            break;
        }
        nsat++;
        int64_t av = 0, cv = 0;
        dvs_bbsolver_value(S, 0, &av);
        dvs_bbsolver_value(S, 1, &cv);
        uint64_t x = (uint64_t)av & mask(w), r = ref(op, x, k, w);
        int ok = neg(x, w) && (is_cmp(op) ? r == 1 : (((uint64_t)cv & mask(w)) == r && r != 0));
        if (!ok) {
            fprintf(stderr, "  op %d w %u k %" PRId64 ": a=%#" PRIx64 " c=%#" PRIx64 " want c=%#" PRIx64 "\n",
                    op, w, kv, x, (uint64_t)cv & mask(w), r);
            bad++;
        }
    }
    if (!S) bad++;
    char msg[96];
    snprintf(msg, sizeof msg, "op %d w %u k %" PRId64 ": %d models", op, w, kv, nsat);
    if (bad) { fprintf(stderr, "FAIL %s\n", msg); failures++; }
    else printf("PASS %s\n", msg);
    dvs_bbsolver_free(S);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

int main(void) {
    static const int ops[] = {
        DVS_BIN_UDIV, DVS_BIN_UREM, DVS_BIN_SDIV, DVS_BIN_SREM, DVS_BIN_SMOD, DVS_BIN_LSHR,
        DVS_BIN_ULT, DVS_BIN_ULE, DVS_BIN_UGT, DVS_BIN_UGE,
        DVS_BIN_SLT, DVS_BIN_SLE, DVS_BIN_SGT, DVS_BIN_SGE,
    };
    static const uint32_t widths[] = { 8, 64 };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        for (size_t j = 0; j < 2; j++) {
            if (ops[i] == DVS_BIN_LSHR) { run(ops[i], widths[j], 3); continue; }
            /* A compare of a negative `a` against k: SLT 7 / ULT -5 hold,
             * so each asserts the one that does. */
            if (is_cmp(ops[i])) {
                int lt = ops[i] == DVS_BIN_ULT || ops[i] == DVS_BIN_ULE
                      || ops[i] == DVS_BIN_SLT || ops[i] == DVS_BIN_SLE;
                int sg = ops[i] >= DVS_BIN_SLT;
                run(ops[i], widths[j], (lt == sg) ? 7 : -5);
                continue;
            }
            run(ops[i], widths[j], 7);
            run(ops[i], widths[j], -5);
        }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
