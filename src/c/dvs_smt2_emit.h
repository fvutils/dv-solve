/*
 * dvs_smt2_emit -- a builder-API problem as SMT-LIB2 (QF_BV).
 *
 * The problem is translated under the builder API's SystemVerilog sizing and
 * signedness rules (dvs_sv.h) by this file's own reading of them -- a symbolic
 * port of the model validator's evaluator (dvs_validate.c), NOT the output of
 * dvs_sv_elaborate(). A reference solver given this text is therefore an
 * independent check of elaboration as well as of the engines. See
 * docs/api_oracle_plan.md §3.
 *
 * Variable `id` is the constant `v<id>`. Internal; the public entry point is
 * dvs_problem_write_smt2() (dv_solve.h).
 */
#ifndef DVS_SMT2_EMIT_H
#define DVS_SMT2_EMIT_H

#include <stdint.h>
#include "dvs_problem.h"
#include "dvs_oracle_core.h"   /* DvsOBuf */

#ifdef __cplusplus
extern "C" {
#endif

/* Semantics flags of an emission (OR-ed into *flags). */
#define DVS_EMIT_F_DIV          0x1u  /* uses / or %: SMT-LIB defines x/0, SV does not */
#define DVS_EMIT_F_AGG          0x2u  /* uses sum / countones / clog2 / array_select */
#define DVS_EMIT_F_UNSUPPORTED  0x4u  /* something could not be translated */

/* Type of a variable the problem names but does not declare (an added
 * problem over the context's variables). 0 and the outputs, or -1. */
typedef int (*dvs_emit_var_type_fn)(void *ud, uint32_t var_id,
                                    uint16_t *width, uint8_t *is_signed);

/* Append `(declare-const v<id> (_ BitVec w))` and the assert of the declared
 * domain [lo, hi] (none when it is the whole type). */
void dvs_smt2_emit_var_decl(DvsOBuf *out, uint32_t id, uint16_t width,
                            uint8_t is_signed, int64_t lo, int64_t hi);

/* dvs_smt2_emit_var_decl() for every VarSpec of `p`. */
int dvs_smt2_emit_decls(const dvs_problem_t *p, DvsOBuf *out);

/* Append one assert per hard constraint and all-different of `p` (soft
 * constraints and distributions are not emitted). Shared sub-expressions
 * become define-funs named `<prefix><n>`, n counting up from *def_counter.
 * Variables without a VarSpec in `p` are typed by `fn` (may be NULL).
 * Returns 0, or -1 when something could not be translated (*flags then has
 * DVS_EMIT_F_UNSUPPORTED and `why`, if given, a description). */
int dvs_smt2_emit_asserts(const dvs_problem_t *p, dvs_emit_var_type_fn fn,
                          void *ud, const char *prefix, uint32_t *def_counter,
                          DvsOBuf *out, unsigned *flags, char *why, size_t why_n);

/* `(_ bv<N> w)` for the low `w` bits of `value`; for w > 64, value >= 0. */
void dvs_smt2_emit_value(DvsOBuf *out, int64_t value, uint16_t w);

#ifdef __cplusplus
}
#endif

#endif /* DVS_SMT2_EMIT_H */
