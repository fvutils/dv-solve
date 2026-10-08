#ifndef DVS_BBSUM_H
#define DVS_BBSUM_H

#include <stdint.h>

#include "dvs_alloc.h"
#include "dvs_bitblast.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * dvs_bbsum — word-level sum normalization during bit-blasting
 * (docs/fw_hdl_implementation_plan.md, piece 5.1).
 *
 * The AIG already hashes gates structurally, but two adder networks that
 * compute the same sum in a different association share no gates: Yosys
 * pipelines an adder tree as a balanced tree whose widths grow by one bit per
 * level (`(concat #b0 x)` operands), while an IR specification adds the same
 * terms in a chain at the final width. Both are the same integer sum.
 *
 * Every addition goes through dvs_bbsum_add, which remembers the result's
 * bits together with its addends. An operand that is such a remembered sum
 * is replaced by its addends when that keeps the value:
 *
 *   - the inner sum was exact (its addends' maximum total fits its width, so
 *     it never wrapped), or
 *   - the outer width is no wider than the inner one (addition is modular).
 *
 * The flattened addend list is sorted (by AIG literals, so equal terms sort
 * equally) and added in that order, each partial sum just wide enough for its
 * bound. Constants are folded into one. The same sum thus yields the same
 * gates on both sides of a miter, and strash merges them.
 *
 * Operands are compared as AIG literal vectors with leading FALSE bits
 * stripped, so zero-extension, `concat #b0` and the solver's own widening all
 * look the same. A vector with the same stripped bits as a remembered sum has
 * the same integer value, which is what the rules above need.
 *
 * Cost: rebuilding a sum at every step of a chain is quadratic in the worst
 * case (strash shares the unchanged prefix of the sorted order, so a chain
 * whose new terms sort last costs one adder per step). A work budget bounds
 * it; past the budget additions are built as written. DV_BB_SUM=0 turns the
 * normalization off.
 */

typedef struct dvs_bbsum_s dvs_bbsum_t;

dvs_bbsum_t *dvs_bbsum_new(dvs_alloc_t *alloc);
void         dvs_bbsum_free(dvs_bbsum_t *T);

/** a + b modulo 2^a.size (a.size == b.size). The bits are owned by `bb`.
 *  `single`: the result has one user, so a later addition may flatten it
 *  (a sum with more users is kept whole, so it stays shared). */
dvs_bv_t dvs_bbsum_add(dvs_bbsum_t *T, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b,
                       int single);

/** Statistics: additions seen, and those whose operands were flattened. */
uint64_t dvs_bbsum_num_adds(const dvs_bbsum_t *T);
uint64_t dvs_bbsum_num_flattened(const dvs_bbsum_t *T);

#ifdef __cplusplus
}
#endif

#endif /* DVS_BBSUM_H */
