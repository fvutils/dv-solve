#include "zsp_bvbounds.h"

#include <assert.h>
#include <stdio.h>

/* ---- helpers ---- */

static uint64_t mask_of(uint8_t w) { return zsp_bvrange_mask(w); }

static int has_msb(uint64_t v, uint8_t w) {
    if (w == 0) return 0;
    return (v >> (w - 1)) & 1u;
}

/** Compare two width-bit values as signed. Returns -1/0/1 for a<b/=/>. */
static int signed_cmp(uint64_t a, uint64_t b, uint8_t w) {
    int sa = has_msb(a, w);
    int sb = has_msb(b, w);
    if (sa != sb) return sa ? -1 : 1;  /* negative < positive */
    /* same sign — unsigned compare on the masked values works */
    uint64_t am = a & mask_of(w);
    uint64_t bm = b & mask_of(w);
    return am < bm ? -1 : am > bm ? 1 : 0;
}

/* ---- range ---- */

void zsp_bvrange_init(zsp_bvrange_t *r, uint8_t width,
                      uint64_t min, uint64_t max) {
    r->width = width;
    r->min   = min & mask_of(width);
    r->max   = max & mask_of(width);
    r->empty = 0;
}

void zsp_bvrange_init_from_domain(zsp_bvrange_t *r, const zsp_bvdom_t *d) {
    zsp_bvrange_init(r, d->width, d->lo, d->hi);
}

void zsp_bvrange_init_empty(zsp_bvrange_t *r, uint8_t width) {
    r->width = width;
    r->min   = 0;
    r->max   = 0;
    r->empty = 1;
}

int zsp_bvrange_is_empty(const zsp_bvrange_t *r) { return r->empty; }

int zsp_bvrange_is_valid(const zsp_bvrange_t *r) {
    if (r->empty) return 1;
    /* bitwuzla's BitVectorRange::valid(): valid if min <= max under
     * unsigned OR signed compare. */
    uint64_t m = mask_of(r->width);
    uint64_t mn = r->min & m;
    uint64_t mx = r->max & m;
    return mn <= mx || signed_cmp(mn, mx, r->width) <= 0;
}

int zsp_bvrange_contains_u(const zsp_bvrange_t *r, uint64_t v) {
    if (r->empty) return 0;
    uint64_t m = mask_of(r->width);
    uint64_t vm = v & m;
    return vm >= (r->min & m) && vm <= (r->max & m);
}

void zsp_bvrange_intersect(zsp_bvrange_t *out,
                           const zsp_bvrange_t *a, const zsp_bvrange_t *b) {
    assert(a->width == b->width);
    if (a->empty || b->empty) {
        zsp_bvrange_init_empty(out, a->width);
        return;
    }
    uint64_t m = mask_of(a->width);
    uint64_t lo = (a->min & m) > (b->min & m) ? (a->min & m) : (b->min & m);
    uint64_t hi = (a->max & m) < (b->max & m) ? (a->max & m) : (b->max & m);
    if (lo > hi) {
        zsp_bvrange_init_empty(out, a->width);
    } else {
        zsp_bvrange_init(out, a->width, lo, hi);
    }
}

/* ---- bounds ---- */

void zsp_bvbounds_init_empty(zsp_bvbounds_t *b, uint8_t width) {
    b->width = width;
    zsp_bvrange_init_empty(&b->lo, width);
    zsp_bvrange_init_empty(&b->hi, width);
}

void zsp_bvbounds_init_range(zsp_bvbounds_t *b, uint8_t width,
                             uint64_t min, uint64_t max) {
    uint64_t m = mask_of(width);
    min &= m;
    max &= m;
    b->width = width;
    if (min > max) {
        zsp_bvrange_init_empty(&b->lo, width);
        zsp_bvrange_init_empty(&b->hi, width);
        return;
    }
    /* Signed split: msb=0 half is [0, max_signed], msb=1 half is
     * [min_signed, ones]. With width w, the boundary is at
     *   max_signed = (1 << (w-1)) - 1
     *   min_signed = (1 << (w-1))      (as unsigned bit pattern). */
    uint64_t max_pos = (width >= 64) ? ((uint64_t)INT64_MAX) : (((uint64_t)1 << (width - 1)) - 1);
    uint64_t min_neg = (width == 0) ? 0 : ((uint64_t)1 << (width - 1));

    if (max <= max_pos) {
        /* entirely in lo half */
        zsp_bvrange_init(&b->lo, width, min, max);
        zsp_bvrange_init_empty(&b->hi, width);
    } else if (min >= min_neg) {
        /* entirely in hi half */
        zsp_bvrange_init_empty(&b->lo, width);
        zsp_bvrange_init(&b->hi, width, min, max);
    } else {
        /* straddles the midpoint: split */
        zsp_bvrange_init(&b->lo, width, min, max_pos);
        zsp_bvrange_init(&b->hi, width, min_neg, max);
    }
}

void zsp_bvbounds_init_from_domain(zsp_bvbounds_t *b, const zsp_bvdom_t *d) {
    /* The domain's [lo, hi] is the tightest enclosing unsigned range
     * of values consistent with the domain bit-wise. lo == d->lo,
     * hi == d->hi (both interpreted unsigned). Note: this is an
     * approximation — not every value in [lo, hi] is consistent with
     * the domain (e.g., domain "1x" allows values {10, 11} but range
     * is [10, 11], which happens to be tight; domain "x0" allows
     * {00, 10} with range [00, 10] which includes the invalid 01).
     * Matches bitwuzla's BitVectorRange(domain) behavior. */
    zsp_bvbounds_init_range(b, d->width, d->lo, d->hi);
}

int zsp_bvbounds_empty(const zsp_bvbounds_t *b) {
    return !zsp_bvbounds_has_lo(b) && !zsp_bvbounds_has_hi(b);
}

int zsp_bvbounds_has_lo(const zsp_bvbounds_t *b) { return !b->lo.empty; }
int zsp_bvbounds_has_hi(const zsp_bvbounds_t *b) { return !b->hi.empty; }

int zsp_bvbounds_is_valid(const zsp_bvbounds_t *b) {
    if (zsp_bvbounds_has_lo(b)) {
        if (!zsp_bvrange_is_valid(&b->lo)) return 0;
        if (has_msb(b->lo.min, b->width)) return 0;
        if (has_msb(b->lo.max, b->width)) return 0;
    }
    if (zsp_bvbounds_has_hi(b)) {
        if (!zsp_bvrange_is_valid(&b->hi)) return 0;
        if (!has_msb(b->hi.min, b->width)) return 0;
        if (!has_msb(b->hi.max, b->width)) return 0;
    }
    return 1;
}

int zsp_bvbounds_contains(const zsp_bvbounds_t *b, uint64_t v) {
    return zsp_bvrange_contains_u(&b->lo, v) || zsp_bvrange_contains_u(&b->hi, v);
}

void zsp_bvbounds_intersect(zsp_bvbounds_t *out,
                            const zsp_bvbounds_t *a, const zsp_bvbounds_t *b) {
    assert(a->width == b->width);
    out->width = a->width;
    zsp_bvrange_intersect(&out->lo, &a->lo, &b->lo);
    zsp_bvrange_intersect(&out->hi, &a->hi, &b->hi);
}

/* ---- to_str ---- */

void zsp_bvrange_to_str(const zsp_bvrange_t *r, char *buf, unsigned cap) {
    if (cap == 0) return;
    if (r->empty) { snprintf(buf, cap, "[empty]"); return; }
    uint64_t m = mask_of(r->width);
    snprintf(buf, cap, "[%llu, %llu]",
             (unsigned long long)(r->min & m),
             (unsigned long long)(r->max & m));
}

void zsp_bvbounds_to_str(const zsp_bvbounds_t *b, char *buf, unsigned cap) {
    if (cap == 0) return;
    char l[64], h[64];
    zsp_bvrange_to_str(&b->lo, l, sizeof(l));
    zsp_bvrange_to_str(&b->hi, h, sizeof(h));
    snprintf(buf, cap, "%s U %s", l, h);
}
