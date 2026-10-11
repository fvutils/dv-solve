#ifndef DVS_AFFINE_H
#define DVS_AFFINE_H

#include <stdint.h>

#include "dvs_aig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * dvs_affine — decide XOR-heavy assertions by Gaussian elimination over GF(2)
 * (docs/fw_hdl_implementation_plan.md, piece 4.1).
 *
 * The AIG's XOR gates (the three-AND shape dvs_aig_mk_xor, dvs_aig_mk_ite and
 * the rewriter produce) are recognized structurally. So are small tables: a
 * gate whose cone, cut at affine nodes, has at most 8 leaves and an affine
 * truth table in them (a CRC lookup table). Every node reachable
 * from the assertions through affine gates gets an affine form: a GF(2) sum of
 * columns plus a constant. A column is an AIG input or an "opaque" node, an
 * AND gate that is not an XOR, treated as a free variable.
 *
 * The assertions are split into conjuncts. A conjunct is either an equation
 * (an affine literal that must be 1) or a clause (a negated AND tree: one of
 * its affine literals must be 1).
 *
 *  - UNSAT: the equations are inconsistent, or they force every literal of
 *    some clause to 0. Sound: a real model, with each opaque column set to its
 *    node's value, satisfies the relaxed system.
 *  - SAT: the system has a solution (one literal per clause made 1, greedily)
 *    and simulating the AIG on its input values satisfies every assertion. The
 *    simulation is what makes it sound when opaque columns are involved.
 *  - otherwise undecided, and the caller goes on to simulation and SAT.
 */
enum {
    DVS_AFFINE_UNDECIDED = 0,
    DVS_AFFINE_SAT       = 1,
    DVS_AFFINE_UNSAT     = 2,
};

typedef struct {
    uint32_t xors;       /* affine gates in the region (XORs, tables) */
    uint32_t tables;     /* small affine tables found in the AIG      */
    uint32_t inputs;     /* input columns                             */
    uint32_t opaque;     /* non-XOR AND gates used as columns         */
    uint32_t equations;
    uint32_t clauses;
    uint32_t rank;       /* pivots after elimination                  */
    const char *why;     /* reason for UNDECIDED, or the deciding rule */
} dvs_affine_stats_t;

/** Run the analysis on the cone of `asserts`. `max_work` bounds the 64-bit
 *  word operations (form storage and elimination; 0 = default 4e7). On
 *  DVS_AFFINE_SAT, *model_out (if non-NULL) receives a malloc'd valuation of
 *  every node (indexed by id, 0..num_nodes) like dvs_aigsim_model(). */
int dvs_affine_check(const dvs_aig_t *m, const dvs_aig_node_t *asserts,
                     uint32_t n_asserts, uint64_t max_work,
                     int8_t **model_out, dvs_affine_stats_t *st);

#ifdef __cplusplus
}
#endif

#endif /* DVS_AFFINE_H */
