#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "zsp_propagator.h"
#include "zsp_ctx.h"
#include "zsp_lcg.h"
#include "zsp_explain.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define PROP_WS(p) ((PropWatchSect *)((char *)(p) + sizeof(Propagator)))

/* (a * b) mod m for a, b < m, computed via a 128-bit intermediate. MSVC lacks
 * __int128, so use the x64 _umul128/_udiv128 intrinsics there (the a,b < m
 * precondition guarantees the 128/64 division does not overflow). */
static inline uint64_t zsp_mulmod_u64(uint64_t a, uint64_t b, uint64_t m) {
#if defined(_MSC_VER)
    unsigned __int64 hi, rem;
    unsigned __int64 lo = _umul128(a, b, &hi);
    _udiv128(hi, lo, m, &rem);
    return rem;
#else
    return (uint64_t)((unsigned __int128)a * (unsigned __int128)b % m);
#endif
}

static int32_t i32_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t i32_max(int32_t a, int32_t b) { return a > b ? a : b; }
static int64_t i64_min(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t i64_max(int64_t a, int64_t b) { return a > b ? a : b; }

/* SystemVerilog (truncated) signed division / remainder. b must be != 0.
 *   q = trunc(a/b)        (truncate toward zero, NOT floor)
 *   r = a - b*q           (remainder takes the sign of the dividend a)
 * C99's '/' and '%' already truncate toward zero and give r the sign of a,
 * so these are thin wrappers that exist to document intent and to keep the
 * propagator/validator semantics provably identical. INT64_MIN/-1 overflow
 * cannot occur here: operands come from bounded BV domains < 64 bits. */
static int64_t sv_sdiv64(int64_t a, int64_t b) { return a / b; }
static int64_t sv_smod64(int64_t a, int64_t b) { return a % b; }

/* Wrap a value into a w-bit 2's-complement signed domain. Used for the one
 * overflowing signed-division case (INT_MIN / -1), where the true quotient
 * exceeds the result width and SV truncates it back into range. */
static int64_t sv_wrap_signed(int64_t v, uint16_t w) {
    if (w == 0 || w >= 64) return v;
    uint64_t m = ((uint64_t)1 << w) - 1;
    uint64_t u = (uint64_t)v & m;
    if (u & ((uint64_t)1 << (w - 1))) u |= ~m;
    return (int64_t)u;
}

/* Does signed value v fit in a w-bit signed domain (no truncation)? */
static int sv_fits_signed(int64_t v, uint16_t w) {
    if (w == 0 || w >= 64) return 1;
    int64_t lo = -((int64_t)1 << (w - 1));
    int64_t hi =  ((int64_t)1 << (w - 1)) - 1;
    return v >= lo && v <= hi;
}

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
    Variable      *r   = &ctx->vars[rid];
    Variable      *a   = &ctx->vars[aid];
    Variable      *b   = &ctx->vars[bid];

    int signed_op = (r->flags & VAR_SIGNED) != 0;

    PropResult res;
    if (signed_op) {
        /* SV-truncated signed division. var_lo64/hi64 return signed values
         * for signed vars. Divisor may be a negative singleton. */
        int64_t blo = var_lo64(ctx, b);
        int64_t bhi = var_hi64(ctx, b);
        if (blo != bhi && blo > 0) {
            /* All-positive divisor range: SV trunc-division (see 64-bit). */
            int64_t alo = var_lo64(ctx, a);
            int64_t ahi = var_hi64(ctx, a);
            int64_t rhi = sv_sdiv64(ahi, ahi >= 0 ? blo : bhi);
            int64_t rlo = sv_sdiv64(alo, alo <  0 ? blo : bhi);
            if ((res = ctx_tighten_lb32(ctx, rid, (int32_t)rlo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, rid, (int32_t)rhi)) != PROP_OK) return res;
            return PROP_OK;
        }
        if (blo == bhi && blo != 0) {
            uint16_t w   = r->width;
            int64_t k   = blo;
            int64_t alo = var_lo64(ctx, a);
            int64_t ahi = var_hi64(ctx, a);
            if (alo == ahi) {
                /* Exact: a singleton too. Wrap covers INT_MIN/-1 overflow. */
                int64_t q = sv_wrap_signed(sv_sdiv64(alo, k), w);
                if ((res = ctx_tighten_lb32(ctx, rid, (int32_t)q)) != PROP_OK) return res;
                if ((res = ctx_tighten_ub32(ctx, rid, (int32_t)q)) != PROP_OK) return res;
            } else {
                /* trunc-toward-zero division is monotonic in the dividend for
                 * a fixed divisor, so the extreme quotients occur at the
                 * dividend endpoints. If either endpoint overflows the result
                 * width (only k==-1 with a==INT_MIN), the wrapped result set
                 * is non-interval -> propagate weakly (SOUND: skip). */
                int64_t q0 = sv_sdiv64(alo, k);
                int64_t q1 = sv_sdiv64(ahi, k);
                if (sv_fits_signed(q0, w) && sv_fits_signed(q1, w)) {
                    int64_t rlo = i64_min(q0, q1);
                    int64_t rhi = i64_max(q0, q1);
                    if ((res = ctx_tighten_lb32(ctx, rid, (int32_t)rlo)) != PROP_OK) return res;
                    if ((res = ctx_tighten_ub32(ctx, rid, (int32_t)rhi)) != PROP_OK) return res;
                }
            }
        }
        return PROP_OK;
    }

    /* Unsigned: floor division, positive divisor only (unchanged). */
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

    int signed_op = (r->flags & VAR_SIGNED) != 0;

    PropResult res;

    if (signed_op) {
        /* SV remainder: r = a % b, takes the SIGN OF THE DIVIDEND a, valid
         * for negative divisors. |r| < |b|. We propagate exactly only when
         * a (and b) are singletons; otherwise we keep a SOUND but weak
         * r-range bound and leave the rest to search + the EQ relation.  */
        int64_t alo = var_lo64(ctx, a);
        int64_t ahi = var_hi64(ctx, a);
        int64_t blo = var_lo64(ctx, b);
        int64_t bhi = var_hi64(ctx, b);

        /* Forward: |r| <= |b|-1. With b a nonzero singleton, the sign of r
         * is constrained by the sign range of a:
         *   a >= 0 over its whole range -> r in [0, |b|-1]
         *   a <= 0 over its whole range -> r in [-(|b|-1), 0]
         *   mixed -> r in [-(|b|-1), |b|-1]                                */
        if (blo == bhi && blo != 0) {
            int64_t absb = blo < 0 ? -blo : blo;
            int64_t r_lo = (alo >= 0) ? 0 : -(absb - 1);
            int64_t r_hi = (ahi <= 0) ? 0 :  (absb - 1);
            if ((res = ctx_tighten_lb32(ctx, rid, (int32_t)r_lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, rid, (int32_t)r_hi)) != PROP_OK) return res;
        }

        /* Forward: singleton a and b -> compute r exactly. */
        if (alo == ahi && blo == bhi && blo != 0) {
            int32_t val = (int32_t)sv_smod64(alo, blo);
            if ((res = ctx_tighten_lb32(ctx, rid, val)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub32(ctx, rid, val)) != PROP_OK) return res;
        }
        return PROP_OK;
    }

    /* ---- Unsigned path (unchanged): non-negative remainder, b > 0. ---- */

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

/* Tier-1 (int64) sibling of _fire_implication_32: guard → (var ≤/≥ bound), using
 * the edge-guarded 64-bit tighten so it is sound on fields whose representable max
 * exceeds INT32_MAX (unsigned width 32). */
static PropResult _fire_implication_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect    *ws    = PROP_WS(self);
    Implication_64_t *iself = (Implication_64_t *)self;
    uint32_t          gid   = ws->var_ids[0];
    uint32_t          vid   = ws->var_ids[1];
    Variable         *g     = &ctx->vars[gid];

    if (g->hi == 0) return PROP_ENTAILED;          /* guard false → entailed */
    if (g->lo == 1) {                              /* guard true → enforce   */
        return iself->is_ub ? ctx_tighten_ub64(ctx, vid, iself->bound)
                            : ctx_tighten_lb64(ctx, vid, iself->bound);
    }
    return PROP_OK;
}

uint32_t prop_add_implication_64(SolveCtx *ctx,
                                   uint32_t guard_id, uint32_t var_id,
                                   int64_t bound, uint8_t is_ub,
                                   uint8_t priority) {
    uint32_t ids[2] = { guard_id, var_id };
    uint32_t ref = _alloc_prop(ctx, _fire_implication_64, priority, 2, ids,
                                sizeof(Implication_64_t));
    if (ref == EXPR_NULL) return EXPR_NULL;

    Implication_64_t *p = (Implication_64_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->bound = bound;
    p->is_ub = is_ub;
    return ref;
}

uint32_t prop_add_implication_32(SolveCtx *ctx,
                                   uint32_t guard_id, uint32_t var_id,
                                   int32_t bound, uint8_t is_ub,
                                   uint8_t priority) {
    /* Auto-promote to the 64-bit propagator when the constrained var is tier-1;
     * the 32-bit fire reads int32 bounds and re-queues forever on such fields. */
    if (!VAR_IS_TIER0(ctx->vars[var_id].flags))
        return prop_add_implication_64(ctx, guard_id, var_id,
                                       (int64_t)bound, is_ub, priority);
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
    /* Auto-promote to the 64-bit propagator when either compared operand is
     * tier-1 (representable max > INT32_MAX). The 32-bit fire reads int32 bounds
     * and is unsound / non-terminating on such fields; delegating here covers
     * every reification call site through one choke point. */
    if (!VAR_IS_TIER0(ctx->vars[x_id].flags) ||
        !VAR_IS_TIER0(ctx->vars[y_id].flags))
        return prop_add_reification_64(ctx, guard_id, x_id, y_id, priority);
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
    /* Auto-promote to the 64-bit propagator when either compared operand is
     * tier-1 (see prop_add_reification_32). */
    if (!VAR_IS_TIER0(ctx->vars[x_id].flags) ||
        !VAR_IS_TIER0(ctx->vars[y_id].flags))
        return prop_add_reification_eq_64(ctx, guard_id, x_id, y_id, priority);
    uint32_t ids[3] = { guard_id, x_id, y_id };
    return _alloc_prop(ctx, _fire_reification_eq_32, priority, 3, ids,
                       sizeof(ReificationEq_32_t));
}

/* ------------------------------------------------------------------ */
/* BitSlice_32:  r = a[hi_bit:lo_bit]                                 */
/* ------------------------------------------------------------------ */

/* Backward bit-slice support: for r == extract(a, hi, lo) with r fixed to a
 * single value v, the bits [lo,hi] of a must equal v. These helpers compute the
 * tightest bound on the operand a within [alo,ahi] consistent with that
 * (all-unsigned arithmetic). Without backward propagation the operand is left
 * free, and because a forward-only slice is not bounds-consistent the search's
 * domain-bisection can wrongly report UNSAT (BUG-2). Exhaustively validated
 * against brute force for widths <= 10 by tests/unit/test_bitslice_backward.py. */

/* Values of `a` whose slice [lo,hi]==v form, within each fixed setting of the
 * bits above `hi`, a contiguous run [group_base, group_base|lomask] where
 * group_base = (high_bits | (v<<lo)) and the free low bits [0,lo) vary. The
 * helpers below walk to the nearest such run. */

/* Smallest a in [alo,ahi] whose slice [lo,hi] == v; *ok=0 if none exists. */
static uint64_t _slice_min_ge(uint64_t alo, uint64_t ahi,
                              uint32_t lo, uint32_t hi, uint64_t v, int *ok) {
    uint32_t sw = hi - lo + 1;
    uint64_t smask  = ((sw >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << sw) - 1)) << lo;
    uint64_t lomask = (lo == 0) ? 0 : (((uint64_t)1 << lo) - 1);
    uint64_t himask = (hi + 1 >= 64) ? 0 : ~(((uint64_t)1 << (hi + 1)) - 1);
    uint64_t tgt = (v << lo) & smask;
    uint64_t base = (alo & himask) | tgt;   /* this group's run, low bits = 0 */
    uint64_t run_hi = base | lomask;
    uint64_t nlo;
    if (base >= alo)          nlo = base;   /* whole run is >= alo */
    else if (run_hi >= alo)   nlo = alo;    /* alo sits inside this run */
    else {                                  /* run is below alo -> next group up */
        if (hi + 1 >= 64) { *ok = 0; return 0; }
        uint64_t nh = (alo >> (hi + 1)) + 1;
        nlo = (nh << (hi + 1)) | tgt;
    }
    if (nlo > ahi) { *ok = 0; return 0; }
    *ok = 1; return nlo;
}

/* Largest a in [alo,ahi] whose slice [lo,hi] == v; *ok=0 if none exists. */
static uint64_t _slice_max_le(uint64_t alo, uint64_t ahi,
                              uint32_t lo, uint32_t hi, uint64_t v, int *ok) {
    uint32_t sw = hi - lo + 1;
    uint64_t smask  = ((sw >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << sw) - 1)) << lo;
    uint64_t lomask = (lo == 0) ? 0 : (((uint64_t)1 << lo) - 1);
    uint64_t himask = (hi + 1 >= 64) ? 0 : ~(((uint64_t)1 << (hi + 1)) - 1);
    uint64_t tgt = (v << lo) & smask;
    uint64_t base = (ahi & himask) | tgt;   /* this group's run, low bits = 0 */
    uint64_t run_hi = base | lomask;
    uint64_t nhi;
    if (run_hi <= ahi)        nhi = run_hi;  /* whole run is <= ahi */
    else if (base <= ahi)     nhi = ahi;     /* ahi sits inside this run */
    else {                                   /* run is above ahi -> next group down */
        uint64_t hh = (hi + 1 >= 64) ? 0 : (ahi >> (hi + 1));
        if (hh == 0) { *ok = 0; return 0; }
        hh -= 1;
        nhi = (hh << (hi + 1)) | tgt | lomask;
    }
    if (nhi < alo) { *ok = 0; return 0; }
    *ok = 1; return nhi;
}

/* Tighten operand `aid` from a singleton result `v` for slice [lo,hi]. Applies
 * only for a non-negative (unsigned-interpretable) operand interval.
 *
 * The `DV_NO_BITSLICE_BACKWARD` env var (read once) disables this backward
 * pruning, leaving the propagator forward-only. That is a *test knob*: with
 * backward pruning off the operand must be resolved by the search alone, which
 * exercises the domain-bisection completeness fix for BUG-3
 * (tests/unit/test_search_completeness.py). It has no effect on results — a
 * correct solver returns the same SAT/UNSAT either way — only on how much work
 * the search does. Not for production use. */
static PropResult _bit_slice_backward(SolveCtx *ctx, uint32_t aid,
                                      uint32_t lo, uint32_t hi, uint64_t v,
                                      int64_t alo, int64_t ahi) {
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("DV_NO_BITSLICE_BACKWARD") ? 1 : 0;
    if (disabled) return PROP_OK;
    if (alo < 0 || ahi < 0 || (uint64_t)alo > (uint64_t)ahi) return PROP_OK;
    int ok1, ok2;
    uint64_t nlo = _slice_min_ge((uint64_t)alo, (uint64_t)ahi, lo, hi, v, &ok1);
    uint64_t nhi = _slice_max_le((uint64_t)alo, (uint64_t)ahi, lo, hi, v, &ok2);
    if (!ok1 || !ok2) return PROP_CONFLICT;
    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, aid, (int64_t)nlo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, aid, (int64_t)nhi)) != PROP_OK) return r;
    return PROP_OK;
}

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

    /* When the operand is pinned, compute r exactly. */
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
    /* Backward: operand not singleton, but if the slice result is fixed, tighten
     * the operand's bounds to values whose slice matches. Required for
     * bounds-consistency and a complete search — see _bit_slice_backward /
     * BUG-2 (the old forward-only version returned spurious UNSAT). */
    {
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        if (rlo == rhi) {
            res = _bit_slice_backward(ctx, aid, bself->lo_bit, bself->hi_bit,
                                      (uint64_t)rlo, alo, ahi);
            if (res != PROP_OK) return res;
        }
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
        out->lits[0].bound = new_bound;
        return 0;
    }
    if (var_id == yid && is_lb) {
        /* y.lo >= new_bound because x.lo >= new_bound */
        out->lits[0].var_id = xid;
        out->lits[0].is_lb  = 1;
        out->lits[0].bound = new_bound;
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
        out->lits[0].bound = new_bound + 1;
        return 0;
    }
    if (var_id == yid && is_lb) {
        /* y.lo >= new_bound because x.lo >= new_bound-1 i.e. (x >= new_bound-1) */
        out->lits[0].var_id = xid;
        out->lits[0].is_lb  = 1;
        out->lits[0].bound = new_bound - 1;
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

    /* Sign-aware intersection: unsigned upper-half bounds must order as
     * unsigned (x and y share the field's signedness). */
    const Variable *vx = &ctx->vars[xid];
    int64_t lo = var_b_max(vx, var_lo64(ctx, &ctx->vars[xid]), var_lo64(ctx, &ctx->vars[yid]));
    int64_t hi = var_b_min(vx, var_hi64(ctx, &ctx->vars[xid]), var_hi64(ctx, &ctx->vars[yid]));
    if (var_b_gt(vx, lo, hi)) return PROP_CONFLICT;

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
    out->lits[0].bound = new_bound;
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
        out->lits[0].var_id = aid; out->lits[0].is_lb = 1; out->lits[0].bound = alo;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 1; out->lits[1].bound = blo;
        return 0;
    }
    if (var_id == rid && !is_lb) {
        int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
        int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
        out->lits[0].var_id = aid; out->lits[0].is_lb = 0; out->lits[0].bound = ahi;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 0; out->lits[1].bound = bhi;
        return 0;
    }
    if (var_id == aid && is_lb) {
        /* a.lo >= new_bound because r.lo - b.hi >= new_bound */
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 1; out->lits[0].bound = rlo;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 0; out->lits[1].bound = bhi;
        return 0;
    }
    if (var_id == aid && !is_lb) {
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 0; out->lits[0].bound = rhi;
        out->lits[1].var_id = bid; out->lits[1].is_lb = 1; out->lits[1].bound = blo;
        return 0;
    }
    if (var_id == bid && is_lb) {
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 1; out->lits[0].bound = rlo;
        out->lits[1].var_id = aid; out->lits[1].is_lb = 0; out->lits[1].bound = ahi;
        return 0;
    }
    if (var_id == bid && !is_lb) {
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
        out->lits[0].var_id = rid; out->lits[0].is_lb = 0; out->lits[0].bound = rhi;
        out->lits[1].var_id = aid; out->lits[1].is_lb = 1; out->lits[1].bound = alo;
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
    out->lits[0].var_id = other; out->lits[0].is_lb = 1; out->lits[0].bound = olo;
    out->lits[0]._pad[0] = out->lits[0]._pad[1] = out->lits[0]._pad[2] = 0;
    out->lits[1].var_id = other; out->lits[1].is_lb = 0; out->lits[1].bound = ohi;
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

/* ------------------------------------------------------------------ */
/* BvBin_64: modular fixed-width 2's-complement var-var arithmetic.    */
/*   r = (a op b) mod 2^w  for op in {ADD, SUB, MUL, SHL}, width 1..63 */
/*   var_ids[0]=r, var_ids[1]=a, var_ids[2]=b.                         */
/*                                                                     */
/* SIGNEDNESS: every variable's signed/unsigned [lo,hi] (which fits in */
/* the w-bit width, so hi-lo < 2^w) maps to a contiguous *modular*     */
/* interval of w-bit unsigned residues. With mask = 2^w-1 the residue  */
/* set is { (lo&mask + i) mod 2^w : 0 <= i <= hi-lo }, represented as a */
/* (start, len) pair. This is bit-identical for signed and unsigned    */
/* operands because 2's-complement add/sub/mul wrap == unsigned mod    */
/* 2^w. All modular arithmetic below is therefore signedness-agnostic; */
/* signedness only re-enters when we map a residue interval back onto a */
/* signed result variable's [lo,hi] for tightening.                    */
/* ------------------------------------------------------------------ */

/* A modular interval: residues { (start+i) mod M : 0<=i<len }, len in
 * 1..M. len==M means the full domain. start is always in [0, M-1]. */
typedef struct { uint64_t start; uint64_t len; } ModIv;

/* Convert a variable's current [lo,hi] domain to its modular interval. */
static ModIv _var_modiv(SolveCtx *ctx, uint32_t id, uint64_t M) {
    uint64_t mask = M - 1;
    int64_t lo = var_lo64(ctx, &ctx->vars[id]);
    int64_t hi = var_hi64(ctx, &ctx->vars[id]);
    ModIv iv;
    /* hi >= lo always; hi-lo < M because the domain fits the width. */
    uint64_t span = (uint64_t)(hi - lo);   /* 0 .. M-1 */
    iv.start = (uint64_t)lo & mask;
    iv.len   = span + 1;                   /* 1 .. M */
    if (iv.len > M) iv.len = M;
    return iv;
}

/* Tighten the w-bit result variable `id` so that its residues are
 * confined to the modular interval `iv`. SOUND: only removes residues
 * proven infeasible. Returns PROP_OK / PROP_CONFLICT.
 *
 * The variable's domain [lo,hi] occupies a modular interval; we narrow
 * it only when one endpoint can be advanced without dropping a feasible
 * residue. To stay simple and sound we map `iv` to the variable's value
 * space and tighten the lo/hi bounds when iv lies wholly on one side. */
static PropResult _tighten_to_modiv(SolveCtx *ctx, uint32_t id,
                                    ModIv iv, uint64_t M) {
    if (iv.len >= M) return PROP_OK;   /* full domain: nothing to learn */
    uint64_t mask = M - 1;
    int signed_v = (ctx->vars[id].flags & VAR_SIGNED) != 0;
    uint8_t w = (uint8_t)ctx->vars[id].width;

    int64_t cur_lo = var_lo64(ctx, &ctx->vars[id]);
    int64_t cur_hi = var_hi64(ctx, &ctx->vars[id]);

    /* iv as a (possibly wrapping) residue interval [a0, a1] mod M. */
    uint64_t a0 = iv.start;
    uint64_t a1 = (iv.start + iv.len - 1) & mask;
    int iv_wraps = (a1 < a0);

    /* Current domain as residue interval [c0,c1] mod M. */
    ModIv cur = _var_modiv(ctx, id, M);
    uint64_t c0 = cur.start;
    uint64_t c1 = (cur.start + cur.len - 1) & mask;
    int cur_wraps = (c1 < c0);

    /* General modular-interval intersection is not necessarily a single
     * interval. We only act in the common, sound, easy cases:
     *  (1) iv does not wrap and the current domain does not wrap: a plain
     *      interval intersection in residue space; map back to value space.
     *  (2) otherwise: fall back to bound-based tightening using the value
     *      space directly when iv is a non-wrapping residue interval that
     *      maps to a contiguous value interval.
     * If anything is ambiguous we propagate nothing (sound). */
    if (iv_wraps) return PROP_OK;          /* keep it simple & sound */

    /* iv is [a0,a1] with a0<=a1 in residue space. Map these residues to
     * the variable's value space. For an unsigned var, residue==value.
     * For a signed var, residues in [0, 2^(w-1)-1] are >=0 and residues
     * in [2^(w-1), 2^w-1] are negative (value = residue - 2^w). */
    int64_t v_a0, v_a1;
    if (!signed_v) {
        v_a0 = (int64_t)a0;
        v_a1 = (int64_t)a1;
    } else {
        uint64_t half = (uint64_t)1 << (w - 1);
        /* If iv straddles the sign boundary it is not a contiguous value
         * interval; skip (sound). */
        int a0_neg = (a0 >= half);
        int a1_neg = (a1 >= half);
        if (a0_neg != a1_neg) {
            /* iv covers both negative (high residues) and non-negative
             * (low residues) values but as a *residue* interval [a0,a1]
             * (a0<=a1) that means low values then ... no: a0<=a1 with
             * a0<half<=a1 means non-neg values [a0..half-1] then neg
             * values [half..a1] — not contiguous in value space. Skip. */
            return PROP_OK;
        }
        v_a0 = a0_neg ? (int64_t)a0 - (int64_t)M : (int64_t)a0;
        v_a1 = a1_neg ? (int64_t)a1 - (int64_t)M : (int64_t)a1;
    }
    /* v_a0 <= v_a1 now (same sign region, residues ordered). */

    (void)c0; (void)c1; (void)cur_wraps;
    PropResult res;
    if ((res = ctx_tighten_lb64(ctx, id, v_a0 > cur_lo ? v_a0 : cur_lo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub64(ctx, id, v_a1 < cur_hi ? v_a1 : cur_hi)) != PROP_OK) return res;
    return PROP_OK;
}

/* Minkowski sum of two modular intervals (exact). */
static ModIv _modiv_add(ModIv x, ModIv y, uint64_t M) {
    ModIv r;
    /* combined length len_x+len_y-1, capped at M (full). */
    uint64_t lx = x.len, ly = y.len;
    if (lx >= M || ly >= M || lx + ly - 1 >= M) { r.start = 0; r.len = M; return r; }
    r.start = (x.start + y.start) & (M - 1);
    r.len   = lx + ly - 1;
    return r;
}

/* Negate a modular interval: { (-u) mod M : u in iv } (exact). */
static ModIv _modiv_neg(ModIv x, uint64_t M) {
    ModIv r;
    if (x.len >= M) { r.start = 0; r.len = M; return r; }
    uint64_t hi = (x.start + x.len - 1) & (M - 1);
    r.start = ((M - hi) & (M - 1));   /* -hi mod M is the new lowest */
    r.len   = x.len;
    return r;
}

/* Product of two modular intervals, bounded soundly. We lift each to its
 * representative non-negative integer interval [start, start+len-1] and
 * take the integer Minkowski product; if its span >= M the result is the
 * full domain, otherwise it reduces to a single modular interval that is
 * a sound superset of the true residue set. */
static ModIv _modiv_mul(ModIv x, ModIv y, uint64_t M) {
    ModIv r;
    /* A singleton-zero operand forces the product to 0 regardless of the
     * other operand's range (handle before the full-domain shortcut). */
    if ((x.len == 1 && x.start == 0) || (y.len == 1 && y.start == 0)) {
        r.start = 0; r.len = 1; return r;
    }
    if (x.len >= M || y.len >= M) { r.start = 0; r.len = M; return r; }
    uint64_t xlo = x.start, xhi = x.start + x.len - 1;
    uint64_t ylo = y.start, yhi = y.start + y.len - 1;
    /* All non-negative; products are monotone in both args. */
    uint64_t plo, phi;
    int of = __builtin_mul_overflow(xlo, ylo, &plo)
           | __builtin_mul_overflow(xhi, yhi, &phi);
    if (of) { r.start = 0; r.len = M; return r; }
    if (phi - plo >= M - 1) { r.start = 0; r.len = M; return r; }
    r.start = plo & (M - 1);
    r.len   = (phi - plo) + 1;
    return r;
}

/* Shift-left of a modular interval by a shift-amount interval [s0,s1]
 * (s0,s1 are actual non-negative shift counts). Sound superset. */
static ModIv _modiv_shl(ModIv x, uint64_t s0, uint64_t s1, uint8_t w, uint64_t M) {
    ModIv r;
    /* Any shift >= w produces residue 0 (checked before the full-domain
     * shortcut: even a full-domain operand shifted out is exactly 0). */
    if (s0 >= w) { r.start = 0; r.len = 1; return r; }   /* all shifts -> 0 */
    if (x.len >= M) { r.start = 0; r.len = M; return r; }
    int has_zero = (s1 >= w);
    uint64_t eff_hi = (s1 >= w) ? (uint64_t)(w - 1) : s1;
    uint64_t xlo = x.start, xhi = x.start + x.len - 1;
    /* Representative integer interval over shifts [s0, eff_hi]:
     * min value = xlo << s0, max value = xhi << eff_hi (monotone). */
    uint64_t plo = xlo << s0;
    uint64_t phi = xhi << eff_hi;
    /* plo,phi < 2^w << (w-1) <= 2^(2w-1) <= 2^125 for w<=63: fits u64?
     * Not necessarily. Guard against overflow by checking the shift. */
    if (eff_hi >= 64 || (xhi != 0 && (phi >> eff_hi) != xhi)) {
        r.start = 0; r.len = M; return r;   /* overflow: full domain */
    }
    ModIv shifted;
    if (phi - plo >= M - 1) { shifted.start = 0; shifted.len = M; }
    else { shifted.start = plo & (M - 1); shifted.len = (phi - plo) + 1; }
    if (!has_zero) return shifted;
    if (shifted.len >= M) return shifted;
    /* Union {0} with `shifted`. If 0 already inside, no change. Else we
     * cannot represent a non-contiguous union as one modular interval, so
     * fall back to the full domain (sound, weak). */
    uint64_t s_lo = shifted.start;
    uint64_t s_hi = (shifted.start + shifted.len - 1) & (M - 1);
    int zero_in = (s_lo <= s_hi) ? (0 >= s_lo && 0 <= s_hi)
                                 : (0 >= s_lo || 0 <= s_hi);
    if (zero_in) { r = shifted; return r; }
    r.start = 0; r.len = M; return r;
}

/* Extended GCD: returns g = gcd(a,b), sets *x,*y with a*x + b*y = g. */
static uint64_t _egcd(uint64_t a, uint64_t b, int64_t *x, int64_t *y) {
    if (b == 0) { *x = 1; *y = 0; return a; }
    int64_t x1, y1;
    uint64_t g = _egcd(b, a % b, &x1, &y1);
    *x = y1;
    *y = x1 - (int64_t)(a / b) * y1;
    return g;
}

/* Backward tighten for r = (a * k) mod M with k a singleton residue, used
 * to detect infeasible r values and pin `a` once `r` is fixed. We solve
 *   a*k ≡ rr (mod M)
 * for the (singleton) required residue rr. SOUND and, crucially, returns
 * PROP_CONFLICT when no `a` residue can produce the required r (e.g. r odd
 * but k even) — this keeps the chronological search complete when r is a
 * decision variable. Solutions form an arithmetic progression of g =
 * gcd(k,M) residues (none if g ∤ rr). We pin `a` only when the solution is
 * unique (g==1) or pin/narrow the operand to the bounding range of its
 * feasible solution set (enumerated when g is small). g>1 with a large
 * enumeration cost is skipped (sound, weaker). */
#define BV_MUL_BACK_MAX_SOLS 4096u
static PropResult _bv_mul_back_singleton(SolveCtx *ctx, uint32_t aid,
                                         uint64_t k, ModIv R, uint64_t M) {
    if (R.len != 1) return PROP_OK;       /* only when r is fixed */
    uint64_t rr = R.start;
    k &= (M - 1);
    if (k == 0) {
        if (rr != 0) return PROP_CONFLICT;   /* 0 != rr -> infeasible */
        return PROP_OK;
    }
    int64_t kx, ky;
    uint64_t g = _egcd(k, M, &kx, &ky);
    if ((rr % g) != 0) return PROP_CONFLICT; /* no solution -> conflict */

    /* The g = gcd(k,M) solution residues are  a0 + t*(M/g), t=0..g-1, where
     *   a0 = (rr/g) * inv(k/g)  (mod M/g),  inv computed over the reduced
     * modulus M/g (where k/g is a unit). */
    uint64_t Mg = M / g;
    uint64_t kg = (k / g) % Mg;
    int64_t  ix, iy;
    (void)_egcd(kg, Mg, &ix, &iy);        /* gcd is 1 by construction */
    int64_t inv = ix % (int64_t)Mg;
    if (inv < 0) inv += (int64_t)Mg;
    uint64_t a0 = zsp_mulmod_u64(rr / g, (uint64_t)inv, Mg);

    if (g == 1) {                         /* unique solution */
        ModIv Aiv; Aiv.start = a0 % M; Aiv.len = 1;
        return _tighten_to_modiv(ctx, aid, Aiv, M);
    }
    if (g > BV_MUL_BACK_MAX_SOLS) return PROP_OK;  /* too many: skip (sound) */

    /* Enumerate the g feasible residues; map each to the operand's value
     * space and keep those inside the current domain. Tighten the operand's
     * bounds to [min_feasible, max_feasible]; empty -> conflict. */
    int      signed_v = (ctx->vars[aid].flags & VAR_SIGNED) != 0;
    uint8_t  w        = (uint8_t)ctx->vars[aid].width;
    uint64_t half     = (uint64_t)1 << (w - 1);
    int64_t  cur_lo   = var_lo64(ctx, &ctx->vars[aid]);
    int64_t  cur_hi   = var_hi64(ctx, &ctx->vars[aid]);
    int64_t  fmin = 0, fmax = 0;
    int      have = 0;
    for (uint64_t t = 0; t < g; t++) {
        uint64_t res = (a0 + t * Mg) & (M - 1);
        int64_t  val = (signed_v && res >= half) ? (int64_t)res - (int64_t)M
                                                 : (int64_t)res;
        if (val < cur_lo || val > cur_hi) continue;
        if (!have) { fmin = fmax = val; have = 1; }
        else { if (val < fmin) fmin = val; if (val > fmax) fmax = val; }
    }
    if (!have) return PROP_CONFLICT;      /* no feasible operand value */
    PropResult res;
    if ((res = ctx_tighten_lb64(ctx, aid, fmin)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub64(ctx, aid, fmax)) != PROP_OK) return res;
    return PROP_OK;
}

/* Derive the shift-amount range from the shift operand's *unsigned w-bit
 * pattern* (matching SystemVerilog / the model validator, which uses the
 * unsigned value of the shift operand). Returns 1 if the patterns form a
 * contiguous range [s0,s1]; 0 if they wrap (non-contiguous) and the caller
 * should fall back to the full domain (sound). */
static int _shift_range(ModIv B, uint64_t M, uint64_t *s0, uint64_t *s1) {
    if (B.len >= M) { *s0 = 0; *s1 = M - 1; return 1; }
    uint64_t hi = (B.start + B.len - 1) & (M - 1);
    if (hi < B.start) return 0;   /* wraps: non-contiguous pattern set */
    *s0 = B.start;
    *s1 = hi;
    return 1;
}

static PropResult _fire_bvbin_64(Propagator *self, SolveCtx *ctx, int op) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       rid = ws->var_ids[0];
    uint32_t       aid = ws->var_ids[1];
    uint32_t       bid = ws->var_ids[2];
    BvBin_64_t    *bp  = (BvBin_64_t *)self;
    uint8_t        w   = bp->width;
    if (w == 0 || w >= 64) return PROP_OK;
    uint64_t M = (uint64_t)1 << w;

    ModIv A = _var_modiv(ctx, aid, M);
    ModIv B = _var_modiv(ctx, bid, M);
    PropResult res;

    if (op == BIN_ADD) {
        ModIv R = _modiv_add(A, B, M);
        if ((res = _tighten_to_modiv(ctx, rid, R, M)) != PROP_OK) return res;
        /* Backward: a = r - b, b = r - a. */
        ModIv Rc = _var_modiv(ctx, rid, M);
        ModIv Anew = _modiv_add(Rc, _modiv_neg(B, M), M);
        if ((res = _tighten_to_modiv(ctx, aid, Anew, M)) != PROP_OK) return res;
        ModIv Ac = _var_modiv(ctx, aid, M);
        ModIv Bnew = _modiv_add(Rc, _modiv_neg(Ac, M), M);
        if ((res = _tighten_to_modiv(ctx, bid, Bnew, M)) != PROP_OK) return res;
    } else if (op == BIN_SUB) {
        ModIv R = _modiv_add(A, _modiv_neg(B, M), M);
        if ((res = _tighten_to_modiv(ctx, rid, R, M)) != PROP_OK) return res;
        /* Backward: a = r + b, b = a - r. */
        ModIv Rc = _var_modiv(ctx, rid, M);
        ModIv Anew = _modiv_add(Rc, B, M);
        if ((res = _tighten_to_modiv(ctx, aid, Anew, M)) != PROP_OK) return res;
        ModIv Ac = _var_modiv(ctx, aid, M);
        ModIv Bnew = _modiv_add(Ac, _modiv_neg(Rc, M), M);
        if ((res = _tighten_to_modiv(ctx, bid, Bnew, M)) != PROP_OK) return res;
    } else if (op == BIN_MUL) {
        ModIv R = _modiv_mul(A, B, M);
        if ((res = _tighten_to_modiv(ctx, rid, R, M)) != PROP_OK) return res;
        /* Backward: when one operand is a singleton constant, derive the
         * other operand from r. This detects infeasible r values (e.g. r
         * odd while the constant is even) so the search stays complete. */
        ModIv Rc = _var_modiv(ctx, rid, M);
        ModIv Ac = _var_modiv(ctx, aid, M);
        ModIv Bc = _var_modiv(ctx, bid, M);
        if (Bc.len == 1) {
            if ((res = _bv_mul_back_singleton(ctx, aid, Bc.start, Rc, M)) != PROP_OK) return res;
        } else if (Ac.len == 1) {
            if ((res = _bv_mul_back_singleton(ctx, bid, Ac.start, Rc, M)) != PROP_OK) return res;
        }
    } else if (op == BIN_LSHIFT) {
        uint64_t s0, s1;
        if (_shift_range(B, M, &s0, &s1)) {
            ModIv R = _modiv_shl(A, s0, s1, w, M);
            if ((res = _tighten_to_modiv(ctx, rid, R, M)) != PROP_OK) return res;
        }
        /* Backward (keeps the search complete when r is a decision var):
         * when r is fixed to rr, rr must be producible by SOME feasible
         * shift s and SOME a-residue. For s<w, rr is producible (by some
         * a-residue in the full space) iff rr's low s bits are 0; for any
         * s>=w, rr is producible iff rr==0. If NO feasible shift can yield
         * rr, the assignment is infeasible -> conflict.  We deliberately
         * ignore a's domain restriction here (only makes us weaker, never
         * unsound). When the shift is a singleton s and rr is producible we
         * additionally pin a for s==0 (a==rr). */
        uint64_t bs0, bs1;
        ModIv Rc = _var_modiv(ctx, rid, M);
        ModIv Ac2 = _var_modiv(ctx, aid, M);
        if (_shift_range(B, M, &bs0, &bs1) && Rc.len == 1 && Ac2.len == 1) {
            /* Both a and r fixed: rr must equal (a<<s)&mask for some feasible
             * shift s, else conflict. Exact and sound. */
            uint64_t rr = Rc.start;
            uint64_t av = Ac2.start;
            int any = 0;
            /* Any shift >= w yields result 0. */
            if (bs1 >= w && rr == 0) any = 1;
            if (!any) {
                uint64_t hi_s = (bs1 > (uint64_t)(w - 1)) ? (uint64_t)(w - 1) : bs1;
                for (uint64_t s = bs0; s <= hi_s; s++) {
                    if (((av << s) & (M - 1)) == rr) { any = 1; break; }
                }
            }
            if (!any) return PROP_CONFLICT;
        } else if (_shift_range(B, M, &bs0, &bs1) && Rc.len == 1) {
            uint64_t rr = Rc.start;
            int any = 0;
            /* If any feasible shift is >= w, the result-0 case is reachable,
             * so rr==0 is producible. */
            if (bs1 >= w && rr == 0) any = 1;
            /* Otherwise scan the meaningful shifts s in [bs0, min(bs1,w-1)]:
             * rr is producible by shift s iff rr's low s bits are 0. */
            if (!any) {
                uint64_t lo_s = bs0;
                uint64_t hi_s = (bs1 > (uint64_t)(w - 1)) ? (uint64_t)(w - 1) : bs1;
                for (uint64_t s = lo_s; s <= hi_s; s++) {
                    uint64_t low_mask = (s == 0) ? 0 : ((uint64_t)1 << s) - 1;
                    if ((rr & low_mask) == 0) { any = 1; break; }
                }
            }
            if (!any) return PROP_CONFLICT;
            if (bs0 == bs1 && bs0 == 0) {        /* shift 0: a == rr */
                ModIv Aiv; Aiv.start = rr; Aiv.len = 1;
                if ((res = _tighten_to_modiv(ctx, aid, Aiv, M)) != PROP_OK) return res;
            }
        }
    }
    return PROP_OK;
}

static PropResult _fire_bvadd_64(Propagator *self, SolveCtx *ctx) { return _fire_bvbin_64(self, ctx, BIN_ADD); }
static PropResult _fire_bvsub_64(Propagator *self, SolveCtx *ctx) { return _fire_bvbin_64(self, ctx, BIN_SUB); }
static PropResult _fire_bvmul_64(Propagator *self, SolveCtx *ctx) { return _fire_bvbin_64(self, ctx, BIN_MUL); }
static PropResult _fire_bvshl_64(Propagator *self, SolveCtx *ctx) { return _fire_bvbin_64(self, ctx, BIN_LSHIFT); }

/* Conservative sound explanation: the modular tightening of any one var
 * depends on the full current domains of the other two vars; cite both
 * bounds of each other variable. */
static int _explain_bvbin_64(Propagator *self, SolveCtx *ctx,
                             uint32_t var_id, uint8_t is_lb,
                             int64_t new_bound, Explanation *out) {
    (void)is_lb; (void)new_bound;
    PropWatchSect *ws = PROP_WS(self);
    uint32_t ids[3] = { ws->var_ids[0], ws->var_ids[1], ws->var_ids[2] };
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (ids[i] == var_id) continue;
        int64_t olo = var_lo64(ctx, &ctx->vars[ids[i]]);
        int64_t ohi = var_hi64(ctx, &ctx->vars[ids[i]]);
        out->lits[n].var_id = ids[i]; out->lits[n].is_lb = 1; out->lits[n].bound = olo;
        out->lits[n]._pad[0] = out->lits[n]._pad[1] = out->lits[n]._pad[2] = 0;
        n++;
        out->lits[n].var_id = ids[i]; out->lits[n].is_lb = 0; out->lits[n].bound = ohi;
        out->lits[n]._pad[0] = out->lits[n]._pad[1] = out->lits[n]._pad[2] = 0;
        n++;
    }
    out->n_lits = (uint32_t)n;
    return 0;
}

static uint32_t _prop_add_bvbin_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id,
                                   uint32_t b_id, uint8_t width, uint8_t priority,
                                   PropResult (*fire)(Propagator *, SolveCtx *)) {
    uint32_t ids[3] = { r_id, a_id, b_id };
    uint32_t ref = _alloc_prop(ctx, fire, priority, 3, ids, sizeof(BvBin_64_t));
    if (ref == EXPR_NULL) return ref;
    BvBin_64_t *bp = (BvBin_64_t *)zsp_pool_ptr(&ctx->pool, ref);
    bp->width = width;
    Propagator *p = (Propagator *)zsp_pool_ptr(&ctx->pool, ref);
    p->explain = _explain_bvbin_64;
    return ref;
}

uint32_t prop_add_bvadd_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority) {
    return _prop_add_bvbin_64(ctx, r_id, a_id, b_id, width, priority, _fire_bvadd_64);
}
uint32_t prop_add_bvsub_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority) {
    return _prop_add_bvbin_64(ctx, r_id, a_id, b_id, width, priority, _fire_bvsub_64);
}
uint32_t prop_add_bvmul_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority) {
    return _prop_add_bvbin_64(ctx, r_id, a_id, b_id, width, priority, _fire_bvmul_64);
}
uint32_t prop_add_bvshl_64(SolveCtx *ctx, uint32_t r_id, uint32_t a_id, uint32_t b_id, uint8_t width, uint8_t priority) {
    return _prop_add_bvbin_64(ctx, r_id, a_id, b_id, width, priority, _fire_bvshl_64);
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

    int signed_op = (ctx->vars[rid].flags & VAR_SIGNED) != 0;

    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    /* Only propagate when divisor is a non-zero singleton or positive range */
    if (signed_op && blo == bhi && blo != 0) {
        /* SV-truncated signed division. */
        uint16_t w   = ctx->vars[rid].width;
        int64_t  k   = blo;
        int64_t  alo = var_lo64(ctx, &ctx->vars[aid]);
        int64_t  ahi = var_hi64(ctx, &ctx->vars[aid]);
        if (alo == ahi) {
            int64_t q = sv_wrap_signed(sv_sdiv64(alo, k), w);
            if ((res = ctx_tighten_lb64(ctx, rid, q)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, q)) != PROP_OK) return res;
        } else {
            /* trunc-toward-zero is monotonic in the dividend for fixed k;
             * extremes occur at the endpoints. Skip (weak/sound) if an
             * endpoint overflows the width (INT_MIN/-1). */
            int64_t q0 = sv_sdiv64(alo, k);
            int64_t q1 = sv_sdiv64(ahi, k);
            if (sv_fits_signed(q0, w) && sv_fits_signed(q1, w)) {
                int64_t rlo = i64_min(q0, q1);
                int64_t rhi = i64_max(q0, q1);
                if ((res = ctx_tighten_lb64(ctx, rid, rlo)) != PROP_OK) return res;
                if ((res = ctx_tighten_ub64(ctx, rid, rhi)) != PROP_OK) return res;
            }
        }
        return PROP_OK;
    }
    if (signed_op && blo > 0) {
        /* Divisor is an all-positive range: SV trunc-division. trunc(a/b) is
         * increasing in a (fixed b>0) and shrinks in |q| as b grows. The
         * extreme quotients are therefore:
         *   max = trunc(ahi / (ahi>=0 ? blo : bhi))
         *   min = trunc(alo / (alo< 0 ? blo : bhi))
         * No INT_MIN/-1 overflow is possible with a positive divisor. */
        int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
        int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
        int64_t rhi = sv_sdiv64(ahi, ahi >= 0 ? blo : bhi);
        int64_t rlo = sv_sdiv64(alo, alo <  0 ? blo : bhi);
        if ((res = ctx_tighten_lb64(ctx, rid, rlo)) != PROP_OK) return res;
        if ((res = ctx_tighten_ub64(ctx, rid, rhi)) != PROP_OK) return res;
        return PROP_OK;
    }
    if (!signed_op && blo == bhi && blo != 0) {
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
    } else if (!signed_op && blo > 0) {
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

    int signed_op = (ctx->vars[rid].flags & VAR_SIGNED) != 0;

    int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
    int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
    int64_t alo = var_lo64(ctx, &ctx->vars[aid]);
    int64_t ahi = var_hi64(ctx, &ctx->vars[aid]);
    int64_t blo = var_lo64(ctx, &ctx->vars[bid]);
    int64_t bhi = var_hi64(ctx, &ctx->vars[bid]);

    PropResult res;

    if (signed_op) {
        /* SV remainder: r takes the SIGN OF THE DIVIDEND a, |r| < |b|, valid
         * for negative divisors. Exact only when a (and b) are singletons;
         * otherwise keep a SOUND but weak r-range bound (sign of r follows
         * the sign range of a) and let search + the EQ relation finish. */
        /* Forward r-range bound. |r| <= max|b| - 1 over the divisor range
         * (a divisor of 0 is excluded by the constraint, contributing
         * nothing). The sign of r follows the sign range of the dividend a:
         *   a >= 0 throughout -> r >= 0;  a <= 0 throughout -> r <= 0. */
        int64_t ablo = blo < 0 ? -blo : blo;
        int64_t abhi = bhi < 0 ? -bhi : bhi;
        int64_t maxb = ablo > abhi ? ablo : abhi;
        if (maxb >= 1) {
            int64_t r_lo = (alo >= 0) ? 0 : -(maxb - 1);
            int64_t r_hi = (ahi <= 0) ? 0 :  (maxb - 1);
            if ((res = ctx_tighten_lb64(ctx, rid, r_lo)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, r_hi)) != PROP_OK) return res;
        }
        if (alo == ahi && blo == bhi && blo != 0) {
            int64_t val = sv_smod64(alo, blo);
            if ((res = ctx_tighten_lb64(ctx, rid, val)) != PROP_OK) return res;
            if ((res = ctx_tighten_ub64(ctx, rid, val)) != PROP_OK) return res;
        }
        return PROP_OK;
    }

    /* ---- Unsigned path (unchanged). ---- */

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

/* x in [los[0],his[0]] U ... U [los[n-1],his[n-1]].  Interval propagator:
 * narrow x's [lo,hi] to the hull of ranges overlapping it; conflict when none
 * overlaps (a gap-only domain, including a singleton that fell in a gap). The
 * gap *between* surviving ranges stays in the hull — membership of in-between
 * values is rejected only once they pin to a singleton, while the paired
 * add_dist keeps draws inside the ranges. Sign-aware (unsigned width-64 safe)
 * via the var_b_* comparators. */
static PropResult _fire_in_ranges_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect  *ws    = PROP_WS(self);
    uint32_t        xid   = ws->var_ids[0];
    InRanges_64_t  *iself = (InRanges_64_t *)self;
    int64_t        *los   = (int64_t *)((char *)self + sizeof(InRanges_64_t));
    int64_t        *his   = los + iself->n_ranges;
    uint32_t        n     = iself->n_ranges;
    const Variable *v     = &ctx->vars[xid];
    int64_t xlo = var_lo64(ctx, v), xhi = var_hi64(ctx, v);

    int     have   = 0;
    int64_t new_lo = 0, new_hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t lo = var_b_max(v, los[i], xlo);   /* overlap of range i with */
        int64_t hi = var_b_min(v, his[i], xhi);   /* the current domain      */
        if (var_b_gt(v, lo, hi)) continue;        /* range i doesn't overlap */
        if (!have || var_b_lt(v, lo, new_lo)) new_lo = lo;
        if (!have || var_b_gt(v, hi, new_hi)) new_hi = hi;
        have = 1;
    }
    if (!have) return PROP_CONFLICT;              /* no feasible range left   */

    PropResult r;
    if ((r = ctx_tighten_lb64(ctx, xid, new_lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, xid, new_hi)) != PROP_OK) return r;
    return PROP_OK;
}
uint32_t prop_add_in_ranges_64(SolveCtx *ctx, uint32_t x_id,
                                uint32_t n_ranges, const int64_t *los,
                                const int64_t *his, uint8_t priority) {
    uint32_t ids[1] = { x_id };
    uint32_t sz  = (uint32_t)(sizeof(InRanges_64_t) +
                              2u * n_ranges * sizeof(int64_t));
    uint32_t ref = _alloc_prop(ctx, _fire_in_ranges_64, priority, 1, ids, sz);
    if (ref == EXPR_NULL) return EXPR_NULL;
    InRanges_64_t *p = (InRanges_64_t *)zsp_pool_ptr(&ctx->pool, ref);
    p->n_ranges = n_ranges;
    int64_t *lo_dst = (int64_t *)((char *)p + sizeof(InRanges_64_t));
    int64_t *hi_dst = lo_dst + n_ranges;
    memcpy(lo_dst, los, n_ranges * sizeof(int64_t));
    memcpy(hi_dst, his, n_ranges * sizeof(int64_t));
    return ref;
}

/* 64-bit (tier-1) reification of `guard <-> (x <= y)`. Sign-aware sibling of
 * _fire_reification_32: reads bounds via var_lo64/var_hi64 (which resolve tier-1
 * WideBounds64) and tightens via the edge-guarded ctx_tighten_*64, so it is sound
 * for fields whose representable max exceeds INT32_MAX (e.g. unsigned width 32). */
static PropResult _fire_reification_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       gid = ws->var_ids[0];
    uint32_t       xid = ws->var_ids[1];
    uint32_t       yid = ws->var_ids[2];
    Variable      *g   = &ctx->vars[gid];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];
    int64_t xlo = var_lo64(ctx, x), xhi = var_hi64(ctx, x);
    int64_t ylo = var_lo64(ctx, y), yhi = var_hi64(ctx, y);

    /* guard=1 -> enforce x <= y */
    if (g->lo == 1) {
        PropResult r;
        if ((r = ctx_tighten_ub64(ctx, xid, yhi)) != PROP_OK) return r;
        if ((r = ctx_tighten_lb64(ctx, yid, xlo)) != PROP_OK) return r;
    }
    /* guard=0 -> x > y: tighten lb of x above y.hi (only when y.hi < x's max, so
     * y.hi+1 cannot overflow the representable range — at the max there is no
     * information to add and the search/model-validation stays authoritative). */
    if (g->hi == 0) {
        if (var_b_lt(x, yhi, var_repr_max(x))) {
            PropResult r;
            if ((r = ctx_tighten_lb64(ctx, xid, yhi + 1)) != PROP_OK) return r;
        }
    }
    /* Backward: if the domains prove x <= y (or x > y) unconditionally, pin g. */
    if (g->lo != g->hi) {
        if (!var_b_gt(x, xhi, ylo)) {            /* x.hi <= y.lo */
            PropResult r;
            if ((r = ctx_tighten_lb64(ctx, gid, 1)) != PROP_OK) return r;
        } else if (var_b_gt(x, xlo, yhi)) {      /* x.lo >  y.hi */
            PropResult r;
            if ((r = ctx_tighten_ub64(ctx, gid, 0)) != PROP_OK) return r;
        }
    }
    return PROP_OK;
}

uint32_t prop_add_reification_64(SolveCtx *ctx, uint32_t guard_id,
                                   uint32_t x_id, uint32_t y_id,
                                   uint8_t priority) {
    uint32_t ids[3] = { guard_id, x_id, y_id };
    return _alloc_prop(ctx, _fire_reification_64, priority, 3, ids,
                       sizeof(Reification_64_t));
}

/* 64-bit (tier-1) reification of `guard <-> (x == y)`. Sign-aware sibling of
 * _fire_reification_eq_32; the guard=1 intersection mirrors _fire_bounds_eq_64 and
 * the guard=0 exclusion mirrors _fire_bounds_ne_64. */
static PropResult _fire_reification_eq_64(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws  = PROP_WS(self);
    uint32_t       gid = ws->var_ids[0];
    uint32_t       xid = ws->var_ids[1];
    uint32_t       yid = ws->var_ids[2];
    Variable      *g   = &ctx->vars[gid];
    Variable      *x   = &ctx->vars[xid];
    Variable      *y   = &ctx->vars[yid];
    int64_t xlo = var_lo64(ctx, x), xhi = var_hi64(ctx, x);
    int64_t ylo = var_lo64(ctx, y), yhi = var_hi64(ctx, y);

    /* Forward: guard=1 -> enforce x == y (intersect bounds). */
    if (g->lo == 1) {
        int64_t lo = var_b_max(x, xlo, ylo);
        int64_t hi = var_b_min(x, xhi, yhi);
        if (var_b_gt(x, lo, hi)) return PROP_CONFLICT;
        PropResult r;
        if ((r = ctx_tighten_lb64(ctx, xid, lo)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub64(ctx, xid, hi)) != PROP_OK) return r;
        if ((r = ctx_tighten_lb64(ctx, yid, lo)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub64(ctx, yid, hi)) != PROP_OK) return r;
    }
    /* Forward: guard=0 -> x != y. Only propagates when one side is singleton. */
    if (g->hi == 0) {
        if (xlo == xhi && ylo == xlo && yhi == xhi) return PROP_CONFLICT;
        if (xlo == xhi) {
            int64_t v = xlo; PropResult r;
            if (ylo == v) { if ((r = ctx_tighten_lb64(ctx, yid, v + 1)) != PROP_OK) return r; }
            else if (yhi == v) { if ((r = ctx_tighten_ub64(ctx, yid, v - 1)) != PROP_OK) return r; }
        }
        if (ylo == yhi) {
            int64_t v = ylo; PropResult r;
            if (xlo == v) { if ((r = ctx_tighten_lb64(ctx, xid, v + 1)) != PROP_OK) return r; }
            else if (xhi == v) { if ((r = ctx_tighten_ub64(ctx, xid, v - 1)) != PROP_OK) return r; }
        }
    }
    /* Reload: the forward branch may have tightened x/y before the backward pins. */
    xlo = var_lo64(ctx, x); xhi = var_hi64(ctx, x);
    ylo = var_lo64(ctx, y); yhi = var_hi64(ctx, y);
    /* Backward: both singleton at the same value -> guard=1. */
    if (xlo == xhi && ylo == yhi && xlo == ylo) {
        PropResult r;
        if ((r = ctx_tighten_lb64(ctx, gid, 1)) != PROP_OK) return r;
    }
    /* Backward: disjoint domains -> guard=0. */
    if (var_b_gt(x, xlo, yhi) || var_b_gt(x, ylo, xhi)) {
        PropResult r;
        if ((r = ctx_tighten_ub64(ctx, gid, 0)) != PROP_OK) return r;
    }
    return PROP_OK;
}

uint32_t prop_add_reification_eq_64(SolveCtx *ctx, uint32_t guard_id,
                                     uint32_t x_id, uint32_t y_id,
                                     uint8_t priority) {
    uint32_t ids[3] = { guard_id, x_id, y_id };
    return _alloc_prop(ctx, _fire_reification_eq_64, priority, 3, ids,
                       sizeof(ReificationEq_64_t));
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
    /* Backward bounds-consistency when the slice result is fixed (BUG-2). */
    {
        int64_t rlo = var_lo64(ctx, &ctx->vars[rid]);
        int64_t rhi = var_hi64(ctx, &ctx->vars[rid]);
        if (rlo == rhi) {
            r = _bit_slice_backward(ctx, aid, bself->lo_bit, bself->hi_bit,
                                    (uint64_t)rlo, alo, ahi);
            if (r != PROP_OK) return r;
        }
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
    /* bvnot is antitone. For a SIGNED domain it is the two's-complement
     * identity ~a = -a-1, whose value stays in the signed w-bit range, so we
     * map directly in signed value space (no masking — masking would store the
     * unsigned bit pattern, e.g. ~0 as 2^w-1 instead of -1, which is out of a
     * signed var's range and never reaches fixpoint). For an UNSIGNED domain,
     * ~a = mask - a, mapped via the masked complement. */
    int is_signed = (rv->flags & VAR_SIGNED) || (av->flags & VAR_SIGNED);

    int64_t alo = var_lo64(ctx, av);
    int64_t ahi = var_hi64(ctx, av);

    PropResult res;

    /* Forward: r = ~a, ordering reversed. */
    int64_t new_rlo, new_rhi;
    if (is_signed) {
        new_rlo = -ahi - 1;
        new_rhi = -alo - 1;
    } else {
        new_rlo = (int64_t)(mask & (uint64_t)~ahi);
        new_rhi = (int64_t)(mask & (uint64_t)~alo);
    }
    if ((res = ctx_tighten_lb64(ctx, rid, new_rlo)) != PROP_OK) return res;
    if ((res = ctx_tighten_ub64(ctx, rid, new_rhi)) != PROP_OK) return res;

    /* Backward: a = ~r. */
    int64_t rlo = var_lo64(ctx, rv);
    int64_t rhi = var_hi64(ctx, rv);
    int64_t new_alo, new_ahi;
    if (is_signed) {
        new_alo = -rhi - 1;
        new_ahi = -rlo - 1;
    } else {
        new_alo = (int64_t)(mask & (uint64_t)~rhi);
        new_ahi = (int64_t)(mask & (uint64_t)~rlo);
    }
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

    /* Clamp the shift amount to the operand width: a logical right shift of a
     * w-bit value by >= w bits yields 0. Clamping *both* bounds to w makes
     * `a >> shift` produce 0 for any shift >= w (and avoids the C undefined
     * behaviour of shifting by >= 64). The previous code clamped only bhi, and
     * to 63 not w, so e.g. b==128 (128 & 63 == 0 on x86) was treated as a
     * shift-by-0 and returned ~a instead of 0. */
    uint16_t aw = ctx->vars[aid].width;
    int64_t maxsh = (aw != 0 && aw <= 63) ? (int64_t)aw : 63;
    if (blo < 0) blo = 0;
    if (bhi < 0) bhi = 0;
    if (blo > maxsh) blo = maxsh;
    if (bhi > maxsh) bhi = maxsh;

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

    /* Read tier-aware 64-bit bounds up front. A width-32 *unsigned* variable is
     * tier-1 -- its true bounds live in a pooled WideBounds64 and the raw
     * Variable.lo/.hi int32 fields are pinned to 0 (see _init_tier1). The old
     * code read those int32 fields directly, so every unsigned-32 var looked
     * like the singleton [0,0]: the pigeonhole check then saw a union of size 1
     * and fabricated PROP_CONFLICT -> spurious UNSAT for the default PSS int.
     * var_lo64/var_hi64 resolve the real bounds for every tier; L[]/H[] cache
     * them (n <= 16) and are refreshed after each tighten. Overflow guards on
     * the span/Hall-width arithmetic ensure a very wide (width-64 unsigned)
     * domain can never wrap negative and manufacture a false conflict; that
     * edge stays sound (it may under-propagate, never mis-report). */
    int64_t L[MAX_ALLDIFF_VARS], H[MAX_ALLDIFF_VARS];
    for (uint32_t i = 0; i < n; i++) {
        Variable *vi = &ctx->vars[ad->var_ids[i]];
        L[i] = var_lo64(ctx, vi);
        H[i] = var_hi64(ctx, vi);
    }

    /* --- Singleton exclusion (most common propagation) --- */
    for (uint32_t i = 0; i < n; i++) {
        if (L[i] != H[i]) continue;
        int64_t val = L[i];
        for (uint32_t j = 0; j < n; j++) {
            if (j == i) continue;
            uint32_t vj_id = ad->var_ids[j];
            if (L[j] == val) {
                if (ctx_tighten_lb64(ctx, vj_id, val + 1) == PROP_CONFLICT)
                    return PROP_CONFLICT;
                L[j] = var_lo64(ctx, &ctx->vars[vj_id]);
            }
            if (H[j] == val) {
                if (ctx_tighten_ub64(ctx, vj_id, val - 1) == PROP_CONFLICT)
                    return PROP_CONFLICT;
                H[j] = var_hi64(ctx, &ctx->vars[vj_id]);
            }
        }
    }

    /* --- Pigeonhole check --- */
    /* Find global lo/hi across all variables */
    int64_t glo = L[0], ghi = H[0];
    for (uint32_t i = 1; i < n; i++) {
        if (L[i] < glo) glo = L[i];
        if (H[i] > ghi) ghi = H[i];
    }
    /* Domain union cardinality upper bound (#values - 1 = ghi - glo). The
     * `span >= 0` guard skips the test when the difference wrapped (a domain
     * too wide to ever pigeonhole n <= 16 vars anyway). */
    int64_t span = ghi - glo;
    if (span >= 0 && span + 1 < (int64_t)n) return PROP_CONFLICT;

    /* --- Hall interval detection via sweep --- */
    /* Sort variables by lo bound (insertion sort, n <= 16) */
    uint32_t sorted[MAX_ALLDIFF_VARS];
    for (uint32_t i = 0; i < n; i++) sorted[i] = i;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = sorted[i];
        int64_t key_lo = L[key];
        uint32_t j = i;
        while (j > 0 && L[sorted[j - 1]] > key_lo) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }

    /* Sweep: for each starting position, count how many domains are
       fully contained in [start_lo, end_hi].  If count > width, tighten. */
    for (uint32_t start = 0; start < n; start++) {
        int64_t lo_s = L[sorted[start]];
        int64_t max_hi = lo_s;
        uint32_t count = 0;
        for (uint32_t end = start; end < n; end++) {
            uint32_t si = sorted[end];
            if (L[si] >= lo_s) {
                if (H[si] > max_hi) max_hi = H[si];
                count++;
            }
            int64_t width = max_hi - lo_s + 1;
            if (width < 0) continue;   /* wrapped: too wide to pigeonhole */
            if ((int64_t)count > width) return PROP_CONFLICT;
            /* If count == width (Hall interval), variables outside must
               avoid [lo_s, max_hi] */
            if ((int64_t)count == width && count >= 2) {
                for (uint32_t k = 0; k < n; k++) {
                    uint32_t kid = ad->var_ids[k];
                    /* Skip variables that are part of this Hall interval */
                    int in_hall = (L[k] >= lo_s && H[k] <= max_hi);
                    if (in_hall) continue;
                    /* If vk overlaps the Hall interval, tighten */
                    if (L[k] >= lo_s && L[k] <= max_hi) {
                        if (ctx_tighten_lb64(ctx, kid, max_hi + 1) == PROP_CONFLICT)
                            return PROP_CONFLICT;
                        L[k] = var_lo64(ctx, &ctx->vars[kid]);
                    }
                    if (H[k] >= lo_s && H[k] <= max_hi) {
                        if (ctx_tighten_ub64(ctx, kid, lo_s - 1) == PROP_CONFLICT)
                            return PROP_CONFLICT;
                        H[k] = var_hi64(ctx, &ctx->vars[kid]);
                    }
                }
            }
        }
    }

    /* --- Entailment check --- */
    int all_assigned = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (L[i] != H[i]) { all_assigned = 0; break; }
    }
    if (all_assigned) {
        /* Verify all distinct (should be guaranteed by propagation) */
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                if (L[i] == L[j]) return PROP_CONFLICT;
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

static int _popcount64(uint64_t v) {
    int c = 0;
    while (v) { v &= v - 1; c++; }
    return c;
}

/* result == popcount(operand).
 *
 * Tier-aware: reads operand/result bounds via var_lo64/var_hi64 and tightens
 * via ctx_tighten_*_64, so it is correct for tier-0 AND tier-1 (33-64 bit, incl.
 * 32-bit unsigned) vars. The earlier _32 version read Variable.lo/hi directly,
 * which is garbage for tier-1 vars -> silent wrong models (BUG-1).
 *
 * Soundness rests on the forward-singleton rule: once the operand is fully
 * assigned, result is tightened to the exact popcount, so any inconsistent leaf
 * conflicts. The backward interval rules only prune; to stay clear of int64 sign
 * issues at the top of the range they are applied only for width <= 62 (the
 * exact all-ones and >=63-bit cases still terminate soundly via the forward
 * rule, just with less pruning). */
static PropResult _fire_countones_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0];  /* result */
    uint32_t xid = ws->var_ids[1];  /* operand */
    Variable *rv = &ctx->vars[rid];
    Variable *xv = &ctx->vars[xid];

    uint16_t width = xv->width;
    if (width > 64) width = 64;
    uint64_t mask = (width < 64) ? (((uint64_t)1 << width) - 1) : ~(uint64_t)0;
    int bw_safe = (width <= 62);  /* backward shifts stay in positive int64 */

    int64_t xlo = var_lo64(ctx, xv);
    int64_t xhi = var_hi64(ctx, xv);
    int64_t rlo = var_lo64(ctx, rv);
    int64_t rhi = var_hi64(ctx, rv);

    PropResult r;

    /* Forward: bound result from operand's domain */
    if (xlo == xhi) {
        /* Operand is singleton: result is exact popcount */
        int pc = _popcount64((uint64_t)xlo & mask);
        if ((r = ctx_tighten_lb64(ctx, rid, pc)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub64(ctx, rid, pc)) != PROP_OK) return r;
    } else {
        /* Coarse bounds: min popcount >= popcount(bits that must be 1),
         * max popcount <= width */
        uint64_t must_1 = (uint64_t)xlo & (uint64_t)xhi & mask;  /* approximate */
        int min_pc = _popcount64(must_1);
        if ((r = ctx_tighten_lb64(ctx, rid, min_pc)) != PROP_OK) return r;
        if ((r = ctx_tighten_ub64(ctx, rid, (int64_t)width)) != PROP_OK) return r;
    }

    /* Backward: bound operand from result */
    if (rlo == rhi) {
        int64_t k = rlo;
        if (k < 0 || k > (int64_t)width) return PROP_CONFLICT;
        if (k == 0) {
            if ((r = ctx_tighten_lb64(ctx, xid, 0)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub64(ctx, xid, 0)) != PROP_OK) return r;
        } else if (k == (int64_t)width && bw_safe) {
            if ((r = ctx_tighten_lb64(ctx, xid, (int64_t)mask)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub64(ctx, xid, (int64_t)mask)) != PROP_OK) return r;
        } else if (k > 0 && k < (int64_t)width && bw_safe) {
            /* min x with popcount k: lowest k bits set = (1<<k)-1 */
            uint64_t min_x = ((uint64_t)1 << k) - 1;
            /* max x with popcount k: highest k bits set (within width) */
            uint64_t max_x = (((uint64_t)1 << k) - 1) << (width - (uint16_t)k);
            if ((r = ctx_tighten_lb64(ctx, xid, (int64_t)min_x)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub64(ctx, xid, (int64_t)max_x)) != PROP_OK) return r;
        }
    } else {
        /* result_lo > 0 means operand cannot be 0 */
        if (rlo > 0) {
            if ((r = ctx_tighten_lb64(ctx, xid, 1)) != PROP_OK) return r;
        }
        /* result_hi < width means not all bits can be set */
        if (rhi >= 0 && rhi < (int64_t)width && bw_safe) {
            /* max x with at most rhi bits set: top rhi bits within width */
            uint64_t max_bits = (uint64_t)rhi;
            uint64_t max_x = (max_bits == 0)
                ? 0
                : ((((uint64_t)1 << max_bits) - 1) << (width - (uint16_t)max_bits));
            if ((r = ctx_tighten_ub64(ctx, xid, (int64_t)max_x)) != PROP_OK) return r;
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

static int32_t _clog2_64(uint64_t v) {
    if (v <= 1) return 0;
    int32_t r = 0;
    uint64_t t = v - 1;
    while (t) { r++; t >>= 1; }
    return r;
}

/* result == ceil(log2(operand)); operand is forced > 0.
 *
 * Tier-aware (see _fire_countones_32 for the BUG-1 rationale). The backward
 * interval rule is applied only for k < 62 so `1 << k` stays in positive int64;
 * wider results still converge soundly via the forward monotone/singleton
 * rules. */
static PropResult _fire_clog2_32(Propagator *self, SolveCtx *ctx) {
    PropWatchSect *ws = PROP_WS(self);
    uint32_t rid = ws->var_ids[0];  /* result */
    uint32_t xid = ws->var_ids[1];  /* operand */
    Variable *rv = &ctx->vars[rid];
    Variable *xv = &ctx->vars[xid];

    PropResult r;

    /* Guard: operand must be > 0 for clog2 to be defined */
    if ((r = ctx_tighten_lb64(ctx, xid, 1)) != PROP_OK) return r;

    /* Forward: clog2 is monotonically non-decreasing */
    int32_t clog_lo = _clog2_64((uint64_t)var_lo64(ctx, xv));
    int32_t clog_hi = _clog2_64((uint64_t)var_hi64(ctx, xv));
    if ((r = ctx_tighten_lb64(ctx, rid, clog_lo)) != PROP_OK) return r;
    if ((r = ctx_tighten_ub64(ctx, rid, clog_hi)) != PROP_OK) return r;

    /* Backward: if result is singleton k, tighten operand range */
    int64_t rlo = var_lo64(ctx, rv);
    int64_t rhi = var_hi64(ctx, rv);
    if (rlo == rhi) {
        int64_t k = rlo;
        if (k == 0) {
            /* clog2(x) == 0 -> x == 1 */
            if ((r = ctx_tighten_lb64(ctx, xid, 1)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub64(ctx, xid, 1)) != PROP_OK) return r;
        } else if (k > 0 && k < 62) {
            /* clog2(x) == k -> x in [(1 << (k-1)) + 1, 1 << k] */
            int64_t x_lo = ((int64_t)1 << (k - 1)) + 1;
            int64_t x_hi = ((int64_t)1 << k);
            if ((r = ctx_tighten_lb64(ctx, xid, x_lo)) != PROP_OK) return r;
            if ((r = ctx_tighten_ub64(ctx, xid, x_hi)) != PROP_OK) return r;
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
    FN_NAME(_fire_implication_64);
    FN_NAME(_fire_reification_32);
    FN_NAME(_fire_reification_eq_32);
    FN_NAME(_fire_reification_64);
    FN_NAME(_fire_reification_eq_64);
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
    FN_NAME(_fire_in_ranges_64);
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
        { _fire_implication_64,     explain_implication },
        { _fire_reification_32,     explain_reification },
        { _fire_reification_eq_32,  explain_reification_eq },
        { _fire_reification_64,     explain_reification },
        { _fire_reification_eq_64,  explain_reification_eq },
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
