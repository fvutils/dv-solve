/* End-to-end test of the BV bit-blaster. For each operator, build a
 * small SMT-like constraint, bit-blast to AIG, CNF-encode, kissat-solve,
 * and check the model. */
#include <stdio.h>
#include <stdlib.h>

#include "zsp_aig.h"
#include "zsp_aig_cnf.h"
#include "zsp_bitblast.h"
#include "zsp_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

typedef struct {
    zsp_aig_t      *aig;
    zsp_sat_t      *sat;
    zsp_aig_cnf_t  *cnf;
    zsp_bitblast_t *bb;
} ctx_t;

static ctx_t ctx_new(void) {
    ctx_t c;
    c.aig = zsp_aig_new(NULL);
    c.sat = zsp_sat_new(NULL);
    c.cnf = zsp_aig_cnf_new(NULL, c.aig, c.sat);
    c.bb  = zsp_bitblast_new(NULL, c.aig);
    return c;
}
static void ctx_free(ctx_t *c) {
    zsp_bitblast_free(c->bb);
    zsp_aig_cnf_free(c->cnf);
    zsp_sat_free(c->sat);
    zsp_aig_free(c->aig);
}

/* Helper: read a uint64 model of `bv` from the SAT solver. Assumes
 * bv.size <= 64. */
static uint64_t model_u64(zsp_aig_cnf_t *enc, zsp_bv_t bv) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < bv.size; i++) {
        int b = zsp_aig_cnf_value(enc, bv.bits[i]);
        if (b == 1) v |= (uint64_t)1 << (bv.size - 1 - i);
    }
    return v;
}

/* Assert that `pred` (a 1-bit bv) is true, solve, and check SAT. */
static int solve_assert(ctx_t *c, zsp_bv_t pred) {
    zsp_aig_cnf_encode(c->cnf, pred.bits[0], /*top_level=*/1);
    return zsp_sat_solve(c->sat);
}

/* Bind two free 8-bit constants a, b with a==<va> and b==<vb>, return SAT
 * result of an asserted equation `lhs == rhs`. */
static void test_add(void) {
    ctx_t c = ctx_new();
    zsp_bv_t a = zsp_bb_constant(c.bb, 8);
    zsp_bv_t b = zsp_bb_constant(c.bb, 8);
    /* assert a = 7 and b = 35 and a+b = result, return result */
    zsp_bv_t v7  = zsp_bb_value_u64(c.bb, 8, 7);
    zsp_bv_t v35 = zsp_bb_value_u64(c.bb, 8, 35);
    zsp_bv_t v42 = zsp_bb_value_u64(c.bb, 8, 42);

    zsp_aig_cnf_encode(c.cnf, zsp_bb_eq(c.bb, a, v7).bits[0], 1);
    zsp_aig_cnf_encode(c.cnf, zsp_bb_eq(c.bb, b, v35).bits[0], 1);
    zsp_bv_t sum = zsp_bb_add(c.bb, a, b);
    zsp_aig_cnf_encode(c.cnf, zsp_bb_eq(c.bb, sum, v42).bits[0], 1);

    CHECK(zsp_sat_solve(c.sat) == ZSP_SAT_SAT, "7+35==42 SAT");
    ctx_free(&c);
}

static void test_add_overflow(void) {
    ctx_t c = ctx_new();
    zsp_bv_t a = zsp_bb_value_u64(c.bb, 8, 200);
    zsp_bv_t b = zsp_bb_value_u64(c.bb, 8, 100);
    zsp_bv_t expected = zsp_bb_value_u64(c.bb, 8, (200 + 100) & 0xff);  /* 44 */
    zsp_bv_t sum = zsp_bb_add(c.bb, a, b);
    CHECK(solve_assert(&c, zsp_bb_eq(c.bb, sum, expected)) == ZSP_SAT_SAT,
          "200+100 mod 256 == 44");
    ctx_free(&c);
}

static void test_mul(void) {
    ctx_t c = ctx_new();
    zsp_bv_t a = zsp_bb_value_u64(c.bb, 8, 7);
    zsp_bv_t b = zsp_bb_value_u64(c.bb, 8, 6);
    zsp_bv_t e = zsp_bb_value_u64(c.bb, 8, 42);
    zsp_bv_t p = zsp_bb_mul(c.bb, a, b);
    CHECK(solve_assert(&c, zsp_bb_eq(c.bb, p, e)) == ZSP_SAT_SAT, "7*6 == 42");
    ctx_free(&c);
}

static void test_mul_solve(void) {
    /* Find a, b such that a*b = 35 and a < 10 and b < 10. Should yield 5,7 or 7,5. */
    ctx_t c = ctx_new();
    zsp_bv_t a = zsp_bb_constant(c.bb, 8);
    zsp_bv_t b = zsp_bb_constant(c.bb, 8);
    zsp_bv_t v35 = zsp_bb_value_u64(c.bb, 8, 35);
    zsp_bv_t v10 = zsp_bb_value_u64(c.bb, 8, 10);
    zsp_bv_t v0  = zsp_bb_value_u64(c.bb, 8, 0);

    zsp_aig_cnf_encode(c.cnf, zsp_bb_eq(c.bb, zsp_bb_mul(c.bb,a,b), v35).bits[0], 1);
    zsp_aig_cnf_encode(c.cnf, zsp_bb_ult(c.bb, a, v10).bits[0], 1);
    zsp_aig_cnf_encode(c.cnf, zsp_bb_ult(c.bb, b, v10).bits[0], 1);
    zsp_aig_cnf_encode(c.cnf, zsp_bb_ult(c.bb, v0, a).bits[0], 1);   /* a > 0 */
    zsp_aig_cnf_encode(c.cnf, zsp_bb_ult(c.bb, v0, b).bits[0], 1);   /* b > 0 */
    int rc = zsp_sat_solve(c.sat);
    CHECK(rc == ZSP_SAT_SAT, "factor 35 in [1,10)x[1,10)");
    if (rc == ZSP_SAT_SAT) {
        uint64_t va = model_u64(c.cnf, a);
        uint64_t vb = model_u64(c.cnf, b);
        printf("       model: a=%llu b=%llu\n", (unsigned long long)va, (unsigned long long)vb);
        CHECK(va * vb == 35, "model satisfies a*b=35");
    }
    ctx_free(&c);
}

static void test_ult(void) {
    /* 0 <_u 1 SAT, 5 <_u 5 UNSAT, 255 <_u 0 UNSAT (unsigned).
     * zsp_bv_t values carry pointers owned by their originating bb
     * context — they MUST be recreated in each fresh context. */
    ctx_t c = ctx_new();
    {
        zsp_bv_t v0 = zsp_bb_value_u64(c.bb, 8, 0);
        zsp_bv_t v1 = zsp_bb_value_u64(c.bb, 8, 1);
        CHECK(solve_assert(&c, zsp_bb_ult(c.bb, v0, v1)) == ZSP_SAT_SAT, "0 < 1");
    }
    ctx_free(&c);

    c = ctx_new();
    {
        zsp_bv_t v5 = zsp_bb_value_u64(c.bb, 8, 5);
        CHECK(solve_assert(&c, zsp_bb_ult(c.bb, v5, v5)) == ZSP_SAT_UNSAT, "~(5 < 5)");
    }
    ctx_free(&c);

    c = ctx_new();
    {
        zsp_bv_t v0   = zsp_bb_value_u64(c.bb, 8, 0);
        zsp_bv_t v255 = zsp_bb_value_u64(c.bb, 8, 255);
        CHECK(solve_assert(&c, zsp_bb_ult(c.bb, v255, v0)) == ZSP_SAT_UNSAT, "~(255 <_u 0)");
    }
    ctx_free(&c);
}

static void test_slt(void) {
    /* signed: -1 (=255) <_s 0 SAT; 1 <_s -1 UNSAT.
     * bv handles do not survive ctx_free — recreate per context. */
    ctx_t c = ctx_new();
    {
        zsp_bv_t v0  = zsp_bb_value_u64(c.bb, 8, 0);
        zsp_bv_t vN1 = zsp_bb_value_u64(c.bb, 8, 255);  /* -1 */
        CHECK(solve_assert(&c, zsp_bb_slt(c.bb, vN1, v0)) == ZSP_SAT_SAT, "-1 <_s 0");
    }
    ctx_free(&c);

    c = ctx_new();
    {
        zsp_bv_t vN1 = zsp_bb_value_u64(c.bb, 8, 255);  /* -1 */
        zsp_bv_t v1  = zsp_bb_value_u64(c.bb, 8, 1);
        CHECK(solve_assert(&c, zsp_bb_slt(c.bb, v1, vN1)) == ZSP_SAT_UNSAT, "~(1 <_s -1)");
    }
    ctx_free(&c);
}

static void test_shl(void) {
    ctx_t c = ctx_new();
    /* 1 << 3 = 8 */
    zsp_bv_t v1 = zsp_bb_value_u64(c.bb, 8, 1);
    zsp_bv_t v3 = zsp_bb_value_u64(c.bb, 8, 3);
    zsp_bv_t v8 = zsp_bb_value_u64(c.bb, 8, 8);
    CHECK(solve_assert(&c, zsp_bb_eq(c.bb, zsp_bb_shl(c.bb, v1, v3), v8)) == ZSP_SAT_SAT,
          "1 << 3 == 8");
    ctx_free(&c);
}

static void test_shr(void) {
    ctx_t c = ctx_new();
    /* 16 >> 2 = 4 */
    zsp_bv_t v16 = zsp_bb_value_u64(c.bb, 8, 16);
    zsp_bv_t v2  = zsp_bb_value_u64(c.bb, 8, 2);
    zsp_bv_t v4  = zsp_bb_value_u64(c.bb, 8, 4);
    CHECK(solve_assert(&c, zsp_bb_eq(c.bb, zsp_bb_shr(c.bb, v16, v2), v4)) == ZSP_SAT_SAT,
          "16 >> 2 == 4");
    ctx_free(&c);
}

static void test_extract_concat(void) {
    ctx_t c = ctx_new();
    /* extract bits [3:0] of 0xa5 = 5 */
    zsp_bv_t v   = zsp_bb_value_u64(c.bb, 8, 0xa5);
    zsp_bv_t lo  = zsp_bb_extract(c.bb, v, 3, 0);
    zsp_bv_t v5  = zsp_bb_value_u64(c.bb, 4, 5);
    CHECK(solve_assert(&c, zsp_bb_eq(c.bb, lo, v5)) == ZSP_SAT_SAT,
          "extract 0xa5[3:0] == 5");
    ctx_free(&c);

    c = ctx_new();
    /* concat 4-bit 0x5 with 4-bit 0xa = 8-bit 0x5a */
    zsp_bv_t hi5 = zsp_bb_value_u64(c.bb, 4, 5);
    zsp_bv_t loA = zsp_bb_value_u64(c.bb, 4, 0xa);
    zsp_bv_t v5a = zsp_bb_value_u64(c.bb, 8, 0x5a);
    CHECK(solve_assert(&c, zsp_bb_eq(c.bb, zsp_bb_concat(c.bb, hi5, loA), v5a)) == ZSP_SAT_SAT,
          "concat(5_4, a_4) == 0x5a");
    ctx_free(&c);
}

static void test_udiv_urem(void) {
    ctx_t c = ctx_new();
    /* 13 udiv 4 = 3 */
    {
        zsp_bv_t v13 = zsp_bb_value_u64(c.bb, 8, 13);
        zsp_bv_t v4  = zsp_bb_value_u64(c.bb, 8, 4);
        zsp_bv_t v3  = zsp_bb_value_u64(c.bb, 8, 3);
        CHECK(solve_assert(&c, zsp_bb_eq(c.bb, zsp_bb_udiv(c.bb,v13,v4), v3)) == ZSP_SAT_SAT,
              "13 udiv 4 == 3");
    }
    ctx_free(&c);

    /* 13 urem 4 = 1 — fresh context (bv handles do not survive ctx_free) */
    c = ctx_new();
    {
        zsp_bv_t v13 = zsp_bb_value_u64(c.bb, 8, 13);
        zsp_bv_t v4  = zsp_bb_value_u64(c.bb, 8, 4);
        zsp_bv_t v1  = zsp_bb_value_u64(c.bb, 8, 1);
        CHECK(solve_assert(&c, zsp_bb_eq(c.bb, zsp_bb_urem(c.bb,v13,v4), v1)) == ZSP_SAT_SAT,
              "13 urem 4 == 1");
    }
    ctx_free(&c);
}

int main(void) {
    test_add();
    test_add_overflow();
    test_mul();
    test_mul_solve();
    test_ult();
    test_slt();
    test_shl();
    test_shr();
    test_extract_concat();
    test_udiv_urem();
    return failures ? 1 : 0;
}
