#ifndef DVS_NARROW_H
#define DVS_NARROW_H
/*
 * Narrowing of expressions wider than 64 bits over variables of 64 bits or
 * less (dvs_narrow.c, compiled as part of dvs_image.c). Needs only libc: the
 * writer runs it in dvs_problem_validate(), the runtime when a handle is
 * made, so the two agree on what is left.
 */
#include <stddef.h>
#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Rewrite each constraint and soft constraint of `*psp` (at the top and in
 * foreach bodies) that reads an expression over 64 bits into an equal one
 * that reads none, where it can: the CDCL engine compiles nothing wider.
 * A value over 64 bits is taken apart into slices of at most 64 bits (of
 * variables, constants and narrow subexpressions); zero and sign extensions,
 * concatenations, slices, casts, bitwise operators and shifts by a constant
 * move the slices, and + - * whose result provably fits 63 bits unsigned (64
 * signed) are done at that width. A comparison of two such values compares
 * them at 64 bits or less, or slice by slice (lexicographically, the top
 * slice signed in a signed context). Each rewrite is an equivalence under
 * the SV rules (dvs_sv.h), and its nodes are in EXPLICIT form, so it means
 * the same in an EXPLICIT problem.
 *
 * Distribution expressions are not rewritten. `*psp` (malloc'ed, `*psize`
 * bytes) may be reallocated; a root that cannot be narrowed keeps its form.
 *
 * @param why  If not NULL, receives the first root left wide, as
 *             "UNSUPPORTED_WIDTH: ...".
 * @return the number of roots still reading an expression over 64 bits (0:
 *         none), or -1 when memory ran out (the problem is then valid, with
 *         the roots rewritten so far). */
int dvs_narrow_problem(dvs_problem_t **psp, size_t *psize, char *why, size_t whylen);

/** Whether a declared variable or array element of `p` is over 64 bits. */
int dvs_narrow_has_wide_decl(const dvs_problem_t *p);

#ifdef __cplusplus
}
#endif

#endif /* DVS_NARROW_H */
