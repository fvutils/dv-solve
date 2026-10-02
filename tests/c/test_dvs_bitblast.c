/* End-to-end test of the BV bit-blaster. For each operator, build a
 * small SMT-like constraint, bit-blast to AIG, CNF-encode, kissat-solve,
 * and check the model. */
#include <stdio.h>
#include <stdlib.h>

#include "dvs_aig.h"
#include "dvs_aig_cnf.h"
#include "dvs_bitblast.h"
#include "dvs_sat.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

typedef struct {
    dvs_aig_t      *aig;
    dvs_sat_t      *sat;
    dvs_aig_cnf_t  *cnf;
    dvs_bitblast_t *bb;
} ctx_t;

static ctx_t ctx_new(void) {
    ctx_t c;
    c.aig = dvs_aig_new(NULL);
    c.sat = dvs_sat_new(NULL);
    c.cnf = dvs_aig_cnf_new(NULL, c.aig, c.sat);
    c.bb  = dvs_bitblast_new(NULL, c.aig);
    return c;
}
static void ctx_free(ctx_t *c) {
    dvs_bitblast_free(c->bb);
    dvs_aig_cnf_free(c->cnf);
    dvs_sat_free(c->sat);
    dvs_aig_free(c->aig);
}

/* Helper: read a uint64 model of `bv` from the SAT solver. Assumes
 * bv.size <= 64. */
static uint64_t model_u64(dvs_aig_cnf_t *enc, dvs_bv_t bv) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < bv.size; i++) {
        int b = dvs_aig_cnf_value(enc, bv.bits[i]);
        if (b == 1) v |= (uint64_t)1 << (bv.size - 1 - i);
    }
    return v;
}

/* Assert that `pred` (a 1-bit bv) is true, solve, and check SAT. */
static int solve_assert(ctx_t *c, dvs_bv_t pred) {
    dvs_aig_cnf_encode(c->cnf, pred.bits[0], /*top_level=*/1);
    return dvs_sat_solve(c->sat);
}

/* Bind two free 8-bit constants a, b with a==<va> and b==<vb>, return SAT
 * result of an asserted equation `lhs == rhs`. */
static void test_add(void) {
    ctx_t c = ctx_new();
    dvs_bv_t a = dvs_bb_constant(c.bb, 8);
    dvs_bv_t b = dvs_bb_constant(c.bb, 8);
    /* assert a = 7 and b = 35 and a+b = result, return result */
    dvs_bv_t v7  = dvs_bb_value_u64(c.bb, 8, 7);
    dvs_bv_t v35 = dvs_bb_value_u64(c.bb, 8, 35);
    dvs_bv_t v42 = dvs_bb_value_u64(c.bb, 8, 42);

    dvs_aig_cnf_encode(c.cnf, dvs_bb_eq(c.bb, a, v7).bits[0], 1);
    dvs_aig_cnf_encode(c.cnf, dvs_bb_eq(c.bb, b, v35).bits[0], 1);
    dvs_bv_t sum = dvs_bb_add(c.bb, a, b);
    dvs_aig_cnf_encode(c.cnf, dvs_bb_eq(c.bb, sum, v42).bits[0], 1);

    CHECK(dvs_sat_solve(c.sat) == DVS_SAT_SAT, "7+35==42 SAT");
    ctx_free(&c);
}

static void test_add_overflow(void) {
    ctx_t c = ctx_new();
    dvs_bv_t a = dvs_bb_value_u64(c.bb, 8, 200);
    dvs_bv_t b = dvs_bb_value_u64(c.bb, 8, 100);
    dvs_bv_t expected = dvs_bb_value_u64(c.bb, 8, (200 + 100) & 0xff);  /* 44 */
    dvs_bv_t sum = dvs_bb_add(c.bb, a, b);
    CHECK(solve_assert(&c, dvs_bb_eq(c.bb, sum, expected)) == DVS_SAT_SAT,
          "200+100 mod 256 == 44");
    ctx_free(&c);
}

static void test_mul(void) {
    ctx_t c = ctx_new();
    dvs_bv_t a = dvs_bb_value_u64(c.bb, 8, 7);
    dvs_bv_t b = dvs_bb_value_u64(c.bb, 8, 6);
    dvs_bv_t e = dvs_bb_value_u64(c.bb, 8, 42);
    dvs_bv_t p = dvs_bb_mul(c.bb, a, b);
    CHECK(solve_assert(&c, dvs_bb_eq(c.bb, p, e)) == DVS_SAT_SAT, "7*6 == 42");
    ctx_free(&c);
}

static void test_mul_solve(void) {
    /* Find a, b such that a*b = 35 and a < 10 and b < 10. Should yield 5,7 or 7,5. */
    ctx_t c = ctx_new();
    dvs_bv_t a = dvs_bb_constant(c.bb, 8);
    dvs_bv_t b = dvs_bb_constant(c.bb, 8);
    dvs_bv_t v35 = dvs_bb_value_u64(c.bb, 8, 35);
    dvs_bv_t v10 = dvs_bb_value_u64(c.bb, 8, 10);
    dvs_bv_t v0  = dvs_bb_value_u64(c.bb, 8, 0);

    dvs_aig_cnf_encode(c.cnf, dvs_bb_eq(c.bb, dvs_bb_mul(c.bb,a,b), v35).bits[0], 1);
    dvs_aig_cnf_encode(c.cnf, dvs_bb_ult(c.bb, a, v10).bits[0], 1);
    dvs_aig_cnf_encode(c.cnf, dvs_bb_ult(c.bb, b, v10).bits[0], 1);
    dvs_aig_cnf_encode(c.cnf, dvs_bb_ult(c.bb, v0, a).bits[0], 1);   /* a > 0 */
    dvs_aig_cnf_encode(c.cnf, dvs_bb_ult(c.bb, v0, b).bits[0], 1);   /* b > 0 */
    int rc = dvs_sat_solve(c.sat);
    CHECK(rc == DVS_SAT_SAT, "factor 35 in [1,10)x[1,10)");
    if (rc == DVS_SAT_SAT) {
        uint64_t va = model_u64(c.cnf, a);
        uint64_t vb = model_u64(c.cnf, b);
        printf("       model: a=%llu b=%llu\n", (unsigned long long)va, (unsigned long long)vb);
        CHECK(va * vb == 35, "model satisfies a*b=35");
    }
    ctx_free(&c);
}

static void test_ult(void) {
    /* 0 <_u 1 SAT, 5 <_u 5 UNSAT, 255 <_u 0 UNSAT (unsigned).
     * dvs_bv_t values carry pointers owned by their originating bb
     * context — they MUST be recreated in each fresh context. */
    ctx_t c = ctx_new();
    {
        dvs_bv_t v0 = dvs_bb_value_u64(c.bb, 8, 0);
        dvs_bv_t v1 = dvs_bb_value_u64(c.bb, 8, 1);
        CHECK(solve_assert(&c, dvs_bb_ult(c.bb, v0, v1)) == DVS_SAT_SAT, "0 < 1");
    }
    ctx_free(&c);

    c = ctx_new();
    {
        dvs_bv_t v5 = dvs_bb_value_u64(c.bb, 8, 5);
        CHECK(solve_assert(&c, dvs_bb_ult(c.bb, v5, v5)) == DVS_SAT_UNSAT, "~(5 < 5)");
    }
    ctx_free(&c);

    c = ctx_new();
    {
        dvs_bv_t v0   = dvs_bb_value_u64(c.bb, 8, 0);
        dvs_bv_t v255 = dvs_bb_value_u64(c.bb, 8, 255);
        CHECK(solve_assert(&c, dvs_bb_ult(c.bb, v255, v0)) == DVS_SAT_UNSAT, "~(255 <_u 0)");
    }
    ctx_free(&c);
}

static void test_slt(void) {
    /* signed: -1 (=255) <_s 0 SAT; 1 <_s -1 UNSAT.
     * bv handles do not survive ctx_free — recreate per context. */
    ctx_t c = ctx_new();
    {
        dvs_bv_t v0  = dvs_bb_value_u64(c.bb, 8, 0);
        dvs_bv_t vN1 = dvs_bb_value_u64(c.bb, 8, 255);  /* -1 */
        CHECK(solve_assert(&c, dvs_bb_slt(c.bb, vN1, v0)) == DVS_SAT_SAT, "-1 <_s 0");
    }
    ctx_free(&c);

    c = ctx_new();
    {
        dvs_bv_t vN1 = dvs_bb_value_u64(c.bb, 8, 255);  /* -1 */
        dvs_bv_t v1  = dvs_bb_value_u64(c.bb, 8, 1);
        CHECK(solve_assert(&c, dvs_bb_slt(c.bb, v1, vN1)) == DVS_SAT_UNSAT, "~(1 <_s -1)");
    }
    ctx_free(&c);
}

static void test_shl(void) {
    ctx_t c = ctx_new();
    /* 1 << 3 = 8 */
    dvs_bv_t v1 = dvs_bb_value_u64(c.bb, 8, 1);
    dvs_bv_t v3 = dvs_bb_value_u64(c.bb, 8, 3);
    dvs_bv_t v8 = dvs_bb_value_u64(c.bb, 8, 8);
    CHECK(solve_assert(&c, dvs_bb_eq(c.bb, dvs_bb_shl(c.bb, v1, v3), v8)) == DVS_SAT_SAT,
          "1 << 3 == 8");
    ctx_free(&c);
}

static void test_shr(void) {
    ctx_t c = ctx_new();
    /* 16 >> 2 = 4 */
    dvs_bv_t v16 = dvs_bb_value_u64(c.bb, 8, 16);
    dvs_bv_t v2  = dvs_bb_value_u64(c.bb, 8, 2);
    dvs_bv_t v4  = dvs_bb_value_u64(c.bb, 8, 4);
    CHECK(solve_assert(&c, dvs_bb_eq(c.bb, dvs_bb_shr(c.bb, v16, v2), v4)) == DVS_SAT_SAT,
          "16 >> 2 == 4");
    ctx_free(&c);
}

static void test_extract_concat(void) {
    ctx_t c = ctx_new();
    /* extract bits [3:0] of 0xa5 = 5 */
    dvs_bv_t v   = dvs_bb_value_u64(c.bb, 8, 0xa5);
    dvs_bv_t lo  = dvs_bb_extract(c.bb, v, 3, 0);
    dvs_bv_t v5  = dvs_bb_value_u64(c.bb, 4, 5);
    CHECK(solve_assert(&c, dvs_bb_eq(c.bb, lo, v5)) == DVS_SAT_SAT,
          "extract 0xa5[3:0] == 5");
    ctx_free(&c);

    c = ctx_new();
    /* concat 4-bit 0x5 with 4-bit 0xa = 8-bit 0x5a */
    dvs_bv_t hi5 = dvs_bb_value_u64(c.bb, 4, 5);
    dvs_bv_t loA = dvs_bb_value_u64(c.bb, 4, 0xa);
    dvs_bv_t v5a = dvs_bb_value_u64(c.bb, 8, 0x5a);
    CHECK(solve_assert(&c, dvs_bb_eq(c.bb, dvs_bb_concat(c.bb, hi5, loA), v5a)) == DVS_SAT_SAT,
          "concat(5_4, a_4) == 0x5a");
    ctx_free(&c);
}

static void test_udiv_urem(void) {
    ctx_t c = ctx_new();
    /* 13 udiv 4 = 3 */
    {
        dvs_bv_t v13 = dvs_bb_value_u64(c.bb, 8, 13);
        dvs_bv_t v4  = dvs_bb_value_u64(c.bb, 8, 4);
        dvs_bv_t v3  = dvs_bb_value_u64(c.bb, 8, 3);
        CHECK(solve_assert(&c, dvs_bb_eq(c.bb, dvs_bb_udiv(c.bb,v13,v4), v3)) == DVS_SAT_SAT,
              "13 udiv 4 == 3");
    }
    ctx_free(&c);

    /* 13 urem 4 = 1 — fresh context (bv handles do not survive ctx_free) */
    c = ctx_new();
    {
        dvs_bv_t v13 = dvs_bb_value_u64(c.bb, 8, 13);
        dvs_bv_t v4  = dvs_bb_value_u64(c.bb, 8, 4);
        dvs_bv_t v1  = dvs_bb_value_u64(c.bb, 8, 1);
        CHECK(solve_assert(&c, dvs_bb_eq(c.bb, dvs_bb_urem(c.bb,v13,v4), v1)) == DVS_SAT_SAT,
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
