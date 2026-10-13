#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "dvs_search.h"
#include "dvs_thread.h"
#include "dvs_ctx.h"
#include "dvs_oracle_hooks.h"
#include "dvs_propagator.h"
#include "dvs_trail.h"
#include "dvs_shave.h"
#include "dvs_lcg.h"
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

/* Pick a value from a distribution-constrained variable using weighted
 * random selection.  Returns the chosen value, or falls through to
 * uniform random if no valid dist entry intersects the feasible domain. */
static int64_t _pick_value_dist(dvs_ctx_t *ctx, uint32_t var_id,
                                 int64_t lo, int64_t hi) {
    uint32_t dm_off = ctx->dist_offsets[var_id];
    if (dm_off == 0) return _rand_range64(ctx, lo, hi);

    DistMeta *dm = (DistMeta *)dvs_spool_ptr(&ctx->pool, dm_off);
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
static int _box_sample_inner(dvs_ctx_t *ctx, uint32_t base, int last_stage);
static int _box_sample(dvs_ctx_t *ctx, uint32_t base, int last_stage) {
    static int stats = -1;
    if (stats < 0) {
        const char *e = getenv("DVS_BOX_STATS");
        stats = (e && e[0] == '1');
        if (stats) atexit(_bx_report);
    }
    uint8_t st0 = ctx->box_state;
    int r = _box_sample_inner(ctx, base, last_stage);
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
 * the probe runs at the root, where it depends on the problem only. */
static int _box_sample_inner(dvs_ctx_t *ctx, uint32_t base, int last_stage) {
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
        ctx->box_state = (r >= 0 && acc >= need)
                         ? BOX_ON : BOX_OFF;
        if (ctx->box_state == BOX_OFF) return 0;
    }
    /* At the root of an ordered problem, only probe: the earlier stages
     * are chosen first, by the search. */
    if (ctx->var_order && !last_stage) return 0;

    for (uint32_t t = 0; t < DVS_BOX_TRIES; t++) {
        int r = _box_try(ctx, base);
        if (r == 1) return 1;
        if (r < 0) return 0;
    }
    return 0;
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
    _build_dec_tier(ctx);

    /* Lazy LCG initialization on first solve when use_lcg is requested. */
    if (opts && opts->use_lcg && !ctx->lcg && ctx->n_vars > 0) {
        LCGCtx *lcg = (LCGCtx *)DVS_ALLOC(ctx->alloc, sizeof(LCGCtx));
        if (lcg) {
            uint32_t nv = ctx->n_vars_capacity > 0
                          ? ctx->n_vars_capacity : ctx->n_vars;
            if (lcg_init_alloc(lcg, nv, ctx->alloc) == 0) {
                ctx->lcg = lcg;
            } else {
                dvs_ctx_free(ctx, lcg, sizeof(LCGCtx));
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

    /* Allocate phase_save array on first call (lazily). */
    int ps_fresh = 0;
    if (opts && opts->use_phase_save && !ctx->phase_save && ctx->n_vars > 0)
        ps_fresh = dvs_ctx_alloc_phase_save(ctx) == 0;

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
    /* The search may decide MAX_DECISION_DEPTH levels above its root, within
     * the level arrays (a checkpoint reserves that room as it opens). */
    ctx->max_depth = base + MAX_DECISION_DEPTH < ctx->level_cap
                   ? base + MAX_DECISION_DEPTH : ctx->level_cap;
    ctx->level_marks[base].trail_top   = ctx->trail_top;
    ctx->level_marks[base].trail_count = ctx->trail_count;
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

    /* Box-rejection tier: uniform over solutions when it succeeds. With a
     * solve order, the search decides the ordered stages first, and the
     * tier then draws the unordered variables given them (box_stage_left
     * bounds how often a solve tries that, as backtracking re-enters it). */
    const int box_ok = opts && opts->seed != 0 && ctx->fair_pick
                       && !opts->seq_pick;
    uint32_t box_stage_left = (box_ok && ctx->var_order) ? 4u : 0u;
    if (box_ok) {
        if (_box_sample(ctx, base, 0)) return DVS_SOLVE_OK;
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
        if (box_stage_left && _order_key(ctx, x_id) == DVS_ORDER_LAST
                && !(ctx->vars[x_id].flags & VAR_AUX)) {
            box_stage_left--;
            if (_box_sample(ctx, ctx->decision_level, 1)) return DVS_SOLVE_OK;
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
    const int default_budget = !(opts && (opts->max_conflicts || opts->max_restarts
                                          || opts->time_limit_ms));
    const uint64_t f0 = ctx->prop_fires;
    ctx->thrash_hit = 0;
    ctx->thrash_limit = 0;
    if (default_budget && ctx->thrashed) {
        uint64_t cap = ctx->ok_fires_max * DVS_THRASH_MULT;
        ctx->thrash_limit = f0 + (cap > DVS_THRASH_FLOOR ? cap : DVS_THRASH_FLOOR);
    }
    dvs_result_t r = _solver_solve_relax(ctx, opts);
    ctx->thrash_limit = 0;
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
    ctx->search_sealed = 0;            /* undone below */
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
            Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool,
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
    dvs_search_discard(ctx);
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
    if (r == PROP_CONFLICT && ctx->prop_aborted) {
        /* Abandoned at the trail cap (a strict compare climbing one value a
         * round, B61), not refuted: the pin stands and the solve carries on
         * the propagation under its own deadline. Reported as a conflict,
         * a satisfiable pin read as unsat, and the flag failed every pin
         * after it until the next solve (B103). */
        ctx->prop_aborted = 0;
        return 0;
    }
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
