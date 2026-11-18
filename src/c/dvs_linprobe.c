/*
 * The weighted value pick's probe on linear components -- see dvs_linprobe.h.
 */
#include <stdint.h>
#include <string.h>
#include "dvs_linprobe.h"
#include "dvs_ctx.h"
#include "dvs_propagator.h"

#define LP_NONE UINT32_MAX

/* A row; a LINROW_SUM is v0 = the sum of leaf[v1 .. v1+v2). */
typedef struct { uint8_t k, w; uint32_t v[3]; } LinRow;
#define LINROW_SUM 5u

/* The index of the problem's linear components, one block from ctx->alloc.
 * Rows are grouped by component; scratch bounds are by variable. */
typedef struct {
    size_t    bytes;
    uint32_t  epoch, restore_gen, n_props, n_vars;
    uint32_t  n_comps, n_rows;
    uint32_t  undo_comp;     /* written in place, LP_NONE: nothing      */
    uint32_t *comp;          /* [n_vars] component, LP_NONE: not linear */
    uint32_t *crow;          /* [n_comps + 1] rows of each component    */
    uint32_t *vrow;          /* [n_vars + 1] into vrows                 */
    uint32_t *vrows;         /* each variable's rows                    */
    LinRow   *rows;          /* [n_rows]                                */
    uint32_t *leaf;          /* [2 * n_rows] the terms of the sum rows  */
    uint64_t *lo, *hi;       /* [n_vars] propagated bounds              */
    uint64_t *slo, *shi;     /* [n_vars] bounds before the probe        */
    uint32_t  gen;           /* the probe; seen[i] == gen: i is loaded  */
    uint32_t *seen;          /* [n_vars]                                */
    uint32_t *chg, n_chg;    /* [n_vars] the variables the probe narrowed */
    uint32_t *q;             /* [n_rows + 1] ring of queued rows        */
    uint8_t  *inq;           /* [n_rows]                                */
} LinIx;

void dvs_linprobe_free(dvs_ctx_t *ctx) {
    LinIx *ix = (LinIx *)ctx->linprobe;
    if (ix) dvs_ctx_free(ctx, ix, ix->bytes);
    ctx->linprobe = NULL;
}

static uint64_t _checks;
void dvs_linprobe_note_check(void) { __atomic_fetch_add(&_checks, 1, __ATOMIC_RELAXED); }
uint64_t dvs_linprobe_checks(void) { return __atomic_load_n(&_checks, __ATOMIC_RELAXED); }

static uint32_t _find(uint32_t *rep, uint32_t v) {
    while (rep[v] != v) { rep[v] = rep[rep[v]]; v = rep[v]; }
    return v;
}

static uint32_t _n_props(const dvs_ctx_t *ctx) {
    return ctx->n_props < ctx->n_prop_refs_capacity ? ctx->n_props : ctx->n_prop_refs_capacity;
}

/* Propagator k, or NULL for none and for the negative-cycle check, which
 * adds nothing a settled propagation needs (dvs_diffcycle.h): a crawl round
 * a cycle exhausts the row budget instead. */
static const Propagator *_prop(const dvs_ctx_t *ctx, uint32_t k) {
    uint32_t ref = ctx->prop_refs[k];
    if (ref == EXPR_NULL || ref == ctx->diffcycle_ref) return NULL;
    return (const Propagator *)dvs_spool_ptr(&ctx->pool, ref);
}

static int _guarded(const dvs_ctx_t *ctx, uint32_t k) {
    return ctx->prop_guard_vars && ctx->prop_guard_vars[k] != EXPR_NULL;
}

static uint32_t _row_nvars(const LinRow *R) {
    return R->k == LINROW_SUM ? R->v[2] + 1u : R->k == LINROW_ADD ? 3u : 2u;
}

/* Variable j of row R: a sum's result, then its terms. */
static uint32_t _row_var(const LinIx *ix, const LinRow *R, uint32_t j) {
    return R->k == LINROW_SUM && j ? ix->leaf[R->v[1] + j - 1u] : R->v[j];
}

/* Fold each chain of adds into one sum row: `s1 = a + b, s2 = s1 + c` is
 * `s2 = a + b + c` when s1 is an aux variable in no other row. A sum's
 * interval is exactly its terms' (they are integer intervals), so the sum
 * row narrows the terms and the result as the chain does, without walking
 * the partial sums: a chain of n adds costs n row evaluations per narrowed
 * term. The partial sums are then not narrowed by a probe; nothing reads
 * them (the box counts no aux variable). A component is folded whole or
 * not at all: not when a term repeats, or a partial sum is not reached from
 * a sum's result. */
static void _fold(dvs_ctx_t *ctx, LinIx *ix) {
    const uint32_t nv = ix->n_vars, nr = ix->n_rows;
    const size_t tsz = (size_t)nv * 4u * sizeof(uint32_t) + (size_t)nr * sizeof(LinRow);
    uint32_t *def = (uint32_t *)dvs_ctx_resize(ctx, NULL, 0, tsz);
    if (!def) return;
    uint32_t *occ = def + nv, *term = occ + nv, *mark = term + nv;  /* mark: a row's terms */
    LinRow *out = (LinRow *)(mark + nv);
    for (uint32_t i = 0; i < nv; i++) {
        def[i] = mark[i] = LP_NONE;
        occ[i] = term[i] = 0;
    }
    for (uint32_t r = 0; r < nr; r++) {
        const LinRow *R = &ix->rows[r];
        for (uint32_t j = 0; j < _row_nvars(R); j++) occ[R->v[j]]++;
        if (R->k != LINROW_ADD) continue;
        def[R->v[0]] = def[R->v[0]] == LP_NONE ? r : LP_NONE - 1u;
        term[R->v[1]]++;
        term[R->v[2]]++;
    }
    /* A partial sum: aux, the result of one add and a term of one other,
     * and in no other row. */
#define LP_PARTIAL(i) ((ctx->vars[i].flags & VAR_AUX) && occ[i] == 2u && term[i] == 1u \
                       && def[i] < nr)
    uint32_t nout = 0, nleaf = 0;
    for (uint32_t c = 0; c < ix->n_comps; c++) {
        const uint32_t r0 = ix->crow[c], r1 = ix->crow[c + 1], out0 = nout, leaf0 = nleaf;
        uint32_t partials = 0, folded = 0;
        int ok = 1;
        for (uint32_t r = r0; r < r1 && ok; r++) {
            const LinRow *R = &ix->rows[r];
            if (R->k == LINROW_ADD && LP_PARTIAL(R->v[0])) { partials++; continue; }
            if (R->k != LINROW_ADD || (!LP_PARTIAL(R->v[1]) && !LP_PARTIAL(R->v[2]))) {
                out[nout++] = *R;
                continue;
            }
            /* Collect the terms below R, depth first; folded rows' widths. */
            LinRow S = { LINROW_SUM, R->w, { R->v[0], nleaf, 0 } };
            uint32_t st[64], top = 0;
            st[top++] = R->v[2];
            st[top++] = R->v[1];
            while (top && ok) {
                const uint32_t u = st[--top];
                if (LP_PARTIAL(u)) {
                    const LinRow *D = &ix->rows[def[u]];
                    if (top + 2u > 64u) { ok = 0; break; }
                    if (D->w < S.w) S.w = D->w;
                    st[top++] = D->v[2];
                    st[top++] = D->v[1];
                    folded++;
                } else if (mark[u] == r) {
                    ok = 0;                   /* a repeated term */
                } else {
                    mark[u] = r;
                    ix->leaf[nleaf++] = u;
                }
            }
            S.v[2] = nleaf - S.v[1];
            out[nout++] = S;
        }
        if (!ok || folded != partials) {      /* keep the component's rows */
            nout = out0;
            nleaf = leaf0;
            for (uint32_t r = r0; r < r1; r++) out[nout++] = ix->rows[r];
        }
        ix->crow[c] = out0;
    }
#undef LP_PARTIAL
    ix->crow[ix->n_comps] = nout;
    ix->n_rows = nout;
    memcpy(ix->rows, out, (size_t)nout * sizeof(LinRow));
    dvs_ctx_free(ctx, def, tsz);
}

static LinIx *_build(dvs_ctx_t *ctx) {
    dvs_linprobe_free(ctx);
    const uint32_t nv = ctx->n_vars, np = _n_props(ctx);
    if (nv == 0 || !ctx->prop_refs) return NULL;

    /* Components: variables joined by a propagator, or by a guard to what
     * it guards (a guard's bound wakes the propagator). A component is
     * linear when all its propagators are rows and its variables unsigned. */
    const size_t tsz = (size_t)nv * (2 * sizeof(uint32_t) + 1);
    uint32_t *rep = (uint32_t *)dvs_ctx_resize(ctx, NULL, 0, tsz);
    if (!rep) return NULL;
    uint32_t *cid = rep + nv;
    uint8_t *bad = (uint8_t *)(cid + nv);
    for (uint32_t i = 0; i < nv; i++) { rep[i] = i; bad[i] = 0; cid[i] = LP_NONE; }
    for (uint32_t k = 0; k < np; k++) {
        const Propagator *p = _prop(ctx, k);
        if (!p) continue;
        uint32_t n;
        const uint32_t *ids = prop_watched_vars(p, &n);
        for (uint32_t j = 1; j < n; j++) {
            uint32_t a = _find(rep, ids[0]), b = _find(rep, ids[j]);
            if (a != b) rep[a] = b;
        }
        if (n && _guarded(ctx, k) && ctx->prop_guard_vars[k] < nv) {
            uint32_t a = _find(rep, ids[0]), b = _find(rep, ctx->prop_guard_vars[k]);
            if (a != b) rep[a] = b;
        }
    }
    for (uint32_t i = 0; i < nv; i++) {
        const Variable *v = &ctx->vars[i];
        if ((v->flags & (VAR_SIGNED | VAR_TIER2)) || v->width == 0 || v->width > 64)
            bad[_find(rep, i)] = 1;
    }
    uint32_t nr = 0;
    for (uint32_t k = 0; k < np; k++) {
        const Propagator *p = _prop(ctx, k);
        if (!p) continue;
        uint32_t n, v[3];
        uint8_t w;
        const uint32_t *ids = prop_watched_vars(p, &n);
        if (n == 0) continue;
        if (_guarded(ctx, k) || prop_linear_row(p, ctx, v, &w) == LINROW_NONE)
            bad[_find(rep, ids[0])] = 1;
        else
            nr++;
    }
    uint32_t nc = 0;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t r = _find(rep, i);
        if (!bad[r] && cid[r] == LP_NONE) cid[r] = nc++;
    }

    /* One block: header, then the arrays, 8-byte ones first. */
    size_t off = (sizeof(LinIx) + 7u) & ~(size_t)7u;
    const size_t o_lo = off;   off += 4u * (size_t)nv * sizeof(uint64_t);
    const size_t o_rows = off; off += (size_t)nr * sizeof(LinRow);
    const size_t o_u32 = off;  off += ((size_t)nv + nc + 1u + nv + 1u + 3u * (size_t)nr
                                       + nr + 1u + 2u * (size_t)nv + 2u * (size_t)nr)
                                      * sizeof(uint32_t);
    const size_t o_inq = off;  off += nr + 1u;
    LinIx *ix = (LinIx *)dvs_ctx_resize(ctx, NULL, 0, off);
    if (!ix) { dvs_ctx_free(ctx, rep, tsz); return NULL; }
    char *base = (char *)ix;
    memset(ix, 0, sizeof *ix);
    ix->bytes = off;
    ix->epoch = ctx->wmemo_epoch;
    ix->restore_gen = ctx->restore_gen;
    ix->n_props = np;
    ix->n_vars = nv;
    ix->n_comps = nc;
    ix->undo_comp = LP_NONE;
    ix->lo = (uint64_t *)(base + o_lo);
    ix->hi = ix->lo + nv;
    ix->slo = ix->hi + nv;
    ix->shi = ix->slo + nv;
    ix->rows = (LinRow *)(base + o_rows);
    ix->comp = (uint32_t *)(base + o_u32);
    ix->crow = ix->comp + nv;
    ix->vrow = ix->crow + nc + 1;
    ix->vrows = ix->vrow + nv + 1;
    ix->q = ix->vrows + 3u * (size_t)nr;
    ix->seen = ix->q + nr + 1u;
    ix->chg = ix->seen + nv;
    ix->leaf = ix->chg + nv;
    memset(ix->seen, 0, (size_t)nv * sizeof(uint32_t));
    ix->inq = (uint8_t *)(base + o_inq);
    memset(ix->inq, 0, nr + 1u);

    /* Rows grouped by component (counting sort). */
    memset(ix->crow, 0, (nc + 1u) * sizeof(uint32_t));
    for (uint32_t i = 0; i < nv; i++) ix->comp[i] = cid[_find(rep, i)];
    for (uint32_t k = 0; k < np; k++) {
        const Propagator *p = _prop(ctx, k);
        uint32_t n, v[3];
        uint8_t w;
        if (!p || !prop_watched_vars(p, &n) || n == 0) continue;
        uint32_t c = ix->comp[prop_watched_vars(p, &n)[0]];
        if (c != LP_NONE && prop_linear_row(p, ctx, v, &w) != LINROW_NONE) ix->crow[c + 1]++;
    }
    for (uint32_t c = 0; c < nc; c++) ix->crow[c + 1] += ix->crow[c];
    /* Fill, with rep reused as each component's next slot. */
    for (uint32_t c = 0; c < nc; c++) rep[c] = ix->crow[c];
    for (uint32_t k = 0; k < np; k++) {
        const Propagator *p = _prop(ctx, k);
        uint32_t n;
        LinRow row;
        if (!p || !prop_watched_vars(p, &n) || n == 0) continue;
        uint32_t c = ix->comp[prop_watched_vars(p, &n)[0]];
        if (c == LP_NONE) continue;
        row.k = prop_linear_row(p, ctx, row.v, &row.w);
        if (row.k != LINROW_NONE) ix->rows[rep[c]++] = row;
    }
    ix->n_rows = ix->crow[nc];
    _fold(ctx, ix);
    /* variable -> rows */
    memset(ix->vrow, 0, ((size_t)nv + 1u) * sizeof(uint32_t));
    for (uint32_t r = 0; r < ix->n_rows; r++) {
        const LinRow *R = &ix->rows[r];
        const uint32_t n = _row_nvars(R);
        for (uint32_t j = 0; j < n; j++) ix->vrow[_row_var(ix, R, j) + 1]++;
    }
    for (uint32_t i = 0; i < nv; i++) ix->vrow[i + 1] += ix->vrow[i];
    for (uint32_t i = 0; i < nv; i++) rep[i] = ix->vrow[i];
    for (uint32_t r = 0; r < ix->n_rows; r++) {
        const LinRow *R = &ix->rows[r];
        const uint32_t n = _row_nvars(R);
        for (uint32_t j = 0; j < n; j++) ix->vrows[rep[_row_var(ix, R, j)]++] = r;
    }
    dvs_ctx_free(ctx, rep, tsz);
    ctx->linprobe = ix;
    return ix;
}

/* The index of the problem as it is now (rebuilt when it changed), or NULL. */
static LinIx *_index(dvs_ctx_t *ctx) {
    LinIx *ix = (LinIx *)ctx->linprobe;
    if (ix && ix->epoch == ctx->wmemo_epoch && ix->n_props == _n_props(ctx)
            && ix->n_vars == ctx->n_vars && ix->restore_gen == ctx->restore_gen)
        return ix;
    return _build(ctx);
}

static void _store(dvs_ctx_t *ctx, uint32_t i, uint64_t lo, uint64_t hi) {
    Variable *v = &ctx->vars[i];
    if (VAR_IS_TIER0(v->flags)) {
        v->lo = (int32_t)lo;
        v->hi = (int32_t)hi;
    } else {
        WideBounds64 *wb = (WideBounds64 *)dvs_spool_ptr(&ctx->pool, v->holes_offset);
        wb->lo = (int64_t)lo;
        wb->hi = (int64_t)hi;
    }
}

int dvs_linprobe_run(dvs_ctx_t *ctx, uint32_t x, int64_t blo, int64_t bhi, uint32_t *fires) {
    *fires = 0;
    LinIx *ix = _index(ctx);
    if (!ix || x >= ix->n_vars || ix->comp[x] == LP_NONE) return -1;
    const uint32_t c = ix->comp[x];
    const uint32_t r0 = ix->crow[c], r1 = ix->crow[c + 1];
    uint64_t *lo = ix->lo, *hi = ix->hi;
    /* A probe reads and writes only the variables it reaches: each is
     * loaded on first use (seen), and the ones it narrows are listed (chg). */
    if (++ix->gen == 0) {
        memset(ix->seen, 0, (size_t)ix->n_vars * sizeof(uint32_t));
        ix->gen = 1;
    }
    ix->n_chg = 0;
#define LP_LOAD(i) do { const uint32_t l_ = (i);                            \
        if (ix->seen[l_] != ix->gen) {                                      \
            ix->seen[l_] = ix->gen;                                         \
            lo[l_] = ix->slo[l_] = (uint64_t)var_lo64(ctx, &ctx->vars[l_]); \
            hi[l_] = ix->shi[l_] = (uint64_t)var_hi64(ctx, &ctx->vars[l_]); \
        } } while (0)

    /* A worklist of rows; a row is queued when a bound of one of its
     * variables narrows. A row's narrowing equals its propagator's: the add
     * forward and exactly backward (_fire_bvadd_exact), the comparisons as
     * _fire_bounds_le/lt/eq. Each leaves its row at a fixpoint, so a row is
     * not queued by its own narrowings. (The add: after a' = a & (b + d),
     * b' = b & (a' - d), d' = d & (a' - b'), the sums b' + d' and a' - d',
     * a' - b' contain a', b', d' again.) */
    const uint32_t nq = r1 - r0 + 1u, cap = 64u * (r1 - r0) + 1024u;
    uint32_t qh = 0, qt = 0, evals = 0, cur = LP_NONE;
    int res = 1;
#define LP_QUEUE(i) do {                                                    \
        for (uint32_t t_ = ix->vrow[i]; t_ < ix->vrow[(i) + 1]; t_++) {     \
            const uint32_t r_ = ix->vrows[t_];                              \
            if (!ix->inq[r_] && r_ != cur) {                                \
                ix->inq[r_] = 1;                                            \
                ix->q[qt] = r_;                                             \
                if (++qt == nq) qt = 0;                                     \
            }                                                               \
        } } while (0)
#define LP_CHG(i) do {                                                      \
        if (lo[i] == ix->slo[i] && hi[i] == ix->shi[i]) ix->chg[ix->n_chg++] = (i); \
        } while (0)
#define LP_LB(i, b) do { const uint64_t b_ = (b);                           \
        if (b_ > lo[i]) {                                                   \
            if (b_ > hi[i]) { res = 0; goto done; }                         \
            LP_CHG(i); lo[i] = b_; LP_QUEUE(i);                             \
        } } while (0)
#define LP_UB(i, b) do { const uint64_t b_ = (b);                           \
        if (b_ < hi[i]) {                                                   \
            if (b_ < lo[i]) { res = 0; goto done; }                         \
            LP_CHG(i); hi[i] = b_; LP_QUEUE(i);                             \
        } } while (0)
    LP_LOAD(x);
    LP_LB(x, (uint64_t)blo);
    LP_UB(x, (uint64_t)bhi);
    while (qh != qt) {
        cur = ix->q[qh];
        const LinRow *R = &ix->rows[cur];
        ix->inq[cur] = 0;
        if (++qh == nq) qh = 0;
        if (++evals > cap) { res = -1; goto done; }
        const uint32_t a = R->v[0], b = R->v[1], d = R->v[2];
        LP_LOAD(a);
        if (R->k == LINROW_SUM) {               /* a = the sum of the terms */
            const uint32_t *t = &ix->leaf[b];
            uint64_t sl = 0, sh = 0, s0 = 0;
            for (uint32_t j = 0; j < d; j++) {
                LP_LOAD(t[j]);
                sl += lo[t[j]];
                sh += hi[t[j]];
                if (__builtin_add_overflow(s0, ix->shi[t[j]], &s0)) s0 = ~(uint64_t)0;
            }
            /* As for an add: exact now, so exact while bounds narrow. */
            if (s0 >= (R->w >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << R->w) - 1u)) {
                res = -1;
                goto done;
            }
            evals += d - 1u;                    /* the adds it stands for */
            LP_LB(a, sl);
            LP_UB(a, sh);
            /* One pass is the fixpoint: a term narrowed to fit a leaves the
             * others' bounds on a as they were. */
            for (uint32_t j = 0; j < d; j++) {
                const uint32_t u = t[j];
                const uint64_t ul = lo[u], uh = hi[u];   /* as in sl, sh */
                if (lo[a] > sh - uh) LP_LB(u, lo[a] - (sh - uh));
                if (hi[a] - (sl - ul) < uh) LP_UB(u, hi[a] - (sl - ul));
            }
            continue;
        }
        LP_LOAD(b);
        switch (R->k) {
        case LINROW_ADD: {                      /* a = b + d */
            LP_LOAD(d);
            /* An add exact now stays exact: bounds only narrow. A row the
             * probe does not reach keeps its bounds, so it is not checked. */
            uint64_t s;
            if (__builtin_add_overflow(ix->shi[b], ix->shi[d], &s)
                    || s >= (R->w >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << R->w) - 1u)) {
                res = -1;
                goto done;
            }
            LP_LB(a, lo[b] + lo[d]);
            LP_UB(a, hi[b] + hi[d]);
            if (lo[a] > hi[d]) LP_LB(b, lo[a] - hi[d]);
            if (hi[a] >= lo[d]) LP_UB(b, hi[a] - lo[d]);
            if (lo[a] > hi[b]) LP_LB(d, lo[a] - hi[b]);
            if (hi[a] >= lo[b]) LP_UB(d, hi[a] - lo[b]);
            break;
        }
        case LINROW_LE:
            LP_UB(a, hi[b]);
            LP_LB(b, lo[a]);
            break;
        case LINROW_LT:
            if (hi[b] == 0 || lo[a] == UINT64_MAX) { res = 0; goto done; }
            LP_UB(a, hi[b] - 1u);
            LP_LB(b, lo[a] + 1u);
            break;
        case LINROW_EQ:
            LP_LB(a, lo[b]);
            LP_UB(a, hi[b]);
            LP_LB(b, lo[a]);
            LP_UB(b, hi[a]);
            break;
        }
    }
done:
#undef LP_LOAD
#undef LP_CHG
#undef LP_QUEUE
#undef LP_LB
#undef LP_UB
    while (qh != qt) {
        ix->inq[ix->q[qh]] = 0;
        if (++qh == nq) qh = 0;
    }
    if (res < 0) return -1;
    *fires = evals;
    if (res == 0) return 0;
    for (uint32_t j = 0; j < ix->n_chg; j++) {
        const uint32_t i = ix->chg[j];
        _store(ctx, i, lo[i], hi[i]);
    }
    ix->undo_comp = c;
    return 1;
}

void dvs_linprobe_undo(dvs_ctx_t *ctx) {
    LinIx *ix = (LinIx *)ctx->linprobe;
    if (!ix || ix->undo_comp == LP_NONE) return;
    for (uint32_t j = 0; j < ix->n_chg; j++) {
        const uint32_t i = ix->chg[j];
        _store(ctx, i, ix->slo[i], ix->shi[i]);
    }
    ix->undo_comp = LP_NONE;
}
