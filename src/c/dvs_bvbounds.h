#ifndef DVS_BVBOUNDS_H
#define DVS_BVBOUNDS_H

#include <stdint.h>

#include "dvs_bvdom.h"

/**
 * dvs_bvbounds — bit-vector range / bounds.
 *
 * Ported from bitwuzla's BitVectorRange + BitVectorBounds, specialized
 * for dv-solve's width <= 64 ceiling.
 *
 * Two layers:
 *  - dvs_bvrange_t: a single contiguous [min, max] (unsigned interpretation)
 *    on a width-bit BV. May be empty or invalid.
 *  - dvs_bvbounds_t: a *pair* of ranges representing the union of values
 *    in [0, max_signed] (d_lo) and [min_signed, ones] (d_hi). This dual
 *    representation lets signed inequality propagation stay precise while
 *    storing only two integer pairs.
 *
 * Phase-A motivation: bounds analysis layered on top of the 3-valued
 * domain. Used by preprocessing passes to tighten ranges before search
 * and by Phase C local search to drive the next-value heuristic.
 */

/* ---- single contiguous range ---- */

typedef struct {
    uint64_t min;
    uint64_t max;
    uint8_t  width;
    uint8_t  empty;     /* 1 if range is empty (min/max meaningless)    */
} dvs_bvrange_t;

/* ---- pair-of-ranges bounds ---- */

typedef struct {
    dvs_bvrange_t lo;   /* in [0, max_signed]   — msb of min/max is 0   */
    dvs_bvrange_t hi;   /* in [min_signed, ones] — msb of min/max is 1  */
    uint8_t       width;
} dvs_bvbounds_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- range API ---- */

/** Initialize a non-empty range [min, max] (no validity check). */
void dvs_bvrange_init(dvs_bvrange_t *r, uint8_t width,
                      uint64_t min, uint64_t max);

/** Initialize from a domain's lo / hi (treated unsigned). */
void dvs_bvrange_init_from_domain(dvs_bvrange_t *r, const dvs_bvdom_t *d);

/** Initialize as empty. */
void dvs_bvrange_init_empty(dvs_bvrange_t *r, uint8_t width);

int  dvs_bvrange_is_empty(const dvs_bvrange_t *r);

/** A range is valid iff empty, or min <= max under at least one ordering
 *  (unsigned or signed). Matches bitwuzla's BitVectorRange::valid(). */
int  dvs_bvrange_is_valid(const dvs_bvrange_t *r);

/** Unsigned containment. */
int  dvs_bvrange_contains_u(const dvs_bvrange_t *r, uint64_t v);

/** Intersect a and b into out. Result may be empty. Width must match. */
void dvs_bvrange_intersect(dvs_bvrange_t *out,
                           const dvs_bvrange_t *a, const dvs_bvrange_t *b);

/** Width-mask helper (same semantics as dvs_bvdom_mask). */
static inline uint64_t dvs_bvrange_mask(uint8_t w) {
    return w >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << w) - 1;
}

/* ---- bounds (pair of ranges) API ---- */

/** Initialize bounds as fully empty for `width`. */
void dvs_bvbounds_init_empty(dvs_bvbounds_t *b, uint8_t width);

/** Initialize from a single contiguous unsigned range [min, max]: split into
 *  the signed-positive and signed-negative halves around the midpoint as
 *  needed. If min > max under unsigned ordering the bounds are empty. */
void dvs_bvbounds_init_range(dvs_bvbounds_t *b, uint8_t width,
                             uint64_t min, uint64_t max);

/** Initialize from a domain's [lo, hi] interval. */
void dvs_bvbounds_init_from_domain(dvs_bvbounds_t *b, const dvs_bvdom_t *d);

int  dvs_bvbounds_empty(const dvs_bvbounds_t *b);
int  dvs_bvbounds_has_lo(const dvs_bvbounds_t *b);
int  dvs_bvbounds_has_hi(const dvs_bvbounds_t *b);

/** True iff the lo and hi range conventions hold (msb=0 in lo, msb=1 in hi). */
int  dvs_bvbounds_is_valid(const dvs_bvbounds_t *b);

/** True iff `v` is contained in either range. */
int  dvs_bvbounds_contains(const dvs_bvbounds_t *b, uint64_t v);

/** Intersect two bounds into out. Either side may end up empty. */
void dvs_bvbounds_intersect(dvs_bvbounds_t *out,
                            const dvs_bvbounds_t *a,
                            const dvs_bvbounds_t *b);

/** String representation of a range or bounds. Writes up to cap-1 chars
 *  plus a null terminator. Formats are "[min, max]" and "{lo}∪{hi}". */
void dvs_bvrange_to_str (const dvs_bvrange_t  *r, char *buf, unsigned cap);
void dvs_bvbounds_to_str(const dvs_bvbounds_t *b, char *buf, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* DVS_BVBOUNDS_H */
