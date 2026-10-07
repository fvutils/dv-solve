#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "dvs_search.h"
#include "dvs_ctx.h"
#include "dvs_oracle_hooks.h"
#include "dvs_propagator.h"
#include "dvs_trail.h"
#include "dvs_shave.h"
#include "dvs_lcg.h"
#include "dvs_explain.h"

#ifdef DVS_STEP_CHECK
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

/* Soundness step checker (DVS_STEP_CHECK builds only).
 *
 * A learnt clause must hold in every solution; one that does not can make a
 * satisfiable problem `unsat`, but only when it happens to cut the last
 * solution, so the answer alone rarely shows it. Check every clause as it is
 * learnt: in a forked child (an exact copy, nothing to restore), backtrack to
 * the root, switch learning off, assert the clause's negation and search with
 * the plain chronological solver. Finding a solution proves the clause wrong:
 * print it and abort. The check shares no code with clause learning or the
 * explainers -- only the propagators and the search. */
static dvs_result_t _solver_solve_core(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts);

extern dvs_problem_t *dvs_sc_problem;
extern int dvs_sc_uncompiled;
static int      _sc_in_child;
static uint64_t _sc_checked, _sc_unknown, _sc_invalid;

static void _sc_report(void) {
    if (getenv("DV_STEP_CHECK_STATS"))
        fprintf(stderr, "[step-check] learnt clauses checked=%llu unknown=%llu invalid=%llu\n",
                (unsigned long long)_sc_checked, (unsigned long long)_sc_unknown,
                (unsigned long long)_sc_invalid);
}

static void _sc_print_lits(const Literal *lits, uint32_t n, const char *sep) {
    for (uint32_t i = 0; i < n; i++)
        fprintf(stderr, "%s v%u %s %lld", i ? sep : " ", lits[i].var_id,
                lits[i].is_lb ? ">=" : "<=", (long long)lits[i].bound);
    fprintf(stderr, "\n");
}

static void _sc_print_clause(const Literal *lits, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        fprintf(stderr, "%s v%u %s %lld", i ? " \\/" : " ", lits[i].var_id,
                lits[i].is_lb ? ">=" : "<=", (long long)lits[i].bound);
    fprintf(stderr, "\n");
}

/* Suite runs set DV_STEP_CHECK_LOG (append the violation, tagged with the
 * pytest test that hit it) and DV_STEP_CHECK_CONTINUE (an abort inside the
 * library would end the whole pytest process). */
static void _sc_fail(const Literal *lits, uint32_t n) {
    const char *lp = getenv("DV_STEP_CHECK_LOG");
    FILE *lf = lp ? fopen(lp, "a") : NULL;
    if (lf) {
        const char *tn = getenv("PYTEST_CURRENT_TEST");
        fprintf(lf, "INVALID test=%s pid=%d clause:", tn ? tn : "-", (int)getpid());
        for (uint32_t i = 0; i < n; i++)
            fprintf(lf, "%s v%u %s %lld", i ? " \\/" : " ", lits[i].var_id,
                    lits[i].is_lb ? ">=" : "<=", (long long)lits[i].bound);
        fprintf(lf, "\n");
        fclose(lf);
    }
    if (!getenv("DV_STEP_CHECK_CONTINUE")) abort();
    _sc_invalid++;
}

/* Fork; in the child backtrack to the root, switch learning off, assert
 * every literal of `assume` and search with the plain solver. Returns 10 if
 * that finds a solution (printing it), 20 if not, 30 if undecided. */
static int _sc_fork_solve(dvs_ctx_t *ctx, const Literal *assume, uint32_t n,
                          const dvs_solve_opts_t *opts) {
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return 30;
    if (pid == 0) {
        _sc_in_child = 1;
        LCGCtx *lcg = (LCGCtx *)ctx->lcg;
        if (lcg) lcg->enabled = 0;
        trail_backtrack(ctx, ctx->search_base);
        PropResult pr = PROP_OK;
        for (uint32_t i = 0; i < n && pr == PROP_OK; i++)
            pr = assume[i].is_lb ? ctx_tighten_lb64(ctx, assume[i].var_id, assume[i].bound)
                                 : ctx_tighten_ub64(ctx, assume[i].var_id, assume[i].bound);
        if (pr != PROP_OK) _exit(20);
        dvs_solve_opts_t o;
        if (opts) o = *opts; else memset(&o, 0, sizeof o);
        o.use_lcg = 0;
        o.time_limit_ms = 5000;
        dvs_result_t r = _solver_solve_core(ctx, &o);
        if (r == DVS_SOLVE_OK && dvs_sc_problem &&
            dvs_solver_validate_model(ctx, dvs_sc_problem, NULL) > 0) {
            /* Compile left constraints out (the real solve validates and
             * escalates): this is no solution, and no evidence either way. */
            if (dvs_sc_uncompiled) _exit(30);
            /* The propagators accepted an assignment the constraints reject:
             * a wrong-model bug in its own right, and no evidence about the
             * step being checked. */
            fprintf(stderr, "[step-check] INVALID model accepted by the propagators"
                    " (an incomplete propagator):\n");
            dvs_solver_validate_model(ctx, dvs_sc_problem, stderr);
            _exit(40);
        }
        if (r == DVS_SOLVE_OK) {
            fprintf(stderr, "[step-check] solution:");
            for (uint32_t i = 0; i < ctx->n_vars; i++) {
                uint32_t v = i;   /* a var merged by `x == y` reads its root */
                while (ctx->var_alias && ctx->var_alias[v] != v) v = ctx->var_alias[v];
                int64_t lo = var_lo64(ctx, &ctx->vars[v]), hi = var_hi64(ctx, &ctx->vars[v]);
                if (lo == hi) fprintf(stderr, " v%u=%lld", i, (long long)lo);
                else fprintf(stderr, " v%u=%lld..%lld", i, (long long)lo, (long long)hi);
            }
            fprintf(stderr, "\n");
            _exit(10);
        }
        _exit(r == DVS_SOLVE_UNSAT ? 20 : 30);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 30;
}

/* With DV_STEP_CHECK_DUMP, list every live propagator after a violation:
 * name, watched variables, guard. */
static void _sc_dump_props_always(dvs_ctx_t *ctx);
static void _sc_dump_props(dvs_ctx_t *ctx) {
    if (!getenv("DV_STEP_CHECK_DUMP")) return;
    _sc_dump_props_always(ctx);
}

static void _sc_dump_props_always(dvs_ctx_t *ctx) {
    uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                   ? ctx->n_props : ctx->n_prop_refs_capacity;
    for (uint32_t i = 0; i < lim; i++) {
        if (ctx->prop_refs[i] == EXPR_NULL) continue;
        Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool, ctx->prop_refs[i]);
        uint32_t nw;
        const uint32_t *wv = prop_watched_vars(p, &nw);
        fprintf(stderr, "[step-check]   prop %u %s", i, prop_fire_name(p->fire));
        for (uint32_t j = 0; j < nw; j++) fprintf(stderr, " v%u", wv[j]);
        if (ctx->prop_guard_vars && ctx->prop_guard_vars[i] != EXPR_NULL)
            fprintf(stderr, " guard=v%u", ctx->prop_guard_vars[i]);
        fprintf(stderr, "\n");
    }
}

/* At a `sat` exit every propagator must be at a fixed point: fire each once
 * more (in a forked child, so the state is untouched) and report any that
 * still conflicts or narrows. A propagator that never fired after its last
 * input changed -- a stale "entailed" flag, a dropped queue entry -- shows
 * here as a wrong model the search believed. */
static void _step_check_sat_exit(dvs_ctx_t *ctx) {
    if (_sc_in_child) return;
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        _sc_in_child = 1;
        uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                       ? ctx->n_props : ctx->n_prop_refs_capacity;
        int bad = 0;
        for (uint32_t i = 0; i < lim; i++) {
            uint32_t ref = ctx->prop_refs[i];
            if (ref == EXPR_NULL) continue;
            Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool, ref);
            if (ctx->prop_guard_vars && ctx->prop_guard_vars[i] != EXPR_NULL) {
                const Variable *g = &ctx->vars[ctx->prop_guard_vars[i]];
                if (var_lo64(ctx, g) == 0) continue;   /* gated off */
            }
            uint8_t flags = p->flags;
            TrailEntry *mark = ctx->trail_top;
            ctx->current_prop_ref = ref;
            PropResult r = p->fire(p, ctx);
            ctx->current_prop_ref = EXPR_NULL;
            if (r == PROP_CONFLICT || ctx->trail_top != mark) {
                fprintf(stderr, "[step-check] INVALID sat: prop %u %s not at a fixed point"
                        " (%s; flags=0x%x%s)\n", i, prop_fire_name(p->fire),
                        r == PROP_CONFLICT ? "conflict" : "narrows", flags,
                        (flags & PROP_FLAG_ENTAILED) ? " ENTAILED" : "");
                bad = 1;
            }
        }
        _exit(bad ? 10 : 20);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) { }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 10) {
        Literal none[1];
        memset(none, 0, sizeof none);
        _sc_fail(none, 0);
    }
}

static void _step_check_learnt(dvs_ctx_t *ctx, const Literal *lits, uint32_t n,
                               const dvs_solve_opts_t *opts) {
    static int registered;
    if (_sc_in_child) return;
    if (!registered) { registered = 1; atexit(_sc_report); }
    /* A conflict clause is the negation of bounds that all hold at the
     * conflict, so every literal must be false now. One that already holds
     * excludes nothing: the search re-derives the same conflict for ever. */
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[lits[i].var_id];
        int is_false = lits[i].is_lb
            ? var_b_lt(v, var_hi64(ctx, v), lits[i].bound)
            : var_b_gt(v, var_lo64(ctx, v), lits[i].bound);
        if (!is_false) {
            fprintf(stderr, "[step-check] INVALID learnt clause (literal %u not false"
                    " at the conflict):", i);
            _sc_print_clause(lits, n);
            _sc_fail(lits, n);
            return;
        }
    }
    /* Valid iff no solution satisfies the clause's negation. */
    Literal neg[MAX_CLAUSE_LITS];
    for (uint32_t i = 0; i < n; i++) neg[i] = literal_negate(lits[i]);
    int code = _sc_fork_solve(ctx, neg, n, opts);
    if (code == 40) { _sc_fail(lits, n); return; }
    if (code == 10) {
        fprintf(stderr, "[step-check] INVALID learnt clause:");
        _sc_print_clause(lits, n);
        _sc_fail(lits, n);
        return;
    }
    if (code == 20) _sc_checked++; else _sc_unknown++;
}

/* Called by conflict analysis (dvs_lcg.c) for every explanation it uses:
 * `ante` (the explainer's literals plus any own-bound and guard literals the
 * analysis added) must imply `lit` in every solution. */
void dvs_step_check_explanation(dvs_ctx_t *ctx, const char *who,
                                const Literal *ante, uint32_t n, Literal lit) {
    static uint64_t checked, unknown;
    if (_sc_in_child || getenv("DV_STEP_CHECK_NO_EXPLAIN")) return;
    Literal as[MAX_CLAUSE_LITS + 1];
    if (n > MAX_CLAUSE_LITS) { unknown++; return; }
    memcpy(as, ante, n * sizeof(Literal));
    as[n] = literal_negate(lit);
    int code = _sc_fork_solve(ctx, as, n + 1, NULL);
    if (code == 40) { Literal l1[1] = { lit }; _sc_fail(l1, 1); return; }
    if (code == 10) {
        fprintf(stderr, "[step-check] INVALID explanation from %s: v%u %s %lld because",
                who, lit.var_id, lit.is_lb ? ">=" : "<=", (long long)lit.bound);
        _sc_print_lits(ante, n, " /\\");
        _sc_dump_props(ctx);
        Literal l1[1] = { lit };
        _sc_fail(l1, 1);
        return;
    }
    if (code == 20) checked++; else unknown++;
    (void)checked; (void)unknown;
}
#endif

/* Forward declarations for hole management */
static int _is_hole(const dvs_ctx_t *ctx, uint32_t var_id, int64_t value);
static uint32_t _count_holes_in_range(const dvs_ctx_t *ctx, uint32_t var_id,
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

static uint64_t _rand64(dvs_ctx_t *ctx) {
    uint64_t x = ctx->rng_state;
    if (x == 0) x = 0xDEADBEEF12345678ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    ctx->rng_state = x;
    return x;
}

/* Return a random integer in [lo, hi] (inclusive), 64-bit range. */
static int64_t _rand_range64(dvs_ctx_t *ctx, int64_t lo, int64_t hi) {
    /* `hi - lo` is correct modulo 2^64 for both signed and unsigned bound
     * patterns -- computed in uint64, where wrapping is defined. In int64 it
     * is signed overflow for any span above INT64_MAX (undefined behaviour,
     * which -O2 exploits). When the domain is the full 64-bit range, span
     * wraps to 0 (2^64 mod 2^64): pick any 64-bit value (and avoid a
     * div-by-zero). */
    uint64_t span = (uint64_t)hi - (uint64_t)lo + 1u;
    if (span == 0u) return (int64_t)_rand64(ctx);
    return (int64_t)((uint64_t)lo + _rand64(ctx) % span);
}

/* ------------------------------------------------------------------ */
/* Variable selection — MRV (minimum remaining values)               */
/*                                                                     */
/* Returns the var_id of the unassigned variable with the smallest    */
/* domain, or EXPR_NULL if all variables are assigned.                */
/* ------------------------------------------------------------------ */

/* A variable's domain size for MRV, less one: its bounds' span, or fewer
 * when holes leave it fewer values. `x in [512, 4096]` has bounds 512..4096
 * but two values; measured by its bounds it is decided after every variable
 * with fewer than 3585 values, and by then those decisions have mostly
 * decided it (one in four values of `nlb in [1..256]` leaves 4096 legal under
 * `nlb * x <= 0x40000`, so 4096 came up 13% of the time, not half). */
static inline uint64_t _dom_size(const dvs_ctx_t *ctx, uint32_t i,
                                 int64_t lo, int64_t hi) {
    uint64_t dom = (uint64_t)hi - (uint64_t)lo;
    if (ctx->var_n_values && ctx->var_n_values[i] != 0
            && ctx->var_n_values[i] - 1u < dom)
        dom = ctx->var_n_values[i] - 1u;
    return dom;
}

static uint32_t _select_unassigned(dvs_ctx_t *ctx) {
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
    /* Domain sizes are hi - lo as uint64: in int64 a span above INT64_MAX
     * overflows (undefined) and in practice wraps negative, which made the
     * WIDEST variable look like the narrowest. A full 64-bit domain has size
     * UINT64_MAX, equal to the initial best, so the first candidate is taken
     * by `best == EXPR_NULL` rather than by comparison. */
    uint64_t best_dom     = UINT64_MAX;
    uint32_t n_best       = 0;       /* # real vars tied at best_dom (reservoir) */
    uint32_t best_aux     = EXPR_NULL;
    uint64_t best_aux_dom = UINT64_MAX;

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
            uint64_t dom = _dom_size(ctx, i, lo, hi);
            if (v->flags & VAR_AUX) {
                /* Aux is a low-priority decision: prefer real vars first. */
                if (best_aux == EXPR_NULL || dom < best_aux_dom) { best_aux_dom = dom; best_aux = i; }
                continue;
            }
            if (best == EXPR_NULL || dom < best_dom) {
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
            uint64_t dom = _dom_size(ctx, i, lo, hi);
            if (v->flags & VAR_AUX) {
                if (best_aux == EXPR_NULL || dom < best_aux_dom) { best_aux_dom = dom; best_aux = i; }
                continue;
            }
            if (best == EXPR_NULL || dom < best_dom) {
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
static int64_t _pick_value_dist(dvs_ctx_t *ctx, uint32_t var_id,
                                 int64_t lo, int64_t hi) {
    uint32_t dm_off = ctx->dist_offsets[var_id];
    if (dm_off == 0) return _rand_range64(ctx, lo, hi);

    DistMeta *dm = (DistMeta *)dvs_pool_ptr(&ctx->pool, dm_off);
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
static int _has_holes(const dvs_ctx_t *ctx, uint32_t var_id) {
    return ctx->var_holes_head &&
           var_id < ctx->n_vars_capacity &&
           ctx->var_holes_head[var_id] != 0;
}

/* Pick a value avoiding holes via rejection sampling, with fallback
 * to enumeration for domains with many holes. */
static int64_t _pick_avoiding_holes(dvs_ctx_t *ctx, uint32_t var_id,
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
    uint64_t n_total = (uint64_t)hi - (uint64_t)lo + 1;
    uint32_t n_holes = _count_holes_in_range(ctx, var_id, lo, hi);
    if ((uint64_t)n_holes >= n_total) return lo;  /* all holes (shouldn't happen) */
    uint64_t k = _rand64(ctx) % (n_total - (uint64_t)n_holes);  /* k-th valid */
    int64_t cur = lo;
    uint32_t hoff = ctx->var_holes_head[var_id];
    while (hoff != 0) {
        const HoleEntry *he = (const HoleEntry *)dvs_pool_ptr(&ctx->pool, hoff);
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

/* A drawn value of `a` moved to a remainder that `r == a % b` still allows
 * (DvsModLink). `x % 8 == 0` under a decided guard fixes r to 0, and the
 * propagator then aligns only a's bounds: a uniform draw between them meets
 * it one time in 8, and the other 7 are conflicts. Conflicts are rejections
 * of the decisions made before them, so the branch that set the guard was
 * drawn less (the NVMe bench's 4096-byte sectors, with
 * `(lba_bytes == 4096) -> (slba % 8 == 0)`, came up 5% of the time, not
 * half). The move keeps the draw's quotient and takes a remainder at random
 * from r's domain, so the values that remain are drawn uniformly. While r
 * still spans [0, b-1] -- the guard undecided -- there is nothing to move. */
static int64_t _pick_mod_aligned(dvs_ctx_t *ctx, uint32_t var_id, int64_t v,
                                 int64_t lo, int64_t hi) {
    if (!ctx->n_mod_links || (ctx->vars[var_id].flags & VAR_SIGNED)) return v;
    uint64_t ulo = (uint64_t)lo, uhi = (uint64_t)hi;
    for (uint32_t i = 0; i < ctx->n_mod_links; i++) {
        const DvsModLink *ml = &ctx->mod_links[i];
        uint32_t a = ml->a, r = ml->r;
        if (ctx->var_alias) {
            while (ctx->var_alias[a] != a) a = ctx->var_alias[a];
            while (ctx->var_alias[r] != r) r = ctx->var_alias[r];
        }
        if (a != var_id) continue;
        uint64_t b = (uint64_t)ml->b;
        uint64_t rlo = (uint64_t)var_lo64(ctx, &ctx->vars[r]);
        uint64_t rhi = (uint64_t)var_hi64(ctx, &ctx->vars[r]);
        if (rhi > b - 1) rhi = b - 1;
        if (rlo > rhi || (rlo == 0 && rhi == b - 1)) continue;
        uint64_t uv = (uint64_t)v, rem = uv % b;
        if (rem >= rlo && rem <= rhi) continue;
        uint64_t t = rlo + _rand64(ctx) % (rhi - rlo + 1);
        uint64_t c = uv - rem + t;
        if (c > uhi || c < uv - rem) c -= b;     /* past hi, or wrapped */
        if (c < ulo) c += b;
        if (c >= ulo && c <= uhi) v = (int64_t)c;
    }
    return v;
}

enum { KEEP_NONE = 0, KEEP_SET = 1, KEEP_DEAD = 2 };

static int _keep_draws_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DVS_KEEP_DRAWS");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached;
}

static int64_t _pick_value(dvs_ctx_t *ctx, uint32_t var_id,
                            const dvs_solve_opts_t *opts) {
    int64_t lo = var_lo64(ctx, &ctx->vars[var_id]);
    int64_t hi = var_hi64(ctx, &ctx->vars[var_id]);

    /* Phase saving: try the last saved value if still in domain. Sign-aware
     * so an unsigned upper-half saved value is range-checked correctly (a
     * signed compare could return an out-of-domain value).
     *
     * Deliberately DISABLED for a diversity solve (seed != 0) -- see B18. This
     * shortcut range-checks only against the coarse [lo,hi] bounds and a single
     * _is_hole() probe; unlike the randomized path below it never runs the
     * saved value through _pick_avoiding_holes(). On a domain that is a union
     * of disjoint ranges (`x inside {[10:20],[100:150]}`) or a bit-count
     * constraint, a value in the gap passes the bounds check and gets returned
     * as an infeasible decision -- and because the saved phase does not change,
     * every restart re-picks it, burning the whole wall-clock budget (measured:
     * 5 s -> unknown, versus 3 ms with this path skipped). Diversity does NOT
     * depend on this: the _rand_range64 path below is seeded from
     * ctx->rng_state and yields fully distinct models per seed (verified
     * 20/20). Seed 0 (BMC/decision) keeps phase saving exactly as before. */
    if (opts && opts->use_phase_save && opts->seed == 0 && ctx->phase_save) {
        const Variable *v = &ctx->vars[var_id];
        int64_t ps = ctx->phase_save[var_id];
        if (!var_b_lt(v, ps, lo) && !var_b_gt(v, ps, hi)
                && !_is_hole(ctx, var_id, ps))
            return ps;
    }

    /* A seeded solve keeps each var's first draw: when the search decides
     * the var again -- after a backjump or a restart undid the decision for
     * a conflict elsewhere -- it takes the same value if that is still in
     * the domain. Drawing afresh instead makes every conflict a rejection of
     * every decision made before it, so a branch of the problem that
     * conflicts more (a wider op's longer chain of dependent fields) is drawn
     * less, and the distribution follows the search rather than the
     * constraints. A kept value whose own decision conflicts is dropped for
     * the rest of the solve (KEEP_DEAD), and keeping stops at the first
     * restart, so an infeasible kept value -- one in a gap of a union domain
     * that only a propagator excludes (B18) -- is retried at most once. */
    int keep = ctx->keep_on && var_id < ctx->keep_cap;
    if (keep && ctx->keep_state[var_id] == KEEP_SET) {
        const Variable *v = &ctx->vars[var_id];
        int64_t kv = ctx->keep_val[var_id];
        if (!var_b_lt(v, kv, lo) && !var_b_gt(v, kv, hi)
                && !_is_hole(ctx, var_id, kv))
            return kv;
    }

    int64_t v;
    /* Distribution-weighted selection if this variable has a dist constraint */
    if (ctx->dist_offsets && var_id < ctx->n_vars_capacity &&
        ctx->dist_offsets[var_id] != 0) {
        v = _pick_value_dist(ctx, var_id, lo, hi);
    } else {
        v = _rand_range64(ctx, lo, hi);
        v = _pick_mod_aligned(ctx, var_id, v, lo, hi);
    }
    v = _pick_avoiding_holes(ctx, var_id, v, lo, hi);
    if (keep && ctx->keep_state[var_id] == KEEP_NONE) {
        ctx->keep_state[var_id] = KEEP_SET;
        ctx->keep_val[var_id]   = v;
    }
    return v;
}

/* Which half of a conflict split to explore first.
 *
 * When a plain value decision conflicts and the value is interior, the search
 * opens a reversible two-way split around it. Always taking the lower half
 * first biases a *diversity* solve hard toward the domain minimum whenever the
 * feasible set is sparse and is enforced by a **propagator** rather than by
 * holes: each conflicting random pick re-restricts the domain to [dlo, val-1],
 * so the walk descends monotonically and lands on the smallest feasible value
 * nearly every time. Measured before this fix: `addr` confined to a 4 KB window
 * with `addr % 64 == 0` (64 legal values) returned the low end ~93% of draws,
 * and only 57/64 distinct values appeared in 5000 draws.
 *
 * (The sibling bias through _pick_avoiding_holes -- domains expressed as holes,
 * e.g. `inside {0,255}` -- was fixed separately; this is the propagator-enforced
 * counterpart, which never reaches the hole list at all.)
 *
 * Choosing the first half at random, weighted by the two halves' sizes, keeps
 * the descent unbiased. Seed 0 (BMC/decision) keeps the old deterministic
 * lower-first order so those solves stay bit-for-bit reproducible. */
static int _split_random_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DVS_SPLIT_RANDOM");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached;
}

static int _split_upper_first(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                               int64_t dlo, int64_t val, int64_t dhi) {
    if (!(opts && opts->seed != 0)) return 0;      /* deterministic mode */
    if (!_split_random_enabled()) return 0;
    /* Sizes as unsigned deltas: dlo < val < dhi holds in the variable's own
     * ordering, so for an unsigned var living in the upper half the bit
     * patterns still subtract correctly (and a signed subtraction could
     * overflow). */
    uint64_t n_lo = (uint64_t)val - (uint64_t)dlo;   /* |[dlo, val-1]| */
    uint64_t n_hi = (uint64_t)dhi - (uint64_t)val;   /* |[val+1, dhi]| */
    uint64_t tot  = n_lo + n_hi;
    if (tot == 0) return 0;
    return (_rand64(ctx) % tot) >= n_lo;
}

/* Should the decision on `x` (value `v` picked) be a bound decision?
 *
 * With clause learning, deciding a wide variable to a value teaches, on
 * failure, only `x != v` -- and when the reason cannot be put as bounds (a
 * product that must divide exactly, say), the search learns the domain one
 * value at a time. A bound decision, `x <= v` or `x >= v`, fails with a clause
 * about a range instead. So a variable that has taken part in conflicts (its
 * activity is non-zero) and still has more than DVS_BOUND_DECISION_MIN values
 * is narrowed by bounds; every other decision stays a value decision, which
 * keeps the decision depth small. The side is chosen as a split's is (random,
 * size-weighted, in diversity mode), and always narrows the domain.
 * Returns 0 (value decision), 1 (x <= v) or 2 (x >= v). */
#define DVS_BOUND_DECISION_MIN 64u
static uint8_t _bound_decision(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                               uint32_t x, int64_t v) {
    const LCGCtx *L = (const LCGCtx *)ctx->lcg;
    if (!L || !L->enabled || x >= L->vsids.n_vars || !(L->vsids.activity[x] > 0.0))
        return 0;
    const Variable *xv = &ctx->vars[x];
    int64_t lo = var_lo64(ctx, xv), hi = var_hi64(ctx, xv);
    if ((uint64_t)hi - (uint64_t)lo <= DVS_BOUND_DECISION_MIN) return 0;
    if (v == lo) return 1;              /* x <= lo: the side that narrows */
    if (v == hi) return 2;
    return _split_upper_first(ctx, opts, lo, v, hi) ? 2 : 1;
}

/* ------------------------------------------------------------------ */
/* dvs_solver_solve                                                        */
/* ------------------------------------------------------------------ */

/* Monotonic wall-clock seconds — for the CDCL time budget (B10). The
 * conflict/restart counters bound *work* but not *time*: a hard problem can
 * churn through a Luby-growing conflict budget for many minutes, which reads as
 * a hang. A wall-clock deadline lets the search bail to DVS_SOLVE_TIMEOUT so the
 * caller degrades to `unknown` (and, outside DV_NO_BITBLAST, escalates to
 * bitblast). CDCL must be correct-or-unknown in bounded time — never hang. */
static double _now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* SplitMix64 finalizer: avalanche a low-entropy integer into a well-mixed
 * 64-bit state. Needed because callers hand us *sequential* seeds -- Verilator
 * mode literally does `seed = ++div_counter`, and any sampling harness does
 * seeds 1..N -- and xorshift64 started from 1, 2, 3, ... produces strongly
 * correlated low bits across those streams. The visible symptom was successive
 * randomize() calls walking an arithmetic progression rather than sampling:
 * `a + b == 100` handed back a = 65, 130, 195, 4, 69, ... (i.e. +65 mod 256)
 * instead of 256 independent draws. Scrambling here fixes every consumer of
 * ctx->rng_state at once. */
static uint64_t _seed_mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

const char *dvs_solver_bail_reason_str(const dvs_ctx_t *ctx) {
    if (!ctx) return "?";
    switch (ctx->bail_reason) {
    case DVS_BAIL_DEADLINE:      return "wall-clock deadline (search)";
    case DVS_BAIL_MAX_DEPTH:     return "decision depth limit";
    case DVS_BAIL_DEADLINE_CONF: return "wall-clock deadline (conflict analysis)";
    case DVS_BAIL_MAX_RESTARTS:  return "restart budget exhausted";
    case DVS_BAIL_PROPAGATION:   return "propagation ran past the deadline or trail cap";
    default:                     return "no reason recorded";
    }
}

static dvs_result_t _solver_solve_core(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    ctx->bail_reason = DVS_BAIL_NONE;
    /* Seed or preserve RNG. Sequential seeds must be avalanched first. */
    if (opts && opts->seed != 0) ctx->rng_state = _seed_mix64(opts->seed);
    if (ctx->rng_state == 0)     ctx->rng_state = 0xDEADBEEF12345678ULL;

    /* Wall-clock budget (B10). Default 10s; DV_CDCL_TIME_LIMIT overrides
     * (<=0 disables). deadline==0.0 means unlimited. Checked cheaply (every
     * ~1024 iters) in both the decision loop and the conflict loop. */
    double _deadline = 0.0;
    {
        double tl = 10.0;
        const char *e = getenv("DV_CDCL_TIME_LIMIT");
        if (e && *e) tl = atof(e);
        /* A per-solve budget wins over the env default: used for the Verilator
         * CDCL-viability probe, which must be bounded without touching env. */
        if (opts && opts->time_limit_ms > 0) tl = (double)opts->time_limit_ms / 1000.0;
        if (tl > 0.0) _deadline = _now_sec() + tl;
    }
    /* Propagation checks the same deadline (and a trail cap) itself: a single
     * propagation can climb for 2^w rounds without reaching the checks below.
     * See prop_aborted in dvs_ctx.h. */
    ctx->prop_aborted  = 0;
    ctx->prop_deadline = _deadline;
#define ABORTED() (ctx->prop_aborted \
                   ? (ctx->bail_reason = DVS_BAIL_PROPAGATION, 1) : 0)
    uint64_t _tick = 0;   /* cheap gate for the clock_gettime checks */

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
    /* `use_lcg` is per solve: an LCG created by an earlier solve learns only
     * in solves that ask for it. (It used to stay on once created, so a
     * caller could not run a plain chronological solve on the context.) */
    if (ctx->lcg)
        ((LCGCtx *)ctx->lcg)->enabled = (opts && opts->use_lcg) ? 1 : 0;

    /* Register explain callbacks every solve — propagators added by
     * compile-time aux materialisation or incremental paths after the
     * first solve would otherwise miss their explain wiring. The walk
     * skips propagators whose explain is already set. */
    if (ctx->lcg) {
        contra_register_explanations(ctx);
    }

    /* Allocate phase_save array on first call (lazily from static pool). */
    int ps_fresh = 0;
    if (opts && opts->use_phase_save && !ctx->phase_save && ctx->n_vars > 0) {
        /* Size to n_vars_capacity for incremental variable support */
        uint32_t ps_size = ctx->n_vars_capacity > 0
                           ? ctx->n_vars_capacity : ctx->n_vars;
        uint32_t ps_ref = dvs_pool_alloc(&ctx->pool,
                                          ps_size * (uint32_t)sizeof(int64_t),
                                          (uint32_t)_Alignof(int64_t));
        if (ps_ref != EXPR_NULL) {
            ctx->phase_save = (int64_t *)dvs_pool_ptr(&ctx->pool, ps_ref);
            ps_fresh = 1;
        }
    }

    /* Initialize the saved phases to each var's lower bound, once, on the solve
     * that allocated the array. Only the seed-0 (BMC/decision) path consults
     * them -- a diversity solve (seed != 0) bypasses phase saving entirely so
     * its value choice goes through the hole-aware randomized path. See B18/B19
     * in docs/solver_bug_backlog.md for why randomizing these was wrong. */
    if (ps_fresh && ctx->phase_save && ctx->n_vars > 0) {
        /* Never write past the allocation: n_vars can grow past the capacity
         * the array was sized to via the incremental paths. */
        uint32_t ps_lim = ctx->n_vars_capacity > 0
                          ? ctx->n_vars_capacity : ctx->n_vars;
        if (ps_lim > ctx->n_vars) ps_lim = ctx->n_vars;
        for (uint32_t i = 0; i < ps_lim; i++)
            ctx->phase_save[i] = var_lo64(ctx, &ctx->vars[i]);
    }

    /* Kept draws (see _pick_value) are per solve: a seeded solve starts
     * with none. DVS_KEEP_DRAWS=0 turns keeping off. */
    ctx->keep_on = 0;
    if (opts && opts->seed != 0 && _keep_draws_enabled() && ctx->n_vars > 0) {
        if (ctx->keep_cap < ctx->n_vars) {
            int64_t *kv = (int64_t *)realloc(ctx->keep_val,
                                             ctx->n_vars * sizeof(int64_t));
            if (kv) ctx->keep_val = kv;
            uint8_t *ks = (uint8_t *)realloc(ctx->keep_state, ctx->n_vars);
            if (ks) ctx->keep_state = ks;
            if (kv && ks) ctx->keep_cap = ctx->n_vars;
        }
        if (ctx->keep_cap >= ctx->n_vars) {
            memset(ctx->keep_state, KEEP_NONE, ctx->n_vars);
            ctx->keep_on = 1;
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
    if (dvs_solver_propagate(ctx) == PROP_CONFLICT)
        return ABORTED() ? DVS_SOLVE_TIMEOUT : DVS_SOLVE_UNSAT;

    /* Check for domains that became empty before search (e.g. from
     * conflicting bounds imposed externally before dvs_solver_solve).
     * Sign-aware: an unsigned domain straddling 2^63 (lo < 2^63 <= hi)
     * has lo "positive" and hi "negative" as int64, so a bare `lo > hi`
     * would wrongly call it empty. */
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *vi = &ctx->vars[i];
        if (var_b_gt(vi, var_lo64(ctx, vi), var_hi64(ctx, vi)))
            return DVS_SOLVE_UNSAT;
    }

    /* Seal compile-time + initial-propagation state as the search's root.
     * The root is the level the solve starts at, not 0: each open checkpoint
     * holds a level, so under checkpoints (and the pins made in them) the
     * solve starts above 0, and levels below it belong to the checkpoints.
     * Restarts, shave probes and backjumps return to this level and never
     * below it; a search that went below it undid the caller's pins, and
     * then backtracked to marks that no longer lay on the trail.
     * Sealing the mark here (level_marks[0] starts zeroed, trail_top=NULL)
     * also keeps compile-time narrowings, such as (assert cond) forcing cond
     * to [1,1], out of reach of a restart. */
    const uint32_t base = ctx->decision_level;
    ctx->search_base = base;
    ctx->level_marks[base].trail_top   = ctx->trail_top;
    ctx->level_marks[base].trail_count = ctx->trail_count;
    ctx->level_marks[base].stack_mark  = dvs_stack_push(ctx->dynamic);

    /* Pre-search bounds shaving (L2): tighten domains beyond what
     * individual propagator fixed-point can achieve. */
    uint32_t max_si = opts ? opts->max_shave_iters : 1000;
    if (max_si > 0) {
        PropResult sr = bounds_shave(ctx, max_si);
        if (ABORTED()) return DVS_SOLVE_TIMEOUT;   /* a probe's verdict is void */
        if (sr == PROP_CONFLICT) return DVS_SOLVE_UNSAT;
    }

    for (;;) {
        /* Wall-clock budget check (decision loop). */
        if (_deadline > 0.0 && (++_tick & 0x3FF) == 0 && _now_sec() > _deadline) {
            ctx->bail_reason = DVS_BAIL_DEADLINE;
            return DVS_SOLVE_TIMEOUT;
        }

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
                assert(_lo == _hi && "DVS_SOLVE_OK but non-singleton domain");
            }
#endif
#ifdef DVS_STEP_CHECK
            _step_check_sat_exit(ctx);
            if (getenv("DV_STEP_CHECK_DUMP_SAT")) {
                for (uint32_t i = 0; i < ctx->n_vars; i++)
                    fprintf(stderr, " v%u=[%lld,%lld]", i,
                            (long long)var_lo64(ctx, &ctx->vars[i]),
                            (long long)var_hi64(ctx, &ctx->vars[i]));
                fprintf(stderr, "\n");
                _sc_dump_props_always(ctx);
            }
#endif
            return DVS_SOLVE_OK;   /* all assigned */
        }
        int64_t v = _pick_value(ctx, x_id, opts);

        /* Decision-depth guard. decisions/level_marks are fixed-size
         * (MAX_DECISION_DEPTH); the search pushes one level per decided
         * variable, so a problem with more decision variables than that depth
         * would overflow both arrays (out-of-bounds heap write -> crash).
         * Bail with TIMEOUT so the caller defers/escalates cleanly instead. */
        if (ctx->decision_level >= ctx->max_depth) {
            ctx->bail_reason = DVS_BAIL_MAX_DEPTH;
            return DVS_SOLVE_TIMEOUT;
        }

        /* ── Record decision ── */
        uint8_t bound = _bound_decision(ctx, opts, x_id, v);
        uint32_t dec_idx = ctx->decision_level;   /* index before push */
        ctx->decisions[dec_idx].var_id      = x_id;
        ctx->decisions[dec_idx].tried_value  = v;
        ctx->decisions[dec_idx].is_split     = 0;  /* plain value decision */
        ctx->decisions[dec_idx].upper_first  = 0;
        ctx->decisions[dec_idx].second_phase = 0;
        ctx->decisions[dec_idx].bound        = bound;

        /* ── Push level and assign ──
         * The tighten helpers auto-detect singleton pinning (lo == hi
         * after the change) and stamp TRAIL_FLAG_SINGLETON on the new
         * entry plus the companion-bound entry at the same level. */
        trail_push_level(ctx);
        /* A conflict from the search's own tightenings (here, and the
         * chronological path below) has no culprit propagator or clause.
         * Clear the last ones so conflict analysis cannot blame them: it
         * would seed from bounds that do not conflict and learn a clause
         * that removes solutions (wrong `unsat`). */
        ctx->conflict_prop_ref   = EXPR_NULL;
        ctx->conflict_clause_idx = EXPR_NULL;
        PropResult pr;
        if (bound == 1)      pr = ctx_tighten_ub64(ctx, x_id, v);
        else if (bound == 2) pr = ctx_tighten_lb64(ctx, x_id, v);
        else {
            pr = ctx_tighten_lb64(ctx, x_id, v);
            if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x_id, v);
        }
        if (pr == PROP_OK) {
            pr = dvs_solver_propagate(ctx);
        }

        /* The decision just made conflicted: its value is not kept. */
        if (pr == PROP_CONFLICT && ctx->keep_on && x_id < ctx->keep_cap)
            ctx->keep_state[x_id] = KEEP_DEAD;

        /* ── Conflict loop ── */
        while (pr == PROP_CONFLICT) {
            /* An aborted propagation proves nothing: no unsat, no learning. */
            if (ABORTED()) return DVS_SOLVE_TIMEOUT;
            ctx->conflict_count++;
            local_conflicts++;

            /* Wall-clock budget check (conflict loop). This loop learns a
             * clause and `continue`s back to itself; a non-progressing
             * learn/propagate cycle would otherwise never reach the outer
             * loop's check, so the deadline must be tested here too.
             * Every 16th conflict, not every 1024th: one conflict can carry
             * a long bounds-propagation climb (one value per round across a
             * 16-bit domain), and at 1024 conflicts per check the 10 s
             * budget overshot to ~40 s. A clock read per 16 conflict
             * analyses is noise. */
            if (_deadline > 0.0 && (++_tick & 0xF) == 0 && _now_sec() > _deadline) {
                ctx->bail_reason = DVS_BAIL_DEADLINE_CONF;
                return DVS_SOLVE_TIMEOUT;
            }

            uint32_t cur = ctx->decision_level;

            /* A conflict at the root means the problem is UNSAT: every
             * tightening at or below the root is the problem's or the
             * caller's, so if their propagation conflicts there is no
             * search path that can satisfy it. */
            if (cur <= base) return DVS_SOLVE_UNSAT;

            /* Restart check */
            if (max_conflicts > 0 && local_conflicts >= luby_limit) {
                trail_backtrack(ctx, base);
                local_conflicts = 0;
                restart_count++;
                ctx->keep_on = 0;   /* see _pick_value */
                luby_idx++;
                luby_limit = _luby(luby_idx) * max_conflicts;

                if (max_restarts > 0 && restart_count >= max_restarts) {
                    ctx->bail_reason = DVS_BAIL_MAX_RESTARTS;
                    return DVS_SOLVE_TIMEOUT;
                }

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


                pr = dvs_solver_propagate(ctx);
                if (pr == PROP_CONFLICT)
                    return ABORTED() ? DVS_SOLVE_TIMEOUT : DVS_SOLVE_UNSAT;
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
#ifdef DVS_STEP_CHECK
                        _step_check_learnt(ctx, learnt_buf, n_lits, opts);
#endif
                        /* Backjump first so trail state matches the
                         * level the asserting literal will fire at. */
                        if (bt_level >= cur) bt_level = cur - 1;
                        /* A clause whose other literals all lie at or below
                         * the root (pins, a checkpoint's bounds) asserts at
                         * the root: it keeps them as conditions, so it stays
                         * true after a restore, but the search does not undo
                         * them. */
                        if (bt_level < base) bt_level = base;
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
                        if (pr != PROP_CONFLICT) pr = dvs_solver_propagate(ctx);
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
            ctx->conflict_prop_ref   = EXPR_NULL;   /* see the decision push */
            ctx->conflict_clause_idx = EXPR_NULL;

            /* Resolve the conflict. Sign-aware ordering so an unsigned domain
             * in/across the upper half is handled correctly (a signed
             * compare/midpoint would mis-order it).
             *
             * A plain value decision that conflicts is excluded. If `val` is at
             * a domain boundary the exclusion is a single bound tightening at
             * this level. If `val` is interior we cannot exclude one value with
             * bounds alone, so we open a *reversible two-way split* that
             * excludes `val`: phase 1 = [dlo, val-1], and — reached by
             * backtracking to this same level — phase 2 = [val+1, dhi]. The
             * split lives at its own decision level (via trail_push_level) so
             * both halves are always explored. (The previous two-phase code
             * kept its phase flag on the decision record but tightened at the
             * *parent* level and returned to the decision loop, which reset the
             * flag on the next decision — so phase 2 never ran and satisfiable
             * problems whose only solutions sat in a variable's upper half were
             * reported UNSAT. See BUG-3.) */
            const Variable *dvv = &ctx->vars[dv];
            int64_t dlo = var_lo64(ctx, dvv);
            int64_t dhi = var_hi64(ctx, dvv);
            if (d->bound) {
                /* A bound decision that failed: its negation holds. (Reached
                 * only when clause learning could not analyse the conflict.) */
                uint8_t b = d->bound;
                d->bound = 0;
                pr = (b == 1) ? ctx_tighten_lb64(ctx, dv, val + 1)
                              : ctx_tighten_ub64(ctx, dv, val - 1);
            } else if (d->is_split && !d->second_phase) {
                /* First half exhausted -> explore the other one. `val` itself
                 * was excluded by the first half's tightening already. */
                d->second_phase = 1;
                trail_push_level(ctx);
                pr = d->upper_first ? ctx_tighten_ub64(ctx, dv, val - 1)
                                    : ctx_tighten_lb64(ctx, dv, val + 1);
            } else if (d->is_split) {
                /* Both halves of the split exhausted -> this subtree is UNSAT;
                 * fail up to the parent decision. */
                d->is_split = 0;
                pr = PROP_CONFLICT;
            } else if (var_b_gt(dvv, dlo, dhi)) {
                /* Domain already empty after backtrack — propagate conflict up */
                pr = PROP_CONFLICT;
            } else if (!var_b_gt(dvv, val, dlo)) {  /* val <= dlo */
                pr = ctx_tighten_lb64(ctx, dv, dlo + 1);
            } else if (!var_b_lt(dvv, val, dhi)) {  /* val >= dhi */
                pr = ctx_tighten_ub64(ctx, dv, dhi - 1);
            } else {
                /* Interior value: open a reversible split excluding `val`.
                 * Phase 1 explores one half at a fresh level; on backtrack to
                 * this level the branch above flips to the other half. Which
                 * half goes first is randomized (size-weighted) in diversity
                 * mode -- always starting low made the search converge on the
                 * domain minimum. See _split_upper_first. */
                d->is_split     = 1;
                d->second_phase = 0;
                d->upper_first  = (uint8_t)_split_upper_first(ctx, opts,
                                                              dlo, val, dhi);
                /* d->tried_value stays == val (the excluded pivot). */
                trail_push_level(ctx);
                pr = d->upper_first ? ctx_tighten_lb64(ctx, dv, val + 1)
                                    : ctx_tighten_ub64(ctx, dv, val - 1);
            }

            if (pr == PROP_OK) {
                pr = dvs_solver_propagate(ctx);
                /* Save phase if enabled */
                if (pr == PROP_OK && opts && opts->use_phase_save
                        && opts->seed == 0 && ctx->phase_save)
                    ctx->phase_save[dv] = (int32_t)val;
            }
            /* If pr == PROP_CONFLICT, loop continues → backtracks further */
        }
    }
}

/* Pin every *inactive* soft assumption (mask bit clear) to its var=[0,0] so a
 * reset + re-solve enforces exactly the current kept-soft set. Active assumptions
 * are left at [1,1] by dvs_solver_reset. Factored out of the relaxation loop so the
 * additive re-add refinement below can reuse the identical pinning. */
static void _pin_inactive_assumptions(dvs_ctx_t *ctx) {
    for (uint32_t i = 0; i < ctx->n_assumptions; i++) {
        if (!(ctx->assumption_active_mask & (1ULL << i))) {
            uint32_t av = ctx->assumption_var_ids[i];
            Variable *v = &ctx->vars[av];
            if (ctx->n_checkpoints > 0) {
                /* Inside a checkpoint scope the write must be undone by the
                 * caller's restore: record it on the trail (it lowers the
                 * lower bound, which the trail restores like any other). */
                if (var_lo64(ctx, v) != 0) trail_record_lb(ctx, av, 0);
                if (var_hi64(ctx, v) != 0) trail_record_ub(ctx, av, 0);
            } else {
                v->lo = 0; v->hi = 0;
            }
            if (av < 64)
                ctx->unassigned_mask &= ~(1ULL << av);
        }
    }
}

/* Additive re-add refinement (soft soundness).
 *
 * The subtractive loop in dvs_solver_solve stops at the *first* satisfiable kept-soft
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
static void _refine_readd_softs(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
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
        dvs_solver_reset(ctx);
        _pin_inactive_assumptions(ctx);
        if (dvs_solver_propagate(ctx) == PROP_CONFLICT ||
                _solver_solve_core(ctx, opts) != DVS_SOLVE_OK) {
            ctx->assumption_active_mask = saved;   /* keep it dropped */
        }
    }
    /* Materialize the final accepted set: the last trial may have been a revert,
     * leaving stale search state. The final mask was proven SAT, so this resolves. */
    dvs_solver_reset(ctx);
    _pin_inactive_assumptions(ctx);
    dvs_solver_propagate(ctx);
    _solver_solve_core(ctx, opts);
}

/* ------------------------------------------------------------------ */
/* dvs_solver_solve — wrapper with assumption relaxation                   */
/* ------------------------------------------------------------------ */

static dvs_result_t _solver_solve_relax(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts);

static dvs_result_t _solver_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts);

dvs_result_t dvs_solver_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    if (!ctx || !ctx->oracle) return _solver_solve(ctx, opts);
    double t0 = dvs_oracle_clock_ms();
    dvs_result_t r = _solver_solve(ctx, opts);
    dvs_oracle_on_solve(ctx, opts, r, dvs_oracle_clock_ms() - t0);
    return r;
}

static dvs_result_t _solver_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    dvs_result_t r = _solver_solve_relax(ctx, opts);
    /* The propagation deadline belongs to this solve only: a later pin or
     * incremental add must not abort on it. */
    ctx->prop_deadline = 0.0;
    if (ctx->prop_aborted) {
        ctx->prop_aborted = 0;
        return DVS_SOLVE_TIMEOUT;
    }
    return r;
}

static dvs_result_t _solver_solve_relax(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    /* Re-activate all soft assumptions at entry. dvs_solver_reset() restores the
     * assumption vars to [1,1] but does NOT touch assumption_active_mask, so on a
     * RE-SOLVE of a reused ctx (the backend's plan-reuse path) the mask would
     * still carry the previous call's relaxations — the conflict then relaxes the
     * remaining kept soft and drops the whole set. Resetting the mask here makes
     * each solve start from the full soft set, matching a fresh compile. The
     * assumption vars are already [1,1] (compile or dvs_solver_reset restored them);
     * any var the previous call pinned to [0,0] was a direct live-write that
     * dvs_solver_reset has since undone from initial_vars. */
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
        dvs_result_t res = _solver_solve_core(ctx, opts);
        if (ctx->prop_aborted) return DVS_SOLVE_TIMEOUT;   /* never relax on it */
        if (res == DVS_SOLVE_OK) {
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
        dvs_solver_reset(ctx);

        /* Pin all relaxed assumptions to 0 by directly setting bounds.
         * We can't use ctx_tighten because the assumption var starts
         * at [1,1] after reset, and tightening UB to 0 would create
         * an empty domain [1,0] -> conflict. Direct writes are safe
         * here since we're at level 0 before search begins. */
        _pin_inactive_assumptions(ctx);

        /* Propagate the relaxations before retrying */
        if (dvs_solver_propagate(ctx) == PROP_CONFLICT) {
            /* Still conflicting — try relaxing more assumptions */
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* dvs_solver_get_value                                                    */
/* ------------------------------------------------------------------ */

/* The variable that stands for var_id. Compile merges the variables of an
 * `x == y` constraint into one (ctx->var_alias); the others are never
 * decided or propagated, so anything that reads or restricts a variable by
 * the caller's id must go through this. */
static uint32_t _alias_root(const dvs_ctx_t *ctx, uint32_t var_id) {
    if (ctx->var_alias) {
        while (ctx->var_alias[var_id] != var_id)
            var_id = ctx->var_alias[var_id];
    }
    return var_id;
}

int64_t dvs_solver_get_value(const dvs_ctx_t *ctx, uint32_t var_id) {
    if (!ctx || var_id >= ctx->n_vars) return 0;
    return var_lo64(ctx, &ctx->vars[_alias_root(ctx, var_id)]);
}


/* ------------------------------------------------------------------ */
/* dvs_solver_reset                                                        */
/* ------------------------------------------------------------------ */

/* dvs_solver_reset inside a checkpoint scope: undo everything since the
 * innermost checkpoint -- the last solve's search, its soft relaxations, and
 * the propagation of what the scope added -- keeping the propagators (and
 * so every constraint asserted in the scope) and the open checkpoints; then
 * re-establish the bounds the scope set (scope_log: pins, compile-time
 * tightenings). Their first propagation may have rested on a soft constraint
 * the solve has since relaxed; re-establishing them is what lets the
 * relaxation take effect without losing them. */
static void _reset_in_scope(dvs_ctx_t *ctx) {
    CheckpointMark *m = &ctx->checkpoints[ctx->n_checkpoints - 1];
    uint32_t lvl = m->decision_level;
    ctx->level_marks[lvl].trail_top   = m->trail_top;
    ctx->level_marks[lvl].trail_count = m->trail_count;
    ctx->level_marks[lvl].stack_mark  = m->stack_mark;
    trail_backtrack(ctx, lvl);
    trail_push_level(ctx);            /* the level the checkpoint opened */
    ctx->conflict_count = 0;

    uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                   ? ctx->n_props : ctx->n_prop_refs_capacity;
    for (uint32_t i = 0; i < lim; i++) {
        if (ctx->prop_refs[i] != EXPR_NULL) {
            Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool, ctx->prop_refs[i]);
            p->flags &= (uint8_t)~(PROP_FLAG_ENTAILED | PROP_FLAG_IN_QUEUE);
            prop_enqueue(ctx, ctx->prop_refs[i]);
        }
    }
    /* Re-establish the scope's bounds (a conflict here leaves an empty
     * domain, which the caller's next propagation reports). */
    for (uint32_t i = 0; i < ctx->n_scope_log; i++) {
        if (ctx->scope_log_depth[i] != ctx->n_checkpoints) continue;
        uint32_t v = ctx->scope_log_var[i];
        PropResult r = ctx->scope_log_is_lb[i]
            ? ctx_tighten_lb64(ctx, v, ctx->scope_log_bound[i])
            : ctx_tighten_ub64(ctx, v, ctx->scope_log_bound[i]);
        if (r == PROP_CONFLICT) break;
    }
}

static void _solver_reset(dvs_ctx_t *ctx);

void dvs_solver_reset(dvs_ctx_t *ctx) {
    _solver_reset(ctx);
    if (ctx && ctx->oracle) dvs_oracle_on_reset(ctx);
}

static void _solver_reset(dvs_ctx_t *ctx) {
    if (!ctx || !ctx->initial_vars || ctx->initial_n_vars == 0) return;
    if (ctx->n_checkpoints > 0 && ctx->prop_refs) {
        _reset_in_scope(ctx);
        return;
    }

    /* A pop can leave fewer variables than the snapshot holds. */
    uint32_t n = ctx->initial_n_vars < ctx->n_vars ? ctx->initial_n_vars : ctx->n_vars;

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
            WideBounds64 *saved = (WideBounds64 *)dvs_pool_ptr(&ctx->pool, iv->holes_offset);
            WideBounds64 *live  = (WideBounds64 *)dvs_pool_ptr(&ctx->pool, live_off);
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
            Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool,
                                                        ctx->prop_refs[i]);
            p->flags &= (uint8_t)~(PROP_FLAG_ENTAILED | PROP_FLAG_IN_QUEUE);
            prop_enqueue(ctx, ctx->prop_refs[i]);
        }
    }
    }
}

/* ------------------------------------------------------------------ */
/* dvs_solver_pin_var                                                      */
/* ------------------------------------------------------------------ */

static int _solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

int dvs_solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    int rc = _solver_pin_var(ctx, var_id, value);
    if (ctx && ctx->oracle) dvs_oracle_on_pin(ctx, var_id, value, rc);
    return rc;
}

static int _solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (!ctx || var_id >= ctx->n_vars) return -1;
    var_id = _alias_root(ctx, var_id);
    if (ctx->n_checkpoints > 0 &&
        (dvs_scope_log_bound(ctx, var_id, 1, value) != 0 ||
         dvs_scope_log_bound(ctx, var_id, 0, value) != 0))
        return -1;

    PropResult r = ctx_tighten_lb64(ctx, var_id, value);
    if (r == PROP_CONFLICT) return -1;
    r = ctx_tighten_ub64(ctx, var_id, value);
    if (r == PROP_CONFLICT) return -1;

    r = dvs_solver_propagate(ctx);
    if (r == PROP_CONFLICT) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* dvs_solver_set_seed                                                     */
/* ------------------------------------------------------------------ */

void dvs_solver_set_seed(dvs_ctx_t *ctx, uint64_t seed) {
    if (!ctx) return;
    ctx->rng_state = seed ? seed : 1;
}

/* ------------------------------------------------------------------ */
/* dvs_solver_get_values                                                   */
/* ------------------------------------------------------------------ */

void dvs_solver_get_values(const dvs_ctx_t *ctx, uint32_t n,
                       const uint32_t *var_ids, int64_t *out) {
    if (!ctx || !var_ids || !out) return;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = dvs_solver_get_value(ctx, var_ids[i]);
    }
}

/* ------------------------------------------------------------------ */
/* dvs_solver_solve_n — batch solve loop (reset+solve+read × N)           */
/* ------------------------------------------------------------------ */

int dvs_solver_solve_n(dvs_ctx_t *ctx, uint32_t n_solves,
                   uint32_t n_vars, const uint32_t *var_ids,
                   int64_t *out,
                   uint64_t base_seed,
                   uint32_t max_shave_iters) {
    if (!ctx || !var_ids || !out) return 0;

    dvs_solve_opts_t opts;
    /* Zero first: dvs_solve_opts_t has fields this list does not name
     * (time_limit_ms), and dvs_solver_solve reads them all. */
    memset(&opts, 0, sizeof(opts));
    opts.max_conflicts  = 100;
    opts.max_restarts   = 10000;
    opts.use_phase_save = 0;
    opts.use_lcg = 0;
    opts.fair_pick = 1;   /* batch solve_n is for diverse solutions → uniform */
    opts._pad[0] = 0;
    opts.max_shave_iters = max_shave_iters;

    int n_ok = 0;
    for (uint32_t i = 0; i < n_solves; i++) {
        dvs_solver_reset(ctx);
        opts.seed = base_seed + i;
        /* dvs_solver_solve, NOT _solver_solve_core: the MaxSAT relaxation loop that
         * makes a soft constraint soft lives in the wrapper, not the core. This
         * called the core directly, so a soft constraint that needed relaxing
         * was simply never relaxed -- a hard `x > 200` with a soft `x < 10`
         * returned ZERO solutions here while the same problem through `solve`
         * returned hundreds. solve_n is the batch entry point a stimulus
         * generator uses, so that was the path most likely to hit it.
         *
         * The wrapper re-activates the full soft set on entry and dvs_solver_reset
         * above restores the assumption vars from initial_vars, so each solve
         * in the batch starts from the same state rather than inheriting the
         * previous iteration's relaxations. */
        dvs_result_t r = dvs_solver_solve(ctx, &opts);
        if (r == DVS_SOLVE_OK) {
            int64_t *row = out + (uint64_t)n_ok * n_vars;
            for (uint32_t j = 0; j < n_vars; j++) {
                row[j] = dvs_solver_get_value(ctx, var_ids[j]);
            }
            n_ok++;
        }
    }
    return n_ok;
}

/* ------------------------------------------------------------------ */
/* dvs_solver_soft_active                                                  */
/* ------------------------------------------------------------------ */

int dvs_solver_soft_active(const dvs_ctx_t *ctx, uint32_t assumption_idx) {
    if (!ctx || assumption_idx >= ctx->n_assumptions) return -1;
    return (ctx->assumption_active_mask & (1ULL << assumption_idx)) ? 1 : 0;
}


/* ------------------------------------------------------------------ */
/* dvs_solver_exclude_value                                                */
/* ------------------------------------------------------------------ */

/* Check if a value is in the hole list for a variable. */
static int _is_hole(const dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (!ctx->var_holes_head) return 0;
    uint32_t off = ctx->var_holes_head[var_id];
    while (off != 0) {
        const HoleEntry *he = (const HoleEntry *)dvs_pool_ptr(&ctx->pool, off);
        if (he->value == value) return 1;
        if (he->value > value) return 0;  /* sorted ascending, done */
        off = he->next;
    }
    return 0;
}

/* Count holes within [lo, hi]. */
static uint32_t _count_holes_in_range(const dvs_ctx_t *ctx, uint32_t var_id,
                                       int64_t lo, int64_t hi) {
    if (!ctx->var_holes_head) return 0;
    uint32_t count = 0;
    uint32_t off = ctx->var_holes_head[var_id];
    while (off != 0) {
        const HoleEntry *he = (const HoleEntry *)dvs_pool_ptr(&ctx->pool, off);
        if (he->value > hi) break;
        if (he->value >= lo) count++;
        off = he->next;
    }
    return count;
}

/* Insert a value into the sorted hole list for a variable.
 * Returns 0 on success, 1 if already present, -1 on alloc failure. */
static int _insert_hole(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (!ctx->var_holes_head) return -1;

    /* Allocate a HoleEntry in the static pool */
    uint32_t he_ref = dvs_pool_alloc(&ctx->pool,
                                      (uint32_t)sizeof(HoleEntry),
                                      (uint32_t)_Alignof(HoleEntry));
    if (he_ref == EXPR_NULL) return -1;

    HoleEntry *he = (HoleEntry *)dvs_pool_ptr(&ctx->pool, he_ref);
    he->value = value;
    he->_hpad = 0;

    /* Insert sorted ascending */
    uint32_t cur_off = ctx->var_holes_head[var_id];
    uint32_t *prev_next_ptr = &ctx->var_holes_head[var_id];

    while (cur_off != 0) {
        HoleEntry *cur = (HoleEntry *)dvs_pool_ptr(&ctx->pool, cur_off);
        if (cur->value == value) return 1;  /* already present */
        if (cur->value > value) break;
        prev_next_ptr = &cur->next;
        cur_off = cur->next;
    }
    he->next = cur_off;
    *prev_next_ptr = he_ref;
    return 0;
}

static int _solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

int dvs_solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    int rc = _solver_exclude_value(ctx, var_id, value);
    if (ctx && ctx->oracle) dvs_oracle_on_exclude(ctx, var_id, value, rc);
    return rc;
}

static int _solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (!ctx || var_id >= ctx->n_vars) return -1;
    var_id = _alias_root(ctx, var_id);
    if (!ctx->var_holes_head) return -1;

    /* Already a hole: no-op */
    if (_is_hole(ctx, var_id, value)) return 0;

    /* Use the initial (pre-solve) domain for capacity checks so that
     * callers can exclude values after dvs_solver_solve() (when the current
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
     * undone by dvs_solver_reset(), but the hole list persists. */
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
