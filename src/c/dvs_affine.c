#include "dvs_affine.h"

#include <stdlib.h>
#include <string.h>

/* Node ids are topological (an AND is created after its children), so one
 * ascending pass evaluates the AIG. Columns are dense: an affine form is W
 * words of column bits plus a constant bit. */

typedef struct {
    uint32_t        N;
    dvs_aig_node_t *L, *R;      /* children of every AND by id (0 others)   */
    dvs_aig_node_t *xa, *xb;    /* XOR operands (xa == 0: not an XOR)       */
    int32_t        *col;        /* column of a leaf, -1 none                */
    uint32_t       *col_node;   /* node of each column                      */
    uint32_t        n_cols, n_inputs;
    /* Small affine gates: a non-XOR gate whose cone, cut at already-affine
     * nodes, has at most SM_LEAVES leaves and an affine truth table in them
     * (a constant table lookup: CRC tables). sm_off indexes sm_pool, where
     * the leaves with coefficient 1 are listed; sm_n of them, constant sm_c. */
    uint32_t       *sm_off;
    uint8_t        *sm_n, *sm_c;
    uint32_t       *sm_pool;
    uint32_t        sm_used, n_small, sm_stamp;
    uint8_t        *lin;        /* affine over the inputs alone             */
    uint32_t       *smark;      /* DFS stamp per node                       */
    uint32_t       *slot;       /* simulation slot per node                 */
    uint64_t        sm_budget;  /* node visits left for the detection       */
    uint32_t       *fidx;       /* form slot of an affine gate              */
    uint64_t       *forms;      /* W words per affine gate                  */
    uint8_t        *fconst;
    uint32_t        W;
    uint64_t        work, max_work;
} aff_t;

#define SM_NONE     UINT32_MAX
#define SM_LEAVES   8           /* independent leaves; 256 lanes: four words */
#define SM_CUT      16          /* leaves, derived ones included            */
#define SM_INTERNAL 256         /* gates between the node and its leaves    */

static inline uint32_t _id(dvs_aig_node_t lit) {
    return (uint32_t)(lit < 0 ? -lit : lit);
}

/* An affine gate: an XOR, or a small table with an affine truth table. */
static inline int _gate_affine(const aff_t *a, uint32_t id) {
    return a->xa[id] || a->sm_off[id] != SM_NONE;
}

/* Affine over the inputs alone: TRUE, an input, a small table, or an XOR of
 * two such nodes. Table cuts stop only here; an XOR with a nonlinear operand
 * is a nonlinear function of whatever is below it. */
static inline int _linear(const aff_t *a, uint32_t id) {
    return a->lin[id];
}

static int _cmp_u32(const void *x, const void *y) {
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return a < b ? -1 : a > b;
}

static const uint64_t SM_PAT[6] = {
    0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
    0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull,
};

static inline uint64_t _sm_pat(uint32_t j, uint32_t w) {
    if (j < 6) return SM_PAT[j];
    return ((w >> (j - 6)) & 1u) ? ~0ull : 0ull;
}

/* Is the gate `id` a small affine table? Cut its cone at linear nodes; with
 * at most SM_LEAVES independent leaves, simulate all 2^k leaf values and test
 * the truth table for affinity. Ids are topological, so every gate below `id`
 * has been classified already. With `pierce`, the cut goes through smaller
 * tables and stops only at inputs and XORs: a mux chain's steps are tables of
 * the same index bits, so they are not independent leaves of the whole. */
static int _find_small(aff_t *a, uint32_t id, uint32_t *stack, int pierce) {
    uint32_t leaves[SM_CUT], internal[SM_INTERNAL + 1];
    uint32_t nl = 0, ni = 0, sp = 0;
    uint32_t stamp = ++a->sm_stamp;
    stack[sp++] = _id(a->L[id]);
    stack[sp++] = _id(a->R[id]);
    while (sp) {
        uint32_t c = stack[--sp];
        if (c == 1 || a->smark[c] == stamp) continue;
        a->smark[c] = stamp;
        if (!a->sm_budget) return 0;
        a->sm_budget--;
        if (_linear(a, c) && !(pierce && a->sm_off[c] != SM_NONE)) {
            if (nl == SM_CUT) return 0;
            leaves[nl++] = c;
        } else {
            if (ni == SM_INTERNAL) return 0;
            internal[ni++] = c;
            stack[sp++] = _id(a->L[c]);
            stack[sp++] = _id(a->R[c]);
        }
    }
    /* The rewriter can reach through an XOR to its operands, so a cut may
     * hold an XOR and both of its operands. Such a leaf is derived from the
     * others, not an independent input of the table. Leaves in id order put
     * operands first. */
    for (uint32_t i = 1; i < nl; i++)
        for (uint32_t j = i; j > 0 && leaves[j - 1] > leaves[j]; j--) {
            uint32_t t = leaves[j]; leaves[j] = leaves[j - 1]; leaves[j - 1] = t;
        }
    uint8_t derived[SM_CUT];
    uint32_t nind = 0, ind_of[SM_CUT];
    for (uint32_t j = 0; j < nl; j++) {
        uint32_t L = leaves[j];
        /* Both operands must be earlier leaves (operands have smaller ids):
         * a visited operand may be an inner gate of the cut, whose value is
         * not computed until after the leaves. */
        derived[j] = 0;
        if (a->xa[L] && a->sm_off[L] == SM_NONE) {
            uint32_t x = _id(a->xa[L]), y = _id(a->xb[L]), hx = x == 1, hy = y == 1;
            for (uint32_t i = 0; i < j; i++) {
                hx |= leaves[i] == x;
                hy |= leaves[i] == y;
            }
            derived[j] = (uint8_t)(hx && hy);
        }
        if (!derived[j]) {
            if (nind == SM_LEAVES) return 0;
            ind_of[j] = nind++;
        }
    }
    /* Evaluate in id order; slot 0 is TRUE, then leaves, then gates. */
    qsort(internal, ni, sizeof(uint32_t), _cmp_u32);
    internal[ni++] = id;
    uint64_t val[(1 + SM_CUT + SM_INTERNAL + 1) * 4];
    for (uint32_t w = 0; w < 4; w++) val[w] = ~0ull;
    a->slot[1] = 0;
    for (uint32_t j = 0; j < nl; j++) {
        uint32_t L = leaves[j];
        a->slot[L] = 1 + j;
        if (!derived[j]) {
            for (uint32_t w = 0; w < 4; w++) val[(1 + j) * 4 + w] = _sm_pat(ind_of[j], w);
            continue;
        }
        dvs_aig_node_t x = a->xa[L], y = a->xb[L];
        const uint64_t *xv = val + (size_t)a->slot[_id(x)] * 4;
        const uint64_t *yv = val + (size_t)a->slot[_id(y)] * 4;
        uint64_t neg = (uint64_t)0 - (uint64_t)((x < 0) ^ (y < 0));
        for (uint32_t w = 0; w < 4; w++) val[(1 + j) * 4 + w] = xv[w] ^ yv[w] ^ neg;
    }
    for (uint32_t i = 0; i < ni; i++) {
        uint32_t g = internal[i], s = 1 + nl + i;
        a->slot[g] = s;
        dvs_aig_node_t l = a->L[g], r = a->R[g];
        const uint64_t *lv = val + (size_t)a->slot[_id(l)] * 4;
        const uint64_t *rv = val + (size_t)a->slot[_id(r)] * 4;
        uint64_t ln = l < 0 ? ~0ull : 0, rn = r < 0 ? ~0ull : 0;
        for (uint32_t w = 0; w < 4; w++) val[s * 4 + w] = (lv[w] ^ ln) & (rv[w] ^ rn);
    }
    const uint64_t *f = val + (size_t)(nl + ni) * 4;
    /* Affine iff f = f(0) + sum of coefficient_j * leaf_j on every lane. */
    uint8_t c0 = (uint8_t)(f[0] & 1u), coef[SM_LEAVES];
    uint32_t nc = 0;
    for (uint32_t k = 0; k < nind; k++) {
        uint32_t lane = 1u << k;
        coef[k] = (uint8_t)(((f[lane / 64] >> (lane % 64)) & 1u) ^ c0);
        nc += coef[k];
    }
    for (uint32_t w = 0; w < 4; w++) {
        uint64_t e = c0 ? ~0ull : 0;
        for (uint32_t k = 0; k < nind; k++) if (coef[k]) e ^= _sm_pat(k, w);
        if (e != f[w]) return 0;
    }
    a->sm_off[id] = a->sm_used;
    a->sm_n[id] = (uint8_t)nc;
    a->sm_c[id] = c0;
    for (uint32_t j = 0; j < nl; j++)
        if (!derived[j] && coef[ind_of[j]]) a->sm_pool[a->sm_used++] = leaves[j];
    a->n_small++;
    return 1;
}

/* AND(~p, ~q), p = AND(u1, u2), q = AND(~u1, ~u2): the node is u1 XOR u2. */
static void _find_xor(aff_t *a, uint32_t id) {
    dvs_aig_node_t l = a->L[id], r = a->R[id];
    if (!l || l >= 0 || r >= 0) return;
    uint32_t p = _id(l), q = _id(r);
    if (!a->L[p] || !a->L[q]) return;
    dvs_aig_node_t u1 = a->L[p], u2 = a->R[p], v1 = a->L[q], v2 = a->R[q];
    if ((v1 == -u1 && v2 == -u2) || (v1 == -u2 && v2 == -u1)) {
        a->xa[id] = u1;
        a->xb[id] = u2;
    }
}

/* Add literal `lit`'s form into (bits, *c). */
static void _add_lit(const aff_t *a, dvs_aig_node_t lit, uint64_t *bits, uint8_t *c) {
    uint32_t id = _id(lit);
    *c ^= (uint8_t)(lit < 0);
    if (id == 1) { *c ^= 1; return; }               /* TRUE */
    if (_gate_affine(a, id)) {
        const uint64_t *f = a->forms + (size_t)a->fidx[id] * a->W;
        for (uint32_t w = 0; w < a->W; w++) bits[w] ^= f[w];
        *c ^= a->fconst[a->fidx[id]];
        return;
    }
    int32_t k = a->col[id];
    bits[k / 64] ^= 1ull << (k % 64);
}

/* Visit the affine region below `lit`: XOR nodes (post-order into `order`)
 * and their leaves (columns). */
static int _region(aff_t *a, dvs_aig_node_t lit, uint8_t *seen, uint32_t *stack,
                   uint32_t *order, uint32_t *n_order) {
    uint32_t sp = 0;
    stack[sp++] = _id(lit) << 1;
    while (sp) {
        uint32_t e = stack[--sp], id = e >> 1;
        if (e & 1) { order[(*n_order)++] = id; continue; }
        if (seen[id]) continue;
        seen[id] = 1;
        if (id == 1) continue;
        if (a->sm_off[id] != SM_NONE) {
            stack[sp++] = (id << 1) | 1;
            for (uint32_t j = 0; j < a->sm_n[id]; j++)
                stack[sp++] = a->sm_pool[a->sm_off[id] + j] << 1;
            continue;
        }
        if (a->xa[id]) {
            stack[sp++] = (id << 1) | 1;
            stack[sp++] = _id(a->xa[id]) << 1;
            stack[sp++] = _id(a->xb[id]) << 1;
            continue;
        }
        a->col[id] = (int32_t)a->n_cols;
        a->col_node[a->n_cols++] = id;
        if (!a->L[id]) a->n_inputs++;
    }
    return 0;
}

/* Gauss–Jordan system: each pivot row has exactly one pivot column. */
typedef struct {
    uint32_t  W, n_rows;
    uint64_t *rows;      /* W words per row  */
    uint8_t  *rhs;
    int32_t  *pcol;      /* pivot column of each row */
    int32_t  *prow;      /* row of each pivot column, -1 none */
    uint64_t *pmask;     /* pivot columns    */
} gauss_t;

/* Reduce (bits, *c) by the pivots: afterwards it has no pivot column, and
 * bits·x + c keeps its value under the system. */
static void _reduce(aff_t *a, gauss_t *g, uint64_t *bits, uint8_t *c) {
    for (uint32_t w = 0; w < g->W; w++) {
        uint64_t m = bits[w] & g->pmask[w];
        while (m) {
            uint32_t k = w * 64 + (uint32_t)__builtin_ctzll(m);
            m &= m - 1;
            const uint64_t *p = g->rows + (size_t)g->prow[k] * g->W;
            for (uint32_t v = 0; v < g->W; v++) bits[v] ^= p[v];
            *c ^= g->rhs[g->prow[k]];
            a->work += g->W;
        }
    }
}

/* Add the equation bits·x = rhs. Returns 0, or -1 when it is inconsistent. */
static int _add_eq(aff_t *a, gauss_t *g, uint64_t *bits, uint8_t rhs) {
    _reduce(a, g, bits, &rhs);
    uint32_t w = 0;
    while (w < g->W && !bits[w]) w++;
    if (w == g->W) return rhs ? -1 : 0;
    uint32_t k = w * 64 + (uint32_t)__builtin_ctzll(bits[w]);
    uint64_t kb = 1ull << (k % 64);
    for (uint32_t r = 0; r < g->n_rows; r++) {
        uint64_t *p = g->rows + (size_t)r * g->W;
        if (p[k / 64] & kb) {
            for (uint32_t v = 0; v < g->W; v++) p[v] ^= bits[v];
            g->rhs[r] ^= rhs;
            a->work += g->W;
        }
    }
    uint32_t r = g->n_rows++;
    memcpy(g->rows + (size_t)r * g->W, bits, g->W * sizeof(uint64_t));
    g->rhs[r] = rhs;
    g->pcol[r] = (int32_t)k;
    g->prow[k] = (int32_t)r;
    g->pmask[k / 64] |= kb;
    return 0;
}

/* A copy of the system, to redo the clause choice from. */
typedef struct {
    uint32_t  n_rows;
    uint64_t *rows, *pmask;
    uint8_t  *rhs;
    int32_t  *pcol, *prow;
} gsnap_t;

static int _gsnap(const aff_t *a, const gauss_t *g, gsnap_t *s) {
    size_t nc = (size_t)a->n_cols + 1;
    s->n_rows = g->n_rows;
    s->rows = (uint64_t *)malloc(nc * g->W * sizeof(uint64_t));
    s->pmask = (uint64_t *)malloc(g->W * sizeof(uint64_t));
    s->rhs = (uint8_t *)malloc(nc);
    s->pcol = (int32_t *)malloc(nc * sizeof(int32_t));
    s->prow = (int32_t *)malloc(nc * sizeof(int32_t));
    if (!s->rows || !s->pmask || !s->rhs || !s->pcol || !s->prow) return -1;
    memcpy(s->rows, g->rows, (size_t)g->n_rows * g->W * sizeof(uint64_t));
    memcpy(s->pmask, g->pmask, g->W * sizeof(uint64_t));
    memcpy(s->rhs, g->rhs, g->n_rows);
    memcpy(s->pcol, g->pcol, (size_t)g->n_rows * sizeof(int32_t));
    memcpy(s->prow, g->prow, nc * sizeof(int32_t));
    return 0;
}

static void _grestore(const aff_t *a, gauss_t *g, const gsnap_t *s) {
    size_t nc = (size_t)a->n_cols + 1;
    g->n_rows = s->n_rows;
    memcpy(g->rows, s->rows, (size_t)s->n_rows * g->W * sizeof(uint64_t));
    memcpy(g->pmask, s->pmask, g->W * sizeof(uint64_t));
    memcpy(g->rhs, s->rhs, s->n_rows);
    memcpy(g->pcol, s->pcol, (size_t)s->n_rows * sizeof(int32_t));
    memcpy(g->prow, s->prow, nc * sizeof(int32_t));
}

static void _gsnap_free(gsnap_t *s) {
    free(s->rows); free(s->pmask); free(s->rhs); free(s->pcol); free(s->prow);
}

/* Literal (bits, c) under the system: 0 or 1 when forced, -1 when free. */
static int _lit_value(aff_t *a, gauss_t *g, dvs_aig_node_t lit, uint64_t *bits) {
    memset(bits, 0, a->W * sizeof(uint64_t));
    uint8_t c = 0;
    _add_lit(a, lit, bits, &c);
    _reduce(a, g, bits, &c);
    for (uint32_t w = 0; w < a->W; w++) if (bits[w]) return -1;
    return c;
}

/* A node's bit in a 64-bit support signature. Collisions only undercount, so
 * popcount(signature) is a lower bound on the leaves of a cut. */
static inline uint64_t _sig_bit(uint32_t id) {
    return 1ull << ((id * 0x9E3779B97F4A7C15ull) >> 58);
}

/* Small-table detection on the cone of the assertions, in id order. A gate is
 * tried only when an XOR or an assertion reads it and its support signature
 * allows a cut of at most SM_CUT leaves: sig1 stops at linear nodes (cut A),
 * sig2 goes through tables (cut B). */
static int _find_tables(aff_t *a, const dvs_aig_node_t *asserts, uint32_t n_asserts,
                        uint32_t *stack) {
    size_t n1 = (size_t)a->N + 1;
    uint8_t *cone = (uint8_t *)calloc(n1, 1);
    uint64_t *sig1 = (uint64_t *)calloc(n1, sizeof(uint64_t));
    uint64_t *sig2 = (uint64_t *)calloc(n1, sizeof(uint64_t));
    if (!cone || !sig1 || !sig2) { free(cone); free(sig1); free(sig2); return -1; }
    uint32_t sp = 0;
    for (uint32_t i = 0; i < n_asserts; i++) stack[sp++] = _id(asserts[i]);
    while (sp) {
        uint32_t id = stack[--sp];
        if (id <= 1 || cone[id]) continue;
        cone[id] = 1;
        if (a->L[id]) { stack[sp++] = _id(a->L[id]); stack[sp++] = _id(a->R[id]); }
    }
    /* Only a gate read by an XOR or asserted can be an opaque column, so only
     * those are tried; a table's inner mux steps never are (bit 2 of cone). */
    uint64_t n_cone = 0;
    for (uint32_t i = 0; i < n_asserts; i++) cone[_id(asserts[i])] |= 2;
    for (uint32_t id = 2; id <= a->N; id++) {
        n_cone += cone[id] != 0;
        if (cone[id] && a->xa[id]) { cone[_id(a->xa[id])] |= 2; cone[_id(a->xb[id])] |= 2; }
    }
    /* Linear in the cone: a CRC table circuit needs about two visits a node. */
    a->sm_budget = 3 * n_cone + 4096;
    a->lin[1] = 1;
    for (uint32_t id = 2; id <= a->N; id++) {
        if (!cone[id]) continue;
        if (!a->L[id] || (a->xa[id] && a->lin[_id(a->xa[id])] && a->lin[_id(a->xb[id])])) {
            a->lin[id] = 1;
            sig1[id] = sig2[id] = _sig_bit(id);
            continue;
        }
        uint32_t l = _id(a->L[id]), r = _id(a->R[id]);
        sig1[id] = sig1[l] | sig1[r];
        sig2[id] = sig2[l] | sig2[r];
        if (!(cone[id] & 2) || !a->sm_budget) continue;
        if ((__builtin_popcountll(sig1[id]) <= SM_CUT && _find_small(a, id, stack, 0))
            || (__builtin_popcountll(sig2[id]) <= SM_CUT && _find_small(a, id, stack, 1))) {
            a->lin[id] = 1;
            sig1[id] = _sig_bit(id);
        }
    }
    free(cone); free(sig1); free(sig2);
    return 0;
}

/* Make one literal of each clause 1, adding its equation. With seed 0 the
 * literals are taken in order, the first already 1 or free deciding (the
 * original choice). With a seed, a clause that already holds adds nothing,
 * and otherwise the literals are tried from a start drawn from the seed.
 * Returns 0, or -1 when a clause cannot be made 1. */
static int _settle(aff_t *a, gauss_t *g, uint32_t n_cl, const uint32_t *cl_start,
                   const dvs_aig_node_t *cl_lits, uint64_t *bits, uint64_t seed) {
    uint64_t cs = seed;
    for (uint32_t i = 0; i < n_cl; i++) {
        uint32_t n_l = cl_start[i + 1] - cl_start[i], j0 = 0;
        int done_cl = 0;
        for (uint32_t j = cl_start[i]; seed && j < cl_start[i + 1] && !done_cl; j++)
            done_cl = _lit_value(a, g, cl_lits[j], bits) == 1;
        if (seed && n_l > 1) {
            cs += 0x9e3779b97f4a7c15ull;      /* splitmix64 */
            uint64_t z = cs;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            j0 = (uint32_t)((z ^ (z >> 31)) % n_l);
        }
        for (uint32_t t = 0; t < n_l && !done_cl; t++) {
            uint32_t j = cl_start[i] + (j0 + t) % n_l;
            int v = _lit_value(a, g, cl_lits[j], bits);
            if (v == 1) done_cl = 1;
            else if (v < 0) {
                uint8_t c = 0;
                memset(bits, 0, a->W * sizeof(uint64_t));
                _add_lit(a, cl_lits[j], bits, &c);
                done_cl = _add_eq(a, g, bits, (uint8_t)(1 ^ c)) == 0;
            }
        }
        if (!done_cl) return -1;
    }
    return 0;
}

/* The model of the system: free input columns take bits drawn from `seed`
 * (0: all 0), the other free columns 0, and a pivot column its row's
 * right-hand side plus the free columns its row still holds; over a pure XOR
 * system a seed draws uniformly from the solutions. The gates are evaluated
 * on the real AIG. Returns the node valuation, or NULL when it violates an
 * assertion (or memory runs out). */
static int8_t *_model(const aff_t *a, const gauss_t *g, uint64_t seed,
                      const dvs_aig_node_t *asserts, uint32_t n_asserts, size_t n1) {
    int8_t *nv = (int8_t *)calloc(n1, 1);
    uint64_t *fv = (uint64_t *)calloc(a->W ? a->W : 1, sizeof(uint64_t));
    if (!nv || !fv) { free(nv); free(fv); return NULL; }
    if (a->N >= 1) nv[1] = 1;
    uint64_t rs = seed;
    for (uint32_t k = 0; seed && k < a->n_cols; k++) {
        uint32_t id = a->col_node[k];
        if (a->L[id] || (g->pmask[k / 64] >> (k % 64) & 1)) continue;
        rs += 0x9e3779b97f4a7c15ull;      /* splitmix64 */
        uint64_t z = rs;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        if ((z ^ (z >> 31)) & 1) {
            fv[k / 64] |= 1ull << (k % 64);
            nv[id] = 1;
        }
    }
    for (uint32_t r = 0; r < g->n_rows; r++) {
        uint32_t id = a->col_node[g->pcol[r]];
        if (a->L[id]) continue;
        const uint64_t *p = g->rows + (size_t)r * g->W;
        uint32_t par = 0;
        for (uint32_t w = 0; w < g->W; w++) par ^= (uint32_t)__builtin_popcountll(p[w] & fv[w]) & 1u;
        nv[id] = (int8_t)(g->rhs[r] ^ par);
    }
    free(fv);
    for (uint32_t id = 2; id <= a->N; id++) {
        if (!a->L[id]) continue;
        dvs_aig_node_t l = a->L[id], r = a->R[id];
        int lv = l < 0 ? !nv[-l] : nv[l], rv = r < 0 ? !nv[-r] : nv[r];
        nv[id] = (int8_t)(lv & rv);
    }
    for (uint32_t i = 0; i < n_asserts; i++) {
        dvs_aig_node_t lit = asserts[i];
        if (!(lit < 0 ? !nv[-lit] : nv[lit])) { free(nv); return NULL; }
    }
    return nv;
}

static int _check(const dvs_aig_t *m, const dvs_aig_node_t *asserts,
                  uint32_t n_asserts, uint64_t max_work, int tables, uint64_t seed,
                  int8_t **model_out, dvs_affine_stats_t *st) {
    dvs_affine_stats_t dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);
    if (model_out) *model_out = NULL;
    uint64_t nn = dvs_aig_num_nodes(m);
    if (nn == 0 || nn > 0x3FFFFFF0ull || !n_asserts) { st->why = "empty"; return 0; }

    aff_t a;
    memset(&a, 0, sizeof a);
    a.N = (uint32_t)nn;
    a.max_work = max_work ? max_work : 40000000ull;
    int verdict = DVS_AFFINE_UNDECIDED;
    st->why = "out of memory";

    uint32_t *stack = NULL, *order = NULL, *conj = NULL, *cl_start = NULL, *cmark = NULL;
    dvs_aig_node_t *eqs = NULL, *cl_lits = NULL;
    uint8_t *seen = NULL;
    uint64_t *bits = NULL;
    gauss_t g;
    memset(&g, 0, sizeof g);

    size_t n1 = (size_t)a.N + 1;
    a.L = (dvs_aig_node_t *)calloc(n1, sizeof(dvs_aig_node_t));
    a.R = (dvs_aig_node_t *)calloc(n1, sizeof(dvs_aig_node_t));
    a.xa = (dvs_aig_node_t *)calloc(n1, sizeof(dvs_aig_node_t));
    a.xb = (dvs_aig_node_t *)calloc(n1, sizeof(dvs_aig_node_t));
    a.col = (int32_t *)malloc(n1 * sizeof(int32_t));
    a.col_node = (uint32_t *)malloc(n1 * sizeof(uint32_t));
    a.fidx = (uint32_t *)malloc(n1 * sizeof(uint32_t));
    seen = (uint8_t *)calloc(n1, 1);
    stack = (uint32_t *)malloc((SM_LEAVES + 2) * n1 * sizeof(uint32_t));
    a.sm_off = (uint32_t *)malloc(n1 * sizeof(uint32_t));
    a.sm_n = (uint8_t *)calloc(n1, 1);
    a.sm_c = (uint8_t *)calloc(n1, 1);
    a.sm_pool = (uint32_t *)malloc(SM_LEAVES * n1 * sizeof(uint32_t));
    a.smark = (uint32_t *)calloc(n1, sizeof(uint32_t));
    a.lin = (uint8_t *)calloc(n1, 1);
    a.slot = (uint32_t *)malloc(n1 * sizeof(uint32_t));
    order = (uint32_t *)malloc(n1 * sizeof(uint32_t));
    eqs = (dvs_aig_node_t *)malloc(2 * n1 * sizeof(dvs_aig_node_t));
    cl_lits = (dvs_aig_node_t *)malloc(2 * n1 * sizeof(dvs_aig_node_t));
    cl_start = (uint32_t *)malloc((2 * n1 + 1) * sizeof(uint32_t));
    conj = (uint32_t *)malloc((2 * n1 + n_asserts) * sizeof(uint32_t));
    cmark = (uint32_t *)calloc(n1, sizeof(uint32_t));
    if (!a.L || !a.R || !a.xa || !a.xb || !a.col || !a.col_node || !a.fidx || !seen
        || !stack || !order || !eqs || !cl_lits || !cl_start || !conj || !cmark
        || !a.sm_off || !a.sm_n || !a.sm_c || !a.sm_pool || !a.smark || !a.slot || !a.lin)
        goto done;
    for (uint32_t id = 2; id <= a.N; id++)
        if (dvs_aig_is_and(m, (dvs_aig_node_t)id))
            dvs_aig_get_children(m, (dvs_aig_node_t)id, &a.L[id], &a.R[id]);
    for (uint32_t id = 2; id <= a.N; id++) _find_xor(&a, id);
    memset(a.sm_off, 0xFF, n1 * sizeof(uint32_t));
    if (tables && _find_tables(&a, asserts, n_asserts, stack) != 0) goto done;
    memset(a.col, 0xFF, n1 * sizeof(int32_t));

    /* Conjuncts: flatten positive non-XOR ANDs. A negated non-XOR AND is a
     * clause: flatten its AND tree, and each leaf, negated, is a literal. The
     * rest are equations. `seen` (bit 1) dedups conjunct literals. */
    uint32_t n_eq = 0, n_cl = 0, n_cll = 0, sp = 0;
    for (uint32_t i = 0; i < n_asserts; i++) {
        dvs_aig_node_t root = asserts[i];
        if (root == DVS_AIG_TRUE) continue;
        if (root == DVS_AIG_FALSE) { verdict = DVS_AFFINE_UNSAT; st->why = "false assertion"; goto done; }
        conj[sp++] = (uint32_t)root;
        while (sp) {
            dvs_aig_node_t lit = (dvs_aig_node_t)conj[--sp];
            uint32_t id = _id(lit);
            if (lit == DVS_AIG_TRUE) continue;
            if (lit == DVS_AIG_FALSE) { verdict = DVS_AFFINE_UNSAT; st->why = "false assertion"; goto done; }
            uint8_t mark = lit > 0 ? 2 : 4;
            if (seen[id] & mark) continue;
            seen[id] |= mark;
            if (a.L[id] && !_gate_affine(&a, id) && lit > 0) {
                conj[sp++] = (uint32_t)a.L[id];
                conj[sp++] = (uint32_t)a.R[id];
            } else if (a.L[id] && !_gate_affine(&a, id)) {
                /* Clause: the leaves of the AND tree under `id`, negated
                 * (cmark dedups within the clause). */
                cl_start[n_cl++] = n_cll;
                uint32_t s2 = 0;
                stack[s2++] = id;
                while (s2) {
                    dvs_aig_node_t c = (dvs_aig_node_t)stack[--s2];
                    if (c > 0 && a.L[c] && !_gate_affine(&a, (uint32_t)c)) {
                        if (cmark[c] == n_cl) continue;
                        cmark[c] = n_cl;
                        stack[s2++] = (uint32_t)a.L[c];
                        stack[s2++] = (uint32_t)a.R[c];
                    } else if (c != DVS_AIG_TRUE) {
                        if (n_cll >= 2 * n1) { st->why = "clauses too large"; goto done; }
                        cl_lits[n_cll++] = -c;   /* FALSE child: literal TRUE */
                    }
                }
            } else {
                eqs[n_eq++] = lit;
            }
        }
    }
    cl_start[n_cl] = n_cll;
    st->equations = n_eq;
    st->clauses = n_cl;
    memset(seen, 0, n1);

    /* The affine region: forms for XOR nodes, columns for the leaves. */
    uint32_t n_order = 0;
    for (uint32_t i = 0; i < n_eq; i++)
        _region(&a, eqs[i], seen, stack, order, &n_order);
    for (uint32_t i = 0; i < n_cll; i++)
        _region(&a, cl_lits[i], seen, stack, order, &n_order);
    st->xors = n_order;
    st->tables = a.n_small;
    st->inputs = a.n_inputs;
    st->opaque = a.n_cols - a.n_inputs;
    if (!n_order) { st->why = "no XOR"; goto done; }
    a.W = (a.n_cols + 63) / 64;
    if (!a.W) a.W = 1;
    a.work = (uint64_t)n_order * a.W;
    if (a.work > a.max_work) { st->why = "region too large"; goto done; }
    a.forms = (uint64_t *)calloc((size_t)n_order * a.W, sizeof(uint64_t));
    a.fconst = (uint8_t *)calloc(n_order, 1);
    bits = (uint64_t *)malloc(a.W * sizeof(uint64_t));
    if (!a.forms || !a.fconst || !bits) goto done;
    for (uint32_t k = 0; k < n_order; k++) {
        uint32_t id = order[k];
        a.fidx[id] = k;
        uint64_t *f = a.forms + (size_t)k * a.W;
        if (a.sm_off[id] == SM_NONE) {
            _add_lit(&a, a.xa[id], f, &a.fconst[k]);
            _add_lit(&a, a.xb[id], f, &a.fconst[k]);
        } else {
            a.fconst[k] = a.sm_c[id];
            for (uint32_t j = 0; j < a.sm_n[id]; j++)
                _add_lit(&a, (dvs_aig_node_t)a.sm_pool[a.sm_off[id] + j], f, &a.fconst[k]);
        }
    }

    /* Eliminate. */
    g.W = a.W;
    g.rows = (uint64_t *)malloc(((size_t)a.n_cols + 1) * a.W * sizeof(uint64_t));
    g.rhs = (uint8_t *)malloc((size_t)a.n_cols + 1);
    g.pcol = (int32_t *)malloc(((size_t)a.n_cols + 1) * sizeof(int32_t));
    g.prow = (int32_t *)malloc(((size_t)a.n_cols + 1) * sizeof(int32_t));
    g.pmask = (uint64_t *)calloc(a.W, sizeof(uint64_t));
    if (!g.rows || !g.rhs || !g.pcol || !g.prow || !g.pmask) goto done;
    for (uint32_t i = 0; i < n_eq; i++) {
        memset(bits, 0, a.W * sizeof(uint64_t));
        uint8_t c = 0;
        _add_lit(&a, eqs[i], bits, &c);
        if (_add_eq(&a, &g, bits, (uint8_t)(1 ^ c)) != 0) {
            verdict = DVS_AFFINE_UNSAT;
            st->why = "inconsistent equations";
            goto done;
        }
        if (a.work > a.max_work) { st->why = "elimination too large"; goto done; }
    }
    for (uint32_t i = 0; i < n_cl; i++) {
        int all_false = 1;
        for (uint32_t j = cl_start[i]; j < cl_start[i + 1] && all_false; j++)
            if (_lit_value(&a, &g, cl_lits[j], bits) != 0) all_false = 0;
        if (all_false) {
            verdict = DVS_AFFINE_UNSAT;
            st->why = "clause forced false";
            goto done;
        }
        if (a.work > a.max_work) { st->why = "elimination too large"; goto done; }
    }

    /* A candidate model: one literal per clause made 1, greedily, first
     * literal first, and free columns 0. Whether that model holds on the real
     * AIG decides whether the analysis answers, independently of the seed:
     * when it does not, simulation or SAT decide as before. With a seed, the
     * returned model is then redrawn: each clause's literals tried from a
     * seeded start, free input bits drawn from the seed. If the redrawn model
     * does not hold, the first one stands. */
    {
        gsnap_t snap;
        memset(&snap, 0, sizeof snap);
        int redo = seed && n_cl && _gsnap(&a, &g, &snap) == 0;
        if (_settle(&a, &g, n_cl, cl_start, cl_lits, bits, 0) != 0) {
            _gsnap_free(&snap);
            st->why = "greedy clause choice failed";
            goto done;
        }
        st->rank = g.n_rows;
        int8_t *nv = _model(&a, &g, 0, asserts, n_asserts, n1);
        if (!nv) {
            _gsnap_free(&snap);
            st->why = "model fails on the AIG";
            goto done;
        }
        if (seed) {
            if (redo) {
                _grestore(&a, &g, &snap);
                if (_settle(&a, &g, n_cl, cl_start, cl_lits, bits, seed) != 0) {
                    _grestore(&a, &g, &snap);
                    _settle(&a, &g, n_cl, cl_start, cl_lits, bits, 0);
                }
            }
            int8_t *sv = _model(&a, &g, seed, asserts, n_asserts, n1);
            if (sv) { free(nv); nv = sv; }
        }
        _gsnap_free(&snap);
        verdict = DVS_AFFINE_SAT;
        st->why = "model";
        if (model_out) *model_out = nv; else free(nv);
    }

done:
    if (verdict != DVS_AFFINE_UNDECIDED) st->rank = g.n_rows;
    free(a.L); free(a.R); free(a.xa); free(a.xb); free(a.col); free(a.col_node);
    free(a.fidx); free(a.sm_off); free(a.sm_n); free(a.sm_c); free(a.sm_pool);
    free(a.smark); free(a.slot); free(a.lin); free(a.forms); free(a.fconst); free(seen); free(stack); free(order);
    free(eqs); free(cl_lits); free(cl_start); free(conj); free(cmark); free(bits);
    free(g.rows); free(g.rhs); free(g.pcol); free(g.prow); free(g.pmask);
    return verdict;
}

int dvs_affine_check(const dvs_aig_t *m, const dvs_aig_node_t *asserts,
                     uint32_t n_asserts, uint64_t max_work, uint64_t seed,
                     int8_t **model_out, dvs_affine_stats_t *st) {
    /* XORs alone first; tables cost a pass over the cone, so look for them
     * only when gates the XOR pass could not read are left over. */
    dvs_affine_stats_t local;
    if (!st) st = &local;
    int r = _check(m, asserts, n_asserts, max_work, 0, seed, model_out, st);
    if (r != DVS_AFFINE_UNDECIDED || !st->opaque || !st->xors) return r;
    return _check(m, asserts, n_asserts, max_work, 1, seed, model_out, st);
}
