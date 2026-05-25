#ifndef ZSP_AIG_H
#define ZSP_AIG_H

#include <stddef.h>
#include <stdint.h>

#include "zsp_alloc.h"

/**
 * zsp_aig — And-Inverter Graph for bit-blasting.
 *
 * Ported from bitwuzla's lib/bitblast/aig/* to C. Differences vs upstream:
 *  - No reference counting / GC. The AIG is built monotonically during a
 *    single check-sat and freed wholesale by zsp_aig_free().
 *  - Nodes are represented as signed 32-bit ids: positive id = positive
 *    literal, negative id = negated literal. 0 means "null". TRUE = 1,
 *    FALSE = -1.
 *  - Node data is stored in a flat arena indexed by (|id| - 1).
 *
 * Hash consing of AND gates uses an open-chained bucket table keyed by
 * (left_id, right_id). All Brummayer/Biere level-1..4 rewriting rules from
 * the original AigManager::rewrite_and are preserved.
 */

typedef int32_t zsp_aig_node_t;

#define ZSP_AIG_NULL   ((zsp_aig_node_t)0)
#define ZSP_AIG_TRUE   ((zsp_aig_node_t)1)
#define ZSP_AIG_FALSE  ((zsp_aig_node_t)-1)

typedef struct zsp_aig_s zsp_aig_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Create a fresh AIG manager. `alloc` may be NULL. */
zsp_aig_t *zsp_aig_new(zsp_alloc_t *alloc);

/** Destroy and free all memory. */
void zsp_aig_free(zsp_aig_t *m);

/** TRUE / FALSE constants. */
static inline zsp_aig_node_t zsp_aig_true(void)  { return ZSP_AIG_TRUE; }
static inline zsp_aig_node_t zsp_aig_false(void) { return ZSP_AIG_FALSE; }

/** Negate a node (sign flip). */
static inline zsp_aig_node_t zsp_aig_not(zsp_aig_node_t a) { return -a; }

/** Allocate a fresh input (uninterpreted) AIG variable. */
zsp_aig_node_t zsp_aig_mk_input(zsp_aig_t *m);

/**
 * Create an AND gate. Applies all Brummayer/Biere two-level rewriting rules
 * and hash-consing; the returned node may be a constant, an existing node,
 * or a freshly created one.
 */
zsp_aig_node_t zsp_aig_mk_and(zsp_aig_t *m, zsp_aig_node_t a, zsp_aig_node_t b);

/** Convenience: OR = !( !a /\ !b ). */
static inline zsp_aig_node_t zsp_aig_mk_or(zsp_aig_t *m,
                                           zsp_aig_node_t a,
                                           zsp_aig_node_t b) {
    return -zsp_aig_mk_and(m, -a, -b);
}

/** Convenience: XOR via two ANDs. */
zsp_aig_node_t zsp_aig_mk_xor(zsp_aig_t *m, zsp_aig_node_t a, zsp_aig_node_t b);

/** Convenience: a == b = !(a XOR b). */
static inline zsp_aig_node_t zsp_aig_mk_iff(zsp_aig_t *m,
                                            zsp_aig_node_t a,
                                            zsp_aig_node_t b) {
    return -zsp_aig_mk_xor(m, a, b);
}

/** Convenience: ITE(c, t, e) = (c /\ t) v (~c /\ e). */
zsp_aig_node_t zsp_aig_mk_ite(zsp_aig_t *m,
                              zsp_aig_node_t c,
                              zsp_aig_node_t t,
                              zsp_aig_node_t e);

/** Predicates. */
int zsp_aig_is_const(const zsp_aig_t *m, zsp_aig_node_t n);  /* TRUE/FALSE */
int zsp_aig_is_and(const zsp_aig_t *m, zsp_aig_node_t n);
int zsp_aig_is_input(const zsp_aig_t *m, zsp_aig_node_t n);

/** Get children of an AND node (in node-id form). Asserts is_and. */
void zsp_aig_get_children(const zsp_aig_t *m,
                          zsp_aig_node_t n,
                          zsp_aig_node_t *left_out,
                          zsp_aig_node_t *right_out);

/** Statistics. */
/** Number of AND nodes that reference n as a child (monotonic). */
uint32_t zsp_aig_parents(const zsp_aig_t *m, zsp_aig_node_t n);

uint64_t zsp_aig_num_nodes(const zsp_aig_t *m);
uint64_t zsp_aig_num_ands(const zsp_aig_t *m);
uint64_t zsp_aig_num_inputs(const zsp_aig_t *m);
uint64_t zsp_aig_num_shared(const zsp_aig_t *m);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_AIG_H */
