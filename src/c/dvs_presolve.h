#ifndef DVS_PRESOLVE_H
#define DVS_PRESOLVE_H
#include <stddef.h>
#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Linear-equality presolve of an EXPLICIT problem (dvs_presolve.c).
 *
 * Adds to `*psp` the constant values that its top-level hard equalities
 * imply for its linear subexpressions: from `sa == sb` and `sa + sb == 400`
 * it adds `sa == 200` and `sb == 200`, which interval propagation cannot
 * derive. The added constraints are implied, so the solution set does not
 * change. `*psp` (malloc'ed, `*psize` bytes) may be reallocated.
 *
 * @return the number of constraints added (0 when nothing applies or on any
 *         failure, leaving the problem as it was). */
uint32_t dvs_presolve_linear(dvs_problem_t **psp, size_t *psize);

#ifdef __cplusplus
}
#endif
#endif
