#ifndef DVS_BBWORD_H
#define DVS_BBWORD_H

#include <stdint.h>

#include "dvs_alloc.h"
#include "dvs_bitblast.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * dvs_bbword — word-level facts about bit-blasted vectors
 * (docs/fw_hdl_implementation_plan.md, piece 5.4).
 *
 * The bit-blaster sees a hash finaliser, or a checksum reduced at every step,
 * only as gates, and SAT cannot see through a multiplier or a divider. This
 * table remembers, for each vector the bit-blaster builds, facts that hold
 * for every input, and uses them to cancel inverse operations and to decide
 * equalities without SAT. Two kinds of fact:
 *
 * Shape facts, keyed by the exact vector (width w <= 64):
 *   - lin:  v = P(S)·u over GF(2), where S is a logical right shift by one
 *           bit and P a polynomial (bit k of `poly` is S^k), truncated at
 *           S^w. `x ^ (x >> k)` is (1 + S^k)·x, and P is invertible exactly
 *           when its constant term is 1.
 *   - mul:  v = u·c mod 2^w for a constant c; invertible exactly when c is
 *           odd.
 *   An XOR whose operands share a base adds their polynomials; when the sum
 *   is 1 the result is the base itself (an un-xorshift undoing a xorshift).
 *   A multiply of v = u·c by c' with c·c' = 1 mod 2^w is u. An equality of
 *   two vectors with the same invertible shape compares the bases instead.
 *
 * Value facts, keyed by the vector with leading FALSE bits stripped (so a
 * zero-extension has the same facts):
 *   - an upper bound on the unsigned value;
 *   - a linear form: value ≡ k + Σ c_i·t_i (mod m), equal when m = 0, over
 *     term vectors t_i.
 *   An addition that cannot wrap adds forms (modulus gcd), a multiply by a
 *   constant that cannot wrap scales one, `bvurem` by a constant m keeps the
 *   form modulo gcd(m, form modulus) with bound m - 1, and an extract of the
 *   low bits that holds every value keeps it. Two values with bounds below a
 *   common modulus d and forms equal modulo d are equal: a checksum reduced
 *   at every step against one reduced once at the end.
 *
 * Every fact is true for every input, so using one never changes an answer.
 * The first fact claimed for a vector is kept. DV_BB_WORD=0 turns it off.
 */

typedef struct dvs_bbword_s dvs_bbword_t;

dvs_bbword_t *dvs_bbword_new(dvs_alloc_t *alloc);
void          dvs_bbword_free(dvs_bbword_t *W);

/** a ^ b (a.size == b.size); the base itself when the xorshifts cancel. */
dvs_bv_t dvs_bbword_xor(dvs_bbword_t *W, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);

/** a * b (a.size == b.size); the base itself when constant factors cancel. */
dvs_bv_t dvs_bbword_mul(dvs_bbword_t *W, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);

/** Record r = a >> b (logical; b's width may differ from a's). */
void dvs_bbword_note_shr(dvs_bbword_t *W, dvs_bv_t a, dvs_bv_t b, dvs_bv_t r);

/** Record r = a + b mod 2^r.size. */
void dvs_bbword_note_add(dvs_bbword_t *W, dvs_bv_t a, dvs_bv_t b, dvs_bv_t r);

/** Record r = a urem b (unsigned). */
void dvs_bbword_note_urem(dvs_bbword_t *W, dvs_bv_t a, dvs_bv_t b, dvs_bv_t r);

/** Record r = a[hi:lo]. */
void dvs_bbword_note_extract(dvs_bbword_t *W, dvs_bv_t a, uint32_t hi, uint32_t lo,
                             dvs_bv_t r);

/** a == b as a 1-bit vector (a.size == b.size): a constant when the facts
 *  decide it, otherwise the comparison of the bases the shapes peel to. */
dvs_bv_t dvs_bbword_eq(dvs_bbword_t *W, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b);

/** Statistics: inverse pairs cancelled, equalities decided, equalities peeled. */
uint64_t dvs_bbword_num_cancelled(const dvs_bbword_t *W);
uint64_t dvs_bbword_num_decided(const dvs_bbword_t *W);
uint64_t dvs_bbword_num_peeled(const dvs_bbword_t *W);

#ifdef __cplusplus
}
#endif

#endif /* DVS_BBWORD_H */
