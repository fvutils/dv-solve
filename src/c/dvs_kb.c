/* Known bits in the CDCL search. See dvs_kb.h. */
#include <stdlib.h>
#include <string.h>
#include "dvs_kb.h"
#include "dvs_ctx.h"
#include "dvs_trail.h"

PropResult ctx_tighten_lb64(dvs_ctx_t *ctx, uint32_t var_id, int64_t new_lb);
PropResult ctx_tighten_ub64(dvs_ctx_t *ctx, uint32_t var_id, int64_t new_ub);
void ctx_wake_var(dvs_ctx_t *ctx, uint32_t var_id);

static uint64_t _wm(uint32_t w) { return w >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1); }

/* The sign bit for a signed variable: flipping it maps pattern order to
 * value order. */
static uint64_t _sflip(const Variable *v) {
    return (v->flags & VAR_SIGNED) ? (uint64_t)1 << (v->width - 1) : 0;
}

static int _eligible(const Variable *v) {
    return v->width >= 1 && v->width <= 64 && !VAR_IS_TIER2(v->flags);
}

static uint64_t _key(const Variable *v, int64_t x) {
    return ((uint64_t)x & _wm(v->width)) ^ _sflip(v);
}

static int64_t _unkey(const Variable *v, uint64_t k) {
    uint64_t p = k ^ _sflip(v), s = _sflip(v);
    if (s && (p & s)) p |= ~_wm(v->width);
    return (int64_t)p;
}

int kb_enabled(dvs_ctx_t *ctx) {
    if (ctx->kb_state > 0 && ctx->kb_cap >= ctx->n_vars) return 1;
    if (ctx->kb_state == 0) {
        const char *e = getenv("DVS_KB");
        ctx->kb_state = (e && e[0] == '0') ? -1 : 1;
    }
    if (ctx->kb_state < 0) return 0;
    if (ctx->kb_cap < ctx->n_vars) {
        uint32_t cap = ctx->n_vars + ctx->n_vars / 2u + 16u;
        KbRec **n = (KbRec **)DVS_RESIZE(ctx->alloc, ctx->kb,
            (size_t)ctx->kb_cap * sizeof(KbRec *), (size_t)cap * sizeof(KbRec *));
        if (!n) return 0;
        memset(n + ctx->kb_cap, 0, (size_t)(cap - ctx->kb_cap) * sizeof(KbRec *));
        ctx->kb = n;
        ctx->kb_cap = cap;
    }
    return 1;
}

void kb_clear(dvs_ctx_t *ctx) {
    if (ctx->kb) memset(ctx->kb, 0, (size_t)ctx->kb_cap * sizeof(KbRec *));
}

/* The stored tnum: the record, else the static bits. */
static void _stored(const dvs_ctx_t *ctx, uint32_t v, uint64_t *m, uint64_t *val) {
    const KbRec *r = (ctx->kb && v < ctx->kb_cap) ? ctx->kb[v] : NULL;
    if (r) { *m = r->mask; *val = r->val; return; }
    if (v < ctx->sk_n && ctx->sk_mask) {
        *m = ctx->sk_mask[v] & _wm(ctx->vars[v].width);
        *val = ctx->sk_val[v] & *m;
        return;
    }
    *m = *val = 0;
}

/* The bits every value in [klo, khi] (value order) shares: the common
 * prefix. */
static uint64_t _prefix(uint64_t klo, uint64_t khi, uint64_t wm) {
    uint64_t d = klo ^ khi;
    if (!d) return wm;
    int h = 63 - __builtin_clzll(d);
    return h == 63 ? 0 : ~(((uint64_t)1 << (h + 1)) - 1) & wm;
}

/* One variable as kb_bitwise reads it: bounds as keys (value order), the
 * stored bits, and the known bits (stored plus the bounds' common prefix). */
typedef struct {
    const Variable *v;
    uint64_t klo, khi, sm, sv, one, zero;
} KbView;

/* 0 if the stored bits and the bounds disagree (an empty domain). */
static int _view(const dvs_ctx_t *ctx, uint32_t id, uint64_t wm, KbView *x) {
    const Variable *v = x->v = &ctx->vars[id];
    _stored(ctx, id, &x->sm, &x->sv);
    x->klo = _key(v, var_lo64(ctx, v));
    x->khi = _key(v, var_hi64(ctx, v));
    uint64_t pm = _prefix(x->klo, x->khi, wm);
    uint64_t pv = (x->klo ^ _sflip(v)) & pm;
    if ((x->sv ^ pv) & x->sm & pm) return 0;
    x->one  = (x->sv & x->sm) | pv;
    x->zero = (~x->sv & x->sm) | (~pv & pm);
    return 1;
}

/* Smallest x >= z (all within wm) whose bits in km equal kv. */
static int _up(uint64_t z, uint64_t km, uint64_t kv, uint64_t wm, uint64_t *out) {
    uint64_t d = (z ^ kv) & km;
    if (!d) { *out = z; return 1; }
    int h = 63 - __builtin_clzll(d);
    uint64_t bh = (uint64_t)1 << h, below = bh - 1;
    if (kv & bh) {   /* z has 0 where a 1 is needed: set it, minimum below */
        *out = (z & ~below) | bh | (kv & below);
        return 1;
    }
    /* z has 1 where a 0 is needed: carry into the lowest free 0 above h. */
    uint64_t cand = ~(below | bh) & wm & ~km & ~z;
    if (!cand) return 0;
    uint64_t bj = cand & -cand, lowj = bj - 1;
    *out = (z & ~lowj) | bj | (kv & lowj);
    return 1;
}

static int _down(uint64_t z, uint64_t km, uint64_t kv, uint64_t wm, uint64_t *out) {
    uint64_t y;
    if (!_up(~z & wm, km, ~kv & km, wm, &y)) return 0;
    *out = ~y & wm;
    return 1;
}

static int _stronger(const dvs_ctx_t *ctx, Literal a, Literal b) {
    const Variable *v = &ctx->vars[a.var_id];
    return a.is_lb ? var_b_gt(v, a.bound, b.bound) : var_b_lt(v, a.bound, b.bound);
}

static Literal _lit(uint32_t var, uint8_t is_lb, int64_t bound) {
    Literal l;
    memset(&l, 0, sizeof l);
    l.var_id = var; l.is_lb = is_lb; l.bound = bound;
    return l;
}

/* Add l to buf[*n] (cap), one literal per (variable, direction), keeping
 * the stronger. 0 when full. */
static int _add(const dvs_ctx_t *ctx, Literal *buf, uint32_t *n, uint32_t cap, Literal l) {
    for (uint32_t i = 0; i < *n; i++) {
        if (buf[i].var_id == l.var_id && buf[i].is_lb == l.is_lb) {
            if (_stronger(ctx, l, buf[i])) buf[i] = l;
            return 1;
        }
    }
    if (*n >= cap) return 0;
    buf[(*n)++] = l;
    return 1;
}

int kb_lits(const dvs_ctx_t *ctx, uint32_t var, const Literal **lits, uint32_t *n) {
    const KbRec *r = (ctx->kb && var < ctx->kb_cap) ? ctx->kb[var] : NULL;
    *lits = NULL; *n = 0;
    if (!r) return 0;
    if (r->n == KB_NOEXPL) return -1;
    *lits = r->lits; *n = r->n;
    return 0;
}

/* The bounds of vars[n] and their records' literals, into buf (cap). */
static int _ground(const dvs_ctx_t *ctx, const uint32_t *vars, uint32_t n,
                   Literal *buf, uint32_t *nb, uint32_t cap) {
    *nb = 0;
    for (uint32_t i = 0; i < n; i++) {
        const Variable *v = &ctx->vars[vars[i]];
        int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
        if (lo != var_repr_min(v) && !_add(ctx, buf, nb, cap, _lit(vars[i], 1, lo))) return -1;
        if (hi != var_repr_max(v) && !_add(ctx, buf, nb, cap, _lit(vars[i], 0, hi))) return -1;
        const Literal *rl; uint32_t rn;
        if (kb_lits(ctx, vars[i], &rl, &rn) != 0) return -1;
        for (uint32_t j = 0; j < rn; j++)
            if (!_add(ctx, buf, nb, cap, rl[j])) return -1;
    }
    return 0;
}

int kb_explain(dvs_ctx_t *ctx, const uint32_t *vars, uint32_t n, Explanation *out) {
    uint32_t nb;
    if (_ground(ctx, vars, n, out->lits, &nb, MAX_EXPLAIN_LITS) != 0) return -1;
    out->n_lits = nb;
    return 0;
}

/* Store x's tnum (one/zero) as a new record grounded on vars[n]. */
static int _store(dvs_ctx_t *ctx, uint32_t x, uint64_t one, uint64_t zero,
                  const uint32_t *vars, uint32_t n) {
    Literal buf[KB_MAX_LITS];
    uint32_t nb = 0;
    int ok = _ground(ctx, vars, n, buf, &nb, KB_MAX_LITS) == 0;
    size_t sz = sizeof(KbRec) + (ok ? nb : 0) * sizeof(Literal);
    KbRec *r = (KbRec *)dvs_stack_alloc(ctx->dynamic, sz, _Alignof(KbRec));
    if (!r) return -1;
    r->prev = ctx->kb[x];
    r->mask = one | zero;
    r->val  = one;
    r->n    = ok ? nb : KB_NOEXPL;
    r->_pad = 0;
    if (ok) memcpy(r->lits, buf, nb * sizeof(Literal));
    if (trail_record_bits(ctx, x, r) != 0) return -1;
    if (ctx->watcher_heads) ctx_wake_var(ctx, x);
    return 0;
}

/* Tnum transfer for r = a op b (index 0, 1, 2; b unused for KB_NOT):
 * one/zero masks of the known bits, grown in place. One pass reaches the
 * fixpoint: what a backward rule adds is already implied forward. */
static void _transfer(KbOp op, uint64_t o[3], uint64_t z[3]) {
    switch (op) {
    case KB_AND:
        o[0] |= o[1] & o[2];  z[0] |= z[1] | z[2];
        o[1] |= o[0];         o[2] |= o[0];
        z[1] |= z[0] & o[2];  z[2] |= z[0] & o[1];
        break;
    case KB_OR:
        z[0] |= z[1] & z[2];  o[0] |= o[1] | o[2];
        z[1] |= z[0];         z[2] |= z[0];
        o[1] |= o[0] & z[2];  o[2] |= o[0] & z[1];
        break;
    case KB_XOR: {
        /* Any two known bits give the third. */
        uint64_t m0 = o[0] | z[0], m1 = o[1] | z[1], m2 = o[2] | z[2];
        uint64_t k0 = m1 & m2, k1 = m0 & m2, k2 = m0 & m1;
        uint64_t v0 = (o[1] ^ o[2]) & k0, v1 = (o[0] ^ o[2]) & k1, v2 = (o[0] ^ o[1]) & k2;
        o[0] |= v0; z[0] |= k0 & ~v0;
        o[1] |= v1; z[1] |= k1 & ~v1;
        o[2] |= v2; z[2] |= k2 & ~v2;
        break;
    }
    case KB_NOT:
        o[0] |= z[1]; z[0] |= o[1];
        o[1] |= z[0]; z[1] |= o[0];
        break;
    }
}

PropResult kb_bitwise(dvs_ctx_t *ctx, KbOp op, uint32_t r, uint32_t a, uint32_t b) {
    if (!kb_enabled(ctx)) return PROP_OK;
    uint32_t ids[3] = { r, a, b };
    uint32_t n = op == KB_NOT ? 2u : 3u;
    uint32_t w = ctx->vars[r].width;
    for (uint32_t i = 0; i < n; i++) {
        if (!_eligible(&ctx->vars[ids[i]]) || ctx->vars[ids[i]].width != w) return PROP_OK;
        for (uint32_t j = 0; j < i; j++)
            if (ids[i] == ids[j]) return PROP_OK;
    }
    uint64_t wm = _wm(w), o[3] = {0}, z[3] = {0}, stored = 0;
    KbView x[3];
    for (uint32_t i = 0; i < n; i++) {
        if (!_view(ctx, ids[i], wm, &x[i])) return PROP_CONFLICT;
        o[i] = x[i].one; z[i] = x[i].zero;
        stored |= x[i].sm;
    }
    _transfer(op, o, z);
    uint64_t grew = 0;
    for (uint32_t i = 0; i < n; i++) {
        o[i] &= wm; z[i] &= wm;
        if (o[i] & z[i]) return PROP_CONFLICT;
        grew |= (o[i] | z[i]) & ~(x[i].one | x[i].zero);
    }
    /* Nothing new and nothing stored: the bounds already match the bits. */
    if (!grew && !stored) return PROP_OK;
    /* Round each variable's bounds to its bits, and store the bits the
     * rounded bounds do not imply (a 1-bit variable never needs a record).
     * A rounding is explained like any narrowing of this propagator (by
     * its variables' bounds and records, kb_explain), so it needs no record
     * of its own; out of stack, the record is skipped. */
    uint64_t nlo[3], nhi[3];
    for (uint32_t i = 0; i < n; i++) {
        uint64_t km = o[i] | z[i], kv = (o[i] ^ _sflip(x[i].v)) & km;
        if (!_up(x[i].klo, km, kv, wm, &nlo[i]) || !_down(x[i].khi, km, kv, wm, &nhi[i])
                || nlo[i] > nhi[i])
            return PROP_CONFLICT;
        if ((km & ~(x[i].one | x[i].zero)) && (km & ~_prefix(nlo[i], nhi[i], wm) & ~x[i].sm))
            (void)_store(ctx, ids[i], o[i], z[i], ids, n);
    }
    for (uint32_t i = 0; i < n; i++) {
        PropResult res;
        if (nlo[i] != x[i].klo
                && (res = ctx_tighten_lb64(ctx, ids[i], _unkey(x[i].v, nlo[i]))) != PROP_OK)
            return res;
        if (nhi[i] != x[i].khi
                && (res = ctx_tighten_ub64(ctx, ids[i], _unkey(x[i].v, nhi[i]))) != PROP_OK)
            return res;
    }
    return PROP_OK;
}
