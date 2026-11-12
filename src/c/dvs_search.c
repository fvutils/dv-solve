#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "dvs_search.h"
#include "dvs_thread.h"
#include "dvs_ctx.h"
#include "dvs_engine.h"
#include "dvs_oracle_hooks.h"
#include "dvs_propagator.h"
#include "dvs_trail.h"
#include "dvs_shave.h"
#include "dvs_lcg.h"
#include "dvs_rng.h"
#include "dvs_explain.h"
#include "dvs_diffcycle.h"

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
        Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool, ctx->prop_refs[i]);
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
            Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool, ref);
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

static void _solver_reset(dvs_ctx_t *ctx);

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
    /* The state is not the output. Callers reduce draws `% span`, i.e. take
     * the low bits, and xorshift's low bits are linear in the previous
     * state's: bit 0 of one draw is bit 0 ^ bit 7 of the draw before it. An
     * 8-bit value drawn and then a 1-bit one gave the second as a function of
     * the first (a field split's top bit fixed by s: 128 of 256 solutions).
     * The SplitMix64 finalizer mixes every bit into every output bit. */
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
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

/* Solve order (dvs_builder_set_var_order): a variable's stage key, with the
 * unordered variables, stage 0, last. */
#define DVS_ORDER_LAST 256u
static inline uint32_t _order_key(const dvs_ctx_t *ctx, uint32_t i) {
    if (!ctx->var_order || i >= ctx->n_decl_vars) return DVS_ORDER_LAST;
    return ctx->var_order[i] ? ctx->var_order[i] : DVS_ORDER_LAST;
}

/* The earliest stage that still has an undecided variable: the search
 * decides only that stage's variables until it is done. */
static uint32_t _open_stage(const dvs_ctx_t *ctx) {
    uint32_t m = DVS_ORDER_LAST;
    uint32_t n = ctx->n_decl_vars < ctx->n_vars ? ctx->n_decl_vars : ctx->n_vars;
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[i];
        if ((v->flags & VAR_AUX) || ctx->var_order[i] == 0
                || ctx->var_order[i] >= m) continue;
        if (var_lo64(ctx, v) != var_hi64(ctx, v)) m = ctx->var_order[i];
    }
    return m;
}

/* A variable the problem defines by an equality, `v == <expression>` (see
 * box_skip in dvs_compile.c). Front-ends declare one for each lifted
 * operand, and pinning the variables it is defined from fixes it, so the
 * search treats it like VAR_AUX: a decision only when nothing else is open.
 * This changes only the order of decisions, so no solution is lost. Deciding
 * it early is what is slow: two variables defined from the same input can be
 * given values that no input yields, and when propagation cannot invert the
 * definition (`x & mask`) the search finds that out only by enumerating the
 * input (pyvsc F5: ~1 s and millions of conflicts on two 8-bit fields). A
 * variable in a solve order keeps its place: the user asked for it. */
static inline int _is_defined(const dvs_ctx_t *ctx, uint32_t i) {
    return ctx->box_skip && i < ctx->box_skip_n && ctx->box_skip[i]
           && _order_key(ctx, i) == DVS_ORDER_LAST;
}

/* Decision tiers from the multiply links (ctx->dec_tier); higher is decided
 * first, and MRV orders the variables inside a tier.
 *   2  the root of a square, `x` in `x * x`: x follows from the square only
 *      when the square is a perfect one, which a draw of the rest of the
 *      problem almost never gives (`a * b == c * c + k` thrashed for seconds
 *      deciding a and b first, and solves in microseconds deciding c first:
 *      the product is then fixed and a is drawn from its divisors).
 *   1  everything else.
 * Operands of a two-variable product are NOT moved later, though they too
 * follow from a fixed product: deciding them after everything else changes
 * which values the rest is drawn with (`nb == nlb * sz` put a guarded
 * `slba % 8 == 0` first, and sz came out 4096 6% of the time, not half).
 * This changes only the order of decisions, so no solution is lost. */
static void _build_dec_tier(dvs_ctx_t *ctx) {
    int any = 0;
    for (uint32_t i = 0; i < ctx->n_mul_links && !any; i++)
        any = ctx->mul_links[i].a == ctx->mul_links[i].b;
    if (!any || ctx->n_vars == 0) {
        if (ctx->dec_tier) dvs_ctx_free(ctx, ctx->dec_tier, ctx->dec_tier_n);
        ctx->dec_tier = NULL; ctx->dec_tier_n = 0;
        return;
    }
    if (ctx->dec_tier_n < ctx->n_vars) {
        if (ctx->dec_tier) dvs_ctx_free(ctx, ctx->dec_tier, ctx->dec_tier_n);
        ctx->dec_tier = (uint8_t *)DVS_ALLOC(ctx->alloc, ctx->n_vars);
        ctx->dec_tier_n = ctx->dec_tier ? ctx->n_vars : 0;
        if (!ctx->dec_tier) return;
    }
    memset(ctx->dec_tier, 1, ctx->dec_tier_n);
    for (uint32_t i = 0; i < ctx->n_mul_links; i++) {
        const DvsMulLink *ml = &ctx->mul_links[i];
        if (ml->a != ml->b || (ctx->vars[ml->a].flags & VAR_SIGNED)) continue;
        uint32_t x = dvs_value_src(ctx, ml->a);
        if (x < ctx->dec_tier_n) ctx->dec_tier[x] = 2;
    }
}

static inline uint32_t _tier(const dvs_ctx_t *ctx, uint32_t i) {
    return (ctx->dec_tier && i < ctx->dec_tier_n) ? ctx->dec_tier[i] : 1u;
}

static uint32_t _select_unassigned(dvs_ctx_t *ctx) {
    /* With a solve order, only the open stage's variables are candidates. */
    const uint32_t stage = ctx->var_order ? _open_stage(ctx) : 0;
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
            uint32_t best_t = 0;
            uint32_t lim = L->vsids.n_vars < ctx->n_vars
                           ? L->vsids.n_vars : ctx->n_vars;
            for (uint32_t i = 0; i < lim; i++) {
                Variable *v = &ctx->vars[i];
                if ((v->flags & VAR_AUX) || _is_defined(ctx, i)) continue;
                if (stage && _order_key(ctx, i) != stage) continue;
                int64_t lo = var_lo64(ctx, v);
                int64_t hi = var_hi64(ctx, v);
                if (lo == hi) continue;
                uint32_t t = _tier(ctx, i);
                if (best_v == EXPR_NULL || t > best_t ||
                    (t == best_t && L->vsids.activity[i] > best_a)) {
                    best_a = L->vsids.activity[i];
                    best_v = i;
                    best_t = t;
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
    uint32_t best_t       = 0;       /* best's decision tier (_build_dec_tier) */
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
            if ((v->flags & VAR_AUX) || _is_defined(ctx, i)) {
                /* Aux is a low-priority decision: prefer real vars first. */
                if (best_aux == EXPR_NULL || dom < best_aux_dom) { best_aux_dom = dom; best_aux = i; }
                continue;
            }
            if (stage && _order_key(ctx, i) != stage) continue;
            uint32_t t = _tier(ctx, i);
            if (best == EXPR_NULL || t > best_t || (t == best_t && dom < best_dom)) {
                best_dom = dom; best = i; n_best = 1; best_t = t;
            } else if (ctx->fair_pick && t == best_t && dom == best_dom) {
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
            if ((v->flags & VAR_AUX) || _is_defined(ctx, i)) {
                if (best_aux == EXPR_NULL || dom < best_aux_dom) { best_aux_dom = dom; best_aux = i; }
                continue;
            }
            if (stage && _order_key(ctx, i) != stage) continue;
            uint32_t t = _tier(ctx, i);
            if (best == EXPR_NULL || t > best_t || (t == best_t && dom < best_dom)) {
                best_dom = dom; best = i; n_best = 1; best_t = t;
            } else if (ctx->fair_pick && t == best_t && dom == best_dom) {
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

/* var_id's dist entries, and an entry's weight inside the feasible domain
 * [lo, hi] (0: none of it is). Entry bounds are bit patterns like the var's
 * own: order them per the var's signedness, or an unsigned 64-bit var's
 * upper-half entries (negative as int64) never overlap its domain and are
 * never drawn. */
static const DistMetaEntry *_dist_entries(const dvs_ctx_t *ctx, uint32_t var_id,
                                          uint32_t *ne) {
    const DistMeta *dm = (const DistMeta *)dvs_spool_ptr(&ctx->pool,
                                                         ctx->dist_offsets[var_id]);
    *ne = dm->n_entries;
    return (const DistMetaEntry *)(dm + 1);
}

static uint64_t _dist_weight(const Variable *var, const DistMetaEntry *e,
                             int64_t lo, int64_t hi, int64_t *elo, int64_t *ehi) {
    if (e->weight == 0) return 0;
    *elo = var_b_max(var, e->lo, lo);
    *ehi = var_b_min(var, e->hi, hi);
    if (var_b_gt(var, *elo, *ehi)) return 0;  /* no overlap with feasible domain */
    if (e->is_per_value)
        return (uint64_t)e->weight * ((uint64_t)(*ehi - *elo) + 1u);
    return (uint64_t)e->weight;
}

/* The total weight of var_id's entries inside [lo, hi]. */
static uint64_t _dist_total(const dvs_ctx_t *ctx, uint32_t var_id,
                            int64_t lo, int64_t hi) {
    uint32_t ne;
    const DistMetaEntry *entries = _dist_entries(ctx, var_id, &ne);
    uint64_t total = 0;
    int64_t elo, ehi;
    for (uint32_t i = 0; i < ne; i++)
        total += _dist_weight(&ctx->vars[var_id], &entries[i], lo, hi, &elo, &ehi);
    return total;
}

/* The entry `roll` (below _dist_total) falls in, as its part of [lo, hi].
 * 0 if none does (shouldn't happen). */
static int _dist_entry(const dvs_ctx_t *ctx, uint32_t var_id, int64_t lo, int64_t hi,
                       uint64_t roll, int64_t *elo, int64_t *ehi) {
    uint32_t ne;
    const DistMetaEntry *entries = _dist_entries(ctx, var_id, &ne);
    uint64_t cum = 0;
    for (uint32_t i = 0; i < ne; i++) {
        cum += _dist_weight(&ctx->vars[var_id], &entries[i], lo, hi, elo, ehi);
        if (roll < cum) return 1;
    }
    return 0;
}

/* Pick a value from a distribution-constrained variable using weighted
 * random selection.  Returns the chosen value, or falls through to
 * uniform random if no valid dist entry intersects the feasible domain. */
static int64_t _pick_value_dist(dvs_ctx_t *ctx, uint32_t var_id,
                                 int64_t lo, int64_t hi) {
    if (ctx->dist_offsets[var_id] == 0) return _rand_range64(ctx, lo, hi);
    /* Only ranges that intersect the current feasible domain count. */
    uint64_t total_weight = _dist_total(ctx, var_id, lo, hi);
    if (total_weight == 0) return _rand_range64(ctx, lo, hi);
    /* Roll a random number in [0, total_weight); the entry it falls in wins,
     * and a value is drawn uniformly within it. */
    int64_t elo, ehi;
    if (_dist_entry(ctx, var_id, lo, hi, _rand64(ctx) % total_weight, &elo, &ehi))
        return _rand_range64(ctx, elo, ehi);
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
    for (const HoleEntry *he = dvs_hole_first(ctx, var_id); he; he = he->next) {
        if (he->value < lo) continue;
        if (he->value > hi) break;
        uint64_t gap = (uint64_t)(he->value - cur);  /* valid count in [cur, hole) */
        if (k < gap) return cur + (int64_t)k;
        k -= gap;
        cur = he->value + 1;
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

/* A drawn value of an operand of a fixed product moved to a divisor of it
 * (DvsMulLink). Once `r == a * b` has r fixed to K, a's feasible values are
 * the divisors of K whose cofactor is in b's domain (_bv_mul_divisors), but
 * its domain is only their hull, so a uniform draw from the hull nearly
 * always fails. Drawing among the divisors themselves is uniform over the
 * feasible values, and b then follows. */
static int64_t _pick_mul_divisor(dvs_ctx_t *ctx, uint32_t var_id, int64_t v,
                                 int64_t lo, int64_t hi) {
    if (!ctx->n_mul_links || (ctx->vars[var_id].flags & VAR_SIGNED)) return v;
    for (uint32_t i = 0; i < ctx->n_mul_links; i++) {
        const DvsMulLink *ml = &ctx->mul_links[i];
        if (ml->a == ml->b) continue;
        for (int side = 0; side < 2; side++) {
            uint32_t x = side ? ml->b : ml->a, y = side ? ml->a : ml->b;
            if (x != var_id && dvs_value_src(ctx, x) != var_id) continue;
            const Variable *rv = &ctx->vars[ml->r], *yv = &ctx->vars[y];
            if ((rv->flags | yv->flags | ctx->vars[x].flags) & VAR_SIGNED) continue;
            uint64_t K = (uint64_t)var_lo64(ctx, rv);
            if (K == 0 || (uint64_t)var_hi64(ctx, rv) != K) continue;
            uint64_t ylo = (uint64_t)var_lo64(ctx, yv), yhi = (uint64_t)var_hi64(ctx, yv);
            if (ylo == yhi || yhi == 0) continue;
            uint64_t mask = ml->width >= 64 ? ~(uint64_t)0
                                            : (((uint64_t)1 << ml->width) - 1);
            if ((uint64_t)hi != 0 && yhi > mask / (uint64_t)hi) continue;   /* may wrap */
            uint64_t L = K / yhi + (K % yhi != 0), U = ylo ? K / ylo : (uint64_t)hi;
            if (L < (uint64_t)lo) L = (uint64_t)lo;
            if (U > (uint64_t)hi) U = (uint64_t)hi;
            if (L > U) continue;
            if (!dvs_divisors(&ctx->divisors, K, 1u << 17)) continue;
            uint32_t j0 = dvs_divisors_lower(&ctx->divisors, L);
            uint32_t j1 = dvs_divisors_upper(&ctx->divisors, U);
            if (j0 >= j1) continue;
            for (int attempt = 0; attempt < 8; attempt++) {
                int64_t d = (int64_t)ctx->divisors.divs[j0 + _rand64(ctx) % (j1 - j0)];
                if (!_is_hole(ctx, var_id, d)) return d;
            }
            return v;
        }
    }
    return v;
}

/* ---- Pick domain: draw only values the problem can still take ----------
 *
 * A variable's domain is its bounds (and a hole list), but propagators know
 * more than bounds say, and a value drawn between the bounds that they rule
 * out is a conflict: one point excluded per miss.
 *   - known bits: `(x & m) == r` once m and r are fixed (the alignment
 *     `(src & ((1 << width) - 1)) == 0` once width is decided), x == a << k
 *     (low k_lo bits zero), x % 2^k fixed, x[h:l] fixed, and the static
 *     known bits of the initial compile (sk_mask);
 *   - a congruence: x % b == r with b and r fixed (b not a power of two);
 *   - a value set: a disjunction whose live disjuncts all constrain x, such
 *     as `len inside {0, 8, 16, 32, 64}` once `xfer == 2` falsified the
 *     disjunct `xfer != 2`, or `a inside {[lo:hi], ...}`. The disjuncts reach
 *     x through the front end's guard variables (reifications, an ITE for
 *     `&&`, `g1 + g2 == 1` for `!`), which are decoded here.
 * The value picker draws uniformly among the values that satisfy all three
 * (and miss the holes), and the count-weighted picker counts them, so a
 * selector such as `width` is weighted by the number of aligned values each
 * of its values leaves (the uniform-over-solutions weights).
 *
 * Every fact is read from a propagator that holds -- unguarded, or guarded
 * by a guard fixed to 1 -- given the current bounds, and each set is an over-
 * approximation of x's feasible values: only values that are impossible are
 * left out, so no solution is lost. When the facts leave no value inside the
 * bounds, or cannot be drawn from, the picker draws as before and the search
 * finds the conflict. Signed, 1-bit and wider-than-64-bit variables have no
 * facts.
 *
 * The propagators each variable takes part in are indexed once per compiled
 * problem (DvsPickIx); x shares the facts of every variable a plain equality
 * makes equal to it (`len` and its 32-bit copy). DVS_PICKDOM=0 turns it off. */

static int _pd_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DVS_PICKDOM");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached;
}

#define PD_NONE   UINT32_MAX
#define PD_MAXIV  32u
#define PD_DEPTH  8
#define PD_BUDGET 256

typedef struct DvsPickIx {
    uint32_t  epoch, n_props, n_vars, restore_gen;
    size_t    bytes;
    uint32_t *rep;     /* [n_vars] class representative; PD_NONE: no facts
                        * (signed, wider than 64 bits)                     */
    uint32_t *def;     /* [n_vars] propagator that defines a guard var    */
    uint32_t *off;     /* [n_vars + 1] facts of class rep r:              */
    uint32_t *fact;    /*   fact[off[r] .. off[r + 1]), propagator ids    */
    uint64_t *skm, *skv;  /* [n_vars] static known bits of the class      */
    int       any;     /* some class has facts                            */
} DvsPickIx;

/* Class r has facts. */
static inline int _pd_has(const DvsPickIx *ix, uint32_t r) {
    return ix->off[r] != ix->off[r + 1] || ix->skm[r];
}

/* A set of unsigned values: sorted disjoint intervals, or everything. */
typedef struct {
    uint32_t n;
    uint8_t  univ;
    uint64_t lo[PD_MAXIV], hi[PD_MAXIV];
} PdSet;

/* What the facts say about one variable. */
typedef struct {
    uint64_t km, kv;      /* known bits: x & km == kv                  */
    uint64_t cm, cr;      /* congruence: x % cm == cr (cm 1: none)     */
    PdSet    set;
    uint64_t maxv;        /* the variable's largest value              */
} PdFacts;

static inline uint64_t _pd_maxv(uint32_t w) {
    return w >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1u);
}

static inline int _pd_fixed(const dvs_ctx_t *ctx, uint32_t v, uint64_t *val) {
    const Variable *x = &ctx->vars[v];
    int64_t lo = var_lo64(ctx, x);
    if (lo != var_hi64(ctx, x)) return 0;
    *val = (uint64_t)lo;
    return 1;
}

/* Propagator `pid` holds: unguarded, or its guard is fixed to 1. */
static inline int _pd_live(const dvs_ctx_t *ctx, uint32_t pid) {
    if (!ctx->prop_guard_vars || pid >= ctx->n_prop_refs_capacity) return 1;
    uint32_t g = ctx->prop_guard_vars[pid];
    if (g == EXPR_NULL) return 1;
    return var_lo64(ctx, &ctx->vars[g]) == 1 && var_hi64(ctx, &ctx->vars[g]) == 1;
}

static inline const Propagator *_pd_prop(const dvs_ctx_t *ctx, uint32_t pid) {
    return (const Propagator *)dvs_spool_ptr(&ctx->pool, ctx->prop_refs[pid]);
}

/* ---- sets ---- */

/* The set operations keep their temporary out of the recursive decoders'
 * frames (_pd_bool). */
#if defined(_MSC_VER)
#define PD_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define PD_NOINLINE __attribute__((noinline))
#else
#define PD_NOINLINE
#endif

static void _pd_iv(PdSet *s, uint64_t lo, uint64_t hi) {
    s->univ = 0;
    s->n = lo <= hi;
    s->lo[0] = lo;
    s->hi[0] = hi;
}

static PD_NOINLINE void _pd_and(PdSet *out, const PdSet *a, const PdSet *b) {
    if (a->univ) { *out = *b; return; }
    if (b->univ) { *out = *a; return; }
    PdSet r;
    r.univ = 0; r.n = 0;
    uint32_t i = 0, j = 0;
    while (i < a->n && j < b->n) {
        uint64_t lo = a->lo[i] > b->lo[j] ? a->lo[i] : b->lo[j];
        uint64_t hi = a->hi[i] < b->hi[j] ? a->hi[i] : b->hi[j];
        if (lo <= hi) {
            if (r.n == PD_MAXIV) { r.hi[r.n - 1] = hi; }   /* hull: wider */
            else { r.lo[r.n] = lo; r.hi[r.n] = hi; r.n++; }
        }
        if (a->hi[i] < b->hi[j]) i++; else j++;
    }
    *out = r;
}

static PD_NOINLINE void _pd_or(PdSet *out, const PdSet *a, const PdSet *b) {
    if (a->univ || b->univ) { out->univ = 1; out->n = 0; return; }
    PdSet r;
    r.univ = 0; r.n = 0;
    uint32_t i = 0, j = 0;
    while (i < a->n || j < b->n) {
        int ta = j == b->n || (i < a->n && a->lo[i] <= b->lo[j]);
        uint64_t lo = ta ? a->lo[i] : b->lo[j], hi = ta ? a->hi[i] : b->hi[j];
        if (ta) i++; else j++;
        if (r.n && (lo <= r.hi[r.n - 1] || lo - 1u == r.hi[r.n - 1])) {
            if (hi > r.hi[r.n - 1]) r.hi[r.n - 1] = hi;
        } else if (r.n == PD_MAXIV) {
            r.hi[r.n - 1] = hi > r.hi[r.n - 1] ? hi : r.hi[r.n - 1];   /* hull */
        } else {
            r.lo[r.n] = lo; r.hi[r.n] = hi; r.n++;
        }
    }
    *out = r;
}

/* The values v of [0, maxv] with `v op c` (op a DisjClause op). */
static void _pd_cmp(PdSet *s, uint32_t op, int64_t c64, uint64_t maxv) {
    s->univ = 0; s->n = 0;
    if (c64 < 0 || (uint64_t)c64 > maxv) { s->univ = 1; return; }
    uint64_t c = (uint64_t)c64;
    switch (op) {
    case DVS_BIN_EQ:  _pd_iv(s, c, c); return;
    case DVS_BIN_LT:  if (c) _pd_iv(s, 0, c - 1u); return;
    case DVS_BIN_LTE: _pd_iv(s, 0, c); return;
    case DVS_BIN_GT:  if (c < maxv) _pd_iv(s, c + 1u, maxv); return;
    case DVS_BIN_GTE: _pd_iv(s, c, maxv); return;
    case DVS_BIN_NEQ:
        if (c) { s->lo[s->n] = 0; s->hi[s->n] = c - 1u; s->n++; }
        if (c < maxv) { s->lo[s->n] = c + 1u; s->hi[s->n] = maxv; s->n++; }
        return;
    default:
        s->univ = 1;
    }
}

/* ---- index ---- */

static void _pd_free(dvs_ctx_t *ctx) {
    dvs_ctx_free(ctx, ctx->pick_ix, ctx->pick_ix_bytes);
    ctx->pick_ix = NULL;
    ctx->pick_ix_bytes = 0;
}

static uint32_t _pd_find(uint32_t *rep, uint32_t v) {
    while (rep[v] != v) { rep[v] = rep[rep[v]]; v = rep[v]; }
    return v;
}

/* The variables a guard var's definition reads, through further guard vars,
 * into out[] (at most cap); used to file a disjunction under each. */
static uint32_t _pd_reach(const dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t v,
                          uint32_t *out, uint32_t n, uint32_t cap, int depth) {
    for (uint32_t i = 0; i < n; i++) if (out[i] == v) return n;
    if (n == cap) return n;
    out[n++] = v;
    if (depth >= PD_DEPTH || ix->def[v] == PD_NONE) return n;
    uint32_t nw;
    const uint32_t *w = prop_watched_vars(_pd_prop(ctx, ix->def[v]), &nw);
    for (uint32_t i = 1; i < nw; i++) n = _pd_reach(ctx, ix, w[i], out, n, cap, depth + 1);
    return n;
}

static int _pd_is_bool(const dvs_ctx_t *ctx, uint32_t v) {
    const Variable *x = &ctx->vars[v];
    return !(x->flags & VAR_SIGNED) && (uint64_t)var_lo64(ctx, x) <= 1u
           && (uint64_t)var_hi64(ctx, x) <= 1u;
}

/* The variables propagator pid gives facts on, into xs[] (at most cap):
 * the count. */
static uint32_t _pd_roles(const dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t pid,
                          uint32_t *xs, uint32_t cap) {
    const Propagator *p = _pd_prop(ctx, pid);
    uint32_t h, l, nw, n = 0;
    const uint32_t *w = prop_watched_vars(p, &nw);
    switch (prop_pick_kind(p, &h, &l)) {
    case PPK_BAND: case PPK_CONCAT:
        if (nw == 3) { xs[n++] = w[0]; xs[n++] = w[1]; xs[n++] = w[2]; }
        break;
    case PPK_SHL:
        if (nw == 3) xs[n++] = w[0];
        break;
    case PPK_MOD: case PPK_SLICE:
        if (nw >= 2) xs[n++] = w[1];
        break;
    case PPK_DISJ: {
        /* Filed under the classes that two or more disjuncts reach: with
         * one, the clause's own propagator already narrows x's bounds to
         * that disjunct once the others are false. */
        const DisjClause_t *d = (const DisjClause_t *)p;
        uint32_t reach[64], cls[64], cnt[64], last[64], nc = 0;
        for (uint32_t j = 0; j < d->n_clauses; j++) {
            if (d->clauses[j].rhs_var_id != UINT32_MAX) continue;
            uint32_t m = _pd_reach(ctx, ix, d->clauses[j].var_id, reach, 0, 64, 0);
            for (uint32_t i = 0; i < m; i++) {
                if (reach[i] >= ix->n_vars || ix->rep[reach[i]] == PD_NONE) continue;
                uint32_t r = ix->rep[reach[i]], k;
                for (k = 0; k < nc; k++) if (cls[k] == r) break;
                if (k == nc) {
                    if (nc == 64) continue;
                    cls[nc] = r; cnt[nc] = 0; last[nc] = UINT32_MAX; nc++;
                }
                if (last[k] != j) { last[k] = j; cnt[k]++; }
            }
        }
        for (uint32_t k = 0; k < nc && n < cap; k++)
            if (cnt[k] >= 2) xs[n++] = cls[k];
        break;
    }
    default:
        break;
    }
    return n;
}

static PD_NOINLINE DvsPickIx *_pd_build(dvs_ctx_t *ctx, uint32_t nv, uint32_t np);

/* The index of the problem as it is now (rebuilt when it changed), or NULL. */
static inline DvsPickIx *_pd_index(dvs_ctx_t *ctx) {
    DvsPickIx *ix = (DvsPickIx *)ctx->pick_ix;
    const uint32_t nv = ctx->n_vars;
    const uint32_t np = ctx->n_props < ctx->n_prop_refs_capacity
                        ? ctx->n_props : ctx->n_prop_refs_capacity;
    if (ix && ix->epoch == ctx->wmemo_epoch && ix->n_props == np && ix->n_vars == nv
            && ix->restore_gen == ctx->restore_gen)
        return ix;
    return _pd_build(ctx, nv, np);
}

static PD_NOINLINE DvsPickIx *_pd_build(dvs_ctx_t *ctx, uint32_t nv, uint32_t np) {
    DvsPickIx *ix;
    _pd_free(ctx);
    if (nv == 0 || !ctx->prop_refs) return NULL;
    /* One block: the header, the per-variable arrays, then fact[], whose
     * size a first pass over the propagators counts (the block grows then). */
    uint32_t xs[64];
    size_t base = sizeof(DvsPickIx);
    size_t arr = (size_t)nv * (3u * sizeof(uint32_t) + 2u * sizeof(uint64_t)) + sizeof(uint32_t);
    ix = (DvsPickIx *)DVS_ALLOC(ctx->alloc, base + arr);
    if (!ix) return NULL;
    memset(ix, 0, base);
    ix->bytes = base + arr;
    char *m = (char *)(ix + 1);
    ix->skm  = (uint64_t *)m;  m += (size_t)nv * sizeof(uint64_t);
    ix->skv  = (uint64_t *)m;  m += (size_t)nv * sizeof(uint64_t);
    ix->rep  = (uint32_t *)m;  m += (size_t)nv * sizeof(uint32_t);
    ix->def  = (uint32_t *)m;  m += (size_t)nv * sizeof(uint32_t);
    ix->off  = (uint32_t *)m;
    ix->fact = NULL;
    ix->epoch = ctx->wmemo_epoch; ix->n_props = np; ix->n_vars = nv;
    ix->restore_gen = ctx->restore_gen;
    for (uint32_t v = 0; v < nv; v++) {
        const Variable *x = &ctx->vars[v];
        /* A 1-bit variable's two values are its bounds: nothing to add. */
        ix->rep[v] = ((x->flags & VAR_SIGNED) || VAR_IS_TIER2(x->flags) || x->width <= 1
                      || x->width > 64) ? PD_NONE : v;
        ix->def[v] = PD_NONE;
        ix->skm[v] = ix->skv[v] = 0;
    }
    /* Classes: alias, then unguarded equalities between unsigned vars. */
    for (uint32_t v = 0; v < nv; v++) {
        if (ix->rep[v] == PD_NONE || !ctx->var_alias || ctx->var_alias[v] == v) continue;
        uint32_t a = v;
        while (ctx->var_alias[a] != a) a = ctx->var_alias[a];
        if (ix->rep[a] != PD_NONE) ix->rep[_pd_find(ix->rep, v)] = _pd_find(ix->rep, a);
    }
    for (uint32_t pid = 0; pid < np; pid++) {
        if (ctx->prop_refs[pid] == EXPR_NULL) continue;
        const Propagator *p = _pd_prop(ctx, pid);
        uint32_t h, l, nw;
        const uint32_t *w = prop_watched_vars(p, &nw);
        dvs_pick_kind_t k = prop_pick_kind(p, &h, &l);
        if (k == PPK_EQ && nw == 2 && w[0] < nv && w[1] < nv && ix->rep[w[0]] != PD_NONE
                && ix->rep[w[1]] != PD_NONE
                && !(ctx->prop_guard_vars && pid < ctx->n_prop_refs_capacity
                     && ctx->prop_guard_vars[pid] != EXPR_NULL)) {
            uint32_t a = _pd_find(ix->rep, w[0]), b = _pd_find(ix->rep, w[1]);
            if (a != b) ix->rep[a < b ? b : a] = a < b ? a : b;
        }
        /* A guard var's definition: a reification or ITE result, or either
         * side of `g1 + g2 == 1`. */
        if ((k == PPK_REIF_LE || k == PPK_REIF_EQ || k == PPK_ITE) && nw >= 3 && w[0] < nv
                && ix->def[w[0]] == PD_NONE)
            ix->def[w[0]] = pid;
        if (k == PPK_ADD && nw == 3) {
            uint64_t one;
            if (_pd_fixed(ctx, w[0], &one) && one == 1 && _pd_is_bool(ctx, w[1])
                    && _pd_is_bool(ctx, w[2])) {
                if (ix->def[w[1]] == PD_NONE) ix->def[w[1]] = pid;
                if (ix->def[w[2]] == PD_NONE) ix->def[w[2]] = pid;
            }
        }
    }
    for (uint32_t v = 0; v < nv; v++)
        if (ix->rep[v] != PD_NONE) ix->rep[v] = _pd_find(ix->rep, v);
    for (uint32_t v = 0; v < nv; v++) {
        uint32_t r = ix->rep[v];
        if (r == PD_NONE || v >= ctx->sk_n || !ctx->sk_mask[v]) continue;
        /* Known high bits over free low ones are an interval, which the
         * bounds already hold (a zero extension's top bits). */
        uint64_t fr = ~ctx->sk_mask[v] & _pd_maxv(ctx->vars[v].width);
        if ((fr & (fr + 1u)) == 0) continue;
        uint64_t both = ix->skm[r] & ctx->sk_mask[v];
        if ((ix->skv[r] & both) != (ctx->sk_val[v] & both)) continue;
        ix->skm[r] |= ctx->sk_mask[v];
        ix->skv[r] |= ctx->sk_val[v] & ctx->sk_mask[v];
    }
    /* Facts per class: count, then fill (off[] is the cursor). */
    memset(ix->off, 0, ((size_t)nv + 1u) * sizeof(uint32_t));
    for (int fill = 0; fill < 2; fill++) {
        if (fill) {
            for (uint32_t v = 0; v < nv; v++) ix->off[v + 1] += ix->off[v];
            uint32_t nf = ix->off[nv];
            size_t nb = base + arr + (size_t)nf * sizeof(uint32_t);
            DvsPickIx *g = (DvsPickIx *)dvs_ctx_resize(ctx, ix, ix->bytes, nb);
            if (!g) { dvs_ctx_free(ctx, ix, ix->bytes); return NULL; }
            ix = g;
            ix->bytes = nb;
            m = (char *)(ix + 1);
            ix->skm  = (uint64_t *)m;  m += (size_t)nv * sizeof(uint64_t);
            ix->skv  = (uint64_t *)m;  m += (size_t)nv * sizeof(uint64_t);
            ix->rep  = (uint32_t *)m;  m += (size_t)nv * sizeof(uint32_t);
            ix->def  = (uint32_t *)m;  m += (size_t)nv * sizeof(uint32_t);
            ix->off  = (uint32_t *)m;  m += ((size_t)nv + 1u) * sizeof(uint32_t);
            ix->fact = (uint32_t *)m;
        }
        for (uint32_t pid = 0; pid < np; pid++) {
            if (ctx->prop_refs[pid] == EXPR_NULL) continue;
            uint32_t n = _pd_roles(ctx, ix, pid, xs, 64);
            for (uint32_t i = 0; i < n; i++) {
                if (xs[i] >= nv || ix->rep[xs[i]] == PD_NONE) continue;
                uint32_t r = ix->rep[xs[i]], j;
                for (j = 0; j < i; j++)            /* once per class */
                    if (xs[j] < nv && ix->rep[xs[j]] == r) break;
                if (j < i) continue;
                if (fill) ix->fact[ix->off[r]++] = pid;
                else ix->off[r + 1]++;
            }
        }
    }
    /* off[r] is now the end of r's facts: shift back. */
    for (uint32_t v = nv; v > 0; v--) ix->off[v] = ix->off[v - 1];
    ix->off[0] = 0;
    /* A concatenation only carries facts between its result and its parts:
     * keep it when the result's class has facts of its own. A field split,
     * `x == {hi, lo}` with nothing else on x, would cost a look at every
     * decision and give nothing. */
    uint8_t *own = (uint8_t *)DVS_ALLOC(ctx->alloc, nv);
    if (own) {
        for (uint32_t v = 0; v < nv; v++) {
            own[v] = ix->rep[v] == v && ix->skm[v] != 0;
            for (uint32_t i = ix->off[v]; i < ix->off[v + 1] && !own[v]; i++) {
                uint32_t h, l;
                own[v] = prop_pick_kind(_pd_prop(ctx, ix->fact[i]), &h, &l) != PPK_CONCAT;
            }
        }
        uint32_t o = 0, b = 0;
        for (uint32_t v = 0; v < nv; v++) {
            uint32_t e = ix->off[v + 1];
            ix->off[v] = o;
            for (uint32_t i = b; i < e; i++) {
                uint32_t pid = ix->fact[i], h, l, nw;
                const Propagator *p = _pd_prop(ctx, pid);
                if (prop_pick_kind(p, &h, &l) == PPK_CONCAT) {
                    const uint32_t r = ix->rep[prop_watched_vars(p, &nw)[0]];
                    if (r == PD_NONE || !own[r]) continue;
                }
                ix->fact[o++] = pid;
            }
            b = e;
        }
        ix->off[nv] = o;
        dvs_ctx_free(ctx, own, nv);
    }
    for (uint32_t v = 0; v < nv && !ix->any; v++) ix->any = ix->rep[v] == v && _pd_has(ix, v);
    ctx->pick_ix = ix;
    ctx->pick_ix_bytes = ix->bytes;
    return ix;
}

/* ---- facts ---- */

static int _pd_bool(const dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t r, uint64_t maxv,
                    uint32_t g, uint64_t want, PdSet *out, int depth, int *budget);

/* The values of class r for which variable `v` can take value `want` (v is
 * then in r's class) or, v a guard var, can be `want`. */
static void _pd_var_is(const dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t r, uint64_t maxv,
                       uint32_t v, uint64_t want, PdSet *out, int depth, int *budget) {
    uint64_t f;
    out->univ = 1; out->n = 0;
    if (v >= ix->n_vars) return;
    if (ix->rep[v] == r) { _pd_cmp(out, DVS_BIN_EQ, (int64_t)want, maxv); return; }
    if (_pd_fixed(ctx, v, &f)) { if (f != want) { out->univ = 0; } return; }
    if (want <= 1u && _pd_is_bool(ctx, v)) _pd_bool(ctx, ix, r, maxv, v, want, out, depth + 1, budget);
}

/* The values of class r (largest maxv) under which guard var g can be
 * `want` (0 or 1): an over-approximation, everything when unknown. */
static int _pd_bool(const dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t r, uint64_t maxv,
                    uint32_t g, uint64_t want, PdSet *out, int depth, int *budget) {
    out->univ = 1; out->n = 0;
    if (depth > PD_DEPTH || --*budget < 0 || ix->def[g] == PD_NONE || !_pd_live(ctx, ix->def[g]))
        return 0;
    const Propagator *p = _pd_prop(ctx, ix->def[g]);
    uint32_t h, l, nw;
    const uint32_t *w = prop_watched_vars(p, &nw);
    uint64_t c;
    switch (prop_pick_kind(p, &h, &l)) {
    case PPK_REIF_LE: case PPK_REIF_EQ: {
        if (w[0] != g) return 0;
        int eq = prop_pick_kind(p, &h, &l) == PPK_REIF_EQ;
        if (w[1] < ix->n_vars && ix->rep[w[1]] == r && _pd_fixed(ctx, w[2], &c)) {
            /* g <-> x <= c,  g <-> x == c */
            if (c > maxv) return 0;
            if (eq) _pd_cmp(out, want ? DVS_BIN_EQ : DVS_BIN_NEQ, (int64_t)c, maxv);
            else    _pd_cmp(out, want ? DVS_BIN_LTE : DVS_BIN_GT, (int64_t)c, maxv);
        } else if (w[2] < ix->n_vars && ix->rep[w[2]] == r && _pd_fixed(ctx, w[1], &c)) {
            /* g <-> c <= x,  g <-> c == x */
            if (c > maxv) return 0;
            if (eq) _pd_cmp(out, want ? DVS_BIN_EQ : DVS_BIN_NEQ, (int64_t)c, maxv);
            else    _pd_cmp(out, want ? DVS_BIN_GTE : DVS_BIN_LT, (int64_t)c, maxv);
        }
        return 0;
    }
    case PPK_ITE: {
        /* g == (c ? a : b): (c and a == want) or (not c and b == want). */
        if (w[0] != g || nw != 4) return 0;
        PdSet t, u;      /* two sets a frame: this recursion's frame is small */
        _pd_var_is(ctx, ix, r, maxv, w[1], 1, &t, depth, budget);
        _pd_var_is(ctx, ix, r, maxv, w[2], want, &u, depth, budget);
        _pd_and(&t, &t, &u);                    /* c and a == want */
        _pd_var_is(ctx, ix, r, maxv, w[1], 0, &u, depth, budget);
        _pd_var_is(ctx, ix, r, maxv, w[3], want, out, depth, budget);
        _pd_and(&u, &u, out);                   /* not c and b == want */
        _pd_or(out, &t, &u);
        return 0;
    }
    case PPK_ADD:
        /* g + o == 1, both 0/1: g == want exactly when o == 1 - want. */
        if (nw != 3) return 0;
        _pd_var_is(ctx, ix, r, maxv, w[1] == g ? w[2] : w[1], 1u - want, out, depth, budget);
        return 0;
    default:
        return 0;
    }
}

/* The values of class r a live disjunction allows. */
static void _pd_disj(const dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t r, uint64_t maxv,
                     const DisjClause_t *d, PdSet *out) {
    int budget = PD_BUDGET;
    out->univ = 0; out->n = 0;
    for (uint32_t j = 0; j < d->n_clauses && !out->univ; j++) {
        uint32_t v = d->clauses[j].var_id, op = d->clauses[j].op;
        int64_t c = d->clauses[j].constant;
        PdSet s;
        s.univ = 1; s.n = 0;
        if (d->clauses[j].rhs_var_id != UINT32_MAX || v >= ix->n_vars) {
            /* var-var: anything */
        } else if (ix->rep[v] == r) {
            _pd_cmp(&s, op, c, maxv);
        } else {
            const Variable *x = &ctx->vars[v];
            uint64_t f;
            if (_pd_fixed(ctx, v, &f)) {
                /* Decided: true leaves everything, false nothing. */
                int t;
                if (x->flags & VAR_SIGNED) t = 1;    /* order: leave it */
                else switch (op) {
                case DVS_BIN_EQ:  t = f == (uint64_t)c; break;
                case DVS_BIN_NEQ: t = f != (uint64_t)c; break;
                case DVS_BIN_LT:  t = c >= 0 && f <  (uint64_t)c; break;
                case DVS_BIN_LTE: t = c >= 0 && f <= (uint64_t)c; break;
                case DVS_BIN_GT:  t = c < 0 || f >  (uint64_t)c; break;
                case DVS_BIN_GTE: t = c < 0 || f >= (uint64_t)c; break;
                default: t = 1;
                }
                if (!t) s.univ = 0;
            } else if (_pd_is_bool(ctx, v) && c >= 0 && c <= 1
                       && (op == DVS_BIN_EQ || op == DVS_BIN_GTE || op == DVS_BIN_NEQ
                           || op == DVS_BIN_LT || op == DVS_BIN_LTE || op == DVS_BIN_GT)) {
                /* g op c over g in {0, 1}: which value of g satisfies it. */
                int ok0, ok1;
                switch (op) {
                case DVS_BIN_EQ:  ok0 = c == 0; ok1 = c == 1; break;
                case DVS_BIN_NEQ: ok0 = c != 0; ok1 = c != 1; break;
                case DVS_BIN_LT:  ok0 = c > 0;  ok1 = c > 1;  break;
                case DVS_BIN_LTE: ok0 = 1;      ok1 = c >= 1; break;
                case DVS_BIN_GT:  ok0 = 0;      ok1 = c < 1;  break;
                default:          ok0 = c <= 0; ok1 = 1;      break;   /* GTE */
                }
                if (ok0 != ok1) _pd_bool(ctx, ix, r, maxv, v, (uint64_t)ok1, &s, 0, &budget);
                else if (!ok0) s.univ = 0;
            }
        }
        _pd_or(out, out, &s);
    }
}

/* Fold `x % b == r` into F's congruence; 0 when they contradict. A
 * modulus past 2^62 is not kept (the facts only get weaker). */
static int _pd_congr(PdFacts *F, uint64_t b, uint64_t r) {
    if (b <= 1u) return 1;
    r %= b;
    if (F->cm == 1u) {
        if (b > ((uint64_t)1 << 62)) return 1;
        F->cm = b; F->cr = r;
        return 1;
    }
    /* x = cr + cm * t; cm * t == r - cr (mod b). g = gcd(cm, b), and
     * u * (cm / g) == 1 (mod b / g) by the extended Euclid. */
    int64_t g = (int64_t)F->cm, h = (int64_t)b, u = 1, u1 = 0;
    if (F->cm > ((uint64_t)1 << 62) || b > ((uint64_t)1 << 62)) return 1;
    while (h) {
        int64_t q = g / h, t = g - q * h;
        g = h; h = t;
        t = u - q * u1; u = u1; u1 = t;
    }
    uint64_t d = (r + b - F->cr % b) % b;
    if (d % (uint64_t)g) return 0;
    uint64_t bg = b / (uint64_t)g;
    if (bg == 1u) return 1;
    if (F->cm > ((uint64_t)1 << 62) / bg) return 1;
    uint64_t inv = (uint64_t)(((u % (int64_t)bg) + (int64_t)bg) % (int64_t)bg);
    uint64_t t = (uint64_t)(((unsigned __int128)((d / (uint64_t)g) % bg) * inv) % bg);
    F->cr += F->cm * t;
    F->cm *= bg;
    F->cr %= F->cm;
    return 1;
}

/* Known bits m, v of class r; 0 when they contradict F's. */
static int _pd_bits(PdFacts *F, uint64_t m, uint64_t v) {
    m &= F->maxv;
    uint64_t both = F->km & m;
    if ((F->kv & both) != (v & both)) return 0;
    F->km |= m;
    F->kv |= v & m;
    return 1;
}

/* The facts on x now. 0: none (draw as before); -1: they contradict (draw
 * as before; the search finds the conflict); 1: in *F. */
static int _pd_class(dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t x, PdFacts *F, int depth);

static int _pd_facts(dvs_ctx_t *ctx, uint32_t x, PdFacts *F) {
    if (!ctx->prop_refs || ctx->n_props == 0 || !_pd_enabled()) return 0;
    DvsPickIx *ix = _pd_index(ctx);
    if (!ix || !ix->any || x >= ix->n_vars || ix->rep[x] == PD_NONE) return 0;
    return _pd_class(ctx, ix, x, F, 0);
}

/* The facts on x's class. depth > 0: a concatenation's result, for its
 * known bits only (no value set, no further concatenation). */
static int _pd_class(dvs_ctx_t *ctx, const DvsPickIx *ix, uint32_t x, PdFacts *F, int depth) {
    const uint32_t r = ix->rep[x];
    F->maxv = _pd_maxv(ctx->vars[x].width);
    F->km = F->kv = 0;
    F->cm = 1; F->cr = 0;
    F->set.univ = 1; F->set.n = 0;
    if (!_pd_has(ix, r)) return 0;
    if (ix->skm[r] && !_pd_bits(F, ix->skm[r], ix->skv[r])) return -1;
    for (uint32_t i = ix->off[r]; i < ix->off[r + 1]; i++) {
        uint32_t pid = ix->fact[i];
        if (!_pd_live(ctx, pid)) continue;
        const Propagator *p = _pd_prop(ctx, pid);
        uint32_t h = 0, l = 0, nw;
        const uint32_t *w = prop_watched_vars(p, &nw);
        uint64_t a, b, c;
        switch (prop_pick_kind(p, &h, &l)) {
        case PPK_BAND:
            /* x & m == c with m, c fixed; x == a & m: x has no bit outside m. */
            for (int s = 1; s <= 2; s++) {
                if (ix->rep[w[s]] == r && _pd_fixed(ctx, w[3 - s], &b) && _pd_fixed(ctx, w[0], &c)
                        && !_pd_bits(F, b, c))
                    return -1;
            }
            if (ix->rep[w[0]] == r) {
                if (_pd_fixed(ctx, w[1], &b) && !_pd_bits(F, ~b, 0)) return -1;
                if (_pd_fixed(ctx, w[2], &b) && !_pd_bits(F, ~b, 0)) return -1;
            }
            break;
        case PPK_SHL: {
            /* x == a << k: the low k bits are zero, k at least its lower bound. */
            if (ix->rep[w[0]] != r) break;
            uint64_t k = (uint64_t)var_lo64(ctx, &ctx->vars[w[2]]);
            if (ctx->vars[w[2]].flags & VAR_SIGNED) break;
            if (k >= 64) k = 63;
            if (k && !_pd_bits(F, ((uint64_t)1 << k) - 1u, 0)) return -1;
            break;
        }
        case PPK_MOD:
            /* c == x % b with b, c fixed. */
            if (ix->rep[w[1]] != r || !_pd_fixed(ctx, w[2], &b) || !_pd_fixed(ctx, w[0], &c)
                    || b == 0 || (ctx->vars[w[2]].flags & VAR_SIGNED)
                    || (ctx->vars[w[0]].flags & VAR_SIGNED))
                break;
            if (c >= b) return -1;
            if ((b & (b - 1u)) == 0) { if (!_pd_bits(F, b - 1u, c)) return -1; }
            else if (!_pd_congr(F, b, c)) return -1;
            break;
        case PPK_SLICE:
            /* c == x[h:l] with c fixed. */
            if (ix->rep[w[1]] != r || h < l || h >= 64 || !_pd_fixed(ctx, w[0], &c)) break;
            a = _pd_maxv(h - l + 1u);
            if (!_pd_bits(F, a << l, (c & a) << l)) return -1;
            break;
        case PPK_CONCAT: {
            /* r == {hi, lo}: x is r, with a part fixed; or x is a part, with
             * r's known bits. */
            if (nw != 3 || l == 0 || l >= 64) break;
            const uint64_t lm = _pd_maxv(l);
            if (ix->rep[w[0]] == r) {
                if (_pd_fixed(ctx, w[2], &c) && !_pd_bits(F, lm, c)) return -1;
                if (_pd_fixed(ctx, w[1], &c) && !_pd_bits(F, ~lm, c << l)) return -1;
            } else if (depth == 0 && ix->rep[w[0]] != PD_NONE) {
                PdFacts G;
                if (_pd_class(ctx, ix, w[0], &G, 1) < 0) return -1;
                if (ix->rep[w[2]] == r && !_pd_bits(F, G.km & lm, G.kv & lm)) return -1;
                if (ix->rep[w[1]] == r && !_pd_bits(F, G.km >> l, G.kv >> l)) return -1;
            }
            break;
        }
        case PPK_DISJ: {
            if (depth) break;
            PdSet s;
            _pd_disj(ctx, ix, r, F->maxv, (const DisjClause_t *)p, &s);
            _pd_and(&F->set, &F->set, &s);
            break;
        }
        default:
            break;
        }
    }
    /* Fold the low known bits into the congruence (x % 2^t == kv's low t
     * bits) when there is one, or when they are all the known bits (an
     * alignment): an arithmetic progression counts and draws in O(1).
     * Known bits elsewhere are counted by rank (_pd_kcount). */
    if (~F->km) {
        uint32_t t = (uint32_t)__builtin_ctzll(~F->km);
        if (t > 40) t = 40;
        const uint64_t low = ((uint64_t)1 << t) - 1u;
        if (t && (F->cm > 1u || !(F->km & ~low))
                && !_pd_congr(F, (uint64_t)1 << t, F->kv & low))
            return -1;
    }
    if (!F->km && F->cm == 1u && F->set.univ) return 0;
    return 1;
}

/* #{y in [0, z) : y & km == kv}. */
static uint64_t _pd_rank(uint64_t z, uint64_t km, uint64_t kv) {
    uint64_t n = 0;
    for (int i = 63; i >= 0; i--) {
        uint64_t bit = (uint64_t)1 << i;
        if (z & bit) {
            /* y agrees with z above i and has 0 at i. */
            if (!(km & bit) || !(kv & bit)) {
                uint64_t below = bit - 1u;
                n += (uint64_t)1 << __builtin_popcountll(~km & below);
            }
            if ((km & bit) && !(kv & bit)) return n;   /* z's 1 not allowed */
        } else if ((km & bit) && (kv & bit)) {
            return n;                                    /* y would exceed z */
        }
    }
    return n;
}

/* #{y in [lo, hi] : y & km == kv}. */
static uint64_t _pd_kcount(uint64_t lo, uint64_t hi, uint64_t km, uint64_t kv) {
    uint64_t n = _pd_rank(hi, km, kv) + ((hi & km) == kv) - _pd_rank(lo, km, kv);
    return n;
}

/* The i-th (from 0) value with y & km == kv. */
static uint64_t _pd_kunrank(uint64_t i, uint64_t km, uint64_t kv) {
    uint64_t y = kv & km, free_ = ~km;
    while (free_ && i) {
        uint64_t bit = free_ & (~free_ + 1u);
        if (i & 1u) y |= bit;
        i >>= 1;
        free_ &= free_ - 1u;
    }
    return y;
}

/* #{y in [lo, hi] : y % cm == cr}. */
static uint64_t _pd_ccount(uint64_t lo, uint64_t hi, uint64_t cm, uint64_t cr) {
    uint64_t f = lo + (cr + cm - lo % cm) % cm;      /* first >= lo */
    if (f < lo || f > hi) return 0;
    return (hi - f) / cm + 1u;
}

/* Interval j of [lo, hi] cut by F's set: *a, *b; 0 past the last. */
static int _pd_part(const PdFacts *F, uint64_t lo, uint64_t hi, uint32_t j,
                    uint64_t *a, uint64_t *b) {
    if (F->set.univ) { *a = lo; *b = hi; return j == 0; }
    for (uint32_t i = 0, k = 0; i < F->set.n; i++) {
        uint64_t x = F->set.lo[i] > lo ? F->set.lo[i] : lo;
        uint64_t y = F->set.hi[i] < hi ? F->set.hi[i] : hi;
        if (x > y) continue;
        if (k++ == j) { *a = x; *b = y; return 1; }
    }
    return 0;
}

/* Values of [a, b] the lattice allows (the congruence's count when there is
 * one: its other known bits are checked when drawn). */
static uint64_t _pd_icount(const PdFacts *F, uint64_t a, uint64_t b) {
    if (F->cm > 1u) return _pd_ccount(a, b, F->cm, F->cr);
    if (F->km) return _pd_kcount(a, b, F->km, F->kv);
    return b - a == ~(uint64_t)0 ? b - a : b - a + 1u;     /* all 2^64: saturate */
}

/* How many values of [lo, hi] the facts allow (as a double: may exceed
 * 2^53, and the count-weighted picker multiplies them). */
static double _pd_count(const PdFacts *F, uint64_t lo, uint64_t hi) {
    double n = 0.0;
    uint64_t a, b;
    for (uint32_t j = 0; _pd_part(F, lo, hi, j, &a, &b); j++) n += (double)_pd_icount(F, a, b);
    return n;
}

/* The count of _pd_count exactly: 1 and *n, or 0 when it overflows. */
static int _pd_total(const PdFacts *F, uint64_t lo, uint64_t hi, uint64_t *n) {
    uint64_t a, b, t = 0;
    for (uint32_t j = 0; _pd_part(F, lo, hi, j, &a, &b); j++) {
        uint64_t c = _pd_icount(F, a, b);
        if (t + c < t) return 0;
        t += c;
    }
    *n = t;
    return 1;
}

/* The value of rank i (from 0) among those _pd_total counts. */
static uint64_t _pd_nth(const PdFacts *F, uint64_t lo, uint64_t hi, uint64_t i) {
    uint64_t a = lo, b = hi;
    for (uint32_t j = 0; _pd_part(F, lo, hi, j, &a, &b); j++) {
        uint64_t c = _pd_icount(F, a, b);
        if (i < c) {
            if (F->cm > 1u) return a + (F->cr + F->cm - a % F->cm) % F->cm + i * F->cm;
            if (!F->km) return a + i;
            return _pd_kunrank(_pd_rank(a, F->km, F->kv) + i, F->km, F->kv);
        }
        i -= c;
    }
    return b;
}

/* A value of x in [lo, hi] (unsigned) drawn uniformly among those the facts
 * allow, missing holes: 1 and *out, or 0 (none found). */
static int _pd_draw(dvs_ctx_t *ctx, uint32_t x, const PdFacts *F, uint64_t lo, uint64_t hi,
                    int64_t *out) {
    uint64_t a, b, tot = 0;
    uint64_t cnt[PD_MAXIV];
    uint32_t np = 0;
    for (; np < PD_MAXIV && _pd_part(F, lo, hi, np, &a, &b); np++) {
        cnt[np] = _pd_icount(F, a, b);
        if (tot + cnt[np] < tot) return 0;              /* overflow */
        tot += cnt[np];
    }
    if (tot == 0) return 0;
    for (int tries = 0; tries < 64; tries++) {
        uint64_t k = _rand64(ctx) % tot;
        uint32_t j = 0;
        while (k >= cnt[j]) { k -= cnt[j]; j++; }
        _pd_part(F, lo, hi, j, &a, &b);
        uint64_t y;
        if (F->cm > 1u) {
            y = a + (F->cr + F->cm - a % F->cm) % F->cm + k * F->cm;
            if ((y & F->km) != F->kv) continue;
        } else {
            y = F->km ? _pd_kunrank(_pd_rank(a, F->km, F->kv) + k, F->km, F->kv) : a + k;
        }
        if (_is_hole(ctx, x, (int64_t)y)) continue;
        *out = (int64_t)y;
        return 1;
    }
    return 0;
}

/* v allowed by the facts on x (1 when there are none). */
static int _pd_allows(dvs_ctx_t *ctx, uint32_t x, int64_t v) {
    PdFacts F;
    if (_pd_facts(ctx, x, &F) != 1) return 1;
    uint64_t y = (uint64_t)v;
    if ((y & F.km) != F.kv || (F.cm > 1u && y % F.cm != F.cr)) return 0;
    return _pd_count(&F, y, y) > 0.0;
}

/* Draw x in [lo, hi] by its facts: 1 and *out, else 0 (draw as before). */
static int _pd_pick(dvs_ctx_t *ctx, uint32_t x, int64_t lo, int64_t hi, int64_t *out) {
    PdFacts F;
    if (_pd_facts(ctx, x, &F) != 1) return 0;
    return _pd_draw(ctx, x, &F, (uint64_t)lo, (uint64_t)hi, out);
}

/* A value of x in [lo, hi]: by its facts (the pick domain), else uniform
 * avoiding holes. */
static int64_t _pick_in(dvs_ctx_t *ctx, uint32_t x, int64_t lo, int64_t hi) {
    int64_t v;
    if (_pd_pick(ctx, x, lo, hi, &v)) return v;
    v = _rand_range64(ctx, lo, hi);
    return _pick_avoiding_holes(ctx, x, v, lo, hi);
}

enum { KEEP_NONE = 0, KEEP_SET = 1, KEEP_DEAD = 2 };

/* Count-weighted value choice (docs/accuracy_closure_plan.md A3).
 *
 * The search decides one variable at a time. A uniform draw over a
 * variable's values gives each value the same chance however many
 * solutions lie under it, so under `a[0] < ... < a[9]` a high a[0], with
 * few completions, was drawn as often as a low one (mean a[0] 43 against
 * 22.4), and every later decision compounded it.
 *
 * Instead, the domain is cut into at most DVS_WPICK_BUCKETS equal buckets
 * (one per value when it has fewer). Each is tried at a level of its own:
 * bound x to it, propagate, and weigh it by the size of the box that is
 * left, the product of the counted variables' domains. A bucket is drawn
 * by weight, then a value uniformly inside it. The box overcounts, since
 * a domain's values do not all extend to solutions, but by far less than
 * treating every value alike: 16 buckets brought the chain to within
 * noise in simulation. Counted: the problem's own variables, not aux,
 * merged or defined (box_skip) ones, which the others fix.
 *
 * This changes only which value is tried, never what is excluded, so no
 * solution is lost. A probe is the box tier's own try: a level pushed and
 * popped, with no learning. Each solve may spend DVS_WPICK_PROBES bucket
 * probes; past that, and for a dist, a multiply link (_pick_mul_divisor
 * draws those) or a seed-0 solve, the draw is uniform as before.
 * DVS_WPICK=0 turns it off.
 *
 * The probes are most of a small solve's time (usb_pkt: 280 a call, 90%
 * of it), and the same weights are asked for again and again: a decision
 * the search retakes after a conflict, and a site's first decisions on
 * every call. So the weights are kept by search state (ctx->wmemo, see
 * _wmemo_find) and reused, with the probes' firings still counted as work
 * so the work budget falls where it did. */
#define DVS_WPICK_PROBES  1024u

static int _wpick_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DVS_WPICK");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached;
}

/* The size of the box left, the product of the counted variables'
 * domains, as m * 2^(DVS_WPICK_SCALE * e): it overflows a double after
 * a few dozen wide variables. (No libm: log2/exp2 are not in every
 * target's link.) */
#define DVS_WPICK_SCALE 512
typedef struct { double m; int e; } WBox;

static WBox _wpick_box(dvs_ctx_t *ctx) {
    uint32_t n = ctx->n_decl_vars < ctx->n_vars ? ctx->n_decl_vars : ctx->n_vars;
    WBox b = { 1.0, 0 };
    const DvsPickIx *ix = ctx->prop_refs && ctx->n_props && _pd_enabled() ? _pd_index(ctx) : NULL;
    if (ix && !ix->any) ix = NULL;
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[i];
        if ((v->flags & VAR_AUX) || (ctx->var_alias && ctx->var_alias[i] != i)
                || _is_defined(ctx, i)) continue;
        int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
        if (lo == hi) continue;
        /* The values its facts allow (aligned, in its value set), when it
         * has any: what makes a selector's buckets weigh what they leave. */
        PdFacts F;
        double d = (double)_dom_size(ctx, i, lo, hi) + 1.0;
        if (ix && ix->rep[i] != PD_NONE && _pd_has(ix, ix->rep[i])
                && _pd_class(ctx, ix, i, &F, 0) == 1) {
            double c = _pd_count(&F, (uint64_t)lo, (uint64_t)hi);
            if (c < d) d = c;
        }
        b.m *= d;
        if (b.m > 0x1p512) { b.m *= 0x1p-512; b.e++; }
    }
    /* A linear equality with two or more variables open fixes one of them
     * given the rest, so its variables are one dimension fewer than their
     * domains say: `a + b + c == 200` with a decided leaves b and c each
     * [0, 200-a], a box of (201-a)^2 for 201-a solutions. Take out the
     * widest open domain of each such group. */
    for (uint32_t g = 0; g < ctx->n_eq_grp; g++) {
        uint32_t open = 0;
        double widest = 0.0;
        for (uint32_t j = ctx->eq_grp_off[g]; j < ctx->eq_grp_off[g + 1]; j++) {
            uint32_t i = ctx->eq_grp[j];
            if (ctx->var_alias) i = ctx->var_alias[i];
            if (i >= n) continue;
            const Variable *v = &ctx->vars[i];
            if ((v->flags & VAR_AUX) || _is_defined(ctx, i)) continue;
            int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
            if (lo == hi) continue;
            uint32_t k;      /* count a merged variable once */
            for (k = ctx->eq_grp_off[g]; k < j; k++) {
                uint32_t i2 = ctx->eq_grp[k];
                if (ctx->var_alias) i2 = ctx->var_alias[i2];
                if (i2 == i) break;
            }
            if (k < j) continue;
            double d = (double)_dom_size(ctx, i, lo, hi) + 1.0;
            if (d > widest) widest = d;
            open++;
        }
        if (open >= 2) b.m /= widest;
    }
    return b;
}

/* x is an operand of a product of two open variables, or a square's root:
 * those are drawn by _pick_mul_divisor and _mul_count_accept. A product
 * with a fixed operand (`2 * c`) is a scaling, and is weighted. */
static int _wpick_in_product(const dvs_ctx_t *ctx, uint32_t x) {
    if (_tier(ctx, x) == 2) return 1;
    for (uint32_t i = 0; i < ctx->n_mul_links; i++) {
        const DvsMulLink *ml = &ctx->mul_links[i];
        int xa = ml->a == x || dvs_value_src(ctx, ml->a) == x;
        int xb = ml->b == x || dvs_value_src(ctx, ml->b) == x;
        if (!xa && !xb) continue;
        uint32_t o = xa ? ml->b : ml->a;
        if (ml->a == ml->b || var_lo64(ctx, &ctx->vars[o]) != var_hi64(ctx, &ctx->vars[o]))
            return 1;
    }
    return 0;
}

static int _no_learnt(const dvs_ctx_t *ctx) {
    const LCGCtx *L = (const LCGCtx *)ctx->lcg;
    return !L || L->clause_db.n_clauses == 0;
}

/* The weights for deciding x (nb buckets) in the current search state, if
 * kept: the entry, else NULL. *put is then the slot for the probes' result
 * (NULL: no memo), and the state's key is in *k1, *k2.
 *
 * The key hashes every variable's bounds, which with the compiled problem
 * (wmemo_epoch: compile, add_constraint and value exclusion bump it;
 * restore_gen) and the active soft constraints settle what the probes find
 * -- but for the learnt clauses, which can only prune more. Two tables:
 *
 *  - this solve's (slots DVS_WMEMO_KEPT on, direct-mapped; an entry of
 *    another solve is an empty slot). Reused with the clauses learnt since
 *    by rejection (_pick_value_weighted).
 *  - across solves (the first DVS_WMEMO_KEPT slots, two ways a set): only
 *    entries made with no learnt clause, used with none, which are exactly
 *    what the probes would find. A hit is also copied into this solve's
 *    table, as the probes' result would have been.
 *
 * So what a solve draws depends on the problem, the state and the seed, not
 * on what the tables kept from earlier calls: a reused handle draws what a
 * fresh one would (test_evict). In the kept table a new entry replaces the
 * way reused less (each reuse counts, to 3; on a tie the older), and the
 * way kept loses a count: a site's first decisions, asked on every call,
 * outlast the deeper states that mostly come once. Wider than 64 bits the
 * bounds are not all in vars[]: no memo. */
static DvsWMemo *_wmemo_find(dvs_ctx_t *ctx, uint32_t x, uint32_t nb,
                             DvsWMemo **put, uint64_t *k1, uint64_t *k2) {
    *put = NULL;
    if (!ctx->wmemo) {
        ctx->wmemo = (DvsWMemo *)DVS_ALLOC(ctx->alloc, DVS_WMEMO_SLOTS * sizeof(DvsWMemo));
        if (!ctx->wmemo) return NULL;
        memset(ctx->wmemo, 0, DVS_WMEMO_SLOTS * sizeof(DvsWMemo));
    }
    uint64_t a = 0x9E3779B97F4A7C15ULL, b = 0xC2B2AE3D27D4EB4FULL;
#define WMIX(v) do { uint64_t v_ = (uint64_t)(v); \
        a = (a ^ v_) * 0x100000001B3ULL; b = ((b ^ v_) * 0xFF51AFD7ED558CCDULL); b ^= b >> 29; } while (0)
    WMIX(x); WMIX(ctx->wmemo_epoch);
    WMIX(ctx->n_vars); WMIX(ctx->n_props); WMIX(ctx->assumption_active_mask);
    WMIX(ctx->restore_gen);
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *v = &ctx->vars[i];
        if (VAR_IS_TIER2(v->flags)) return NULL;
        WMIX(var_lo64(ctx, v)); WMIX(var_hi64(ctx, v));
    }
#undef WMIX
    *k1 = a | 1u;
    *k2 = b;
    const uint32_t h = (uint32_t)(b >> 32);
    DvsWMemo *s = &ctx->wmemo[DVS_WMEMO_KEPT + h % (DVS_WMEMO_SLOTS - DVS_WMEMO_KEPT)];
    if (s->solve == ctx->wmemo_solve && s->k1 == *k1 && s->k2 == *k2 && s->nb == nb)
        return s;
    *put = s;
    if (!_no_learnt(ctx)) return NULL;
    DvsWMemo *w = &ctx->wmemo[(h % (DVS_WMEMO_KEPT / 2u)) * 2u];
    for (int i = 0; i < 2; i++)
        if (w[i].k1 == *k1 && w[i].k2 == *k2 && w[i].nb == nb) {
            if (w[i].hits < 3) w[i].hits++;
            *s = w[i];
            s->solve = ctx->wmemo_solve;
            return s;
        }
    return NULL;
}

/* Keep the probes' result `m` (exact: made with no learnt clause) across
 * solves too. */
static void _wmemo_keep(dvs_ctx_t *ctx, const DvsWMemo *m) {
    DvsWMemo *w = &ctx->wmemo[((uint32_t)(m->k2 >> 32) % (DVS_WMEMO_KEPT / 2u)) * 2u];
    int v = w[1].hits < w[0].hits || (w[1].hits == w[0].hits && w[1].solve < w[0].solve);
    if (w[!v].hits) w[!v].hits--;
    w[v] = *m;
    w[v].hits = 0;
}

/* One bucket probe: x in [blo, bhi] at a level of its own, propagated. 1 when
 * it holds, with what propagation left of the bucket in *plo, *phi and the
 * box in *w; 0 when it conflicts or propagation gave up (prop_aborted). */
static int _wpick_probe(dvs_ctx_t *ctx, uint32_t x, int64_t blo, int64_t bhi,
                        uint32_t level, int64_t *plo, int64_t *phi, WBox *w) {
    trail_push_level(ctx);
    ctx->conflict_prop_ref   = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
    PropResult pr = ctx_tighten_lb64(ctx, x, blo);
    if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x, bhi);
    if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
    int ok = pr == PROP_OK && !ctx->prop_aborted;
    *plo = blo;
    *phi = bhi;
    if (ok) {
        /* Draw inside what propagation left of the bucket: a bucket
         * across a gap (`x inside {[10:20],[100:150]}`) weighs only
         * its feasible part, so its value must come from that part. */
        *plo = var_lo64(ctx, &ctx->vars[x]);
        *phi = var_hi64(ctx, &ctx->vars[x]);
        *w = _wpick_box(ctx);
    }
    trail_backtrack(ctx, level);
    ctx->conflict_prop_ref   = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
    return ok;
}

/* 1: a value for x is in *out. 0: draw uniformly instead. */
static int _pick_value_weighted(dvs_ctx_t *ctx, uint32_t x, int64_t lo, int64_t hi,
                                int64_t *out) {
    /* Buckets k = 0..nb-1 over the span+1 values, as offsets from lo in
     * unsigned arithmetic (the bit patterns of a signed or upper-half
     * domain still add correctly). span+1 may not fit, so: bucket k
     * starts at k*q + k*(r+1)/nb, with span = q*nb + r. */
    const uint64_t span = (uint64_t)hi - (uint64_t)lo;
    uint32_t nb = span < DVS_WPICK_BUCKETS - 1u ? (uint32_t)span + 1u : DVS_WPICK_BUCKETS;
    /* With facts on x (the pick domain), the buckets split the N values they
     * allow into nb runs of about N/nb values each, so that a short value
     * set (`len inside {1, 2, 4, ..., 64}`) is probed one value at a time:
     * a fixed len fixes `len - 1`, and the box then counts addr's values
     * aligned to it. */
    PdFacts F;
    uint64_t pdn = 0;
    if (_pd_facts(ctx, x, &F) == 1) {
        if (!_pd_total(&F, (uint64_t)lo, (uint64_t)hi, &pdn)) pdn = 0;
        else if (pdn < 2) return 0;
        else nb = pdn < DVS_WPICK_BUCKETS ? (uint32_t)pdn : DVS_WPICK_BUCKETS;
    }
    if (nb < 2 || ctx->wpick_left < nb || ctx->decision_level + 1 >= ctx->max_depth)
        return 0;
    /* `solve x before y` asks for x uniform over its feasible values
     * (IEEE 1800 18.5.10), not by solution count: an ordered variable is
     * drawn as before. The unordered ones, decided last, are weighted
     * given the ordered ones. */
    if (_order_key(ctx, x) != DVS_ORDER_LAST) return 0;
    /* With no other counted variable open, every bucket's weight is just
     * its feasible size: the uniform draw (and redraw on a conflict) is
     * already exact, and cheaper. */
    {
        uint32_t nd = ctx->n_decl_vars < ctx->n_vars ? ctx->n_decl_vars : ctx->n_vars, i;
        for (i = 0; i < nd; i++) {
            const Variable *v = &ctx->vars[i];
            if (i == x || (v->flags & VAR_AUX) || (ctx->var_alias && ctx->var_alias[i] != i)
                    || _is_defined(ctx, i)) continue;
            if (var_lo64(ctx, v) != var_hi64(ctx, v)) break;
        }
        if (i == nd) return 0;
    }
    ctx->wpick_left -= nb;
    int64_t blo[DVS_WPICK_BUCKETS], bhi[DVS_WPICK_BUCKETS];
    double wt[DVS_WPICK_BUCKETS], tot = 0.0;
    const uint32_t level = ctx->decision_level;
    uint64_t k1 = 0, k2 = 0;
    DvsWMemo *put, *m = _wmemo_find(ctx, x, nb, &put, &k1, &k2);
    if (m && m->exact && _no_learnt(ctx)) {
        /* Exactly what the probes would find: draw as they would have. */
        memcpy(blo, m->blo, nb * sizeof *blo);
        memcpy(bhi, m->bhi, nb * sizeof *bhi);
        memcpy(wt, m->wt, nb * sizeof *wt);
        for (uint32_t k = 0; k < nb; k++) tot += wt[k];
        ctx->prop_fires += m->fires;
        ctx->conflict_prop_ref   = EXPR_NULL;
        ctx->conflict_clause_idx = EXPR_NULL;
        goto draw;
    }
    if (m) {
        /* Made earlier in this solve: the clauses learnt since can only have
         * pruned, so each weight is an upper bound of the bucket's weight
         * now. Rejection sampling draws by the weights now with one probe a
         * try: draw a bucket by the kept weights, probe it, keep it with
         * probability now/kept, else store what the probe found and draw
         * again. A rejected bucket's weight is then current, and it is kept
         * if drawn again, so this ends within nb + 1 tries. A weight that
         * grew (the equality-group correction is not monotone) means a full
         * probe. */
        m->exact = 0;
        for (;;) {
            tot = 0.0;
            for (uint32_t k = 0; k < nb; k++) tot += m->wt[k];
            if (tot == 0.0) return 0;
            double u = (double)(_rand64(ctx) >> 11) * 0x1p-53 * tot;
            uint32_t k = 0;
            while (k + 1 < nb && u >= m->wt[k]) { u -= m->wt[k]; k++; }
            while (m->wt[k] == 0.0) k--;
            int64_t plo, phi;
            WBox b;
            int ok = _wpick_probe(ctx, x, m->blo[k], m->bhi[k], level, &plo, &phi, &b);
            if (ctx->prop_aborted) return 0;
            double now = 0.0;
            if (ok) {
                if (b.e > m->emax) goto probe_all;
                now = b.m;
                for (int d = b.e; d < m->emax && now > 0.0; d++) now *= 0x1p-512;
                if (now > m->wt[k]) goto probe_all;
                m->blo[k] = plo;
                m->bhi[k] = phi;
            }
            int keep = ok && (double)(_rand64(ctx) >> 11) * 0x1p-53 * m->wt[k] < now;
            m->wt[k] = now;
            if (keep) {
                *out = _pick_in(ctx, x, plo, phi);
                return 1;
            }
        }
    }
probe_all:;
    if (m) put = m;
    const uint64_t fires0 = ctx->prop_fires;
    const uint64_t q = span / nb, r1 = span % nb + 1u;
    WBox w[DVS_WPICK_BUCKETS];
    int ok[DVS_WPICK_BUCKETS], emax = 0, any = 0;
    for (uint32_t k = 0; k < nb; k++) {
        uint64_t o0 = k * q + (k * r1) / nb;
        uint64_t o1 = k + 1 == nb ? span : (k + 1) * q + ((k + 1) * r1) / nb - 1u;
        int64_t b0 = (int64_t)((uint64_t)lo + o0), b1 = (int64_t)((uint64_t)lo + o1);
        if (pdn) {
            /* Values of rank pdn*k/nb .. pdn*(k+1)/nb - 1 among those allowed. */
            uint64_t r0 = (uint64_t)(((unsigned __int128)pdn * k) / nb);
            uint64_t r1_ = (uint64_t)(((unsigned __int128)pdn * (k + 1)) / nb) - 1u;
            b0 = (int64_t)_pd_nth(&F, (uint64_t)lo, (uint64_t)hi, r0);
            b1 = (int64_t)_pd_nth(&F, (uint64_t)lo, (uint64_t)hi, r1_);
        }
        ok[k] = _wpick_probe(ctx, x, b0, b1, level, &blo[k], &bhi[k], &w[k]);
        if (ok[k]) {
            if (!any || w[k].e > emax) emax = w[k].e;
            any = 1;
        }
        if (ctx->prop_aborted) break;
    }
    if (ctx->prop_aborted || !any) return 0;
    for (uint32_t k = 0; k < nb; k++) {
        wt[k] = 0.0;
        if (!ok[k]) continue;
        wt[k] = w[k].m;
        for (int d = w[k].e; d < emax && wt[k] > 0.0; d++) wt[k] *= 0x1p-512;
        tot += wt[k];
    }
    if (put) {
        m = put;
        m->k1 = k1; m->k2 = k2; m->nb = (uint8_t)nb; m->hits = 0;
        m->solve = ctx->wmemo_solve;
        m->exact = (uint8_t)_no_learnt(ctx);
        m->emax = emax;
        m->fires = ctx->prop_fires - fires0;
        memcpy(m->blo, blo, nb * sizeof *blo);
        memcpy(m->bhi, bhi, nb * sizeof *bhi);
        memcpy(m->wt, wt, nb * sizeof *wt);
        if (m->exact) _wmemo_keep(ctx, m);
    }
draw:;
    if (tot == 0.0) return 0;          /* no bucket has a value its facts allow */
    double u = (double)(_rand64(ctx) >> 11) * 0x1p-53 * tot;
    uint32_t k = 0;
    while (k + 1 < nb && u >= wt[k]) { u -= wt[k]; k++; }
    while (wt[k] == 0.0) k--;          /* rounding ran past the last weight */
    *out = _pick_in(ctx, x, blo[k], bhi[k]);
    return 1;
}

/* A seed-0 solve's saved phases start at each var's lower bound. With
 * DV_PHASE_INIT=none they start unset: a var's first decision takes the
 * seeded pick (_pick_value's hole-aware path), and phase saving repeats only
 * values that propagated. `none` solves `c = a >> k, c != a` at once, where
 * lower bounds first decide k = 0 (so c = a, refuted one value of a at a
 * time until the first restart, 2.3 s); but on pure CDCL it is slower on
 * the tier 1-3 cross-check (45 s -> 54 s) and stalls t_constraint_operators,
 * so it is opt-in (docs/solver_gap_plan.md, 2026-11-05). */
static int _phase_init_lo(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DV_PHASE_INIT");
        cached = (e && strcmp(e, "none") == 0) ? 0 : 1;
    }
    return cached;
}

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
    if (opts && opts->use_phase_save && opts->seed == 0 && ctx->phase_save
            && ctx->phase_set[var_id]) {
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
                && !_is_hole(ctx, var_id, kv) && _pd_allows(ctx, var_id, kv))
            return kv;
    }

    int64_t v;
    /* Distribution-weighted selection if this variable has a dist constraint */
    if (ctx->dist_offsets && var_id < ctx->n_vars_capacity &&
        ctx->dist_offsets[var_id] != 0) {
        v = _pick_value_dist(ctx, var_id, lo, hi);
    } else if (opts && opts->seed != 0 && ctx->fair_pick && !opts->seq_pick
               && !_wpick_in_product(ctx, var_id) && !(ctx->vars[var_id].flags & VAR_AUX)
               && _wpick_enabled() && _pick_value_weighted(ctx, var_id, lo, hi, &v)) {
        /* drawn by weight, holes already avoided */
    } else if (!_pd_pick(ctx, var_id, lo, hi, &v)) {
        v = _rand_range64(ctx, lo, hi);
        v = _pick_mod_aligned(ctx, var_id, v, lo, hi);
        v = _pick_mul_divisor(ctx, var_id, v, lo, hi);
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

#define DVS_BOUND_DECISION_MIN 64u
#define DVS_BOUND_DECISION_TRIES 32u

/* A plain value decision `x = v` has failed, and v is interior to x's
 * domain. Should the search draw again instead of splitting around v?
 *
 * Excluding an interior value takes a split, [lo, v-1] or [v+1, hi], chosen
 * by size (_split_upper_first), and the search then draws inside that half.
 * That hands v's share to its half's values, and the values next to a run of
 * infeasible ones collect the run's share: with s's feasible values
 * {3..127} and {-128..-126}, the three negative ones got 8% of the draws
 * instead of 2.3% (pyvsc F5). Undoing the decision and drawing again from the
 * whole domain is rejection sampling: each feasible value of x stays equally
 * likely. v is not excluded, so it may be drawn again; that costs time, not
 * uniformity. A seeded fair_pick solve does this up to
 * DVS_BOUND_DECISION_TRIES failed value decisions per variable (the count
 * _bound_decision uses), then splits, so a sparse domain still converges.
 * `counted`: this failure is already in value_fails (the decision's own
 * propagation conflicted); a failure found deeper in the search is not. */
static int _redraw(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                   uint32_t x, int counted) {
    if (!(opts && opts->seed != 0 && ctx->fair_pick)) return 0;
    if (!ctx->value_fails_on || x >= ctx->value_fails_cap) return 0;
    if (!counted && ctx->value_fails[x] < 255) ctx->value_fails[x]++;
    if (ctx->value_fails[x] > DVS_BOUND_DECISION_TRIES) return 0;
    if (ctx->keep_on && x < ctx->keep_cap) ctx->keep_state[x] = KEEP_DEAD;
    return 1;
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
 *
 * A seeded solve makes value decisions on a variable until
 * DVS_BOUND_DECISION_TRIES of them have failed in this solve. A failed value
 * decision teaches `x != v` and the next draw is uniform over what is left --
 * rejection sampling, which keeps the result uniform. A failed bound decision
 * drops a side chosen by the weight of its interval, not of its solutions, so
 * bounding from the first decision skewed every gappy domain; and activity
 * outlives a reset, so in a randomize() loop nearly every variable had some
 * (`x inside {[10:20],[100:150]}`, `ot_aon_wkup` under Verilator's protocol:
 * chi-square p < 0.001). Sparse domains, where draws keep failing, still
 * switch to bounds after a few tries.
 * Returns 0 (value decision), 1 (x <= v) or 2 (x >= v). */
static uint8_t _bound_decision(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                               uint32_t x, int64_t v) {
    const LCGCtx *L = (const LCGCtx *)ctx->lcg;
    if (!L || !L->enabled || x >= L->vsids.n_vars || !(L->vsids.activity[x] > 0.0))
        return 0;
    if (ctx->value_fails_on && x < ctx->value_fails_cap
            && ctx->value_fails[x] < DVS_BOUND_DECISION_TRIES)
        return 0;
    const Variable *xv = &ctx->vars[x];
    int64_t lo = var_lo64(ctx, xv), hi = var_hi64(ctx, xv);
    if ((uint64_t)hi - (uint64_t)lo <= DVS_BOUND_DECISION_MIN) return 0;
    if (v == lo) return 1;              /* x <= lo: the side that narrows */
    if (v == hi) return 2;
    return _split_upper_first(ctx, opts, lo, v, hi) ? 2 : 1;
}

/* The number of solutions left once a square's root is decided, when what is
 * left is one fixed product: r == a * b with r fixed to K and a, b free, and
 * every other free variable a copy of a or of b. Each divisor d of K in a's
 * domain whose cofactor K / d is in b's is one solution. 1 when nothing is
 * left free. 0 when anything else is left -- a second product, another
 * variable, a propagator over a free variable other than the product, the
 * copies and the cycle detector (which prunes nothing of its own) -- since
 * the count would then not be the number of solutions. (Learned clauses
 * follow from the problem, so they remove none.) */
static uint64_t _mul_leftover_count(dvs_ctx_t *ctx) {
    const DvsMulLink *ml = NULL;
    for (uint32_t i = 0; i < ctx->n_mul_links; i++) {
        const DvsMulLink *m = &ctx->mul_links[i];
        if (m->a == m->b) continue;
        const Variable *rv = &ctx->vars[m->r];
        const Variable *av = &ctx->vars[m->a], *bv = &ctx->vars[m->b];
        if (var_lo64(ctx, rv) != var_hi64(ctx, rv)) continue;
        if (var_lo64(ctx, av) == var_hi64(ctx, av) || var_lo64(ctx, bv) == var_hi64(ctx, bv))
            continue;
        if (ml) return 0;                                   /* two products */
        ml = m;
    }
    if (!ml) {
        for (uint32_t i = 0; i < ctx->n_vars; i++) {
            if (ctx->var_alias && ctx->var_alias[i] != i) continue;   /* reads its root */
            if (var_lo64(ctx, &ctx->vars[i]) != var_hi64(ctx, &ctx->vars[i])) return 0;
        }
        return 1;
    }
    const uint32_t sa = dvs_value_src(ctx, ml->a), sb = dvs_value_src(ctx, ml->b);
    if (sa == sb) return 0;
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *v = &ctx->vars[i];
        if (ctx->var_alias && ctx->var_alias[i] != i) continue;
        if (var_lo64(ctx, v) == var_hi64(ctx, v)) continue;
        uint32_t s = dvs_value_src(ctx, i);
        if (s != sa && s != sb) return 0;
    }
    for (uint32_t p = 0; p < ctx->n_props; p++) {
        if (p == ml->prop) continue;
        const Propagator *pp = (const Propagator *)dvs_spool_ptr(&ctx->pool, ctx->prop_refs[p]);
        uint32_t n;
        const uint32_t *w = prop_watched_vars(pp, &n);
        for (uint32_t k = 0; k < n; k++) {
            const Variable *v = &ctx->vars[w[k]];
            if (var_lo64(ctx, v) != var_hi64(ctx, v)) {
                if (!prop_is_bounds_eq(pp) && !prop_is_diffcycle(pp)) return 0;
                break;
            }
        }
    }
    const Variable *rv = &ctx->vars[ml->r];
    const Variable *av = &ctx->vars[ml->a], *bv = &ctx->vars[ml->b];
    if ((rv->flags | av->flags | bv->flags) & VAR_SIGNED) return 0;
    uint64_t K = (uint64_t)var_lo64(ctx, rv);
    uint64_t alo = (uint64_t)var_lo64(ctx, av), ahi = (uint64_t)var_hi64(ctx, av);
    uint64_t blo = (uint64_t)var_lo64(ctx, bv), bhi = (uint64_t)var_hi64(ctx, bv);
    uint64_t mask = ml->width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << ml->width) - 1);
    if (K == 0 || bhi == 0 || (ahi != 0 && bhi > mask / ahi)) return 0;    /* may wrap */
    uint64_t L = K / bhi + (K % bhi != 0), U = blo ? K / blo : ahi;
    if (L < alo) L = alo;
    if (U > ahi) U = ahi;
    if (L > U || !dvs_divisors(&ctx->divisors, K, 1u << 17)) return 0;
    uint32_t j0 = dvs_divisors_lower(&ctx->divisors, L);
    uint32_t j1 = dvs_divisors_upper(&ctx->divisors, U);
    uint64_t n = 0;
    for (uint32_t j = j0; j < j1; j++) {
        int64_t d = (int64_t)ctx->divisors.divs[j], e = (int64_t)(K / (uint64_t)d);
        if (!_is_hole(ctx, ml->a, d) && !_is_hole(ctx, sa, d)
                && !_is_hole(ctx, ml->b, e) && !_is_hole(ctx, sb, e))
            n++;
    }
    return n;
}

#define DVS_MUL_COUNT_PROBES 64u
#define DVS_MUL_COUNT_TRIES  4096u

/* The largest leftover count over DVS_MUL_COUNT_PROBES draws of x, each
 * decided and propagated at a level of its own above `level` and undone. */
static void _mul_count_probe(dvs_ctx_t *ctx, uint32_t x, uint32_t level) {
    const Variable *xv = &ctx->vars[x];
    const int64_t lo = var_lo64(ctx, xv), hi = var_hi64(ctx, xv);
    for (uint32_t k = 0; k < DVS_MUL_COUNT_PROBES; k++) {
        int64_t v = _rand_range64(ctx, lo, hi);
        if (_is_hole(ctx, x, v)) continue;
        trail_push_level(ctx);
        ctx->conflict_prop_ref   = EXPR_NULL;
        ctx->conflict_clause_idx = EXPR_NULL;
        PropResult pr = ctx_tighten_lb64(ctx, x, v);
        if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x, v);
        if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
        if (pr == PROP_OK) {
            uint64_t n = _mul_leftover_count(ctx);
            if (n > ctx->mul_count_max) ctx->mul_count_max = n;
        }
        trail_backtrack(ctx, level);
        if (ctx->prop_aborted) break;
    }
    ctx->conflict_prop_ref   = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
}

/* Keep the value just decided for x, the root of a square (decision tier 2),
 * or reject it?
 *
 * Deciding the root first fixes the product (_build_dec_tier), and then each
 * divisor pair of the product is equally likely (_pick_mul_divisor). But the
 * roots are drawn uniformly while the number of pairs differs from root to
 * root, 1 to 64 for `a * b == c * c + 345` over u8, so a solution's
 * probability was 1 / (its root's pair count): chi^2/df 6.9 over the 180
 * solutions. When what is left after the decision is that one product
 * (_mul_leftover_count), the value is kept with probability n / n_max, and
 * otherwise undone -- excluding nothing -- and drawn again: rejection
 * sampling, uniform over the solutions.
 *
 * n_max is not known up front; it is the largest count seen, kept across
 * solves of the problem, and the first time the hook runs it probes
 * DVS_MUL_COUNT_PROBES draws of x for it. A count above it raises it (the
 * value is then kept), so a root whose count is above every one seen so far
 * is under-drawn until it is seen; past that the draws are exactly uniform.
 * After DVS_MUL_COUNT_TRIES rejections in one solve the value is kept.
 * Returns 1 to keep the value; 0 when the decision was undone (the search is
 * back at `level`). */
static int _mul_count_accept(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                             uint32_t x, uint32_t level, PropResult pr,
                             uint32_t *rejects) {
    if (!(opts && opts->seed != 0 && ctx->fair_pick)) return 1;
    if (*rejects >= DVS_MUL_COUNT_TRIES) return 1;
    if (pr == PROP_CONFLICT) {
        /* A root with no solution at all: rejected like any other, so it
         * is not one of the failed draws after which the search splits
         * x's domain (_redraw), which favours the values next to a run of
         * infeasible ones (c = 219 of `a * b == c * c + 345`, above 12
         * infeasible roots, was drawn 1.3x as often). */
        trail_backtrack(ctx, level);
        ctx->conflict_prop_ref   = EXPR_NULL;
        ctx->conflict_clause_idx = EXPR_NULL;
        if (ctx->keep_on && x < ctx->keep_cap) ctx->keep_state[x] = KEEP_DEAD;
        ++*rejects;
        return 0;
    }
    uint64_t n = _mul_leftover_count(ctx);
    if (n == 0) return 1;
    if (n > ctx->mul_count_max) ctx->mul_count_max = n;
    if (ctx->mul_count_probed && n == ctx->mul_count_max) return 1;
    if (ctx->mul_count_probed && _rand64(ctx) % ctx->mul_count_max < n) return 1;
    trail_backtrack(ctx, level);
    if (!ctx->mul_count_probed) {
        /* Undone whatever its value was, so this rejection is fair too. */
        ctx->mul_count_probed = 1;
        _mul_count_probe(ctx, x, level);
    }
    if (ctx->keep_on && x < ctx->keep_cap) ctx->keep_state[x] = KEEP_DEAD;
    ++*rejects;
    return 0;
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
    case DVS_BAIL_THRASH:        return "work cap after an earlier solve of this problem gave up";
    case DVS_BAIL_WORK:          return "work budget for this solve";
    default:                     return "no reason recorded";
    }
}

/* ------------------------------------------------------------------ */
/* Box-rejection tier                                                  */
/*                                                                     */
/* The search draws each decision uniformly from the variable's domain */
/* as propagated so far. That is sequential uniform sampling, which is */
/* not uniform over solutions when variables are related: under        */
/* `x <= y` on [0:9], (0,0) and (9,9) come up 5.5% of the time instead */
/* of 1.8%. Instead, draw every decision variable at once from the     */
/* domains the root propagation left, pin them all, propagate, and     */
/* keep the result only if nothing conflicts. Root propagation removes */
/* no solution, so each solution lies in the box exactly once and an   */
/* accepted draw is uniform over solutions.                            */
/*                                                                     */
/* Up to DVS_BOX_TRIES draws per solve; when they all fail, the        */
/* ordinary search runs. The first solve of a problem probes it:       */
/* DVS_BOX_PROBE draws from a fixed seed, and a problem that accepts   */
/* fewer than one in DVS_BOX_MIN_RATE never uses the tier, so tight    */
/* problems (whose solutions are a small part of their box) keep their */
/* speed. The probe's seed is fixed, so whether the tier runs depends  */
/* on the problem only: a reused context draws the same values as a    */
/* fresh one. Not used when a variable has a dist (its draw is not     */
/* uniform), for a deterministic (seed 0) solve, or with seq_pick.     */
/* Only the problem's own variables are drawn (compile adds more,      */
/* which the pins then fix). DVS_BOX=0 turns the tier off; DVS_BOX=2   */
/* skips the probe and always tries it.                                */
/* Returns 1 when a draw was accepted (the solve is done), 0 to search. */
/* ------------------------------------------------------------------ */
#define DVS_BOX_TRIES     32u
/* More draws for a solve the first DVS_BOX_TRIES missed and the enumeration
 * tier could not take: a rejection draw is uniform however many tries it
 * took, so only giving up is biased. Searching instead decides one variable
 * at a time, uniformly over its values, which over-weights a value with few
 * solutions under it (`cond_weight`, docs/accuracy_closure_plan.md A1): at
 * the 1-in-16 rate the probe admits, 32 tries miss 13% of the time, and
 * 32 + 224 miss 7e-8 of the time. */
#define DVS_BOX_RETRIES  224u
#define DVS_BOX_PROBE     64u
#define DVS_BOX_MIN_RATE  16u
enum { BOX_UNPROBED = 0, BOX_ON = 1, BOX_OFF = 2 };

static int _box_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DVS_BOX");
        cached = (e && e[0] == '0') ? 0 : (e && e[0] == '2') ? 2 : 1;
    }
    return cached;   /* 0 off, 1 on where the probe allows, 2 on always */
}

/* One draw: pin every decision variable to a value drawn from the root
 * domains, then propagate. 1: a complete solution, left on the trail.
 * 0: rejected (trail back at `base`). -1: stop trying (propagation was
 * aborted, or the pins left an aux variable open). */
static int _box_try(dvs_ctx_t *ctx, uint32_t base) {
    if (ctx->decision_level + 1 >= ctx->max_depth) return -1;
    uint32_t n = ctx->n_decl_vars < ctx->n_vars ? ctx->n_decl_vars : ctx->n_vars;
    /* Draw every value before pinning any: a pin followed by propagation
     * would narrow the next draw's domain, which is the sequential sampling
     * this tier replaces. */
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[i];
        int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
        if ((v->flags & VAR_AUX) || lo == hi
                || (ctx->var_alias && ctx->var_alias[i] != i)
                || (ctx->box_skip && ctx->box_skip[i])) continue;
        int64_t x = _rand_range64(ctx, lo, hi);
        if (_is_hole(ctx, i, x)) return 0;
        ctx->box_val[i] = x;
    }
    trail_push_level(ctx);
    ctx->conflict_prop_ref   = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
    PropResult pr = PROP_OK;
    for (uint32_t i = 0; i < n && pr == PROP_OK; i++) {
        const Variable *v = &ctx->vars[i];
        if ((v->flags & VAR_AUX)
                || (ctx->var_alias && ctx->var_alias[i] != i)
                || (ctx->box_skip && ctx->box_skip[i])) continue;
        if (var_lo64(ctx, v) == var_hi64(ctx, v)) continue;
        pr = ctx_tighten_lb64(ctx, i, ctx->box_val[i]);
        if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, i, ctx->box_val[i]);
    }
    if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
    if (ctx->prop_aborted) { trail_backtrack(ctx, base); return -1; }
    if (pr != PROP_OK) { trail_backtrack(ctx, base); return 0; }
    /* Accept only a complete assignment: an aux variable the pins did not
     * fix would need a search decision, and this level has no decision
     * record for the search to undo it by. */
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *v = &ctx->vars[i];
        if (ctx->var_alias && ctx->var_alias[i] != i) continue;
        if (var_lo64(ctx, v) != var_hi64(ctx, v)) {
            trail_backtrack(ctx, base);
            return -1;
        }
    }
    return 1;
}

/* DVS_BOX_STATS=1: at exit, print what the tier did in this process. */
static uint64_t _bx_calls, _bx_skip, _bx_probe_on, _bx_probe_off, _bx_acc, _bx_fall;
static void _bx_report(void) {
    fprintf(stderr, "[box] calls=%llu skipped=%llu probes_on=%llu probes_off=%llu "
            "accepted=%llu fell_through=%llu\n",
            (unsigned long long)_bx_calls, (unsigned long long)_bx_skip,
            (unsigned long long)_bx_probe_on, (unsigned long long)_bx_probe_off,
            (unsigned long long)_bx_acc, (unsigned long long)_bx_fall);
}
static int _box_sample_inner(dvs_ctx_t *ctx, uint32_t base, int last_stage,
                             uint32_t tries);
static int _box_sample(dvs_ctx_t *ctx, uint32_t base, int last_stage, uint32_t tries) {
    static int stats = -1;
    if (stats < 0) {
        const char *e = getenv("DVS_BOX_STATS");
        stats = (e && e[0] == '1');
        if (stats) atexit(_bx_report);
    }
    uint8_t st0 = ctx->box_state;
    int r = _box_sample_inner(ctx, base, last_stage, tries);
    if (stats) {
        _bx_calls++;
        if (st0 == BOX_UNPROBED && ctx->box_state == BOX_ON) _bx_probe_on++;
        if (st0 == BOX_UNPROBED && ctx->box_state == BOX_OFF) _bx_probe_off++;
        if (r) _bx_acc++;
        else if (ctx->box_state == BOX_ON) _bx_fall++;
        else _bx_skip++;
    }
    return r;
}

/* `last_stage`: called mid-search, once a solve order's stages are decided,
 * for the unordered variables (see _solver_solve_core). It never probes:
 * the probe runs at the root, where it depends on the problem only.
 * `tries`: draws to make before giving up. */
static int _box_sample_inner(dvs_ctx_t *ctx, uint32_t base, int last_stage,
                             uint32_t tries) {
    int mode = _box_mode();
    if (mode == 0) return 0;
    if (mode == 1 && ctx->box_state == BOX_OFF) return 0;
    uint32_t n = ctx->n_vars;
    if (n == 0) return 0;
    if (ctx->box_cap < n) {
        int64_t *bv = (int64_t *)dvs_ctx_resize(ctx, ctx->box_val,
                                                ctx->box_cap * sizeof(int64_t),
                                                n * sizeof(int64_t));
        if (!bv) return 0;
        ctx->box_val = bv;
        ctx->box_cap = n;
    }
    uint32_t n_draw = 0;
    uint32_t nd = ctx->n_decl_vars < n ? ctx->n_decl_vars : n;
    for (uint32_t i = 0; i < nd; i++) {
        const Variable *v = &ctx->vars[i];
        if (v->flags & VAR_AUX) continue;
        if (ctx->var_alias && ctx->var_alias[i] != i) continue;
        if (var_lo64(ctx, v) == var_hi64(ctx, v)) continue;
        if (ctx->box_skip && ctx->box_skip[i]) continue;
        if (ctx->dist_offsets && i < ctx->n_vars_capacity
                && ctx->dist_offsets[i] != 0)
            return 0;
        n_draw++;
    }
    if (n_draw < 2) return 0;   /* one variable: the search is already uniform */

    if (mode == 1 && ctx->box_state == BOX_UNPROBED) {
        if (last_stage) return 0;
        uint64_t saved = ctx->rng_state;
        ctx->rng_state = _seed_mix64(0x626f78ULL);
        uint32_t acc = 0, t;
        int r = 0;
        /* Stop as soon as the verdict is settled: enough accepts, or too
         * few tries left to reach them. */
        const uint32_t need = DVS_BOX_PROBE / DVS_BOX_MIN_RATE;
        for (t = 0; t < DVS_BOX_PROBE && acc < need
                    && acc + (DVS_BOX_PROBE - t) >= need; t++) {
            r = _box_try(ctx, base);
            if (r < 0) break;
            if (r == 1) { acc++; trail_backtrack(ctx, base); }
        }
        ctx->rng_state = saved;
        ctx->box_probe_acc = (uint8_t)acc;
        ctx->box_probe_n   = (uint8_t)t;
        ctx->box_state = (r >= 0 && acc >= need)
                         ? BOX_ON : BOX_OFF;
        if (ctx->box_state == BOX_OFF) return 0;
    }
    /* At the root of an ordered problem, only probe: the earlier stages
     * are chosen first, by the search. */
    if (ctx->var_order && !last_stage) return 0;

    for (uint32_t t = 0; t < tries; t++) {
        int r = _box_try(ctx, base);
        if (r == 1) return 1;
        if (r < 0) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Enumeration tier: uniform over the solutions of a small problem.   */
/*                                                                     */
/* The box tier draws from the root box and accepts a draw that is a  */
/* solution; it is off where solutions are a small part of the box   */
/* (below 1 in 16): `lo <= a * b <= hi`, `a * b == c * d`. There the  */
/* search decided one variable at a time, uniformly over the values   */
/* left, so a solution's probability went as 1 / the solutions next  */
/* to it: in `1000 <= a * b <= 1100` (u8), pairs with a small operand */
/* were drawn 1.4x as often as balanced ones.                         */
/*                                                                     */
/* This tier lists every solution once, by a depth-first walk that    */
/* pins each drawn variable (the box tier's: not aux, not defined by  */
/* an equality, not merged, not fixed) to each value its domain still */
/* holds and propagates, so its cost follows the solutions and the    */
/* dead ends propagation finds, not the box. A solve then pins one    */
/* listed solution, drawn uniformly. Exact, since propagation removes */
/* no solution.                                                       */
/*                                                                     */
/* The walk stops (and the tier is off for the context) past          */
/* DVS_ENUM_NODES pins, DVS_ENUM_FIRES propagator firings or         */
/* DVS_ENUM_MAX listed values, when a leaf                            */
/* leaves a variable open, or when a drawn variable spans 2^32 values */
/* or more. The list holds for the root state it was made at, hashed  */
/* in enum_key; a later root that differs (pins, a soft relaxed, an   */
/* added constraint) lists again, DVS_ENUM_REDO times at most. A walk */
/* that finds no solution has refuted every value: UNSAT.             */
/* DVS_ENUM=0 turns the tier off; DVS_ENUM_DEBUG=1 reports each walk.  */
/* ------------------------------------------------------------------ */
#define DVS_ENUM_NODES  (1u << 14)
#define DVS_ENUM_FIRES  (1ULL << 17)
#define DVS_ENUM_MAX    (1u << 18)
#define DVS_ENUM_REDO   4u
#define DVS_ENUM_TRIES  256u   /* draws from a covering list before a walk */
enum { ENUM_UNTRIED = 0, ENUM_ON = 1, ENUM_OFF = 2 };

static int _enum_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DVS_ENUM");
        cached = !(e && e[0] == '0');
    }
    return cached;
}

/* DVS_ENUM_REUSE=0: walk again on every new root (see _enum_sample). */
static int _enum_reuse_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DVS_ENUM_REUSE");
        cached = !(e && e[0] == '0');
    }
    return cached;
}

static uint64_t _enum_key(const dvs_ctx_t *ctx, uint32_t base) {
    uint64_t h = 0xcbf29ce484222325ULL;
#define ENUM_MIX(x) (h = (h ^ (uint64_t)(x)) * 0x100000001b3ULL)
    ENUM_MIX(ctx->n_vars);
    ENUM_MIX(ctx->n_props);
    ENUM_MIX(base);
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *v = &ctx->vars[i];
        int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
        ENUM_MIX(lo);
        ENUM_MIX(hi);
        ENUM_MIX(_count_holes_in_range(ctx, i, lo, hi));
    }
#undef ENUM_MIX
    return h;
}

/* Whether every variable is fixed (a complete assignment). */
static int _all_fixed(const dvs_ctx_t *ctx) {
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        if (ctx->var_alias && ctx->var_alias[i] != i) continue;
        const Variable *v = &ctx->vars[i];
        if (var_lo64(ctx, v) != var_hi64(ctx, v)) return 0;
    }
    return 1;
}

/* Pin var `id` to `x` and propagate, at a new level. PROP_OK or not. */
static PropResult _enum_pin(dvs_ctx_t *ctx, uint32_t id, int64_t x) {
    trail_push_level(ctx);
    ctx->conflict_prop_ref   = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
    PropResult pr = ctx_tighten_lb64(ctx, id, x);
    if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, id, x);
    if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
    return pr;
}

/* List the solutions at the root `base`. 1: listed (possibly none). 0: the
 * tier does not suit this problem. The trail is back at `base` either way. */
static int _enum_build(dvs_ctx_t *ctx, uint32_t base) {
    ctx->enum_nsol = 0;
    uint32_t n = ctx->n_decl_vars < ctx->n_vars ? ctx->n_decl_vars : ctx->n_vars;
    uint32_t nv = 0;
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[i];
        if ((v->flags & VAR_AUX) || (ctx->var_alias && ctx->var_alias[i] != i)
                || (ctx->box_skip && i < ctx->box_skip_n && ctx->box_skip[i]))
            continue;
        int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
        if (lo == hi) continue;
        if ((uint64_t)hi - (uint64_t)lo >= (1ULL << 32)) return 0;
        if (ctx->dist_offsets && i < ctx->n_vars_capacity && ctx->dist_offsets[i])
            return 0;
        nv++;
    }
    if (nv < 2 || base + 2 * nv + 1 >= ctx->max_depth) return 0;
    /* The box probe's accepts estimate the solutions: box size times the
     * accept rate. Far more than the list holds, and the walk is not tried
     * (`sum(list) == K` over ten elements). */
    if (ctx->box_probe_acc && ctx->box_probe_n) {
        double est = (double)ctx->box_probe_acc / ctx->box_probe_n;
        for (uint32_t i = 0; i < n; i++) {
            const Variable *v = &ctx->vars[i];
            if ((v->flags & VAR_AUX) || (ctx->var_alias && ctx->var_alias[i] != i)
                    || (ctx->box_skip && i < ctx->box_skip_n && ctx->box_skip[i]))
                continue;
            est *= (double)((uint64_t)var_hi64(ctx, v) - (uint64_t)var_lo64(ctx, v)) + 1.0;
        }
        if (est > 4.0 * (double)DVS_ENUM_MAX / nv) return 0;
    }

    if (ctx->enum_nv != nv) {
        uint32_t *ev = (uint32_t *)dvs_ctx_resize(ctx, ctx->enum_vars,
                                                  (size_t)ctx->enum_nv * sizeof(uint32_t),
                                                  (size_t)nv * sizeof(uint32_t));
        if (!ev) return 0;
        ctx->enum_vars = ev;
        ctx->enum_nv = nv;
    }
    /* Smallest domain first: fewer, shallower dead ends. */
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[i];
        if ((v->flags & VAR_AUX) || (ctx->var_alias && ctx->var_alias[i] != i)
                || (ctx->box_skip && i < ctx->box_skip_n && ctx->box_skip[i])
                || var_lo64(ctx, v) == var_hi64(ctx, v))
            continue;
        uint64_t span = (uint64_t)var_hi64(ctx, v) - (uint64_t)var_lo64(ctx, v);
        uint32_t j = k++;
        while (j > 0) {
            const Variable *u = &ctx->vars[ctx->enum_vars[j - 1]];
            if ((uint64_t)var_hi64(ctx, u) - (uint64_t)var_lo64(ctx, u) <= span) break;
            ctx->enum_vars[j] = ctx->enum_vars[j - 1];
            j--;
        }
        ctx->enum_vars[j] = i;
    }

    /* Per depth: a scan level, under the value pins, where the variable's
     * lower bound is raised past each value tried, so propagation skips the
     * values that cannot hold (`a * b == K` steps from divisor to divisor);
     * and the value last tried. */
    size_t wsz = (size_t)nv * (2 * sizeof(int64_t) + sizeof(double) + sizeof(uint32_t));
    int64_t *cur = (int64_t *)DVS_ALLOC(ctx->alloc, wsz);
    if (!cur) return 0;
    int64_t *lo = cur + nv;
    double *span = (double *)(lo + nv);
    uint32_t *lvl = (uint32_t *)(span + nv);
    int ok = 1;
    uint32_t nodes = 0;
    const uint64_t fires0 = ctx->prop_fires;
    uint32_t d = 0;
    int advance = 0;     /* back at depth d's scan level after a try */
    trail_push_level(ctx);
    lvl[0] = ctx->decision_level;
    /* Each variable's root range, to project the walk's totals from how far
     * into the box (mixed radix over the open depths) the walk is: a walk
     * headed far past a budget stops early, not at the budget (a failed walk
     * is what a problem the tier does not suit pays, once per process). */
    for (uint32_t j = 0; j < nv; j++) {
        const Variable *u = &ctx->vars[ctx->enum_vars[j]];
        lo[j] = var_lo64(ctx, u);
        span[j] = (double)((uint64_t)var_hi64(ctx, u) - (uint64_t)lo[j]) + 1.0;
    }
    for (;;) {
        if (ctx->prop_fires - fires0 > DVS_ENUM_FIRES) { ok = 0; break; }
        uint32_t id = ctx->enum_vars[d];
        const Variable *v = &ctx->vars[id];
        int done = 0;
        if (advance) {
            advance = 0;
            if (cur[d] == var_hi64(ctx, v)) {
                done = 1;
            } else {
                ctx->conflict_prop_ref   = EXPR_NULL;
                ctx->conflict_clause_idx = EXPR_NULL;
                PropResult pr = ctx_tighten_lb64(ctx, id, cur[d] + 1);
                if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
                if (ctx->prop_aborted) { ok = 0; break; }
                done = pr != PROP_OK;
            }
        }
        if (done) {
            /* Depth d is through its values: drop its scan level, and go on
             * with the next value of the depth above. */
            if (d == 0) break;
            d--;
            trail_backtrack(ctx, lvl[d]);
            advance = 1;
            continue;
        }
        int64_t x = var_lo64(ctx, v);
        cur[d] = x;
        if (_is_hole(ctx, id, x)) { advance = 1; continue; }
        if (++nodes > DVS_ENUM_NODES) { ok = 0; break; }
        /* Only once the walk has done some work: a walk whose first variable
         * steps between sparse values (divisors) has covered little of the
         * range when it has listed most solutions. */
        if ((nodes & 63) == 0
                && (nodes >= 1024 || ctx->prop_fires - fires0 >= (1u << 14))) {
            double done_frac = 0, scale = 1;
            for (uint32_t j = 0; j <= d; j++) {
                scale /= span[j];
                done_frac += ((double)((uint64_t)cur[j] - (uint64_t)lo[j]) + (j == d)) * scale;
            }
            if ((double)ctx->enum_nsol * nv / done_frac > 4.0 * DVS_ENUM_MAX
                    || (double)(ctx->prop_fires - fires0) / done_frac > 4.0 * DVS_ENUM_FIRES)
                { ok = 0; break; }
        }
        PropResult pr = _enum_pin(ctx, id, x);
        if (ctx->prop_aborted) { ok = 0; break; }
        if (pr != PROP_OK) {
            trail_backtrack(ctx, lvl[d]);
            advance = 1;
            continue;
        }
        if (d + 1 < nv) {
            d++;
            trail_push_level(ctx);
            lvl[d] = ctx->decision_level;
            continue;
        }
        /* A leaf: every enumerated var pinned, propagation quiet. */
        if (!_all_fixed(ctx)) { ok = 0; break; }
        if ((uint64_t)(ctx->enum_nsol + 1) * nv > DVS_ENUM_MAX) { ok = 0; break; }
        size_t need = (size_t)(ctx->enum_nsol + 1) * nv;
        if (need > ctx->enum_cap) {
            size_t cap = ctx->enum_cap ? (size_t)ctx->enum_cap * 2 : (size_t)nv * 64;
            if (cap > DVS_ENUM_MAX) cap = DVS_ENUM_MAX;
            if (cap < need) cap = need;
            int64_t *ev = (int64_t *)dvs_ctx_resize(ctx, ctx->enum_vals,
                                                    (size_t)ctx->enum_cap * sizeof(int64_t),
                                                    cap * sizeof(int64_t));
            if (!ev) { ok = 0; break; }
            ctx->enum_vals = ev;
            ctx->enum_cap = (uint32_t)cap;
        }
        int64_t *row = ctx->enum_vals + (size_t)ctx->enum_nsol * nv;
        for (uint32_t j = 0; j < nv; j++)
            row[j] = var_lo64(ctx, &ctx->vars[ctx->enum_vars[j]]);
        ctx->enum_nsol++;
        trail_backtrack(ctx, lvl[d]);
        advance = 1;
    }
    trail_backtrack(ctx, base);
    if (getenv("DVS_ENUM_DEBUG"))
        fprintf(stderr, "[enum] ok=%d nv=%u nsol=%u nodes=%u fires=%llu probe=%u/%u\n", ok, nv,
                ctx->enum_nsol, nodes, (unsigned long long)(ctx->prop_fires - fires0),
                ctx->box_probe_acc, ctx->box_probe_n);
    DVS_RELEASE(ctx->alloc, cur, wsz);
    if (!ok) ctx->enum_nsol = 0;
    return ok;
}

/* Walks that gave up, process-wide, by problem fingerprint and root key: a
 * caller that compiles a fresh context per solve (the Verilator front end,
 * a client that does not reuse contexts) pays for one walk, not one each. */
#define DVS_ENUM_SLOTS 256u
static uint64_t    _enum_fail_tab[DVS_ENUM_SLOTS];
static dvs_mutex_t _enum_mu = DVS_MUTEX_INIT;

static uint64_t _enum_fail_key(const dvs_ctx_t *ctx, uint64_t key) {
    return ctx->problem_fp ? (ctx->problem_fp * 0x9E3779B97F4A7C15ULL) ^ key : 0;
}

static int _enum_failed_before(uint64_t fk) {
    if (!fk) return 0;
    dvs_mutex_lock(&_enum_mu);
    int hit = _enum_fail_tab[fk % DVS_ENUM_SLOTS] == fk;
    dvs_mutex_unlock(&_enum_mu);
    return hit;
}

static void _enum_note_failed(uint64_t fk) {
    if (!fk) return;
    dvs_mutex_lock(&_enum_mu);
    _enum_fail_tab[fk % DVS_ENUM_SLOTS] = fk;
    dvs_mutex_unlock(&_enum_mu);
}

/* Keep the root the list was made at, for _enum_covers. Not kept when a
 * var has holes (a later root without them would hold values the list
 * skipped) or there is no room. */
static void _enum_keep_root(dvs_ctx_t *ctx) {
    ctx->enum_root_n = 0;
    uint32_t n = ctx->n_vars;
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[i];
        if (_count_holes_in_range(ctx, i, var_lo64(ctx, v), var_hi64(ctx, v))) return;
    }
    if (n > ctx->enum_root_cap) {
        int64_t *r = (int64_t *)dvs_ctx_resize(ctx, ctx->enum_root,
                                               (size_t)ctx->enum_root_cap * 2u * sizeof(int64_t),
                                               (size_t)n * 2u * sizeof(int64_t));
        if (!r) return;
        ctx->enum_root = r;
        ctx->enum_root_cap = n;
    }
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[i];
        ctx->enum_root[2 * i]     = var_lo64(ctx, v);
        ctx->enum_root[2 * i + 1] = var_hi64(ctx, v);
    }
    ctx->enum_n_props     = ctx->n_props;
    ctx->enum_n_decl      = ctx->n_decl_vars;
    ctx->enum_restore_gen = ctx->restore_gen;
    ctx->enum_root_n      = n;
}

/* Whether the root now can only have lost solutions against the list's
 * root: no propagator removed (no restore since, none fewer), the same
 * declared vars, and every var the list's root had within its bounds then.
 * Constraints only added and domains only narrowed: the solutions now are
 * among those listed. Vars added since are the new constraints' own. */
static int _enum_covers(const dvs_ctx_t *ctx) {
    if (ctx->enum_root_n == 0 || ctx->restore_gen != ctx->enum_restore_gen
            || ctx->n_props < ctx->enum_n_props || ctx->n_decl_vars != ctx->enum_n_decl
            || ctx->n_vars < ctx->enum_root_n)
        return 0;
    for (uint32_t i = 0; i < ctx->enum_root_n; i++) {
        const Variable *v = &ctx->vars[i];
        if (var_lo64(ctx, v) < ctx->enum_root[2 * i] || var_hi64(ctx, v) > ctx->enum_root[2 * i + 1])
            return 0;
        if (_count_holes_in_range(ctx, i, var_lo64(ctx, v), var_hi64(ctx, v))) return 0;
    }
    return 1;
}

/* Pin listed row `r` at a new level and propagate. 1: a solution (every
 * var fixed, propagation quiet); 0: not, and the trail is back at `base`. */
static int _enum_pin_row(dvs_ctx_t *ctx, uint32_t base, uint32_t r) {
    uint32_t nv = ctx->enum_nv;
    const int64_t *row = ctx->enum_vals + (size_t)r * nv;
    trail_push_level(ctx);
    ctx->conflict_prop_ref   = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
    PropResult pr = PROP_OK;
    for (uint32_t j = 0; j < nv && pr == PROP_OK; j++) {
        pr = ctx_tighten_lb64(ctx, ctx->enum_vars[j], row[j]);
        if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, ctx->enum_vars[j], row[j]);
    }
    if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
    if (pr == PROP_OK && !ctx->prop_aborted && _all_fixed(ctx)) return 1;
    trail_backtrack(ctx, base);
    return 0;
}

/* 1: a listed solution is pinned (the solve is done). 2: the walk found
 * none (UNSAT). 0: search instead. Called at the root, `base`. */
static int _enum_sample(dvs_ctx_t *ctx, uint32_t base) {
    if (!_enum_mode() || ctx->enum_state == ENUM_OFF) return 0;
    uint64_t key = _enum_key(ctx, base);
    /* A root that only added constraints to the list's (an assert after a
     * check-sat: Verilator's XOR-hash asserts, four per randomize): draw
     * from the list and keep the first row that still holds. Each try is
     * uniform over the list, so the kept row is uniform over the rows that
     * hold, which are all the solutions now. A few pins, not a walk. */
    if (ctx->enum_state == ENUM_ON && key != ctx->enum_key && ctx->enum_nsol
            && _enum_reuse_mode() && _enum_covers(ctx)) {
        for (uint32_t t = 0; t < DVS_ENUM_TRIES; t++) {
            uint32_t r = (uint32_t)_rand_range64(ctx, 0, (int64_t)ctx->enum_nsol - 1);
            if (_enum_pin_row(ctx, base, r)) return 1;
            if (ctx->prop_aborted) return 0;
        }
    }
    if (ctx->enum_state == ENUM_UNTRIED || key != ctx->enum_key) {
        if (ctx->enum_state == ENUM_UNTRIED) ctx->enum_redo = DVS_ENUM_REDO;
        else if (ctx->enum_redo-- == 0) { ctx->enum_state = ENUM_OFF; return 0; }
        uint64_t fk = _enum_fail_key(ctx, key);
        if (_enum_failed_before(fk) || !_enum_build(ctx, base)) {
            _enum_note_failed(fk);
            ctx->enum_state = ENUM_OFF;
            return 0;
        }
        ctx->enum_state = ENUM_ON;
        ctx->enum_key = key;
        _enum_keep_root(ctx);
    }
    if (ctx->enum_nsol == 0) return 2;
    if (_enum_pin_row(ctx, base, (uint32_t)_rand_range64(ctx, 0, (int64_t)ctx->enum_nsol - 1)))
        return 1;
    /* A listed solution that no longer holds: the list is stale. */
    ctx->enum_state = ENUM_OFF;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Canonical sampling (dvs_solve_opts_t.canonical)                     */
/* ------------------------------------------------------------------ */

/* A canonical solve's values depend only on the constraints, the pinned
 * values and the seed. The variables are decided one at a time
 * (_canon_select), and each takes the first of its candidates that extends
 * the values before it to a solution. Candidate k is drawn from the random word k of the variable's
 * stream (dvs_rng.h), uniformly (or by its dist weights) over the domain the
 * problem's own propagators leave it, less the values earlier candidates
 * ruled out. No learnt clause is read or learnt (see the LCG setup in
 * _solver_solve_core), so nothing an earlier solve left behind can matter.
 *
 * A rejected candidate is ruled out with the largest aligned block around it
 * that a propagation refutes (a binary search over the block size, about
 * six probes). Values are only ever ruled out when they have no solution,
 * so every draw is uniform over a superset of the feasible values, and the
 * first feasible one drawn is uniform over them: rejection sampling that
 * learns where not to draw, and still finds a sparse feasible set (`addr`
 * aligned in a 4 KB window of 32 bits) in a few dozen candidates. A variable
 * with nothing left sends the search back to the previous variable's next
 * candidate (no backjump: the candidates in between must be tried in
 * order). */

typedef struct { uint64_t a, b; } CanonGap;   /* ruled-out offsets [a, b] */

typedef struct {
    uint32_t x;        /* this frame's variable                           */
    uint32_t k;        /* candidates drawn                                */
    uint32_t gap0;     /* this frame's ruled-out offsets: gaps[gap0..n)   */
    int64_t  lo;       /* the domain the candidates come from: lo + [0, span] */
    uint64_t span;
    uint64_t v;        /* the candidate being tried, as an offset         */
} CanonFrame;

typedef struct {
    CanonGap *gaps;    /* every open frame's, sorted and disjoint per frame */
    uint32_t  n, cap;
} CanonGaps;

/* x <= y + 1, without the overflow of y + 1. */
static inline int _le_succ(uint64_t x, uint64_t y) { return x <= y || x - y == 1u; }

/* Rule out [a, b] for the top frame f. -1 when out of memory. */
static int _canon_gap_add(dvs_ctx_t *ctx, CanonGaps *g, const CanonFrame *f,
                          uint64_t a, uint64_t b) {
    if (g->n == g->cap) {
        uint32_t cap = g->cap ? 2u * g->cap : 64u;
        CanonGap *ng = (CanonGap *)dvs_ctx_resize(ctx, g->gaps, g->cap * sizeof(CanonGap),
                                                  cap * sizeof(CanonGap));
        if (!ng) return -1;
        g->gaps = ng;
        g->cap = cap;
    }
    /* The first gap that is not wholly below a - 1, then the run that
     * overlaps or touches [a, b]. */
    uint32_t i = f->gap0, hi = g->n;
    while (i < hi) {
        uint32_t m = i + (hi - i) / 2u;
        if (_le_succ(a, g->gaps[m].b)) hi = m; else i = m + 1u;
    }
    uint32_t j = i;
    for (; j < g->n && _le_succ(g->gaps[j].a, b); j++) {
        if (g->gaps[j].a < a) a = g->gaps[j].a;
        if (g->gaps[j].b > b) b = g->gaps[j].b;
    }
    memmove(&g->gaps[i + 1u], &g->gaps[j], (g->n - j) * sizeof(CanonGap));
    g->n = g->n - (j - i) + 1u;
    g->gaps[i] = (CanonGap){ a, b };
    return 0;
}

/* The offset of the i-th value of [0, span] not ruled out. */
static uint64_t _canon_nth(const CanonGaps *g, const CanonFrame *f, uint64_t i) {
    uint64_t cur = 0;
    for (uint32_t j = f->gap0; j < g->n; j++) {
        uint64_t len = g->gaps[j].a - cur;
        if (i < len) break;
        i -= len;
        cur = g->gaps[j].b + 1u;
    }
    return cur + i;
}

/* Is offset o ruled out? */
static int _canon_out(const CanonGaps *g, const CanonFrame *f, uint64_t o) {
    for (uint32_t j = f->gap0; j < g->n && g->gaps[j].a <= o; j++)
        if (o <= g->gaps[j].b) return 1;
    return 0;
}

/* Draw frame f's next candidate (variable x) into f->v; 0 when every value
 * is ruled out. */
static int _canon_draw(dvs_ctx_t *ctx, const CanonGaps *g, CanonFrame *f, uint32_t x,
                       uint64_t key) {
    uint64_t out = 0;
    for (uint32_t j = f->gap0; j < g->n; j++) out += g->gaps[j].b - g->gaps[j].a + 1u;
    if (g->n > f->gap0 && out == f->span + 1u) return 0;   /* (0: 2^64 all ruled out) */
    const uint64_t r = dvs_rng_draw(key, f->k++);
    /* A dist draws by its weights (an entry, then a value in it), rejecting
     * ruled-out values: exact while they are rare; then uniform. */
    if (ctx->dist_offsets && x < ctx->n_vars_capacity && ctx->dist_offsets[x]) {
        const int64_t hi = (int64_t)((uint64_t)f->lo + f->span);
        uint64_t total = _dist_total(ctx, x, f->lo, hi);
        for (uint32_t t = 0; total && t < 64u; t++) {
            uint64_t rt = dvs_rng_mix(r + t);
            int64_t elo, ehi;
            if (!_dist_entry(ctx, x, f->lo, hi, dvs_rng_below(rt, total), &elo, &ehi)) break;
            uint64_t o = (uint64_t)elo - (uint64_t)f->lo
                         + dvs_rng_below(dvs_rng_mix(rt), (uint64_t)ehi - (uint64_t)elo + 1u);
            if (!_canon_out(g, f, o)) { f->v = o; return 1; }
        }
    }
    f->v = _canon_nth(g, f, dvs_rng_below(r, f->span + 1u - out));
    return 1;
}

/* Does propagation refute x in lo + [a, b]? */
static int _canon_refuted(dvs_ctx_t *ctx, const CanonFrame *f, uint32_t x,
                          uint64_t a, uint64_t b) {
    const uint32_t lvl = ctx->decision_level;
    trail_push_level(ctx);
    ctx->conflict_prop_ref   = EXPR_NULL;
    ctx->conflict_clause_idx = EXPR_NULL;
    PropResult pr = ctx_tighten_lb64(ctx, x, (int64_t)((uint64_t)f->lo + a));
    if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x, (int64_t)((uint64_t)f->lo + b));
    if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
    trail_backtrack(ctx, lvl);
    return pr != PROP_OK && !ctx->prop_aborted;
}

/* f->v has no solution: rule it out with the largest aligned block around
 * it that propagation refutes. Refuting a block refutes the blocks inside
 * it, so the size is a binary search. */
static int _canon_reject(dvs_ctx_t *ctx, CanonGaps *g, const CanonFrame *f, uint32_t x) {
    const uint64_t o = f->v;
    uint32_t lo_j = 0, hi_j = 63u - (uint32_t)__builtin_clzll(f->span);
    while (lo_j < hi_j && !ctx->prop_aborted) {
        uint32_t j = lo_j + (hi_j - lo_j + 1u) / 2u;
        uint64_t a = o & ~((1ull << j) - 1u);
        uint64_t b = a + ((1ull << j) - 1u);
        if (b > f->span) b = f->span;
        if (_canon_refuted(ctx, f, x, a, b)) lo_j = j; else hi_j = j - 1u;
    }
    uint64_t a = lo_j ? o & ~((1ull << lo_j) - 1u) : o;
    uint64_t b = lo_j ? a + ((1ull << lo_j) - 1u) : o;
    return _canon_gap_add(ctx, g, f, a, b > f->span ? f->span : b);
}

/* The variable a canonical solve decides next, EXPR_NULL when all are
 * fixed: as _select_unassigned without VSIDS -- the open solve-order stage,
 * the highest decision tier, then the smallest domain (derived variables
 * after every other), ties broken by a hash of (seed, variable). Domains
 * are the problem's propagators' alone, so the order is canonical. */
static uint32_t _canon_select(const dvs_ctx_t *ctx, uint64_t seed) {
    const uint32_t stage = ctx->var_order ? _open_stage(ctx) : 0;
    uint32_t best = EXPR_NULL;
    uint64_t best_key[4] = {0};
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *v = &ctx->vars[i];
        int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
        if (lo == hi) continue;
        const int derived = (v->flags & VAR_AUX) || _is_defined(ctx, i);
        if (!derived && stage && _order_key(ctx, i) != stage) continue;
        uint64_t key[4] = { (uint64_t)derived, derived ? 0u : 3u - _tier(ctx, i),
                            _dom_size(ctx, i, lo, hi), dvs_rng_key(seed, i) };
        int lt = best == EXPR_NULL;
        for (int k = 0; k < 4 && !lt && key[k] <= best_key[k]; k++)
            lt = key[k] < best_key[k];
        if (lt) { best = i; memcpy(best_key, key, sizeof key); }
    }
    return best;
}

static dvs_result_t _canon_search(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                                  uint32_t base, double deadline) {
    const size_t bytes = (size_t)ctx->n_vars * sizeof(CanonFrame);
    CanonFrame *fr = ctx->n_vars ? (CanonFrame *)DVS_ALLOC(ctx->alloc, bytes) : NULL;
    if (ctx->n_vars && !fr) return DVS_SOLVE_TIMEOUT;
    CanonGaps gaps = { NULL, 0, 0 };
    const uint64_t max_conf = opts->max_conflicts && opts->max_restarts
        ? (uint64_t)opts->max_conflicts * opts->max_restarts : 0;
    const uint64_t c0 = ctx->conflict_count;
    dvs_result_t res;
    uint64_t tick = 0;
    uint32_t depth = 0;
    int open = 1;    /* the last candidate held: open the next variable's frame */
    int failed = 0;  /* the top frame's candidate f->v has no solution */
    for (;;) {
        if (deadline > 0.0 && (++tick & 0x3FF) == 0 && _now_sec() > deadline) {
            ctx->bail_reason = DVS_BAIL_DEADLINE;
            res = DVS_SOLVE_TIMEOUT;
            break;
        }
        if (ctx->work_limit && dvs_ctx_work(ctx) > ctx->work_limit) {
            ctx->work_hit = 1;
            res = DVS_SOLVE_TIMEOUT;
            break;
        }
        if (open) {
            open = 0;
            const uint32_t x = _canon_select(ctx, opts->seed);
            if (x == EXPR_NULL) { res = DVS_SOLVE_OK; break; }
            if (ctx->decision_level + 1u >= ctx->max_depth) {
                ctx->bail_reason = DVS_BAIL_MAX_DEPTH;
                res = DVS_SOLVE_TIMEOUT;
                break;
            }
            const int64_t lo = var_lo64(ctx, &ctx->vars[x]);
            CanonFrame *f = &fr[depth++];
            *f = (CanonFrame){ .x = x, .gap0 = gaps.n, .lo = lo,
                               .span = (uint64_t)var_hi64(ctx, &ctx->vars[x]) - (uint64_t)lo };
            /* Holes are ruled out from the start. */
            for (const HoleEntry *he = _has_holes(ctx, x) ? dvs_hole_first(ctx, x) : NULL;
                    he; he = he->next) {
                uint64_t o = (uint64_t)he->value - (uint64_t)lo;
                if (o <= f->span && _canon_gap_add(ctx, &gaps, f, o, o) != 0) break;
            }
        }
        CanonFrame *f = &fr[depth - 1];
        const uint32_t x = f->x;
        if (failed && _canon_reject(ctx, &gaps, f, x) != 0) { res = DVS_SOLVE_TIMEOUT; break; }
        failed = 0;
        if (ctx->prop_aborted) { res = DVS_SOLVE_TIMEOUT; break; }
        if (!_canon_draw(ctx, &gaps, f, x, dvs_rng_key(opts->seed, x))) {
            /* x has nothing left: the values before it do not extend. */
            gaps.n = f->gap0;
            if (--depth == 0) { res = DVS_SOLVE_UNSAT; break; }
            trail_backtrack(ctx, base + depth - 1u);
            failed = 1;
            continue;
        }
        const int64_t v = (int64_t)((uint64_t)f->lo + f->v);
        trail_push_level(ctx);
        ctx->conflict_prop_ref   = EXPR_NULL;   /* see the decision push */
        ctx->conflict_clause_idx = EXPR_NULL;
        PropResult pr = ctx_tighten_lb64(ctx, x, v);
        if (pr == PROP_OK) pr = ctx_tighten_ub64(ctx, x, v);
        if (pr == PROP_OK) pr = dvs_solver_propagate(ctx);
        if (pr == PROP_OK) { open = 1; continue; }
        if (ctx->prop_aborted) { res = DVS_SOLVE_TIMEOUT; break; }
        failed = 1;
        ctx->conflict_count++;
        if (max_conf && ctx->conflict_count - c0 >= max_conf) {
            ctx->bail_reason = DVS_BAIL_MAX_RESTARTS;
            res = DVS_SOLVE_TIMEOUT;
            break;
        }
        trail_backtrack(ctx, base + depth - 1u);
    }
    dvs_ctx_free(ctx, gaps.gaps, gaps.cap * sizeof(CanonGap));
    dvs_ctx_free(ctx, fr, bytes);
    return res;
}

/* Words ahead of the enumeration list in dvs_search_export_tiers. */
#define TIERS_HEAD 13

size_t dvs_search_export_tiers(const dvs_ctx_t *ctx, int64_t *buf, size_t cap) {
    const int on = ctx->enum_state == ENUM_ON;
    const uint32_t nv = on ? ctx->enum_nv : 0, ns = on ? ctx->enum_nsol : 0;
    const uint32_t nr = on && ctx->enum_restore_gen == ctx->restore_gen ? ctx->enum_root_n : 0;
    const int64_t head[TIERS_HEAD] = {
        ctx->box_state, ctx->box_probe_acc, ctx->box_probe_n, (int64_t)ctx->mul_count_max,
        ctx->mul_count_probed, ctx->enum_state, ctx->enum_redo, (int64_t)ctx->enum_key,
        nv, ns, nr, ctx->enum_n_props, ctx->enum_n_decl };
    size_t n = TIERS_HEAD + nv + (size_t)ns * nv + 2u * (size_t)nr;
    if (n > cap) return n;
    memcpy(buf, head, sizeof head);
    int64_t *w = buf + TIERS_HEAD;
    for (uint32_t i = 0; i < nv; i++) *w++ = ctx->enum_vars[i];
    if (ns) memcpy(w, ctx->enum_vals, (size_t)ns * nv * sizeof *w);
    w += (size_t)ns * nv;
    if (nr) memcpy(w, ctx->enum_root, 2u * (size_t)nr * sizeof *w);
    return n;
}

int dvs_search_import_tiers(dvs_ctx_t *ctx, const int64_t *buf, size_t n) {
    if (n < TIERS_HEAD) return -1;
    const uint64_t nv = (uint64_t)buf[8], ns = (uint64_t)buf[9], nr = (uint64_t)buf[10];
    if (nv > ctx->n_vars || nr > ctx->n_vars || (nv ? ns > (n - TIERS_HEAD) / nv : ns > 0)
            || n != TIERS_HEAD + nv + ns * nv + 2 * nr)
        return -1;
    const int64_t *w = buf + TIERS_HEAD;
    if (nv) {
        uint32_t *ev = (uint32_t *)dvs_ctx_resize(ctx, ctx->enum_vars,
                                                  (size_t)ctx->enum_nv * sizeof(uint32_t),
                                                  nv * sizeof(uint32_t));
        if (!ev) return -1;
        ctx->enum_vars = ev;
        ctx->enum_nv = (uint32_t)nv;
        for (uint32_t i = 0; i < nv; i++) ev[i] = (uint32_t)*w++;
    }
    if (ns * nv > ctx->enum_cap) {
        int64_t *ev = (int64_t *)dvs_ctx_resize(ctx, ctx->enum_vals,
                                                (size_t)ctx->enum_cap * sizeof(int64_t),
                                                ns * nv * sizeof(int64_t));
        if (!ev) return -1;
        ctx->enum_vals = ev;
        ctx->enum_cap = (uint32_t)(ns * nv);
    }
    if (ns) memcpy(ctx->enum_vals, w, ns * nv * sizeof *w);
    w += ns * nv;
    if (nr > ctx->enum_root_cap) {
        int64_t *r = (int64_t *)dvs_ctx_resize(ctx, ctx->enum_root,
                                               (size_t)ctx->enum_root_cap * 2u * sizeof(int64_t),
                                               nr * 2u * sizeof(int64_t));
        if (!r) return -1;
        ctx->enum_root = r;
        ctx->enum_root_cap = (uint32_t)nr;
    }
    if (nr) memcpy(ctx->enum_root, w, 2 * nr * sizeof *w);
    ctx->box_state        = (uint8_t)buf[0];
    ctx->box_probe_acc    = (uint8_t)buf[1];
    ctx->box_probe_n      = (uint8_t)buf[2];
    ctx->mul_count_max    = (uint64_t)buf[3];
    ctx->mul_count_probed = (uint8_t)buf[4];
    ctx->enum_state       = (uint8_t)buf[5];
    ctx->enum_redo        = (uint8_t)buf[6];
    ctx->enum_key         = (uint64_t)buf[7];
    ctx->enum_nsol        = (uint32_t)ns;
    ctx->enum_root_n      = (uint32_t)nr;
    ctx->enum_n_props     = (uint32_t)buf[11];
    ctx->enum_n_decl      = (uint32_t)buf[12];
    ctx->enum_restore_gen = ctx->restore_gen;
    return 0;
}

int dvs_search_lcg_open(dvs_ctx_t *ctx) {
    if (ctx->lcg || ctx->n_vars == 0) return ctx->lcg ? 0 : -1;
    LCGCtx *lcg = (LCGCtx *)DVS_ALLOC(ctx->alloc, sizeof(LCGCtx));
    if (!lcg) return -1;
    uint32_t nv = ctx->n_vars_capacity > 0 ? ctx->n_vars_capacity : ctx->n_vars;
    if (lcg_init_alloc(lcg, nv, ctx->alloc) != 0) {
        dvs_ctx_free(ctx, lcg, sizeof(LCGCtx));
        return -1;
    }
    lcg->enabled = 0;   /* each solve turns it on or off */
    ctx->lcg = lcg;
    return 0;
}

static dvs_result_t _solver_solve_core(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    ctx->bail_reason = DVS_BAIL_NONE;
    /* Seed or preserve RNG. Sequential seeds must be avalanched first. Under
     * DV_PHASE_INIT=none a seed-0 solve's first decisions draw from the RNG,
     * so it restarts from one fixed state on every solve: a state carried
     * over from the last solve made a repeated seed-0 solve return a
     * different model (test_seed_zero_stays_deterministic). */
    if (opts && opts->seed != 0)  ctx->rng_state = _seed_mix64(opts->seed);
    else if (!_phase_init_lo())   ctx->rng_state = 0xDEADBEEF12345678ULL;
    if (ctx->rng_state == 0)      ctx->rng_state = 0xDEADBEEF12345678ULL;

    /* Wall-clock budget (B10). Default 10s; DV_CDCL_TIME_LIMIT overrides
     * (<=0 disables). deadline==0.0 means unlimited. Checked cheaply (every
     * ~1024 iters) in both the decision loop and the conflict loop. */
    double _deadline = 0.0;
    {
        /* Read once: a getenv per solve was 4% of a small one. */
        static double env_tl = -1.0;
        if (env_tl < 0.0) {
            const char *e = getenv("DV_CDCL_TIME_LIMIT");
            env_tl = e && *e ? atof(e) : 10.0;
            if (env_tl < 0.0) env_tl = 0.0;
        }
        double tl = env_tl;
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
    _build_dec_tier(ctx);

    /* Lazy LCG initialization on first solve when use_lcg is requested. A
     * canonical solve learns nothing and reads no learnt clause: what it
     * returns must not depend on them, and as evidence only (no unit
     * propagation) they cost more than they saved. */
    const int use_lcg = opts && opts->use_lcg && !opts->canonical;
    if (use_lcg) (void)dvs_search_lcg_open(ctx);
    /* `use_lcg` is per solve: an LCG created by an earlier solve learns only
     * in solves that ask for it. (It used to stay on once created, so a
     * caller could not run a plain chronological solve on the context.) */
    if (ctx->lcg)
        ((LCGCtx *)ctx->lcg)->enabled = use_lcg;

    /* Register explain callbacks every solve — propagators added by
     * compile-time aux materialisation or incremental paths after the
     * first solve would otherwise miss their explain wiring. The walk
     * skips propagators whose explain is already set. */
    if (ctx->lcg) {
        contra_register_explanations(ctx);
    }

    /* Allocate phase_save array on first call (lazily). */
    int ps_fresh = 0;
    if (opts && opts->use_phase_save && !ctx->phase_save && ctx->n_vars > 0)
        ps_fresh = dvs_ctx_alloc_phase_save(ctx) == 0;

    /* Initialize the saved phases, once, on the solve that allocated the
     * array: each var's lower bound, or none set (see _phase_init_lo).
     * Only the seed-0 (BMC/decision) path consults them -- a diversity solve
     * (seed != 0) bypasses phase saving entirely so its value choice goes
     * through the hole-aware randomized path. See B18/B19 in docs/solver_bug_backlog.md for why
     * saved phases must never be random values the pick did not vet. */
    if (ps_fresh && ctx->phase_save && ctx->n_vars > 0) {
        /* Never write past the allocation: n_vars can grow past the capacity
         * the array was sized to via the incremental paths. */
        uint32_t ps_lim = ctx->n_vars_capacity > 0
                          ? ctx->n_vars_capacity : ctx->n_vars;
        if (ps_lim > ctx->n_vars) ps_lim = ctx->n_vars;
        int init_lo = _phase_init_lo();
        for (uint32_t i = 0; i < ps_lim; i++) {
            ctx->phase_save[i] = var_lo64(ctx, &ctx->vars[i]);
            ctx->phase_set[i]  = (uint8_t)init_lo;
        }
    }

    /* Kept draws (see _pick_value) are per solve: a seeded solve starts
     * with none. DVS_KEEP_DRAWS=0 turns keeping off. */
    ctx->keep_on = 0;
    if (opts && opts->seed != 0 && _keep_draws_enabled() && ctx->n_vars > 0) {
        if (ctx->keep_cap < ctx->n_vars) {
            /* One block, keep_val[n] then keep_state[n]; per solve, so the
             * old contents need not move. */
            size_t row = sizeof(int64_t) + 1u;
            int64_t *kv = (int64_t *)DVS_ALLOC(ctx->alloc, ctx->n_vars * row);
            if (kv) {
                dvs_ctx_free(ctx, ctx->keep_val, ctx->keep_cap * row);
                ctx->keep_val   = kv;
                ctx->keep_state = (uint8_t *)(kv + ctx->n_vars);
                ctx->keep_cap   = ctx->n_vars;
            }
        }
        if (ctx->keep_cap >= ctx->n_vars) {
            memset(ctx->keep_state, KEEP_NONE, ctx->n_vars);
            ctx->keep_on = 1;
        }
    }

    /* Failed value decisions (see _bound_decision) are per seeded solve. */
    ctx->value_fails_on = 0;
    if (opts && opts->seed != 0 && ctx->n_vars > 0) {
        if (ctx->value_fails_cap < ctx->n_vars) {
            uint8_t *vf = (uint8_t *)dvs_ctx_resize(ctx, ctx->value_fails,
                                                    ctx->value_fails_cap, ctx->n_vars);
            if (vf) { ctx->value_fails = vf; ctx->value_fails_cap = ctx->n_vars; }
        }
        if (ctx->value_fails_cap >= ctx->n_vars) {
            memset(ctx->value_fails, 0, ctx->n_vars);
            ctx->value_fails_on = 1;
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
    uint32_t mul_rejects    = 0;   /* _mul_count_accept */
    ctx->wpick_left = DVS_WPICK_PROBES;
    ctx->wmemo_solve++;
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
    /* The search may decide one level per variable plus MAX_DECISION_DEPTH
     * more (for splits) above its root. A fixed 256 made any problem with
     * more decision variables bail (a 300-element array freed by a symbolic
     * select). A growable context grows its level arrays to that; a fixed
     * pool keeps the room its checkpoints reserved. */
    {
        uint32_t want = base + MAX_DECISION_DEPTH + ctx->n_vars + 2u;
        if (want > 0xFFFFu) want = 0xFFFFu;
        if (want > ctx->level_cap) (void)dvs_ctx_reserve_levels(ctx, want);
        uint32_t room = ctx->level_cap - 2u;
        ctx->max_depth = want - 2u < room ? want - 2u : room;
    }
    ctx->level_marks[base].trail_top   = ctx->trail_top;
    ctx->level_marks[base].trail_count = ctx->trail_count;
    ctx->level_marks[base].entail_top  = ctx->entail_top;
    ctx->level_marks[base].stack_mark  = dvs_stack_push(ctx->dynamic);
    ctx->search_sealed = 1;

    /* Pre-search bounds shaving (L2): tighten domains beyond what
     * individual propagator fixed-point can achieve. */
    uint32_t max_si = opts ? opts->max_shave_iters : 1000;
    if (max_si > 0) {
        PropResult sr = bounds_shave(ctx, max_si);
        if (ABORTED()) return DVS_SOLVE_TIMEOUT;   /* a probe's verdict is void */
        if (sr == PROP_CONFLICT) return DVS_SOLVE_UNSAT;
    }
    if (opts && opts->canonical) return _canon_search(ctx, opts, base, _deadline);

    /* Box-rejection tier: uniform over solutions when it succeeds. With a
     * solve order, the search decides the ordered stages first, and the
     * tier then draws the unordered variables given them (box_stage_left
     * bounds how often a solve tries that, as backtracking re-enters it). */
    const int box_ok = opts && opts->seed != 0 && ctx->fair_pick
                       && !opts->seq_pick;
    uint32_t box_stage_left = (box_ok && ctx->var_order) ? 4u : 0u;
    if (box_ok) {
        /* Every tier below is uniform over solutions, so any mix of them
         * is too; the search after them is not. Order is for speed: a
         * listed solution is one pin, a box draw 1/rate tries on average.
         * The probe has run once box_state is not BOX_UNPROBED; before
         * then, or when the box declined (a dist, one variable), the
         * enumeration tier is not used either. */
        const int listed = !ctx->var_order && ctx->box_state != BOX_UNPROBED
                           && ctx->enum_state == ENUM_ON;
        if (listed) {
            int er = _enum_sample(ctx, base);
            if (ABORTED()) return DVS_SOLVE_TIMEOUT;
            if (er == 1) return DVS_SOLVE_OK;
            if (er == 2) return DVS_SOLVE_UNSAT;
        }
        if (_box_sample(ctx, base, 0, DVS_BOX_TRIES)) return DVS_SOLVE_OK;
        if (ABORTED()) return DVS_SOLVE_TIMEOUT;
        /* The box is off, or its draws missed: few solutions may be listed
         * instead. A miss used to go straight to the search, which made a
         * solve a mix of an exact draw and a biased one. */
        if (!listed && !ctx->var_order && ctx->box_state != BOX_UNPROBED) {
            int er = _enum_sample(ctx, base);
            if (ABORTED()) return DVS_SOLVE_TIMEOUT;
            if (er == 1) return DVS_SOLVE_OK;
            if (er == 2) return DVS_SOLVE_UNSAT;
        }
        /* Too many solutions to list: keep drawing from the box. */
        if (!ctx->var_order && ctx->box_state == BOX_ON) {
            if (_box_sample(ctx, base, 0, DVS_BOX_RETRIES)) return DVS_SOLVE_OK;
            if (ABORTED()) return DVS_SOLVE_TIMEOUT;
        }
    }

    for (;;) {
        /* Wall-clock budget check (decision loop). */
        if (_deadline > 0.0 && (++_tick & 0x3FF) == 0 && _now_sec() > _deadline) {
            ctx->bail_reason = DVS_BAIL_DEADLINE;
            return DVS_SOLVE_TIMEOUT;
        }
        /* Work budget (deterministic; _solver_solve reports it). */
        if (ctx->work_limit && dvs_ctx_work(ctx) > ctx->work_limit) {
            ctx->work_hit = 1;
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
        if (box_stage_left && _order_key(ctx, x_id) == DVS_ORDER_LAST
                && !(ctx->vars[x_id].flags & VAR_AUX)) {
            box_stage_left--;
            if (_box_sample(ctx, ctx->decision_level, 1, DVS_BOX_TRIES)) return DVS_SOLVE_OK;
            if (ABORTED()) return DVS_SOLVE_TIMEOUT;
        }
        int64_t v = _pick_value(ctx, x_id, opts);

        /* Decision-depth guard: MAX_DECISION_DEPTH above the root (and
         * within the level arrays). The search pushes one level per decided
         * variable, so a problem with more decision variables than that bails
         * with TIMEOUT and the caller defers/escalates cleanly. */
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

        /* ── Push level and assign ── */
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
        if (!bound && _tier(ctx, x_id) == 2 && !ABORTED()
                && !_mul_count_accept(ctx, opts, x_id, dec_idx, pr, &mul_rejects))
            continue;   /* a square's root rejected: draw again */

        /* The decision just made conflicted: its value is not kept. */
        if (pr == PROP_CONFLICT && ctx->keep_on && x_id < ctx->keep_cap)
            ctx->keep_state[x_id] = KEEP_DEAD;
        if (pr == PROP_CONFLICT && !bound && ctx->value_fails_on
                && x_id < ctx->value_fails_cap && ctx->value_fails[x_id] < 255)
            ctx->value_fails[x_id]++;

        /* ── Conflict loop ── */
        int fresh = 1;   /* first pass: the conflict is the decision just made */
        while (pr == PROP_CONFLICT) {
            const int counted = fresh;   /* value_fails already has it */
            fresh = 0;
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
            if (ctx->work_limit && dvs_ctx_work(ctx) > ctx->work_limit) {
                ctx->work_hit = 1;
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
            } else if (_redraw(ctx, opts, dv, counted)) {
                /* Rejection: the decision is undone and nothing excluded; the
                 * search draws again (see _redraw). */
                pr = PROP_OK;
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
                        && opts->seed == 0 && ctx->phase_save) {
                    ctx->phase_save[dv] = val;
                    ctx->phase_set[dv]  = 1;
                }
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
        _solver_reset(ctx);
        _pin_inactive_assumptions(ctx);
        if (ctx->pin_conflict || dvs_solver_propagate(ctx) == PROP_CONFLICT ||
                _solver_solve_core(ctx, opts) != DVS_SOLVE_OK) {
            ctx->assumption_active_mask = saved;   /* keep it dropped */
        }
    }
    /* Materialize the final accepted set: the last trial may have been a revert,
     * leaving stale search state. The final mask was proven SAT, so this resolves. */
    _solver_reset(ctx);
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
    /* The oracle checks the answer whichever engine gave it. */
    double t0 = ctx && ctx->oracle ? dvs_oracle_clock_ms() : 0;
    dvs_result_t r;
    if (!dvs_engine_solve(ctx, opts, &r)) {
        r = _solver_solve(ctx, opts);
        /* The search gave up: bit-blast, when the context may move. */
        if (r == DVS_SOLVE_TIMEOUT) dvs_engine_take_over(ctx, opts, &r);
    }
    if (ctx && ctx->oracle) dvs_oracle_on_solve(ctx, opts, r, dvs_oracle_clock_ms() - t0);
    return r;
}

void dvs_search_discard(dvs_ctx_t *ctx) {
    if (!ctx || !ctx->search_sealed) return;
    ctx->search_sealed = 0;
    trail_backtrack(ctx, ctx->search_base);
}

/* Escalating a search that cannot finish.
 *
 * DVS_SOLVE_TIMEOUT tells the caller to use another engine (the bit-blaster),
 * and the default budget -- DV_CDCL_TIME_LIMIT (10 s) or 10000 restarts -- is
 * sized for one hard solve. A randomize() loop re-solves the same compiled
 * problem, so a problem the search thrashes on paid the whole budget on every
 * call: ~1.7 s a call on pyvsc's F5 before it was fixed, 10 s on a product
 * of 16-bit fields. Each client grew its own probe-and-remember policy to
 * avoid that (the SMT-LIB2 front end's cdcl_route). The context remembers
 * instead: once a default-budget solve of this problem has given up, later
 * ones stop after DVS_THRASH_MULT times the most propagator firings any
 * solve of it needed to answer (at least DVS_THRASH_FLOOR), so the caller
 * escalates in milliseconds. Firings, not conflicts or time: a conflict can
 * cost one firing or a million, and a count keeps the cut deterministic. A
 * solve that sets a budget of its own (max_conflicts, max_restarts or
 * time_limit_ms) is not capped. */
#define DVS_THRASH_FLOOR 200000u
#define DVS_THRASH_MULT  8u

/* The memory outlives the context: a client may compile the problem afresh
 * for every call (pyvsc does, once its solve needed a fallback), and each
 * fresh context would pay the whole budget again. So it is also kept per
 * process, by problem fingerprint, in a small direct-mapped table: a
 * collision only forgets, and what it holds only shortens a search that
 * returns "unknown", never changes an answer. */
#define DVS_THRASH_SLOTS 64u
typedef struct { uint64_t fp, ok_fires_max; uint8_t thrashed; } ThrashSlot;
static ThrashSlot  _thrash_tab[DVS_THRASH_SLOTS];
static dvs_mutex_t _thrash_mu = DVS_MUTEX_INIT;

void dvs_thrash_recall(dvs_ctx_t *ctx, uint64_t fp) {
    ctx->problem_fp = fp;
    ctx->ok_fires_max = 0;
    ctx->thrashed = 0;
    if (!fp) return;
    dvs_mutex_lock(&_thrash_mu);
    const ThrashSlot *s = &_thrash_tab[fp % DVS_THRASH_SLOTS];
    if (s->fp == fp) {
        ctx->ok_fires_max = s->ok_fires_max;
        ctx->thrashed = s->thrashed;
    }
    dvs_mutex_unlock(&_thrash_mu);
}

static void _thrash_store(const dvs_ctx_t *ctx) {
    if (!ctx->problem_fp) return;
    dvs_mutex_lock(&_thrash_mu);
    ThrashSlot *s = &_thrash_tab[ctx->problem_fp % DVS_THRASH_SLOTS];
    *s = (ThrashSlot){ ctx->problem_fp, ctx->ok_fires_max, ctx->thrashed };
    dvs_mutex_unlock(&_thrash_mu);
}

static dvs_result_t _solver_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    dvs_search_discard(ctx);
    const uint64_t work_budget = ctx->work_budget;
    ctx->work_budget = 0;
    const int default_budget = !(opts && (opts->max_conflicts || opts->max_restarts
                                          || opts->time_limit_ms)) && !work_budget;
    const uint64_t f0 = ctx->prop_fires;
    ctx->thrash_hit = 0;
    ctx->thrash_limit = 0;
    ctx->work_hit = 0;
    ctx->work_limit = work_budget ? dvs_ctx_work(ctx) + work_budget : 0;
    if (default_budget && ctx->thrashed) {
        uint64_t cap = ctx->ok_fires_max * DVS_THRASH_MULT;
        ctx->thrash_limit = f0 + (cap > DVS_THRASH_FLOOR ? cap : DVS_THRASH_FLOOR);
    }
    dvs_result_t r = _solver_solve_relax(ctx, opts);
    ctx->thrash_limit = 0;
    ctx->work_limit = 0;
    /* The propagation deadline belongs to this solve only: a later pin or
     * incremental add must not abort on it. */
    ctx->prop_deadline = 0.0;
    if (ctx->prop_aborted) {
        ctx->prop_aborted = 0;
        r = DVS_SOLVE_TIMEOUT;
    }
    if (ctx->thrash_hit) {
        ctx->thrash_hit = 0;
        r = DVS_SOLVE_TIMEOUT;
        ctx->bail_reason = DVS_BAIL_THRASH;
    }
    if (ctx->work_hit) {
        ctx->work_hit = 0;
        r = DVS_SOLVE_TIMEOUT;
        ctx->bail_reason = DVS_BAIL_WORK;
    }
    if (r == DVS_SOLVE_OK || r == DVS_SOLVE_UNSAT) {
        uint64_t used = ctx->prop_fires - f0;
        if (used > ctx->ok_fires_max) {
            ctx->ok_fires_max = used;
            if (ctx->thrashed) _thrash_store(ctx);
        }
    } else if (r == DVS_SOLVE_TIMEOUT && default_budget && !ctx->thrashed
               && ctx->bail_reason != DVS_BAIL_MAX_DEPTH) {
        ctx->thrashed = 1;
        _thrash_store(ctx);
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
        dvs_result_t res = ctx->pin_conflict ? DVS_SOLVE_UNSAT
                                             : _solver_solve_core(ctx, opts);
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

        /* Reset solver to post-compile state, keeping the pins */
        _solver_reset(ctx);

        /* Pin all relaxed assumptions to 0 by directly setting bounds.
         * We can't use ctx_tighten because the assumption var starts
         * at [1,1] after reset, and tightening UB to 0 would create
         * an empty domain [1,0] -> conflict. Direct writes are safe
         * here since we're at level 0 before search begins. */
        _pin_inactive_assumptions(ctx);

        /* Propagate the relaxations before retrying */
        if (ctx->pin_conflict || dvs_solver_propagate(ctx) == PROP_CONFLICT) {
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
    int64_t bv;
    if (dvs_engine_value(ctx, var_id, &bv)) return bv;
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
    ctx->level_marks[lvl].entail_top  = m->entail_top;
    ctx->level_marks[lvl].stack_mark  = m->stack_mark;
    trail_backtrack(ctx, lvl);
    trail_push_level(ctx);            /* the level the checkpoint opened */
    ctx->conflict_count = 0;

    uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                   ? ctx->n_props : ctx->n_prop_refs_capacity;
    for (uint32_t i = 0; i < lim; i++) {
        if (ctx->prop_refs[i] != EXPR_NULL) {
            Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool, ctx->prop_refs[i]);
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
        if (r == PROP_CONFLICT) {
            ctx->pin_conflict = 1;
            break;
        }
    }
}

void dvs_solver_reset(dvs_ctx_t *ctx) {
    if (ctx && ctx->n_checkpoints == 0 && ctx->n_root_pins > 0) {
        /* A learnt clause drops its root literals, and a pin is one: what
         * was learnt under the pins (`x != 7` from `x != i`, i pinned to 7)
         * would outlive them, losing solutions on every later solve. */
        ctx->n_root_pins = 0;
        if (ctx->lcg) lcg_reset((LCGCtx *)ctx->lcg);
    }
    _solver_reset(ctx);
    if (ctx && ctx->oracle) dvs_oracle_on_reset(ctx);
}

static void _solver_reset(dvs_ctx_t *ctx) {
    if (!ctx || !ctx->initial_vars || ctx->initial_n_vars == 0) return;
    ctx->search_sealed = 0;            /* undone below */
    ctx->pin_conflict = 0;             /* the pins are re-established below */
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
            WideBounds64 *saved = (WideBounds64 *)dvs_spool_ptr(&ctx->pool, iv->holes_offset);
            WideBounds64 *live  = (WideBounds64 *)dvs_spool_ptr(&ctx->pool, live_off);
            *live = *saved;              /* live wide bounds back to pristine */
            v->holes_offset = live_off;  /* keep the var on its live struct */
        }
    }

    /* Reset search state. The trail and entailment records live on the
     * dynamic stack; release them, or every solve grows it. */
    ctx->decision_level = 0;
    ctx->trail_top      = NULL;
    ctx->trail_count    = 0;
    ctx->entail_top     = NULL;
    if (ctx->dynamic) dvs_stack_pop(ctx->dynamic, (dvs_stack_mark_t){0});
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
            Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool,
                                                        ctx->prop_refs[i]);
            p->flags &= (uint8_t)~(PROP_FLAG_ENTAILED | PROP_FLAG_IN_QUEUE);
            prop_enqueue(ctx, ctx->prop_refs[i]);
        }
    }
    }

    /* Re-establish the pins (none after dvs_solver_reset, which drops them
     * first). */
    for (uint32_t i = 0; i < ctx->n_root_pins; i++) {
        const DvsRootPin *rp = &ctx->root_pins[i];
        if (ctx_tighten_lb64(ctx, rp->var, rp->value) == PROP_CONFLICT
                || ctx_tighten_ub64(ctx, rp->var, rp->value) == PROP_CONFLICT) {
            ctx->pin_conflict = 1;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* dvs_solver_pin_var                                                      */
/* ------------------------------------------------------------------ */

static int _solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

int dvs_solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (dvs_engine_need_cdcl(ctx) != 0) return -1;
    int rc = _solver_pin_var(ctx, var_id, value);
    if (ctx && ctx->oracle) dvs_oracle_on_pin(ctx, var_id, value, rc);
    return rc;
}

static int _solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (!ctx || var_id >= ctx->n_vars) return -1;
    dvs_search_discard(ctx);
    var_id = _alias_root(ctx, var_id);
    if (ctx->n_checkpoints > 0) {
        if (dvs_scope_log_bound(ctx, var_id, 1, value) != 0 ||
                dvs_scope_log_bound(ctx, var_id, 0, value) != 0)
            return -1;
    } else {
        /* Kept so relaxing a soft constraint, which resets the context,
         * keeps the pin (see _solver_reset). */
        if (ctx->n_root_pins == ctx->root_pins_cap) {
            uint32_t cap = ctx->root_pins_cap ? 2 * ctx->root_pins_cap : 8;
            DvsRootPin *n = (DvsRootPin *)dvs_ctx_resize(ctx, ctx->root_pins,
                (size_t)ctx->root_pins_cap * sizeof(DvsRootPin),
                (size_t)cap * sizeof(DvsRootPin));
            if (!n) return -1;
            ctx->root_pins = n;
            ctx->root_pins_cap = cap;
        }
        ctx->root_pins[ctx->n_root_pins++] = (DvsRootPin){ var_id, 0, value };
    }

    /* With soft constraints the propagation below assumes them all, so a
     * conflict may only mean some must be relaxed: the pin stands, and the
     * solve decides (it relaxes softs, and is UNSAT only when the pins
     * contradict the hard constraints). */
    const int soft = ctx->n_assumptions > 0;
    PropResult r = ctx_tighten_lb64(ctx, var_id, value);
    if (r != PROP_CONFLICT) r = ctx_tighten_ub64(ctx, var_id, value);
    if (r != PROP_CONFLICT) r = dvs_solver_propagate(ctx);
    if (r == PROP_CONFLICT && ctx->prop_aborted) {
        /* Abandoned at the trail cap (a strict compare climbing one value a
         * round, B61), not refuted: the pin stands and the solve carries on
         * the propagation under its own deadline. Reported as a conflict,
         * a satisfiable pin read as unsat, and the flag failed every pin
         * after it until the next solve (B103). */
        ctx->prop_aborted = 0;
        return 0;
    }
    if (r == PROP_CONFLICT) {
        if (!soft) return -1;
        ctx->pin_conflict = 1;
    }
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
    opts.seq_pick = 0;
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
    for (const HoleEntry *he = dvs_hole_first(ctx, var_id); he; he = he->next) {
        if (he->value == value) return 1;
        if (he->value > value) return 0;  /* sorted ascending, done */
    }
    return 0;
}

/* Count holes within [lo, hi]. */
static uint32_t _count_holes_in_range(const dvs_ctx_t *ctx, uint32_t var_id,
                                       int64_t lo, int64_t hi) {
    if (!ctx->var_holes_head) return 0;
    uint32_t count = 0;
    for (const HoleEntry *he = dvs_hole_first(ctx, var_id); he; he = he->next) {
        if (he->value > hi) break;
        if (he->value >= lo) count++;
    }
    return count;
}

/* Insert a value into the sorted hole list for a variable.
 * Returns 0 on success, 1 if already present, -1 on alloc failure. */
static int _insert_hole(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (!ctx->var_holes_head) return -1;
    ctx->wmemo_epoch++;

    /* Find the place (sorted ascending) before allocating. */
    HoleEntry *prev = NULL, *cur = dvs_hole_first(ctx, var_id);
    while (cur) {
        if (cur->value == value) return 1;  /* already present */
        if (cur->value > value) break;
        prev = cur;
        cur = cur->next;
    }

    uint32_t he_ref = dvs_spool_alloc(&ctx->pool,
                                      (uint32_t)sizeof(HoleEntry),
                                      (uint32_t)_Alignof(HoleEntry));
    if (he_ref == EXPR_NULL) return -1;
    HoleEntry *he = (HoleEntry *)dvs_spool_ptr(&ctx->pool, he_ref);
    he->value = value;
    he->next  = cur;
    he->self  = he_ref;
    he->_hpad = 0;
    if (prev) prev->next = he;
    else      ctx->var_holes_head[var_id] = he_ref;
    return 0;
}

int dvs_search_insert_hole(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    return _insert_hole(ctx, var_id, value);
}

static int _solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

int dvs_solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value) {
    if (dvs_engine_need_cdcl(ctx) != 0) return -1;
    dvs_search_discard(ctx);
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
