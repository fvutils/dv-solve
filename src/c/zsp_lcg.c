#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsp_lcg.h"
#include "zsp_ctx.h"
#include "zsp_explain.h"
#include "zsp_propagator.h"
#include "zsp_trail.h"

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

static const char *_pname(SolveCtx *ctx, uint32_t prop_ref) {
    if (prop_ref == EXPR_NULL) return "<decision>";
    Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, prop_ref);
    return prop_fire_name(p->fire);
}

static void _trace_lit(const char *prefix, Literal lit) {
    fprintf(stderr, "[lcg-trace] %s v%u %s %d\n",
            prefix, lit.var_id, lit.is_lb ? ">=" : "<=", lit.bound);
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

uint32_t vsids_pick(const VSIDS *vs, const SolveCtx *ctx) {
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
    lcg->learnt_cap = MAX_CLAUSE_LITS;
    lcg->learnt_buf = (Literal *)calloc(lcg->learnt_cap, sizeof(Literal));
    if (!lcg->seen || !lcg->learnt_buf) {
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
    free(lcg->learnt_buf);
    memset(lcg, 0, sizeof(*lcg));
}

/* Debug counters for analyzer bail-outs. Printed by smt2 frontend when
 * DV_LCG_STATS is set.  Tagged: A=other_no_explain, B=other_explain_fail,
 * C=neither_at_cur_level, D=propagator_conflict_no_var,
 * E=cannot_determine_source, F=cur_no_explain, G=cur_explain_fail,
 * H=resolution_no_explain, I=resolution_explain_fail. */
uint64_t lcg_dbg_bail[16];

int lcg_analyze_conflict(LCGCtx *lcg, SolveCtx *ctx,
                          Literal *out_lits, uint32_t *out_n,
                          uint32_t *out_bt) {
    if (!lcg || !lcg->enabled || !ctx) return -1;

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

    uint32_t n_at_cur_level = 0;
    uint32_t learnt_idx = 0;
    uint32_t bt_level = 0;

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
        int64_t lo = var_lo64(ctx, &ctx->vars[i]);
        int64_t hi = var_hi64(ctx, &ctx->vars[i]);
        if (lo > hi) {
            conflict_var = i;
            break;
        }
    }

    /* Helper: add explanation literals to the working set */
    /* Helper: add a literal from an explanation to the working set.
     * Stores the literal so it can be used for UIP / clause body. */
    #define ADD_EXPL_LIT(lit) do {                                   \
        uint32_t _vid = (lit).var_id;                                \
        uint32_t _slot = SEEN_IX(_vid, (lit).is_lb);                 \
        /* Antecedent literals should be currently TRUE.  Process    \
         * them to find their decision level and resolve or add to   \
         * the learnt clause body. Skip if already seen or invalid.  \
         * seen[] is indexed per (var, kind) so an antecedent of     \
         * "x == c" (both x >= c AND x <= c) records both literals. */\
        if (_vid < ctx->n_vars && !lcg->seen[_slot]) {              \
            lcg->seen[_slot] = 1;                                   \
            lcg->seen_lit[_slot] = (lit);                            \
            vsids_bump(&lcg->vsids, _vid);                          \
            /* Find the decision level where this literal became     \
             * true by matching the trail entry's bound kind. */     \
            uint16_t _vlevel = 0;                                   \
            uint8_t _match_kind = (lit).is_lb ? TRAIL_LB : TRAIL_UB;\
            TrailEntry *_ts = ctx->trail_top;                       \
            while (_ts) {                                            \
                if (_ts->var_id == _vid &&                           \
                    _ts->kind == _match_kind) {                     \
                    _vlevel = _ts->decision_level; break;            \
                }                                                    \
                _ts = _ts->prev;                                    \
            }                                                        \
            if (_vlevel == cur_level) {                              \
                n_at_cur_level++;                                    \
            } else if (_vlevel > 0) {                                \
                Literal _neg = literal_negate(lit);                  \
                if (learnt_idx < lcg->learnt_cap) {                 \
                    lcg->learnt_buf[learnt_idx++] = _neg;           \
                }                                                    \
                if (_vlevel > bt_level) bt_level = _vlevel;         \
            }                                                        \
        }                                                            \
    } while(0)

    if (conflict_var != EXPR_NULL) {
        if (_tron()) {
            fprintf(stderr,
                "[lcg-trace] empty-domain conflict on v%u (lo>%ld hi<%ld)\n",
                conflict_var,
                (long)var_lo64(ctx, &ctx->vars[conflict_var]),
                (long)var_hi64(ctx, &ctx->vars[conflict_var]));
        }
        /* Empty-domain conflict: lo > hi for conflict_var.
         * Both the LB and UB bound tightenings contributed to the
         * conflict. Find the most recent trail entries for both. */
        TrailEntry *lb_entry = NULL, *ub_entry = NULL;
        TrailEntry *e = ctx->trail_top;
        while (e) {
            if (e->var_id == conflict_var) {
                if (e->kind == TRAIL_LB && !lb_entry) lb_entry = e;
                if (e->kind == TRAIL_UB && !ub_entry) ub_entry = e;
                if (lb_entry && ub_entry) break;
            }
            e = e->prev;
        }

        /* Determine which entry is at the current level.
         * Process current-level entries as UIP candidates and
         * earlier-level entries as clause body literals. */
        TrailEntry *cur_entry = NULL;   /* entry at current level */
        TrailEntry *other_entry = NULL; /* entry at earlier level */

        /* Prefer the entry at the current level. If both are,
         * pick the one that is a DECISION as the UIP (or the most
         * recent one if both are propagated). */
        if (lb_entry && lb_entry->decision_level == cur_level &&
            ub_entry && ub_entry->decision_level == cur_level) {
            /* Both at current level. The decision is the UIP;
             * the propagation should be explained. */
            if (ub_entry->prop_ref == EXPR_NULL) {
                cur_entry = ub_entry; other_entry = lb_entry;
            } else if (lb_entry->prop_ref == EXPR_NULL) {
                cur_entry = lb_entry; other_entry = ub_entry;
            } else {
                /* Both propagated: pick the most recent as UIP */
                cur_entry = lb_entry; other_entry = ub_entry;
            }
            /* "Other" is also at cur_level: add it as n_at_cur_level too */
            Literal other_lit;
            other_lit.var_id = conflict_var;
            other_lit.is_lb = (other_entry->kind == TRAIL_LB) ? 1 : 0;
            other_lit.bound = (int32_t)(other_lit.is_lb
                ? var_lo64(ctx, &ctx->vars[conflict_var])
                : var_hi64(ctx, &ctx->vars[conflict_var]));
            other_lit._pad[0] = other_lit._pad[1] = other_lit._pad[2] = 0;
            /* We can't use ADD_EXPL_LIT for the same variable since
             * seen[] is per-variable. Instead, directly process
             * the other entry's antecedents. */
            if (other_entry->prop_ref != EXPR_NULL) {
                Propagator *p = (Propagator *)zsp_pool_ptr(
                    &ctx->pool, other_entry->prop_ref);
                if (!p->explain) { lcg_dbg_bail[0]++; return -1; }
                Explanation expl;
                int64_t bv = other_lit.is_lb
                    ? var_lo64(ctx, &ctx->vars[conflict_var])
                    : var_hi64(ctx, &ctx->vars[conflict_var]);
                if (p->explain(p, ctx, conflict_var,
                               other_lit.is_lb, bv, &expl) != 0) {
                    lcg_dbg_bail[1]++; return -1;
                }
                if (_tron()) {
                    fprintf(stderr,
                        "[lcg-trace] explain(other) prop=%s v%u %s=%ld -> %u lits\n",
                        prop_fire_name(p->fire), conflict_var,
                        other_lit.is_lb ? "lb" : "ub", (long)bv, expl.n_lits);
                    for (uint32_t i = 0; i < expl.n_lits; i++)
                        _trace_lit("  ante", expl.lits[i]);
                }
                for (uint32_t i = 0; i < expl.n_lits; i++)
                    ADD_EXPL_LIT(expl.lits[i]);
            }
            /* Count the other_entry as being at the current level.
             * Since it was resolved (explained), don't increment
             * n_at_cur_level -- only the UIP remains. But if it's
             * a decision (prop_ref == EXPR_NULL), it can't be
             * resolved, so count it. */
            if (other_entry->prop_ref == EXPR_NULL)
                n_at_cur_level++;
        } else if (lb_entry && lb_entry->decision_level == cur_level) {
            cur_entry = lb_entry;
            other_entry = ub_entry;
        } else if (ub_entry && ub_entry->decision_level == cur_level) {
            cur_entry = ub_entry;
            other_entry = lb_entry;
        } else {
            /* Neither at current level: shouldn't happen. */
            lcg_dbg_bail[2]++; return -1;
        }

        /* Process the current-level entry as the UIP candidate */
        if (cur_entry) {
            Literal cl;
            cl.var_id = conflict_var;
            cl.is_lb = (cur_entry->kind == TRAIL_LB) ? 1 : 0;
            cl.bound = (int32_t)(cl.is_lb
                ? var_lo64(ctx, &ctx->vars[conflict_var])
                : var_hi64(ctx, &ctx->vars[conflict_var]));
            cl._pad[0] = cl._pad[1] = cl._pad[2] = 0;

            lcg->seen[SEEN_IX(conflict_var, cl.is_lb)] = 1;
            lcg->seen_lit[SEEN_IX(conflict_var, cl.is_lb)] = cl;
            vsids_bump(&lcg->vsids, conflict_var);
            n_at_cur_level++;

            /* If the current-level entry was propagated, explain it */
            if (cur_entry->prop_ref != EXPR_NULL) {
                Propagator *p = (Propagator *)zsp_pool_ptr(
                    &ctx->pool, cur_entry->prop_ref);
                if (!p->explain) { lcg_dbg_bail[5]++; return -1; }
                Explanation expl;
                int64_t bv = cl.is_lb
                    ? var_lo64(ctx, &ctx->vars[conflict_var])
                    : var_hi64(ctx, &ctx->vars[conflict_var]);
                if (p->explain(p, ctx, conflict_var,
                               cl.is_lb, bv, &expl) != 0) {
                    lcg_dbg_bail[6]++; return -1;
                }
                if (_tron()) {
                    fprintf(stderr,
                        "[lcg-trace] explain(cur) prop=%s v%u %s=%ld -> %u lits\n",
                        prop_fire_name(p->fire), conflict_var,
                        cl.is_lb ? "lb" : "ub", (long)bv, expl.n_lits);
                    for (uint32_t i = 0; i < expl.n_lits; i++)
                        _trace_lit("  ante", expl.lits[i]);
                }
                for (uint32_t i = 0; i < expl.n_lits; i++)
                    ADD_EXPL_LIT(expl.lits[i]);
            }
        }

        /* Process the earlier-level entry as a clause body literal */
        if (other_entry && other_entry->decision_level > 0 &&
            other_entry->decision_level < cur_level) {
            Literal ol;
            ol.var_id = conflict_var;
            ol.is_lb = (other_entry->kind == TRAIL_LB) ? 1 : 0;
            ol.bound = (int32_t)(ol.is_lb
                ? var_lo64(ctx, &ctx->vars[conflict_var])
                : var_hi64(ctx, &ctx->vars[conflict_var]));
            ol._pad[0] = ol._pad[1] = ol._pad[2] = 0;

            /* Negate and add directly to clause body */
            Literal neg = literal_negate(ol);
            if (learnt_idx < lcg->learnt_cap)
                lcg->learnt_buf[learnt_idx++] = neg;
            if (other_entry->decision_level > bt_level)
                bt_level = other_entry->decision_level;
        }
    } else if (ctx->conflict_prop_ref != EXPR_NULL) {
        if (_tron()) {
            Propagator *cp_ = (Propagator *)zsp_pool_ptr(
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
        Propagator *cp = (Propagator *)zsp_pool_ptr(
            &ctx->pool, ctx->conflict_prop_ref);
        PropWatchSect *ws = PROP_WS(cp);
        if (ws->n_watches == 0) {
            lcg_dbg_bail[3]++; return -1;
        }
        for (uint32_t i = 0; i < ws->n_watches; i++) {
            uint32_t vid = ws->var_ids[i];
            if (vid >= ctx->n_vars) continue;
            int64_t vlo = var_lo64(ctx, &ctx->vars[vid]);
            int64_t vhi = var_hi64(ctx, &ctx->vars[vid]);
            Literal llb; llb.var_id = vid; llb.is_lb = 1;
            llb.bound = (int32_t)vlo;
            llb._pad[0] = llb._pad[1] = llb._pad[2] = 0;
            ADD_EXPL_LIT(llb);
            Literal lub; lub.var_id = vid; lub.is_lb = 0;
            lub.bound = (int32_t)vhi;
            lub._pad[0] = lub._pad[1] = lub._pad[2] = 0;
            ADD_EXPL_LIT(lub);
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

    /* Step 2: Resolution loop (1UIP). Resolve until only one literal
     * at cur_level remains in the working set. */
    TrailEntry *e = ctx->trail_top;
    while (n_at_cur_level > 1 && e) {
        uint8_t e_is_lb = (e->kind == TRAIL_LB) ? 1 : 0;
        uint32_t e_slot = SEEN_IX(e->var_id, e_is_lb);
        if (e->decision_level != cur_level || !lcg->seen[e_slot]) {
            e = e->prev;
            continue;
        }

        lcg->seen[e_slot] = 0;
        n_at_cur_level--;

        if (e->prop_ref == EXPR_NULL) {
            /* Decision at current level: this becomes the 1UIP.
             * Stop resolution -- the remaining decisions at this level
             * that can't be resolved ARE the UIP. */
            n_at_cur_level = 1;  /* force loop exit, this literal is UIP */
            lcg->seen[e_slot] = 1;  /* re-mark as seen for UIP search */
            continue;
        }

        /* Resolve through propagator explanation */
        Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, e->prop_ref);
        if (!p->explain) { lcg_dbg_bail[7]++; return -1; }
        Explanation expl;
        int64_t bv = (e->kind == TRAIL_LB)
            ? var_lo64(ctx, &ctx->vars[e->var_id])
            : var_hi64(ctx, &ctx->vars[e->var_id]);
        int rc = p->explain(p, ctx, e->var_id,
                             (e->kind == TRAIL_LB) ? 1 : 0,
                             bv, &expl);
        if (rc != 0) { lcg_dbg_bail[8]++; return -1; }
        if (_tron()) {
            fprintf(stderr,
                "[lcg-trace] resolve  prop=%s v%u %s=%ld lvl=%u (old=%ld) -> %u lits\n",
                prop_fire_name(p->fire), e->var_id,
                (e->kind == TRAIL_LB) ? "lb" : "ub",
                (long)bv, e->decision_level, (long)e->old_value,
                expl.n_lits);
            for (uint32_t i = 0; i < expl.n_lits; i++)
                _trace_lit("  ante", expl.lits[i]);
        }
        for (uint32_t i = 0; i < expl.n_lits; i++)
            ADD_EXPL_LIT(expl.lits[i]);
        e = e->prev;
        continue;
    }

    #undef ADD_EXPL_LIT

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

    /* Output */
    *out_n = learnt_idx;
    *out_bt = bt_level;
    if (out_lits && learnt_idx > 0)
        memcpy(out_lits, lcg->learnt_buf, learnt_idx * sizeof(Literal));

    if (_tron()) {
        fprintf(stderr,
            "[lcg-trace] learnt (%u lits) bt=%u:\n", learnt_idx, bt_level);
        for (uint32_t i = 0; i < learnt_idx; i++)
            _trace_lit(i == 0 ? "  UIP " : "  body", lcg->learnt_buf[i]);
        fprintf(stderr, "[lcg-trace] ===\n");
    }

    vsids_decay(&lcg->vsids);
    lcg->n_analyses++;

    return 0;
}
