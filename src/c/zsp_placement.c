#include <stdint.h>
#include <string.h>
#include <time.h>
#include "zsp_placement.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_lcg.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int32_t i32_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t i32_max(int32_t a, int32_t b) { return a > b ? a : b; }

/* ================================================================== */
/* MinOf_N: r == min(operands[0..n-1])                                */
/*                                                                     */
/* Forward propagation:                                                */
/*   r_lo = min(op_lo_i)                                              */
/*   r_hi = min(op_hi_i)                                              */
/* Backward propagation:                                               */
/*   op_lo_i >= r_lo  (each operand >= result)                        */
/*   If op_lo_i > min-of-other-hi, operand cannot be the minimum;    */
/*     then op_lo_i >= r_lo is still valid but not tighter.           */
/* ================================================================== */

static PropResult _fire_min_of_n_32(Propagator *self, SolveCtx *ctx) {
    MinOfN_32_t *m = (MinOfN_32_t *)self;
    uint32_t n = m->n_vars;    /* [0]=result, [1..n-1]=operands */
    uint32_t rid = m->var_ids[0];
    PropResult r;

    /* Forward: compute bounds on result from operand bounds */
    int64_t min_lo = var_lo64(ctx, &ctx->vars[m->var_ids[1]]);
    int64_t min_hi = var_hi64(ctx, &ctx->vars[m->var_ids[1]]);
    for (uint32_t i = 2; i < n; i++) {
        int64_t lo = var_lo64(ctx, &ctx->vars[m->var_ids[i]]);
        int64_t hi = var_hi64(ctx, &ctx->vars[m->var_ids[i]]);
        if (lo < min_lo) min_lo = lo;
        if (hi < min_hi) min_hi = hi;
    }
    /* r_lo >= min of all operand lows (at least one operand must achieve it) */
    if ((r = ctx_tighten_lb64(ctx, rid, min_lo)) != PROP_OK) return r;
    /* r_hi <= min of all operand highs (result can't exceed any operand's max) */
    if ((r = ctx_tighten_ub64(ctx, rid, min_hi)) != PROP_OK) return r;

    /* Backward: tighten operand bounds from result */
    int64_t rv_lo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t rv_hi = var_hi64(ctx, &ctx->vars[rid]);
    /* Each operand must be >= result_lo */
    for (uint32_t i = 1; i < n; i++) {
        if ((r = ctx_tighten_lb64(ctx, m->var_ids[i], rv_lo)) != PROP_OK) return r;
    }

    /* If result is singleton, at least one operand must equal it.
     * If only one operand can still achieve the result value, fix it. */
    rv_lo = var_lo64(ctx, &ctx->vars[rid]);
    rv_hi = var_hi64(ctx, &ctx->vars[rid]);
    if (rv_lo == rv_hi) {
        int64_t rval = rv_lo;
        uint32_t can_achieve = 0;
        uint32_t last_achiever = 0;
        for (uint32_t i = 1; i < n; i++) {
            int64_t lo = var_lo64(ctx, &ctx->vars[m->var_ids[i]]);
            int64_t hi = var_hi64(ctx, &ctx->vars[m->var_ids[i]]);
            if (lo <= rval && rval <= hi) {
                can_achieve++;
                last_achiever = i;
            }
        }
        if (can_achieve == 0) return PROP_CONFLICT;
        if (can_achieve == 1) {
            /* Force the sole achiever to exactly rval */
            if ((r = ctx_tighten_lb64(ctx, m->var_ids[last_achiever], rval)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub64(ctx, m->var_ids[last_achiever], rval)) != PROP_OK) return r;
        }
    }

    /* Entailment: all singletons */
    int all_sing = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (var_lo64(ctx, &ctx->vars[m->var_ids[i]]) !=
            var_hi64(ctx, &ctx->vars[m->var_ids[i]])) { all_sing = 0; break; }
    }
    if (all_sing) return PROP_ENTAILED;

    return PROP_OK;
}

uint32_t prop_add_min_of_n_32(SolveCtx *ctx, uint32_t result_id,
                               uint32_t n_operands, const uint32_t *operand_ids,
                               uint8_t priority) {
    if (n_operands < 1 || n_operands > MAX_MINMAX_VARS) return EXPR_NULL;

    uint32_t n_total = 1 + n_operands;
    uint32_t ref = zsp_pool_alloc(&ctx->pool, (uint32_t)sizeof(MinOfN_32_t), 8u);
    if (ref == EXPR_NULL) return EXPR_NULL;

    MinOfN_32_t *m = (MinOfN_32_t *)zsp_pool_ptr(&ctx->pool, ref);
    memset(m, 0, sizeof(MinOfN_32_t));

    m->hdr.fire       = _fire_min_of_n_32;
    m->hdr.queue_next = EXPR_NULL;
    m->hdr.prop_id    = (uint16_t)ctx->n_props++;
    m->hdr.priority   = priority;
    m->hdr.flags      = PROP_FLAG_WIDE_WATCH;
    m->n_vars         = n_total;
    m->_capacity      = MAX_MINMAX_VARS + 1;

    m->var_ids[0] = result_id;
    for (uint32_t i = 0; i < n_operands; i++)
        m->var_ids[1 + i] = operand_ids[i];

    for (uint32_t i = 0; i < n_total; i++) {
        uint32_t vid = m->var_ids[i];
        m->watcher_nexts[i]     = ctx->watcher_heads[vid];
        ctx->watcher_heads[vid] = ref;
    }

    prop_enqueue(ctx, ref);
    if (ctx->prop_refs && m->hdr.prop_id < ctx->n_prop_refs_capacity)
        ctx->prop_refs[m->hdr.prop_id] = ref;

    return ref;
}

/* ================================================================== */
/* MaxOf_N: r == max(operands[0..n-1])                                */
/* ================================================================== */

static PropResult _fire_max_of_n_32(Propagator *self, SolveCtx *ctx) {
    MaxOfN_32_t *m = (MaxOfN_32_t *)self;
    uint32_t n = m->n_vars;
    uint32_t rid = m->var_ids[0];
    PropResult r;

    /* Forward: r_lo = max(op_lo_i), r_hi = max(op_hi_i) */
    int64_t max_lo = var_lo64(ctx, &ctx->vars[m->var_ids[1]]);
    int64_t max_hi = var_hi64(ctx, &ctx->vars[m->var_ids[1]]);
    for (uint32_t i = 2; i < n; i++) {
        int64_t lo = var_lo64(ctx, &ctx->vars[m->var_ids[i]]);
        int64_t hi = var_hi64(ctx, &ctx->vars[m->var_ids[i]]);
        if (lo > max_lo) max_lo = lo;
        if (hi > max_hi) max_hi = hi;
    }
    if ((r = ctx_tighten_lb64(ctx, rid, max_lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, rid, max_hi)) != PROP_OK) return r;

    /* Backward: each operand must be <= result_hi */
    int64_t rv_lo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t rv_hi = var_hi64(ctx, &ctx->vars[rid]);
    for (uint32_t i = 1; i < n; i++) {
        if ((r = ctx_tighten_ub64(ctx, m->var_ids[i], rv_hi)) != PROP_OK) return r;
    }

    /* If result is singleton, at least one operand must equal it */
    rv_lo = var_lo64(ctx, &ctx->vars[rid]);
    rv_hi = var_hi64(ctx, &ctx->vars[rid]);
    if (rv_lo == rv_hi) {
        int64_t rval = rv_lo;
        uint32_t can_achieve = 0;
        uint32_t last_achiever = 0;
        for (uint32_t i = 1; i < n; i++) {
            int64_t lo = var_lo64(ctx, &ctx->vars[m->var_ids[i]]);
            int64_t hi = var_hi64(ctx, &ctx->vars[m->var_ids[i]]);
            if (lo <= rval && rval <= hi) {
                can_achieve++;
                last_achiever = i;
            }
        }
        if (can_achieve == 0) return PROP_CONFLICT;
        if (can_achieve == 1) {
            if ((r = ctx_tighten_lb64(ctx, m->var_ids[last_achiever], rval)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub64(ctx, m->var_ids[last_achiever], rval)) != PROP_OK) return r;
        }
    }

    /* Entailment */
    int all_sing = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (var_lo64(ctx, &ctx->vars[m->var_ids[i]]) !=
            var_hi64(ctx, &ctx->vars[m->var_ids[i]])) { all_sing = 0; break; }
    }
    if (all_sing) return PROP_ENTAILED;

    return PROP_OK;
}

uint32_t prop_add_max_of_n_32(SolveCtx *ctx, uint32_t result_id,
                               uint32_t n_operands, const uint32_t *operand_ids,
                               uint8_t priority) {
    if (n_operands < 1 || n_operands > MAX_MINMAX_VARS) return EXPR_NULL;

    uint32_t n_total = 1 + n_operands;
    uint32_t ref = zsp_pool_alloc(&ctx->pool, (uint32_t)sizeof(MaxOfN_32_t), 8u);
    if (ref == EXPR_NULL) return EXPR_NULL;

    MaxOfN_32_t *m = (MaxOfN_32_t *)zsp_pool_ptr(&ctx->pool, ref);
    memset(m, 0, sizeof(MaxOfN_32_t));

    m->hdr.fire       = _fire_max_of_n_32;
    m->hdr.queue_next = EXPR_NULL;
    m->hdr.prop_id    = (uint16_t)ctx->n_props++;
    m->hdr.priority   = priority;
    m->hdr.flags      = PROP_FLAG_WIDE_WATCH;
    m->n_vars         = n_total;
    m->_capacity      = MAX_MINMAX_VARS + 1;

    m->var_ids[0] = result_id;
    for (uint32_t i = 0; i < n_operands; i++)
        m->var_ids[1 + i] = operand_ids[i];

    for (uint32_t i = 0; i < n_total; i++) {
        uint32_t vid = m->var_ids[i];
        m->watcher_nexts[i]     = ctx->watcher_heads[vid];
        ctx->watcher_heads[vid] = ref;
    }

    prop_enqueue(ctx, ref);
    if (ctx->prop_refs && m->hdr.prop_id < ctx->n_prop_refs_capacity)
        ctx->prop_refs[m->hdr.prop_id] = ref;

    return ref;
}

/* ================================================================== */
/* NoOverlap2D: pairwise non-overlap for rectangles                   */
/*                                                                     */
/* Strategy:                                                           */
/*   For each pair (i,j), check which relative positions are still    */
/*   feasible. If none -> conflict. If exactly one -> enforce it      */
/*   by tightening the relevant position bounds.                      */
/*                                                                     */
/*   Additionally, apply overload checking: for any axis-aligned      */
/*   strip, the sum of widths of rectangles that must intersect it    */
/*   cannot exceed the strip width.                                   */
/* ================================================================== */

/* Effective dimensions including halo */
static int32_t _eff_w(const RectSpec *r) { return r->width  + r->halo_l + r->halo_r; }
static int32_t _eff_h(const RectSpec *r) { return r->height + r->halo_t + r->halo_b; }
/* Effective left/top offsets (halo shifts the occupied region) */
static int32_t _eff_x_off(const RectSpec *r) { return -r->halo_l; }
static int32_t _eff_y_off(const RectSpec *r) { return -r->halo_t; }

/* Record which pair (i,j) caused a tightening for LCG explain */
static void _record_pair(NoOverlap2D_t *no, uint32_t var_id,
                          uint8_t is_lb, uint32_t pi, uint32_t pj) {
    /* Find the rect index for this variable */
    for (uint32_t r = 0; r < no->n_rects; r++) {
        if (no->rects[r].x_id == var_id || no->rects[r].y_id == var_id) {
            uint32_t slot = r * 4;
            if (no->rects[r].y_id == var_id) slot += 2;
            if (!is_lb) slot += 1;
            if (slot < MAX_NOOVERLAP2D_RECTS * 4)
                no->last_pair[slot] = (uint16_t)((pi << 8) | pj);
            return;
        }
    }
}

/* Look up the pair that caused a tightening */
static int _lookup_pair(const NoOverlap2D_t *no, uint32_t var_id,
                         uint8_t is_lb, uint32_t *out_i, uint32_t *out_j) {
    for (uint32_t r = 0; r < no->n_rects; r++) {
        if (no->rects[r].x_id == var_id || no->rects[r].y_id == var_id) {
            uint32_t slot = r * 4;
            if (no->rects[r].y_id == var_id) slot += 2;
            if (!is_lb) slot += 1;
            if (slot < MAX_NOOVERLAP2D_RECTS * 4 &&
                no->last_pair[slot] != 0xFFFF) {
                *out_i = no->last_pair[slot] >> 8;
                *out_j = no->last_pair[slot] & 0xFF;
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

static int _explain_no_overlap_2d(Propagator *self, SolveCtx *ctx,
                                   uint32_t var_id, uint8_t is_lb,
                                   int64_t new_bound, Explanation *out) {
    NoOverlap2D_t *no = (NoOverlap2D_t *)self;
    uint32_t nr = no->n_rects;
    out->n_lits = 0;

    /* Conflict explanation (var_id == UINT32_MAX):  find the conflicting
     * pair and return the geometric thresholds that would restore
     * feasibility of at least one separation direction.
     *
     * For each infeasible direction, compute the threshold that would
     * make it feasible again.  The conflict clause is the disjunction:
     *   "at least one direction must become feasible."
     *
     * Separation condition for "i left of j":
     *   xi + exi + ewi <= xj + exj
     * Feasibility threshold:
     *   xi <= xj_hi + exj - exi - ewi   (literal: xi <= T)
     *   xj >= xi_lo + exi + ewi - exj   (literal: xj >= T)
     * Only include the literal for the variable whose bound made it
     * infeasible. */
    if (var_id == UINT32_MAX) {
        /* Conflict explanation: find the pair with all 4 directions
         * infeasible and return CURRENT bounds as antecedents.
         * These are all TRUE right now and their conjunction causes
         * the conflict.  The resolution process will negate them. */
        for (uint32_t i = 0; i < nr; i++) {
            const RectSpec *ri = &no->rects[i];
            int32_t xi_lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->x_id]);
            int32_t xi_hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->x_id]);
            int32_t yi_lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->y_id]);
            int32_t yi_hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->y_id]);
            int32_t ewi = _eff_w(ri), ehi_r = _eff_h(ri);
            int32_t exi = _eff_x_off(ri), eyi = _eff_y_off(ri);

            for (uint32_t j = i + 1; j < nr; j++) {
                const RectSpec *rj = &no->rects[j];
                int32_t xj_lo = (int32_t)var_lo64(ctx, &ctx->vars[rj->x_id]);
                int32_t xj_hi = (int32_t)var_hi64(ctx, &ctx->vars[rj->x_id]);
                int32_t yj_lo = (int32_t)var_lo64(ctx, &ctx->vars[rj->y_id]);
                int32_t yj_hi = (int32_t)var_hi64(ctx, &ctx->vars[rj->y_id]);
                int32_t ewj = _eff_w(rj), ehj = _eff_h(rj);
                int32_t exj = _eff_x_off(rj), eyj = _eff_y_off(rj);

                int can_left  = (xi_lo + exi + ewi <= xj_hi + exj);
                int can_right = (xj_lo + exj + ewj <= xi_hi + exi);
                int can_above = (yi_lo + eyi + ehi_r <= yj_hi + eyj);
                int can_below = (yj_lo + eyj + ehj <= yi_hi + eyi);

                if (!can_left && !can_right && !can_above && !can_below) {
                    Literal l;
                    l._pad[0] = l._pad[1] = l._pad[2] = 0;

                    /* Include all 8 current bounds of the conflicting pair.
                     * Each bound is currently TRUE; their conjunction causes
                     * the conflict (no feasible separation direction). */
                    l.var_id = ri->x_id; l.is_lb = 1; l.bound = xi_lo;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;
                    l.var_id = ri->x_id; l.is_lb = 0; l.bound = xi_hi;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;
                    l.var_id = ri->y_id; l.is_lb = 1; l.bound = yi_lo;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;
                    l.var_id = ri->y_id; l.is_lb = 0; l.bound = yi_hi;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;
                    l.var_id = rj->x_id; l.is_lb = 1; l.bound = xj_lo;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;
                    l.var_id = rj->x_id; l.is_lb = 0; l.bound = xj_hi;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;
                    l.var_id = rj->y_id; l.is_lb = 1; l.bound = yj_lo;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;
                    l.var_id = rj->y_id; l.is_lb = 0; l.bound = yj_hi;
                    if (out->n_lits < MAX_EXPLAIN_LITS) out->lits[out->n_lits++] = l;

                    return 0;
                }
            }
        }
        return -1;  /* no conflict found (shouldn't happen) */
    }

    /* Bound-change explanation: use the recorded pair (i,j) that
     * caused this tightening.  Include all current bounds of both
     * rects as antecedents except the one being explained. */
    {
        uint32_t pi = 0, pj = 0;
        if (_lookup_pair(no, var_id, is_lb, &pi, &pj) != 0)
            return -1;  /* unknown pair: can't explain */
        if (pi >= nr || pj >= nr)
            return -1;  /* invalid pair */

        /* Use the recorded pair for explanation */
        uint32_t i = pi, j = pj;
        const RectSpec *ri = &no->rects[i];
        const RectSpec *rj = &no->rects[j];
        {

            int32_t xi_lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->x_id]);
            int32_t xi_hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->x_id]);
            int32_t yi_lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->y_id]);
            int32_t yi_hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->y_id]);
            int32_t xj_lo = (int32_t)var_lo64(ctx, &ctx->vars[rj->x_id]);
            int32_t xj_hi = (int32_t)var_hi64(ctx, &ctx->vars[rj->x_id]);
            int32_t yj_lo = (int32_t)var_lo64(ctx, &ctx->vars[rj->y_id]);
            int32_t yj_hi = (int32_t)var_hi64(ctx, &ctx->vars[rj->y_id]);

            int32_t ewi = _eff_w(ri), ehi_r = _eff_h(ri);
            int32_t ewj = _eff_w(rj), ehj = _eff_h(rj);
            int32_t exi = _eff_x_off(ri), eyi = _eff_y_off(ri);
            int32_t exj = _eff_x_off(rj), eyj = _eff_y_off(rj);

            /* Check if this pair could have caused the tightening:
             * at most one direction must be feasible, AND the remaining
             * direction must actually enforce the bound being explained. */
            int can_left  = (xi_lo + exi + ewi <= xj_hi + exj);
            int can_right = (xj_lo + exj + ewj <= xi_hi + exi);
            int can_above = (yi_lo + eyi + ehi_r <= yj_hi + eyj);
            int can_below = (yj_lo + eyj + ehj <= yi_hi + eyi);
            int n_feasible = can_left + can_right + can_above + can_below;
            if (n_feasible > 1) return -1;  /* pair didn't cause tightening */

            /* Verify the enforced direction matches the bound being explained.
             * Skip if the pair's remaining direction doesn't enforce var_id. */
            int pair_matches = 0;
            if (n_feasible == 1) {
                if (can_left) {
                    /* i left of j: enforces xi UB and xj LB */
                    if ((var_id == ri->x_id && !is_lb) ||
                        (var_id == rj->x_id && is_lb))
                        pair_matches = 1;
                } else if (can_right) {
                    /* i right of j: enforces xj UB and xi LB */
                    if ((var_id == rj->x_id && !is_lb) ||
                        (var_id == ri->x_id && is_lb))
                        pair_matches = 1;
                } else if (can_above) {
                    /* i above j: enforces yi UB and yj LB */
                    if ((var_id == ri->y_id && !is_lb) ||
                        (var_id == rj->y_id && is_lb))
                        pair_matches = 1;
                } else if (can_below) {
                    /* i below j: enforces yj UB and yi LB */
                    if ((var_id == rj->y_id && !is_lb) ||
                        (var_id == ri->y_id && is_lb))
                        pair_matches = 1;
                }
            } else {
                /* n_feasible == 0: geometric conflict. The pair can still
                 * explain if the var was involved. */
                pair_matches = 1;
            }
            if (!pair_matches) return -1;

            /* Verify the threshold: the enforced direction must produce
             * exactly new_bound (or tighter).  If not, this pair didn't
             * cause THIS tightening. */
            {
                int32_t expected = (int32_t)new_bound;
                int32_t threshold = 0;
                int valid = 0;
                if (n_feasible == 1) {
                    if (can_left && var_id == ri->x_id && !is_lb)
                        { threshold = xj_hi + exj - exi - ewi; valid = (expected >= threshold); }
                    else if (can_left && var_id == rj->x_id && is_lb)
                        { threshold = xi_lo + exi + ewi - exj; valid = (expected <= threshold); }
                    else if (can_right && var_id == rj->x_id && !is_lb)
                        { threshold = xi_hi + exi - exj - ewj; valid = (expected >= threshold); }
                    else if (can_right && var_id == ri->x_id && is_lb)
                        { threshold = xj_lo + exj + ewj - exi; valid = (expected <= threshold); }
                    else if (can_above && var_id == ri->y_id && !is_lb)
                        { threshold = yj_hi + eyj - eyi - ehi_r; valid = (expected >= threshold); }
                    else if (can_above && var_id == rj->y_id && is_lb)
                        { threshold = yi_lo + eyi + ehi_r - eyj; valid = (expected <= threshold); }
                    else if (can_below && var_id == rj->y_id && !is_lb)
                        { threshold = yi_hi + eyi - eyj - ehj; valid = (expected >= threshold); }
                    else if (can_below && var_id == ri->y_id && is_lb)
                        { threshold = yj_lo + eyj + ehj - eyi; valid = (expected <= threshold); }
                    if (!valid) return -1;  /* threshold mismatch */
                }
            }

            /* Include all current bounds of both rects as antecedents,
             * EXCLUDING the bound being explained (var_id, is_lb). */
            Literal l;
            l._pad[0] = l._pad[1] = l._pad[2] = 0;

            /* Collect all 8 bounds, skip the one being explained */
            struct { uint32_t vid; uint8_t lb; int32_t bnd; } bounds[8] = {
                {ri->x_id, 1, xi_lo}, {ri->x_id, 0, xi_hi},
                {ri->y_id, 1, yi_lo}, {ri->y_id, 0, yi_hi},
                {rj->x_id, 1, xj_lo}, {rj->x_id, 0, xj_hi},
                {rj->y_id, 1, yj_lo}, {rj->y_id, 0, yj_hi},
            };
            for (int k = 0; k < 8 && out->n_lits < MAX_EXPLAIN_LITS; k++) {
                if (bounds[k].vid == var_id && bounds[k].lb == is_lb)
                    continue;  /* skip the bound being explained */
                l.var_id = bounds[k].vid;
                l.is_lb = bounds[k].lb;
                l.bound = bounds[k].bnd;
                out->lits[out->n_lits++] = l;
            }

        }
    }

    return (out->n_lits > 0) ? 0 : -1;
}

static PropResult _fire_no_overlap_2d(Propagator *self, SolveCtx *ctx) {
    NoOverlap2D_t *no = (NoOverlap2D_t *)self;
    uint32_t nr = no->n_rects;  /* actual rect count (not n_vars) */
    PropResult res;

    /* Clear pair tracking: only record pairs from THIS propagation round.
     * Stale entries from before backtracking would produce wrong explains. */
    memset(no->last_pair, 0xFF, sizeof(no->last_pair));

    /* For each pair, check which separation directions remain feasible.
     * If only one remains, enforce it. */
    for (uint32_t i = 0; i < nr; i++) {
        const RectSpec *ri = &no->rects[i];
        int32_t xi_lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->x_id]);
        int32_t xi_hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->x_id]);
        int32_t yi_lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->y_id]);
        int32_t yi_hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->y_id]);
        int32_t ewi = _eff_w(ri);
        int32_t ehi = _eff_h(ri);
        int32_t exi_off = _eff_x_off(ri);
        int32_t eyi_off = _eff_y_off(ri);

        for (uint32_t j = i + 1; j < nr; j++) {
            const RectSpec *rj = &no->rects[j];
            int32_t xj_lo = (int32_t)var_lo64(ctx, &ctx->vars[rj->x_id]);
            int32_t xj_hi = (int32_t)var_hi64(ctx, &ctx->vars[rj->x_id]);
            int32_t yj_lo = (int32_t)var_lo64(ctx, &ctx->vars[rj->y_id]);
            int32_t yj_hi = (int32_t)var_hi64(ctx, &ctx->vars[rj->y_id]);
            int32_t ewj = _eff_w(rj);
            int32_t ehj = _eff_h(rj);
            int32_t exj_off = _eff_x_off(rj);
            int32_t eyj_off = _eff_y_off(rj);

            /* Check feasibility of each separation direction.
             * Direction "i left of j": (xi + exi_off + ewi) <= (xj + exj_off)
             *   i.e., xi <= xj + exj_off - exi_off - ewi
             *   Feasible if xi_lo <= xj_hi + exj_off - exi_off - ewi */
            int can_i_left  = (xi_lo + exi_off + ewi <= xj_hi + exj_off);
            int can_i_right = (xj_lo + exj_off + ewj <= xi_hi + exi_off);
            int can_i_above = (yi_lo + eyi_off + ehi <= yj_hi + eyj_off);
            int can_i_below = (yj_lo + eyj_off + ehj <= yi_hi + eyi_off);

            int n_feasible = can_i_left + can_i_right + can_i_above + can_i_below;

            if (n_feasible == 0) return PROP_CONFLICT;

            if (n_feasible == 1) {
                if (can_i_left) {
                    int32_t gap = exj_off - exi_off - ewi;
                    _record_pair(no, ri->x_id, 0, i, j);
                    if ((res = ctx_tighten_ub64(ctx, ri->x_id, (int64_t)xj_hi + gap)) != PROP_OK) return res;
                    _record_pair(no, rj->x_id, 1, i, j);
                    if ((res = ctx_tighten_lb64(ctx, rj->x_id, (int64_t)xi_lo - gap)) != PROP_OK) return res;
                } else if (can_i_right) {
                    int32_t gap = exi_off - exj_off - ewj;
                    _record_pair(no, rj->x_id, 0, i, j);
                    if ((res = ctx_tighten_ub64(ctx, rj->x_id, (int64_t)xi_hi + gap)) != PROP_OK) return res;
                    _record_pair(no, ri->x_id, 1, i, j);
                    if ((res = ctx_tighten_lb64(ctx, ri->x_id, (int64_t)xj_lo - gap)) != PROP_OK) return res;
                } else if (can_i_above) {
                    int32_t gap = eyj_off - eyi_off - ehi;
                    _record_pair(no, ri->y_id, 0, i, j);
                    if ((res = ctx_tighten_ub64(ctx, ri->y_id, (int64_t)yj_hi + gap)) != PROP_OK) return res;
                    _record_pair(no, rj->y_id, 1, i, j);
                    if ((res = ctx_tighten_lb64(ctx, rj->y_id, (int64_t)yi_lo - gap)) != PROP_OK) return res;
                } else { /* can_i_below */
                    int32_t gap = eyi_off - eyj_off - ehj;
                    _record_pair(no, rj->y_id, 0, i, j);
                    if ((res = ctx_tighten_ub64(ctx, rj->y_id, (int64_t)yi_hi + gap)) != PROP_OK) return res;
                    _record_pair(no, ri->y_id, 1, i, j);
                    if ((res = ctx_tighten_lb64(ctx, ri->y_id, (int64_t)yj_lo - gap)) != PROP_OK) return res;
                }
            }
            /* n_feasible >= 2: can still tighten bounds in forced directions.
             * For each pair of infeasible directions, tighten accordingly.
             * E.g., if i cannot be left of j, then xi >= xj + exj_off - exi_off - ewi + 1
             * ... but this is weaker when other directions are still open. */
        }
    }

    /* Energetic overload check on X axis:
     * For each horizontal strip [y_min, y_max], sum the widths of rectangles
     * whose Y intervals *must* intersect the strip. If total width exceeds
     * the canvas, conflict. We approximate with the global bounding box. */
    int32_t global_x_lo = INT32_MAX, global_x_hi = INT32_MIN;
    for (uint32_t i = 0; i < nr; i++) {
        const RectSpec *ri = &no->rects[i];
        int32_t lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->x_id]) + _eff_x_off(ri);
        int32_t hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->x_id]) + _eff_x_off(ri) + _eff_w(ri);
        if (lo < global_x_lo) global_x_lo = lo;
        if (hi > global_x_hi) global_x_hi = hi;
    }
    int32_t global_y_lo = INT32_MAX, global_y_hi = INT32_MIN;
    for (uint32_t i = 0; i < nr; i++) {
        const RectSpec *ri = &no->rects[i];
        int32_t lo = (int32_t)var_lo64(ctx, &ctx->vars[ri->y_id]) + _eff_y_off(ri);
        int32_t hi = (int32_t)var_hi64(ctx, &ctx->vars[ri->y_id]) + _eff_y_off(ri) + _eff_h(ri);
        if (lo < global_y_lo) global_y_lo = lo;
        if (hi > global_y_hi) global_y_hi = hi;
    }

    /* Check: total mandatory area cannot exceed bounding box area */
    int64_t total_area = 0;
    for (uint32_t i = 0; i < nr; i++) {
        total_area += (int64_t)_eff_w(&no->rects[i]) * (int64_t)_eff_h(&no->rects[i]);
    }
    int64_t bbox_area = (int64_t)(global_x_hi - global_x_lo) *
                        (int64_t)(global_y_hi - global_y_lo);
    if (bbox_area > 0 && total_area > bbox_area) return PROP_CONFLICT;

    /* ── Energetic reasoning: subregion overload checking ──
     *
     * For each candidate strip on one axis (defined by mandatory interval
     * endpoints), sum the effective widths/heights of rects that MUST
     * cross that strip.  If total demand exceeds the available range on
     * the perpendicular axis, the configuration is infeasible.
     *
     * A rect's mandatory interval on axis A is:
     *   [hi_pos + off, lo_pos + off + dim)
     * where hi_pos is the upper bound of the position variable (rightmost
     * possible left-edge), and lo_pos is the lower bound.  This is the
     * interval the rect occupies in ALL possible placements.
     *
     * Complexity: O(n * n_endpoints) per axis.  With n_endpoints <= 2*n,
     * this is O(n^2) which is dominated by the pairwise check above.
     */

    /* Precompute mandatory intervals for each rect */
    int32_t my_lo[MAX_NOOVERLAP2D_RECTS], my_hi[MAX_NOOVERLAP2D_RECTS];
    int32_t mx_lo[MAX_NOOVERLAP2D_RECTS], mx_hi[MAX_NOOVERLAP2D_RECTS];
    int32_t y_endpoints[MAX_NOOVERLAP2D_RECTS * 2];
    int32_t x_endpoints[MAX_NOOVERLAP2D_RECTS * 2];
    uint32_t n_y_ep = 0, n_x_ep = 0;

    for (uint32_t i = 0; i < nr; i++) {
        const RectSpec *ri = &no->rects[i];
        int32_t yi_lo_pos = (int32_t)var_lo64(ctx, &ctx->vars[ri->y_id]);
        int32_t yi_hi_pos = (int32_t)var_hi64(ctx, &ctx->vars[ri->y_id]);
        my_lo[i] = yi_hi_pos + _eff_y_off(ri);
        my_hi[i] = yi_lo_pos + _eff_y_off(ri) + _eff_h(ri);
        if (my_lo[i] < my_hi[i]) {
            y_endpoints[n_y_ep++] = my_lo[i];
            y_endpoints[n_y_ep++] = my_hi[i];
        }

        int32_t xi_lo_pos = (int32_t)var_lo64(ctx, &ctx->vars[ri->x_id]);
        int32_t xi_hi_pos = (int32_t)var_hi64(ctx, &ctx->vars[ri->x_id]);
        mx_lo[i] = xi_hi_pos + _eff_x_off(ri);
        mx_hi[i] = xi_lo_pos + _eff_x_off(ri) + _eff_w(ri);
        if (mx_lo[i] < mx_hi[i]) {
            x_endpoints[n_x_ep++] = mx_lo[i];
            x_endpoints[n_x_ep++] = mx_hi[i];
        }
    }

    /* Sort endpoints (insertion sort, small n) */
    for (uint32_t i = 1; i < n_y_ep; i++) {
        int32_t key = y_endpoints[i];
        int j = (int)i - 1;
        while (j >= 0 && y_endpoints[j] > key) { y_endpoints[j+1] = y_endpoints[j]; j--; }
        y_endpoints[j+1] = key;
    }
    for (uint32_t i = 1; i < n_x_ep; i++) {
        int32_t key = x_endpoints[i];
        int j = (int)i - 1;
        while (j >= 0 && x_endpoints[j] > key) { x_endpoints[j+1] = x_endpoints[j]; j--; }
        x_endpoints[j+1] = key;
    }

    /* Deduplicate */
    if (n_y_ep > 1) {
        uint32_t w = 1;
        for (uint32_t r = 1; r < n_y_ep; r++)
            if (y_endpoints[r] != y_endpoints[w-1]) y_endpoints[w++] = y_endpoints[r];
        n_y_ep = w;
    }
    if (n_x_ep > 1) {
        uint32_t w = 1;
        for (uint32_t r = 1; r < n_x_ep; r++)
            if (x_endpoints[r] != x_endpoints[w-1]) x_endpoints[w++] = x_endpoints[r];
        n_x_ep = w;
    }

    /* 1D overload on X axis: for each Y-strip, check total width demand */
    for (uint32_t yi = 0; yi + 1 < n_y_ep; yi++) {
        int32_t strip_lo = y_endpoints[yi];
        int32_t strip_hi = y_endpoints[yi + 1];
        if (strip_lo >= strip_hi) continue;

        int64_t x_demand = 0;
        int32_t x_lo_min = INT32_MAX, x_hi_max = INT32_MIN;

        for (uint32_t i = 0; i < nr; i++) {
            /* Does rect i's mandatory Y interval contain this strip? */
            if (my_lo[i] <= strip_lo && my_hi[i] >= strip_hi) {
                x_demand += (int64_t)_eff_w(&no->rects[i]);
                const RectSpec *ri = &no->rects[i];
                int32_t xlo = (int32_t)var_lo64(ctx, &ctx->vars[ri->x_id]) + _eff_x_off(ri);
                int32_t xhi = (int32_t)var_hi64(ctx, &ctx->vars[ri->x_id]) + _eff_x_off(ri) + _eff_w(ri);
                if (xlo < x_lo_min) x_lo_min = xlo;
                if (xhi > x_hi_max) x_hi_max = xhi;
            }
        }

        if (x_demand > 0 && x_hi_max > x_lo_min) {
            int64_t x_supply = (int64_t)(x_hi_max - x_lo_min);
            if (x_demand > x_supply) return PROP_CONFLICT;
        }
    }

    /* 1D overload on Y axis: for each X-strip, check total height demand */
    for (uint32_t xi = 0; xi + 1 < n_x_ep; xi++) {
        int32_t strip_lo = x_endpoints[xi];
        int32_t strip_hi = x_endpoints[xi + 1];
        if (strip_lo >= strip_hi) continue;

        int64_t y_demand = 0;
        int32_t y_lo_min = INT32_MAX, y_hi_max = INT32_MIN;

        for (uint32_t i = 0; i < nr; i++) {
            if (mx_lo[i] <= strip_lo && mx_hi[i] >= strip_hi) {
                y_demand += (int64_t)_eff_h(&no->rects[i]);
                const RectSpec *ri = &no->rects[i];
                int32_t ylo = (int32_t)var_lo64(ctx, &ctx->vars[ri->y_id]) + _eff_y_off(ri);
                int32_t yhi = (int32_t)var_hi64(ctx, &ctx->vars[ri->y_id]) + _eff_y_off(ri) + _eff_h(ri);
                if (ylo < y_lo_min) y_lo_min = ylo;
                if (yhi > y_hi_max) y_hi_max = yhi;
            }
        }

        if (y_demand > 0 && y_hi_max > y_lo_min) {
            int64_t y_supply = (int64_t)(y_hi_max - y_lo_min);
            if (y_demand > y_supply) return PROP_CONFLICT;
        }
    }

    /* Entailment: all position vars are singletons */
    int all_fixed = 1;
    for (uint32_t i = 0; i < nr; i++) {
        int64_t xlo = var_lo64(ctx, &ctx->vars[no->rects[i].x_id]);
        int64_t xhi = var_hi64(ctx, &ctx->vars[no->rects[i].x_id]);
        int64_t ylo = var_lo64(ctx, &ctx->vars[no->rects[i].y_id]);
        int64_t yhi = var_hi64(ctx, &ctx->vars[no->rects[i].y_id]);
        if (xlo != xhi || ylo != yhi) {
            all_fixed = 0;
            break;
        }
    }
    if (all_fixed) return PROP_ENTAILED;

    return PROP_OK;
}

uint32_t prop_add_no_overlap_2d(SolveCtx *ctx, uint32_t n_rects,
                                 const RectSpec *rects, uint8_t priority) {
    if (n_rects < 2 || n_rects > MAX_NOOVERLAP2D_RECTS) return EXPR_NULL;

    uint32_t ref = zsp_pool_alloc(&ctx->pool, (uint32_t)sizeof(NoOverlap2D_t), 8u);
    if (ref == EXPR_NULL) return EXPR_NULL;

    NoOverlap2D_t *no = (NoOverlap2D_t *)zsp_pool_ptr(&ctx->pool, ref);
    memset(no, 0, sizeof(NoOverlap2D_t));
    memset(no->last_pair, 0xFF, sizeof(no->last_pair));

    no->hdr.fire       = _fire_no_overlap_2d;
    no->hdr.explain    = NULL;  /* TODO: per-pair trail tracking needed for sound explains */
    no->hdr.queue_next = EXPR_NULL;
    no->hdr.prop_id    = (uint16_t)ctx->n_props++;
    no->hdr.priority   = priority;
    no->hdr.flags      = PROP_FLAG_WIDE_WATCH;
    no->n_vars         = 2 * n_rects;  /* wide-watch: n_vars for var_ids count */
    no->_capacity      = MAX_NOOVERLAP2D_RECTS * 2;
    no->n_rects        = n_rects;      /* actual rectangle count */
    no->_rpad          = 0;

    for (uint32_t i = 0; i < n_rects; i++) {
        no->rects[i] = rects[i];
        no->var_ids[2 * i]     = rects[i].x_id;
        no->var_ids[2 * i + 1] = rects[i].y_id;
    }

    uint32_t n_watches = 2 * n_rects;
    for (uint32_t i = 0; i < n_watches; i++) {
        uint32_t vid = no->var_ids[i];
        no->watcher_nexts[i]    = ctx->watcher_heads[vid];
        ctx->watcher_heads[vid] = ref;
    }

    prop_enqueue(ctx, ref);
    if (ctx->prop_refs && no->hdr.prop_id < ctx->n_prop_refs_capacity)
        ctx->prop_refs[no->hdr.prop_id] = ref;

    return ref;
}

/* ================================================================== */
/* solver_optimize: branch-and-bound minimization                     */
/* ================================================================== */

static double _now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int solver_optimize(SolveCtx *ctx, uint32_t objective_var_id,
                    const OptimizeOpts *opts, OptimizeResult *result) {
    if (!ctx || !result) return -1;
    if (objective_var_id >= ctx->n_vars) return -1;

    memset(result, 0, sizeof(OptimizeResult));
    double t_start = _now_sec();

    uint32_t max_rounds = (opts && opts->max_rounds > 0) ? opts->max_rounds : 1000;
    double time_limit   = (opts && opts->time_limit_sec > 0.0) ? opts->time_limit_sec : 300.0;

    /* Save initial variable state for restoring between rounds */
    int64_t *best_values = NULL;
    uint32_t n = ctx->n_vars;

    /* Allocate best-solution storage on the stack for small problems,
     * in the pool for larger ones. */
    int64_t stack_buf[256];
    if (n <= 256) {
        best_values = stack_buf;
    } else {
        /* Use block allocator */
        uint32_t bv_ref = zsp_pool_alloc(&ctx->pool, n * (uint32_t)sizeof(int64_t),
                                          (uint32_t)_Alignof(int64_t));
        if (bv_ref == EXPR_NULL) return -1;
        best_values = (int64_t *)zsp_pool_ptr(&ctx->pool, bv_ref);
    }

    SolveOpts sopts;
    memset(&sopts, 0, sizeof(sopts));
    if (opts) {
        sopts.seed            = opts->seed;
        sopts.max_conflicts   = opts->max_conflicts > 0 ? opts->max_conflicts : 200;
        sopts.max_restarts    = opts->max_restarts > 0 ? opts->max_restarts : 5000;
        sopts.use_phase_save  = opts->use_phase_save;
        sopts.max_shave_iters = opts->max_shave_iters;
    } else {
        sopts.max_conflicts   = 200;
        sopts.max_restarts    = 5000;
        sopts.use_phase_save  = 1;
    }

    for (uint32_t round = 0; round < max_rounds; round++) {
        double elapsed = _now_sec() - t_start;
        if (elapsed >= time_limit) break;

        /* Vary seed each round for diversity */
        sopts.seed = (opts ? opts->seed : 42) + round * 1000003ULL;

        SolveResult sr = solver_solve(ctx, &sopts);

        if (sr == SOLVE_OK) {
            int64_t obj_val = solver_get_value(ctx, objective_var_id);

            if (!result->found || obj_val < result->best_objective) {
                result->best_objective = obj_val;
                result->found = 1;
                /* Save solution */
                for (uint32_t i = 0; i < n; i++) {
                    best_values[i] = solver_get_value(ctx, i);
                }
            }
            result->n_rounds = round + 1;

            /* Tighten objective for next round */
            solver_reset(ctx);
            PropResult pr = ctx_tighten_ub64(ctx, objective_var_id,
                                              obj_val - 1);
            if (pr == PROP_CONFLICT) {
                /* Optimal found */
                break;
            }
            pr = solver_propagate(ctx);
            if (pr == PROP_CONFLICT) break;
        } else {
            /* UNSAT or TIMEOUT: current bound is infeasible or budget exhausted */
            break;
        }
    }

    result->elapsed_sec = _now_sec() - t_start;

    /* Restore best solution into solver context */
    if (result->found) {
        solver_reset(ctx);
        for (uint32_t i = 0; i < n; i++) {
            solver_pin_var(ctx, i, best_values[i]);
        }
    }

    return 0;
}
