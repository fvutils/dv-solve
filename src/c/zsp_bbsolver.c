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
#include "zsp_thread.h"

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
    int      bounds_asserted; /* lo<=v<=hi already emitted (incremental reuse) */
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

    uint64_t        seed;    /* from zsp_bbsolver_check; 0 = deterministic */

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

    /* DSE-2 soft-aware serve. soft_keep[i] (indexed by softs_head walk order)
     * selects which soft constraints to encode as *hard* top-level assertions
     * for this check. NULL (the default) honors no soft — the legacy soft-less
     * behavior. The MaxSAT wrapper (zsp_bbsolver_solve_maxsat) sets it to the
     * maximal priority-respecting kept set. */
    const uint8_t  *soft_keep;
    uint32_t        n_softs;

    /* Diversity flip-check (post-solve). assert_nodes collects every top-level
     * asserted AIG node (hard constraints, kept softs, var bounds) so the
     * flip-check can re-verify them after tentatively flipping a bit. node_val
     * is the AIG node valuation after the flip-check pass (NULL if it did not
     * run, i.e. seed 0); the readback reads variable bits from it. */
    zsp_aig_node_t *assert_nodes;
    uint32_t        n_asserts;
    uint32_t        asserts_cap;
    int8_t         *node_val;
};

/* ----------------------------- helpers ------------------------------------ */

static void *xalloc(zsp_alloc_t *a, size_t sz) {
    return a ? ZSP_ALLOC(a, sz) : malloc(sz);
}
static void xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) ZSP_RELEASE(a, p, sz); else free(p);
}

/* Encode `node` as a hard top-level assertion AND remember it, so the diversity
 * flip-check can re-verify every assertion after a tentative bit flip. */
static void assert_top(zsp_bbsolver_t *S, zsp_aig_node_t node) {
    zsp_aig_cnf_encode(S->cnf, node, /*top_level=*/1);
    if (S->n_asserts == S->asserts_cap) {
        uint32_t nc = S->asserts_cap ? S->asserts_cap * 2 : 64;
        zsp_aig_node_t *na = (zsp_aig_node_t *)xalloc(S->alloc,
                                                      nc * sizeof(zsp_aig_node_t));
        if (!na) return;   /* out of memory: skip recording (flip-check no-ops) */
        if (S->assert_nodes) {
            memcpy(na, S->assert_nodes, S->n_asserts * sizeof(zsp_aig_node_t));
            xfree(S->alloc, S->assert_nodes, S->asserts_cap * sizeof(zsp_aig_node_t));
        }
        S->assert_nodes = na;
        S->asserts_cap = nc;
    }
    S->assert_nodes[S->n_asserts++] = node;
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
    case EXPR_IN_RANGES: {
        ExprInRanges *irs = (ExprInRanges *)kp;
        if (subst_reaches_var(S, irs->value, target_var, visited)) return 1;
        ExprRef *los = expr_in_ranges_los(S->problem, ref);
        ExprRef *his = expr_in_ranges_his(S->problem, ref);
        for (uint32_t i = 0; i < irs->n_ranges; i++)
            if (subst_reaches_var(S, los[i], target_var, visited) ||
                subst_reaches_var(S, his[i], target_var, visited)) return 1;
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
    /* Flatten the BIN_AND spine ITERATIVELY. A deep conjunction of equalities
     * (large graph-colouring / sudoku instances) would otherwise recurse one
     * C-stack frame per conjunct and overflow the stack (was crash B10, subst
     * pre-pass site). Leaves are visited LEFT-TO-RIGHT, exactly as the former
     * recursion, so subst-recording order — hence the cycle-gate outcome in
     * try_record_subst — is byte-for-byte unchanged. Returns 1 iff EVERY
     * sub-assert under root was consumed as a substitution (root can be
     * skipped); every leaf is still visited even after one fails to consume, so
     * all recordable substitutions are recorded (matching the old `l && r`). */
    ExprRef *stk = NULL;
    size_t n = 0, cap = 0;
    int all_consumed = 1;
    #define CS_PUSH(R) do {                                                 \
        if (n == cap) { size_t nc = cap ? cap * 2 : 32;                     \
            ExprRef *t = (ExprRef *)realloc(stk, nc * sizeof(ExprRef));     \
            if (!t) { free(stk); return 0; }                               \
            stk = t; cap = nc; }                                            \
        stk[n++] = (R);                                                     \
    } while (0)
    CS_PUSH(root);
    while (n > 0) {
        ExprRef r = stk[--n];
        if (r == EXPR_NULL) { all_consumed = 0; continue; }
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, r);
        if (!kp) { all_consumed = 0; continue; }
        if (*kp == EXPR_BINARY) {
            ExprBinary *b = (ExprBinary *)kp;
            if (b->op == BIN_AND) {
                CS_PUSH(b->rhs);   /* push rhs first so lhs pops (is visited) first */
                CS_PUSH(b->lhs);
                continue;
            }
            if (b->op == BIN_EQ) {
                if (try_record_subst(S, b->lhs, b->rhs)) continue;  /* consumed */
                if (try_record_subst(S, b->rhs, b->lhs)) continue;  /* consumed */
            }
        }
        all_consumed = 0;   /* this leaf was not consumable as a substitution */
    }
    #undef CS_PUSH
    free(stk);
    return all_consumed;
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
 * width (64 — the engine's int64 value domain — for unconstrained constants).
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
    /* A width-less constant defaults to 64 bits — the engine's int64 value
     * domain — NOT 32: a constant carrying a full 64-bit pattern (e.g. a limb of
     * a wide >64-bit literal, requested by bb_concat with hint=0) must not be
     * truncated to 32 bits. Where a constant has real context (a comparison /
     * arithmetic operand) the caller passes a non-zero hint, and width
     * reconciliation (max_w + zext/sext) sizes it to the other operand anyway,
     * so a wider default never changes a value outcome. */
    uint16_t w = hint ? hint : 64;
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

/* True iff `rf` is an in-range AND/OR EXPR_BINARY node — i.e. one the iterative
 * AND/OR evaluator manages on its explicit stack (and memoizes), as opposed to a
 * leaf operand it blasts directly via bb_predicate. */
static int bb_is_andor_node(zsp_bbsolver_t *S, ExprRef rf) {
    if (rf == EXPR_NULL || rf >= S->cache_cap) return 0;
    ExprKind *kk = (ExprKind *)POOL_PTR(S->problem, rf);
    return kk && *kk == EXPR_BINARY &&
           (((const ExprBinary *)kk)->op == BIN_AND ||
            ((const ExprBinary *)kk)->op == BIN_OR);
}

static zsp_bv_t bb_binary(zsp_bbsolver_t *S, const ExprBinary *b, ExprRef ref,
                          uint16_t hint) {
    /* Comparison / logical ops always produce 1-bit; arithmetic / bitwise
     * widen lhs and rhs to a common width = max(lhs, rhs, hint). */
    switch (b->op) {
    case BIN_AND:
    case BIN_OR: {
        /* Evaluate the AND/OR expression tree ITERATIVELY with the SAME
         * memoization as the recursive path, so the produced AIG is
         * byte-identical (DAG-shared sub-formulas stay shared) while the C stack
         * stays O(1) on deep spines. A deep (and c1 (and c2 (and c3 ...))) chain
         * — thousands of conjuncts, as large graph-colouring / edge-matching /
         * sudoku instances build — otherwise recurses one C-stack frame per
         * conjunct (bb_binary->bb_predicate->bb_expr->bb_binary) and overflows
         * the stack (was crash B10). A NAIVE flatten avoids the crash but
         * dissolves shared sub-conjunctions (they memoize once in the recursive
         * path), changing the CNF and REGRESSING heavy-tail solves (mcm/70
         * 40s->timeout) — hence this structure-preserving version instead.
         *
         * Only AND/OR nodes go on the explicit stack; non-AND/OR operands are
         * blasted via bb_predicate (bounded recursion). Each AND/OR node memoizes
         * into S->cache — the exact slot bb_expr uses — so a shared node is
         * computed once. Falls back to plain recursion when memoization is off
         * (DV_BB_NO_MEMO) or the ref is out of cache range (the only path that
         * can still deep-recurse, both non-default). */
        int memo = (getenv("DV_BB_NO_MEMO") == NULL) && S->cache != NULL;
        if (!memo || ref >= S->cache_cap) {
            zsp_bv_t a = bb_predicate(S, b->lhs);
            zsp_bv_t c = bb_predicate(S, b->rhs);
            if (S->had_error) return a;
            return (b->op == BIN_AND) ? zsp_bb_and(S->bb, a, c)
                                      : zsp_bb_or (S->bb, a, c);
        }
        typedef struct { ExprRef ref; int phase; } bb_fr;
        bb_fr *stk = NULL; size_t n = 0, cap = 0;
        #define BB_PUSH(R,P) do {                                              \
            if (n == cap) { size_t nc = cap ? cap * 2 : 64;                    \
                bb_fr *t = (bb_fr *)realloc(stk, nc * sizeof(bb_fr));          \
                if (!t) { free(stk); return err_bv(S, "oom: and/or eval"); }   \
                stk = t; cap = nc; }                                           \
            stk[n].ref = (R); stk[n].phase = (P); n++;                         \
        } while (0)
        /* operand predicate: cached AND/OR result, else blast the leaf. */
        #define BB_OPND(RF) ( (bb_is_andor_node(S, (RF)) &&                    \
            S->cache[(RF)].bv.size != 0) ? S->cache[(RF)].bv                   \
                                         : bb_predicate(S, (RF)) )
        BB_PUSH(ref, 0);
        while (n > 0) {
            bb_fr it = stk[--n];
            const ExprBinary *nb =
                (const ExprBinary *)POOL_PTR(S->problem, it.ref);
            if (it.phase == 0) {
                if (S->cache[it.ref].bv.size != 0) continue;   /* shared: done */
                BB_PUSH(it.ref, 1);                            /* combine later */
                if (bb_is_andor_node(S, nb->rhs)) BB_PUSH(nb->rhs, 0);
                if (bb_is_andor_node(S, nb->lhs)) BB_PUSH(nb->lhs, 0); /* lhs 1st */
            } else {
                zsp_bv_t a = BB_OPND(nb->lhs);
                if (S->had_error) { free(stk); return a; }
                zsp_bv_t c = BB_OPND(nb->rhs);
                if (S->had_error) { free(stk); return c; }
                S->cache[it.ref].bv = (nb->op == BIN_AND)
                    ? zsp_bb_and(S->bb, a, c) : zsp_bb_or(S->bb, a, c);
            }
        }
        #undef BB_PUSH
        #undef BB_OPND
        free(stk);
        return S->cache[ref].bv;
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

static zsp_bv_t bb_in_ranges(zsp_bbsolver_t *S, ExprRef ref) {
    /* OR over ranges of (value >= lo_i AND value <= hi_i). */
    ExprInRanges *node = (ExprInRanges *)POOL_PTR(S->problem, ref);
    ExprRef *los = expr_in_ranges_los(S->problem, ref);
    ExprRef *his = expr_in_ranges_his(S->problem, ref);
    zsp_bv_t v = bb_expr(S, node->value, 0);
    if (S->had_error) return v;
    int is_signed = 0;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, node->value);
    if (kp && *kp == EXPR_VAR) {
        ExprVar *vv = (ExprVar *)kp;
        if (vv->var_id < S->n_vars) is_signed = S->vars[vv->var_id].is_signed;
    }
    zsp_bv_t acc = { NULL, 0 };
    for (uint32_t i = 0; i < node->n_ranges; i++) {
        zsp_bv_t lo = bb_expr(S, los[i], v.size);
        if (S->had_error) return lo;
        zsp_bv_t hi = bb_expr(S, his[i], v.size);
        if (S->had_error) return hi;
        uint16_t w = max_w(max_w(v.size, lo.size), hi.size);
        zsp_bv_t vw = v, lw = lo, hw = hi;
        if (is_signed) {
            if (vw.size < w) vw = sext_to(S, vw, w);
            if (lw.size < w) lw = sext_to(S, lw, w);
            if (hw.size < w) hw = sext_to(S, hw, w);
        } else {
            if (vw.size < w) vw = zext_to(S, vw, w);
            if (lw.size < w) lw = zext_to(S, lw, w);
            if (hw.size < w) hw = zext_to(S, hw, w);
        }
        zsp_bv_t v_lt_lo = is_signed ? zsp_bb_slt(S->bb, vw, lw) : zsp_bb_ult(S->bb, vw, lw);
        zsp_bv_t hi_lt_v = is_signed ? zsp_bb_slt(S->bb, hw, vw) : zsp_bb_ult(S->bb, hw, vw);
        zsp_bv_t in_i = zsp_bb_and(S->bb, zsp_bb_not(S->bb, v_lt_lo),
                                          zsp_bb_not(S->bb, hi_lt_v));
        acc = (i == 0) ? in_i : zsp_bb_or(S->bb, acc, in_i);
    }
    if (acc.size == 0) acc = zsp_bb_value_u64(S->bb, 1, 0);  /* empty -> false */
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
    case EXPR_BINARY:   out = bb_binary(S, (ExprBinary *)kp, ref, hint_width); break;
    case EXPR_UNARY:    out = bb_unary(S, (ExprUnary *)kp, hint_width); break;
    case EXPR_ITE:      out = bb_ite(S, (ExprITE *)kp, hint_width); break;
    case EXPR_IN_RANGE: out = bb_in_range(S, (ExprInRange *)kp); break;
    case EXPR_IN_SET:   out = bb_in_set(S, ref); break;
    case EXPR_IN_RANGES: out = bb_in_ranges(S, ref); break;
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
    assert_top(S, pred.bits[0]);
    return 0;
}

/* ----------------------------- public API --------------------------------- */

/* Select the SAT backend for a fresh bbsolver. Defaults to KISSAT (the
 * historical one-shot behavior). DV_SAT_BACKEND=cadical|bb forces the
 * incremental CaDiCaL backend (audit/benchmark knob, mirroring DV_ENGINE);
 * =kissat forces kissat. When CaDiCaL is not compiled in, a cadical request
 * transparently falls back to kissat (see zsp_sat_new_backend). This is the
 * single creation site; interaction-shape routing (BMC/incremental → cadical)
 * layers on top of this later. */
/* prefer_cadical: -1 = env-driven (DV_SAT_BACKEND, default kissat), 0 = force
 * kissat, 1 = force CaDiCaL. A forced CaDiCaL request still falls back to
 * kissat transparently when CaDiCaL is not compiled in (see
 * zsp_sat_new_backend); callers that require incrementality re-check via
 * zsp_bbsolver_is_incremental. */
static zsp_sat_t *bb_new_sat(zsp_alloc_t *alloc, int prefer_cadical) {
    if (prefer_cadical < 0) {
        const char *e = getenv("DV_SAT_BACKEND");
        if (e && (strcmp(e, "cadical") == 0 || strcmp(e, "cd") == 0 ||
                  strcmp(e, "bb") == 0)) {
            prefer_cadical = 1;
        } else {
            prefer_cadical = 0;
        }
    }
    return zsp_sat_new_backend(alloc, prefer_cadical ? ZSP_SAT_BACKEND_CADICAL
                                                     : ZSP_SAT_BACKEND_KISSAT);
}

static zsp_bbsolver_t *bbsolver_new_ex(zsp_alloc_t *alloc, SolveProblem *problem,
                                       int prefer_cadical) {
    if (!problem) return NULL;
    zsp_bbsolver_t *S = (zsp_bbsolver_t *)xalloc(alloc, sizeof(*S));
    if (!S) return NULL;
    memset(S, 0, sizeof(*S));
    S->alloc = alloc;
    S->problem = problem;
    S->aig = zsp_aig_new(alloc);
    S->sat = bb_new_sat(alloc, prefer_cadical);
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

zsp_bbsolver_t *zsp_bbsolver_new(zsp_alloc_t *alloc, SolveProblem *problem) {
    return bbsolver_new_ex(alloc, problem, /*prefer_cadical=*/-1);
}

zsp_bbsolver_t *zsp_bbsolver_new_backend(zsp_alloc_t *alloc,
                                         SolveProblem *problem,
                                         int prefer_cadical) {
    return bbsolver_new_ex(alloc, problem, prefer_cadical);
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
    xfree(S->alloc, S->assert_nodes, S->asserts_cap * sizeof(zsp_aig_node_t));
    free(S->node_val);
    xfree(S->alloc, S, sizeof(*S));
}

static uint64_t _free_var_fill(uint64_t seed, uint32_t var_id, uint32_t limb);

/* Value of AIG literal `lit` under node valuation nv[] (nv indexed by |id|). */
static inline int _lit_val(const int8_t *nv, zsp_aig_node_t lit) {
    if (lit == ZSP_AIG_TRUE)  return 1;
    if (lit == ZSP_AIG_FALSE) return 0;
    uint32_t i = (uint32_t)(lit < 0 ? -lit : lit);
    int v = nv[i];
    return lit < 0 ? !v : v;
}

/* Propagate a just-changed node value through its fanout cone. nv[start] has
 * already been set to its new value; recompute every AND node reachable upward
 * (via the fo_off/fo_adj CSR fanout index) with a worklist until the valuation
 * reaches its fixed point. Only nodes in the changed cone are touched -- cost is
 * proportional to the affected region, not the whole AIG. `inq` guards against a
 * node sitting in the worklist twice and is left all-zero on return (every
 * pushed node is popped). Node ids are topological but the worklist may pop out
 * of order; that only causes a few redundant recomputes -- a DAG converges to
 * the same fixed point regardless of visit order. */
static void _prop_cone(int8_t *nv, const zsp_aig_node_t *L, const zsp_aig_node_t *R,
                       const uint32_t *fo_off, const uint32_t *fo_adj,
                       uint32_t *wl, uint8_t *inq, uint32_t start) {
    uint32_t top = 0;
    wl[top++] = start; inq[start] = 1;
    while (top) {
        uint32_t u = wl[--top]; inq[u] = 0;
        for (uint32_t e = fo_off[u]; e < fo_off[u + 1]; e++) {
            uint32_t pn = fo_adj[e];
            int np = _lit_val(nv, L[pn]) & _lit_val(nv, R[pn]);
            if (np != nv[pn]) {
                nv[pn] = (int8_t)np;
                if (!inq[pn]) { inq[pn] = 1; wl[top++] = pn; }
            }
        }
    }
}

/* Diversity flip-check: after a SAT solve, randomize the don't-care variable
 * bits. Each rand bit is set toward a seeded-random target; the flip is kept
 * only if EVERY top-level assertion still holds under the updated model. Flips
 * are applied sequentially, so each is verified against the running model and
 * coupled constraints (e.g. a sum bound) are never violated -- the result stays
 * a valid solution, just a more diverse one. node_val[] holds that model for the
 * readback.
 *
 * Cost per candidate bit is one *incremental* re-evaluation: only the flipped
 * bit's fanout cone is repropagated (built once as the fo_off/fo_adj CSR index),
 * not the entire AIG. For the localized cones typical of constraint bits this is
 * far cheaper than the old full O(nodes) re-simulation. Skipped on very large
 * problems (where BMC-style seed 0 is used anyway). */
#define ZSP_DIVERSIFY_MAX_NODES 200000u
static void _diversify(zsp_bbsolver_t *S) {
    if (S->seed == 0 || !S->aig) return;
    uint32_t N = (uint32_t)zsp_aig_num_nodes(S->aig);
    if (N == 0 || N > ZSP_DIVERSIFY_MAX_NODES) return;

    zsp_aig_t *m = S->aig;
    /* Plain malloc for these transient per-solve buffers: node_val (nv) is freed
     * in zsp_bbsolver_free, the rest here -- all independent of the arena. */
    int8_t *nv        = (int8_t *)malloc((size_t)(N + 1));
    zsp_aig_node_t *L = (zsp_aig_node_t *)malloc((size_t)(N + 1) * sizeof(zsp_aig_node_t));
    zsp_aig_node_t *R = (zsp_aig_node_t *)malloc((size_t)(N + 1) * sizeof(zsp_aig_node_t));
    if (!nv || !L || !R) { free(nv); free(L); free(R); return; }

    /* Cache AND children (0 = input/const) and build the initial valuation in
     * topological (ascending id) order -- children always have a smaller id. */
    nv[0] = 0; L[0] = 0; R[0] = 0;
    for (uint32_t id = 1; id <= N; id++) {
        if (zsp_aig_is_and(m, (zsp_aig_node_t)id)) {
            zsp_aig_get_children(m, (zsp_aig_node_t)id, &L[id], &R[id]);
            nv[id] = (int8_t)(_lit_val(nv, L[id]) & _lit_val(nv, R[id]));
        } else {
            L[id] = 0; R[id] = 0;
            nv[id] = (int8_t)(zsp_aig_cnf_value(S->cnf, (zsp_aig_node_t)id) == 1);
        }
    }

    /* Build the fanout index (CSR): fo_adj[fo_off[c]..fo_off[c+1]) lists the AND
     * nodes that reference node c as a child. Constants (|child| <= 1, i.e.
     * TRUE=id1 / FALSE) get no edge -- their value never changes. */
    uint32_t *cnt    = (uint32_t *)calloc((size_t)N + 2, sizeof(uint32_t));
    uint32_t *fo_off = (uint32_t *)malloc(((size_t)N + 2) * sizeof(uint32_t));
    uint8_t  *inq    = (uint8_t  *)calloc((size_t)N + 1, 1);
    uint32_t *wl     = (uint32_t *)malloc(((size_t)N + 1) * sizeof(uint32_t));
    if (!cnt || !fo_off || !inq || !wl) {
        free(cnt); free(fo_off); free(inq); free(wl);
        free(L); free(R); S->node_val = nv; return;   /* keep the valid (undiversified) model */
    }
    for (uint32_t id = 1; id <= N; id++) {
        if (!L[id]) continue;
        uint32_t cl = (uint32_t)(L[id] < 0 ? -L[id] : L[id]);
        uint32_t cr = (uint32_t)(R[id] < 0 ? -R[id] : R[id]);
        if (cl > 1) cnt[cl]++;
        if (cr > 1) cnt[cr]++;
    }
    uint32_t acc = 0;
    for (uint32_t i = 0; i <= N; i++) { fo_off[i] = acc; acc += cnt[i]; }
    fo_off[N + 1] = acc;
    uint32_t *fo_adj = (uint32_t *)malloc((acc ? (size_t)acc : 1) * sizeof(uint32_t));
    if (!fo_adj) {
        free(cnt); free(fo_off); free(inq); free(wl);
        free(L); free(R); S->node_val = nv; return;
    }
    for (uint32_t i = 0; i <= N; i++) cnt[i] = fo_off[i];   /* reuse cnt as fill cursor */
    for (uint32_t id = 1; id <= N; id++) {
        if (!L[id]) continue;
        uint32_t cl = (uint32_t)(L[id] < 0 ? -L[id] : L[id]);
        uint32_t cr = (uint32_t)(R[id] < 0 ? -R[id] : R[id]);
        if (cl > 1) fo_adj[cnt[cl]++] = id;
        if (cr > 1) fo_adj[cnt[cr]++] = id;
    }

    for (uint32_t vi = 0; vi < S->n_vars; vi++) {
        bb_var_t *v = &S->vars[vi];
        if (!v->defined || !v->bv_built) continue;
        for (uint32_t p = 0; p < v->bv.size; p++) {
            zsp_aig_node_t lit = v->bv.bits[p];
            uint32_t base = (uint32_t)(lit < 0 ? -lit : lit);
            if (base == 0 || base > N) continue;
            if (L[base] != 0) continue;                 /* not a primary input */
            int tgt = (int)(_free_var_fill(S->seed, vi, p) & 1);
            int desired = (lit < 0) ? !tgt : tgt;       /* input-node polarity */
            if (nv[base] == desired) continue;
            int old = nv[base];
            nv[base] = (int8_t)desired;
            _prop_cone(nv, L, R, fo_off, fo_adj, wl, inq, base);
            int ok = 1;
            for (uint32_t a = 0; a < S->n_asserts; a++)
                if (_lit_val(nv, S->assert_nodes[a]) != 1) { ok = 0; break; }
            if (!ok) {                                  /* revert: repropagate old value */
                nv[base] = (int8_t)old;
                _prop_cone(nv, L, R, fo_off, fo_adj, wl, inq, base);
            }
        }
    }

    free(cnt);
    free(fo_off);
    free(fo_adj);
    free(inq);
    free(wl);
    free(L);
    free(R);
    S->node_val = nv;
}

/* Bit-blast + CNF-encode the whole problem (constraints, kept softs, subst
 * force-builds, variable bounds) WITHOUT solving. Shared by the one-shot
 * zsp_bbsolver_check and the cube engine's zsp_bbsolver_prepare, which then
 * drives its own assumption-based solve loop over the same encoded instance.
 * Returns ZSP_BB_ENCODE_READY when the instance is fully encoded and ready to
 * solve, or ZSP_BB_UNKNOWN / ZSP_BB_ERROR (with S->last_result set) when a
 * construct is unsupported or a hard error occurred. NOTE: the "ready" sentinel
 * is deliberately NOT 0 — ZSP_BB_UNKNOWN is 0, so a 0-means-ready contract would
 * make a deferral look like success and drop the unsupported hard constraint. */
static int _bb_encode(zsp_bbsolver_t *S) {
    /* A-4 soundness guard: the bit-blaster does not encode AllDifferent or
     * Source groups. pyvsc never emits these on the dv-solve path (unique
     * lowers to NEQ pairs; no sources), but if one ever appears we must NOT
     * silently drop a hard constraint — defer (UNKNOWN) instead. */
    if (S->problem->allDiff_head != EXPR_NULL ||
        S->problem->sources_head != EXPR_NULL) {
        S->last_result = ZSP_BB_UNKNOWN;
        return ZSP_BB_UNKNOWN;
    }

    int subst_enabled = getenv("DV_BB_NO_SUBST") == NULL;

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
        assert_top(S, pred.bits[0]);
        cur = cs->next;
    }

    /* DSE-2: encode the kept soft constraints as hard top-level assertions.
     * soft_keep[i] selects which softs (in softs_head walk order) to enforce;
     * the MaxSAT wrapper has already chosen the maximal priority-respecting set,
     * so here they are simply additional hard assertions. NULL → honor none. */
    if (S->soft_keep) {
        uint32_t si = 0;
        for (ExprRef scur = S->problem->softs_head; scur != EXPR_NULL; si++) {
            SoftSpec *ss = (SoftSpec *)POOL_PTR(S->problem, scur);
            if (S->soft_keep[si]) {
                zsp_bv_t pred = bb_predicate(S, ss->root);
                if (S->had_error) { S->last_result = ZSP_BB_ERROR; return ZSP_BB_ERROR; }
                if (S->had_unsupported) { S->last_result = ZSP_BB_UNKNOWN; return ZSP_BB_UNKNOWN; }
                assert_top(S, pred.bits[0]);
            }
            scur = ss->next;
        }
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
        if (S->vars[i].defined && S->vars[i].bv_built && !S->vars[i].bounds_asserted) {
            assert_var_bounds(S, i);
            S->vars[i].bounds_asserted = 1;
        }
    }

    return ZSP_BB_ENCODE_READY;
}

/* ------------------------------------------------------------------------- *
 * Diversified SAT portfolio (B1).
 *
 * The per-solve cost on hard single QF_BV instances is a SAT-search HEAVY TAIL,
 * not an encoding defect: identical-size CNFs solve 30x apart, and the runtime
 * tails of different backends are ANTI-CORRELATED (kissat wins some, CaDiCaL
 * others). So the lever is min-of-N over a diverse fleet: replay the recorded
 * clause DB into N independent solver instances — spread across backends first
 * (the strongest diversity axis), then seeds — run them concurrently on the
 * FULL instance, and take the first verdict. A full-instance solve carries no
 * assumptions, so ANY worker's SAT/UNSAT is authoritative for the whole problem;
 * the winner's model installs exactly like a cube worker's. Losers are aborted
 * mid-solve via the terminate hook the instant a peer finishes.
 *
 * Opt-in via DV_PORTFOLIO=N (N>=2). Unset/0/1 leaves the single-solve path (and
 * thus every existing caller) byte-for-byte unchanged.
 * ------------------------------------------------------------------------- */

/* Golden-ratio odd constant: spreads per-worker seeds so their low bits (hence
 * kissat/CaDiCaL initial phases) differ. */
#define BB_PORT_GOLDEN 0x9E3779B97F4A7C15ull

static uint32_t bb_portfolio_workers(void) {
    const char *e = getenv("DV_PORTFOLIO");
    if (!e || !*e) return 0;
    long n = atol(e);
    if (n < 2) return 0;         /* 0/1 => portfolio off (single solve) */
    if (n > 256) n = 256;        /* hard cap on the fleet */
    return (uint32_t)n;
}

typedef struct bb_port_arg_s {
    /* shared, read-only after spawn */
    const int32_t     *db;
    size_t             db_n;
    int32_t            max_var;
    zsp_sat_backend_t  backend;
    uint64_t           seed;
    int                verbose;
    uint32_t           idx;
    /* peer roster so the winner can interrupt the losers (see bb_port_worker) */
    struct bb_port_arg_s *all;
    uint32_t           n_all;
    /* shared, mutable (guarded by *mtx except *stop which is poll-only) */
    zsp_mutex_t       *mtx;
    volatile int      *stop;     /* 1 once any worker has a verdict            */
    int               *result;   /* ZSP_BB_SAT/UNSAT written by the winner     */
    zsp_sat_t        **winner;   /* winning instance (model source), or NULL   */
    /* out: this worker's instance (freed by the driver after model install).
     * Published under *mtx so a winning peer sees it before interrupting.      */
    zsp_sat_t         *sat;
} bb_port_arg;

/* Terminate callback: the losing workers poll the shared stop flag and abort.
 * A benign lock-free read — the flag only ever transitions 0->1, and a slightly
 * stale read just means a worker grinds a few more conflicts before quitting. */
static int bb_port_terminate(void *state) {
    return *(volatile int *)state;
}

static void *bb_port_worker(void *arg) {
    bb_port_arg *w = (bb_port_arg *)arg;

    zsp_sat_t *sat = zsp_sat_new_backend(NULL, w->backend);
    if (!sat) return NULL;   /* w->sat stays NULL (calloc'd); peers skip us */
    /* Seed BEFORE any clause is added (CaDiCaL only honors seed/phase at init). */
    if (w->seed) zsp_sat_set_seed(sat, w->seed);
    zsp_sat_reserve(sat, (zsp_sat_var_t)w->max_var);
    for (size_t i = 0; i < w->db_n; i++)
        zsp_sat_add(sat, (zsp_sat_lit_t)w->db[i]);
    /* CaDiCaL polls this and aborts on the shared stop flag; kissat stores but
     * ignores it (interrupted instead via zsp_sat_interrupt below). */
    zsp_sat_set_terminate(sat, (void *)w->stop, bb_port_terminate);

    /* Publish our instance so a winning peer can interrupt us, and bail if a
     * peer already won while we were building. */
    zsp_mutex_lock(w->mtx);
    w->sat = sat;
    int already = *w->stop;
    zsp_mutex_unlock(w->mtx);
    if (already) return NULL;

    /* Full-instance solve (no assumptions, no conflict limit). */
    int rc = zsp_sat_solve(sat);

    zsp_mutex_lock(w->mtx);
    if (!*w->stop && (rc == ZSP_BB_SAT || rc == ZSP_BB_UNSAT)) {
        *w->result = rc;
        if (rc == ZSP_BB_SAT) *w->winner = sat;
        *w->stop = 1;
        /* Stop the losers now: CaDiCaL peers see the flag via their callback,
         * kissat peers need the explicit poke. Safe under the mutex — every peer
         * that could be mid-solve has already published its instance. */
        for (uint32_t j = 0; j < w->n_all; j++)
            if (j != w->idx && w->all[j].sat)
                zsp_sat_interrupt(w->all[j].sat);
        if (w->verbose)
            fprintf(stderr, "[bb-port] worker %u (%s seed=%llu) won: %s\n",
                    w->idx,
                    w->backend == ZSP_SAT_BACKEND_CADICAL ? "cadical" : "kissat",
                    (unsigned long long)w->seed,
                    rc == ZSP_BB_SAT ? "sat" : "unsat");
    }
    zsp_mutex_unlock(w->mtx);
    return NULL;   /* keep `sat` alive; the driver reads the winner then frees. */
}

/* Assign (backend, seed) to worker `i`. Backend is the primary diversity axis:
 * worker 0 = kissat, worker 1 = CaDiCaL (the proven anti-correlated pair, both
 * at the caller's base seed = the field-measured fast configs). Extra workers
 * alternate backends and take a spread seed. When CaDiCaL is absent everything
 * runs on kissat, so seed diversity carries all the load (worker 0 keeps the
 * base seed; the rest are spread). */
static void bb_port_assign(uint32_t i, uint64_t base_seed, int have_cd,
                           zsp_sat_backend_t *bk, uint64_t *seed) {
    /* DV_PORT_BACKENDS overrides the backend mix (E2.3 calibration knob):
     *   "cadical" -> all workers CaDiCaL (seed diversity only); every worker gets
     *               a spread seed since there is no backend axis to diversify on.
     *   "kissat"  -> all workers kissat.
     *   unset/"mixed" -> the default anti-correlated kissat/CaDiCaL alternation. */
    const char *mix = getenv("DV_PORT_BACKENDS");
    if (mix && have_cd && strcmp(mix, "cadical") == 0) {
        *bk = ZSP_SAT_BACKEND_CADICAL;
        *seed = (i == 0) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
        return;
    }
    if (mix && strcmp(mix, "kissat") == 0) {
        *bk = ZSP_SAT_BACKEND_KISSAT;
        *seed = (i == 0) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
        return;
    }
    if (have_cd) {
        *bk = (i & 1u) ? ZSP_SAT_BACKEND_CADICAL : ZSP_SAT_BACKEND_KISSAT;
        *seed = (i < 2) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
    } else {
        *bk = ZSP_SAT_BACKEND_KISSAT;
        *seed = (i == 0) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
    }
}

/* Run an n-way diversified portfolio over the already-encoded instance. Returns
 * a ZSP_BB_* verdict, or sets *ran=0 (leaving the caller to fall back to the
 * single solve) if the DB could not be captured or no worker could start. */
static int bb_run_portfolio(zsp_bbsolver_t *S, uint32_t n_port, uint64_t seed,
                            int verbose, int *ran) {
    *ran = 0;

    size_t db_n = 0;
    int32_t max_var = 0;
    const int32_t *db = zsp_bbsolver_clause_db(S, &db_n, &max_var);
    if (!db || db_n == 0) return ZSP_BB_UNKNOWN;   /* recording off/failed */

    zsp_mutex_t mtx;
    if (zsp_mutex_init(&mtx) != 0) return ZSP_BB_UNKNOWN;

    volatile int stop = 0;
    int result = ZSP_BB_UNKNOWN;
    zsp_sat_t *winner = NULL;
    int have_cd = zsp_sat_has_cadical();

    bb_port_arg  *wa = (bb_port_arg *)calloc(n_port, sizeof(*wa));
    zsp_thread_t *th = (zsp_thread_t *)calloc(n_port, sizeof(*th));
    if (!wa || !th) {
        free(wa); free(th); zsp_mutex_destroy(&mtx);
        return ZSP_BB_UNKNOWN;
    }

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < n_port; i++) {
        wa[i].db = db; wa[i].db_n = db_n; wa[i].max_var = max_var;
        bb_port_assign(i, seed, have_cd, &wa[i].backend, &wa[i].seed);
        wa[i].verbose = verbose; wa[i].idx = i;
        wa[i].all = wa; wa[i].n_all = n_port;
        wa[i].mtx = &mtx; wa[i].stop = &stop;
        wa[i].result = &result; wa[i].winner = &winner;
        if (zsp_thread_create(&th[i], bb_port_worker, &wa[i]) == 0) spawned++;
        else break;
    }

    if (spawned == 0) {
        free(wa); free(th); zsp_mutex_destroy(&mtx);
        return ZSP_BB_UNKNOWN;   /* nothing started: caller falls back */
    }
    if (verbose)
        fprintf(stderr, "[bb-port] %u workers (cadical=%s), %zu db lits\n",
                spawned, have_cd ? "yes" : "no", db_n);

    for (uint32_t i = 0; i < spawned; i++)
        zsp_thread_join(&th[i], NULL);

    /* Materialize the winner's model into S before any instance is freed. */
    if (result == ZSP_BB_SAT && winner) {
        if (zsp_bbsolver_install_worker_model(S, winner) != 0)
            result = ZSP_BB_UNKNOWN;   /* couldn't build the model: stays sound */
    }

    for (uint32_t i = 0; i < spawned; i++)
        if (wa[i].sat) zsp_sat_free(wa[i].sat);
    free(wa); free(th); zsp_mutex_destroy(&mtx);

    *ran = 1;
    return result;
}

int zsp_bbsolver_check(zsp_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->problem) return ZSP_BB_ERROR;

    S->seed = seed;
    /* Seed kissat's randomness so repeated checks can return different models
     * (the completeness-fallback's only source of stimulus diversity). */
    zsp_sat_set_seed(S->sat, seed);

    /* Diversified portfolio (opt-in, DV_PORTFOLIO>=2). Enable clause recording
     * BEFORE encoding so the exact literal stream can be replayed into the
     * worker instances; the recorder is a no-op otherwise. */
    uint32_t n_port = bb_portfolio_workers();
    if (n_port >= 2) zsp_bbsolver_record_clauses(S);

    int stats_enabled = getenv("DV_BB_STATS") != NULL;
    struct timespec t0, t1, t2;
    if (stats_enabled) clock_gettime(CLOCK_MONOTONIC, &t0);

    int enc = _bb_encode(S);
    if (enc != ZSP_BB_ENCODE_READY) return enc;   /* UNKNOWN/ERROR: S->last_result already set */

    if (stats_enabled) clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Phase-0 instrumentation: emit the CNF size BEFORE solving, so it is
     * observable even when the solve runs long or never returns (the plain
     * bitblast path is otherwise uncapped — see DV_BB_MAX_CONFLICTS below). */
    if (stats_enabled) {
        double enc_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                      + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr,
                "[bb-encode] bb=%.2fms ands=%llu vars=%llu clauses=%llu substs=%llu\n",
                enc_ms,
                (unsigned long long)zsp_aig_num_ands(S->aig),
                (unsigned long long)zsp_aig_cnf_num_vars(S->cnf),
                (unsigned long long)zsp_aig_cnf_num_clauses(S->cnf),
                (unsigned long long)S->n_substs);
        fflush(stderr);
    }

    /* Diversified portfolio: race N backend/seed-diverse solvers on the full
     * instance, first verdict wins. On success the winner's model is already
     * installed in S; return straight away. On a setup miss (ran==0) fall
     * through to the single solve so the verdict is never worse than bitblast. */
    if (n_port >= 2) {
        int ran = 0;
        int prc = bb_run_portfolio(S, n_port, seed, stats_enabled, &ran);
        if (ran) {
            S->last_result = prc;
            return prc;
        }
    }

    /* Optional conflict cap on the bitblast solve (unset/0 = unlimited, the
     * default so normal runs are unaffected). The CDCL engine's DV_MAX_CONFLICTS
     * does NOT reach this path, so bounding a bitblast experiment needs its own
     * knob. On hit, the backend returns ZSP_BB_UNKNOWN (sound: never a verdict). */
    {
        const char *mc = getenv("DV_BB_MAX_CONFLICTS");
        if (mc && *mc) zsp_sat_set_conflict_limit(S->sat, (uint32_t)atoi(mc));
    }

    /* Size-gated light search: kissat's failed-literal probing is pure overhead
     * on almost everything the one-shot bitblast path sees — the array/BMC class
     * (refuted by plain CDCL almost immediately) AND the hard QF_BV UNSAT band.
     * Measured (docs/perf_sweep_medium_band_2026-07-21.md): probe-OFF wins on
     * deep array-BMC (2-3x), vlsat3_a57 @181k cl (1.46x) and vlsat3_a80 @1.47M cl
     * (3.8x); it only *loses* on the single 11.6M-clause monster vlsat3_a67
     * (~1.45x), which genuinely needs the deep implication chains probing finds.
     * So the gate is a HIGH clause threshold (2M): everything below goes light,
     * only the >>1M-clause hardest instances keep probing. Toggling probe never
     * changes sat/unsat — this is a pure schedule/latency lever. */
    {
        const char *mcl = getenv("DV_KISSAT_LIGHT_MAXCLAUSES");
        uint64_t thresh = mcl && *mcl ? strtoull(mcl, NULL, 10) : 2000000;
        uint64_t ncl = zsp_aig_cnf_num_clauses(S->cnf);
        zsp_sat_set_light_search(S->sat, ncl < thresh);
    }

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
    /* On SAT, diversify the don't-care bits toward seeded-random targets (no-op
     * for seed 0). Sound: only flips that keep every assertion true are kept. */
    if (rc == ZSP_BB_SAT) _diversify(S);
    return rc;
}

/* ---- cube-and-conquer support (see zsp_cube.c, docs/cube_and_conquer_design.md) ---- */

int zsp_bbsolver_prepare(zsp_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->problem) return ZSP_BB_ERROR;
    S->seed = seed;
    zsp_sat_set_seed(S->sat, seed);
    return _bb_encode(S);   /* ZSP_BB_ENCODE_READY = ready, else ZSP_BB_UNKNOWN/ERROR */
}

int zsp_bbsolver_is_incremental(const zsp_bbsolver_t *S) {
    return (S && S->sat) ? zsp_sat_is_incremental(S->sat) : 0;
}

uint32_t zsp_bbsolver_split_lits(zsp_bbsolver_t *S, int32_t *out, uint32_t cap) {
    if (!S || !out || cap == 0) return 0;
    uint32_t n = 0;
    /* Candidate split literals are the SAT variables backing bits of
     * referenced (bv_built) problem variables. A var's bit is a genuine AIG
     * input node (id == SAT var id, 1:1); constant bits are not splittable.
     * The caller forms the two exhaustive cubes {+v} and {-v} per literal. */
    for (uint32_t i = 0; i < S->n_vars && n < cap; i++) {
        bb_var_t *v = &S->vars[i];
        if (!v->defined || !v->bv_built) continue;
        for (uint32_t b = 0; b < v->bv.size && n < cap; b++) {
            zsp_aig_node_t node = v->bv.bits[b];
            if (node == ZSP_AIG_TRUE || node == ZSP_AIG_FALSE) continue;
            if (!zsp_aig_is_input(S->aig, node)) continue;
            int32_t var = node < 0 ? -node : node;
            out[n++] = var;   /* positive literal; assumes bit = 1 */
        }
    }
    return n;
}

/* Flatten a BIN_OR chain rooted at `ref` into out[] (up to `cap`), counting
 * ALL leaves even past `cap` so the caller can detect an over-wide disjunction
 * that it cannot represent exhaustively. `n` is the running count. */
static uint32_t bb_flatten_or(zsp_bbsolver_t *S, ExprRef ref,
                              ExprRef *out, uint32_t cap, uint32_t n) {
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (kp && *kp == EXPR_BINARY && ((ExprBinary *)kp)->op == BIN_OR) {
        ExprBinary *b = (ExprBinary *)kp;
        n = bb_flatten_or(S, b->lhs, out, cap, n);
        n = bb_flatten_or(S, b->rhs, out, cap, n);
        return n;
    }
    if (out && n < cap) out[n] = ref;
    return n + 1;
}

uint32_t zsp_bbsolver_or_split_lits(zsp_bbsolver_t *S, int32_t *out, uint32_t cap) {
    if (!S || !out || cap == 0) return 0;

    /* Pass 1: find the surviving (non-substituted) top-level constraint that is
     * the WIDEST disjunction. Its `or` is asserted true, so splitting on which
     * disjunct holds is an exhaustive k-way cover — the most SAT-directed cut. */
    ExprRef best = EXPR_NULL;
    uint32_t best_k = 0, idx = 0;
    for (ExprRef cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        ExprRef root = cs->root;
        cur = cs->next;
        if (S->constraint_skip && S->constraint_skip[idx]) continue;
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, root);
        if (!kp || *kp != EXPR_BINARY || ((ExprBinary *)kp)->op != BIN_OR) continue;
        uint32_t k = bb_flatten_or(S, root, NULL, 0, 0);   /* count only */
        if (k > best_k) { best_k = k; best = root; }
    }
    /* Reject a disjunction wider than the caller's buffer: a partial split
     * would be non-exhaustive and break the UNSAT soundness contract. */
    if (best == EXPR_NULL || best_k < 2 || best_k > cap) return 0;

    /* Pass 2: materialize one assumption literal per disjunct. Each disjunct is
     * already in the AIG (the parent `or` was bit-blasted at prepare), so
     * bb_predicate hits the memo cache; encoding non-top-level guarantees the
     * disjunct node has a SAT variable to assume on. */
    ExprRef *ds = (ExprRef *)xalloc(S->alloc, best_k * sizeof(ExprRef));
    if (!ds) return 0;
    bb_flatten_or(S, best, ds, best_k, 0);
    uint32_t n = 0;
    for (uint32_t i = 0; i < best_k; i++) {
        zsp_bv_t p = bb_predicate(S, ds[i]);
        if (S->had_error || S->had_unsupported || p.size == 0) { n = 0; break; }
        zsp_aig_node_t node = p.bits[0];
        zsp_aig_cnf_encode(S->cnf, node, /*top_level=*/0);
        out[n++] = (int32_t)node;   /* literal true ⟺ disjunct i holds */
    }
    xfree(S->alloc, ds, best_k * sizeof(ExprRef));
    return n;
}

uint32_t zsp_bbsolver_or_groups(zsp_bbsolver_t *S,
                                int32_t *lits, uint32_t lits_cap,
                                uint32_t *sizes, uint32_t max_groups,
                                uint32_t per_group_cap) {
    if (!S || !lits || !sizes || lits_cap == 0 || max_groups == 0) return 0;

    /* Enumerate EVERY surviving top-level `(or ...)` constraint as a group of
     * disjunct assumption literals (same per-disjunct encoding as
     * zsp_bbsolver_or_split_lits, but for all ors, not just the widest). Because
     * each such `or` is asserted true, every group is individually exhaustive;
     * the CARTESIAN PRODUCT across groups (built by the cube driver) therefore
     * remains an exhaustive partition of the search space — all-cubes-UNSAT
     * still soundly implies UNSAT. We only encode groups whose arity fits
     * [2, per_group_cap] and that fit the flat `lits` / `max_groups` budgets;
     * skipped groups just aren't folded into the product (still sound: fewer
     * splits, never a lost model). Groups are returned in constraint order; the
     * driver sorts and selects which to fold under its cube-count cap. */
    uint32_t ngroups = 0, nlits = 0, idx = 0;
    for (ExprRef cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        ExprRef root = cs->root;
        cur = cs->next;
        if (S->constraint_skip && S->constraint_skip[idx]) continue;
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, root);
        if (!kp || *kp != EXPR_BINARY || ((ExprBinary *)kp)->op != BIN_OR) continue;
        uint32_t k = bb_flatten_or(S, root, NULL, 0, 0);   /* count only */
        if (k < 2 || k > per_group_cap) continue;          /* too small / too wide */
        if (nlits > lits_cap - k) continue;                /* wouldn't fit flat buf */

        ExprRef *ds = (ExprRef *)xalloc(S->alloc, k * sizeof(ExprRef));
        if (!ds) break;
        bb_flatten_or(S, root, ds, k, 0);
        uint32_t got = 0;
        for (uint32_t i = 0; i < k; i++) {
            zsp_bv_t p = bb_predicate(S, ds[i]);
            if (S->had_error || S->had_unsupported || p.size == 0) { got = 0; break; }
            zsp_aig_node_t node = p.bits[0];
            zsp_aig_cnf_encode(S->cnf, node, /*top_level=*/0);
            lits[nlits + i] = (int32_t)node;   /* literal true ⟺ disjunct i holds */
            got++;
        }
        xfree(S->alloc, ds, k * sizeof(ExprRef));
        if (got != k) continue;                /* encoding failed: skip this group */

        sizes[ngroups++] = k;
        nlits += k;
        if (ngroups >= max_groups) break;
    }
    return ngroups;
}

int zsp_bbsolver_solve_assuming(zsp_bbsolver_t *S, const int32_t *lits,
                                uint32_t n, uint32_t conflict_limit) {
    if (!S || !S->sat) return ZSP_BB_ERROR;
    /* Per-solve assumptions are retracted after each solve on the incremental
     * backend, so each call solves original ∧ cube in isolation. On a
     * non-incremental backend only n==0 (a single plain solve) is valid; the
     * cube driver guards this via zsp_bbsolver_is_incremental. */
    zsp_sat_set_conflict_limit(S->sat, conflict_limit);
    for (uint32_t i = 0; i < n; i++)
        zsp_sat_assume(S->sat, (zsp_sat_lit_t)lits[i]);
    int rc = zsp_sat_solve(S->sat);
    S->last_result = rc;
    if (rc == ZSP_BB_SAT) {
        /* Fresh model for this cube: drop any stale flip-check valuation and
         * re-diversify so model read-back reflects THIS solve's assignment. */
        free(S->node_val);
        S->node_val = NULL;
        _diversify(S);
    }
    return rc;
}

/* --- Parallel cube-and-conquer support (P2; see zsp_bbsolver.h, zsp_cube.c). */

void zsp_bbsolver_record_clauses(zsp_bbsolver_t *S) {
    if (S && S->sat) zsp_sat_record_start(S->sat);
}

const int32_t *zsp_bbsolver_clause_db(zsp_bbsolver_t *S, size_t *n_lits,
                                      int32_t *max_var) {
    if (n_lits) *n_lits = 0;
    if (max_var) *max_var = 0;
    if (!S || !S->sat) return NULL;
    if (max_var) *max_var = (int32_t)zsp_sat_max_var(S->sat);
    return zsp_sat_recorded(S->sat, n_lits);
}

int zsp_bbsolver_install_worker_model(zsp_bbsolver_t *S, zsp_sat_t *wsat) {
    if (!S || !wsat || !S->aig) return -1;
    uint32_t N = (uint32_t)zsp_aig_num_nodes(S->aig);
    /* node_val is indexed by AIG node id (0..N); readback prefers it over the
     * live SAT instance, so installing it here makes THIS solver report the
     * worker's model without ever solving on its own instance. AIG node id ==
     * SAT var id (identity), so each node's truth is read straight from the
     * worker's assignment; the TRUE constant (id 1) is forced, and free/undriven
     * nodes take whatever the worker assigned (a sound don't-care — they lie
     * outside every asserted cone). */
    int8_t *nv = (int8_t *)malloc((size_t)(N + 1));
    if (!nv) return -1;
    nv[0] = 0;
    if (N >= 1) nv[1] = 1;                 /* ZSP_AIG_TRUE */
    for (uint32_t id = 2; id <= N; id++)
        nv[id] = (int8_t)(zsp_sat_value(wsat, (zsp_sat_var_t)id) > 0 ? 1 : 0);
    free(S->node_val);
    S->node_val = nv;
    S->last_result = ZSP_BB_SAT;
    return 0;
}

int zsp_bbsolver_rediversify(zsp_bbsolver_t *S, uint64_t seed) {
    if (!S || S->last_result != ZSP_BB_SAT) return -1;
    /* Drop the prior flip-check result and re-run the diversity pass over the
     * still-valid SAT model with the new seed. _diversify re-reads the base
     * model via zsp_aig_cnf_value() (kissat's assignment persists until
     * zsp_bbsolver_free) and only depends on S->seed for the flip targets, so a
     * new seed yields a different sound model with no re-solve. */
    free(S->node_val);
    S->node_val = NULL;
    S->seed = seed;
    _diversify(S);
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Incremental extension (Phase 5a/5b foundation).
 *
 * Keep ONE live bbsolver instance across successive solves: add constraints and
 * re-solve without tearing down the AIG/SAT/CNF. On the CaDiCaL backend this
 * retains learned clauses across solves (true incrementality); on kissat it
 * still re-solves the accumulated DB correctly, just without cross-solve
 * learning. Monotonic add ONLY — there is no scoped retraction here. Scoped
 * push/pop over incrementally added clauses needs selector variables minted in
 * the AIG id space (AIG node id == SAT var id), which is deliberately out of
 * this layer; see docs/phase5_incremental_bitblast_scope.md (Gap B, sub-phase
 * 5c, held). The frontend delta plumbing that would drive these is 5d (held).
 * ------------------------------------------------------------------------- */

/* Bit-blast one additional predicate (an ExprRef into the live problem's pool)
 * and assert it as a hard top-level constraint on the running instance,
 * building and bounding any newly referenced variables. Does not solve — call
 * zsp_bbsolver_resolve afterward. The predicate is bit-blasted directly (no
 * substitution pass), so it composes with whatever a prior check already
 * asserted. Returns 0 on success, ZSP_BB_UNKNOWN for an unsupported construct,
 * ZSP_BB_ERROR on a hard error. */
/* Grow S->vars/subst/resolving to cover variables added to S->problem after
 * construction (e.g. read vars the lazy array loop mints between solves). Safe
 * to call before any assert; a no-op when no new vars appeared. bv_for_var and
 * assert's bounds loop both index by var_id < S->n_vars, so this keeps them in
 * range. The memo cache bounds-checks out-of-range refs itself, so it needs no
 * growth (new lemma nodes are simply re-blasted without memoization). */
static void bb_grow_vars(zsp_bbsolver_t *S) {
    uint32_t max_id = 0; int any = 0;
    for (ExprRef cur = S->problem->vars_head; cur != EXPR_NULL; ) {
        VarSpec *vs = (VarSpec *)POOL_PTR(S->problem, cur);
        if (!any || vs->var_id > max_id) { max_id = vs->var_id; any = 1; }
        cur = vs->next;
    }
    uint32_t need = any ? max_id + 1 : 0;
    if (need <= S->n_vars) return;
    uint32_t old = S->n_vars;
    bb_var_t *nv = (bb_var_t *)xalloc(S->alloc, need * sizeof(bb_var_t));
    if (!nv) return;   /* OOM: leave as-is; assert's var_id<n_vars guard holds */
    memset(nv, 0, need * sizeof(bb_var_t));
    if (S->vars) {
        memcpy(nv, S->vars, old * sizeof(bb_var_t));
        xfree(S->alloc, S->vars, old * sizeof(bb_var_t));
    }
    S->vars = nv;
    if (S->subst) {
        ExprRef *ns = (ExprRef *)xalloc(S->alloc, need * sizeof(ExprRef));
        if (ns) {
            memcpy(ns, S->subst, old * sizeof(ExprRef));
            for (uint32_t i = old; i < need; i++) ns[i] = EXPR_NULL;
            xfree(S->alloc, S->subst, old * sizeof(ExprRef));
            S->subst = ns;
        }
    }
    if (S->resolving) {
        uint8_t *nr = (uint8_t *)xalloc(S->alloc, need * sizeof(uint8_t));
        if (nr) {
            memcpy(nr, S->resolving, old * sizeof(uint8_t));
            memset(nr + old, 0, need - old);
            xfree(S->alloc, S->resolving, old * sizeof(uint8_t));
            S->resolving = nr;
        }
    }
    S->n_vars = need;
    for (ExprRef cur = S->problem->vars_head; cur != EXPR_NULL; ) {
        VarSpec *vs = (VarSpec *)POOL_PTR(S->problem, cur);
        if (vs->var_id >= old && vs->var_id < need) {
            bb_var_t *v = &S->vars[vs->var_id];
            v->width = vs->width; v->is_signed = vs->is_signed;
            v->defined = 1; v->lo = vs->lo; v->hi = vs->hi;
        }
        cur = vs->next;
    }
}

int zsp_bbsolver_assert(zsp_bbsolver_t *S, ExprRef pred_ref) {
    if (!S || !S->problem || pred_ref == EXPR_NULL) return ZSP_BB_ERROR;
    /* Adding clauses after a solve is only legal on an incremental backend;
     * kissat aborts on add-after-solve. Non-incremental callers must free +
     * rebuild instead (the current frontend behavior). */
    if (!zsp_sat_is_incremental(S->sat)) return ZSP_BB_ERROR;
    /* Cover any vars minted into the problem since construction (lazy arrays). */
    bb_grow_vars(S);

    /* bv_for_var uses S->resolving as its recursion guard; clear it (the base
     * check left it as run_subst_pass scratch). */
    if (S->resolving) memset(S->resolving, 0, S->n_vars);

    zsp_bv_t pred = bb_predicate(S, pred_ref);
    if (S->had_error)       { S->last_result = ZSP_BB_ERROR;   return ZSP_BB_ERROR; }
    if (S->had_unsupported) { S->last_result = ZSP_BB_UNKNOWN; return ZSP_BB_UNKNOWN; }
    assert_top(S, pred.bits[0]);

    /* Bound any variable this predicate caused to be built for the first time.
     * bounds_asserted keeps this idempotent across calls and vs. the base
     * check's bounds pass. */
    for (uint32_t i = 0; i < S->n_vars; i++) {
        if (S->vars[i].defined && S->vars[i].bv_built && !S->vars[i].bounds_asserted) {
            assert_var_bounds(S, i);
            S->vars[i].bounds_asserted = 1;
        }
    }
    if (S->had_unsupported) { S->last_result = ZSP_BB_UNKNOWN; return ZSP_BB_UNKNOWN; }
    if (S->had_error)       { S->last_result = ZSP_BB_ERROR;   return ZSP_BB_ERROR; }
    return 0;
}

/* Re-solve the live instance, reusing the accumulated clause DB (and, on
 * CaDiCaL, the retained learned clauses). Mirrors the solve tail of
 * zsp_bbsolver_check. Returns ZSP_BB_SAT/UNSAT/UNKNOWN/ERROR. */
int zsp_bbsolver_resolve(zsp_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->problem) return ZSP_BB_ERROR;
    /* Re-solving a live instance requires an incremental backend; kissat is
     * one-shot per lifetime. */
    if (!zsp_sat_is_incremental(S->sat)) return ZSP_BB_ERROR;
    S->seed = seed;
    zsp_sat_set_seed(S->sat, seed);
    /* Drop the prior solve's flip-check model; the next readback re-derives. */
    free(S->node_val);
    S->node_val = NULL;
    int rc = zsp_sat_solve(S->sat);
    S->last_result = rc;
    if (rc == ZSP_BB_SAT) _diversify(S);
    return rc;
}

/* Re-solve WITHOUT the don't-care diversification, reading back the solver's
 * raw assignment (seed 0 => zsp_bbsolver_value returns the actual SAT model, not
 * a seeded-random fill of free bits). The lazy array refinement loop must see the
 * true model: a lazily-unconstrained read var is a don't-care bit that diversify
 * would randomize, corrupting the array-consistency check into a spurious SAT. */
int zsp_bbsolver_resolve_raw(zsp_bbsolver_t *S) {
    if (!S || !S->problem) return ZSP_BB_ERROR;
    if (!zsp_sat_is_incremental(S->sat)) return ZSP_BB_ERROR;
    S->seed = 0;
    zsp_sat_set_seed(S->sat, 0);
    free(S->node_val);
    S->node_val = NULL;   /* value readback -> raw cnf model (seed 0) */
    int rc = zsp_sat_solve(S->sat);
    S->last_result = rc;
    return rc;
}

/* ------------------------------------------------------------------------- *
 * DSE-2: soft-aware MaxSAT serve.
 *
 * The BV-SAT serve path is otherwise soft-less.  This wrapper keeps the
 * maximal priority-respecting set of soft constraints, mirroring the primary
 * engine's relaxation policy (solver_solve in zsp_search.c): start with all
 * softs kept; on UNSAT, drop the single kept soft with the highest priority
 * *value* (= lowest preference; ties broken toward the last in walk order, as
 * solver_solve does with `>=`) and retry.  Each attempt is a fresh bbsolver
 * because the SAT layer is non-incremental — cheap in the common case (all
 * softs satisfiable → one solve), and bounded by the number that must be
 * dropped otherwise.
 *
 * On ZSP_BB_SAT, *out_bb receives the solved bbsolver (caller frees it and
 * reads the model via zsp_bbsolver_value); the kept-set is reflected in its
 * model.  On UNSAT/UNKNOWN/ERROR, *out_bb is NULL.  A hard-UNSAT problem
 * (UNSAT even with every soft dropped) returns ZSP_BB_UNSAT.
 * ------------------------------------------------------------------------- */
int zsp_bbsolver_check_maxsat(zsp_alloc_t *alloc, SolveProblem *problem,
                              uint64_t seed, zsp_bbsolver_t **out_bb,
                              uint8_t *out_keep, uint32_t keep_cap) {
    if (!problem || !out_bb) return ZSP_BB_ERROR;
    *out_bb = NULL;

    uint32_t n = problem->n_softs;
    if (n == 0) {
        /* No softs — a plain check. */
        zsp_bbsolver_t *bb = zsp_bbsolver_new(alloc, problem);
        if (!bb) return ZSP_BB_ERROR;
        int rc = zsp_bbsolver_check(bb, seed);
        if (rc == ZSP_BB_SAT) { *out_bb = bb; return rc; }
        zsp_bbsolver_free(bb);
        return rc;
    }

    /* Gather soft priorities in softs_head walk order (the soft_keep index). */
    uint32_t *pri = (uint32_t *)xalloc(alloc, n * sizeof(uint32_t));
    uint8_t  *keep = (uint8_t *)xalloc(alloc, n * sizeof(uint8_t));
    if (!pri || !keep) {
        if (pri) xfree(alloc, pri, n * sizeof(uint32_t));
        if (keep) xfree(alloc, keep, n * sizeof(uint8_t));
        return ZSP_BB_ERROR;
    }
    {
        uint32_t i = 0;
        for (ExprRef cur = problem->softs_head; cur != EXPR_NULL && i < n; i++) {
            SoftSpec *ss = (SoftSpec *)POOL_PTR(problem, cur);
            pri[i] = ss->priority;
            keep[i] = 1;
            cur = ss->next;
        }
    }

    /* Fast path: try ALL softs kept. The common case is no conflicting softs, so
     * one solve settles it. */
    int rc;
    {
        zsp_bbsolver_t *bb = zsp_bbsolver_new(alloc, problem);
        if (!bb) { rc = ZSP_BB_ERROR; goto done; }
        bb->soft_keep = keep;   /* keep[] is all-1 from the init above */
        bb->n_softs   = n;
        rc = zsp_bbsolver_check(bb, seed);
        if (rc == ZSP_BB_SAT) {
            *out_bb = bb;
            if (out_keep) {
                uint32_t m = (keep_cap < n) ? keep_cap : n;
                for (uint32_t i = 0; i < m; i++) out_keep[i] = keep[i];
            }
            goto done;
        }
        zsp_bbsolver_free(bb);
        if (rc != ZSP_BB_UNSAT) goto done;   /* UNKNOWN / ERROR — defer */
    }

    /* Some soft conflicts. Use ADDITIVE greedy (Boolector-parity): start from the
     * hard core only and add softs one at a time in DESCENDING preference
     * (ascending priority value; ties by declaration / walk order), keeping each
     * soft that is still SAT with the hard core + the already-kept softs. This
     * keeps EVERY individually-compatible soft. A subtractive "drop the worst on
     * UNSAT" greedy is wrong here: working down to a conflicting higher-preference
     * soft, it sheds satisfiable lower-preference softs as collateral (e.g. it
     * dropped a satisfiable `d==40` while a conflicting sibling forced relaxation).
     * Cost is one solve per soft only on the (rare) conflicting path; the all-kept
     * fast path above covers the common no-conflict case in a single solve. */

    /* Preference order: indices sorted by priority value ascending (0 = keep
     * hardest), ties broken by walk-order index. n is small (<= a few dozen). */
    uint32_t *order = (uint32_t *)xalloc(alloc, n * sizeof(uint32_t));
    if (!order) { rc = ZSP_BB_ERROR; goto done; }
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t best_j = i;
        for (uint32_t j = i + 1; j < n; j++) {
            if (pri[order[j]] < pri[order[best_j]] ||
                (pri[order[j]] == pri[order[best_j]] && order[j] < order[best_j]))
                best_j = j;
        }
        uint32_t tmp = order[i]; order[i] = order[best_j]; order[best_j] = tmp;
    }

    /* Base model: hard core only (all softs relaxed). */
    for (uint32_t i = 0; i < n; i++) keep[i] = 0;
    zsp_bbsolver_t *best = zsp_bbsolver_new(alloc, problem);
    if (!best) { xfree(alloc, order, n * sizeof(uint32_t)); rc = ZSP_BB_ERROR; goto done; }
    best->soft_keep = keep;
    best->n_softs   = n;
    rc = zsp_bbsolver_check(best, seed);
    if (rc != ZSP_BB_SAT) {
        /* Hard core itself UNSAT (or undecided) — softs can't help. */
        zsp_bbsolver_free(best);
        xfree(alloc, order, n * sizeof(uint32_t));
        goto done;
    }

    /* Additively try each soft, most-preferred first. */
    for (uint32_t k = 0; k < n; k++) {
        uint32_t idx = order[k];
        keep[idx] = 1;                                  /* tentatively add */
        zsp_bbsolver_t *cand = zsp_bbsolver_new(alloc, problem);
        if (!cand) { keep[idx] = 0; continue; }         /* OOM: skip this soft */
        cand->soft_keep = keep;
        cand->n_softs   = n;
        int crc = zsp_bbsolver_check(cand, seed);
        if (crc == ZSP_BB_SAT) {
            zsp_bbsolver_free(best);                    /* this soft fits — keep it */
            best = cand;
        } else {
            zsp_bbsolver_free(cand);                    /* UNSAT/undecided — drop it */
            keep[idx] = 0;
        }
    }

    *out_bb = best;
    rc = ZSP_BB_SAT;
    if (out_keep) {
        uint32_t m = (keep_cap < n) ? keep_cap : n;
        for (uint32_t i = 0; i < m; i++) out_keep[i] = keep[i];
    }
    xfree(alloc, order, n * sizeof(uint32_t));

done:
    xfree(alloc, pri, n * sizeof(uint32_t));
    xfree(alloc, keep, n * sizeof(uint8_t));
    return rc;
}

void zsp_bbsolver_set_soft_keep(zsp_bbsolver_t *S, const uint8_t *keep,
                                uint32_t n) {
    if (!S) return;
    S->soft_keep = keep;   /* caller owns the array; must outlive the check */
    S->n_softs   = n;
}

/* A variable that appears in no constraint is never bit-blasted, so the SAT
 * model says nothing about it. For a design-verification (randomization) solve
 * every rand variable should still get a value, and an unconstrained one should
 * be uniformly random rather than a fixed 0. Derive a value from (seed, var_id,
 * limb): deterministic per seed (reproducible), varying across vars/seeds.
 * Only used when a nonzero (diversity) seed was supplied; seed 0 keeps the
 * legacy deterministic 0 so BMC/decision flows are unchanged. */
static uint64_t _free_var_fill(uint64_t seed, uint32_t var_id, uint32_t limb) {
    uint64_t x = seed + 0x9E3779B97F4A7C15ULL * ((uint64_t)var_id + 1)
                      + 0xD1B54A32D192ED03ULL * ((uint64_t)limb + 1);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int zsp_bbsolver_value(zsp_bbsolver_t *S, uint32_t var_id, int64_t *out_value) {
    if (!S || !out_value) return -1;
    if (S->last_result != ZSP_BB_SAT) return -1;
    if (var_id >= S->n_vars) return -1;
    bb_var_t *v = &S->vars[var_id];
    if (!v->defined) return -1;
    if (!v->bv_built) {
        /* Unreferenced variable: seeded-random for a diversity solve, else 0. */
        uint64_t r = S->seed ? _free_var_fill(S->seed, var_id, 0) : 0;
        if (v->width < 64) r &= ((uint64_t)1 << v->width) - 1;
        *out_value = (int64_t)r;
        return 0;
    }
    /* bits[] are stored MSB-first: bits[0] is the high bit (logical position
     * size-1), bits[size-1] is the LSB. Read only the low 64 bits here; the
     * shift stays < 64 so there is no UB even for a wide var (the high bits are
     * simply dropped — callers needing them use zsp_bbsolver_value_wide). */
    uint64_t val = 0;
    uint32_t n = v->bv.size < 64 ? v->bv.size : 64;
    /* Fallback seeded fill for don't-care bits when the flip-check pass did not
     * run (e.g. it was skipped or out of memory). */
    uint64_t rnd = S->seed ? _free_var_fill(S->seed, var_id, 0) : 0;
    for (uint32_t p = 0; p < n; p++) {
        /* logical bit p (0 = LSB) lives at bits[size-1-p] */
        zsp_aig_node_t bit = v->bv.bits[v->bv.size - 1 - p];
        int set;
        if (S->node_val)                                 /* flip-check result */
            set = _lit_val(S->node_val, bit);
        else if (S->seed && zsp_aig_cnf_is_free(S->cnf, bit))
            set = (int)((rnd >> p) & 1);
        else
            set = (zsp_aig_cnf_value(S->cnf, bit) == 1) ? 1 : 0;
        if (set) val |= (uint64_t)1 << p;
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
        /* Unreferenced variable: seeded-random for a diversity solve, else 0. */
        if (S->seed) {
            uint32_t full = v->width / 64;      /* fully-used limbs */
            for (uint32_t i = 0; i < n_limbs; i++) {
                uint64_t r = _free_var_fill(S->seed, var_id, i);
                if (i < full) limbs[i] = r;
                else if (i == full && (v->width & 63))
                    limbs[i] = r & (((uint64_t)1 << (v->width & 63)) - 1);
            }
        }
        return 0;
    }
    /* Little-endian limbs: logical bit p (0 = LSB) → limbs[p/64] bit (p%64).
     * bits[] is MSB-first, so logical bit p is at bits[size-1-p]. The shift is
     * always < 64, so no UB regardless of width. */
    uint32_t cap = n_limbs * 64;
    uint32_t n = v->bv.size < cap ? v->bv.size : cap;
    for (uint32_t p = 0; p < n; p++) {
        zsp_aig_node_t bit = v->bv.bits[v->bv.size - 1 - p];
        int set;
        if (S->node_val) {                               /* flip-check result */
            set = _lit_val(S->node_val, bit);
        } else if (S->seed && zsp_aig_cnf_is_free(S->cnf, bit)) {
            /* don't-care bit: seeded-random (see zsp_bbsolver_value) */
            uint64_t r = _free_var_fill(S->seed, var_id, p >> 6);
            set = (int)((r >> (p & 63)) & 1);
        } else {
            set = (zsp_aig_cnf_value(S->cnf, bit) == 1) ? 1 : 0;
        }
        if (set) limbs[p >> 6] |= (uint64_t)1 << (p & 63);
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
