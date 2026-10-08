/* An independent evaluator for SMT-LIB terms over a model. See smt2_eval.h.
 *
 * Values live in an arena owned by the evaluator. Bit-vectors are little-endian
 * 64-bit limb vectors of any width; bits above the width are always zero.
 * define-fun applications are memoized on (definition, argument values), so a
 * term built from shared definitions -- yosys-smtbmc's unrolled transition
 * functions -- costs its DAG size, not its tree size. */

#include "smt2/smt2_eval.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Widest bit-vector a term may build (a value is width/8 bytes). */
#define EVAL_MAX_WIDTH (1u << 24)
/* Arrays with at most this many indices are compared element by element. */
#define EVAL_ENUM_INDEX_BITS 16

#define NL(w) (((w) + 63u) / 64u)

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct ESort {
    uint8_t             kind;      /* SMT2_VAL_* */
    uint32_t            width;     /* BV */
    const struct ESort *idx;       /* ARRAY */
    const struct ESort *elem;      /* ARRAY */
} ESort;

typedef struct Store {
    const Smt2Value    *key;
    const Smt2Value    *val;
    const struct Store *next;      /* older */
} Store;

typedef struct {
    const ESort     *sort;         /* the array's sort */
    /* Base: a constant array, or a declared array of the model. */
    const Smt2Value *const_elem;
    const void      *model;
    uint32_t         flat_w;       /* model: flat index width */
    uint32_t         inner_w;      /* model: inner index width of a nested array */
    uint32_t         elem_w;       /* model: element width */
    int              total;        /* model: every element has a model value */
    const uint64_t  *prefix;       /* model slice: the fixed outer index, or NULL */
    const Store     *stores;       /* newest first */
} ArrVal;

struct Smt2Value {
    uint8_t  kind;
    uint8_t  arb;                  /* depends on an arbitrary array element */
    uint32_t width;                /* BV */
    union {
        uint64_t        b;         /* BOOL */
        uint64_t       *limbs;     /* BV */
        const ArrVal   *arr;       /* ARRAY */
        struct { const char *s; uint32_t len; } opq;  /* OPAQUE */
    } u;
};

typedef struct Env {
    const char        *name;
    uint32_t           len;
    const Smt2Value   *val;
    const struct Env  *next;
} Env;

/* What a global name resolves to, cached per evaluator: the host's symbol
 * tables are searched linearly. */
typedef struct {
    const char       *name;        /* NULL: empty slot */
    uint32_t          len;
    uint8_t           fun_known;   /* fun / uf resolved */
    uint8_t           val_known;   /* val resolved */
    const Smt2EvalFun *fun;        /* a define-fun, or NULL */
    uint32_t          uf;          /* arity of an uninterpreted function, or 0 */
    const Smt2Value  *val;         /* the symbol's value, or NULL */
} SymEnt;

typedef struct {
    const Sexpr      *body;        /* NULL: empty slot */
    uint32_t          n;
    const Smt2Value **args;
    const Smt2Value  *res;
    uint64_t          hash;
} Memo;

struct Smt2Eval {
    Smt2EvalHost  host;
    SexprArena    arena;
    uint32_t      depth;
    char          err[200];
    int           failed;
    Memo         *memo;
    uint32_t      memo_cap;
    uint32_t      memo_n;
    ESort         bool_sort;
    SymEnt       *syms;
    uint32_t      syms_cap;
    uint32_t      syms_n;
};

/* ------------------------------------------------------------------ */
/* Errors and allocation                                               */
/* ------------------------------------------------------------------ */

static const Smt2Value *_fail(Smt2Eval *ev, const char *fmt, ...) {
    if (!ev->failed) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(ev->err, sizeof ev->err, fmt, ap);
        va_end(ap);
        ev->failed = 1;
    }
    return NULL;
}

static void *_alloc(Smt2Eval *ev, size_t n) {
    void *p = sexpr_arena_alloc(&ev->arena, n ? n : 1, 8);
    if (!p) { _fail(ev, "out of memory"); return NULL; }
    memset(p, 0, n);
    return p;
}

static Smt2Value *_bv_new(Smt2Eval *ev, uint32_t w) {
    if (w == 0 || w > EVAL_MAX_WIDTH) {
        _fail(ev, "bit-vector width %u is out of range", (unsigned)w);
        return NULL;
    }
    Smt2Value *v = (Smt2Value *)_alloc(ev, sizeof *v);
    if (!v) return NULL;
    v->u.limbs = (uint64_t *)_alloc(ev, NL(w) * sizeof(uint64_t));
    if (!v->u.limbs) return NULL;
    v->kind = SMT2_VAL_BV;
    v->width = w;
    return v;
}

static const Smt2Value *_bool(Smt2Eval *ev, int b, int arb) {
    Smt2Value *v = (Smt2Value *)_alloc(ev, sizeof *v);
    if (!v) return NULL;
    v->kind = SMT2_VAL_BOOL;
    v->u.b = b ? 1 : 0;
    v->arb = (uint8_t)(arb != 0);
    return v;
}

static const ESort *_sort_bv(Smt2Eval *ev, uint32_t w) {
    ESort *s = (ESort *)_alloc(ev, sizeof *s);
    if (!s) return NULL;
    s->kind = SMT2_VAL_BV;
    s->width = w;
    return s;
}

static const ESort *_sort_array(Smt2Eval *ev, const ESort *idx, const ESort *elem) {
    if (!idx || !elem) return NULL;
    ESort *s = (ESort *)_alloc(ev, sizeof *s);
    if (!s) return NULL;
    s->kind = SMT2_VAL_ARRAY;
    s->idx = idx;
    s->elem = elem;
    return s;
}

static int _sort_eq(const ESort *a, const ESort *b) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind) return 0;
    if (a->kind == SMT2_VAL_BV) return a->width == b->width;
    if (a->kind == SMT2_VAL_ARRAY) return _sort_eq(a->idx, b->idx) && _sort_eq(a->elem, b->elem);
    return 1;   /* Bool; declare-sort sorts are not told apart */
}

/* Whether value `v` has sort `s`. */
static int _has_sort(const Smt2Value *v, const ESort *s) {
    if (v->kind != s->kind) return 0;
    if (v->kind == SMT2_VAL_BV) return v->width == s->width;
    if (v->kind == SMT2_VAL_ARRAY) return _sort_eq(v->u.arr->sort, s);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Bit-vector arithmetic on limb vectors                               */
/* ------------------------------------------------------------------ */

static void _mask(Smt2Value *v) {
    uint32_t r = v->width % 64u;
    if (r) v->u.limbs[NL(v->width) - 1] &= (~0ull) >> (64u - r);
}

static int _bit(const Smt2Value *v, uint32_t i) {
    return (int)((v->u.limbs[i / 64u] >> (i % 64u)) & 1u);
}

static void _set_bit(Smt2Value *v, uint32_t i, int b) {
    uint64_t m = 1ull << (i % 64u);
    if (b) v->u.limbs[i / 64u] |= m;
    else   v->u.limbs[i / 64u] &= ~m;
}

static int _msb(const Smt2Value *v) { return _bit(v, v->width - 1); }

static int _is_zero(const Smt2Value *v) {
    for (uint32_t i = 0; i < NL(v->width); i++) if (v->u.limbs[i]) return 0;
    return 1;
}

static int _ucmp(const Smt2Value *a, const Smt2Value *b) {
    for (uint32_t i = NL(a->width); i-- > 0;) {
        if (a->u.limbs[i] != b->u.limbs[i]) return a->u.limbs[i] < b->u.limbs[i] ? -1 : 1;
    }
    return 0;
}

static int _scmp(const Smt2Value *a, const Smt2Value *b) {
    int sa = _msb(a), sb = _msb(b);
    if (sa != sb) return sa ? -1 : 1;
    return _ucmp(a, b);
}

static Smt2Value *_copy(Smt2Eval *ev, const Smt2Value *a) {
    Smt2Value *r = _bv_new(ev, a->width);
    if (!r) return NULL;
    memcpy(r->u.limbs, a->u.limbs, NL(a->width) * sizeof(uint64_t));
    r->arb = a->arb;
    return r;
}

static Smt2Value *_add(Smt2Eval *ev, const Smt2Value *a, const Smt2Value *b) {
    Smt2Value *r = _bv_new(ev, a->width);
    if (!r) return NULL;
    uint64_t c = 0;
    for (uint32_t i = 0; i < NL(a->width); i++) {
        uint64_t s = a->u.limbs[i] + c;
        uint64_t c1 = s < c;
        s += b->u.limbs[i];
        c1 += s < b->u.limbs[i];
        r->u.limbs[i] = s;
        c = c1;
    }
    _mask(r);
    r->arb = a->arb | b->arb;
    return r;
}

static Smt2Value *_not(Smt2Eval *ev, const Smt2Value *a) {
    Smt2Value *r = _bv_new(ev, a->width);
    if (!r) return NULL;
    for (uint32_t i = 0; i < NL(a->width); i++) r->u.limbs[i] = ~a->u.limbs[i];
    _mask(r);
    r->arb = a->arb;
    return r;
}

static Smt2Value *_neg(Smt2Eval *ev, const Smt2Value *a) {
    Smt2Value *r = _not(ev, a);
    if (!r) return NULL;
    for (uint32_t i = 0; i < NL(a->width); i++) if (++r->u.limbs[i]) break;
    _mask(r);
    return r;
}

static Smt2Value *_sub(Smt2Eval *ev, const Smt2Value *a, const Smt2Value *b) {
    Smt2Value *nb = _neg(ev, b);
    return nb ? _add(ev, a, nb) : NULL;
}

static void _mul64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    *lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static Smt2Value *_mul(Smt2Eval *ev, const Smt2Value *a, const Smt2Value *b) {
    Smt2Value *r = _bv_new(ev, a->width);
    if (!r) return NULL;
    uint32_t n = NL(a->width);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t carry = 0;
        if (!a->u.limbs[i]) continue;
        for (uint32_t j = 0; i + j < n; j++) {
            uint64_t lo, hi;
            _mul64(a->u.limbs[i], b->u.limbs[j], &lo, &hi);
            uint64_t t = r->u.limbs[i + j] + lo;
            uint64_t c1 = t < lo;
            t += carry;
            c1 += t < carry;
            r->u.limbs[i + j] = t;
            carry = hi + c1;   /* fits: the column sum is below 2^128 */
        }
    }
    _mask(r);
    r->arb = a->arb | b->arb;
    return r;
}

/* Unsigned division with SMT-LIB's total semantics: x / 0 is all ones and
 * x % 0 is x. */
static int _udivrem(Smt2Eval *ev, const Smt2Value *a, const Smt2Value *b,
                    Smt2Value **q, Smt2Value **rem) {
    uint32_t w = a->width;
    Smt2Value *qq = _bv_new(ev, w), *r = _bv_new(ev, w);
    if (!qq || !r) return 0;
    if (_is_zero(b)) {
        for (uint32_t i = 0; i < NL(w); i++) qq->u.limbs[i] = ~0ull;
        _mask(qq);
        memcpy(r->u.limbs, a->u.limbs, NL(w) * sizeof(uint64_t));
    } else {
        uint32_t n = NL(w);
        for (uint32_t i = w; i-- > 0;) {
            /* r = (r << 1) | a[i], kept in w+1 bits via the shifted-out bit. */
            uint64_t out = r->u.limbs[n - 1] >> 63;
            if (w % 64u) out = (r->u.limbs[n - 1] >> ((w % 64u) - 1)) & 1u;
            for (uint32_t k = n; k-- > 0;) {
                r->u.limbs[k] <<= 1;
                if (k) r->u.limbs[k] |= r->u.limbs[k - 1] >> 63;
            }
            r->u.limbs[0] |= (uint64_t)_bit(a, i);
            _mask(r);
            if (out || _ucmp(r, b) >= 0) {
                uint64_t br = 0;
                for (uint32_t k = 0; k < n; k++) {
                    uint64_t d = r->u.limbs[k] - b->u.limbs[k];
                    uint64_t b1 = r->u.limbs[k] < b->u.limbs[k];
                    uint64_t d2 = d - br;
                    b1 |= d < br;
                    r->u.limbs[k] = d2;
                    br = b1;
                }
                _mask(r);
                _set_bit(qq, i, 1);
            }
        }
    }
    qq->arb = r->arb = a->arb | b->arb;
    if (q) *q = qq;
    if (rem) *rem = r;
    return 1;
}

/* The shift amount of `b` for a `w`-bit shift, saturated at w. */
static uint32_t _shift_amount(const Smt2Value *b, uint32_t w) {
    for (uint32_t i = 1; i < NL(b->width); i++) if (b->u.limbs[i]) return w;
    return b->u.limbs[0] >= w ? w : (uint32_t)b->u.limbs[0];
}

/* kind: 0 shl, 1 lshr, 2 ashr. */
static Smt2Value *_shift(Smt2Eval *ev, const Smt2Value *a, uint32_t s, int kind) {
    uint32_t w = a->width;
    Smt2Value *r = _bv_new(ev, w);
    if (!r) return NULL;
    int fill = kind == 2 ? _msb(a) : 0;
    for (uint32_t i = 0; i < w; i++) {
        int bit;
        if (kind == 0) bit = i >= s ? _bit(a, i - s) : 0;
        else           bit = (uint64_t)i + s < w ? _bit(a, i + s) : fill;
        if (bit) _set_bit(r, i, 1);
    }
    r->arb = a->arb;
    return r;
}

/* r[dst_off .. dst_off+n) = a[src_off .. src_off+n). */
static void _copy_bits(Smt2Value *r, uint32_t dst_off, const Smt2Value *a,
                       uint32_t src_off, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (_bit(a, src_off + i)) _set_bit(r, dst_off + i, 1);
}

static Smt2Value *_extend(Smt2Eval *ev, const Smt2Value *a, uint32_t w, int sign) {
    Smt2Value *r = _bv_new(ev, w);
    if (!r) return NULL;
    _copy_bits(r, 0, a, 0, a->width);
    if (sign && _msb(a))
        for (uint32_t i = a->width; i < w; i++) _set_bit(r, i, 1);
    r->arb = a->arb;
    return r;
}

/* ------------------------------------------------------------------ */
/* Equality and arrays                                                 */
/* ------------------------------------------------------------------ */

static const Smt2Value *_select(Smt2Eval *ev, const ArrVal *a, const Smt2Value *k);

/* 1 equal, 0 different, -1 undecidable (arrays the model cannot compare). */
static int _eq(Smt2Eval *ev, const Smt2Value *a, const Smt2Value *b);

static int _arr_enumerable(const ArrVal *a) {
    return a->const_elem || a->total;
}

/* Extensional equality: the arrays agree at every index. */
static int _arr_eq(Smt2Eval *ev, const ArrVal *a, const ArrVal *b) {
    const ESort *is = a->sort->idx;
    if (is->kind == SMT2_VAL_BOOL || (is->kind == SMT2_VAL_BV
            && is->width <= EVAL_ENUM_INDEX_BITS
            && _arr_enumerable(a) && _arr_enumerable(b))) {
        uint32_t n = is->kind == SMT2_VAL_BOOL ? 2u : 1u << is->width;
        for (uint32_t i = 0; i < n; i++) {
            const Smt2Value *k;
            if (is->kind == SMT2_VAL_BOOL) {
                k = _bool(ev, (int)i, 0);
            } else {
                Smt2Value *kv = _bv_new(ev, is->width);
                if (kv) kv->u.limbs[0] = i;
                k = kv;
            }
            if (!k) return -1;
            const Smt2Value *x = _select(ev, a, k), *y = _select(ev, b, k);
            if (!x || !y) return -1;
            int e = _eq(ev, x, y);
            if (e != 1) return e;
        }
        return 1;
    }
    /* Too many indices to enumerate: compare at every stored index, then the
     * bases at the rest -- decidable only when both bases are constant, or are
     * the same declared array. */
    for (int side = 0; side < 2; side++) {
        for (const Store *s = (side ? b : a)->stores; s; s = s->next) {
            const Smt2Value *x = _select(ev, a, s->key), *y = _select(ev, b, s->key);
            if (!x || !y) return -1;
            int e = _eq(ev, x, y);
            if (e != 1) return e;
        }
    }
    if (a->const_elem && b->const_elem) return _eq(ev, a->const_elem, b->const_elem);
    if (a->model && a->model == b->model) {
        if (!a->prefix && !b->prefix) return 1;
        if (a->prefix && b->prefix) {
            uint32_t ow = a->flat_w - a->inner_w;
            if (!memcmp(a->prefix, b->prefix, NL(ow) * sizeof(uint64_t))) return 1;
        }
    }
    return -1;
}

static int _eq(Smt2Eval *ev, const Smt2Value *a, const Smt2Value *b) {
    if (a->kind != b->kind) return -1;
    switch (a->kind) {
    case SMT2_VAL_BOOL: return a->u.b == b->u.b;
    case SMT2_VAL_BV:
        if (a->width != b->width) return -1;
        return _ucmp(a, b) == 0;
    case SMT2_VAL_OPAQUE:
        /* Two names may denote one element: only identical names decide. */
        if (a->u.opq.len == b->u.opq.len && !memcmp(a->u.opq.s, b->u.opq.s, a->u.opq.len))
            return 1;
        return -1;
    case SMT2_VAL_ARRAY:
        if (a->u.arr == b->u.arr) return 1;
        if (!_sort_eq(a->u.arr->sort, b->u.arr->sort)) return -1;
        return _arr_eq(ev, a->u.arr, b->u.arr);
    }
    return -1;
}

/* A declared array of the model, or its slice at an outer index. */
static const Smt2Value *_model_array(Smt2Eval *ev, const void *h, uint32_t flat_w,
                                     uint32_t inner_w, uint32_t elem_w, int total,
                                     const uint64_t *prefix) {
    ArrVal *a = (ArrVal *)_alloc(ev, sizeof *a);
    Smt2Value *v = (Smt2Value *)_alloc(ev, sizeof *v);
    if (!a || !v) return NULL;
    const ESort *elem = _sort_bv(ev, elem_w);
    if (inner_w && !prefix)
        a->sort = _sort_array(ev, _sort_bv(ev, flat_w - inner_w),
                              _sort_array(ev, _sort_bv(ev, inner_w), elem));
    else
        a->sort = _sort_array(ev, _sort_bv(ev, inner_w ? inner_w : flat_w), elem);
    if (!a->sort) return NULL;
    a->model = h;
    a->flat_w = flat_w;
    a->inner_w = inner_w;
    a->elem_w = elem_w;
    a->total = total;
    a->prefix = prefix;
    v->kind = SMT2_VAL_ARRAY;
    v->u.arr = a;
    return v;
}

static const Smt2Value *_select(Smt2Eval *ev, const ArrVal *a, const Smt2Value *k) {
    if (!_has_sort(k, a->sort->idx)) return _fail(ev, "select: the index has the wrong sort");
    for (const Store *s = a->stores; s; s = s->next) {
        int e = _eq(ev, k, s->key);
        if (e < 0) return _fail(ev, "select: cannot compare array indices");
        if (e) return s->val;
    }
    if (a->const_elem) return a->const_elem;
    if (!a->model) return _fail(ev, "select: array has no base");
    if (a->inner_w && !a->prefix) {
        uint64_t *pre = (uint64_t *)_alloc(ev, NL(k->width) * sizeof(uint64_t));
        if (!pre) return NULL;
        memcpy(pre, k->u.limbs, NL(k->width) * sizeof(uint64_t));
        return _model_array(ev, a->model, a->flat_w, a->inner_w, a->elem_w, a->total, pre);
    }
    /* The flat key: (outer index ++ inner index) for a slice. */
    Smt2Value *key = _bv_new(ev, a->flat_w);
    if (!key) return NULL;
    _copy_bits(key, 0, k, 0, k->width);
    if (a->prefix) {
        Smt2Value pv;
        pv.kind = SMT2_VAL_BV;
        pv.width = a->flat_w - a->inner_w;
        pv.u.limbs = (uint64_t *)a->prefix;
        _copy_bits(key, a->inner_w, &pv, 0, pv.width);
    }
    Smt2Value *r = _bv_new(ev, a->elem_w);
    if (!r) return NULL;
    int rc = ev->host.array_elem(ev->host.ctx, a->model, key->u.limbs, r->u.limbs, a->elem_w);
    if (!rc) return _fail(ev, "select: no model value for an array element");
    _mask(r);
    r->arb = (uint8_t)(rc == 2 || k->arb);
    return r;
}

static const Smt2Value *_store(Smt2Eval *ev, const Smt2Value *av, const Smt2Value *k,
                               const Smt2Value *v) {
    const ArrVal *a = av->u.arr;
    if (!_has_sort(k, a->sort->idx) || !_has_sort(v, a->sort->elem))
        return _fail(ev, "store: index or value has the wrong sort");
    ArrVal *r = (ArrVal *)_alloc(ev, sizeof *r);
    Store *s = (Store *)_alloc(ev, sizeof *s);
    Smt2Value *rv = (Smt2Value *)_alloc(ev, sizeof *rv);
    if (!r || !s || !rv) return NULL;
    *r = *a;
    s->key = k;
    s->val = v;
    s->next = a->stores;
    r->stores = s;
    rv->kind = SMT2_VAL_ARRAY;
    rv->u.arr = r;
    rv->arb = (uint8_t)(av->arb | k->arb | v->arb);
    return rv;
}

/* ------------------------------------------------------------------ */
/* Memo of define-fun applications                                     */
/* ------------------------------------------------------------------ */

static uint64_t _hash_mix(uint64_t h, uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

static uint64_t _hash_val(const Smt2Value *v) {
    uint64_t h = v->kind;
    switch (v->kind) {
    case SMT2_VAL_BOOL: return _hash_mix(h, v->u.b);
    case SMT2_VAL_BV:
        h = _hash_mix(h, v->width);
        for (uint32_t i = 0; i < NL(v->width); i++) h = _hash_mix(h, v->u.limbs[i]);
        return h;
    case SMT2_VAL_OPAQUE:
        for (uint32_t i = 0; i < v->u.opq.len; i++) h = _hash_mix(h, (uint8_t)v->u.opq.s[i]);
        return h;
    default:
        return _hash_mix(h, (uint64_t)(uintptr_t)v->u.arr);
    }
}

/* Identical for memoization: arrays only by identity. */
static int _same(const Smt2Value *a, const Smt2Value *b) {
    if (a == b) return 1;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case SMT2_VAL_BOOL: return a->u.b == b->u.b;
    case SMT2_VAL_BV:
        return a->width == b->width
            && !memcmp(a->u.limbs, b->u.limbs, NL(a->width) * sizeof(uint64_t));
    case SMT2_VAL_OPAQUE:
        return a->u.opq.len == b->u.opq.len && !memcmp(a->u.opq.s, b->u.opq.s, a->u.opq.len);
    default:
        return a->u.arr == b->u.arr;
    }
}

static uint64_t _memo_hash(const Sexpr *body, uint32_t n, const Smt2Value *const *args) {
    uint64_t h = (uint64_t)(uintptr_t)body;
    for (uint32_t i = 0; i < n; i++) h = _hash_mix(h, _hash_val(args[i]));
    return h;
}

static const Smt2Value *_memo_find(Smt2Eval *ev, const Sexpr *body, uint32_t n,
                                   const Smt2Value *const *args, uint64_t h) {
    if (!ev->memo_cap) return NULL;
    for (uint32_t i = (uint32_t)h & (ev->memo_cap - 1);; i = (i + 1) & (ev->memo_cap - 1)) {
        const Memo *m = &ev->memo[i];
        if (!m->body) return NULL;
        if (m->hash != h || m->body != body || m->n != n) continue;
        uint32_t j = 0;
        while (j < n && _same(m->args[j], args[j])) j++;
        if (j == n) return m->res;
    }
}

static void _memo_put(Smt2Eval *ev, const Sexpr *body, uint32_t n,
                      const Smt2Value *const *args, uint64_t h, const Smt2Value *res) {
    if ((ev->memo_n + 1) * 2 > ev->memo_cap) {
        uint32_t nc = ev->memo_cap ? ev->memo_cap * 2 : 256;
        Memo *nm = (Memo *)calloc(nc, sizeof *nm);
        if (!nm) return;   /* memoization is only an optimisation */
        for (uint32_t i = 0; i < ev->memo_cap; i++) {
            const Memo *m = &ev->memo[i];
            if (!m->body) continue;
            uint32_t j = (uint32_t)m->hash & (nc - 1);
            while (nm[j].body) j = (j + 1) & (nc - 1);
            nm[j] = *m;
        }
        free(ev->memo);
        ev->memo = nm;
        ev->memo_cap = nc;
    }
    const Smt2Value **saved = NULL;
    if (n) {
        saved = (const Smt2Value **)_alloc(ev, n * sizeof *saved);
        if (!saved) { ev->failed = 0; ev->err[0] = 0; return; }
        memcpy(saved, args, n * sizeof *saved);
    }
    uint32_t j = (uint32_t)h & (ev->memo_cap - 1);
    while (ev->memo[j].body) j = (j + 1) & (ev->memo_cap - 1);
    ev->memo[j].body = body;
    ev->memo[j].n = n;
    ev->memo[j].args = saved;
    ev->memo[j].res = res;
    ev->memo[j].hash = h;
    ev->memo_n++;
}

/* ------------------------------------------------------------------ */
/* Sorts and literals                                                  */
/* ------------------------------------------------------------------ */

static const ESort *_parse_sort(Smt2Eval *ev, const Sexpr *s, uint32_t depth) {
    if (depth > 64) { _fail(ev, "sort nested too deeply"); return NULL; }
    if (s->kind == SEXPR_SYMBOL) {
        if (sexpr_is_symbol(s, "Bool")) return &ev->bool_sort;
        const Sexpr *def = ev->host.sort_alias
            ? ev->host.sort_alias(ev->host.ctx, s->sym.str, s->sym.len) : NULL;
        if (def) return _parse_sort(ev, def, depth + 1);
        ESort *o = (ESort *)_alloc(ev, sizeof *o);
        if (o) o->kind = SMT2_VAL_OPAQUE;
        return o;
    }
    if (s->kind == SEXPR_LIST && s->list.count == 3
            && sexpr_is_symbol(s->list.items[0], "_")
            && sexpr_is_symbol(s->list.items[1], "BitVec")
            && s->list.items[2]->kind == SEXPR_NUMERAL) {
        uint64_t w = s->list.items[2]->numval;
        if (w == 0 || w > EVAL_MAX_WIDTH) { _fail(ev, "bad BitVec width"); return NULL; }
        return _sort_bv(ev, (uint32_t)w);
    }
    if (s->kind == SEXPR_LIST && s->list.count == 3 && sexpr_is_symbol(s->list.items[0], "Array"))
        return _sort_array(ev, _parse_sort(ev, s->list.items[1], depth + 1),
                           _parse_sort(ev, s->list.items[2], depth + 1));
    _fail(ev, "unsupported sort");
    return NULL;
}

/* (_ bvN W): N in decimal, reduced mod 2^W. */
static const Smt2Value *_bv_decimal(Smt2Eval *ev, const Sexpr *sym, uint64_t w) {
    if (w == 0 || w > EVAL_MAX_WIDTH) return _fail(ev, "bit-vector width out of range");
    const char *p = sym->sym.str + 2;
    uint32_t n = sym->sym.len - 2;
    if (n == 0) return _fail(ev, "malformed bit-vector literal");
    Smt2Value *r = _bv_new(ev, (uint32_t)w);
    if (!r) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return _fail(ev, "malformed bit-vector literal");
        /* r = r * 10 + digit */
        uint64_t carry = (uint64_t)(p[i] - '0');
        for (uint32_t k = 0; k < NL(r->width); k++) {
            uint64_t lo, hi;
            _mul64(r->u.limbs[k], 10u, &lo, &hi);
            lo += carry;
            hi += lo < carry;
            r->u.limbs[k] = lo;
            carry = hi;
        }
        _mask(r);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* The evaluator                                                       */
/* ------------------------------------------------------------------ */

static const Smt2Value *_ev(Smt2Eval *ev, const Sexpr *s, const Env *env);

static int _is_bv(const Smt2Value *v) { return v->kind == SMT2_VAL_BV; }

static uint64_t _hash_name(const char *s, uint32_t len) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 0x100000001B3ull;
    return h;
}

/* The cache entry for a global name, created empty. NULL when out of memory
 * (the caller then resolves uncached). */
static SymEnt *_sym(Smt2Eval *ev, const char *name, uint32_t len) {
    if ((ev->syms_n + 1) * 2 > ev->syms_cap) {
        uint32_t nc = ev->syms_cap ? ev->syms_cap * 2 : 256;
        SymEnt *ns = (SymEnt *)calloc(nc, sizeof *ns);
        if (!ns) return NULL;
        for (uint32_t i = 0; i < ev->syms_cap; i++) {
            if (!ev->syms[i].name) continue;
            uint32_t j = (uint32_t)_hash_name(ev->syms[i].name, ev->syms[i].len) & (nc - 1);
            while (ns[j].name) j = (j + 1) & (nc - 1);
            ns[j] = ev->syms[i];
        }
        free(ev->syms);
        ev->syms = ns;
        ev->syms_cap = nc;
    }
    uint32_t j = (uint32_t)_hash_name(name, len) & (ev->syms_cap - 1);
    for (;; j = (j + 1) & (ev->syms_cap - 1)) {
        SymEnt *e = &ev->syms[j];
        if (!e->name) break;
        if (e->len == len && !memcmp(e->name, name, len)) return e;
    }
    ev->syms[j].name = name;   /* names live in the term or the evaluator arena */
    ev->syms[j].len = len;
    ev->syms_n++;
    return &ev->syms[j];
}

/* Resolve `name` as a define-fun or an uninterpreted function. */
static void _resolve_fun(Smt2Eval *ev, SymEnt *e, const char *name, uint32_t len) {
    if (e->fun_known) return;
    e->fun_known = 1;
    Smt2EvalFun f;
    if (ev->host.fun && ev->host.fun(ev->host.ctx, name, len, &f)) {
        Smt2EvalFun *c = (Smt2EvalFun *)_alloc(ev, sizeof *c);
        if (c) { *c = f; e->fun = c; }
        return;
    }
    if (ev->host.uf_arity) e->uf = ev->host.uf_arity(ev->host.ctx, name, len);
}

static const Smt2Value *_lookup_env(const Env *env, const char *name, uint32_t len) {
    for (; env; env = env->next)
        if (env->len == len && !memcmp(env->name, name, len)) return env->val;
    return NULL;
}

/* Apply a define-fun to evaluated arguments: its body in an environment of the
 * parameters alone (define-fun bodies are closed over the global scope). */
static const Smt2Value *_apply_fun(Smt2Eval *ev, const Smt2EvalFun *f,
                                   const Smt2Value *const *args) {
    uint64_t h = _memo_hash(f->body, f->n_params, args);
    const Smt2Value *m = _memo_find(ev, f->body, f->n_params, args, h);
    if (m) return m;
    Env *env = NULL;
    for (uint32_t i = 0; i < f->n_params; i++) {
        Env *e = (Env *)_alloc(ev, sizeof *e);
        if (!e) return NULL;
        e->name = f->param_names[i];
        e->len = (uint32_t)strlen(f->param_names[i]);
        e->val = args[i];
        e->next = env;
        env = e;
    }
    const Smt2Value *r = _ev(ev, f->body, env);
    if (r) _memo_put(ev, f->body, f->n_params, args, h, r);
    return r;
}

static const Smt2Value *_global(Smt2Eval *ev, const Sexpr *s);

static const Smt2Value *_symbol(Smt2Eval *ev, const Sexpr *s, const Env *env) {
    const char *name = s->sym.str;
    uint32_t len = s->sym.len;
    const Smt2Value *v = _lookup_env(env, name, len);
    if (v) return v;
    SymEnt *e = _sym(ev, name, len);
    if (e && e->val_known) return e->val;
    v = _global(ev, s);
    /* Look the entry up again: evaluating a define-fun body may have grown
     * the table. Failures are not cached; they end the evaluation anyway. */
    if (v && (e = _sym(ev, name, len)) != NULL) { e->val_known = 1; e->val = v; }
    return v;
}

/* A global symbol's value: a constant, a nullary define-fun, a declared array
 * or a sort constant. */
static const Smt2Value *_global(Smt2Eval *ev, const Sexpr *s) {
    const char *name = s->sym.str;
    uint32_t len = s->sym.len;
    if (sexpr_is_symbol(s, "true"))  return _bool(ev, 1, 0);
    if (sexpr_is_symbol(s, "false")) return _bool(ev, 0, 0);
    SymEnt *e = _sym(ev, name, len);
    Smt2EvalFun f;
    const Smt2EvalFun *fp = NULL;
    if (e) {
        _resolve_fun(ev, e, name, len);
        fp = e->fun;
    } else if (ev->host.fun && ev->host.fun(ev->host.ctx, name, len, &f)) {
        fp = &f;
    }
    if (fp) {
        if (fp->n_params) return _fail(ev, "'%.*s' takes %u arguments", (int)len, name, fp->n_params);
        return _apply_fun(ev, fp, NULL);
    }
    uint32_t w = 0;
    int is_bool = 0;
    uint64_t small[4];
    if (ev->host.scalar(ev->host.ctx, name, len, &w, &is_bool, small, 4)) {
        if (is_bool) return _bool(ev, (int)(small[0] & 1u), 0);
        Smt2Value *r = _bv_new(ev, w);
        if (!r) return NULL;
        if (NL(w) <= 4) {
            memcpy(r->u.limbs, small, NL(w) * sizeof(uint64_t));
        } else if (!ev->host.scalar(ev->host.ctx, name, len, &w, &is_bool,
                                    r->u.limbs, NL(w))) {
            return _fail(ev, "no model value for '%.*s'", (int)len, name);
        }
        _mask(r);
        return r;
    }
    uint32_t iw, inw, ew;
    int total = 0;
    const void *h = ev->host.array(ev->host.ctx, name, len, &iw, &inw, &ew, &total);
    if (h) {
        return _model_array(ev, h, iw, inw, ew, total, NULL);
    }
    if (ev->host.opaque_const && ev->host.opaque_const(ev->host.ctx, name, len)) {
        Smt2Value *o = (Smt2Value *)_alloc(ev, sizeof *o);
        if (!o) return NULL;
        o->kind = SMT2_VAL_OPAQUE;
        o->u.opq.s = name;
        o->u.opq.len = len;
        return o;
    }
    return _fail(ev, "unknown symbol '%.*s'", (int)len, name);
}

/* An uninterpreted function over declare-sort constants: the model holds its
 * value at each argument tuple the assertions use, named "f@a@b". */
static const Smt2Value *_apply_uf(Smt2Eval *ev, const Sexpr *head,
                                  const Smt2Value *const *args, uint32_t n) {
    char name[512];
    uint32_t len = 0;
    if (head->sym.len >= sizeof name) return _fail(ev, "name too long");
    memcpy(name, head->sym.str, head->sym.len);
    len = head->sym.len;
    for (uint32_t i = 0; i < n; i++) {
        if (args[i]->kind != SMT2_VAL_OPAQUE)
            return _fail(ev, "'%.*s': argument %u is not a sort constant",
                         (int)head->sym.len, head->sym.str, i + 1);
        if (len + 1 + args[i]->u.opq.len >= sizeof name) return _fail(ev, "name too long");
        name[len++] = '@';
        memcpy(name + len, args[i]->u.opq.s, args[i]->u.opq.len);
        len += args[i]->u.opq.len;
    }
    char *copy = (char *)_alloc(ev, len + 1);
    if (!copy) return NULL;
    memcpy(copy, name, len);
    Sexpr sym;
    sym.kind = SEXPR_SYMBOL;
    sym.sym.str = copy;
    sym.sym.len = len;
    const Smt2Value *r = _symbol(ev, &sym, NULL);
    if (!r) {
        ev->failed = 0;
        return _fail(ev, "no model value for '%.*s' at these arguments",
                     (int)head->sym.len, head->sym.str);
    }
    return r;
}

/* Indexed operator `(_ op i [j])` applied to one argument. */
static const Smt2Value *_indexed(Smt2Eval *ev, const Sexpr *head, const Sexpr *s,
                                 const Env *env) {
    if (head->list.count < 3 || head->list.items[1]->kind != SEXPR_SYMBOL)
        return _fail(ev, "unknown indexed operator");
    for (uint32_t i = 2; i < head->list.count; i++)
        if (head->list.items[i]->kind != SEXPR_NUMERAL)
            return _fail(ev, "indexed operator: numeral expected");
    const Sexpr *op = head->list.items[1];
    uint64_t i0 = head->list.items[2]->numval;
    if (s->list.count != 2) return _fail(ev, "indexed operator takes one argument");
    const Smt2Value *a = _ev(ev, s->list.items[1], env);
    if (!a) return NULL;
    if (!_is_bv(a)) return _fail(ev, "indexed operator: bit-vector argument expected");
    uint32_t w = a->width;
    Smt2Value *r;
    if (sexpr_is_symbol(op, "extract") && head->list.count == 4) {
        uint64_t lo = head->list.items[3]->numval;
        if (i0 < lo || i0 >= w) return _fail(ev, "extract: bad indices");
        r = _bv_new(ev, (uint32_t)(i0 - lo + 1));
        if (!r) return NULL;
        _copy_bits(r, 0, a, (uint32_t)lo, r->width);
    } else if (head->list.count != 3) {
        return _fail(ev, "unknown indexed operator");
    } else if (sexpr_is_symbol(op, "zero_extend") || sexpr_is_symbol(op, "sign_extend")) {
        if (i0 > EVAL_MAX_WIDTH - w) return _fail(ev, "extend: width out of range");
        r = _extend(ev, a, w + (uint32_t)i0, sexpr_is_symbol(op, "sign_extend"));
        if (!r) return NULL;
    } else if (sexpr_is_symbol(op, "repeat")) {
        if (i0 == 0 || i0 > EVAL_MAX_WIDTH / w) return _fail(ev, "repeat: bad count");
        r = _bv_new(ev, w * (uint32_t)i0);
        if (!r) return NULL;
        for (uint32_t k = 0; k < (uint32_t)i0; k++) _copy_bits(r, k * w, a, 0, w);
    } else if (sexpr_is_symbol(op, "rotate_left") || sexpr_is_symbol(op, "rotate_right")) {
        uint32_t n = (uint32_t)(i0 % w);
        if (sexpr_is_symbol(op, "rotate_right")) n = (w - n) % w;
        r = _bv_new(ev, w);
        if (!r) return NULL;
        for (uint32_t k = 0; k < w; k++)
            if (_bit(a, k)) _set_bit(r, (k + n) % w, 1);
    } else {
        return _fail(ev, "unknown indexed operator '%.*s'", (int)op->sym.len, op->sym.str);
    }
    r->arb = a->arb;
    return r;
}

static const Smt2Value *_let(Smt2Eval *ev, const Sexpr *s, const Env *env) {
    if (s->list.count != 3 || s->list.items[1]->kind != SEXPR_LIST)
        return _fail(ev, "malformed let");
    const Sexpr *binds = s->list.items[1];
    const Env *inner = env;
    /* Parallel binding: every bound term is evaluated in the outer scope. */
    for (uint32_t i = 0; i < binds->list.count; i++) {
        const Sexpr *b = binds->list.items[i];
        if (b->kind != SEXPR_LIST || b->list.count != 2 || b->list.items[0]->kind != SEXPR_SYMBOL)
            return _fail(ev, "malformed let binding");
        const Smt2Value *v = _ev(ev, b->list.items[1], env);
        if (!v) return NULL;
        Env *e = (Env *)_alloc(ev, sizeof *e);
        if (!e) return NULL;
        e->name = b->list.items[0]->sym.str;
        e->len = b->list.items[0]->sym.len;
        e->val = v;
        e->next = inner;
        inner = e;
    }
    return _ev(ev, s->list.items[2], inner);
}

/* Bool connectives and ite, which evaluate only the arguments they need. */
static const Smt2Value *_lazy_op(Smt2Eval *ev, const Sexpr *s, const Env *env, int *handled) {
    const Sexpr *h = s->list.items[0];
    uint32_t n = s->list.count - 1;
    *handled = 1;
    if (sexpr_is_symbol(h, "ite")) {
        if (n != 3) return _fail(ev, "ite takes 3 arguments");
        const Smt2Value *c = _ev(ev, s->list.items[1], env);
        if (!c) return NULL;
        if (c->kind != SMT2_VAL_BOOL) return _fail(ev, "ite: Bool condition expected");
        const Smt2Value *t = _ev(ev, s->list.items[c->u.b ? 2 : 3], env);
        if (!t || !c->arb) return t;
        Smt2Value *r = (Smt2Value *)_alloc(ev, sizeof *r);
        if (!r) return NULL;
        *r = *t;
        r->arb = 1;
        return r;
    }
    int is_and = sexpr_is_symbol(h, "and"), is_or = sexpr_is_symbol(h, "or");
    int is_imp = sexpr_is_symbol(h, "=>");
    if (!is_and && !is_or && !is_imp) { *handled = 0; return NULL; }
    if (is_imp && n < 2) return _fail(ev, "=> takes at least 2 arguments");
    int arb = 0;
    for (uint32_t i = 1; i <= n; i++) {
        const Smt2Value *a = _ev(ev, s->list.items[i], env);
        if (!a) return NULL;
        if (a->kind != SMT2_VAL_BOOL) return _fail(ev, "%.*s: Bool argument expected",
                                                   (int)h->sym.len, h->sym.str);
        arb |= a->arb;
        if (is_imp) {
            if (i == n) return _bool(ev, (int)a->u.b, arb);
            if (!a->u.b) return _bool(ev, 1, arb);
        } else if (is_and ? !a->u.b : (int)a->u.b) {
            return _bool(ev, is_or, arb);
        }
    }
    return _bool(ev, is_and, arb);
}

/* Operators over evaluated arguments. */
static const Smt2Value *_op(Smt2Eval *ev, const Sexpr *h, const Smt2Value *const *a, uint32_t n) {
    const char *op = h->sym.str;
    uint32_t oplen = h->sym.len;
#define IS(name) (oplen == sizeof(name) - 1 && !memcmp(op, name, oplen))
    int arb = 0;
    for (uint32_t i = 0; i < n; i++) arb |= a[i]->arb;

    if (IS("not")) {
        if (n != 1 || a[0]->kind != SMT2_VAL_BOOL) return _fail(ev, "not: one Bool argument expected");
        return _bool(ev, !a[0]->u.b, arb);
    }
    if (IS("xor")) {
        uint64_t x = 0;
        if (n < 1) return _fail(ev, "xor: arguments expected");
        for (uint32_t i = 0; i < n; i++) {
            if (a[i]->kind != SMT2_VAL_BOOL) return _fail(ev, "xor: Bool argument expected");
            x ^= a[i]->u.b;
        }
        return _bool(ev, (int)x, arb);
    }
    if (IS("=") || IS("distinct")) {
        if (n < 2) return _fail(ev, "%.*s takes at least 2 arguments", (int)oplen, op);
        int dist = IS("distinct");
        /* = is chainable (adjacent pairs); distinct is pairwise. */
        for (uint32_t i = 0; i + 1 < n; i++) {
            for (uint32_t j = i + 1; j < (dist ? n : i + 2); j++) {
                if (a[i]->kind != a[j]->kind || (_is_bv(a[i]) && a[i]->width != a[j]->width))
                    return _fail(ev, "%.*s: arguments of different sorts", (int)oplen, op);
                int e = _eq(ev, a[i], a[j]);
                if (e < 0) {
                    if (ev->failed) return NULL;
                    return _fail(ev, "%.*s: the model does not decide this equality",
                                 (int)oplen, op);
                }
                if (dist ? e : !e) return _bool(ev, 0, arb);
            }
        }
        return _bool(ev, 1, arb);
    }
    if (IS("select")) {
        if (n != 2 || a[0]->kind != SMT2_VAL_ARRAY) return _fail(ev, "select: array and index expected");
        const Smt2Value *r = _select(ev, a[0]->u.arr, a[1]);
        if (!r || !arb || r->arb) return r;
        Smt2Value *c = (Smt2Value *)_alloc(ev, sizeof *c);
        if (!c) return NULL;
        *c = *r;
        c->arb = 1;
        return c;
    }
    if (IS("store")) {
        if (n != 3 || a[0]->kind != SMT2_VAL_ARRAY) return _fail(ev, "store: array, index and value expected");
        return _store(ev, a[0], a[1], a[2]);
    }

    /* Everything below takes bit-vectors of one width. */
    if (n == 0) return _fail(ev, "unknown function '%.*s'", (int)oplen, op);
    for (uint32_t i = 0; i < n; i++)
        if (!_is_bv(a[i])) return _fail(ev, "%.*s: bit-vector argument expected", (int)oplen, op);

    if (IS("concat")) {
        uint64_t w = 0;
        for (uint32_t i = 0; i < n; i++) w += a[i]->width;
        if (w > EVAL_MAX_WIDTH) return _fail(ev, "concat: result too wide");
        Smt2Value *r = _bv_new(ev, (uint32_t)w);
        if (!r) return NULL;
        uint32_t off = 0;
        for (uint32_t i = n; i-- > 0;) { _copy_bits(r, off, a[i], 0, a[i]->width); off += a[i]->width; }
        r->arb = (uint8_t)arb;
        return r;
    }
    uint32_t w = a[0]->width;
    for (uint32_t i = 1; i < n; i++)
        if (a[i]->width != w) return _fail(ev, "%.*s: arguments of different widths", (int)oplen, op);

    /* Unary */
    if (n == 1) {
        if (IS("bvnot")) return _not(ev, a[0]);
        if (IS("bvneg")) return _neg(ev, a[0]);
        if (IS("bvredor") || IS("bvredand")) {
            Smt2Value *r = _bv_new(ev, 1);
            if (!r) return NULL;
            int all = 1;
            for (uint32_t i = 0; i < w; i++) all &= _bit(a[0], i);
            r->u.limbs[0] = IS("bvredor") ? !_is_zero(a[0]) : (uint64_t)all;
            r->arb = (uint8_t)arb;
            return r;
        }
        if (IS("bvnego")) {
            /* -x overflows only for the most negative value. */
            int min = _msb(a[0]);
            for (uint32_t i = 0; i + 1 < w && min; i++) min = !_bit(a[0], i);
            return _bool(ev, min, arb);
        }
    }

    /* Left-associative, n-ary */
    if (IS("bvand") || IS("bvor") || IS("bvxor") || IS("bvadd") || IS("bvmul") || IS("bvsub")) {
        if (n < 2) return _fail(ev, "%.*s takes at least 2 arguments", (int)oplen, op);
        const Smt2Value *acc = a[0];
        for (uint32_t i = 1; i < n && acc; i++) {
            if (IS("bvadd")) { acc = _add(ev, acc, a[i]); continue; }
            if (IS("bvsub")) { acc = _sub(ev, acc, a[i]); continue; }
            if (IS("bvmul")) { acc = _mul(ev, acc, a[i]); continue; }
            Smt2Value *r = _bv_new(ev, w);
            if (!r) return NULL;
            for (uint32_t k = 0; k < NL(w); k++) {
                uint64_t x = acc->u.limbs[k], y = a[i]->u.limbs[k];
                r->u.limbs[k] = IS("bvand") ? (x & y) : IS("bvor") ? (x | y) : (x ^ y);
            }
            r->arb = (uint8_t)(acc->arb | a[i]->arb);
            acc = r;
        }
        return acc;
    }

    if (n != 2) return _fail(ev, "unknown function '%.*s' with %u arguments", (int)oplen, op, n);
    const Smt2Value *x = a[0], *y = a[1];

    if (IS("bvult")) return _bool(ev, _ucmp(x, y) < 0, arb);
    if (IS("bvule")) return _bool(ev, _ucmp(x, y) <= 0, arb);
    if (IS("bvugt")) return _bool(ev, _ucmp(x, y) > 0, arb);
    if (IS("bvuge")) return _bool(ev, _ucmp(x, y) >= 0, arb);
    if (IS("bvslt")) return _bool(ev, _scmp(x, y) < 0, arb);
    if (IS("bvsle")) return _bool(ev, _scmp(x, y) <= 0, arb);
    if (IS("bvsgt")) return _bool(ev, _scmp(x, y) > 0, arb);
    if (IS("bvsge")) return _bool(ev, _scmp(x, y) >= 0, arb);

    if (IS("bvnand") || IS("bvnor") || IS("bvxnor")) {
        Smt2Value *r = _bv_new(ev, w);
        if (!r) return NULL;
        for (uint32_t k = 0; k < NL(w); k++) {
            uint64_t p = x->u.limbs[k], q = y->u.limbs[k];
            r->u.limbs[k] = ~(IS("bvnand") ? (p & q) : IS("bvnor") ? (p | q) : (p ^ q));
        }
        _mask(r);
        r->arb = (uint8_t)arb;
        return r;
    }
    if (IS("bvcomp")) {
        Smt2Value *r = _bv_new(ev, 1);
        if (!r) return NULL;
        r->u.limbs[0] = _ucmp(x, y) == 0;
        r->arb = (uint8_t)arb;
        return r;
    }
    if (IS("bvshl"))  return _shift(ev, x, _shift_amount(y, w), 0);
    if (IS("bvlshr")) return _shift(ev, x, _shift_amount(y, w), 1);
    if (IS("bvashr")) return _shift(ev, x, _shift_amount(y, w), 2);
    if (IS("bvudiv") || IS("bvurem")) {
        Smt2Value *q, *r;
        if (!_udivrem(ev, x, y, &q, &r)) return NULL;
        return IS("bvudiv") ? q : r;
    }
    if (IS("bvsdiv") || IS("bvsrem") || IS("bvsmod")) {
        int sx = _msb(x), sy = _msb(y);
        const Smt2Value *ax = sx ? _neg(ev, x) : x, *ay = sy ? _neg(ev, y) : y;
        if (!ax || !ay) return NULL;
        Smt2Value *q, *r;
        if (!_udivrem(ev, ax, ay, &q, &r)) return NULL;
        if (IS("bvsdiv")) return sx != sy ? _neg(ev, q) : q;
        if (IS("bvsrem")) return sx ? _neg(ev, r) : r;
        /* bvsmod: the remainder takes the divisor's sign. */
        if (_is_zero(r) || sx == sy) return sx ? _neg(ev, r) : r;
        if (!sx) return _add(ev, r, y);
        Smt2Value *nr = _neg(ev, r);
        return nr ? _add(ev, nr, y) : NULL;
    }
    if (IS("bvuaddo")) {
        Smt2Value *s = _add(ev, x, y);
        return s ? _bool(ev, _ucmp(s, x) < 0, arb) : NULL;
    }
    if (IS("bvsaddo")) {
        Smt2Value *s = _add(ev, x, y);
        return s ? _bool(ev, _msb(x) == _msb(y) && _msb(s) != _msb(x), arb) : NULL;
    }
    if (IS("bvusubo")) return _bool(ev, _ucmp(x, y) < 0, arb);
    if (IS("bvssubo")) {
        Smt2Value *s = _sub(ev, x, y);
        return s ? _bool(ev, _msb(x) != _msb(y) && _msb(s) != _msb(x), arb) : NULL;
    }
    if (IS("bvumulo") || IS("bvsmulo")) {
        if (w > EVAL_MAX_WIDTH / 2) return _fail(ev, "%.*s: too wide", (int)oplen, op);
        int sg = IS("bvsmulo");
        Smt2Value *xx = _extend(ev, x, 2 * w, sg), *yy = _extend(ev, y, 2 * w, sg);
        if (!xx || !yy) return NULL;
        Smt2Value *p = _mul(ev, xx, yy);
        if (!p) return NULL;
        int ovf = 0;
        if (!sg) {
            for (uint32_t i = w; i < 2 * w && !ovf; i++) ovf = _bit(p, i);
        } else {
            int top = _bit(p, w - 1);
            for (uint32_t i = w; i < 2 * w && !ovf; i++) ovf = _bit(p, i) != top;
        }
        return _bool(ev, ovf, arb);
    }
    if (IS("bvsdivo")) {
        int min = _msb(x);
        for (uint32_t i = 0; i + 1 < w && min; i++) min = !_bit(x, i);
        int all = 1;
        for (uint32_t i = 0; i < w && all; i++) all = _bit(y, i);
        return _bool(ev, min && all, arb);
    }
#undef IS
    return _fail(ev, "unknown function '%.*s'", (int)oplen, op);
}

static const Smt2Value *_list(Smt2Eval *ev, const Sexpr *s, const Env *env) {
    if (s->list.count == 0) return _fail(ev, "empty term");
    const Sexpr *h = s->list.items[0];

    if (h->kind == SEXPR_SYMBOL) {
        if (sexpr_is_symbol(h, "let")) return _let(ev, s, env);
        if (sexpr_is_symbol(h, "!")) {
            if (s->list.count < 2) return _fail(ev, "malformed annotation");
            return _ev(ev, s->list.items[1], env);
        }
        if (sexpr_is_symbol(h, "_")) {
            if (s->list.count == 3 && s->list.items[1]->kind == SEXPR_SYMBOL
                    && s->list.items[1]->sym.len > 2
                    && !memcmp(s->list.items[1]->sym.str, "bv", 2)
                    && s->list.items[2]->kind == SEXPR_NUMERAL)
                return _bv_decimal(ev, s->list.items[1], s->list.items[2]->numval);
            return _fail(ev, "unsupported indexed term");
        }
        if (sexpr_is_symbol(h, "forall") || sexpr_is_symbol(h, "exists")
                || sexpr_is_symbol(h, "match"))
            return _fail(ev, "unsupported: quantifiers and match");
        int handled;
        const Smt2Value *r = _lazy_op(ev, s, env, &handled);
        if (handled) return r;
    } else if (h->kind == SEXPR_LIST && h->list.count >= 2
               && sexpr_is_symbol(h->list.items[0], "_")) {
        return _indexed(ev, h, s, env);
    } else if (h->kind == SEXPR_LIST && h->list.count == 3
               && sexpr_is_symbol(h->list.items[0], "as")
               && sexpr_is_symbol(h->list.items[1], "const")) {
        const ESort *st = _parse_sort(ev, h->list.items[2], 0);
        if (!st) return NULL;
        if (st->kind != SMT2_VAL_ARRAY) return _fail(ev, "as const: array sort expected");
        if (s->list.count != 2) return _fail(ev, "constant array takes one value");
        const Smt2Value *e = _ev(ev, s->list.items[1], env);
        if (!e) return NULL;
        if (!_has_sort(e, st->elem)) return _fail(ev, "constant array: value has the wrong sort");
        ArrVal *a = (ArrVal *)_alloc(ev, sizeof *a);
        Smt2Value *v = (Smt2Value *)_alloc(ev, sizeof *v);
        if (!a || !v) return NULL;
        a->sort = st;
        a->const_elem = e;
        v->kind = SMT2_VAL_ARRAY;
        v->u.arr = a;
        v->arb = e->arb;
        return v;
    } else {
        return _fail(ev, "unsupported term");
    }

    /* A function applied to evaluated arguments. */
    uint32_t n = s->list.count - 1;
    const Smt2Value **args = (const Smt2Value **)_alloc(ev, (n ? n : 1) * sizeof *args);
    if (!args) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        args[i] = _ev(ev, s->list.items[i + 1], env);
        if (!args[i]) return NULL;
    }
    SymEnt *e = _sym(ev, h->sym.str, h->sym.len);
    if (!e) return _fail(ev, "out of memory");
    _resolve_fun(ev, e, h->sym.str, h->sym.len);
    if (e->fun) {
        if (e->fun->n_params != n)
            return _fail(ev, "'%.*s' takes %u arguments, not %u",
                         (int)h->sym.len, h->sym.str, e->fun->n_params, n);
        return _apply_fun(ev, e->fun, args);
    }
    if (e->uf) {
        if (e->uf != n) return _fail(ev, "'%.*s' takes %u arguments", (int)h->sym.len, h->sym.str, e->uf);
        return _apply_uf(ev, h, args, n);
    }
    return _op(ev, h, args, n);
}

static const Smt2Value *_ev(Smt2Eval *ev, const Sexpr *s, const Env *env) {
    if (ev->failed) return NULL;
    if (ev->depth >= ev->host.max_depth) return _fail(ev, "term nested too deeply");
    ev->depth++;
    const Smt2Value *r = NULL;
    switch (s->kind) {
    case SEXPR_SYMBOL:
        r = _symbol(ev, s, env);
        break;
    case SEXPR_BITVEC: {
        Smt2Value *v = _bv_new(ev, s->bv.width);
        if (!v) break;
        if (s->bv.width > 64) {
            if (!s->bv.limbs) { _fail(ev, "bit-vector literal has no value"); break; }
            memcpy(v->u.limbs, s->bv.limbs, NL(s->bv.width) * sizeof(uint64_t));
        } else {
            v->u.limbs[0] = s->bv.value;
        }
        _mask(v);
        r = v;
        break;
    }
    case SEXPR_LIST:
        r = _list(ev, s, env);
        break;
    case SEXPR_NUMERAL:
        _fail(ev, "a numeral is not a bit-vector term (use (_ bvN W))");
        break;
    default:
        _fail(ev, "unsupported term");
        break;
    }
    ev->depth--;
    return r;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Smt2Eval *smt2_eval_new(const Smt2EvalHost *host) {
    Smt2Eval *ev = (Smt2Eval *)calloc(1, sizeof *ev);
    if (!ev) return NULL;
    ev->host = *host;
    if (!ev->host.max_depth) ev->host.max_depth = 10000;
    sexpr_arena_init(&ev->arena, 64 * 1024);
    ev->bool_sort.kind = SMT2_VAL_BOOL;
    return ev;
}

void smt2_eval_free(Smt2Eval *ev) {
    if (!ev) return;
    sexpr_arena_destroy(&ev->arena);
    free(ev->memo);
    free(ev->syms);
    free(ev);
}

const Smt2Value *smt2_eval(Smt2Eval *ev, const Sexpr *term) {
    ev->failed = 0;
    ev->err[0] = 0;
    ev->depth = 0;
    return _ev(ev, term, NULL);
}

const char *smt2_eval_error(const Smt2Eval *ev) {
    return ev->err[0] ? ev->err : "cannot evaluate";
}

int smt2_value_kind(const Smt2Value *v) { return v->kind; }

uint32_t smt2_value_width(const Smt2Value *v) {
    return v->kind == SMT2_VAL_BV ? v->width : 0;
}

uint64_t smt2_value_u64(const Smt2Value *v) {
    if (v->kind == SMT2_VAL_BOOL) return v->u.b;
    if (v->kind == SMT2_VAL_BV) return v->u.limbs[0];
    return 0;
}

int smt2_value_arbitrary(const Smt2Value *v) { return v->arb; }

static void _print_sort(FILE *out, const ESort *s) {
    switch (s->kind) {
    case SMT2_VAL_BOOL: fputs("Bool", out); break;
    case SMT2_VAL_BV:   fprintf(out, "(_ BitVec %u)", (unsigned)s->width); break;
    case SMT2_VAL_ARRAY:
        fputs("(Array ", out);
        _print_sort(out, s->idx);
        fputc(' ', out);
        _print_sort(out, s->elem);
        fputc(')', out);
        break;
    default: fputs("?", out); break;
    }
}

int smt2_value_print(FILE *out, const Smt2Value *v, const Smt2EvalHost *host) {
    switch (v->kind) {
    case SMT2_VAL_BOOL:
        fputs(v->u.b ? "true" : "false", out);
        return 1;
    case SMT2_VAL_BV:
        fputs("#b", out);
        for (uint32_t i = v->width; i-- > 0;) fputc(_bit(v, i) ? '1' : '0', out);
        return 1;
    case SMT2_VAL_OPAQUE:
        fprintf(out, "%.*s", (int)v->u.opq.len, v->u.opq.s);
        return 1;
    default: break;
    }
    const ArrVal *a = v->u.arr;
    if (a->model && a->prefix) return 0;
    if (a->model && !host->print_array) return 0;
    uint32_t n = 0;
    for (const Store *s = a->stores; s; s = s->next) n++;
    for (uint32_t i = 0; i < n; i++) fputs("(store ", out);
    if (a->const_elem) {
        fputs("((as const ", out);
        _print_sort(out, a->sort);
        fputs(") ", out);
        if (!smt2_value_print(out, a->const_elem, host)) return 0;
        fputc(')', out);
    } else if (!host->print_array(host->ctx, out, a->model)) {
        return 0;
    }
    /* Oldest store innermost. */
    for (uint32_t i = n; i-- > 0;) {
        const Store *s = a->stores;
        for (uint32_t j = 0; j < i; j++) s = s->next;
        fputc(' ', out);
        if (!smt2_value_print(out, s->key, host)) return 0;
        fputc(' ', out);
        if (!smt2_value_print(out, s->val, host)) return 0;
        fputc(')', out);
    }
    return 1;
}
