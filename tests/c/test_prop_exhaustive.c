/* Exhaustive soundness check of each propagator and its explainer.
 *
 * For every propagator kind, small variables (2-3 bits, unsigned and signed)
 * and EVERY box of sub-intervals of their domains:
 *
 *   - fire the propagator once; a conflict needs the box to hold no solution,
 *     and otherwise no solution in the box may be pruned;
 *   - on a single point that violates the constraint, propagation to a fixed
 *     point must conflict (an "incomplete" propagator accepts a wrong model);
 *   - every bound it tightened is explained, and the explanation (plus the
 *     narrowed variable's previous bound, which conflict analysis adds for
 *     explainers not on its whitelist) must imply the bound for every
 *     assignment of the full domains that satisfies the constraint.
 *
 * "Solution" is decided by a truth function written here, independent of the
 * propagator. See docs/soundness_coverage_plan.md (P1, narrowing and
 * explanation checkers).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_ctx.h"
#include "dvs_explain.h"
#include "dvs_lcg.h"
#include "dvs_problem.h"
#include "dvs_propagator.h"
#include "dvs_trail.h"

#define MAXV 4

typedef struct Case Case;
struct Case {
    const char *name;
    int nv;
    uint8_t width[MAXV];
    uint8_t is_signed[MAXV];
    uint32_t (*add)(dvs_ctx_t *ctx, const Case *c);
    int (*holds)(const int64_t *v, const Case *c);
    int64_t k[4];           /* case parameters (constants, widths, bits) */
    int samples;            /* 0: every box; else this many random boxes */
};

static int64_t vmin(const Case *c, int i) {
    return c->is_signed[i] ? -((int64_t)1 << (c->width[i] - 1)) : 0;
}
static int64_t vmax(const Case *c, int i) {
    return c->is_signed[i] ? ((int64_t)1 << (c->width[i] - 1)) - 1
                           : ((int64_t)1 << c->width[i]) - 1;
}
static int64_t wrapw(int64_t x, int w, int sgn) {
    uint64_t m = (w >= 64) ? ~0ULL : ((1ULL << w) - 1);
    uint64_t u = (uint64_t)x & m;
    if (sgn && w < 64 && (u >> (w - 1)) & 1) return (int64_t)(u | ~m);
    return (int64_t)u;
}

/* ---- truth functions ---- */
static int h_le(const int64_t *v, const Case *c)  { (void)c; return v[0] <= v[1]; }
static int h_lt(const int64_t *v, const Case *c)  { (void)c; return v[0] <  v[1]; }
static int h_eq(const int64_t *v, const Case *c)  { (void)c; return v[0] == v[1]; }
static int h_ne(const int64_t *v, const Case *c)  { (void)c; return v[0] != v[1]; }
static int h_add(const int64_t *v, const Case *c) { (void)c; return v[0] == v[1] + v[2]; }
static int h_mul(const int64_t *v, const Case *c) { (void)c; return v[0] == v[1] * v[2]; }
static int h_div(const int64_t *v, const Case *c) {
    if (v[2] == 0) return 1;                     /* SystemVerilog x: any r */
    return v[0] == wrapw(v[1] / v[2], c->width[0], c->is_signed[0]);
}
static int h_mod(const int64_t *v, const Case *c) {
    if (v[2] == 0) return 1;
    return v[0] == wrapw(v[1] % v[2], c->width[0], c->is_signed[0]);
}
/* unary_neg and bounds_shl are integer operations: compile uses them only
 * where the result cannot wrap (the modular bv* propagators take every
 * result width 1..64), so -MIN and overflowing shifts have no solution. */
static int h_neg(const int64_t *v, const Case *c) { (void)c; return v[0] == -v[1]; }
static int h_bvadd(const int64_t *v, const Case *c) { return v[0] == wrapw(v[1] + v[2], (int)c->k[0], 0); }
static int h_bvsub(const int64_t *v, const Case *c) { return v[0] == wrapw(v[1] - v[2], (int)c->k[0], 0); }
static int h_bvmul(const int64_t *v, const Case *c) { return v[0] == wrapw(v[1] * v[2], (int)c->k[0], 0); }
static int h_bvshl(const int64_t *v, const Case *c) {
    return v[0] == (v[2] >= c->k[0] ? 0 : wrapw(v[1] << v[2], (int)c->k[0], 0));
}
static int h_bvaddc(const int64_t *v, const Case *c) { return v[0] == wrapw(v[1] + c->k[1], (int)c->k[0], 0); }
static int h_band(const int64_t *v, const Case *c) { (void)c; return v[0] == (v[1] & v[2]); }
static int h_bor(const int64_t *v, const Case *c)  { (void)c; return v[0] == (v[1] | v[2]); }
static int h_bxor(const int64_t *v, const Case *c) { (void)c; return v[0] == (v[1] ^ v[2]); }
static int h_bnot(const int64_t *v, const Case *c) { return v[0] == wrapw(~v[1], c->width[0], 0); }
static int h_shl(const int64_t *v, const Case *c) {
    (void)c; return v[2] < 63 && v[0] == (v[1] << v[2]);
}
static int h_lshr(const int64_t *v, const Case *c) { (void)c; return v[0] == (v[2] >= 63 ? 0 : v[1] >> v[2]); }
static int h_concat(const int64_t *v, const Case *c) { return v[0] == ((v[1] << c->k[0]) | v[2]); }
static int h_ite(const int64_t *v, const Case *c)  { (void)c; return v[1] ? v[0] == v[2] : v[0] == v[3]; }
static int h_reif(const int64_t *v, const Case *c)    { (void)c; return v[0] == (v[1] <= v[2]); }
static int h_reif_eq(const int64_t *v, const Case *c) { (void)c; return v[0] == (v[1] == v[2]); }
static int h_impl(const int64_t *v, const Case *c) {
    return !v[0] || (c->k[1] ? v[1] <= c->k[0] : v[1] >= c->k[0]);
}
static int h_slice(const int64_t *v, const Case *c) {
    int64_t hi = c->k[0], lo = c->k[1];
    return v[0] == (((uint64_t)wrapw(v[1], c->width[1], 0) >> lo) & ((1ULL << (hi - lo + 1)) - 1));
}
static int h_inset(const int64_t *v, const Case *c) {
    return v[0] == c->k[0] || v[0] == c->k[1] || v[0] == c->k[2];
}
static int h_alldiff(const int64_t *v, const Case *c) {
    (void)c; return v[0] != v[1] && v[0] != v[2] && v[1] != v[2];
}
static int h_disj3(const int64_t *v, const Case *c) { (void)c; return v[0] == 2 || v[1] < v[2] || v[2] >= 5; }
static int h_disj2(const int64_t *v, const Case *c) { (void)c; return v[0] != v[1] || v[0] <= 1; }
static int h_disj4(const int64_t *v, const Case *c) {
    (void)c; return v[0] > v[1] || v[1] == 3 || v[2] != v[3] || v[3] <= 0;
}
static int h_sum(const int64_t *v, const Case *c)  { (void)c; return v[0] == v[1] + v[2] + v[3]; }
static int h_sum2x(const int64_t *v, const Case *c) { (void)c; return v[0] == v[1] + v[1] + v[2]; }
static int h_countones(const int64_t *v, const Case *c) {
    return v[0] == __builtin_popcountll((uint64_t)wrapw(v[1], c->width[1], 0));
}
static int h_clog2(const int64_t *v, const Case *c) {
    uint64_t a = (uint64_t)wrapw(v[1], c->width[1], 0);
    int64_t r = 0;
    while (((uint64_t)1 << r) < a) r++;
    return v[0] == r;
}

/* ---- constructors ---- */
#define ADD2(fn) static uint32_t a_##fn(dvs_ctx_t *x, const Case *c) { (void)c; return prop_add_##fn(x, 0, 1, 0); }
#define ADD3(fn) static uint32_t a_##fn(dvs_ctx_t *x, const Case *c) { (void)c; return prop_add_##fn(x, 0, 1, 2, 0); }
#define ADD3W(fn) static uint32_t a_##fn(dvs_ctx_t *x, const Case *c) { return prop_add_##fn(x, 0, 1, 2, (uint8_t)c->k[0], 0); }
ADD2(bounds_le_32) ADD2(bounds_lt_32) ADD2(bounds_eq_32) ADD2(bounds_ne_32)
ADD2(bounds_le_64) ADD2(bounds_lt_64) ADD2(bounds_eq_64) ADD2(bounds_ne_64)
ADD3(bounds_add_32) ADD3(bounds_mul_32) ADD3(bounds_div_32) ADD3(bounds_mod_32)
ADD3(bounds_add_64) ADD3(bounds_mul_64) ADD3(bounds_div_64) ADD3(bounds_mod_64)
ADD3(bounds_band_64) ADD3(bounds_bor_64) ADD3(bounds_bxor_64)
ADD3(bounds_shl_64) ADD3(bounds_lshr_64)
ADD3(reification_32) ADD3(reification_64) ADD3(reification_eq_32) ADD3(reification_eq_64)
ADD3W(bvadd_64) ADD3W(bvsub_64) ADD3W(bvmul_64) ADD3W(bvshl_64)
static uint32_t a_unary_neg_32(dvs_ctx_t *x, const Case *c) { (void)c; return prop_add_unary_neg_32(x, 0, 1, 0); }
static uint32_t a_unary_neg_64(dvs_ctx_t *x, const Case *c) { (void)c; return prop_add_unary_neg_64(x, 0, 1, 0); }
static uint32_t a_bnot(dvs_ctx_t *x, const Case *c) { (void)c; return prop_add_bounds_bnot_64(x, 0, 1, 0); }
static uint32_t a_bvaddc(dvs_ctx_t *x, const Case *c) {
    return prop_add_bvadd_const_64(x, 0, 1, (uint64_t)c->k[1], (uint8_t)c->k[0], 0);
}
static uint32_t a_concat(dvs_ctx_t *x, const Case *c) {
    return prop_add_bounds_concat_64(x, 0, 1, 2, (uint8_t)c->k[0], 0);
}
static uint32_t a_ite(dvs_ctx_t *x, const Case *c) { (void)c; return prop_add_ite_value_64(x, 0, 1, 2, 3, 0); }
static uint32_t a_impl32(dvs_ctx_t *x, const Case *c) {
    return prop_add_implication_32(x, 0, 1, (int32_t)c->k[0], (uint8_t)c->k[1], 0);
}
static uint32_t a_impl64(dvs_ctx_t *x, const Case *c) {
    return prop_add_implication_64(x, 0, 1, c->k[0], (uint8_t)c->k[1], 0);
}
static uint32_t a_slice32(dvs_ctx_t *x, const Case *c) {
    return prop_add_bit_slice_32(x, 0, 1, (uint8_t)c->k[0], (uint8_t)c->k[1], 0);
}
static uint32_t a_slice64(dvs_ctx_t *x, const Case *c) {
    return prop_add_bit_slice_64(x, 0, 1, (uint8_t)c->k[0], (uint8_t)c->k[1], 0);
}
static uint32_t a_inset32(dvs_ctx_t *x, const Case *c) {
    int32_t e[3] = { (int32_t)c->k[0], (int32_t)c->k[1], (int32_t)c->k[2] };
    return prop_add_in_set_32(x, 0, 3, e, 0);
}
static uint32_t a_inset64(dvs_ctx_t *x, const Case *c) {
    int64_t e[3] = { c->k[0], c->k[1], c->k[2] };
    return prop_add_in_set_64(x, 0, 3, e, 0);
}
static uint32_t a_alldiff(dvs_ctx_t *x, const Case *c) {
    (void)c; uint32_t ids[3] = { 0, 1, 2 };
    return prop_add_all_different(x, 3, ids, 0);
}
static uint32_t a_sum(dvs_ctx_t *x, const Case *c) {
    (void)c; uint32_t ids[3] = { 1, 2, 3 };
    return prop_add_sum_eq_32(x, 0, 3, ids, 0);
}
static uint32_t a_sum2x(dvs_ctx_t *x, const Case *c) {
    (void)c; uint32_t ids[3] = { 1, 1, 2 };
    return prop_add_sum_eq_32(x, 0, 3, ids, 0);
}
static uint32_t a_disj3(dvs_ctx_t *x, const Case *c) {
    (void)c;
    uint32_t vars[3] = { 0, 1, 2 }, ops[3] = { DVS_BIN_EQ, DVS_BIN_LT, DVS_BIN_GTE };
    int64_t k[3] = { 2, 0, 5 };
    uint32_t rhs[3] = { UINT32_MAX, 2, UINT32_MAX };
    return prop_add_disj_clause(x, 3, vars, ops, k, 0, rhs);
}
static uint32_t a_disj2(dvs_ctx_t *x, const Case *c) {
    (void)c;
    uint32_t vars[2] = { 0, 0 }, ops[2] = { DVS_BIN_NEQ, DVS_BIN_LTE };
    int64_t k[2] = { 0, 1 };
    uint32_t rhs[2] = { 1, UINT32_MAX };
    return prop_add_disj_clause(x, 2, vars, ops, k, 0, rhs);
}
static uint32_t a_disj4(dvs_ctx_t *x, const Case *c) {
    (void)c;
    uint32_t vars[4] = { 0, 1, 2, 3 }, ops[4] = { DVS_BIN_GT, DVS_BIN_EQ, DVS_BIN_NEQ, DVS_BIN_LTE };
    int64_t k[4] = { 0, 3, 0, 0 };
    uint32_t rhs[4] = { 1, UINT32_MAX, 3, UINT32_MAX };
    return prop_add_disj_clause(x, 4, vars, ops, k, 0, rhs);
}
static uint32_t a_countones(dvs_ctx_t *x, const Case *c) { (void)c; return prop_add_countones_32(x, 0, 1, 0); }
static uint32_t a_clog2(dvs_ctx_t *x, const Case *c)     { (void)c; return prop_add_clog2_32(x, 0, 1, 0); }

#define U3 3, 3, 3, 3
#define S3 1, 1, 1, 1
static const Case CASES[] = {
    { "le_32 u",  2, {U3}, {0},  a_bounds_le_32, h_le, {0} },
    { "le_32 s",  2, {U3}, {S3}, a_bounds_le_32, h_le, {0} },
    { "lt_32 u",  2, {U3}, {0},  a_bounds_lt_32, h_lt, {0} },
    { "lt_32 s",  2, {U3}, {S3}, a_bounds_lt_32, h_lt, {0} },
    { "eq_32 s",  2, {U3}, {S3}, a_bounds_eq_32, h_eq, {0} },
    { "ne_32 u",  2, {U3}, {0},  a_bounds_ne_32, h_ne, {0} },
    { "ne_32 s",  2, {U3}, {S3}, a_bounds_ne_32, h_ne, {0} },
    { "le_64 u",  2, {U3}, {0},  a_bounds_le_64, h_le, {0} },
    { "lt_64 s",  2, {U3}, {S3}, a_bounds_lt_64, h_lt, {0} },
    { "eq_64 u",  2, {U3}, {0},  a_bounds_eq_64, h_eq, {0} },
    { "ne_64 u",  2, {U3}, {0},  a_bounds_ne_64, h_ne, {0} },
    { "add_32 u", 3, {U3}, {0},  a_bounds_add_32, h_add, {0} },
    { "add_32 s", 3, {U3}, {S3}, a_bounds_add_32, h_add, {0} },
    { "add_64 u", 3, {U3}, {0},  a_bounds_add_64, h_add, {0} },
    { "mul_32 u", 3, {U3}, {0},  a_bounds_mul_32, h_mul, {0} },
    { "mul_32 s", 3, {U3}, {S3}, a_bounds_mul_32, h_mul, {0} },
    { "mul_64 u", 3, {U3}, {0},  a_bounds_mul_64, h_mul, {0} },
    { "div_32 u", 3, {U3}, {0},  a_bounds_div_32, h_div, {0} },
    { "div_32 s", 3, {U3}, {S3}, a_bounds_div_32, h_div, {0} },
    { "div_64 u", 3, {U3}, {0},  a_bounds_div_64, h_div, {0} },
    { "mod_32 u", 3, {U3}, {0},  a_bounds_mod_32, h_mod, {0} },
    { "mod_32 s", 3, {U3}, {S3}, a_bounds_mod_32, h_mod, {0} },
    { "mod_64 u", 3, {U3}, {0},  a_bounds_mod_64, h_mod, {0} },
    { "neg_32 s", 2, {U3}, {S3}, a_unary_neg_32, h_neg, {0} },
    { "neg_64 s", 2, {U3}, {S3}, a_unary_neg_64, h_neg, {0} },
    { "bvadd w3", 3, {U3}, {0},  a_bvadd_64, h_bvadd, {3} },
    { "bvsub w3", 3, {U3}, {0},  a_bvsub_64, h_bvsub, {3} },
    { "bvmul w3", 3, {U3}, {0},  a_bvmul_64, h_bvmul, {3} },
    { "bvshl w3", 3, {U3}, {0},  a_bvshl_64, h_bvshl, {3} },
    { "bvaddc w3 +5", 2, {U3}, {0}, a_bvaddc, h_bvaddc, {3, 5} },
    { "band",     3, {U3}, {0},  a_bounds_band_64, h_band, {0} },
    { "bor",      3, {U3}, {0},  a_bounds_bor_64,  h_bor,  {0} },
    { "bxor",     3, {U3}, {0},  a_bounds_bxor_64, h_bxor, {0} },
    { "bnot",     2, {U3}, {0},  a_bnot, h_bnot, {0} },
    { "shl",      3, {U3}, {0},  a_bounds_shl_64,  h_shl,  {0} },
    { "lshr",     3, {U3}, {0},  a_bounds_lshr_64, h_lshr, {0} },
    { "concat 2|1", 3, {3, 2, 1}, {0}, a_concat, h_concat, {1} },
    { "ite",      4, {2, 1, 2, 2}, {0}, a_ite, h_ite, {0} },
    { "reif_32 u", 3, {1, 3, 3}, {0}, a_reification_32, h_reif, {0} },
    { "reif_32 s", 3, {1, 3, 3}, {0, 1, 1}, a_reification_32, h_reif, {0} },
    { "reif_64 u", 3, {1, 3, 3}, {0}, a_reification_64, h_reif, {0} },
    { "reif_eq_32 u", 3, {1, 3, 3}, {0}, a_reification_eq_32, h_reif_eq, {0} },
    { "reif_eq_32 s", 3, {1, 3, 3}, {0, 1, 1}, a_reification_eq_32, h_reif_eq, {0} },
    { "reif_eq_64 u", 3, {1, 3, 3}, {0}, a_reification_eq_64, h_reif_eq, {0} },
    { "impl_32 <=5", 2, {1, 3}, {0}, a_impl32, h_impl, {5, 1} },
    { "impl_32 >=2", 2, {1, 3}, {0}, a_impl32, h_impl, {2, 0} },
    { "impl_64 >=-1 s", 2, {1, 3}, {0, 1}, a_impl64, h_impl, {-1, 0} },
    { "slice_32 [2:1]", 2, {2, 3}, {0}, a_slice32, h_slice, {2, 1} },
    { "slice_64 [1:0]", 2, {2, 3}, {0}, a_slice64, h_slice, {1, 0} },
    { "slice_32 [2:1] s", 2, {2, 3}, {0, 1}, a_slice32, h_slice, {2, 1} },
    { "in_set_32 {1,4,6}", 1, {3}, {0}, a_inset32, h_inset, {1, 4, 6} },
    { "in_set_64 {0,3,7}", 1, {3}, {0}, a_inset64, h_inset, {0, 3, 7} },
    { "all_different", 3, {U3}, {0}, a_alldiff, h_alldiff, {0} },
    { "disj x==2|y<z|z>=5", 3, {U3}, {0}, a_disj3, h_disj3, {0} },
    { "disj x!=y|x<=1 s", 2, {U3}, {S3}, a_disj2, h_disj2, {0} },
    { "disj 4 vars", 4, {2, 2, 2, 2}, {0}, a_disj4, h_disj4, {0} },
    { "sum_eq 3", 4, {3, 2, 2, 2}, {0}, a_sum, h_sum, {0} },
    { "sum_eq x+x+y", 3, {3, 2, 2}, {0}, a_sum2x, h_sum2x, {0} },
    { "countones", 2, {3, 3}, {0}, a_countones, h_countones, {0} },
    { "clog2", 2, {3, 3}, {0}, a_clog2, h_clog2, {0} },
    /* Wider, sampled: bit-level reasoning needs more than 3 bits to go wrong. */
    { "band w6",  3, {6, 6, 6}, {0}, a_bounds_band_64, h_band, {0}, 40000 },
    { "bor w6",   3, {6, 6, 6}, {0}, a_bounds_bor_64,  h_bor,  {0}, 40000 },
    { "bxor w6",  3, {6, 6, 6}, {0}, a_bounds_bxor_64, h_bxor, {0}, 40000 },
    { "bnot w6",  2, {6, 6}, {0}, a_bnot, h_bnot, {0}, 40000 },
    { "lshr w6",  3, {6, 6, 6}, {0}, a_bounds_lshr_64, h_lshr, {0}, 40000 },
    { "bvadd w6", 3, {6, 6, 6}, {0}, a_bvadd_64, h_bvadd, {6}, 40000 },
    { "bvsub w6", 3, {6, 6, 6}, {0}, a_bvsub_64, h_bvsub, {6}, 40000 },
    { "bvmul w6", 3, {6, 6, 6}, {0}, a_bvmul_64, h_bvmul, {6}, 40000 },
    { "bvshl w6", 3, {6, 6, 6}, {0}, a_bvshl_64, h_bvshl, {6}, 40000 },
    { "bvaddc w6 +45", 2, {6, 6}, {0}, a_bvaddc, h_bvaddc, {6, 45}, 40000 },
    { "concat w6 3|3", 3, {6, 3, 3}, {0}, a_concat, h_concat, {3}, 40000 },
    { "slice_64 w6 [4:2]", 2, {3, 6}, {0}, a_slice64, h_slice, {4, 2}, 40000 },
    { "reif_64 w6 s", 3, {1, 6, 6}, {0, 1, 1}, a_reification_64, h_reif, {0}, 40000 },
    { "reif_eq_64 w6", 3, {1, 6, 6}, {0}, a_reification_eq_64, h_reif_eq, {0}, 40000 },
    { "ne_64 w6", 2, {6, 6}, {0}, a_bounds_ne_64, h_ne, {0}, 40000 },
    { "countones w6", 2, {3, 6}, {0}, a_countones, h_countones, {0}, 40000 },
};

/* ---- harness ---- */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;   /* fixed seed: reproducible */
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}

static int n_err, n_incomplete, n_needs_other_own, n_expl, n_refused;

static void set_bounds(dvs_ctx_t *ctx, uint32_t v, int64_t lo, int64_t hi) {
    Variable *var = &ctx->vars[v];
    if (VAR_IS_TIER0(var->flags)) { var->lo = (int32_t)lo; var->hi = (int32_t)hi; }
    else {
        WideBounds64 *wb = (WideBounds64 *)dvs_pool_ptr(&ctx->pool, var->holes_offset);
        wb->lo = lo; wb->hi = hi;
    }
}

static int own_bound_added(const Propagator *p) {
    /* Mirror of _explains_without_own_bound in dvs_lcg.c. */
    return !(p->explain == explain_bounds_le || p->explain == explain_bounds_lt
          || p->explain == explain_bounds_eq || p->explain == explain_bounds_add
          || p->explain == explain_sum_eq    || p->explain == explain_implication
          || p->explain == explain_ite_value || p->explain == explain_reification);
}

static int lit_holds(const Literal *l, const int64_t *v) {
    return l->is_lb ? v[l->var_id] >= l->bound : v[l->var_id] <= l->bound;
}

/* Iterate over every assignment in [lo[i], hi[i]]. */
static int next_point(int nv, const int64_t *lo, const int64_t *hi, int64_t *v) {
    for (int i = 0; i < nv; i++) {
        if (v[i] < hi[i]) { v[i]++; return 1; }
        v[i] = lo[i];
    }
    return 0;
}

/* Does every solution (within the full domains) that satisfies `ante`
 * satisfy `lit`? */
static int implied(const Case *c, const Literal *ante, uint32_t n, const Literal *lit) {
    int64_t lo[MAXV], hi[MAXV], v[MAXV];
    for (int i = 0; i < c->nv; i++) { lo[i] = vmin(c, i); hi[i] = vmax(c, i); v[i] = lo[i]; }
    do {
        if (!c->holds(v, c)) continue;
        int ok = 1;
        for (uint32_t j = 0; j < n && ok; j++)
            if (ante[j].var_id >= (uint32_t)c->nv || !lit_holds(&ante[j], v)) ok = 0;
        if (ok && !lit_holds(lit, v)) return 0;
    } while (next_point(c->nv, lo, hi, v));
    return 1;
}

static void run_case(const Case *c) {
    static uint8_t ctx_buf[1 << 20], sp_buf[1 << 16];
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1 << 20);
    dvs_problem_t *sp = solve_problem_init(sp_buf, sizeof sp_buf);
    for (int i = 0; i < c->nv; i++)
        problem_add_var(sp, (uint32_t)i, c->width[i], c->is_signed[i], vmin(c, i), vmax(c, i));
    dvs_ctx_t *ctx = dvs_solver_create(ctx_buf, sizeof ctx_buf, ba);
    if (!ctx || dvs_solver_compile(ctx, sp) < 0) {
        printf("FAIL %s: setup\n", c->name); n_err++; return;
    }
    uint32_t ref = c->add(ctx, c);
    if (ref == EXPR_NULL) { printf("FAIL %s: add\n", c->name); n_err++; return; }
    contra_register_explanations(ctx);
    Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool, ref);
    int own = p->explain ? own_bound_added(p) : 0;
    int errs = 0, max_report = 4;

    /* Box iteration: per variable, every interval [l, h]. */
    int64_t bl[MAXV], bh[MAXV];
    int sampled = 0;
    for (int i = 0; i < c->nv; i++) { bl[i] = bh[i] = vmin(c, i); }
    for (;;) {
        trail_push_level(ctx);
        int ok = 1;
        for (int i = 0; i < c->nv && ok; i++)
            ok = ctx_tighten_lb64(ctx, (uint32_t)i, bl[i]) == PROP_OK &&
                 ctx_tighten_ub64(ctx, (uint32_t)i, bh[i]) == PROP_OK;
        /* The box itself is the pre-fire state; drain anything queued. */
        ctx->queue.non_empty_mask = 0;
        for (int q = 0; q < 16; q++) ctx->queue.heads[q] = ctx->queue.tails[q] = EXPR_NULL;
        p->flags &= (uint8_t)~(PROP_FLAG_IN_QUEUE | PROP_FLAG_ENTAILED);
        if (ok) {
            int64_t v[MAXV], nsol = 0;
            for (int i = 0; i < c->nv; i++) v[i] = bl[i];
            do { nsol += c->holds(v, c); } while (next_point(c->nv, bl, bh, v));

            TrailEntry *mark = ctx->trail_top;
            ctx->current_prop_ref = ref;
            PropResult r = p->fire(p, ctx);
            ctx->current_prop_ref = EXPR_NULL;
            int empty = 0;
            int64_t nl[MAXV], nh[MAXV];
            for (int i = 0; i < c->nv; i++) {
                nl[i] = var_lo64(ctx, &ctx->vars[i]);
                nh[i] = var_hi64(ctx, &ctx->vars[i]);
                if (nl[i] > nh[i]) empty = 1;
            }
            if (r == PROP_CONFLICT || empty) {
                if (nsol > 0 && errs++ < max_report)
                    printf("FAIL %s: conflict on a box with %lld solutions\n", c->name, (long long)nsol);
            } else {
                for (int i = 0; i < c->nv; i++) v[i] = bl[i];
                do {
                    if (!c->holds(v, c)) continue;
                    for (int i = 0; i < c->nv; i++)
                        if (v[i] < nl[i] || v[i] > nh[i]) {
                            if (errs++ < max_report) {
                                printf("FAIL %s: pruned solution (", c->name);
                                for (int j = 0; j < c->nv; j++) printf("%s%lld", j ? "," : "", (long long)v[j]);
                                printf(") from box");
                                for (int j = 0; j < c->nv; j++) printf(" [%lld,%lld]", (long long)bl[j], (long long)bh[j]);
                                printf("\n");
                            }
                            break;
                        }
                } while (next_point(c->nv, bl, bh, v));

                /* Explanations: each tightened (var, direction). */
                for (TrailEntry *t = ctx->trail_top; t && t != mark; t = t->prev) {
                    if (t->kind != TRAIL_LB && t->kind != TRAIL_UB) continue;
                    uint32_t vi = t->var_id;
                    uint8_t is_lb = t->kind == TRAIL_LB;
                    /* Explain each var/direction once, from its oldest entry. */
                    int newest = 1;
                    for (TrailEntry *u = t->prev; u && u != mark; u = u->prev)
                        if (u->var_id == vi && u->kind == t->kind) { newest = 0; break; }
                    (void)newest;
                    int oldest = 1;
                    for (TrailEntry *u = t->prev; u && u != mark; u = u->prev)
                        if (u->var_id == vi && u->kind == t->kind) { oldest = 0; break; }
                    if (!oldest || !p->explain) continue;
                    Literal lit;
                    memset(&lit, 0, sizeof lit);
                    lit.var_id = vi; lit.is_lb = is_lb;
                    lit.bound = is_lb ? nl[vi] : nh[vi];
                    /* Explain in the state the propagator read (the box),
                     * as conflict analysis does: after the firing an
                     * explainer can cite the very bound it is explaining. */
                    for (int j = 0; j < c->nv; j++) {
                        set_bounds(ctx, (uint32_t)j, bl[j], bh[j]);
                    }
                    Explanation ex;
                    memset(&ex, 0, sizeof ex);
                    int erc = p->explain(p, ctx, vi, is_lb, lit.bound, &ex);
                    for (int j = 0; j < c->nv; j++) set_bounds(ctx, (uint32_t)j, nl[j], nh[j]);
                    if (erc != 0) { n_refused++; continue; }
                    n_expl++;
                    Literal a[MAX_EXPLAIN_LITS + 2];
                    uint32_t na = ex.n_lits;
                    memcpy(a, ex.lits, na * sizeof(Literal));
                    if (implied(c, a, na, &lit)) continue;
                    Literal o;
                    memset(&o, 0, sizeof o);
                    o.var_id = vi; o.is_lb = is_lb; o.bound = t->old_value;
                    a[na] = o;
                    if (own && implied(c, a, na + 1, &lit)) continue;
                    /* Does it hold with the variable's other (pre-fire) bound? */
                    Literal o2;
                    memset(&o2, 0, sizeof o2);
                    o2.var_id = vi; o2.is_lb = !is_lb; o2.bound = is_lb ? bh[vi] : bl[vi];
                    a[na + (own ? 1 : 0)] = o2;
                    int with_other = implied(c, a, na + (own ? 1 : 0) + 1, &lit);
                    if (with_other) n_needs_other_own++;
                    if (errs++ < max_report) {
                        printf("FAIL %s: explanation of v%u %s %lld%s does not imply it"
                               " (box", c->name, vi, is_lb ? ">=" : "<=", (long long)lit.bound,
                               own ? " (+own)" : "");
                        for (int j = 0; j < c->nv; j++) printf(" [%lld,%lld]", (long long)bl[j], (long long)bh[j]);
                        printf("):");
                        for (uint32_t j = 0; j < ex.n_lits; j++)
                            printf(" v%u%s%lld", ex.lits[j].var_id, ex.lits[j].is_lb ? ">=" : "<=",
                                   (long long)ex.lits[j].bound);
                        printf("%s\n", with_other ? "  [holds given its other own bound]" : "");
                    }
                }
            }

            /* Completeness on a point: propagate to a fixed point. */
            int point = 1;
            for (int i = 0; i < c->nv; i++) if (bl[i] != bh[i]) point = 0;
            if (point && r != PROP_CONFLICT && !empty && !c->holds(bl, c)) {
                PropResult r2 = PROP_OK;
                for (int it = 0; it < 64 && r2 != PROP_CONFLICT; it++) {
                    ctx->current_prop_ref = ref;
                    r2 = p->fire(p, ctx);
                    ctx->current_prop_ref = EXPR_NULL;
                }
                int empty2 = 0;
                for (int i = 0; i < c->nv; i++)
                    if (var_lo64(ctx, &ctx->vars[i]) > var_hi64(ctx, &ctx->vars[i])) empty2 = 1;
                if (r2 != PROP_CONFLICT && !empty2) {
                    n_incomplete++;
                    if (errs++ < max_report) {
                        printf("FAIL %s: accepts the violating point (", c->name);
                        for (int j = 0; j < c->nv; j++) printf("%s%lld", j ? "," : "", (long long)bl[j]);
                        printf(")\n");
                    }
                }
            }
        }
        trail_backtrack(ctx, 0);

        /* Next box. */
        if (c->samples) {
            if (++sampled >= c->samples) break;
            for (int i = 0; i < c->nv; i++) {
                int64_t lo = vmin(c, i), span = vmax(c, i) - lo + 1;
                int64_t a = lo + (int64_t)(rnd() % (uint64_t)span), b;
                uint64_t shape = rnd() % 10;
                if (shape < 3) b = a;                                        /* point */
                else if (shape < 7) b = a + (int64_t)(rnd() % 4);            /* narrow */
                else b = lo + (int64_t)(rnd() % (uint64_t)span);             /* any */
                if (b > vmax(c, i)) b = vmax(c, i);
                if (b < a) { int64_t t = a; a = b; b = t; }
                bl[i] = a; bh[i] = b;
            }
            continue;
        }
        int i = 0;
        for (; i < c->nv; i++) {
            if (bh[i] < vmax(c, i)) { bh[i]++; break; }
            if (bl[i] < vmax(c, i)) { bl[i]++; bh[i] = bl[i]; break; }
            bl[i] = bh[i] = vmin(c, i);
        }
        if (i == c->nv) break;
    }
    if (errs) { printf("     %s: %d failures\n", c->name, errs); n_err++; }
    else printf("ok   %s\n", c->name);
    dvs_block_alloc_destroy(ba);
}

int main(int argc, char **argv) {
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++)
        if (argc < 2 || strstr(CASES[i].name, argv[1])) run_case(&CASES[i]);
    printf("%d failing cases; %d explanations checked, %d refused; %d incomplete points;"
           " %d explanations valid only with the other own bound\n",
           n_err, n_expl, n_refused, n_incomplete, n_needs_other_own);
    return n_err ? 1 : 0;
}
