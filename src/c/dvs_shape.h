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

#ifdef __cplusplus
}
#endif

#endif /* DVS_SHAPE_H */
