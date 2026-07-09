#ifndef ZSP_AIG_CNF_H
#define ZSP_AIG_CNF_H

#include <stdint.h>

#include "zsp_aig.h"
#include "zsp_alloc.h"
#include "zsp_sat.h"

/**
 * zsp_aig_cnf — Tseitin-encode an AIG into clauses for the SAT layer.
 *
 * Ported from bitwuzla's AigCnfEncoder. The encoder keeps a per-AIG-node
 * "already encoded" bitmap, so calling encode() on multiple roots over the
 * same AIG manager is fast and deduplicated.
 *
 * Mapping from AIG ids to SAT variable ids is the identity: AIG id `n`
 * (1-based) becomes SAT variable `n`. This makes the encoder a pure pass
 * over the AIG without needing a side table.
 *
 * The encoder also detects 2-level patterns that encode an ITE and emits
 * the standard 4-clause ITE encoding instead of two separate AND triples,
 * provided extracting the ITE does not destroy sharing (parents() == 1 on
 * the relevant subterms). This matches bitwuzla's optimization.
 */

typedef struct zsp_aig_cnf_s zsp_aig_cnf_t;

#ifdef __cplusplus
extern "C" {
#endif

zsp_aig_cnf_t *zsp_aig_cnf_new(zsp_alloc_t *alloc, zsp_aig_t *aig, zsp_sat_t *sat);
void           zsp_aig_cnf_free(zsp_aig_cnf_t *e);

/**
 * Encode `root` into CNF and add it as a constraint.
 *  - If `top_level` is non-zero, the root is treated as a conjunction of
 *    assertions: we flatten through positive ANDs and emit each leaf as a
 *    *unit* clause asserting it true. This is the standard "and(a,b) is a
 *    pair of assertions" optimization.
 *  - If `top_level` is zero, the node is encoded but not asserted; the
 *    caller is expected to add the asserting unit clause(s) themselves.
 */
void zsp_aig_cnf_encode(zsp_aig_cnf_t *e, zsp_aig_node_t root, int top_level);

/** Returns +1 if the AIG node is true under the current SAT model, -1 if
 *  false, 0 if unknown (not encoded). Valid only after zsp_sat_solve()
 *  returned ZSP_SAT_SAT. */
int zsp_aig_cnf_value(zsp_aig_cnf_t *e, zsp_aig_node_t node);

/** True if `node` is a non-constant AIG node that was never encoded into CNF --
 *  i.e. a don't-care whose value the SAT model does not determine. */
int zsp_aig_cnf_is_free(const zsp_aig_cnf_t *e, zsp_aig_node_t node);

/** Statistics. */
uint64_t zsp_aig_cnf_num_vars(const zsp_aig_cnf_t *e);
uint64_t zsp_aig_cnf_num_clauses(const zsp_aig_cnf_t *e);
uint64_t zsp_aig_cnf_num_literals(const zsp_aig_cnf_t *e);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_AIG_CNF_H */
