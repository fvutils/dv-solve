/*
 * dvs_smt2_emit -- a builder-API problem as SMT-LIB2, under SV semantics.
 * See dvs_smt2_emit.h.
 *
 * The rules are the validator's (dvs_validate.c `_vtype` / `_ev`), read
 * symbolically: every node has a self-determined type; val(ref, W, S) is the
 * node evaluated in a context of width W and signedness S, a (_ BitVec W)
 * term; truth(ref) is the node's truth in its own type, a Bool term.
 * Conversions between widths extend by the SOURCE's signedness, as SV does.
 */
#include "dvs_smt2_emit.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "dv_solve.h"
#include "dvs_sv.h"

typedef struct { uint16_t w; uint8_t s; } ETy;

/* A small open-addressing map from 64-bit keys to 32-bit values. */
typedef struct { uint64_t *k; uint32_t *v; uint32_t cap, n; } EMap;

static uint32_t _h(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33;
    return (uint32_t)k;
}
static int _map_get(const EMap *m, uint64_t key, uint32_t *out) {
    if (!m->cap) return 0;
    for (uint32_t i = _h(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == 0) return 0;
        if (m->k[i] == key) { *out = m->v[i]; return 1; }
    }
}
static int _map_put(EMap *m, uint64_t key, uint32_t val) {
    if ((m->n + 1) * 2 > m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 256;
        uint64_t *nk = (uint64_t *)calloc(nc, sizeof(uint64_t));
        uint32_t *nv = (uint32_t *)calloc(nc, sizeof(uint32_t));
        if (!nk || !nv) { free(nk); free(nv); return -1; }
        for (uint32_t i = 0; i < m->cap; i++) {
            if (!m->k[i]) continue;
            uint32_t j = _h(m->k[i]) & (nc - 1);
            while (nk[j]) j = (j + 1) & (nc - 1);
            nk[j] = m->k[i]; nv[j] = m->v[i];
        }
        free(m->k); free(m->v);
        m->k = nk; m->v = nv; m->cap = nc;
    }
    uint32_t i = _h(key) & (m->cap - 1);
    while (m->k[i] && m->k[i] != key) i = (i + 1) & (m->cap - 1);
    if (!m->k[i]) m->n++;
    m->k[i] = key; m->v[i] = val;
    return 0;
}
static void _map_free(EMap *m) { free(m->k); free(m->v); memset(m, 0, sizeof(*m)); }

typedef struct {
    const dvs_problem_t *p;
    dvs_emit_var_type_fn fn;
    void      *ud;
    EMap       vtypes;     /* var id + 1 -> (w << 8 | s), from the VarSpecs */
    EMap       types;      /* ref + 1 -> (w << 8 | s) */
    EMap       uses;       /* ref + 1 -> parent edges */
    EMap       memo;       /* (ref, W, S, mode) -> define-fun number */
    DvsOBuf   *defs;       /* define-funs, written before the assert using them */
    const char *prefix;
    uint32_t  *counter;
    unsigned   flags;
    char      *why;
    size_t     why_n;
} E;

static void *_P(E *e, dvs_expr_t r) {
    return dvs_pool_ptr((dvs_pool_t *)&e->p->pool, r);
}
static ExprKind _kind(E *e, dvs_expr_t r) { return *(ExprKind *)_P(e, r); }

static void _fail(E *e, const char *msg) {
    if (!(e->flags & DVS_EMIT_F_UNSUPPORTED) && e->why && e->why_n)
        snprintf(e->why, e->why_n, "%s", msg);
    e->flags |= DVS_EMIT_F_UNSUPPORTED;
}

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
    return op == DVS_BIN_EQ || op == DVS_BIN_NEQ || op == DVS_BIN_LT ||
           op == DVS_BIN_LTE || op == DVS_BIN_GT || op == DVS_BIN_GTE;
}

/* A node whose value is a truth (1-bit unsigned): comparisons, connectives,
 * membership, and the aggregate relations. */
static int _is_boolish(E *e, dvs_expr_t r) {
    switch (_kind(e, r)) {
    case EXPR_BINARY: {
        dvs_binop_t op = ((ExprBinary *)_P(e, r))->op;
        return _is_cmp(op) || op == DVS_BIN_AND || op == DVS_BIN_OR;
    }
    case EXPR_UNARY:
        return ((ExprUnary *)_P(e, r))->op == DVS_UN_NOT;
    case EXPR_IN_RANGE: case EXPR_IN_SET: case EXPR_IN_RANGES:
    case EXPR_SUM: case EXPR_COUNTONES: case EXPR_CLOG2: case EXPR_ARRAY_SELECT:
        return 1;
    default:
        return 0;
    }
}

static int _var_type(E *e, uint32_t id, ETy *t) {
    uint32_t v;
    if (_map_get(&e->vtypes, (uint64_t)id + 1, &v)) {
        t->w = (uint16_t)(v >> 8); t->s = (uint8_t)(v & 1);
        return 0;
    }
    uint16_t w; uint8_t s;
    if (e->fn && e->fn(e->ud, id, &w, &s) == 0) { t->w = w; t->s = s ? 1 : 0; return 0; }
    char msg[64];
    snprintf(msg, sizeof(msg), "undeclared variable %u", id);
    _fail(e, msg);
    t->w = 1; t->s = 0;
    return -1;
}

/* Self-determined type of a node (dvs_validate.c _vtype). */
static ETy _type(E *e, dvs_expr_t ref) {
    ETy t = { 1, 0 };
    if (ref == EXPR_NULL) { _fail(e, "null expression"); return t; }
    uint32_t mv;
    if (_map_get(&e->types, (uint64_t)ref + 1, &mv)) {
        t.w = (uint16_t)(mv >> 8); t.s = (uint8_t)(mv & 1);
        return t;
    }
    switch (_kind(e, ref)) {
    case EXPR_VAR:
        _var_type(e, ((ExprVar *)_P(e, ref))->var_id, &t);
        break;
    case EXPR_CONST: {
        uint16_t w; uint8_t s;
        dvs_sv_const_type((ExprConst *)_P(e, ref), &w, &s);
        t.w = w; t.s = s;
        break;
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)_P(e, ref);
        if (_is_arith(eb->op)) {
            ETy a = _type(e, eb->lhs), b = _type(e, eb->rhs);
            t.w = a.w > b.w ? a.w : b.w;
            t.s = (uint8_t)(a.s && b.s);
        } else if (eb->op == DVS_BIN_LSHIFT || eb->op == DVS_BIN_RSHIFT ||
                   eb->op == DVS_BIN_ASHR) {
            t = _type(e, eb->lhs);
        }
        break;
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)_P(e, ref);
        if (eu->op != DVS_UN_NOT) t = _type(e, eu->operand);
        break;
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)_P(e, ref);
        ETy a = _type(e, ei->then_e), b = _type(e, ei->else_e);
        t.w = a.w > b.w ? a.w : b.w;
        t.s = (uint8_t)(a.s && b.s);
        break;
    }
    case EXPR_EXTEND: {
        ExprExtend *ee = (ExprExtend *)_P(e, ref);
        t.w = ee->to_bits;
        t.s = _type(e, ee->operand).s;
        break;
    }
    case EXPR_SV_CAST: {
        ExprSvCast *ec = (ExprSvCast *)_P(e, ref);
        t.w = ec->to_bits;
        t.s = ec->dst_signed ? 1 : 0;
        break;
    }
    case EXPR_CAST: {
        ExprCast *ec = (ExprCast *)_P(e, ref);
        t.w = ec->to_bits;
        t.s = ec->to_signed ? 1 : 0;
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract *ex = (ExprExtract *)_P(e, ref);
        t.w = (uint16_t)(ex->hi_bit - ex->lo_bit + 1);
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat *ec = (ExprConcat *)_P(e, ref);
        t.w = (uint16_t)(_type(e, ec->hi).w + ec->lo_width);
        break;
    }
    default:
        break;      /* comparisons, connectives, membership: 1-bit unsigned */
    }
    if (t.w == 0) t.w = 1;
    _map_put(&e->types, (uint64_t)ref + 1, ((uint32_t)t.w << 8) | t.s);
    return t;
}

/* ------------------------------------------------------------------ */
/* Term writers                                                        */
/* ------------------------------------------------------------------ */

void dvs_smt2_emit_value(DvsOBuf *out, int64_t value, uint16_t w) {
    uint64_t u = (uint64_t)value;
    if (w < 64) u &= (((uint64_t)1) << w) - 1;
    dvs_ob_printf(out, "(_ bv%" PRIu64 " %u)", u, (unsigned)w);
}

/* Open a conversion of a term of (from_w, from_s) to width to_w: extension by
 * the source's signedness, or truncation. Returns the number of ')' to close. */
static int _conv_open(DvsOBuf *o, uint16_t from_w, uint8_t from_s, uint16_t to_w) {
    if (to_w > from_w) {
        dvs_ob_printf(o, "((_ %s %u) ", from_s ? "sign_extend" : "zero_extend",
                      (unsigned)(to_w - from_w));
        return 1;
    }
    if (to_w < from_w) {
        dvs_ob_printf(o, "((_ extract %u 0) ", (unsigned)(to_w - 1));
        return 1;
    }
    return 0;
}
static void _close(DvsOBuf *o, int n) { while (n-- > 0) dvs_ob_add(o, ")", 1); }

static void _val(E *e, dvs_expr_t ref, uint16_t W, uint8_t S, DvsOBuf *o);
static void _val_raw(E *e, dvs_expr_t ref, uint16_t W, uint8_t S, DvsOBuf *o);
static void _truth(E *e, dvs_expr_t ref, DvsOBuf *o);
static void _bool(E *e, dvs_expr_t ref, DvsOBuf *o);

static void _var_term(E *e, uint32_t id, uint16_t W, DvsOBuf *o) {
    ETy t;
    _var_type(e, id, &t);
    int c = _conv_open(o, t.w, t.s, W);
    dvs_ob_printf(o, "v%u", id);
    _close(o, c);
}

/* `l op r` for a comparison, in the comparison's own context. */
static void _cmp(E *e, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r, DvsOBuf *o) {
    ETy a = _type(e, l), b = _type(e, r);
    uint16_t w = a.w > b.w ? a.w : b.w;
    uint8_t  s = (uint8_t)(a.s && b.s);
    const char *f;
    switch (op) {
    case DVS_BIN_EQ:  f = "="; break;
    case DVS_BIN_NEQ: f = "distinct"; break;
    case DVS_BIN_LT:  f = s ? "bvslt" : "bvult"; break;
    case DVS_BIN_LTE: f = s ? "bvsle" : "bvule"; break;
    case DVS_BIN_GT:  f = s ? "bvsgt" : "bvugt"; break;
    case DVS_BIN_GTE: f = s ? "bvsge" : "bvuge"; break;
    default: _fail(e, "bad comparison"); dvs_ob_str(o, "false"); return;
    }
    dvs_ob_printf(o, "(%s ", f);
    _val(e, l, w, s, o);
    dvs_ob_add(o, " ", 1);
    _val(e, r, w, s, o);
    dvs_ob_add(o, ")", 1);
}

/* Smallest width holding the unsigned value n. */
static uint16_t _bits_for(uint64_t n) {
    uint16_t b = 1;
    while (b < 64 && (n >> b)) b++;
    return b;
}

/* The aggregate relations, as Bool terms. */
static void _aggregate(E *e, dvs_expr_t ref, DvsOBuf *o) {
    e->flags |= DVS_EMIT_F_AGG;
    switch (_kind(e, ref)) {
    case EXPR_SUM: {
        /* result == v0 + v1 + ..., as integers: every term widened (by its
         * own signedness) to a width the sum cannot overflow. */
        ExprSum *es = (ExprSum *)_P(e, ref);
        const dvs_expr_t *xs = (const dvs_expr_t *)(es + 1);
        ETy rt = _type(e, es->result);
        uint16_t mw = rt.w;
        for (uint32_t i = 0; i < es->n_vars; i++) {
            ETy t = _type(e, xs[i]);
            if (t.w > mw) mw = t.w;
        }
        uint16_t W = (uint16_t)(mw + _bits_for((uint64_t)es->n_vars + 1) + 1);
        dvs_ob_str(o, "(= ");
        _val(e, es->result, W, rt.s, o);
        dvs_ob_add(o, " ", 1);
        if (es->n_vars == 0) {
            dvs_smt2_emit_value(o, 0, W);
        } else {
            if (es->n_vars > 1) dvs_ob_str(o, "(bvadd");
            for (uint32_t i = 0; i < es->n_vars; i++) {
                if (es->n_vars > 1) dvs_ob_add(o, " ", 1);
                _val(e, xs[i], W, _type(e, xs[i]).s, o);
            }
            if (es->n_vars > 1) dvs_ob_add(o, ")", 1);
        }
        dvs_ob_add(o, ")", 1);
        return;
    }
    case EXPR_COUNTONES: {
        ExprCountones *ec = (ExprCountones *)_P(e, ref);
        ETy xt = _type(e, ec->operand), rt = _type(e, ec->result);
        uint16_t W = (uint16_t)((rt.w > _bits_for(xt.w) ? rt.w : _bits_for(xt.w)) + 1);
        dvs_ob_str(o, "(= ");
        _val(e, ec->result, W, rt.s, o);
        dvs_ob_str(o, " (bvadd");
        for (uint16_t i = 0; i < xt.w; i++) {
            dvs_ob_printf(o, " ((_ zero_extend %u) ((_ extract %u %u) ",
                          (unsigned)(W - 1), (unsigned)i, (unsigned)i);
            _val(e, ec->operand, xt.w, xt.s, o);
            dvs_ob_str(o, "))");
        }
        if (xt.w == 1) { dvs_ob_add(o, " ", 1); dvs_smt2_emit_value(o, 0, W); }
        dvs_ob_str(o, "))");
        return;
    }
    case EXPR_CLOG2: {
        /* result == ceil(log2(x)), x read unsigned: the least k with
         * x <= 2^k (0 for x <= 1). */
        ExprClog2 *ec = (ExprClog2 *)_P(e, ref);
        ETy xt = _type(e, ec->operand), rt = _type(e, ec->result);
        if (xt.w > 64) { _fail(e, "clog2 of a value wider than 64 bits"); dvs_ob_str(o, "false"); return; }
        uint16_t XW = (uint16_t)(xt.w + 1);
        uint16_t W = (uint16_t)((rt.w > _bits_for(xt.w) ? rt.w : _bits_for(xt.w)) + 1);
        dvs_ob_str(o, "(= ");
        _val(e, ec->result, W, rt.s, o);
        dvs_ob_add(o, " ", 1);
        for (uint16_t k = 0; k < xt.w; k++) {
            dvs_ob_printf(o, "(ite (bvule ((_ zero_extend 1) ");
            _val(e, ec->operand, xt.w, xt.s, o);
            dvs_ob_printf(o, ") (_ bv%" PRIu64 " %u)) ", ((uint64_t)1) << k, (unsigned)XW);
            dvs_smt2_emit_value(o, k, W);
            dvs_ob_add(o, " ", 1);
        }
        dvs_smt2_emit_value(o, xt.w, W);
        _close(o, xt.w);
        dvs_ob_add(o, ")", 1);
        return;
    }
    case EXPR_ARRAY_SELECT: {
        /* result == base[index]; an index outside the array is false. */
        ExprArraySelect *as = (ExprArraySelect *)_P(e, ref);
        ETy it = _type(e, as->index), rt = _type(e, as->result);
        uint16_t iw = it.w > 32 ? it.w : 32;     /* the index vs an int literal */
        if (as->n_elems == 0) { dvs_ob_str(o, "false"); return; }
        dvs_ob_str(o, "(or");
        for (uint32_t i = 0; i < as->n_elems; i++) {
            ETy vt;
            uint32_t vid = as->base_var_id + i;
            _var_type(e, vid, &vt);
            uint16_t w = rt.w > vt.w ? rt.w : vt.w;
            dvs_ob_str(o, " (and (= ");
            _val(e, as->index, iw, it.s, o);
            dvs_ob_add(o, " ", 1);
            dvs_smt2_emit_value(o, i, iw);
            dvs_ob_str(o, ") (= ");
            _val(e, as->result, w, (uint8_t)(rt.s && vt.s), o);
            dvs_ob_add(o, " ", 1);
            _var_term(e, vid, w, o);
            dvs_ob_str(o, "))");
        }
        dvs_ob_add(o, ")", 1);
        return;
    }
    default:
        _fail(e, "unknown aggregate");
        dvs_ob_str(o, "false");
        return;
    }
}

/* The Bool of a truth-valued node. */
static void _bool_raw(E *e, dvs_expr_t ref, DvsOBuf *o) {
    switch (_kind(e, ref)) {
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)_P(e, ref);
        if (_is_cmp(eb->op)) { _cmp(e, eb->op, eb->lhs, eb->rhs, o); return; }
        dvs_ob_str(o, eb->op == DVS_BIN_AND ? "(and " : "(or ");
        _truth(e, eb->lhs, o);
        dvs_ob_add(o, " ", 1);
        _truth(e, eb->rhs, o);
        dvs_ob_add(o, ")", 1);
        return;
    }
    case EXPR_UNARY: {
        dvs_ob_str(o, "(not ");
        _truth(e, ((ExprUnary *)_P(e, ref))->operand, o);
        dvs_ob_add(o, ")", 1);
        return;
    }
    case EXPR_IN_RANGE: {
        ExprInRange *ir = (ExprInRange *)_P(e, ref);
        dvs_ob_str(o, "(and ");
        _cmp(e, DVS_BIN_GTE, ir->value, ir->lo, o);
        dvs_ob_add(o, " ", 1);
        _cmp(e, DVS_BIN_LTE, ir->value, ir->hi, o);
        dvs_ob_add(o, ")", 1);
        return;
    }
    case EXPR_IN_SET: {
        ExprInSet *is = (ExprInSet *)_P(e, ref);
        const dvs_expr_t *el = (const dvs_expr_t *)(is + 1);
        if (is->n_elems == 0) { dvs_ob_str(o, "false"); return; }
        dvs_ob_str(o, "(or");
        for (uint32_t i = 0; i < is->n_elems; i++) {
            dvs_ob_add(o, " ", 1);
            _cmp(e, DVS_BIN_EQ, is->value, el[i], o);
        }
        dvs_ob_str(o, " false)");
        return;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *ir = (ExprInRanges *)_P(e, ref);
        const dvs_expr_t *los = (const dvs_expr_t *)(ir + 1);
        const dvs_expr_t *his = los + ir->n_ranges;
        dvs_ob_str(o, "(or");
        for (uint32_t i = 0; i < ir->n_ranges; i++) {
            dvs_ob_str(o, " (and ");
            _cmp(e, DVS_BIN_GTE, ir->value, los[i], o);
            dvs_ob_add(o, " ", 1);
            _cmp(e, DVS_BIN_LTE, ir->value, his[i], o);
            dvs_ob_add(o, ")", 1);
        }
        dvs_ob_str(o, " false)");
        return;
    }
    default:
        _aggregate(e, ref, o);
        return;
    }
}

/* A node with more than one parent becomes a define-fun, so a shared DAG is
 * not expanded into a tree. `mode` 0: value at (W, S); 1: Bool. Returns 1 when
 * the reference to the definition was written. */
static int _shared(E *e, dvs_expr_t ref, uint16_t W, uint8_t S, int mode, DvsOBuf *o) {
    uint32_t uses = 0;
    _map_get(&e->uses, (uint64_t)ref + 1, &uses);
    if (uses < 2) return 0;
    ExprKind k = _kind(e, ref);
    if (k == EXPR_VAR || k == EXPR_CONST) return 0;
    uint64_t key = ((uint64_t)ref << 26) ^ ((uint64_t)W << 2) ^ ((uint64_t)S << 1)
                 ^ (uint64_t)mode;
    key = key * 2 + 1;   /* never 0 */
    uint32_t n;
    if (!_map_get(&e->memo, key, &n)) {
        DvsOBuf t = {0};
        if (mode) _bool_raw(e, ref, &t);
        else      _val_raw(e, ref, W, S, &t);
        n = (*e->counter)++;
        if (mode) dvs_ob_printf(e->defs, "(define-fun %s%u () Bool ", e->prefix, n);
        else      dvs_ob_printf(e->defs, "(define-fun %s%u () (_ BitVec %u) ",
                                e->prefix, n, (unsigned)W);
        dvs_ob_add(e->defs, t.p ? t.p : "", t.n);
        dvs_ob_str(e->defs, ")\n");
        dvs_ob_free(&t);
        _map_put(&e->memo, key, n);
    }
    dvs_ob_printf(o, "%s%u", e->prefix, n);
    return 1;
}

static void _bool(E *e, dvs_expr_t ref, DvsOBuf *o) {
    if (_shared(e, ref, 0, 0, 1, o)) return;
    _bool_raw(e, ref, o);
}

/* The truth of any node in its own type. */
static void _truth(E *e, dvs_expr_t ref, DvsOBuf *o) {
    if (ref == EXPR_NULL) { _fail(e, "null expression"); dvs_ob_str(o, "false"); return; }
    if (_is_boolish(e, ref)) { _bool(e, ref, o); return; }
    ETy t = _type(e, ref);
    dvs_ob_str(o, "(not (= ");
    _val(e, ref, t.w, t.s, o);
    dvs_ob_add(o, " ", 1);
    dvs_smt2_emit_value(o, 0, t.w);
    dvs_ob_str(o, "))");
}

static void _val_raw(E *e, dvs_expr_t ref, uint16_t W, uint8_t S, DvsOBuf *o) {
    if (_is_boolish(e, ref)) {
        dvs_ob_str(o, "(ite ");
        _bool(e, ref, o);
        dvs_ob_add(o, " ", 1);
        dvs_smt2_emit_value(o, 1, W);
        dvs_ob_add(o, " ", 1);
        dvs_smt2_emit_value(o, 0, W);
        dvs_ob_add(o, ")", 1);
        return;
    }
    switch (_kind(e, ref)) {
    case EXPR_CONST: {
        ExprConst *ec = (ExprConst *)_P(e, ref);
        uint16_t cw; uint8_t cs;
        dvs_sv_const_type(ec, &cw, &cs);
        int c = _conv_open(o, cw, cs, W);
        if (cw <= 64) {
            dvs_smt2_emit_value(o, ec->value, cw);
        } else {
            /* `value` spells the low 64 bits; wider is its extension by the
             * constant's own signedness. */
            dvs_ob_printf(o, "((_ %s %u) ", cs ? "sign_extend" : "zero_extend",
                          (unsigned)(cw - 64));
            dvs_smt2_emit_value(o, ec->value, 64);
            dvs_ob_add(o, ")", 1);
        }
        _close(o, c);
        return;
    }
    case EXPR_VAR:
        _var_term(e, ((ExprVar *)_P(e, ref))->var_id, W, o);
        return;
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)_P(e, ref);
        dvs_binop_t op = eb->op;
        if (_is_arith(op)) {
            const char *f = NULL;
            switch (op) {
            case DVS_BIN_ADD:  f = "bvadd"; break;
            case DVS_BIN_SUB:  f = "bvsub"; break;
            case DVS_BIN_MUL:  f = "bvmul"; break;
            case DVS_BIN_BAND: f = "bvand"; break;
            case DVS_BIN_BOR:  f = "bvor"; break;
            case DVS_BIN_BXOR: f = "bvxor"; break;
            case DVS_BIN_DIV:  f = S ? "bvsdiv" : "bvudiv"; e->flags |= DVS_EMIT_F_DIV; break;
            case DVS_BIN_MOD:  f = S ? "bvsrem" : "bvurem"; e->flags |= DVS_EMIT_F_DIV; break;
            default: break;
            }
            dvs_ob_printf(o, "(%s ", f);
            _val(e, eb->lhs, W, S, o);
            dvs_ob_add(o, " ", 1);
            _val(e, eb->rhs, W, S, o);
            dvs_ob_add(o, ")", 1);
            return;
        }
        if (op == DVS_BIN_LSHIFT || op == DVS_BIN_RSHIFT || op == DVS_BIN_ASHR) {
            /* The amount is self-determined and read unsigned. */
            int arith = (op == DVS_BIN_ASHR && S);
            const char *f = op == DVS_BIN_LSHIFT ? "bvshl" : arith ? "bvashr" : "bvlshr";
            ETy rt = _type(e, eb->rhs);
            if (rt.w <= W) {
                dvs_ob_printf(o, "(%s ", f);
                _val(e, eb->lhs, W, S, o);
                dvs_ob_add(o, " ", 1);
                int c = _conv_open(o, rt.w, 0, W);
                _val(e, eb->rhs, rt.w, rt.s, o);
                _close(o, c);
                dvs_ob_add(o, ")", 1);
            } else {
                /* An amount wider than the value: >= W shifts everything out. */
                dvs_ob_str(o, "(ite (bvuge ");
                _val(e, eb->rhs, rt.w, rt.s, o);
                dvs_ob_add(o, " ", 1);
                dvs_smt2_emit_value(o, W, rt.w);
                dvs_ob_str(o, ") ");
                if (arith) {
                    dvs_ob_str(o, "(bvashr ");
                    _val(e, eb->lhs, W, S, o);
                    dvs_ob_add(o, " ", 1);
                    dvs_smt2_emit_value(o, W - 1, W);
                    dvs_ob_add(o, ")", 1);
                } else {
                    dvs_smt2_emit_value(o, 0, W);
                }
                dvs_ob_printf(o, " (%s ", f);
                _val(e, eb->lhs, W, S, o);
                dvs_ob_printf(o, " ((_ extract %u 0) ", (unsigned)(W - 1));
                _val(e, eb->rhs, rt.w, rt.s, o);
                dvs_ob_str(o, ")))");
            }
            return;
        }
        _fail(e, "unknown binary operator");
        dvs_smt2_emit_value(o, 0, W);
        return;
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)_P(e, ref);
        if (eu->op != DVS_UN_NEG && eu->op != DVS_UN_INVERT) {
            _fail(e, "unknown unary operator");
            dvs_smt2_emit_value(o, 0, W);
            return;
        }
        dvs_ob_str(o, eu->op == DVS_UN_NEG ? "(bvneg " : "(bvnot ");
        _val(e, eu->operand, W, S, o);
        dvs_ob_add(o, ")", 1);
        return;
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)_P(e, ref);
        dvs_ob_str(o, "(ite ");
        _truth(e, ei->cond, o);
        dvs_ob_add(o, " ", 1);
        _val(e, ei->then_e, W, S, o);
        dvs_ob_add(o, " ", 1);
        _val(e, ei->else_e, W, S, o);
        dvs_ob_add(o, ")", 1);
        return;
    }
    case EXPR_EXTEND:
    case EXPR_SV_CAST: {
        /* The operand's low from_bits bits (in its own type), extended to
         * to_bits (sign_extend), typed (to_bits, operand signedness) for an
         * extend and (to_bits, dst_signed) for a cast. */
        ExprExtend *ee = (ExprExtend *)_P(e, ref);
        ETy ot = _type(e, ee->operand);
        if (ee->from_bits == 0 || ee->to_bits == 0) {
            _fail(e, "zero-width extend");
            dvs_smt2_emit_value(o, 0, W);
            return;
        }
        uint8_t own_s = _kind(e, ref) == EXPR_EXTEND ? ot.s
                      : (((ExprSvCast *)(void *)ee)->dst_signed ? 1 : 0);
        int c3 = _conv_open(o, ee->to_bits, own_s, W);
        int c2 = _conv_open(o, ee->from_bits, ee->sign_extend ? 1 : 0, ee->to_bits);
        int c1 = _conv_open(o, ot.w, ot.s, ee->from_bits);
        _val(e, ee->operand, ot.w, ot.s, o);
        _close(o, c1 + c2 + c3);
        return;
    }
    case EXPR_CAST: {
        /* The operand at the wider of its own width and the cast's, at its
         * own signedness; its low to_bits bits, read at the cast's. */
        ExprCast *ec = (ExprCast *)_P(e, ref);
        ETy ot = _type(e, ec->operand);
        uint16_t ow = ot.w > ec->to_bits ? ot.w : ec->to_bits;
        if (ec->to_bits == 0) { _fail(e, "zero-width cast"); dvs_smt2_emit_value(o, 0, W); return; }
        int c2 = _conv_open(o, ec->to_bits, ec->to_signed ? 1 : 0, W);
        int c1 = _conv_open(o, ow, ot.s, ec->to_bits);
        _val(e, ec->operand, ow, ot.s, o);
        _close(o, c1 + c2);
        return;
    }
    case EXPR_EXTRACT: {
        /* Bits beyond the operand's width are its extension (validator: an
         * arithmetic shift of the signed value). */
        ExprExtract *ex = (ExprExtract *)_P(e, ref);
        ETy ot = _type(e, ex->operand);
        if (ex->hi_bit < ex->lo_bit) { _fail(e, "bad extract"); dvs_smt2_emit_value(o, 0, W); return; }
        uint16_t w = (uint16_t)(ex->hi_bit - ex->lo_bit + 1);
        uint16_t ew = ot.w > ex->hi_bit + 1 ? ot.w : (uint16_t)(ex->hi_bit + 1);
        int c2 = _conv_open(o, w, 0, W);
        dvs_ob_printf(o, "((_ extract %u %u) ", (unsigned)ex->hi_bit, (unsigned)ex->lo_bit);
        int c1 = _conv_open(o, ot.w, ot.s, ew);
        _val(e, ex->operand, ot.w, ot.s, o);
        _close(o, c1 + 1 + c2);
        return;
    }
    case EXPR_CONCAT: {
        ExprConcat *ec = (ExprConcat *)_P(e, ref);
        ETy ht = _type(e, ec->hi), lt = _type(e, ec->lo);
        if (ec->lo_width == 0) { _fail(e, "zero-width concat"); dvs_smt2_emit_value(o, 0, W); return; }
        int c2 = _conv_open(o, (uint16_t)(ht.w + ec->lo_width), 0, W);
        dvs_ob_str(o, "(concat ");
        _val(e, ec->hi, ht.w, ht.s, o);
        dvs_ob_add(o, " ", 1);
        int c1 = _conv_open(o, lt.w, lt.s, ec->lo_width);
        _val(e, ec->lo, lt.w, lt.s, o);
        _close(o, c1 + 1 + c2);
        return;
    }
    default:
        _fail(e, "unknown expression kind");
        dvs_smt2_emit_value(o, 0, W);
        return;
    }
}

static void _val(E *e, dvs_expr_t ref, uint16_t W, uint8_t S, DvsOBuf *o) {
    if (W == 0) W = 1;
    if (ref == EXPR_NULL) { _fail(e, "null expression"); dvs_smt2_emit_value(o, 0, W); return; }
    if (_shared(e, ref, W, S, 0, o)) return;
    _val_raw(e, ref, W, S, o);
}

/* ------------------------------------------------------------------ */
/* Use counts                                                          */
/* ------------------------------------------------------------------ */

static void _count(E *e, dvs_expr_t ref) {
    if (ref == EXPR_NULL) return;
    uint32_t n = 0;
    int seen = _map_get(&e->uses, (uint64_t)ref + 1, &n);
    _map_put(&e->uses, (uint64_t)ref + 1, n + 1);
    if (seen) return;
    switch (_kind(e, ref)) {
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)_P(e, ref);
        _count(e, b->lhs); _count(e, b->rhs); break;
    }
    case EXPR_UNARY: _count(e, ((ExprUnary *)_P(e, ref))->operand); break;
    case EXPR_ITE: {
        ExprITE *i = (ExprITE *)_P(e, ref);
        _count(e, i->cond); _count(e, i->then_e); _count(e, i->else_e); break;
    }
    case EXPR_IN_RANGE: {
        ExprInRange *r = (ExprInRange *)_P(e, ref);
        _count(e, r->value); _count(e, r->lo); _count(e, r->hi); break;
    }
    case EXPR_IN_SET: {
        ExprInSet *s = (ExprInSet *)_P(e, ref);
        const dvs_expr_t *el = (const dvs_expr_t *)(s + 1);
        _count(e, s->value);
        for (uint32_t i = 0; i < s->n_elems; i++) _count(e, el[i]);
        break;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *r = (ExprInRanges *)_P(e, ref);
        const dvs_expr_t *b = (const dvs_expr_t *)(r + 1);
        _count(e, r->value);
        for (uint32_t i = 0; i < 2 * r->n_ranges; i++) _count(e, b[i]);
        break;
    }
    case EXPR_EXTEND: _count(e, ((ExprExtend *)_P(e, ref))->operand); break;
    case EXPR_SV_CAST: _count(e, ((ExprSvCast *)_P(e, ref))->operand); break;
    case EXPR_CAST: _count(e, ((ExprCast *)_P(e, ref))->operand); break;
    case EXPR_EXTRACT: _count(e, ((ExprExtract *)_P(e, ref))->operand); break;
    case EXPR_CONCAT: {
        ExprConcat *c = (ExprConcat *)_P(e, ref);
        _count(e, c->hi); _count(e, c->lo); break;
    }
    /* Aggregates are emitted with their operands repeated; leaves need no
     * definitions. */
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

void dvs_smt2_emit_var_decl(DvsOBuf *out, uint32_t id, uint16_t width,
                            uint8_t is_signed, int64_t lo, int64_t hi) {
    uint16_t w = width ? width : 1;
    dvs_ob_printf(out, "(declare-const v%u (_ BitVec %u))\n", id, (unsigned)w);
    if (w > 64) return;
    /* An unsigned 64-bit variable's bounds are bit patterns. */
    int full;
    if (w == 64) full = is_signed ? (lo == INT64_MIN && hi == INT64_MAX)
                                  : (lo == 0 && hi == -1);
    else if (is_signed)
        full = lo <= -(((int64_t)1) << (w - 1)) && hi >= (((int64_t)1) << (w - 1)) - 1;
    else
        full = lo <= 0 && hi >= (int64_t)((((uint64_t)1) << w) - 1);
    if (full) return;
    if (w < 64) {
        /* An out-of-type bound is clamped to the type. */
        int64_t tmin = is_signed ? -(((int64_t)1) << (w - 1)) : 0;
        int64_t tmax = is_signed ? (((int64_t)1) << (w - 1)) - 1
                                 : (int64_t)((((uint64_t)1) << w) - 1);
        if (lo < tmin) lo = tmin;
        if (hi > tmax) hi = tmax;
        if (lo > hi) { dvs_ob_str(out, "(assert false)\n"); return; }
    }
    const char *le = is_signed ? "bvsle" : "bvule";
    dvs_ob_printf(out, "(assert (and (%s ", le);
    dvs_smt2_emit_value(out, lo, w);
    dvs_ob_printf(out, " v%u) (%s v%u ", id, le, id);
    dvs_smt2_emit_value(out, hi, w);
    dvs_ob_str(out, ")))\n");
}

int dvs_smt2_emit_decls(const dvs_problem_t *p, DvsOBuf *out) {
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = (const VarSpec *)dvs_pool_ptr((dvs_pool_t *)&p->pool, r);
        dvs_smt2_emit_var_decl(out, vs->var_id, vs->width, vs->is_signed, vs->lo, vs->hi);
        r = vs->next;
    }
    return 0;
}

int dvs_smt2_emit_asserts(const dvs_problem_t *p, dvs_emit_var_type_fn fn,
                          void *ud, const char *prefix, uint32_t *def_counter,
                          DvsOBuf *out, unsigned *flags, char *why, size_t why_n) {
    E e;
    memset(&e, 0, sizeof(e));
    e.p = p; e.fn = fn; e.ud = ud;
    e.prefix = prefix ? prefix : "_d";
    uint32_t local_counter = 0;
    e.counter = def_counter ? def_counter : &local_counter;
    e.why = why; e.why_n = why_n;
    if (why && why_n) why[0] = '\0';

    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = (const VarSpec *)dvs_pool_ptr((dvs_pool_t *)&p->pool, r);
        _map_put(&e.vtypes, (uint64_t)vs->var_id + 1,
                 ((uint32_t)(vs->width ? vs->width : 1) << 8) | (vs->is_signed ? 1u : 0u));
        r = vs->next;
    }
    for (dvs_expr_t r = p->constraints_head; r != EXPR_NULL;) {
        const ConstraintSpec *cs = (const ConstraintSpec *)dvs_pool_ptr((dvs_pool_t *)&p->pool, r);
        _count(&e, cs->root);
        r = cs->next;
    }

    DvsOBuf defs = {0}, term = {0};
    e.defs = &defs;
    for (dvs_expr_t r = p->constraints_head; r != EXPR_NULL;) {
        const ConstraintSpec *cs = (const ConstraintSpec *)dvs_pool_ptr((dvs_pool_t *)&p->pool, r);
        dvs_ob_clear(&defs);
        dvs_ob_clear(&term);
        _truth(&e, cs->root, &term);
        if (defs.n) dvs_ob_add(out, defs.p, defs.n);
        dvs_ob_str(out, "(assert ");
        dvs_ob_add(out, term.p ? term.p : "", term.n);
        dvs_ob_str(out, ")\n");
        r = cs->next;
    }
    for (dvs_expr_t r = p->allDiff_head; r != EXPR_NULL;) {
        const AllDiffSpec *ad = (const AllDiffSpec *)dvs_pool_ptr((dvs_pool_t *)&p->pool, r);
        const uint32_t *ids = (const uint32_t *)(ad + 1);
        /* Pairwise, each pair an SV comparison (the variables may differ in
         * width or signedness). */
        dvs_ob_str(out, "(assert (and true");
        for (uint32_t i = 0; i < ad->n_vars; i++) {
            for (uint32_t j = i + 1; j < ad->n_vars; j++) {
                ETy a, b;
                _var_type(&e, ids[i], &a);
                _var_type(&e, ids[j], &b);
                uint16_t w = a.w > b.w ? a.w : b.w;
                dvs_ob_str(out, " (distinct ");
                _var_term(&e, ids[i], w, out);
                dvs_ob_add(out, " ", 1);
                _var_term(&e, ids[j], w, out);
                dvs_ob_add(out, ")", 1);
            }
        }
        dvs_ob_str(out, "))\n");
        r = ad->next;
    }
    dvs_ob_free(&defs);
    dvs_ob_free(&term);
    _map_free(&e.vtypes); _map_free(&e.types); _map_free(&e.uses); _map_free(&e.memo);
    if (flags) *flags |= e.flags;
    return (e.flags & DVS_EMIT_F_UNSUPPORTED) ? -1 : 0;
}

int dvs_problem_write_smt2(const dvs_problem_t *p, FILE *out) {
    if (!p || !out) return -1;
    DvsOBuf b = {0};
    unsigned flags = 0;
    char why[128];
    dvs_ob_str(&b, "(set-logic QF_BV)\n");
    dvs_smt2_emit_decls(p, &b);
    int rc = dvs_smt2_emit_asserts(p, NULL, NULL, "_d", NULL, &b, &flags, why, sizeof(why));
    if (flags & DVS_EMIT_F_DIV)
        dvs_ob_str(&b, "; note: divides; SMT-LIB defines x/0, SystemVerilog does not\n");
    if (rc != 0) dvs_ob_printf(&b, "; error: not translated: %s\n", why);
    dvs_ob_str(&b, "(check-sat)\n");
    size_t w = fwrite(b.p, 1, b.n, out);
    int ok = (w == b.n) ? rc : -1;
    dvs_ob_free(&b);
    return ok;
}
