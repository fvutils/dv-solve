#ifndef ZSP_BVDOM_H
#define ZSP_BVDOM_H

#include <stddef.h>
#include <stdint.h>

/**
 * zsp_bvdom — three-valued bit-vector domain.
 *
 * Ported from bitwuzla's BitVectorDomain, specialized for dv-solve's
 * width <= 64 ceiling so the underlying value can be a single uint64_t
 * (no multi-precision support).
 *
 * Representation:
 *   lo: each bit set in `lo` is *fixed to 1*.
 *   hi: each bit set in `hi` *may* be 1. Bits clear in `hi` are fixed-0.
 * Within the active width:
 *   bit fixed to 1   : lo[i]=1, hi[i]=1
 *   bit fixed to 0   : lo[i]=0, hi[i]=0
 *   bit unknown      : lo[i]=0, hi[i]=1
 *   invalid (empty)  : lo[i]=1, hi[i]=0
 * The domain is *valid* iff (lo & ~hi) == 0 (within width).
 * The domain is *fixed* iff lo == hi (within width); then lo == hi == value.
 *
 * Bits above the width are not meaningful and are typically 0 in both
 * `lo` and `hi`. Helper macros expose a width mask.
 *
 * Phase-A motivation: backs richer wiremask analysis in preprocessing,
 * supplies the substrate for Phase C local search's invertibility /
 * consistency predicates (those are a separate body of work).
 */

typedef struct {
    uint64_t lo;
    uint64_t hi;
    uint8_t  width;   /* 1..64 */
} zsp_bvdom_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Mask of the bits in the active width (0 if width==0). */
static inline uint64_t zsp_bvdom_mask(uint8_t width) {
    return width >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << width) - 1;
}

/** Initialize to "fully unknown" domain of width `w`. */
void zsp_bvdom_init_unknown(zsp_bvdom_t *d, uint8_t w);

/** Initialize to a fixed value `v` (masked to w bits). */
void zsp_bvdom_init_value(zsp_bvdom_t *d, uint8_t w, uint64_t v);

/** Initialize from lo / hi components (caller-supplied; result may be invalid). */
void zsp_bvdom_init_lohi(zsp_bvdom_t *d, uint8_t w, uint64_t lo, uint64_t hi);

/**
 * Initialize from an unsigned value range [lo_val, hi_val]. The leading bits
 * where lo_val and hi_val agree become fixed; below the first differing bit
 * every bit is unknown. Returns 0 on success, non-zero if lo_val > hi_val
 * (caller's responsibility — the domain will be set to fully-unknown).
 *
 * Example: width=8, [0, 5] -> leading 5 bits are fixed-0 (since 0..5 all
 * have bits 7..3 = 0), bits 2..0 are unknown.
 */
int zsp_bvdom_init_from_range_u(zsp_bvdom_t *d, uint8_t w,
                                uint64_t lo_val, uint64_t hi_val);

/** True iff `(lo & ~hi) & mask == 0`. */
int zsp_bvdom_is_valid(const zsp_bvdom_t *d);

/** True iff every bit (within width) is fixed (lo == hi when masked). */
int zsp_bvdom_is_fixed(const zsp_bvdom_t *d);

/** True iff at least one bit is fixed. Requires the domain to be valid. */
int zsp_bvdom_has_fixed_bits(const zsp_bvdom_t *d);
int zsp_bvdom_has_fixed_bits_true(const zsp_bvdom_t *d);
int zsp_bvdom_has_fixed_bits_false(const zsp_bvdom_t *d);

/** Per-bit predicates. `idx` is 0 = LSB ... width-1 = MSB. */
int zsp_bvdom_is_fixed_bit(const zsp_bvdom_t *d, uint8_t idx);
int zsp_bvdom_is_fixed_bit_true(const zsp_bvdom_t *d, uint8_t idx);
int zsp_bvdom_is_fixed_bit_false(const zsp_bvdom_t *d, uint8_t idx);

/** Force bit `idx` to value `v` in place. */
void zsp_bvdom_fix_bit(zsp_bvdom_t *d, uint8_t idx, int v);

/** Force the entire domain to a single value. */
void zsp_bvdom_fix(zsp_bvdom_t *d, uint64_t v);

/** True iff every fixed bit of `d` agrees with the corresponding bit of `v`. */
int zsp_bvdom_match(const zsp_bvdom_t *d, uint64_t v);

/** Return `v` with every fixed bit of `d` replaced by its fixed value. */
uint64_t zsp_bvdom_copy_with_fixed_bits(const zsp_bvdom_t *d, uint64_t v);

/** Equality (same width and same lo/hi within width). */
int zsp_bvdom_eq(const zsp_bvdom_t *a, const zsp_bvdom_t *b);

/** Intersection of two domains of the same width. Result is invalid iff
 *  any bit is fixed to opposite values in `a` and `b`. */
void zsp_bvdom_meet(zsp_bvdom_t *out, const zsp_bvdom_t *a, const zsp_bvdom_t *b);

/* ---- domain transformations: r = op(a [, b]) over the *whole* domain --- */

void zsp_bvdom_bvnot   (zsp_bvdom_t *r, const zsp_bvdom_t *a);
void zsp_bvdom_bvand   (zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b);
void zsp_bvdom_bvor    (zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b);
void zsp_bvdom_bvxor   (zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b);

/** Shift the domain left by a *known constant* amount. */
void zsp_bvdom_bvshl_const(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n);
/** Shift the domain right (logical) by a known constant amount. */
void zsp_bvdom_bvshr_const(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n);
/** Shift the domain right (arithmetic) by a known constant amount. */
void zsp_bvdom_bvashr_const(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n);

/** Concatenate two domains: result width = a.width + b.width, a is the high part. */
void zsp_bvdom_bvconcat(zsp_bvdom_t *r, const zsp_bvdom_t *a, const zsp_bvdom_t *b);

/** Extract bits [hi_bit:lo_bit] (inclusive, lo_bit <= hi_bit < a->width). */
void zsp_bvdom_bvextract(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t hi_bit, uint8_t lo_bit);

/** Zero-extend by n bits. */
void zsp_bvdom_zero_ext(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n);

/** Sign-extend by n bits. */
void zsp_bvdom_sign_ext(zsp_bvdom_t *r, const zsp_bvdom_t *a, uint8_t n);

/** Generate a 3-valued string representation: '0', '1', 'x', 'i'.
 *  Writes at most `cap` bytes including the null terminator. */
void zsp_bvdom_to_str(const zsp_bvdom_t *d, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_BVDOM_H */
