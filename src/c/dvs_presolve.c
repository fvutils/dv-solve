#include "dvs_presolve.h"

#include <stdlib.h>
#include <string.h>

/*
 * Linear-equality presolve -- see dvs_presolve.h.
 *
 * A FORM is a node's value as an integer linear function of variables,
 * sum(c_j * x_j) + k. A node has one only when no wrap can occur: each
 * operator's result range, from the variables' type ranges, fits the node's
 * own type, so its bit pattern read by that type is the form's value. The
 * engines read an operator as signed when either operand is (the CDCL
 * compile's _expr_has_signed); an EXPLICIT problem extends a narrower
 * operand by its own signedness, which keeps that operand's value.
 *
 * Each top-level `l == r` whose sides have forms is a row l - r = 0, exact
 * when the two sides' ranges together span fewer than 2^width values (equal
 * patterns then mean equal integers). Fraction-free elimination brings the
 * rows to echelon form; every formed subexpression whose form the rows
 * reduce to a constant, and that is not itself one of the rows, gets
 * `node == constant` as a new constraint, built from the variables at a
 * signed width wide enough that it cannot wrap either.
 *
 * Only EXPLICIT problems (Verilator, SMT-LIB2), without arrays or slots.
 * DVS_PRESOLVE=0 turns it off.
 */

#define PS_MAX_TERMS   64      /* terms of one form                  */
#define PS_MAX_DEPTH   256     /* expression nesting walked          */
#define PS_MAX_COLS    256     /* variables in the rows              */
#define PS_MAX_ROWS    256
#define PS_MAX_EMIT    64      /* constraints added                  */
#define PS_LIM         ((__int128)1 << 62)   /* every value and coefficient */
#define PS_NONE        UINT32_MAX

typedef __int128 i128;

typedef struct { uint32_t col; int64_t c; } Term;

typedef struct {
    uint32_t t0, n;     /* terms[t0 .. t0+n), sorted by col */
    int64_t  k;         /* constant part                    */
    int64_t  lo, hi;    /* value range                      */
    uint32_t w;         /* the node's width                 */
    uint8_t  s;         /* the node's signedness            */
    dvs_expr_t ref;     /* the node                         */
} Form;

typedef struct {
    dvs_problem_t *sp;
    uint32_t  nv;
    uint32_t *vw;       /* by var id: width, 0 if undeclared */
    uint8_t  *vs;       /* by var id: signed                 */
    uint32_t *memo;     /* by pool offset / 8: form id + 1, PS_NONE no form */
    uint32_t  n_memo;
    uint8_t  *walked;   /* by pool offset / 8: visited by _walk */
    Form     *forms;
    uint32_t  n_forms, cap_forms;
    Term     *terms;
    uint32_t  n_terms, cap_terms;
    int       oom;
} Ps;

static void *_P(const Ps *P, dvs_expr_t r) { return dvs_pool_ptr(&P->sp->pool, r); }

static i128 _vlo(const Ps *P, uint32_t v) {
    return P->vs[v] ? -((i128)1 << (P->vw[v] - 1)) : 0;
}
static i128 _vhi(const Ps *P, uint32_t v) {
    return P->vs[v] ? ((i128)1 << (P->vw[v] - 1)) - 1 : ((i128)1 << P->vw[v]) - 1;
}

/* Does every value in [lo, hi] fit a `w`-bit number of signedness `s`? */
static int _fits(i128 lo, i128 hi, uint32_t w, int s) {
    if (w == 0 || w > 64) return 0;
    if (s) return lo >= -((i128)1 << (w - 1)) && hi < ((i128)1 << (w - 1));
    return lo >= 0 && hi < ((i128)1 << w);
}

/* Append a form of `n` terms, value range from the terms; PS_NONE when a
 * coefficient or the range leaves +-2^62. */
static uint32_t _new_form(Ps *P, const Term *t, uint32_t n, i128 k,
                          dvs_expr_t ref, uint32_t w, int s) {
    if (n > PS_MAX_TERMS || k <= -PS_LIM || k >= PS_LIM) return PS_NONE;
    i128 lo = k, hi = k;
    for (uint32_t i = 0; i < n; i++) {
        i128 a = (i128)t[i].c * _vlo(P, t[i].col), b = (i128)t[i].c * _vhi(P, t[i].col);
        lo += a < b ? a : b;
        hi += a < b ? b : a;
    }
    if (lo <= -PS_LIM || hi >= PS_LIM || !_fits(lo, hi, w, s)) return PS_NONE;
    if (P->n_forms == P->cap_forms) {
        uint32_t nc = P->cap_forms ? 2 * P->cap_forms : 64;
        Form *f = realloc(P->forms, nc * sizeof *f);
        if (!f) { P->oom = 1; return PS_NONE; }
        P->forms = f; P->cap_forms = nc;
    }
    if (P->n_terms + n > P->cap_terms) {
        uint32_t nc = P->cap_terms ? 2 * P->cap_terms : 256;
        while (nc < P->n_terms + n) nc *= 2;
        Term *tt = realloc(P->terms, nc * sizeof *tt);
        if (!tt) { P->oom = 1; return PS_NONE; }
        P->terms = tt; P->cap_terms = nc;
    }
    memcpy(P->terms + P->n_terms, t, n * sizeof *t);
    Form *f = &P->forms[P->n_forms];
    f->t0 = P->n_terms; f->n = n; f->k = (int64_t)k;
    f->lo = (int64_t)lo; f->hi = (int64_t)hi;
    f->w = w; f->s = (uint8_t)(s != 0); f->ref = ref;
    P->n_terms += n;
    return P->n_forms++;
}

/* a * A + b * B (sorted merge), as the form of `ref`. */
static uint32_t _combine(Ps *P, uint32_t A, int64_t a, uint32_t B, int64_t b,
                         dvs_expr_t ref, uint32_t w, int s) {
    Term t[2 * PS_MAX_TERMS];
    uint32_t n = 0, i = 0, j = 0;
    const Form fa = P->forms[A];
    const Form fb = B == PS_NONE ? (Form){0} : P->forms[B];
    while (i < fa.n || j < fb.n) {
        const Term *x = i < fa.n ? &P->terms[fa.t0 + i] : NULL;
        const Term *y = j < fb.n ? &P->terms[fb.t0 + j] : NULL;
        uint32_t col;
        i128 c = 0;
        if (x && (!y || x->col <= y->col)) { col = x->col; c += (i128)a * x->c; i++; }
        else col = y->col;
        if (y && y->col == col) { c += (i128)b * y->c; j++; }
        if (c <= -PS_LIM || c >= PS_LIM) return PS_NONE;
        if (c) t[n++] = (Term){ col, (int64_t)c };
    }
    return _new_form(P, t, n, (i128)a * fa.k + (i128)b * fb.k, ref, w, s);
}

static uint32_t _form(Ps *P, dvs_expr_t ref, int depth);

/* An extension (EXPR_EXTEND / EXPR_SV_CAST): the low `from` bits of the
 * operand extended to `to` and read with signedness `s`. It keeps the value
 * when the value fits `from` bits read the way it is extended (only needed
 * when widening) and fits the result type. */
static uint32_t _ext(Ps *P, dvs_expr_t ref, dvs_expr_t op, uint32_t from,
                     uint32_t to, int se, int s, int depth) {
    uint32_t a = _form(P, op, depth + 1);
    if (a == PS_NONE || from > P->forms[a].w) return PS_NONE;
    const Form *f = &P->forms[a];
    if (to > from && !_fits(f->lo, f->hi, from, se)) return PS_NONE;
    return _combine(P, a, 1, PS_NONE, 0, ref, to, s);
}

static uint32_t _form_node(Ps *P, dvs_expr_t ref, int depth) {
    if (depth > PS_MAX_DEPTH) return PS_NONE;
    switch (*(uint32_t *)_P(P, ref)) {
    case EXPR_CONST: {
        const ExprConst *c = _P(P, ref);
        uint32_t w = c->width;
        uint8_t s = c->is_signed;
        if (!w) return PS_NONE;     /* not in an EXPLICIT problem */
        i128 v = s || w == 64 ? (i128)c->value : (i128)(uint64_t)c->value;
        if (!s && w == 64 && c->value < 0) v += (i128)1 << 64;
        return _new_form(P, NULL, 0, v, ref, w, s);
    }
    case EXPR_VAR: {
        uint32_t v = ((const ExprVar *)_P(P, ref))->var_id;
        if (v >= P->nv || !P->vw[v] || P->vw[v] > 62) return PS_NONE;
        Term t = { v, 1 };
        return _new_form(P, &t, 1, 0, ref, P->vw[v], P->vs[v]);
    }
    case EXPR_BINARY: {
        const ExprBinary *b = _P(P, ref);
        if (b->op != DVS_BIN_ADD && b->op != DVS_BIN_SUB && b->op != DVS_BIN_MUL)
            return PS_NONE;
        uint32_t l = _form(P, b->lhs, depth + 1), r = _form(P, b->rhs, depth + 1);
        if (l == PS_NONE || r == PS_NONE) return PS_NONE;
        const Form *fl = &P->forms[l], *fr = &P->forms[r];
        uint32_t w = fl->w > fr->w ? fl->w : fr->w;
        int s = fl->s || fr->s;
        if (b->op == DVS_BIN_ADD) return _combine(P, l, 1, r, 1, ref, w, s);
        if (b->op == DVS_BIN_SUB) return _combine(P, l, 1, r, -1, ref, w, s);
        if (!fr->n) return _combine(P, l, fr->k, PS_NONE, 0, ref, w, s);
        if (!fl->n) return _combine(P, r, fl->k, PS_NONE, 0, ref, w, s);
        return PS_NONE;
    }
    case EXPR_UNARY: {
        const ExprUnary *u = _P(P, ref);
        if (u->op != DVS_UN_NEG) return PS_NONE;
        uint32_t a = _form(P, u->operand, depth + 1);
        if (a == PS_NONE) return PS_NONE;
        return _combine(P, a, -1, PS_NONE, 0, ref, P->forms[a].w, P->forms[a].s);
    }
    case EXPR_EXTEND: {
        const ExprExtend *e = _P(P, ref);
        uint32_t a = _form(P, e->operand, depth + 1);
        if (a == PS_NONE) return PS_NONE;
        return _ext(P, ref, e->operand, e->from_bits, e->to_bits, e->sign_extend,
                    P->forms[a].s, depth);
    }
    case EXPR_SV_CAST: {
        const ExprSvCast *e = _P(P, ref);
        return _ext(P, ref, e->operand, e->from_bits, e->to_bits, e->sign_extend,
                    e->dst_signed, depth);
    }
    case EXPR_EXTRACT: {
        const ExprExtract *e = _P(P, ref);
        uint32_t a = _form(P, e->operand, depth + 1);
        if (a == PS_NONE || e->lo_bit || e->hi_bit >= P->forms[a].w) return PS_NONE;
        return _combine(P, a, 1, PS_NONE, 0, ref, e->hi_bit + 1, 0);
    }
    default:
        return PS_NONE;
    }
}

static uint32_t _form(Ps *P, dvs_expr_t ref, int depth) {
    if (ref == EXPR_NULL || ref / 8 >= P->n_memo) return PS_NONE;
    uint32_t *m = &P->memo[ref / 8];
    if (*m) return *m == PS_NONE ? PS_NONE : *m - 1;
    uint32_t f = _form_node(P, ref, depth);
    *m = f == PS_NONE ? PS_NONE : f + 1;
    return f;
}

/* Visit every node of a constraint tree once, giving each its form. */
static void _walk(Ps *P, dvs_expr_t ref, int depth) {
    if (ref == EXPR_NULL || depth > PS_MAX_DEPTH || ref / 8 >= P->n_memo
        || P->walked[ref / 8])
        return;
    P->walked[ref / 8] = 1;
    _form(P, ref, depth);
    switch (*(uint32_t *)_P(P, ref)) {
    case EXPR_BINARY: {
        const ExprBinary *b = _P(P, ref);
        _walk(P, b->lhs, depth + 1); _walk(P, b->rhs, depth + 1); break;
    }
    case EXPR_UNARY:   _walk(P, ((const ExprUnary *)_P(P, ref))->operand, depth + 1); break;
    case EXPR_EXTEND:  _walk(P, ((const ExprExtend *)_P(P, ref))->operand, depth + 1); break;
    case EXPR_SV_CAST: _walk(P, ((const ExprSvCast *)_P(P, ref))->operand, depth + 1); break;
    case EXPR_EXTRACT: _walk(P, ((const ExprExtract *)_P(P, ref))->operand, depth + 1); break;
    case EXPR_ITE: {
        const ExprITE *e = _P(P, ref);
        _walk(P, e->cond, depth + 1); _walk(P, e->then_e, depth + 1);
        _walk(P, e->else_e, depth + 1); break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t nc;        /* columns: variables, then the constant */
    uint32_t *col_of;   /* by var id: column + 1, 0 none         */
    int64_t  *row;      /* PS_MAX_ROWS rows of nc + 1 entries    */
    uint32_t  n_rows;
    int64_t  *orig;     /* the rows as given, normalized         */
    uint32_t  n_orig;
    uint32_t *piv;      /* pivot column of each echelon row      */
    uint32_t  n_piv;
} Sys;

static int64_t _gcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a;
}

/* Divide by the gcd and make the first nonzero entry positive. */
static void _normalize(int64_t *v, uint32_t n) {
    int64_t g = 0;
    uint32_t first = n;
    for (uint32_t i = 0; i < n; i++)
        if (v[i]) { g = _gcd(g, v[i]); if (first == n) first = i; }
    if (!g) return;
    if (v[first] < 0) g = -g;
    for (uint32_t i = 0; i < n; i++) v[i] /= g;
}

/* v := m * v - c * row with m, c making v[col] zero; returns 0 on overflow.
 * `mult` (if not NULL) collects the factor v was multiplied by. */
static int _eliminate(int64_t *v, const int64_t *row, uint32_t col, uint32_t n,
                      i128 *mult) {
    int64_t g = _gcd(row[col], v[col]);
    int64_t m = row[col] / g, c = v[col] / g;
    if (m < 0) { m = -m; c = -c; }
    for (uint32_t i = 0; i < n; i++) {
        i128 x = (i128)m * v[i] - (i128)c * row[i];
        if (x <= -PS_LIM || x >= PS_LIM) return 0;
        v[i] = (int64_t)x;
    }
    if (mult) { *mult *= m; if (*mult >= PS_LIM) return 0; }
    return 1;
}

/* Reduce `v` (nc + 1 entries) by the echelon rows. */
static int _reduce(const Sys *S, int64_t *v, i128 *mult) {
    for (uint32_t r = 0; r < S->n_piv; r++)
        if (v[S->piv[r]] && !_eliminate(v, S->row + (size_t)r * (S->nc + 1),
                                        S->piv[r], S->nc + 1, mult))
            return 0;
    return 1;
}

/* Fill v (nc + 1 entries) with form f minus `rhs`; 0 if f uses a variable
 * outside the columns. */
static int _vec(const Ps *P, const Sys *S, const Form *f, i128 rhs, int64_t *v) {
    memset(v, 0, (S->nc + 1) * sizeof *v);
    for (uint32_t i = 0; i < f->n; i++) {
        const Term *t = &P->terms[f->t0 + i];
        if (!S->col_of[t->col]) return 0;
        v[S->col_of[t->col] - 1] = t->c;
    }
    i128 k = (i128)f->k - rhs;
    if (k <= -PS_LIM || k >= PS_LIM) return 0;
    v[S->nc] = (int64_t)k;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Emission                                                            */
/* ------------------------------------------------------------------ */

static dvs_expr_t _mk_cast(dvs_problem_t *sp, dvs_expr_t x, uint32_t from,
                           uint32_t to, uint8_t se) {
    dvs_expr_t r = dvs_pool_alloc(&sp->pool, sizeof(ExprSvCast), DVS_IMAGE_ALIGN);
    if (r == EXPR_NULL) return EXPR_NULL;
    ExprSvCast *n = POOL_PTR(sp, r);
    memset(n, 0, sizeof *n);
    n->kind = EXPR_SV_CAST;
    n->sign_extend = se;
    n->dst_signed = 1;
    n->from_bits = from;
    n->to_bits = to;
    n->operand = x;
    return r;
}

/* `sum(c_j * x_j) == value` at signed width W (x_j sign- or zero-extended
 * by their own type), or `x == value` for a single variable. */
static dvs_expr_t _emit(Ps *P, const Form *f, int64_t value, uint32_t W) {
    dvs_problem_t *sp = P->sp;
    const Term *t = &P->terms[f->t0];
    if (f->n == 1 && t->c == 1) {
        dvs_expr_t x = expr_var(sp, t->col);
        dvs_expr_t c = expr_const_sized(sp, value, P->vs[t->col], P->vw[t->col]);
        return x && c ? expr_binary(sp, DVS_BIN_EQ, x, c) : EXPR_NULL;
    }
    dvs_expr_t acc = EXPR_NULL;
    for (uint32_t i = 0; i < f->n; i++) {
        uint32_t v = t[i].col;
        int64_t c = t[i].c < 0 ? -t[i].c : t[i].c;
        dvs_expr_t x = _mk_cast(sp, expr_var(sp, v), P->vw[v], W, P->vs[v]);
        if (c != 1) x = expr_binary(sp, DVS_BIN_MUL, x, expr_const_sized(sp, c, 1, W));
        if (acc == EXPR_NULL)
            acc = t[i].c < 0 ? expr_unary(sp, DVS_UN_NEG, x) : x;
        else
            acc = expr_binary(sp, t[i].c < 0 ? DVS_BIN_SUB : DVS_BIN_ADD, acc, x);
    }
    return expr_binary(sp, DVS_BIN_EQ, acc, expr_const_sized(sp, value, 1, W));
}

/* Signed width that holds every partial sum of the terms and `value`. */
static uint32_t _emit_width(const Ps *P, const Form *f, int64_t value) {
    i128 b = value < 0 ? -(i128)value : value;
    uint32_t wmax = 0;
    for (uint32_t i = 0; i < f->n; i++) {
        const Term *t = &P->terms[f->t0 + i];
        i128 lo = _vlo(P, t->col), hi = _vhi(P, t->col);
        i128 m = -lo > hi ? -lo : hi;
        b += (t->c < 0 ? -(i128)t->c : t->c) * m;
        if (P->vw[t->col] > wmax) wmax = P->vw[t->col];
    }
    uint32_t W = 2;
    while (W < 64 && b >= ((i128)1 << (W - 1))) W++;
    if (W <= wmax) W = wmax + 1;
    return b < ((i128)1 << (W - 1)) && W <= 64 ? W : 0;
}

/* Bytes _emit may allocate for a form. */
static size_t _emit_bytes(const Form *f) {
    return (size_t)f->n * 96 + 128;
}

static int _same(const int64_t *a, const int64_t *b, uint32_t n) {
    return !memcmp(a, b, n * sizeof *a);
}

uint32_t dvs_presolve_linear(dvs_problem_t **psp, size_t *psize) {
    dvs_problem_t *sp = *psp;
    const char *e = getenv("DVS_PRESOLVE");
    if ((e && *e == '0') || !(sp->flags & DVS_PROBLEM_F_EXPLICIT) || sp->n_arrays
        || sp->n_foreachs || sp->n_slots || !sp->n_constraints)
        return 0;

    Ps P = { .sp = sp };
    uint32_t added = 0;
    Sys S = {0};
    int64_t *v = NULL;
    uint32_t *cand = NULL;
    int64_t *cval = NULL;
    uint32_t n_cand = 0;

    for (dvs_expr_t r = sp->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(sp, r);
        if (vs->var_id >= P.nv) P.nv = vs->var_id + 1;
        r = vs->next;
    }
    P.vw = calloc(P.nv ? P.nv : 1, sizeof *P.vw);
    P.vs = calloc(P.nv ? P.nv : 1, 1);
    P.n_memo = sp->pool.used / 8 + 1;
    P.memo = calloc(P.n_memo, sizeof *P.memo);
    P.walked = calloc(P.n_memo, 1);
    S.col_of = calloc(P.nv ? P.nv : 1, sizeof *S.col_of);
    if (!P.vw || !P.vs || !P.memo || !P.walked || !S.col_of) goto done;
    for (dvs_expr_t r = sp->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(sp, r);
        P.vw[vs->var_id] = vs->width;
        P.vs[vs->var_id] = vs->is_signed != 0;
        r = vs->next;
    }

    /* Forms of every node; the top-level equalities (through &&) as rows. */
    dvs_expr_t eqs[PS_MAX_ROWS];
    uint32_t n_eq = 0;
    for (dvs_expr_t r = sp->constraints_head; r != EXPR_NULL;) {
        const ConstraintSpec *cs = POOL_PTR(sp, r);
        dvs_expr_t stack[64];
        uint32_t ns = 0;
        stack[ns++] = cs->root;
        while (ns) {
            dvs_expr_t x = stack[--ns];
            _walk(&P, x, 0);
            if (x == EXPR_NULL || *(uint32_t *)POOL_PTR(sp, x) != EXPR_BINARY) continue;
            const ExprBinary *b = POOL_PTR(sp, x);
            if (b->op == DVS_BIN_AND && ns + 2 <= 64) {
                stack[ns++] = b->lhs;
                stack[ns++] = b->rhs;
            } else if (b->op == DVS_BIN_EQ && n_eq < PS_MAX_ROWS) {
                eqs[n_eq++] = x;
            }
        }
        r = cs->next;
    }
    if (P.oom || !n_eq) goto done;

    /* Columns: the variables of the rows. */
    uint32_t rl[PS_MAX_ROWS], rr[PS_MAX_ROWS], n_rows = 0;
    for (uint32_t i = 0; i < n_eq; i++) {
        const ExprBinary *b = POOL_PTR(sp, eqs[i]);
        uint32_t l = _form(&P, b->lhs, 0), r = _form(&P, b->rhs, 0);
        if (l == PS_NONE || r == PS_NONE) continue;
        const Form *fl = &P.forms[l], *fr = &P.forms[r];
        uint32_t w = fl->w > fr->w ? fl->w : fr->w;
        i128 lo = fl->lo < fr->lo ? fl->lo : fr->lo;
        i128 hi = fl->hi > fr->hi ? fl->hi : fr->hi;
        if (w > 64 || hi - lo >= ((i128)1 << w)) continue;
        if (!fl->n && !fr->n) continue;
        rl[n_rows] = l; rr[n_rows] = r; n_rows++;
        for (int side = 0; side < 2; side++) {
            const Form *f = side ? fr : fl;
            for (uint32_t j = 0; j < f->n; j++) {
                uint32_t col = P.terms[f->t0 + j].col;
                if (!S.col_of[col]) {
                    if (S.nc == PS_MAX_COLS) goto done;
                    S.col_of[col] = ++S.nc;
                }
            }
        }
    }
    if (!n_rows) goto done;

    size_t rw = S.nc + 1;
    S.row = calloc((size_t)PS_MAX_ROWS * rw, sizeof *S.row);
    S.orig = calloc((size_t)PS_MAX_ROWS * rw, sizeof *S.orig);
    S.piv = calloc(PS_MAX_ROWS, sizeof *S.piv);
    v = calloc(rw, sizeof *v);
    if (!S.row || !S.orig || !S.piv || !v) goto done;
    for (uint32_t i = 0; i < n_rows; i++) {
        /* l - r: the right side as a form with negated terms. */
        const Form *fl = &P.forms[rl[i]], *fr = &P.forms[rr[i]];
        int64_t *o = S.orig + (size_t)S.n_orig * rw;
        if (!_vec(&P, &S, fl, 0, o)) goto done;
        for (uint32_t j = 0; j < fr->n; j++) {
            const Term *t = &P.terms[fr->t0 + j];
            i128 c = (i128)o[S.col_of[t->col] - 1] - t->c;
            if (c <= -PS_LIM || c >= PS_LIM) goto done;
            o[S.col_of[t->col] - 1] = (int64_t)c;
        }
        i128 k = (i128)o[S.nc] - fr->k;
        if (k <= -PS_LIM || k >= PS_LIM) goto done;
        o[S.nc] = (int64_t)k;
        _normalize(o, rw);
        S.n_orig++;
        memcpy(v, o, rw * sizeof *v);
        if (!_reduce(&S, v, NULL)) goto done;
        _normalize(v, rw);
        uint32_t p = 0;
        while (p < S.nc && !v[p]) p++;
        if (p == S.nc) {
            if (v[S.nc]) goto done;     /* 0 == k != 0: leave it to the engines */
            continue;
        }
        memcpy(S.row + (size_t)S.n_piv * rw, v, rw * sizeof *v);
        S.piv[S.n_piv++] = p;
    }
    if (!S.n_piv) goto done;

    /* Formed nodes the rows fix, not already a row, one per form. */
    cand = malloc((P.n_forms ? P.n_forms : 1) * sizeof *cand);
    cval = malloc((P.n_forms ? P.n_forms : 1) * sizeof *cval);
    int64_t *seen = calloc((size_t)PS_MAX_EMIT * rw, sizeof *seen);
    if (!cand || !cval || !seen) { free(seen); goto done; }
    size_t need = 0;
    for (uint32_t fi = 0; fi < P.n_forms && n_cand < PS_MAX_EMIT; fi++) {
        const Form *f = &P.forms[fi];
        if (!f->n || P.memo[f->ref / 8] != fi + 1) continue;  /* constants, partials */
        if (!_vec(&P, &S, f, 0, v)) continue;
        i128 mult = 1;
        if (!_reduce(&S, v, &mult)) continue;
        uint32_t j = 0;
        while (j < S.nc && !v[j]) j++;
        if (j < S.nc) continue;
        /* v is mult * f less multiples of the rows: mult * f == v[nc]. */
        if (v[S.nc] % (int64_t)mult) continue;
        int64_t value = v[S.nc] / (int64_t)mult;
        if (value < f->lo || value > f->hi) continue;
        if (!_vec(&P, &S, f, value, v)) continue;
        _normalize(v, rw);
        int dup = 0;
        for (uint32_t i = 0; i < S.n_orig && !dup; i++) dup = _same(v, S.orig + i * rw, rw);
        for (uint32_t i = 0; i < n_cand && !dup; i++) dup = _same(v, seen + i * rw, rw);
        if (dup || (f->n > 1 && !_emit_width(&P, f, value))) continue;
        memcpy(seen + (size_t)n_cand * rw, v, rw * sizeof *v);
        cand[n_cand] = fi;
        cval[n_cand++] = value;
        need += _emit_bytes(f);
    }
    free(seen);
    if (!n_cand) goto done;

    /* Grow the buffer: pool offsets do not move. */
    size_t hdr = sizeof(dvs_problem_t);
    size_t nsize = hdr + (size_t)sp->pool.used + need;
    if (nsize < *psize) nsize = *psize;
    if (nsize - hdr > UINT32_MAX) goto done;
    dvs_problem_t *np = realloc(sp, nsize);
    if (!np) goto done;
    *psp = sp = P.sp = np;
    *psize = nsize;
    sp->pool.capacity = (uint32_t)(nsize - hdr);
    dvs_pool_mark_t mark = dvs_pool_mark(&sp->pool);
    for (uint32_t i = 0; i < n_cand; i++) {
        const Form *f = &P.forms[cand[i]];
        dvs_expr_t root = _emit(&P, f, cval[i], _emit_width(&P, f, cval[i]));
        if (root == EXPR_NULL || problem_add_constraint(sp, root) == EXPR_NULL
            || sp->pool.overflow) {
            /* Out of room: nothing half-built is reachable; drop it all. */
            for (; added; added--) {
                const ConstraintSpec *cs = POOL_PTR(sp, sp->constraints_head);
                sp->constraints_head = cs->next;
                sp->n_constraints--;
                sp->next_constraint_id--;
            }
            sp->pool.used = mark.used;
            sp->pool.overflow = mark.overflow;
            break;
        }
        added++;
    }

done:
    free(P.vw); free(P.vs); free(P.memo); free(P.walked); free(P.forms); free(P.terms);
    free(S.col_of); free(S.row); free(S.orig); free(S.piv);
    free(v); free(cand); free(cval);
    return added;
}
