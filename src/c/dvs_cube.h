#ifndef DVS_CUBE_H
#define DVS_CUBE_H

#include <stdint.h>

#include "dvs_bbsolver.h"

/**
 * dvs_cube — cube-and-conquer driver over the bit-blast engine.
 *
 * Search-space partitioning for single hard QF_BV instances: bit-blast once,
 * then split the search space into cubes (each a set of assumption literals)
 * and solve them over the shared encoded instance. See
 * docs/cube_and_conquer_design.md.
 *
 * Phase 0 (this file): a sequential, single-literal structural split
 * (Option B in the design). The one split literal x yields the two exhaustive
 * cubes {x} and {~x}; their union is the whole space, so the soundness
 * contract is preserved:
 *   - SAT  as soon as any cube is SAT (its model is a model of the original).
 *   - UNSAT only if BOTH cubes proved UNSAT (exhaustive partition).
 *   - UNKNOWN otherwise (a cube hit a budget / non-exhaustive) — never a
 *     claimed UNSAT we did not prove.
 *
 * Requires an incremental SAT backend (CaDiCaL) for retractable assumptions;
 * without it (or without a usable split literal) it falls back to a single
 * plain solve, so the cube engine is never worse than plain bit-blast.
 *
 * The bbsolver is created and owned by the caller; on SAT the model is read
 * back via dvs_bbsolver_value[_wide] exactly as for dvs_bbsolver_check.
 * Returns DVS_BB_SAT / DVS_BB_UNSAT / DVS_BB_UNKNOWN / DVS_BB_ERROR.
 */

#ifdef __cplusplus
extern "C" {
#endif

int dvs_cube_check(dvs_bbsolver_t *bb, uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif /* DVS_CUBE_H */
