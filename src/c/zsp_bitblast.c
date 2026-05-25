#include "zsp_bitblast.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------- bump arena --------------------------------- */

#define ZSP_BB_CHUNK_SIZE 4096

typedef struct zsp_bb_chunk_s {
    struct zsp_bb_chunk_s *next;
    uint32_t used;
    uint32_t cap;
    /* flexible-array follows */
} zsp_bb_chunk_t;

struct zsp_bitblast_s {
    zsp_alloc_t    *alloc;
    zsp_aig_t      *aig;
    zsp_bb_chunk_t *chunks;
};

static void *xalloc(zsp_alloc_t *a, size_t sz) {
    return a ? ZSP_ALLOC(a, sz) : malloc(sz);
}
static void xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) ZSP_RELEASE(a, p, sz); else free(p);
}

static zsp_aig_node_t *bb_alloc_bits(zsp_bitblast_t *bb, uint32_t n) {
    uint32_t want = n * (uint32_t)sizeof(zsp_aig_node_t);
    /* Round up to alignment of 8. */
    want = (want + 7u) & ~7u;
    if (!bb->chunks || bb->chunks->cap - bb->chunks->used < want) {
        uint32_t cap = want > ZSP_BB_CHUNK_SIZE ? want : ZSP_BB_CHUNK_SIZE;
        size_t total = sizeof(zsp_bb_chunk_t) + cap;
        zsp_bb_chunk_t *c = (zsp_bb_chunk_t *)xalloc(bb->alloc, total);
        c->next = bb->chunks;
        c->used = 0;
        c->cap  = cap;
        bb->chunks = c;
    }
    uint8_t *base = (uint8_t *)(bb->chunks + 1);
    zsp_aig_node_t *out = (zsp_aig_node_t *)(base + bb->chunks->used);
    bb->chunks->used += want;
    return out;
}

static zsp_bv_t bb_alloc(zsp_bitblast_t *bb, uint32_t n) {
    zsp_bv_t r;
    r.size = n;
    r.bits = bb_alloc_bits(bb, n);
    return r;
}

/* ----------------------------- lifecycle ---------------------------------- */

zsp_bitblast_t *zsp_bitblast_new(zsp_alloc_t *alloc, zsp_aig_t *aig) {
    zsp_bitblast_t *bb = (zsp_bitblast_t *)xalloc(alloc, sizeof(*bb));
    if (!bb) return NULL;
    bb->alloc = alloc;
    bb->aig   = aig;
    bb->chunks = NULL;
    return bb;
}

void zsp_bitblast_free(zsp_bitblast_t *bb) {
    if (!bb) return;
    zsp_bb_chunk_t *c = bb->chunks;
    while (c) {
        zsp_bb_chunk_t *n = c->next;
        xfree(bb->alloc, c, sizeof(zsp_bb_chunk_t) + c->cap);
        c = n;
    }
    xfree(bb->alloc, bb, sizeof(*bb));
}

zsp_aig_t *zsp_bitblast_aig(zsp_bitblast_t *bb) { return bb->aig; }

/* ----------------------------- constants ---------------------------------- */

zsp_bv_t zsp_bb_constant(zsp_bitblast_t *bb, uint32_t size) {
    zsp_bv_t r = bb_alloc(bb, size);
    for (uint32_t i = 0; i < size; i++) r.bits[i] = zsp_aig_mk_input(bb->aig);
    return r;
}

zsp_bv_t zsp_bb_constant_dom(zsp_bitblast_t *bb, uint32_t size,
                             uint64_t fixed_value, uint64_t unknown_mask) {
    zsp_bv_t r = bb_alloc(bb, size);
    for (uint32_t i = 0; i < size; i++) {
        uint32_t bitpos = size - 1 - i;
        uint64_t bit = (bitpos < 64) ? ((uint64_t)1 << bitpos) : 0;
        if (bitpos >= 64 || (unknown_mask & bit)) {
            r.bits[i] = zsp_aig_mk_input(bb->aig);
        } else {
            r.bits[i] = (fixed_value & bit) ? ZSP_AIG_TRUE : ZSP_AIG_FALSE;
        }
    }
    return r;
}

zsp_bv_t zsp_bb_value_u64(zsp_bitblast_t *bb, uint32_t size, uint64_t value) {
    zsp_bv_t r = bb_alloc(bb, size);
    /* index 0 is MSB */
    for (uint32_t i = 0; i < size; i++) {
        uint32_t bit = size - 1 - i;
        int v = (bit < 64) ? (int)((value >> bit) & 1u) : 0;
        r.bits[i] = v ? ZSP_AIG_TRUE : ZSP_AIG_FALSE;
    }
    return r;
}

zsp_bv_t zsp_bb_value_bits(zsp_bitblast_t *bb, uint32_t size,
                           const zsp_aig_node_t *src) {
    zsp_bv_t r = bb_alloc(bb, size);
    memcpy(r.bits, src, size * sizeof(zsp_aig_node_t));
    return r;
}

/* ----------------------------- bitwise ------------------------------------ */

zsp_bv_t zsp_bb_not(zsp_bitblast_t *bb, zsp_bv_t a) {
    zsp_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++) r.bits[i] = zsp_aig_not(a.bits[i]);
    return r;
}

zsp_bv_t zsp_bb_and(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = zsp_aig_mk_and(bb->aig, a.bits[i], b.bits[i]);
    return r;
}

zsp_bv_t zsp_bb_or(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = zsp_aig_mk_or(bb->aig, a.bits[i], b.bits[i]);
    return r;
}

zsp_bv_t zsp_bb_xor(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = zsp_aig_mk_xor(bb->aig, a.bits[i], b.bits[i]);
    return r;
}

/* ----------------------------- predicates --------------------------------- */

zsp_bv_t zsp_bb_eq(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_aig_node_t res = zsp_aig_mk_iff(bb->aig, a.bits[0], b.bits[0]);
    for (uint32_t i = 1; i < a.size; i++) {
        res = zsp_aig_mk_and(bb->aig, res, zsp_aig_mk_iff(bb->aig, a.bits[i], b.bits[i]));
    }
    zsp_bv_t r = bb_alloc(bb, 1);
    r.bits[0] = res;
    return r;
}

/* Internal: produce a single AIG bit for ULT. */
static zsp_aig_node_t ult_helper(zsp_aig_t *aig, zsp_bv_t a, zsp_bv_t b) {
    uint32_t lsb = a.size - 1;
    /* a[lsb] < b[lsb] = ~a[lsb] /\ b[lsb] */
    zsp_aig_node_t res = zsp_aig_mk_and(aig, zsp_aig_not(a.bits[lsb]), b.bits[lsb]);
    for (uint32_t i = 1; i < a.size; i++) {
        uint32_t j = a.size - 1 - i;
        zsp_aig_node_t lt = zsp_aig_mk_and(aig, zsp_aig_not(a.bits[j]), b.bits[j]);
        zsp_aig_node_t not_gt =
            zsp_aig_not(zsp_aig_mk_and(aig, a.bits[j], zsp_aig_not(b.bits[j])));
        zsp_aig_node_t carry = zsp_aig_mk_and(aig, not_gt, res);
        res = zsp_aig_mk_or(aig, lt, carry);
    }
    return res;
}

zsp_bv_t zsp_bb_ult(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_bv_t r = bb_alloc(bb, 1);
    r.bits[0] = ult_helper(bb->aig, a, b);
    return r;
}

zsp_bv_t zsp_bb_slt(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_aig_t *aig = bb->aig;
    zsp_aig_node_t a_sign = a.bits[0];
    zsp_aig_node_t b_sign = b.bits[0];

    /* a[msb]=1, b[msb]=0 -> true */
    zsp_aig_node_t strict_neg = zsp_aig_mk_and(aig, a_sign, zsp_aig_not(b_sign));

    if (a.size == 1) {
        zsp_bv_t r = bb_alloc(bb, 1);
        r.bits[0] = strict_neg;
        return r;
    }

    /* sub-vector ULT */
    zsp_bv_t a_rem = zsp_bb_extract(bb, a, a.size - 2, 0);
    zsp_bv_t b_rem = zsp_bb_extract(bb, b, b.size - 2, 0);
    zsp_aig_node_t ult = ult_helper(aig, a_rem, b_rem);

    zsp_aig_node_t strict_pos = zsp_aig_mk_and(aig, zsp_aig_not(a_sign), b_sign);
    zsp_aig_node_t eq_sign = zsp_aig_mk_and(aig,
                                            zsp_aig_not(strict_neg),
                                            zsp_aig_not(strict_pos));
    zsp_aig_node_t res = zsp_aig_mk_or(aig, strict_neg,
                                       zsp_aig_mk_and(aig, eq_sign, ult));
    zsp_bv_t r = bb_alloc(bb, 1);
    r.bits[0] = res;
    return r;
}

/* ----------------------------- arithmetic --------------------------------- */

static void half_adder(zsp_aig_t *aig,
                       zsp_aig_node_t a, zsp_aig_node_t b,
                       zsp_aig_node_t *sum_out, zsp_aig_node_t *cout_out) {
    zsp_aig_node_t a_and_b = zsp_aig_mk_and(aig, a, b);
    zsp_aig_node_t a_or_b  = zsp_aig_mk_or(aig, a, b);
    *sum_out = zsp_aig_mk_and(aig, zsp_aig_not(a_and_b), a_or_b);
    *cout_out = a_and_b;
}

static void full_adder(zsp_aig_t *aig,
                       zsp_aig_node_t a, zsp_aig_node_t b, zsp_aig_node_t cin,
                       zsp_aig_node_t *sum_out, zsp_aig_node_t *cout_out) {
    zsp_aig_node_t s1, c1, s2, c2;
    half_adder(aig, a, b, &s1, &c1);
    half_adder(aig, s1, cin, &s2, &c2);
    *sum_out = s2;
    *cout_out = zsp_aig_mk_or(aig, c1, c2);
}

zsp_bv_t zsp_bb_add(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_aig_t *aig = bb->aig;
    zsp_bv_t r = bb_alloc(bb, a.size);
    uint32_t size = a.size;
    zsp_aig_node_t cout;
    half_adder(aig, a.bits[size - 1], b.bits[size - 1], &r.bits[size - 1], &cout);
    for (uint32_t i = 1; i < size; i++) {
        uint32_t j = size - 1 - i;
        full_adder(aig, a.bits[j], b.bits[j], cout, &r.bits[j], &cout);
    }
    return r;
}

zsp_bv_t zsp_bb_neg(zsp_bitblast_t *bb, zsp_bv_t a) {
    /* -a = ~a + 1 */
    zsp_bv_t na = zsp_bb_not(bb, a);
    zsp_bv_t one = zsp_bb_value_u64(bb, a.size, 1);
    return zsp_bb_add(bb, na, one);
}

zsp_bv_t zsp_bb_sub(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    return zsp_bb_add(bb, a, zsp_bb_neg(bb, b));
}

zsp_bv_t zsp_bb_mul(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_aig_t *aig = bb->aig;
    uint32_t size = a.size;
    zsp_bv_t r = bb_alloc(bb, size);
    /* initial partial product: a /\ b[lsb] */
    for (uint32_t i = 0; i < size; i++) {
        r.bits[i] = zsp_aig_mk_and(aig, a.bits[i], b.bits[size - 1]);
    }
    for (uint32_t i = 1; i < size; i++) {
        uint32_t ib = size - 1 - i;
        zsp_aig_node_t b_bit = b.bits[ib];
        if (b_bit == ZSP_AIG_FALSE) continue;
        zsp_aig_node_t cout;
        half_adder(aig, r.bits[ib],
                   zsp_aig_mk_and(aig, a.bits[size - 1], b_bit),
                   &r.bits[ib], &cout);
        uint32_t ir = ib - 1;
        uint32_t ia = size - 2;
        for (uint32_t j = 1; j <= ib; j++) {
            if (a.bits[ia] == ZSP_AIG_FALSE && cout == ZSP_AIG_FALSE) {
                ir--;
                ia--;
                continue;
            }
            full_adder(aig, r.bits[ir],
                       zsp_aig_mk_and(aig, a.bits[ia], b_bit), cout,
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

static zsp_bv_t do_shl_or_shr(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b, int is_shl) {
    assert(a.size == b.size);
    zsp_aig_t *aig = bb->aig;
    uint32_t size = a.size;
    if (size == 1) {
        zsp_bv_t r = bb_alloc(bb, 1);
        /* size 1: a if b=0, 0 if b=1 — same recipe for shl and shr */
        r.bits[0] = zsp_aig_mk_and(aig, a.bits[0], zsp_aig_not(b.bits[0]));
        return r;
    }
    uint32_t shift_size = ceil_log2(b.size);
    if (shift_size > b.size) shift_size = b.size;

    /* Working copy */
    zsp_bv_t work = zsp_bb_value_bits(bb, size, a.bits);

    for (uint32_t i = 0; i < shift_size; i++) {
        uint32_t shift_step = 1u << i;
        uint32_t shift_bit  = b.size - 1 - i;
        assert(shift_step < size);
        if (is_shl) {
            for (uint32_t j = 0; j + shift_step < size; j++) {
                work.bits[j] = zsp_aig_mk_ite(aig, b.bits[shift_bit],
                                              work.bits[j + shift_step],
                                              work.bits[j]);
            }
            zsp_aig_node_t not_s = zsp_aig_not(b.bits[shift_bit]);
            for (uint32_t j = size - shift_step; j < size; j++) {
                work.bits[j] = zsp_aig_mk_and(aig, not_s, work.bits[j]);
            }
        } else {
            for (uint32_t j = 0, k = size - 1; j + shift_step < size; j++, k--) {
                work.bits[k] = zsp_aig_mk_ite(aig, b.bits[shift_bit],
                                              work.bits[k - shift_step],
                                              work.bits[k]);
            }
            zsp_aig_node_t not_s = zsp_aig_not(b.bits[shift_bit]);
            for (uint32_t j = 0; j < shift_step; j++) {
                work.bits[j] = zsp_aig_mk_and(aig, not_s, work.bits[j]);
            }
        }
    }

    /* if (b >= size) result is zero, else result is `work` */
    zsp_bv_t size_const = zsp_bb_value_u64(bb, b.size, size);
    zsp_aig_node_t in_range = ult_helper(aig, b, size_const);
    zsp_bv_t zero = zsp_bb_value_u64(bb, size, 0);
    return zsp_bb_ite(bb, in_range, work, zero);
}

zsp_bv_t zsp_bb_shl(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    return do_shl_or_shr(bb, a, b, /*is_shl=*/1);
}

zsp_bv_t zsp_bb_shr(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    return do_shl_or_shr(bb, a, b, /*is_shl=*/0);
}

zsp_bv_t zsp_bb_ashr(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_aig_t *aig = bb->aig;
    uint32_t size = a.size;
    if (size == 1) {
        /* size-1 arithmetic shift right is identity. */
        return zsp_bb_value_bits(bb, 1, a.bits);
    }
    uint32_t shift_size = ceil_log2(b.size);
    if (shift_size > b.size) shift_size = b.size;
    zsp_bv_t work = zsp_bb_value_bits(bb, size, a.bits);
    for (uint32_t i = 0; i < shift_size; i++) {
        uint32_t shift_step = 1u << i;
        uint32_t shift_bit  = b.size - 1 - i;
        assert(shift_step < size);
        for (uint32_t j = 0, k = size - 1; j + shift_step < size; j++, k--) {
            work.bits[k] = zsp_aig_mk_ite(aig, b.bits[shift_bit],
                                          work.bits[k - shift_step], work.bits[k]);
        }
        /* fill from sign bit (work.bits[0]) instead of zero */
        for (uint32_t j = 0; j < shift_step; j++) {
            work.bits[j] = zsp_aig_mk_ite(aig, b.bits[shift_bit],
                                          work.bits[0], work.bits[j]);
        }
    }
    /* if b >= size, result is the sign bit replicated */
    zsp_bv_t size_const = zsp_bb_value_u64(bb, b.size, size);
    zsp_aig_node_t in_range = ult_helper(aig, b, size_const);
    for (uint32_t i = 0; i < size; i++) {
        work.bits[i] = zsp_aig_mk_ite(aig, in_range, work.bits[i], a.bits[0]);
    }
    return work;
}

/* ----------------------------- divider ------------------------------------ */

static zsp_aig_node_t fa_div_carry(zsp_aig_t *aig,
                                   zsp_aig_node_t r,
                                   zsp_aig_node_t d,
                                   zsp_aig_node_t c) {
    return zsp_aig_mk_or(aig,
                         zsp_aig_mk_and(aig, zsp_aig_mk_or(aig, d, c), r),
                         zsp_aig_mk_and(aig, d, c));
}

static zsp_aig_node_t fa_div_sum(zsp_aig_t *aig,
                                 zsp_aig_node_t r,
                                 zsp_aig_node_t d,
                                 zsp_aig_node_t c,
                                 zsp_aig_node_t q) {
    /* mk_xor(mk_and(mk_xor(d,c), q), r) */
    zsp_aig_node_t dc = zsp_aig_mk_xor(aig, d, c);
    zsp_aig_node_t dq = zsp_aig_mk_and(aig, dc, q);
    return zsp_aig_mk_xor(aig, dq, r);
}

/* Returns quotient (rem optional). Both output buffers must have a.size cells. */
static void udiv_urem_helper(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b,
                              zsp_bv_t *quot_out, zsp_bv_t *rem_out) {
    assert(a.size == b.size);
    zsp_aig_t *aig = bb->aig;
    uint32_t size = a.size;

    /* Prepare divisor reversed and negated (~b reversed). */
    zsp_aig_node_t *d = bb_alloc_bits(bb, size);
    for (uint32_t i = 0; i < size; i++) {
        d[i] = zsp_aig_not(b.bits[size - 1 - i]);
    }

    zsp_aig_node_t *rem   = bb_alloc_bits(bb, size + 1);
    zsp_aig_node_t *carry = bb_alloc_bits(bb, size + 1);
    for (uint32_t i = 0; i <= size; i++) rem[i] = ZSP_AIG_FALSE;

    zsp_bv_t q = bb_alloc(bb, size);
    uint32_t q_count = 0;

    for (uint32_t i = 0; i < size; i++) {
        rem[0] = a.bits[i];
        carry[0] = ZSP_AIG_TRUE;
        for (uint32_t j = 0; j < size; j++) {
            carry[j + 1] = fa_div_carry(aig, rem[j], d[j], carry[j]);
        }
        q.bits[q_count++] = carry[size];

        zsp_aig_node_t prev_r = rem[0];
        zsp_aig_node_t qi = carry[size];
        for (uint32_t j = 0; j < size; j++) {
            zsp_aig_node_t tmp = fa_div_sum(aig, prev_r, d[j], carry[j], qi);
            prev_r = rem[j + 1];
            rem[j + 1] = tmp;
        }
    }
    *quot_out = q;

    if (rem_out) {
        zsp_bv_t r = bb_alloc(bb, size);
        for (uint32_t i = 0; i < size; i++) r.bits[i] = rem[size - i];
        *rem_out = r;
    }
}

zsp_bv_t zsp_bb_udiv(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    zsp_bv_t q, r;
    udiv_urem_helper(bb, a, b, &q, &r);
    return q;
}

zsp_bv_t zsp_bb_urem(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    zsp_bv_t q, r;
    udiv_urem_helper(bb, a, b, &q, &r);
    return r;
}

/* ----------------------------- structural --------------------------------- */

zsp_bv_t zsp_bb_extract(zsp_bitblast_t *bb, zsp_bv_t a,
                        uint32_t upper, uint32_t lower) {
    assert(lower <= upper);
    assert(upper < a.size);
    uint32_t n = upper - lower + 1;
    zsp_bv_t r = bb_alloc(bb, n);
    /* a is MSB-first: bit index in `a` for value-bit `k` is (a.size - 1 - k).
     * We want bits [upper..lower], MSB-first in result. */
    uint32_t start = a.size - 1 - upper;
    memcpy(r.bits, a.bits + start, n * sizeof(zsp_aig_node_t));
    return r;
}

zsp_bv_t zsp_bb_concat(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b) {
    zsp_bv_t r = bb_alloc(bb, a.size + b.size);
    memcpy(r.bits, a.bits, a.size * sizeof(zsp_aig_node_t));
    memcpy(r.bits + a.size, b.bits, b.size * sizeof(zsp_aig_node_t));
    return r;
}

zsp_bv_t zsp_bb_zero_ext(zsp_bitblast_t *bb, zsp_bv_t a, uint32_t n) {
    if (n == 0) return zsp_bb_value_bits(bb, a.size, a.bits);
    zsp_bv_t r = bb_alloc(bb, a.size + n);
    for (uint32_t i = 0; i < n; i++) r.bits[i] = ZSP_AIG_FALSE;
    memcpy(r.bits + n, a.bits, a.size * sizeof(zsp_aig_node_t));
    return r;
}

zsp_bv_t zsp_bb_sign_ext(zsp_bitblast_t *bb, zsp_bv_t a, uint32_t n) {
    if (n == 0) return zsp_bb_value_bits(bb, a.size, a.bits);
    zsp_bv_t r = bb_alloc(bb, a.size + n);
    zsp_aig_node_t sign = a.bits[0];
    for (uint32_t i = 0; i < n; i++) r.bits[i] = sign;
    memcpy(r.bits + n, a.bits, a.size * sizeof(zsp_aig_node_t));
    return r;
}

zsp_bv_t zsp_bb_ite(zsp_bitblast_t *bb, zsp_aig_node_t cond,
                    zsp_bv_t a, zsp_bv_t b) {
    assert(a.size == b.size);
    zsp_bv_t r = bb_alloc(bb, a.size);
    for (uint32_t i = 0; i < a.size; i++)
        r.bits[i] = zsp_aig_mk_ite(bb->aig, cond, a.bits[i], b.bits[i]);
    return r;
}
