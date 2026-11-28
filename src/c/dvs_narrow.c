/**
 * dvs_narrow.c -- rewrite expressions over 64 bits whose variables are 64
 * bits or less into expressions of 64 bits or less (dvs_narrow.h). Needs only
 * libc. Not a translation unit of its own: dvs_image.c includes it, so it is
 * in the writer (dvs_problem_validate) and the runtime (dvs_handle.c).
 *
 * A value in a context of W bits (W over 64) is held as segments, from bit 0
 * up, each of at most 64 bits: a constant; bits [b, b + w) of a narrow node;
 * or w copies of bit b of a narrow node (a sign extension). The SV rules of
 * dvs_sv.h decide each node's context: a comparison's is the wider side's
 * type, and it reaches the operands of the context-determined operators
 * (+ - * / % & | ^, unary - ~, the arms of ?:, the left operand of a shift);
 * every other node is self-determined, and extended to the context by its own
 * signedness. A narrow result is made of the segments: a slice is the
 * concatenation of the segment bits it covers; + - * are done at the width
 * their operands' ranges need (when their values are narrow: a narrow node
 * zero- or sign-extended, or a constant); a comparison is done at 64 bits or
 * less when both values are narrow, else slice by slice from the top.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvs_narrow.h"

#define NW_HDR       ((uint32_t)sizeof(dvs_pool_t))
#define NW_MAX_W     4096u   /* the widest context taken apart            */
#define NW_MAX_DEPTH 512     /* recursion: a deeper root keeps its form     */
#define NW_PIECE     32u     /* a slice-by-slice comparison's slice width   */
#define SLOT(r)      (((r) - NW_HDR) / DVS_IMAGE_ALIGN)

typedef struct { uint32_t w; uint8_t s; } Ty;

enum { SG_CONST, SG_EXPR, SG_SIGN };
typedef struct {
    uint8_t    k;   /* SG_*                                               */
    uint32_t   w;   /* 1..64 bits                                         */
    uint64_t   c;   /* SG_CONST: the bits                                 */
    dvs_expr_t n;   /* SG_EXPR: bits [b, b + w) of n; SG_SIGN: bit b of n */
    uint32_t   b;
} Seg;

typedef struct { Seg *s; uint32_t n, cap, W; } Segs;

/* A narrow value: ext_s(n) (or the constant cv) of w bits. */
typedef struct {
    int        is_const;
    uint64_t   cv;          /* the constant's w-bit pattern */
    uint32_t   w;
    uint8_t    s;
    dvs_expr_t n;
} Int;

/* A Boolean, folded while it is constant. */
typedef struct { int k; dvs_expr_t n; } Bo;   /* k: 0 false, 1 true, 2 n */

typedef struct ArBlk { struct ArBlk *next; size_t used, cap; } ArBlk;

typedef struct {
    dvs_problem_t *p;
    size_t         size;
    uint32_t       n_slots;          /* slots the tables cover            */
    uint32_t      *tw;               /* per slot: type width (0: none)    */
    uint8_t       *ts;               /* signedness                        */
    uint8_t       *wd;               /* 1: reads a node over 64 bits      */
    uint8_t       *mark;             /* reachable from a root             */
    uint32_t       nv, na;
    uint32_t      *vw, *aw;
    uint8_t       *vs, *as, *vk, *ak;
    ArBlk         *ar;
    int            err, oom, depth;
    uint32_t       fail_w;           /* the context width of the failure  */
} Nw;

/* ---- arena for segment lists, dropped after each root --------------- */

static void *ar_alloc(Nw *N, size_t bytes) {
    bytes = (bytes + 15u) & ~(size_t)15u;
    if (!N->ar || N->ar->used + bytes > N->ar->cap) {
        size_t cap = bytes > 65536 ? bytes : 65536;
        ArBlk *b = malloc(sizeof(ArBlk) + cap);
        if (!b) { N->err = N->oom = 1; return NULL; }
        b->next = N->ar; b->used = 0; b->cap = cap;
        N->ar = b;
    }
    void *r = (char *)(N->ar + 1) + N->ar->used;
    N->ar->used += bytes;
    return r;
}

static void ar_free(Nw *N) {
    while (N->ar) { ArBlk *n = N->ar->next; free(N->ar); N->ar = n; }
}

static int fail(Nw *N, uint32_t w) {
    if (!N->err && w > N->fail_w) N->fail_w = w;
    N->err = 1;
    return -1;
}

/* ---- nodes ------------------------------------------------------------ */

static void *P(Nw *N, dvs_expr_t r) { return POOL_PTR(N->p, r); }
static uint32_t kind_of(Nw *N, dvs_expr_t r) { return *(const uint32_t *)P(N, r); }

static int tables_cover(Nw *N, uint32_t slots) {
    if (slots <= N->n_slots) return 0;
    uint32_t n = N->n_slots ? N->n_slots : 1024;
    while (n < slots) n *= 2;
    uint32_t *tw = realloc(N->tw, n * sizeof *tw);
    if (tw) N->tw = tw;
    uint8_t *ts = realloc(N->ts, n), *wd = ts ? realloc(N->wd, n) : NULL;
    if (ts) N->ts = ts;
    if (wd) N->wd = wd;
    uint8_t *mk = wd ? realloc(N->mark, n) : NULL;
    if (mk) N->mark = mk;
    if (!tw || !ts || !wd || !mk) { N->err = N->oom = 1; return -1; }
    memset(N->tw + N->n_slots, 0, (n - N->n_slots) * sizeof *N->tw);
    memset(N->ts + N->n_slots, 0, n - N->n_slots);
    memset(N->wd + N->n_slots, 0, n - N->n_slots);
    memset(N->mark + N->n_slots, 0, n - N->n_slots);
    N->n_slots = n;
    return 0;
}

static dvs_expr_t nw_alloc(Nw *N, uint32_t bytes, uint32_t w, uint8_t s) {
    if (N->err) return EXPR_NULL;
    if (w == 0 || w > 64) { fail(N, w); return EXPR_NULL; }
    dvs_pool_t *pl = &N->p->pool;
    uint32_t base = (pl->used + (DVS_IMAGE_ALIGN - 1)) & ~(DVS_IMAGE_ALIGN - 1);
    if ((uint64_t)base + bytes > pl->capacity) {
        uint64_t cap = (uint64_t)pl->capacity * 2 + bytes + 4096;
        if (cap > 0xF0000000u) { N->err = N->oom = 1; return EXPR_NULL; }
        dvs_problem_t *q = realloc(N->p, sizeof(dvs_problem_t) + (size_t)cap);
        if (!q) { N->err = N->oom = 1; return EXPR_NULL; }
        N->p = q;
        q->pool.capacity = (uint32_t)cap;
        N->size = sizeof(dvs_problem_t) + (size_t)cap;
        pl = &q->pool;
    }
    pl->used = base + bytes;
    dvs_expr_t r = NW_HDR + base;
    if (tables_cover(N, SLOT(r) + 1)) return EXPR_NULL;
    memset(P(N, r), 0, bytes);
    N->tw[SLOT(r)] = w;
    N->ts[SLOT(r)] = s;
    N->wd[SLOT(r)] = 0;
    return r;
}

static Ty ty(Nw *N, dvs_expr_t r) {
    Ty t = { 1, 0 };
    if (r == EXPR_NULL || SLOT(r) >= N->n_slots || !N->tw[SLOT(r)]) {
        N->err = 1;
        return t;
    }
    t.w = N->tw[SLOT(r)];
    t.s = N->ts[SLOT(r)];
    return t;
}

static int wide(Nw *N, dvs_expr_t r) {
    return r != EXPR_NULL && SLOT(r) < N->n_slots && N->wd[SLOT(r)];
}

static dvs_expr_t mk_const(Nw *N, uint64_t v, uint32_t w, uint8_t s) {
    dvs_expr_t r = nw_alloc(N, sizeof(ExprConst), w, s);
    if (r == EXPR_NULL) return r;
    ExprConst *c = P(N, r);
    c->kind = EXPR_CONST;
    c->is_signed = s;
    c->width = w;
    c->value = dvs_const_wrap((int64_t)v, w, s);
    return r;
}

static int is_cmp(uint32_t op) { return op >= DVS_BIN_EQ && op <= DVS_BIN_GTE; }
static int is_arith(uint32_t op) { return op <= DVS_BIN_BXOR; }
static int is_shift(uint32_t op) {
    return op == DVS_BIN_LSHIFT || op == DVS_BIN_RSHIFT || op == DVS_BIN_ASHR;
}

static dvs_expr_t mk_bin(Nw *N, uint32_t op, dvs_expr_t l, dvs_expr_t r) {
    if (l == EXPR_NULL || r == EXPR_NULL) { N->err = 1; return EXPR_NULL; }
    Ty a = ty(N, l), b = ty(N, r), t = { 1, 0 };
    if (is_arith(op)) { t.w = a.w > b.w ? a.w : b.w; t.s = a.s && b.s; }
    else if (is_shift(op)) t = a;
    dvs_expr_t x = nw_alloc(N, sizeof(ExprBinary), t.w, t.s);
    if (x == EXPR_NULL) return x;
    ExprBinary *n = P(N, x);
    n->kind = EXPR_BINARY; n->op = op; n->lhs = l; n->rhs = r;
    return x;
}

static dvs_expr_t mk_un(Nw *N, uint32_t op, dvs_expr_t a) {
    if (a == EXPR_NULL) { N->err = 1; return EXPR_NULL; }
    Ty t = op == DVS_UN_NOT ? (Ty){ 1, 0 } : ty(N, a);
    dvs_expr_t x = nw_alloc(N, sizeof(ExprUnary), t.w, t.s);
    if (x == EXPR_NULL) return x;
    ExprUnary *n = P(N, x);
    n->kind = EXPR_UNARY; n->op = op; n->operand = a;
    return x;
}

static dvs_expr_t mk_ite(Nw *N, dvs_expr_t c, dvs_expr_t a, dvs_expr_t b) {
    if (c == EXPR_NULL || a == EXPR_NULL || b == EXPR_NULL) { N->err = 1; return EXPR_NULL; }
    Ty ta = ty(N, a), tb = ty(N, b);
    dvs_expr_t x = nw_alloc(N, sizeof(ExprITE), ta.w > tb.w ? ta.w : tb.w, ta.s && tb.s);
    if (x == EXPR_NULL) return x;
    ExprITE *n = P(N, x);
    n->kind = EXPR_ITE; n->cond = c; n->then_e = a; n->else_e = b;
    return x;
}

/* The low `from` bits of `a`, extended to `to` (by sign when `sx`), read
 * signed when `ds`. `a` itself when that is its type already. */
static dvs_expr_t mk_cast(Nw *N, dvs_expr_t a, uint32_t from, uint32_t to, uint8_t sx,
                          uint8_t ds) {
    if (a == EXPR_NULL) { N->err = 1; return EXPR_NULL; }
    Ty t = ty(N, a);
    if (t.w == from && from == to && t.s == ds) return a;
    dvs_expr_t x = nw_alloc(N, sizeof(ExprSvCast), to, ds);
    if (x == EXPR_NULL) return x;
    ExprSvCast *n = P(N, x);
    n->kind = EXPR_SV_CAST; n->sign_extend = sx; n->dst_signed = ds;
    n->from_bits = from; n->to_bits = to; n->operand = a;
    return x;
}

static dvs_expr_t mk_extract(Nw *N, dvs_expr_t a, uint32_t hi, uint32_t lo) {
    if (a == EXPR_NULL) { N->err = 1; return EXPR_NULL; }
    Ty t = ty(N, a);
    if (lo == 0 && hi + 1 == t.w && !t.s) return a;
    dvs_expr_t x = nw_alloc(N, sizeof(ExprExtract), hi - lo + 1, 0);
    if (x == EXPR_NULL) return x;
    ExprExtract *n = P(N, x);
    n->kind = EXPR_EXTRACT; n->hi_bit = hi; n->lo_bit = lo; n->operand = a;
    return x;
}

static dvs_expr_t mk_concat(Nw *N, dvs_expr_t hi, dvs_expr_t lo, uint32_t lo_w) {
    if (hi == EXPR_NULL || lo == EXPR_NULL) { N->err = 1; return EXPR_NULL; }
    dvs_expr_t x = nw_alloc(N, sizeof(ExprConcat), ty(N, hi).w + lo_w, 0);
    if (x == EXPR_NULL) return x;
    ExprConcat *n = P(N, x);
    n->kind = EXPR_CONCAT; n->lo_width = lo_w; n->hi = hi; n->lo = lo;
    return x;
}

/* ---- Booleans --------------------------------------------------------- */

static Bo bo_c(int v) { Bo b = { v ? 1 : 0, EXPR_NULL }; return b; }
static Bo bo_n(dvs_expr_t n) { Bo b = { 2, n }; return b; }

static Bo bo_and(Nw *N, Bo a, Bo b) {
    if (a.k == 0 || b.k == 0) return bo_c(0);
    if (a.k == 1) return b;
    if (b.k == 1) return a;
    return bo_n(mk_bin(N, DVS_BIN_AND, a.n, b.n));
}

static Bo bo_or(Nw *N, Bo a, Bo b) {
    if (a.k == 1 || b.k == 1) return bo_c(1);
    if (a.k == 0) return b;
    if (b.k == 0) return a;
    return bo_n(mk_bin(N, DVS_BIN_OR, a.n, b.n));
}

static dvs_expr_t bo_node(Nw *N, Bo b) {
    return b.k == 2 ? b.n : mk_const(N, (uint64_t)b.k, 1, 0);
}

/* ---- segments --------------------------------------------------------- */

static uint64_t mask(uint32_t w) { return w >= 64 ? ~0ull : (1ull << w) - 1; }

static void sg_push(Nw *N, Segs *A, Seg g) {
    if (N->err || g.w == 0) return;
    if (A->n) {
        Seg *t = &A->s[A->n - 1];
        if (t->w + g.w <= 64 && t->k == g.k) {
            if (g.k == SG_CONST) {
                t->c |= g.c << t->w;
                t->w += g.w;
                A->W += g.w;
                return;
            }
            if (g.n == t->n && ((g.k == SG_EXPR && g.b == t->b + t->w)
                                || (g.k == SG_SIGN && g.b == t->b))) {
                t->w += g.w;
                A->W += g.w;
                return;
            }
        }
    }
    if (A->n == A->cap) {
        uint32_t cap = A->cap ? 2 * A->cap : 8;
        Seg *s = ar_alloc(N, cap * sizeof *s);
        if (!s) return;
        if (A->n) memcpy(s, A->s, A->n * sizeof *s);
        A->s = s;
        A->cap = cap;
    }
    A->s[A->n++] = g;
    A->W += g.w;
}

static void sg_const(Nw *N, Segs *A, uint64_t c, uint32_t w) {
    Seg g = { SG_CONST, w, c & mask(w), EXPR_NULL, 0 };
    sg_push(N, A, g);
}

/* `w` bits all `one`, or all copies of bit b of n (n != EXPR_NULL). */
static void sg_fill(Nw *N, Segs *A, uint32_t w, int one, dvs_expr_t n, uint32_t b) {
    while (w && !N->err) {
        uint32_t k = w > 64 ? 64 : w;
        if (n != EXPR_NULL) {
            Seg g = { SG_SIGN, k, 0, n, b };
            sg_push(N, A, g);
        } else {
            sg_const(N, A, one ? ~0ull : 0, k);
        }
        w -= k;
    }
}

static void sg_node(Nw *N, Segs *A, dvs_expr_t n, uint32_t w) {
    Seg g = { SG_EXPR, w, 0, n, 0 };
    sg_push(N, A, g);
}

/* Bits [lo, lo + w) of A, appended to `out`. */
static void sg_slice(Nw *N, const Segs *A, uint32_t lo, uint32_t w, Segs *out) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < A->n && w && !N->err; i++) {
        const Seg *g = &A->s[i];
        if (at + g->w <= lo) { at += g->w; continue; }
        uint32_t off = lo > at ? lo - at : 0;
        uint32_t k = g->w - off < w ? g->w - off : w;
        Seg h = *g;
        h.w = k;
        if (g->k == SG_CONST) h.c = (g->c >> off) & mask(k);
        else if (g->k == SG_EXPR) h.b = g->b + off;
        sg_push(N, out, h);
        lo += k;
        w -= k;
        at += g->w;
    }
    if (w && !N->err) fail(N, A->W);
}

/* A truncated or extended to W bits (by sign when `sx`). */
static void sg_ext(Nw *N, Segs *A, uint32_t W, int sx) {
    if (N->err) return;
    if (A->W > W) {
        Segs T = { 0 };
        sg_slice(N, A, 0, W, &T);
        *A = T;
        return;
    }
    if (A->W == W) return;
    if (!sx || !A->n) { sg_fill(N, A, W - A->W, 0, EXPR_NULL, 0); return; }
    Seg t = A->s[A->n - 1];
    if (t.k == SG_CONST) sg_fill(N, A, W - A->W, (int)((t.c >> (t.w - 1)) & 1), EXPR_NULL, 0);
    else sg_fill(N, A, W - A->W, 0, t.n, t.k == SG_EXPR ? t.b + t.w - 1 : t.b);
}

/* The node of one segment, unsigned, of its width. */
static dvs_expr_t sg_piece(Nw *N, const Seg *g) {
    if (g->k == SG_CONST) return mk_const(N, g->c, g->w, 0);
    if (g->k == SG_EXPR) return mk_extract(N, g->n, g->b + g->w - 1, g->b);
    dvs_expr_t e = mk_extract(N, g->n, g->b, g->b);
    return g->w == 1 ? e : mk_cast(N, e, 1, g->w, 1, 0);
}

/* Bits [lo, lo + w) of A (w <= 64) as an unsigned node. */
static dvs_expr_t sg_mat(Nw *N, const Segs *A, uint32_t lo, uint32_t w) {
    Segs T = { 0 };
    sg_slice(N, A, lo, w, &T);
    dvs_expr_t acc = EXPR_NULL;
    uint32_t accw = 0;
    for (uint32_t i = 0; i < T.n && !N->err; i++) {
        dvs_expr_t p = sg_piece(N, &T.s[i]);
        acc = acc == EXPR_NULL ? p : mk_concat(N, p, acc, accw);
        accw += T.s[i].w;
    }
    if (acc == EXPR_NULL) N->err = 1;
    return N->err ? EXPR_NULL : acc;
}

/* Cut A at every boundary of B and at multiples of `step` (0: none): the
 * pieces of the result line up with those of B cut the same way. */
static void sg_cut(Nw *N, const Segs *A, const Segs *B, uint32_t step, Segs *out) {
    uint32_t i = 0, j = 0, ea = 0, eb = 0, at = 0;
    while (at < A->W && !N->err) {
        while (i < A->n && ea <= at) ea += A->s[i++].w;
        while (B && j < B->n && eb <= at) eb += B->s[j++].w;
        uint32_t end = ea;
        if (B && eb < end) end = eb;
        if (step && (at / step + 1) * step < end) end = (at / step + 1) * step;
        Segs T = { 0 };
        sg_slice(N, A, at, end - at, &T);
        /* One segment, not merged with its neighbour. */
        if (T.n != 1) { fail(N, A->W); return; }
        if (out->n == out->cap) {
            uint32_t cap = out->cap ? 2 * out->cap : 8;
            Seg *s = ar_alloc(N, cap * sizeof *s);
            if (!s) return;
            if (out->n) memcpy(s, out->s, out->n * sizeof *s);
            out->s = s;
            out->cap = cap;
        }
        out->s[out->n++] = T.s[0];
        out->W += T.s[0].w;
        at = end;
    }
}

/* ---- narrow values ------------------------------------------------------ */

static uint32_t bitlen(uint64_t v) { uint32_t n = 0; while (v) { n++; v >>= 1; } return n; }

/* A's W-bit pattern as a narrow value: ext_s(n) (sign-extended when s), or a
 * constant. 0 when it is none. */
static int as_int(Nw *N, const Segs *A, Int *o) {
    memset(o, 0, sizeof *o);
    if (N->err || !A->n) return 0;
    int all_const = 1;
    for (uint32_t i = 0; i < A->n; i++) all_const &= A->s[i].k == SG_CONST;
    if (all_const) {
        /* Bits 63 and up all 0: an unsigned value; all 1: a negative int64. */
        int b63 = 0, hi0 = 1, hi1 = 1;
        uint64_t v = 0;
        uint32_t at = 0;
        for (uint32_t i = 0; i < A->n; i++) {
            const Seg *g = &A->s[i];
            for (uint32_t k = 0; k < g->w; k++, at++) {
                int bit = (int)((g->c >> k) & 1);
                if (at < 64) v |= (uint64_t)bit << at;
                if (at == 63) b63 = bit;
                if (at >= 64) { hi0 &= !bit; hi1 &= bit; }
            }
        }
        o->is_const = 1;
        if (A->W <= 64) { hi0 = 1; hi1 = 0; b63 = 0; }
        if (hi0 && !b63) {
            o->cv = v;
            o->w = A->W < 64 ? A->W : 64;
            uint32_t bl = bitlen(v);
            o->w = bl ? bl : 1;
            o->s = 0;
            return 1;
        }
        if (hi1 && b63) {
            /* Negative: the fewest bits that hold it signed. */
            uint32_t w = 64;
            while (w > 1 && (((v >> (w - 2)) & 1) == 1)) w--;
            o->cv = v & mask(w);
            o->w = w;
            o->s = 1;
            return 1;
        }
        return 0;
    }
    /* Strip the zero (or sign) segments at the top. */
    uint32_t top = A->n, L = A->W;
    int sx = 0;
    const Seg *t = &A->s[top - 1];
    if (t->k == SG_SIGN) {
        dvs_expr_t n = t->n;
        uint32_t b = t->b;
        while (top && A->s[top - 1].k == SG_SIGN && A->s[top - 1].n == n && A->s[top - 1].b == b)
            L -= A->s[--top].w;
        if (!top) return 0;
        const Seg *u = &A->s[top - 1];
        if (u->k != SG_EXPR || u->n != n || u->b + u->w - 1 != b) return 0;
        sx = 1;
    } else {
        while (top && A->s[top - 1].k == SG_CONST && A->s[top - 1].c == 0)
            L -= A->s[--top].w;
        if (top && A->s[top - 1].k == SG_CONST) L -= A->s[top - 1].w - bitlen(A->s[top - 1].c);
        if (L == 0) { o->is_const = 1; o->w = 1; return 1; }
    }
    if (L > 64) return 0;
    o->w = L;
    o->s = (uint8_t)sx;
    /* One segment that is a whole node of this type: the node. */
    if (top == 1 && A->s[0].k == SG_EXPR && A->s[0].b == 0 && A->s[0].w == L) {
        Ty nt = ty(N, A->s[0].n);
        if (nt.w == L && nt.s == sx) { o->n = A->s[0].n; return !N->err; }
    }
    o->n = sg_mat(N, A, 0, L);
    if (sx) o->n = mk_cast(N, o->n, L, L, 0, 1);
    return !N->err;
}

static uint32_t sw_of(const Int *a) { return a->s ? a->w : a->w + 1; }

/* `a` at (w, s), the type it is used at. */
static dvs_expr_t int_at(Nw *N, const Int *a, uint32_t w, uint8_t s) {
    if (a->is_const) {
        uint64_t v = a->cv;
        if (a->s && a->w < 64 && ((v >> (a->w - 1)) & 1)) v |= ~mask(a->w);
        return mk_const(N, v, w, s);
    }
    return mk_cast(N, a->n, a->w, w, a->s, s);
}

/* ---- the rewrite ---------------------------------------------------------- */

static dvs_expr_t rw(Nw *N, dvs_expr_t r);
static dvs_expr_t truth(Nw *N, dvs_expr_t r);
static void seg_of(Nw *N, dvs_expr_t r, uint32_t W, uint8_t S, Segs *out);
static void selfseg(Nw *N, dvs_expr_t r, Segs *out);

static int enter(Nw *N) {
    if (N->err) return -1;
    if (++N->depth > NW_MAX_DEPTH) { N->depth--; N->err = 1; return -1; }
    return 0;
}
static void leave(Nw *N) { N->depth--; }

/* `+ - *` (or unary - when B is NULL) of narrow values in a W-bit context,
 * at the width their ranges need. */
static void seg_arith(Nw *N, uint32_t op, const Segs *A, const Segs *B, uint32_t W, Segs *out) {
    Int a, b;
    if (!as_int(N, A, &a)) { fail(N, W); return; }
    if (B) {
        if (!as_int(N, B, &b)) { fail(N, W); return; }
    } else {
        b = a;
        memset(&a, 0, sizeof a);
        a.is_const = 1;
        a.w = 1;
        op = DVS_BIN_SUB;
    }
    uint32_t r, wa = sw_of(&a), wb = sw_of(&b);
    uint8_t rs;
    switch (op) {
    case DVS_BIN_ADD:
        if (!a.s && !b.s) { r = (a.w > b.w ? a.w : b.w) + 1; rs = 0; }
        else { r = (wa > wb ? wa : wb) + 1; rs = 1; }
        break;
    case DVS_BIN_SUB: r = (wa > wb ? wa : wb) + 1; rs = 1; break;
    case DVS_BIN_MUL:
        if (!a.s && !b.s) { r = a.w + b.w; rs = 0; }
        else { r = wa + wb; rs = 1; }
        break;
    default: fail(N, W); return;
    }
    /* An unsigned 64-bit value can pass 2^63, which the engines' int64
     * storage reads as negative: stop at 63 bits unsigned. */
    if (r > (rs ? 64u : 63u) || r > W) { fail(N, W); return; }
    if (a.is_const && b.is_const) {
        /* Exact in 64 bits: the result fits r <= 64 bits. */
        uint64_t va = a.cv, vb = b.cv;
        if (a.s && a.w < 64 && ((va >> (a.w - 1)) & 1)) va |= ~mask(a.w);
        if (b.s && b.w < 64 && ((vb >> (b.w - 1)) & 1)) vb |= ~mask(b.w);
        uint64_t v = op == DVS_BIN_ADD ? va + vb : op == DVS_BIN_SUB ? va - vb : va * vb;
        sg_const(N, out, v, r);
        sg_ext(N, out, W, rs);
        return;
    }
    dvs_expr_t x = mk_bin(N, op, int_at(N, &a, r, rs), int_at(N, &b, r, rs));
    sg_node(N, out, x, r);
    sg_ext(N, out, W, rs);
}

/* `& | ^` piece by piece. */
static void seg_bitwise(Nw *N, uint32_t op, const Segs *A, const Segs *B, Segs *out) {
    Segs CA = { 0 }, CB = { 0 };
    sg_cut(N, A, B, 0, &CA);
    sg_cut(N, B, &CA, 0, &CB);
    if (!N->err && CA.n != CB.n) fail(N, A->W);
    for (uint32_t i = 0; i < CA.n && !N->err; i++) {
        const Seg *a = &CA.s[i], *b = &CB.s[i];
        const uint32_t k = a->w;
        const uint64_t m = mask(k);
        if (a->k == SG_CONST && b->k == SG_CONST) {
            uint64_t v = op == DVS_BIN_BAND ? a->c & b->c : op == DVS_BIN_BOR ? a->c | b->c
                                                                          : a->c ^ b->c;
            sg_const(N, out, v, k);
            continue;
        }
        const Seg *c = a->k == SG_CONST ? a : b->k == SG_CONST ? b : NULL;
        const Seg *o = c == a ? b : a;
        if (c && op == DVS_BIN_BAND && c->c == 0) { sg_const(N, out, 0, k); continue; }
        if (c && op == DVS_BIN_BOR && c->c == m) { sg_const(N, out, m, k); continue; }
        if (c && ((op == DVS_BIN_BAND && c->c == m) || (op != DVS_BIN_BAND && c->c == 0))) {
            sg_push(N, out, *o);
            continue;
        }
        sg_node(N, out, mk_bin(N, op, sg_piece(N, a), sg_piece(N, b)), k);
    }
}

/* The shift amount of `r` when it is a constant (UINT32_MAX: at least 2^32). */
static int shift_amount(Nw *N, dvs_expr_t r, uint32_t *k) {
    uint32_t kd = kind_of(N, r);
    if (kd == EXPR_CONST) {
        Ty t = ty(N, r);
        uint64_t v = (uint64_t)((const ExprConst *)P(N, r))->value & mask(t.w);
        *k = v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
        return 1;
    }
    if (kd == EXPR_CONST_WIDE) {
        const ExprConstWide *c = P(N, r);
        for (uint32_t i = 1; i < c->n_limbs; i++)
            if (c->limbs[i]) { *k = UINT32_MAX; return 1; }
        *k = c->limbs[0] > UINT32_MAX ? UINT32_MAX : (uint32_t)c->limbs[0];
        return 1;
    }
    return 0;
}

/* `r`, a context-determined node or a self-determined one, in the context
 * (W, S). */
static void seg_of(Nw *N, dvs_expr_t r, uint32_t W, uint8_t S, Segs *out) {
    if (enter(N)) return;
    if (W > NW_MAX_W) { fail(N, W); leave(N); return; }
    uint32_t kd = kind_of(N, r);
    if (kd == EXPR_BINARY) {
        ExprBinary b = *(const ExprBinary *)P(N, r);
        if (b.op == DVS_BIN_ADD || b.op == DVS_BIN_SUB || b.op == DVS_BIN_MUL) {
            Segs A = { 0 }, B = { 0 };
            seg_of(N, b.lhs, W, S, &A);
            seg_of(N, b.rhs, W, S, &B);
            if (!N->err) seg_arith(N, b.op, &A, &B, W, out);
            leave(N);
            return;
        }
        if (b.op == DVS_BIN_BAND || b.op == DVS_BIN_BOR || b.op == DVS_BIN_BXOR) {
            Segs A = { 0 }, B = { 0 };
            seg_of(N, b.lhs, W, S, &A);
            seg_of(N, b.rhs, W, S, &B);
            if (!N->err) seg_bitwise(N, b.op, &A, &B, out);
            leave(N);
            return;
        }
        if (b.op == DVS_BIN_DIV || b.op == DVS_BIN_MOD) { fail(N, W); leave(N); return; }
        if (is_shift(b.op)) {
            /* >> of a signed value: SV elaboration makes it logical, but an
             * EXPLICIT problem's engines read it as they please; left as is. */
            uint32_t k;
            if ((b.op == DVS_BIN_RSHIFT && S) || !shift_amount(N, b.rhs, &k)) {
                fail(N, W);
                leave(N);
                return;
            }
            Segs A = { 0 };
            seg_of(N, b.lhs, W, S, &A);
            if (N->err) { leave(N); return; }
            if (k > W) k = W;
            if (b.op == DVS_BIN_LSHIFT) {
                sg_fill(N, out, k, 0, EXPR_NULL, 0);
                if (k < W) sg_slice(N, &A, 0, W - k, out);
            } else {
                if (k < W) sg_slice(N, &A, k, W - k, out);
                /* >>> of a signed context is arithmetic; >> logical. */
                if (b.op == DVS_BIN_ASHR && S) {
                    if (k == W) { sg_slice(N, &A, W - 1, 1, out); k--; }
                    sg_ext(N, out, W, 1);
                } else {
                    sg_ext(N, out, W, 0);
                }
            }
            leave(N);
            return;
        }
    } else if (kd == EXPR_UNARY) {
        ExprUnary u = *(const ExprUnary *)P(N, r);
        if (u.op == DVS_UN_NEG) {
            Segs A = { 0 };
            seg_of(N, u.operand, W, S, &A);
            if (!N->err) seg_arith(N, DVS_BIN_SUB, &A, NULL, W, out);
            leave(N);
            return;
        }
        if (u.op == DVS_UN_INVERT) {
            Segs A = { 0 };
            seg_of(N, u.operand, W, S, &A);
            for (uint32_t i = 0; i < A.n && !N->err; i++) {
                const Seg *g = &A.s[i];
                if (g->k == SG_CONST) sg_const(N, out, ~g->c, g->w);
                else sg_node(N, out, mk_un(N, DVS_UN_INVERT, sg_piece(N, g)), g->w);
            }
            leave(N);
            return;
        }
    } else if (kd == EXPR_ITE) {
        ExprITE it = *(const ExprITE *)P(N, r);
        dvs_expr_t c = truth(N, it.cond);
        Segs A = { 0 }, B = { 0 }, CA = { 0 }, CB = { 0 };
        seg_of(N, it.then_e, W, S, &A);
        seg_of(N, it.else_e, W, S, &B);
        sg_cut(N, &A, &B, 0, &CA);
        sg_cut(N, &B, &CA, 0, &CB);
        if (!N->err && CA.n != CB.n) fail(N, W);
        for (uint32_t i = 0; i < CA.n && !N->err; i++) {
            const Seg *a = &CA.s[i], *bb = &CB.s[i];
            if (a->k == SG_CONST && bb->k == SG_CONST && a->c == bb->c) sg_push(N, out, *a);
            else sg_node(N, out, mk_ite(N, c, sg_piece(N, a), sg_piece(N, bb)), a->w);
        }
        leave(N);
        return;
    }
    /* Self-determined: its own value, extended by its own signedness. */
    Ty t = ty(N, r);
    selfseg(N, r, out);
    sg_ext(N, out, W, t.s);
    leave(N);
}

/* `r` at its own type. */
static void selfseg(Nw *N, dvs_expr_t r, Segs *out) {
    if (enter(N)) return;
    Ty t = ty(N, r);
    if (t.w <= 64 && kind_of(N, r) == EXPR_CONST) {
        sg_const(N, out, (uint64_t)((const ExprConst *)P(N, r))->value, t.w);
        leave(N);
        return;
    }
    if (t.w <= 64) {
        dvs_expr_t n = wide(N, r) ? rw(N, r) : r;
        if (!N->err) sg_node(N, out, n, t.w);
        leave(N);
        return;
    }
    if (t.w > NW_MAX_W) { fail(N, t.w); leave(N); return; }
    switch (kind_of(N, r)) {
    case EXPR_CONST_WIDE: {
        const ExprConstWide *c = P(N, r);
        uint32_t w = c->width;
        for (uint32_t i = 0; i < c->n_limbs && !N->err; i++) {
            const ExprConstWide *cc = P(N, r);
            uint32_t k = w - 64 * i < 64 ? w - 64 * i : 64;
            sg_const(N, out, cc->limbs[i], k);
        }
        break;
    }
    case EXPR_EXTEND: {
        ExprExtend x = *(const ExprExtend *)P(N, r);
        Ty ot = ty(N, x.operand);
        selfseg(N, x.operand, out);
        sg_ext(N, out, x.from_bits, ot.s);
        sg_ext(N, out, x.to_bits, x.sign_extend);
        break;
    }
    case EXPR_SV_CAST: {
        ExprSvCast x = *(const ExprSvCast *)P(N, r);
        Ty ot = ty(N, x.operand);
        selfseg(N, x.operand, out);
        sg_ext(N, out, x.from_bits, ot.s);
        sg_ext(N, out, x.to_bits, x.sign_extend);
        break;
    }
    case EXPR_CAST: {
        /* The operand at the wider of its width and the cast's. */
        ExprCast x = *(const ExprCast *)P(N, r);
        Ty ot = ty(N, x.operand);
        uint32_t ow = ot.w > x.to_bits ? ot.w : x.to_bits;
        if (ow <= 64) {
            /* A narrow cast is a node of its own (only its type is wide: no). */
            fail(N, t.w);
            break;
        }
        seg_of(N, x.operand, ow, ot.s, out);
        sg_ext(N, out, x.to_bits, 0);
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract x = *(const ExprExtract *)P(N, r);
        Segs A = { 0 };
        selfseg(N, x.operand, &A);
        if (!N->err && x.hi_bit >= A.W) { fail(N, t.w); break; }
        sg_slice(N, &A, x.lo_bit, x.hi_bit - x.lo_bit + 1, out);
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat x = *(const ExprConcat *)P(N, r);
        Ty lt = ty(N, x.lo);
        Segs H = { 0 };
        selfseg(N, x.lo, out);
        sg_ext(N, out, x.lo_width, lt.s);
        selfseg(N, x.hi, &H);
        for (uint32_t i = 0; i < H.n && !N->err; i++) sg_push(N, out, H.s[i]);
        break;
    }
    case EXPR_BINARY: case EXPR_UNARY: case EXPR_ITE:
        seg_of(N, r, t.w, t.s, out);
        break;
    default:
        fail(N, t.w);
        break;
    }
    leave(N);
}

/* One comparison of two equal-width pieces (unsigned; signed when `sg`). */
static Bo cmp_piece(Nw *N, uint32_t op, const Seg *a, const Seg *b, int sg) {
    const uint32_t k = a->w;
    if (a->k == SG_CONST && b->k == SG_CONST) {
        uint64_t x = a->c, y = b->c;
        if (sg && k < 64) {
            x ^= 1ull << (k - 1);   /* signed order as unsigned: flip the sign */
            y ^= 1ull << (k - 1);
        } else if (sg) {
            x ^= 1ull << 63;
            y ^= 1ull << 63;
        }
        switch (op) {
        case DVS_BIN_EQ:  return bo_c(x == y);
        case DVS_BIN_NEQ: return bo_c(x != y);
        case DVS_BIN_LT:  return bo_c(x < y);
        case DVS_BIN_LTE: return bo_c(x <= y);
        case DVS_BIN_GT:  return bo_c(x > y);
        default:          return bo_c(x >= y);
        }
    }
    dvs_expr_t pa = sg_piece(N, a), pb = sg_piece(N, b);
    if (sg) {
        pa = mk_cast(N, pa, k, k, 0, 1);
        pb = mk_cast(N, pb, k, k, 0, 1);
    }
    return bo_n(mk_bin(N, op, pa, pb));
}

/* A constant's value, read signed or unsigned: below every int64, above
 * every uint64, or exact (negative: the int64 `u`; else the uint64 `u`). */
typedef struct { int big; int neg; uint64_t u; } Val;   /* big: -1, 0, 1 */

static int all_const(const Segs *A) {
    for (uint32_t i = 0; i < A->n; i++)
        if (A->s[i].k != SG_CONST) return 0;
    return A->n != 0;
}

static Val const_val(const Segs *A, int sgn) {
    Val v = { 0, 0, 0 };
    int b63 = 0, top = 0, hi0 = 1, hi1 = 1;
    uint32_t at = 0;
    for (uint32_t i = 0; i < A->n; i++) {
        const Seg *g = &A->s[i];
        for (uint32_t k = 0; k < g->w; k++, at++) {
            int bit = (int)((g->c >> k) & 1);
            if (at < 64) v.u |= (uint64_t)bit << at;
            if (at == 63) b63 = bit;
            if (at >= 64) { hi0 &= !bit; hi1 &= bit; }
            top = bit;
        }
    }
    if (!sgn) {
        if (!hi0) v.big = 1;
        return v;
    }
    if ((b63 && hi1) || (!b63 && hi0)) { v.neg = b63; return v; }
    v.big = top ? -1 : 1;
    return v;
}

static int val_cmp(Val a, Val b) {
    if (a.big != b.big) return a.big < b.big ? -1 : 1;
    if (a.big) return 0;
    if (a.neg != b.neg) return a.neg ? -1 : 1;
    if (a.neg) return (int64_t)a.u < (int64_t)b.u ? -1 : (int64_t)a.u > (int64_t)b.u;
    return a.u < b.u ? -1 : a.u > b.u;
}

static uint32_t flip_cmp(uint32_t op) {
    switch (op) {
    case DVS_BIN_LT:  return DVS_BIN_GT;
    case DVS_BIN_LTE: return DVS_BIN_GTE;
    case DVS_BIN_GT:  return DVS_BIN_LT;
    case DVS_BIN_GTE: return DVS_BIN_LTE;
    default:          return op;
    }
}

/* `x op c`, x narrow and c a constant: decided by x's range when c lies
 * outside it, else compared at x's own type. 0 when it does not apply. */
static int cmp_const(Nw *N, uint32_t op, const Int *x, const Segs *K, uint8_t S, Bo *out) {
    int eq = op == DVS_BIN_EQ || op == DVS_BIN_NEQ;
    /* A narrow unsigned 64-bit value can pass 2^63 (see seg_arith). */
    if (x->is_const || (!x->s && x->w > 63) || (!eq && !S && x->s)) return 0;
    Val c = const_val(K, eq ? x->s : S);
    Val lo = { 0, 0, 0 }, hi = { 0, 0, mask(x->w) };
    if (x->s) {
        lo.neg = 1;
        lo.u = x->w >= 64 ? 1ull << 63 : (uint64_t)(-(int64_t)(1ull << (x->w - 1)));
        hi.u = x->w >= 64 ? ~0ull >> 1 : (1ull << (x->w - 1)) - 1;
    }
    int below = val_cmp(c, lo) < 0, above = val_cmp(c, hi) > 0;
    int at_lo = val_cmp(c, lo) <= 0, at_hi = val_cmp(c, hi) >= 0;
    int t;
    switch (op) {
    case DVS_BIN_EQ:  t = below || above ? 0 : -1; break;
    case DVS_BIN_NEQ: t = below || above ? 1 : -1; break;
    case DVS_BIN_LT:  t = at_lo ? 0 : above ? 1 : -1; break;
    case DVS_BIN_LTE: t = below ? 0 : at_hi ? 1 : -1; break;
    case DVS_BIN_GT:  t = at_hi ? 0 : below ? 1 : -1; break;
    default:          t = above ? 0 : at_lo ? 1 : -1; break;
    }
    if (t >= 0) { *out = bo_c(t); return 1; }
    *out = bo_n(mk_bin(N, op, x->n, mk_const(N, c.u, x->w, x->s)));
    return !N->err;
}

/* `A op B`, two W-bit values compared signed when S. */
static Bo cmp_segs(Nw *N, uint32_t op, const Segs *A, const Segs *B, uint8_t S) {
    if (N->err) return bo_c(0);
    if (A->W != B->W) { fail(N, A->W); return bo_c(0); }
    Int a, b;
    int eq = op == DVS_BIN_EQ || op == DVS_BIN_NEQ;
    int ka = all_const(A), kb = all_const(B);
    if (ka != kb) {
        /* A narrow value against a constant, which may not be narrow. */
        Int x;
        Bo r;
        if (as_int(N, ka ? B : A, &x) && cmp_const(N, ka ? flip_cmp(op) : op, &x, ka ? A : B, S, &r))
            return r;
    }
    if (as_int(N, A, &a) && as_int(N, B, &b) && (eq || S || (!a.s && !b.s))) {
        /* Two narrow values: their own comparison, at 64 bits or less. An
         * unsigned 64-bit pair is compared slice by slice instead: the
         * engines read a value over 2^63 as negative. */
        uint32_t cw;
        uint8_t cs;
        if (!a.s && !b.s) { cw = a.w > b.w ? a.w : b.w; cs = 0; }
        else { cw = sw_of(&a) > sw_of(&b) ? sw_of(&a) : sw_of(&b); cs = 1; }
        if (cw <= (cs ? 64u : 63u)) {
            if (a.is_const && b.is_const) {
                Seg x = { SG_CONST, cw, 0, EXPR_NULL, 0 }, y = x;
                uint64_t va = a.cv, vb = b.cv;
                if (a.s && a.w < 64 && ((va >> (a.w - 1)) & 1)) va |= ~mask(a.w);
                if (b.s && b.w < 64 && ((vb >> (b.w - 1)) & 1)) vb |= ~mask(b.w);
                x.c = va & mask(cw);
                y.c = vb & mask(cw);
                return cmp_piece(N, op, &x, &y, cs);
            }
            return bo_n(mk_bin(N, op, int_at(N, &a, cw, cs), int_at(N, &b, cw, cs)));
        }
    }
    if (N->err) return bo_c(0);
    /* Slice by slice, the top slice first. */
    Segs CA = { 0 }, CB = { 0 };
    sg_cut(N, A, B, NW_PIECE, &CA);
    sg_cut(N, B, &CA, NW_PIECE, &CB);
    if (!N->err && CA.n != CB.n) fail(N, A->W);
    if (N->err) return bo_c(0);
    const uint32_t m = CA.n;
    if (eq) {
        Bo acc = bo_c(op == DVS_BIN_EQ);
        for (uint32_t i = 0; i < m && !N->err; i++) {
            Bo c = cmp_piece(N, op, &CA.s[i], &CB.s[i], 0);
            acc = op == DVS_BIN_EQ ? bo_and(N, acc, c) : bo_or(N, acc, c);
        }
        return acc;
    }
    /* a > b is b < a. */
    const Segs *L = &CA, *R = &CB;
    if (op == DVS_BIN_GT || op == DVS_BIN_GTE) {
        L = &CB; R = &CA;
        op = op == DVS_BIN_GT ? DVS_BIN_LT : DVS_BIN_LTE;
    }
    Bo acc = cmp_piece(N, op, &L->s[0], &R->s[0], S && m == 1);
    for (uint32_t i = 1; i < m && !N->err; i++) {
        int sg = S && i == m - 1;
        Bo lt = cmp_piece(N, DVS_BIN_LT, &L->s[i], &R->s[i], sg);
        Bo e = cmp_piece(N, DVS_BIN_EQ, &L->s[i], &R->s[i], 0);
        acc = bo_or(N, lt, bo_and(N, e, acc));
    }
    return acc;
}

/* `r`, a Boolean position: nonzero. */
static dvs_expr_t truth(Nw *N, dvs_expr_t r) {
    if (N->err) return EXPR_NULL;
    if (!wide(N, r)) return r;
    Ty t = ty(N, r);
    if (t.w <= 64) return rw(N, r);
    if (enter(N)) return EXPR_NULL;
    Segs A = { 0 }, Z = { 0 };
    selfseg(N, r, &A);
    sg_fill(N, &Z, A.W, 0, EXPR_NULL, 0);
    dvs_expr_t out = bo_node(N, cmp_segs(N, DVS_BIN_NEQ, &A, &Z, 0));
    leave(N);
    return N->err ? EXPR_NULL : out;
}

/* `r` rewritten, of the same type (64 bits or less). */
static dvs_expr_t rw(Nw *N, dvs_expr_t r) {
    if (N->err) return EXPR_NULL;
    if (!wide(N, r)) return r;
    Ty t = ty(N, r);
    if (t.w > 64) { fail(N, t.w); return EXPR_NULL; }
    if (enter(N)) return EXPR_NULL;
    dvs_expr_t out = EXPR_NULL;
    switch (kind_of(N, r)) {
    case EXPR_BINARY: {
        ExprBinary b = *(const ExprBinary *)P(N, r);
        if (b.op == DVS_BIN_AND || b.op == DVS_BIN_OR) {
            dvs_expr_t l = truth(N, b.lhs), rr = truth(N, b.rhs);
            out = mk_bin(N, b.op, l, rr);
        } else if (is_cmp(b.op)) {
            Ty lt = ty(N, b.lhs), rt = ty(N, b.rhs);
            uint32_t W = lt.w > rt.w ? lt.w : rt.w;
            uint8_t S = lt.s && rt.s;
            if (W <= 64) {
                dvs_expr_t l = rw(N, b.lhs), rr = rw(N, b.rhs);
                out = mk_bin(N, b.op, l, rr);
            } else {
                Segs A = { 0 }, B = { 0 };
                seg_of(N, b.lhs, W, S, &A);
                seg_of(N, b.rhs, W, S, &B);
                out = bo_node(N, cmp_segs(N, b.op, &A, &B, S));
            }
        } else if (is_arith(b.op) || is_shift(b.op)) {
            /* Narrow, with a wide node below a self-determined boundary. */
            dvs_expr_t l = rw(N, b.lhs), rr = rw(N, b.rhs);
            out = mk_bin(N, b.op, l, rr);
        } else {
            fail(N, t.w);
        }
        break;
    }
    case EXPR_UNARY: {
        ExprUnary u = *(const ExprUnary *)P(N, r);
        out = mk_un(N, u.op, u.op == DVS_UN_NOT ? truth(N, u.operand) : rw(N, u.operand));
        break;
    }
    case EXPR_ITE: {
        ExprITE it = *(const ExprITE *)P(N, r);
        dvs_expr_t c = truth(N, it.cond), a = rw(N, it.then_e), e = rw(N, it.else_e);
        out = mk_ite(N, c, a, e);
        break;
    }
    case EXPR_EXTEND: {
        ExprExtend x = *(const ExprExtend *)P(N, r);
        if (ty(N, x.operand).w > 64) { fail(N, ty(N, x.operand).w); break; }
        out = mk_cast(N, rw(N, x.operand), x.from_bits, x.to_bits, x.sign_extend,
                      ty(N, x.operand).s);
        break;
    }
    case EXPR_SV_CAST: {
        ExprSvCast x = *(const ExprSvCast *)P(N, r);
        Ty ot = ty(N, x.operand);
        if (ot.w <= 64) {
            out = mk_cast(N, rw(N, x.operand), x.from_bits, x.to_bits, x.sign_extend,
                          x.dst_signed);
            break;
        }
        Segs A = { 0 };
        selfseg(N, x.operand, &A);
        uint32_t k = x.from_bits < x.to_bits ? x.from_bits : x.to_bits;
        dvs_expr_t m = sg_mat(N, &A, 0, k);
        out = mk_cast(N, m, k, x.to_bits, x.sign_extend, x.dst_signed);
        break;
    }
    case EXPR_CAST: {
        ExprCast x = *(const ExprCast *)P(N, r);
        Ty ot = ty(N, x.operand);
        uint32_t ow = ot.w > x.to_bits ? ot.w : x.to_bits;
        if (ow <= 64) {
            /* The operand reads a wide node below a boundary: rebuild. */
            dvs_expr_t o = rw(N, x.operand);
            dvs_expr_t c = nw_alloc(N, sizeof(ExprCast), x.to_bits, x.to_signed);
            if (c != EXPR_NULL) {
                ExprCast *n = P(N, c);
                n->kind = EXPR_CAST; n->to_bits = x.to_bits; n->to_signed = x.to_signed;
                n->operand = o;
            }
            out = o == EXPR_NULL ? EXPR_NULL : c;
            break;
        }
        Segs A = { 0 };
        seg_of(N, x.operand, ow, ot.s, &A);
        dvs_expr_t m = sg_mat(N, &A, 0, x.to_bits);
        out = x.to_signed ? mk_cast(N, m, x.to_bits, x.to_bits, 0, 1) : m;
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract x = *(const ExprExtract *)P(N, r);
        Ty ot = ty(N, x.operand);
        if (ot.w <= 64) {
            out = mk_extract(N, rw(N, x.operand), x.hi_bit, x.lo_bit);
            /* mk_extract returns a whole unsigned operand as it is. */
            break;
        }
        Segs A = { 0 };
        selfseg(N, x.operand, &A);
        if (!N->err && x.hi_bit >= A.W) { fail(N, ot.w); break; }
        out = sg_mat(N, &A, x.lo_bit, x.hi_bit - x.lo_bit + 1);
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat x = *(const ExprConcat *)P(N, r);
        out = mk_concat(N, rw(N, x.hi), rw(N, x.lo), x.lo_width);
        break;
    }
    default:
        fail(N, t.w);
        break;
    }
    leave(N);
    if (!N->err && out != EXPR_NULL) {
        Ty o = ty(N, out);
        if (o.w != t.w || o.s != t.s) fail(N, t.w);
    }
    return N->err ? EXPR_NULL : out;
}

/* ---- types of the problem's own nodes -------------------------------- */

static void const_type(const ExprConst *c, uint32_t *w, uint8_t *s) {
    if (c->width) { *w = c->width; *s = c->is_signed ? 1 : 0; return; }
    int64_t v = c->value;
    if (v >= INT32_MIN && v <= INT32_MAX) { *w = 32; *s = 1; }
    else if (!c->is_signed && v >= 0 && v <= (int64_t)UINT32_MAX) { *w = 32; *s = 0; }
    else if (v < 0 || c->is_signed) { *w = 64; *s = 1; }
    else { *w = 64; *s = 0; }
}

static Ty node_type(Nw *N, dvs_expr_t r) {
    Ty t = { 1, 0 };
    const void *n = P(N, r);
    switch (*(const uint32_t *)n) {
    case EXPR_VAR: {
        uint32_t v = ((const ExprVar *)n)->var_id;
        if (v < N->nv && N->vk[v]) { t.w = N->vw[v]; t.s = N->vs[v]; }
        else N->err = 1;
        break;
    }
    case EXPR_CONST: const_type(n, &t.w, &t.s); break;
    case EXPR_CONST_WIDE:
        t.w = ((const ExprConstWide *)n)->width;
        t.s = ((const ExprConstWide *)n)->is_signed ? 1 : 0;
        break;
    case EXPR_BINARY: {
        const ExprBinary *b = n;
        if (is_arith(b->op)) {
            Ty l = ty(N, b->lhs), rr = ty(N, b->rhs);
            t.w = l.w > rr.w ? l.w : rr.w;
            t.s = l.s && rr.s;
        } else if (is_shift(b->op)) {
            t = ty(N, b->lhs);
        }
        break;
    }
    case EXPR_UNARY: {
        const ExprUnary *u = n;
        if (u->op != DVS_UN_NOT) t = ty(N, u->operand);
        break;
    }
    case EXPR_ITE: {
        const ExprITE *i = n;
        Ty a = ty(N, i->then_e), b = ty(N, i->else_e);
        t.w = a.w > b.w ? a.w : b.w;
        t.s = a.s && b.s;
        break;
    }
    case EXPR_EXTEND:
        t.w = ((const ExprExtend *)n)->to_bits;
        t.s = ty(N, ((const ExprExtend *)n)->operand).s;
        break;
    case EXPR_SV_CAST:
        t.w = ((const ExprSvCast *)n)->to_bits;
        t.s = ((const ExprSvCast *)n)->dst_signed ? 1 : 0;
        break;
    case EXPR_CAST:
        t.w = ((const ExprCast *)n)->to_bits;
        t.s = ((const ExprCast *)n)->to_signed ? 1 : 0;
        break;
    case EXPR_EXTRACT: {
        const ExprExtract *x = n;
        t.w = x->hi_bit >= x->lo_bit ? x->hi_bit - x->lo_bit + 1 : 1;
        break;
    }
    case EXPR_CONCAT: {
        const ExprConcat *x = n;
        uint64_t w = (uint64_t)ty(N, x->hi).w + x->lo_width;
        t.w = w > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w;
        break;
    }
    case EXPR_ELEM: {
        uint32_t a = ((const ExprElem *)n)->array_id;
        if (a < N->na && N->ak[a]) { t.w = N->aw[a]; t.s = N->as[a]; }
        else N->err = 1;
        break;
    }
    case EXPR_ARRAY_SIZE:
    case EXPR_LOOP_INDEX:
        t.w = 32; t.s = 1;
        break;
    case EXPR_REDUCE:
        t = ty(N, ((const ExprReduce *)n)->body);
        break;
    default:
        break;   /* membership, sum, countones, clog2, select: Boolean */
    }
    if (t.w == 0) t.w = 1;
    return t;
}

/* ---- the driver -------------------------------------------------------- */

static void mark_root(Nw *N, dvs_expr_t r) {
    if (r != EXPR_NULL && SLOT(r) < N->n_slots) N->mark[SLOT(r)] = 1;
}

#define FOR_ROOTS(N, BODY_C, BODY_S)                                                       \
    do {                                                                                   \
        const dvs_problem_t *p_ = (N)->p;                                                  \
        for (dvs_expr_t c_ = p_->constraints_head; c_ != EXPR_NULL;) {                     \
            dvs_expr_t at_ = c_;                                                           \
            c_ = ((const ConstraintSpec *)POOL_PTR((N)->p, at_))->next;                    \
            BODY_C;                                                                        \
        }                                                                                  \
        for (dvs_expr_t c_ = (N)->p->softs_head; c_ != EXPR_NULL;) {                       \
            dvs_expr_t at_ = c_;                                                           \
            c_ = ((const SoftSpec *)POOL_PTR((N)->p, at_))->next;                          \
            BODY_S;                                                                        \
        }                                                                                  \
        for (dvs_expr_t f_ = (N)->p->n_foreachs ? (N)->p->foreachs_head : EXPR_NULL;       \
             f_ != EXPR_NULL;) {                                                           \
            const ForeachSpec *fe_ = POOL_PTR((N)->p, f_);                                 \
            dvs_expr_t fnext_ = fe_->next;                                                 \
            for (dvs_expr_t c_ = fe_->n_constraints ? fe_->constraints_head : EXPR_NULL;   \
                 c_ != EXPR_NULL;) {                                                       \
                dvs_expr_t at_ = c_;                                                       \
                c_ = ((const ConstraintSpec *)POOL_PTR((N)->p, at_))->next;                \
                BODY_C;                                                                    \
            }                                                                              \
            fe_ = POOL_PTR((N)->p, f_);                                                    \
            for (dvs_expr_t c_ = fe_->n_softs ? fe_->softs_head : EXPR_NULL;               \
                 c_ != EXPR_NULL;) {                                                       \
                dvs_expr_t at_ = c_;                                                       \
                c_ = ((const SoftSpec *)POOL_PTR((N)->p, at_))->next;                      \
                BODY_S;                                                                    \
            }                                                                              \
            f_ = fnext_;                                                                   \
        }                                                                                  \
        (void)p_;                                                                          \
    } while (0)

int dvs_narrow_has_wide_decl(const dvs_problem_t *p) {
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(p, r);
        if (vs->width > 64) return 1;
        r = vs->next;
    }
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL;) {
        const ArraySpec *a = POOL_PTR(p, r);
        if (a->elem_width > 64) return 1;
        r = a->next;
    }
    return 0;
}

static int set_why(char *why, size_t whylen, const char *fmt, ...) {
    if (why && whylen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, whylen, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* Rewrite one root; 1 when it is left wide. */
static int narrow_root(Nw *N, dvs_expr_t spec, int soft, uint32_t *n_left, char *why,
                       size_t whylen) {
    dvs_expr_t root = soft ? ((const SoftSpec *)P(N, spec))->root
                           : ((const ConstraintSpec *)P(N, spec))->root;
    if (!wide(N, root)) return 0;
    N->err = 0;
    N->depth = 0;
    N->fail_w = 0;
    dvs_expr_t nr = truth(N, root);
    ar_free(N);
    if (N->oom) return -1;
    if (!N->err && nr != EXPR_NULL) {
        if (soft) ((SoftSpec *)P(N, spec))->root = nr;
        else ((ConstraintSpec *)P(N, spec))->root = nr;
        return 0;
    }
    if ((*n_left)++ == 0) {
        uint32_t id = soft ? ((const SoftSpec *)P(N, spec))->constraint_id
                           : ((const ConstraintSpec *)P(N, spec))->constraint_id;
        uint32_t w = N->fail_w;
        if (!w) w = 65;
        set_why(why, whylen,
                "UNSUPPORTED_WIDTH: %sconstraint %u has a %u-bit expression that cannot be "
                "narrowed to 64 bits, in a problem with soft constraints, a distribution "
                "or randc", soft ? "soft " : "", id, w);
    }
    return 1;
}

int dvs_narrow_problem(dvs_problem_t **psp, size_t *psize, char *why, size_t whylen) {
    Nw Nv, *N = &Nv;
    memset(N, 0, sizeof *N);
    N->p = *psp;
    N->size = *psize;
    const dvs_problem_t *p = N->p;
    int rc = -1;
    uint32_t n_left = 0;
    /* Types of the variables and array elements. */
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(p, r);
        if (vs->var_id >= N->nv) N->nv = vs->var_id + 1;
        r = vs->next;
    }
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL;) {
        const ArraySpec *a = POOL_PTR(p, r);
        if (a->array_id >= N->na) N->na = a->array_id + 1;
        r = a->next;
    }
    N->vw = calloc(N->nv + 1, sizeof *N->vw);
    N->vs = calloc(N->nv + 1, 1);
    N->vk = calloc(N->nv + 1, 1);
    N->aw = calloc(N->na + 1, sizeof *N->aw);
    N->as = calloc(N->na + 1, 1);
    N->ak = calloc(N->na + 1, 1);
    const uint32_t slots0 = (p->pool.used + DVS_IMAGE_ALIGN - 1) / DVS_IMAGE_ALIGN + 1;
    if (!N->vw || !N->vs || !N->vk || !N->aw || !N->as || !N->ak || tables_cover(N, slots0))
        goto out;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(p, r);
        N->vw[vs->var_id] = vs->width;
        N->vs[vs->var_id] = vs->is_signed ? 1 : 0;
        N->vk[vs->var_id] = 1;
        r = vs->next;
    }
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL;) {
        const ArraySpec *a = POOL_PTR(p, r);
        N->aw[a->array_id] = a->elem_width;
        N->as[a->array_id] = a->elem_signed ? 1 : 0;
        N->ak[a->array_id] = 1;
        r = a->next;
    }
    /* Mark the nodes the roots reach (each node's children lie below it),
     * then type them from the bottom up. */
    FOR_ROOTS(N, mark_root(N, ((const ConstraintSpec *)POOL_PTR(N->p, at_))->root),
              mark_root(N, ((const SoftSpec *)POOL_PTR(N->p, at_))->root));
    const uint8_t *base = (const uint8_t *)&N->p->pool;
    const uint64_t end = NW_HDR + (uint64_t)N->p->pool.used;
    image_node_t nd;
    for (uint32_t s = slots0; s-- > 0;) {
        if (!N->mark[s]) continue;
        uint64_t r = NW_HDR + (uint64_t)s * DVS_IMAGE_ALIGN;
        if (r >= end || dvs_image_node(base + r, end - r, &nd) != 0) { N->mark[s] = 0; continue; }
        for (uint64_t i = 0; i < nd.n_kids + nd.n_more; i++) {
            dvs_expr_t k = i < nd.n_kids ? nd.kids[i] : nd.more[i - nd.n_kids];
            mark_root(N, k);
        }
    }
    for (uint32_t s = 0; s < slots0; s++) {
        if (!N->mark[s]) continue;
        dvs_expr_t r = NW_HDR + s * DVS_IMAGE_ALIGN;
        dvs_image_node(base + r, end - r, &nd);
        N->err = 0;
        Ty t = node_type(N, r);
        if (N->err) { N->mark[s] = 0; continue; }
        uint8_t wd = t.w > 64;
        for (uint64_t i = 0; i < nd.n_kids + nd.n_more && !wd; i++) {
            dvs_expr_t k = i < nd.n_kids ? nd.kids[i] : nd.more[i - nd.n_kids];
            wd = k != EXPR_NULL && SLOT(k) < N->n_slots && N->wd[SLOT(k)];
        }
        N->tw[s] = t.w;
        N->ts[s] = t.s;
        N->wd[s] = wd;
    }
    rc = 0;
    FOR_ROOTS(N, { int k = narrow_root(N, at_, 0, &n_left, why, whylen);
                   if (k < 0) { rc = -1; goto out; } },
              { int k = narrow_root(N, at_, 1, &n_left, why, whylen);
                if (k < 0) { rc = -1; goto out; } });
    rc = (int)n_left;
out:
    ar_free(N);
    *psp = N->p;
    *psize = N->size;
    free(N->tw); free(N->ts); free(N->wd); free(N->mark);
    free(N->vw); free(N->vs); free(N->vk);
    free(N->aw); free(N->as); free(N->ak);
    return rc;
}
