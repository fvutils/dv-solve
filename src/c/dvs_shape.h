#ifndef DVS_SHAPE_H
#define DVS_SHAPE_H

#include <stdint.h>

#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * dvs_shape — what kind of operators a problem's constraints are made of.
 *
 * Counts the distinct expression nodes reachable from the hard constraints by
 * class. The SMT-LIB front end uses it to pick an engine for one-shot QF_BV
 * (docs/fw_hdl_implementation_plan.md, piece 3.1): CDCL is built for
 * randomization (ranges, orderings and a few equalities over a handful of
 * variables), bit-blast for circuits (XOR/AND/shift networks, multipliers).
 */
typedef struct {
    uint32_t n_constraints;  /* top-level conjuncts (AND spines flattened)   */
    uint32_t n_nodes;        /* distinct operator nodes (not vars/consts)    */
    uint32_t n_vars;         /* distinct variables referenced               */
    uint32_t n_bit;          /* & | ^ ~ shifts extract concat extend         */
    uint32_t n_arith;        /* + - unary -                                  */
    uint32_t n_mul;          /* * / %                                        */
    uint32_t n_cmp;          /* < <= > >=, range and set membership          */
    uint32_t n_eq;           /* == !=                                        */
    uint32_t n_bool;         /* && || ! ite                                  */
    uint32_t n_other;        /* sum, countones, clog2, array select, casts   */
    uint32_t truncated;      /* walk stopped at its node cap                 */
} dvs_shape_t;

/** Fill `out`. Returns 0, or -1 on allocation failure. */
int dvs_shape_compute(dvs_problem_t *p, dvs_shape_t *out);

/**
 * The engine routing rule for a one-shot solve (piece 3.1). CDCL is built for
 * randomization: ranges, orderings and a few equalities over a handful of
 * variables, where it answers in milliseconds and samples near uniformly. On
 * circuits -- XOR/AND/shift networks, multipliers, miters asserting two
 * circuits differ -- it stalls until its budget runs out, while bit-blast
 * answers in milliseconds (fwhdl-core baseline, plan section 1.3).
 * Returns why `sh` goes to bitblast, or NULL to keep CDCL. Used by the
 * SMT-LIB front end and by problem handles (dvs_handle.c).
 */
const char *dvs_shape_route(const dvs_shape_t *sh);

#ifdef __cplusplus
}
#endif

#endif /* DVS_SHAPE_H */
