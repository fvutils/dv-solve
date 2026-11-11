/*
 * dvs_diffcycle.c -- negative-cycle detection over difference relations.
 * See dvs_diffcycle.h for why.
 *
 * Graph: one node per variable of a relation on a cycle of relations; an
 * edge u -> v of weight w states  val(v) <= val(u) + w.
 *
 *   x <= y + c          y -> x  (c)
 *   r == a + b          a -> r  (hi b)   b -> r  (hi a)
 *                       r -> a  (-lo b)  r -> b  (-lo a)
 *   r == a - b          a -> r  (-lo b)  r -> a  (hi b)
 *                       a -> b  (-lo r)  b -> a  (hi r)
 *   r == x + c          x -> r  (c)      r -> x  (-c)
 *
 * An arithmetic relation states an integer equation only while the operand
 * bounds prove that the modular result cannot wrap; otherwise its edges are
 * left out of this round (sound: fewer edges can only hide a cycle). Edge
 * weights and node values are kept within +-2^61 so no path sum overflows.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dvs_ctx.h"
#include "dvs_propagator.h"
#include "dvs_lcg.h"
#include "dvs_pool.h"
#include "dvs_diffcycle.h"

#define DC_MAX_NODES   512u
#define DC_MAX_RELS    1024u
#define DC_LIM         ((int64_t)1 << 61)
/* Firings a propagation spends before the check runs. A negative cycle is
 * what bounds propagation crawls round, so it shows as a propagation that
 * keeps firing; one that settles first has none (dvs_diffcycle.h). Most
 * settle within a few dozen, and the check, which rebuilds its graph on
 * every firing, cost more than they did (shift_width, page_table_entry). */
#define DC_FIRES_FIRST 64u

typedef struct {
    uint32_t u, v;     /* local node indices */
    int64_t  w;
    uint32_t rel;      /* relation index */
    uint8_t  wlit;     /* weight literal: 0 none, else 1 + index into the relation's bound slots */
} DcEdge;

/* Wide-watch layout (see _wake_var): hdr, n_vars, _capacity, var_ids[cap],
 * watcher_nexts[cap]; this propagator's own data follows. */
typedef struct {
    Propagator hdr;
    uint32_t   n_vars;
    uint32_t   _capacity;
    /* uint32_t var_ids[n_vars]; uint32_t watcher_nexts[n_vars]; */
} DcHead;

typedef struct {
    uint32_t n_rels;
    uint32_t rels_off;     /* byte offsets from the propagator start */
    uint32_t dist_off;
    uint32_t pred_off;
    uint32_t edges_off;
    uint32_t pot_ok;       /* dist[] is a feasible potential from the last
                            * run that found no negative cycle */
    uint32_t n_lits;       /* the last conflict's explanation */
    Literal  lits[MAX_EXPLAIN_LITS];
} DcData;

static inline uint32_t *_ids(DcHead *h)    { return (uint32_t *)(h + 1); }
static inline DcData   *_data(DcHead *h)   { return (DcData *)(_ids(h) + 2u * h->_capacity); }
#define DC_AT(h, off, T) ((T *)((char *)(h) + (off)))

/* A variable's current value interval, if within +-DC_LIM. */
static int _ival(const dvs_ctx_t *ctx, uint32_t id, int64_t *lo, int64_t *hi) {
    const Variable *v = &ctx->vars[id];
    int64_t l = var_lo64(ctx, v), h = var_hi64(ctx, v);
    if (v->flags & VAR_SIGNED) {
        if (l < -DC_LIM || h > DC_LIM) return 0;
    } else {
        if ((uint64_t)h > (uint64_t)DC_LIM) return 0;
    }
    *lo = l; *hi = h;
    return 1;
}

/* The values variable `id` can represent, clamped to +-DC_LIM. */
static void _repr(const Variable *v, int64_t *mn, int64_t *mx) {
    int64_t a = var_repr_min(v), b = var_repr_max(v);
    if (!(v->flags & VAR_SIGNED) && (v->width >= 62)) b = DC_LIM;
    if (a < -DC_LIM) a = -DC_LIM;
    if (b > DC_LIM)  b = DC_LIM;
    *mn = a; *mx = b;
}

/* Bound slots of a relation, for explanations: per variable k (0..2),
 * slot 2k is its lower bound, 2k+1 its upper bound. */
typedef struct { int64_t lo[3], hi[3]; uint8_t need; } RelState;  /* need: bitmask of slots */

static Literal _lit(uint32_t var, int is_lb, int64_t bound) {
    Literal l;
    l.var_id = var; l.is_lb = is_lb ? 1 : 0;
    l._pad[0] = l._pad[1] = l._pad[2] = 0;
    l.bound = bound;
    return l;
}

/* Is relation `r` an exact integer equation under the current bounds? Fills
 * the values in `st` and the validity slots in st->need. For ADDC, the
 * constant as it acts on values is returned in *cc. */
static int _rel_valid(const dvs_ctx_t *ctx, const DiffRel *r, RelState *st, int64_t *cc) {
    st->need = 0;
    if (r->kind == DREL_LE) {
        const Variable *a = &ctx->vars[r->v[0]], *b = &ctx->vars[r->v[1]];
        return (a->flags & VAR_SIGNED) == (b->flags & VAR_SIGNED);
    }
    int n = (r->kind == DREL_ADDC) ? 2 : 3;
    const Variable *rv = &ctx->vars[r->v[0]];
    uint32_t sg = rv->flags & VAR_SIGNED;
    for (int k = 0; k < n; k++) {
        const Variable *vk = &ctx->vars[r->v[k]];
        if ((vk->flags & VAR_SIGNED) != sg) return 0;
        if (r->width && vk->width > r->width) return 0;
        if (!_ival(ctx, r->v[k], &st->lo[k], &st->hi[k])) return 0;
    }
    if (r->width && rv->width != r->width) return 0;
    int64_t rmin, rmax;
    _repr(rv, &rmin, &rmax);
    int64_t lo, hi;
    if (r->kind == DREL_SUM) {
        lo = st->lo[1] + st->lo[2]; hi = st->hi[1] + st->hi[2];
        st->need = sg ? 0x3C : 0x28;       /* signed: lo/hi of a, b; unsigned: hi a, hi b */
    } else if (r->kind == DREL_SUB) {
        lo = st->lo[1] - st->hi[2]; hi = st->hi[1] - st->lo[2];
        st->need = sg ? 0x3C : 0x2C;       /* unsigned: lo a, hi a, hi b */
    } else {                               /* ADDC */
        int64_t c = r->c;
        if (r->width && r->width < 64 && c >= ((int64_t)1 << (r->width - 1)))
            c -= (int64_t)1 << r->width;   /* x - k is stored as x + (2^w - k) */
        if (c < -DC_LIM || c > DC_LIM) return 0;
        *cc = c;
        lo = st->lo[1] + c; hi = st->hi[1] + c;
        st->need = 0x0C;                   /* lo x, hi x */
    }
    return lo >= rmin && hi <= rmax;
}

static PropResult _fire_diffcycle(Propagator *self, dvs_ctx_t *ctx) {
    DcHead  *h  = (DcHead *)self;
    DcData  *d  = _data(h);
    uint32_t nv = h->n_vars;
    uint32_t *ids = _ids(h);
    const DiffRel *rels  = DC_AT(h, d->rels_off, const DiffRel);
    int64_t  *dist       = DC_AT(h, d->dist_off, int64_t);
    uint32_t *pred       = DC_AT(h, d->pred_off, uint32_t);
    DcEdge   *edges      = DC_AT(h, d->edges_off, DcEdge);
    /* Woken again by the crawl's next tightening. */
    if (ctx->prop_fires - ctx->prop_call_fires < DC_FIRES_FIRST) return PROP_OK;

    /* Local index of each relation variable: relations store local ids. */
    uint32_t ne = 0;
    for (uint32_t i = 0; i < d->n_rels; i++) {
        const DiffRel *r = &rels[i];
        RelState st; int64_t cc = 0;
        DiffRel gr = *r;                   /* global ids for the validity check */
        for (int k = 0; k < 3; k++) gr.v[k] = ids[r->v[k]];
        if (!_rel_valid(ctx, &gr, &st, &cc)) continue;
        uint32_t x = r->v[0], a = r->v[1], b = r->v[2];
#define EDGE(U, V, W, SLOT) do { edges[ne].u = (U); edges[ne].v = (V); edges[ne].w = (W); \
        edges[ne].rel = i; edges[ne].wlit = (SLOT); ne++; } while (0)
        switch (r->kind) {
        case DREL_LE:   EDGE(a, x, r->c, 0); break;
        case DREL_SUM:  EDGE(a, x, st.hi[2], 1 + 5); EDGE(b, x, st.hi[1], 1 + 3);
                        EDGE(x, a, -st.lo[2], 1 + 4); EDGE(x, b, -st.lo[1], 1 + 2); break;
        case DREL_SUB:  EDGE(a, x, -st.lo[2], 1 + 4); EDGE(x, a, st.hi[2], 1 + 5);
                        EDGE(a, b, -st.lo[0], 1 + 0); EDGE(b, a, st.hi[0], 1 + 1); break;
        case DREL_ADDC: EDGE(a, x, cc, 0); EDGE(x, a, -cc, 0); break;
        }
#undef EDGE
    }
    if (ne == 0) return PROP_OK;

    /* The distances the last run converged to are a potential: if every
     * edge still holds under them (dist[v] <= dist[u] + w), the graph has no
     * negative cycle, and the run below would find none. One pass instead of
     * up to nv: most firings follow a tightening that closes no cycle. */
    if (d->pot_ok) {
        uint32_t e = 0;
        while (e < ne && dist[edges[e].u] + edges[e].w >= dist[edges[e].v]) e++;
        if (e == ne) return PROP_OK;
    }

    /* Bellman-Ford from a virtual source (every dist starts at 0). Still
     * relaxing after nv passes means a negative cycle. */
    d->pot_ok = 0;
    for (uint32_t i = 0; i < nv; i++) { dist[i] = 0; pred[i] = UINT32_MAX; }
    uint32_t last = UINT32_MAX;
    for (uint32_t pass = 0; pass <= nv; pass++) {
        last = UINT32_MAX;
        int clipped = 0;
        for (uint32_t e = 0; e < ne; e++) {
            int64_t nd = dist[edges[e].u] + edges[e].w;   /* |dist| <= nv * 2^61 / ... */
            if (nd < dist[edges[e].v]) {
                if (nd < -((int64_t)1 << 62)) { clipped = 1; continue; }   /* keep sums in range */
                dist[edges[e].v] = nd;
                pred[edges[e].v] = e;
                last = edges[e].v;
            }
        }
        if (last == UINT32_MAX) {                         /* converged: no negative cycle */
            d->pot_ok = !clipped;
            return PROP_OK;
        }
    }

    /* `last` was relaxed on pass nv: walking pred nv times lands on the cycle. */
    uint32_t y = last;
    for (uint32_t i = 0; i < nv; i++) {
        if (pred[y] == UINT32_MAX) return PROP_OK;
        y = edges[pred[y]].u;
    }

    /* Collect the cycle's explanation: each edge's weight literal and its
     * relation's no-wrap literals. */
    d->n_lits = 0;
    uint32_t node = y, steps = 0;
    int64_t total = 0;
    do {
        uint32_t e = pred[node];
        if (e == UINT32_MAX || ++steps > nv) return PROP_OK;
        total += edges[e].w;
        const DiffRel *r = &rels[edges[e].rel];
        if (r->kind != DREL_LE) {
            DiffRel gr = *r; RelState st; int64_t cc = 0;
            for (int k = 0; k < 3; k++) gr.v[k] = ids[r->v[k]];
            if (!_rel_valid(ctx, &gr, &st, &cc)) return PROP_OK;
            uint8_t need = st.need;
            if (edges[e].wlit) need |= (uint8_t)(1u << (edges[e].wlit - 1));
            for (int s = 0; s < 6; s++) {
                if (!(need & (1u << s))) continue;
                int k = s / 2, is_lb = (s % 2) == 0;
                Literal l = _lit(gr.v[k], is_lb, is_lb ? st.lo[k] : st.hi[k]);
                int dup = 0;
                for (uint32_t q = 0; q < d->n_lits; q++)
                    if (d->lits[q].var_id == l.var_id && d->lits[q].is_lb == l.is_lb) { dup = 1; break; }
                if (dup) continue;
                if (d->n_lits >= MAX_EXPLAIN_LITS) return PROP_OK;   /* too long to explain */
                d->lits[d->n_lits++] = l;
            }
        }
        node = edges[e].u;
    } while (node != y);
    if (total >= 0) return PROP_OK;        /* not a negative cycle after all */

    /* Report it as an empty domain on a cycle variable, so conflict analysis
     * explains it through _explain_diffcycle. */
    node = y;
    do {
        uint32_t vid = ids[node];
        const Variable *v = &ctx->vars[vid];
        int64_t lo = var_lo64(ctx, v), hi = var_hi64(ctx, v);
        if (var_b_gt(v, lo, var_repr_min(v))) {
            (void)ctx_tighten_ub64(ctx, vid, lo - 1);     /* empties the domain */
            return PROP_CONFLICT;
        }
        if (var_b_lt(v, hi, var_repr_max(v))) {
            (void)ctx_tighten_lb64(ctx, vid, hi + 1);
            return PROP_CONFLICT;
        }
        node = edges[pred[node]].u;
    } while (node != y);
    return PROP_OK;                        /* no variable can carry it: stay silent */
}

static int _explain_diffcycle(Propagator *self, dvs_ctx_t *ctx,
                              uint32_t var_id, uint8_t is_lb,
                              int64_t new_bound, Explanation *out) {
    (void)ctx; (void)var_id; (void)is_lb; (void)new_bound;
    DcData *d = _data((DcHead *)self);
    out->n_lits = d->n_lits;
    memcpy(out->lits, d->lits, d->n_lits * sizeof(Literal));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static uint32_t _uf_find(uint32_t *p, uint32_t x) {
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}

/* Strongly connected components of the graph of n nodes and the m edges
 * u[k] -> v[k] (Kosaraju, iterative): comp[x] for each node. 0, or -1 on
 * allocation failure. */
static int _scc(uint32_t n, uint32_t m, const uint32_t *u, const uint32_t *v, uint32_t *comp) {
    uint32_t *off  = (uint32_t *)calloc(2u * (n + 1u), sizeof(uint32_t));
    uint32_t *adj  = (uint32_t *)malloc(((size_t)2u * m + 1u) * sizeof(uint32_t));
    uint32_t *ord  = (uint32_t *)malloc(((size_t)n + 1u) * sizeof(uint32_t));
    uint32_t *stk  = (uint32_t *)malloc(((size_t)n + 1u) * sizeof(uint32_t));
    uint32_t *pos  = (uint32_t *)malloc(((size_t)n + 1u) * sizeof(uint32_t));
    int rc = -1;
    if (!off || !adj || !ord || !stk || !pos) goto out;
    /* Forward adjacency in adj[off[x] .. off[x+1]), reverse after it. */
    uint32_t *roff = off + n + 1u, *radj = adj + m;
    for (uint32_t k = 0; k < m; k++) { off[u[k] + 1]++; roff[v[k] + 1]++; }
    for (uint32_t x = 0; x < n; x++) { off[x + 1] += off[x]; roff[x + 1] += roff[x]; }
    for (uint32_t x = 0; x < n; x++) pos[x] = off[x];
    for (uint32_t k = 0; k < m; k++) adj[pos[u[k]]++] = v[k];
    for (uint32_t x = 0; x < n; x++) pos[x] = roff[x];
    for (uint32_t k = 0; k < m; k++) radj[pos[v[k]]++] = u[k];
    /* Pass 1: nodes in order of finishing a depth-first walk. */
    uint32_t no = 0;
    for (uint32_t x = 0; x < n; x++) comp[x] = UINT32_MAX;   /* unvisited */
    for (uint32_t s = 0; s < n; s++) {
        if (comp[s] != UINT32_MAX) continue;
        uint32_t sp = 0;
        stk[sp++] = s; comp[s] = 0; pos[s] = off[s];
        while (sp) {
            uint32_t x = stk[sp - 1];
            if (pos[x] < off[x + 1]) {
                uint32_t y = adj[pos[x]++];
                if (comp[y] == UINT32_MAX) { comp[y] = 0; pos[y] = off[y]; stk[sp++] = y; }
            } else {
                ord[no++] = x;
                sp--;
            }
        }
    }
    /* Pass 2: on the reverse graph, in reverse finishing order. */
    for (uint32_t x = 0; x < n; x++) comp[x] = UINT32_MAX;
    uint32_t nc = 0;
    for (uint32_t i = n; i-- > 0;) {
        uint32_t s = ord[i];
        if (comp[s] != UINT32_MAX) continue;
        uint32_t sp = 0;
        stk[sp++] = s; comp[s] = nc;
        while (sp) {
            uint32_t x = stk[--sp];
            for (uint32_t k = roff[x]; k < roff[x + 1]; k++)
                if (comp[radj[k]] == UINT32_MAX) { comp[radj[k]] = nc; stk[sp++] = radj[k]; }
        }
        nc++;
    }
    rc = 0;
out:
    free(off); free(adj); free(ord); free(stk); free(pos);
    return rc;
}

int diffcycle_build(dvs_ctx_t *ctx) {
    if (!ctx->prop_refs || ctx->n_vars == 0) return 0;
    uint32_t lim = ctx->n_props < ctx->n_prop_refs_capacity
                   ? ctx->n_props : ctx->n_prop_refs_capacity;
    DiffRel *rels = (DiffRel *)malloc((size_t)(2u * lim + 1u) * sizeof(DiffRel));
    uint32_t *uf  = (uint32_t *)malloc((size_t)ctx->n_vars * sizeof(uint32_t));
    uint8_t  *cyc = (uint8_t *)calloc(ctx->n_vars, 1);
    uint32_t *loc = (uint32_t *)malloc((size_t)ctx->n_vars * sizeof(uint32_t));
    uint32_t *comp = (uint32_t *)malloc((size_t)ctx->n_vars * sizeof(uint32_t));
    uint8_t  *keep = (uint8_t *)calloc(2u * lim + 1u, 1);   /* per relation */
    uint32_t *eu = NULL, *ev = NULL, *er = NULL;
    int rc = 0;
    if (!rels || !uf || !cyc || !loc || !comp || !keep) { rc = -1; goto out; }

    uint32_t nr = 0;
    for (uint32_t pi = 0; pi < lim; pi++) {
        if (ctx->prop_refs[pi] == EXPR_NULL) continue;
        /* A guard-gated propagator (an array select's `r == a[i]` under
         * `idx == i`) states its relation only while the guard holds; as an
         * unconditional edge it closes cycles that do not exist. */
        if (ctx->prop_guard_vars && ctx->prop_guard_vars[pi] != EXPR_NULL) continue;
        Propagator *p = (Propagator *)dvs_spool_ptr(&ctx->pool, ctx->prop_refs[pi]);
        DiffRel two[2];
        int k = prop_difference_relations(p, ctx, two);
        for (int j = 0; j < k; j++) {
            int nvk = (two[j].kind == DREL_LE || two[j].kind == DREL_ADDC) ? 2 : 3;
            int ok = 1;
            for (int q = 0; q < nvk; q++) if (two[j].v[q] >= ctx->n_vars) ok = 0;
            if (nvk == 2) two[j].v[2] = two[j].v[1];
            if (ok) rels[nr++] = two[j];
        }
    }
    if (nr < 2) goto out;

    /* A cycle of relations: one whose variables are already connected
     * through other relations. A lone r = a + b is a triangle of its own,
     * but its edges can never sum negative. */
    for (uint32_t i = 0; i < ctx->n_vars; i++) uf[i] = i;
    int any = 0;
    for (uint32_t i = 0; i < nr; i++) {
        uint32_t root[3];
        int hit = 0;
        for (int q = 0; q < 3; q++) {
            root[q] = _uf_find(uf, rels[i].v[q]);
            for (int t = 0; t < q; t++)
                if (rels[i].v[t] != rels[i].v[q] && root[t] == root[q]) hit = 1;
        }
        for (int q = 1; q < 3; q++) {
            uint32_t ra = _uf_find(uf, rels[i].v[0]), rb = _uf_find(uf, rels[i].v[q]);
            if (ra != rb) uf[rb] = ra;
        }
        if (hit) { any = 1; rels[i]._pad[0] = 1; }
    }
    if (!any) goto out;
    /* A component is cyclic if any of its relations closed a cycle. */
    for (uint32_t i = 0; i < nr; i++)
        if (rels[i]._pad[0]) cyc[_uf_find(uf, rels[i].v[0])] = 1;

    /* Of those, a negative cycle is a directed one through two relations
     * or more: one relation's own edges never sum negative (r = a + b, or
     * x and x + c both ways). So keep only the relations with an edge
     * inside a strongly connected component that holds edges of two
     * relations. The edge directions (the table above) depend on the
     * kinds only, not on bounds. Both tests are needed: x == y + c both
     * ways along a chain is one component with no cycle. */
    for (uint32_t i = 0; i < ctx->n_vars; i++) loc[i] = UINT32_MAX;
    uint32_t n0 = 0;
    for (uint32_t i = 0; i < nr; i++)
        for (int q = 0; q < 3; q++)
            if (loc[rels[i].v[q]] == UINT32_MAX) loc[rels[i].v[q]] = n0++;
    eu = (uint32_t *)malloc((size_t)4u * nr * sizeof(uint32_t));
    ev = (uint32_t *)malloc((size_t)4u * nr * sizeof(uint32_t));
    er = (uint32_t *)malloc((size_t)4u * nr * sizeof(uint32_t));
    if (!eu || !ev || !er) { rc = -1; goto out; }
    uint32_t ne = 0;
    for (uint32_t i = 0; i < nr; i++) {
        uint32_t x = loc[rels[i].v[0]], a = loc[rels[i].v[1]], b = loc[rels[i].v[2]];
#define DIR(U, V) do { eu[ne] = (U); ev[ne] = (V); er[ne] = i; ne++; } while (0)
        switch (rels[i].kind) {
        case DREL_LE:   DIR(a, x); break;
        case DREL_SUM:  DIR(a, x); DIR(b, x); DIR(x, a); DIR(x, b); break;
        case DREL_SUB:  DIR(a, x); DIR(x, a); DIR(a, b); DIR(b, a); break;
        case DREL_ADDC: DIR(a, x); DIR(x, a); break;
        }
#undef DIR
    }
    if (_scc(n0, ne, eu, ev, comp) != 0) { rc = -1; goto out; }
    /* loc[c] (reused): 0 no edge inside component c yet, 1 + the relation
     * of the first, or UINT32_MAX once edges of two relations are inside. */
    for (uint32_t c = 0; c < n0; c++) loc[c] = 0;
    for (uint32_t k = 0; k < ne; k++) {
        uint32_t c = comp[eu[k]];
        if (c != comp[ev[k]] || loc[c] == UINT32_MAX) continue;
        if (!loc[c]) loc[c] = 1 + er[k];
        else if (loc[c] != 1 + er[k]) loc[c] = UINT32_MAX;
    }
    for (uint32_t k = 0; k < ne; k++)
        if (comp[eu[k]] == comp[ev[k]] && loc[comp[eu[k]]] == UINT32_MAX) keep[er[k]] = 1;

    /* Keep the relations of cyclic components; number their variables. */
    uint32_t nv = 0, nk = 0;
    for (uint32_t i = 0; i < ctx->n_vars; i++) loc[i] = UINT32_MAX;
    for (uint32_t i = 0; i < nr; i++) {
        if (!cyc[_uf_find(uf, rels[i].v[0])] || !keep[i]) continue;
        for (int q = 0; q < 3; q++) {
            uint32_t v = rels[i].v[q];
            if (loc[v] == UINT32_MAX) loc[v] = nv++;
        }
        rels[nk++] = rels[i];
    }
    if (nk == 0) goto out;
    if (nv > DC_MAX_NODES || nk > DC_MAX_RELS) goto out;   /* too big: leave it to propagation */
    for (uint32_t i = 0; i < nk; i++)
        for (int q = 0; q < 3; q++) rels[i].v[q] = loc[rels[i].v[q]];

    /* Lay the propagator out. */
    uint32_t off = (uint32_t)sizeof(DcHead) + 2u * nv * (uint32_t)sizeof(uint32_t);
    off = (off + 7u) & ~7u;
    uint32_t data_off = off;       off += (uint32_t)sizeof(DcData);
    off = (off + 7u) & ~7u;
    uint32_t rels_off = off;       off += nk * (uint32_t)sizeof(DiffRel);
    uint32_t dist_off = off;       off += nv * (uint32_t)sizeof(int64_t);
    uint32_t pred_off = off;       off += nv * (uint32_t)sizeof(uint32_t);
    off = (off + 7u) & ~7u;
    uint32_t edges_off = off;      off += 4u * nk * (uint32_t)sizeof(DcEdge);
    if (dvs_ctx_room_prop(ctx) != 0) goto out;

    uint32_t ref = dvs_spool_alloc(&ctx->pool, off, 8u);
    if (ref == EXPR_NULL) { rc = -1; goto out; }
    DcHead *h = (DcHead *)dvs_spool_ptr(&ctx->pool, ref);
    memset(h, 0, off);
    h->hdr.fire       = _fire_diffcycle;
    h->hdr.explain    = _explain_diffcycle;
    h->hdr.queue_next = EXPR_NULL;
    h->hdr.prop_id    = (uint16_t)ctx->n_props++;
    h->hdr.priority   = 0;      /* with the propagators that would crawl */
    h->hdr.flags      = PROP_FLAG_WIDE_WATCH;
    h->n_vars         = nv;
    h->_capacity      = nv;
    if ((uint32_t)((char *)_data(h) - (char *)h) != data_off) { rc = -1; goto out; }
    DcData *d = _data(h);
    d->n_rels    = nk;
    d->rels_off  = rels_off;
    d->dist_off  = dist_off;
    d->pred_off  = pred_off;
    d->edges_off = edges_off;
    memcpy(DC_AT(h, rels_off, DiffRel), rels, nk * sizeof(DiffRel));

    uint32_t *ids = _ids(h), *wn = ids + nv;
    for (uint32_t i = 0; i < ctx->n_vars; i++)
        if (loc[i] != UINT32_MAX) ids[loc[i]] = i;
    for (uint32_t i = 0; i < nv; i++) {
        wn[i] = ctx->watcher_heads[ids[i]];
        ctx->watcher_heads[ids[i]] = ref;
    }
    ctx->prop_refs[h->hdr.prop_id] = ref;
    prop_enqueue(ctx, ref);

out:
    free(rels); free(uf); free(cyc); free(loc); free(comp); free(keep); free(eu); free(ev); free(er);
    return rc;
}

int prop_is_diffcycle(const Propagator *p) {
    return p->fire == _fire_diffcycle;
}
