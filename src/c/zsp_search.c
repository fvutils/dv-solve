#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include "zsp_search.h"
#include "zsp_ctx.h"
#include "zsp_propagator.h"
#include "zsp_trail.h"
#include "zsp_shave.h"
#include "zsp_lcg.h"
#include "zsp_explain.h"

/* Forward declarations for hole management */
static int _is_hole(const SolveCtx *ctx, uint32_t var_id, int64_t value);
static uint32_t _count_holes_in_range(const SolveCtx *ctx, uint32_t var_id,
                                       int64_t lo, int64_t hi);

/* ------------------------------------------------------------------ */
/* Luby sequence                                                       */
/* ------------------------------------------------------------------ */

/* Returns the n-th element (1-indexed) of the Luby sequence. */
static uint32_t _luby(uint32_t n) {
    /* Find k such that 2^(k-1) <= n < 2^k */
    uint32_t p = 1;
    uint32_t k = 1;
    while (p < n + 1) { p *= 2; k++; }
    if (p == n + 1) return p / 2;
    return _luby(n - (p / 2) + 1);
}

/* ------------------------------------------------------------------ */
/* Fast xorshift64 RNG                                                 */
/* ------------------------------------------------------------------ */

static uint64_t _rand64(SolveCtx *ctx) {
    uint64_t x = ctx->rng_state;
    if (x == 0) x = 0xDEADBEEF12345678ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    ctx->rng_state = x;
    return x;
}

/* Return a random integer in [lo, hi] (inclusive), 64-bit range. */
static int64_t _rand_range64(SolveCtx *ctx, int64_t lo, int64_t hi) {
    /* `hi - lo` is correct modulo 2^64 for both signed and unsigned bound
     * patterns. When the domain is the full 64-bit range, span wraps to 0
     * (2^64 mod 2^64): pick any 64-bit value (and avoid a div-by-zero). */
    uint64_t span = (uint64_t)(hi - lo) + 1u;
    if (span == 0u) return (int64_t)_rand64(ctx);
    return lo + (int64_t)(_rand64(ctx) % span);
}

/* ------------------------------------------------------------------ */
/* Variable selection — MRV (minimum remaining values)               */
/*                                                                     */
/* Returns the var_id of the unassigned variable with the smallest    */
/* domain, or EXPR_NULL if all variables are assigned.                */
/* ------------------------------------------------------------------ */

static uint32_t _select_unassigned(SolveCtx *ctx) {
    /* VSIDS path: when LCG is active and we have non-zero activity
     * (i.e. at least one conflict has fired the bumper), pick the
     * highest-activity unassigned, non-aux variable. Falls through to
     * MRV when no var has activity (initial decisions before any
     * conflict) so behavior matches the existing strategy until CDCL
     * has something to say. */
    if (ctx->lcg) {
        const LCGCtx *L = (const LCGCtx *)ctx->lcg;
        if (L->enabled && L->n_analyses > 0) {
            uint32_t best_v = EXPR_NULL;
            double   best_a = -1.0;
            uint32_t lim = L->vsids.n_vars < ctx->n_vars
                           ? L->vsids.n_vars : ctx->n_vars;
            for (uint32_t i = 0; i < lim; i++) {
                Variable *v = &ctx->vars[i];
                if (v->flags & VAR_AUX) continue;
                int64_t lo = var_lo64(ctx, v);
                int64_t hi = var_hi64(ctx, v);
                if (lo == hi) continue;
                if (L->vsids.activity[i] > best_a) {
                    best_a = L->vsids.activity[i];
                    best_v = i;
                }
            }
            if (best_v != EXPR_NULL) return best_v;
        }
    }

    uint32_t best         = EXPR_NULL;
    int64_t  best_dom     = INT64_MAX;
    uint32_t n_best       = 0;       /* # real vars tied at best_dom (reservoir) */
    uint32_t best_aux     = EXPR_NULL;
    int64_t  best_aux_dom = INT64_MAX;

    /* MRV (minimum remaining values) with a *randomized* tie-break: among the
     * unassigned real variables that share the smallest domain, pick one
     * uniformly at random (reservoir sampling). A fixed (lowest-index)
     * tie-break makes the same variable the first decision every solve, so its
     * marginal is uniform but every other variable is sampled over a
     * conditional (skewed) domain — e.g. under `a < b`, `a` (var 0) is always
     * decided first, starving `b`'s low values. Random tie-breaking gives every
     * variable a fair turn at being decided first, so all marginals become
     * uniform-ish and coverage follows the n·log(n) coupon-collector bound. */
    if (ctx->n_vars <= 64 && ctx->unassigned_mask != 0) {
        uint64_t m = ctx->unassigned_mask;
        while (m) {
            uint32_t i = (uint32_t)__builtin_ctzll(m);
            m &= m - 1;  /* clear lowest set bit */
            Variable *v = &ctx->vars[i];
            int64_t lo = var_lo64(ctx, v);
            int64_t hi = var_hi64(ctx, v);
            if (lo == hi) continue;  /* singleton -- already assigned */
            int64_t dom = hi - lo;
            if (v->flags & VAR_AUX) {
                /* Aux is a low-priority decision: prefer real vars first. */
                if (dom < best_aux_dom) { best_aux_dom = dom; best_aux = i; }
                continue;
            }
            if (dom < best_dom) {
                best_dom = dom; best = i; n_best = 1;
            } else if (ctx->fair_pick && dom == best_dom) {
                /* uniform mode: reservoir-sample among smallest-domain vars */
                n_best++;
                if ((_rand64(ctx) % n_best) == 0) best = i;
            }
            /* fast mode: keep the first (lowest-index) smallest-domain var */
        }
    } else {
        for (uint32_t i = 0; i < ctx->n_vars; i++) {
            Variable *v = &ctx->vars[i];
            int64_t lo = var_lo64(ctx, v);
            int64_t hi = var_hi64(ctx, v);
            if (lo == hi) continue;
            int64_t dom = hi - lo;
            if (v->flags & VAR_AUX) {
                if (dom < best_aux_dom) { best_aux_dom = dom; best_aux = i; }
                continue;
            }
            if (dom < best_dom) {
                best_dom = dom; best = i; n_best = 1;
            } else if (ctx->fair_pick && dom == best_dom) {
                /* uniform mode: reservoir-sample among smallest-domain vars */
                n_best++;
                if ((_rand64(ctx) % n_best) == 0) best = i;
            }
            /* fast mode: keep the first (lowest-index) smallest-domain var */
        }
    }
    if (best != EXPR_NULL) return best;
    return best_aux;
}

/* ------------------------------------------------------------------ */
/* Value selection                                                     */
/* ------------------------------------------------------------------ */

/* Pick a value from a distribution-constrained variable using weighted
 * random selection.  Returns the chosen value, or falls through to
 * uniform random if no valid dist entry intersects the feasible domain. */
static int64_t _pick_value_dist(SolveCtx *ctx, uint32_t var_id,
                                 int64_t lo, int64_t hi) {
    uint32_t dm_off = ctx->dist_offsets[var_id];
    if (dm_off == 0) return _rand_range64(ctx, lo, hi);

    DistMeta *dm = (DistMeta *)zsp_pool_ptr(&ctx->pool, dm_off);
    DistMetaEntry *entries = (DistMetaEntry *)(dm + 1);
    uint32_t ne = dm->n_entries;

    /* Recompute effective weights considering only ranges that intersect
     * the current feasible domain [lo, hi]. */
    uint64_t total_weight = 0;
    for (uint32_t i = 0; i < ne; i++) {
        if (entries[i].weight == 0) continue;
        int64_t elo = entries[i].lo < lo ? lo : entries[i].lo;
        int64_t ehi = entries[i].hi > hi ? hi : entries[i].hi;
        if (elo > ehi) continue;  /* no overlap with feasible domain */

        uint64_t ew;
        if (entries[i].is_per_value) {
            uint64_t count = (uint64_t)(ehi - elo) + 1u;
            ew = (uint64_t)entries[i].weight * count;
        } else {
            ew = (uint64_t)entries[i].weight;
        }
        total_weight += ew;
    }

    if (total_weight == 0) return _rand_range64(ctx, lo, hi);

    /* Roll a random number in [0, total_weight) */
    uint64_t roll = _rand64(ctx) % total_weight;

    /* Walk entries to find which one the roll falls into */
    uint64_t cum = 0;
    for (uint32_t i = 0; i < ne; i++) {
        if (entries[i].weight == 0) continue;
        int64_t elo = entries[i].lo < lo ? lo : entries[i].lo;
        int64_t ehi = entries[i].hi > hi ? hi : entries[i].hi;
        if (elo > ehi) continue;

        uint64_t ew;
        if (entries[i].is_per_value) {
            uint64_t count = (uint64_t)(ehi - elo) + 1u;
            ew = (uint64_t)entries[i].weight * count;
        } else {
            ew = (uint64_t)entries[i].weight;
        }

        if (roll < cum + ew) {
            /* This entry wins; pick a uniform value within [elo, ehi] */
            return _rand_range64(ctx, elo, ehi);
        }
        cum += ew;
    }

    /* Fallback (shouldn't reach here) */
    return _rand_range64(ctx, lo, hi);
}

/* Check if variable has any holes. */
static int _has_holes(const SolveCtx *ctx, uint32_t var_id) {
    return ctx->var_holes_head &&
           var_id < ctx->n_vars_capacity &&
           ctx->var_holes_head[var_id] != 0;
}

/* Pick a value avoiding holes via rejection sampling, with fallback
 * to enumeration for domains with many holes. */
static int64_t _pick_avoiding_holes(SolveCtx *ctx, uint32_t var_id,
                                     int64_t candidate, int64_t lo, int64_t hi) {
    /* Fast path: no holes */
    if (!_has_holes(ctx, var_id)) return candidate;

    /* Rejection sampling: try up to 32 random picks */
    if (!_is_hole(ctx, var_id, candidate)) return candidate;

    for (int attempt = 0; attempt < 32; attempt++) {
        int64_t v = _rand_range64(ctx, lo, hi);
        if (!_is_hole(ctx, var_id, v)) return v;
    }

    /* Fallback: rejection sampling failed (the feasible set is a small fraction
     * of [lo,hi], e.g. a domain of {0,255} with 254 holes). Pick a
     * *uniformly-random* non-hole value rather than the first one. The old code
     * walked from lo and returned the first valid value, which biased selection
     * hard toward lo (a {0,255} domain returned 0 ~99% of the time). Count the
     * valid values and select the k-th, advancing through the sorted hole list
     * in O(#holes). */
    uint64_t n_total = (uint64_t)(hi - lo) + 1;
    uint32_t n_holes = _count_holes_in_range(ctx, var_id, lo, hi);
    if ((uint64_t)n_holes >= n_total) return lo;  /* all holes (shouldn't happen) */
    uint64_t k = _rand64(ctx) % (n_total - (uint64_t)n_holes);  /* k-th valid */
    int64_t cur = lo;
    uint32_t hoff = ctx->var_holes_head[var_id];
    while (hoff != 0) {
        const HoleEntry *he = (const HoleEntry *)zsp_pool_ptr(&ctx->pool, hoff);
        if (he->value < lo) { hoff = he->next; continue; }
        if (he->value > hi) break;
        uint64_t gap = (uint64_t)(he->value - cur);  /* valid count in [cur, hole) */
        if (k < gap) return cur + (int64_t)k;
        k -= gap;
        cur = he->value + 1;
        hoff = he->next;
    }
    return cur + (int64_t)k;
}

static int64_t _pick_value(SolveCtx *ctx, uint32_t var_id,
                            const SolveOpts *opts) {
    int64_t lo = var_lo64(ctx, &ctx->vars[var_id]);
    int64_t hi = var_hi64(ctx, &ctx->vars[var_id]);

    /* Phase saving: try the last saved value if still in domain. Sign-aware
     * so an unsigned upper-half saved value is range-checked correctly (a
     * signed compare could return an out-of-domain value). */
    if (opts && opts->use_phase_save && ctx->phase_save) {
        const Variable *v = &ctx->vars[var_id];
        int64_t ps = ctx->phase_save[var_id];
        if (!var_b_lt(v, ps, lo) && !var_b_gt(v, ps, hi)
                && !_is_hole(ctx, var_id, ps))
            return ps;
    }

    /* Distribution-weighted selection if this variable has a dist constraint */
    if (ctx->dist_offsets && var_id < ctx->n_vars_capacity &&
        ctx->dist_offsets[var_id] != 0) {
        int64_t v = _pick_value_dist(ctx, var_id, lo, hi);
        return _pick_avoiding_holes(ctx, var_id, v, lo, hi);
    }

    int64_t v = _rand_range64(ctx, lo, hi);
    return _pick_avoiding_holes(ctx, var_id, v, lo, hi);
}

/* ------------------------------------------------------------------ */
/* solver_solve                                                        */
/* ------------------------------------------------------------------ */

static SolveResult _solver_solve_core(SolveCtx *ctx, const SolveOpts *opts) {
    /* Seed or preserve RNG. */
    if (opts && opts->seed != 0) ctx->rng_state = opts->seed;
    if (ctx->rng_state == 0)     ctx->rng_state = 0xDEADBEEF12345678ULL;

    /* Decision-variable tie-break mode for this solve (default fast). */
    ctx->fair_pick = (opts && opts->fair_pick) ? 1 : 0;

    /* Lazy LCG initialization on first solve when use_lcg is requested. */
    if (opts && opts->use_lcg && !ctx->lcg && ctx->n_vars > 0) {
        LCGCtx *lcg = (LCGCtx *)calloc(1, sizeof(LCGCtx));
        if (lcg) {
            uint32_t nv = ctx->n_vars_capacity > 0
                          ? ctx->n_vars_capacity : ctx->n_vars;
            if (lcg_init(lcg, nv) == 0) {
                ctx->lcg = lcg;
            } else {
                free(lcg);
            }
        }
    }
    /* Register explain callbacks every solve — propagators added by
     * compile-time aux materialisation or incremental paths after the
     * first solve would otherwise miss their explain wiring. The walk
     * skips propagators whose explain is already set. */
    if (ctx->lcg) {
        contra_register_explanations(ctx);
    }

    /* Allocate phase_save array on first call (lazily from static pool). */
    if (opts && opts->use_phase_save && !ctx->phase_save && ctx->n_vars > 0) {
        /* Size to n_vars_capacity for incremental variable support */
        uint32_t ps_size = ctx->n_vars_capacity > 0
                           ? ctx->n_vars_capacity : ctx->n_vars;
        uint32_t ps_ref = zsp_pool_alloc(&ctx->pool,
                                          ps_size * (uint32_t)sizeof(int64_t),
                                          (uint32_t)_Alignof(int64_t));
        if (ps_ref != EXPR_NULL) {
            ctx->phase_save = (int64_t *)zsp_pool_ptr(&ctx->pool, ps_ref);
            /* For a diversity solve (nonzero seed) seed each var's initial phase
             * to a RANDOM in-domain value rather than its lower bound. Otherwise
             * _pick_value keeps returning the saved lower bound and an otherwise-
             * unconstrained (or loosely bounded) rand var never varies across
             * seeds. The value is still assigned through _pick_value ->
             * propagation, so coupled constraints stay respected. Seed 0
             * (BMC/decision) keeps the deterministic lower-bound phase. */
            int diversify = (opts->seed != 0);
            for (uint32_t i = 0; i < ctx->n_vars; i++) {
                int64_t lo = var_lo64(ctx, &ctx->vars[i]);
                int64_t hi = var_hi64(ctx, &ctx->vars[i]);
                ctx->phase_save[i] = diversify ? _rand_range64(ctx, lo, hi) : lo;
            }
        }
    }

    /* Default restart parameters: 100 conflicts per restart,
     * 10000 max restarts.  Caller can override via opts. */
    uint32_t max_conflicts  = (opts && opts->max_conflicts > 0)
                              ? opts->max_conflicts : 100;
    uint32_t max_restarts   = (opts && opts->max_restarts > 0)
                              ? opts->max_restarts  : 10000;
    uint32_t restart_count  = 0;
    uint32_t local_conflicts = 0;
    uint32_t luby_idx       = 1;  /* 1-indexed Luby sequence */
    uint32_t luby_limit     = (max_conflicts > 0)
                              ? _luby(luby_idx) * max_conflicts
                              : UINT32_MAX;

    /* Level-0 BCP */
    if (solver_propagate(ctx) == PROP_CONFLICT) return SOLVE_UNSAT;

    /* Check for domains that became empty before search (e.g. from
     * conflicting bounds imposed externally before solver_solve).
     * Sign-aware: an unsigned domain straddling 2^63 (lo < 2^63 <= hi)
     * has lo "positive" and hi "negative" as int64, so a bare `lo > hi`
     * would wrongly call it empty. */
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *vi = &ctx->vars[i];
        if (var_b_gt(vi, var_lo64(ctx, vi), var_hi64(ctx, vi)))
            return SOLVE_UNSAT;
    }

    /* Seal compile-time + initial-propagation state as the level-0 baseline.
     * level_marks[0] starts zeroed (trail_top=NULL), so trail_backtrack(ctx,0)
     * would undo every trail entry including compile-time domain narrowings
     * (e.g. (assert cond) → cond forced to [1,1]).  By resetting the mark
     * here we ensure restarts and bounds_shave probes only undo search-time
     * changes, not the compile-time ones. */
    ctx->level_marks[0].trail_top   = ctx->trail_top;
    ctx->level_marks[0].trail_count = ctx->trail_count;
    ctx->level_marks[0].stack_mark  = zsp_stack_push(ctx->dynamic);

    /* Pre-search bounds shaving (L2): tighten domains beyond what
     * individual propagator fixed-point can achieve. */
    uint32_t max_si = opts ? opts->max_shave_iters : 1000;
    if (max_si > 0) {
        PropResult sr = bounds_shave(ctx, max_si);
        if (sr == PROP_CONFLICT) return SOLVE_UNSAT;
    }

    for (;;) {
        /* ── Variable selection ── */
        uint32_t x_id = _select_unassigned(ctx);
        if (x_id == EXPR_NULL) {
#ifndef NDEBUG
            /* Verify all non-aliased *decision* variables are singleton at
             * SAT exit. Aux vars (VAR_AUX) are determined by propagation;
             * if the constraints don't pin them tightly we tolerate a
             * loose interval here — the model-validation pass will catch
             * any case where the looseness produces an inconsistent
             * answer. */
            for (uint32_t _di = 0; _di < ctx->n_vars; _di++) {
                if (ctx->var_alias && ctx->var_alias[_di] != _di) continue;
                if (ctx->vars[_di].flags & VAR_AUX) continue;
                int64_t _lo = var_lo64(ctx, &ctx->vars[_di]);
                int64_t _hi = var_hi64(ctx, &ctx->vars[_di]);
                (void)_lo; (void)_hi;
                assert(_lo == _hi && "SOLVE_OK but non-singleton domain");
            }
#endif
            return SOLVE_OK;   /* all assigned */
        }
        int64_t v = _pick_value(ctx, x_id, opts);

        /* Decision-depth guard. decisions/level_marks are fixed-size
         * (MAX_DECISION_DEPTH); the search pushes one level per decided
         * variable, so a problem with more decision variables than that depth
         * would overflow both arrays (out-of-bounds heap write -> crash).
         * Bail with TIMEOUT so the caller defers/escalates cleanly instead. */
        if (ctx->decision_level >= ctx->max_depth)
            return SOLVE_TIMEOUT;

        /* ── Record decision ── */
        uint32_t dec_idx = ctx->decision_level;   /* index before push */
        ctx->decisions[dec_idx].var_id      = x_id;
        ctx->decisions[dec_idx].tried_value = v;
        ctx->decisions[dec_idx].tried_lower = 0;

        /* ── Push level and assign ──
         * The tighten helpers auto-detect singleton pinning (lo == hi
         * after the change) and stamp TRAIL_FLAG_SINGLETON on the new
         * entry plus the companion-bound entry at the same level. */
        trail_push_level(ctx);
        PropResult pr = ctx_tighten_lb64(ctx, x_id, v);
        if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x_id, v);
        if (pr == PROP_OK) {
            pr = solver_propagate(ctx);
        }

        /* ── Conflict loop ── */
        while (pr == PROP_CONFLICT) {
            ctx->conflict_count++;
            local_conflicts++;

            uint32_t cur = ctx->decision_level;

            /* A conflict at decision level 0 means the problem is UNSAT:
             * every permanent domain tightening at level 0 is justified by
             * a propagation conflict, so if level-0 propagation itself
             * conflicts there is no search path that can satisfy it. */
            if (cur == 0) return SOLVE_UNSAT;

            /* Restart check */
            if (max_conflicts > 0 && local_conflicts >= luby_limit) {
                trail_backtrack(ctx, 0);
                local_conflicts = 0;
                restart_count++;
                luby_idx++;
                luby_limit = _luby(luby_idx) * max_conflicts;

                if (max_restarts > 0 && restart_count >= max_restarts)
                    return SOLVE_TIMEOUT;

                /* Clause GC: drop clauses with high LBD (literal block
                 * distance — distinct decision levels in the clause).
                 * Keep glue clauses (LBD ≤ 4) which are most useful for
                 * unit propagation; ditch the rest after the DB grows
                 * past a threshold. */
                if (ctx->lcg) {
                    LCGCtx *lcg_gc = (LCGCtx *)ctx->lcg;
                    if (lcg_gc->enabled &&
                        lcg_gc->clause_db.n_clauses > 2048) {
                        clause_db_gc(&lcg_gc->clause_db, /* lbd_threshold */ 4);
                    }
                }


                pr = solver_propagate(ctx);
                if (pr == PROP_CONFLICT) return SOLVE_UNSAT;
                break;  /* restart outer for-loop */
            }

            /* CDCL path: try to learn a clause from this conflict.
             * If lcg_analyze_conflict produces a non-empty learnt clause,
             * add it, backjump, and let unit propagation continue.
             * If it returns -1 (no explain available for some antecedent
             * propagator), fall through to the chronological bisection
             * path below — this is the safe fallback while propagator
             * explain callbacks are being rolled out. */
            {
                LCGCtx *lcg = (LCGCtx *)ctx->lcg;
                if (lcg && lcg->enabled) {
                    Literal  learnt_buf[MAX_CLAUSE_LITS];
                    uint32_t n_lits   = 0;
                    uint32_t bt_level = 0;
                    uint32_t lbd = 0;
                    int rc = lcg_analyze_conflict(lcg, ctx,
                                                   learnt_buf, &n_lits,
                                                   &bt_level, &lbd);
                    if (rc == 0 && n_lits > 0) {
                        /* Backjump first so trail state matches the
                         * level the asserting literal will fire at. */
                        if (bt_level >= cur) bt_level = cur - 1;
                        trail_backtrack(ctx, bt_level);

                        /* Record the learnt clause with its LBD (count
                         * of distinct decision levels). Clause GC at
                         * restart uses this to discard low-utility
                         * clauses. */
                        clause_db_add(&lcg->clause_db, n_lits,
                                       learnt_buf, lbd ? lbd : n_lits);
                        lcg->n_learnt++;

                        /* Force unit propagation from the new clause,
                         * then resume the propagator queue. */
                        pr = clause_propagate(&lcg->clause_db, ctx);
                        if (pr != PROP_CONFLICT) pr = solver_propagate(ctx);
                        continue;  /* re-enter while(pr==CONFLICT) */
                    }
                    /* rc != 0 or empty clause: fall through to bisection */
                }
            }

            /* Retrieve the decision that created this level */
            DecisionRecord *d   = &ctx->decisions[cur - 1];
            uint32_t        dv  = d->var_id;
            int64_t         val = d->tried_value;

            /* Backtrack to the previous level */
            trail_backtrack(ctx, cur - 1);

            /* Exclude `val` from dv's domain at the previous level.
             * Sign-aware ordering so an unsigned domain in/across the upper
             * half is split correctly (a signed compare/midpoint would mis-
             * order it and either declare it empty or bisect the wrong way). */
            const Variable *dvv = &ctx->vars[dv];
            int64_t dlo = var_lo64(ctx, dvv);
            int64_t dhi = var_hi64(ctx, dvv);
            if (var_b_gt(dvv, dlo, dhi)) {
                /* Domain already empty after backtrack — propagate conflict up */
                pr = PROP_CONFLICT;
            } else if (!var_b_gt(dvv, val, dlo)) {  /* val <= dlo */
                pr = ctx_tighten_lb64(ctx, dv, dlo + 1);
            } else if (!var_b_lt(dvv, val, dhi)) {  /* val >= dhi */
                pr = ctx_tighten_ub64(ctx, dv, dhi - 1);
            } else {
                /* Middle value: two-phase domain bisection.
                 * Split at the midpoint of [dlo, dhi] (not at val)
                 * for systematic, logarithmic-depth exploration.
                 * Phase 1: explore lower half [dlo, mid].
                 * Phase 2: explore upper half [mid+1, dhi].
                 * Unsigned-safe midpoint (avoids signed overflow and a
                 * negative half-width when the span exceeds 2^63). */
                int64_t mid = (int64_t)((uint64_t)dlo +
                                        (((uint64_t)dhi - (uint64_t)dlo) >> 1));
                if (!d->tried_lower) {
                    d->tried_lower = 1;
                    pr = ctx_tighten_ub64(ctx, dv, mid);
                } else {
                    d->tried_lower = 0;
                    pr = ctx_tighten_lb64(ctx, dv, mid + 1);
                }
            }

            if (pr == PROP_OK) {
                pr = solver_propagate(ctx);
                /* Save phase if enabled */
                if (pr == PROP_OK && opts && opts->use_phase_save && ctx->phase_save)
                    ctx->phase_save[dv] = (int32_t)val;
            }
            /* If pr == PROP_CONFLICT, loop continues → backtracks further */
        }
    }
}

/* Pin every *inactive* soft assumption (mask bit clear) to its var=[0,0] so a
 * reset + re-solve enforces exactly the current kept-soft set. Active assumptions
 * are left at [1,1] by solver_reset. Factored out of the relaxation loop so the
 * additive re-add refinement below can reuse the identical pinning. */
static void _pin_inactive_assumptions(SolveCtx *ctx) {
    for (uint32_t i = 0; i < ctx->n_assumptions; i++) {
        if (!(ctx->assumption_active_mask & (1ULL << i))) {
            uint32_t av = ctx->assumption_var_ids[i];
            Variable *v = &ctx->vars[av];
            v->lo = 0; v->hi = 0;
            if (av < 64)
                ctx->unassigned_mask &= ~(1ULL << av);
        }
    }
}

/* Additive re-add refinement (soft soundness).
 *
 * The subtractive loop in solver_solve stops at the *first* satisfiable kept-soft
 * set: on each UNSAT it drops the lowest-preference (highest priority *value*)
 * active soft. To reach a genuinely-conflicting higher-preference soft it may walk
 * past — and shed — satisfiable lower-preference softs as collateral, returning a
 * model that needlessly violates them (the intermittent test_soft_nested failure,
 * locked by tests/unit/test_soft.py::test_soft_no_collateral_drop_primary).
 *
 * Recover that collateral: re-add each currently-dropped soft, highest preference
 * (lowest priority value) first, committing any whose re-activation keeps the
 * problem SAT. This makes the primary keep the same maximal priority-respecting
 * set the BV-SAT serve path's additive greedy keeps (primary == serve).
 *
 * It never visits the all-relaxed state (≥1 soft stays active in every trial), so
 * it sidesteps the "pin all assumption vars to 0 → re-solve spuriously UNSAT"
 * engine quirk that blocks a from-scratch additive primary. Monotonic: it only
 * ever re-activates softs, so it cannot worsen the subtractive result. Bounded by
 * the (≤64) assumption count, and only runs when a relaxation actually occurred
 * (soft conflict present), so the common no-conflict hot path is untouched. */
static void _refine_readd_softs(SolveCtx *ctx, const SolveOpts *opts) {
    uint32_t na = ctx->n_assumptions;
    if (na > 64) na = 64;            /* assumption_active_mask is 64-bit */
    uint64_t tried = 0;              /* dropped softs already attempted   */
    for (;;) {
        /* Pick the not-yet-tried, currently-inactive soft with the smallest
         * priority value (= highest preference). */
        int best = -1;
        uint32_t best_pri = 0;
        for (uint32_t i = 0; i < na; i++) {
            uint64_t bit = (1ULL << i);
            if ((ctx->assumption_active_mask & bit) || (tried & bit))
                continue;
            if (best < 0 || ctx->assumption_priorities[i] < best_pri) {
                best_pri = ctx->assumption_priorities[i];
                best = (int)i;
            }
        }
        if (best < 0)
            break;
        tried |= (1ULL << best);

        uint64_t saved = ctx->assumption_active_mask;
        ctx->assumption_active_mask |= (1ULL << best);
        solver_reset(ctx);
        _pin_inactive_assumptions(ctx);
        if (solver_propagate(ctx) == PROP_CONFLICT ||
                _solver_solve_core(ctx, opts) != SOLVE_OK) {
            ctx->assumption_active_mask = saved;   /* keep it dropped */
        }
    }
    /* Materialize the final accepted set: the last trial may have been a revert,
     * leaving stale search state. The final mask was proven SAT, so this resolves. */
    solver_reset(ctx);
    _pin_inactive_assumptions(ctx);
    solver_propagate(ctx);
    _solver_solve_core(ctx, opts);
}

/* ------------------------------------------------------------------ */
/* solver_solve — wrapper with assumption relaxation                   */
/* ------------------------------------------------------------------ */

SolveResult solver_solve(SolveCtx *ctx, const SolveOpts *opts) {
    /* Re-activate all soft assumptions at entry. solver_reset() restores the
     * assumption vars to [1,1] but does NOT touch assumption_active_mask, so on a
     * RE-SOLVE of a reused ctx (the backend's plan-reuse path) the mask would
     * still carry the previous call's relaxations — the conflict then relaxes the
     * remaining kept soft and drops the whole set. Resetting the mask here makes
     * each solve start from the full soft set, matching a fresh compile. The
     * assumption vars are already [1,1] (compile or solver_reset restored them);
     * any var the previous call pinned to [0,0] was a direct live-write that
     * solver_reset has since undone from initial_vars. */
    if (ctx->n_assumptions > 0) {
        uint32_t na = ctx->n_assumptions;
        ctx->assumption_active_mask =
            (na >= 64) ? ~0ULL : ((1ULL << na) - 1);
    }
    /* Subtractive MaxSAT relaxation: drop the lowest-preference (highest priority
     * value) active soft on each UNSAT, re-propagate, retry.
     *
     * On its own this can shed a *satisfiable* lower-preference soft as collateral
     * when walking down to a conflicting higher-preference sibling, returning a
     * model that needlessly violates it (the intermittent test_soft_nested bug). To
     * keep the same maximal priority-respecting set the serve path's additive greedy
     * keeps, the SAT exit below runs _refine_readd_softs() to greedily re-add the
     * dropped softs — recovering the collateral. The refinement re-adds one soft at
     * a time (never the all-relaxed state), so it sidesteps the engine quirk
     * (pinning ALL assumption vars to 0 + re-solve spuriously UNSATs for >1 soft)
     * that blocks a from-scratch additive primary. See
     * dv_solve_soft_constraints_engine_plan.md DSE-3 follow-up and the locks
     * tests/unit/test_soft.py::test_soft_no_collateral_drop_primary +
     * test_soft_maxsat_serve::test_serve_soft_no_collateral_drop.
     *
     * (Residual: a `!=` soft compiles via the generic guard-gated path whose
     * compile-time tightening doesn't fully undo on relaxation, so an all-`!=`
     * conflict can still spurious-UNSAT here; that is safe — the backend escalates
     * such a primary non-OK to the complete BV-SAT engine.) */
    int relaxed_any = 0;
    for (;;) {
        SolveResult res = _solver_solve_core(ctx, opts);
        if (res == SOLVE_OK) {
            /* If we shed any soft to get here, the subtractive walk may have
             * dropped satisfiable softs as collateral; recover them greedily. */
            if (relaxed_any)
                _refine_readd_softs(ctx, opts);
            return res;
        }

        /* Check if we can relax a soft constraint assumption */
        if (ctx->n_assumptions == 0 || ctx->assumption_active_mask == 0)
            return res;

        /* Find the lowest-priority active assumption (highest priority value) */
        uint32_t worst_idx = 0;
        uint32_t worst_pri = 0;
        int found = 0;
        for (uint32_t i = 0; i < ctx->n_assumptions; i++) {
            if (ctx->assumption_active_mask & (1ULL << i)) {
                if (!found || ctx->assumption_priorities[i] >= worst_pri) {
                    worst_pri = ctx->assumption_priorities[i];
                    worst_idx = i;
                    found = 1;
                }
            }
        }
        if (!found) return res;

        /* Relax this assumption */
        ctx->assumption_active_mask &= ~(1ULL << worst_idx);
        relaxed_any = 1;

        /* Reset solver to post-compile state */
        solver_reset(ctx);

        /* Pin all relaxed assumptions to 0 by directly setting bounds.
         * We can't use ctx_tighten because the assumption var starts
         * at [1,1] after reset, and tightening UB to 0 would create
         * an empty domain [1,0] -> conflict. Direct writes are safe
         * here since we're at level 0 before search begins. */
        _pin_inactive_assumptions(ctx);

        /* Propagate the relaxations before retrying */
        if (solver_propagate(ctx) == PROP_CONFLICT) {
            /* Still conflicting — try relaxing more assumptions */
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* solver_get_value                                                    */
/* ------------------------------------------------------------------ */

int64_t solver_get_value(const SolveCtx *ctx, uint32_t var_id) {
    if (!ctx || var_id >= ctx->n_vars) return 0;
    /* Resolve through alias table: aliased vars read from their root */
    uint32_t resolved = var_id;
    if (ctx->var_alias) {
        while (ctx->var_alias[resolved] != resolved)
            resolved = ctx->var_alias[resolved];
    }
    return var_lo64(ctx, &ctx->vars[resolved]);
}


/* ------------------------------------------------------------------ */
/* solver_reset                                                        */
/* ------------------------------------------------------------------ */

void solver_reset(SolveCtx *ctx) {
    if (!ctx || !ctx->initial_vars || ctx->initial_n_vars == 0) return;

    uint32_t n = ctx->initial_n_vars;

    /* Restore variable domains.
     *
     * For tier-0 vars the bounds (lo/hi) live in the Variable struct, so a plain
     * copy restores them. For tier-1 vars (33–64 bit, incl. unsigned-32) the
     * bounds live in a pooled WideBounds64 referenced by `holes_offset`, and
     * compile saved a *separate* pristine copy (initial_vars[i].holes_offset
     * points at that copy). The correct restore is: copy the pristine bounds
     * back into the var's *live* WideBounds64 and keep the var pointing at the
     * live struct.
     *
     * The previous implementation memcpy'd the whole Variable array, which
     * repointed each tier-1 var's holes_offset at the *saved* copy. That made
     * the wide-bounds "restore" a self-copy no-op, and—worse—the next solve then
     * mutated the saved copy, so reset only worked once: tier-1 vars stayed
     * pinned to their first solved value on every subsequent reset. */
    for (uint32_t i = 0; i < n; i++) {
        Variable *v  = &ctx->vars[i];
        Variable *iv = &ctx->initial_vars[i];
        uint32_t live_off = v->holes_offset;
        int tier1 = VAR_IS_TIER1(v->flags) && live_off != 0 && iv->holes_offset != 0;
        *v = *iv;   /* restore lo/hi/flags/width (and holes_offset := saved copy) */
        if (tier1) {
            WideBounds64 *saved = (WideBounds64 *)zsp_pool_ptr(&ctx->pool, iv->holes_offset);
            WideBounds64 *live  = (WideBounds64 *)zsp_pool_ptr(&ctx->pool, live_off);
            *live = *saved;              /* live wide bounds back to pristine */
            v->holes_offset = live_off;  /* keep the var on its live struct */
        }
    }

    /* Reset search state */
    ctx->decision_level = 0;
    ctx->trail_top      = NULL;
    ctx->trail_count    = 0;
    ctx->conflict_count = 0;

    /* Clear propagator queue */
    ctx->queue.non_empty_mask = 0;
    for (int i = 0; i < 16; i++) {
        ctx->queue.heads[i] = EXPR_NULL;
        ctx->queue.tails[i] = EXPR_NULL;
    }

    /* Rebuild unassigned_mask and re-enqueue all propagators */
    ctx->unassigned_mask = 0;
    if (n <= 64) {
        for (uint32_t i = 0; i < n; i++) {
            Variable *v = &ctx->vars[i];
            int64_t lo = var_lo64(ctx, v);
            int64_t hi = var_hi64(ctx, v);
            if (lo != hi) ctx->unassigned_mask |= (1ULL << i);
        }
    }

    /* Re-enqueue all non-entailed propagators. Cap to the side-table
     * capacity since prop_refs above that range is out of bounds. */
    {
    uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                   ? ctx->n_props : ctx->n_prop_refs_capacity;
    for (uint32_t i = 0; i < lim; i++) {
        if (ctx->prop_refs[i] != EXPR_NULL) {
            Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool,
                                                        ctx->prop_refs[i]);
            p->flags &= (uint8_t)~(PROP_FLAG_ENTAILED | PROP_FLAG_IN_QUEUE);
            prop_enqueue(ctx, ctx->prop_refs[i]);
        }
    }
    }
}

/* ------------------------------------------------------------------ */
/* solver_pin_var                                                      */
/* ------------------------------------------------------------------ */

int solver_pin_var(SolveCtx *ctx, uint32_t var_id, int64_t value) {
    if (!ctx || var_id >= ctx->n_vars) return -1;

    PropResult r = ctx_tighten_lb64(ctx, var_id, value);
    if (r == PROP_CONFLICT) return -1;
    r = ctx_tighten_ub64(ctx, var_id, value);
    if (r == PROP_CONFLICT) return -1;

    r = solver_propagate(ctx);
    if (r == PROP_CONFLICT) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* solver_set_seed                                                     */
/* ------------------------------------------------------------------ */

void solver_set_seed(SolveCtx *ctx, uint64_t seed) {
    if (!ctx) return;
    ctx->rng_state = seed ? seed : 1;
}

/* ------------------------------------------------------------------ */
/* solver_get_values                                                   */
/* ------------------------------------------------------------------ */

void solver_get_values(const SolveCtx *ctx, uint32_t n,
                       const uint32_t *var_ids, int64_t *out) {
    if (!ctx || !var_ids || !out) return;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = solver_get_value(ctx, var_ids[i]);
    }
}

/* ------------------------------------------------------------------ */
/* solver_solve_n — batch solve loop (reset+solve+read × N)           */
/* ------------------------------------------------------------------ */

int solver_solve_n(SolveCtx *ctx, uint32_t n_solves,
                   uint32_t n_vars, const uint32_t *var_ids,
                   int64_t *out,
                   uint64_t base_seed,
                   uint32_t max_shave_iters) {
    if (!ctx || !var_ids || !out) return 0;

    SolveOpts opts;
    opts.max_conflicts  = 100;
    opts.max_restarts   = 10000;
    opts.use_phase_save = 0;
    opts.use_lcg = 0;
    opts.fair_pick = 1;   /* batch solve_n is for diverse solutions → uniform */
    opts._pad[0] = 0;
    opts.max_shave_iters = max_shave_iters;

    int n_ok = 0;
    for (uint32_t i = 0; i < n_solves; i++) {
        solver_reset(ctx);
        opts.seed = base_seed + i;
        SolveResult r = _solver_solve_core(ctx, &opts);
        if (r == SOLVE_OK) {
            int64_t *row = out + (uint64_t)n_ok * n_vars;
            for (uint32_t j = 0; j < n_vars; j++) {
                row[j] = solver_get_value(ctx, var_ids[j]);
            }
            n_ok++;
        }
    }
    return n_ok;
}

/* ------------------------------------------------------------------ */
/* solver_soft_active                                                  */
/* ------------------------------------------------------------------ */

int solver_soft_active(const SolveCtx *ctx, uint32_t assumption_idx) {
    if (!ctx || assumption_idx >= ctx->n_assumptions) return -1;
    return (ctx->assumption_active_mask & (1ULL << assumption_idx)) ? 1 : 0;
}


/* ------------------------------------------------------------------ */
/* solver_exclude_value                                                */
/* ------------------------------------------------------------------ */

/* Check if a value is in the hole list for a variable. */
static int _is_hole(const SolveCtx *ctx, uint32_t var_id, int64_t value) {
    if (!ctx->var_holes_head) return 0;
    uint32_t off = ctx->var_holes_head[var_id];
    while (off != 0) {
        const HoleEntry *he = (const HoleEntry *)zsp_pool_ptr(&ctx->pool, off);
        if (he->value == value) return 1;
        if (he->value > value) return 0;  /* sorted ascending, done */
        off = he->next;
    }
    return 0;
}

/* Count holes within [lo, hi]. */
static uint32_t _count_holes_in_range(const SolveCtx *ctx, uint32_t var_id,
                                       int64_t lo, int64_t hi) {
    if (!ctx->var_holes_head) return 0;
    uint32_t count = 0;
    uint32_t off = ctx->var_holes_head[var_id];
    while (off != 0) {
        const HoleEntry *he = (const HoleEntry *)zsp_pool_ptr(&ctx->pool, off);
        if (he->value > hi) break;
        if (he->value >= lo) count++;
        off = he->next;
    }
    return count;
}

/* Insert a value into the sorted hole list for a variable.
 * Returns 0 on success, 1 if already present, -1 on alloc failure. */
static int _insert_hole(SolveCtx *ctx, uint32_t var_id, int64_t value) {
    if (!ctx->var_holes_head) return -1;

    /* Allocate a HoleEntry in the static pool */
    uint32_t he_ref = zsp_pool_alloc(&ctx->pool,
                                      (uint32_t)sizeof(HoleEntry),
                                      (uint32_t)_Alignof(HoleEntry));
    if (he_ref == EXPR_NULL) return -1;

    HoleEntry *he = (HoleEntry *)zsp_pool_ptr(&ctx->pool, he_ref);
    he->value = value;
    he->_hpad = 0;

    /* Insert sorted ascending */
    uint32_t cur_off = ctx->var_holes_head[var_id];
    uint32_t *prev_next_ptr = &ctx->var_holes_head[var_id];

    while (cur_off != 0) {
        HoleEntry *cur = (HoleEntry *)zsp_pool_ptr(&ctx->pool, cur_off);
        if (cur->value == value) return 1;  /* already present */
        if (cur->value > value) break;
        prev_next_ptr = &cur->next;
        cur_off = cur->next;
    }
    he->next = cur_off;
    *prev_next_ptr = he_ref;
    return 0;
}

int solver_exclude_value(SolveCtx *ctx, uint32_t var_id, int64_t value) {
    if (!ctx || var_id >= ctx->n_vars) return -1;
    if (!ctx->var_holes_head) return -1;

    /* Already a hole: no-op */
    if (_is_hole(ctx, var_id, value)) return 0;

    /* Use the initial (pre-solve) domain for capacity checks so that
     * callers can exclude values after solver_solve() (when the current
     * bounds have been narrowed to a singleton). */
    int64_t init_lo, init_hi;
    if (ctx->initial_vars && var_id < ctx->initial_n_vars) {
        init_lo = var_lo64(ctx, &ctx->initial_vars[var_id]);
        init_hi = var_hi64(ctx, &ctx->initial_vars[var_id]);
    } else {
        init_lo = var_lo64(ctx, &ctx->vars[var_id]);
        init_hi = var_hi64(ctx, &ctx->vars[var_id]);
    }

    /* Value outside initial domain: no-op */
    if (value < init_lo || value > init_hi) return 0;

    /* Check initial domain has room after this exclusion */
    uint32_t n_holes = _count_holes_in_range(ctx, var_id, init_lo, init_hi);
    uint64_t domain_size = (uint64_t)(init_hi - init_lo) + 1u;
    if (n_holes + 1 >= domain_size)
        return -1;  /* would leave no valid values in the initial domain */

    /* Always insert into hole list so exclusion persists across resets */
    int ir = _insert_hole(ctx, var_id, value);
    if (ir < 0) return -1;

    /* Optimisation: if the excluded value is at a current boundary,
     * tighten bounds now to help propagation. This tightening gets
     * undone by solver_reset(), but the hole list persists. */
    int64_t lo = var_lo64(ctx, &ctx->vars[var_id]);
    int64_t hi = var_hi64(ctx, &ctx->vars[var_id]);
    if (value >= lo && value <= hi) {
        if (value == lo) {
            int64_t new_lo = lo + 1;
            while (new_lo <= hi && _is_hole(ctx, var_id, new_lo)) new_lo++;
            if (new_lo <= hi)
                ctx_tighten_lb64(ctx, var_id, new_lo);
        } else if (value == hi) {
            int64_t new_hi = hi - 1;
            while (new_hi >= lo && _is_hole(ctx, var_id, new_hi)) new_hi--;
            if (new_hi >= lo)
                ctx_tighten_ub64(ctx, var_id, new_hi);
        }
    }

    return 0;
}
