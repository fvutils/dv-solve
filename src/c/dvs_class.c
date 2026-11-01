/*
 * dvs_class -- the canonical shape class of a problem. See dvs_class.h.
 *
 * Every expression node gets a hash from its operator, its own widths and
 * flags, and its operands' hashes; operands of commutative operators, set
 * members, ranges and summands are sorted first, and `a > b` is hashed as
 * `b < a`. A constant contributes only its class and width. A variable
 * contributes a colour, computed in two rounds:
 *
 *   1. its type: width, signedness, kind, solve-order stage, and the classes
 *      of its declared bounds;
 *   2. that, plus the round-1 hashes of every top-level constraint, soft
 *      constraint, all-different and dist it appears in.
 *
 * Round 2 tells apart problems whose constraints match one for one but share
 * variables differently (x < y, y < z against x < y, z < w), as one step of
 * colour refinement. The class is the sorted round-2 hashes of the top-level
 * conjuncts and the other specs, plus the sorted variable colours. Every walk
 * is iterative: problems from SMT-LIB tools can nest a million levels deep.
 */
#include "dvs_class.h"
#include "dvs_shape.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Distinct nodes hashed (and constraint-to-variable visits made) before the
 * walk gives up and marks the result truncated. */
#define CLASS_NODE_CAP 4000000u

enum {
    K_CONST = 1, K_CONST_WIDE, K_VAR, K_VAR_UNDECL, K_BIN, K_UN, K_ITE, K_IN_RANGE,
    K_IN_SET, K_IN_RANGES, K_EXTEND, K_SV_CAST, K_CAST, K_EXTRACT, K_CONCAT, K_SUM,
    K_COUNTONES, K_CLOG2, K_ARRAY_SELECT, K_UNKNOWN, K_TRUNC, K_HARD, K_SOFT,
    K_ALLDIFF, K_DIST, K_SOURCE, K_VARS, K_CTX, K_CTX_ARRAY, K_PROBLEM
};

static inline uint64_t _fmix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static inline uint64_t _mix(uint64_t h, uint64_t v) {
    return _fmix(h ^ (v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2)));
}

static int _cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Order-free: sort, then combine in order. Sorts `v` in place. */
static uint64_t _combine_sorted(uint64_t h, uint64_t *v, uint32_t n) {
    if (n > 1) qsort(v, n, sizeof *v, _cmp_u64);
    h = _mix(h, n);
    for (uint32_t i = 0; i < n; i++) h = _mix(h, v[i]);
    return h;
}

/* ------------------------------------------------------------------ */
/* Constant classes                                                    */
/* ------------------------------------------------------------------ */

enum { C_ZERO = 1, C_ONE, C_ALL_ONES, C_POW2, C_SMALL, C_SMALL_NEG, C_OTHER,
       C_TYPE_MIN, C_TYPE_MAX };

static uint64_t _mask(uint32_t width) {
    return width == 0 || width >= 64 ? ~0ULL : (1ULL << width) - 1;
}

static unsigned _cclass(uint64_t bits, uint32_t width, int is_signed) {
    uint64_t m = _mask(width);
    bits &= m;
    if (bits == 0)                return C_ZERO;
    if (bits == 1)                return C_ONE;
    if (bits == m)                return C_ALL_ONES;
    if ((bits & (bits - 1)) == 0) return C_POW2;
    if (bits < 256)               return C_SMALL;
    if (is_signed && width > 0 && width < 64) {
        /* sign-extend from `width` */
        int64_t s = (int64_t)(bits << (64 - width)) >> (64 - width);
        if (s < 0 && s > -256) return C_SMALL_NEG;
    } else if (is_signed && (int64_t)bits < 0 && (int64_t)bits > -256) {
        return C_SMALL_NEG;
    }
    return C_OTHER;
}

static unsigned _cclass_wide(const ExprConstWide *c) {
    uint32_t n = c->n_limbs, top = c->width % 64u;
    uint64_t top_mask = top ? (1ULL << top) - 1 : ~0ULL;
    int zero = 1, ones = 1, pop1 = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t l = c->limbs[i], m = i + 1 == n ? top_mask : ~0ULL;
        if (l) zero = 0;
        if (l != m) ones = 0;
        pop1 += __builtin_popcountll(l);
    }
    int high = 0;
    for (uint32_t i = 1; i < n; i++) high |= c->limbs[i] != 0;
    if (zero)               return C_ZERO;
    if (!high && c->limbs[0] == 1) return C_ONE;
    if (ones)               return C_ALL_ONES;
    if (pop1 == 1)          return C_POW2;
    if (!high && c->limbs[0] < 256) return C_SMALL;
    return C_OTHER;
}

/* A declared bound: its class, or the type's own minimum or maximum. */
static unsigned _bound_class(int64_t v, uint32_t width, int is_signed) {
    if (width > 0 && width < 64) {
        if (is_signed) {
            int64_t hi = (int64_t)((1ULL << (width - 1)) - 1);
            if (v == hi)      return C_TYPE_MAX;
            if (v == -hi - 1) return C_TYPE_MIN;
        } else if ((uint64_t)v == _mask(width)) {
            return C_TYPE_MAX;
        }
    } else if (width >= 64) {
        if (v == INT64_MAX || (!is_signed && v == -1)) return C_TYPE_MAX;
        if (is_signed && v == INT64_MIN)                return C_TYPE_MIN;
    }
    return _cclass((uint64_t)v, width, is_signed);
}

/* ------------------------------------------------------------------ */
/* Node -> hash map                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    dvs_expr_t *key;
    uint64_t   *val;
    uint32_t    cap, n;
} Memo;

static int _memo_init(Memo *m, uint32_t cap) {
    m->cap = cap;
    m->n = 0;
    m->key = (dvs_expr_t *)malloc((size_t)cap * sizeof *m->key);
    m->val = (uint64_t *)malloc((size_t)cap * sizeof *m->val);
    if (!m->key || !m->val) return -1;
    memset(m->key, 0xFF, (size_t)cap * sizeof *m->key);   /* EXPR_NULL: empty */
    return 0;
}

static void _memo_free(Memo *m) {
    free(m->key);
    free(m->val);
    memset(m, 0, sizeof *m);
}

static void _memo_clear(Memo *m) {
    memset(m->key, 0xFF, (size_t)m->cap * sizeof *m->key);
    m->n = 0;
}

static inline uint32_t _slot(const Memo *m, dvs_expr_t r) {
    uint32_t i = (uint32_t)_fmix(r) & (m->cap - 1);
    while (m->key[i] != EXPR_NULL && m->key[i] != r) i = (i + 1) & (m->cap - 1);
    return i;
}

static inline int _memo_get(const Memo *m, dvs_expr_t r, uint64_t *v) {
    uint32_t i = _slot(m, r);
    if (m->key[i] == EXPR_NULL) return 0;
    *v = m->val[i];
    return 1;
}

static int _memo_put(Memo *m, dvs_expr_t r, uint64_t v) {
    if ((m->n + 1) * 2 > m->cap) {
        Memo g;
        if (_memo_init(&g, m->cap * 2) < 0) { _memo_free(&g); return -1; }
        for (uint32_t i = 0; i < m->cap; i++)
            if (m->key[i] != EXPR_NULL) {
                uint32_t j = _slot(&g, m->key[i]);
                g.key[j] = m->key[i];
                g.val[j] = m->val[i];
                g.n++;
            }
        _memo_free(m);
        *m = g;
    }
    uint32_t i = _slot(m, r);
    if (m->key[i] == EXPR_NULL) m->n++;
    m->key[i] = r;
    m->val[i] = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Growable arrays                                                     */
/* ------------------------------------------------------------------ */

typedef struct { dvs_expr_t *v; uint32_t n, cap; } RefVec;
typedef struct { uint64_t *v; uint32_t n, cap; } U64Vec;

static int _ref_push(RefVec *a, dvs_expr_t r) {
    if (a->n == a->cap) {
        uint32_t nc = a->cap ? a->cap * 2 : 64;
        dvs_expr_t *nv = (dvs_expr_t *)realloc(a->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n++] = r;
    return 0;
}

static int _u64_push(U64Vec *a, uint64_t x) {
    if (a->n == a->cap) {
        uint32_t nc = a->cap ? a->cap * 2 : 64;
        uint64_t *nv = (uint64_t *)realloc(a->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n++] = x;
    return 0;
}

/* ------------------------------------------------------------------ */
/* The walk                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const dvs_problem_t *p;
    uint32_t  n_ids;        /* variable ids below this are declared */
    uint64_t *sig;          /* round-1 colour per variable id */
    uint64_t *color;        /* the colour in use: sig, then the refined one */
    uint64_t *acc;          /* per id: the sum of its constraint contexts */
    Memo      memo;         /* node -> hash, for the current round */
    Memo      seen;         /* node -> visit stamp, for the context walks */
    RefVec    stack;        /* post-order stack (refs) */
    RefVec    phase;        /* 0 expand, 1 compute, per stack entry */
    RefVec    kids;
    U64Vec    tmp;
    uint64_t  work;         /* node hashes plus context visits */
    int       truncated;
    uint32_t *uf;           /* union-find over variable ids (components) */
    Memo      depth;        /* node -> depth | guard depth << 32 */
} Walk;

static uint32_t _uf_find(uint32_t *uf, uint32_t x) {
    while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; }
    return x;
}

static void _uf_union(uint32_t *uf, uint32_t a, uint32_t b) {
    a = _uf_find(uf, a);
    b = _uf_find(uf, b);
    if (a != b) uf[a < b ? b : a] = a < b ? a : b;
}

static const void *_node(const dvs_problem_t *p, dvs_expr_t r) {
    return dvs_pool_ptr((dvs_pool_t *)&p->pool, r);
}

static ExprKind _kind(const dvs_problem_t *p, dvs_expr_t r) {
    return *(const ExprKind *)_node(p, r);
}

/* The operand refs of `r`, in a fixed order, into w->kids. */
static int _kids(Walk *w, dvs_expr_t r) {
    const dvs_problem_t *p = w->p;
    dvs_problem_t *pm = (dvs_problem_t *)p;
    const void *n = _node(p, r);
    RefVec *k = &w->kids;
    k->n = 0;
    switch (*(const ExprKind *)n) {
    case EXPR_BINARY: {
        const ExprBinary *b = (const ExprBinary *)n;
        return _ref_push(k, b->lhs) | _ref_push(k, b->rhs);
    }
    case EXPR_UNARY:   return _ref_push(k, ((const ExprUnary *)n)->operand);
    case EXPR_ITE: {
        const ExprITE *e = (const ExprITE *)n;
        return _ref_push(k, e->cond) | _ref_push(k, e->then_e) | _ref_push(k, e->else_e);
    }
    case EXPR_IN_RANGE: {
        const ExprInRange *e = (const ExprInRange *)n;
        return _ref_push(k, e->value) | _ref_push(k, e->lo) | _ref_push(k, e->hi);
    }
    case EXPR_IN_SET: {
        const ExprInSet *e = (const ExprInSet *)n;
        int rc = _ref_push(k, e->value);
        const dvs_expr_t *el = expr_in_set_elems(pm, r);
        for (uint32_t i = 0; rc == 0 && i < e->n_elems; i++) rc = _ref_push(k, el[i]);
        return rc;
    }
    case EXPR_IN_RANGES: {
        const ExprInRanges *e = (const ExprInRanges *)n;
        int rc = _ref_push(k, e->value);
        const dvs_expr_t *lo = expr_in_ranges_los(pm, r), *hi = expr_in_ranges_his(pm, r);
        for (uint32_t i = 0; rc == 0 && i < e->n_ranges; i++) rc = _ref_push(k, lo[i]);
        for (uint32_t i = 0; rc == 0 && i < e->n_ranges; i++) rc = _ref_push(k, hi[i]);
        return rc;
    }
    case EXPR_EXTEND:  return _ref_push(k, ((const ExprExtend *)n)->operand);
    case EXPR_SV_CAST: return _ref_push(k, ((const ExprSvCast *)n)->operand);
    case EXPR_CAST:    return _ref_push(k, ((const ExprCast *)n)->operand);
    case EXPR_EXTRACT: return _ref_push(k, ((const ExprExtract *)n)->operand);
    case EXPR_CONCAT: {
        const ExprConcat *e = (const ExprConcat *)n;
        return _ref_push(k, e->hi) | _ref_push(k, e->lo);
    }
    case EXPR_SUM: {
        const ExprSum *e = (const ExprSum *)n;
        int rc = _ref_push(k, e->result);
        const dvs_expr_t *refs = (const dvs_expr_t *)((const char *)e + sizeof(ExprSum));
        for (uint32_t i = 0; rc == 0 && i < e->n_vars; i++) rc = _ref_push(k, refs[i]);
        return rc;
    }
    case EXPR_COUNTONES: {
        const ExprCountones *e = (const ExprCountones *)n;
        return _ref_push(k, e->result) | _ref_push(k, e->operand);
    }
    case EXPR_CLOG2: {
        const ExprClog2 *e = (const ExprClog2 *)n;
        return _ref_push(k, e->result) | _ref_push(k, e->operand);
    }
    case EXPR_ARRAY_SELECT: {
        const ExprArraySelect *e = (const ExprArraySelect *)n;
        return _ref_push(k, e->result) | _ref_push(k, e->index);
    }
    default:
        return 0;   /* constants and variables are leaves */
    }
}

static uint64_t _var_color(const Walk *w, uint32_t id) {
    return id < w->n_ids ? w->color[id] : _mix(K_VAR_UNDECL, 0);
}

static int _commutative(dvs_binop_t op) {
    switch (op) {
    case DVS_BIN_ADD: case DVS_BIN_MUL: case DVS_BIN_BAND: case DVS_BIN_BOR:
    case DVS_BIN_BXOR: case DVS_BIN_EQ: case DVS_BIN_NEQ: case DVS_BIN_AND:
    case DVS_BIN_OR:
        return 1;
    default:
        return 0;
    }
}

/* The hash of `r`, whose operands (w->kids, filled for r) are all in memo. */
static int _hash_node(Walk *w, dvs_expr_t r, uint64_t *out) {
    const dvs_problem_t *p = w->p;
    const void *n = _node(p, r);
    ExprKind kind = *(const ExprKind *)n;
    U64Vec *t = &w->tmp;
    t->n = 0;
    for (uint32_t i = 0; i < w->kids.n; i++) {
        uint64_t h = _mix(K_TRUNC, 0);
        if (w->kids.v[i] != EXPR_NULL) (void)_memo_get(&w->memo, w->kids.v[i], &h);
        if (_u64_push(t, h) < 0) return -1;
    }
    uint64_t *kh = t->v;
    uint64_t h;
    switch (kind) {
    case EXPR_CONST: {
        const ExprConst *c = (const ExprConst *)n;
        h = _mix(_mix(_mix(K_CONST, c->width), c->is_signed),
                 _cclass((uint64_t)c->value, c->width, c->is_signed));
        break;
    }
    case EXPR_CONST_WIDE: {
        const ExprConstWide *c = (const ExprConstWide *)n;
        h = _mix(_mix(_mix(K_CONST_WIDE, c->width), c->is_signed), _cclass_wide(c));
        break;
    }
    case EXPR_VAR:
        h = _mix(K_VAR, _var_color(w, ((const ExprVar *)n)->var_id));
        break;
    case EXPR_BINARY: {
        dvs_binop_t op = ((const ExprBinary *)n)->op;
        uint64_t a = kh[0], b = kh[1];
        /* a > b is b < a; a >= b is b <= a */
        if (op == DVS_BIN_GT)  { op = DVS_BIN_LT;  a = kh[1]; b = kh[0]; }
        if (op == DVS_BIN_GTE) { op = DVS_BIN_LTE; a = kh[1]; b = kh[0]; }
        if (_commutative(op) && a > b) { uint64_t x = a; a = b; b = x; }
        h = _mix(_mix(_mix(K_BIN, (uint64_t)op), a), b);
        break;
    }
    case EXPR_UNARY:
        h = _mix(_mix(K_UN, (uint64_t)((const ExprUnary *)n)->op), kh[0]);
        break;
    case EXPR_ITE:
        h = _mix(_mix(_mix(K_ITE, kh[0]), kh[1]), kh[2]);
        break;
    case EXPR_IN_RANGE:
        h = _mix(_mix(_mix(K_IN_RANGE, kh[0]), kh[1]), kh[2]);
        break;
    case EXPR_IN_SET:
        h = _combine_sorted(_mix(K_IN_SET, kh[0]), kh + 1, t->n - 1);
        break;
    case EXPR_IN_RANGES: {
        uint32_t nr = (t->n - 1) / 2;
        for (uint32_t i = 0; i < nr; i++) kh[1 + i] = _mix(kh[1 + i], kh[1 + nr + i]);
        h = _combine_sorted(_mix(K_IN_RANGES, kh[0]), kh + 1, nr);
        break;
    }
    case EXPR_EXTEND: {
        const ExprExtend *e = (const ExprExtend *)n;
        h = _mix(_mix(_mix(_mix(K_EXTEND, e->sign_extend), e->from_bits), e->to_bits), kh[0]);
        break;
    }
    case EXPR_SV_CAST: {
        const ExprSvCast *e = (const ExprSvCast *)n;
        h = _mix(_mix(_mix(_mix(_mix(K_SV_CAST, e->sign_extend), e->dst_signed),
                           e->from_bits), e->to_bits), kh[0]);
        break;
    }
    case EXPR_CAST: {
        const ExprCast *e = (const ExprCast *)n;
        h = _mix(_mix(_mix(K_CAST, e->to_bits), e->to_signed), kh[0]);
        break;
    }
    case EXPR_EXTRACT: {
        const ExprExtract *e = (const ExprExtract *)n;
        h = _mix(_mix(_mix(K_EXTRACT, e->hi_bit), e->lo_bit), kh[0]);
        break;
    }
    case EXPR_CONCAT:
        h = _mix(_mix(_mix(K_CONCAT, ((const ExprConcat *)n)->lo_width), kh[0]), kh[1]);
        break;
    case EXPR_SUM:
        h = _combine_sorted(_mix(K_SUM, kh[0]), kh + 1, t->n - 1);
        break;
    case EXPR_COUNTONES:
        h = _mix(_mix(K_COUNTONES, kh[0]), kh[1]);
        break;
    case EXPR_CLOG2:
        h = _mix(_mix(K_CLOG2, kh[0]), kh[1]);
        break;
    case EXPR_ARRAY_SELECT: {
        const ExprArraySelect *e = (const ExprArraySelect *)n;
        h = _mix(_mix(_mix(K_ARRAY_SELECT, e->n_elems), kh[0]), kh[1]);
        for (uint32_t i = 0; i < e->n_elems; i++)          /* element order matters */
            h = _mix(h, _var_color(w, e->base_var_id + i));
        break;
    }
    default:
        h = _mix(K_UNKNOWN, (uint64_t)kind);
        break;
    }
    *out = h;
    return 0;
}

/* Hash `root` and everything under it into w->memo (post-order, iterative). */
static int _hash_tree(Walk *w, dvs_expr_t root, uint64_t *out) {
    uint64_t h;
    if (root == EXPR_NULL) { *out = _mix(K_TRUNC, 1); return 0; }
    if (_memo_get(&w->memo, root, &h)) { *out = h; return 0; }
    w->stack.n = w->phase.n = 0;
    if (_ref_push(&w->stack, root) < 0 || _ref_push(&w->phase, 0) < 0) return -1;
    while (w->stack.n) {
        dvs_expr_t r = w->stack.v[w->stack.n - 1];
        uint32_t ph = w->phase.v[w->phase.n - 1];
        if (_memo_get(&w->memo, r, &h)) { w->stack.n--; w->phase.n--; continue; }
        if (w->work >= CLASS_NODE_CAP) {
            w->truncated = 1;
            if (_memo_put(&w->memo, r, _mix(K_TRUNC, 2)) < 0) return -1;
            w->stack.n--; w->phase.n--;
            continue;
        }
        if (_kids(w, r) < 0) return -1;
        if (ph == 0) {
            w->phase.v[w->phase.n - 1] = 1;
            uint32_t nk = w->kids.n;
            for (uint32_t i = 0; i < nk; i++) {
                dvs_expr_t c = w->kids.v[i];
                if (c == EXPR_NULL || _memo_get(&w->memo, c, &h)) continue;
                if (_ref_push(&w->stack, c) < 0 || _ref_push(&w->phase, 0) < 0) return -1;
            }
            continue;
        }
        if (_hash_node(w, r, &h) < 0 || _memo_put(&w->memo, r, h) < 0) return -1;
        w->work++;
        w->stack.n--; w->phase.n--;
    }
    return _memo_get(&w->memo, root, out) ? 0 : -1;
}

/* Add context `ctx` to every variable under `root` (each once per root). */
static int _context(Walk *w, dvs_expr_t root, uint64_t ctx, uint64_t stamp) {
    if (root == EXPR_NULL) return 0;
    uint32_t anchor = UINT32_MAX;   /* the first variable met: the rest join it */
    w->stack.n = 0;
    if (_ref_push(&w->stack, root) < 0) return -1;
    while (w->stack.n) {
        dvs_expr_t r = w->stack.v[--w->stack.n];
        uint64_t s;
        if (_memo_get(&w->seen, r, &s) && s == stamp) continue;
        if (_memo_put(&w->seen, r, stamp) < 0) return -1;
        if (++w->work >= CLASS_NODE_CAP) { w->truncated = 1; return 0; }
        const void *n = _node(w->p, r);
        ExprKind kind = *(const ExprKind *)n;
        if (kind == EXPR_VAR) {
            uint32_t id = ((const ExprVar *)n)->var_id;
            if (id < w->n_ids) {
                w->acc[id] += _fmix(ctx ^ K_CTX);
                if (anchor == UINT32_MAX) anchor = id;
                else _uf_union(w->uf, anchor, id);
            }
            continue;
        }
        if (kind == EXPR_ARRAY_SELECT) {
            const ExprArraySelect *e = (const ExprArraySelect *)n;
            for (uint32_t i = 0; i < e->n_elems; i++) {
                uint32_t id = e->base_var_id + i;
                if (id >= w->n_ids) continue;
                w->acc[id] += _fmix(_mix(ctx ^ K_CTX_ARRAY, i));
                if (anchor == UINT32_MAX) anchor = id;
                else _uf_union(w->uf, anchor, id);
            }
        }
        if (_kids(w, r) < 0) return -1;
        uint32_t nk = w->kids.n;
        for (uint32_t i = 0; i < nk; i++)
            if (w->kids.v[i] != EXPR_NULL && _ref_push(&w->stack, w->kids.v[i]) < 0) return -1;
    }
    return 0;
}

/* The top-level conjuncts of every hard constraint (AND spines flattened). */
static int _conjuncts(const dvs_problem_t *p, RefVec *out) {
    RefVec st = { 0 };
    int rc = 0;
    for (dvs_expr_t c = p->constraints_head; c != EXPR_NULL && rc == 0; ) {
        const ConstraintSpec *cs = (const ConstraintSpec *)_node(p, c);
        st.n = 0;
        rc = _ref_push(&st, cs->root);
        while (rc == 0 && st.n) {
            dvs_expr_t r = st.v[--st.n];
            if (r != EXPR_NULL && _kind(p, r) == EXPR_BINARY
                    && ((const ExprBinary *)_node(p, r))->op == DVS_BIN_AND) {
                const ExprBinary *b = (const ExprBinary *)_node(p, r);
                rc = _ref_push(&st, b->rhs) | _ref_push(&st, b->lhs);
            } else {
                rc = _ref_push(out, r);
            }
        }
        c = cs->next;
    }
    free(st.v);
    return rc;
}

/* The class of every spec, with the colours in w->color. */
typedef struct {
    uint64_t *hard;     /* per conjunct */
    uint64_t *soft;     /* per soft constraint: root hash with priority rank */
    uint64_t *alldiff;
    uint64_t *dist;
} SpecHashes;

static uint32_t _rank(const uint32_t *sorted, uint32_t n, uint32_t v) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (sorted[i] == v) return r;
        if (i + 1 < n && sorted[i + 1] != sorted[i]) r++;
    }
    return r;
}

static int _cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int _spec_hashes(Walk *w, const RefVec *conj, const uint32_t *prio_sorted,
                        SpecHashes *sh) {
    const dvs_problem_t *p = w->p;
    dvs_problem_t *pm = (dvs_problem_t *)p;
    for (uint32_t i = 0; i < conj->n; i++) {
        if (_hash_tree(w, conj->v[i], &sh->hard[i]) < 0) return -1;
        sh->hard[i] = _mix(K_HARD, sh->hard[i]);
    }
    uint32_t i = 0;
    for (dvs_expr_t s = p->softs_head; s != EXPR_NULL; s = ((const SoftSpec *)_node(p, s))->next, i++) {
        const SoftSpec *ss = (const SoftSpec *)_node(p, s);
        uint64_t h;
        if (_hash_tree(w, ss->root, &h) < 0) return -1;
        sh->soft[i] = _mix(_mix(K_SOFT, h), _rank(prio_sorted, p->n_softs, ss->priority));
    }
    i = 0;
    for (dvs_expr_t a = p->allDiff_head; a != EXPR_NULL; a = ((const AllDiffSpec *)_node(p, a))->next, i++) {
        const AllDiffSpec *ad = (const AllDiffSpec *)_node(p, a);
        const uint32_t *ids = (const uint32_t *)(ad + 1);
        w->tmp.n = 0;
        for (uint32_t k = 0; k < ad->n_vars; k++)
            if (_u64_push(&w->tmp, _var_color(w, ids[k])) < 0) return -1;
        sh->alldiff[i] = _combine_sorted(K_ALLDIFF, w->tmp.v, w->tmp.n);
    }
    i = 0;
    for (dvs_expr_t d = p->dists_head; d != EXPR_NULL; d = ((const DistSpec *)_node(p, d))->next, i++) {
        const DistSpec *ds = (const DistSpec *)_node(p, d);
        const dvs_dist_entry_t *e = dist_spec_entries(pm, d);
        w->tmp.n = 0;
        for (uint32_t k = 0; k < ds->n_entries; k++) {
            uint64_t eh = _mix(_mix(_mix(K_DIST, e[k].is_per_value), e[k].lo == e[k].hi),
                               _cclass(e[k].weight, 32, 0));
            if (_u64_push(&w->tmp, eh) < 0) return -1;
        }
        sh->dist[i] = _combine_sorted(_mix(K_DIST, _var_color(w, ds->var_id)),
                                      w->tmp.v, w->tmp.n);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Depths                                                              */
/* ------------------------------------------------------------------ */

static int _is_guard(const dvs_problem_t *p, dvs_expr_t r);

/* The depth of `root` in operators, and its deepest nest of guards, into
 * *d and *g (post-order, iterative; memoized across roots). */
static int _depths(Walk *w, dvs_expr_t root, uint32_t *d, uint32_t *g) {
    uint64_t v;
    *d = *g = 0;
    if (root == EXPR_NULL) return 0;
    w->stack.n = w->phase.n = 0;
    if (_ref_push(&w->stack, root) < 0 || _ref_push(&w->phase, 0) < 0) return -1;
    while (w->stack.n) {
        dvs_expr_t r = w->stack.v[w->stack.n - 1];
        uint32_t ph = w->phase.v[w->phase.n - 1];
        if (_memo_get(&w->depth, r, &v)) { w->stack.n--; w->phase.n--; continue; }
        if (_kids(w, r) < 0) return -1;
        if (ph == 0) {
            w->phase.v[w->phase.n - 1] = 1;
            uint32_t nk = w->kids.n;
            for (uint32_t i = 0; i < nk; i++) {
                dvs_expr_t c = w->kids.v[i];
                if (c == EXPR_NULL || _memo_get(&w->depth, c, &v)) continue;
                if (_ref_push(&w->stack, c) < 0 || _ref_push(&w->phase, 0) < 0) return -1;
            }
            continue;
        }
        uint32_t md = 0, mg = 0;
        for (uint32_t i = 0; i < w->kids.n; i++) {
            if (w->kids.v[i] == EXPR_NULL || !_memo_get(&w->depth, w->kids.v[i], &v)) continue;
            if ((uint32_t)v > md) md = (uint32_t)v;
            if ((uint32_t)(v >> 32) > mg) mg = (uint32_t)(v >> 32);
        }
        if (w->kids.n) md++;
        if (_is_guard(w->p, r)) mg++;
        if (_memo_put(&w->depth, r, md | (uint64_t)mg << 32) < 0) return -1;
        w->stack.n--; w->phase.n--;
    }
    if (!_memo_get(&w->depth, root, &v)) return -1;
    *d = (uint32_t)v;
    *g = (uint32_t)(v >> 32);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Motifs                                                              */
/* ------------------------------------------------------------------ */

static const char *const k_motif[DVS_N_MOTIFS] = {
    "var-cmp-const", "var-eq-const", "var-cmp-var", "var-eq-var", "range", "set",
    "ranges", "mod-eq", "mask-eq", "shift-eq", "linear", "nonlinear", "bitwise",
    "countones", "sum", "array", "guarded", "disjunction", "bool-var", "ite-value",
    "constant", "other"
};

const char *dvs_motif_name(dvs_motif_t m) {
    return (unsigned)m < DVS_N_MOTIFS ? k_motif[m] : k_motif[DVS_MOTIF_OTHER];
}

/* `r` read through casts and extends. */
static dvs_expr_t _strip(const dvs_problem_t *p, dvs_expr_t r) {
    for (int i = 0; i < 16 && r != EXPR_NULL; i++) {
        const void *n = _node(p, r);
        switch (*(const ExprKind *)n) {
        case EXPR_EXTEND:  r = ((const ExprExtend *)n)->operand; continue;
        case EXPR_SV_CAST: r = ((const ExprSvCast *)n)->operand; continue;
        case EXPR_CAST:    r = ((const ExprCast *)n)->operand;   continue;
        default:           return r;
        }
    }
    return r;
}

static int _is_const(const dvs_problem_t *p, dvs_expr_t r) {
    r = _strip(p, r);
    if (r == EXPR_NULL) return 0;
    ExprKind k = _kind(p, r);
    return k == EXPR_CONST || k == EXPR_CONST_WIDE;
}

/* A variable, or a bit-slice of one (a field, or a field of a field). */
static int _is_var(const dvs_problem_t *p, dvs_expr_t r) {
    for (int i = 0; i < 16; i++) {
        r = _strip(p, r);
        if (r == EXPR_NULL) return 0;
        ExprKind k = _kind(p, r);
        if (k != EXPR_EXTRACT) return k == EXPR_VAR;
        r = ((const ExprExtract *)_node(p, r))->operand;
    }
    return 0;
}

static int _is_not(const dvs_problem_t *p, dvs_expr_t r) {
    return r != EXPR_NULL && _kind(p, r) == EXPR_UNARY
           && ((const ExprUnary *)_node(p, r))->op == DVS_UN_NOT;
}

/* A guard: `!g || body` (how `g -> body` and `if (g) body` arrive), or ?:. */
static int _is_guard(const dvs_problem_t *p, dvs_expr_t r) {
    if (r == EXPR_NULL) return 0;
    ExprKind k = _kind(p, r);
    if (k == EXPR_ITE) return 1;
    if (k != EXPR_BINARY) return 0;
    const ExprBinary *b = (const ExprBinary *)_node(p, r);
    return b->op == DVS_BIN_OR && (_is_not(p, b->lhs) || _is_not(p, b->rhs));
}

static int _is_cmp(dvs_binop_t op) {
    return op == DVS_BIN_LT || op == DVS_BIN_LTE || op == DVS_BIN_GT || op == DVS_BIN_GTE;
}

/* What a compare's operands are made of (up to 64 nodes): the motif of the
 * most telling construct found. */
static dvs_motif_t _content(Walk *w, dvs_expr_t root) {
    dvs_expr_t st[64];
    uint32_t sp = 0, seen = 0;
    int mul = 0, bit = 0, arith = 0, ite = 0, cnt = 0, sum = 0, arr = 0;
    st[sp++] = root;
    while (sp && seen < 64) {
        dvs_expr_t r = st[--sp];
        if (r == EXPR_NULL) continue;
        seen++;
        const void *n = _node(w->p, r);
        switch (*(const ExprKind *)n) {
        case EXPR_BINARY:
            switch (((const ExprBinary *)n)->op) {
            case DVS_BIN_MUL: case DVS_BIN_DIV: case DVS_BIN_MOD: mul = 1; break;
            case DVS_BIN_ADD: case DVS_BIN_SUB:                   arith = 1; break;
            case DVS_BIN_BAND: case DVS_BIN_BOR: case DVS_BIN_BXOR:
            case DVS_BIN_LSHIFT: case DVS_BIN_RSHIFT: case DVS_BIN_ASHR: bit = 1; break;
            default: break;
            }
            break;
        case EXPR_UNARY:
            if (((const ExprUnary *)n)->op == DVS_UN_NEG) arith = 1;
            else if (((const ExprUnary *)n)->op == DVS_UN_INVERT) bit = 1;
            break;
        case EXPR_CONCAT:       bit = 1; break;
        case EXPR_ITE:          ite = 1; break;
        case EXPR_COUNTONES:
        case EXPR_CLOG2:        cnt = 1; break;
        case EXPR_SUM:          sum = 1; break;
        case EXPR_ARRAY_SELECT: arr = 1; break;
        default: break;
        }
        if (_kids(w, r) < 0) break;
        for (uint32_t i = 0; i < w->kids.n && sp < 64; i++) st[sp++] = w->kids.v[i];
    }
    return cnt ? DVS_MOTIF_COUNTONES : sum ? DVS_MOTIF_SUM : arr ? DVS_MOTIF_ARRAY
         : mul ? DVS_MOTIF_NONLINEAR : ite ? DVS_MOTIF_ITE_VALUE
         : bit ? DVS_MOTIF_BITWISE : arith ? DVS_MOTIF_LINEAR : DVS_MOTIF_OTHER;
}

/* The motif of a compare `l op r` (op ==, != or an ordering). */
static dvs_motif_t _compare(Walk *w, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r) {
    const dvs_problem_t *p = w->p;
    int eq = op == DVS_BIN_EQ || op == DVS_BIN_NEQ;
    if (_is_const(p, l) && _is_const(p, r)) return DVS_MOTIF_CONSTANT;
    if (_is_var(p, l) && _is_var(p, r)) return eq ? DVS_MOTIF_VAR_EQ_VAR : DVS_MOTIF_VAR_CMP_VAR;
    if (_is_const(p, l)) { dvs_expr_t t = l; l = r; r = t; }
    if (_is_const(p, r)) {
        if (_is_var(p, l)) return eq ? DVS_MOTIF_VAR_EQ_CONST : DVS_MOTIF_VAR_CMP_CONST;
        dvs_expr_t e = _strip(p, l);
        if (eq && e != EXPR_NULL && _kind(p, e) == EXPR_BINARY) {
            const ExprBinary *b = (const ExprBinary *)_node(p, e);
            int vc = (_is_var(p, b->lhs) && _is_const(p, b->rhs))
                  || (_is_const(p, b->lhs) && _is_var(p, b->rhs));
            if (vc && b->op == DVS_BIN_MOD) return DVS_MOTIF_MOD_EQ;
            if (vc && b->op == DVS_BIN_BAND) return DVS_MOTIF_MASK_EQ;
            if (vc && (b->op == DVS_BIN_LSHIFT || b->op == DVS_BIN_RSHIFT
                       || b->op == DVS_BIN_ASHR))
                return DVS_MOTIF_SHIFT_EQ;
        }
    }
    dvs_motif_t a = _content(w, l), b = _content(w, r);
    /* the more specific of the two sides (lower ids are not more specific:
     * order by the enum's construct ranking below) */
    static const uint8_t rank[DVS_N_MOTIFS] = {
        [DVS_MOTIF_COUNTONES] = 7, [DVS_MOTIF_SUM] = 6, [DVS_MOTIF_ARRAY] = 5,
        [DVS_MOTIF_NONLINEAR] = 4, [DVS_MOTIF_ITE_VALUE] = 3, [DVS_MOTIF_BITWISE] = 2,
        [DVS_MOTIF_LINEAR] = 1,
    };
    dvs_motif_t m = rank[a] >= rank[b] ? a : b;
    return m;
}

/* Count the motifs of top-level constraint `r` into f->motifs. */
static void _motifs(Walk *w, dvs_expr_t r, dvs_class_features_t *f, int depth) {
    const dvs_problem_t *p = w->p;
    dvs_motif_t m = DVS_MOTIF_OTHER;
    r = _strip(p, r);
    if (r == EXPR_NULL) { f->motifs[DVS_MOTIF_OTHER]++; return; }
    const void *n = _node(p, r);
    switch (*(const ExprKind *)n) {
    case EXPR_CONST:
    case EXPR_CONST_WIDE:
        m = DVS_MOTIF_CONSTANT;
        break;
    case EXPR_VAR:
    case EXPR_EXTRACT:
        m = _is_var(p, r) ? DVS_MOTIF_BOOL_VAR : DVS_MOTIF_OTHER;
        break;
    case EXPR_UNARY: {
        const ExprUnary *u = (const ExprUnary *)n;
        if (u->op == DVS_UN_NOT && _is_var(p, u->operand)) {
            m = DVS_MOTIF_BOOL_VAR;
        } else if (u->op == DVS_UN_NOT && depth < 4) {
            _motifs(w, u->operand, f, depth + 1);   /* !(x inside S): as x inside S */
            return;
        } else {
            m = _content(w, r);
        }
        break;
    }
    case EXPR_IN_RANGE:  m = DVS_MOTIF_RANGE;  break;
    case EXPR_IN_SET:    m = DVS_MOTIF_SET;    break;
    case EXPR_IN_RANGES: m = DVS_MOTIF_RANGES; break;
    case EXPR_SUM:       m = DVS_MOTIF_SUM;    break;
    case EXPR_COUNTONES:
    case EXPR_CLOG2:     m = DVS_MOTIF_COUNTONES; break;
    case EXPR_ARRAY_SELECT: m = DVS_MOTIF_ARRAY; break;
    case EXPR_ITE: {
        const ExprITE *e = (const ExprITE *)n;
        f->motifs[DVS_MOTIF_GUARDED]++;
        if (depth < 4) {
            if (!_is_const(p, e->then_e)) _motifs(w, e->then_e, f, depth + 1);
            if (!_is_const(p, e->else_e)) _motifs(w, e->else_e, f, depth + 1);
        }
        return;
    }
    case EXPR_BINARY: {
        const ExprBinary *b = (const ExprBinary *)n;
        if (_is_cmp(b->op) || b->op == DVS_BIN_EQ || b->op == DVS_BIN_NEQ) {
            m = _compare(w, b->op, b->lhs, b->rhs);
        } else if (_is_guard(p, r)) {
            f->motifs[DVS_MOTIF_GUARDED]++;
            if (depth < 4) _motifs(w, _is_not(p, b->lhs) ? b->rhs : b->lhs, f, depth + 1);
            return;
        } else if (b->op == DVS_BIN_OR) {
            m = DVS_MOTIF_DISJUNCTION;
        } else if (b->op == DVS_BIN_AND && depth < 4) {
            /* a conjunction under a guard: each side is a constraint */
            _motifs(w, b->lhs, f, depth + 1);
            _motifs(w, b->rhs, f, depth + 1);
            return;
        } else {
            m = _content(w, r);
        }
        break;
    }
    default:
        break;
    }
    f->motifs[m]++;
}

/* A variable as the tracked list sorts it. */
typedef struct { uint32_t width; uint64_t color; uint32_t id; uint8_t free; } TrackKey;

static int _cmp_track(const void *a, const void *b) {
    const TrackKey *x = (const TrackKey *)a, *y = (const TrackKey *)b;
    if (x->width != y->width) return x->width > y->width ? -1 : 1;
    if (x->color != y->color) return x->color < y->color ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* The tracked variables (dvs_class_track_t), with the refined colours. */
static int _track(const Walk *w, dvs_class_track_t *t) {
    const dvs_problem_t *p = w->p;
    uint32_t n = 0;
    for (dvs_expr_t v = p->vars_head; v != EXPR_NULL; v = ((const VarSpec *)_node(p, v))->next) n++;
    TrackKey *k = (TrackKey *)malloc(((size_t)n + 1) * sizeof *k);
    if (!k) return -1;
    n = 0;
    for (dvs_expr_t v = p->vars_head; v != EXPR_NULL; v = ((const VarSpec *)_node(p, v))->next) {
        const VarSpec *vs = (const VarSpec *)_node(p, v);
        if (vs->kind != DVS_VAR_RAND && vs->kind != DVS_VAR_RANDC) continue;
        k[n].width = vs->width;
        k[n].color = w->color[vs->var_id];
        k[n].id = vs->var_id;
        k[n].free = vs->lo != vs->hi;
        n++;
    }
    qsort(k, n, sizeof *k, _cmp_track);
    t->n = n < DVS_CLASS_TRACK ? n : DVS_CLASS_TRACK;
    for (uint32_t i = 0; i < t->n; i++) {
        t->var_id[i] = k[i].id;
        t->width[i] = (uint8_t)(k[i].width < 64 ? k[i].width : 64);
        t->free[i] = k[i].free;
    }
    free(k);
    return 0;
}

static uint32_t _count(const dvs_problem_t *p, dvs_expr_t head, size_t next_off) {
    uint32_t n = 0;
    for (dvs_expr_t r = head; r != EXPR_NULL; n++)
        r = *(const dvs_expr_t *)((const char *)_node(p, r) + next_off);
    return n;
}

size_t dvs_class_sizeof(void) { return sizeof(dvs_class_t); }

int dvs_class_compute(const dvs_problem_t *p, dvs_class_t *out) {
    memset(out, 0, sizeof *out);
    Walk w;
    memset(&w, 0, sizeof w);
    w.p = p;
    RefVec conj = { 0 };
    SpecHashes sh = { 0 };
    uint32_t *prio = NULL;
    int rc = -1;

    /* Variable types (round 1). */
    for (dvs_expr_t v = p->vars_head; v != EXPR_NULL; v = ((const VarSpec *)_node(p, v))->next) {
        uint32_t id = ((const VarSpec *)_node(p, v))->var_id;
        if (id + 1 > w.n_ids) w.n_ids = id + 1;
    }
    size_t nid = w.n_ids ? w.n_ids : 1;
    w.sig = (uint64_t *)calloc(nid, sizeof *w.sig);
    w.color = (uint64_t *)calloc(nid, sizeof *w.color);
    w.acc = (uint64_t *)calloc(nid, sizeof *w.acc);
    w.uf = (uint32_t *)malloc(nid * sizeof *w.uf);
    if (!w.sig || !w.color || !w.acc || !w.uf) goto done;
    for (uint32_t i = 0; i < nid; i++) w.uf[i] = i;
    for (uint32_t i = 0; i < w.n_ids; i++) w.sig[i] = _mix(K_VAR_UNDECL, 1);
    for (dvs_expr_t v = p->vars_head; v != EXPR_NULL; v = ((const VarSpec *)_node(p, v))->next) {
        const VarSpec *vs = (const VarSpec *)_node(p, v);
        uint64_t h = _mix(_mix(_mix(_mix(K_VAR, vs->width), vs->is_signed), vs->kind), vs->order);
        h = _mix(h, _bound_class(vs->lo, vs->width, vs->is_signed));
        h = _mix(h, _bound_class(vs->hi, vs->width, vs->is_signed));
        w.sig[vs->var_id] = h;
    }
    memcpy(w.color, w.sig, nid * sizeof *w.color);

    if (_conjuncts(p, &conj) < 0) goto done;
    uint32_t n_soft = _count(p, p->softs_head, offsetof(SoftSpec, next));
    uint32_t n_ad = _count(p, p->allDiff_head, offsetof(AllDiffSpec, next));
    uint32_t n_dist = _count(p, p->dists_head, offsetof(DistSpec, next));
    sh.hard = (uint64_t *)malloc(((size_t)conj.n + 1) * sizeof *sh.hard);
    sh.soft = (uint64_t *)malloc(((size_t)n_soft + 1) * sizeof *sh.soft);
    sh.alldiff = (uint64_t *)malloc(((size_t)n_ad + 1) * sizeof *sh.alldiff);
    sh.dist = (uint64_t *)malloc(((size_t)n_dist + 1) * sizeof *sh.dist);
    prio = (uint32_t *)malloc(((size_t)n_soft + 1) * sizeof *prio);
    if (!sh.hard || !sh.soft || !sh.alldiff || !sh.dist || !prio) goto done;
    {
        uint32_t i = 0;
        for (dvs_expr_t s = p->softs_head; s != EXPR_NULL; s = ((const SoftSpec *)_node(p, s))->next)
            prio[i++] = ((const SoftSpec *)_node(p, s))->priority;
        qsort(prio, n_soft, sizeof *prio, _cmp_u32);
    }
    uint32_t cap = 1024;
    while (cap < p->pool.used / 4u && cap < (1u << 28)) cap <<= 1;
    if (_memo_init(&w.memo, cap) < 0 || _memo_init(&w.seen, cap) < 0
            || _memo_init(&w.depth, cap) < 0)
        goto done;

    /* Round 1: hash every spec with the type colours. */
    if (_spec_hashes(&w, &conj, prio, &sh) < 0) goto done;

    /* Each variable's contexts: the round-1 hashes of what it appears in. */
    {
        uint64_t stamp = 0;
        for (uint32_t i = 0; i < conj.n; i++)
            if (_context(&w, conj.v[i], sh.hard[i], ++stamp) < 0) goto done;
        uint32_t i = 0;
        for (dvs_expr_t s = p->softs_head; s != EXPR_NULL; s = ((const SoftSpec *)_node(p, s))->next, i++)
            if (_context(&w, ((const SoftSpec *)_node(p, s))->root, sh.soft[i], ++stamp) < 0) goto done;
        i = 0;
        for (dvs_expr_t a = p->allDiff_head; a != EXPR_NULL; a = ((const AllDiffSpec *)_node(p, a))->next, i++) {
            const AllDiffSpec *ad = (const AllDiffSpec *)_node(p, a);
            const uint32_t *ids = (const uint32_t *)(ad + 1);
            for (uint32_t k = 0; k < ad->n_vars; k++) {
                if (ids[k] >= w.n_ids) continue;
                w.acc[ids[k]] += _fmix(sh.alldiff[i] ^ K_CTX);
                if (k) _uf_union(w.uf, ids[0] < w.n_ids ? ids[0] : ids[k], ids[k]);
            }
        }
        i = 0;
        for (dvs_expr_t d = p->dists_head; d != EXPR_NULL; d = ((const DistSpec *)_node(p, d))->next, i++) {
            uint32_t id = ((const DistSpec *)_node(p, d))->var_id;
            if (id < w.n_ids) w.acc[id] += _fmix(sh.dist[i] ^ K_CTX);
        }
    }

    /* Round 2: refined colours, hashes again from scratch. */
    for (uint32_t i = 0; i < w.n_ids; i++) w.color[i] = _mix(w.sig[i], w.acc[i]);
    _memo_clear(&w.memo);
    if (_spec_hashes(&w, &conj, prio, &sh) < 0) goto done;

    {
        uint64_t h = _mix(K_PROBLEM, p->flags & DVS_PROBLEM_F_EXPLICIT);
        h = _combine_sorted(h, sh.hard, conj.n);
        h = _combine_sorted(h, sh.soft, n_soft);
        h = _combine_sorted(h, sh.alldiff, n_ad);
        h = _combine_sorted(h, sh.dist, n_dist);
        /* Source groups: sets of variables randomized together. */
        w.tmp.n = 0;
        U64Vec groups = { 0 };
        for (dvs_expr_t s = p->sources_head; s != EXPR_NULL; s = ((const SourceSpec *)_node(p, s))->next) {
            const SourceSpec *ss = (const SourceSpec *)_node(p, s);
            const uint32_t *ids = source_spec_vars((dvs_problem_t *)p, s);
            w.tmp.n = 0;
            for (uint32_t k = 0; k < ss->n_vars; k++)
                if (_u64_push(&w.tmp, _var_color(&w, ids[k])) < 0) { free(groups.v); goto done; }
            if (_u64_push(&groups, _combine_sorted(K_SOURCE, w.tmp.v, w.tmp.n)) < 0) {
                free(groups.v);
                goto done;
            }
        }
        h = _combine_sorted(h, groups.v, groups.n);
        free(groups.v);
        /* Every declared variable, referenced or not. */
        w.tmp.n = 0;
        for (dvs_expr_t v = p->vars_head; v != EXPR_NULL; v = ((const VarSpec *)_node(p, v))->next)
            if (_u64_push(&w.tmp, w.color[((const VarSpec *)_node(p, v))->var_id]) < 0) goto done;
        h = _combine_sorted(_mix(h, K_VARS), w.tmp.v, w.tmp.n);
        out->hash = h ? h : 1;
    }
    if (_track(&w, &out->track) < 0) goto done;
    /* The feature vector. */
    {
        dvs_class_features_t *f = &out->f;
        f->n_conjuncts = conj.n;
        f->n_softs = n_soft;
        f->n_alldiffs = n_ad;
        f->n_dists = n_dist;
        uint32_t *size = (uint32_t *)calloc(nid, sizeof *size);
        if (!size) goto done;
        for (dvs_expr_t v = p->vars_head; v != EXPR_NULL; v = ((const VarSpec *)_node(p, v))->next) {
            const VarSpec *vs = (const VarSpec *)_node(p, v);
            f->n_vars++;
            f->n_ordered += vs->order != 0;
            f->widths[vs->width <= 1 ? DVS_WIDTH_1 : vs->width <= 8 ? DVS_WIDTH_8
                      : vs->width <= 16 ? DVS_WIDTH_16 : vs->width <= 32 ? DVS_WIDTH_32
                      : vs->width <= 64 ? DVS_WIDTH_64 : DVS_WIDTH_WIDE]++;
            uint32_t root = _uf_find(w.uf, vs->var_id);
            if (size[root]++ == 0) f->n_components++;
            if (size[root] > f->max_component) f->max_component = size[root];
        }
        free(size);
        for (uint32_t i = 0; i < conj.n; i++) {
            uint32_t d, g;
            if (_depths(&w, conj.v[i], &d, &g) < 0) goto done;
            if (d > f->max_depth) f->max_depth = d;
            if (g > f->guard_depth) f->guard_depth = g;
            _motifs(&w, conj.v[i], f, 0);
        }
        dvs_shape_t shp;
        if (dvs_shape_compute((dvs_problem_t *)p, &shp) == 0) {
            f->n_nodes = shp.n_nodes; f->n_bit = shp.n_bit; f->n_arith = shp.n_arith;
            f->n_mul = shp.n_mul; f->n_cmp = shp.n_cmp; f->n_eq = shp.n_eq;
            f->n_bool = shp.n_bool; f->n_other = shp.n_other;
        }
    }
    out->inst = dvs_problem_fingerprint(p);
    out->n_nodes = w.memo.n;
    out->truncated = (uint32_t)w.truncated;
    rc = 0;
done:
    free(w.sig);
    free(w.color);
    free(w.acc);
    _memo_free(&w.memo);
    _memo_free(&w.seen);
    _memo_free(&w.depth);
    free(w.uf);
    free(w.stack.v);
    free(w.phase.v);
    free(w.kids.v);
    free(w.tmp.v);
    free(conj.v);
    free(sh.hard);
    free(sh.soft);
    free(sh.alldiff);
    free(sh.dist);
    free(prio);
    return rc;
}
