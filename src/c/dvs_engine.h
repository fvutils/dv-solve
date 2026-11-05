#ifndef DVS_ENGINE_H
#define DVS_ENGINE_H

/**
 * dvs_engine — the engine a context solves on (docs/solver_gap_plan.md,
 * step 3).
 *
 * Every context compiles its problem for CDCL, as it always has. With
 * DVS_ENGINE_AUTO (the default for dvs_solver_new) the bit-blast engine
 * answers instead when CDCL cannot compile the problem whole, or when a CDCL
 * search gives up. The caller sees
 * one context either way: dvs_solver_solve, _get_value, _reset and
 * _validate_model dispatch here.
 *
 * Bit-blast has no pins, value exclusion, checkpoints, incremental adds,
 * soft constraints, solve order or dist weights. A problem that uses the
 * last four stays on CDCL from the start. A call that needs one of the
 * first ones (dvs_engine_need_cdcl) moves the context back to CDCL for good
 * when CDCL compiled the problem whole, and fails otherwise.
 */
#include <stdint.h>
#include "dv_solve.h"
#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The rest of dvs_solver_compile, after the CDCL compile returned `rc`:
 *  choose the engine, and return the compile's result as the caller sees
 *  it (DVS_COMPILE_OK when bit-blast takes a problem CDCL left incomplete). */
int dvs_engine_compile(dvs_ctx_t *ctx, dvs_problem_t *p, int rc);

/** Solve on bit-blast when the context is there. Returns 1 and sets `*r`
 *  if it handled the solve, 0 to let CDCL solve. */
int dvs_engine_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts, dvs_result_t *r);

/** A CDCL search gave up: move to bit-blast when allowed, and solve there.
 *  Returns 1 and sets `*r` if it did. */
int dvs_engine_take_over(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts, dvs_result_t *r);

/** The bit-blast model's value of `var_id`. Returns 1 and sets `*v` when the
 *  context holds a bit-blast model, else 0. */
int dvs_engine_value(const dvs_ctx_t *ctx, uint32_t var_id, int64_t *v);

/** A call that only CDCL serves is about to run: stay on (or return to)
 *  CDCL for good. Returns 0, or -1 when CDCL could not compile the problem
 *  whole, so the call must fail. */
int dvs_engine_need_cdcl(dvs_ctx_t *ctx);

/** Release the engine state (dvs_solver_destroy). */
void dvs_engine_free(dvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DVS_ENGINE_H */
