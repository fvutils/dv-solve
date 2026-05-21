#ifndef ZSP_SHAVE_H
#define ZSP_SHAVE_H

#include <stdint.h>
#include "zsp_propagator.h"  /* PropResult */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolveCtx SolveCtx;

/**
 * Pre-search bounds shaving (Singleton Arc Consistency on bounds).
 *
 * For each variable, tentatively assigns each bound value and propagates.
 * If propagation conflicts, that bound value is infeasible and is
 * permanently excluded at level 0.  Repeats until a full pass produces
 * no domain reductions.
 *
 * @param ctx        Solver context (must be at decision_level 0).
 * @param max_iters  Maximum total bound removals before stopping.
 * @return PROP_OK on success, PROP_CONFLICT if all domains emptied.
 */
PropResult bounds_shave(SolveCtx *ctx, uint32_t max_iters);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_SHAVE_H */
