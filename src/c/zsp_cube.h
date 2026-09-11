#ifndef ZSP_CUBE_H
#define ZSP_CUBE_H

#include <stdint.h>

#include "zsp_bbsolver.h"

/**
 * zsp_cube — cube-and-conquer driver over the bit-blast engine.
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
 * back via zsp_bbsolver_value[_wide] exactly as for zsp_bbsolver_check.
 * Returns ZSP_BB_SAT / ZSP_BB_UNSAT / ZSP_BB_UNKNOWN / ZSP_BB_ERROR.
 */

#ifdef __cplusplus
extern "C" {
#endif

int zsp_cube_check(zsp_bbsolver_t *bb, uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_CUBE_H */
