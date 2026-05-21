/*
 * zsp_nogood.c — Nogood ring buffer (stub for Phase 7)
 *
 * Full two-watched-literal propagation and ring-buffer eviction are
 * Phase 11 extensions.  This file provides the structure and stubs
 * needed so the rest of the code compiles.
 */
#include <stdint.h>
#include "zsp_ctx.h"

/*
 * Nogood struct (future use): a conflict clause stored as a compact
 * list of (var_id, bound) literals.
 *
 * For Phase 7, no nogoods are added — the search relies on domain
 * exclusion during backtracking.
 */

/* Stub: add a nogood to the ring buffer. No-op in Phase 7. */
void nogood_add(SolveCtx *ctx, uint32_t *var_ids, int32_t *bounds,
                uint32_t n_lits) {
    (void)ctx; (void)var_ids; (void)bounds; (void)n_lits;
}

/* Stub: propagate all nogoods.  Returns PROP_OK always in Phase 7. */
int nogood_propagate(SolveCtx *ctx) {
    (void)ctx;
    return 0;  /* PROP_OK */
}
