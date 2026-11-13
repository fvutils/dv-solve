/**
 * dvs_tmpl.c -- problems with arrays whose size is given on each call: the
 * classification of arrays, the expansion of a template for given sizes,
 * and the per-element problems of element-local arrays. See dvs_tmpl.h.
 *
 * Expansion rebuilds the template through the builder:
 *  - scalar variables keep their ids; the elements of each expanded array
 *    follow, array by array, in index order;
 *  - EXPR_ARRAY_SIZE and a bound EXPR_LOOP_INDEX become 32-bit signed
 *    constants;
 *  - EXPR_ELEM with an index that folds to a constant becomes that element,
 *    or 0 out of range; with any other index, a choice over the elements;
 *  - EXPR_REDUCE is folded over the elements; a foreach is instantiated once
 *    per element.
 * A node whose value does not depend on a loop index is built once per
 * expansion; one that does is built again when an index it reads changes.
 *
 * The size of an element-local array is a STATE variable of the expansion,
 * not a constant, so a new size of such an array needs no new expansion.
 *
 * Random sizes (IEEE 1800-2023 18.5.8.1: solved before the elements) are a
 * problem of their own, derived from the template: the scalars, a variable
 * per array size (RAND when random, else STATE), and the top-level
 * constraints that read no element.
 */
#include <stdlib.h>
#include <string.h>
#include "dvs_tmpl.h"
#include "dvs_builder.h"
#include "dvs_sv.h"           /* dvs_sv_const_type */
#include "dvs_stackinfo.h"

/** Most elements one expanded problem may have (all its arrays together). */
#define TMPL_MAX_ELEMS   65536u
/** Most elements of an array of random size: a size solved above it fails the
 *  call (DVS_RANDOMIZE_TOO_LARGE), to catch a size left unbounded by mistake. */
#define TMPL_MAX_SIZE    65535u

/** Expanded problems kept per handle. */
#define TMPL_LRU         4u
#define NO_VAR           0xFFFFFFFFu

/* What a variable of an element-local problem reads on each call. */
enum { SRC_ELEM, SRC_INDEX, SRC_SCALAR, SRC_STATE_ELEM, SRC_SIZE };
typedef struct {
    uint8_t  src;
    uint32_t id;           /* the scalar var or array it reads */
} local_src_t;

typedef struct {
    uint32_t     *sizes;   /* the key: size of every array (key arrays only) */
    uint32_t     *base;    /* per array: the first element var id when
                            * expanded, the size var id when element-local */
    dvs_handle_t *h;
    uint32_t      n_refs;
    uint64_t      used;    /* LRU stamp */
} expansion_t;

typedef struct {
    uint32_t      width;
    uint8_t       is_signed, kind, order, local;
    uint8_t       in_key;  /* the expansion depends on its size: expanded */
    uint8_t       size_random;
    uint32_t      slot;    /* a slot array's slot, or DVS_NO_SLOT */
    dvs_handle_t *h;       /* element-local: the one-element problem */
    local_src_t  *srcs;
    uint32_t      n_srcs;
    uint8_t      *out;     /* element-local: this call's values, copied out on
                            * success alone */
    size_t        cap_out;
} array_t;

struct dvs_tmpl_s {
    const dvs_problem_t *p;
    uint32_t     n_vars;   /* one past the highest scalar var id */
    uint32_t    *vwidth;
    uint8_t     *vsigned, *vkind;
    uint32_t     n_arrays;
    array_t     *arr;
    uint32_t     n_slots;
    expansion_t  lru[TMPL_LRU];
    uint64_t     stamp;
    uint32_t     n_expansions;
    int          engine;
    dvs_ref_t   *refs;     /* scratch: refs of the problem being solved */
    uint32_t     cap_refs;
    int          any_size_random;
    dvs_handle_t *size_h;  /* the problem of the random sizes */
    dvs_stack_guard_t sg;  /* the recursive walks' guard, written through a
                            * const template too (dvs_stackinfo.h) */
};

/* Too little stack left for a walk to go a level deeper. */
static int deep(const dvs_tmpl_t *t) { return dvs_stack_low((dvs_stack_guard_t *)&t->sg); }

/* ------------------------------------------------------------------ */
/* Types and constant folding                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t w;
    uint8_t  sg;
} ty_t;

static uint64_t mask_w(uint64_t v, uint32_t w) {
    return w >= 64 ? v : v & ((1ull << w) - 1);
}

static int64_t sext(uint64_t v, uint32_t w) {
    if (w >= 64) return (int64_t)v;
    v = mask_w(v, w);
    return (v >> (w - 1)) & 1 ? (int64_t)(v | (~0ull << w)) : (int64_t)v;
}

#define NODE(t, r)  ((const void *)POOL_PTR((t)->p, (r)))
#define KIND(t, r)  (*(const uint32_t *)NODE(t, r))

static ty_t type_of(const dvs_tmpl_t *t, dvs_expr_t r) {
    /* Each node's type is a leaf's or one operand's, so a loop down that
     * operand: an extend above fixes the width, a concat above adds its low
     * part and makes it unsigned. */
    uint32_t add = 0, wfix = 0;
    int fixed = 0, unsg = 0;
    for (;;) {
        const void *n = NODE(t, r);
        ty_t ty = {1, 0};
        dvs_expr_t next = EXPR_NULL;
        switch (KIND(t, r)) {
        case EXPR_CONST: dvs_sv_const_type(n, &ty.w, &ty.sg); break;
        case EXPR_CONST_WIDE:
            ty.w = ((const ExprConstWide *)n)->width;
            ty.sg = ((const ExprConstWide *)n)->is_signed;
            break;
        case EXPR_VAR: {
            uint32_t v = ((const ExprVar *)n)->var_id;
            if (v < t->n_vars) { ty.w = t->vwidth[v]; ty.sg = t->vsigned[v]; }
            break;
        }
        case EXPR_BINARY: {
            uint32_t op = ((const ExprBinary *)n)->op;
            if (op < DVS_BIN_EQ || op == DVS_BIN_ASHR) next = ((const ExprBinary *)n)->lhs;
            break;
        }
        case EXPR_UNARY:
            if (((const ExprUnary *)n)->op != DVS_UN_NOT) next = ((const ExprUnary *)n)->operand;
            break;
        case EXPR_ITE: next = ((const ExprITE *)n)->then_e; break;
        case EXPR_EXTEND:
            if (!fixed) { wfix = ((const ExprExtend *)n)->to_bits + add; fixed = 1; }
            next = ((const ExprExtend *)n)->operand;
            break;
        case EXPR_SV_CAST:
            ty.w = ((const ExprSvCast *)n)->to_bits;
            ty.sg = ((const ExprSvCast *)n)->dst_signed;
            break;
        case EXPR_CAST:
            ty.w = ((const ExprCast *)n)->to_bits;
            ty.sg = ((const ExprCast *)n)->to_signed;
            break;
        case EXPR_EXTRACT:
            ty.w = ((const ExprExtract *)n)->hi_bit - ((const ExprExtract *)n)->lo_bit + 1;
            break;
        case EXPR_CONCAT:
            if (!fixed) add += ((const ExprConcat *)n)->lo_width;
            unsg = 1;
            next = ((const ExprConcat *)n)->hi;
            break;
        case EXPR_ARRAY_SIZE:
        case EXPR_LOOP_INDEX: ty.w = 32; ty.sg = 1; break;
        case EXPR_ELEM: {
            const array_t *a = &t->arr[((const ExprElem *)n)->array_id];
            ty.w = a->width;
            ty.sg = a->is_signed;
            break;
        }
        case EXPR_REDUCE: next = ((const ExprReduce *)n)->body; break;
        default: break;   /* in-range/in-set/in-ranges and the relations: Boolean */
        }
        if (next == EXPR_NULL) {
            ty.w = fixed ? wfix : ty.w + add;
            if (unsg) ty.sg = 0;
            return ty;
        }
        r = next;
    }
}

/* The binding of loop levels during an expansion. */
typedef struct {
    int32_t  val[DVS_MAX_LOOP_LEVELS];
    uint32_t bound;                         /* bit per level */
    uint64_t changed[DVS_MAX_LOOP_LEVELS];  /* tick of the last rebinding */
    uint64_t tick;
} env_t;

static void env_bind(env_t *e, uint32_t level, int32_t v) {
    e->val[level] = v;
    e->bound |= 1u << level;
    e->changed[level] = ++e->tick;
}

/* Fold `r` to a constant of at most 64 bits under `e`, with sizes `sizes`:
 * 0 and *v (masked to its width), or -1 if it is not constant. */
static int fold(const dvs_tmpl_t *t, const env_t *e, const uint32_t *sizes, dvs_expr_t r,
                uint64_t *v, ty_t *ty) {
    if (deep(t)) return -1;
    const void *n = NODE(t, r);
    uint64_t a, b;
    ty_t ta, tb;
    switch (KIND(t, r)) {
    case EXPR_CONST:
        dvs_sv_const_type(n, &ty->w, &ty->sg);
        if (ty->w > 64) return -1;
        *v = mask_w((uint64_t)((const ExprConst *)n)->value, ty->w);
        return 0;
    case EXPR_LOOP_INDEX: {
        uint32_t l = ((const ExprLoopIndex *)n)->level;
        if (!((e->bound >> l) & 1)) return -1;
        *v = (uint32_t)e->val[l];
        ty->w = 32; ty->sg = 1;
        return 0;
    }
    case EXPR_ARRAY_SIZE:
        /* The size of an element-local array is a STATE variable */
        if (!sizes || t->arr[((const ExprArraySize *)n)->array_id].local) return -1;
        *v = sizes[((const ExprArraySize *)n)->array_id];
        ty->w = 32; ty->sg = 1;
        return 0;
    case EXPR_UNARY: {
        const ExprUnary *u = n;
        if (fold(t, e, sizes, u->operand, &a, &ta)) return -1;
        *ty = ta;
        if (u->op == DVS_UN_NOT) { *v = a == 0; ty->w = 1; ty->sg = 0; return 0; }
        *v = mask_w(u->op == DVS_UN_NEG ? 0 - a : ~a, ta.w);
        return 0;
    }
    case EXPR_ITE: {
        const ExprITE *it = n;
        if (fold(t, e, sizes, it->cond, &a, &ta)) return -1;
        return fold(t, e, sizes, a ? it->then_e : it->else_e, v, ty);
    }
    case EXPR_EXTEND:
    case EXPR_SV_CAST: {
        const ExprSvCast *c = n;   /* EXPR_EXTEND has the same leading fields */
        dvs_expr_t op = KIND(t, r) == EXPR_EXTEND ? ((const ExprExtend *)n)->operand
                                                  : c->operand;
        if (c->to_bits > 64 || c->from_bits > 64 || c->from_bits == 0
                || fold(t, e, sizes, op, &a, &ta))
            return -1;
        *v = mask_w(c->sign_extend ? (uint64_t)sext(a, c->from_bits)
                                   : mask_w(a, c->from_bits), c->to_bits);
        ty->w = c->to_bits;
        ty->sg = KIND(t, r) == EXPR_EXTEND ? ta.sg : c->dst_signed;
        return 0;
    }
    case EXPR_EXTRACT: {
        const ExprExtract *x = n;
        if (x->hi_bit >= 64 || x->hi_bit < x->lo_bit || fold(t, e, sizes, x->operand, &a, &ta))
            return -1;
        ty->w = x->hi_bit - x->lo_bit + 1;
        ty->sg = 0;
        *v = mask_w(a >> x->lo_bit, ty->w);
        return 0;
    }
    case EXPR_BINARY: {
        const ExprBinary *bn = n;
        if (fold(t, e, sizes, bn->lhs, &a, &ta) || fold(t, e, sizes, bn->rhs, &b, &tb))
            return -1;
        const uint32_t w = ta.w;
        const int sg = ta.sg && tb.sg;
        const int64_t sa = sext(a, w), sb = sext(b, tb.w);
        *ty = ta;
        switch (bn->op) {
        case DVS_BIN_ADD: *v = mask_w(a + b, w); return 0;
        case DVS_BIN_SUB: *v = mask_w(a - b, w); return 0;
        case DVS_BIN_MUL: *v = mask_w(a * b, w); return 0;
        case DVS_BIN_DIV:
        case DVS_BIN_MOD:
            if (b == 0 || (sg && sb == -1)) return -1;
            if (sg) *v = (uint64_t)(bn->op == DVS_BIN_DIV ? sa / sb : sa % sb);
            else *v = bn->op == DVS_BIN_DIV ? a / b : a % b;
            *v = mask_w(*v, w);
            return 0;
        case DVS_BIN_BAND: *v = a & b; return 0;
        case DVS_BIN_BOR:  *v = a | b; return 0;
        case DVS_BIN_BXOR: *v = a ^ b; return 0;
        case DVS_BIN_LSHIFT: *v = b >= w ? 0 : mask_w(a << b, w); return 0;
        case DVS_BIN_RSHIFT: *v = b >= w ? 0 : a >> b; return 0;
        case DVS_BIN_ASHR:
            if (!ta.sg) { *v = b >= w ? 0 : a >> b; return 0; }
            *v = mask_w((uint64_t)(sa >> (b >= w ? w - 1 : b)), w);
            return 0;
        default: break;
        }
        ty->w = 1;
        ty->sg = 0;
        switch (bn->op) {
        case DVS_BIN_EQ:  *v = a == b; return 0;
        case DVS_BIN_NEQ: *v = a != b; return 0;
        case DVS_BIN_LT:  *v = sg ? sa < sb : a < b; return 0;
        case DVS_BIN_LTE: *v = sg ? sa <= sb : a <= b; return 0;
        case DVS_BIN_GT:  *v = sg ? sa > sb : a > b; return 0;
        case DVS_BIN_GTE: *v = sg ? sa >= sb : a >= b; return 0;
        case DVS_BIN_AND: *v = a && b; return 0;
        case DVS_BIN_OR:  *v = a || b; return 0;
        default: return -1;
        }
    }
    default: return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Walking the template                                                */
/* ------------------------------------------------------------------ */

static uint32_t slot_of(dvs_expr_t r) {
    return (r - (uint32_t)sizeof(dvs_pool_t)) / DVS_IMAGE_ALIGN;
}

/* The children of `r`, as the image check reads them. */
static uint32_t kids_of(const dvs_tmpl_t *t, dvs_expr_t r, dvs_expr_t *k, uint32_t cap) {
    const void *n = NODE(t, r);
    uint32_t c = 0;
#define KID(x) do { if (c < cap) k[c] = (x); c++; } while (0)
    switch (KIND(t, r)) {
    case EXPR_BINARY: KID(((const ExprBinary *)n)->lhs); KID(((const ExprBinary *)n)->rhs); break;
    case EXPR_UNARY: KID(((const ExprUnary *)n)->operand); break;
    case EXPR_ITE:
        KID(((const ExprITE *)n)->cond); KID(((const ExprITE *)n)->then_e);
        KID(((const ExprITE *)n)->else_e);
        break;
    case EXPR_IN_RANGE:
        KID(((const ExprInRange *)n)->value); KID(((const ExprInRange *)n)->lo);
        KID(((const ExprInRange *)n)->hi);
        break;
    case EXPR_IN_SET: {
        const ExprInSet *s = n;
        KID(s->value);
        for (uint32_t i = 0; i < s->n_elems; i++) KID(((const dvs_expr_t *)(s + 1))[i]);
        break;
    }
    case EXPR_IN_RANGES: {
        const ExprInRanges *s = n;
        KID(s->value);
        for (uint32_t i = 0; i < 2 * s->n_ranges; i++) KID(((const dvs_expr_t *)(s + 1))[i]);
        break;
    }
    case EXPR_SUM: {
        const ExprSum *s = n;
        KID(s->result);
        for (uint32_t i = 0; i < s->n_vars; i++) KID(((const dvs_expr_t *)(s + 1))[i]);
        break;
    }
    case EXPR_EXTEND: KID(((const ExprExtend *)n)->operand); break;
    case EXPR_SV_CAST: KID(((const ExprSvCast *)n)->operand); break;
    case EXPR_CAST: KID(((const ExprCast *)n)->operand); break;
    case EXPR_EXTRACT: KID(((const ExprExtract *)n)->operand); break;
    case EXPR_CONCAT: KID(((const ExprConcat *)n)->hi); KID(((const ExprConcat *)n)->lo); break;
    case EXPR_COUNTONES:
        KID(((const ExprCountones *)n)->result); KID(((const ExprCountones *)n)->operand);
        break;
    case EXPR_CLOG2:
        KID(((const ExprClog2 *)n)->result); KID(((const ExprClog2 *)n)->operand);
        break;
    case EXPR_ARRAY_SELECT:
        KID(((const ExprArraySelect *)n)->result); KID(((const ExprArraySelect *)n)->index);
        break;
    case EXPR_ELEM: KID(((const ExprElem *)n)->index); break;
    case EXPR_REDUCE: KID(((const ExprReduce *)n)->body); break;
    default: break;
    }
#undef KID
    return c;
}

/* Visit every node under `r` once per walk (`seen` holds the walk's mark). */
typedef int (*visit_fn)(const dvs_tmpl_t *t, dvs_expr_t r, void *arg);

static int walk(const dvs_tmpl_t *t, dvs_expr_t r, uint32_t *seen, uint32_t mark,
                visit_fn fn, void *arg) {
    uint32_t s = slot_of(r);
    if (seen[s] == mark) return 0;
    if (deep(t)) return -1;
    seen[s] = mark;
    if (fn(t, r, arg)) return -1;
    dvs_expr_t k[4], *all = k;
    uint32_t nk = kids_of(t, r, k, 4);
    if (nk > 4) {
        if (!(all = malloc(nk * sizeof *all))) return -1;
        kids_of(t, r, all, nk);
    }
    int rc = 0;
    for (uint32_t i = 0; i < nk && !rc; i++) rc = walk(t, all[i], seen, mark, fn, arg);
    if (all != k) free(all);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t array, level;
} body_arg_t;

/* A node of a body that keeps array `array` element-local. */
static int body_ok(const dvs_tmpl_t *t, dvs_expr_t r, void *arg) {
    const body_arg_t *ba = arg;
    const void *n = NODE(t, r);
    switch (KIND(t, r)) {
    case EXPR_VAR: {
        uint32_t v = ((const ExprVar *)n)->var_id;
        return v < t->n_vars && t->vkind[v] == DVS_VAR_STATE ? 0 : -1;
    }
    case EXPR_LOOP_INDEX: return ((const ExprLoopIndex *)n)->level == ba->level ? 0 : -1;
    case EXPR_ELEM: {
        const ExprElem *x = n;
        if (x->array_id != ba->array && t->arr[x->array_id].kind != DVS_VAR_STATE) return -1;
        return KIND(t, x->index) == EXPR_LOOP_INDEX
                       && ((const ExprLoopIndex *)NODE(t, x->index))->level == ba->level
                   ? 0 : -1;
    }
    case EXPR_REDUCE:
    case EXPR_ARRAY_SELECT:
    case EXPR_SUM:
    case EXPR_COUNTONES:
    case EXPR_CLOG2: return -1;
    default: return 0;
    }
}

/* Outside the local bodies: every array an element or reduction reads is
 * expanded. */
static int mark_outside(const dvs_tmpl_t *t, dvs_expr_t r, void *arg) {
    (void)arg;
    const void *n = NODE(t, r);
    array_t *arr = ((dvs_tmpl_t *)t)->arr;
    switch (KIND(t, r)) {
    case EXPR_ELEM: arr[((const ExprElem *)n)->array_id].local = 0; break;
    case EXPR_REDUCE: arr[((const ExprReduce *)n)->array_id].local = 0; break;
    default: break;
    }
    return 0;
}

static const ForeachSpec *fe_at(const dvs_tmpl_t *t, dvs_expr_t r) {
    return (const ForeachSpec *)POOL_PTR(t->p, r);
}

static int fe_has_children(const dvs_tmpl_t *t, dvs_expr_t f) {
    for (dvs_expr_t r = t->p->foreachs_head; t->p->n_foreachs && r != EXPR_NULL;
         r = fe_at(t, r)->next)
        if (fe_at(t, r)->parent == f) return 1;
    return 0;
}

/* Run `fn` over every root of a foreach body. */
static int fe_body_walk(const dvs_tmpl_t *t, const ForeachSpec *fe, uint32_t *seen,
                        uint32_t mark, visit_fn fn, void *arg) {
    for (dvs_expr_t r = fe->n_constraints ? fe->constraints_head : EXPR_NULL; r != EXPR_NULL;) {
        const ConstraintSpec *c = POOL_PTR(t->p, r);
        if (walk(t, c->root, seen, mark, fn, arg)) return -1;
        r = c->next;
    }
    for (dvs_expr_t r = fe->n_softs ? fe->softs_head : EXPR_NULL; r != EXPR_NULL;) {
        const SoftSpec *s = POOL_PTR(t->p, r);
        if (walk(t, s->root, seen, mark, fn, arg)) return -1;
        r = s->next;
    }
    for (dvs_expr_t r = fe->n_dists ? fe->dists_head : EXPR_NULL; r != EXPR_NULL;) {
        const DistSpec *d = POOL_PTR(t->p, r);
        if (walk(t, d->index, seen, mark, fn, arg)) return -1;
        r = d->next;
    }
    return 0;
}

/* A top-level foreach without nested ones, over a RAND array, whose body
 * reads only element [i] of it, STATE values and the index; its
 * distributions are on that element. */
static int fe_is_local(const dvs_tmpl_t *t, dvs_expr_t f, uint32_t *seen, uint32_t *mark) {
    const ForeachSpec *fe = fe_at(t, f);
    if (fe->parent != EXPR_NULL || fe_has_children(t, f)
            || t->arr[fe->array_id].kind != DVS_VAR_RAND)
        return 0;
    for (dvs_expr_t r = fe->n_dists ? fe->dists_head : EXPR_NULL; r != EXPR_NULL;) {
        const DistSpec *d = POOL_PTR(t->p, r);
        if (d->var_id != fe->array_id || KIND(t, d->index) != EXPR_LOOP_INDEX
                || ((const ExprLoopIndex *)NODE(t, d->index))->level != fe->level)
            return 0;
        r = d->next;
    }
    body_arg_t ba = {fe->array_id, fe->level};
    return fe_body_walk(t, fe, seen, ++*mark, body_ok, &ba) == 0;
}

static int classify(dvs_tmpl_t *t) {
    const dvs_problem_t *p = t->p;
    uint32_t *seen = calloc(t->n_slots ? t->n_slots : 1, sizeof *seen);
    if (!seen) return -1;
    uint32_t mark = 0;
    for (uint32_t a = 0; a < t->n_arrays; a++) t->arr[a].local = t->arr[a].kind == DVS_VAR_RAND;

    /* A foreach that is not local expands its array. */
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;
         f = fe_at(t, f)->next)
        if (!fe_is_local(t, f, seen, &mark)) t->arr[fe_at(t, f)->array_id].local = 0;
    /* A whole array in an all-different expands it. */
    for (dvs_expr_t r = p->allDiff_head; r != EXPR_NULL;) {
        const AllDiffSpec *ad = POOL_PTR(p, r);
        const uint32_t *ids = (const uint32_t *)(ad + 1);
        for (uint32_t i = 0; i < ad->n_vars; i++)
            if (ids[i] & DVS_ARRAY_MEMBER_BIT) t->arr[ids[i] & ~DVS_ARRAY_MEMBER_BIT].local = 0;
        r = ad->next;
    }
    /* Then the roots outside the local foreachs: the top-level constraints
     * and the bodies of foreachs over expanded arrays. */
    ++mark;
    int rc = 0;
    for (dvs_expr_t r = p->constraints_head; r != EXPR_NULL && !rc;) {
        const ConstraintSpec *c = POOL_PTR(p, r);
        rc = walk(t, c->root, seen, mark, mark_outside, NULL);
        r = c->next;
    }
    for (dvs_expr_t r = p->softs_head; r != EXPR_NULL && !rc;) {
        const SoftSpec *s = POOL_PTR(p, r);
        rc = walk(t, s->root, seen, mark, mark_outside, NULL);
        r = s->next;
    }
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL && !rc;
         f = fe_at(t, f)->next) {
        const ForeachSpec *fe = fe_at(t, f);
        if (t->arr[fe->array_id].local) continue;
        rc = fe_body_walk(t, fe, seen, mark, mark_outside, NULL);
    }
    free(seen);
    for (uint32_t a = 0; a < t->n_arrays; a++)
        if (!t->arr[a].local) t->arr[a].in_key = 1;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Rebuilding expressions                                              */
/* ------------------------------------------------------------------ */

enum { XP_EXPAND, XP_LOCAL, XP_SIZES, XP_ASSEMBLE };

typedef struct {
    dvs_tmpl_t    *t;
    dvs_builder_t *b;
    int            mode;     /* XP_*: an expansion, an element-local problem,
                              * the problem of the random sizes, or a part of an
                              * assembled problem */
    env_t          env;
    const uint32_t *sizes;   /* expansion: every array's size; else NULL */
    const uint32_t *base;    /* expansion: first element var per array */
    /* memo, per template slot */
    dvs_expr_t    *memo;
    uint64_t      *memo_tick;
    uint8_t       *dep_done;
    uint8_t       *dep;      /* bit per loop level the node reads */
    /* element-local problems: the array and level, and the var of each source */
    uint32_t       local_array, local_level;
    uint32_t      *map_scalar, *map_elem, *map_size, map_index;
    local_src_t   *srcs;
    uint32_t       n_srcs;
    int            err;      /* a DVS_RANDOMIZE_* failure */
    /* a part of an assembled problem: where its vars, arrays and soft
     * priorities go (dvs_tmpl_assemble) */
    const uint32_t *var_map;
    uint32_t       array_base, prio_base;
    uint32_t      *prios, n_prios;  /* its soft priorities, sorted, distinct */
    const dvs_asm_slots_t *sa;      /* its slot arrays */
    uint8_t       *vdep;            /* per var: the loop level its import's element reads */
} xp_t;

static uint8_t dep_of(xp_t *x, dvs_expr_t r) {
    uint32_t s = slot_of(r);
    if (x->dep_done[s]) return x->dep[s];
    if (deep(x->t)) { x->err = DVS_RANDOMIZE_ERROR; return 0xFF; }
    uint8_t d = 0;
    if (KIND(x->t, r) == EXPR_LOOP_INDEX)
        d = (uint8_t)(1u << ((const ExprLoopIndex *)NODE(x->t, r))->level);
    if (x->vdep && KIND(x->t, r) == EXPR_VAR) d = x->vdep[((const ExprVar *)NODE(x->t, r))->var_id];
    dvs_expr_t k[4], *all = k;
    uint32_t nk = kids_of(x->t, r, k, 4);
    if (nk > 4) {
        if (!(all = malloc(nk * sizeof *all))) { x->err = DVS_RANDOMIZE_ERROR; return 0xFF; }
        kids_of(x->t, r, all, nk);
    }
    for (uint32_t i = 0; i < nk; i++) d |= dep_of(x, all[i]);
    if (all != k) free(all);
    if (KIND(x->t, r) == EXPR_REDUCE)
        d &= (uint8_t)~(1u << ((const ExprReduce *)NODE(x->t, r))->level);
    x->dep[s] = d;
    x->dep_done[s] = 1;
    return d;
}

/* A new var of the element-local problem, reading `src`/`id` on each call. */
static uint32_t local_var(xp_t *x, uint8_t src, uint32_t id, uint32_t width, uint8_t sg) {
    uint32_t v = x->n_srcs++;
    x->srcs[v].src = src;
    x->srcs[v].id = id;
    int64_t lo = sg ? (width >= 64 ? INT64_MIN : -(1ll << (width - 1))) : 0;
    int64_t hi = sg ? (width >= 64 ? INT64_MAX : (1ll << (width - 1)) - 1)
                    : (int64_t)mask_w(~0ull, width);
    if (dvs_builder_add_var(x->b, v, width, sg, lo, hi) == EXPR_NULL
            || dvs_builder_set_var_kind(x->b, v, src == SRC_ELEM ? DVS_VAR_RAND
                                                                 : DVS_VAR_STATE))
        x->err = DVS_RANDOMIZE_ERROR;
    return v;
}

static dvs_expr_t xp(xp_t *x, dvs_expr_t r);

static dvs_expr_t xp_list(xp_t *x, const dvs_expr_t *in, uint32_t n, dvs_expr_t *out) {
    for (uint32_t i = 0; i < n; i++)
        if ((out[i] = xp(x, in[i])) == EXPR_NULL) return EXPR_NULL;
    return 0;
}

static dvs_expr_t const_of(dvs_builder_t *b, uint64_t v, ty_t ty) {
    return dvs_builder_expr_const_sized(b, (int64_t)v, ty.sg, ty.w);
}

static dvs_expr_t xp_elem(xp_t *x, const ExprElem *n) {
    const array_t *a = &x->t->arr[n->array_id];
    const ty_t ety = {a->width, a->is_signed};
    if (x->mode == XP_ASSEMBLE && a->slot != DVS_NO_SLOT) {
        /* A slot array's element: whether the handle is not null, known */
        uint64_t iv;
        ty_t ity;
        if (fold(x->t, &x->env, NULL, n->index, &iv, &ity)) { x->err = DVS_RANDOMIZE_ERROR; return EXPR_NULL; }
        int64_t i = ity.sg ? sext(iv, ity.w) : (int64_t)iv;
        return const_of(x->b, i >= 0 && i <= UINT32_MAX && x->sa->live(x->sa->ctx, n->array_id, (uint32_t)i), ety);
    }
    if (x->mode == XP_ASSEMBLE) {
        dvs_expr_t i = xp(x, n->index);
        return i == EXPR_NULL ? EXPR_NULL
                              : dvs_builder_expr_elem(x->b, x->array_base + n->array_id, i);
    }
    if (x->mode == XP_LOCAL) {
        /* Element-local: [i] of the local array (var 0) or of a STATE array. */
        uint32_t *m = &x->map_elem[n->array_id];
        if (*m == NO_VAR) *m = local_var(x, SRC_STATE_ELEM, n->array_id, a->width, a->is_signed);
        return dvs_builder_expr_var(x->b, *m);
    }
    const uint32_t size = x->sizes[n->array_id], base = x->base[n->array_id];
    uint64_t iv;
    ty_t ity;
    if (fold(x->t, &x->env, x->sizes, n->index, &iv, &ity) == 0) {
        int64_t i = ity.sg ? sext(iv, ity.w) : (int64_t)iv;
        if (i >= 0 && (uint64_t)i < size) return dvs_builder_expr_var(x->b, base + (uint32_t)i);
        return const_of(x->b, 0, ety);
    }
    ity = type_of(x->t, n->index);
    if (ity.w > 64) { x->err = DVS_RANDOMIZE_ERROR; return EXPR_NULL; }
    dvs_expr_t idx = xp(x, n->index);
    if (idx == EXPR_NULL) return EXPR_NULL;
    dvs_expr_t acc = const_of(x->b, 0, ety);
    /* The indices the index's type can hold. */
    uint64_t lim = ity.w >= 64 ? ~0ull : ity.sg ? 1ull << (ity.w - 1) : 1ull << ity.w;
    for (uint32_t k = size; k-- > 0 && acc != EXPR_NULL;) {
        if (k >= lim) continue;
        dvs_expr_t eq = dvs_builder_expr_binary(x->b, DVS_BIN_EQ, idx, const_of(x->b, k, ity));
        acc = dvs_builder_expr_ite(x->b, eq, dvs_builder_expr_var(x->b, base + k), acc);
    }
    return acc;
}

static dvs_expr_t xp_reduce(xp_t *x, const ExprReduce *n) {
    if (x->mode == XP_ASSEMBLE) {
        dvs_expr_t body = xp(x, n->body);
        return body == EXPR_NULL ? EXPR_NULL
                                 : dvs_builder_expr_reduce(x->b, (dvs_binop_t)n->op,
                                                           x->array_base + n->array_id,
                                                           n->level, body);
    }
    const ty_t ty = type_of(x->t, n->body);
    const uint32_t size = x->sizes[n->array_id];
    if ((x->env.bound >> n->level) & 1) { x->err = DVS_RANDOMIZE_ERROR; return EXPR_NULL; }
    if (size == 0) {
        uint64_t id = n->op == DVS_BIN_MUL ? 1 : n->op == DVS_BIN_BAND ? ~0ull : 0;
        return const_of(x->b, mask_w(id, ty.w), ty);
    }
    dvs_expr_t acc = EXPR_NULL;
    for (uint32_t k = 0; k < size; k++) {
        env_bind(&x->env, n->level, (int32_t)k);
        dvs_expr_t term = xp(x, n->body);
        if (term == EXPR_NULL) break;
        acc = k ? dvs_builder_expr_binary(x->b, (dvs_binop_t)n->op, acc, term) : term;
        if (acc == EXPR_NULL) break;
    }
    x->env.bound &= ~(1u << n->level);
    x->env.changed[n->level] = ++x->env.tick;
    return acc;
}

static dvs_expr_t xp_node(xp_t *x, dvs_expr_t r) {
    dvs_tmpl_t *t = x->t;
    dvs_builder_t *b = x->b;
    const void *n = NODE(t, r);
    dvs_expr_t k[3];
    switch (KIND(t, r)) {
    case EXPR_CONST: {
        const ExprConst *c = n;
        return dvs_builder_expr_const_sized(b, c->value, c->is_signed, c->width);
    }
    case EXPR_CONST_WIDE: {
        const ExprConstWide *c = n;
        return dvs_builder_expr_const_wide(b, c->n_limbs, c->limbs, c->width, c->is_signed);
    }
    case EXPR_VAR: {
        uint32_t v = ((const ExprVar *)n)->var_id;
        if (x->mode == XP_ASSEMBLE) {
            uint32_t m = x->var_map[v];
            if (m == ASM_PER_ELEM) m = x->sa->var(x->sa->ctx, v, x->env.val, x->env.bound);
            if (m == NO_VAR) { x->err = DVS_RANDOMIZE_ERROR; return EXPR_NULL; }
            return dvs_builder_expr_var(b, m);
        }
        if (x->mode != XP_LOCAL) return dvs_builder_expr_var(b, v);
        if (x->map_scalar[v] == NO_VAR)
            x->map_scalar[v] = local_var(x, SRC_SCALAR, v, t->vwidth[v], t->vsigned[v]);
        return dvs_builder_expr_var(b, x->map_scalar[v]);
    }
    case EXPR_BINARY: {
        const ExprBinary *bn = n;
        if ((k[0] = xp(x, bn->lhs)) == EXPR_NULL || (k[1] = xp(x, bn->rhs)) == EXPR_NULL)
            return EXPR_NULL;
        return dvs_builder_expr_binary(b, (dvs_binop_t)bn->op, k[0], k[1]);
    }
    case EXPR_UNARY: {
        const ExprUnary *u = n;
        if ((k[0] = xp(x, u->operand)) == EXPR_NULL) return EXPR_NULL;
        return dvs_builder_expr_unary(b, (dvs_unop_t)u->op, k[0]);
    }
    case EXPR_ITE: {
        const ExprITE *it = n;
        uint64_t c;
        ty_t cty;
        /* A condition on the index alone picks its branch now. */
        if (fold(t, &x->env, x->sizes, it->cond, &c, &cty) == 0)
            return xp(x, c ? it->then_e : it->else_e);
        if (xp_list(x, &it->cond, 1, &k[0]) || xp_list(x, &it->then_e, 1, &k[1])
                || xp_list(x, &it->else_e, 1, &k[2]))
            return EXPR_NULL;
        return dvs_builder_expr_ite(b, k[0], k[1], k[2]);
    }
    case EXPR_IN_RANGE: {
        const ExprInRange *ir = n;
        const dvs_expr_t in[3] = {ir->value, ir->lo, ir->hi};
        if (xp_list(x, in, 3, k)) return EXPR_NULL;
        return dvs_builder_expr_in_range(b, k[0], k[1], k[2]);
    }
    case EXPR_IN_SET:
    case EXPR_IN_RANGES:
    case EXPR_SUM: {
        /* The value (or result) and then `m` more. */
        uint32_t m = KIND(t, r) == EXPR_IN_SET ? ((const ExprInSet *)n)->n_elems
                     : KIND(t, r) == EXPR_SUM  ? ((const ExprSum *)n)->n_vars
                                               : 2 * ((const ExprInRanges *)n)->n_ranges;
        dvs_expr_t *in = malloc((m + 1) * sizeof *in), *out = malloc((m + 1) * sizeof *out);
        dvs_expr_t res = EXPR_NULL;
        if (in && out) {
            kids_of(t, r, in, m + 1);
            if (xp_list(x, in, m + 1, out) == 0) {
                if (KIND(t, r) == EXPR_IN_SET)
                    res = dvs_builder_expr_in_set(b, out[0], m, out + 1);
                else if (KIND(t, r) == EXPR_SUM)
                    res = dvs_builder_expr_sum(b, out[0], m, out + 1);
                else
                    res = dvs_builder_expr_in_ranges(b, out[0], m / 2, out + 1, out + 1 + m / 2);
            }
        } else {
            x->err = DVS_RANDOMIZE_ERROR;
        }
        free(in);
        free(out);
        return res;
    }
    case EXPR_EXTEND: {
        const ExprExtend *e = n;
        if ((k[0] = xp(x, e->operand)) == EXPR_NULL) return EXPR_NULL;
        return dvs_builder_expr_extend(b, k[0], e->from_bits, e->to_bits, e->sign_extend);
    }
    case EXPR_SV_CAST: {
        const ExprSvCast *c = n;
        if ((k[0] = xp(x, c->operand)) == EXPR_NULL) return EXPR_NULL;
        return dvs_builder_expr_sv_cast(b, k[0], c->from_bits, c->to_bits, c->sign_extend,
                                        c->dst_signed);
    }
    case EXPR_CAST: {
        const ExprCast *c = n;
        if ((k[0] = xp(x, c->operand)) == EXPR_NULL) return EXPR_NULL;
        return dvs_builder_expr_cast(b, k[0], c->to_bits, c->to_signed);
    }
    case EXPR_EXTRACT: {
        const ExprExtract *e = n;
        if ((k[0] = xp(x, e->operand)) == EXPR_NULL) return EXPR_NULL;
        return dvs_builder_expr_extract(b, k[0], e->hi_bit, e->lo_bit);
    }
    case EXPR_CONCAT: {
        const ExprConcat *c = n;
        if ((k[0] = xp(x, c->hi)) == EXPR_NULL || (k[1] = xp(x, c->lo)) == EXPR_NULL)
            return EXPR_NULL;
        return dvs_builder_expr_concat(b, k[0], k[1], c->lo_width);
    }
    case EXPR_COUNTONES: {
        const ExprCountones *c = n;
        if ((k[0] = xp(x, c->result)) == EXPR_NULL || (k[1] = xp(x, c->operand)) == EXPR_NULL)
            return EXPR_NULL;
        return dvs_builder_expr_countones(b, k[0], k[1]);
    }
    case EXPR_CLOG2: {
        const ExprClog2 *c = n;
        if ((k[0] = xp(x, c->result)) == EXPR_NULL || (k[1] = xp(x, c->operand)) == EXPR_NULL)
            return EXPR_NULL;
        return dvs_builder_expr_clog2(b, k[0], k[1]);
    }
    case EXPR_ARRAY_SELECT: {
        const ExprArraySelect *s = n;
        if ((k[0] = xp(x, s->result)) == EXPR_NULL || (k[1] = xp(x, s->index)) == EXPR_NULL)
            return EXPR_NULL;
        return dvs_builder_expr_array_select(
            b, x->mode == XP_ASSEMBLE ? x->var_map[s->base_var_id] : s->base_var_id,
            s->n_elems, k[0], k[1]);
    }
    case EXPR_ARRAY_SIZE: {
        uint32_t a = ((const ExprArraySize *)n)->array_id;
        if (x->mode == XP_ASSEMBLE) return dvs_builder_expr_array_size(b, x->array_base + a);
        if (x->mode == XP_SIZES) return dvs_builder_expr_var(b, t->n_vars + a);
        if (x->mode == XP_EXPAND) {
            if (t->arr[a].local) return dvs_builder_expr_var(b, x->base[a]);
            return dvs_builder_expr_const_sized(b, (int64_t)x->sizes[a], 1, 32);
        }
        if (x->map_size[a] == NO_VAR) x->map_size[a] = local_var(x, SRC_SIZE, a, 32, 1);
        return dvs_builder_expr_var(b, x->map_size[a]);
    }
    case EXPR_LOOP_INDEX: {
        uint32_t l = ((const ExprLoopIndex *)n)->level;
        if (x->mode == XP_ASSEMBLE && !((x->env.bound >> l) & 1))
            return dvs_builder_expr_loop_index(b, l);
        if (x->mode == XP_LOCAL) {
            if (x->map_index == NO_VAR) x->map_index = local_var(x, SRC_INDEX, 0, 32, 1);
            return dvs_builder_expr_var(b, x->map_index);
        }
        if (!((x->env.bound >> l) & 1)) { x->err = DVS_RANDOMIZE_ERROR; return EXPR_NULL; }
        return dvs_builder_expr_const_sized(b, x->env.val[l], 1, 32);
    }
    case EXPR_ELEM: return xp_elem(x, n);
    case EXPR_REDUCE: return xp_reduce(x, n);
    default:
        x->err = DVS_RANDOMIZE_ERROR;
        return EXPR_NULL;
    }
}

static dvs_expr_t xp(xp_t *x, dvs_expr_t r) {
    if (x->err) return EXPR_NULL;
    if (deep(x->t)) { x->err = DVS_RANDOMIZE_ERROR; return EXPR_NULL; }
    uint32_t s = slot_of(r);
    uint8_t d = dep_of(x, r);
    if (x->memo[s] != EXPR_NULL) {
        int fresh = 1;
        for (uint32_t l = 0; l < DVS_MAX_LOOP_LEVELS; l++)
            if ((d >> l) & 1 && x->env.changed[l] > x->memo_tick[s]) fresh = 0;
        if (fresh) return x->memo[s];
    }
    dvs_expr_t out = xp_node(x, r);
    if (out == EXPR_NULL) {
        if (!x->err) x->err = DVS_RANDOMIZE_ERROR;
        return EXPR_NULL;
    }
    x->memo[s] = out;
    x->memo_tick[s] = x->env.tick;
    return out;
}

/* Rebuild into `b`, or into a builder of its own when `b` is NULL. */
static int xp_init(xp_t *x, dvs_tmpl_t *t, dvs_builder_t *b) {
    memset(x, 0, sizeof *x);
    x->t = t;
    uint32_t n = t->n_slots ? t->n_slots : 1;
    x->memo = malloc(n * sizeof *x->memo);
    x->memo_tick = calloc(n, sizeof *x->memo_tick);
    x->dep_done = calloc(n, 1);
    x->dep = calloc(n, 1);
    x->b = b ? b : dvs_builder_create(0, NULL);
    if (!x->memo || !x->memo_tick || !x->dep_done || !x->dep || !x->b) return -1;
    for (uint32_t i = 0; i < n; i++) x->memo[i] = EXPR_NULL;
    if (t->p->flags & DVS_PROBLEM_F_EXPLICIT) dvs_builder_set_explicit(x->b);
    return 0;
}

static void xp_fini(xp_t *x) {
    free(x->memo);
    free(x->memo_tick);
    free(x->dep_done);
    free(x->dep);
    free(x->map_scalar);
    free(x->map_elem);
    free(x->map_size);
    free(x->prios);
    free(x->vdep);
    if (x->b) dvs_builder_destroy(x->b);
}

/* The handle of the problem `x` has built, or NULL. */
static dvs_handle_t *xp_finish(xp_t *x) {
    size_t sz = 0;
    dvs_problem_t *img = dvs_builder_finalize(x->b, &sz);
    if (!img) return NULL;
    dvs_handle_t *h = dvs_handle_new(img, sz);
    dvs_builder_free_problem(x->b, img, sz);
    if (h && x->t->engine != DVS_ENGINE_AUTO && dvs_handle_set_engine(h, x->t->engine)) {
        dvs_handle_free(h);
        return NULL;
    }
    return h;
}

/* The specs of a list, oldest first (the builder links them newest first). */
static uint32_t list_rev(const dvs_problem_t *p, dvs_expr_t head, uint32_t count,
                         dvs_expr_t *out) {
    uint32_t n = 0;
    for (dvs_expr_t r = count ? head : EXPR_NULL; r != EXPR_NULL && n < count;) {
        out[n++] = r;
        r = *(const dvs_expr_t *)POOL_PTR(p, r);
    }
    for (uint32_t i = 0; i < n / 2; i++) {
        dvs_expr_t s = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = s;
    }
    return n;
}

/* Add the constraints, soft constraints and distributions of a body. */
static int add_body(xp_t *x, dvs_expr_t c_head, uint32_t n_c, dvs_expr_t s_head,
                    uint32_t n_s, dvs_expr_t d_head, uint32_t n_d, int in_foreach) {
    const dvs_problem_t *p = x->t->p;
    uint32_t most = n_c > n_s ? n_c : n_s;
    if (n_d > most) most = n_d;
    dvs_expr_t *l = malloc((most ? most : 1) * sizeof *l);
    if (!l) return -1;
    int rc = 0;
    uint32_t n = list_rev(p, c_head, n_c, l);
    for (uint32_t i = 0; i < n && !rc; i++) {
        dvs_expr_t e = xp(x, ((const ConstraintSpec *)POOL_PTR(p, l[i]))->root);
        rc = e == EXPR_NULL || dvs_builder_add_constraint(x->b, e) == EXPR_NULL;
    }
    n = list_rev(p, s_head, n_s, l);
    for (uint32_t i = 0; i < n && !rc; i++) {
        const SoftSpec *s = POOL_PTR(p, l[i]);
        dvs_expr_t e = xp(x, s->root);
        uint32_t prio = s->priority;
        if (x->mode == XP_ASSEMBLE) {
            uint32_t lo = 0, hi = x->n_prios;   /* its rank among the part's */
            while (x->prios[lo] != prio) {
                uint32_t mid = lo + (hi - lo) / 2;
                if (x->prios[mid] <= prio) lo = mid; else hi = mid;
            }
            prio = x->prio_base + lo;
        }
        rc = e == EXPR_NULL || dvs_builder_add_soft_constraint(x->b, e, prio) == EXPR_NULL;
    }
    n = list_rev(p, d_head, n_d, l);
    for (uint32_t i = 0; i < n && !rc; i++) {
        const DistSpec *d = POOL_PTR(p, l[i]);
        uint32_t var = d->var_id;
        if (x->mode == XP_ASSEMBLE) {
            const dvs_dist_entry_t *de = (const dvs_dist_entry_t *)(d + 1);
            dvs_expr_t ix = in_foreach ? xp(x, d->index) : EXPR_NULL;
            rc = in_foreach ? ix == EXPR_NULL
                                  || dvs_builder_add_dist_elem(x->b, x->array_base + var, ix,
                                                               d->n_entries, de) == EXPR_NULL
                            : dvs_builder_add_dist(x->b, x->var_map[var], d->n_entries, de)
                                  == EXPR_NULL;
            continue;
        }
        if (in_foreach && x->mode == XP_EXPAND) {
            /* The element's index folds, as the foreach binds it. */
            uint64_t iv;
            ty_t ity;
            if (fold(x->t, &x->env, x->sizes, d->index, &iv, &ity)) { rc = 1; break; }
            int64_t ix = ity.sg ? sext(iv, ity.w) : (int64_t)iv;
            if (ix < 0 || (uint64_t)ix >= x->sizes[var]) continue;
            var = x->base[var] + (uint32_t)ix;
        } else if (in_foreach) {
            var = 0;   /* the element of an element-local problem */
        }
        rc = dvs_builder_add_dist(x->b, var, d->n_entries,
                                  (const dvs_dist_entry_t *)(d + 1)) == EXPR_NULL;
    }
    free(l);
    if (rc && !x->err) x->err = DVS_RANDOMIZE_ERROR;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Expansion                                                           */
/* ------------------------------------------------------------------ */

static int expand_foreach(xp_t *x, dvs_expr_t f) {
    const ForeachSpec *fe = fe_at(x->t, f);
    const dvs_problem_t *p = x->t->p;
    if ((x->env.bound >> fe->level) & 1) { x->err = DVS_RANDOMIZE_ERROR; return -1; }
    for (uint32_t k = 0; k < x->sizes[fe->array_id]; k++) {
        env_bind(&x->env, fe->level, (int32_t)k);
        if (add_body(x, fe->constraints_head, fe->n_constraints, fe->softs_head, fe->n_softs,
                     fe->dists_head, fe->n_dists, 1))
            return -1;
        for (dvs_expr_t c = p->foreachs_head; c != EXPR_NULL; c = fe_at(x->t, c)->next)
            if (fe_at(x->t, c)->parent == f && expand_foreach(x, c)) return -1;
    }
    x->env.bound &= ~(1u << fe->level);
    x->env.changed[fe->level] = ++x->env.tick;
    return 0;
}

static int64_t full_lo(uint32_t w, uint8_t sg) {
    return sg ? (w >= 64 ? INT64_MIN : -(1ll << (w - 1))) : 0;
}

static int64_t full_hi(uint32_t w, uint8_t sg) {
    return sg ? (w >= 64 ? INT64_MAX : (1ll << (w - 1)) - 1) : (int64_t)mask_w(~0ull, w);
}

/* Build the expansion for `sizes` into `e`; a DVS_RANDOMIZE_* code. */
static int expand(dvs_tmpl_t *t, const uint32_t *sizes, expansion_t *e) {
    const dvs_problem_t *p = t->p;
    xp_t x;
    int rc = DVS_RANDOMIZE_ERROR;
    if (xp_init(&x, t, NULL)) goto out;
    x.mode = XP_EXPAND;
    x.sizes = sizes;
    x.base = e->base;

    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *v = POOL_PTR(p, r);
        if (dvs_builder_add_var(x.b, v->var_id, v->width, v->is_signed, v->lo, v->hi)
                    == EXPR_NULL
                || dvs_builder_set_var_kind(x.b, v->var_id, (dvs_var_kind_t)v->kind)
                || dvs_builder_set_var_order(x.b, v->var_id, v->order))
            goto out;
        r = v->next;
    }
    uint32_t next = t->n_vars;
    for (uint32_t a = 0; a < t->n_arrays; a++) {
        const array_t *ar = &t->arr[a];
        e->base[a] = NO_VAR;
        if (ar->local) {
            /* Its size, read by the constraints outside its foreachs */
            e->base[a] = next;
            if (dvs_builder_add_var(x.b, next, 32, 1, 0, INT32_MAX) == EXPR_NULL
                    || dvs_builder_set_var_kind(x.b, next++, DVS_VAR_STATE))
                goto out;
            continue;
        }
        if (sizes[a] > TMPL_MAX_ELEMS - (next - t->n_vars)) {
            rc = DVS_RANDOMIZE_TOO_LARGE;
            goto out;
        }
        e->base[a] = next;
        for (uint32_t k = 0; k < sizes[a]; k++, next++)
            if (dvs_builder_add_var(x.b, next, ar->width, ar->is_signed,
                                    full_lo(ar->width, ar->is_signed),
                                    full_hi(ar->width, ar->is_signed)) == EXPR_NULL
                    || dvs_builder_set_var_kind(x.b, next, (dvs_var_kind_t)ar->kind)
                    || dvs_builder_set_var_order(x.b, next, ar->order))
                goto out;
    }
    /* The solver compiles a problem without variables to nothing: keep one,
     * so that a constraint on sizes alone is still checked. */
    if (next == 0 && dvs_builder_add_var(x.b, next++, 1, 0, 0, 1) == EXPR_NULL) goto out;
    e->n_refs = next;

    if (add_body(&x, p->constraints_head, p->n_constraints, p->softs_head, p->n_softs,
                 p->dists_head, p->n_dists, 0))
        goto fail;
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;
         f = fe_at(t, f)->next)
        if (fe_at(t, f)->parent == EXPR_NULL && !t->arr[fe_at(t, f)->array_id].local
                && expand_foreach(&x, f))
            goto fail;
    for (dvs_expr_t r = p->allDiff_head; r != EXPR_NULL;) {
        const AllDiffSpec *ad = POOL_PTR(p, r);
        const uint32_t *ids = (const uint32_t *)(ad + 1);
        uint32_t n = 0;
        for (uint32_t i = 0; i < ad->n_vars; i++)
            n += ids[i] & DVS_ARRAY_MEMBER_BIT ? sizes[ids[i] & ~DVS_ARRAY_MEMBER_BIT] : 1;
        uint32_t *m = malloc((n ? n : 1) * sizeof *m);
        if (!m) goto out;
        n = 0;
        for (uint32_t i = 0; i < ad->n_vars; i++) {
            if (!(ids[i] & DVS_ARRAY_MEMBER_BIT)) { m[n++] = ids[i]; continue; }
            uint32_t a = ids[i] & ~DVS_ARRAY_MEMBER_BIT;
            for (uint32_t k = 0; k < sizes[a]; k++) m[n++] = e->base[a] + k;
        }
        dvs_expr_t ok = dvs_builder_add_all_different(x.b, n, m);
        free(m);
        if (ok == EXPR_NULL) goto out;
        r = ad->next;
    }
    for (dvs_expr_t r = p->sources_head; r != EXPR_NULL;) {
        const SourceSpec *s = POOL_PTR(p, r);
        if (dvs_builder_add_source(x.b, s->n_vars, (const uint32_t *)(s + 1)) == EXPR_NULL)
            goto out;
        r = s->next;
    }
    e->h = xp_finish(&x);
    rc = e->h ? DVS_RANDOMIZE_OK : DVS_RANDOMIZE_ERROR;
    goto out;
fail:
    rc = x.err ? x.err : DVS_RANDOMIZE_ERROR;
out:
    xp_fini(&x);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Element-local problems                                              */
/* ------------------------------------------------------------------ */

static int build_local(dvs_tmpl_t *t, uint32_t a) {
    array_t *ar = &t->arr[a];
    xp_t x;
    int rc = -1;
    if (xp_init(&x, t, NULL)) goto out;
    x.mode = XP_LOCAL;
    x.local_array = a;
    x.map_index = NO_VAR;
    x.map_scalar = malloc((t->n_vars ? t->n_vars : 1) * sizeof *x.map_scalar);
    x.map_elem = malloc(t->n_arrays * sizeof *x.map_elem);
    x.map_size = malloc(t->n_arrays * sizeof *x.map_size);
    /* At most: the element, the index, each scalar, each array twice. */
    x.srcs = malloc((2 + t->n_vars + 2 * t->n_arrays) * sizeof *x.srcs);
    if (!x.map_scalar || !x.map_elem || !x.map_size || !x.srcs) goto out;
    for (uint32_t v = 0; v < t->n_vars; v++) x.map_scalar[v] = NO_VAR;
    for (uint32_t i = 0; i < t->n_arrays; i++) x.map_elem[i] = x.map_size[i] = NO_VAR;
    x.map_elem[a] = local_var(&x, SRC_ELEM, a, ar->width, ar->is_signed);

    const dvs_problem_t *p = t->p;
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;
         f = fe_at(t, f)->next) {
        const ForeachSpec *fe = fe_at(t, f);
        if (fe->array_id != a) continue;
        /* Each foreach binds its own level: the index var reads it. The
         * memo holds nodes of the previous one, which read the same vars. */
        x.local_level = fe->level;
        if (add_body(&x, fe->constraints_head, fe->n_constraints, fe->softs_head,
                     fe->n_softs, fe->dists_head, fe->n_dists, 1))
            goto out;
    }
    if (x.err) goto out;
    ar->h = xp_finish(&x);
    if (!ar->h) goto out;
    ar->srcs = x.srcs;
    ar->n_srcs = x.n_srcs;
    x.srcs = NULL;
    rc = 0;
out:
    free(x.srcs);
    xp_fini(&x);
    return rc;
}

/* ------------------------------------------------------------------ */
/* A part of an assembled problem                                      */
/* ------------------------------------------------------------------ */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* The distinct soft priorities of the part, sorted, into x->prios. */
static int collect_prios(xp_t *x) {
    const dvs_problem_t *p = x->t->p;
    uint32_t n = p->n_softs;
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;
         f = fe_at(x->t, f)->next)
        n += fe_at(x->t, f)->n_softs;
    if (!(x->prios = malloc((n ? n : 1) * sizeof *x->prios))) return -1;
    n = 0;
    for (dvs_expr_t r = p->softs_head; p->n_softs && r != EXPR_NULL;) {
        const SoftSpec *s = POOL_PTR(p, r);
        x->prios[n++] = s->priority;
        r = s->next;
    }
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;
         f = fe_at(x->t, f)->next)
        for (dvs_expr_t r = fe_at(x->t, f)->n_softs ? fe_at(x->t, f)->softs_head : EXPR_NULL;
             r != EXPR_NULL;) {
            const SoftSpec *s = POOL_PTR(p, r);
            x->prios[n++] = s->priority;
            r = s->next;
        }
    qsort(x->prios, n, sizeof *x->prios, cmp_u32);
    x->n_prios = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!i || x->prios[i] != x->prios[i - 1]) x->prios[x->n_prios++] = x->prios[i];
    return 0;
}

/* Foreach `f` and the foreachs nested in it, as foreachs; over a slot
 * array, instantiated once per element. */
static int assemble_foreach(xp_t *x, dvs_expr_t f) {
    const ForeachSpec *fe = fe_at(x->t, f);
    if (x->t->arr[fe->array_id].slot != DVS_NO_SLOT) {
        if ((x->env.bound >> fe->level) & 1) return -1;
        for (uint32_t k = 0, n = x->sa->length(x->sa->ctx, fe->array_id); k < n; k++) {
            env_bind(&x->env, fe->level, (int32_t)k);
            if (add_body(x, fe->constraints_head, fe->n_constraints, fe->softs_head,
                         fe->n_softs, fe->dists_head, fe->n_dists, 1))
                return -1;
            for (dvs_expr_t c = x->t->p->foreachs_head; c != EXPR_NULL; c = fe_at(x->t, c)->next)
                if (fe_at(x->t, c)->parent == f && assemble_foreach(x, c)) return -1;
        }
        x->env.bound &= ~(1u << fe->level);
        x->env.changed[fe->level] = ++x->env.tick;
        return 0;
    }
    if (dvs_builder_foreach_begin(x->b, x->array_base + fe->array_id, fe->level)
            || add_body(x, fe->constraints_head, fe->n_constraints, fe->softs_head, fe->n_softs,
                        fe->dists_head, fe->n_dists, 1))
        return -1;
    for (dvs_expr_t c = x->t->p->foreachs_head; c != EXPR_NULL; c = fe_at(x->t, c)->next)
        if (fe_at(x->t, c)->parent == f && assemble_foreach(x, c)) return -1;
    return dvs_builder_foreach_end(x->b);
}

int dvs_tmpl_assemble(dvs_builder_t *b, const dvs_problem_t *p, const uint32_t *var_map,
                      uint32_t array_base, uint32_t prio_base, uint32_t *n_prios,
                      const dvs_asm_slots_t *sa) {
    dvs_tmpl_t *t = dvs_tmpl_new(p);
    xp_t x;
    memset(&x, 0, sizeof x);
    int rc = -1;
    if (!t || xp_init(&x, t, b) || collect_prios(&x)
            || !(x.vdep = calloc(t->n_vars ? t->n_vars : 1, 1)))
        goto out;
    x.mode = XP_ASSEMBLE;
    x.var_map = var_map;
    x.array_base = array_base;
    x.prio_base = prio_base;
    x.sa = sa;
    *n_prios = x.n_prios;
    /* The loop level of each import through a DVS_PATH_LOOP element */
    for (dvs_expr_t r = p->n_imports ? p->imports_head : EXPR_NULL; r != EXPR_NULL;) {
        const ImportSpec *im = POOL_PTR(p, r);
        const uint32_t *path = (const uint32_t *)(im + 1);
        for (uint32_t i = 0; i < im->n_path; i++)
            if (path[i] & DVS_PATH_LOOP(0)) {
                if ((path[i] & ~DVS_PATH_LOOP(0)) >= DVS_MAX_LOOP_LEVELS) goto out;
                x.vdep[im->var_id] = (uint8_t)(1u << (path[i] & ~DVS_PATH_LOOP(0)));
            }
        r = im->next;
    }
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL;) {
        const ArraySpec *a = POOL_PTR(p, r);
        uint32_t id = array_base + a->array_id;
        /* A slot array's elements are no longer read: a plain array, RAND if
         * its size is random */
        const int kind = a->slot == DVS_NO_SLOT ? a->kind
                         : a->size_random       ? DVS_VAR_RAND
                                                : DVS_VAR_STATE;
        if ((a->slot != DVS_NO_SLOT && !sa)
                || dvs_builder_add_array(b, id, a->elem_width, a->elem_signed,
                                         (dvs_var_kind_t)kind)
                || dvs_builder_set_array_order(b, id, a->order)
                || (a->size_random && dvs_builder_set_array_size_random(b, id)))
            goto out;
        r = a->next;
    }
    if (add_body(&x, p->constraints_head, p->n_constraints, p->softs_head, p->n_softs,
                 p->dists_head, p->n_dists, 0))
        goto out;
    for (dvs_expr_t f = p->n_foreachs ? p->foreachs_head : EXPR_NULL; f != EXPR_NULL;
         f = fe_at(t, f)->next)
        if (fe_at(t, f)->parent == EXPR_NULL && assemble_foreach(&x, f)) goto out;
    /* All-different and source members, mapped */
    const dvs_expr_t heads[2] = {p->allDiff_head, p->sources_head};
    for (int l = 0; l < 2; l++)
        for (dvs_expr_t r = heads[l]; r != EXPR_NULL;) {
            const AllDiffSpec *ad = POOL_PTR(p, r);   /* SourceSpec has the same layout */
            const uint32_t *ids = (const uint32_t *)(ad + 1);
            uint32_t *m = malloc((ad->n_vars ? ad->n_vars : 1) * sizeof *m);
            if (!m) goto out;
            for (uint32_t i = 0; i < ad->n_vars; i++)
                m[i] = ids[i] & DVS_ARRAY_MEMBER_BIT
                           ? DVS_ARRAY_MEMBER_BIT | (array_base + (ids[i] & ~DVS_ARRAY_MEMBER_BIT))
                           : var_map[ids[i]];
            dvs_expr_t ok = l ? dvs_builder_add_source(b, ad->n_vars, m)
                              : dvs_builder_add_all_different(b, ad->n_vars, m);
            free(m);
            if (ok == EXPR_NULL) goto out;
            r = ad->next;
        }
    rc = x.err ? -1 : 0;
out:
    x.b = NULL;   /* the caller's */
    xp_fini(&x);
    dvs_tmpl_free(t);
    return rc;
}

static uint32_t elem_bytes(uint32_t w) {
    return w <= 8 ? 1 : w <= 16 ? 2 : w <= 32 ? 4 : 8;
}

static int refs_reserve(dvs_tmpl_t *t, uint32_t n) {
    if (n <= t->cap_refs) return 0;
    dvs_ref_t *r = realloc(t->refs, n * sizeof *r);
    if (!r) return -1;
    t->refs = r;
    t->cap_refs = n;
    return 0;
}

static void ref_set(dvs_ref_t *r, void *addr, uint32_t width, uint8_t sg) {
    memset(r, 0, sizeof *r);
    r->addr = addr;
    r->width = width;
    r->is_signed = sg;
    r->layout = DVS_REF_SCALAR;
}

static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* ------------------------------------------------------------------ */
/* Random sizes                                                        */
/* ------------------------------------------------------------------ */

/* A node a size constraint may hold: none that reads an element. */
static int no_elem(const dvs_tmpl_t *t, dvs_expr_t r, void *arg) {
    (void)arg;
    uint32_t k = KIND(t, r);
    return k == EXPR_ELEM || k == EXPR_REDUCE || k == EXPR_LOOP_INDEX ? -1 : 0;
}

static int build_sizes(dvs_tmpl_t *t) {
    const dvs_problem_t *p = t->p;
    xp_t x;
    int rc = -1;
    uint32_t *seen = calloc(t->n_slots ? t->n_slots : 1, sizeof *seen), mark = 0;
    dvs_expr_t *l = NULL;
    if (!seen || xp_init(&x, t, NULL)) goto out;
    x.mode = XP_SIZES;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *v = POOL_PTR(p, r);
        if (dvs_builder_add_var(x.b, v->var_id, v->width, v->is_signed, v->lo, v->hi)
                    == EXPR_NULL
                || dvs_builder_set_var_kind(x.b, v->var_id, (dvs_var_kind_t)v->kind)
                || dvs_builder_set_var_order(x.b, v->var_id, v->order))
            goto out;
        r = v->next;
    }
    for (uint32_t a = 0; a < t->n_arrays; a++) {
        const int rnd = t->arr[a].size_random;
        if (dvs_builder_add_var(x.b, t->n_vars + a, 32, 1, 0, INT32_MAX) == EXPR_NULL
                || dvs_builder_set_var_kind(x.b, t->n_vars + a,
                                            rnd ? DVS_VAR_RAND : DVS_VAR_STATE))
            goto out;
    }
    uint32_t most = p->n_constraints > p->n_softs ? p->n_constraints : p->n_softs;
    if (!(l = malloc((most ? most : 1) * sizeof *l))) goto out;
    uint32_t n = list_rev(p, p->constraints_head, p->n_constraints, l);
    for (uint32_t i = 0; i < n; i++) {
        dvs_expr_t root = ((const ConstraintSpec *)POOL_PTR(p, l[i]))->root;
        if (walk(t, root, seen, ++mark, no_elem, NULL)) continue;
        dvs_expr_t e = xp(&x, root);
        if (e == EXPR_NULL || dvs_builder_add_constraint(x.b, e) == EXPR_NULL) goto out;
    }
    n = list_rev(p, p->softs_head, p->n_softs, l);
    for (uint32_t i = 0; i < n; i++) {
        const SoftSpec *sp = POOL_PTR(p, l[i]);
        if (walk(t, sp->root, seen, ++mark, no_elem, NULL)) continue;
        dvs_expr_t e = xp(&x, sp->root);
        if (e == EXPR_NULL || dvs_builder_add_soft_constraint(x.b, e, sp->priority) == EXPR_NULL)
            goto out;
    }
    for (dvs_expr_t r = p->dists_head; r != EXPR_NULL;) {
        const DistSpec *d = POOL_PTR(p, r);
        if (dvs_builder_add_dist(x.b, d->var_id, d->n_entries,
                                 (const dvs_dist_entry_t *)(d + 1)) == EXPR_NULL)
            goto out;
        r = d->next;
    }
    for (dvs_expr_t r = p->allDiff_head; r != EXPR_NULL;) {
        const AllDiffSpec *ad = POOL_PTR(p, r);
        const uint32_t *ids = (const uint32_t *)(ad + 1);
        int scalars = 1;
        for (uint32_t i = 0; i < ad->n_vars; i++) scalars &= !(ids[i] & DVS_ARRAY_MEMBER_BIT);
        if (scalars && dvs_builder_add_all_different(x.b, ad->n_vars, ids) == EXPR_NULL)
            goto out;
        r = ad->next;
    }
    if (x.err) goto out;
    t->size_h = xp_finish(&x);
    rc = t->size_h ? 0 : -1;
out:
    free(l);
    free(seen);
    xp_fini(&x);
    return rc;
}

int dvs_tmpl_randomize_sizes(dvs_tmpl_t *t, const dvs_ref_t *refs, uint32_t n_vars,
                             uint32_t *sizes, uint64_t seed) {
    if (n_vars != t->n_vars || (t->n_arrays && !sizes)) return DVS_RANDOMIZE_ERROR;
    if (!t->any_size_random) return DVS_RANDOMIZE_OK;
    if (!t->size_h && build_sizes(t)) return DVS_RANDOMIZE_ERROR;
    /* The scalars are read from copies: this call writes no ref. */
    uint64_t *vals = calloc(n_vars ? n_vars : 1, sizeof *vals);
    int32_t *sizes32 = calloc(t->n_arrays ? t->n_arrays : 1, sizeof *sizes32);
    int rc = DVS_RANDOMIZE_ERROR;
    if (!vals || !sizes32 || refs_reserve(t, n_vars + t->n_arrays)) goto out;
    for (uint32_t v = 0; v < n_vars; v++) {
        t->refs[v] = refs[v];
        if (!refs[v].addr) continue;
        memcpy(&vals[v], refs[v].addr, elem_bytes(refs[v].width));
        t->refs[v].addr = &vals[v];
    }
    for (uint32_t a = 0; a < t->n_arrays; a++) {
        if (sizes[a] > INT32_MAX) goto out;
        sizes32[a] = (int32_t)sizes[a];
        ref_set(&t->refs[n_vars + a], &sizes32[a], 32, 1);
    }
    rc = dvs_randomize(t->size_h, t->refs, NULL, mix64(seed ^ 0x5157e5u));
    for (uint32_t a = 0; a < t->n_arrays && rc == DVS_RANDOMIZE_OK; a++)
        if (t->arr[a].size_random && (uint32_t)sizes32[a] > TMPL_MAX_SIZE)
            rc = DVS_RANDOMIZE_TOO_LARGE;
    for (uint32_t a = 0; a < t->n_arrays && rc == DVS_RANDOMIZE_OK; a++)
        if (t->arr[a].size_random) sizes[a] = (uint32_t)sizes32[a];
out:
    free(vals);
    free(sizes32);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Lifecycle and solving                                               */
/* ------------------------------------------------------------------ */

dvs_tmpl_t *dvs_tmpl_new(const dvs_problem_t *p) {
    dvs_tmpl_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->p = p;
    t->n_slots = (p->pool.used + DVS_IMAGE_ALIGN - 1) / DVS_IMAGE_ALIGN;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *v = POOL_PTR(p, r);
        if (v->var_id >= t->n_vars) t->n_vars = v->var_id + 1;
        r = v->next;
    }
    uint32_t nv = t->n_vars ? t->n_vars : 1;
    t->vwidth = calloc(nv, sizeof *t->vwidth);
    t->vsigned = calloc(nv, 1);
    t->vkind = calloc(nv, 1);
    t->n_arrays = p->n_arrays;
    t->arr = calloc(t->n_arrays ? t->n_arrays : 1, sizeof *t->arr);
    if (!t->vwidth || !t->vsigned || !t->vkind || !t->arr) goto fail;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *v = POOL_PTR(p, r);
        t->vwidth[v->var_id] = v->width;
        t->vsigned[v->var_id] = v->is_signed;
        t->vkind[v->var_id] = v->kind;
        r = v->next;
    }
    for (dvs_expr_t r = p->n_arrays ? p->arrays_head : EXPR_NULL; r != EXPR_NULL;) {
        const ArraySpec *a = POOL_PTR(p, r);
        array_t *ar = &t->arr[a->array_id];
        if (ar->width) goto fail;   /* declared twice */
        ar->width = a->elem_width;
        ar->is_signed = a->elem_signed;
        ar->kind = a->kind;
        ar->order = a->order;
        ar->size_random = a->size_random;
        ar->slot = a->slot;
        t->any_size_random |= a->size_random;
        r = a->next;
    }
    for (uint32_t a = 0; a < t->n_arrays; a++)
        if (!t->arr[a].width) goto fail;   /* never declared */
    if (classify(t)) goto fail;
    for (uint32_t i = 0; i < TMPL_LRU; i++) {
        t->lru[i].sizes = calloc(t->n_arrays ? t->n_arrays : 1, sizeof *t->lru[i].sizes);
        t->lru[i].base = calloc(t->n_arrays ? t->n_arrays : 1, sizeof *t->lru[i].base);
        if (!t->lru[i].sizes || !t->lru[i].base) goto fail;
    }
    return t;
fail:
    dvs_tmpl_free(t);
    return NULL;
}

void dvs_tmpl_free(dvs_tmpl_t *t) {
    if (!t) return;
    for (uint32_t a = 0; t->arr && a < t->n_arrays; a++) {
        dvs_handle_free(t->arr[a].h);
        free(t->arr[a].srcs);
        free(t->arr[a].out);
    }
    dvs_handle_free(t->size_h);
    for (uint32_t i = 0; i < TMPL_LRU; i++) {
        dvs_handle_free(t->lru[i].h);
        free(t->lru[i].sizes);
        free(t->lru[i].base);
    }
    free(t->arr);
    free(t->vwidth);
    free(t->vsigned);
    free(t->vkind);
    free(t->refs);
    free(t);
}

void dvs_tmpl_set_engine(dvs_tmpl_t *t, int engine) { t->engine = engine; }

uint32_t dvs_tmpl_n_arrays(const dvs_tmpl_t *t) { return t->n_arrays; }

int dvs_tmpl_array_is_local(const dvs_tmpl_t *t, uint32_t a) {
    return a < t->n_arrays && t->arr[a].local;
}

uint32_t dvs_tmpl_n_expansions(const dvs_tmpl_t *t) { return t->n_expansions; }

/* Solve element-local array `a`, one element at a time. */
static int solve_local(dvs_tmpl_t *t, uint32_t a, const dvs_ref_t *refs,
                       const dvs_array_ref_t *arrays, const int32_t *sizes32,
                       uint64_t seed) {
    array_t *ar = &t->arr[a];
    if (!ar->h && build_local(t, a)) return DVS_RANDOMIZE_ERROR;
    if (refs_reserve(t, ar->n_srcs)) return DVS_RANDOMIZE_ERROR;
    size_t need = (size_t)arrays[a].n_elems * elem_bytes(ar->width);
    if (need > ar->cap_out) {
        uint8_t *o = realloc(ar->out, need);
        if (!o) return DVS_RANDOMIZE_ERROR;
        ar->out = o;
        ar->cap_out = need;
    }
    static const uint64_t zero = 0;
    int32_t index = 0;
    for (uint32_t k = 0; k < arrays[a].n_elems; k++) {
        index = (int32_t)k;
        for (uint32_t v = 0; v < ar->n_srcs; v++) {
            const local_src_t *s = &ar->srcs[v];
            dvs_ref_t *r = &t->refs[v];
            switch (s->src) {
            case SRC_ELEM:
                ref_set(r, ar->out + (size_t)k * elem_bytes(ar->width), ar->width,
                        ar->is_signed);
                break;
            case SRC_INDEX: ref_set(r, &index, 32, 1); break;
            case SRC_SCALAR: *r = refs[s->id]; break;
            case SRC_SIZE: ref_set(r, (void *)&sizes32[s->id], 32, 1); break;
            case SRC_STATE_ELEM: {
                const array_t *sa = &t->arr[s->id];
                void *at = k < arrays[s->id].n_elems
                               ? (uint8_t *)arrays[s->id].data
                                     + (size_t)k * elem_bytes(sa->width)
                               : (void *)&zero;
                ref_set(r, at, sa->width, sa->is_signed);
                break;
            }
            }
        }
        int rc = dvs_randomize(ar->h, t->refs, NULL,
                               mix64(seed ^ mix64(((uint64_t)a << 32) | k)));
        if (rc != DVS_RANDOMIZE_OK) return rc;
    }
    return DVS_RANDOMIZE_OK;
}

int dvs_tmpl_randomize(dvs_tmpl_t *t, const dvs_ref_t *refs, uint32_t n_vars,
                       const dvs_array_ref_t *arrays, uint64_t seed) {
    if (n_vars != t->n_vars || (t->n_arrays && !arrays)) return DVS_RANDOMIZE_ERROR;
    int32_t *sizes32 = malloc((t->n_arrays ? t->n_arrays : 1) * sizeof *sizes32);
    uint32_t *sizes = malloc((t->n_arrays ? t->n_arrays : 1) * sizeof *sizes);
    int rc = DVS_RANDOMIZE_ERROR;
    if (!sizes32 || !sizes) goto out;
    for (uint32_t a = 0; a < t->n_arrays; a++) {
        if (arrays[a].n_elems > INT32_MAX || (arrays[a].n_elems && !arrays[a].data)) goto out;
        sizes[a] = arrays[a].n_elems;
        sizes32[a] = (int32_t)arrays[a].n_elems;
    }

    /* The element-local arrays first: they read only STATE values. Their
     * values reach the caller's buffers only once the whole call succeeds. */
    for (uint32_t a = 0; a < t->n_arrays; a++) {
        if (!t->arr[a].local) continue;
        if ((rc = solve_local(t, a, refs, arrays, sizes32, seed)) != DVS_RANDOMIZE_OK)
            goto out;
    }

    /* The expansion for the sizes it reads. */
    expansion_t *e = NULL, *victim = &t->lru[0];
    for (uint32_t i = 0; i < TMPL_LRU && !e; i++) {
        expansion_t *c = &t->lru[i];
        int same = c->h != NULL;
        for (uint32_t a = 0; a < t->n_arrays && same; a++)
            if (t->arr[a].in_key && c->sizes[a] != sizes[a]) same = 0;
        if (same) e = c;
        else if (!c->h || (victim->h && c->used < victim->used)) victim = c;
    }
    if (!e) {
        e = victim;
        dvs_handle_free(e->h);
        e->h = NULL;
        for (uint32_t a = 0; a < t->n_arrays; a++)
            e->sizes[a] = t->arr[a].in_key ? sizes[a] : 0;
        t->n_expansions++;
        if ((rc = expand(t, sizes, e)) != DVS_RANDOMIZE_OK) goto out;
    }
    e->used = ++t->stamp;

    rc = DVS_RANDOMIZE_ERROR;
    if (refs_reserve(t, e->n_refs)) goto out;
    memset(t->refs, 0, e->n_refs * sizeof *t->refs);
    if (n_vars) memcpy(t->refs, refs, n_vars * sizeof *refs);
    for (uint32_t a = 0; a < t->n_arrays; a++) {
        const array_t *ar = &t->arr[a];
        if (ar->local) {
            ref_set(&t->refs[e->base[a]], &sizes32[a], 32, 1);
            continue;
        }
        for (uint32_t k = 0; k < sizes[a]; k++)
            ref_set(&t->refs[e->base[a] + k],
                    (uint8_t *)arrays[a].data + (size_t)k * elem_bytes(ar->width), ar->width,
                    ar->is_signed);
    }
    rc = dvs_randomize(e->h, t->refs, NULL, seed);
    for (uint32_t a = 0; a < t->n_arrays && rc == DVS_RANDOMIZE_OK; a++)
        if (t->arr[a].local && arrays[a].n_elems)
            memcpy(arrays[a].data, t->arr[a].out,
                   (size_t)arrays[a].n_elems * elem_bytes(t->arr[a].width));
out:
    free(sizes32);
    free(sizes);
    return rc;
}
