#include "zsp_bvdom.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

/* ---- core ---- */

void zsp_bvdom_init_unknown(zsp_bvdom_t *d, uint8_t w) {
    d->width = w;
    d->lo = 0;
    d->hi = zsp_bvdom_mask(w);
}

void zsp_bvdom_init_value(zsp_bvdom_t *d, uint8_t w, uint64_t v) {
    uint64_t m = zsp_bvdom_mask(w);
    d->width = w;
    d->lo = v & m;
    d->hi = v & m;
}

void zsp_bvdom_init_lohi(zsp_bvdom_t *d, uint8_t w, uint64_t lo, uint64_t hi) {
    uint64_t m = zsp_bvdom_mask(w);
    d->width = w;
    d->lo = lo & m;
    d->hi = hi & m;
}

int zsp_bvdom_is_valid(const zsp_bvdom_t *d) {
    uint64_t m = zsp_bvdom_mask(d->width);
    return ((d->lo & ~d->hi) & m) == 0;
}

int zsp_bvdom_is_fixed(const zsp_bvdom_t *d) {
    uint64_t m = zsp_bvdom_mask(d->width);
    return (d->lo & m) == (d->hi & m);
}

int zsp_bvdom_has_fixed_bits(const zsp_bvdom_t *d) {
    uint64_t m = zsp_bvdom_mask(d->width);
    /* fixed bits are those where lo == hi. fixed-1 = lo & hi. fixed-0 = ~lo & ~hi.
     * fixed = (lo & hi) | (~lo & ~hi). non-fixed bit = lo XOR hi. */
    uint64_t fixed = ~(d->lo ^ d->hi) & m;
    return fixed != 0;
}

int zsp_bvdom_has_fixed_bits_true(const zsp_bvdom_t *d) {
    uint64_t m = zsp_bvdom_mask(d->width);
    return (d->lo & d->hi & m) != 0;
}

int zsp_bvdom_has_fixed_bits_false(const zsp_bvdom_t *d) {
    uint64_t m = zsp_bvdom_mask(d->width);
    return (~d->lo & ~d->hi & m) != 0;
}

int zsp_bvdom_is_fixed_bit(const zsp_bvdom_t *d, uint8_t idx) {
    if (idx >= d->width) return 0;
    uint64_t b = (uint64_t)1 << idx;
    return ((d->lo & b) != 0) == ((d->hi & b) != 0);
}

int zsp_bvdom_is_fixed_bit_true(const zsp_bvdom_t *d, uint8_t idx) {
    if (idx >= d->width) return 0;
    uint64_t b = (uint64_t)1 << idx;
    return (d->lo & b) && (d->hi & b);
}

int zsp_bvdom_is_fixed_bit_false(const zsp_bvdom_t *d, uint8_t idx) {
    if (idx >= d->width) return 0;
    uint64_t b = (uint64_t)1 << idx;
    return !(d->lo & b) && !(d->hi & b);
}

void zsp_bvdom_fix_bit(zsp_bvdom_t *d, uint8_t idx, int v) {
    if (idx >= d->width) return;
    uint64_t b = (uint64_t)1 << idx;
    if (v) { d->lo |= b; d->hi |= b; }
    else   { d->lo &= ~b; d->hi &= ~b; }
}

void zsp_bvdom_fix(zsp_bvdom_t *d, uint64_t v) {
    uint64_t m = zsp_bvdom_mask(d->width);
    d->lo = v & m;
    d->hi = v & m;
}

int zsp_bvdom_match(const zsp_bvdom_t *d, uint64_t v) {
    uint64_t m = zsp_bvdom_mask(d->width);
    /* For each fixed bit, value must match lo (== hi). The fixed-bit
     * mask is ~(lo XOR hi) & m. The constraint is
     *   (fixed & lo) == (fixed & v) within width. */
    uint64_t fixed = ~(d->lo ^ d->hi) & m;
    return (fixed & d->lo) == (fixed & v & m);
}

uint64_t zsp_bvdom_copy_with_fixed_bits(const zsp_bvdom_t *d, uint64_t v) {
    uint64_t m = zsp_bvdom_mask(d->width);
    uint64_t fixed = ~(d->lo ^ d->hi) & m;
    /* Clear the fixed bits in v, then OR the fixed-1 bits from d->lo. */
    return ((v & ~fixed) | (d->lo & fixed)) & m;
}

int zsp_bvdom_eq(const zsp_bvdom_t *a, const zsp_bvdom_t *b) {
    if (a->width != b->width) return 0;
    uint64_t m = zsp_bvdom_mask(a->width);
    return ((a->lo & m) == (b->lo & m)) && ((a->hi & m) == (b->hi & m));
}

void zsp_bvdom_meet(zsp_bvdom_t *out, const zsp_bvdom_t *a, const zsp_bvdom_t *b) {
    assert(a->width == b->width);
    /* Intersection of allowed sets: each bit's lo = max(a->lo, b->lo) (i.e. OR),
     * each bit's hi = min(a->hi, b->hi) (i.e. AND). Result is invalid iff any
     * bit ends up lo=1, hi=0 — caller can check zsp_bvdom_is_valid. */
    uint64_t m = zsp_bvdom_mask(a->width);
    out->width = a->width;
    out->lo = (a->lo | b->lo) & m;
    out->hi = (a->hi & b->hi) & m;
}

/* ---- domain transformations ---- */

void zsp_bvdom_bvnot(zsp_bvdom_t *r, const zsp_bvdom_t *a) {
    uint64_t m = zsp_bvdom_mask(a->width);
    /* bit i fixed-1 in a => fixed-0 in ~a. fixed-0 => fixed-1. unknown => unknown.
     * new_lo[i] = ~hi_a[i], new_hi[i] = ~lo_a[i] (within mask). */
    r->width = a->width;
    r->lo = ~a->hi & m;
    r->hi = ~a->lo & m;
}

void zsp_bvdom_bvand(zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b) {
    assert(a->width == b->width);
    uint64_t m = zsp_bvdom_mask(a->width);
    /* result bit fixed-1 iff both inputs fixed-1; fixed-0 iff either is fixed-0;
     * else unknown.
     * new_lo = a.lo & b.lo (both must be fixed-1).
     * new_hi = a.hi & b.hi (if either is fixed-0, result is fixed-0). */
    r->width = a->width;
    r->lo = (a->lo & b->lo) & m;
    r->hi = (a->hi & b->hi) & m;
}

void zsp_bvdom_bvor(zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b) {
    assert(a->width == b->width);
    uint64_t m = zsp_bvdom_mask(a->width);
    /* result bit fixed-1 iff either is fixed-1; fixed-0 iff both fixed-0.
     * new_lo = a.lo | b.lo. new_hi = a.hi | b.hi. */
    r->width = a->width;
    r->lo = (a->lo | b->lo) & m;
    r->hi = (a->hi | b->hi) & m;
}

void zsp_bvdom_bvxor(zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b) {
    assert(a->width == b->width);
    uint64_t m = zsp_bvdom_mask(a->width);
    /* result bit fixed iff both inputs fixed (a fixed XOR b fixed). Otherwise unknown.
     * fixed_a = ~(a.lo ^ a.hi), fixed_b = ~(b.lo ^ b.hi).
     * fixed_r = fixed_a & fixed_b.
     * value = (a.lo XOR b.lo) on fixed_r bits.
     * new_lo = value & fixed_r. new_hi = value | ~fixed_r. */
    uint64_t fa = ~(a->lo ^ a->hi) & m;
    uint64_t fb = ~(b->lo ^ b->hi) & m;
    uint64_t fr = fa & fb;
    uint64_t val = (a->lo ^ b->lo) & m;
    r->width = a->width;
    r->lo = val & fr;
    r->hi = (val & fr) | (~fr & m);
}

void zsp_bvdom_bvshl_const(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n) {
    uint64_t m = zsp_bvdom_mask(a->width);
    r->width = a->width;
    if (n >= a->width) {
        r->lo = 0;
        r->hi = 0;
        return;
    }
    r->lo = (a->lo << n) & m;
    r->hi = (a->hi << n) & m;
    /* The low n bits are introduced as fixed-0 (shift-in zeros). lo=0, hi=0 there. */
}

void zsp_bvdom_bvshr_const(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n) {
    uint64_t m = zsp_bvdom_mask(a->width);
    r->width = a->width;
    if (n >= a->width) {
        r->lo = 0;
        r->hi = 0;
        return;
    }
    r->lo = (a->lo & m) >> n;
    r->hi = (a->hi & m) >> n;
    /* High n bits are introduced as fixed-0. */
}

void zsp_bvdom_bvashr_const(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n) {
    uint64_t m = zsp_bvdom_mask(a->width);
    r->width = a->width;
    if (a->width == 0) { r->lo = 0; r->hi = 0; return; }
    if (n >= a->width) {
        /* result is sign-bit replicated. The sign bit's domain in `a` is
         * msb_pos = width-1. */
        uint8_t msb = a->width - 1;
        uint64_t mb = (uint64_t)1 << msb;
        int msb_lo = (a->lo & mb) != 0;
        int msb_hi = (a->hi & mb) != 0;
        if (msb_lo == msb_hi) {
            r->lo = msb_lo ? m : 0;
            r->hi = r->lo;
        } else {
            r->lo = 0;
            r->hi = m;
        }
        return;
    }
    uint8_t msb = a->width - 1;
    uint64_t mb = (uint64_t)1 << msb;
    int msb_lo = (a->lo & mb) != 0;
    int msb_hi = (a->hi & mb) != 0;
    uint64_t lo_shifted = (a->lo & m) >> n;
    uint64_t hi_shifted = (a->hi & m) >> n;
    /* New top n bits come from the sign bit. */
    uint64_t top_mask = (m << (a->width - n)) & m;
    if (msb_lo && msb_hi) {
        /* sign bit fixed-1: fill with 1s */
        r->lo = (lo_shifted | top_mask) & m;
        r->hi = (hi_shifted | top_mask) & m;
    } else if (!msb_lo && !msb_hi) {
        /* sign bit fixed-0: fill with 0s (same as logical shr) */
        r->lo = lo_shifted;
        r->hi = hi_shifted;
    } else {
        /* sign bit unknown: top n bits are unknown */
        r->lo = lo_shifted & ~top_mask;
        r->hi = hi_shifted | top_mask;
    }
}

void zsp_bvdom_bvconcat(zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b) {
    /* result = (a << b->width) | b, width = a.width + b.width. */
    uint8_t w = (uint8_t)(a->width + b->width);
    r->width = w;
    uint64_t mb = zsp_bvdom_mask(b->width);
    r->lo = ((a->lo & zsp_bvdom_mask(a->width)) << b->width) | (b->lo & mb);
    r->hi = ((a->hi & zsp_bvdom_mask(a->width)) << b->width) | (b->hi & mb);
}

void zsp_bvdom_bvextract(zsp_bvdom_t *r, const zsp_bvdom_t *a,
                         uint8_t hi_bit, uint8_t lo_bit) {
    assert(lo_bit <= hi_bit);
    assert(hi_bit < a->width);
    uint8_t w = (uint8_t)(hi_bit - lo_bit + 1);
    uint64_t m = zsp_bvdom_mask(w);
    r->width = w;
    r->lo = ((a->lo >> lo_bit) & m);
    r->hi = ((a->hi >> lo_bit) & m);
}

void zsp_bvdom_zero_ext(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n) {
    uint8_t w = (uint8_t)(a->width + n);
    uint64_t am = zsp_bvdom_mask(a->width);
    r->width = w;
    r->lo = a->lo & am;
    r->hi = a->hi & am;
}

void zsp_bvdom_sign_ext(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n) {
    uint8_t w = (uint8_t)(a->width + n);
    uint64_t am = zsp_bvdom_mask(a->width);
    r->width = w;
    if (n == 0) {
        r->lo = a->lo & am;
        r->hi = a->hi & am;
        return;
    }
    uint8_t msb = a->width - 1;
    uint64_t mb = (uint64_t)1 << msb;
    int msb_lo = (a->lo & mb) != 0;
    int msb_hi = (a->hi & mb) != 0;
    uint64_t top_mask = zsp_bvdom_mask(w) & ~am;  /* bits added by sign-ext */
    if (msb_lo && msb_hi) {           /* fixed-1: replicate 1s */
        r->lo = (a->lo & am) | top_mask;
        r->hi = (a->hi & am) | top_mask;
    } else if (!msb_lo && !msb_hi) {  /* fixed-0: replicate 0s */
        r->lo = a->lo & am;
        r->hi = a->hi & am;
    } else {                           /* unknown: top bits unknown */
        r->lo = a->lo & am;
        r->hi = (a->hi & am) | top_mask;
    }
}

void zsp_bvdom_to_str(const zsp_bvdom_t *d, char *buf, size_t cap) {
    if (cap == 0) return;
    size_t out = 0;
    /* MSB first. */
    for (int i = (int)d->width - 1; i >= 0 && out + 1 < cap; i--) {
        uint64_t b = (uint64_t)1 << i;
        int lo = (d->lo & b) != 0;
        int hi = (d->hi & b) != 0;
        char c;
        if (lo && !hi)      c = 'i';   /* invalid */
        else if (lo && hi)  c = '1';
        else if (!lo && hi) c = 'x';
        else                c = '0';
        buf[out++] = c;
    }
    buf[out] = '\0';
}
