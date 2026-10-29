#include "dvs_shape.h"

#include <stdlib.h>
#include <string.h>

/* Large enough for any problem the engines handle quickly; a bigger one is
 * reported truncated and classified on what was seen. */
#define SHAPE_NODE_CAP 4000000u

typedef struct {
    dvs_problem_t *p;
    uint8_t       *seen;      /* bitmap over pool byte offsets */
    size_t         seen_bits;
    dvs_expr_t    *stack;
    uint32_t       sp, cap;
    dvs_shape_t   *out;
} Walk;

static int _mark(Walk *w, dvs_expr_t r) {
    if ((size_t)r >= w->seen_bits) return 0;
    uint8_t m = (uint8_t)(1u << (r & 7));
    if (w->seen[r >> 3] & m) return 0;
    w->seen[r >> 3] |= m;
    return 1;
}

static int _push(Walk *w, dvs_expr_t r) {
    if (r == EXPR_NULL || !_mark(w, r)) return 0;
    if (w->sp == w->cap) {
        uint32_t nc = w->cap ? w->cap * 2 : 1024;
        dvs_expr_t *ns = (dvs_expr_t *)realloc(w->stack, nc * sizeof *ns);
        if (!ns) return -1;
        w->stack = ns;
        w->cap = nc;
    }
    w->stack[w->sp++] = r;
    return 0;
}

static ExprKind _kind(dvs_problem_t *p, dvs_expr_t r) {
    return *(const ExprKind *)POOL_PTR(p, r);
}

/* Visit one node: classify it and push its operands. */
static int _visit(Walk *w, dvs_expr_t r) {
    dvs_problem_t *p = w->p;
    dvs_shape_t *o = w->out;
    void *n = POOL_PTR(p, r);
    int rc = 0;
    switch (*(const ExprKind *)n) {
    case EXPR_CONST:
    case EXPR_CONST_WIDE:
        return 0;
    case EXPR_VAR:
        o->n_vars++;
        return 0;
    case EXPR_BINARY: {
        const ExprBinary *b = (const ExprBinary *)n;
        switch (b->op) {
        case DVS_BIN_ADD: case DVS_BIN_SUB:                  o->n_arith++; break;
        case DVS_BIN_MUL: case DVS_BIN_DIV: case DVS_BIN_MOD: o->n_mul++;   break;
        case DVS_BIN_BAND: case DVS_BIN_BOR: case DVS_BIN_BXOR:
        case DVS_BIN_LSHIFT: case DVS_BIN_RSHIFT: case DVS_BIN_ASHR:
                                                              o->n_bit++;   break;
        case DVS_BIN_EQ: case DVS_BIN_NEQ:                    o->n_eq++;    break;
        case DVS_BIN_LT: case DVS_BIN_LTE: case DVS_BIN_GT: case DVS_BIN_GTE:
                                                              o->n_cmp++;   break;
        case DVS_BIN_AND: case DVS_BIN_OR:                    o->n_bool++;  break;
        default:                                              o->n_other++; break;
        }
        if (_push(w, b->lhs) < 0 || _push(w, b->rhs) < 0) rc = -1;
        break;
    }
    case EXPR_UNARY: {
        const ExprUnary *u = (const ExprUnary *)n;
        if (u->op == DVS_UN_NEG)         o->n_arith++;
        else if (u->op == DVS_UN_INVERT) o->n_bit++;
        else                             o->n_bool++;
        rc = _push(w, u->operand);
        break;
    }
    case EXPR_ITE: {
        const ExprITE *e = (const ExprITE *)n;
        o->n_bool++;
        if (_push(w, e->cond) < 0 || _push(w, e->then_e) < 0 || _push(w, e->else_e) < 0)
            rc = -1;
        break;
    }
    case EXPR_IN_RANGE: {
        const ExprInRange *e = (const ExprInRange *)n;
        o->n_cmp++;
        if (_push(w, e->value) < 0 || _push(w, e->lo) < 0 || _push(w, e->hi) < 0) rc = -1;
        break;
    }
    case EXPR_IN_SET: {
        const ExprInSet *e = (const ExprInSet *)n;
        o->n_cmp++;
        rc = _push(w, e->value);
        dvs_expr_t *el = expr_in_set_elems(p, r);
        for (uint32_t i = 0; rc == 0 && i < e->n_elems; i++) rc = _push(w, el[i]);
        break;
    }
    case EXPR_IN_RANGES: {
        const ExprInRanges *e = (const ExprInRanges *)n;
        o->n_cmp++;
        rc = _push(w, e->value);
        dvs_expr_t *lo = expr_in_ranges_los(p, r), *hi = expr_in_ranges_his(p, r);
        for (uint32_t i = 0; rc == 0 && i < e->n_ranges; i++)
            if (_push(w, lo[i]) < 0 || _push(w, hi[i]) < 0) rc = -1;
        break;
    }
    case EXPR_EXTEND:
        o->n_bit++;
        rc = _push(w, ((const ExprExtend *)n)->operand);
        break;
    case EXPR_EXTRACT:
        o->n_bit++;
        rc = _push(w, ((const ExprExtract *)n)->operand);
        break;
    case EXPR_CONCAT: {
        const ExprConcat *e = (const ExprConcat *)n;
        o->n_bit++;
        if (_push(w, e->hi) < 0 || _push(w, e->lo) < 0) rc = -1;
        break;
    }
    case EXPR_SV_CAST:
        o->n_other++;
        rc = _push(w, ((const ExprSvCast *)n)->operand);
        break;
    case EXPR_CAST:
        o->n_other++;
        rc = _push(w, ((const ExprCast *)n)->operand);
        break;
    default:
        /* sum, countones, clog2, array select: their operands are variables,
         * which do not change the classification. */
        o->n_other++;
        break;
    }
    if (*(const ExprKind *)n != EXPR_VAR && *(const ExprKind *)n != EXPR_CONST)
        o->n_nodes++;
    return rc;
}

/* Count the conjuncts under a top-level AND spine. Iterative:
 * a long conjunction is a deep left spine. */
static int _top(Walk *w, dvs_expr_t root) {
    dvs_problem_t *p = w->p;
    dvs_expr_t *st = NULL;
    uint32_t n = 0, cap = 0;
    int rc = 0;
    dvs_expr_t cur = root;
    for (;;) {
        if (_kind(p, cur) == EXPR_BINARY
                && ((const ExprBinary *)POOL_PTR(p, cur))->op == DVS_BIN_AND) {
            const ExprBinary *b = (const ExprBinary *)POOL_PTR(p, cur);
            if (n == cap) {
                uint32_t nc = cap ? cap * 2 : 64;
                dvs_expr_t *ns = (dvs_expr_t *)realloc(st, nc * sizeof *ns);
                if (!ns) { rc = -1; break; }
                st = ns;
                cap = nc;
            }
            st[n++] = b->rhs;
            cur = b->lhs;
            continue;
        }
        w->out->n_constraints++;
        if (n == 0) break;
        cur = st[--n];
    }
    free(st);
    return rc;
}

int dvs_shape_compute(dvs_problem_t *p, dvs_shape_t *out) {
    memset(out, 0, sizeof *out);
    Walk w;
    memset(&w, 0, sizeof w);
    w.p = p;
    w.out = out;
    w.seen_bits = (size_t)p->pool.used + 1;
    w.seen = (uint8_t *)calloc((w.seen_bits + 7) / 8, 1);
    if (!w.seen) return -1;
    int rc = 0;
    for (dvs_expr_t c = p->constraints_head; c != EXPR_NULL && rc == 0; ) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(p, c);
        rc = _top(&w, cs->root);
        if (rc == 0) rc = _push(&w, cs->root);
        while (rc == 0 && w.sp > 0) {
            if (out->n_nodes >= SHAPE_NODE_CAP) { out->truncated = 1; w.sp = 0; break; }
            rc = _visit(&w, w.stack[--w.sp]);
        }
        c = cs->next;
    }
    free(w.stack);
    free(w.seen);
    return rc;
}

const char *dvs_shape_route(const dvs_shape_t *sh) {
    if (sh->n_mul)                                    return "multiplier";
    /* The front end binds every let and define-fun to a variable through an
     * equality, so equalities say nothing about the shape; orderings do.
     * Randomization is mostly orderings, circuits mostly bit operations. */
    if ((uint64_t)sh->n_bit + sh->n_arith >= 3ull * sh->n_cmp) return "bit-level";
    return NULL;
}
