/* Typed operators (DVS_BIN_ULT .. DVS_BIN_LSHR) on CDCL, in an EXPLICIT
 * problem, the form the SMT-LIB front end builds. CDCL compiles only the SV
 * operators, so compile lowers them first (dvs_sv_lower_typed), the way SV
 * elaboration does for the builder door.
 *
 * The problems are test_bb_typed.c's: `c == a op k` with `a`'s sign bit set
 * and `c != 0`, or `a op k` for a compare. The context is set to CDCL, so an
 * incomplete compile is a failure, not a fall-back to bit-blast. Every model
 * is checked here against SMT-LIB's semantics and by validate_model. Widths 8
 * and 62: CDCL holds a domain as an int64 interval, and an unsigned 63-64-bit
 * value is design step 4's later work (docs/signed_unsigned_ops_design.md);
 * at 64 bits this test only checks that each answer is right or declined.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_builder.h"
#include "dvs_problem.h"

#define DRAWS 30

static int failures = 0;

static uint64_t mask(uint32_t w) { return w >= 64 ? ~0ull : (1ull << w) - 1; }
static int neg(uint64_t x, uint32_t w) { return (int)((x >> (w - 1)) & 1); }
static uint64_t sneg(uint64_t x, uint32_t w) { return (0 - x) & mask(w); }
static int64_t sval(uint64_t x, uint32_t w) {
    return neg(x, w) ? -(int64_t)sneg(x, w) : (int64_t)x;
}

/* SMT-LIB's value of `x op y` at w bits, as a w-bit pattern. */
static uint64_t ref(int op, uint64_t x, uint64_t y, uint32_t w) {
    uint64_t M = mask(w);
    int64_t sx = sval(x, w), sy = sval(y, w);
    switch (op) {
    case DVS_BIN_ULT: return x <  y;
    case DVS_BIN_ULE: return x <= y;
    case DVS_BIN_UGT: return x >  y;
    case DVS_BIN_UGE: return x >= y;
    case DVS_BIN_SLT: return sx <  sy;
    case DVS_BIN_SLE: return sx <= sy;
    case DVS_BIN_SGT: return sx >  sy;
    case DVS_BIN_SGE: return sx >= sy;
    case DVS_BIN_UDIV: return y ? x / y : M;
    case DVS_BIN_UREM: return y ? x % y : x;
    case DVS_BIN_LSHR: return y >= w ? 0 : x >> y;
    default: break;
    }
    /* Signed division on the magnitudes, as SMT-LIB defines it. */
    uint64_t ax = neg(x, w) ? sneg(x, w) : x, ay = neg(y, w) ? sneg(y, w) : y;
    if (op == DVS_BIN_SDIV) {
        if (y == 0) return neg(x, w) ? 1 : M;
        uint64_t q = ax / ay;
        return neg(x, w) != neg(y, w) ? sneg(q, w) : q;
    }
    uint64_t r = y ? ax % ay : ax;
    uint64_t rr = neg(x, w) ? sneg(r, w) : r;        /* SREM: the dividend's sign */
    if (op == DVS_BIN_SREM || y == 0 || r == 0) return rr;
    /* SMOD: the divisor's sign. */
    if (neg(x, w) == neg(y, w)) return rr;
    return (rr + y) & M;
}

static int is_cmp(int op) { return op >= DVS_BIN_ULT && op <= DVS_BIN_SGE; }

static dvs_solve_opts_t opts(uint64_t seed) {
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    o.seed = seed;
    o.fair_pick = 1;
    return o;
}

/* `strict`: compile must be complete. Otherwise a decline passes, and only
 * the answers given are checked. */
static void run(int op, uint32_t w, int64_t kv, int strict) {
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

    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    dvs_solver_set_engine(ctx, DVS_ENGINE_CDCL);
    int crc = dvs_solver_compile(ctx, p);
    int bad = 0, nsat = 0;
    if (crc != DVS_COMPILE_OK) {
        if (strict) {
            fprintf(stderr, "  op %d w %u k %" PRId64 ": compile rc %d\n", op, w, kv, crc);
            bad++;
        }
    } else {
        for (uint64_t seed = 1; seed <= DRAWS; seed++) {
            dvs_solver_reset(ctx);
            dvs_solve_opts_t o = opts(seed);
            int rc = dvs_solver_solve(ctx, &o);
            if (rc != DVS_SOLVE_OK) {
                fprintf(stderr, "  op %d w %u k %" PRId64 " seed %" PRIu64 ": rc %d\n",
                        op, w, kv, seed, rc);
                bad++;
                break;
            }
            nsat++;
            uint64_t x = (uint64_t)dvs_solver_get_value(ctx, 0) & mask(w);
            uint64_t cv = (uint64_t)dvs_solver_get_value(ctx, 1) & mask(w);
            uint64_t r = ref(op, x, k, w);
            int ok = neg(x, w) && (is_cmp(op) ? r == 1 : (cv == r && r != 0));
            if (!ok || dvs_solver_validate_model(ctx, p, NULL) != 0) {
                fprintf(stderr, "  op %d w %u k %" PRId64 ": a=%#" PRIx64 " c=%#" PRIx64
                        " want c=%#" PRIx64 "\n", op, w, kv, x, cv, r);
                bad++;
            }
        }
    }
    char msg[96];
    snprintf(msg, sizeof msg, "op %d w %u k %" PRId64 ": %s, %d models", op, w, kv,
             crc == DVS_COMPILE_OK ? "compiled" : "declined", nsat);
    if (bad) { fprintf(stderr, "FAIL %s\n", msg); failures++; }
    else printf("PASS %s\n", msg);
    dvs_solver_free(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

int main(void) {
    static const int ops[] = {
        DVS_BIN_UDIV, DVS_BIN_UREM, DVS_BIN_SDIV, DVS_BIN_SREM, DVS_BIN_SMOD, DVS_BIN_LSHR,
        DVS_BIN_ULT, DVS_BIN_ULE, DVS_BIN_UGT, DVS_BIN_UGE,
        DVS_BIN_SLT, DVS_BIN_SLE, DVS_BIN_SGT, DVS_BIN_SGE,
    };
    static const uint32_t widths[] = { 8, 62, 64 };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        for (size_t j = 0; j < 3; j++) {
            uint32_t w = widths[j];
            int strict = w < 63;
            if (ops[i] == DVS_BIN_LSHR) { run(ops[i], w, 3, strict); continue; }
            if (is_cmp(ops[i])) {
                int lt = ops[i] == DVS_BIN_ULT || ops[i] == DVS_BIN_ULE
                      || ops[i] == DVS_BIN_SLT || ops[i] == DVS_BIN_SLE;
                int sg = ops[i] >= DVS_BIN_SLT;
                run(ops[i], w, (lt == sg) ? 7 : -5, strict);
                continue;
            }
            run(ops[i], w, 7, strict);
            run(ops[i], w, -5, strict);
        }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
