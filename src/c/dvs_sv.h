#ifndef DVS_SV_H
#define DVS_SV_H
/*
 * SystemVerilog expression sizing and signedness for builder-API problems.
 *
 * The builder API (expr_const / expr_var / expr_binary ...) follows IEEE 1800
 * expression rules:
 *
 *   - Every node has a SELF-DETERMINED type (width, signed):
 *       variable      its declared width / signedness
 *       constant      sized: (width, is_signed); unsized: see dvs_sv_const_type
 *       + - * / % & | ^        (max operand width, both operands signed)
 *       << >> >>>              the left operand's type
 *       == != < <= > >= && ||  (1, unsigned)       ! likewise
 *       unary - ~              the operand's type
 *       c ? a : b              like a binary operator over a and b
 *       extend(x, from, to)    (to, x's signedness)
 *       extract / concat       unsigned, their own widths
 *   - A comparison evaluates both sides in the context (max width of the two
 *     sides, both sides signed). The context propagates down through the
 *     context-determined operators (arithmetic, bitwise, unary - ~, the left
 *     operand of a shift, both arms of ?:). Shift amounts, conditions, the
 *     operands of && || ! and of extend/extract/concat are self-determined.
 *   - Each operand is first extended to the context width by ITS OWN
 *     signedness, the operation is done at the context width (2's complement
 *     wrap), and the result is read per the context signedness.
 *   - / and % truncate toward zero when signed. >> (DVS_BIN_RSHIFT) is a LOGICAL
 *     shift of the context-width bit pattern; >>> (DVS_BIN_ASHR) is an
 *     ARITHMETIC shift of it in a signed context and the same as >> in an
 *     unsigned one. A shift amount is the unsigned value of its own pattern.
 *
 * The engines (CDCL compile, bit-blaster) do not interpret these rules
 * themselves. dvs_sv_elaborate() rewrites a problem into an EXPLICIT form in
 * which every constant is sized at the type it is used at, every operator's
 * operands already have the operator's width/signedness (or are narrower
 * operands whose value is unchanged by the implicit extension), and every
 * value-changing conversion is an explicit EXPR_SV_CAST node. The model
 * validator (dvs_validate.c) evaluates the ORIGINAL problem under the same
 * rules independently.
 */
#include <stdint.h>
#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Self-determined type of a constant.
 *
 * Sized (width > 0): (width, is_signed).
 * Unsized: a SystemVerilog integer literal --
 *   value in [INT32_MIN, INT32_MAX]                    -> 32-bit signed
 *   else !is_signed and value in [2^31, 2^32 - 1]      -> 32-bit unsigned
 *   else value < 0 or is_signed                        -> 64-bit signed
 *   else                                               -> 64-bit unsigned
 * So an ordinary literal (`5`, `-3`) is a signed int whatever is_signed says;
 * is_signed only matters for a value that does not fit in int32. */
void dvs_sv_const_type(const ExprConst *c, uint16_t *width, uint8_t *is_signed);

/** The value of `v` read as a `width`-bit (<= 64) number of the given
 * signedness: its low `width` bits, sign-extended when signed. For width 64
 * an unsigned value >= 2^63 is returned as its (negative) int64 pattern. */
int64_t dvs_sv_wrap(int64_t v, uint16_t width, uint8_t is_signed);

/** Width / signedness of a variable that has no VarSpec in the problem being
 * elaborated (an incrementally added constraint over existing vars). Return 0
 * and fill the outputs, or -1 if unknown. */
typedef int (*dvs_sv_var_type_fn)(void *ud, uint32_t var_id,
                                  uint16_t *width, uint8_t *is_signed);

/** Rewrite every constraint and soft-constraint root of `sp` into explicit
 * form (see above).
 *
 * Returns `sp` itself when nothing needed rewriting (or `sp` is already flagged
 * DVS_PROBLEM_F_EXPLICIT), else a malloc'd copy -- the original pool plus the
 * new nodes, with the roots replaced -- that the caller releases with
 * dvs_sv_release(). Every dvs_expr_t of `sp` is valid in the copy.
 *
 * On failure (out of memory, a width the node formats cannot hold, a variable
 * of unknown type) returns `sp` unchanged and sets *err non-zero. */
dvs_problem_t *dvs_sv_elaborate(dvs_problem_t *sp, dvs_sv_var_type_fn fn,
                               void *ud, int *err);

/** Incremental use of a private copy from dvs_sv_elaborate(): first sync into
 * `*elab` whatever the caller appended to `orig` in place since the copy was
 * taken (`*synced_used`, initially orig->pool.used at copy time, tracks that),
 * then elaborate the Boolean `root` of `orig` into the copy. `*elab` may move.
 * Returns 0 and the elaborated root, or -1. */
int dvs_sv_elaborate_more(dvs_problem_t **elab, const dvs_problem_t *orig,
                          uint32_t *synced_used, dvs_expr_t root,
                          dvs_expr_t *out_root);

/** Release a problem returned by dvs_sv_elaborate (no-op when it is `orig`). */
void dvs_sv_release(dvs_problem_t *orig, dvs_problem_t *elab);

#ifdef __cplusplus
}
#endif

#endif /* DVS_SV_H */
