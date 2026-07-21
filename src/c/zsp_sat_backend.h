#ifndef ZSP_SAT_BACKEND_H
#define ZSP_SAT_BACKEND_H

/*
 * Internal SAT backend seam (IPASIR-shaped). The public zsp_sat.c dispatcher
 * owns the shared bookkeeping (clause/var counters, the assumption buffer,
 * push/pop taint state) and forwards backend-specific operations through this
 * vtable. Each backend lives in its own translation unit:
 *
 *   zsp_sat_kissat.c   — one-shot, always compiled
 *   zsp_sat_cadical.c  — incremental, compiled only when ZSP_WITH_CADICAL
 *
 * Not a public header — do not install.
 */

#include <stddef.h>
#include <stdint.h>

#include "zsp_alloc.h"
#include "zsp_sat.h"

typedef struct zsp_sat_vtbl {
    /* Non-zero if the backend supports genuine incremental solving: real
     * per-solve assumptions and adding clauses/units after a solve. The
     * dispatcher uses this to choose selector-literal push/pop (incremental
     * backends) vs. the conservative rebuild-on-taint stub (kissat). */
    int             incremental;

    /* Destroy the backend impl. `alloc` is the same allocator passed at
     * create time (may be NULL). */
    void          (*destroy)(void *impl, zsp_alloc_t *alloc);

    /* Reserve variable capacity (optional hint). */
    void          (*reserve)(void *impl, zsp_sat_var_t max_var);

    /* Push one literal to the current clause; 0 finalizes it (DIMACS). */
    void          (*add)(void *impl, zsp_sat_lit_t lit);

    /* Solve under the given assumptions (may be NULL/0). Each backend applies
     * them in its native way (kissat: replayed as permanent unit clauses;
     * CaDiCaL: real per-solve assumptions). Returns ZSP_SAT_{SAT,UNSAT,UNKNOWN}. */
    int           (*solve)(void *impl, const zsp_sat_lit_t *assumps, size_t n);

    /* Model value of `var` after SAT: +var if true, -var if false. */
    zsp_sat_lit_t (*value)(void *impl, zsp_sat_var_t var);

    /* Was `lit` in the unsat core of the last (failed) assumption solve?
     * KISSAT stub returns 0; CaDiCaL returns real core membership. */
    int           (*failed)(void *impl, zsp_sat_lit_t lit);

    /* Diversity seed and search limits. */
    void          (*set_seed)(void *impl, uint64_t seed);
    void          (*set_conflict_limit)(void *impl, uint32_t limit);
    void          (*set_decision_limit)(void *impl, uint32_t limit);

    /* Register a terminate callback the backend polls during solve; when it
     * returns non-zero the solver aborts and returns ZSP_SAT_UNKNOWN. Optional
     * (NULL if the backend cannot be interrupted); NULL fn clears it. Used by
     * the parallel portfolio to stop losers the instant a peer produces a
     * verdict — the difference between joining on the fastest vs the slowest. */
    void          (*set_terminate)(void *impl, void *state, int (*fn)(void *state));

    /* Asynchronously request the CURRENT solve to abort (thread-safe: called
     * from another thread while this backend is inside solve()). Optional. For
     * kissat this raises its internal termination flag (the callback in
     * set_terminate is stored but never polled by this build); CaDiCaL instead
     * honors set_terminate directly, so its interrupt is a no-op. */
    void          (*interrupt)(void *impl);

    /* Clause-arena observers (kissat-specific telemetry; CaDiCaL returns 0). */
    size_t        (*arena_size_bytes)(void *impl);
    size_t        (*arena_capacity_bytes)(void *impl);
    size_t        (*arena_save_mark)(void *impl);
} zsp_sat_vtbl;

/* Backend factories. Return the opaque impl pointer and set *vt_out, or return
 * NULL on failure. */
void *zsp_sat_kissat_create(zsp_alloc_t *alloc, const zsp_sat_vtbl **vt_out);

#ifdef ZSP_WITH_CADICAL
void *zsp_sat_cadical_create(zsp_alloc_t *alloc, const zsp_sat_vtbl **vt_out);
#endif

#endif /* ZSP_SAT_BACKEND_H */
