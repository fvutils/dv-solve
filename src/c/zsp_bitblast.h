#ifndef ZSP_BITBLAST_H
#define ZSP_BITBLAST_H

#include <stddef.h>
#include <stdint.h>

#include "zsp_aig.h"
#include "zsp_alloc.h"

/**
 * zsp_bitblast — bit-blast BV expressions to AIG.
 *
 * Ported from bitwuzla's lib/bitblast/bitblaster.h. The template parameter T
 * (bit) is collapsed to `zsp_aig_node_t`; the template parameter Bits
 * (vector of bits) becomes a pair (pointer, size) returned as `zsp_bv_t`.
 *
 * Bit ordering matches upstream: index 0 is the MSB, index (size-1) is the
 * LSB. All ops follow that convention.
 *
 * Memory: every returned `zsp_bv_t` is owned by the bit-blaster context and
 * lives until the context is freed. The intent is one context per
 * check-sat call.
 */

typedef struct {
    zsp_aig_node_t *bits;
    uint32_t        size;
} zsp_bv_t;

typedef struct zsp_bitblast_s zsp_bitblast_t;

#ifdef __cplusplus
extern "C" {
#endif

zsp_bitblast_t *zsp_bitblast_new(zsp_alloc_t *alloc, zsp_aig_t *aig);
void            zsp_bitblast_free(zsp_bitblast_t *bb);

/** Get the AIG manager driving this bit-blaster. */
zsp_aig_t *zsp_bitblast_aig(zsp_bitblast_t *bb);

/** Allocate a fresh `size`-bit symbolic constant (each bit is a new AIG input). */
zsp_bv_t zsp_bb_constant(zsp_bitblast_t *bb, uint32_t size);

/** Allocate a `size`-bit literal from a uint64 value. MSB is bit 0. */
zsp_bv_t zsp_bb_value_u64(zsp_bitblast_t *bb, uint32_t size, uint64_t value);

/** Allocate a `size`-bit literal whose bits come from `bits_msb_first`. */
zsp_bv_t zsp_bb_value_bits(zsp_bitblast_t *bb,
                           uint32_t size,
                           const zsp_aig_node_t *bits_msb_first);

/** Bitwise */
zsp_bv_t zsp_bb_not (zsp_bitblast_t *bb, zsp_bv_t a);
zsp_bv_t zsp_bb_and (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_or  (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_xor (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);

/** Predicates — return a 1-bit zsp_bv_t */
zsp_bv_t zsp_bb_eq  (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_ult (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_slt (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);

/** Shifts (logical and arithmetic) */
zsp_bv_t zsp_bb_shl (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_shr (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_ashr(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);

/** Arithmetic */
zsp_bv_t zsp_bb_add (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_neg (zsp_bitblast_t *bb, zsp_bv_t a);   /* two's complement */
zsp_bv_t zsp_bb_sub (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_mul (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_udiv(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_urem(zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);

/** Structural ops */
zsp_bv_t zsp_bb_extract(zsp_bitblast_t *bb, zsp_bv_t a, uint32_t upper, uint32_t lower);
zsp_bv_t zsp_bb_concat (zsp_bitblast_t *bb, zsp_bv_t a, zsp_bv_t b);
zsp_bv_t zsp_bb_zero_ext(zsp_bitblast_t *bb, zsp_bv_t a, uint32_t n);
zsp_bv_t zsp_bb_sign_ext(zsp_bitblast_t *bb, zsp_bv_t a, uint32_t n);

/** Vector ITE (cond is a single AIG bit). */
zsp_bv_t zsp_bb_ite(zsp_bitblast_t *bb, zsp_aig_node_t cond, zsp_bv_t a, zsp_bv_t b);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_BITBLAST_H */
