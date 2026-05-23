/*
 * LCG explanation functions for propagators.
 *
 * Each explain function produces a set of literals that, when all true,
 * imply the bound change. The clause form is:
 *   NOT(antecedent_1) OR NOT(antecedent_2) OR ... OR consequent
 *
 * For BoundsLE (x <= y):
 *   When x's UB was tightened to y_hi: reason is "y <= y_hi"
 *   When y's LB was tightened to x_lo: reason is "x >= x_lo"
 *
 * For BoundsLT (x < y):
 *   Same but shifted by 1.
 *
 * For BoundsEQ (x == y):
 *   When x's LB was tightened to y_lo: reason is "y >= y_lo"
 *   When x's UB was tightened to y_hi: reason is "y <= y_hi"
 *   (and symmetrically)
 *
 * For BoundsAdd (r = a + b):
 *   When r's LB was tightened: reason is "a >= a_lo" AND "b >= b_lo"
 *   When a's UB was tightened: reason is "r <= r_hi" AND "b >= b_lo"
 *   etc.
 */

#include <stdint.h>
#include "zsp_propagator.h"
#include "zsp_lcg.h"
#include "zsp_ctx.h"

#define PROP_WS(p) ((PropWatchSect *)((char *)(p) + sizeof(Propagator)))

/* Resolve a var_id through the alias table to its root representative. */
static inline uint32_t _resolve_var(const SolveCtx *ctx, uint32_t var_id) {
    if (!ctx->var_alias) return var_id;
    uint32_t root = var_id;
    while (ctx->var_alias[root] != root) root = ctx->var_alias[root];
    return root;
}

static Literal _mk_lb(uint32_t var_id, int32_t bound) {
    Literal l = {var_id, bound, 1, {0, 0, 0}};  /* caller resolves alias */
    return l;
}

static Literal _mk_ub(uint32_t var_id, int32_t bound) {
    Literal l = {var_id, bound, 0, {0, 0, 0}};
    return l;
}

/* ------------------------------------------------------------------ */
/* BoundsLE: x <= y                                                    */
/* ------------------------------------------------------------------ */

int explain_bounds_le(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    uint32_t yid = ws->var_ids[1];
    out->n_lits = 0;

    if (var_id == xid && !is_lb) {
        /* x's UB was tightened to y_hi. Reason: y <= y_hi */
        out->lits[out->n_lits++] = _mk_ub(yid, (int32_t)new_bound);
    } else if (var_id == yid && is_lb) {
        /* y's LB was tightened to x_lo. Reason: x >= x_lo */
        out->lits[out->n_lits++] = _mk_lb(xid, (int32_t)new_bound);
    } else {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* BoundsLT: x < y                                                    */
/* ------------------------------------------------------------------ */

int explain_bounds_lt(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    uint32_t yid = ws->var_ids[1];
    out->n_lits = 0;

    if (var_id == xid && !is_lb) {
        /* x's UB tightened to y_hi - 1. Reason: y <= y_hi (where y_hi = new_bound + 1) */
        out->lits[out->n_lits++] = _mk_ub(yid, (int32_t)(new_bound + 1));
    } else if (var_id == yid && is_lb) {
        /* y's LB tightened to x_lo + 1. Reason: x >= x_lo (where x_lo = new_bound - 1) */
        out->lits[out->n_lits++] = _mk_lb(xid, (int32_t)(new_bound - 1));
    } else {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* BoundsEQ: x == y                                                    */
/* ------------------------------------------------------------------ */

int explain_bounds_eq(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    uint32_t yid = ws->var_ids[1];
    uint32_t other = (var_id == xid) ? yid : xid;
    out->n_lits = 0;

    if (is_lb) {
        /* var's LB tightened. Reason: other >= new_bound */
        out->lits[out->n_lits++] = _mk_lb(other, (int32_t)new_bound);
    } else {
        /* var's UB tightened. Reason: other <= new_bound */
        out->lits[out->n_lits++] = _mk_ub(other, (int32_t)new_bound);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* BoundsNE: x != y                                                    */
/* ------------------------------------------------------------------ */

int explain_bounds_ne(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    uint32_t yid = ws->var_ids[1];
    uint32_t other = (var_id == xid) ? yid : xid;
    out->n_lits = 0;

    /* x != y propagates when one is singleton.
     * If other is singleton at value v, and var's bound was at v,
     * the reason is "other == v" (i.e., other >= v AND other <= v). */
    int64_t other_val = var_lo64(ctx, &ctx->vars[other]);
    out->lits[out->n_lits++] = _mk_lb(other, (int32_t)other_val);
    out->lits[out->n_lits++] = _mk_ub(other, (int32_t)other_val);
    return 0;
}

/* ------------------------------------------------------------------ */
/* BoundsAdd: r = a + b                                                */
/* ------------------------------------------------------------------ */

int explain_bounds_add(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0];
    uint32_t aid = ws->var_ids[1];
    uint32_t bid = ws->var_ids[2];
    out->n_lits = 0;

    if (var_id == rid) {
        if (is_lb) {
            /* r_lo tightened. Reason: a >= a_lo AND b >= b_lo */
            out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
            out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
        } else {
            /* r_hi tightened. Reason: a <= a_hi AND b <= b_hi */
            out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
            out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
        }
    } else if (var_id == aid) {
        if (is_lb) {
            /* a_lo tightened. Reason: r >= r_lo AND b <= b_hi */
            out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)var_lo64(ctx, &ctx->vars[rid]));
            out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
        } else {
            /* a_hi tightened. Reason: r <= r_hi AND b >= b_lo */
            out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)var_hi64(ctx, &ctx->vars[rid]));
            out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
        }
    } else if (var_id == bid) {
        if (is_lb) {
            out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)var_lo64(ctx, &ctx->vars[rid]));
            out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
        } else {
            out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)var_hi64(ctx, &ctx->vars[rid]));
            out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
        }
    } else {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Remaining explain callbacks (Sprint 3)                              */
/*                                                                     */
/* Each function follows the same pattern: given (var_id, is_lb,       */
/* new_bound), return the set of antecedent literals that imply the    */
/* bound change. Uses current variable bounds from ctx->vars[].        */
/* ------------------------------------------------------------------ */

/* ---- BoundsMul: r = a * b ---- */

int explain_bounds_mul(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1], bid = ws->var_ids[2];
    out->n_lits = 0;
    /* Conservative: both operand bounds are antecedents */
    out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
    out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
    if (var_id != rid) {
        out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)var_lo64(ctx, &ctx->vars[rid]));
        out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)var_hi64(ctx, &ctx->vars[rid]));
    }
    (void)new_bound; (void)is_lb;
    return 0;
}

/* ---- BoundsDiv: r = a / b ---- */

int explain_bounds_div(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1], bid = ws->var_ids[2];
    out->n_lits = 0;
    out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
    out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
    if (var_id != rid) {
        out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)var_lo64(ctx, &ctx->vars[rid]));
        out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)var_hi64(ctx, &ctx->vars[rid]));
    }
    (void)new_bound; (void)is_lb;
    return 0;
}

/* ---- BoundsMod: r = a % b ---- */

int explain_bounds_mod(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1], bid = ws->var_ids[2];
    out->n_lits = 0;
    out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
    out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
    (void)var_id; (void)new_bound; (void)is_lb;
    return 0;
}

/* ---- UnaryNeg: r = -a ---- */

int explain_unary_neg(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1];
    out->n_lits = 0;
    if (var_id == rid) {
        if (is_lb) {
            /* r_lo = -a_hi. Reason: a <= a_hi */
            out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
        } else {
            /* r_hi = -a_lo. Reason: a >= a_lo */
            out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
        }
    } else {
        if (is_lb) {
            out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)var_hi64(ctx, &ctx->vars[rid]));
        } else {
            out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)var_lo64(ctx, &ctx->vars[rid]));
        }
    }
    (void)new_bound;
    return 0;
}

/* ---- Implication: guard=1 -> var bound ---- */

int explain_implication(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t guard_id = ws->var_ids[0];
    out->n_lits = 0;
    /* The implication fires because guard >= 1 */
    out->lits[out->n_lits++] = _mk_lb(guard_id, 1);
    (void)var_id; (void)is_lb; (void)new_bound; (void)ctx;
    return 0;
}

/* ---- ITE: r = cond ? a : b ---- */

int explain_ite_value(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], cid = ws->var_ids[1];
    uint32_t aid = ws->var_ids[2], bid = ws->var_ids[3];
    out->n_lits = 0;

    int64_t clo = var_lo64(ctx, &ctx->vars[cid]);
    int64_t chi = var_hi64(ctx, &ctx->vars[cid]);

    if (clo >= 1) {
        /* cond is true -> r tracks a */
        out->lits[out->n_lits++] = _mk_lb(cid, 1);
        if (is_lb)
            out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
        else
            out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
    } else if (chi <= 0) {
        /* cond is false -> r tracks b */
        out->lits[out->n_lits++] = _mk_ub(cid, 0);
        if (is_lb)
            out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
        else
            out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
    } else {
        /* cond undecided -> both branches contribute */
        out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
        out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
        out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
        out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
    }
    (void)var_id; (void)new_bound; (void)rid;
    return 0;
}

/* ---- InSet: x in {elems} ---- */

int explain_in_set(Propagator *self, SolveCtx *ctx,
                    uint32_t var_id, uint8_t is_lb,
                    int64_t new_bound, Explanation *out) {
    /* For InSet, the bound change is implied by set membership itself.
     * We produce the current bounds of the variable as antecedents. */
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    out->n_lits = 0;
    out->lits[out->n_lits++] = _mk_lb(xid, (int32_t)var_lo64(ctx, &ctx->vars[xid]));
    out->lits[out->n_lits++] = _mk_ub(xid, (int32_t)var_hi64(ctx, &ctx->vars[xid]));
    (void)var_id; (void)is_lb; (void)new_bound;
    return 0;
}

/* ---- DisjClause: x1 op1 c1 OR x2 op2 c2 OR ... ---- */

int explain_disj_clause(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, Explanation *out) {
    /* When enforcing a survivor, the reason is that all other clauses
     * are falsified. We use their current bounds as antecedents. */
    PropWatchSect *ws = PROP_WS(self);
    out->n_lits = 0;
    for (uint32_t i = 0; i < ws->n_watches && out->n_lits < MAX_EXPLAIN_LITS - 2; i++) {
        uint32_t vid = ws->var_ids[i];
        if (vid == var_id) continue;
        out->lits[out->n_lits++] = _mk_lb(vid, (int32_t)var_lo64(ctx, &ctx->vars[vid]));
        out->lits[out->n_lits++] = _mk_ub(vid, (int32_t)var_hi64(ctx, &ctx->vars[vid]));
    }
    (void)is_lb; (void)new_bound;
    return 0;
}

/* ---- SumEq: r = sum(vars) ---- */

int explain_sum_eq(Propagator *self, SolveCtx *ctx,
                    uint32_t var_id, uint8_t is_lb,
                    int64_t new_bound, Explanation *out) {
    /* Generalized Add: all summand bounds are antecedents */
    PropWatchSect *ws = PROP_WS(self);
    out->n_lits = 0;
    for (uint32_t i = 0; i < ws->n_watches && out->n_lits < MAX_EXPLAIN_LITS - 2; i++) {
        uint32_t vid = ws->var_ids[i];
        if (vid == var_id) continue;
        if (is_lb)
            out->lits[out->n_lits++] = _mk_lb(vid, (int32_t)var_lo64(ctx, &ctx->vars[vid]));
        else
            out->lits[out->n_lits++] = _mk_ub(vid, (int32_t)var_hi64(ctx, &ctx->vars[vid]));
    }
    (void)new_bound;
    return 0;
}

/* ---- AllDifferent: singleton exclusion ---- */

int explain_all_different(Propagator *self, SolveCtx *ctx,
                           uint32_t var_id, uint8_t is_lb,
                           int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    out->n_lits = 0;
    /* The reason for excluding a value is that another var is singleton
     * at that value. Report all other singleton vars as antecedents. */
    for (uint32_t i = 0; i < ws->n_watches && out->n_lits < MAX_EXPLAIN_LITS - 2; i++) {
        uint32_t vid = ws->var_ids[i];
        if (vid == var_id) continue;
        int64_t lo = var_lo64(ctx, &ctx->vars[vid]);
        int64_t hi = var_hi64(ctx, &ctx->vars[vid]);
        if (lo == hi) {
            out->lits[out->n_lits++] = _mk_lb(vid, (int32_t)lo);
            out->lits[out->n_lits++] = _mk_ub(vid, (int32_t)hi);
        }
    }
    (void)is_lb; (void)new_bound;
    return 0;
}

/* ---- Reification: guard <-> (x <= y) ---- */

int explain_reification(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t gid = ws->var_ids[0], xid = ws->var_ids[1], yid = ws->var_ids[2];
    out->n_lits = 0;

    if (var_id == gid) {
        /* Guard was tightened based on x and y bounds */
        out->lits[out->n_lits++] = _mk_lb(xid, (int32_t)var_lo64(ctx, &ctx->vars[xid]));
        out->lits[out->n_lits++] = _mk_ub(xid, (int32_t)var_hi64(ctx, &ctx->vars[xid]));
        out->lits[out->n_lits++] = _mk_lb(yid, (int32_t)var_lo64(ctx, &ctx->vars[yid]));
        out->lits[out->n_lits++] = _mk_ub(yid, (int32_t)var_hi64(ctx, &ctx->vars[yid]));
    } else {
        /* x or y was tightened based on guard value */
        out->lits[out->n_lits++] = _mk_lb(gid, (int32_t)var_lo64(ctx, &ctx->vars[gid]));
        out->lits[out->n_lits++] = _mk_ub(gid, (int32_t)var_hi64(ctx, &ctx->vars[gid]));
    }
    (void)is_lb; (void)new_bound;
    return 0;
}

/* ---- ReificationEq: guard <-> (x == y) ---- */

int explain_reification_eq(Propagator *self, SolveCtx *ctx,
                            uint32_t var_id, uint8_t is_lb,
                            int64_t new_bound, Explanation *out) {
    /* Same structure as reification */
    return explain_reification(self, ctx, var_id, is_lb, new_bound, out);
}

/* ---- BitSlice: r = a[hi:lo] ---- */

int explain_bit_slice(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1];
    out->n_lits = 0;
    uint32_t src = (var_id == rid) ? aid : rid;
    out->lits[out->n_lits++] = _mk_lb(src, (int32_t)var_lo64(ctx, &ctx->vars[src]));
    out->lits[out->n_lits++] = _mk_ub(src, (int32_t)var_hi64(ctx, &ctx->vars[src]));
    (void)is_lb; (void)new_bound;
    return 0;
}

/* ---- Bitwise: BAND, BOR, BXOR, BNOT, SHL, LSHR, Concat ---- */
/* All use conservative "both operand bounds" as antecedents. */

static int _explain_binary_bitwise(Propagator *self, SolveCtx *ctx,
                                    uint32_t var_id, uint8_t is_lb,
                                    int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1], bid = ws->var_ids[2];
    out->n_lits = 0;
    out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)var_lo64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)var_hi64(ctx, &ctx->vars[aid]));
    out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)var_lo64(ctx, &ctx->vars[bid]));
    out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)var_hi64(ctx, &ctx->vars[bid]));
    if (var_id != rid) {
        out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)var_lo64(ctx, &ctx->vars[rid]));
        out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)var_hi64(ctx, &ctx->vars[rid]));
    }
    (void)is_lb; (void)new_bound;
    return 0;
}

/* Rule-aware bvand explanation. Mirrors _fire_bounds_band_64's six
 * propagation rules and cites only the antecedents the actually-fired
 * rule needed. Soundness arguments per rule:
 *
 *   r.ub  (is_lb=0): rules 1 (r<=min(a.ub,b.ub)), 3 (a sing -> r<=a),
 *     4 (b sing -> r<=b). For all three, "a.ub<=ahi AND b.ub<=bhi"
 *     implies "r=a&b <= min(ahi,bhi) <= new_bound". Cite both ub's.
 *   r.ub from both-singletons exact (rule 0): need full singletons.
 *   r.lb=0 (is_lb=1, new_bound<=0): rule 2 needs a>=0 AND b>=0.
 *   r.lb from both-singletons exact: need full singletons.
 *   a.lb (var_id==aid): rule 5 needs r and b as singletons.
 */
int explain_bounds_band(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1], bid = ws->var_ids[2];
    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
    int both_singleton = (alo == ahi && blo == bhi);
    out->n_lits = 0;

    if (var_id == rid) {
        if (is_lb) {
            if (both_singleton) {
                out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)alo);
                out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)alo);
                out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)blo);
                out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)blo);
            } else if (new_bound <= 0 && alo >= 0 && blo >= 0) {
                out->lits[out->n_lits++] = _mk_lb(aid, 0);
                out->lits[out->n_lits++] = _mk_lb(bid, 0);
            } else {
                return -1;
            }
        } else {
            if (both_singleton) {
                out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)alo);
                out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)alo);
                out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)blo);
                out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)blo);
            } else {
                out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)ahi);
                out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)bhi);
            }
        }
    } else if (var_id == aid) {
        /* rule 5: a.lb |= r.val when r and b singletons */
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)rlo);
        out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)rhi);
        out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)blo);
        out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)bhi);
    } else if (var_id == bid) {
        /* symmetric of rule 5 (not currently fired by band fire) */
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)rlo);
        out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)rhi);
        out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)alo);
        out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)ahi);
    } else {
        return -1;
    }
    return 0;
}

/* Rule-aware bvor explanation. _fire_bounds_bor_64 only forward-props.
 *
 *   r.lb (is_lb=1) when both non-negative: r >= max(alo, blo). Citing
 *     a.lb>=alo AND b.lb>=blo suffices because a|b >= a >= alo and
 *     a|b >= b >= blo (when both non-negative).
 *   r.lb / r.ub from both-singletons exact: full singletons.
 */
int explain_bounds_bor(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1], bid = ws->var_ids[2];
    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
    int both_singleton = (alo == ahi && blo == bhi);
    out->n_lits = 0;
    (void)new_bound;

    if (var_id == rid) {
        if (both_singleton) {
            out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)alo);
            out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)alo);
            out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)blo);
            out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)blo);
        } else if (is_lb && alo >= 0 && blo >= 0) {
            /* r.lb >= max(alo, blo) — cite both LBs */
            out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)alo);
            out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)blo);
        } else {
            return -1;
        }
    } else {
        /* bor fire has no backward propagation */
        return -1;
    }
    return 0;
}

/* Rule-aware bvxor explanation. _fire_bounds_bxor_64 only tightens
 * exact values (both-singletons or one-singleton-and-r-singleton).
 * Every rule needs both operands fully pinned, so cite full bounds. */
int explain_bounds_bxor(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1], bid = ws->var_ids[2];
    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
    int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
    out->n_lits = 0;
    (void)is_lb; (void)new_bound;

    if (var_id == rid) {
        out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)alo);
        out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)ahi);
        out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)blo);
        out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)bhi);
    } else if (var_id == aid) {
        out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)rlo);
        out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)rhi);
        out->lits[out->n_lits++] = _mk_lb(bid, (int32_t)blo);
        out->lits[out->n_lits++] = _mk_ub(bid, (int32_t)bhi);
    } else if (var_id == bid) {
        out->lits[out->n_lits++] = _mk_lb(rid, (int32_t)rlo);
        out->lits[out->n_lits++] = _mk_ub(rid, (int32_t)rhi);
        out->lits[out->n_lits++] = _mk_lb(aid, (int32_t)alo);
        out->lits[out->n_lits++] = _mk_ub(aid, (int32_t)ahi);
    } else {
        return -1;
    }
    return 0;
}

int explain_bounds_bnot(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1];
    out->n_lits = 0;
    uint32_t src = (var_id == rid) ? aid : rid;
    if (is_lb)
        out->lits[out->n_lits++] = _mk_ub(src, (int32_t)var_hi64(ctx, &ctx->vars[src]));
    else
        out->lits[out->n_lits++] = _mk_lb(src, (int32_t)var_lo64(ctx, &ctx->vars[src]));
    (void)new_bound;
    return 0;
}

int explain_bounds_shl(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, Explanation *out) {
    return _explain_binary_bitwise(self, ctx, var_id, is_lb, new_bound, out);
}

int explain_bounds_lshr(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, Explanation *out) {
    return _explain_binary_bitwise(self, ctx, var_id, is_lb, new_bound, out);
}

int explain_bounds_concat(Propagator *self, SolveCtx *ctx,
                           uint32_t var_id, uint8_t is_lb,
                           int64_t new_bound, Explanation *out) {
    return _explain_binary_bitwise(self, ctx, var_id, is_lb, new_bound, out);
}

int explain_countones(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1];
    out->n_lits = 0;
    uint32_t src = (var_id == rid) ? aid : rid;
    out->lits[out->n_lits++] = _mk_lb(src, (int32_t)var_lo64(ctx, &ctx->vars[src]));
    out->lits[out->n_lits++] = _mk_ub(src, (int32_t)var_hi64(ctx, &ctx->vars[src]));
    (void)is_lb; (void)new_bound;
    return 0;
}

int explain_clog2(Propagator *self, SolveCtx *ctx,
                   uint32_t var_id, uint8_t is_lb,
                   int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0], aid = ws->var_ids[1];
    out->n_lits = 0;
    uint32_t src = (var_id == rid) ? aid : rid;
    out->lits[out->n_lits++] = _mk_lb(src, (int32_t)var_lo64(ctx, &ctx->vars[src]));
    out->lits[out->n_lits++] = _mk_ub(src, (int32_t)var_hi64(ctx, &ctx->vars[src]));
    (void)is_lb; (void)new_bound;
    return 0;
}

/* ---- T-21: Explanation soundness verifier ---- */

#ifndef NDEBUG
#include <assert.h>

/**
 * Verify that an explanation is sound:
 * (a) All antecedent literals are currently true.
 * (b) The number of literals is within bounds.
 *
 * Called in debug builds after every explain callback during
 * proof extraction. Triggers an assertion on failure.
 */
void _verify_explanation(const SolveCtx *ctx,
                          uint32_t var_id, uint8_t is_lb,
                          int64_t new_bound, const Explanation *expl) {
    assert(expl->n_lits <= MAX_EXPLAIN_LITS);

    for (uint32_t i = 0; i < expl->n_lits; i++) {
        const Literal *lit = &expl->lits[i];
        assert(lit->var_id < ctx->n_vars);

        /* Check that the literal is true under current bounds */
        const Variable *v = &ctx->vars[lit->var_id];
        if (lit->is_lb) {
            /* x >= bound should be true: var_lo >= bound */
            int64_t lo = var_lo64(ctx, v);
            assert(lo >= lit->bound);
        } else {
            /* x <= bound should be true: var_hi <= bound */
            int64_t hi = var_hi64(ctx, v);
            assert(hi <= lit->bound);
        }
    }

    (void)var_id; (void)is_lb; (void)new_bound;
}
#endif /* NDEBUG */
