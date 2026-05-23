#include <stdint.h>
#include <string.h>
#include "zsp_propagator.h"
#include "zsp_ctx.h"
#include "zsp_lcg.h"
#include "zsp_explain.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define PROP_WS(p) ((PropWatchSect *)((char *)(p) + sizeof(Propagator)))

static int32_t i32_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t i32_max(int32_t a, int32_t b) { return a > b ? a : b; }
static int64_t i64_min(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t i64_max(int64_t a, int64_t b) { return a > b ? a : b; }

/* ------------------------------------------------------------------ */
/* Common constructor helpers                                          */
/* ------------------------------------------------------------------ */

/* Register this propagator into the watcher chain for var_id at slot i. */
static void _register_watcher(SolveCtx *ctx, uint32_t prop_ref,
                               uint32_t var_id, uint32_t slot) {
    PropWatchSect *ws = PROP_WS(
        (Propagator *)zsp_pool_ptr(&ctx->pool, prop_ref));
    ws->next_watchers[slot]     = ctx->watcher_heads[var_id];
    ctx->watcher_heads[var_id]  = prop_ref;
}

/* Allocate a propagator block of `size` bytes from the static pool,
   fill the header fields, fill the PropWatchSect, and enqueue it.
   Returns pool offset or EXPR_NULL. */
static uint32_t _alloc_prop(SolveCtx *ctx,
                             PropResult (*fire)(Propagator *, SolveCtx *),
                             uint8_t priority,
                             uint32_t n_watches, const uint32_t *var_ids,
                             uint32_t total_bytes) {
    uint32_t ref = zsp_pool_alloc(&ctx->pool, total_bytes, 8u);
    if (ref == EXPR_NULL) return EXPR_NULL;

    Propagator *p    = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
    memset(p, 0, total_bytes);

    p->fire       = fire;
    p->queue_next = EXPR_NULL;
    p->prop_id    = (uint16_t)ctx->n_props++;
    p->priority   = priority;
    p->flags      = 0;

    PropWatchSect *ws = PROP_WS(p);
    ws->n_watches = n_watches;
    for (uint32_t i = 0; i < n_watches; i++) {
        ws->var_ids[i]      = var_ids[i];
        ws->next_watchers[i] = EXPR_NULL;
    }
    for (uint32_t i = 0; i < n_watches; i++) {
        _register_watcher(ctx, ref, var_ids[i], i);
    }

    prop_enqueue(ctx, ref);

    /* Record prop ref for checkpoint/restore */
    if (ctx->prop_refs && p->prop_id < ctx->n_prop_refs_capacity)
        ctx->prop_refs[p->prop_id] = ref;

    return ref;
}

/* ------------------------------------------------------------------ */
/* BoundsLE_32:  x ≤ y                                                */
/*   ws.var_ids[0] = x, ws.var_ids[1] = y                            */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_le_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];

    PropResult r;
    if ((r = ctx_tighten_ub32(ctx, xid, y->hi)) != PROP_OK) return r;
    if ((r = ctx_tighten_lb32(ctx, yid, x->lo)) != PROP_OK) return r;

    if (x->hi <= y->lo) return PROP_ENTAILED;
    return PROP_OK;
}

uint32_t prop_add_bounds_le_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id,
                                uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    return _alloc_prop(ctx, _fire_bounds_le_32, priority, 2, ids,
                       sizeof(BoundsLE_32_t));
}

/* ------------------------------------------------------------------ */
/* BoundsLT_32:  x < y                                                */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_lt_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];

    PropResult r;
    if ((r = ctx_tighten_ub32(ctx, xid, y->hi - 1)) != PROP_OK) return r;
    if ((r = ctx_tighten_lb32(ctx, yid, x->lo + 1)) != PROP_OK) return r;

    if (x->hi < y->lo) return PROP_ENTAILED;
    return PROP_OK;
}

uint32_t prop_add_bounds_lt_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id,
                                uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    return _alloc_prop(ctx, _fire_bounds_lt_32, priority, 2, ids,
                       sizeof(BoundsLT_32_t));
}

/* ------------------------------------------------------------------ */
/* BoundsEQ_32:  x == y                                               */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_eq_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];

    PropResult r;
    int32_t lo = i32_max(x->lo, y->lo);
    int32_t hi = i32_min(x->hi, y->hi);
    if (lo > hi) return PROP_CONFLICT;

    if ((r = ctx_tighten_lb32(ctx, xid, lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub32(ctx, xid, hi)) != PROP_OK) return r;
    if ((r = ctx_tighten_lb32(ctx, yid, lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub32(ctx, yid, hi)) != PROP_OK) return r;

    if (x->lo == x->hi && y->lo == y->hi) return PROP_ENTAILED;
    return PROP_OK;
}

uint32_t prop_add_bounds_eq_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id,
                                uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    return _alloc_prop(ctx, _fire_bounds_eq_32, priority, 2, ids,
                       sizeof(BoundsEQ_32_t));
}

/* ------------------------------------------------------------------ */
/* BoundsNE_32:  x != y  (singleton-only propagation)                */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_ne_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];

    PropResult r;
    /* if x is singleton, remove from y */
    if (x->lo == x->hi) {
        int32_t v = x->lo;
        if (y->lo == v) {
            if ((r = ctx_tighten_lb32(ctx, yid, v + 1)) != PROP_OK) return r;
        } else if (y->hi == v) {
            if ((r = ctx_tighten_ub32(ctx, yid, v - 1)) != PROP_OK) return r;
        }
    }
    /* if y is singleton, remove from x */
    if (y->lo == y->hi) {
        int32_t v = y->lo;
        if (x->lo == v) {
            if ((r = ctx_tighten_lb32(ctx, xid, v + 1)) != PROP_OK) return r;
        } else if (x->hi == v) {
            if ((r = ctx_tighten_ub32(ctx, xid, v - 1)) != PROP_OK) return r;
        }
    }
    /* entailed if domains don't overlap */
    if (x->hi < y->lo || y->hi < x->lo) return PROP_ENTAILED;
    return PROP_OK;
}

uint32_t prop_add_bounds_ne_32(SolveCtx *ctx, uint32_t x_id, uint32_t y_id,
                                uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    return _alloc_prop(ctx, _fire_bounds_ne_32, priority, 2, ids,
                       sizeof(BoundsNE_32_t));
}

/* ------------------------------------------------------------------ */
/* BoundsAdd_32:  r = a + b                                           */
/*   var_ids[0]=r, var_ids[1]=a, var_ids[2]=b                        */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_add_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];
    Variable      *r   = &ctx->vars[rid];
    Variable      *a   = &ctx->vars[aid];
    Variable      *b   = &ctx->vars[bid];

    PropResult res;
    /* Use 64-bit intermediate arithmetic to avoid int32 overflow when
     * summing large unsigned 32-bit bounds (e.g. 0x7FFFFFFF + 0x02000000). */
    int64_t alo = a->lo, ahi = a->hi, blo = b->lo, bhi = b->hi;
    int64_t rlo = r->lo, rhi = r->hi;

    /* r in [a.lo+b.lo, a.hi+b.hi], clamped to int32 range */
    int64_t fwd_lo = alo + blo;
    int64_t fwd_hi = ahi + bhi;
    if (fwd_lo > INT32_MAX) fwd_lo = INT32_MAX;
    if (fwd_lo < INT32_MIN) fwd_lo = INT32_MIN;
    if (fwd_hi > INT32_MAX) fwd_hi = INT32_MAX;
    if (fwd_hi < INT32_MIN) fwd_hi = INT32_MIN;
    if ((res = ctx_tighten_lb32(ctx, rid, (int32_t)fwd_lo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub32(ctx, rid, (int32_t)fwd_hi)) != PROP_OK) return res;

    /* Re-read r bounds after possible tightening */
    rlo = r->lo; rhi = r->hi;

    /* a in [r.lo-b.hi, r.hi-b.lo] */
    int64_t a_new_lo = rlo - bhi;
    int64_t a_new_hi = rhi - blo;
    if (a_new_lo < INT32_MIN) a_new_lo = INT32_MIN;
    if (a_new_hi > INT32_MAX) a_new_hi = INT32_MAX;
    if ((res = ctx_tighten_lb32(ctx, aid, (int32_t)a_new_lo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub32(ctx, aid, (int32_t)a_new_hi)) != PROP_OK) return res;

    /* b in [r.lo-a.hi, r.hi-a.lo] */
    alo = a->lo; ahi = a->hi;  /* re-read after tightening */
    int64_t b_new_lo = rlo - ahi;
    int64_t b_new_hi = rhi - alo;
    if (b_new_lo < INT32_MIN) b_new_lo = INT32_MIN;
    if (b_new_hi > INT32_MAX) b_new_hi = INT32_MAX;
    if ((res = ctx_tighten_lb32(ctx, bid, (int32_t)b_new_lo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub32(ctx, bid, (int32_t)b_new_hi)) != PROP_OK) return res;

    if (r->lo == r->hi && a->lo == a->hi && b->lo == b->hi) return PROP_ENTAILED;
    return PROP_OK;
}

uint32_t prop_add_bounds_add_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                  uint32_t b_id, uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_add_32, priority, 3, ids,
                       sizeof(BoundsAdd_32_t));
}

/* ------------------------------------------------------------------ */
/* BoundsMul_32:  r = a * b  (conservative: only when one is fixed)  */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_mul_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];
    Variable      *r   = &ctx->vars[rid];
    Variable      *a   = &ctx->vars[aid];
    Variable      *b   = &ctx->vars[bid];

    PropResult res;
    if (a->lo == a->hi) {
        int32_t k = a->lo;
        if (k > 0) {
            if ((res = ctx_tighten_lb32(ctx, rid, k * b->lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, rid, k * b->hi)) != PROP_OK) return res;
            /* Backward: b = r / k */
            if ((res = ctx_tighten_lb32(ctx, bid, r->lo / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, bid, r->hi / k)) != PROP_OK) return res;
        } else if (k < 0) {
            if ((res = ctx_tighten_lb32(ctx, rid, k * b->hi)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, rid, k * b->lo)) != PROP_OK) return res;
            /* Backward: b = r / k (reversed due to negative k) */
            if ((res = ctx_tighten_lb32(ctx, bid, r->hi / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, bid, r->lo / k)) != PROP_OK) return res;
        }
    }
    if (b->lo == b->hi) {
        int32_t k = b->lo;
        if (k > 0) {
            if ((res = ctx_tighten_lb32(ctx, rid, a->lo * k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, rid, a->hi * k)) != PROP_OK) return res;
            /* Backward: a = r / k */
            if ((res = ctx_tighten_lb32(ctx, aid, r->lo / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, aid, r->hi / k)) != PROP_OK) return res;
        } else if (k < 0) {
            if ((res = ctx_tighten_lb32(ctx, rid, a->hi * k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, rid, a->lo * k)) != PROP_OK) return res;
            /* Backward: a = r / k (reversed due to negative k) */
            if ((res = ctx_tighten_lb32(ctx, aid, r->hi / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, aid, r->lo / k)) != PROP_OK) return res;
        }
    }
    return PROP_OK;
}

uint32_t prop_add_bounds_mul_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                  uint32_t b_id, uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_mul_32, priority, 3, ids,
                       sizeof(BoundsMul_32_t));
}

/* ------------------------------------------------------------------ */
/* BoundsDiv_32:  r = a / b  (conservative, b > 0)                   */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_div_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];
    Variable      *a   = &ctx->vars[aid];
    Variable      *b   = &ctx->vars[bid];

    PropResult res;
    if (b->lo == b->hi && b->lo > 0) {
        int32_t k = b->lo;
        /* integer division: floor(a.lo/k) .. floor(a.hi/k) */
        int32_t rlo = a->lo / k;
        int32_t rhi = a->hi / k;
        if (rlo > rhi) { int32_t t = rlo; rlo = rhi; rhi = t; }
        if ((res = ctx_tighten_lb32(ctx, rid, rlo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub32(ctx, rid, rhi)) != PROP_OK) return res;
    }
    (void)aid; (void)bid;
    return PROP_OK;
}

uint32_t prop_add_bounds_div_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                  uint32_t b_id, uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_div_32, priority, 3, ids,
                       sizeof(BoundsDiv_32_t));
}

/* ------------------------------------------------------------------ */
/* BoundsMod_32:  r = a % b  (conservative, b > 0)                   */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_mod_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];
    Variable      *r   = &ctx->vars[rid];
    Variable      *a   = &ctx->vars[aid];
    Variable      *b   = &ctx->vars[bid];

    PropResult res;

    /* Forward: tighten r range */
    if (b->lo == b->hi && b->lo > 0) {
        int32_t k = b->lo;
        if ((res = ctx_tighten_lb32(ctx, rid, 0))     != PROP_OK) return res;
        if ((res = ctx_tighten_ub32(ctx, rid, k - 1)) != PROP_OK) return res;
    }

    /* Forward: singleton a and b -> compute r exactly */
    if (a->lo == a->hi && b->lo == b->hi && b->lo > 0) {
        int32_t val = a->lo % b->lo;
        if (val < 0) val += b->lo;
        if ((res = ctx_tighten_lb32(ctx, rid, val)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub32(ctx, rid, val)) != PROP_OK) return res;
    }

    /* Backward: r and b singletons -> constrain a */
    r = &ctx->vars[rid]; a = &ctx->vars[aid]; b = &ctx->vars[bid];
    if (r->lo == r->hi && b->lo == b->hi && b->lo > 0) {
        int32_t r_val = r->lo;
        int32_t b_val = b->lo;
        int32_t cur_lo = a->lo;
        int32_t cur_hi = a->hi;

        int32_t rem = cur_lo % b_val;
        if (rem < 0) rem += b_val;
        int32_t new_lo = cur_lo + (int32_t)(((int64_t)r_val - rem + b_val) % b_val);
        if (new_lo > cur_hi) return PROP_CONFLICT;
        if ((res = ctx_tighten_lb32(ctx, aid, new_lo)) != PROP_OK) return res;

        rem = cur_hi % b_val;
        if (rem < 0) rem += b_val;
        int32_t new_hi = cur_hi - (int32_t)(((int64_t)rem - r_val + b_val) % b_val);
        if (new_hi < new_lo) return PROP_CONFLICT;
        if ((res = ctx_tighten_ub32(ctx, aid, new_hi)) != PROP_OK) return res;
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_mod_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                  uint32_t b_id, uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_mod_32, priority, 3, ids,
                       sizeof(BoundsMod_32_t));
}

/* ------------------------------------------------------------------ */
/* UnaryNeg_32:  r = -a                                               */
/* ------------------------------------------------------------------ */

static PropResult _fire_unary_neg_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    Variable      *r   = &ctx->vars[rid];
    Variable      *a   = &ctx->vars[aid];

    PropResult res;
    if ((res = ctx_tighten_lb32(ctx, rid, -a->hi)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub32(ctx, rid, -a->lo)) != PROP_OK) return res;
    if ((res = ctx_tighten_lb32(ctx, aid, -r->hi)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub32(ctx, aid, -r->lo)) != PROP_OK) return res;

    if (r->lo == r->hi && a->lo == a->hi) return PROP_ENTAILED;
    return PROP_OK;
}

uint32_t prop_add_unary_neg_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                uint8_t priority) {
    uint32_t ids[2] = { r_id, a_id };
    return _alloc_prop(ctx, _fire_unary_neg_32, priority, 2, ids,
                       sizeof(UnaryNeg_32_t));
}

/* ------------------------------------------------------------------ */
/* InSet_32:  x ∈ {elems[0], …}                                      */
/* ------------------------------------------------------------------ */

static PropResult _fire_in_set_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws    = PROP_WS(self);
    uint32_t       xid   = ws->var_ids[0];
    Variable      *x     = &ctx->vars[xid];
    InSet_32_t    *iself = (InSet_32_t *)self;
    int32_t       *elems = (int32_t *)((char *)self + sizeof(InSet_32_t));
    uint32_t       n     = iself->n_elems;

    PropResult res;
    /* Find min/max in set that overlap [x->lo, x->hi] */
    int32_t new_lo = INT32_MAX, new_hi = INT32_MIN;
    for (uint32_t i = 0; i < n; i++) {
        if (elems[i] >= x->lo && elems[i] <= x->hi) {
            if (elems[i] < new_lo) new_lo = elems[i];
            if (elems[i] > new_hi) new_hi = elems[i];
        }
    }
    if (new_lo == INT32_MAX) return PROP_CONFLICT;  /* no valid element */

    if ((res = ctx_tighten_lb32(ctx, xid, new_lo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub32(ctx, xid, new_hi)) != PROP_OK) return res;
    return PROP_OK;
}

uint32_t prop_add_in_set_32(SolveCtx *ctx, uint32_t x_id,
                              uint32_t n_elems, const int32_t *elems,
                              uint8_t priority) {
    uint32_t ids[1] = { x_id };
    uint32_t sz = (uint32_t)(sizeof(InSet_32_t) + n_elems * sizeof(int32_t));
    uint32_t ref = _alloc_prop(ctx, _fire_in_set_32, priority, 1, ids, sz);
    if (ref == EXPR_NULL) return EXPR_NULL;

    InSet_32_t *p = (InSet_32_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->n_elems = n_elems;
    int32_t   *dst = (int32_t *)((char *)p + sizeof(InSet_32_t));
    memcpy(dst, elems, n_elems * sizeof(int32_t));
    return ref;
}

/* ------------------------------------------------------------------ */
/* Implication_32:  guard → (var ≤/≥ bound)                          */
/*   var_ids[0]=guard, var_ids[1]=var                                 */
/* ------------------------------------------------------------------ */

static PropResult _fire_implication_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect    *ws    = PROP_WS(self);
    Implication_32_t *iself = (Implication_32_t *)self;
    uint32_t          gid   = ws->var_ids[0];
    uint32_t          vid   = ws->var_ids[1];
    Variable         *g     = &ctx->vars[gid];

    /* guard is definitively false → entailed */
    if (g->hi == 0) return PROP_ENTAILED;

    /* guard is definitively true → enforce bound */
    if (g->lo == 1) {
        PropResult r;
        if (iself->is_ub)
            r = ctx_tighten_ub32(ctx, vid, iself->bound);
        else
            r = ctx_tighten_lb32(ctx, vid, iself->bound);
        return r;
    }
    return PROP_OK;
}

uint32_t prop_add_implication_32(SolveCtx *ctx,
                                   uint32_t guard_id, uint32_t var_id,
                                   int32_t bound, uint8_t is_ub,
                                   uint8_t priority) {
    uint32_t ids[2] = { guard_id, var_id };
    uint32_t ref = _alloc_prop(ctx, _fire_implication_32, priority, 2, ids,
                                sizeof(Implication_32_t));
    if (ref == EXPR_NULL) return EXPR_NULL;

    Implication_32_t *p = (Implication_32_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->bound = bound;
    p->is_ub = is_ub;
    return ref;
}

/* ------------------------------------------------------------------ */
/* Reification_32:  guard ↔ (x ≤ y)  (stub — only one direction)    */
/* ------------------------------------------------------------------ */

static PropResult _fire_reification_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       gid = ws->var_ids[0];
    uint32_t       xid = ws->var_ids[1];
    uint32_t       yid = ws->var_ids[2];
    Variable      *g   = &ctx->vars[gid];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];

    /* if guard=1, enforce x ≤ y */
    if (g->lo == 1) {
        PropResult r;
        if ((r = ctx_tighten_ub32(ctx, xid, y->hi)) != PROP_OK) return r;
        if ((r = ctx_tighten_lb32(ctx, yid, x->lo)) != PROP_OK) return r;
    }
    /* if guard=0, x > y must hold — tighten lb of x above y.hi */
    if (g->hi == 0) {
        PropResult r;
        if ((r = ctx_tighten_lb32(ctx, xid, y->hi + 1)) != PROP_OK) return r;
    }
    /* Backward: if domain proves x ≤ y unconditionally, force guard=1.
     * If domain proves x > y unconditionally, force guard=0. */
    if (g->lo != g->hi) {
        if (x->hi <= y->lo) {
            PropResult r;
            if ((r = ctx_tighten_lb32(ctx, gid, 1)) != PROP_OK) return r;
        } else if (x->lo > y->hi) {
            PropResult r;
            if ((r = ctx_tighten_ub32(ctx, gid, 0)) != PROP_OK) return r;
        }
    }
    return PROP_OK;
}

uint32_t prop_add_reification_32(SolveCtx *ctx, uint32_t guard_id,
                                   uint32_t x_id, uint32_t y_id,
                                   uint8_t priority) {
    uint32_t ids[3] = { guard_id, x_id, y_id };
    return _alloc_prop(ctx, _fire_reification_32, priority, 3, ids,
                       sizeof(Reification_32_t));
}

/* ------------------------------------------------------------------ */
/* ReificationEq_32: guard <-> (x == y)                                */
/* ------------------------------------------------------------------ */

static PropResult _fire_reification_eq_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       gid = ws->var_ids[0];
    uint32_t       xid = ws->var_ids[1];
    uint32_t       yid = ws->var_ids[2];
    Variable      *g   = &ctx->vars[gid];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];

    /* Forward: guard=1 -> enforce x == y (intersect bounds) */
    if (g->lo == 1) {
        PropResult r;
        int32_t new_lo = x->lo > y->lo ? x->lo : y->lo;
        int32_t new_hi = x->hi < y->hi ? x->hi : y->hi;
        if (new_lo > new_hi) return PROP_CONFLICT;
        if ((r = ctx_tighten_lb32(ctx, xid, new_lo)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub32(ctx, xid, new_hi)) != PROP_OK) return r;
        if ((r = ctx_tighten_lb32(ctx, yid, new_lo)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub32(ctx, yid, new_hi)) != PROP_OK) return r;
    }
    /* Forward: guard=0 -> x != y. Only propagate when one is singleton. */
    if (g->hi == 0) {
        if (x->lo == x->hi && y->lo == x->lo && y->hi == x->hi) {
            /* Both pinned to same value: conflict */
            return PROP_CONFLICT;
        }
        if (x->lo == x->hi) {
            /* x is singleton: exclude x->lo from y */
            if (y->lo == x->lo) {
                PropResult r;
                if ((r = ctx_tighten_lb32(ctx, yid, x->lo + 1)) != PROP_OK) return r;
            }
            if (y->hi == x->lo) {
                PropResult r;
                if ((r = ctx_tighten_ub32(ctx, yid, x->lo - 1)) != PROP_OK) return r;
            }
        }
        if (y->lo == y->hi) {
            /* y is singleton: exclude y->lo from x */
            if (x->lo == y->lo) {
                PropResult r;
                if ((r = ctx_tighten_lb32(ctx, xid, y->lo + 1)) != PROP_OK) return r;
            }
            if (x->hi == y->lo) {
                PropResult r;
                if ((r = ctx_tighten_ub32(ctx, xid, y->lo - 1)) != PROP_OK) return r;
            }
        }
    }
    /* Backward: if x definitely == y (both singletons with same value), guard=1 */
    if (x->lo == x->hi && y->lo == y->hi && x->lo == y->lo) {
        PropResult r;
        if ((r = ctx_tighten_lb32(ctx, gid, 1)) != PROP_OK) return r;
    }
    /* Backward: if x and y domains don't overlap, guard=0 */
    if (x->lo > y->hi || y->lo > x->hi) {
        PropResult r;
        if ((r = ctx_tighten_ub32(ctx, gid, 0)) != PROP_OK) return r;
    }
    return PROP_OK;
}

uint32_t prop_add_reification_eq_32(SolveCtx *ctx, uint32_t guard_id,
                                     uint32_t x_id, uint32_t y_id,
                                     uint8_t priority) {
    uint32_t ids[3] = { guard_id, x_id, y_id };
    return _alloc_prop(ctx, _fire_reification_eq_32, priority, 3, ids,
                       sizeof(ReificationEq_32_t));
}

/* ------------------------------------------------------------------ */
/* BitSlice_32:  r = a[hi_bit:lo_bit]                                 */
/* ------------------------------------------------------------------ */

static PropResult _fire_bit_slice_32(Propagator *self, SolveCtx *ctx) {
    BitSlice_32_t *bself = (BitSlice_32_t *)self;
    PropWatchSect *ws    = PROP_WS(self);
    uint32_t       rid   = ws->var_ids[0];
    uint32_t       aid   = ws->var_ids[1];

    uint32_t width = (uint32_t)(bself->hi_bit - bself->lo_bit + 1);
    int32_t  max_v = (width < 32) ? (int32_t)((1u << width) - 1) : INT32_MAX;

    PropResult res;
    if ((res = ctx_tighten_lb32(ctx, rid, 0))     != PROP_OK) return res;
    if ((res = ctx_tighten_ub32(ctx, rid, max_v)) != PROP_OK) return res;

    /* When the operand is pinned, compute r exactly. Also back-propagate
     * a singleton r into the operand bits when the operand is loose by
     * only one bit-slice position. */
    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    if (alo == ahi) {
        uint64_t mask = (width >= 64) ? ~(uint64_t)0
                                     : (((uint64_t)1 << width) - 1);
        uint64_t v = ((uint64_t)alo >> bself->lo_bit) & mask;
        if ((res = ctx_tighten_lb32(ctx, rid, (int32_t)v)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub32(ctx, rid, (int32_t)v)) != PROP_OK) return res;
        return PROP_ENTAILED;
    }
    return PROP_OK;
}

uint32_t prop_add_bit_slice_32(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                uint8_t hi_bit, uint8_t lo_bit,
                                uint8_t priority) {
    uint32_t ids[2] = { r_id, a_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bit_slice_32, priority, 2, ids,
                                sizeof(BitSlice_32_t));
    if (ref == EXPR_NULL) return EXPR_NULL;

    BitSlice_32_t *p = (BitSlice_32_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->hi_bit = hi_bit;
    p->lo_bit = lo_bit;
    return ref;
}

/* ================================================================== */
/* _64 variants                                                       */
/* ================================================================== */

static PropResult _fire_bounds_le_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];

    PropResult r;
    if ((r = ctx_tighten_ub64(ctx, xid, var_hi64(ctx, &ctx->vars[yid]))) != PROP_OK) return r;
    if ((r = ctx_tighten_lb64(ctx, yid, var_lo64(ctx, &ctx->vars[xid]))) != PROP_OK) return r;
    return PROP_OK;
}
/* Explain: x <= y. We tighten x.hi from y.hi and y.lo from x.lo. */
static int _explain_bounds_le_64(Propagator *self, SolveCtx *ctx,
                                  uint32_t var_id, uint8_t is_lb,
                                  int64_t new_bound, Explanation *out) {
    (void)ctx;
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    uint32_t yid = ws->var_ids[1];
    out->n_lits = 1;
    out->lits[0]._pad[0] = out->lits[0]._pad[1] = out->lits[0]._pad[2] = 0;
    if (var_id == xid && !is_lb) {
        /* x.hi <= new_bound because y.hi <= new_bound */
        out->lits[0].var_id = yid;
        out->lits[0].is_lb  = 0;
        out->lits[0].bound  = (int32_t)new_bound;
        return 0;
    }
    if (var_id == yid && is_lb) {
        /* y.lo >= new_bound because x.lo >= new_bound */
        out->lits[0].var_id = xid;
        out->lits[0].is_lb  = 1;
        out->lits[0].bound  = (int32_t)new_bound;
        return 0;
    }
    return -1;
}
uint32_t prop_add_bounds_le_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bounds_le_64, priority, 2, ids, sizeof(BoundsLE_64_t));
    if (ref != EXPR_NULL) {
        Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
        p->explain = _explain_bounds_le_64;
    }
    return ref;
}

static PropResult _fire_bounds_lt_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];

    PropResult r;
    if ((r = ctx_tighten_ub64(ctx, xid, var_hi64(ctx, &ctx->vars[yid]) - 1)) != PROP_OK) return r;
    if ((r = ctx_tighten_lb64(ctx, yid, var_lo64(ctx, &ctx->vars[xid]) + 1)) != PROP_OK) return r;
    return PROP_OK;
}
/* Explain: x < y. x.hi = y.hi - 1 ⇒ (y >= new_bound+1).
 *          y.lo = x.lo + 1 ⇒ (x <= new_bound-1). */
static int _explain_bounds_lt_64(Propagator *self, SolveCtx *ctx,
                                  uint32_t var_id, uint8_t is_lb,
                                  int64_t new_bound, Explanation *out) {
    (void)ctx;
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    uint32_t yid = ws->var_ids[1];
    out->n_lits = 1;
    out->lits[0]._pad[0] = out->lits[0]._pad[1] = out->lits[0]._pad[2] = 0;
    if (var_id == xid && !is_lb) {
        /* x.hi <= new_bound because y.hi <= new_bound+1 i.e. (y <= new_bound+1) */
        out->lits[0].var_id = yid;
        out->lits[0].is_lb  = 0;
        out->lits[0].bound  = (int32_t)(new_bound + 1);
        return 0;
    }
    if (var_id == yid && is_lb) {
        /* y.lo >= new_bound because x.lo >= new_bound-1 i.e. (x >= new_bound-1) */
        out->lits[0].var_id = xid;
        out->lits[0].is_lb  = 1;
        out->lits[0].bound  = (int32_t)(new_bound - 1);
        return 0;
    }
    return -1;
}
uint32_t prop_add_bounds_lt_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bounds_lt_64, priority, 2, ids, sizeof(BoundsLT_64_t));
    if (ref != EXPR_NULL) {
        Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
        p->explain = _explain_bounds_lt_64;
    }
    return ref;
}

static PropResult _fire_bounds_eq_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];

    int64_t lo = i64_max(var_lo64(ctx, &ctx->vars[xid]), var_lo64(ctx, &ctx->vars[yid]));
    int64_t hi = i64_min(var_hi64(ctx, &ctx->vars[xid]), var_hi64(ctx, &ctx->vars[yid]));
    if (lo > hi) return PROP_CONFLICT;

    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, xid, lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, xid, hi)) != PROP_OK) return r;
    if ((r = ctx_tighten_lb64(ctx, yid, lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, yid, hi)) != PROP_OK) return r;
    return PROP_OK;
}
/* Explain: x == y. Whichever bound was tightened on one side came
 * from the same-direction bound on the other side. */
static int _explain_bounds_eq_64(Propagator *self, SolveCtx *ctx,
                                  uint32_t var_id, uint8_t is_lb,
                                  int64_t new_bound, Explanation *out) {
    (void)ctx;
    PropWatchSect *ws = PROP_WS(self);
    uint32_t xid = ws->var_ids[0];
    uint32_t yid = ws->var_ids[1];
    uint32_t other = (var_id == xid) ? yid : (var_id == yid ? xid : EXPR_NULL);
    if (other == EXPR_NULL) return -1;
    out->n_lits = 1;
    out->lits[0].var_id = other;
    out->lits[0].is_lb  = is_lb;
    out->lits[0].bound  = (int32_t)new_bound;
    out->lits[0]._pad[0] = out->lits[0]._pad[1] = out->lits[0]._pad[2] = 0;
    return 0;
}
uint32_t prop_add_bounds_eq_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bounds_eq_64, priority, 2, ids, sizeof(BoundsEQ_64_t));
    if (ref != EXPR_NULL) {
        Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
        p->explain = _explain_bounds_eq_64;
    }
    return ref;
}

static PropResult _fire_bounds_ne_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       xid = ws->var_ids[0];
    uint32_t       yid = ws->var_ids[1];
    int64_t xlo = var_lo64(ctx, &ctx->vars[xid]), xhi = var_hi64(ctx, &ctx->vars[xid]);
    int64_t ylo = var_lo64(ctx, &ctx->vars[yid]), yhi = var_hi64(ctx, &ctx->vars[yid]);

    PropResult r;
    if (xlo == xhi) {
        int64_t v = xlo;
        if (ylo == v) { if ((r = ctx_tighten_lb64(ctx, yid, v+1)) != PROP_OK) return r; }
        else if (yhi == v) { if ((r = ctx_tighten_ub64(ctx, yid, v-1)) != PROP_OK) return r; }
    }
    if (ylo == yhi) {
        int64_t v = ylo;
        if (xlo == v) { if ((r = ctx_tighten_lb64(ctx, xid, v+1)) != PROP_OK) return r; }
        else if (xhi == v) { if ((r = ctx_tighten_ub64(ctx, xid, v-1)) != PROP_OK) return r; }
    }
    return PROP_OK;
}
uint32_t prop_add_bounds_ne_64(SolveCtx *ctx, uint32_t x_id, uint32_t y_id, uint8_t priority) {
    uint32_t ids[2] = { x_id, y_id };
    return _alloc_prop(ctx, _fire_bounds_ne_64, priority, 2, ids, sizeof(BoundsNE_64_t));
}

static PropResult _fire_bounds_add_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];
    int64_t rlo = var_lo64(ctx,&ctx->vars[rid]), rhi = var_hi64(ctx,&ctx->vars[rid]);
    int64_t alo = var_lo64(ctx,&ctx->vars[aid]), ahi = var_hi64(ctx,&ctx->vars[aid]);
    int64_t blo = var_lo64(ctx,&ctx->vars[bid]), bhi = var_hi64(ctx,&ctx->vars[bid]);

    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, rid, alo+blo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, rid, ahi+bhi)) != PROP_OK) return r;
    if ((r = ctx_tighten_lb64(ctx, aid, rlo-bhi)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, aid, rhi-blo)) != PROP_OK) return r;
    if ((r = ctx_tighten_lb64(ctx, bid, rlo-ahi)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, bid, rhi-alo)) != PROP_OK) return r;
    return PROP_OK;
}
/* Explain: r = a + b. Two-literal explanations using current bounds. */
static int _explain_bounds_add_64(Propagator *self, SolveCtx *ctx,
                                   uint32_t var_id, uint8_t is_lb,
                                   int64_t new_bound, Explanation *out) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0];
    uint32_t aid = ws->var_ids[1];
    uint32_t bid = ws->var_ids[2];
    out->n_lits = 2;
    for (int i = 0; i < 2; i++) {
        out->lits[i]._pad[0] = out->lits[i]._pad[1] = out->lits[i]._pad[2] = 0;
    }
    /* For each tightening direction, build a sound explanation using
     * the current bounds on the two "other" variables. Monotonicity
     * keeps these literals true at conflict-analysis time. */
    if (var_id == rid && is_lb) {
        /* r.lo >= new_bound because a.lo + b.lo >= new_bound */
        int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
        int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
        out->lits[0].var_id = aid; out->lits[0].is_lb = 1; out->lits[0].bound = (int32_t)alo;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 1; out->lits[1].bound = (int32_t)blo;
        return 0;
    }
    if (var_id == rid && !is_lb) {
        int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
        int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
        out->lits[0].var_id = aid; out->lits[0].is_lb = 0; out->lits[0].bound = (int32_t)ahi;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 0; out->lits[1].bound = (int32_t)bhi;
        return 0;
    }
    if (var_id == aid && is_lb) {
        /* a.lo >= new_bound because r.lo - b.hi >= new_bound */
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 1; out->lits[0].bound = (int32_t)rlo;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 0; out->lits[1].bound = (int32_t)bhi;
        return 0;
    }
    if (var_id == aid && !is_lb) {
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 0; out->lits[0].bound = (int32_t)rhi;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 1; out->lits[1].bound = (int32_t)blo;
        return 0;
    }
    if (var_id == bid && is_lb) {
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 1; out->lits[0].bound = (int32_t)rlo;
        out->lits[1].var_id = aid; out->lits[1].is_lb = 0; out->lits[1].bound = (int32_t)ahi;
        return 0;
    }
    if (var_id == bid && !is_lb) {
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 0; out->lits[0].bound = (int32_t)rhi;
        out->lits[1].var_id = aid; out->lits[1].is_lb = 1; out->lits[1].bound = (int32_t)alo;
        return 0;
    }
    return -1;
}
uint32_t prop_add_bounds_add_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bounds_add_64, priority, 3, ids, sizeof(BoundsAdd_64_t));
    if (ref != EXPR_NULL) {
        Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
        p->explain = _explain_bounds_add_64;
    }
    return ref;
}

/* ------------------------------------------------------------------ */
/* BvAddConst_64:  r = (x + c) mod 2^width  (BV modular add with const) */
/*   var_ids[0]=r, var_ids[1]=x                                       */
/* ------------------------------------------------------------------ */
static PropResult _fire_bvadd_const_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       xid = ws->var_ids[1];
    BvAddConst_64_t *bp = (BvAddConst_64_t *)self;
    uint8_t  w = bp->width;
    uint64_t c = bp->c;

    /* w==64 is not handled cleanly: bounds are stored signed and the wrap
     * would split [0, 2^63-1] vs [2^63, 2^64-1] in ways that mix with
     * sign. Fall back to non-modular bounds (caller should use the
     * regular bounds_add path for w=64). */
    if (w == 0 || w >= 64) return PROP_OK;

    uint64_t M = (uint64_t)1 << w;
    /* c reduced mod M; the constructor already does this but be defensive */
    c &= (M - 1);

    uint64_t xlo = (uint64_t)var_lo64(ctx, &ctx->vars[xid]);
    uint64_t xhi = (uint64_t)var_hi64(ctx, &ctx->vars[xid]);

    PropResult res;

    /* Forward: r = (x + c) mod M.  Three sub-cases by where xlo+c, xhi+c
     * land relative to M:
     *   - xhi+c < M           : no wrap, single interval [xlo+c, xhi+c]
     *   - xlo+c >= M          : full wrap, single interval [xlo+c-M, xhi+c-M]
     *   - else                : partial wrap, two disjoint intervals
     *                           [xlo+c, M-1] ∪ [0, xhi+c-M]. We only
     *                           tighten when r's current interval lies
     *                           entirely inside one piece. */
    if (xhi + c < M) {
        if ((res = ctx_tighten_lb64(ctx, rid, (int64_t)(xlo + c))) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, (int64_t)(xhi + c))) != PROP_OK) return res;
    } else if (xlo + c >= M) {
        if ((res = ctx_tighten_lb64(ctx, rid, (int64_t)(xlo + c - M))) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, (int64_t)(xhi + c - M))) != PROP_OK) return res;
    } else {
        uint64_t up_lo = xlo + c;            /* lower bound of upper piece */
        uint64_t low_hi = xhi + c - M;       /* upper bound of lower piece */
        uint64_t rlo = (uint64_t)var_lo64(ctx, &ctx->vars[rid]);
        uint64_t rhi = (uint64_t)var_hi64(ctx, &ctx->vars[rid]);
        if (rlo > low_hi) {
            /* r confined to upper piece [up_lo, M-1] */
            if ((res = ctx_tighten_lb64(ctx, rid, (int64_t)up_lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, (int64_t)(M - 1))) != PROP_OK) return res;
        } else if (rhi < up_lo) {
            /* r confined to lower piece [0, low_hi] */
            if ((res = ctx_tighten_lb64(ctx, rid, 0)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, (int64_t)low_hi)) != PROP_OK) return res;
        }
    }

    /* Backward: x = (r - c) mod M = (r + (M-c)) mod M */
    uint64_t nc  = (c == 0) ? 0 : (M - c);
    uint64_t rlo = (uint64_t)var_lo64(ctx, &ctx->vars[rid]);
    uint64_t rhi = (uint64_t)var_hi64(ctx, &ctx->vars[rid]);

    if (rhi + nc < M) {
        if ((res = ctx_tighten_lb64(ctx, xid, (int64_t)(rlo + nc))) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, xid, (int64_t)(rhi + nc))) != PROP_OK) return res;
    } else if (rlo + nc >= M) {
        if ((res = ctx_tighten_lb64(ctx, xid, (int64_t)(rlo + nc - M))) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, xid, (int64_t)(rhi + nc - M))) != PROP_OK) return res;
    } else {
        uint64_t up_lo  = rlo + nc;
        uint64_t low_hi = rhi + nc - M;
        uint64_t xlo_cur = (uint64_t)var_lo64(ctx, &ctx->vars[xid]);
        uint64_t xhi_cur = (uint64_t)var_hi64(ctx, &ctx->vars[xid]);
        if (xlo_cur > low_hi) {
            if ((res = ctx_tighten_lb64(ctx, xid, (int64_t)up_lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, xid, (int64_t)(M - 1))) != PROP_OK) return res;
        } else if (xhi_cur < up_lo) {
            if ((res = ctx_tighten_lb64(ctx, xid, 0)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, xid, (int64_t)low_hi)) != PROP_OK) return res;
        }
    }

    return PROP_OK;
}

/* Explain: r = (x + c) mod 2^w. The modular tightening depends on the
 * full current domain of the other variable (since wrap can split the
 * feasible region into two pieces). Conservative sound explanation:
 * both LB and UB of the "other" variable. */
static int _explain_bvadd_const_64(Propagator *self, SolveCtx *ctx,
                                    uint32_t var_id, uint8_t is_lb,
                                    int64_t new_bound, Explanation *out) {
    (void)is_lb; (void)new_bound;
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0];
    uint32_t xid = ws->var_ids[1];
    uint32_t other = (var_id == rid) ? xid : (var_id == xid ? rid : EXPR_NULL);
    if (other == EXPR_NULL) return -1;
    int64_t olo = var_lo64(ctx, &ctx->vars[other]);
    int64_t ohi = var_hi64(ctx, &ctx->vars[other]);
    out->n_lits = 2;
    out->lits[0].var_id = other; out->lits[0].is_lb = 1; out->lits[0].bound = (int32_t)olo;
    out->lits[0]._pad[0] = out->lits[0]._pad[1] = out->lits[0]._pad[2] = 0;
    out->lits[1].var_id = other; out->lits[1].is_lb = 0; out->lits[1].bound = (int32_t)ohi;
    out->lits[1]._pad[0] = out->lits[1]._pad[1] = out->lits[1]._pad[2] = 0;
    return 0;
}

uint32_t prop_add_bvadd_const_64(SolveCtx *ctx, uint32_t r_id, uint32_t x_id,
                                  uint64_t c, uint8_t width, uint8_t priority) {
    /* Reduce c mod 2^width */
    if (width < 64) c &= ((uint64_t)1 << width) - 1;
    uint32_t ids[2] = { r_id, x_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bvadd_const_64, priority, 2, ids,
                                sizeof(BvAddConst_64_t));
    if (ref == EXPR_NULL) return ref;
    BvAddConst_64_t *bp = (BvAddConst_64_t *)zsp_pool_ptr(&ctx->pool, ref);
    bp->c     = c;
    bp->width = width;
    Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
    p->explain = _explain_bvadd_const_64;
    return ref;
}

/* Stubs for Mul/Div/Mod _64 (conservative: no propagation) */
static PropResult _fire_noop(Propagator *self, SolveCtx *ctx) { (void)self;(void)ctx; return PROP_OK; }

/* ------------------------------------------------------------------ */
/* BoundsMul_64:  r = a * b  (singleton specialisation + range approx) */
/* ------------------------------------------------------------------ */

/* Overflow-safe 64-bit multiply: returns 1 if a*b overflows int64 */
static int _mul64_overflow(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) { *out = 0; return 0; }
#ifdef __GNUC__
    return __builtin_mul_overflow(a, b, out);
#else
    /* Conservative fallback: clamp to INT64_MIN/MAX */
    int64_t res = a * b;
    if (a != 0 && res / a != b) {
        *out = ((a > 0) == (b > 0)) ? INT64_MAX : INT64_MIN;
        return 1;
    }
    *out = res;
    return 0;
#endif
}

static PropResult _fire_bounds_mul_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Singleton a: r = k * b */
    if (alo == ahi) {
        int64_t k = alo;
        if (k > 0) {
            int64_t fwd_lo, fwd_hi;
            if (!_mul64_overflow(k, blo, &fwd_lo))
                if ((res = ctx_tighten_lb64(ctx, rid, fwd_lo)) != PROP_OK) return res;
            if (!_mul64_overflow(k, bhi, &fwd_hi))
                if ((res = ctx_tighten_ub64(ctx, rid, fwd_hi)) != PROP_OK) return res;
            /* Backward: b = r / k */
            int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
            int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
            if ((res = ctx_tighten_lb64(ctx, bid, rlo / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, bid, rhi / k)) != PROP_OK) return res;
        } else if (k < 0) {
            int64_t fwd_lo, fwd_hi;
            if (!_mul64_overflow(k, bhi, &fwd_lo))
                if ((res = ctx_tighten_lb64(ctx, rid, fwd_lo)) != PROP_OK) return res;
            if (!_mul64_overflow(k, blo, &fwd_hi))
                if ((res = ctx_tighten_ub64(ctx, rid, fwd_hi)) != PROP_OK) return res;
            /* Backward: b = r / k (reversed due to negative k) */
            int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
            int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
            if ((res = ctx_tighten_lb64(ctx, bid, rhi / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, bid, rlo / k)) != PROP_OK) return res;
        }
        /* k == 0: r must be 0 */
        if (k == 0) {
            if ((res = ctx_tighten_lb64(ctx, rid, 0)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, 0)) != PROP_OK) return res;
        }
    }

    /* Singleton b: r = a * k (symmetric case) */
    if (blo == bhi) {
        int64_t k = blo;
        if (k > 0) {
            int64_t fwd_lo, fwd_hi;
            if (!_mul64_overflow(alo, k, &fwd_lo))
                if ((res = ctx_tighten_lb64(ctx, rid, fwd_lo)) != PROP_OK) return res;
            if (!_mul64_overflow(ahi, k, &fwd_hi))
                if ((res = ctx_tighten_ub64(ctx, rid, fwd_hi)) != PROP_OK) return res;
            int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
            int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
            if ((res = ctx_tighten_lb64(ctx, aid, rlo / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, aid, rhi / k)) != PROP_OK) return res;
        } else if (k < 0) {
            int64_t fwd_lo, fwd_hi;
            if (!_mul64_overflow(ahi, k, &fwd_lo))
                if ((res = ctx_tighten_lb64(ctx, rid, fwd_lo)) != PROP_OK) return res;
            if (!_mul64_overflow(alo, k, &fwd_hi))
                if ((res = ctx_tighten_ub64(ctx, rid, fwd_hi)) != PROP_OK) return res;
            int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
            int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
            if ((res = ctx_tighten_lb64(ctx, aid, rhi / k)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, aid, rlo / k)) != PROP_OK) return res;
        }
        if (k == 0) {
            if ((res = ctx_tighten_lb64(ctx, rid, 0)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, 0)) != PROP_OK) return res;
        }
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_mul_64(SolveCtx *c, uint32_t r, uint32_t a, uint32_t b, uint8_t p) {
    uint32_t ids[3]={r,a,b};
    return _alloc_prop(c, _fire_bounds_mul_64, p, 3, ids, sizeof(BoundsMul_64_t));
}

/* ------------------------------------------------------------------ */
/* BoundsDiv_64:  r = a / b  (conservative, singleton b > 0)          */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_div_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Only propagate when divisor is a non-zero singleton or positive range */
    if (blo == bhi && blo != 0) {
        int64_t k = blo;
        int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
        int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
        int64_t rlo, rhi;
        if (k > 0) {
            rlo = alo / k;
            rhi = ahi / k;
        } else {
            rlo = ahi / k;
            rhi = alo / k;
        }
        if (rlo > rhi) { int64_t t = rlo; rlo = rhi; rhi = t; }
        if ((res = ctx_tighten_lb64(ctx, rid, rlo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, rhi)) != PROP_OK) return res;
    } else if (blo > 0) {
        /* Divisor is positive range: r in [a_lo/b_hi, a_hi/b_lo] */
        int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
        int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
        int64_t rlo = alo / bhi;
        int64_t rhi = ahi / blo;
        if ((res = ctx_tighten_lb64(ctx, rid, rlo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, rhi)) != PROP_OK) return res;
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_div_64(SolveCtx *c, uint32_t r, uint32_t a, uint32_t b, uint8_t p) {
    uint32_t ids[3]={r,a,b};
    return _alloc_prop(c, _fire_bounds_div_64, p, 3, ids, sizeof(BoundsDiv_64_t));
}

/* ------------------------------------------------------------------ */
/* BoundsMod_64:  r = a % b  (tighten r range from b)                */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_mod_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Forward: tighten r from b */
    if (blo == bhi && blo > 0) {
        int64_t k = blo;
        if ((res = ctx_tighten_lb64(ctx, rid, 0))     != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, k - 1)) != PROP_OK) return res;
    } else if (blo > 0) {
        if ((res = ctx_tighten_lb64(ctx, rid, 0))       != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, bhi - 1)) != PROP_OK) return res;
    }

    /* Forward: when a is singleton, compute r exactly */
    if (alo == ahi && blo == bhi && blo > 0) {
        int64_t val = alo % blo;
        if (val < 0) val += blo;  /* ensure non-negative remainder */
        if ((res = ctx_tighten_lb64(ctx, rid, val)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, val)) != PROP_OK) return res;
    }

    /* Backward: when r and b are singletons, constrain a.
     * a % b == r means a = b*k + r for some integer k >= 0.
     * Tighten a_lo up to the nearest value >= a_lo with a%b == r,
     * and a_hi down to the nearest value <= a_hi with a%b == r. */
    rlo = var_lo64(ctx, &ctx->vars[rid]);
    rhi = var_hi64(ctx, &ctx->vars[rid]);
    if (rlo == rhi && blo == bhi && blo > 0) {
        int64_t r_val = rlo;
        int64_t b_val = blo;
        /* Refresh a bounds after possible forward tightening */
        alo = var_lo64(ctx, &ctx->vars[aid]);
        ahi = var_hi64(ctx, &ctx->vars[aid]);

        /* Find smallest a >= alo with a % b == r */
        int64_t rem = alo % b_val;
        if (rem < 0) rem += b_val;
        int64_t new_alo = alo + ((r_val - rem + b_val) % b_val);
        if (new_alo > ahi) return PROP_CONFLICT;
        if ((res = ctx_tighten_lb64(ctx, aid, new_alo)) != PROP_OK) return res;

        /* Find largest a <= ahi with a % b == r */
        rem = ahi % b_val;
        if (rem < 0) rem += b_val;
        int64_t new_ahi = ahi - ((rem - r_val + b_val) % b_val);
        if (new_ahi < new_alo) return PROP_CONFLICT;
        if ((res = ctx_tighten_ub64(ctx, aid, new_ahi)) != PROP_OK) return res;
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_mod_64(SolveCtx *c, uint32_t r, uint32_t a, uint32_t b, uint8_t p) {
    uint32_t ids[3]={r,a,b};
    return _alloc_prop(c, _fire_bounds_mod_64, p, 3, ids, sizeof(BoundsMod_64_t));
}

static PropResult _fire_unary_neg_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];

    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, rid, -var_hi64(ctx,&ctx->vars[aid]))) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, rid, -var_lo64(ctx,&ctx->vars[aid]))) != PROP_OK) return r;
    if ((r = ctx_tighten_lb64(ctx, aid, -var_hi64(ctx,&ctx->vars[rid]))) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, aid, -var_lo64(ctx,&ctx->vars[rid]))) != PROP_OK) return r;
    return PROP_OK;
}
uint32_t prop_add_unary_neg_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint8_t priority) {
    uint32_t ids[2] = { r_id, a_id };
    return _alloc_prop(ctx, _fire_unary_neg_64, priority, 2, ids, sizeof(UnaryNeg_64_t));
}


/* ------------------------------------------------------------------ */
/* ITEValue_64:  r = cond ? a : b                                     */
/*   var_ids[0]=r, var_ids[1]=cond, var_ids[2]=a, var_ids[3]=b       */
/* ------------------------------------------------------------------ */

static PropResult _fire_ite_value_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws   = PROP_WS(self);
    uint32_t       rid  = ws->var_ids[0];
    uint32_t       cid  = ws->var_ids[1];
    uint32_t       aid  = ws->var_ids[2];
    uint32_t       bid  = ws->var_ids[3];

    int64_t clo = var_lo64(ctx, &ctx->vars[cid]);
    int64_t chi = var_hi64(ctx, &ctx->vars[cid]);
    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
    int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);

    PropResult res;

    if (clo == 1 && chi == 1) {
        /* cond is true: r == a */
        int64_t lo = i64_max(rlo, alo);
        int64_t hi = i64_min(rhi, ahi);
        if (lo > hi) return PROP_CONFLICT;
        if ((res = ctx_tighten_lb64(ctx, rid, lo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, hi)) != PROP_OK) return res;
        if ((res = ctx_tighten_lb64(ctx, aid, lo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, aid, hi)) != PROP_OK) return res;
        /* Check entailment */
        rlo = var_lo64(ctx, &ctx->vars[rid]);
        rhi = var_hi64(ctx, &ctx->vars[rid]);
        alo = var_lo64(ctx, &ctx->vars[aid]);
        if (rlo == rhi && alo == rlo) return PROP_ENTAILED;
    } else if (clo == 0 && chi == 0) {
        /* cond is false: r == b */
        int64_t lo = i64_max(rlo, blo);
        int64_t hi = i64_min(rhi, bhi);
        if (lo > hi) return PROP_CONFLICT;
        if ((res = ctx_tighten_lb64(ctx, rid, lo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, hi)) != PROP_OK) return res;
        if ((res = ctx_tighten_lb64(ctx, bid, lo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, bid, hi)) != PROP_OK) return res;
        rlo = var_lo64(ctx, &ctx->vars[rid]);
        rhi = var_hi64(ctx, &ctx->vars[rid]);
        blo = var_lo64(ctx, &ctx->vars[bid]);
        if (rlo == rhi && blo == rlo) return PROP_ENTAILED;
    } else {
        /* cond is undecided: r covers the union of a's and b's ranges.
         * Also back-propagate the condition when r forces a branch:
         *   r disjoint from a's domain  -> cond must be 0 (else branch)
         *   r disjoint from b's domain  -> cond must be 1 (then branch)
         * Without this, an aux r whose domain is already pinned by an
         * outer constraint cannot drive the search away from values that
         * couldn't possibly come from the surviving branch. */
        int a_disjoint = (rhi < alo) || (rlo > ahi);
        int b_disjoint = (rhi < blo) || (rlo > bhi);
        if (a_disjoint && b_disjoint) return PROP_CONFLICT;
        if (a_disjoint) {
            /* must be else-branch -> cond = 0 */
            if ((res = ctx_tighten_ub64(ctx, cid, 0)) != PROP_OK) return res;
            int64_t lo = i64_max(rlo, blo);
            int64_t hi = i64_min(rhi, bhi);
            if (lo > hi) return PROP_CONFLICT;
            if ((res = ctx_tighten_lb64(ctx, rid, lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, hi)) != PROP_OK) return res;
            if ((res = ctx_tighten_lb64(ctx, bid, lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, bid, hi)) != PROP_OK) return res;
            return PROP_OK;
        }
        if (b_disjoint) {
            /* must be then-branch -> cond = 1 */
            if ((res = ctx_tighten_lb64(ctx, cid, 1)) != PROP_OK) return res;
            int64_t lo = i64_max(rlo, alo);
            int64_t hi = i64_min(rhi, ahi);
            if (lo > hi) return PROP_CONFLICT;
            if ((res = ctx_tighten_lb64(ctx, rid, lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, hi)) != PROP_OK) return res;
            if ((res = ctx_tighten_lb64(ctx, aid, lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, aid, hi)) != PROP_OK) return res;
            return PROP_OK;
        }
        /* Both branches still feasible: r is the interval hull of a, b. */
        int64_t lo = i64_min(alo, blo);
        int64_t hi = i64_max(ahi, bhi);
        if ((res = ctx_tighten_lb64(ctx, rid, lo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, hi)) != PROP_OK) return res;
    }

    return PROP_OK;
}

uint32_t prop_add_ite_value_64(SolveCtx *ctx, uint32_t r_id,
                                uint32_t cond_id, uint32_t a_id,
                                uint32_t b_id, uint8_t priority) {
    uint32_t ids[4] = { r_id, cond_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_ite_value_64, priority, 4, ids,
                       sizeof(ITEValue_64_t));
}

static PropResult _fire_in_set_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws    = PROP_WS(self);
    uint32_t       xid   = ws->var_ids[0];
    InSet_64_t    *iself = (InSet_64_t *)self;
    int64_t       *elems = (int64_t *)((char *)self + sizeof(InSet_64_t));
    uint32_t       n     = iself->n_elems;
    int64_t xlo = var_lo64(ctx,&ctx->vars[xid]), xhi = var_hi64(ctx,&ctx->vars[xid]);

    int64_t new_lo = INT64_MAX, new_hi = INT64_MIN;
    for (uint32_t i = 0; i < n; i++) {
        if (elems[i] >= xlo && elems[i] <= xhi) {
            if (elems[i] < new_lo) new_lo = elems[i];
            if (elems[i] > new_hi) new_hi = elems[i];
        }
    }
    if (new_lo == INT64_MAX) return PROP_CONFLICT;

    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, xid, new_lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, xid, new_hi)) != PROP_OK) return r;
    return PROP_OK;
}
uint32_t prop_add_in_set_64(SolveCtx *ctx, uint32_t x_id,
                              uint32_t n_elems, const int64_t *elems,
                              uint8_t priority) {
    uint32_t ids[1] = { x_id };
    uint32_t sz  = (uint32_t)(sizeof(InSet_64_t) + n_elems * sizeof(int64_t));
    uint32_t ref = _alloc_prop(ctx, _fire_in_set_64, priority, 1, ids, sz);
    if (ref == EXPR_NULL) return EXPR_NULL;
    InSet_64_t *p = (InSet_64_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->n_elems = n_elems;
    int64_t *dst = (int64_t *)((char *)p + sizeof(InSet_64_t));
    memcpy(dst, elems, n_elems * sizeof(int64_t));
    return ref;
}

uint32_t prop_add_reification_64(SolveCtx *ctx, uint32_t guard_id,
                                   uint32_t x_id, uint32_t y_id,
                                   uint8_t priority) {
    uint32_t ids[3] = { guard_id, x_id, y_id };
    return _alloc_prop(ctx, _fire_noop, priority, 3, ids, sizeof(Reification_64_t));
}

static PropResult _fire_bit_slice_64(Propagator *self, SolveCtx *ctx) {
    BitSlice_64_t *bself = (BitSlice_64_t *)self;
    PropWatchSect *ws    = PROP_WS(self);
    uint32_t       rid   = ws->var_ids[0];
    uint32_t       aid   = ws->var_ids[1];
    uint32_t width = (uint32_t)(bself->hi_bit - bself->lo_bit + 1);
    int64_t  max_v = (width < 64) ? (int64_t)((1ULL << width) - 1) : INT64_MAX;

    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, rid, 0))     != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, rid, max_v)) != PROP_OK) return r;

    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    if (alo == ahi) {
        uint64_t mask = (width >= 64) ? ~(uint64_t)0
                                     : (((uint64_t)1 << width) - 1);
        uint64_t v = ((uint64_t)alo >> bself->lo_bit) & mask;
        if ((r = ctx_tighten_lb64(ctx, rid, (int64_t)v)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub64(ctx, rid, (int64_t)v)) != PROP_OK) return r;
        return PROP_ENTAILED;
    }
    return PROP_OK;
}
uint32_t prop_add_bit_slice_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                uint8_t hi_bit, uint8_t lo_bit, uint8_t priority) {
    uint32_t ids[2] = { r_id, a_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bit_slice_64, priority, 2, ids, sizeof(BitSlice_64_t));
    if (ref == EXPR_NULL) return EXPR_NULL;
    BitSlice_64_t *p = (BitSlice_64_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->hi_bit = hi_bit; p->lo_bit = lo_bit;
    return ref;
}


/* ------------------------------------------------------------------ */
/* BoundsBAND_64:  r = a & b                                          */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_band_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Both singletons: exact result */
    if (alo == ahi && blo == bhi) {
        int64_t exact = alo & blo;
        if ((res = ctx_tighten_lb64(ctx, rid, exact)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, exact)) != PROP_OK) return res;
        return PROP_OK;
    }

    /* Upper bound: r <= min(a_hi, b_hi) since AND can only clear bits */
    int64_t r_hi_new = i64_min(ahi, bhi);
    if ((res = ctx_tighten_ub64(ctx, rid, r_hi_new)) != PROP_OK) return res;

    /* Lower bound: only tighten when the AND result is guaranteed.
     * AND is non-monotone so a_lo & b_lo is NOT a valid lower bound.
     * Example: a in [10,200], b=15 -> a&b ranges from 0 to 15.
     * We only set r_lo = 0 if it helps (non-negative operands). */
    if (alo >= 0 && blo >= 0) {
        if ((res = ctx_tighten_lb64(ctx, rid, 0)) != PROP_OK) return res;
    }

    /* Singleton a: r = a_val & b, so r <= a_val */
    if (alo == ahi) {
        if ((res = ctx_tighten_ub64(ctx, rid, alo)) != PROP_OK) return res;
    }
    /* Singleton b: r = a & b_val, so r <= b_val */
    if (blo == bhi) {
        if ((res = ctx_tighten_ub64(ctx, rid, blo)) != PROP_OK) return res;
    }

    /* Backward: when r and b are singletons, constrain a.
     * r = a & mask. Bits set in r must also be set in a.
     * Bits clear in mask cannot be set in a (irrelevant for bounds).
     * For bounds propagation with singleton r and b:
     * a must have all bits of r set: a_lo |= r_val (set required bits) */
    {
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        if (rlo == rhi && blo == bhi && blo > 0) {
            /* Bits in r must be set in a; bits outside mask are free.
             * a_lo: ensure bits of r are present.
             * We can also narrow a_hi: clear bits in a that are outside mask
             * and would push the result above r. */
            int64_t r_val = rlo;
            int64_t mask = blo;
            /* Forward-compute exact check: for each bit in mask that is NOT
             * in r, that bit in a could be 0 or 1 (won't affect r).
             * For each bit in mask that IS in r, that bit in a MUST be 1.
             * Backward tighten: set the mandatory bits in a_lo. */
            alo = var_lo64(ctx, &ctx->vars[aid]);
            int64_t new_alo = alo | r_val;  /* bits required by r */
            if (new_alo > alo) {
                if ((res = ctx_tighten_lb64(ctx, aid, new_alo)) != PROP_OK) return res;
            }
        }
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_band_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint32_t b_id,
                                  uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_band_64, priority, 3, ids,
                       sizeof(BoundsBAND_64_t));
}

/* ------------------------------------------------------------------ */
/* BoundsBOR_64:  r = a | b                                           */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_bor_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Both singletons: exact result */
    if (alo == ahi && blo == bhi) {
        int64_t exact = alo | blo;
        if ((res = ctx_tighten_lb64(ctx, rid, exact)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, exact)) != PROP_OK) return res;
        return PROP_OK;
    }

    /* Lower bound: r >= max(a_lo, b_lo) since a|b >= a and a|b >= b.
     * NOT a_lo | b_lo — that's unsound when low bits of the OR'd value
     * are not actually forced by the range constraints. Counter:
     * a in [2,_], b in [1,_]; a=2, b=2 gives r=2 < (2|1)=3. */
    if (alo >= 0 && blo >= 0) {
        int64_t r_lb = (alo > blo) ? alo : blo;
        if ((res = ctx_tighten_lb64(ctx, rid, r_lb)) != PROP_OK) return res;
    }

    /* Singleton: r >= singleton_val */
    if (alo == ahi && alo >= 0) {
        if ((res = ctx_tighten_lb64(ctx, rid, alo)) != PROP_OK) return res;
    }
    if (blo == bhi && blo >= 0) {
        if ((res = ctx_tighten_lb64(ctx, rid, blo)) != PROP_OK) return res;
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_bor_64(SolveCtx *ctx, uint32_t r_id,
                                 uint32_t a_id, uint32_t b_id,
                                 uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_bor_64, priority, 3, ids,
                       sizeof(BoundsBOR_64_t));
}

/* ------------------------------------------------------------------ */
/* BoundsBXOR_64:  r = a ^ b                                         */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_bxor_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Both singletons: exact result */
    if (alo == ahi && blo == bhi) {
        int64_t exact = alo ^ blo;
        if ((res = ctx_tighten_lb64(ctx, rid, exact)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, exact)) != PROP_OK) return res;
        return PROP_OK;
    }

    /* One singleton: XOR is a bijection, but bounds approximation only
     * works correctly when the other operand is also singleton.
     * For non-singleton cases, XOR can scramble bit ordering.
     * Only do backward propagation when both endpoints XOR to valid bounds. */
    if (alo == ahi && blo == bhi) {
        /* Already handled above */
    } else if (alo == ahi) {
        int64_t k = alo;
        /* Only safe if b is singleton (already handled) or for backward
         * propagation when r is singleton */
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        if (rlo == rhi) {
            /* r is singleton: b = r ^ k */
            int64_t b_exact = rlo ^ k;
            if ((res = ctx_tighten_lb64(ctx, bid, b_exact)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, bid, b_exact)) != PROP_OK) return res;
        }
    } else if (blo == bhi) {
        int64_t k = blo;
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        if (rlo == rhi) {
            /* r is singleton: a = r ^ k */
            int64_t a_exact = rlo ^ k;
            if ((res = ctx_tighten_lb64(ctx, aid, a_exact)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, aid, a_exact)) != PROP_OK) return res;
        }
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_bxor_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint32_t b_id,
                                  uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_bxor_64, priority, 3, ids,
                       sizeof(BoundsBXOR_64_t));
}

/* ------------------------------------------------------------------ */
/* BoundsBNOT_64:  r = ~a                                            */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_bnot_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];

    /* Bit-width mask: bvnot on an n-bit value flips n bits, so the
     * complement must be re-confined to [0, 2^n - 1]. C's `~` flips
     * all 64 bits and produces a negative int64 for any small
     * unsigned value (e.g. ~5 = -6, not 250 for an 8-bit value).
     * Use min(r.width, a.width) since they should match for bvnot. */
    Variable *rv = &ctx->vars[rid];
    Variable *av = &ctx->vars[aid];
    uint16_t  w  = rv->width ? rv->width : av->width;
    uint64_t  mask = (w >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1);

    int64_t alo = var_lo64(ctx, av);
    int64_t ahi = var_hi64(ctx, av);

    PropResult res;

    /* ~a within w bits reverses ordering: r_lo = mask & ~a_hi,
     * r_hi = mask & ~a_lo. */
    int64_t new_rlo = (int64_t)(mask & (uint64_t)~ahi);
    int64_t new_rhi = (int64_t)(mask & (uint64_t)~alo);
    if ((res = ctx_tighten_lb64(ctx, rid, new_rlo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub64(ctx, rid, new_rhi)) != PROP_OK) return res;

    /* Backward: a_lo = mask & ~r_hi, a_hi = mask & ~r_lo. */
    int64_t rlo = var_lo64(ctx, rv);
    int64_t rhi = var_hi64(ctx, rv);
    int64_t new_alo = (int64_t)(mask & (uint64_t)~rhi);
    int64_t new_ahi = (int64_t)(mask & (uint64_t)~rlo);
    if ((res = ctx_tighten_lb64(ctx, aid, new_alo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub64(ctx, aid, new_ahi)) != PROP_OK) return res;

    return PROP_OK;
}

uint32_t prop_add_bounds_bnot_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint8_t priority) {
    uint32_t ids[2] = { r_id, a_id };
    return _alloc_prop(ctx, _fire_bounds_bnot_64, priority, 2, ids,
                       sizeof(BoundsBNOT_64_t));
}


/* ------------------------------------------------------------------ */
/* BoundsSHL_64:  r = a << b                                          */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_shl_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Clamp shift amount to [0, 63] */
    if (blo < 0) blo = 0;
    if (bhi > 63) bhi = 63;

    /* Both singletons: exact result */
    if (alo == ahi && blo == bhi) {
        int64_t exact = alo << blo;
        if ((res = ctx_tighten_lb64(ctx, rid, exact)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, exact)) != PROP_OK) return res;
        return PROP_OK;
    }

    /* Singleton shift amount */
    if (blo == bhi) {
        int64_t s = blo;
        if (alo >= 0) {
            if ((res = ctx_tighten_lb64(ctx, rid, alo << s)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, ahi << s)) != PROP_OK) return res;
        }
        /* Backward: a = r >> s */
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        if (s > 0 && rlo >= 0) {
            if ((res = ctx_tighten_lb64(ctx, aid, rlo >> s)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, aid, rhi >> s)) != PROP_OK) return res;
        }
    } else if (alo >= 0) {
        /* Variable shift: conservative bounds */
        if ((res = ctx_tighten_lb64(ctx, rid, alo << blo)) != PROP_OK) return res;
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_shl_64(SolveCtx *ctx, uint32_t r_id,
                                 uint32_t a_id, uint32_t b_id,
                                 uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_shl_64, priority, 3, ids,
                       sizeof(BoundsSHL_64_t));
}

/* ------------------------------------------------------------------ */
/* BoundsLSHR_64:  r = a >> b  (logical right shift)                  */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_lshr_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];

    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    if (blo < 0) blo = 0;
    if (bhi > 63) bhi = 63;

    /* Both singletons: exact result */
    if (alo == ahi && blo == bhi) {
        int64_t exact = (alo >= 0) ? (alo >> blo) : (int64_t)((uint64_t)alo >> blo);
        if ((res = ctx_tighten_lb64(ctx, rid, exact)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, exact)) != PROP_OK) return res;
        return PROP_OK;
    }

    /* Singleton shift amount */
    if (blo == bhi && alo >= 0) {
        int64_t s = blo;
        if ((res = ctx_tighten_lb64(ctx, rid, alo >> s)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, ahi >> s)) != PROP_OK) return res;
        /* Backward */
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        if ((res = ctx_tighten_lb64(ctx, aid, rlo << s)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, aid, ((rhi + 1) << s) - 1)) != PROP_OK) return res;
    } else if (alo >= 0 && bhi > 0) {
        /* Variable shift: r_lo = a_lo >> b_hi, r_hi = a_hi >> b_lo */
        if ((res = ctx_tighten_lb64(ctx, rid, alo >> bhi)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, ahi >> blo)) != PROP_OK) return res;
    }

    return PROP_OK;
}

uint32_t prop_add_bounds_lshr_64(SolveCtx *ctx, uint32_t r_id,
                                  uint32_t a_id, uint32_t b_id,
                                  uint8_t priority) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    return _alloc_prop(ctx, _fire_bounds_lshr_64, priority, 3, ids,
                       sizeof(BoundsLSHR_64_t));
}


/* ------------------------------------------------------------------ */
/* BoundsConcat_64:  r = {hi, lo}                                     */
/*   var_ids[0]=r, var_ids[1]=hi, var_ids[2]=lo                      */
/*   lo_width = bit width of lo operand                               */
/* ------------------------------------------------------------------ */

static PropResult _fire_bounds_concat_64(Propagator *self, SolveCtx *ctx) {
    BoundsConcat_64_t *cself = (BoundsConcat_64_t *)self;
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       hid = ws->var_ids[1];
    uint32_t       lid = ws->var_ids[2];

    uint8_t lo_w = cself->lo_width;
    int64_t lo_mask = (lo_w < 64) ? ((int64_t)1 << lo_w) - 1 : -1;

    int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
    int64_t hlo = var_lo64(ctx, &ctx->vars[hid]);
    int64_t hhi = var_hi64(ctx, &ctx->vars[hid]);
    int64_t llo = var_lo64(ctx, &ctx->vars[lid]);
    int64_t lhi = var_hi64(ctx, &ctx->vars[lid]);

    PropResult res;

    /* Forward: r = (hi << lo_w) | lo (unsigned concat) */
    if (hlo >= 0 && llo >= 0 && lo_w < 64) {
        int64_t fwd_lo = (hlo << lo_w) | llo;
        int64_t fwd_hi = (hhi << lo_w) | lhi;
        if ((res = ctx_tighten_lb64(ctx, rid, fwd_lo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, fwd_hi)) != PROP_OK) return res;
    }

    /* Re-read r after possible tightening */
    rlo = var_lo64(ctx, &ctx->vars[rid]);
    rhi = var_hi64(ctx, &ctx->vars[rid]);

    /* Backward: hi = r >> lo_w */
    if (rlo >= 0 && lo_w < 64) {
        if ((res = ctx_tighten_lb64(ctx, hid, rlo >> lo_w)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, hid, rhi >> lo_w)) != PROP_OK) return res;
    }

    /* Backward: lo = r & lo_mask (only precise when hi is singleton) */
    hlo = var_lo64(ctx, &ctx->vars[hid]);
    hhi = var_hi64(ctx, &ctx->vars[hid]);
    if (hlo == hhi && rlo >= 0 && lo_w < 64) {
        int64_t base = hlo << lo_w;
        int64_t lo_from_rlo = rlo - base;
        int64_t lo_from_rhi = rhi - base;
        if (lo_from_rlo >= 0 && lo_from_rhi >= 0) {
            if ((res = ctx_tighten_lb64(ctx, lid, lo_from_rlo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, lid, lo_from_rhi)) != PROP_OK) return res;
        }
    }
    /* Constrain lo to valid range */
    if (lo_w < 64) {
        if ((res = ctx_tighten_lb64(ctx, lid, 0)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, lid, lo_mask)) != PROP_OK) return res;
    }

    /* Entailment: all three singletons */
    rlo = var_lo64(ctx, &ctx->vars[rid]);
    rhi = var_hi64(ctx, &ctx->vars[rid]);
    hlo = var_lo64(ctx, &ctx->vars[hid]);
    hhi = var_hi64(ctx, &ctx->vars[hid]);
    llo = var_lo64(ctx, &ctx->vars[lid]);
    lhi = var_hi64(ctx, &ctx->vars[lid]);
    if (rlo == rhi && hlo == hhi && llo == lhi) return PROP_ENTAILED;

    return PROP_OK;
}

uint32_t prop_add_bounds_concat_64(SolveCtx *ctx, uint32_t r_id,
                                    uint32_t hi_id, uint32_t lo_id,
                                    uint8_t lo_width, uint8_t priority) {
    uint32_t ids[3] = { r_id, hi_id, lo_id };
    uint32_t ref = _alloc_prop(ctx, _fire_bounds_concat_64, priority, 3, ids,
                                sizeof(BoundsConcat_64_t));
    if (ref == EXPR_NULL) return EXPR_NULL;

    BoundsConcat_64_t *p = (BoundsConcat_64_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->lo_width = lo_width;
    return ref;
}

/* ------------------------------------------------------------------ */
/* DisjClause: (v0 op0 c0) OR ... OR (vN opN cN)                     */
/*                                                                     */
/* Fire logic: check each clause against current bounds.              */
/* "definitely false" means every value in the domain violates it.    */
/* When all but one are definitely false, enforce the survivor.       */
/* ------------------------------------------------------------------ */

/** Is comparison (lo..hi) op constant definitely false? */
static int _clause_definitely_false(Variable *v, SolveCtx *ctx,
                                     uint32_t op, int64_t c) {
    int64_t lo = var_lo64(ctx, v);
    int64_t hi = var_hi64(ctx, v);
    /* Negate the op and check if negation is definitely true */
    switch (op) {
    case BIN_EQ:   return (lo > c || hi < c);        /* !(lo <= c <= hi) */
    case BIN_NEQ:  return (lo == hi && lo == c);      /* singleton == c */
    case BIN_LT:   return (lo >= c);                  /* all >= c => none < c */
    case BIN_LTE:  return (lo > c);
    case BIN_GT:   return (hi <= c);
    case BIN_GTE:  return (hi < c);
    default:       return 0;
    }
}

/** Enforce clause: tighten var's domain so (var op constant) can hold. */
static PropResult _enforce_clause(SolveCtx *ctx, uint32_t var_id,
                                   uint32_t op, int64_t c) {
    switch (op) {
    case BIN_EQ:
        if (ctx_tighten_lb64(ctx, var_id, c) == PROP_CONFLICT) return PROP_CONFLICT;
        if (ctx_tighten_ub64(ctx, var_id, c) == PROP_CONFLICT) return PROP_CONFLICT;
        return PROP_OK;
    case BIN_NEQ:
        /* Can only tighten if domain is singleton or c is at a bound */
        {
            int64_t lo = var_lo64(ctx, &ctx->vars[var_id]);
            int64_t hi = var_hi64(ctx, &ctx->vars[var_id]);
            if (lo == c) return ctx_tighten_lb64(ctx, var_id, c + 1);
            if (hi == c) return ctx_tighten_ub64(ctx, var_id, c - 1);
        }
        return PROP_OK;
    case BIN_LT:   return ctx_tighten_ub64(ctx, var_id, c - 1);
    case BIN_LTE:  return ctx_tighten_ub64(ctx, var_id, c);
    case BIN_GT:   return ctx_tighten_lb64(ctx, var_id, c + 1);
    case BIN_GTE:  return ctx_tighten_lb64(ctx, var_id, c);
    default:       return PROP_OK;
    }
}

static PropResult _fire_disj_clause(Propagator *self, SolveCtx *ctx) {
    DisjClause_t *dc = (DisjClause_t *)self;
    uint32_t n = dc->n_clauses;

    /* Count how many clauses are definitely false */
    uint32_t n_false = 0;
    uint32_t survivor = 0;  /* index of the last non-false clause */
    for (uint32_t i = 0; i < n; i++) {
        if (dc->clauses[i].rhs_var_id == UINT32_MAX) {
            Variable *v = &ctx->vars[dc->clauses[i].var_id];
            if (_clause_definitely_false(v, ctx, dc->clauses[i].op,
                                         dc->clauses[i].constant)) {
                n_false++;
            } else {
                survivor = i;
            }
        } else {
            /* var-var clause: check if definitely false */
            Variable *lv = &ctx->vars[dc->clauses[i].var_id];
            Variable *rv = &ctx->vars[dc->clauses[i].rhs_var_id];
            int64_t l_lo = var_lo64(ctx, lv), l_hi = var_hi64(ctx, lv);
            int64_t r_lo = var_lo64(ctx, rv), r_hi = var_hi64(ctx, rv);
            int def_false = 0;
            switch (dc->clauses[i].op) {
            case BIN_LT:  def_false = (l_lo >= r_hi); break;
            case BIN_LTE: def_false = (l_lo > r_hi);  break;
            case BIN_GT:  def_false = (l_hi <= r_lo); break;
            case BIN_GTE: def_false = (l_hi < r_lo);  break;
            case BIN_EQ:  def_false = (l_lo > r_hi || l_hi < r_lo); break;
            case BIN_NEQ: def_false = (l_lo == l_hi && r_lo == r_hi && l_lo == r_lo); break;
            default: break;
            }
            if (def_false) n_false++;
            else survivor = i;
        }
    }

    if (n_false == n) return PROP_CONFLICT;  /* all false */
    if (n_false < n - 1) return PROP_OK;     /* 2+ undecided */

    /* Exactly one survivor — enforce it */
    if (dc->clauses[survivor].rhs_var_id == UINT32_MAX) {
        return _enforce_clause(ctx, dc->clauses[survivor].var_id,
                               dc->clauses[survivor].op,
                               dc->clauses[survivor].constant);
    } else {
        /* var-var survivor: tighten both sides */
        uint32_t lv = dc->clauses[survivor].var_id;
        uint32_t rv = dc->clauses[survivor].rhs_var_id;
        int64_t l_lo = var_lo64(ctx, &ctx->vars[lv]);
        int64_t r_hi = var_hi64(ctx, &ctx->vars[rv]);
        int64_t r_lo = var_lo64(ctx, &ctx->vars[rv]);
        int64_t l_hi = var_hi64(ctx, &ctx->vars[lv]);
        switch (dc->clauses[survivor].op) {
        case BIN_GTE:
            if (ctx_tighten_lb64(ctx, lv, r_lo) == PROP_CONFLICT) return PROP_CONFLICT;
            if (ctx_tighten_ub64(ctx, rv, l_hi) == PROP_CONFLICT) return PROP_CONFLICT;
            return PROP_OK;
        case BIN_LTE:
            if (ctx_tighten_ub64(ctx, lv, r_hi) == PROP_CONFLICT) return PROP_CONFLICT;
            if (ctx_tighten_lb64(ctx, rv, l_lo) == PROP_CONFLICT) return PROP_CONFLICT;
            return PROP_OK;
        case BIN_GT:
            if (ctx_tighten_lb64(ctx, lv, r_lo + 1) == PROP_CONFLICT) return PROP_CONFLICT;
            if (ctx_tighten_ub64(ctx, rv, l_hi - 1) == PROP_CONFLICT) return PROP_CONFLICT;
            return PROP_OK;
        case BIN_LT:
            if (ctx_tighten_ub64(ctx, lv, r_hi - 1) == PROP_CONFLICT) return PROP_CONFLICT;
            if (ctx_tighten_lb64(ctx, rv, l_lo + 1) == PROP_CONFLICT) return PROP_CONFLICT;
            return PROP_OK;
        case BIN_EQ:
            if (ctx_tighten_lb64(ctx, lv, r_lo) == PROP_CONFLICT) return PROP_CONFLICT;
            if (ctx_tighten_ub64(ctx, lv, r_hi) == PROP_CONFLICT) return PROP_CONFLICT;
            if (ctx_tighten_lb64(ctx, rv, l_lo) == PROP_CONFLICT) return PROP_CONFLICT;
            if (ctx_tighten_ub64(ctx, rv, l_hi) == PROP_CONFLICT) return PROP_CONFLICT;
            return PROP_OK;
        default:
            return PROP_OK;
        }
    }
}

uint32_t prop_add_disj_clause(SolveCtx *ctx,
                               uint32_t n_clauses,
                               const uint32_t *var_ids,
                               const uint32_t *ops,
                               const int64_t *constants,
                               uint8_t priority,
                               const uint32_t *rhs_var_ids) {
    if (n_clauses == 0 || n_clauses > MAX_DISJ_CLAUSES) return EXPR_NULL;

    /* Collect all watched var IDs (lhs + rhs vars) */
    uint32_t all_vids[MAX_DISJ_CLAUSES * 2];
    uint32_t n_watch = 0;
    for (uint32_t i = 0; i < n_clauses; i++) {
        all_vids[n_watch++] = var_ids[i];
        if (rhs_var_ids && rhs_var_ids[i] != UINT32_MAX) {
            all_vids[n_watch++] = rhs_var_ids[i];
        }
    }
    uint32_t ref = _alloc_prop(ctx, _fire_disj_clause, priority,
                                n_watch, all_vids, sizeof(DisjClause_t));
    if (ref == EXPR_NULL) return EXPR_NULL;

    DisjClause_t *dc = (DisjClause_t *)zsp_pool_ptr(&ctx->pool, ref);
    dc->n_clauses = n_clauses;
    for (uint32_t i = 0; i < n_clauses; i++) {
        dc->clauses[i].var_id   = var_ids[i];
        dc->clauses[i].op       = ops[i];
        dc->clauses[i].constant = constants[i];
        dc->clauses[i].rhs_var_id = (rhs_var_ids ? rhs_var_ids[i] : UINT32_MAX);
    }
    return ref;
}

/* ------------------------------------------------------------------ */
/* AllDifferent_32:  x0, x1, ..., xN are all distinct                 */
/*                                                                     */
/* Propagation strategy (in order):                                   */
/*   1. Pigeonhole check: count distinct reachable values; if fewer   */
/*      than n_vars, return PROP_CONFLICT.                            */
/*   2. Singleton exclusion: for each variable that is assigned        */
/*      (lo == hi), tighten bounds of all other variables to exclude  */
/*      that value.  This is the most common propagation in practice. */
/*   3. Hall interval detection: sort by lo, sweep to find intervals  */
/*      where count of contained domains exceeds interval width.      */
/*   4. Entailment: all variables assigned and distinct.              */
/* ------------------------------------------------------------------ */

static PropResult _fire_all_different_32(Propagator *self, SolveCtx *ctx) {
    AllDifferent_t *ad = (AllDifferent_t *)self;
    uint32_t n = ad->n_vars;

    /* --- Singleton exclusion (most common propagation) --- */
    for (uint32_t i = 0; i < n; i++) {
        Variable *vi = &ctx->vars[ad->var_ids[i]];
        if (vi->lo != vi->hi) continue;
        int32_t val = vi->lo;
        for (uint32_t j = 0; j < n; j++) {
            if (j == i) continue;
            uint32_t vj_id = ad->var_ids[j];
            Variable *vj = &ctx->vars[vj_id];
            if (vj->lo == val) {
                PropResult r = ctx_tighten_lb32(ctx, vj_id, val + 1);
                if (r == PROP_CONFLICT) return PROP_CONFLICT;
            }
            if (vj->hi == val) {
                PropResult r = ctx_tighten_ub32(ctx, vj_id, val - 1);
                if (r == PROP_CONFLICT) return PROP_CONFLICT;
            }
        }
    }

    /* --- Pigeonhole check --- */
    /* Find global lo/hi across all variables */
    int32_t glo = ctx->vars[ad->var_ids[0]].lo;
    int32_t ghi = ctx->vars[ad->var_ids[0]].hi;
    for (uint32_t i = 1; i < n; i++) {
        Variable *vi = &ctx->vars[ad->var_ids[i]];
        if (vi->lo < glo) glo = vi->lo;
        if (vi->hi > ghi) ghi = vi->hi;
    }
    /* Domain union cardinality upper bound */
    int64_t union_size = (int64_t)ghi - (int64_t)glo + 1;
    if (union_size < (int64_t)n) return PROP_CONFLICT;

    /* --- Hall interval detection via sweep --- */
    /* Sort variables by lo bound (insertion sort, n <= 16) */
    uint32_t sorted[MAX_ALLDIFF_VARS];
    for (uint32_t i = 0; i < n; i++) sorted[i] = i;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = sorted[i];
        int32_t key_lo = ctx->vars[ad->var_ids[key]].lo;
        uint32_t j = i;
        while (j > 0 && ctx->vars[ad->var_ids[sorted[j - 1]]].lo > key_lo) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    /* Sweep: for each starting position, count how many domains are
       fully contained in [start_lo, end_hi].  If count > width, tighten. */
    for (uint32_t start = 0; start < n; start++) {
        int32_t lo_s = ctx->vars[ad->var_ids[sorted[start]]].lo;
        int32_t max_hi = lo_s;
        uint32_t count = 0;
        for (uint32_t end = start; end < n; end++) {
            uint32_t idx = ad->var_ids[sorted[end]];
            Variable *ve = &ctx->vars[idx];
            if (ve->lo >= lo_s) {
                if (ve->hi > max_hi) max_hi = ve->hi;
                count++;
            }
            int64_t width = (int64_t)max_hi - (int64_t)lo_s + 1;
            if ((int64_t)count > width) return PROP_CONFLICT;
            /* If count == width (Hall interval), variables outside must
               avoid [lo_s, max_hi] */
            if ((int64_t)count == width && count >= 2) {
                for (uint32_t k = 0; k < n; k++) {
                    uint32_t kid = ad->var_ids[k];
                    Variable *vk = &ctx->vars[kid];
                    /* Skip variables that are part of this Hall interval */
                    int in_hall = (vk->lo >= lo_s && vk->hi <= max_hi);
                    if (in_hall) continue;
                    /* If vk overlaps the Hall interval, tighten */
                    if (vk->lo >= lo_s && vk->lo <= max_hi) {
                        PropResult r = ctx_tighten_lb32(ctx, kid, max_hi + 1);
                        if (r == PROP_CONFLICT) return PROP_CONFLICT;
                    }
                    if (vk->hi >= lo_s && vk->hi <= max_hi) {
                        PropResult r = ctx_tighten_ub32(ctx, kid, lo_s - 1);
                        if (r == PROP_CONFLICT) return PROP_CONFLICT;
                    }
                }
            }
        }
    }

    /* --- Entailment check --- */
    int all_assigned = 1;
    for (uint32_t i = 0; i < n; i++) {
        Variable *vi = &ctx->vars[ad->var_ids[i]];
        if (vi->lo != vi->hi) { all_assigned = 0; break; }
    }
    if (all_assigned) {
        /* Verify all distinct (should be guaranteed by propagation) */
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                if (ctx->vars[ad->var_ids[i]].lo ==
                    ctx->vars[ad->var_ids[j]].lo)
                    return PROP_CONFLICT;
            }
        }
        return PROP_ENTAILED;
    }

    return PROP_OK;
}

uint32_t prop_add_all_different(SolveCtx *ctx, uint32_t n_vars,
                                 const uint32_t *var_ids, uint8_t priority) {
    if (n_vars < 2 || n_vars > MAX_ALLDIFF_VARS) return EXPR_NULL;

    /* Reject variables wider than 32 bits */
    for (uint32_t i = 0; i < n_vars; i++) {
        if (ctx->vars[var_ids[i]].width > 32) return EXPR_NULL;
    }

    uint32_t ref = zsp_pool_alloc(&ctx->pool, (uint32_t)sizeof(AllDifferent_t), 8u);
    if (ref == EXPR_NULL) return EXPR_NULL;

    AllDifferent_t *ad = (AllDifferent_t *)zsp_pool_ptr(&ctx->pool, ref);
    memset(ad, 0, sizeof(AllDifferent_t));

    ad->hdr.fire       = _fire_all_different_32;
    ad->hdr.queue_next = EXPR_NULL;
    ad->hdr.prop_id    = (uint16_t)ctx->n_props++;
    ad->hdr.priority   = priority;
    ad->hdr.flags      = PROP_FLAG_WIDE_WATCH;
    ad->n_vars         = n_vars;
    ad->_capacity      = MAX_ALLDIFF_VARS;

    for (uint32_t i = 0; i < n_vars; i++) {
        ad->var_ids[i] = var_ids[i];
    }

    /* Register watchers: insert this propagator into each variable's
       watcher chain.  Store the previous head as watcher_nexts[i]. */
    for (uint32_t i = 0; i < n_vars; i++) {
        uint32_t vid = var_ids[i];
        ad->watcher_nexts[i]    = ctx->watcher_heads[vid];
        ctx->watcher_heads[vid] = ref;
    }

    prop_enqueue(ctx, ref);

    /* Record prop ref for checkpoint/restore */
    if (ctx->prop_refs && ad->hdr.prop_id < ctx->n_prop_refs_capacity)
        ctx->prop_refs[ad->hdr.prop_id] = ref;

    return ref;
}

/* ------------------------------------------------------------------ */
/* SumEq_32: result == summand[0] + summand[1] + ... + summand[N-1]   */
/* ------------------------------------------------------------------ */

static PropResult _fire_sum_eq_32(Propagator *self, SolveCtx *ctx) {
    SumEq_32_t *s = (SumEq_32_t *)self;
    uint32_t n = s->n_vars;  /* total watches: [0]=result, [1..n-1]=summands */
    uint32_t rid = s->var_ids[0];

    /* Compute sum of lower bounds and sum of upper bounds.
     * Use var_lo64/var_hi64 to correctly handle tier-0 (32-bit signed/unsigned)
     * and tier-1 (32-bit unsigned promoted to 64-bit) variable storage. */
    int64_t sum_lo = 0, sum_hi = 0;
    for (uint32_t i = 1; i < n; i++) {
        sum_lo += var_lo64(ctx, &ctx->vars[s->var_ids[i]]);
        sum_hi += var_hi64(ctx, &ctx->vars[s->var_ids[i]]);
    }

    /* Forward: tighten result bounds */
    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, rid, sum_lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, rid, sum_hi)) != PROP_OK) return r;

    /* Backward: for each summand i, tighten using
     *   xi_lo >= result_lo - sum_of_others_hi
     *   xi_hi <= result_hi - sum_of_others_lo */
    int64_t r_lo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t r_hi = var_hi64(ctx, &ctx->vars[rid]);

    for (uint32_t i = 1; i < n; i++) {
        /* sum of all OTHER summands' bounds */
        int64_t vi_lo = var_lo64(ctx, &ctx->vars[s->var_ids[i]]);
        int64_t vi_hi = var_hi64(ctx, &ctx->vars[s->var_ids[i]]);
        int64_t others_lo = sum_lo - vi_lo;
        int64_t others_hi = sum_hi - vi_hi;

        int64_t new_lo = r_lo - others_hi;
        int64_t new_hi = r_hi - others_lo;

        if ((r = ctx_tighten_lb64(ctx, s->var_ids[i], new_lo)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub64(ctx, s->var_ids[i], new_hi)) != PROP_OK) return r;
    }

    return PROP_OK;
}

uint32_t prop_add_sum_eq_32(SolveCtx *ctx, uint32_t result_id,
                             uint32_t n_summands, const uint32_t *summand_ids,
                             uint8_t priority) {
    if (n_summands < 1 || n_summands > MAX_SUM_VARS) return EXPR_NULL;

    uint32_t n_total = 1 + n_summands;  /* result + summands */

    uint32_t ref = zsp_pool_alloc(&ctx->pool, (uint32_t)sizeof(SumEq_32_t), 8u);
    if (ref == EXPR_NULL) return EXPR_NULL;

    SumEq_32_t *s = (SumEq_32_t *)zsp_pool_ptr(&ctx->pool, ref);
    memset(s, 0, sizeof(SumEq_32_t));

    s->hdr.fire       = _fire_sum_eq_32;
    s->hdr.queue_next = EXPR_NULL;
    s->hdr.prop_id    = (uint16_t)ctx->n_props++;
    s->hdr.priority   = priority;
    s->hdr.flags      = PROP_FLAG_WIDE_WATCH;
    s->n_vars         = n_total;
    s->_capacity      = MAX_SUM_VARS + 1;

    s->var_ids[0] = result_id;
    for (uint32_t i = 0; i < n_summands; i++) {
        s->var_ids[1 + i] = summand_ids[i];
    }

    /* Register watchers */
    for (uint32_t i = 0; i < n_total; i++) {
        uint32_t vid = s->var_ids[i];
        s->watcher_nexts[i]     = ctx->watcher_heads[vid];
        ctx->watcher_heads[vid] = ref;
    }

    prop_enqueue(ctx, ref);

    if (ctx->prop_refs && s->hdr.prop_id < ctx->n_prop_refs_capacity)
        ctx->prop_refs[s->hdr.prop_id] = ref;

    return ref;
}

/* ------------------------------------------------------------------ */
/* Countones_32: result == popcount(operand)                           */
/* ------------------------------------------------------------------ */

static int _popcount32(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (int)(((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u >> 24);
}

static PropResult _fire_countones_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0];  /* result */
    uint32_t xid = ws->var_ids[1];  /* operand */
    Variable *rv = &ctx->vars[rid];
    Variable *xv = &ctx->vars[xid];

    uint16_t width = xv->width;
    if (width > 32) width = 32;

    PropResult r;

    /* Forward: bound result from operand's domain */
    if (xv->lo == xv->hi) {
        /* Operand is singleton: result is exact popcount */
        int pc = _popcount32((uint32_t)xv->lo & ((width < 32) ? ((1u << width) - 1) : 0xFFFFFFFFu));
        if ((r = ctx_tighten_lb32(ctx, rid, pc)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub32(ctx, rid, pc)) != PROP_OK) return r;
    } else {
        /* Coarse bounds: min popcount >= popcount(bits that must be 1),
         * max popcount <= width - count of bits that must be 0 */
        uint32_t mask = (width < 32) ? ((1u << width) - 1) : 0xFFFFFFFFu;
        uint32_t must_1 = (uint32_t)xv->lo & (uint32_t)xv->hi & mask;  /* approximate */
        int min_pc = _popcount32(must_1);
        if ((r = ctx_tighten_lb32(ctx, rid, min_pc)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub32(ctx, rid, (int32_t)width)) != PROP_OK) return r;
    }

    /* Backward: bound operand from result */
    if (rv->lo == rv->hi) {
        int32_t k = rv->lo;
        if (k < 0 || k > (int32_t)width) return PROP_CONFLICT;
        if (k == 0) {
            if ((r = ctx_tighten_lb32(ctx, xid, 0)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub32(ctx, xid, 0)) != PROP_OK) return r;
        } else if (k == (int32_t)width) {
            uint32_t mask = (width < 32) ? ((1u << width) - 1) : 0xFFFFFFFFu;
            if ((r = ctx_tighten_lb32(ctx, xid, (int32_t)mask)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub32(ctx, xid, (int32_t)mask)) != PROP_OK) return r;
        } else if (k > 0 && k <= (int32_t)width) {
            /* min x with popcount k: lowest k bits set = (1<<k)-1 */
            uint32_t min_x = (uint32_t)((1u << k) - 1);
            /* max x with popcount k: highest k bits set (within width) */
            uint32_t max_x = (width < 32)
                ? (uint32_t)(((1u << k) - 1) << (width - (uint16_t)k))
                : (uint32_t)(((1u << k) - 1) << (32 - k));
            if ((r = ctx_tighten_lb32(ctx, xid, (int32_t)min_x)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub32(ctx, xid, (int32_t)max_x)) != PROP_OK) return r;
        }
    } else {
        /* result_lo > 0 means operand cannot be 0 */
        if (rv->lo > 0) {
            if ((r = ctx_tighten_lb32(ctx, xid, 1)) != PROP_OK) return r;
        }
        /* result_hi < width means not all bits can be set */
        if (rv->hi < (int32_t)width && rv->hi >= 0) {
            /* max x with at most rv->hi bits set */
            uint32_t max_bits = (uint32_t)rv->hi;
            uint32_t max_x = (width < 32)
                ? (uint32_t)(((1u << max_bits) - 1) << (width - (uint16_t)max_bits))
                : (uint32_t)(((1u << max_bits) - 1) << (32 - max_bits));
            if ((r = ctx_tighten_ub32(ctx, xid, (int32_t)max_x)) != PROP_OK) return r;
        }
    }

    return PROP_OK;
}

uint32_t prop_add_countones_32(SolveCtx *ctx, uint32_t result_id,
                                uint32_t operand_id, uint8_t priority) {
    uint32_t ids[2] = { result_id, operand_id };
    return _alloc_prop(ctx, _fire_countones_32, priority, 2, ids,
                       sizeof(Countones_32_t));
}

/* ------------------------------------------------------------------ */
/* Clog2_32: result == ceil(log2(operand))                             */
/* ------------------------------------------------------------------ */

static int32_t _clog2_32(uint32_t v) {
    if (v <= 1) return 0;
    int32_t r = 0;
    uint32_t t = v - 1;
    while (t) { r++; t >>= 1; }
    return r;
}

static PropResult _fire_clog2_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0];  /* result */
    uint32_t xid = ws->var_ids[1];  /* operand */
    Variable *rv = &ctx->vars[rid];
    Variable *xv = &ctx->vars[xid];

    PropResult r;

    /* Guard: operand must be > 0 for clog2 to be defined */
    if ((r = ctx_tighten_lb32(ctx, xid, 1)) != PROP_OK) return r;

    /* Forward: clog2 is monotonically non-decreasing */
    int32_t clog_lo = _clog2_32((uint32_t)xv->lo);
    int32_t clog_hi = _clog2_32((uint32_t)xv->hi);
    if ((r = ctx_tighten_lb32(ctx, rid, clog_lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub32(ctx, rid, clog_hi)) != PROP_OK) return r;

    /* Backward: if result is singleton k, tighten operand range */
    if (rv->lo == rv->hi) {
        int32_t k = rv->lo;
        if (k == 0) {
            /* clog2(x) == 0 -> x == 1 */
            if ((r = ctx_tighten_lb32(ctx, xid, 1)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub32(ctx, xid, 1)) != PROP_OK) return r;
        } else if (k > 0 && k < 31) {
            /* clog2(x) == k -> x in [(1 << (k-1)) + 1, 1 << k] */
            int32_t x_lo = (1 << (k - 1)) + 1;
            int32_t x_hi = (1 << k);
            if ((r = ctx_tighten_lb32(ctx, xid, x_lo)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub32(ctx, xid, x_hi)) != PROP_OK) return r;
        }
    }

    return PROP_OK;
}

uint32_t prop_add_clog2_32(SolveCtx *ctx, uint32_t result_id,
                            uint32_t operand_id, uint8_t priority) {
    uint32_t ids[2] = { result_id, operand_id };
    return _alloc_prop(ctx, _fire_clog2_32, priority, 2, ids,
                       sizeof(Clog2_32_t));
}

/* ------------------------------------------------------------------ */
/* contra_register_explanations -- walk all propagators and set        */
/* explain callbacks based on fire function pointer lookup.            */
/* ------------------------------------------------------------------ */

typedef struct {
    PropResult (*fire)(Propagator *, SolveCtx *);
    int (*explain)(Propagator *, SolveCtx *, uint32_t, uint8_t, int64_t, Explanation *);
} ExplainEntry;

/* Reverse-lookup: propagator fire-fn pointer -> short human-readable
 * name. Kept in lockstep with the explain table below; if you add a
 * propagator there, add it here too. Used only by DV_LCG_TRACE. */
const char *prop_fire_name(PropResult (*fire)(Propagator *, SolveCtx *)) {
    #define FN_NAME(F) if (fire == F) return #F
    FN_NAME(_fire_bounds_le_32);
    FN_NAME(_fire_bounds_lt_32);
    FN_NAME(_fire_bounds_eq_32);
    FN_NAME(_fire_bounds_ne_32);
    FN_NAME(_fire_bounds_add_32);
    FN_NAME(_fire_bounds_mul_32);
    FN_NAME(_fire_bounds_div_32);
    FN_NAME(_fire_bounds_mod_32);
    FN_NAME(_fire_unary_neg_32);
    FN_NAME(_fire_in_set_32);
    FN_NAME(_fire_implication_32);
    FN_NAME(_fire_reification_32);
    FN_NAME(_fire_reification_eq_32);
    FN_NAME(_fire_bit_slice_32);
    FN_NAME(_fire_bounds_le_64);
    FN_NAME(_fire_bounds_lt_64);
    FN_NAME(_fire_bounds_eq_64);
    FN_NAME(_fire_bounds_ne_64);
    FN_NAME(_fire_bounds_add_64);
    FN_NAME(_fire_bounds_mul_64);
    FN_NAME(_fire_bounds_div_64);
    FN_NAME(_fire_bounds_mod_64);
    FN_NAME(_fire_unary_neg_64);
    FN_NAME(_fire_ite_value_64);
    FN_NAME(_fire_in_set_64);
    FN_NAME(_fire_bit_slice_64);
    FN_NAME(_fire_bounds_band_64);
    FN_NAME(_fire_bounds_bor_64);
    FN_NAME(_fire_bounds_bxor_64);
    FN_NAME(_fire_bounds_bnot_64);
    FN_NAME(_fire_bounds_shl_64);
    FN_NAME(_fire_bounds_lshr_64);
    FN_NAME(_fire_bounds_concat_64);
    FN_NAME(_fire_disj_clause);
    FN_NAME(_fire_all_different_32);
    FN_NAME(_fire_sum_eq_32);
    FN_NAME(_fire_countones_32);
    FN_NAME(_fire_clog2_32);
    #undef FN_NAME
    return "?fire";
}

void contra_register_explanations(SolveCtx *ctx) {
    static const ExplainEntry table[] = {
        { _fire_bounds_le_32,       explain_bounds_le },
        { _fire_bounds_lt_32,       explain_bounds_lt },
        { _fire_bounds_eq_32,       explain_bounds_eq },
        { _fire_bounds_ne_32,       explain_bounds_ne },
        { _fire_bounds_add_32,      explain_bounds_add },
        { _fire_bounds_mul_32,      explain_bounds_mul },
        { _fire_bounds_div_32,      explain_bounds_div },
        { _fire_bounds_mod_32,      explain_bounds_mod },
        { _fire_unary_neg_32,       explain_unary_neg },
        { _fire_in_set_32,          explain_in_set },
        { _fire_implication_32,     explain_implication },
        { _fire_reification_32,     explain_reification },
        { _fire_reification_eq_32,  explain_reification_eq },
        { _fire_bit_slice_32,       explain_bit_slice },
        { _fire_bounds_le_64,       explain_bounds_le },
        { _fire_bounds_lt_64,       explain_bounds_lt },
        { _fire_bounds_eq_64,       explain_bounds_eq },
        { _fire_bounds_ne_64,       explain_bounds_ne },
        { _fire_bounds_add_64,      explain_bounds_add },
        { _fire_bounds_mul_64,      explain_bounds_mul },
        { _fire_bounds_div_64,      explain_bounds_div },
        { _fire_bounds_mod_64,      explain_bounds_mod },
        { _fire_unary_neg_64,       explain_unary_neg },
        { _fire_ite_value_64,       explain_ite_value },
        { _fire_in_set_64,          explain_in_set },
        { _fire_bit_slice_64,       explain_bit_slice },
        { _fire_bounds_band_64,     explain_bounds_band },
        { _fire_bounds_bor_64,      explain_bounds_bor },
        { _fire_bounds_bxor_64,     explain_bounds_bxor },
        { _fire_bounds_bnot_64,     explain_bounds_bnot },
        { _fire_bounds_shl_64,      explain_bounds_shl },
        { _fire_bounds_lshr_64,     explain_bounds_lshr },
        { _fire_bounds_concat_64,   explain_bounds_concat },
        { _fire_disj_clause,        explain_disj_clause },
        { _fire_all_different_32,   explain_all_different },
        { _fire_sum_eq_32,          explain_sum_eq },
        { _fire_countones_32,       explain_countones },
        { _fire_clog2_32,           explain_clog2 },
    };
    static const uint32_t n_entries = sizeof(table) / sizeof(table[0]);

    uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                   ? ctx->n_props : ctx->n_prop_refs_capacity;
    for (uint32_t pi = 0; pi < lim; pi++) {
        if (ctx->prop_refs[pi] == EXPR_NULL) continue;
        Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ctx->prop_refs[pi]);
        if (p->explain) continue;  /* already set */

        for (uint32_t j = 0; j < n_entries; j++) {
            if (p->fire == table[j].fire) {
                p->explain = table[j].explain;
                break;
            }
        }
    }
}
