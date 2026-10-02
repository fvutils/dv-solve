#ifndef DVS_BITBLAST_H
#define DVS_BITBLAST_H

#include <stddef.h>
#include <stdint.h>

#include "dvs_aig.h"
#include "dvs_alloc.h"

/**
 * dvs_bitblast — bit-blast BV expressions to AIG.
 *
 * Ported from bitwuzla's lib/bitblast/bitblaster.h. The template parameter T
 * (bit) is collapsed to `dvs_aig_node_t`; the template parameter Bits
 * (vector of bits) becomes a pair (pointer, size) returned as `dvs_bv_t`.
 *
 * Bit ordering matches upstream: index 0 is the MSB, index (size-1) is the
 * LSB. All ops follow that convention.
 *
 * Memory: every returned `dvs_bv_t` is owned by the bit-blaster context and
 * lives until the context is freed. The intent is one context per
 * check-sat call.
 */

typedef struct {
    dvs_aig_node_t *bits;
    uint32_t        size;
} dvs_bv_t;

typedef struct dvs_bitblast_s dvs_bitblast_t;

#ifdef __cplusplus
extern "C" {
#endif

dvs_bitblast_t *dvs_bitblast_new(dvs_alloc_t *alloc, dvs_aig_t *aig);
void            dvs_bitblast_free(dvs_bitblast_t *bb);

/** Get the AIG manager driving this bit-blaster. */
dvs_aig_t *dvs_bitblast_aig(dvs_bitblast_t *bb);

/** Allocate a fresh `size`-bit symbolic constant (each bit is a new AIG input). */
dvs_bv_t dvs_bb_constant(dvs_bitblast_t *bb, uint32_t size);

/**
 * Allocate a `size`-bit BV with per-bit domain: each bit is a fresh AIG
 * input where the corresponding bit in `unknown_mask` is 1, otherwise the
 * corresponding bit in `fixed_value` (0 -> FALSE, 1 -> TRUE).
 *
 * Mapping: bit position `i` of the value (LSB = 0) lands at array index
 * `size - 1 - i` (since the bit-blaster's bv arrays are MSB-first).
 * This is what bvdom + VarSpec bounds want.
 */
dvs_bv_t dvs_bb_constant_dom(dvs_bitblast_t *bb, uint32_t size,
                             uint64_t fixed_value, uint64_t unknown_mask);

/** Allocate a `size`-bit literal from a uint64 value. MSB is bit 0. */
dvs_bv_t dvs_bb_value_u64(dvs_bitblast_t *bb, uint32_t size, uint64_t value);

/** Allocate a `size`-bit literal whose bits come from `bits_msb_first`. */
dvs_bv_t dvs_bb_value_bits(dvs_bitblast_t *bb,
                           uint32_t size,
                           const dvs_aig_node_t *bits_msb_first);

/** Bitwise */
dvs_bv_t dvs_bb_not (dvs_bitblast_t *bb, dvs_bv_t a);
dvs_bv_t dvs_bb_and (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_or  (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_xor (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);

/** Predicates — return a 1-bit dvs_bv_t */
dvs_bv_t dvs_bb_eq  (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_ult (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_slt (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);

/** Shifts (logical and arithmetic) */
dvs_bv_t dvs_bb_shl (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_shr (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_ashr(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);

/** Arithmetic */
dvs_bv_t dvs_bb_add (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_neg (dvs_bitblast_t *bb, dvs_bv_t a);   /* two's complement */
dvs_bv_t dvs_bb_sub (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_mul (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_udiv(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_urem(dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);

/** Structural ops */
dvs_bv_t dvs_bb_extract(dvs_bitblast_t *bb, dvs_bv_t a, uint32_t upper, uint32_t lower);
dvs_bv_t dvs_bb_concat (dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);
dvs_bv_t dvs_bb_zero_ext(dvs_bitblast_t *bb, dvs_bv_t a, uint32_t n);
dvs_bv_t dvs_bb_sign_ext(dvs_bitblast_t *bb, dvs_bv_t a, uint32_t n);

/** Vector ITE (cond is a single AIG bit). */
dvs_bv_t dvs_bb_ite(dvs_bitblast_t *bb, dvs_aig_node_t cond, dvs_bv_t a, dvs_bv_t b);

#ifdef __cplusplus
}
#endif

#endif /* DVS_BITBLAST_H */
