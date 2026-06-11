#include "zsp_bbsolver.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zsp_aig.h"
#include "zsp_aig_cnf.h"
#include "zsp_bitblast.h"
#include "zsp_bvdom.h"
#include "zsp_pool.h"
#include "zsp_sat.h"

/* ----------------------------- types -------------------------------------- */

#define BB_INVALID_WIDTH 0

typedef struct {
    uint16_t width;
    uint8_t  is_signed;
    uint8_t  defined;   /* 0 if var_id not seen */
    int64_t  lo;
    int64_t  hi;
    zsp_bv_t bv;        /* lazily filled on first reference */
    int      bv_built;
} bb_var_t;

/* Memoization cache entry — one per ExprRef byte offset in the pool. */
typedef struct {
    zsp_bv_t bv;      /* size == 0 marks "not yet bit-blasted" */
} bb_cache_entry_t;

struct zsp_bbsolver_s {
    zsp_alloc_t    *alloc;
    SolveProblem   *problem;
    zsp_aig_t      *aig;
    zsp_sat_t      *sat;
    zsp_aig_cnf_t  *cnf;
    zsp_bitblast_t *bb;

    bb_var_t       *vars;
    uint32_t        n_vars;

    /* Equality-substitution map: subst[var_id] = ExprRef of the RHS to
     * bit-blast in place of the variable, or EXPR_NULL if no substitution
     * applies. Populated during the preprocessing pass from top-level
     * (= var expr) sub-asserts (descending through BIN_AND chains).
     *
     * resolving[var_id] is a re-entrancy guard: if bb_var_expr() is called
     * for a var while already resolving its substitution, fall back to a
     * fresh BV. Protects against transitive cycles in the substitution
     * graph that the linear-scan acyclicity check might miss. */
    ExprRef        *subst;       /* size n_vars, or NULL */
    uint8_t        *resolving;   /* size n_vars, or NULL */

    /* Bitmap of constraint indices to skip (those consumed by substitution).
     * Indexed by enumeration order of constraints_head walk. */
    uint8_t        *constraint_skip;
    uint32_t        n_constraints;

    uint64_t        n_substs;    /* statistics */

    /* Memoization of bit-blast results by ExprRef. Sparse array indexed
     * by ExprRef (byte offset into the pool). EXPR_CONST nodes are not
     * memoized because their result depends on the caller's hint_width;
     * every other node has a natural width that doesn't depend on hint
     * (variables ignore hint, ops widen to max(operands)). */
    bb_cache_entry_t *cache;
    uint32_t          cache_cap;

    int             last_result;
    int             had_error;
    /* Set when a construct is recognized but cannot be soundly bit-blasted
     * (e.g. signed div/mod — see A-4 soundness guards). Distinct from
     * had_error: maps to ZSP_BB_UNKNOWN so the caller defers to its fallback
     * rather than treating the result as a real verdict. */
    int             had_unsupported;
};

/* ----------------------------- helpers ------------------------------------ */

static void *xalloc(zsp_alloc_t *a, size_t sz) {
    return a ? ZSP_ALLOC(a, sz) : malloc(sz);
}
static void xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) ZSP_RELEASE(a, p, sz); else free(p);
}

static uint16_t max_w(uint16_t a, uint16_t b) { return a > b ? a : b; }

/* True if `ref`'s value should be interpreted as signed — i.e. its subtree
 * references a signed variable / constant / sign-extend. Used by the div/mod
 * guard: the bit-blaster only implements *unsigned* division/modulo, so a
 * signed operand would silently produce a wrong quotient. We conservatively
 * treat "contains a signed leaf" as signed and defer (ZSP_BB_UNKNOWN). */
static int subtree_is_signed(zsp_bbsolver_t *S, ExprRef ref, int depth) {
    if (ref == EXPR_NULL || depth > 256) return 0;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_CONST: return ((ExprConst *)kp)->is_signed != 0;
    case EXPR_VAR: {
        uint32_t id = ((ExprVar *)kp)->var_id;
        return id < S->n_vars && S->vars[id].is_signed;
    }
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)kp;
        return subtree_is_signed(S, b->lhs, depth + 1)
            || subtree_is_signed(S, b->rhs, depth + 1);
    }
    case EXPR_UNARY:
        return subtree_is_signed(S, ((ExprUnary *)kp)->operand, depth + 1);
    case EXPR_EXTEND:
        return ((ExprExtend *)kp)->sign_extend != 0;
    default:
        return 0;
    }
}

/* Whether the *value produced by* `ref` should be interpreted as signed when
 * width-extending it (e.g. when a `(= var expr)` substitution feeds a narrower
 * expr into a wider variable). This differs from subtree_is_signed: a
 * relational / equality / logical-connective op yields a 1-bit unsigned boolean
 * (0/1) *regardless* of its operands' signedness — sign-extending that boolean
 * would turn a true result (1) into all-ones (e.g. an int8 `eq` becoming -1).
 * Value-producing ops inherit signedness from their operands. */
static int result_is_signed(zsp_bbsolver_t *S, ExprRef ref, int depth) {
    if (ref == EXPR_NULL || depth > 256) return 0;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_CONST: return ((ExprConst *)kp)->is_signed != 0;
    case EXPR_VAR: {
        uint32_t id = ((ExprVar *)kp)->var_id;
        return id < S->n_vars && S->vars[id].is_signed;
    }
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)kp;
        switch (b->op) {
        /* Boolean-producing: comparisons and the logical connectives (the
         * bitwise forms are BIN_BAND/BIN_BOR/BIN_BXOR, handled below). */
        case BIN_EQ: case BIN_NEQ:
        case BIN_LT: case BIN_LTE: case BIN_GT: case BIN_GTE:
        case BIN_AND: case BIN_OR:
            return 0;
        default:
            return result_is_signed(S, b->lhs, depth + 1)
                || result_is_signed(S, b->rhs, depth + 1);
        }
    }
    case EXPR_UNARY: {
        ExprUnary *u = (ExprUnary *)kp;
        if (u->op == UN_NOT) return 0;   /* logical NOT → boolean */
        return result_is_signed(S, u->operand, depth + 1);
    }
    case EXPR_EXTEND:
        return ((ExprExtend *)kp)->sign_extend != 0;
    default:
        return 0;
    }
}

static zsp_bv_t zext_to(zsp_bbsolver_t *S, zsp_bv_t v, uint16_t target) {
    if (v.size >= target) return v;
    return zsp_bb_zero_ext(S->bb, v, (uint32_t)(target - v.size));
}

static zsp_bv_t sext_to(zsp_bbsolver_t *S, zsp_bv_t v, uint16_t target) {
    if (v.size >= target) return v;
    return zsp_bb_sign_ext(S->bb, v, (uint32_t)(target - v.size));
}

/* Forward decl — needed because bv_for_var may recurse through bb_expr. */
static zsp_bv_t bb_expr(zsp_bbsolver_t *S, ExprRef ref, uint16_t hint_width);

/* True iff `ref`'s subtree (transitively following the current subst[]
 * map for any EXPR_VAR encountered) reaches EXPR_VAR with id `target_var`.
 *
 * The transitive walk through subst[] is what makes this sound under the
 * chained-substitution scenario: x := f(y), y := g(x) would slip past
 * a direct reference check (neither RHS mentions the LHS directly), but
 * following subst[y] from the f(y) branch reaches x.
 *
 * `visited` is a S->n_vars bitmap to avoid revisiting (and infinite
 * recursion through pre-existing cycles in subst, although those
 * shouldn't exist if every prior insertion was guarded by this check). */
static int subst_reaches_var(zsp_bbsolver_t *S,
                             ExprRef ref,
                             uint32_t target_var,
                             uint8_t *visited) {
    if (ref == EXPR_NULL) return 0;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_CONST: return 0;
    case EXPR_VAR: {
        ExprVar *v = (ExprVar *)kp;
        if (v->var_id == target_var) return 1;
        if (v->var_id >= S->n_vars) return 0;
        if (visited[v->var_id]) return 0;
        visited[v->var_id] = 1;
        /* If this variable has its own substitution, look through it. */
        if (S->subst[v->var_id] != EXPR_NULL) {
            return subst_reaches_var(S, S->subst[v->var_id], target_var, visited);
        }
        return 0;
    }
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)kp;
        return subst_reaches_var(S, b->lhs, target_var, visited)
            || subst_reaches_var(S, b->rhs, target_var, visited);
    }
    case EXPR_UNARY:
        return subst_reaches_var(S, ((ExprUnary *)kp)->operand, target_var, visited);
    case EXPR_ITE: {
        ExprITE *e = (ExprITE *)kp;
        return subst_reaches_var(S, e->cond, target_var, visited)
            || subst_reaches_var(S, e->then_e, target_var, visited)
            || subst_reaches_var(S, e->else_e, target_var, visited);
    }
    case EXPR_IN_RANGE: {
        ExprInRange *r = (ExprInRange *)kp;
        return subst_reaches_var(S, r->value, target_var, visited)
            || subst_reaches_var(S, r->lo, target_var, visited)
            || subst_reaches_var(S, r->hi, target_var, visited);
    }
    case EXPR_IN_SET: {
        ExprInSet *iset = (ExprInSet *)kp;
        if (subst_reaches_var(S, iset->value, target_var, visited)) return 1;
        ExprRef *elems = expr_in_set_elems(S->problem, ref);
        for (uint32_t i = 0; i < iset->n_elems; i++)
            if (subst_reaches_var(S, elems[i], target_var, visited)) return 1;
        return 0;
    }
    case EXPR_EXTEND:
        return subst_reaches_var(S, ((ExprExtend *)kp)->operand, target_var, visited);
    case EXPR_EXTRACT:
        return subst_reaches_var(S, ((ExprExtract *)kp)->operand, target_var, visited);
    case EXPR_CONCAT: {
        ExprConcat *c = (ExprConcat *)kp;
        return subst_reaches_var(S, c->hi, target_var, visited)
            || subst_reaches_var(S, c->lo, target_var, visited);
    }
    default: return 0;
    }
}

/* Attempt to register a substitution var(vref) := eref. Returns 1 if
 * a new substitution was recorded. Rejects if eref transitively
 * (through the current subst graph) reaches the var being substituted —
 * this is the soundness gate. */
static int try_record_subst(zsp_bbsolver_t *S, ExprRef vref, ExprRef eref) {
    ExprKind *vk = (ExprKind *)POOL_PTR(S->problem, vref);
    if (!vk || *vk != EXPR_VAR) return 0;
    ExprVar *v = (ExprVar *)vk;
    if (v->var_id >= S->n_vars) return 0;
    if (S->subst[v->var_id] != EXPR_NULL) return 0;
    /* Transitive cycle check: would substituting create a cycle in the
     * subst graph reachable from this var? */
    memset(S->resolving, 0, S->n_vars);
    if (subst_reaches_var(S, eref, v->var_id, S->resolving)) return 0;
    S->subst[v->var_id] = eref;
    S->n_substs++;
    return 1;
}

/* Walk `root` descending through BIN_AND chains, looking for BIN_EQ
 * sub-asserts of the shape (= var expr). Records substitutions. Returns
 * 1 iff every sub-assert under this root was consumed (the whole root
 * can be skipped); 0 if any sub-assert remained (root must still be
 * encoded). */
static int collect_substs_from(zsp_bbsolver_t *S, ExprRef root) {
    if (root == EXPR_NULL) return 0;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, root);
    if (!kp) return 0;
    if (*kp == EXPR_BINARY) {
        ExprBinary *b = (ExprBinary *)kp;
        if (b->op == BIN_AND) {
            int l = collect_substs_from(S, b->lhs);
            int r = collect_substs_from(S, b->rhs);
            return l && r;
        }
        if (b->op == BIN_EQ) {
            if (try_record_subst(S, b->lhs, b->rhs)) return 1;
            if (try_record_subst(S, b->rhs, b->lhs)) return 1;
        }
    }
    return 0;
}

/* Pre-pass: scan all top-level constraints, populate S->subst, mark
 * fully-consumed constraints in S->constraint_skip. */
static void run_subst_pass(zsp_bbsolver_t *S) {
    /* Count constraints first so we can size constraint_skip. */
    uint32_t n = 0;
    for (ExprRef cur = S->problem->constraints_head; cur != EXPR_NULL; ) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        n++;
        cur = cs->next;
    }
    S->n_constraints = n;
    if (n == 0 || S->n_vars == 0) return;
    S->subst = (ExprRef *)xalloc(S->alloc, S->n_vars * sizeof(ExprRef));
    S->resolving = (uint8_t *)xalloc(S->alloc, S->n_vars * sizeof(uint8_t));
    S->constraint_skip = (uint8_t *)xalloc(S->alloc, n * sizeof(uint8_t));
    if (!S->subst || !S->resolving || !S->constraint_skip) return;
    for (uint32_t i = 0; i < S->n_vars; i++) S->subst[i] = EXPR_NULL;
    memset(S->resolving, 0, S->n_vars);
    memset(S->constraint_skip, 0, n);

    uint32_t idx = 0;
    for (ExprRef cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        if (collect_substs_from(S, cs->root)) {
            S->constraint_skip[idx] = 1;
        }
        cur = cs->next;
    }
}

/* Build (or fetch) the bit-vector for a declared variable.
 *
 * If the variable's bounds [lo, hi] (interpreted unsigned) yield a useful
 * bit-fix mask — e.g. an 8-bit variable bounded to [0, 7] has its top 5
 * bits fixed-0 — those bits are emitted as AIG constants instead of fresh
 * inputs. Saves SAT variables and clauses on every bounded variable.
 *
 * Signed variables with mixed-sign ranges are not folded (the unsigned
 * range straddles the midpoint and no bit is uniformly fixed); they get
 * the all-fresh behavior. */
static zsp_bv_t bv_for_var(zsp_bbsolver_t *S, uint32_t var_id) {
    assert(var_id < S->n_vars);
    bb_var_t *v = &S->vars[var_id];
    if (!v->bv_built) {
        /* If a substitution applies and we aren't already resolving it
         * (cycle guard), bit-blast the substituted expression and use
         * its result. The expression's natural width may differ from
         * the variable's declared width — adjust via zext or extract. */
        if (S->subst && S->subst[var_id] != EXPR_NULL && !S->resolving[var_id]) {
            S->resolving[var_id] = 1;
            zsp_bv_t e = bb_expr(S, S->subst[var_id], v->width);
            S->resolving[var_id] = 0;
            if (!S->had_error) {
                if (e.size < v->width) {
                    /* Extend per the *expression's* result signedness, not the
                     * variable's: a boolean result (e.g. `var == (a == 5)`) is
                     * unsigned 0/1 and must zero-extend even into a signed var,
                     * else true (1) becomes all-ones (-1). */
                    e = result_is_signed(S, S->subst[var_id], 0)
                            ? sext_to(S, e, v->width)
                            : zext_to(S, e, v->width);
                } else if (e.size > v->width) {
                    e = zsp_bb_extract(S->bb, e, v->width - 1, 0);
                }
                v->bv = e;
                v->bv_built = 1;
                return v->bv;
            }
            /* fall through on error to allocate fresh */
        }

        /* Bit-fix folding is a 64-bit-domain optimization: the [lo,hi] range and
         * the fixed/unknown masks are uint64. For width > 64 it cannot represent
         * the true range and would wrongly fix the bits above 63 to 0 (e.g. a
         * 128-bit unconstrained var passed [0, INT64_MAX] would have bits 63..127
         * pinned). Skip it for wide vars — they get all-fresh bits, and any real
         * sub-range is enforced by assert_var_bounds / explicit constraints. */
        int use_fix = !v->is_signed && v->lo >= 0 && v->hi >= v->lo
                      && v->width <= 64;
        if (use_fix) {
            zsp_bvdom_t d;
            zsp_bvdom_init_from_range_u(&d, v->width,
                                        (uint64_t)v->lo, (uint64_t)v->hi);
            uint64_t mask = zsp_bvdom_mask(v->width);
            /* fixed bit mask = bits where lo == hi (within width). */
            uint64_t fixed_mask = ~(d.lo ^ d.hi) & mask;
            uint64_t unknown_mask = ~fixed_mask & mask;
            v->bv = zsp_bb_constant_dom(S->bb, v->width, d.lo, unknown_mask);
        } else {
            v->bv = zsp_bb_constant(S->bb, v->width);
        }
        v->bv_built = 1;
    }
    return v->bv;
}

/* ----------------------------- expression dispatch ------------------------ */

/* Returns the bit-blasted value of expression `ref`. `hint_width` may be
 * passed to size EXPR_CONST values to context; pass 0 to use the natural
 * width (32 for unconstrained constants).
 * On error, sets S->had_error = 1 and returns {NULL, 0}. */
static zsp_bv_t bb_expr(zsp_bbsolver_t *S, ExprRef ref, uint16_t hint_width);

/* Same as bb_expr but the result is a 1-bit predicate. */
static zsp_bv_t bb_predicate(zsp_bbsolver_t *S, ExprRef ref);

/* Build a width-`w` bit-vector for the int64 `value`. For w <= 64 this is the
 * exact low-w-bit pattern (zsp_bb_value_u64). For w > 64, value_u64 alone would
 * zero-fill bits 64+, corrupting a negative value; build the 64-bit pattern and
 * extend — sign-extend when `is_signed` (a negative value must set the high
 * bits), zero-extend otherwise. */
static zsp_bv_t bb_value_i64(zsp_bbsolver_t *S, uint16_t w, int64_t value,
                             int is_signed) {
    if (w <= 64) return zsp_bb_value_u64(S->bb, w, (uint64_t)value);
    zsp_bv_t lo64 = zsp_bb_value_u64(S->bb, 64, (uint64_t)value);
    return is_signed ? sext_to(S, lo64, w) : zext_to(S, lo64, w);
}

static zsp_bv_t bb_const(zsp_bbsolver_t *S, const ExprConst *c, uint16_t hint) {
    uint16_t w = hint ? hint : 32;
    return bb_value_i64(S, w, c->value, c->is_signed);
}

static zsp_bv_t bb_var_expr(zsp_bbsolver_t *S, const ExprVar *v) {
    return bv_for_var(S, v->var_id);
}

static zsp_bv_t err_bv(zsp_bbsolver_t *S, const char *msg) {
    fprintf(stderr, "[zsp_bbsolver] error: %s\n", msg);
    S->had_error = 1;
    zsp_bv_t empty = { NULL, 0 };
    return empty;
}

static zsp_bv_t bb_binary(zsp_bbsolver_t *S, const ExprBinary *b, uint16_t hint) {
    /* Comparison / logical ops always produce 1-bit; arithmetic / bitwise
     * widen lhs and rhs to a common width = max(lhs, rhs, hint). */
    switch (b->op) {
    case BIN_AND:
    case BIN_OR: {
        zsp_bv_t a = bb_predicate(S, b->lhs);
        zsp_bv_t c = bb_predicate(S, b->rhs);
        if (S->had_error) return a;
        return (b->op == BIN_AND) ? zsp_bb_and(S->bb, a, c)
                                  : zsp_bb_or (S->bb, a, c);
    }
    case BIN_EQ:
    case BIN_NEQ: {
        /* Recurse with hint=0 on lhs to learn its natural width, then size
         * rhs to match. */
        zsp_bv_t l = bb_expr(S, b->lhs, 0);
        if (S->had_error) return l;
        zsp_bv_t r = bb_expr(S, b->rhs, l.size);
        if (S->had_error) return r;
        uint16_t w = max_w(l.size, r.size);
        if (l.size < w) l = zext_to(S, l, w);
        if (r.size < w) r = zext_to(S, r, w);
        zsp_bv_t eq = zsp_bb_eq(S->bb, l, r);
        if (b->op == BIN_NEQ) eq = zsp_bb_not(S->bb, eq);
        return eq;
    }
    case BIN_LT:
    case BIN_LTE:
    case BIN_GT:
    case BIN_GTE: {
        zsp_bv_t l = bb_expr(S, b->lhs, 0);
        if (S->had_error) return l;
        zsp_bv_t r = bb_expr(S, b->rhs, l.size);
        if (S->had_error) return r;
        /* Determine signedness: peek at lhs ExprKind for an EXPR_VAR. */
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, b->lhs);
        int is_signed = 0;
        if (kp && *kp == EXPR_VAR) {
            ExprVar *vv = (ExprVar *)kp;
            if (vv->var_id < S->n_vars) is_signed = S->vars[vv->var_id].is_signed;
        }
        uint16_t w = max_w(l.size, r.size);
        if (is_signed) {
            if (l.size < w) l = sext_to(S, l, w);
            if (r.size < w) r = sext_to(S, r, w);
        } else {
            if (l.size < w) l = zext_to(S, l, w);
            if (r.size < w) r = zext_to(S, r, w);
        }
        zsp_bv_t lt;
        switch (b->op) {
        case BIN_LT:  lt = is_signed ? zsp_bb_slt(S->bb, l, r) : zsp_bb_ult(S->bb, l, r); break;
        case BIN_LTE: lt = is_signed ? zsp_bb_slt(S->bb, l, r) : zsp_bb_ult(S->bb, l, r);
                      lt = zsp_bb_or(S->bb, lt, zsp_bb_eq(S->bb, l, r)); break;
        case BIN_GT:  lt = is_signed ? zsp_bb_slt(S->bb, r, l) : zsp_bb_ult(S->bb, r, l); break;
        case BIN_GTE: lt = is_signed ? zsp_bb_slt(S->bb, r, l) : zsp_bb_ult(S->bb, r, l);
                      lt = zsp_bb_or(S->bb, lt, zsp_bb_eq(S->bb, l, r)); break;
        default: lt = err_bv(S, "unreachable cmp"); break;
        }
        return lt;
    }
    case BIN_LSHIFT:
    case BIN_RSHIFT: {
        zsp_bv_t l = bb_expr(S, b->lhs, hint);
        if (S->had_error) return l;
        zsp_bv_t r = bb_expr(S, b->rhs, l.size);
        if (S->had_error) return r;
        if (r.size != l.size) r = zext_to(S, r, l.size);
        return (b->op == BIN_LSHIFT) ? zsp_bb_shl(S->bb, l, r) : zsp_bb_shr(S->bb, l, r);
    }
    case BIN_ADD:
    case BIN_SUB:
    case BIN_MUL:
    case BIN_BAND:
    case BIN_BOR:
    case BIN_BXOR: {
        zsp_bv_t l = bb_expr(S, b->lhs, hint);
        if (S->had_error) return l;
        zsp_bv_t r = bb_expr(S, b->rhs, l.size > hint ? l.size : hint);
        if (S->had_error) return r;
        uint16_t w = max_w(l.size, r.size);
        if (hint > w) w = hint;
        if (l.size < w) l = zext_to(S, l, w);
        if (r.size < w) r = zext_to(S, r, w);
        switch (b->op) {
        case BIN_ADD:  return zsp_bb_add(S->bb, l, r);
        case BIN_SUB:  return zsp_bb_sub(S->bb, l, r);
        case BIN_MUL:  return zsp_bb_mul(S->bb, l, r);
        case BIN_BAND: return zsp_bb_and(S->bb, l, r);
        case BIN_BOR:  return zsp_bb_or (S->bb, l, r);
        case BIN_BXOR: return zsp_bb_xor(S->bb, l, r);
        default: break;
        }
        return err_bv(S, "unreachable arith");
    }
    case BIN_DIV:
    case BIN_MOD: {
        /* Unsigned integer division/modulo only. Signed div/mod would need
         * the SMT-LIB signed-div semantics (round-toward-zero), unimplemented.
         * A-4 guard: if either operand is signed, mark the problem unsupported
         * so check() returns ZSP_BB_UNKNOWN and the caller defers (the primary
         * engine / Boolector handle signed div correctly) rather than computing
         * a wrong unsigned quotient. */
        if (subtree_is_signed(S, b->lhs, 0) || subtree_is_signed(S, b->rhs, 0))
            S->had_unsupported = 1;
        zsp_bv_t l = bb_expr(S, b->lhs, hint);
        if (S->had_error) return l;
        zsp_bv_t r = bb_expr(S, b->rhs, l.size > hint ? l.size : hint);
        if (S->had_error) return r;
        uint16_t w = max_w(l.size, r.size);
        if (hint > w) w = hint;
        if (l.size < w) l = zext_to(S, l, w);
        if (r.size < w) r = zext_to(S, r, w);
        return (b->op == BIN_DIV) ? zsp_bb_udiv(S->bb, l, r)
                                  : zsp_bb_urem(S->bb, l, r);
    }
    default:
        return err_bv(S, "unknown BinOp");
    }
}

static zsp_bv_t bb_unary(zsp_bbsolver_t *S, const ExprUnary *u, uint16_t hint) {
    switch (u->op) {
    case UN_NOT: {
        zsp_bv_t a = bb_predicate(S, u->operand);
        if (S->had_error) return a;
        return zsp_bb_not(S->bb, a);
    }
    case UN_NEG: {
        zsp_bv_t a = bb_expr(S, u->operand, hint);
        if (S->had_error) return a;
        return zsp_bb_neg(S->bb, a);
    }
    case UN_INVERT: {
        zsp_bv_t a = bb_expr(S, u->operand, hint);
        if (S->had_error) return a;
        return zsp_bb_not(S->bb, a);
    }
    }
    return err_bv(S, "unknown UnaryOp");
}

static zsp_bv_t bb_ite(zsp_bbsolver_t *S, const ExprITE *e, uint16_t hint) {
    zsp_bv_t c = bb_predicate(S, e->cond);
    if (S->had_error) return c;
    zsp_bv_t t = bb_expr(S, e->then_e, hint);
    if (S->had_error) return t;
    zsp_bv_t f = bb_expr(S, e->else_e, t.size > hint ? t.size : hint);
    if (S->had_error) return f;
    uint16_t w = max_w(t.size, f.size);
    if (hint > w) w = hint;
    if (t.size < w) t = zext_to(S, t, w);
    if (f.size < w) f = zext_to(S, f, w);
    return zsp_bb_ite(S->bb, c.bits[0], t, f);
}

static zsp_bv_t bb_in_range(zsp_bbsolver_t *S, const ExprInRange *r) {
    /* in_range(value, lo, hi) := value >= lo AND value <= hi.
     * For signedness, peek at `value` like in cmp. */
    zsp_bv_t v = bb_expr(S, r->value, 0);
    if (S->had_error) return v;
    zsp_bv_t lo = bb_expr(S, r->lo, v.size);
    if (S->had_error) return lo;
    zsp_bv_t hi = bb_expr(S, r->hi, v.size);
    if (S->had_error) return hi;
    uint16_t w = max_w(max_w(v.size, lo.size), hi.size);
    int is_signed = 0;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, r->value);
    if (kp && *kp == EXPR_VAR) {
        ExprVar *vv = (ExprVar *)kp;
        if (vv->var_id < S->n_vars) is_signed = S->vars[vv->var_id].is_signed;
    }
    if (is_signed) {
        if (v.size  < w) v  = sext_to(S, v,  w);
        if (lo.size < w) lo = sext_to(S, lo, w);
        if (hi.size < w) hi = sext_to(S, hi, w);
    } else {
        if (v.size  < w) v  = zext_to(S, v,  w);
        if (lo.size < w) lo = zext_to(S, lo, w);
        if (hi.size < w) hi = zext_to(S, hi, w);
    }
    /* v >= lo  <=>  NOT (v <_u lo)   (and signed analog) */
    zsp_bv_t v_lt_lo = is_signed ? zsp_bb_slt(S->bb, v, lo) : zsp_bb_ult(S->bb, v, lo);
    zsp_bv_t hi_lt_v = is_signed ? zsp_bb_slt(S->bb, hi, v) : zsp_bb_ult(S->bb, hi, v);
    zsp_bv_t ge_lo = zsp_bb_not(S->bb, v_lt_lo);
    zsp_bv_t le_hi = zsp_bb_not(S->bb, hi_lt_v);
    return zsp_bb_and(S->bb, ge_lo, le_hi);
}

static zsp_bv_t bb_in_set(zsp_bbsolver_t *S, ExprRef ref) {
    ExprInSet *node = (ExprInSet *)POOL_PTR(S->problem, ref);
    ExprRef *elems = expr_in_set_elems(S->problem, ref);
    zsp_bv_t v = bb_expr(S, node->value, 0);
    if (S->had_error) return v;
    /* OR of (v == e[i]) */
    zsp_bv_t acc = { NULL, 0 };
    for (uint32_t i = 0; i < node->n_elems; i++) {
        zsp_bv_t ei = bb_expr(S, elems[i], v.size);
        if (S->had_error) return ei;
        uint16_t w = max_w(v.size, ei.size);
        zsp_bv_t vw = v.size < w ? zext_to(S, v, w) : v;
        zsp_bv_t ew = ei.size < w ? zext_to(S, ei, w) : ei;
        zsp_bv_t eq = zsp_bb_eq(S->bb, vw, ew);
        if (i == 0) acc = eq;
        else        acc = zsp_bb_or(S->bb, acc, eq);
    }
    if (acc.size == 0) {
        /* Empty set is always false. */
        acc = zsp_bb_value_u64(S->bb, 1, 0);
    }
    return acc;
}

static zsp_bv_t bb_extend(zsp_bbsolver_t *S, const ExprExtend *e) {
    zsp_bv_t op = bb_expr(S, e->operand, e->from_bits);
    if (S->had_error) return op;
    if (op.size > e->from_bits) {
        /* Truncate down — shouldn't happen but be safe. */
        op = zsp_bb_extract(S->bb, op, e->from_bits - 1, 0);
    } else if (op.size < e->from_bits) {
        op = zext_to(S, op, e->from_bits);
    }
    uint32_t n = (uint32_t)e->to_bits - (uint32_t)e->from_bits;
    if (n == 0) return op;
    return e->sign_extend ? zsp_bb_sign_ext(S->bb, op, n)
                          : zsp_bb_zero_ext(S->bb, op, n);
}

static zsp_bv_t bb_extract(zsp_bbsolver_t *S, const ExprExtract *e) {
    /* operand width must be at least hi_bit + 1 — infer from operand. */
    zsp_bv_t op = bb_expr(S, e->operand, 0);
    if (S->had_error) return op;
    if (op.size <= e->hi_bit) {
        /* zero-extend operand up to hi_bit+1 bits */
        op = zext_to(S, op, e->hi_bit + 1);
    }
    return zsp_bb_extract(S->bb, op, e->hi_bit, e->lo_bit);
}

static zsp_bv_t bb_concat(zsp_bbsolver_t *S, const ExprConcat *e) {
    zsp_bv_t lo = bb_expr(S, e->lo, e->lo_width);
    if (S->had_error) return lo;
    if (lo.size > e->lo_width) {
        lo = zsp_bb_extract(S->bb, lo, e->lo_width - 1, 0);
    } else if (lo.size < e->lo_width) {
        lo = zext_to(S, lo, e->lo_width);
    }
    zsp_bv_t hi = bb_expr(S, e->hi, 0);
    if (S->had_error) return hi;
    return zsp_bb_concat(S->bb, hi, lo);
}

static zsp_bv_t bb_expr(zsp_bbsolver_t *S, ExprRef ref, uint16_t hint_width) {
    if (S->had_error) { zsp_bv_t e = {NULL, 0}; return e; }
    if (ref == EXPR_NULL) return err_bv(S, "EXPR_NULL");

    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return err_bv(S, "bad ExprRef");

    /* Memoization: most ExprKinds have a hint-independent natural width.
     * Cache them by ExprRef alone. EXPR_CONST is hint-dependent (sized to
     * caller context) so it stays uncached and cheap. DV_BB_NO_MEMO
     * disables the cache for debugging. */
    int memoize = (*kp != EXPR_CONST) && getenv("DV_BB_NO_MEMO") == NULL;
    if (memoize && ref < S->cache_cap && S->cache[ref].bv.size != 0) {
        return S->cache[ref].bv;
    }

    zsp_bv_t out;
    switch (*kp) {
    case EXPR_CONST:    return bb_const(S, (ExprConst *)kp, hint_width);
    case EXPR_VAR:      out = bb_var_expr(S, (ExprVar *)kp); break;
    case EXPR_BINARY:   out = bb_binary(S, (ExprBinary *)kp, hint_width); break;
    case EXPR_UNARY:    out = bb_unary(S, (ExprUnary *)kp, hint_width); break;
    case EXPR_ITE:      out = bb_ite(S, (ExprITE *)kp, hint_width); break;
    case EXPR_IN_RANGE: out = bb_in_range(S, (ExprInRange *)kp); break;
    case EXPR_IN_SET:   out = bb_in_set(S, ref); break;
    case EXPR_EXTEND:   out = bb_extend(S, (ExprExtend *)kp); break;
    case EXPR_EXTRACT:  out = bb_extract(S, (ExprExtract *)kp); break;
    case EXPR_CONCAT:   out = bb_concat(S, (ExprConcat *)kp); break;
    case EXPR_SUM:
    case EXPR_COUNTONES:
    case EXPR_CLOG2:
    case EXPR_ARRAY_SELECT:
        return err_bv(S, "high-level IR node not yet supported (SUM/COUNTONES/CLOG2/ARRAY_SELECT)");
    default:
        return err_bv(S, "unknown ExprKind");
    }

    if (memoize && !S->had_error && ref < S->cache_cap) {
        S->cache[ref].bv = out;
    }
    return out;
}

static zsp_bv_t bb_predicate(zsp_bbsolver_t *S, ExprRef ref) {
    zsp_bv_t r = bb_expr(S, ref, 1);
    if (S->had_error) return r;
    if (r.size != 1) {
        /* Reduce to (r != 0) */
        zsp_bv_t zero = zsp_bb_value_u64(S->bb, r.size, 0);
        zsp_bv_t eq0 = zsp_bb_eq(S->bb, r, zero);
        r = zsp_bb_not(S->bb, eq0);
    }
    return r;
}

/* ----------------------------- bound assertions --------------------------- */

/* Encode `lo <= v <= hi` for variable v of width w. If is_signed, use signed
 * comparison; else unsigned. Caller asserts at top level. Returns 1-bit BV
 * or no-op (returns NULL bits) if bounds are vacuous. */
static int assert_var_bounds(zsp_bbsolver_t *S, uint32_t var_id) {
    bb_var_t *v = &S->vars[var_id];
    /* Natural full range for the width. */
    int64_t natural_lo, natural_hi;
    if (v->is_signed) {
        if (v->width >= 64) { natural_lo = INT64_MIN; natural_hi = INT64_MAX; }
        else {
            natural_lo = -((int64_t)1 << (v->width - 1));
            natural_hi =  ((int64_t)1 << (v->width - 1)) - 1;
        }
    } else {
        natural_lo = 0;
        natural_hi = (v->width >= 64) ? INT64_MAX : (((int64_t)1 << v->width) - 1);
    }
    int need_lo = v->lo != natural_lo;
    int need_hi = v->hi != natural_hi;
    if (!need_lo && !need_hi) return 0;

    zsp_bv_t bv = bv_for_var(S, var_id);
    /* bb_value_i64 sign/zero-extends correctly for width > 64 (value_u64 alone
     * would zero-fill bits 64+, corrupting a negative bound). */
    zsp_bv_t lo = bb_value_i64(S, v->width, v->lo, v->is_signed);
    zsp_bv_t hi = bb_value_i64(S, v->width, v->hi, v->is_signed);

    zsp_bv_t pred = zsp_bb_value_u64(S->bb, 1, 1);
    if (need_lo) {
        zsp_bv_t v_lt_lo = v->is_signed ? zsp_bb_slt(S->bb, bv, lo)
                                        : zsp_bb_ult(S->bb, bv, lo);
        zsp_bv_t ge_lo = zsp_bb_not(S->bb, v_lt_lo);
        pred = zsp_bb_and(S->bb, pred, ge_lo);
    }
    if (need_hi) {
        zsp_bv_t hi_lt_v = v->is_signed ? zsp_bb_slt(S->bb, hi, bv)
                                        : zsp_bb_ult(S->bb, hi, bv);
        zsp_bv_t le_hi = zsp_bb_not(S->bb, hi_lt_v);
        pred = zsp_bb_and(S->bb, pred, le_hi);
    }
    zsp_aig_cnf_encode(S->cnf, pred.bits[0], /*top_level=*/1);
    return 0;
}

/* ----------------------------- public API --------------------------------- */

zsp_bbsolver_t *zsp_bbsolver_new(zsp_alloc_t *alloc, SolveProblem *problem) {
    if (!problem) return NULL;
    zsp_bbsolver_t *S = (zsp_bbsolver_t *)xalloc(alloc, sizeof(*S));
    if (!S) return NULL;
    memset(S, 0, sizeof(*S));
    S->alloc = alloc;
    S->problem = problem;
    S->aig = zsp_aig_new(alloc);
    S->sat = zsp_sat_new(alloc);
    S->cnf = zsp_aig_cnf_new(alloc, S->aig, S->sat);
    S->bb  = zsp_bitblast_new(alloc, S->aig);
    if (!S->aig || !S->sat || !S->cnf || !S->bb) {
        zsp_bbsolver_free(S);
        return NULL;
    }

    /* Collect variables — find the max var_id, allocate table. */
    uint32_t max_id = 0;
    ExprRef cur = problem->vars_head;
    while (cur != EXPR_NULL) {
        VarSpec *vs = (VarSpec *)POOL_PTR(problem, cur);
        if (vs->var_id > max_id) max_id = vs->var_id;
        cur = vs->next;
    }
    uint32_t n = problem->n_vars > 0 ? max_id + 1 : 0;
    S->n_vars = n;
    if (n > 0) {
        S->vars = (bb_var_t *)xalloc(alloc, n * sizeof(bb_var_t));
        memset(S->vars, 0, n * sizeof(bb_var_t));
        cur = problem->vars_head;
        while (cur != EXPR_NULL) {
            VarSpec *vs = (VarSpec *)POOL_PTR(problem, cur);
            bb_var_t *v = &S->vars[vs->var_id];
            v->width = vs->width;
            v->is_signed = vs->is_signed;
            v->defined = 1;
            v->lo = vs->lo;
            v->hi = vs->hi;
            cur = vs->next;
        }
    }

    /* Memoization cache: sized to the pool's current used range, since
     * ExprRefs are byte offsets and never exceed used. Zero-initialized
     * (bv.size == 0 marks "not yet bit-blasted"). */
    S->cache_cap = zsp_pool_used(&problem->pool);
    if (S->cache_cap > 0) {
        S->cache = (bb_cache_entry_t *)xalloc(alloc,
                                              S->cache_cap * sizeof(bb_cache_entry_t));
        if (S->cache) memset(S->cache, 0, S->cache_cap * sizeof(bb_cache_entry_t));
    }
    return S;
}

void zsp_bbsolver_free(zsp_bbsolver_t *S) {
    if (!S) return;
    if (S->bb)  zsp_bitblast_free(S->bb);
    if (S->cnf) zsp_aig_cnf_free(S->cnf);
    if (S->sat) zsp_sat_free(S->sat);
    if (S->aig) zsp_aig_free(S->aig);
    xfree(S->alloc, S->vars, S->n_vars * sizeof(bb_var_t));
    xfree(S->alloc, S->cache, S->cache_cap * sizeof(bb_cache_entry_t));
    xfree(S->alloc, S->subst, S->n_vars * sizeof(ExprRef));
    xfree(S->alloc, S->resolving, S->n_vars * sizeof(uint8_t));
    xfree(S->alloc, S->constraint_skip, S->n_constraints * sizeof(uint8_t));
    xfree(S->alloc, S, sizeof(*S));
}

int zsp_bbsolver_check(zsp_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->problem) return ZSP_BB_ERROR;

    /* Seed kissat's randomness so repeated checks can return different models
     * (the completeness-fallback's only source of stimulus diversity). */
    zsp_sat_set_seed(S->sat, seed);

    /* A-4 soundness guard: the bit-blaster does not encode AllDifferent or
     * Source groups. pyvsc never emits these on the dv-solve path (unique
     * lowers to NEQ pairs; no sources), but if one ever appears we must NOT
     * silently drop a hard constraint — defer (UNKNOWN) instead. */
    if (S->problem->allDiff_head != EXPR_NULL ||
        S->problem->sources_head != EXPR_NULL) {
        S->last_result = ZSP_BB_UNKNOWN;
        return ZSP_BB_UNKNOWN;
    }

    int stats_enabled = getenv("DV_BB_STATS") != NULL;
    int subst_enabled = getenv("DV_BB_NO_SUBST") == NULL;
    struct timespec t0, t1, t2;
    if (stats_enabled) clock_gettime(CLOCK_MONOTONIC, &t0);

    if (subst_enabled) run_subst_pass(S);
    /* run_subst_pass uses S->resolving as a scratch visited bitmap
     * during cycle detection. Reset it before the bit-blast path
     * starts using it as the recursion guard in bv_for_var. */
    if (S->resolving) memset(S->resolving, 0, S->n_vars);

    /* Encode each constraint as a top-level assertion. Skip constraints
     * fully consumed by the substitution pass. */
    uint32_t idx = 0;
    for (ExprRef cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        if (S->constraint_skip && S->constraint_skip[idx]) {
            cur = cs->next;
            continue;
        }
        zsp_bv_t pred = bb_predicate(S, cs->root);
        if (S->had_error) { S->last_result = ZSP_BB_ERROR; return ZSP_BB_ERROR; }
        if (S->had_unsupported) { S->last_result = ZSP_BB_UNKNOWN; return ZSP_BB_UNKNOWN; }
        zsp_aig_cnf_encode(S->cnf, pred.bits[0], /*top_level=*/1);
        cur = cs->next;
    }

    /* Force-build any substituted variable that no surviving constraint
     * referenced. The subst pass consumed its defining constraint (e.g. the
     * sole `x == 42`), so without this its bv is never built and
     * zsp_bbsolver_value() would default it to 0 instead of the substituted
     * value. bv_for_var() ties var->bv to the (bit-blasted) RHS, so the model
     * — and thus readback — reflects the substitution. */
    if (S->subst) {
        for (uint32_t i = 0; i < S->n_vars; i++) {
            if (S->vars[i].defined && S->subst[i] != EXPR_NULL) {
                /* A substituted var's bv is a *computed* expression (AND/XOR
                 * gates), unlike a normal var whose bits are AIG inputs (always
                 * readable). Its gate bits are only recoverable if encoded into
                 * the CNF, and a constraint may reference only *some* of them
                 * (e.g. the sampler hashes just a few low bits via extract), or
                 * none (its defining `==` was consumed by the subst pass). Build
                 * the bv and encode *every* bit at non-top-level so the SAT model
                 * assigns all of them consistently with the substituted
                 * expression's inputs — otherwise zsp_aig_cnf_value() reads each
                 * unencoded bit as 0, a silent wrong model. Encoding is
                 * idempotent for already-encoded bits. */
                zsp_bv_t bv = bv_for_var(S, i);
                if (S->had_error) {
                    S->last_result = ZSP_BB_ERROR;
                    return ZSP_BB_ERROR;
                }
                for (uint32_t bit = 0; bit < bv.size; bit++) {
                    zsp_aig_cnf_encode(S->cnf, bv.bits[bit], /*top_level=*/0);
                }
            }
        }
    }

    /* A construct may have been bit-blasted only in the force-build pass above
     * (e.g. a substituted `y == <signed-div>` whose constraint was skipped),
     * so re-check the A-4 unsupported flag before committing to a verdict. */
    if (S->had_unsupported) { S->last_result = ZSP_BB_UNKNOWN; return ZSP_BB_UNKNOWN; }

    /* Variable bounds — only for variables that were actually referenced
     * (and therefore had a bv built). Vars never referenced are unconstrained. */
    for (uint32_t i = 0; i < S->n_vars; i++) {
        if (S->vars[i].defined && S->vars[i].bv_built) {
            assert_var_bounds(S, i);
        }
    }

    if (stats_enabled) clock_gettime(CLOCK_MONOTONIC, &t1);
    int rc = zsp_sat_solve(S->sat);
    if (stats_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double bb_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                     + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        double sat_ms = (t2.tv_sec - t1.tv_sec) * 1000.0
                      + (t2.tv_nsec - t1.tv_nsec) / 1e6;
        fprintf(stderr,
                "[bb-stats] bb=%.2fms sat=%.2fms ands=%llu vars=%llu clauses=%llu substs=%llu\n",
                bb_ms, sat_ms,
                (unsigned long long)zsp_aig_num_ands(S->aig),
                (unsigned long long)zsp_aig_cnf_num_vars(S->cnf),
                (unsigned long long)zsp_aig_cnf_num_clauses(S->cnf),
                (unsigned long long)S->n_substs);
    }
    S->last_result = rc;
    return rc;
}

int zsp_bbsolver_value(zsp_bbsolver_t *S, uint32_t var_id, int64_t *out_value) {
    if (!S || !out_value) return -1;
    if (S->last_result != ZSP_BB_SAT) return -1;
    if (var_id >= S->n_vars) return -1;
    bb_var_t *v = &S->vars[var_id];
    if (!v->defined) return -1;
    if (!v->bv_built) {
        /* Variable never referenced — pick zero by convention. */
        *out_value = 0;
        return 0;
    }
    /* bits[] are stored MSB-first: bits[0] is the high bit (logical position
     * size-1), bits[size-1] is the LSB. Read only the low 64 bits here; the
     * shift stays < 64 so there is no UB even for a wide var (the high bits are
     * simply dropped — callers needing them use zsp_bbsolver_value_wide). */
    uint64_t val = 0;
    uint32_t n = v->bv.size < 64 ? v->bv.size : 64;
    for (uint32_t p = 0; p < n; p++) {
        /* logical bit p (0 = LSB) lives at bits[size-1-p] */
        int b = zsp_aig_cnf_value(S->cnf, v->bv.bits[v->bv.size - 1 - p]);
        if (b == 1) val |= (uint64_t)1 << p;
    }
    if (v->is_signed && v->width < 64) {
        /* sign-extend from `width` to 64 */
        uint64_t sign_bit = (uint64_t)1 << (v->width - 1);
        if (val & sign_bit) {
            val |= ~(((uint64_t)1 << v->width) - 1);
        }
    }
    *out_value = (int64_t)val;
    return 0;
}

int zsp_bbsolver_value_wide(zsp_bbsolver_t *S, uint32_t var_id,
                            uint64_t *limbs, uint32_t n_limbs) {
    if (!S || !limbs || n_limbs == 0) return -1;
    if (S->last_result != ZSP_BB_SAT) return -1;
    if (var_id >= S->n_vars) return -1;
    bb_var_t *v = &S->vars[var_id];
    if (!v->defined) return -1;
    for (uint32_t i = 0; i < n_limbs; i++) limbs[i] = 0;
    if (!v->bv_built) {
        /* Variable never referenced — zero by convention (limbs already 0). */
        return 0;
    }
    /* Little-endian limbs: logical bit p (0 = LSB) → limbs[p/64] bit (p%64).
     * bits[] is MSB-first, so logical bit p is at bits[size-1-p]. The shift is
     * always < 64, so no UB regardless of width. */
    uint32_t cap = n_limbs * 64;
    uint32_t n = v->bv.size < cap ? v->bv.size : cap;
    for (uint32_t p = 0; p < n; p++) {
        int b = zsp_aig_cnf_value(S->cnf, v->bv.bits[v->bv.size - 1 - p]);
        if (b == 1) limbs[p >> 6] |= (uint64_t)1 << (p & 63);
    }
    return 0;
}

uint64_t zsp_bbsolver_num_aig_ands(const zsp_bbsolver_t *S) {
    return S ? zsp_aig_num_ands(S->aig) : 0;
}
uint64_t zsp_bbsolver_num_sat_clauses(const zsp_bbsolver_t *S) {
    return S ? zsp_aig_cnf_num_clauses(S->cnf) : 0;
}
uint64_t zsp_bbsolver_num_sat_vars(const zsp_bbsolver_t *S) {
    return S ? zsp_aig_cnf_num_vars(S->cnf) : 0;
}
