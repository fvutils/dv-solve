#ifndef DVS_AIG_H
#define DVS_AIG_H

#include <stddef.h>
#include <stdint.h>

#include "dvs_alloc.h"

/**
 * dvs_aig — And-Inverter Graph for bit-blasting.
 *
 * Ported from bitwuzla's lib/bitblast/aig/* to C. Differences vs upstream:
 *  - No reference counting / GC. The AIG is built monotonically during a
 *    single check-sat and freed wholesale by dvs_aig_free().
 *  - Nodes are represented as signed 32-bit ids: positive id = positive
 *    literal, negative id = negated literal. 0 means "null". TRUE = 1,
 *    FALSE = -1.
 *  - Node data is stored in a flat arena indexed by (|id| - 1).
 *
 * Hash consing of AND gates uses an open-chained bucket table keyed by
 * (left_id, right_id). All Brummayer/Biere level-1..4 rewriting rules from
 * the original AigManager::rewrite_and are preserved.
 */

typedef int32_t dvs_aig_node_t;

#define DVS_AIG_NULL   ((dvs_aig_node_t)0)
#define DVS_AIG_TRUE   ((dvs_aig_node_t)1)
#define DVS_AIG_FALSE  ((dvs_aig_node_t)-1)

typedef struct dvs_aig_s dvs_aig_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Create a fresh AIG manager. `alloc` may be NULL. */
dvs_aig_t *dvs_aig_new(dvs_alloc_t *alloc);

/** Destroy and free all memory. */
void dvs_aig_free(dvs_aig_t *m);

/** TRUE / FALSE constants. */
static inline dvs_aig_node_t dvs_aig_true(void)  { return DVS_AIG_TRUE; }
static inline dvs_aig_node_t dvs_aig_false(void) { return DVS_AIG_FALSE; }

/** Negate a node (sign flip). */
static inline dvs_aig_node_t dvs_aig_not(dvs_aig_node_t a) { return -a; }

/** Allocate a fresh input (uninterpreted) AIG variable. */
dvs_aig_node_t dvs_aig_mk_input(dvs_aig_t *m);

/**
 * Create an AND gate. Applies all Brummayer/Biere two-level rewriting rules
 * and hash-consing; the returned node may be a constant, an existing node,
 * or a freshly created one.
 */
dvs_aig_node_t dvs_aig_mk_and(dvs_aig_t *m, dvs_aig_node_t a, dvs_aig_node_t b);

/** Convenience: OR = !( !a /\ !b ). */
static inline dvs_aig_node_t dvs_aig_mk_or(dvs_aig_t *m,
                                           dvs_aig_node_t a,
                                           dvs_aig_node_t b) {
    return -dvs_aig_mk_and(m, -a, -b);
}

/** Convenience: XOR via two ANDs. */
dvs_aig_node_t dvs_aig_mk_xor(dvs_aig_t *m, dvs_aig_node_t a, dvs_aig_node_t b);

/** Convenience: a == b = !(a XOR b). */
static inline dvs_aig_node_t dvs_aig_mk_iff(dvs_aig_t *m,
                                            dvs_aig_node_t a,
                                            dvs_aig_node_t b) {
    return -dvs_aig_mk_xor(m, a, b);
}

/** Convenience: ITE(c, t, e) = (c /\ t) v (~c /\ e). */
dvs_aig_node_t dvs_aig_mk_ite(dvs_aig_t *m,
                              dvs_aig_node_t c,
                              dvs_aig_node_t t,
                              dvs_aig_node_t e);

/** Predicates. */
int dvs_aig_is_const(const dvs_aig_t *m, dvs_aig_node_t n);  /* TRUE/FALSE */
int dvs_aig_is_and(const dvs_aig_t *m, dvs_aig_node_t n);
int dvs_aig_is_input(const dvs_aig_t *m, dvs_aig_node_t n);

/** Get children of an AND node (in node-id form). Asserts is_and. */
void dvs_aig_get_children(const dvs_aig_t *m,
                          dvs_aig_node_t n,
                          dvs_aig_node_t *left_out,
                          dvs_aig_node_t *right_out);

/** Statistics. */
/** Number of AND nodes that reference n as a child (monotonic). */
uint32_t dvs_aig_parents(const dvs_aig_t *m, dvs_aig_node_t n);

uint64_t dvs_aig_num_nodes(const dvs_aig_t *m);
uint64_t dvs_aig_num_ands(const dvs_aig_t *m);
uint64_t dvs_aig_num_inputs(const dvs_aig_t *m);
uint64_t dvs_aig_num_shared(const dvs_aig_t *m);

#ifdef __cplusplus
}
#endif

#endif /* DVS_AIG_H */
