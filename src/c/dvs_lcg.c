#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvs_lcg.h"
#include "dvs_ctx.h"
#include "dvs_explain.h"
#include "dvs_propagator.h"
#include "dvs_trail.h"

/* Is literal `a` already implied by an earlier learnt-buffer literal on the
 * same variable (within [0, learnt_idx), skipping skip_idx)? Extracted from a
 * GCC statement-expression macro so it also compiles under MSVC. */
static inline int dvs__implied_by_learnt(const LCGCtx *lcg, uint32_t learnt_idx,
                                         Literal a, uint32_t skip_idx) {
    for (uint32_t _li = 0; _li < learnt_idx; _li++) {
        if (_li == skip_idx) continue;
        Literal _L = lcg->learnt_buf[_li];
        if (_L.var_id != a.var_id) continue;
        if (a.is_lb && !_L.is_lb && (int64_t)_L.bound >= (int64_t)a.bound - 1)
            return 1;
        if (!a.is_lb && _L.is_lb && (int64_t)_L.bound <= (int64_t)a.bound + 1)
            return 1;
    }
    return 0;
}

/* DV_LCG_TRACE: emit per-conflict trace to stderr. Resolved once per
 * process. Off (0) by default. */
static int _trace_check(void) {
    const char *e = getenv("DV_LCG_TRACE");
    return (e && *e && *e != '0') ? 1 : 0;
}
static inline int _tron(void) {
    static int cached = -1;
    if (cached < 0) cached = _trace_check();
    return cached;
}

static const char *_pname(dvs_ctx_t *ctx, uint32_t prop_ref) {
    if (prop_ref == EXPR_NULL) return "<decision>";
    Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool, prop_ref);
    return prop_fire_name(p->fire);
}

static void _trace_lit(const char *prefix, Literal lit) {
    fprintf(stderr, "[lcg-trace] %s v%u %s %lld\n",
            prefix, lit.var_id, lit.is_lb ? ">=" : "<=", (long long)lit.bound);
}

/* Index into lcg->seen[] / seen_lit[]: separate slots for LB and UB
 * literals on the same variable. Slot layout: [v0_lb, v0_ub, v1_lb, v1_ub, ...]. */
#define SEEN_IX(v, is_lb_)  ((2u * (v)) + ((is_lb_) ? 0u : 1u))

#define PROP_WS(p) ((PropWatchSect *)((char *)(p) + sizeof(Propagator)))

/* ================================================================== */
/* Clause Database                                                     */
/* ================================================================== */

int clause_db_init(ClauseDB *db, uint32_t n_vars) {
    memset(db, 0, sizeof(*db));

    db->clauses_cap = CLAUSE_DB_INIT_CAP;
    db->clauses = (Clause **)calloc(db->clauses_cap, sizeof(Clause *));
    if (!db->clauses) return -1;

    db->n_watch_vars = n_vars;
    db->watch_lb = (WatchEntry **)calloc(n_vars, sizeof(WatchEntry *));
    db->watch_ub = (WatchEntry **)calloc(n_vars, sizeof(WatchEntry *));
    if (!db->watch_lb || !db->watch_ub) {
        clause_db_destroy(db);
        return -1;
    }

    db->arena_cap = 1u << 22;  /* 4 MiB arena (no realloc) */
    db->arena = (uint8_t *)malloc(db->arena_cap);
    if (!db->arena) {
        clause_db_destroy(db);
        return -1;
    }
    db->arena_used = 0;

    return 0;
}

void clause_db_destroy(ClauseDB *db) {
    free(db->clauses);
    free(db->watch_lb);
    free(db->watch_ub);
    free(db->arena);
    memset(db, 0, sizeof(*db));
}

/* Arena allocator for clauses and watch entries */
static void *_arena_alloc(ClauseDB *db, uint32_t size, uint32_t align) {
    uint32_t off = (db->arena_used + align - 1) & ~(align - 1);
    if (off + size > db->arena_cap) {
        /* Arena full: cannot grow without invalidating pointers.
         * Return NULL; caller should GC or give up. */
        return NULL;
    }
    void *ptr = db->arena + off;
    db->arena_used = off + size;
    return ptr;
}

/* Add a watch entry for a literal in a clause */
static void _add_watch(ClauseDB *db, Literal lit, uint32_t clause_idx) {
    WatchEntry *we = (WatchEntry *)_arena_alloc(db, sizeof(WatchEntry), 8);
    if (!we) return;
    we->clause_idx = clause_idx;
    if (lit.is_lb) {
        if (lit.var_id < db->n_watch_vars) {
            we->next = db->watch_lb[lit.var_id];
            db->watch_lb[lit.var_id] = we;
        }
    } else {
        if (lit.var_id < db->n_watch_vars) {
            we->next = db->watch_ub[lit.var_id];
            db->watch_ub[lit.var_id] = we;
        }
    }
}

uint32_t clause_db_add(ClauseDB *db, uint32_t n_lits, const Literal *lits,
                        uint32_t lbd) {
    if (n_lits == 0 || n_lits > MAX_CLAUSE_LITS) return UINT32_MAX;

    /* Grow clause pointer array if needed */
    if (db->n_clauses >= db->clauses_cap) {
        uint32_t new_cap = db->clauses_cap * 2;
        Clause **new_arr = (Clause **)realloc(db->clauses, new_cap * sizeof(Clause *));
        if (!new_arr) return UINT32_MAX;
        db->clauses = new_arr;
        db->clauses_cap = new_cap;
    }

    /* Allocate clause in arena */
    uint32_t cl_size = (uint32_t)(sizeof(Clause) + n_lits * sizeof(Literal));
    Clause *cl = (Clause *)_arena_alloc(db, cl_size, 8);
    if (!cl) return UINT32_MAX;

    cl->n_lits = n_lits;
    cl->lbd = lbd;
    cl->watch0 = 0;
    cl->watch1 = n_lits > 1 ? 1 : 0;
    memcpy((Literal *)(cl + 1), lits, n_lits * sizeof(Literal));

    uint32_t idx = db->n_clauses++;
    db->clauses[idx] = cl;

    /* Watch all literals so event-driven propagation can find this clause
     * when any of its variables' bounds change. */
    for (uint32_t i = 0; i < n_lits; i++)
        _add_watch(db, lits[i], idx);

    return idx;
}

void clause_db_gc(ClauseDB *db, uint32_t lbd_threshold) {
    /* Simple GC: mark clauses with high LBD as inactive.
     * Full compaction would require rebuilding watch lists. */
    uint32_t removed = 0;
    for (uint32_t i = 0; i < db->n_clauses; i++) {
        if (db->clauses[i] && db->clauses[i]->lbd > lbd_threshold) {
            db->clauses[i] = NULL;
            removed++;
        }
    }
    (void)removed;
}

/* ================================================================== */
/* VSIDS                                                               */
/* ================================================================== */

int vsids_init(VSIDS *vs, uint32_t n_vars) {
    memset(vs, 0, sizeof(*vs));
    vs->n_vars = n_vars;
    vs->var_inc = 1.0;
    vs->var_decay = 0.95;
    vs->activity = (double *)calloc(n_vars, sizeof(double));
    return vs->activity ? 0 : -1;
}

void vsids_destroy(VSIDS *vs) {
    free(vs->activity);
    memset(vs, 0, sizeof(*vs));
}

void vsids_bump(VSIDS *vs, uint32_t var_id) {
    if (var_id < vs->n_vars) {
        vs->activity[var_id] += vs->var_inc;
        /* Rescale if activity gets too large */
        if (vs->activity[var_id] > 1e100) {
            for (uint32_t i = 0; i < vs->n_vars; i++)
                vs->activity[i] *= 1e-100;
            vs->var_inc *= 1e-100;
        }
    }
}

void vsids_decay(VSIDS *vs) {
    vs->var_inc /= vs->var_decay;
}

uint32_t vsids_pick(const VSIDS *vs, const dvs_ctx_t *ctx) {
    uint32_t best = EXPR_NULL;
    double best_act = -1.0;

    for (uint32_t i = 0; i < vs->n_vars && i < ctx->n_vars; i++) {
        int64_t lo = var_lo64(ctx, &ctx->vars[i]);
        int64_t hi = var_hi64(ctx, &ctx->vars[i]);
        if (lo == hi) continue;  /* already assigned */
        if (vs->activity[i] > best_act) {
            best_act = vs->activity[i];
            best = i;
        }
    }
    return best;
}

/* ================================================================== */
/* LCG Context                                                         */
/* ================================================================== */

int lcg_init(LCGCtx *lcg, uint32_t n_vars) {
    memset(lcg, 0, sizeof(*lcg));

    if (clause_db_init(&lcg->clause_db, n_vars) != 0) return -1;
    if (vsids_init(&lcg->vsids, n_vars) != 0) {
        clause_db_destroy(&lcg->clause_db);
        return -1;
    }

    /* seen[] and seen_lit[] are indexed by (var_id, is_lb) pair so the
     * analyzer can track LB and UB literals on the same variable
     * independently — required for non-monotone propagators (bvand,
     * etc.) whose antecedents include both bounds on a singleton var. */
    lcg->seen = (uint8_t *)calloc(2u * n_vars, sizeof(uint8_t));
    lcg->seen_lit = (Literal *)calloc(2u * n_vars, sizeof(Literal));
    lcg->body_pos = (uint32_t *)calloc(2u * n_vars, sizeof(uint32_t));
    lcg->mk_start = (uint32_t *)calloc(2u * n_vars + 1u, sizeof(uint32_t));
    lcg->mk_cur = (uint32_t *)calloc(2u * n_vars, sizeof(uint32_t));
    lcg->learnt_cap = MAX_CLAUSE_LITS;
    lcg->learnt_buf = (Literal *)calloc(lcg->learnt_cap, sizeof(Literal));
    if (!lcg->seen || !lcg->seen_lit || !lcg->body_pos || !lcg->learnt_buf ||
        !lcg->mk_start || !lcg->mk_cur) {
        lcg_destroy(lcg);
        return -1;
    }

    lcg->enabled = 1;
    return 0;
}

uint64_t lcg_n_learnt(const LCGCtx *lcg)   { return lcg ? lcg->n_learnt : 0; }
uint64_t lcg_n_analyses(const LCGCtx *lcg) { return lcg ? lcg->n_analyses : 0; }
uint32_t lcg_n_clauses(const LCGCtx *lcg)  { return lcg ? lcg->clause_db.n_clauses : 0; }

void lcg_destroy(LCGCtx *lcg) {
    clause_db_destroy(&lcg->clause_db);
    vsids_destroy(&lcg->vsids);
    free(lcg->seen);
    free(lcg->seen_lit);
    free(lcg->body_pos);
    free(lcg->mk_ent);
    free(lcg->mk_start);
    free(lcg->mk_cur);
    free(lcg->undo_var);
    free(lcg->undo_kind);
    free(lcg->undo_val);
    free(lcg->learnt_buf);
    memset(lcg, 0, sizeof(*lcg));
}

/* Debug counters for analyzer bail-outs. Printed by smt2 frontend when
 * DV_LCG_STATS is set.  Tagged: A=other_no_explain, B=other_explain_fail,
 * C=neither_at_cur_level, D=propagator_conflict_no_var,
 * E=cannot_determine_source, F=cur_no_explain, G=cur_explain_fail,
 * H=resolution_no_explain, I=resolution_explain_fail. */
uint64_t lcg_dbg_bail[16];

static Literal _mk_lit(uint32_t var_id, uint8_t is_lb, int64_t bound) {
    Literal l;
    l.var_id = var_id;
    l.is_lb = is_lb;
    l._pad[0] = l._pad[1] = l._pad[2] = 0;
    l.bound = bound;
    return l;
}

/* Does literal `l` hold when its variable's bound (in l's direction) is v? */
static int _lit_holds(const dvs_ctx_t *ctx, Literal l, int64_t v) {
    const Variable *var = &ctx->vars[l.var_id];
    return l.is_lb ? !var_b_lt(var, v, l.bound) : !var_b_gt(var, v, l.bound);
}

static int _lit_holds_now(const dvs_ctx_t *ctx, Literal l) {
    const Variable *var = &ctx->vars[l.var_id];
    return _lit_holds(ctx, l, l.is_lb ? var_lo64(ctx, var) : var_hi64(ctx, var));
}

/* Is `a` a strictly stronger bound than `b` (same variable and direction)? */
static int _lit_stronger(const dvs_ctx_t *ctx, Literal a, Literal b) {
    const Variable *var = &ctx->vars[a.var_id];
    return a.is_lb ? var_b_gt(var, a.bound, b.bound) : var_b_lt(var, a.bound, b.bound);
}

/* Bucket the above-root bound tightenings by (variable, kind), oldest first,
 * so _lit_maker can binary-search instead of scanning the trail. The trail is
 * ordered by level, so the walk stops at the first level-0 entry. Returns -1
 * when out of memory (the caller bails to chronological backtracking). */
static int _mk_index(LCGCtx *lcg, dvs_ctx_t *ctx) {
    uint32_t ns = 2u * ctx->n_vars, n = 0;
    memset(lcg->mk_start, 0, (ns + 1u) * sizeof(uint32_t));
    for (TrailEntry *t = ctx->trail_top; t && t->decision_level > 0; t = t->prev) {
        if ((t->kind != TRAIL_LB && t->kind != TRAIL_UB) || t->var_id >= ctx->n_vars)
            continue;
        lcg->mk_start[SEEN_IX(t->var_id, t->kind == TRAIL_LB) + 1u]++;
        n++;
    }
    for (uint32_t i = 1; i <= ns; i++) lcg->mk_start[i] += lcg->mk_start[i - 1];
    if (n > lcg->mk_cap) {
        uint32_t cap = n + n / 2u + 64u;
        TrailEntry **ne = (TrailEntry **)realloc(lcg->mk_ent, cap * sizeof(TrailEntry *));
        if (!ne) return -1;
        lcg->mk_ent = ne;
        lcg->mk_cap = cap;
    }
    memcpy(lcg->mk_cur, lcg->mk_start + 1, ns * sizeof(uint32_t));
    for (TrailEntry *t = ctx->trail_top; t && t->decision_level > 0; t = t->prev) {
        if ((t->kind != TRAIL_LB && t->kind != TRAIL_UB) || t->var_id >= ctx->n_vars)
            continue;
        lcg->mk_ent[--lcg->mk_cur[SEEN_IX(t->var_id, t->kind == TRAIL_LB)]] = t;
    }
    return 0;
}

/* The trail entry that made `l` true: the latest tightening of its bound
 * whose old value did not satisfy it. NULL if `l` already held at the root
 * (in the initial domain or by a level-0 tightening); the caller drops root
 * literals. A bound only tightens, so along its bucket "the old value
 * satisfies l" is false then true: binary-search the boundary. (A linear
 * trail scan per literal made one analysis quadratic in the trail, and a
 * bound climbing one value per round makes that tens of thousands long.) */
static TrailEntry *_lit_maker(const LCGCtx *lcg, dvs_ctx_t *ctx, Literal l) {
    uint32_t slot = SEEN_IX(l.var_id, l.is_lb);
    uint32_t lo = lcg->mk_start[slot], hi = lcg->mk_start[slot + 1u];
    /* Invariant: entries before lo do not satisfy l; entries from hi do. */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (_lit_holds(ctx, l, lcg->mk_ent[mid]->old_value)) hi = mid;
        else lo = mid + 1u;
    }
    return lo > lcg->mk_start[slot] ? lcg->mk_ent[lo - 1u] : NULL;
}

/* The guard of a guard-gated propagator (compile gates the branches of a
 * constraint-level if/else, soft constraints and array-select cases this
 * way), or EXPR_NULL. Its narrowings hold only while the guard is 1, which
 * the explainers do not cite: the analysis adds `guard >= 1` itself, or the
 * learnt clause would hold the gated constraint unconditionally. */
static uint32_t _prop_guard(const dvs_ctx_t *ctx, const Propagator *p) {
    if (!ctx->prop_guard_vars || p->prop_id >= ctx->n_prop_refs_capacity)
        return EXPR_NULL;
    return ctx->prop_guard_vars[p->prop_id];
}

/* Propagators whose every narrowing is a function of the OTHER variables'
 * bounds alone (the new bound is the other side's bound, an interval sum, or
 * the guard), so their explanations are complete without the narrowed
 * variable's own previous bound. Every other explainer gets that bound added.
 * Leaving one off this list only costs learning strength; putting one on it
 * wrongly costs soundness. */
static int _explains_without_own_bound(const Propagator *p) {
    return p->explain == explain_bounds_le || p->explain == explain_bounds_lt
        || p->explain == explain_bounds_eq || p->explain == explain_bounds_add
        || p->explain == explain_sum_eq    || p->explain == explain_implication
        || p->explain == explain_ite_value || p->explain == explain_reification;
}

#ifdef DVS_STEP_CHECK
void dvs_step_check_explanation(dvs_ctx_t *ctx, const char *who,
                                const Literal *ante, uint32_t n, Literal lit);
#endif

/* Set a bound directly, as trail_backtrack does. */
static void _set_bound(dvs_ctx_t *ctx, uint32_t var_id, uint8_t kind, int64_t val) {
    Variable *v = &ctx->vars[var_id];
    if (VAR_IS_TIER0(v->flags)) {
        if (kind == TRAIL_LB) v->lo = (int32_t)val; else v->hi = (int32_t)val;
    } else if (VAR_IS_TIER1(v->flags)) {
        WideBounds64 *wb = (WideBounds64 *)dvs_pool_ptr(&ctx->pool, v->holes_offset);
        if (kind == TRAIL_LB) wb->lo = val; else wb->hi = val;
    }
}

/* Rewind the domains to the state just before trail entry `e` was made, so
 * its propagator's explainer reads the bounds the propagator read. `*applied`
 * is the newest entry still in force; everything newer than it was undone by
 * an earlier call. Explainers read CURRENT bounds, and at a conflict those
 * include bounds derived from the very step being explained: `v2 <= 45`
 * explained by `v8 <= 12` and `v8 <= 12` by `v2 == 45`, each locally valid,
 * the learnt clause not (B56). Every rewound bound is logged and restored by
 * _restore_rewound before analysis returns. Returns -1 when out of memory. */
static int _rewind_before(LCGCtx *lcg, dvs_ctx_t *ctx, TrailEntry **applied,
                          TrailEntry *e) {
    while (*applied && *applied != e->prev) {
        TrailEntry *t = *applied;
        if (t->kind == TRAIL_LB || t->kind == TRAIL_UB) {
            if (lcg->n_undo == lcg->undo_cap) {
                uint32_t cap = lcg->undo_cap ? 2u * lcg->undo_cap : 256u;
                uint32_t *nv = (uint32_t *)realloc(lcg->undo_var, cap * sizeof *nv);
                if (nv) lcg->undo_var = nv;
                uint8_t *nk = (uint8_t *)realloc(lcg->undo_kind, cap * sizeof *nk);
                if (nk) lcg->undo_kind = nk;
                int64_t *nb = (int64_t *)realloc(lcg->undo_val, cap * sizeof *nb);
                if (nb) lcg->undo_val = nb;
                if (!nv || !nk || !nb) return -1;
                lcg->undo_cap = cap;
            }
            const Variable *v = &ctx->vars[t->var_id];
            lcg->undo_var[lcg->n_undo]  = t->var_id;
            lcg->undo_kind[lcg->n_undo] = t->kind;
            lcg->undo_val[lcg->n_undo]  = t->kind == TRAIL_LB ? var_lo64(ctx, v) : var_hi64(ctx, v);
            lcg->n_undo++;
            _set_bound(ctx, t->var_id, t->kind, t->old_value);
        }
        *applied = t->prev;
    }
    return 0;
}

static void _restore_rewound(LCGCtx *lcg, dvs_ctx_t *ctx) {
    while (lcg->n_undo > 0) {
        lcg->n_undo--;
        _set_bound(ctx, lcg->undo_var[lcg->n_undo], lcg->undo_kind[lcg->n_undo],
                   lcg->undo_val[lcg->n_undo]);
    }
}

static int _analyze(LCGCtx *lcg, dvs_ctx_t *ctx, Literal *out_lits, uint32_t *out_n,
                    uint32_t *out_bt, uint32_t *out_lbd);

int lcg_analyze_conflict(LCGCtx *lcg, dvs_ctx_t *ctx,
                          Literal *out_lits, uint32_t *out_n,
                          uint32_t *out_bt, uint32_t *out_lbd) {
    if (!lcg || !lcg->enabled || !ctx) return -1;
    lcg->n_undo = 0;
    int rc = _analyze(lcg, ctx, out_lits, out_n, out_bt, out_lbd);
    _restore_rewound(lcg, ctx);
    return rc;
}

static int _analyze(LCGCtx *lcg, dvs_ctx_t *ctx, Literal *out_lits, uint32_t *out_n,
                    uint32_t *out_bt, uint32_t *out_lbd) {

    uint32_t cur_level = ctx->decision_level;
    if (cur_level == 0) {
        *out_n = 0;
        *out_bt = 0;
        return 0;
    }

    if (_tron()) {
        fprintf(stderr,
            "[lcg-trace] === analyze cur_level=%u conflict_prop=%s ===\n",
            cur_level, _pname(ctx, ctx->conflict_prop_ref));
    }

    memset(lcg->seen, 0, 2u * ctx->n_vars * sizeof(uint8_t));
    memset(lcg->seen_lit, 0, 2u * ctx->n_vars * sizeof(Literal));
    memset(lcg->body_pos, 0, 2u * ctx->n_vars * sizeof(uint32_t));
    if (_mk_index(lcg, ctx) != 0) return -1;

    uint32_t n_at_cur_level = 0;
    uint32_t learnt_idx = 0;
    uint32_t bt_level = 0;
    int64_t  force_slot = -1;

    /* Step 1: Seed the conflict.
     *
     * Two types of conflict:
     * (a) Empty domain: some variable has lo > hi.
     * (b) Propagator conflict: a propagator returned PROP_CONFLICT
     *     without emptying any domain (e.g., NoOverlap2D geometric infeasibility).
     *
     * For (a), seed with the trail entries that tightened the conflicting var.
     * For (b), use the conflicting propagator's explain to get the conflict clause.
     */

    /* Check for empty-domain conflict */
    uint32_t conflict_var = EXPR_NULL;
    for (uint32_t i = 0; i < ctx->n_vars; i++) {
        const Variable *vi = &ctx->vars[i];
        /* Sign-aware: an unsigned width-64 var's FULL domain is lo=0, hi=-1
         * (the bit pattern 2^64-1), which a bare `lo > hi` calls empty — this
         * loop would then pick an unconstrained variable as the conflict var
         * and explain the conflict from the wrong place. Same 2^63 cliff as
         * B22/B26; mirrors the guard in dvs_solver_solve. */
        if (var_b_gt(vi, var_lo64(ctx, vi), var_hi64(ctx, vi))) {
            conflict_var = i;
            break;
        }
    }

    /* The working set is a conjunction of literals that together imply the
     * conflict. Every step must keep that true: the learnt clause is its
     * negation, and a clause stronger than what the constraints imply
     * removes solutions (wrong `unsat`).
     *
     * A literal is attributed to the trail entry that MADE it true -- the
     * latest tightening of its bound whose old value did not yet satisfy it
     * -- not simply the latest tightening, which may have pushed the bound
     * further for an unrelated reason. Literals made at the current level go
     * into seen[] (one per variable and direction, keeping the strongest
     * bound) to be resolved; earlier-level literals go into the clause body
     * (body_pos[] keeps the strongest per slot); root literals are dropped.
     * A literal that does not hold at all means an explainer cited a bound it
     * has no right to: bail to chronological backtracking rather than learn
     * from it. So does running out of clause space -- dropping a literal
     * would make the clause stronger than the truth. */
    #define ADD_EXPL_LIT(lit) do {                                         \
        Literal _l = (lit);                                                \
        _l._pad[0] = _l._pad[1] = _l._pad[2] = 0;                          \
        if (_l.var_id >= ctx->n_vars) { lcg_dbg_bail[11]++; return -1; }   \
        if (!_lit_holds_now(ctx, _l)) { lcg_dbg_bail[11]++; return -1; }   \
        TrailEntry *_m = _lit_maker(lcg, ctx, _l);                         \
        if (!_m || _m->decision_level == 0) break;                         \
        uint32_t _slot = SEEN_IX(_l.var_id, _l.is_lb);                     \
        vsids_bump(&lcg->vsids, _l.var_id);                                \
        if (_m->decision_level == cur_level) {                             \
            if (!lcg->seen[_slot]) {                                       \
                lcg->seen[_slot] = 1;                                      \
                lcg->seen_lit[_slot] = _l;                                 \
                n_at_cur_level++;                                          \
            } else if (_lit_stronger(ctx, _l, lcg->seen_lit[_slot])) {     \
                lcg->seen_lit[_slot] = _l;                                 \
            }                                                              \
        } else {                                                           \
            uint32_t _p = lcg->body_pos[_slot];                            \
            if (_p) {                                                      \
                Literal _old = literal_negate(lcg->learnt_buf[_p - 1]);    \
                if (_lit_stronger(ctx, _l, _old))                          \
                    lcg->learnt_buf[_p - 1] = literal_negate(_l);          \
            } else {                                                       \
                if (learnt_idx >= lcg->learnt_cap) {                       \
                    lcg_dbg_bail[12]++; return -1;                         \
                }                                                          \
                lcg->learnt_buf[learnt_idx++] = literal_negate(_l);        \
                lcg->body_pos[_slot] = learnt_idx;                         \
            }                                                              \
            if (_m->decision_level > bt_level)                             \
                bt_level = _m->decision_level;                             \
        }                                                                  \
    } while (0)

    if (conflict_var != EXPR_NULL) {
        if (_tron()) {
            fprintf(stderr,
                "[lcg-trace] empty-domain conflict on v%u (lo>%ld hi<%ld)\n",
                conflict_var,
                (long)var_lo64(ctx, &ctx->vars[conflict_var]),
                (long)var_hi64(ctx, &ctx->vars[conflict_var]));
        }
        /* lo > hi. The bound tightened last is the one that crossed: the
         * conflict is "the other bound, and the crossing bound pushed just
         * past it". Seed with that weakest crossing literal and force it to
         * be resolved: it is the propagation that failed, not a candidate
         * UIP. Left unresolved it can become the UIP, and when the other
         * bound holds at root the learnt clause (the other bound itself) is
         * already true -- it excludes nothing and the search re-derives the
         * same conflict for ever. */
        const Variable *cv = &ctx->vars[conflict_var];
        int64_t clo = var_lo64(ctx, cv), chi = var_hi64(ctx, cv);
        int ub_crossed = 1;
        for (TrailEntry *t = ctx->trail_top; t; t = t->prev) {
            if (t->var_id != conflict_var) continue;
            if (t->kind == TRAIL_LB) { ub_crossed = 0; break; }
            if (t->kind == TRAIL_UB) break;
        }
        Literal llb, lub;
        if (ub_crossed) {
            llb = _mk_lit(conflict_var, 1, clo);
            lub = _mk_lit(conflict_var, 0, (int64_t)((uint64_t)clo - 1u));
        } else {
            llb = _mk_lit(conflict_var, 1, (int64_t)((uint64_t)chi + 1u));
            lub = _mk_lit(conflict_var, 0, chi);
        }
        ADD_EXPL_LIT(llb);
        ADD_EXPL_LIT(lub);
        if (n_at_cur_level == 0) { lcg_dbg_bail[2]++; return -1; }
        force_slot = (int64_t)SEEN_IX(conflict_var, ub_crossed ? 0 : 1);
    } else if (ctx->conflict_clause_idx != EXPR_NULL &&
               ctx->conflict_clause_idx < lcg->clause_db.n_clauses &&
               lcg->clause_db.clauses[ctx->conflict_clause_idx]) {
        /* Clause conflict: every literal of a learnt clause went false.
         * No domain is empty, and conflict_prop_ref is whatever propagator
         * fired last (or a stale one from an earlier conflict) -- seeding
         * from it builds the learnt clause from unrelated bounds, which
         * produced bogus units and wrong `unsat` answers. The conflict's
         * antecedents are exactly the bounds that falsify the clause:
         * for each literal (v >= b) the current v.ub (< b), for each
         * (v <= b) the current v.lb (> b). */
        Clause  *ccl   = lcg->clause_db.clauses[ctx->conflict_clause_idx];
        Literal *clits = (Literal *)(ccl + 1);
        if (_tron())
            fprintf(stderr, "[lcg-trace] clause-conflict seed from clause %u "
                    "(%u lits)\n", ctx->conflict_clause_idx, ccl->n_lits);
        for (uint32_t i = 0; i < ccl->n_lits; i++) {
            uint32_t vid = clits[i].var_id;
            if (vid >= ctx->n_vars) { lcg_dbg_bail[3]++; return -1; }
            ADD_EXPL_LIT(literal_negate(clits[i]));
        }
        ctx->conflict_clause_idx = EXPR_NULL;
        if (n_at_cur_level == 0) {
            lcg_dbg_bail[3]++; return -1;
        }
    } else if (ctx->conflict_prop_ref != EXPR_NULL) {
        if (_tron()) {
            Propagator *cp_ = (Propagator *)dvs_pool_ptr(
                &ctx->pool, ctx->conflict_prop_ref);
            fprintf(stderr,
                "[lcg-trace] prop-conflict seed from %s (watch-vars)\n",
                prop_fire_name(cp_->fire));
        }
        /* Propagator-conflict path: a propagator's fire returned
         * PROP_CONFLICT without any var hitting lo > hi (e.g.
         * bounds_eq saw x ∩ y = ∅ before tightening).  Build the
         * antecedent set from the propagator's current watch-var
         * bounds: the conjunction of these current LB/UB literals
         * implies the conflict.  The learnt clause is their negation,
         * which says "at least one of these bounds must change". */
        Propagator *cp = (Propagator *)dvs_pool_ptr(
            &ctx->pool, ctx->conflict_prop_ref);
        uint32_t nw;
        const uint32_t *wv = prop_watched_vars(cp, &nw);
        if (nw == 0) {
            lcg_dbg_bail[3]++; return -1;
        }
        for (uint32_t i = 0; i < nw; i++) {
            uint32_t vid = wv[i];
            if (vid >= ctx->n_vars) continue;
            if (_tron()) {
                _trace_lit("  seed", _mk_lit(vid, 1, var_lo64(ctx, &ctx->vars[vid])));
                _trace_lit("  seed", _mk_lit(vid, 0, var_hi64(ctx, &ctx->vars[vid])));
            }
            ADD_EXPL_LIT(_mk_lit(vid, 1, var_lo64(ctx, &ctx->vars[vid])));
            ADD_EXPL_LIT(_mk_lit(vid, 0, var_hi64(ctx, &ctx->vars[vid])));
        }
        {   /* A guard-gated propagator can only fail while its guard is 1. */
            uint32_t gid = _prop_guard(ctx, cp);
            if (gid != EXPR_NULL) ADD_EXPL_LIT(_mk_lit(gid, 1, 1));
        }
        /* If nothing got added at current level, we can't form a UIP.
         * Fall back to chronological backtracking. */
        if (n_at_cur_level == 0) {
            lcg_dbg_bail[3]++; return -1;
        }
    } else {
        lcg_dbg_bail[4]++;
        return -1;
    }

    /* Step 2: Resolution loop (1UIP). Walk the trail from the top and
     * replace each current-level literal by the reasons of the entry that
     * made it true, until only one current-level literal remains. */
    TrailEntry *e = ctx->trail_top;
    TrailEntry *applied = ctx->trail_top;   /* see _rewind_before */
    while ((n_at_cur_level > 1 ||
            (force_slot >= 0 && lcg->seen[force_slot])) && e) {
        uint8_t e_is_lb = (e->kind == TRAIL_LB) ? 1 : 0;
        uint32_t e_slot = SEEN_IX(e->var_id, e_is_lb);
        if (e->decision_level != cur_level ||
            (e->kind != TRAIL_LB && e->kind != TRAIL_UB) ||
            !lcg->seen[e_slot] ||
            _lit_holds(ctx, lcg->seen_lit[e_slot], e->old_value)) {
            /* Not at this level, not a bound, not needed, or the needed
             * literal already held before this entry (a later, stronger
             * tightening of the same bound). */
            e = e->prev;
            continue;
        }
        Literal need = lcg->seen_lit[e_slot];

        if (e->prop_ref == EXPR_NULL && !(e->flags & TRAIL_FLAG_FROM_CLAUSE)) {
            /* Decision at current level: this becomes the 1UIP.
             * Stop resolution -- the remaining decisions at this level
             * that can't be resolved ARE the UIP. */
            n_at_cur_level = 1;  /* force loop exit, this literal is UIP */
            force_slot = -1;
            continue;
        }

        lcg->seen[e_slot] = 0;
        n_at_cur_level--;

        /* Clause-reason resolution: when a learnt clause unit-propagated
         * this entry, prop_ref holds the clause index and the antecedents
         * are the negations of the clause's other literals (which were
         * false at unit-prop time). */
        if (e->flags & TRAIL_FLAG_FROM_CLAUSE) {
            uint32_t clause_idx = e->prop_ref;
            ClauseDB *db = &lcg->clause_db;
            if (clause_idx >= db->n_clauses || !db->clauses[clause_idx]) {
                lcg_dbg_bail[7]++; return -1;
            }
            Clause *cl = db->clauses[clause_idx];
            Literal *lits = (Literal *)(cl + 1);
            uint32_t n = cl->n_lits;
            if (_tron()) {
                fprintf(stderr,
                    "[lcg-trace] resolve  clause=%u v%u %s=%ld lvl=%u (old=%ld) -> %u lits\n",
                    clause_idx, e->var_id, e_is_lb ? "lb" : "ub",
                    (long)need.bound, e->decision_level, (long)e->old_value, n - 1);
            }
            for (uint32_t i = 0; i < n; i++) {
                /* Skip the unit literal: that's the entry we're
                 * resolving. Compare by (var_id, is_lb) — the unit
                 * is the one this trail entry tightened. */
                if (lits[i].var_id == e->var_id && lits[i].is_lb == e_is_lb)
                    continue;
                Literal neg = literal_negate(lits[i]);
                if (_tron()) _trace_lit("  ante", neg);
                ADD_EXPL_LIT(neg);
            }
            e = e->prev;
            continue;
        }

        /* Resolve through propagator explanation, asking for exactly the
         * literal the working set needs. */
        Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool, e->prop_ref);
        if (!p->explain) { lcg_dbg_bail[7]++; return -1; }
        if (_rewind_before(lcg, ctx, &applied, e) != 0) { lcg_dbg_bail[14]++; return -1; }
        Explanation expl;
        int rc = p->explain(p, ctx, e->var_id, e_is_lb, need.bound, &expl);
        if (rc != 0) { lcg_dbg_bail[8]++; return -1; }
        if (_tron()) {
            fprintf(stderr,
                "[lcg-trace] resolve  prop=%s v%u %s=%ld lvl=%u (old=%ld) -> %u lits\n",
                prop_fire_name(p->fire), e->var_id, e_is_lb ? "lb" : "ub",
                (long)need.bound, e->decision_level, (long)e->old_value,
                expl.n_lits);
            for (uint32_t i = 0; i < expl.n_lits; i++)
                _trace_lit("  ante", expl.lits[i]);
        }
#ifdef DVS_STEP_CHECK
        {   /* Check the full antecedent set this step relies on. */
            Literal chk[MAX_EXPLAIN_LITS + 2];
            uint32_t nc = 0;
            for (uint32_t i = 0; i < expl.n_lits && nc < MAX_EXPLAIN_LITS; i++)
                chk[nc++] = expl.lits[i];
            if (!_explains_without_own_bound(p))
                chk[nc++] = _mk_lit(e->var_id, e_is_lb, e->old_value);
            if (_prop_guard(ctx, p) != EXPR_NULL)
                chk[nc++] = _mk_lit(_prop_guard(ctx, p), 1, 1);
            dvs_step_check_explanation(ctx, prop_fire_name(p->fire), chk, nc, need);
        }
#endif
        for (uint32_t i = 0; i < expl.n_lits; i++)
            ADD_EXPL_LIT(expl.lits[i]);
        {   /* A guard-gated propagator narrows only while its guard is 1. */
            uint32_t gid = _prop_guard(ctx, p);
            if (gid != EXPR_NULL) ADD_EXPL_LIT(_mk_lit(gid, 1, 1));
        }
        /* Most narrowings also rest on the variable's own previous bound:
         * `x != 5` raises x.lo from 5 to 6 only because x.lo was 5. An
         * explanation that leaves it out claims the bound follows from the
         * other variables alone, and the learnt clause then removes real
         * solutions. Add it for every propagator not known to narrow from
         * the other variables only. */
        if (!_explains_without_own_bound(p)) {
            Literal own = _mk_lit(e->var_id, e_is_lb, e->old_value);
            if (_tron()) _trace_lit("  own ", own);
            ADD_EXPL_LIT(own);
        }
        e = e->prev;
        continue;
    }

    #undef ADD_EXPL_LIT

    /* Resolving the crossing literal can leave nothing at this level (the
     * failed propagation read only earlier-level bounds). There is no UIP,
     * so the clause would not be asserting: backtrack chronologically. */
    if (n_at_cur_level == 0) { lcg_dbg_bail[13]++; return -1; }

    /* Step 3: Emit the remaining seen literals at cur_level. The first
     * one walked back from trail_top is the 1UIP (the asserting literal,
     * placed at learnt_buf[0]). Any other still-seen slots are *also*
     * antecedents at cur_level — they typically come from a singleton
     * decision "v = c" which sets BOTH (v, LB=c) and (v, UB=c) as
     * separate trail entries. Both must be negated into the learnt
     * clause for it to correctly mean "v != c". */
    /* Only cur_level seen slots remain to be emitted here. Earlier-level
     * antecedents were already pushed onto learnt_buf as body literals
     * inside ADD_EXPL_LIT; re-emitting them here would double-count.
     * Multiple cur_level slots can be seen when a singleton decision
     * (v=c) creates both (v,LB=c) and (v,UB=c) trail entries — for the
     * learnt clause to mean "v != c" rather than just "v >= c+1", both
     * bound negations must be in the clause. */
    {
        int uip_set = 0;
        e = ctx->trail_top;
        while (e) {
            if (e->decision_level != cur_level) { e = e->prev; continue; }
            uint32_t e_slot = SEEN_IX(e->var_id, (e->kind == TRAIL_LB) ? 1 : 0);
            if (lcg->seen[e_slot]) {
                Literal lit = lcg->seen_lit[e_slot];
                Literal neg = literal_negate(lit);
                if (!uip_set) {
                    if (learnt_idx < lcg->learnt_cap) {
                        for (uint32_t j = learnt_idx; j > 0; j--)
                            lcg->learnt_buf[j] = lcg->learnt_buf[j - 1];
                        lcg->learnt_buf[0] = neg;
                        learnt_idx++;
                    }
                    uip_set = 1;
                } else if (learnt_idx < lcg->learnt_cap) {
                    lcg->learnt_buf[learnt_idx++] = neg;
                }
                lcg->seen[e_slot] = 0;
            }
            e = e->prev;
        }
    }

    /* Self-subsumption clause minimization. Originally implemented to
     * recover the 10 fixtures Phase 2 lost to longer learnt clauses,
     * but: (a) the implementation is sound, (b) it doesn't recover any
     * fixtures (still 96/22 vs phase-2 baseline 98/20), (c) the trail
     * walks per body literal per antecedent are slow enough that wall
     * time grew from 102s → 230s on cross-check. Disabled by default;
     * enable with DV_LCG_MIN=1 to experiment. A proper implementation
     * needs O(1) "literal in learnt clause" lookups (hash or per-var
     * direct-index) rather than the linear scan we do here. */
    if (getenv("DV_LCG_MIN") && learnt_idx > 1) {
        /* Helper macro: is antecedent literal `a` implied by the
         * negation of some body literal in [0..learnt_idx)? For BV
         * bounds, ~L implies `a` iff a and ~L are same-direction on
         * the same var AND ~L's bound is at least as strong as a's.
         *
         * - body L = "v <= u"  →  ~L = "v >= u+1".  Implies a="v>=b"
         *   iff u+1 >= b iff u >= b-1.
         * - body L = "v >= u"  →  ~L = "v <= u-1".  Implies a="v<=b"
         *   iff u-1 <= b iff u <= b+1.
         */
        /* Skip _li == read so L_i can't be used to subsume its own
         * antecedent (circular). */
        #define IMPLIED_BY_LEARNT(a, skip_idx) \
            dvs__implied_by_learnt(lcg, learnt_idx, (a), (skip_idx))

        uint32_t write = 1; /* UIP at [0], always keep */
        for (uint32_t read = 1; read < learnt_idx; read++) {
            Literal L = lcg->learnt_buf[read];
            uint8_t ant_is_lb = !L.is_lb;
            uint8_t ant_kind  = ant_is_lb ? TRAIL_LB : TRAIL_UB;

            /* Find the trail entry that makes ant=~L currently true. */
            TrailEntry *te = NULL;
            for (TrailEntry *t = ctx->trail_top; t; t = t->prev) {
                if (t->var_id == L.var_id && t->kind == ant_kind) {
                    te = t; break;
                }
            }

            int redundant = 0;
            if (te && te->decision_level > 0) {
                if (te->flags & TRAIL_FLAG_FROM_CLAUSE) {
                    uint32_t cidx = te->prop_ref;
                    ClauseDB *db = &lcg->clause_db;
                    if (cidx < db->n_clauses && db->clauses[cidx]) {
                        Clause *cl = db->clauses[cidx];
                        Literal *clits = (Literal *)(cl + 1);
                        redundant = 1;
                        for (uint32_t i = 0; i < cl->n_lits; i++) {
                            if (clits[i].var_id == L.var_id &&
                                clits[i].is_lb == ant_is_lb) continue;
                            Literal neg = literal_negate(clits[i]);
                            if (neg.var_id >= ctx->n_vars) { redundant = 0; break; }
                            if (IMPLIED_BY_LEARNT(neg, read)) continue;
                            uint8_t nk = neg.is_lb ? TRAIL_LB : TRAIL_UB;
                            uint16_t nlvl = 0;
                            int found = 0;
                            for (TrailEntry *t = ctx->trail_top; t; t = t->prev) {
                                if (t->var_id == neg.var_id && t->kind == nk) {
                                    nlvl = t->decision_level; found = 1; break;
                                }
                            }
                            if (found && nlvl == 0) continue;
                            redundant = 0; break;
                        }
                    }
                } else if (te->prop_ref != EXPR_NULL) {
                    Propagator *p = (Propagator *)dvs_pool_ptr(&ctx->pool, te->prop_ref);
                    if (p->explain) {
                        Explanation expl;
                        int64_t bv = (ant_kind == TRAIL_LB)
                            ? var_lo64(ctx, &ctx->vars[L.var_id])
                            : var_hi64(ctx, &ctx->vars[L.var_id]);
                        if (p->explain(p, ctx, L.var_id, ant_is_lb, bv, &expl) == 0) {
                            redundant = 1;
                            for (uint32_t i = 0; i < expl.n_lits; i++) {
                                Literal a = expl.lits[i];
                                if (a.var_id >= ctx->n_vars) { redundant = 0; break; }
                                if (IMPLIED_BY_LEARNT(a, read)) continue;
                                uint8_t ak = a.is_lb ? TRAIL_LB : TRAIL_UB;
                                uint16_t alvl = 0;
                                int found = 0;
                                for (TrailEntry *t = ctx->trail_top; t; t = t->prev) {
                                    if (t->var_id == a.var_id && t->kind == ak) {
                                        alvl = t->decision_level; found = 1; break;
                                    }
                                }
                                if (found && alvl == 0) continue;
                                redundant = 0; break;
                            }
                        }
                    }
                }
            }

            if (!redundant) {
                lcg->learnt_buf[write++] = L;
            } else if (_tron()) {
                fprintf(stderr, "[lcg-trace]   drop  v%u %s %lld\n",
                        L.var_id, L.is_lb ? ">=" : "<=", (long long)L.bound);
            }
        }
        #undef IMPLIED_BY_LEARNT

        if (_tron() && write < learnt_idx) {
            fprintf(stderr, "[lcg-trace] minimize %u -> %u lits\n",
                    learnt_idx, write);
        }
        if (write < learnt_idx) {
            learnt_idx = write;
            /* Recompute bt_level since the literal that contributed the
             * old max may have been dropped. */
            bt_level = 0;
            for (uint32_t i = 1; i < learnt_idx; i++) {
                Literal L = lcg->learnt_buf[i];
                if (L.var_id >= ctx->n_vars) continue;
                uint8_t look = L.is_lb ? TRAIL_UB : TRAIL_LB;
                for (TrailEntry *t = ctx->trail_top; t; t = t->prev) {
                    if (t->var_id == L.var_id && t->kind == look) {
                        if (t->decision_level > bt_level) bt_level = t->decision_level;
                        break;
                    }
                }
            }
        }
    }

    /* Compute LBD (Literal Block Distance): the number of distinct
     * decision levels among the clause's literals. Standard CDCL
     * quality measure — clauses with LBD ≤ 2 are "glue" and almost
     * always kept, larger LBD signals lower utility. */
    uint32_t lbd = 0;
    {
        /* Tiny set of seen levels, capped at 32. Beyond that we just
         * stop counting — clauses with > 32 distinct levels are very
         * low-quality regardless. */
        uint16_t seen_levels[32];
        uint32_t n_seen = 0;
        for (uint32_t i = 0; i < learnt_idx; i++) {
            Literal lit = lcg->learnt_buf[i];
            if (lit.var_id >= ctx->n_vars) continue;
            uint8_t k = lit.is_lb ? TRAIL_LB : TRAIL_UB;
            /* For the UIP literal, the slot we want is the negation's
             * level (the one it'll fire at). For body literals, same
             * thing — they're negations of antecedents whose level we
             * captured during ADD_EXPL_LIT. Just look up the most
             * recent matching entry on the OPPOSITE kind. */
            uint8_t look = (k == TRAIL_LB) ? TRAIL_UB : TRAIL_LB;
            uint16_t lvl = 0;
            for (TrailEntry *t = ctx->trail_top; t; t = t->prev) {
                if (t->var_id == lit.var_id && t->kind == look) {
                    lvl = t->decision_level; break;
                }
            }
            if (lvl == 0) continue;
            int hit = 0;
            for (uint32_t s = 0; s < n_seen; s++) {
                if (seen_levels[s] == lvl) { hit = 1; break; }
            }
            if (!hit && n_seen < 32) seen_levels[n_seen++] = lvl;
        }
        lbd = n_seen;
    }

    /* A tautological clause (it holds `v >= a` and `v <= b` with a <= b+1)
     * excludes nothing: after the backjump the search re-makes the same
     * decision, hits the same conflict and learns the same clause again --
     * a livelock that burns the whole CDCL budget. It arises when an
     * explainer cites a bound that only became true after the propagation
     * it explains (explainers read CURRENT bounds). Fall back to the
     * chronological path, which always excludes the failed decision. */
    for (uint32_t i = 0; i < learnt_idx; i++) {
        Literal A = lcg->learnt_buf[i];
        if (!A.is_lb || A.var_id >= ctx->n_vars) continue;
        const Variable *tv = &ctx->vars[A.var_id];
        for (uint32_t j = 0; j < learnt_idx; j++) {
            Literal B = lcg->learnt_buf[j];
            if (B.var_id != A.var_id || B.is_lb) continue;
            int64_t a = (int64_t)A.bound, b = (int64_t)B.bound;
            if (!var_b_gt(tv, a, b) ||
                (uint64_t)a == (uint64_t)b + 1u) {
                if (_tron())
                    fprintf(stderr, "[lcg-trace] tautological learnt clause"
                            " on v%u -> chronological fallback\n", A.var_id);
                lcg_dbg_bail[9]++;
                return -1;
            }
        }
    }

    /* Re-deriving a clause already in the database is the same livelock in
     * another guise: that clause was already present and still failed to
     * keep the search out of this conflict, i.e. it is not asserting at the
     * backjump level (typically a hole `v <= a \/ v >= b` on the decided
     * variable). Adding it again and backjumping only undoes the progress
     * the chronological path made. Check the most recent clauses. */
    {
        ClauseDB *db = &lcg->clause_db;
        uint32_t lim = db->n_clauses < 8u ? db->n_clauses : 8u;
        for (uint32_t k = 0; k < lim; k++) {
            const Clause *cl = db->clauses[db->n_clauses - 1u - k];
            if (!cl || cl->n_lits != learnt_idx) continue;
            const Literal *cls = (const Literal *)(cl + 1);
            int same = 1;
            for (uint32_t i = 0; i < learnt_idx && same; i++) {
                Literal A = lcg->learnt_buf[i];
                int hit = 0;
                for (uint32_t j = 0; j < cl->n_lits; j++) {
                    if (cls[j].var_id == A.var_id && cls[j].is_lb == A.is_lb &&
                        (int64_t)cls[j].bound == (int64_t)A.bound) {
                        hit = 1; break;
                    }
                }
                same = hit;
            }
            if (same) {
                if (_tron())
                    fprintf(stderr, "[lcg-trace] learnt clause duplicates"
                            " clause %u -> chronological fallback\n",
                            db->n_clauses - 1u - k);
                lcg_dbg_bail[10]++;
                return -1;
            }
        }
    }

    /* Output */
    *out_n = learnt_idx;
    *out_bt = bt_level;
    if (out_lbd) *out_lbd = lbd;
    if (out_lits && learnt_idx > 0)
        memcpy(out_lits, lcg->learnt_buf, learnt_idx * sizeof(Literal));

    if (_tron()) {
        fprintf(stderr,
            "[lcg-trace] learnt (%u lits, lbd=%u) bt=%u:\n",
            learnt_idx, lbd, bt_level);
        for (uint32_t i = 0; i < learnt_idx; i++)
            _trace_lit(i == 0 ? "  UIP " : "  body", lcg->learnt_buf[i]);
        fprintf(stderr, "[lcg-trace] ===\n");
    }

    vsids_decay(&lcg->vsids);
    lcg->n_analyses++;

    return 0;
}
