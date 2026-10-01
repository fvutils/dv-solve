#include "dvs_bitblast.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------- bump arena --------------------------------- */

#define DVS_BB_CHUNK_SIZE 4096

typedef struct dvs_bb_chunk_s {
    struct dvs_bb_chunk_s *next;
    uint32_t used;
    uint32_t cap;
    /* flexible-array follows */
} dvs_bb_chunk_t;

struct dvs_bitblast_s {
    dvs_alloc_t    *alloc;
    dvs_aig_t      *aig;
    dvs_bb_chunk_t *chunks;
};

static void *xalloc(dvs_alloc_t *a, size_t sz) {
    return a ? DVS_ALLOC(a, sz) : malloc(sz);
}
static void xfree(dvs_alloc_t *a, void *p, size_t sz) {
    if (a) DVS_RELEASE(a, p, sz); else free(p);
}

static dvs_aig_node_t *bb_alloc_bits(dvs_bitblast_t *bb, uint32_t n) {
    uint32_t want = n * (uint32_t)sizeof(dvs_aig_node_t);
    /* Round up to alignment of 8. */
    want = (want + 7u) & ~7u;
    if (!bb->chunks || bb->chunks->cap - bb->chunks->used < want) {
        uint32_t cap = want > DVS_BB_CHUNK_SIZE ? want : DVS_BB_CHUNK_SIZE;
        size_t total = sizeof(dvs_bb_chunk_t) + cap;
        dvs_bb_chunk_t *c = (dvs_bb_chunk_t *)xalloc(bb->alloc, total);
        c->next = bb->chunks;
        c->used = 0;
        c->cap  = cap;
        bb->chunks = c;
    }
    uint8_t *base = (uint8_t *)(bb->chunks + 1);
    dvs_aig_node_t *out = (dvs_aig_node_t *)(base + bb->chunks->used);
    bb->chunks->used += want;
    return out;
}

static dvs_bv_t bb_alloc(dvs_bitblast_t *bb, uint32_t n) {
    dvs_bv_t r;
    r.size = n;
    r.bits = bb_alloc_bits(bb, n);
    return r;
}

/* ----------------------------- lifecycle ---------------------------------- */

dvs_bitblast_t *dvs_bitblast_new(dvs_alloc_t *alloc, dvs_aig_t *aig) {
    dvs_bitblast_t *bb = (dvs_bitblast_t *)xalloc(alloc, sizeof(*bb));
    if (!bb) return NULL;
    bb->alloc = alloc;
    bb->aig   = aig;
    bb->chunks = NULL;
    return bb;
}

void dvs_bitblast_free(dvs_bitblast_t *bb) {
    if (!bb) return;
    dvs_bb_chunk_t *c = bb->chunks;
    while (c) {
        dvs_bb_chunk_t *n = c->next;
        xfree(bb->alloc, c, sizeof(dvs_bb_chunk_t) + c->cap);
        c = n;
    }
    xfree(bb->alloc, bb, sizeof(*bb));
}

dvs_aig_t *dvs_bitblast_aig(dvs_bitblast_t *bb) { return bb->aig; }

/* ----------------------------- constants ---------------------------------- */

dvs_bv_t dvs_bb_constant(dvs_bitblast_t *bb, uint32_t size) {
    dvs_bv_t r = bb_alloc(bb, size);
    for (uint32_t i = 0; i < size; i++) r.bits[i] = dvs_aig_mk_input(bb->aig);
    return r;
}

dvs_bv_t dvs_bb_constant_dom(dvs_bitblast_t *bb, uint32_t size,
                             uint64_t fixed_value, uint64_t unknown_mask) {
    dvs_bv_t r = bb_alloc(bb, size);
    for (uint32_t i = 0; i < size; i++) {
        uint32_t bitpos = size - 1 - i;
        uint64_t bit = (bitpos < 64) ? ((uint64_t)1 << bitpos) : 0;
        if (bitpos >= 64 || (unknown_mask & bit)) {
            r.bits[i] = dvs_aig_mk_input(bb->aig);
        } else {
            r.bits[i] = (fixed_value & bit) ? DVS_AIG_TRUE : DVS_AIG_FALSE;
        }
    }
    return r;
}

dvs_bv_t dvs_bb_value_u64(dvs_bitblast_t *bb, uint32_t size, uint64_t value) {
    dvs_bv_t r = bb_alloc(bb, size);
    /* index 0 is MSB */
    for (uint32_t i = 0; i < size; i++) {
        uint32_t bit = size - 1 - i;
        int v = (bit < 64) ? (int)((value >> bit) & 1u) : 0;
        r.bits[i] = v ? DVS_AIG_TRUE : DVS_AIG_FALSE;
    }
    return r;
}

dvs_bv_t dvs_bb_value_bits(dvs_bitblast_t *bb, uint32_t size,
                           const dvs_aig_node_t *src) {
    dvs_bv_t r = bb_alloc(bb, size);
    memcpy(r.bits, src, size * sizeof(dvs_aig_node_t));
    return r;
}

/* ----------------------------- bitwise ------------------------------------ */

dvs_bv_t dvs_bb_not(dvs_bitblast_t *bb, dvs_bv_t a) {
    dvs_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++) r.bits[i] = dvs_aig_not(a.bits[i]);
    return r;
}

dvs_bv_t dvs_bb_and(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = dvs_aig_mk_and(bb->aig, a.bits[i], b.bits[i]);
    return r;
}

dvs_bv_t dvs_bb_or(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = dvs_aig_mk_or(bb->aig, a.bits[i], b.bits[i]);
    return r;
}

dvs_bv_t dvs_bb_xor(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = dvs_aig_mk_xor(bb->aig, a.bits[i], b.bits[i]);
    return r;
}

/* ----------------------------- predicates --------------------------------- */

dvs_bv_t dvs_bb_eq(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_aig_node_t res = dvs_aig_mk_iff(bb->aig, a.bits[0], b.bits[0]);
    for (uint32_t i = 1; i < a.size; i++) {
        res = dvs_aig_mk_and(bb->aig, res, dvs_aig_mk_iff(bb->aig, a.bits[i], b.bits[i]));
    }
    dvs_bv_t r = bb_alloc(bb, 1);
    r.bits[0] = res;
    return r;
}

/* Internal: produce a single AIG bit for ULT. */
static dvs_aig_node_t ult_helper(dvs_aig_t *aig, dvs_bv_t a, dvs_bv_t b) {
    uint32_t lsb = a.size - 1;
    /* a[lsb] < b[lsb] = ~a[lsb] /\ b[lsb] */
    dvs_aig_node_t res = dvs_aig_mk_and(aig, dvs_aig_not(a.bits[lsb]), b.bits[lsb]);
    for (uint32_t i = 1; i < a.size; i++) {
        uint32_t j = a.size - 1 - i;
        dvs_aig_node_t lt = dvs_aig_mk_and(aig, dvs_aig_not(a.bits[j]), b.bits[j]);
        dvs_aig_node_t not_gt =
            dvs_aig_not(dvs_aig_mk_and(aig, a.bits[j], dvs_aig_not(b.bits[j])));
        dvs_aig_node_t carry = dvs_aig_mk_and(aig, not_gt, res);
        res = dvs_aig_mk_or(aig, lt, carry);
    }
    return res;
}

dvs_bv_t dvs_bb_ult(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_bv_t r = bb_alloc(bb, 1);
    r.bits[0] = ult_helper(bb->aig, a, b);
    return r;
}

dvs_bv_t dvs_bb_slt(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_aig_t *aig = bb->aig;
    dvs_aig_node_t a_sign = a.bits[0];
    dvs_aig_node_t b_sign = b.bits[0];

    /* a[msb]=1, b[msb]=0 -> true */
    dvs_aig_node_t strict_neg = dvs_aig_mk_and(aig, a_sign, dvs_aig_not(b_sign));

    if (a.size == 1) {
        dvs_bv_t r = bb_alloc(bb, 1);
        r.bits[0] = strict_neg;
        return r;
    }

    /* sub-vector ULT */
    dvs_bv_t a_rem = dvs_bb_extract(bb, a, a.size - 2, 0);
    dvs_bv_t b_rem = dvs_bb_extract(bb, b, b.size - 2, 0);
    dvs_aig_node_t ult = ult_helper(aig, a_rem, b_rem);

    dvs_aig_node_t strict_pos = dvs_aig_mk_and(aig, dvs_aig_not(a_sign), b_sign);
    dvs_aig_node_t eq_sign = dvs_aig_mk_and(aig,
                                            dvs_aig_not(strict_neg),
                                            dvs_aig_not(strict_pos));
    dvs_aig_node_t res = dvs_aig_mk_or(aig, strict_neg,
                                       dvs_aig_mk_and(aig, eq_sign, ult));
    dvs_bv_t r = bb_alloc(bb, 1);
    r.bits[0] = res;
    return r;
}

/* ----------------------------- arithmetic --------------------------------- */

static void half_adder(dvs_aig_t *aig,
                       dvs_aig_node_t a, dvs_aig_node_t b,
                       dvs_aig_node_t *sum_out, dvs_aig_node_t *cout_out) {
    dvs_aig_node_t a_and_b = dvs_aig_mk_and(aig, a, b);
    dvs_aig_node_t a_or_b  = dvs_aig_mk_or(aig, a, b);
    *sum_out = dvs_aig_mk_and(aig, dvs_aig_not(a_and_b), a_or_b);
    *cout_out = a_and_b;
}

static void full_adder(dvs_aig_t *aig,
                       dvs_aig_node_t a, dvs_aig_node_t b, dvs_aig_node_t cin,
                       dvs_aig_node_t *sum_out, dvs_aig_node_t *cout_out) {
    dvs_aig_node_t s1, c1, s2, c2;
    half_adder(aig, a, b, &s1, &c1);
    half_adder(aig, s1, cin, &s2, &c2);
    *sum_out = s2;
    *cout_out = dvs_aig_mk_or(aig, c1, c2);
}

dvs_bv_t dvs_bb_add(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_aig_t *aig = bb->aig;
    dvs_bv_t r = bb_alloc(bb, a.size);
    uint32_t size = a.size;
    dvs_aig_node_t cout;
    half_adder(aig, a.bits[size - 1], b.bits[size - 1], &r.bits[size - 1], &cout);
    for (uint32_t i = 1; i < size; i++) {
        uint32_t j = size - 1 - i;
        full_adder(aig, a.bits[j], b.bits[j], cout, &r.bits[j], &cout);
    }
    return r;
}

dvs_bv_t dvs_bb_neg(dvs_bitblast_t *bb, dvs_bv_t a) {
    /* -a = ~a + 1 */
    dvs_bv_t na = dvs_bb_not(bb, a);
    dvs_bv_t one = dvs_bb_value_u64(bb, a.size, 1);
    return dvs_bb_add(bb, na, one);
}

dvs_bv_t dvs_bb_sub(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    return dvs_bb_add(bb, a, dvs_bb_neg(bb, b));
}

dvs_bv_t dvs_bb_mul(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_aig_t *aig = bb->aig;
    uint32_t size = a.size;
    dvs_bv_t r = bb_alloc(bb, size);
    /* initial partial product: a /\ b[lsb] */
    for (uint32_t i = 0; i < size; i++) {
        r.bits[i] = dvs_aig_mk_and(aig, a.bits[i], b.bits[size - 1]);
    }
    for (uint32_t i = 1; i < size; i++) {
        uint32_t ib = size - 1 - i;
        dvs_aig_node_t b_bit = b.bits[ib];
        if (b_bit == DVS_AIG_FALSE) continue;
        dvs_aig_node_t cout;
        half_adder(aig, r.bits[ib],
                   dvs_aig_mk_and(aig, a.bits[size - 1], b_bit),
                   &r.bits[ib], &cout);
        uint32_t ir = ib - 1;
        uint32_t ia = size - 2;
        for (uint32_t j = 1; j <= ib; j++) {
            if (a.bits[ia] == DVS_AIG_FALSE && cout == DVS_AIG_FALSE) {
                ir--;
                ia--;
                continue;
            }
            full_adder(aig, r.bits[ir],
                       dvs_aig_mk_and(aig, a.bits[ia], b_bit), cout,
                       &r.bits[ir], &cout);
            if (ir == 0) break;
            ir--;
            ia--;
        }
    }
    return r;
}

/* ----------------------------- shifts ------------------------------------- */

static uint32_t ceil_log2(uint32_t n) {
    uint32_t r = 0;
    uint32_t v = 1;
    while (v < n) { v <<= 1; r++; }
    return r;
}

static dvs_bv_t do_shl_or_shr(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b, int is_shl) {
    assert(a.size == b.size);
    dvs_aig_t *aig = bb->aig;
    uint32_t size = a.size;
    if (size == 1) {
        dvs_bv_t r = bb_alloc(bb, 1);
        /* size 1: a if b=0, 0 if b=1 — same recipe for shl and shr */
        r.bits[0] = dvs_aig_mk_and(aig, a.bits[0], dvs_aig_not(b.bits[0]));
        return r;
    }
    uint32_t shift_size = ceil_log2(b.size);
    if (shift_size > b.size) shift_size = b.size;

    /* Working copy */
    dvs_bv_t work = dvs_bb_value_bits(bb, size, a.bits);

    for (uint32_t i = 0; i < shift_size; i++) {
        uint32_t shift_step = 1u << i;
        uint32_t shift_bit  = b.size - 1 - i;
        assert(shift_step < size);
        if (is_shl) {
            for (uint32_t j = 0; j + shift_step < size; j++) {
                work.bits[j] = dvs_aig_mk_ite(aig, b.bits[shift_bit],
                                              work.bits[j + shift_step],
                                              work.bits[j]);
            }
            dvs_aig_node_t not_s = dvs_aig_not(b.bits[shift_bit]);
            for (uint32_t j = size - shift_step; j < size; j++) {
                work.bits[j] = dvs_aig_mk_and(aig, not_s, work.bits[j]);
            }
        } else {
            for (uint32_t j = 0, k = size - 1; j + shift_step < size; j++, k--) {
                work.bits[k] = dvs_aig_mk_ite(aig, b.bits[shift_bit],
                                              work.bits[k - shift_step],
                                              work.bits[k]);
            }
            dvs_aig_node_t not_s = dvs_aig_not(b.bits[shift_bit]);
            for (uint32_t j = 0; j < shift_step; j++) {
                work.bits[j] = dvs_aig_mk_and(aig, not_s, work.bits[j]);
            }
        }
    }

    /* if (b >= size) result is zero, else result is `work` */
    dvs_bv_t size_const = dvs_bb_value_u64(bb, b.size, size);
    dvs_aig_node_t in_range = ult_helper(aig, b, size_const);
    dvs_bv_t zero = dvs_bb_value_u64(bb, size, 0);
    return dvs_bb_ite(bb, in_range, work, zero);
}

dvs_bv_t dvs_bb_shl(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    return do_shl_or_shr(bb, a, b, /*is_shl=*/1);
}

dvs_bv_t dvs_bb_shr(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    return do_shl_or_shr(bb, a, b, /*is_shl=*/0);
}

dvs_bv_t dvs_bb_ashr(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_aig_t *aig = bb->aig;
    uint32_t size = a.size;
    if (size == 1) {
        /* size-1 arithmetic shift right is identity. */
        return dvs_bb_value_bits(bb, 1, a.bits);
    }
    uint32_t shift_size = ceil_log2(b.size);
    if (shift_size > b.size) shift_size = b.size;
    dvs_bv_t work = dvs_bb_value_bits(bb, size, a.bits);
    for (uint32_t i = 0; i < shift_size; i++) {
        uint32_t shift_step = 1u << i;
        uint32_t shift_bit  = b.size - 1 - i;
        assert(shift_step < size);
        for (uint32_t j = 0, k = size - 1; j + shift_step < size; j++, k--) {
            work.bits[k] = dvs_aig_mk_ite(aig, b.bits[shift_bit],
                                          work.bits[k - shift_step], work.bits[k]);
        }
        /* fill from sign bit (work.bits[0]) instead of zero */
        for (uint32_t j = 0; j < shift_step; j++) {
            work.bits[j] = dvs_aig_mk_ite(aig, b.bits[shift_bit],
                                          work.bits[0], work.bits[j]);
        }
    }
    /* if b >= size, result is the sign bit replicated */
    dvs_bv_t size_const = dvs_bb_value_u64(bb, b.size, size);
    dvs_aig_node_t in_range = ult_helper(aig, b, size_const);
    for (uint32_t i = 0; i < size; i++) {
        work.bits[i] = dvs_aig_mk_ite(aig, in_range, work.bits[i], a.bits[0]);
    }
    return work;
}

/* ----------------------------- divider ------------------------------------ */

static dvs_aig_node_t fa_div_carry(dvs_aig_t *aig,
                                   dvs_aig_node_t r,
                                   dvs_aig_node_t d,
                                   dvs_aig_node_t c) {
    return dvs_aig_mk_or(aig,
                         dvs_aig_mk_and(aig, dvs_aig_mk_or(aig, d, c), r),
                         dvs_aig_mk_and(aig, d, c));
}

static dvs_aig_node_t fa_div_sum(dvs_aig_t *aig,
                                 dvs_aig_node_t r,
                                 dvs_aig_node_t d,
                                 dvs_aig_node_t c,
                                 dvs_aig_node_t q) {
    /* mk_xor(mk_and(mk_xor(d,c), q), r) */
    dvs_aig_node_t dc = dvs_aig_mk_xor(aig, d, c);
    dvs_aig_node_t dq = dvs_aig_mk_and(aig, dc, q);
    return dvs_aig_mk_xor(aig, dq, r);
}

/* Returns quotient (rem optional). Both output buffers must have a.size cells. */
static void udiv_urem_helper(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b,
                              dvs_bv_t *quot_out, dvs_bv_t *rem_out) {
    assert(a.size == b.size);
    dvs_aig_t *aig = bb->aig;
    uint32_t size = a.size;

    /* Prepare divisor reversed and negated (~b reversed). */
    dvs_aig_node_t *d = bb_alloc_bits(bb, size);
    for (uint32_t i = 0; i < size; i++) {
        d[i] = dvs_aig_not(b.bits[size - 1 - i]);
    }

    dvs_aig_node_t *rem   = bb_alloc_bits(bb, size + 1);
    dvs_aig_node_t *carry = bb_alloc_bits(bb, size + 1);
    for (uint32_t i = 0; i <= size; i++) rem[i] = DVS_AIG_FALSE;

    dvs_bv_t q = bb_alloc(bb, size);
    uint32_t q_count = 0;

    for (uint32_t i = 0; i < size; i++) {
        rem[0] = a.bits[i];
        carry[0] = DVS_AIG_TRUE;
        for (uint32_t j = 0; j < size; j++) {
            carry[j + 1] = fa_div_carry(aig, rem[j], d[j], carry[j]);
        }
        q.bits[q_count++] = carry[size];

        dvs_aig_node_t prev_r = rem[0];
        dvs_aig_node_t qi = carry[size];
        for (uint32_t j = 0; j < size; j++) {
            dvs_aig_node_t tmp = fa_div_sum(aig, prev_r, d[j], carry[j], qi);
            prev_r = rem[j + 1];
            rem[j + 1] = tmp;
        }
    }
    *quot_out = q;

    if (rem_out) {
        dvs_bv_t r = bb_alloc(bb, size);
        for (uint32_t i = 0; i < size; i++) r.bits[i] = rem[size - i];
        *rem_out = r;
    }
}

dvs_bv_t dvs_bb_udiv(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    dvs_bv_t q, r;
    udiv_urem_helper(bb, a, b, &q, &r);
    return q;
}

dvs_bv_t dvs_bb_urem(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    dvs_bv_t q, r;
    udiv_urem_helper(bb, a, b, &q, &r);
    return r;
}

/* ----------------------------- structural --------------------------------- */

dvs_bv_t dvs_bb_extract(dvs_bitblast_t *bb, dvs_bv_t a,
                        uint32_t upper, uint32_t lower) {
    assert(lower <= upper);
    assert(upper < a.size);
    uint32_t n = upper - lower + 1;
    dvs_bv_t r = bb_alloc(bb, n);
    /* a is MSB-first: bit index in `a` for value-bit `k` is (a.size - 1 - k).
     * We want bits [upper..lower], MSB-first in result. */
    uint32_t start = a.size - 1 - upper;
    memcpy(r.bits, a.bits + start, n * sizeof(dvs_aig_node_t));
    return r;
}

dvs_bv_t dvs_bb_concat(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    dvs_bv_t r = bb_alloc(bb, a.size + b.size);
    memcpy(r.bits, a.bits, a.size * sizeof(dvs_aig_node_t));
    memcpy(r.bits + a.size, b.bits, b.size * sizeof(dvs_aig_node_t));
    return r;
}

dvs_bv_t dvs_bb_zero_ext(dvs_bitblast_t *bb, dvs_bv_t a, uint32_t n) {
    if (n == 0) return dvs_bb_value_bits(bb, a.size, a.bits);
    dvs_bv_t r = bb_alloc(bb, a.size + n);
    for (uint32_t i = 0; i < n; i++) r.bits[i] = DVS_AIG_FALSE;
    memcpy(r.bits + n, a.bits, a.size * sizeof(dvs_aig_node_t));
    return r;
}

dvs_bv_t dvs_bb_sign_ext(dvs_bitblast_t *bb, dvs_bv_t a, uint32_t n) {
    if (n == 0) return dvs_bb_value_bits(bb, a.size, a.bits);
    dvs_bv_t r = bb_alloc(bb, a.size + n);
    dvs_aig_node_t sign = a.bits[0];
    for (uint32_t i = 0; i < n; i++) r.bits[i] = sign;
    memcpy(r.bits + n, a.bits, a.size * sizeof(dvs_aig_node_t));
    return r;
}

dvs_bv_t dvs_bb_ite(dvs_bitblast_t *bb, dvs_aig_node_t cond,
                    dvs_bv_t a, dvs_bv_t b) {
    assert(a.size == b.size);
    dvs_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = dvs_aig_mk_ite(bb->aig, cond, a.bits[i], b.bits[i]);
    return r;
}
