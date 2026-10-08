#ifndef DVS_AIGSIM_H
#define DVS_AIGSIM_H

#include <stdint.h>

#include "dvs_aig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * dvs_aigsim — 64-lane bit-parallel simulation of an AIG's assertions
 * (docs/fw_hdl_implementation_plan.md, pieces 3.3 and 3.4).
 *
 * Only the cone of influence of the assertions is simulated; its free inputs
 * are the AIG inputs it reaches. Two uses:
 *
 *  - exhaustive: every assignment of the cone's inputs, 64 per pass. Decides
 *    the problem: SAT with a model, or UNSAT. Costs about
 *    dvs_aigsim_exhaustive_work() gate-word evaluations (decision D6).
 *  - random: passes of 64 seeded-random assignments. Can only find a model.
 *
 * The AIG is only read, so a sweep may run on another thread while a SAT
 * solver works on the same problem.
 */
enum {
    DVS_AIGSIM_UNDECIDED = 0,
    DVS_AIGSIM_SAT       = 1,
    DVS_AIGSIM_UNSAT     = 2,   /* exhaustive only */
};

typedef struct dvs_aigsim_s dvs_aigsim_t;

/** Build the simulator for the cone of `asserts` (copied). NULL when out of
 *  memory or the AIG is too large. */
dvs_aigsim_t *dvs_aigsim_new(const dvs_aig_t *m, const dvs_aig_node_t *asserts,
                             uint32_t n_asserts);
void dvs_aigsim_free(dvs_aigsim_t *s);

uint32_t dvs_aigsim_inputs(const dvs_aigsim_t *s);   /* free inputs in the cone */
uint32_t dvs_aigsim_gates(const dvs_aigsim_t *s);    /* AND gates in the cone   */
uint64_t dvs_aigsim_vectors(const dvs_aigsim_t *s);  /* assignments simulated   */

/** Gate-word evaluations a full exhaustive sweep costs; UINT64_MAX above 32
 *  inputs, where it is never run. */
uint64_t dvs_aigsim_exhaustive_work(const dvs_aigsim_t *s);

/** Sweep every assignment. Returns DVS_AIGSIM_SAT, DVS_AIGSIM_UNSAT, or
 *  DVS_AIGSIM_UNDECIDED when *stop became non-zero (polled every pass; NULL
 *  never stops) or there are more than 32 inputs. */
int dvs_aigsim_exhaustive(dvs_aigsim_t *s, const volatile int *stop);

/** Up to `blocks` passes of random assignments. DVS_AIGSIM_SAT or
 *  DVS_AIGSIM_UNDECIDED. */
int dvs_aigsim_random(dvs_aigsim_t *s, uint32_t blocks, uint64_t seed);

/** After DVS_AIGSIM_SAT: a malloc'd valuation of every AIG node (indexed by
 *  node id, 0..num_nodes) in which every assertion is 1; inputs outside the
 *  cone are 0. The caller frees it. NULL when out of memory. */
int8_t *dvs_aigsim_model(const dvs_aigsim_t *s);

#ifdef __cplusplus
}
#endif

#endif /* DVS_AIGSIM_H */
