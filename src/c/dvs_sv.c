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
#include "dvs_stackinfo.h"

#define SV_POOL_HDR   ((uint32_t)sizeof(dvs_pool_t))
#define SV_MAX_W      65535u    /* node formats hold widths in a uint16_t */
/* Nesting limit where the stack cannot be measured (dvs_stack_deep); where it
 * can, the stack itself is the limit. */
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
    dvs_stack_guard_t sg;      /* recursion stops when the stack runs low */
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
    n->width = w;
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
    n->from_bits = from; n->to_bits = to; n->_p8 = 0; n->_p16 = 0;
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
    n->from_bits = from; n->to_bits = to; n->_p16 = 0;
    n->dst_signed = dst_s; n->operand = x;
    return r;
}

static dvs_expr_t _mk_extract(SvE *E, dvs_expr_t x, uint16_t hi, uint16_t lo) {
    if (x == EXPR_NULL) { E->err = 1; return EXPR_NULL; }
    dvs_expr_t r = _alloc(E, sizeof(ExprExtract), _Alignof(ExprExtract));
    if (r == EXPR_NULL) return r;
    ExprExtract *n = (ExprExtract *)_P(E, r);
    n->kind = EXPR_EXTRACT; n->hi_bit = hi; n->lo_bit = lo;
    n->operand = x;
    return r;
}

static dvs_expr_t _mk_concat(SvE *E, dvs_expr_t hi, dvs_expr_t lo, uint16_t lo_w) {
    if (hi == EXPR_NULL || lo == EXPR_NULL) { E->err = 1; return EXPR_NULL; }
    dvs_expr_t r = _alloc(E, sizeof(ExprConcat), _Alignof(ExprConcat));
    if (r == EXPR_NULL) return r;
    ExprConcat *n = (ExprConcat *)_P(E, r);
    n->kind = EXPR_CONCAT; n->lo_width = lo_w; n->_p16 = 0;
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
    if (dvs_stack_deep(&E->sg, ++E->depth, SV_MAX_DEPTH)) { E->err = 1; E->depth--; return t; }

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
    case EXPR_CONST_WIDE: {
        const ExprConstWide *c = (const ExprConstWide *)_P(E, ref);
        if (c->width > SV_MAX_W) { E->err = 1; break; }
        t.w = (uint16_t)c->width; t.s = c->is_signed ? 1 : 0;
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
    case EXPR_CAST: {
        ExprCast x = *(ExprCast *)_P(E, ref);
        t.w = x.to_bits; t.s = x.to_signed ? 1 : 0;
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract x = *(ExprExtract *)_P(E, ref);
        if (x.hi_bit < x.lo_bit) { E->err = 1; break; }
        t.w = (uint16_t)(x.hi_bit - x.lo_bit + 1); t.s = 0;
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat x = *(ExprConcat *)_P(E, ref);
        uint32_t cw = (uint32_t)_type(E, x.hi).w + x.lo_width;
        if (cw > SV_MAX_W) { E->err = 1; break; }   /* a width would wrap */
        t.w = (uint16_t)cw; t.s = 0;
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

static dvs_expr_t _cmp(SvE *E, dvs_binop_t op, dvs_expr_t l0, dvs_expr_t r0,
                       dvs_expr_t reuse);

/* A value in a Boolean position (a constraint of its own, an operand of
 * && || !, a condition) holds when it is nonzero: `x != 0` in the value's
 * own type. The engines compile a comparison, and a bare variable, as a
 * constraint; a multi-bit value of any other shape they could not. */
static dvs_expr_t _nonzero(SvE *E, dvs_expr_t ref) {
    SvTy t = _type(E, ref);
    dvs_expr_t z = _mk_const(E, 0, t.s, t.w);
    if (E->err) return EXPR_NULL;
    return _cmp(E, DVS_BIN_NEQ, ref, z, EXPR_NULL);
}

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
    if (dvs_stack_deep(&E->sg, ++E->depth, SV_MAX_DEPTH)) { E->err = 1; E->depth--; return EXPR_NULL; }

    dvs_expr_t out = EXPR_NULL;
    switch (_kind(E, ref)) {
    case EXPR_VAR:
        out = _conv(E, ref, W, S);
        break;
    case EXPR_CONST:
        out = _val_const(E, ref, W, S);
        break;
    case EXPR_CONST_WIDE:
        out = _conv(E, ref, W, S);   /* a leaf at its own type, like a variable */
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
            /* ~x of the operand widened to the context width. Kept as `~`
             * rather than `x ^ all-ones`: the CDCL bnot propagator maps x's
             * range exactly (x in [0,10] gives ~x in [245,255] at 8 bits),
             * where the XOR propagator acts only on a fixed operand. */
            dvs_expr_t x = _widen(E, _val(E, u.operand, W, S), W);
            if (E->err) break;
            out = (x == u.operand) ? ref : _mk_un(E, DVS_UN_INVERT, x);
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
    case EXPR_CAST: {
        /* An assignment-like context: the operand is evaluated at the wider
         * of its own width and the cast's, at its own signedness; the value
         * is then truncated to the cast's width or extended by its own
         * signedness, and read at the cast's signedness. */
        ExprCast x = *(ExprCast *)_P(E, ref);
        SvTy ot = _type(E, x.operand);
        uint16_t ow = ot.w > x.to_bits ? ot.w : x.to_bits;
        dvs_expr_t o = _val(E, x.operand, ow, ot.s);
        if (E->err) break;
        SvTy vt = _type(E, o);
        dvs_expr_t n;
        if (vt.w > x.to_bits) {
            if (vt.w > SV_MAX_W) { E->err = 1; break; }
            n = _mk_extract(E, o, (uint16_t)(x.to_bits - 1), 0);
            if (x.to_signed)
                n = _mk_cast(E, n, x.to_bits, x.to_bits, 0, 1);
        } else {
            n = _conv(E, o, x.to_bits, x.to_signed ? 1 : 0);
        }
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
    if (dvs_stack_deep(&E->sg, ++E->depth, SV_MAX_DEPTH)) { E->err = 1; E->depth--; return EXPR_NULL; }

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
            out = _nonzero(E, ref);
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
            out = _nonzero(E, ref);
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
    case EXPR_CONST_WIDE:
    case EXPR_EXTEND:
    case EXPR_EXTRACT:
    case EXPR_CONCAT:
    case EXPR_SV_CAST:
    case EXPR_CAST:
        out = _nonzero(E, ref);
        break;
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

/* ------------------------------------------------------------------ */
/* Constant-mask rewriting                                             */
/* ------------------------------------------------------------------ */
/*
 * `(x & M) == V` gives the CDCL engine nothing about x: the bitwise-and
 * propagator only bounds its result, so the search draws x and rejects it, at
 * a cost that doubles with every masked bit (a 16-bit alignment mask: ~7 ms a
 * solve; 20 bits: the time limit). The same constraint on bit fields,
 * `x[h:l] == V[h:l]`, propagates back to x in microseconds. See
 * docs/bitfield_linearization_research.md.
 */

#define MRW_MAX_RUNS 16

/* x read as bits of a variable: bit i of x (i < avail) is bit i + off of the
 * variable `var` (`vw` bits wide); bits avail.. of x are zero. */
typedef struct {
    dvs_expr_t var;
    uint16_t   vw;
    uint16_t   off;
    uint16_t   avail;
} MrwSrc;

static int _mrw_src(SvE *E, dvs_expr_t x, uint16_t W, MrwSrc *o, int depth) {
    if (x == EXPR_NULL || depth > 8) return 0;
    switch (_kind(E, x)) {
    case EXPR_VAR: {
        SvTy t = _type(E, x);
        if (E->err || t.w > 64) return 0;
        /* Narrower than the context: extended by its own signedness. */
        if (t.w < W && t.s) return 0;
        o->var = x; o->vw = t.w; o->off = 0; o->avail = t.w;
        return 1;
    }
    case EXPR_EXTEND: {
        ExprExtend e = *(ExprExtend *)_P(E, x);
        if (e.sign_extend || e.to_bits < e.from_bits) return 0;
        if (!_mrw_src(E, e.operand, e.from_bits, o, depth + 1)) return 0;
        if (o->avail > e.from_bits) o->avail = e.from_bits;
        return 1;
    }
    case EXPR_SV_CAST: {
        ExprSvCast c = *(ExprSvCast *)_P(E, x);
        if (c.to_bits > c.from_bits && c.sign_extend) return 0;
        if (!_mrw_src(E, c.operand, c.from_bits, o, depth + 1)) return 0;
        uint16_t keep = c.to_bits < c.from_bits ? c.to_bits : c.from_bits;
        if (o->avail > keep) o->avail = keep;
        return 1;
    }
    case EXPR_EXTRACT: {
        ExprExtract e = *(ExprExtract *)_P(E, x);
        SvTy ot = _type(E, e.operand);
        if (E->err || e.hi_bit < e.lo_bit || e.hi_bit >= ot.w) return 0;
        if (!_mrw_src(E, e.operand, ot.w, o, depth + 1)) return 0;
        if (e.lo_bit >= o->avail) return 0;       /* all zero: leave it */
        o->off = (uint16_t)(o->off + e.lo_bit);
        o->avail = (uint16_t)(o->avail - e.lo_bit);
        uint16_t ew = (uint16_t)(e.hi_bit - e.lo_bit + 1);
        if (o->avail > ew) o->avail = ew;
        return 1;
    }
    case EXPR_BINARY: {
        ExprBinary b = *(ExprBinary *)_P(E, x);
        if (b.op != DVS_BIN_RSHIFT || _kind(E, b.rhs) != EXPR_CONST) return 0;
        SvTy lt = _type(E, b.lhs);
        if (E->err || lt.w != W) return 0;
        int64_t k = ((ExprConst *)_P(E, b.rhs))->value;
        if (!_mrw_src(E, b.lhs, W, o, depth + 1)) return 0;
        if (k < 0 || k >= o->avail) return 0;     /* all zero: leave it */
        o->off = (uint16_t)(o->off + k);
        o->avail = (uint16_t)(o->avail - k);
        return 1;
    }
    default:
        return 0;
    }
}

/* Bits [h:l] of x, all within src->avail: the variable itself or an extract. */
static dvs_expr_t _mrw_field(SvE *E, const MrwSrc *s, uint16_t h, uint16_t l) {
    uint16_t vh = (uint16_t)(h + s->off), vl = (uint16_t)(l + s->off);
    if (vl == 0 && vh == s->vw - 1) return s->var;
    return _mk_extract(E, s->var, vh, vl);
}

/* The value of a constant leaf as a W-bit pattern. */
static int _mrw_const(SvE *E, dvs_expr_t c, uint16_t W, uint64_t *out) {
    if (c == EXPR_NULL || _kind(E, c) != EXPR_CONST) return 0;
    ExprConst *k = (ExprConst *)_P(E, c);
    uint16_t w; uint8_t s;
    dvs_sv_const_type(k, &w, &s);
    if (w > 64 || W > 64) return 0;
    uint64_t m = W >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << W) - 1);
    *out = (uint64_t)dvs_sv_wrap(k->value, w, s) & m;
    return 1;
}

/* Runs of set bits of m (below W), low to high. */
static int _mrw_runs(uint64_t m, uint16_t W, uint16_t *lo, uint16_t *hi) {
    int n = 0;
    for (uint16_t i = 0; i < W; ) {
        if (!((m >> i) & 1u)) { i++; continue; }
        uint16_t j = i;
        while (j + 1 < W && ((m >> (j + 1)) & 1u)) j++;
        if (n == MRW_MAX_RUNS) return -1;
        lo[n] = i; hi[n] = j; n++;
        i = (uint16_t)(j + 1);
    }
    return n;
}

static uint64_t _mrw_bits(uint64_t v, uint16_t h, uint16_t l) {
    uint16_t n = (uint16_t)(h - l + 1);
    return (v >> l) & (n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1));
}

/* `x & M` (either operand order) or, with or_form, `x | M`: x and M's
 * pattern. */
static int _mrw_masked(SvE *E, dvs_expr_t a, uint16_t W, int or_form,
                       dvs_expr_t *x, uint64_t *m) {
    if (_kind(E, a) != EXPR_BINARY) return 0;
    ExprBinary b = *(ExprBinary *)_P(E, a);
    if (b.op != (or_form ? DVS_BIN_BOR : DVS_BIN_BAND)) return 0;
    if (_mrw_const(E, b.rhs, W, m)) { *x = b.lhs; return 1; }
    if (_mrw_const(E, b.lhs, W, m)) { *x = b.rhs; return 1; }
    return 0;
}

/* (x & M) == V or != V over x's runs, or EXPR_NULL. For the OR form, the
 * runs are those of ~M and M's bits of V must be ones. */
static dvs_expr_t _mrw_eq_const(SvE *E, dvs_binop_t op, dvs_expr_t a, dvs_expr_t c,
                                uint16_t W) {
    uint64_t wm = W >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << W) - 1);
    for (int or_form = 0; or_form < 2; or_form++) {
        dvs_expr_t x; uint64_t m, v;
        if (!_mrw_masked(E, a, W, or_form, &x, &m) || !_mrw_const(E, c, W, &v)) continue;
        if (or_form) {
            if ((v & m) != m) return EXPR_NULL;      /* constant: leave it */
            m = ~m & wm;
        } else if (v & ~m & wm) {
            return EXPR_NULL;                        /* constant: leave it */
        }
        MrwSrc s;
        if (!_mrw_src(E, x, W, &s, 0)) return EXPR_NULL;
        uint16_t lo[MRW_MAX_RUNS], hi[MRW_MAX_RUNS];
        int n = _mrw_runs(m, W, lo, hi);
        if (n <= 0) return EXPR_NULL;
        dvs_expr_t acc = EXPR_NULL;
        for (int i = 0; i < n; i++) {
            /* Bits at or above avail are zero in x. */
            if (lo[i] >= s.avail) {
                if (_mrw_bits(v, hi[i], lo[i])) return EXPR_NULL;
                continue;
            }
            uint16_t h = hi[i] < s.avail ? hi[i] : (uint16_t)(s.avail - 1);
            if (h < hi[i] && _mrw_bits(v, hi[i], (uint16_t)(h + 1))) return EXPR_NULL;
            uint16_t n_w = (uint16_t)(h - lo[i] + 1);
            dvs_expr_t f = _mrw_field(E, &s, h, lo[i]);
            dvs_expr_t k = _mk_const(E, (int64_t)_mrw_bits(v, h, lo[i]), 0, n_w);
            dvs_expr_t t = _mk_bin(E, op, f, k);
            acc = acc == EXPR_NULL ? t
                : _mk_bin(E, op == DVS_BIN_EQ ? DVS_BIN_AND : DVS_BIN_OR, acc, t);
            if (E->err) return EXPR_NULL;
        }
        return acc;
    }
    return EXPR_NULL;
}

/* (x & M) == (y & M) or != with one M. */
static dvs_expr_t _mrw_eq_masked(SvE *E, dvs_binop_t op, dvs_expr_t a, dvs_expr_t b,
                                 uint16_t W) {
    dvs_expr_t x, y; uint64_t mx, my;
    if (!_mrw_masked(E, a, W, 0, &x, &mx) || !_mrw_masked(E, b, W, 0, &y, &my) || mx != my)
        return EXPR_NULL;
    MrwSrc sx, sy;
    if (!_mrw_src(E, x, W, &sx, 0) || !_mrw_src(E, y, W, &sy, 0)) return EXPR_NULL;
    uint16_t lo[MRW_MAX_RUNS], hi[MRW_MAX_RUNS];
    int n = _mrw_runs(mx, W, lo, hi);
    if (n <= 0) return EXPR_NULL;
    dvs_expr_t acc = EXPR_NULL;
    for (int i = 0; i < n; i++) {
        if (hi[i] >= sx.avail || hi[i] >= sy.avail) return EXPR_NULL;
        dvs_expr_t t = _mk_bin(E, op, _mrw_field(E, &sx, hi[i], lo[i]),
                               _mrw_field(E, &sy, hi[i], lo[i]));
        acc = acc == EXPR_NULL ? t
            : _mk_bin(E, op == DVS_BIN_EQ ? DVS_BIN_AND : DVS_BIN_OR, acc, t);
        if (E->err) return EXPR_NULL;
    }
    return acc;
}

/* (x & M) op K, op an unsigned ordering, M one run [h:l]: with f = x[h:l],
 * x & M == f * 2^l, so
 *   < K  : f <  ceil(K / 2^l)      <= K : f <= floor(K / 2^l)
 *   > K  : f >  floor(K / 2^l)     >= K : f >= ceil(K / 2^l)
 * A comparison the field's range decides either way is left as it was. */
static dvs_expr_t _mrw_order(SvE *E, dvs_binop_t op, dvs_expr_t a, dvs_expr_t c,
                             uint16_t W) {
    dvs_expr_t x; uint64_t m, k;
    if (!_mrw_masked(E, a, W, 0, &x, &m) || !_mrw_const(E, c, W, &k)) return EXPR_NULL;
    if (_type(E, a).s || _type(E, c).s || E->err) return EXPR_NULL;
    uint16_t lo[MRW_MAX_RUNS], hi[MRW_MAX_RUNS];
    if (_mrw_runs(m, W, lo, hi) != 1) return EXPR_NULL;
    MrwSrc s;
    if (!_mrw_src(E, x, W, &s, 0) || hi[0] >= s.avail) return EXPR_NULL;
    uint16_t l = lo[0], n_w = (uint16_t)(hi[0] - lo[0] + 1);
    uint64_t fmax = n_w >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n_w) - 1);
    uint64_t fl = l ? k >> l : k;                               /* floor */
    uint64_t ce = fl + ((l && (k & (((uint64_t)1 << l) - 1))) ? 1 : 0);  /* ceil */
    uint64_t b;
    switch (op) {
    case DVS_BIN_LT:  if (ce == 0 || ce > fmax) return EXPR_NULL; b = ce; break;
    case DVS_BIN_LTE: if (fl >= fmax) return EXPR_NULL;           b = fl; break;
    case DVS_BIN_GT:  if (fl >= fmax) return EXPR_NULL;           b = fl; break;
    case DVS_BIN_GTE: if (ce == 0 || ce > fmax) return EXPR_NULL; b = ce; break;
    default: return EXPR_NULL;
    }
    return _mk_bin(E, op, _mrw_field(E, &s, hi[0], l), _mk_const(E, (int64_t)b, 0, n_w));
}

/* `s` read as a whole unsigned variable, zero-extended at most: that
 * variable, or EXPR_NULL. */
static dvs_expr_t _mrw_shift_var(SvE *E, dvs_expr_t s, uint16_t *vw) {
    SvTy t = _type(E, s);
    MrwSrc o;
    if (E->err || t.s || !_mrw_src(E, s, t.w, &o, 0)) return EXPR_NULL;
    if (o.off != 0 || o.avail != o.vw || _type(E, o.var).s || E->err) return EXPR_NULL;
    *vw = o.vw;
    return o.var;
}

/* `1 << s`, s a whole unsigned variable: s, or EXPR_NULL. */
static dvs_expr_t _mrw_pow2(SvE *E, dvs_expr_t a, uint16_t *vw) {
    if (a == EXPR_NULL || _kind(E, a) != EXPR_BINARY) return EXPR_NULL;
    ExprBinary b = *(ExprBinary *)_P(E, a);
    uint64_t one;
    SvTy t = _type(E, a);
    if (E->err || b.op != DVS_BIN_LSHIFT || t.s || t.w > 64
            || !_mrw_const(E, b.lhs, t.w, &one) || one != 1) return EXPR_NULL;
    dvs_expr_t v = _mrw_shift_var(E, b.rhs, vw);
    /* Every shift must stay inside the word: 1 << k is 2^k. */
    if (v == EXPR_NULL || *vw > 6 || ((1u << *vw) - 1) >= t.w) return EXPR_NULL;
    return v;
}

#define MRW_MAX_SHIFT_VW 4

/* (x & ((1 << s) - 1)) == 0 or x % (1 << s) == 0 -- alignment to a random
 * size, as AXI testbenches write it -- or their !=, s a narrow unsigned
 * variable: the slice form `s == k -> x[k-1:0] == 0` for every k, which
 * propagates like any field constraint. */
static dvs_expr_t _mrw_align(SvE *E, dvs_binop_t op, dvs_expr_t a, dvs_expr_t c,
                             uint16_t W) {
    uint64_t z;
    if (!_mrw_const(E, c, W, &z) || z != 0 || _kind(E, a) != EXPR_BINARY) return EXPR_NULL;
    ExprBinary b = *(ExprBinary *)_P(E, a);
    if (_type(E, a).s || E->err) return EXPR_NULL;
    dvs_expr_t x = EXPR_NULL, sv = EXPR_NULL;
    uint16_t vw = 0;
    if (b.op == DVS_BIN_MOD) {
        x = b.lhs;
        sv = _mrw_pow2(E, b.rhs, &vw);
    } else if (b.op == DVS_BIN_BAND) {
        for (int side = 0; side < 2 && sv == EXPR_NULL; side++) {
            dvs_expr_t m = side ? b.lhs : b.rhs;
            if (_kind(E, m) != EXPR_BINARY) continue;
            ExprBinary mb = *(ExprBinary *)_P(E, m);
            uint64_t one;
            if (mb.op != DVS_BIN_SUB || !_mrw_const(E, mb.rhs, _type(E, m).w, &one) || one != 1)
                continue;
            sv = _mrw_pow2(E, mb.lhs, &vw);
            x = side ? b.rhs : b.lhs;
        }
    }
    if (sv == EXPR_NULL || vw > MRW_MAX_SHIFT_VW || _type(E, x).s || E->err) return EXPR_NULL;
    MrwSrc src;
    if (!_mrw_src(E, x, W, &src, 0)) return EXPR_NULL;
    dvs_expr_t acc = EXPR_NULL;
    for (uint32_t k = 1; k < (1u << vw); k++) {
        uint16_t h = (uint16_t)(k <= src.avail ? k - 1 : src.avail - 1);
        dvs_expr_t is_k = _mk_bin(E, op == DVS_BIN_EQ ? DVS_BIN_NEQ : DVS_BIN_EQ,
                                  sv, _mk_const(E, (int64_t)k, 0, vw));
        dvs_expr_t f = _mk_bin(E, op, _mrw_field(E, &src, h, 0),
                               _mk_const(E, 0, 0, (uint16_t)(h + 1)));
        /* ==: s != k || x[k-1:0] == 0, all k.  !=: s == k && x[k-1:0] != 0,
         * some k. */
        dvs_expr_t t = _mk_bin(E, op == DVS_BIN_EQ ? DVS_BIN_OR : DVS_BIN_AND, is_k, f);
        acc = acc == EXPR_NULL ? t
            : _mk_bin(E, op == DVS_BIN_EQ ? DVS_BIN_AND : DVS_BIN_OR, acc, t);
        if (E->err) return EXPR_NULL;
    }
    return acc;
}

static dvs_binop_t _mrw_flip(dvs_binop_t op) {
    switch (op) {
    case DVS_BIN_LT:  return DVS_BIN_GT;
    case DVS_BIN_LTE: return DVS_BIN_GTE;
    case DVS_BIN_GT:  return DVS_BIN_LT;
    case DVS_BIN_GTE: return DVS_BIN_LTE;
    default:          return op;
    }
}

/* Whether a and b are the same expression, node for node (front ends build
 * a fresh node for every mention of a variable). */
static int _mrw_same(SvE *E, dvs_expr_t a, dvs_expr_t b, int depth) {
    if (a == b) return 1;
    if (a == EXPR_NULL || b == EXPR_NULL || depth > 16 || _kind(E, a) != _kind(E, b))
        return 0;
    switch (_kind(E, a)) {
    case EXPR_VAR:
        return ((ExprVar *)_P(E, a))->var_id == ((ExprVar *)_P(E, b))->var_id;
    case EXPR_CONST: {
        ExprConst x = *(ExprConst *)_P(E, a), y = *(ExprConst *)_P(E, b);
        return x.value == y.value && x.width == y.width && x.is_signed == y.is_signed;
    }
    case EXPR_BINARY: {
        ExprBinary x = *(ExprBinary *)_P(E, a), y = *(ExprBinary *)_P(E, b);
        return x.op == y.op && _mrw_same(E, x.lhs, y.lhs, depth + 1)
            && _mrw_same(E, x.rhs, y.rhs, depth + 1);
    }
    case EXPR_UNARY: {
        ExprUnary x = *(ExprUnary *)_P(E, a), y = *(ExprUnary *)_P(E, b);
        return x.op == y.op && _mrw_same(E, x.operand, y.operand, depth + 1);
    }
    case EXPR_EXTEND: {
        ExprExtend x = *(ExprExtend *)_P(E, a), y = *(ExprExtend *)_P(E, b);
        return x.sign_extend == y.sign_extend && x.from_bits == y.from_bits
            && x.to_bits == y.to_bits && _mrw_same(E, x.operand, y.operand, depth + 1);
    }
    case EXPR_SV_CAST: {
        ExprSvCast x = *(ExprSvCast *)_P(E, a), y = *(ExprSvCast *)_P(E, b);
        return x.sign_extend == y.sign_extend && x.dst_signed == y.dst_signed
            && x.from_bits == y.from_bits && x.to_bits == y.to_bits
            && _mrw_same(E, x.operand, y.operand, depth + 1);
    }
    case EXPR_EXTRACT: {
        ExprExtract x = *(ExprExtract *)_P(E, a), y = *(ExprExtract *)_P(E, b);
        return x.hi_bit == y.hi_bit && x.lo_bit == y.lo_bit
            && _mrw_same(E, x.operand, y.operand, depth + 1);
    }
    default:
        return 0;
    }
}

static dvs_expr_t _mrw_bool(SvE *E, dvs_expr_t ref);
static dvs_expr_t _mrw_neg(SvE *E, dvs_expr_t ref, int depth);

/* ite(c, a, b) op e with e the same expression as a branch: that branch
 * compares a value with itself, so `ite(c, a, b) < b` is `c && a < b` and
 * `ite(c, a, b) <= b` is `!c || a <= b`. The engine could not see it: with c
 * open, r = ite(..) takes the hull of a and b, `r < b` raises b above r's
 * low end, the hull follows, and the pair climbs one value a round to the
 * propagation cap (B103). */
static dvs_expr_t _mrw_ite_self(SvE *E, dvs_binop_t op, dvs_expr_t a, dvs_expr_t e) {
    if (a == EXPR_NULL || e == EXPR_NULL) return EXPR_NULL;
    /* The same extension on both sides -- the elaborator sizes `ite(..) < y`
     * that way when the comparison is wider -- is the comparison of what it
     * extends, in the same signedness: an extension preserves the order. */
    for (int peel = 0; peel < 2; peel++) {
        ExprKind k = _kind(E, a);
        if (k != _kind(E, e)) break;
        if (k == EXPR_EXTEND) {
            ExprExtend x = *(ExprExtend *)_P(E, a), y = *(ExprExtend *)_P(E, e);
            if (x.sign_extend != y.sign_extend || x.from_bits != y.from_bits
                    || x.to_bits != y.to_bits || x.to_bits < x.from_bits) break;
            SvTy ix = _type(E, x.operand), iy = _type(E, y.operand), ox = _type(E, a);
            /* A zero extension keeps an unsigned order, a sign extension a
             * signed one; anything else would change how the comparison
             * reads its operands. */
            if (E->err || ix.w != x.from_bits || iy.w != y.from_bits || ix.s != iy.s
                    || ox.s != ix.s || x.sign_extend != ix.s) break;
            a = x.operand; e = y.operand;
        } else if (k == EXPR_SV_CAST) {
            ExprSvCast x = *(ExprSvCast *)_P(E, a), y = *(ExprSvCast *)_P(E, e);
            if (x.sign_extend != y.sign_extend || x.dst_signed != y.dst_signed
                    || x.from_bits != y.from_bits || x.to_bits != y.to_bits
                    || x.to_bits < x.from_bits) break;
            SvTy ix = _type(E, x.operand), iy = _type(E, y.operand);
            if (E->err || ix.w != x.from_bits || iy.w != y.from_bits || ix.s != iy.s
                    || x.dst_signed != ix.s || x.sign_extend != ix.s) break;
            a = x.operand; e = y.operand;
        } else {
            break;
        }
    }
    if (_kind(E, a) != EXPR_ITE) return EXPR_NULL;
    ExprITE i = *(ExprITE *)_P(E, a);
    SvTy ta = _type(E, a), tt = _type(E, i.then_e), tf = _type(E, i.else_e), te = _type(E, e);
    if (E->err || ta.w != tt.w || ta.w != tf.w || ta.w != te.w
            || ta.s != tt.s || ta.s != tf.s || ta.s != te.s) return EXPR_NULL;
    int then_self = _mrw_same(E, i.then_e, e, 0);
    if (!then_self && !_mrw_same(E, i.else_e, e, 0)) return EXPR_NULL;
    int refl = op == DVS_BIN_EQ || op == DVS_BIN_LTE || op == DVS_BIN_GTE;
    /* The branch that is not e decides when it is taken. */
    dvs_expr_t c = i.cond;
    dvs_expr_t nc = _mrw_neg(E, c, 0);
    if (nc == EXPR_NULL) nc = _mk_un(E, DVS_UN_NOT, c);
    dvs_expr_t taken = then_self ? nc : c;        /* the other branch is taken */
    dvs_expr_t other = then_self ? i.else_e : i.then_e;
    dvs_expr_t cmp = _mk_bin(E, op, other, e);
    if (E->err) return EXPR_NULL;
    /* Irreflexive: the self branch is false, so the other must be taken and
     * hold. Reflexive: the self branch is true, so only the other is checked. */
    dvs_expr_t r = refl ? _mk_bin(E, DVS_BIN_OR, then_self ? c : nc, cmp)
                        : _mk_bin(E, DVS_BIN_AND, taken, cmp);
    return E->err ? EXPR_NULL : _mrw_bool(E, r);
}

/* `x & M` as a value, M one run of ones [h:l] within x's bits: the same
 * value as x's bits [h:l] in place, zero-extended -- x[h:l] for l == 0, else
 * {x[h:l], l'b0}. The bitwise-and propagator only bounds its result, so a
 * pinned `t == x & 127` said nothing about x and the search found x by
 * enumeration (pyvsc F5: abs(s - (b & 0x7F)) == 3 drew b ~2/256 per try).
 * As a slice it propagates both ways, and the field split (_fd_split) makes
 * x's bits under and outside the mask separate decisions. A mask that keeps
 * every bit of x, or several runs, is left as it is. */
static dvs_expr_t _mrw_runmask(SvE *E, dvs_expr_t a) {
    if (a == EXPR_NULL || _kind(E, a) != EXPR_BINARY) return EXPR_NULL;
    SvTy t = _type(E, a);
    if (E->err || t.s || t.w > 64) return EXPR_NULL;
    dvs_expr_t x; uint64_t m;
    if (!_mrw_masked(E, a, t.w, 0, &x, &m)) return EXPR_NULL;
    uint16_t lo[MRW_MAX_RUNS], hi[MRW_MAX_RUNS];
    if (_mrw_runs(m, t.w, lo, hi) != 1) return EXPR_NULL;
    MrwSrc s;
    if (!_mrw_src(E, x, t.w, &s, 0) || lo[0] >= s.avail) return EXPR_NULL;
    uint16_t h = hi[0] < s.avail ? hi[0] : (uint16_t)(s.avail - 1);
    if (lo[0] == 0 && h == s.avail - 1) return EXPR_NULL;   /* keeps every bit */
    dvs_expr_t f = _mrw_field(E, &s, h, lo[0]);
    uint16_t fw = (uint16_t)(h + 1);                        /* bits [h:0] */
    if (lo[0] > 0)
        f = _mk_concat(E, f, _mk_const(E, 0, 0, lo[0]), lo[0]);
    if (fw < t.w) f = _mk_ext(E, f, fw, t.w, 0);
    return E->err ? EXPR_NULL : f;
}

static dvs_expr_t _mrw_cmp(SvE *E, dvs_expr_t ref) {
    ExprBinary b = *(ExprBinary *)_P(E, ref);
    SvTy lt = _type(E, b.lhs), rt = _type(E, b.rhs);
    if (E->err) return EXPR_NULL;
    dvs_expr_t r;
    if ((r = _mrw_ite_self(E, b.op, b.lhs, b.rhs)) != EXPR_NULL) return r;
    if ((r = _mrw_ite_self(E, _mrw_flip(b.op), b.rhs, b.lhs)) != EXPR_NULL) return r;
    uint16_t W = lt.w > rt.w ? lt.w : rt.w;
    if (W > 64) return EXPR_NULL;
    if (b.op == DVS_BIN_EQ || b.op == DVS_BIN_NEQ) {
        if ((r = _mrw_eq_const(E, b.op, b.lhs, b.rhs, W)) != EXPR_NULL) return r;
        if ((r = _mrw_eq_const(E, b.op, b.rhs, b.lhs, W)) != EXPR_NULL) return r;
        if ((r = _mrw_align(E, b.op, b.lhs, b.rhs, W)) != EXPR_NULL) return r;
        if ((r = _mrw_align(E, b.op, b.rhs, b.lhs, W)) != EXPR_NULL) return r;
        if ((r = _mrw_eq_masked(E, b.op, b.lhs, b.rhs, W)) != EXPR_NULL) return r;
    } else {
        if ((r = _mrw_order(E, b.op, b.lhs, b.rhs, W)) != EXPR_NULL) return r;
        if ((r = _mrw_order(E, _mrw_flip(b.op), b.rhs, b.lhs, W)) != EXPR_NULL) return r;
    }
    /* Neither side compared with a constant: replace a masked operand by its
     * bits, an equal value. */
    dvs_expr_t l = _mrw_runmask(E, b.lhs), rr = _mrw_runmask(E, b.rhs);
    if (E->err || (l == EXPR_NULL && rr == EXPR_NULL)) return EXPR_NULL;
    return _mk_bin(E, b.op, l != EXPR_NULL ? l : b.lhs, rr != EXPR_NULL ? rr : b.rhs);
}

/* Whether ref is Boolean-valued: a comparison, or && || ! over them. */
static int _mrw_is_bool(SvE *E, dvs_expr_t ref) {
    if (ref == EXPR_NULL) return 0;
    if (_kind(E, ref) == EXPR_BINARY) {
        dvs_binop_t op = ((ExprBinary *)_P(E, ref))->op;
        return op == DVS_BIN_AND || op == DVS_BIN_OR || _is_cmp(op);
    }
    if (_kind(E, ref) == EXPR_UNARY)
        return ((ExprUnary *)_P(E, ref))->op == DVS_UN_NOT;
    return 0;
}

/* !ref with the negation pushed to the comparisons (De Morgan, flipped
 * operators), or EXPR_NULL when ref is not Boolean structure all the way
 * down. The compiler negates a var-const comparison under `!(a || b)` but
 * nothing else there, so `!(x[31:20] inside {..})` did not compile; nor did
 * `!(a && b)`. Pushed down, both are ordinary conjunctions and disjunctions
 * of comparisons. */
static dvs_expr_t _mrw_neg(SvE *E, dvs_expr_t ref, int depth) {
    if (depth > 64 || !_mrw_is_bool(E, ref)) return EXPR_NULL;
    if (_kind(E, ref) == EXPR_UNARY) {
        dvs_expr_t x = ((ExprUnary *)_P(E, ref))->operand;
        return _mrw_is_bool(E, x) ? x : EXPR_NULL;
    }
    ExprBinary b = *(ExprBinary *)_P(E, ref);
    dvs_binop_t op;
    switch (b.op) {
    case DVS_BIN_AND: op = DVS_BIN_OR;  break;
    case DVS_BIN_OR:  op = DVS_BIN_AND; break;
    case DVS_BIN_EQ:  return _mk_bin(E, DVS_BIN_NEQ, b.lhs, b.rhs);
    case DVS_BIN_NEQ: return _mk_bin(E, DVS_BIN_EQ,  b.lhs, b.rhs);
    case DVS_BIN_LT:  return _mk_bin(E, DVS_BIN_GTE, b.lhs, b.rhs);
    case DVS_BIN_LTE: return _mk_bin(E, DVS_BIN_GT,  b.lhs, b.rhs);
    case DVS_BIN_GT:  return _mk_bin(E, DVS_BIN_LTE, b.lhs, b.rhs);
    case DVS_BIN_GTE: return _mk_bin(E, DVS_BIN_LT,  b.lhs, b.rhs);
    default:          return EXPR_NULL;
    }
    dvs_expr_t l = _mrw_neg(E, b.lhs, depth + 1);
    if (l == EXPR_NULL) l = _mk_un(E, DVS_UN_NOT, b.lhs);
    dvs_expr_t r = _mrw_neg(E, b.rhs, depth + 1);
    if (r == EXPR_NULL) r = _mk_un(E, DVS_UN_NOT, b.rhs);
    return _mk_bin(E, op, l, r);
}

/* The Boolean structure of a root: && || ! over comparisons. */
static dvs_expr_t _mrw_bool(SvE *E, dvs_expr_t ref) {
    if (ref == EXPR_NULL || E->err) return ref;
    if (dvs_stack_deep(&E->sg, ++E->depth, SV_MAX_DEPTH)) {
        E->err = 1; E->depth--; return ref;
    }
    dvs_expr_t out = ref;
    switch (_kind(E, ref)) {
    case EXPR_BINARY: {
        ExprBinary b = *(ExprBinary *)_P(E, ref);
        if (b.op == DVS_BIN_AND || b.op == DVS_BIN_OR) {
            dvs_expr_t l = _mrw_bool(E, b.lhs), r = _mrw_bool(E, b.rhs);
            if (!E->err && (l != b.lhs || r != b.rhs)) out = _mk_bin(E, b.op, l, r);
        } else if (_is_cmp(b.op)) {
            dvs_expr_t r = _mrw_cmp(E, ref);
            if (!E->err && r != EXPR_NULL) out = r;
        }
        break;
    }
    case EXPR_UNARY: {
        ExprUnary u = *(ExprUnary *)_P(E, ref);
        if (u.op == DVS_UN_NOT) {
            dvs_expr_t n = _mrw_neg(E, u.operand, 0);
            if (n != EXPR_NULL && !E->err) {
                out = _mrw_bool(E, n);
            } else {
                dvs_expr_t x = _mrw_bool(E, u.operand);
                if (!E->err && x != u.operand) out = _mk_un(E, DVS_UN_NOT, x);
            }
        }
        break;
    }
    default:
        break;
    }
    E->depth--;
    return E->err ? ref : out;
}

/* ------------------------------------------------------------------ */
/* Field decomposition                                                 */
/* ------------------------------------------------------------------ */
/*
 * A packed word whose fields are constrained one by one -- a register offset
 * from a set in x[11:2], a locality below 5 in x[15:12] -- leaves the engine
 * a 24-bit variable to draw while each field reaches it only through a
 * bit-slice propagator: ~2 ms a solve and a skewed spread. Split at the
 * extracts' boundaries into hidden field variables, x == {f_k, ..., f_0},
 * the fields become the decisions (they parametrise x one to one, so a
 * uniform draw over them is uniform over x) and each extract that is exactly
 * one field becomes that field. See docs/bitfield_linearization_research.md
 * sections 9-10.
 *
 * Only a word that needs it is split: one with an extract anywhere other than
 * a top-level `x[h:l] == const` (which propagates exactly as it is). Extracts
 * may nest (x[11:0] around x[1:0]: the span becomes a hidden word of fields)
 * but not cross. A 64-bit word is tied to its parts by bit slices, since the
 * concat propagator reasons in int64. Section 11 of the same document.
 */

#define FD_MAX_SEGS 16
#define FD_MAX_WORDS 64
#ifndef FD_MIN_WIDTH
#define FD_MIN_WIDTH 0
#endif

typedef struct {
    uint64_t cuts;              /* bit i set: a field starts at bit i */
    uint8_t  need;
    uint8_t  skip;              /* not splittable (cut overflow, bad extract) */
    uint16_t n_iv;              /* distinct extracts */
} FdWord;

typedef struct {
    dvs_expr_t node;
    uint32_t   var;
    uint16_t   hi, lo;
} FdUse;

typedef struct {
    FdWord  *w;
    FdUse   *use;
    uint32_t n_use, cap_use;
} FdState;

/* The extract node `r` of a declared, unsigned, at most 63-bit variable:
 * record its cuts. */
static void _fd_extract(SvE *E, FdState *S, dvs_expr_t r, int need) {
    ExprExtract ex = *(ExprExtract *)_P(E, r);
    if (ex.operand == EXPR_NULL || _kind(E, ex.operand) != EXPR_VAR) return;
    uint32_t v = ((ExprVar *)_P(E, ex.operand))->var_id;
    if (v >= E->nv || !E->vk[v] || E->vs[v] || E->vw[v] > 64) return;
    FdWord *w = &S->w[v];
    if (ex.hi_bit < ex.lo_bit || ex.hi_bit >= E->vw[v]) { w->skip = 1; return; }
    if (need) w->need = 1;
    w->cuts |= 1ULL << ex.lo_bit;
    if (ex.hi_bit + 1 < E->vw[v]) w->cuts |= 1ULL << (ex.hi_bit + 1);
    if (S->n_use == S->cap_use) {
        uint32_t nc = S->cap_use ? S->cap_use * 2 : 64;
        FdUse *nu = (FdUse *)realloc(S->use, nc * sizeof(FdUse));
        if (!nu) { E->err = E->oom = 1; return; }
        S->use = nu; S->cap_use = nc;
    }
    S->use[S->n_use++] = (FdUse){ r, v, ex.hi_bit, ex.lo_bit };
}

/* Collect the extracts under `r`. Kinds it does not walk keep theirs: an
 * extract left over x is still exact, only not split.
 *
 * `term`: r is an operand of arithmetic (+ - * / %), through extensions. An
 * extract there records its cuts but does not by itself ask for a split: bits
 * summed into a count (`$countones` written out, sum of `int'(x[i])`) gain
 * nothing from deciding each bit on its own, and a 4-bit mask split into
 * four fields cost OpenTitan hmac_base 80 -> 134 us a solve (svcorpus). */
static void _fd_walk(SvE *E, FdState *S, dvs_expr_t r, int depth, int term) {
    if (r == EXPR_NULL || E->err) return;
    if (dvs_stack_deep(&E->sg, depth, SV_MAX_DEPTH)) { E->err = 1; return; }
    uint32_t seen;
    uint64_t k = _key(r, (uint32_t)term, 0, 0);
    if (_memo_get(E, k, &seen)) return;
    _memo_put(E, k, 1);
    switch (_kind(E, r)) {
    case EXPR_BINARY: {
        ExprBinary b = *(ExprBinary *)_P(E, r);
        int t = b.op == DVS_BIN_ADD || b.op == DVS_BIN_SUB || b.op == DVS_BIN_MUL
             || b.op == DVS_BIN_DIV || b.op == DVS_BIN_MOD;
        _fd_walk(E, S, b.lhs, depth + 1, t);
        _fd_walk(E, S, b.rhs, depth + 1, t);
        break;
    }
    case EXPR_UNARY:
        _fd_walk(E, S, ((ExprUnary *)_P(E, r))->operand, depth + 1, 0);
        break;
    case EXPR_ITE: {
        ExprITE t = *(ExprITE *)_P(E, r);
        _fd_walk(E, S, t.cond, depth + 1, 0);
        _fd_walk(E, S, t.then_e, depth + 1, 0);
        _fd_walk(E, S, t.else_e, depth + 1, 0);
        break;
    }
    case EXPR_IN_RANGE: {
        ExprInRange t = *(ExprInRange *)_P(E, r);
        _fd_walk(E, S, t.value, depth + 1, 0);
        _fd_walk(E, S, t.lo, depth + 1, 0);
        _fd_walk(E, S, t.hi, depth + 1, 0);
        break;
    }
    case EXPR_IN_SET: {
        ExprInSet *t = (ExprInSet *)_P(E, r);
        uint32_t n = t->n_elems;
        dvs_expr_t v = t->value;
        _fd_walk(E, S, v, depth + 1, 0);
        for (uint32_t i = 0; i < n && !E->err; i++)
            _fd_walk(E, S, ((dvs_expr_t *)((ExprInSet *)_P(E, r) + 1))[i], depth + 1, 0);
        break;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *t = (ExprInRanges *)_P(E, r);
        uint32_t n = t->n_ranges;
        _fd_walk(E, S, t->value, depth + 1, 0);
        for (uint32_t i = 0; i < 2 * n && !E->err; i++)
            _fd_walk(E, S, ((dvs_expr_t *)((ExprInRanges *)_P(E, r) + 1))[i], depth + 1, 0);
        break;
    }
    case EXPR_EXTEND: case EXPR_SV_CAST:
        _fd_walk(E, S, ((ExprExtend *)_P(E, r))->operand, depth + 1, term);
        break;
    case EXPR_CONCAT: {
        ExprConcat c = *(ExprConcat *)_P(E, r);
        _fd_walk(E, S, c.hi, depth + 1, 0);
        _fd_walk(E, S, c.lo, depth + 1, 0);
        break;
    }
    case EXPR_EXTRACT:
        _fd_extract(E, S, r, !term);
        _fd_walk(E, S, ((ExprExtract *)_P(E, r))->operand, depth + 1, 0);
        break;
    default:
        break;
    }
}

/* A top-level conjunct. `x[h:l] == const` records its cuts without asking
 * for a split; anything else is walked. */
static void _fd_root(SvE *E, FdState *S, dvs_expr_t r, int depth) {
    if (r == EXPR_NULL || E->err) return;
    if (dvs_stack_deep(&E->sg, depth, SV_MAX_DEPTH)) { E->err = 1; return; }
    if (_kind(E, r) == EXPR_BINARY) {
        ExprBinary b = *(ExprBinary *)_P(E, r);
        if (b.op == DVS_BIN_AND) {
            _fd_root(E, S, b.lhs, depth + 1);
            _fd_root(E, S, b.rhs, depth + 1);
            return;
        }
        if (b.op == DVS_BIN_EQ && b.lhs != EXPR_NULL && b.rhs != EXPR_NULL) {
            dvs_expr_t x = EXPR_NULL;
            if (_kind(E, b.lhs) == EXPR_EXTRACT && _kind(E, b.rhs) == EXPR_CONST) x = b.lhs;
            if (_kind(E, b.rhs) == EXPR_EXTRACT && _kind(E, b.lhs) == EXPR_CONST) x = b.rhs;
            if (x != EXPR_NULL) { _fd_extract(E, S, x, 0); return; }
        }
    }
    _fd_walk(E, S, r, depth + 1, 0);
}

static dvs_expr_t _fd_add_var(SvE *E, uint32_t id, uint16_t w, uint8_t aux) {
    dvs_expr_t r = _alloc(E, sizeof(VarSpec), _Alignof(VarSpec));
    if (r == EXPR_NULL) return r;
    VarSpec *v = (VarSpec *)_P(E, r);
    memset(v, 0, sizeof(*v));
    v->next = E->dst->vars_head;
    v->var_id = id;
    v->width = w;
    v->is_aux = aux;
    v->lo = 0;
    v->hi = (int64_t)((1ULL << w) - 1);
    E->dst->vars_head = r;
    E->dst->n_vars++;
    dvs_expr_t e = _alloc(E, sizeof(ExprVar), _Alignof(ExprVar));
    if (e == EXPR_NULL) return e;
    ExprVar *ev = (ExprVar *)_P(E, e);
    ev->kind = EXPR_VAR; ev->var_id = id;
    return e;
}

static void _fd_add_constraint(SvE *E, dvs_expr_t root) {
    dvs_expr_t r = _alloc(E, sizeof(ConstraintSpec), _Alignof(ConstraintSpec));
    if (r == EXPR_NULL || root == EXPR_NULL) { E->err = 1; return; }
    ConstraintSpec *c = (ConstraintSpec *)_P(E, r);
    c->next = E->dst->constraints_head;
    c->root = root;
    c->constraint_id = 0;
    E->dst->constraints_head = r;
    E->dst->n_constraints++;
}

/* One word's split: its fields (the segments between cuts) and the hidden
 * words built for the extracts that span several of them. */
typedef struct {
    FdState  *S;
    uint32_t  v, *next_id;
    uint16_t  W;
    uint64_t  cuts;
    int       n;                                   /* fields */
    uint16_t  seg_lo[FD_MAX_SEGS], seg_w[FD_MAX_SEGS];
    uint32_t  seg_id[FD_MAX_SEGS];
    dvs_expr_t seg_e[FD_MAX_SEGS];
    int       nw;                                  /* words over several */
    uint16_t  w_lo[FD_MAX_WORDS], w_hi[FD_MAX_WORDS];
    uint32_t  w_id[FD_MAX_WORDS];
} FdBuild;

static int _fd_lookup(FdBuild *B, uint16_t lo, uint16_t hi, uint32_t *id) {
    for (int i = 0; i < B->n; i++)
        if (B->seg_lo[i] == lo && B->seg_lo[i] + B->seg_w[i] - 1 == hi) {
            *id = B->seg_id[i]; return 1;
        }
    for (int i = 0; i < B->nw; i++)
        if (B->w_lo[i] == lo && B->w_hi[i] == hi) { *id = B->w_id[i]; return 1; }
    return 0;
}

/* The variable of bits [hi:lo], a field or an extract's bits: a field is
 * already made; a span of several is `self` (x itself at the top, else a
 * new hidden word) tied to its parts, the largest extracts inside it and the
 * fields between them, by a chain of one-level concats. */
static dvs_expr_t _fd_build(SvE *E, FdBuild *B, uint16_t lo, uint16_t hi,
                            dvs_expr_t self, uint32_t self_id, int depth) {
    if (E->err) return EXPR_NULL;
    if (depth > FD_MAX_SEGS + 1) { E->err = 1; return EXPR_NULL; }
    for (int i = 0; i < B->n; i++)
        if (B->seg_lo[i] == lo && B->seg_lo[i] + B->seg_w[i] - 1 == hi)
            return B->seg_e[i];
    if (self == EXPR_NULL) {
        if (B->nw == FD_MAX_WORDS) { E->err = 1; return EXPR_NULL; }
        self_id = (*B->next_id)++;
        self = _fd_add_var(E, self_id, (uint16_t)(hi - lo + 1), 1);
        if (self == EXPR_NULL) return EXPR_NULL;
        B->w_lo[B->nw] = lo; B->w_hi[B->nw] = hi; B->w_id[B->nw] = self_id;
        B->nw++;
    } else if (B->nw < FD_MAX_WORDS) {
        B->w_lo[B->nw] = lo; B->w_hi[B->nw] = hi; B->w_id[B->nw] = self_id;
        B->nw++;
    }
    dvs_expr_t cur = EXPR_NULL;
    uint16_t cur_w = 0;
    for (uint16_t b = lo; b <= hi && !E->err; ) {
        /* The largest extract starting at b inside [hi:lo], else the field
         * at b. */
        uint16_t e = b;
        while (e < hi && !((B->cuts >> (e + 1)) & 1)) e++;
        for (uint32_t u = 0; u < B->S->n_use; u++) {
            FdUse *x = &B->S->use[u];
            if (x->var != B->v || x->lo != b || x->hi > hi || x->hi <= e) continue;
            if (x->lo == lo && x->hi == hi) continue;
            e = x->hi;
        }
        dvs_expr_t part = _fd_build(E, B, b, e, EXPR_NULL, 0, depth + 1);
        uint16_t pw = (uint16_t)(e - b + 1);
        if (hi - lo + 1 >= 64) {
            /* A 64-bit word: the concat propagator reasons in int64, so each
             * part is tied to its bits by a slice instead. */
            _fd_add_constraint(E, _mk_bin(E, DVS_BIN_EQ, part, _mk_extract(E, self, e, b)));
        } else if (cur == EXPR_NULL) {
            cur = part; cur_w = pw;
        } else {
            int last = e == hi;
            dvs_expr_t t = last ? self
                : _fd_add_var(E, (*B->next_id)++, (uint16_t)(cur_w + pw), 1);
            _fd_add_constraint(E, _mk_bin(E, DVS_BIN_EQ, t, _mk_concat(E, part, cur, cur_w)));
            cur = t; cur_w = (uint16_t)(cur_w + pw);
        }
        b = (uint16_t)(e + 1);
        if (e == hi) break;
    }
    return self;
}

/* Split the words that need it. On error the problem may be half split, so
 * the caller discards it. Nothing is copied unless a word is split. */
static void _fd_split(SvE *E) {
    FdState S;
    memset(&S, 0, sizeof(S));
    dvs_problem_t *p = E->dst ? E->dst : E->src;
    if (E->nv == 0 || p->n_vars != E->nv) return;   /* ids must be 0..n-1 */
    S.w = (FdWord *)calloc(E->nv, sizeof(FdWord));
    if (!S.w) { E->err = E->oom = 1; return; }
    for (dvs_expr_t c = p->constraints_head; c != EXPR_NULL && !E->err; ) {
        ConstraintSpec cs = *(ConstraintSpec *)_P(E, c);
        _fd_root(E, &S, cs.root, 0);
        c = cs.next;
    }
    for (dvs_expr_t c = p->softs_head; c != EXPR_NULL && !E->err; ) {
        SoftSpec ss = *(SoftSpec *)_P(E, c);
        _fd_walk(E, &S, ss.root, 0, 0);
        c = ss.next;
    }
    /* A distribution or an all-different names the word itself; leave it. */
    for (dvs_expr_t c = p->dists_head; c != EXPR_NULL; ) {
        DistSpec *d = (DistSpec *)_P(E, c);
        if (d->var_id < E->nv) S.w[d->var_id].skip = 1;
        c = d->next;
    }
    if (E->err) {                       /* nothing written yet: split none */
        if (!E->oom) E->err = 0;
        free(S.w); free(S.use);
        return;
    }
    /* The extracts of a word must nest or be disjoint: x[11:0] around x[1:0]
     * becomes a hidden word of fields, but crossing ones (x[15:13] and
     * x[19:15]) would leave a slice across fields, which the search can only
     * check after deciding them -- 40x slower than no split on riscv-dv's
     * illegal-instruction shape. */
    for (uint32_t u = 0; u < S.n_use; u++) {
        FdUse *a = &S.use[u];
        FdWord *w = &S.w[a->var];
        for (uint32_t t = u + 1; t < S.n_use && !w->skip; t++) {
            FdUse *c = &S.use[t];
            if (c->var != a->var) continue;
            int disjoint = c->lo > a->hi || a->lo > c->hi;
            int nested = (c->lo >= a->lo && c->hi <= a->hi)
                || (a->lo >= c->lo && a->hi <= c->hi);
            if (!disjoint && !nested) w->skip = 1;
        }
        /* Count distinct extracts, the hidden words' bound. */
        int first = 1;
        for (uint32_t t = 0; t < u && first; t++)
            if (S.use[t].var == a->var && S.use[t].lo == a->lo && S.use[t].hi == a->hi)
                first = 0;
        if (first && ++w->n_iv >= FD_MAX_WORDS) w->skip = 1;
    }
    int any = 0;
    for (uint32_t v = 0; v < E->nv; v++) {
        FdWord *w = &S.w[v];
        uint64_t cuts = w->cuts & ~1ULL;
        if (!w->need || w->skip || cuts == 0 || E->vw[v] < FD_MIN_WIDTH
                || __builtin_popcountll(cuts) + 1 > FD_MAX_SEGS) w->skip = 1;
        else any = 1;
    }
    if (any && !E->err && !E->dst) (void)_alloc(E, 0, 1);   /* split in a copy */
    uint32_t next_id = E->nv;
    for (uint32_t v = 0; v < E->nv && any && !E->err; v++) {
        FdWord *w = &S.w[v];
        uint64_t cuts = w->cuts & ~1ULL;
        if (w->skip) continue;
        uint16_t W = E->vw[v];
        FdBuild B;
        memset(&B, 0, sizeof(B));
        B.S = &S; B.v = v; B.W = W; B.cuts = cuts; B.next_id = &next_id;
        /* The fields first, lowest bits first: they are the decisions. */
        for (uint16_t b = 0; b < W; ) {
            uint16_t e = b + 1;
            while (e < W && !((cuts >> e) & 1)) e++;
            B.seg_lo[B.n] = b; B.seg_w[B.n] = (uint16_t)(e - b);
            B.seg_id[B.n] = next_id++;
            B.seg_e[B.n] = _fd_add_var(E, B.seg_id[B.n], B.seg_w[B.n], 0);
            B.n++;
            b = e;
        }
        if (E->err) break;
        dvs_expr_t xv = _alloc(E, sizeof(ExprVar), _Alignof(ExprVar));
        if (xv == EXPR_NULL) break;
        ((ExprVar *)_P(E, xv))->kind = EXPR_VAR;
        ((ExprVar *)_P(E, xv))->var_id = v;
        (void)_fd_build(E, &B, 0, (uint16_t)(W - 1), xv, v, 0);
        /* The fields are the decisions and fix x; x drawn first would be a
         * guess the fields then reject (a 64-bit word whose low field is
         * `inside {0, max}` ties with x once its top field is drawn, and
         * timed out half the time). */
        for (dvs_expr_t c = E->dst->vars_head; c != EXPR_NULL; ) {
            VarSpec *vs = (VarSpec *)_P(E, c);
            if (vs->var_id == v) { vs->is_aux = 1; break; }
            c = vs->next;
        }
        /* Each extract becomes the variable of its bits. ExprVar fits in the
         * ExprExtract node, and every reference to the node means the same
         * bits. */
        for (uint32_t u = 0; u < S.n_use && !E->err; u++) {
            if (S.use[u].var != v) continue;
            uint32_t id;
            if (!_fd_lookup(&B, S.use[u].lo, S.use[u].hi, &id)) { E->err = 1; break; }
            ExprVar *ev = (ExprVar *)_P(E, S.use[u].node);
            ev->kind = EXPR_VAR;
            ev->var_id = id;
        }
    }
    free(S.w);
    free(S.use);
}

dvs_problem_t *dvs_sv_rewrite_masks(dvs_problem_t *sp, dvs_sv_var_type_fn fn, void *ud) {
#ifdef DVS_NO_MASK_REWRITE
    (void)fn; (void)ud;
    return sp;
#else
    if (!sp || (sp->constraints_head == EXPR_NULL && sp->softs_head == EXPR_NULL))
        return sp;
    SvE E;
    memset(&E, 0, sizeof(E));
    E.src = sp;
    E.vfn = fn;
    E.vud = ud;
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
        if (!E.vw || !E.vs || !E.vk) E.err = 1;
        for (dvs_expr_t c = sp->vars_head; c != EXPR_NULL && !E.err; ) {
            VarSpec *v = (VarSpec *)POOL_PTR(sp, c);
            E.vw[v->var_id] = v->width;
            E.vs[v->var_id] = v->is_signed ? 1 : 0;
            E.vk[v->var_id] = 1;
            c = v->next;
        }
    }
    /* A root that fails to rewrite keeps its old form; E.err is cleared so
     * the others still get their turn. */
    for (dvs_expr_t c = sp->constraints_head; c != EXPR_NULL && !E.oom; ) {
        ConstraintSpec cs = *(ConstraintSpec *)_P(&E, c);
        E.err = 0;
        dvs_expr_t nr = _mrw_bool(&E, cs.root);
        if (!E.err && nr != cs.root) ((ConstraintSpec *)_P(&E, c))->root = nr;
        c = cs.next;
    }
    for (dvs_expr_t c = sp->softs_head; c != EXPR_NULL && !E.oom; ) {
        SoftSpec ss = *(SoftSpec *)_P(&E, c);
        E.err = 0;
        dvs_expr_t nr = _mrw_bool(&E, ss.root);
        if (!E.err && nr != ss.root) ((SoftSpec *)_P(&E, c))->root = nr;
        c = ss.next;
    }
    /* Field decomposition adds variables, so only where the whole problem is
     * compiled at once: an incremental add sees the context's variables. */
#ifndef DVS_NO_FIELD_SPLIT
    if (!fn && !E.oom) {
        free(E.memo);
        E.memo = NULL; E.memo_cap = E.memo_n = 0;
        E.err = 0;
        _fd_split(&E);
        if (E.err) E.oom = 1;                  /* half split: keep none of it */
    }
#endif
    free(E.vw); free(E.vs); free(E.vk);
    free(E.memo);
    if (E.oom) { free(E.dst); return sp; }
    return E.dst ? E.dst : sp;
#endif
}
