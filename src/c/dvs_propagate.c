#include <stdint.h>
#include <time.h>
#include <string.h>
#include "dvs_propagator.h"
#include "dvs_ctx.h"
#include "dvs_trail.h"
#include "dvs_lcg.h"
#include "dvs_search.h"

/* ------------------------------------------------------------------ */
/* Internal: the guard index (guard_ix in dvs_ctx.h)                   */
/* ------------------------------------------------------------------ */

/* Make the guard index current for `nv` vars and `np` props. 0 on
 * success; -1 (no growable pool, or no memory) leaves the caller to scan. */
static int _guard_ix_build(dvs_ctx_t *ctx, uint32_t nv, uint32_t np) {
    if (ctx->guard_ix && !ctx->guard_ix_dirty
            && ctx->guard_ix_nvars == nv && ctx->guard_ix_nprops == np)
        return 0;
    if (!ctx->pool.alloc) return -1;
    const uint32_t *g = ctx->prop_guard_vars;
    uint32_t n_guarded = 0;
    for (uint32_t i = 0; i < np; i++)
        if (g[i] < nv) n_guarded++;
    size_t bytes = ((size_t)nv + 1u + n_guarded) * sizeof(uint32_t);
    if (bytes > ctx->guard_ix_bytes) {
        size_t cap = ctx->guard_ix_bytes * 2u > bytes ? ctx->guard_ix_bytes * 2u : bytes;
        uint32_t *b = (uint32_t *)DVS_ALLOC(ctx->alloc, cap);
        if (!b) return -1;
        if (ctx->guard_ix)
            DVS_RELEASE(ctx->alloc, ctx->guard_ix, ctx->guard_ix_bytes);
        ctx->guard_ix = b;
        ctx->guard_ix_bytes = cap;
    }
    uint32_t *off = ctx->guard_ix, *list = off + nv + 1u;
    memset(off, 0, ((size_t)nv + 1u) * sizeof *off);
    for (uint32_t i = 0; i < np; i++)
        if (g[i] < nv) off[g[i] + 1u]++;
    for (uint32_t v = 0; v < nv; v++) off[v + 1u] += off[v];
    /* Fill in id order, using off[v] as v's cursor, then shift back. */
    for (uint32_t i = 0; i < np; i++)
        if (g[i] < nv) list[off[g[i]]++] = i;
    for (uint32_t v = nv; v > 0; v--) off[v] = off[v - 1u];
    off[0] = 0;
    ctx->guard_ix_props  = list;
    ctx->guard_ix_nvars  = nv;
    ctx->guard_ix_nprops = np;
    ctx->guard_ix_dirty  = 0;
    return 0;
}

void dvs_guard_ix_free(dvs_ctx_t *ctx) {
    if (ctx->guard_ix)
        DVS_RELEASE(ctx->alloc, ctx->guard_ix, ctx->guard_ix_bytes);
    ctx->guard_ix = ctx->guard_ix_props = NULL;
    ctx->guard_ix_bytes = 0;
    ctx->guard_ix_nvars = ctx->guard_ix_nprops = 0;
}

/* ------------------------------------------------------------------ */
/* Internal: wake all watchers on a variable                          */
/* ------------------------------------------------------------------ */

static void _wake_var(dvs_ctx_t *ctx, uint32_t var_id) {
    uint32_t prop_ref = ctx->watcher_heads[var_id];
    while (prop_ref != EXPR_NULL) {
        Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool, prop_ref);

        /* Find next in chain for this variable before potentially re-queuing */
        uint32_t next = EXPR_NULL;

        if (p->flags & PROP_FLAG_WIDE_WATCH) {
            /* Wide-watch propagator (AllDifferent, SumEq, etc.):
             * Layout: hdr, n_vars, _capacity, var_ids[cap], watcher_nexts[cap].
             * Use _capacity to locate watcher_nexts correctly. */
            uint32_t *nv_ptr = (uint32_t *)((char *)p + sizeof(Propagator));
            uint32_t nv = nv_ptr[0];
            uint32_t cap = nv_ptr[1];
            uint32_t *vids = nv_ptr + 2;
            uint32_t *wnexts = vids + cap;
            for (uint32_t i = 0; i < nv; i++) {
                if (vids[i] == var_id) {
                    next = wnexts[i];
                    break;
                }
            }
        } else {
            PropWatchSect *ws = (PropWatchSect *)((char *)p + sizeof(Propagator));
            for (uint32_t i = 0; i < ws->n_watches; i++) {
                if (ws->var_ids[i] == var_id) {
                    next = ws->next_watchers[i];
                    break;
                }
            }
        }

        prop_enqueue(ctx, prop_ref);
        prop_ref = next;
    }

    /* Also re-enqueue any propagator whose guard is this variable.
     * Cap to n_prop_refs_capacity: when n_props grows past the side-table
     * capacity (e.g. via ITE-chain aux materialisation), the higher
     * prop_ids weren't registered and reading prop_refs[i] there would
     * be an out-of-bounds read into adjacent pool memory. The guard index
     * lists them; scanning every propagator on every wake made a search
     * quadratic in the problem (2000-element select: 79% of the time). */
    if (ctx->prop_guard_vars && ctx->prop_refs) {
        uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                       ? ctx->n_props : ctx->n_prop_refs_capacity;
        if (_guard_ix_build(ctx, ctx->n_vars, lim) == 0) {
            if (var_id >= ctx->guard_ix_nvars) return;
            for (uint32_t k = ctx->guard_ix[var_id]; k < ctx->guard_ix[var_id + 1u]; k++) {
                uint32_t i = ctx->guard_ix_props[k];
                if (ctx->prop_refs[i] != EXPR_NULL)
                    prop_enqueue(ctx, ctx->prop_refs[i]);
            }
            return;
        }
        for (uint32_t i = 0; i < lim; i++) {
            if (ctx->prop_guard_vars[i] == var_id &&
                ctx->prop_refs[i] != EXPR_NULL) {
                prop_enqueue(ctx, ctx->prop_refs[i]);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Queue management                                                    */
/* ------------------------------------------------------------------ */

void prop_enqueue(dvs_ctx_t *ctx, uint32_t prop_ref) {
    Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool, prop_ref);
    if (p->flags & (PROP_FLAG_IN_QUEUE | PROP_FLAG_ENTAILED)) return;

    p->flags    |= PROP_FLAG_IN_QUEUE;
    p->queue_next = EXPR_NULL;

    uint32_t lvl = p->priority;
    if (ctx->queue.heads[lvl] == EXPR_NULL) {
        ctx->queue.heads[lvl] = prop_ref;
        ctx->queue.tails[lvl] = prop_ref;
        ctx->queue.non_empty_mask |= (uint16_t)(1u << lvl);
    } else {
        Propagator *tail = (Propagator *)dvs_spool_ptr(&ctx->pool,
                                                       ctx->queue.tails[lvl]);
        tail->queue_next      = prop_ref;
        ctx->queue.tails[lvl] = prop_ref;
    }
}

/* ------------------------------------------------------------------ */
/* Guard management                                                    */
/* ------------------------------------------------------------------ */

void prop_set_guard(dvs_ctx_t *ctx, uint32_t prop_ref, uint32_t guard_var_id) {
    Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool, prop_ref);
    if (ctx->prop_guard_vars && p->prop_id < ctx->n_prop_refs_capacity) {
        ctx->prop_guard_vars[p->prop_id] = guard_var_id;
        ctx->guard_ix_dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Domain-tightening                                                   */
/* ------------------------------------------------------------------ */

#include "dvs_trail.h"

/* When LCG is active, notify the clause DB so that learnt clauses
 * containing literals on this variable can propagate (or detect
 * conflict).  Returns PROP_OK or PROP_CONFLICT. */
static inline PropResult _lcg_notify_lb(dvs_ctx_t *ctx, uint32_t var_id,
                                         int64_t new_lb) {
    if (!ctx->lcg) return PROP_OK;
    LCGCtx *lcg = (LCGCtx *)ctx->lcg;
    if (!lcg->enabled) return PROP_OK;
    return clause_notify_lb(&lcg->clause_db, ctx, var_id, new_lb);
}

static inline PropResult _lcg_notify_ub(dvs_ctx_t *ctx, uint32_t var_id,
                                         int64_t new_ub) {
    if (!ctx->lcg) return PROP_OK;
    LCGCtx *lcg = (LCGCtx *)ctx->lcg;
    if (!lcg->enabled) return PROP_OK;
    return clause_notify_ub(&lcg->clause_db, ctx, var_id, new_ub);
}

PropResult ctx_tighten_lb32(dvs_ctx_t *ctx, uint32_t var_id, int32_t new_lb) {
    Variable *v = &ctx->vars[var_id];
    if (new_lb <= v->lo) return PROP_OK;

    trail_record_lb(ctx, var_id, (int64_t)new_lb);  /* applies change */

    if (v->lo > v->hi) return PROP_CONFLICT;
    /* Clear the unassigned bit when the variable becomes singleton. */
    if (v->lo == v->hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_lb(ctx, var_id, (int64_t)new_lb);
}

PropResult ctx_tighten_ub32(dvs_ctx_t *ctx, uint32_t var_id, int32_t new_ub) {
    Variable *v = &ctx->vars[var_id];
    if (new_ub >= v->hi) return PROP_OK;

    trail_record_ub(ctx, var_id, (int64_t)new_ub);  /* applies change */

    if (v->lo > v->hi) return PROP_CONFLICT;
    /* Clear the unassigned bit when the variable becomes singleton. */
    if (v->lo == v->hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_ub(ctx, var_id, (int64_t)new_ub);
}

PropResult ctx_tighten_lb64(dvs_ctx_t *ctx, uint32_t var_id, int64_t new_lb) {
    Variable *v = &ctx->vars[var_id];
    int64_t curr = var_lo64(ctx, v);
    if (!var_b_gt(v, new_lb, curr)) return PROP_OK;  /* new_lb <= curr */

    /* Edge-crossing guard: a lower bound above the variable's largest
     * representable value empties the domain (e.g. unsigned `v > max`).
     * Detect it on the logical value before storage, where truncation
     * would otherwise hide the conflict. Sign-aware so an unsigned bound
     * in the upper half (> INT64_MAX) is not misread as negative. */
    if (var_b_gt(v, new_lb, var_repr_max(v))) return PROP_CONFLICT;

    trail_record_lb(ctx, var_id, new_lb);

    /* check conflict */
    int64_t lo = var_lo64(ctx, v);
    int64_t hi = var_hi64(ctx, v);
    if (var_b_gt(v, lo, hi)) return PROP_CONFLICT;
    /* Clear the unassigned bit when the variable becomes singleton. */
    if (lo == hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_lb(ctx, var_id, new_lb);
}

PropResult ctx_tighten_ub64(dvs_ctx_t *ctx, uint32_t var_id, int64_t new_ub) {
    Variable *v = &ctx->vars[var_id];
    int64_t curr = var_hi64(ctx, v);
    if (!var_b_lt(v, new_ub, curr)) return PROP_OK;  /* new_ub >= curr */

    /* Edge-crossing guard: an upper bound below the variable's smallest
     * representable value empties the domain (e.g. unsigned `v < 0`).
     * Detect it on the logical value before storage, where truncation
     * would otherwise reinterpret a negative bound as a huge unsigned one. */
    if (var_b_lt(v, new_ub, var_repr_min(v))) return PROP_CONFLICT;

    trail_record_ub(ctx, var_id, new_ub);

    int64_t lo = var_lo64(ctx, v);
    int64_t hi = var_hi64(ctx, v);
    if (var_b_gt(v, lo, hi)) return PROP_CONFLICT;
    /* Clear the unassigned bit when the variable becomes singleton. */
    if (lo == hi && var_id < 64)
        ctx->unassigned_mask &= ~(1ULL << var_id);
    if (ctx->watcher_heads) _wake_var(ctx, var_id);
    return _lcg_notify_ub(ctx, var_id, new_ub);
}

/* ------------------------------------------------------------------ */
/* Propagation loop                                                    */
/* ------------------------------------------------------------------ */

/* Trail entries beyond which a propagation is abandoned rather than allowed to
 * exhaust memory (32 bytes each: 256 MiB). See prop_aborted in dvs_ctx.h. */
#define DVS_PROP_TRAIL_CAP (1ull << 23)

static int _prop_over_budget(dvs_ctx_t *ctx) {
    if (ctx->trail_count > DVS_PROP_TRAIL_CAP) return 1;
    if (ctx->thrash_limit && ctx->prop_fires > ctx->thrash_limit) {
        ctx->thrash_hit = 1;      /* see _solver_solve */
        return 1;
    }
    if (ctx->prop_deadline > 0.0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if ((double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 > ctx->prop_deadline) return 1;
    }
    return 0;
}

PropResult dvs_solver_propagate(dvs_ctx_t *ctx) {
    if (ctx->prop_aborted) return PROP_CONFLICT;
    /* Only a propagator that fails in THIS call is the conflict's culprit. */
    ctx->conflict_prop_ref = EXPR_NULL;
    /* A clause conflict recorded by an earlier, unanalysed conflict (e.g. a
     * shaving probe) must not be mistaken for the source of the next one. */
    ctx->conflict_clause_idx = EXPR_NULL;
    while (ctx->queue.non_empty_mask) {
        /* Lowest set bit = highest priority level with entries */
        uint32_t lvl = (uint32_t)__builtin_ctz(ctx->queue.non_empty_mask);

        /* Dequeue head */
        uint32_t    prop_ref = ctx->queue.heads[lvl];
        Propagator *p        = (Propagator *)dvs_spool_ptr(&ctx->pool, prop_ref);

        ctx->queue.heads[lvl] = p->queue_next;
        if (ctx->queue.heads[lvl] == EXPR_NULL) {
            ctx->queue.tails[lvl]      = EXPR_NULL;
            ctx->queue.non_empty_mask &= (uint16_t)~(1u << lvl);
        }
        p->queue_next  = EXPR_NULL;
        p->flags      &= (uint8_t)~PROP_FLAG_IN_QUEUE;

        if (p->flags & PROP_FLAG_ENTAILED) continue;

        ctx->prop_fires++;
        if ((++ctx->prop_ticks & 0x3FFu) == 0 && _prop_over_budget(ctx)) {
            /* Put back the propagator just taken: an abandoned propagation
             * must leave the queue whole, or whoever carries on from here
             * (a solve after a pin) never runs it. */
            prop_enqueue(ctx, prop_ref);
            ctx->prop_aborted = 1;
            return PROP_CONFLICT;
        }

        /* Guard-gated check: skip or entail based on guard variable */
        if (ctx->prop_guard_vars && p->prop_id < ctx->n_prop_refs_capacity) {
            uint32_t gid = ctx->prop_guard_vars[p->prop_id];
            if (gid != EXPR_NULL) {
                int64_t glo = var_lo64(ctx, &ctx->vars[gid]);
                int64_t ghi = var_hi64(ctx, &ctx->vars[gid]);
                if (glo == 0 && ghi == 0) {
                    /* Guard is false: propagator is entailed */
                    p->flags |= PROP_FLAG_ENTAILED;
                    continue;
                }
                if (glo != ghi) {
                    /* Guard is undecided: skip this firing */
                    continue;
                }
                /* Guard is true (glo == ghi == 1): fire normally */
            }
        }

        /* Learnt clauses wait until the fire returns. A propagator reads
         * bounds, tightens, and judges entailment from what it read; a
         * clause propagating inside one of its tightenings moved those
         * bounds under it. `x == y` with both in [4,5] tightened x, a clause
         * fixed x = 4 and then y = 5, and the propagator saw two singletons
         * and called itself entailed: a model with x != y (array selects
         * under learning, once clause chains were followed). */
        ClauseDB *cdb = (ctx->lcg && ((LCGCtx *)ctx->lcg)->enabled)
                        ? &((LCGCtx *)ctx->lcg)->clause_db : NULL;
        int held = cdb && clause_notify_hold(cdb);

        /* Tag the firing propagator so trail_record_* attributes the
         * resulting bound tightenings to it. Cleared after fire so
         * non-propagation tightenings (e.g. decision pushes, level-0
         * compile-time clamps) record reason=EXPR_NULL. */
        ctx->current_prop_ref = prop_ref;
        PropResult r = p->fire(p, ctx);
        ctx->current_prop_ref = EXPR_NULL;
        /* A clause conflict leaves conflict_clause_idx set for analysis. An
         * entailment holds through the clauses' tightenings: they only
         * narrow the domains it held over. */
        if (held && clause_notify_release(cdb, ctx, r != PROP_CONFLICT) == PROP_CONFLICT)
            r = PROP_CONFLICT;
        if (r == PROP_CONFLICT) {
            /* Stash the conflicting propagator for downstream conflict
             * analysis (dvs_analyze_conflict). */
            ctx->conflict_prop_ref = prop_ref;
            /* Keep it queued. A backtrack clears the queue; without one
             * (a root conflict, a refuted pin) the next propagation must
             * find the conflict again. A propagator can fail without
             * emptying a domain (the ite's, on two disjoint branches), and
             * dropped here, the next solve started from the contradictory
             * bounds with nothing queued to object and returned a model
             * that broke the constraint (B104). */
            prop_enqueue(ctx, prop_ref);
            return PROP_CONFLICT;
        }
        if (r == PROP_ENTAILED) p->flags |= PROP_FLAG_ENTAILED;
    }
    return PROP_OK;
}

/* ------------------------------------------------------------------ */
/* dvs_solver_propagate_only -- public propagation-only feasibility check */
/* ------------------------------------------------------------------ */

PropResult dvs_solver_propagate_only(dvs_ctx_t *ctx) {
    dvs_search_discard(ctx);
    PropResult r = dvs_solver_propagate(ctx);
    /* Abandoned at the trail cap, not refuted: no conflict to report, and
     * the flag must not outlive the call (every later propagation would
     * fail on it). */
    if (r == PROP_CONFLICT && ctx->prop_aborted) {
        ctx->prop_aborted = 0;
        return PROP_OK;
    }
    return r;
}
