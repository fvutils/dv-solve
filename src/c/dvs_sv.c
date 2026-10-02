/*
 * SystemVerilog elaboration of builder-API problems -- see dvs_sv.h.
 *
 * The rewrite is copy-on-write: nothing is allocated until the first node
 * that needs a new form, at which point the whole problem buffer is copied
 * (so every original dvs_expr_t stays valid) and new nodes are appended to the
 * copy's pool. Unchanged subtrees are shared, never copied.
 *
 * INVARIANT of _val(ref, W, S): the returned node's value, read by its own
 * (emitted) type, equals the SystemVerilog value of `ref` evaluated in the
 * context (W, S); its emitted signedness is S and its emitted width is <= W.
 * Context-determined operators (arithmetic, bitwise, shifts, unary - ~, ?:)
 * are emitted at exactly W; a narrower result is only ever a leaf (variable,
 * constant, comparison, extend/extract/concat) whose implicit extension by its
 * own signedness -- which equals S -- does not change its value. That is what
 * lets the engines keep their fast paths for `var op const` comparisons: a
 * variable is never wrapped unless SV actually changes its value.
 */
#include <stdlib.h>
#include <string.h>
#include "dvs_sv.h"
#include "dvs_i128.h"

#define SV_POOL_HDR   ((uint32_t)sizeof(dvs_pool_t))
#define SV_MAX_W      255u      /* node formats hold widths in a uint8_t */
#define SV_MAX_DEPTH  20000

void dvs_sv_const_type(const ExprConst *c, uint16_t *width, uint8_t *is_signed) {
    if (c->width) {
        *width = c->width;
        *is_signed = c->is_signed ? 1 : 0;
        return;
    }
    int64_t v = c->value;
    if (v >= INT32_MIN && v <= INT32_MAX) { *width = 32; *is_signed = 1; return; }
    if (!c->is_signed && v >= 0 && v <= (int64_t)UINT32_MAX) {
        *width = 32; *is_signed = 0; return;
    }
    *width = 64;
    *is_signed = (v < 0 || c->is_signed) ? 1 : 0;
}

int64_t dvs_sv_wrap(int64_t v, uint16_t width, uint8_t is_signed) {
    if (width == 0 || width >= 64) return v;
    uint64_t m = ((uint64_t)1 << width) - 1;
    uint64_t u = (uint64_t)v & m;
    if (is_signed && ((u >> (width - 1)) & 1u)) u |= ~m;
    return (int64_t)u;
}

/* ------------------------------------------------------------------ */
/* Elaboration state                                                   */
/* ------------------------------------------------------------------ */

typedef struct { uint16_t w; uint8_t s; } SvTy;

typedef struct {
    uint64_t key;
    uint32_t val;
    uint32_t used;
} SvMemo;

typedef struct {
    dvs_problem_t *src;
    dvs_problem_t *dst;         /* copy-on-write target, NULL until needed */
    size_t        dst_bytes;   /* allocated size of dst                    */

    dvs_sv_var_type_fn vfn;
    void              *vud;
    uint16_t *vw;              /* VarSpec widths, by var id     */
    uint8_t  *vs;              /* VarSpec signedness            */
    uint8_t  *vk;              /* 1 if a VarSpec exists          */
    uint32_t  nv;

    SvMemo   *memo;
    uint32_t  memo_cap;        /* power of two */
    uint32_t  memo_n;

    int       err;
    int       oom;             /* err was caused by running out of memory */
    int       unknown_var;     /* err was caused by an undeclared variable */
    int       depth;
} SvE;

enum { M_VAL = 0, M_BOOL = 1, M_TYPE = 2 };

static void *_P(SvE *E, dvs_expr_t r) {
    return POOL_PTR(E->dst ? E->dst : E->src, r);
}

static ExprKind _kind(SvE *E, dvs_expr_t r) {
    return *(ExprKind *)_P(E, r);
}

/* ---- memo -------------------------------------------------------- */

static uint64_t _key(dvs_expr_t r, uint16_t w, uint8_t s, int mode) {
    return ((uint64_t)r << 32) | ((uint64_t)w << 8) | ((uint64_t)s << 4)
         | (uint64_t)mode;
}

static uint32_t _hash(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

static int _memo_get(SvE *E, uint64_t k, uint32_t *out) {
    if (!E->memo) return 0;
    uint32_t m = E->memo_cap - 1;
    for (uint32_t i = _hash(k) & m;; i = (i + 1) & m) {
        if (!E->memo[i].used) return 0;
        if (E->memo[i].key == k) { *out = E->memo[i].val; return 1; }
    }
}

static void _memo_put(SvE *E, uint64_t k, uint32_t v) {
    if (E->err) return;
    if ((E->memo_n + 1) * 2 > E->memo_cap) {
        uint32_t nc = E->memo_cap ? E->memo_cap * 2 : 1024;
        SvMemo *nm = (SvMemo *)calloc(nc, sizeof(SvMemo));
        if (!nm) { E->err = E->oom = 1; return; }
        for (uint32_t i = 0; i < E->memo_cap; i++) {
            if (!E->memo[i].used) continue;
            uint32_t j = _hash(E->memo[i].key) & (nc - 1);
            while (nm[j].used) j = (j + 1) & (nc - 1);
            nm[j] = E->memo[i];
        }
        free(E->memo);
        E->memo = nm;
        E->memo_cap = nc;
    }
    uint32_t m = E->memo_cap - 1;
    uint32_t i = _hash(k) & m;
    while (E->memo[i].used && E->memo[i].key != k) i = (i + 1) & m;
    if (!E->memo[i].used) E->memo_n++;
    E->memo[i].key = k;
    E->memo[i].val = v;
    E->memo[i].used = 1;
}

/* ---- allocation in the copy ------------------------------------- */

static dvs_expr_t _alloc(SvE *E, uint32_t bytes, uint32_t align) {
    if (E->err) return EXPR_NULL;
    if (!E->dst) {
        /* New nodes go AFTER the source's whole capacity, not just after
         * what it uses: a caller may still append to its problem in place
         * (into finalize_reserve slack), and dvs_sv_elaborate_more() syncs
         * those appended bytes into the copy at the same offsets. */
        uint32_t used = E->src->pool.used;
        uint32_t base = E->src->pool.capacity > used ? E->src->pool.capacity : used;
        base = (base + 15u) & ~15u;
        size_t cap = (size_t)base + used / 2 + 4096;
        if (cap > 0xF0000000u) { E->err = E->oom = 1; return EXPR_NULL; }
        size_t total = sizeof(dvs_problem_t) + cap;
        dvs_problem_t *d = (dvs_problem_t *)malloc(total);
        if (!d) { E->err = E->oom = 1; return EXPR_NULL; }
        memcpy(d, E->src, sizeof(dvs_problem_t) + used);
        d->pool.capacity = (uint32_t)cap;
        d->pool.used = base;
        d->pool.overflow = 0;
        E->dst = d;
        E->dst_bytes = total;
    }
    dvs_problem_t *d = E->dst;
    uint32_t base = d->pool.used;
    if (align > 1) base = (base + align - 1) & ~(align - 1);
    if ((uint64_t)base + bytes > d->pool.capacity) {
        size_t cap = (size_t)d->pool.capacity * 2;
        if (cap < (size_t)base + bytes + 4096) cap = (size_t)base + bytes + 4096;
        if (cap > 0xF0000000u) { E->err = E->oom = 1; return EXPR_NULL; }
        dvs_problem_t *nd = (dvs_problem_t *)realloc(d, sizeof(dvs_problem_t) + cap);
        if (!nd) { E->err = E->oom = 1; return EXPR_NULL; }
        nd->pool.capacity = (uint32_t)cap;
        E->dst = d = nd;
        E->dst_bytes = sizeof(dvs_problem_t) + cap;
    }
    d->pool.used = base + bytes;
    return SV_POOL_HDR + base;
}

static dvs_expr_t _mk_const(SvE *E, int64_t v, uint8_t s, uint16_t w) {
    if (w > SV_MAX_W) { E->err = 1; return EXPR_NULL; }
    dvs_expr_t r = _alloc(E, sizeof(ExprConst), _Alignof(ExprConst));
    if (r == EXPR_NULL) return r;
    ExprConst *n = (ExprConst *)_P(E, r);
    memset(n, 0, sizeof(*n));
    n->kind = EXPR_CONST;
    n->is_signed = s;
    n->width = (uint8_t)w;
    n->value = v;
    return r;
}

static dvs_expr_t _mk_bin(SvE *E, dvs_binop_t op, dvs_expr_t l, dvs_expr_t rr) {
    if (l == EXPR_NULL || rr == EXPR_NULL) { E->err = 1; return EXPR_NULL; }
    dvs_expr_t r = _alloc(E, sizeof(ExprBinary), _Alignof(ExprBinary));
    if (r == EXPR_NULL) return r;
    ExprBinary *n = (ExprBinary *)_P(E, r);
    n->kind = EXPR_BINARY; n->op = op; n->lhs = l; n->rhs = rr;
    return r;
}

static dvs_expr_t _mk_un(SvE *E, dvs_unop_t op, dvs_expr_t x) {
    if (x == EXPR_NULL) { E->err = 1; return EXPR_NULL; }
    dvs_expr_t r = _alloc(E, sizeof(ExprUnary), _Alignof(ExprUnary));
    if (r == EXPR_NULL) return r;
    ExprUnary *n = (ExprUnary *)_P(E, r);
    n->kind = EXPR_UNARY; n->op = op; n->operand = x;
    return r;
}

static dvs_expr_t _mk_ite(SvE *E, dvs_expr_t c, dvs_expr_t t, dvs_expr_t e) {
    if (c == EXPR_NULL || t == EXPR_NULL || e == EXPR_NULL) {
        E->err = 1; return EXPR_NULL;
    }
    dvs_expr_t r = _alloc(E, sizeof(ExprITE), _Alignof(ExprITE));
    if (r == EXPR_NULL) return r;
    ExprITE *n = (ExprITE *)_P(E, r);
    n->kind = EXPR_ITE; n->cond = c; n->then_e = t; n->else_e = e;
    return r;
}

static dvs_expr_t _mk_ext(SvE *E, dvs_expr_t x, uint16_t from, uint16_t to,
                       uint8_t sext) {
    if (x == EXPR_NULL || from > SV_MAX_W || to > SV_MAX_W) {
        E->err = 1; return EXPR_NULL;
    }
    dvs_expr_t r = _alloc(E, sizeof(ExprExtend), _Alignof(ExprExtend));
    if (r == EXPR_NULL) return r;
    ExprExtend *n = (ExprExtend *)_P(E, r);
    n->kind = EXPR_EXTEND; n->sign_extend = sext;
    n->from_bits = (uint8_t)from; n->to_bits = (uint8_t)to; n->_pad = 0;
    n->operand = x;
    return r;
}

static dvs_expr_t _mk_cast(SvE *E, dvs_expr_t x, uint16_t from, uint16_t to,
                        uint8_t sext, uint8_t dst_s) {
    if (x == EXPR_NULL || from > SV_MAX_W || to > SV_MAX_W) {
        E->err = 1; return EXPR_NULL;
    }
    dvs_expr_t r = _alloc(E, sizeof(ExprSvCast), _Alignof(ExprSvCast));
    if (r == EXPR_NULL) return r;
    ExprSvCast *n = (ExprSvCast *)_P(E, r);
    n->kind = EXPR_SV_CAST; n->sign_extend = sext;
    n->from_bits = (uint8_t)from; n->to_bits = (uint8_t)to;
    n->dst_signed = dst_s; n->operand = x;
    return r;
}

static dvs_expr_t _mk_extract(SvE *E, dvs_expr_t x, uint8_t hi, uint8_t lo) {
    if (x == EXPR_NULL) { E->err = 1; return EXPR_NULL; }
    dvs_expr_t r = _alloc(E, sizeof(ExprExtract), _Alignof(ExprExtract));
    if (r == EXPR_NULL) return r;
    ExprExtract *n = (ExprExtract *)_P(E, r);
    n->kind = EXPR_EXTRACT; n->hi_bit = hi; n->lo_bit = lo;
    n->_pad[0] = n->_pad[1] = 0; n->operand = x;
    return r;
}

static dvs_expr_t _mk_concat(SvE *E, dvs_expr_t hi, dvs_expr_t lo, uint8_t lo_w) {
    if (hi == EXPR_NULL || lo == EXPR_NULL) { E->err = 1; return EXPR_NULL; }
    dvs_expr_t r = _alloc(E, sizeof(ExprConcat), _Alignof(ExprConcat));
    if (r == EXPR_NULL) return r;
    ExprConcat *n = (ExprConcat *)_P(E, r);
    n->kind = EXPR_CONCAT; n->lo_width = lo_w;
    n->_pad[0] = n->_pad[1] = n->_pad[2] = 0;
    n->hi = hi; n->lo = lo;
    return r;
}

static dvs_expr_t _mk_in_range(SvE *E, dvs_expr_t v, dvs_expr_t lo, dvs_expr_t hi) {
    if (v == EXPR_NULL || lo == EXPR_NULL || hi == EXPR_NULL) {
        E->err = 1; return EXPR_NULL;
    }
    dvs_expr_t r = _alloc(E, sizeof(ExprInRange), _Alignof(ExprInRange));
    if (r == EXPR_NULL) return r;
    ExprInRange *n = (ExprInRange *)_P(E, r);
    n->kind = EXPR_IN_RANGE; n->value = v; n->lo = lo; n->hi = hi;
    return r;
}

static dvs_expr_t _mk_in_set(SvE *E, dvs_expr_t v, uint32_t n, const dvs_expr_t *el) {
    dvs_expr_t r = _alloc(E, (uint32_t)(sizeof(ExprInSet) + n * sizeof(dvs_expr_t)),
                       _Alignof(ExprInSet));
    if (r == EXPR_NULL) return r;
    ExprInSet *s = (ExprInSet *)_P(E, r);
    s->kind = EXPR_IN_SET; s->value = v; s->n_elems = n;
    memcpy(s + 1, el, n * sizeof(dvs_expr_t));
    return r;
}

static dvs_expr_t _mk_in_ranges(SvE *E, dvs_expr_t v, uint32_t n,
                             const dvs_expr_t *los, const dvs_expr_t *his) {
    dvs_expr_t r = _alloc(E, (uint32_t)(sizeof(ExprInRanges) + 2 * n * sizeof(dvs_expr_t)),
                       _Alignof(ExprInRanges));
    if (r == EXPR_NULL) return r;
    ExprInRanges *s = (ExprInRanges *)_P(E, r);
    s->kind = EXPR_IN_RANGES; s->value = v; s->n_ranges = n;
    dvs_expr_t *p = (dvs_expr_t *)(s + 1);
    memcpy(p, los, n * sizeof(dvs_expr_t));
    memcpy(p + n, his, n * sizeof(dvs_expr_t));
    return r;
}

/* ---- types ------------------------------------------------------- */

static int _is_arith(dvs_binop_t op) {
    switch (op) {
    case DVS_BIN_ADD: case DVS_BIN_SUB: case DVS_BIN_MUL: case DVS_BIN_DIV: case DVS_BIN_MOD:
    case DVS_BIN_BAND: case DVS_BIN_BOR: case DVS_BIN_BXOR:
        return 1;
    default:
        return 0;
    }
}

static int _is_cmp(dvs_binop_t op) {
    switch (op) {
    case DVS_BIN_EQ: case DVS_BIN_NEQ: case DVS_BIN_LT: case DVS_BIN_LTE:
    case DVS_BIN_GT: case DVS_BIN_GTE:
        return 1;
    default:
        return 0;
    }
}

static SvTy _var_type(SvE *E, uint32_t vid) {
    SvTy t = { 0, 0 };
    if (vid < E->nv && E->vk[vid]) {
        t.w = E->vw[vid]; t.s = E->vs[vid];
        return t;
    }
    if (E->vfn) {
        uint16_t w; uint8_t s;
        if (E->vfn(E->vud, vid, &w, &s) == 0) { t.w = w; t.s = s ? 1 : 0; return t; }
    }
    E->err = E->unknown_var = 1;   /* cannot type the expression */
    t.w = 1;
    return t;
}

static SvTy _type(SvE *E, dvs_expr_t ref) {
    SvTy t = { 1, 0 };
    if (ref == EXPR_NULL || E->err) return t;
    uint64_t k = _key(ref, 0, 0, M_TYPE);
    uint32_t mv;
    if (_memo_get(E, k, &mv)) { t.w = (uint16_t)(mv >> 8); t.s = (uint8_t)(mv & 1); return t; }
    if (++E->depth > SV_MAX_DEPTH) { E->err = 1; E->depth--; return t; }

    switch (_kind(E, ref)) {
    case EXPR_VAR:
        t = _var_type(E, ((ExprVar *)_P(E, ref))->var_id);
        break;
    case EXPR_CONST: {
        uint16_t w; uint8_t s;
        dvs_sv_const_type((ExprConst *)_P(E, ref), &w, &s);
        t.w = w; t.s = s;
        break;
    }
    case EXPR_BINARY: {
        ExprBinary b = *(ExprBinary *)_P(E, ref);
        if (_is_arith(b.op)) {
            SvTy l = _type(E, b.lhs), r = _type(E, b.rhs);
            t.w = l.w > r.w ? l.w : r.w;
            t.s = (uint8_t)(l.s && r.s);
        } else if (b.op == DVS_BIN_LSHIFT || b.op == DVS_BIN_RSHIFT ||
                   b.op == DVS_BIN_ASHR) {
            t = _type(E, b.lhs);
        }
        break;
    }
    case EXPR_UNARY: {
        ExprUnary u = *(ExprUnary *)_P(E, ref);
        if (u.op != DVS_UN_NOT) t = _type(E, u.operand);
        break;
    }
    case EXPR_ITE: {
        ExprITE i = *(ExprITE *)_P(E, ref);
        SvTy a = _type(E, i.then_e), b = _type(E, i.else_e);
        t.w = a.w > b.w ? a.w : b.w;
        t.s = (uint8_t)(a.s && b.s);
        break;
    }
    case EXPR_EXTEND: {
        ExprExtend x = *(ExprExtend *)_P(E, ref);
        t.w = x.to_bits;
        t.s = _type(E, x.operand).s;
        break;
    }
    case EXPR_SV_CAST: {
        ExprSvCast x = *(ExprSvCast *)_P(E, ref);
        t.w = x.to_bits; t.s = x.dst_signed ? 1 : 0;
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract x = *(ExprExtract *)_P(E, ref);
        t.w = (uint16_t)(x.hi_bit - x.lo_bit + 1); t.s = 0;
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat x = *(ExprConcat *)_P(E, ref);
        t.w = (uint16_t)(_type(E, x.hi).w + x.lo_width); t.s = 0;
        break;
    }
    default:
        break;   /* membership / sum / countones / clog2 / select: Boolean */
    }
    E->depth--;
    if (t.w == 0) t.w = 1;
    _memo_put(E, k, ((uint32_t)t.w << 8) | t.s);
    return t;
}

/* ---- rewriting --------------------------------------------------- */

static dvs_expr_t _val(SvE *E, dvs_expr_t ref, uint16_t W, uint8_t S);
static dvs_expr_t _bool(SvE *E, dvs_expr_t ref);

/* Convert an already-elaborated node to the context (W, S). */
static dvs_expr_t _conv(SvE *E, dvs_expr_t x, uint16_t W, uint8_t S) {
    if (x == EXPR_NULL || E->err) return EXPR_NULL;
    SvTy t = _type(E, x);
    if (t.s == S && t.w <= W) return x;
    return _mk_cast(E, x, t.w, W, t.s, S);
}

/* Value-preserving widening of an elaborated node to width W. */
static dvs_expr_t _widen(SvE *E, dvs_expr_t x, uint16_t W) {
    if (x == EXPR_NULL || E->err) return EXPR_NULL;
    SvTy t = _type(E, x);
    if (t.w >= W) return x;
    return _mk_ext(E, x, t.w, W, t.s);
}

static dvs_expr_t _val_const(SvE *E, dvs_expr_t ref, uint16_t W, uint8_t S) {
    ExprConst c = *(ExprConst *)_P(E, ref);
    uint16_t cw; uint8_t cs;
    dvs_sv_const_type(&c, &cw, &cs);
    int64_t own = dvs_sv_wrap(c.value, cw, cs);
    if (W <= 64) {
        int64_t v = dvs_sv_wrap(own, W, S);
        if (c.width == W && (c.is_signed != 0) == (S != 0) && c.value == v)
            return ref;
        return _mk_const(E, v, S, W);
    }
    /* Wider than an int64 can spell: keep the constant at its own type and
     * convert it explicitly. */
    if (c.width == W && (c.is_signed != 0) == (S != 0) && (S || c.value >= 0))
        return ref;
    dvs_expr_t base = ref;
    if (!(c.width == cw && (c.is_signed != 0) == (cs != 0) && c.value == own))
        base = _mk_const(E, own, cs, cw);
    return _conv(E, base, W, S);
}

static dvs_expr_t _val(SvE *E, dvs_expr_t ref, uint16_t W, uint8_t S) {
    if (ref == EXPR_NULL || E->err) { E->err = 1; return EXPR_NULL; }
    uint64_t k = _key(ref, W, S, M_VAL);
    uint32_t mv;
    if (_memo_get(E, k, &mv)) return mv;
    if (++E->depth > SV_MAX_DEPTH) { E->err = 1; E->depth--; return EXPR_NULL; }

    dvs_expr_t out = EXPR_NULL;
    switch (_kind(E, ref)) {
    case EXPR_VAR:
        out = _conv(E, ref, W, S);
        break;
    case EXPR_CONST:
        out = _val_const(E, ref, W, S);
        break;
    case EXPR_BINARY: {
        ExprBinary b = *(ExprBinary *)_P(E, ref);
        if (_is_arith(b.op)) {
            dvs_expr_t l = _val(E, b.lhs, W, S);
            dvs_expr_t r = _val(E, b.rhs, W, S);
            if (E->err) break;
            if (_type(E, l).w < W && _type(E, r).w < W) l = _widen(E, l, W);
            out = (l == b.lhs && r == b.rhs) ? ref : _mk_bin(E, b.op, l, r);
        } else if (b.op == DVS_BIN_LSHIFT || b.op == DVS_BIN_RSHIFT ||
                   b.op == DVS_BIN_ASHR) {
            /* The amount is self-determined and always read unsigned. */
            SvTy rt = _type(E, b.rhs);
            dvs_expr_t r = _val(E, b.rhs, rt.w, 0);
            if (b.op == DVS_BIN_ASHR) {
                /* SV `>>>`: in a signed context an ARITHMETIC shift of the
                 * W-bit pattern (= floor(v / 2^k) of the signed value, which
                 * the engines compute for a signed left operand); in an
                 * unsigned context it is exactly `>>`. The engines thus only
                 * ever see DVS_BIN_ASHR with a signed left operand. */
                dvs_expr_t l = _widen(E, _val(E, b.lhs, W, S), W);
                if (E->err) break;
                dvs_binop_t op = S ? DVS_BIN_ASHR : DVS_BIN_RSHIFT;
                out = (op == b.op && l == b.lhs && r == b.rhs)
                    ? ref : _mk_bin(E, op, l, r);
            } else if (b.op == DVS_BIN_LSHIFT || !S) {
                dvs_expr_t l = _widen(E, _val(E, b.lhs, W, S), W);
                if (E->err) break;
                out = (l == b.lhs && r == b.rhs) ? ref : _mk_bin(E, b.op, l, r);
            } else {
                /* SV `>>` of a signed value is a LOGICAL shift of its W-bit
                 * pattern: reinterpret unsigned, shift, reinterpret signed. */
                dvs_expr_t l = _val(E, b.lhs, W, 1);
                if (E->err) break;
                dvs_expr_t lu = _mk_cast(E, l, _type(E, l).w, W, 1, 0);
                dvs_expr_t sh = _mk_bin(E, DVS_BIN_RSHIFT, lu, r);
                out = _mk_cast(E, sh, W, W, 0, 1);
            }
        } else {
            out = _conv(E, _bool(E, ref), W, S);
        }
        break;
    }
    case EXPR_UNARY: {
        ExprUnary u = *(ExprUnary *)_P(E, ref);
        if (u.op == DVS_UN_NOT) {
            out = _conv(E, _bool(E, ref), W, S);
        } else if (u.op == DVS_UN_NEG) {
            /* -x == 0 - x at the context width: wraps like every other op. */
            dvs_expr_t x = _val(E, u.operand, W, S);
            dvs_expr_t z = _mk_const(E, 0, S, W);
            out = _mk_bin(E, DVS_BIN_SUB, z, x);
        } else {
            dvs_expr_t x = _widen(E, _val(E, u.operand, W, S), W);
            if (E->err) break;
            if (W <= 64) {
                /* ~x == x ^ all-ones at the context width. */
                int64_t ones = S ? -1 : (W >= 64 ? -1 : (int64_t)(((uint64_t)1 << W) - 1));
                out = _mk_bin(E, DVS_BIN_BXOR, x, _mk_const(E, ones, S, W));
            } else {
                out = (x == u.operand) ? ref : _mk_un(E, DVS_UN_INVERT, x);
            }
        }
        break;
    }
    case EXPR_ITE: {
        ExprITE i = *(ExprITE *)_P(E, ref);
        dvs_expr_t c = _bool(E, i.cond);
        dvs_expr_t t = _val(E, i.then_e, W, S);
        dvs_expr_t e = _val(E, i.else_e, W, S);
        if (E->err) break;
        if (_type(E, t).w < W && _type(E, e).w < W) t = _widen(E, t, W);
        out = (c == i.cond && t == i.then_e && e == i.else_e)
            ? ref : _mk_ite(E, c, t, e);
        break;
    }
    case EXPR_EXTEND: {
        ExprExtend x = *(ExprExtend *)_P(E, ref);
        SvTy ot = _type(E, x.operand);
        dvs_expr_t o = _val(E, x.operand, ot.w, ot.s);
        if (E->err) break;
        dvs_expr_t n = (o == x.operand) ? ref
                  : _mk_ext(E, o, x.from_bits, x.to_bits, x.sign_extend);
        out = _conv(E, n, W, S);
        break;
    }
    case EXPR_SV_CAST: {
        ExprSvCast x = *(ExprSvCast *)_P(E, ref);
        SvTy ot = _type(E, x.operand);
        dvs_expr_t o = _val(E, x.operand, ot.w, ot.s);
        if (E->err) break;
        dvs_expr_t n = (o == x.operand) ? ref
                  : _mk_cast(E, o, x.from_bits, x.to_bits, x.sign_extend,
                             x.dst_signed);
        out = _conv(E, n, W, S);
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract x = *(ExprExtract *)_P(E, ref);
        SvTy ot = _type(E, x.operand);
        dvs_expr_t o = _val(E, x.operand, ot.w, ot.s);
        if (E->err) break;
        dvs_expr_t n = (o == x.operand) ? ref : _mk_extract(E, o, x.hi_bit, x.lo_bit);
        out = _conv(E, n, W, S);
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat x = *(ExprConcat *)_P(E, ref);
        SvTy ht = _type(E, x.hi), lt = _type(E, x.lo);
        dvs_expr_t h = _val(E, x.hi, ht.w, ht.s);
        dvs_expr_t l = _val(E, x.lo, lt.w, lt.s);
        if (E->err) break;
        dvs_expr_t n = (h == x.hi && l == x.lo) ? ref
                  : _mk_concat(E, h, l, x.lo_width);
        out = _conv(E, n, W, S);
        break;
    }
    case EXPR_IN_RANGE:
    case EXPR_IN_SET:
    case EXPR_IN_RANGES:
        out = _conv(E, _bool(E, ref), W, S);
        break;
    default:
        out = ref;     /* sum / countones / clog2 / select: not SV operators */
        break;
    }
    E->depth--;
    if (out == EXPR_NULL) E->err = 1;
    _memo_put(E, k, out);
    return out;
}

/* Can this (elaborated, unsigned 64-bit) node take a value >= 2^63? A
 * variable cannot (the int64 API caps an unsigned 64-bit domain at
 * INT64_MAX), nor can a zero-extension of something narrower; a computed or
 * converted 64-bit value can. */
static int _may_be_huge(SvE *E, dvs_expr_t x) {
    SvTy t = _type(E, x);
    if (t.w < 64 || t.s) return 0;
    ExprKind k = _kind(E, x);
    if (k == EXPR_VAR || k == EXPR_CONST) return 0;
    if (k == EXPR_EXTEND) {
        ExprExtend ee = *(ExprExtend *)_P(E, x);
        if (!ee.sign_extend && ee.from_bits < 64) return 0;
    }
    return 1;
}

/* Comparison `l op r` in its own SV context. */
static dvs_expr_t _cmp(SvE *E, dvs_binop_t op, dvs_expr_t l0, dvs_expr_t r0, dvs_expr_t reuse) {
    SvTy lt = _type(E, l0), rt = _type(E, r0);
    uint16_t W = lt.w > rt.w ? lt.w : rt.w;
    uint8_t  S = (uint8_t)(lt.s && rt.s);
    dvs_expr_t l = _val(E, l0, W, S);
    dvs_expr_t r = _val(E, r0, W, S);
    if (E->err) return EXPR_NULL;

    /* An UNSIGNED 64-bit context can hold values >= 2^63, which the engines'
     * int64 storage spells as negative patterns ordered unsigned -- an order
     * only a 64-bit unsigned variable knows to apply. A narrower operand
     * compared with such a value would read it as negative. So in that
     * context two non-constant sides are both made 64 bits wide. (Against a
     * constant, the range fold below keeps the constant within the
     * variable's own range, where the readings agree.) */
    if (W == 64 && !S && _kind(E, l) != EXPR_CONST && _kind(E, r) != EXPR_CONST) {
        if (_may_be_huge(E, r)) l = _widen(E, l, 64);
        if (_may_be_huge(E, l)) r = _widen(E, r, 64);
        if (E->err) return EXPR_NULL;
    }

    /* `var op const` decided by the variable's whole range folds to a
     * constant. The engines' var-const fast paths tighten the variable's
     * bounds with the constant, which is only meaningful when the constant
     * lies inside that range (an unsigned 64-bit context can hand an 8-bit
     * variable the constant 2^64-1, whose int64 spelling is -1). */
    if (W <= 64) {
        ExprKind lk = _kind(E, l), rk = _kind(E, r);
        if ((lk == EXPR_VAR && rk == EXPR_CONST) ||
            (lk == EXPR_CONST && rk == EXPR_VAR)) {
            int var_left = (lk == EXPR_VAR);
            SvTy vt = _type(E, var_left ? l : r);
            ExprConst c = *(ExprConst *)_P(E, var_left ? r : l);
            if (vt.w <= 64) {
                dvs_i128 cv = c.is_signed ? dvs_i128_from_i64(c.value)
                                          : dvs_i128_from_u64((uint64_t)c.value);
                dvs_i128 vmin, vmax;
                if (vt.s) {
                    vmin = dvs_i128_from_i64(vt.w >= 64 ? INT64_MIN
                                             : -((int64_t)1 << (vt.w - 1)));
                    vmax = dvs_i128_from_i64(vt.w >= 64 ? INT64_MAX
                                             : ((int64_t)1 << (vt.w - 1)) - 1);
                } else {
                    vmin = dvs_i128_from_i64(0);
                    vmax = dvs_i128_from_u64(vt.w >= 64 ? UINT64_MAX
                                             : ((uint64_t)1 << vt.w) - 1);
                }
                dvs_binop_t vop = op;      /* as `var vop const` */
                if (!var_left) {
                    switch (op) {
                    case DVS_BIN_LT:  vop = DVS_BIN_GT;  break;
                    case DVS_BIN_LTE: vop = DVS_BIN_GTE; break;
                    case DVS_BIN_GT:  vop = DVS_BIN_LT;  break;
                    case DVS_BIN_GTE: vop = DVS_BIN_LTE; break;
                    default: break;
                    }
                }
                int below = dvs_i128_lt(cv, vmin);      /* cv <  vmin */
                int above = dvs_i128_lt(vmax, cv);      /* cv >  vmax */
                int at_lo = dvs_i128_le(cv, vmin);      /* cv <= vmin */
                int at_hi = dvs_i128_le(vmax, cv);      /* cv >= vmax */
                int t = -1;
                switch (vop) {
                case DVS_BIN_EQ:  if (below || above) t = 0; break;
                case DVS_BIN_NEQ: if (below || above) t = 1; break;
                case DVS_BIN_LT:  t = at_lo ? 0 : (above ? 1 : -1); break;
                case DVS_BIN_LTE: t = below ? 0 : (at_hi ? 1 : -1); break;
                case DVS_BIN_GT:  t = at_hi ? 0 : (below ? 1 : -1); break;
                case DVS_BIN_GTE: t = above ? 0 : (at_lo ? 1 : -1); break;
                default: break;
                }
                if (t >= 0) return _mk_const(E, t, 0, 1);
            }
        }
    }
    if (reuse != EXPR_NULL && l == l0 && r == r0) return reuse;
    return _mk_bin(E, op, l, r);
}

/* Is `v` a variable whose value no membership context changes? */
static int _in_keepable(SvE *E, dvs_expr_t v, const dvs_expr_t *items, uint32_t n) {
    if (_kind(E, v) != EXPR_VAR) return 0;
    SvTy vt = _type(E, v);
    for (uint32_t i = 0; i < n; i++) {
        SvTy it = _type(E, items[i]);
        if ((uint8_t)(vt.s && it.s) != vt.s) return 0;
        /* unsigned 64-bit context with a non-constant item: see _cmp */
        if (!vt.s && (it.w >= 64 || vt.w >= 64) && it.w != vt.w &&
            _kind(E, items[i]) != EXPR_CONST)
            return 0;
    }
    return 1;
}

static dvs_expr_t _in(SvE *E, dvs_expr_t ref) {
    ExprKind k = _kind(E, ref);
    if (k == EXPR_IN_RANGE) {
        ExprInRange x = *(ExprInRange *)_P(E, ref);
        dvs_expr_t items[2] = { x.lo, x.hi };
        if (_in_keepable(E, x.value, items, 2)) {
            SvTy vt = _type(E, x.value);
            SvTy a = _type(E, x.lo), b = _type(E, x.hi);
            dvs_expr_t lo = _val(E, x.lo, a.w > vt.w ? a.w : vt.w, vt.s);
            dvs_expr_t hi = _val(E, x.hi, b.w > vt.w ? b.w : vt.w, vt.s);
            if (E->err) return EXPR_NULL;
            return (lo == x.lo && hi == x.hi) ? ref
                 : _mk_in_range(E, x.value, lo, hi);
        }
        dvs_expr_t ge = _cmp(E, DVS_BIN_GTE, x.value, x.lo, EXPR_NULL);
        dvs_expr_t le = _cmp(E, DVS_BIN_LTE, x.value, x.hi, EXPR_NULL);
        return _mk_bin(E, DVS_BIN_AND, ge, le);
    }

    /* IN_SET / IN_RANGES: copy the item refs out (the pool may move). */
    uint32_t n;
    dvs_expr_t v;
    if (k == EXPR_IN_SET) {
        ExprInSet *s = (ExprInSet *)_P(E, ref);
        n = s->n_elems; v = s->value;
    } else {
        ExprInRanges *s = (ExprInRanges *)_P(E, ref);
        n = 2 * s->n_ranges; v = s->value;
    }
    if (n == 0) return ref;
    dvs_expr_t *it = (dvs_expr_t *)malloc(n * sizeof(dvs_expr_t));
    dvs_expr_t *nw = (dvs_expr_t *)malloc(n * sizeof(dvs_expr_t));
    if (!it || !nw) { free(it); free(nw); E->err = E->oom = 1; return EXPR_NULL; }
    memcpy(it, (char *)_P(E, ref) + (k == EXPR_IN_SET ? sizeof(ExprInSet)
                                                      : sizeof(ExprInRanges)),
           n * sizeof(dvs_expr_t));
    dvs_expr_t out = EXPR_NULL;
    if (_in_keepable(E, v, it, n)) {
        SvTy vt = _type(E, v);
        int changed = 0;
        for (uint32_t i = 0; i < n && !E->err; i++) {
            SvTy t = _type(E, it[i]);
            nw[i] = _val(E, it[i], t.w > vt.w ? t.w : vt.w, vt.s);
            if (nw[i] != it[i]) changed = 1;
        }
        if (!E->err)
            out = !changed ? ref
                : (k == EXPR_IN_SET ? _mk_in_set(E, v, n, nw)
                                    : _mk_in_ranges(E, v, n / 2, nw, nw + n / 2));
    } else if (k == EXPR_IN_SET) {
        for (uint32_t i = 0; i < n && !E->err; i++) {
            dvs_expr_t eq = _cmp(E, DVS_BIN_EQ, v, it[i], EXPR_NULL);
            out = (i == 0) ? eq : _mk_bin(E, DVS_BIN_OR, out, eq);
        }
    } else {
        uint32_t nr = n / 2;
        for (uint32_t i = 0; i < nr && !E->err; i++) {
            dvs_expr_t ge = _cmp(E, DVS_BIN_GTE, v, it[i], EXPR_NULL);
            dvs_expr_t le = _cmp(E, DVS_BIN_LTE, v, it[nr + i], EXPR_NULL);
            dvs_expr_t a = _mk_bin(E, DVS_BIN_AND, ge, le);
            out = (i == 0) ? a : _mk_bin(E, DVS_BIN_OR, out, a);
        }
    }
    free(it);
    free(nw);
    return out;
}

static dvs_expr_t _bool(SvE *E, dvs_expr_t ref) {
    if (ref == EXPR_NULL || E->err) { E->err = 1; return EXPR_NULL; }
    uint64_t k = _key(ref, 0, 0, M_BOOL);
    uint32_t mv;
    if (_memo_get(E, k, &mv)) return mv;
    if (++E->depth > SV_MAX_DEPTH) { E->err = 1; E->depth--; return EXPR_NULL; }

    dvs_expr_t out = ref;
    switch (_kind(E, ref)) {
    case EXPR_VAR:
    case EXPR_CONST:
        out = ref;     /* truthiness does not depend on typing */
        break;
    case EXPR_BINARY: {
        ExprBinary b = *(ExprBinary *)_P(E, ref);
        if (b.op == DVS_BIN_AND || b.op == DVS_BIN_OR) {
            dvs_expr_t l = _bool(E, b.lhs), r = _bool(E, b.rhs);
            if (E->err) break;
            out = (l == b.lhs && r == b.rhs) ? ref : _mk_bin(E, b.op, l, r);
        } else if (_is_cmp(b.op)) {
            out = _cmp(E, b.op, b.lhs, b.rhs, ref);
        } else {
            SvTy t = _type(E, ref);
            out = _val(E, ref, t.w, t.s);
        }
        break;
    }
    case EXPR_UNARY: {
        ExprUnary u = *(ExprUnary *)_P(E, ref);
        if (u.op == DVS_UN_NOT) {
            dvs_expr_t x = _bool(E, u.operand);
            if (E->err) break;
            out = (x == u.operand) ? ref : _mk_un(E, DVS_UN_NOT, x);
        } else {
            SvTy t = _type(E, ref);
            out = _val(E, ref, t.w, t.s);
        }
        break;
    }
    case EXPR_ITE: {
        ExprITE i = *(ExprITE *)_P(E, ref);
        dvs_expr_t c = _bool(E, i.cond);
        dvs_expr_t t = _bool(E, i.then_e);
        dvs_expr_t e = _bool(E, i.else_e);
        if (E->err) break;
        out = (c == i.cond && t == i.then_e && e == i.else_e)
            ? ref : _mk_ite(E, c, t, e);
        break;
    }
    case EXPR_IN_RANGE:
    case EXPR_IN_SET:
    case EXPR_IN_RANGES:
        out = _in(E, ref);
        break;
    case EXPR_EXTEND:
    case EXPR_EXTRACT:
    case EXPR_CONCAT:
    case EXPR_SV_CAST: {
        SvTy t = _type(E, ref);
        out = _val(E, ref, t.w, t.s);
        break;
    }
    default:
        out = ref;
        break;
    }
    E->depth--;
    if (out == EXPR_NULL) E->err = 1;
    _memo_put(E, k, out);
    return out;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

dvs_problem_t *dvs_sv_elaborate(dvs_problem_t *sp, dvs_sv_var_type_fn fn,
                               void *ud, int *err) {
    if (err) *err = 0;
    if (!sp || (sp->flags & DVS_PROBLEM_F_EXPLICIT)) return sp;
    if (sp->constraints_head == EXPR_NULL && sp->softs_head == EXPR_NULL)
        return sp;

    SvE E;
    memset(&E, 0, sizeof(E));
    E.src = sp;
    E.vfn = fn;
    E.vud = ud;

    /* Variable types from the VarSpecs. */
    uint32_t max_id = 0;
    int any = 0;
    for (dvs_expr_t c = sp->vars_head; c != EXPR_NULL; ) {
        VarSpec *v = (VarSpec *)POOL_PTR(sp, c);
        if (!any || v->var_id > max_id) max_id = v->var_id;
        any = 1;
        c = v->next;
    }
    if (any) {
        E.nv = max_id + 1;
        E.vw = (uint16_t *)calloc(E.nv, sizeof(uint16_t));
        E.vs = (uint8_t *)calloc(E.nv, 1);
        E.vk = (uint8_t *)calloc(E.nv, 1);
        if (!E.vw || !E.vs || !E.vk) E.err = E.oom = 1;
        for (dvs_expr_t c = sp->vars_head; c != EXPR_NULL && !E.err; ) {
            VarSpec *v = (VarSpec *)POOL_PTR(sp, c);
            E.vw[v->var_id] = v->width;
            E.vs[v->var_id] = v->is_signed ? 1 : 0;
            E.vk[v->var_id] = 1;
            c = v->next;
        }
    }

    for (dvs_expr_t c = sp->constraints_head; c != EXPR_NULL && !E.err; ) {
        ConstraintSpec cs = *(ConstraintSpec *)_P(&E, c);
        if (cs.root != EXPR_NULL) {
            dvs_expr_t nr = _bool(&E, cs.root);
            if (!E.err && nr != cs.root)
                ((ConstraintSpec *)_P(&E, c))->root = nr;
        }
        c = cs.next;
    }
    for (dvs_expr_t c = sp->softs_head; c != EXPR_NULL && !E.err; ) {
        SoftSpec ss = *(SoftSpec *)_P(&E, c);
        if (ss.root != EXPR_NULL) {
            dvs_expr_t nr = _bool(&E, ss.root);
            if (!E.err && nr != ss.root)
                ((SoftSpec *)_P(&E, c))->root = nr;
        }
        c = ss.next;
    }

    free(E.vw); free(E.vs); free(E.vk);
    free(E.memo);
    if (E.err) {
        free(E.dst);
        if (err) *err = E.unknown_var ? DVS_SV_ERR_UNKNOWN_VAR
                      : E.oom         ? DVS_SV_ERR_NOMEM
                      :                 DVS_SV_ERR_UNSUPPORTED;
        return sp;
    }
    if (!E.dst) return sp;
    E.dst->flags |= DVS_PROBLEM_F_EXPLICIT;
    return E.dst;
}

int dvs_sv_elaborate_more(dvs_problem_t **elab, const dvs_problem_t *orig,
                          uint32_t *synced_used, dvs_expr_t root,
                          dvs_expr_t *out_root) {
    if (!elab || !*elab || !orig || !synced_used || !out_root) return -1;
    dvs_problem_t *d = *elab;
    /* 1. Sync what the caller appended to its own problem since. Those bytes
     *    lie below orig's capacity, where the copy holds orig's own region;
     *    the elaborated nodes start above it. */
    uint32_t ou = orig->pool.used;
    if (ou > *synced_used) {
        if (ou > d->pool.used) return -1;   /* would overlap elaborated nodes */
        memcpy((char *)&d->pool + sizeof(dvs_pool_t) + *synced_used,
               (const char *)&orig->pool + sizeof(dvs_pool_t) + *synced_used,
               ou - *synced_used);
        *synced_used = ou;
    }
    /* Header: the variable / constraint lists as the caller now has them. */
    {
        dvs_pool_t pool = d->pool;
        uint32_t flags = d->flags;
        memcpy(d, orig, sizeof(dvs_problem_t));
        d->pool = pool;
        d->flags = flags;
    }

    /* 2. Elaborate the new root, appending to the copy in place. */
    SvE E;
    memset(&E, 0, sizeof(E));
    E.src = d;
    E.dst = d;
    uint32_t max_id = 0;
    int any = 0;
    for (dvs_expr_t c = d->vars_head; c != EXPR_NULL; ) {
        VarSpec *v = (VarSpec *)POOL_PTR(d, c);
        if (!any || v->var_id > max_id) max_id = v->var_id;
        any = 1;
        c = v->next;
    }
    if (any) {
        E.nv = max_id + 1;
        E.vw = (uint16_t *)calloc(E.nv, sizeof(uint16_t));
        E.vs = (uint8_t *)calloc(E.nv, 1);
        E.vk = (uint8_t *)calloc(E.nv, 1);
        if (!E.vw || !E.vs || !E.vk) E.err = E.oom = 1;
        for (dvs_expr_t c = d->vars_head; c != EXPR_NULL && !E.err; ) {
            VarSpec *v = (VarSpec *)POOL_PTR(d, c);
            E.vw[v->var_id] = v->width;
            E.vs[v->var_id] = v->is_signed ? 1 : 0;
            E.vk[v->var_id] = 1;
            c = v->next;
        }
    }
    dvs_expr_t nr = E.err ? EXPR_NULL : _bool(&E, root);
    free(E.vw); free(E.vs); free(E.vk);
    free(E.memo);
    *elab = E.dst;                /* may have moved */
    if (E.err || nr == EXPR_NULL) return -1;
    *out_root = nr;
    return 0;
}

void dvs_sv_release(dvs_problem_t *orig, dvs_problem_t *elab) {
    if (elab && elab != orig) free(elab);
}
