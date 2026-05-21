/*
 * zsp_conflict.c — Conflict analysis (embedded-profile stub)
 *
 * In the embedded profile, conflict analysis is performed implicitly by
 * the search loop in zsp_search.c (simple chronological backtracking with
 * domain exclusion).  This file provides logging/stats hooks that can be
 * expanded to full 1-UIP analysis in a later phase.
 */
#include <stdint.h>
#include "zsp_ctx.h"

/* ------------------------------------------------------------------ */
/* analyze_conflict (stub)                                            */
/*                                                                     */
/* Returns the backjump level.  For embedded-profile (Phase 7),       */
/* always returns decision_level - 1 (chronological backtracking).    */
/* ------------------------------------------------------------------ */
uint32_t analyze_conflict(SolveCtx *ctx) {
    if (ctx->decision_level == 0) return 0;
    return ctx->decision_level - 1;
}
