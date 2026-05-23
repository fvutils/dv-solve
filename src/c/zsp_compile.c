#include <string.h>
#include <stdio.h>
#include "zsp_ctx.h"
#include "zsp_propagator.h"
#include "zsp_problem.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/** Number of 64-bit limbs needed for `width` bits. */
static uint32_t _n_limbs(uint16_t width) {
    return (uint32_t)((width + 63u) / 64u);
}

/** Initialise a tier-0 Variable (width ≤ 32). */
static void _init_tier0(Variable *v, uint16_t width, uint8_t flags,
                         int64_t lo, int64_t hi) {
    v->lo           = (int32_t)lo;
    v->hi           = (int32_t)hi;
    v->holes_offset = 0;
    v->width        = width;
    v->flags        = (flags & ~(VAR_TIER1 | VAR_TIER2));
    v->_pad         = 0;
}

/** Initialise a tier-1 Variable (33–64 bits); allocates WideBounds64. */
static int _init_tier1(SolveCtx *ctx, Variable *v, uint16_t width,
                        uint8_t flags, int64_t lo, int64_t hi) {
    uint32_t ref = zsp_pool_alloc(&ctx->pool,
                                  (uint32_t)sizeof(WideBounds64),
                                  (uint32_t)_Alignof(WideBounds64));
    if (ref == EXPR_NULL) return -1;

    WideBounds64 *wb = (WideBounds64 *)zsp_pool_ptr(&ctx->pool, ref);
    wb->lo = lo;
    wb->hi = hi;

    v->lo           = 0;
    v->hi           = 0;
    v->holes_offset = ref;
    v->width        = width;
    v->flags        = (uint8_t)((flags & ~VAR_TIER2) | VAR_TIER1);
    v->_pad         = 0;
    return 0;
}

/** Initialise a tier-2 Variable (> 64 bits); allocates WideBoundsN. */
static int _init_tier2(SolveCtx *ctx, Variable *v, uint16_t width,
                        uint8_t flags, int64_t lo, int64_t hi) {
    uint32_t n = _n_limbs(width);
    /* Header + 2 limb arrays */
    uint32_t total = (uint32_t)sizeof(WideBoundsN) + 2u * n * (uint32_t)sizeof(uint64_t);
    uint32_t ref = zsp_pool_alloc(&ctx->pool, total,
                                  (uint32_t)_Alignof(WideBoundsN));
    if (ref == EXPR_NULL) return -1;

    WideBoundsN *wn = (WideBoundsN *)zsp_pool_ptr(&ctx->pool, ref);
    wn->n_limbs = n;
    wn->_pad    = 0;

    uint64_t *lo_limbs = (uint64_t *)(wn + 1);
    uint64_t *hi_limbs = lo_limbs + n;

    /* Zero-initialise all limbs first */
    memset(lo_limbs, 0, 2u * n * sizeof(uint64_t));

    /* Store lo / hi in the first limb (for the common ≤128-bit case) */
    if (lo >= 0 || (flags & VAR_SIGNED)) {
        lo_limbs[0] = (uint64_t)lo;
        hi_limbs[0] = (uint64_t)hi;
        /* Sign-extend negative values across remaining limbs */
        if ((flags & VAR_SIGNED) && lo < 0) {
            for (uint32_t i = 1; i < n; i++) lo_limbs[i] = ~(uint64_t)0;
        }
        if ((flags & VAR_SIGNED) && hi < 0) {
            for (uint32_t i = 1; i < n; i++) hi_limbs[i] = ~(uint64_t)0;
        }
    } else {
        lo_limbs[0] = (uint64_t)lo;
        hi_limbs[0] = (uint64_t)hi;
    }

    v->lo           = 0;
    v->hi           = 0;
    v->holes_offset = ref;
    v->width        = width;
    v->flags        = (uint8_t)((flags & ~VAR_TIER1) | VAR_TIER2);
    v->_pad         = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: should a variable use 64-bit propagators?                   */
/*                                                                     */
/* Returns true when the variable's actual storage is tier-1 or above, */
/* regardless of its declared width.  This covers unsigned 32-bit vars */
/* that have been promoted to tier-1 to avoid int32 overflow.          */
/* ------------------------------------------------------------------ */
static int _var_needs_wide(const SolveCtx *ctx, uint32_t var_id) {
    return !VAR_IS_TIER0(ctx->vars[var_id].flags);
}

/* ------------------------------------------------------------------ */
/* _compile_constraint — DAG → propagator translation                  */
/* ------------------------------------------------------------------ */

/* Helper: is this ExprRef an ExprVar?  Returns var_id via out_id. */
static int _is_var(SolveProblem *sp, ExprRef r, uint32_t *out_id) {
    if (r == EXPR_NULL) return 0;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, r);
    if (k != EXPR_VAR) return 0;
    ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, r);
    *out_id = ev->var_id;
    return 1;
}

/* Helper: is this ExprRef an ExprConst?  Returns value via out_val. */
static int _is_const(SolveProblem *sp, ExprRef r, int64_t *out_val) {
    if (r == EXPR_NULL) return 0;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, r);
    if (k != EXPR_CONST) return 0;
    ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, r);
    *out_val = ec->value;
    return 1;
}

/*
 * Handle a var-const or const-var comparison by tightening bounds
 * directly at compile time (no propagator needed; constant bounds).
 * `flipped` is 1 when the constant is on the left side of the operator.
 * Returns 1 if handled, 0 otherwise.
 */
static int _compile_var_const_cmp(SolveCtx *ctx, BinOp op,
                                   uint32_t vid, int64_t cv, int flipped) {
    BinOp eff = op;
    if (flipped) {
        switch (op) {
        case BIN_LTE: eff = BIN_GTE; break;
        case BIN_LT:  eff = BIN_GT;  break;
        case BIN_GTE: eff = BIN_LTE; break;
        case BIN_GT:  eff = BIN_LT;  break;
        default:      eff = op;      break;
        }
    }

    /* Use 64-bit tighten functions to avoid truncating large constants. */
    switch (eff) {
    case BIN_EQ:
        if (ctx_tighten_lb64(ctx, vid, cv) == PROP_CONFLICT) return -1;
        if (ctx_tighten_ub64(ctx, vid, cv) == PROP_CONFLICT) return -1;
        return 1;
    case BIN_NEQ:
        /* Cheap tightenings first: if the constant equals a current
         * bound, we can shrink it without allocating a propagator. */
        {
            int64_t lo = (int64_t)var_lo64(ctx, &ctx->vars[vid]);
            int64_t hi = (int64_t)var_hi64(ctx, &ctx->vars[vid]);
            if (cv < lo || cv > hi) return 1;  /* already excluded */
            if (cv == lo) {
                if (ctx_tighten_lb64(ctx, vid, cv + 1) == PROP_CONFLICT) return -1;
                return 1;
            }
            if (cv == hi) {
                if (ctx_tighten_ub64(ctx, vid, cv - 1) == PROP_CONFLICT) return -1;
                return 1;
            }
        }
        /* General case: create a const-var and add an NE propagator. */
        if (ctx->n_vars < ctx->n_vars_capacity) {
            uint32_t cv_id = ctx->n_vars;
            _init_tier0(&ctx->vars[cv_id], 32, 0, cv, cv);
            ctx->n_vars = cv_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[cv_id] = EXPR_NULL;
            prop_add_bounds_ne_32(ctx, vid, cv_id, 0);
            return 1;
        }
        return 0;
    case BIN_LTE:
        if (ctx_tighten_ub64(ctx, vid, cv) == PROP_CONFLICT) return -1;
        return 1;
    case BIN_LT:
        if (ctx_tighten_ub64(ctx, vid, cv - 1) == PROP_CONFLICT) return -1;
        return 1;
    case BIN_GTE:
        if (ctx_tighten_lb64(ctx, vid, cv) == PROP_CONFLICT) return -1;
        return 1;
    case BIN_GT:
        if (ctx_tighten_lb64(ctx, vid, cv + 1) == PROP_CONFLICT) return -1;
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* _flatten_or — collect var-const comparison clauses from an OR tree */
/* ------------------------------------------------------------------ */

#define MAX_OR_CLAUSES 16

typedef struct {
    uint32_t var_id;
    uint32_t op;
    int64_t  constant;
    uint32_t rhs_var_id;  /* UINT32_MAX = use constant; else = var-var comparison */
} OrClause;

/* Strip zero/sign extend of an EXPR_VAR; return underlying var id and the
 * inner bit width via out-params. Returns 1 if `ref` is either an
 * EXPR_VAR or an EXPR_EXTEND whose operand is an EXPR_VAR; 0 otherwise. */
static int _is_var_maybe_extend(SolveProblem *sp, ExprRef ref,
                                 uint32_t *out_vid, uint16_t *out_inner_width,
                                 int *out_sign_extend) {
    if (ref == EXPR_NULL) return 0;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    if (k == EXPR_VAR) {
        *out_vid = ((ExprVar *)zsp_pool_ptr(&sp->pool, ref))->var_id;
        *out_inner_width = 0;   /* "no extend wrapper" */
        *out_sign_extend = 0;
        return 1;
    }
    if (k == EXPR_EXTEND) {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        if (_is_var(sp, ee->operand, out_vid)) {
            *out_inner_width = ee->from_bits;
            *out_sign_extend = ee->sign_extend;
            return 1;
        }
    }
    return 0;
}

/* Flip a comparison operator (for the `not` case). */
static uint32_t _flip_cmp(uint32_t op) {
    switch (op) {
    case BIN_EQ:  return BIN_NEQ;
    case BIN_NEQ: return BIN_EQ;
    case BIN_LT:  return BIN_GTE;
    case BIN_LTE: return BIN_GT;
    case BIN_GT:  return BIN_LTE;
    case BIN_GTE: return BIN_LT;
    default:      return op;
    }
}

/* Swap-side flip: `c op v` -> `v swapped_op c` keeping semantics. */
static uint32_t _swap_cmp(uint32_t op) {
    switch (op) {
    case BIN_LT:  return BIN_GT;
    case BIN_LTE: return BIN_GTE;
    case BIN_GT:  return BIN_LT;
    case BIN_GTE: return BIN_LTE;
    default:      return op;   /* EQ/NEQ symmetric */
    }
}

/* Try to resolve a comparison leaf cv-vs-var (or vice versa) at compile
 * time when the var is zero-extended to a wider type than the constant
 * can occupy. Sets *out_truth to 1 (always true) or 0 (always false)
 * and returns 1 if resolved; returns 0 otherwise. Only used for the
 * zero-extend case — sign-extend has more shape cases we don't fold. */
static int _try_fold_extend_cmp(uint32_t op, int64_t cv,
                                 uint16_t inner_w, int *out_truth) {
    if (inner_w == 0 || inner_w >= 64) return 0;
    uint64_t inner_max = ((uint64_t)1 << inner_w) - 1;  /* value range upper bound */
    uint64_t ucv = (uint64_t)cv;
    /* Value range of zero_extend(v) is [0, inner_max], compared against ucv. */
    switch (op) {
    case BIN_LT:  /* v < ucv: true iff inner_max < ucv */
        if (ucv > inner_max) { *out_truth = 1; return 1; }
        if (ucv == 0)        { *out_truth = 0; return 1; }
        return 0;
    case BIN_LTE: /* v <= ucv: true iff inner_max <= ucv */
        if (ucv >= inner_max) { *out_truth = 1; return 1; }
        return 0;
    case BIN_GT:  /* v > ucv: false iff inner_max <= ucv */
        if (ucv >= inner_max) { *out_truth = 0; return 1; }
        return 0;
    case BIN_GTE: /* v >= ucv: false iff inner_max < ucv */
        if (ucv > inner_max) { *out_truth = 0; return 1; }
        if (ucv == 0)        { *out_truth = 1; return 1; }
        return 0;
    case BIN_EQ:  /* v == ucv: false iff ucv > inner_max */
        if (ucv > inner_max) { *out_truth = 0; return 1; }
        return 0;
    case BIN_NEQ: /* v != ucv: true iff ucv > inner_max */
        if (ucv > inner_max) { *out_truth = 1; return 1; }
        return 0;
    default:
        return 0;
    }
}

/* Classify a single (non-OR) leaf of the OR tree. Possible outcomes:
 *   *truth_known = 1, *truth = 0/1  -- leaf is a compile-time constant
 *   *truth_known = 0, returns 1     -- produced one OrClause in *out
 *   *truth_known = 0, returns 0     -- unsupported shape
 *
 * Handles:
 *   - direct (cmp var const) / (cmp const var) / (cmp var var)
 *   - (not (cmp ...)) by flipping the operator and recursing
 *   - (cmp (zero_extend v) const) by folding to truth or rewriting
 *     to (cmp v masked_const) when the constant fits in v's width
 *   - (cmp const (zero_extend v))  symmetrically */
static int _classify_or_leaf(SolveProblem *sp, ExprRef ref,
                              OrClause *out, int *truth_known, int *truth) {
    *truth_known = 0;
    *truth = 0;
    if (ref == EXPR_NULL) return 0;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);

    /* (not X): flip and recurse. */
    if (k == EXPR_UNARY) {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        if (eu->op != UN_NOT) return 0;
        OrClause inner;
        int it_known = 0, it_truth = 0;
        int rc = _classify_or_leaf(sp, eu->operand, &inner,
                                    &it_known, &it_truth);
        if (it_known) {
            *truth_known = 1;
            *truth = !it_truth;
            return 0;
        }
        if (rc == 1) {
            *out = inner;
            out->op = _flip_cmp(inner.op);
            return 1;
        }
        return 0;
    }

    if (k != EXPR_BINARY) return 0;
    ExprBinary *e = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);

    /* Only comparison ops are valid leaves. */
    switch (e->op) {
    case BIN_EQ: case BIN_NEQ:
    case BIN_LT: case BIN_LTE: case BIN_GT: case BIN_GTE:
        break;
    default:
        return 0;
    }

    /* var-const / extend(var)-const */
    uint32_t vid, vid2; int64_t cv;
    uint16_t inner_w; int sign_ext;
    if (_is_var_maybe_extend(sp, e->lhs, &vid, &inner_w, &sign_ext)
        && _is_const(sp, e->rhs, &cv)) {
        if (inner_w > 0 && !sign_ext) {
            if (_try_fold_extend_cmp(e->op, cv, inner_w, truth)) {
                *truth_known = 1;
                return 0;
            }
            /* Constant fits in inner width: rewrite without extend. */
            uint64_t inner_mask = ((uint64_t)1 << inner_w) - 1;
            cv = (int64_t)((uint64_t)cv & inner_mask);
        }
        if (inner_w > 0 && sign_ext) return 0;   /* don't fold sign-extend yet */
        out->var_id = vid;
        out->op = e->op;
        out->constant = cv;
        out->rhs_var_id = UINT32_MAX;
        return 1;
    }
    if (_is_const(sp, e->lhs, &cv)
        && _is_var_maybe_extend(sp, e->rhs, &vid, &inner_w, &sign_ext)) {
        uint32_t swapped = _swap_cmp(e->op);
        if (inner_w > 0 && !sign_ext) {
            if (_try_fold_extend_cmp(swapped, cv, inner_w, truth)) {
                *truth_known = 1;
                return 0;
            }
            uint64_t inner_mask = ((uint64_t)1 << inner_w) - 1;
            cv = (int64_t)((uint64_t)cv & inner_mask);
        }
        if (inner_w > 0 && sign_ext) return 0;
        out->var_id = vid;
        out->op = swapped;
        out->constant = cv;
        out->rhs_var_id = UINT32_MAX;
        return 1;
    }

    /* var-var */
    if (_is_var(sp, e->lhs, &vid) && _is_var(sp, e->rhs, &vid2)) {
        out->var_id = vid;
        out->op = e->op;
        out->constant = 0;
        out->rhs_var_id = vid2;
        return 1;
    }

    return 0;
}

/**
 * Recursively flatten a BIN_OR tree.  Each leaf must classify to a single
 * comparison clause (after stripping outer `not` and inner zero-extend).
 * Always-false leaves are dropped; always-true leaves set *any_true and
 * short-circuit (the caller can then skip adding a propagator at all).
 *
 * Returns number of clauses written to `out`, or -1 if any leaf has a
 * shape we cannot handle.
 */
static int _flatten_or(SolveProblem *sp, ExprRef ref,
                        OrClause *out, int max_clauses, int *any_true) {
    if (ref == EXPR_NULL) return -1;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);

    if (k == EXPR_BINARY) {
        ExprBinary *e = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        if (e->op == BIN_OR) {
            int n_left = _flatten_or(sp, e->lhs, out, max_clauses, any_true);
            if (n_left < 0) return -1;
            if (*any_true) return n_left;
            int n_right = _flatten_or(sp, e->rhs, out + n_left,
                                       max_clauses - n_left, any_true);
            if (n_right < 0) return -1;
            return n_left + n_right;
        }
    }

    /* Non-OR leaf */
    OrClause leaf;
    int truth_known = 0, truth = 0;
    int rc = _classify_or_leaf(sp, ref, &leaf, &truth_known, &truth);
    if (truth_known) {
        if (truth) *any_true = 1;
        return 0;   /* either short-circuit, or drop the false leaf */
    }
    if (rc != 1) return -1;
    if (max_clauses < 1) return -1;
    out[0] = leaf;
    return 1;
}

/* Forward declaration */
static int _compile_constraint(SolveCtx *ctx, SolveProblem *sp, ExprRef root);
static int _compile_neg_constraint(SolveCtx *ctx, SolveProblem *sp, ExprRef root);

/* ------------------------------------------------------------------ */
/* Union-Find for variable aliasing                                    */
/* ------------------------------------------------------------------ */

/** Find the root representative of var_id in the alias table. */
static uint32_t _alias_find(uint32_t *alias, uint32_t id) {
    while (alias[id] != id) {
        alias[id] = alias[alias[id]];  /* path compression */
        id = alias[id];
    }
    return id;
}

/** Merge two variables: the one with the smaller ID becomes root. */
static void _alias_union(uint32_t *alias, uint32_t a, uint32_t b) {
    uint32_t ra = _alias_find(alias, a);
    uint32_t rb = _alias_find(alias, b);
    if (ra == rb) return;
    /* Smaller ID is the root (deterministic, preserves user var ordering) */
    if (ra < rb) alias[rb] = ra;
    else         alias[ra] = rb;
}

/** Resolve a var_id through the alias table. No-op if alias is NULL. */
static inline uint32_t _resolve(const SolveCtx *ctx, uint32_t vid) {
    if (ctx->var_alias == NULL) return vid;
    return _alias_find(ctx->var_alias, vid);
}

/* Helper: compile a constraint body in a guard-gated context.
 * Unlike _compile_constraint, this avoids compile-time bound tightening for
 * var-const patterns (which is irreversible and can't be gated). Instead,
 * it uses Implication propagators for var-const EQ/LTE/GTE. */
static int _compile_gated_constraint(SolveCtx *ctx, SolveProblem *sp,
                                      ExprRef root, uint32_t guard_id) {
    if (root == EXPR_NULL) return 1;

    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, root);
    if (k == EXPR_BINARY) {
        ExprBinary *e = (ExprBinary *)zsp_pool_ptr(&sp->pool, root);
        uint32_t vid; int64_t cv;
        int is_vc = _is_var(sp, e->lhs, &vid) && _is_const(sp, e->rhs, &cv);
        int is_cv = !is_vc && _is_const(sp, e->lhs, &cv) && _is_var(sp, e->rhs, &vid);

        if (is_vc || is_cv) {
            BinOp op = e->op;
            if (is_cv) {
                switch (op) {
                case BIN_LT:  op = BIN_GT;  break;
                case BIN_LTE: op = BIN_GTE; break;
                case BIN_GT:  op = BIN_LT;  break;
                case BIN_GTE: op = BIN_LTE; break;
                default: break;
                }
            }
            /* Use Implication propagators gated by guard_id */
            switch (op) {
            case BIN_EQ:
                prop_add_implication_32(ctx, guard_id, vid, (int32_t)cv, 1, 0);
                prop_add_implication_32(ctx, guard_id, vid, (int32_t)cv, 0, 0);
                return 1;
            case BIN_LTE:
                prop_add_implication_32(ctx, guard_id, vid, (int32_t)cv, 1, 0);
                return 1;
            case BIN_LT:
                prop_add_implication_32(ctx, guard_id, vid, (int32_t)(cv - 1), 1, 0);
                return 1;
            case BIN_GTE:
                prop_add_implication_32(ctx, guard_id, vid, (int32_t)cv, 0, 0);
                return 1;
            case BIN_GT:
                prop_add_implication_32(ctx, guard_id, vid, (int32_t)(cv + 1), 0, 0);
                return 1;
            default: break;
            }
        }
    }

    /* Fallback: compile normally and gate resulting propagators */
    uint32_t props_before = ctx->n_props;
    int rc = _compile_constraint(ctx, sp, root);
    if (rc > 0) {
        for (uint32_t pi = props_before; pi < ctx->n_props; pi++) {
            if (ctx->prop_guard_vars && pi < ctx->n_prop_refs_capacity)
                ctx->prop_guard_vars[pi] = guard_id;
        }
    }
    return rc;
}

/* Helper: compile r_id == BinaryExpr(expr_ref) without allocating in sp pool.
 * Used when the sp pool may be full (e.g. after builder finalization). */
static int _compile_binexpr_eq_var(SolveCtx *ctx, SolveProblem *sp,
                                    ExprRef binexpr_ref, uint32_t r_id) {
    ExprKind bk = *(ExprKind *)zsp_pool_ptr(&sp->pool, binexpr_ref);
    if (bk != EXPR_BINARY) return 0;

    ExprBinary *binop = (ExprBinary *)zsp_pool_ptr(&sp->pool, binexpr_ref);
    uint32_t a_id, b_id;
    int64_t cv;
    int has_var_var = _is_var(sp, binop->lhs, &a_id) && _is_var(sp, binop->rhs, &b_id);
    int has_const_var = _is_const(sp, binop->lhs, &cv) && _is_var(sp, binop->rhs, &b_id);
    int has_var_const = _is_var(sp, binop->lhs, &a_id) && _is_const(sp, binop->rhs, &cv);
    if (has_var_var) { a_id = _resolve(ctx, a_id); b_id = _resolve(ctx, b_id); }
    if (has_const_var) { b_id = _resolve(ctx, b_id); }
    if (has_var_const) { a_id = _resolve(ctx, a_id); }

    /* Promote constants to const-variables */
    if (has_var_const && !has_var_var) {
        if (ctx->n_vars < ctx->n_vars_capacity) {
            b_id = ctx->n_vars;
            Variable *cvv = &ctx->vars[b_id];
            _init_tier0(cvv, 32, 0, cv, cv);
            ctx->n_vars = b_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[b_id] = EXPR_NULL;
            has_var_var = 1;
        }
    }
    if (has_const_var && !has_var_var) {
        if (ctx->n_vars < ctx->n_vars_capacity) {
            a_id = ctx->n_vars;
            Variable *cvv = &ctx->vars[a_id];
            _init_tier0(cvv, 32, 0, cv, cv);
            ctx->n_vars = a_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[a_id] = EXPR_NULL;
            has_var_var = 1;
        }
    }

    if (!has_var_var) return 0;

    /* Reified comparison: r ↔ (a op b) — wire to reification propagators
     * before falling through to arithmetic patterns. */
    switch (binop->op) {
    case BIN_EQ:
        prop_add_reification_eq_32(ctx, r_id, a_id, b_id, 0);
        return 1;
    case BIN_LTE:
        prop_add_reification_32(ctx, r_id, a_id, b_id, 0);
        return 1;
    case BIN_GTE:
        prop_add_reification_32(ctx, r_id, b_id, a_id, 0);
        return 1;
    case BIN_LT: {
        /* r ↔ (a < b)  ≡  r ↔ (a ≤ b-1).  Need a const-var for b-1 when
         * b is a constant; otherwise fall back to materialising b-1 by
         * tying a fresh aux through an add propagator (not done here). */
        int64_t b_cv;
        if (_is_const(sp, binop->rhs, &b_cv) && ctx->n_vars < ctx->n_vars_capacity) {
            uint32_t bm1 = ctx->n_vars;
            _init_tier0(&ctx->vars[bm1], 32, 0, b_cv - 1, b_cv - 1);
            ctx->n_vars = bm1 + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[bm1] = EXPR_NULL;
            prop_add_reification_32(ctx, r_id, a_id, bm1, 0);
            return 1;
        }
        break;
    }
    case BIN_GT: {
        int64_t a_cv;
        if (_is_const(sp, binop->lhs, &a_cv) && ctx->n_vars < ctx->n_vars_capacity) {
            uint32_t am1 = ctx->n_vars;
            _init_tier0(&ctx->vars[am1], 32, 0, a_cv - 1, a_cv - 1);
            ctx->n_vars = am1 + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[am1] = EXPR_NULL;
            prop_add_reification_32(ctx, r_id, b_id, am1, 0);
            return 1;
        }
        break;
    }
    default: break;
    }

    /* Modular BV add/sub with a constant operand: route to the wrap-aware
     * propagator so that x.hi + c >= 2^width wraps mod 2^width, instead
     * of the non-modular bounds_add path which silently produces unsound
     * (over-tight or out-of-domain) results on overflow. Width comes from
     * the result variable. Only widths 1..63 are handled (the propagator
     * falls back to a no-op at width 64). */
    uint16_t r_w = ctx->vars[r_id].width;
    if (r_w >= 1 && r_w <= 63 &&
        (has_var_const || has_const_var) &&
        binop->op == BIN_ADD) {
        uint64_t M = (uint64_t)1 << r_w;
        uint64_t cmask = M - 1;
        uint64_t c_red = (uint64_t)cv & cmask;
        /* r = (x + c) mod M; commutative so const-var is the same */
        uint32_t x_id = has_var_const ? a_id : b_id;
        prop_add_bvadd_const_64(ctx, r_id, x_id, c_red, (uint8_t)r_w, 0);
        return 1;
    }

    int wide = _var_needs_wide(ctx, r_id) ||
               _var_needs_wide(ctx, a_id) ||
               _var_needs_wide(ctx, b_id);
    if (!wide) {
        switch (binop->op) {
        case BIN_ADD: prop_add_bounds_add_32(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_SUB: prop_add_bounds_add_32(ctx, a_id, r_id, b_id, 0); return 1;
        case BIN_MUL: prop_add_bounds_mul_32(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_DIV: prop_add_bounds_div_32(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_MOD: prop_add_bounds_mod_32(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_BAND: prop_add_bounds_band_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_BOR:  prop_add_bounds_bor_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_BXOR: prop_add_bounds_bxor_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_LSHIFT: prop_add_bounds_shl_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_RSHIFT: prop_add_bounds_lshr_64(ctx, r_id, a_id, b_id, 0); return 1;
        default: break;
        }
    } else {
        switch (binop->op) {
        case BIN_ADD: prop_add_bounds_add_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_SUB: prop_add_bounds_add_64(ctx, a_id, r_id, b_id, 0); return 1;
        case BIN_MUL: prop_add_bounds_mul_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_DIV: prop_add_bounds_div_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_MOD: prop_add_bounds_mod_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_BAND: prop_add_bounds_band_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_BOR:  prop_add_bounds_bor_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_BXOR: prop_add_bounds_bxor_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_LSHIFT: prop_add_bounds_shl_64(ctx, r_id, a_id, b_id, 0); return 1;
        case BIN_RSHIFT: prop_add_bounds_lshr_64(ctx, r_id, a_id, b_id, 0); return 1;
        default: break;
        }
    }
    return 0;
}

/* Materialise a Boolean expression as a 0/1 var ID.  Handles:
 *   - EXPR_VAR (already a Bool var)
 *   - EXPR_CONST (creates a singleton)
 *   - EXPR_BINARY(EQ, var|extend(var), const|var) via reification_eq
 *   - EXPR_BINARY(AND/OR, X, Y) via recursive g_and = ite(gX, gY, 0) /
 *     g_or  = ite(gX, 1, gY)
 *   - EXPR_UNARY(NOT, X) by inverting (creates `1 - gX` via add)
 * Returns the guard var ID, or EXPR_NULL when the shape is too complex
 * for the current materialiser to handle. */
static uint32_t _value_to_var(SolveCtx *ctx, SolveProblem *sp,
                               ExprRef ref, uint8_t width);

static uint32_t _bool_to_var(SolveCtx *ctx, SolveProblem *sp, ExprRef ref) {
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);

    if (k == EXPR_VAR) {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        return _resolve(ctx, ev->var_id);
    }
    if (k == EXPR_CONST) {
        if (ctx->n_vars >= ctx->n_vars_capacity) return EXPR_NULL;
        ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, ref);
        uint32_t gid = ctx->n_vars;
        int64_t v = ec->value ? 1 : 0;
        _init_tier0(&ctx->vars[gid], 1, VAR_AUX, v, v);
        ctx->n_vars = gid + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
        return gid;
    }
    if (k == EXPR_UNARY) {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        if (eu->op == UN_NOT) {
            uint32_t inner = _bool_to_var(ctx, sp, eu->operand);
            if (inner == EXPR_NULL) return EXPR_NULL;
            if (ctx->n_vars + 1 >= ctx->n_vars_capacity) return EXPR_NULL;
            /* not_gid = 1 - inner.  Use add: 1_const + (not_gid + inner) ?
             * Simpler: not_gid + inner == 1. Allocate a const-1 var and use
             * bounds_add_32 (a + b == c form: prop_add_bounds_add_32(c=one,
             * a=not_gid, b=inner)) to encode not_gid + inner == 1. */
            uint32_t not_gid = ctx->n_vars;
            _init_tier0(&ctx->vars[not_gid], 1, VAR_AUX, 0, 1);
            ctx->n_vars = not_gid + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[not_gid] = EXPR_NULL;
            uint32_t one_id = ctx->n_vars;
            _init_tier0(&ctx->vars[one_id], 32, VAR_AUX, 1, 1);
            ctx->n_vars = one_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[one_id] = EXPR_NULL;
            prop_add_bounds_add_32(ctx, one_id, not_gid, inner, 0);
            return not_gid;
        }
        return EXPR_NULL;
    }
    if (k != EXPR_BINARY) return EXPR_NULL;

    ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);

    if (eb->op == BIN_AND) {
        /* g_and = ite(g_lhs, g_rhs, 0) */
        uint32_t gl = _bool_to_var(ctx, sp, eb->lhs);
        if (gl == EXPR_NULL) return EXPR_NULL;
        uint32_t gr = _bool_to_var(ctx, sp, eb->rhs);
        if (gr == EXPR_NULL) return EXPR_NULL;
        if (ctx->n_vars + 1 >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t zero_id = ctx->n_vars;
        _init_tier0(&ctx->vars[zero_id], 1, VAR_AUX, 0, 0);
        ctx->n_vars = zero_id + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[zero_id] = EXPR_NULL;
        uint32_t gid = ctx->n_vars;
        _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
        ctx->n_vars = gid + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
        prop_add_ite_value_64(ctx, gid, gl, gr, zero_id, 0);
        return gid;
    }
    if (eb->op == BIN_OR) {
        /* g_or = ite(g_lhs, 1, g_rhs) */
        uint32_t gl = _bool_to_var(ctx, sp, eb->lhs);
        if (gl == EXPR_NULL) return EXPR_NULL;
        uint32_t gr = _bool_to_var(ctx, sp, eb->rhs);
        if (gr == EXPR_NULL) return EXPR_NULL;
        if (ctx->n_vars + 1 >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t one_id = ctx->n_vars;
        _init_tier0(&ctx->vars[one_id], 1, VAR_AUX, 1, 1);
        ctx->n_vars = one_id + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[one_id] = EXPR_NULL;
        uint32_t gid = ctx->n_vars;
        _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
        ctx->n_vars = gid + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
        prop_add_ite_value_64(ctx, gid, gl, one_id, gr, 0);
        return gid;
    }

    /* Comparison: turn into a reified guard. Supported shapes are
     * (cmp var const), (cmp const var), and (cmp var var). For non-EQ
     * comparisons we canonicalise to `x ≤ y` (the form reification_32
     * speaks) and invert when needed via an extra add-to-1 var. */
    uint32_t cmp_vid; int64_t cmp_cv;
    int is_vc = _is_var(sp, eb->lhs, &cmp_vid) && _is_const(sp, eb->rhs, &cmp_cv);
    int is_cv = !is_vc && _is_const(sp, eb->lhs, &cmp_cv) && _is_var(sp, eb->rhs, &cmp_vid);

    if (is_vc || is_cv) {
        /* Effective op as if var is on the left. */
        BinOp eff = eb->op;
        if (is_cv) {
            switch (eff) {
            case BIN_LT:  eff = BIN_GT;  break;
            case BIN_LTE: eff = BIN_GTE; break;
            case BIN_GT:  eff = BIN_LT;  break;
            case BIN_GTE: eff = BIN_LTE; break;
            default: break;
            }
        }
        cmp_vid = _resolve(ctx, cmp_vid);
        uint8_t cv_w = ctx->vars[cmp_vid].width;

        if (eff == BIN_EQ) {
            if (ctx->n_vars + 1 >= ctx->n_vars_capacity) return EXPR_NULL;
            uint32_t gid = ctx->n_vars;
            _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
            ctx->n_vars = gid + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
            uint32_t cvc_id = ctx->n_vars;
            _init_tier0(&ctx->vars[cvc_id], cv_w, VAR_AUX, cmp_cv, cmp_cv);
            ctx->n_vars = cvc_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[cvc_id] = EXPR_NULL;
            prop_add_reification_eq_32(ctx, gid, cmp_vid, cvc_id, 0);
            return gid;
        }

        /* Inequalities / negated equality: reify the canonical (var ≤ k)
         * form, then invert with a not-aux when necessary. */
        int64_t k = cmp_cv;
        int invert = 0;
        switch (eff) {
        case BIN_LT:   k = cmp_cv - 1; invert = 0; break;  /* var < c  -> var ≤ c-1 */
        case BIN_LTE:  k = cmp_cv;     invert = 0; break;  /* var ≤ c            */
        case BIN_GT:   k = cmp_cv;     invert = 1; break;  /* var > c  -> ¬(var ≤ c) */
        case BIN_GTE:  k = cmp_cv - 1; invert = 1; break;  /* var ≥ c  -> ¬(var ≤ c-1) */
        case BIN_NEQ: {
            if (ctx->n_vars + 3 >= ctx->n_vars_capacity) return EXPR_NULL;
            uint32_t geq = ctx->n_vars;
            _init_tier0(&ctx->vars[geq], 1, VAR_AUX, 0, 1);
            ctx->n_vars = geq + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[geq] = EXPR_NULL;
            uint32_t cvc_id = ctx->n_vars;
            _init_tier0(&ctx->vars[cvc_id], cv_w, VAR_AUX, cmp_cv, cmp_cv);
            ctx->n_vars = cvc_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[cvc_id] = EXPR_NULL;
            prop_add_reification_eq_32(ctx, geq, cmp_vid, cvc_id, 0);
            /* gid = 1 - geq */
            uint32_t gid = ctx->n_vars;
            _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
            ctx->n_vars = gid + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
            uint32_t one_id = ctx->n_vars;
            _init_tier0(&ctx->vars[one_id], 32, VAR_AUX, 1, 1);
            ctx->n_vars = one_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[one_id] = EXPR_NULL;
            prop_add_bounds_add_32(ctx, one_id, gid, geq, 0);
            return gid;
        }
        default: return EXPR_NULL;
        }

        if (ctx->n_vars + 1 >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t cmp_const = ctx->n_vars;
        _init_tier0(&ctx->vars[cmp_const], cv_w, VAR_AUX, k, k);
        ctx->n_vars = cmp_const + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[cmp_const] = EXPR_NULL;
        uint32_t gle = ctx->n_vars;
        _init_tier0(&ctx->vars[gle], 1, VAR_AUX, 0, 1);
        ctx->n_vars = gle + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[gle] = EXPR_NULL;
        prop_add_reification_32(ctx, gle, cmp_vid, cmp_const, 0);

        if (!invert) return gle;
        if (ctx->n_vars + 1 >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t gid = ctx->n_vars;
        _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
        ctx->n_vars = gid + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
        uint32_t one_id = ctx->n_vars;
        _init_tier0(&ctx->vars[one_id], 32, VAR_AUX, 1, 1);
        ctx->n_vars = one_id + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[one_id] = EXPR_NULL;
        prop_add_bounds_add_32(ctx, one_id, gid, gle, 0);
        return gid;
    }

    /* var-var EQ */
    uint32_t lhs_vid, rhs_vid;
    if (eb->op == BIN_EQ
        && _is_var(sp, eb->lhs, &lhs_vid)
        && _is_var(sp, eb->rhs, &rhs_vid)) {
        if (ctx->n_vars >= ctx->n_vars_capacity) return EXPR_NULL;
        lhs_vid = _resolve(ctx, lhs_vid);
        rhs_vid = _resolve(ctx, rhs_vid);
        uint32_t gid = ctx->n_vars;
        _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
        ctx->n_vars = gid + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
        prop_add_reification_eq_32(ctx, gid, lhs_vid, rhs_vid, 0);
        return gid;
    }

    return EXPR_NULL;
}

/* Recursively materialise an expression as a solver variable.
 * Handles EXPR_VAR, EXPR_CONST, and EXPR_ITE whose condition is
 * a BIN_EQ(var, const) comparison (including deeply nested ITE chains).
 * Returns the var_id on success, EXPR_NULL on failure.
 * `width` hints the bit-width for new const/result vars (0 → 32). */
static uint32_t _value_to_var(SolveCtx *ctx, SolveProblem *sp,
                               ExprRef ref, uint8_t width) {
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);

    if (k == EXPR_VAR) {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        return _resolve(ctx, ev->var_id);
    }
    if (k == EXPR_CONST) {
        ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, ref);
        if (ctx->n_vars >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t cv_id = ctx->n_vars;
        uint8_t w = width ? width : 32;
        _init_tier0(&ctx->vars[cv_id], w, VAR_AUX, ec->value, ec->value);
        ctx->n_vars = cv_id + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[cv_id] = EXPR_NULL;
        return cv_id;
    }
    if (k == EXPR_ITE) {
        ExprITE *ite = (ExprITE *)zsp_pool_ptr(&sp->pool, ref);

        /* Resolve condition to a boolean var ID. */
        uint32_t cond_id = EXPR_NULL;
        uint32_t cmp_vid; int64_t cmp_cv;
        int cond_ok = 0;

        if (_is_var(sp, ite->cond, &cond_id)) {
            cond_id = _resolve(ctx, cond_id);
            cond_ok = 1;
        } else {
            /* Try the general Boolean materialiser first (handles AND/OR/
             * NOT and nested comparisons). Fall back to the legacy
             * single-comparison path below only if that fails. */
            uint32_t b = _bool_to_var(ctx, sp, ite->cond);
            if (b != EXPR_NULL) {
                cond_id = b;
                cond_ok = 1;
            }
            ExprKind ck = *(ExprKind *)zsp_pool_ptr(&sp->pool, ite->cond);
            if (!cond_ok && ck == EXPR_BINARY && ctx->n_vars + 2 <= ctx->n_vars_capacity) {
                ExprBinary *cmp = (ExprBinary *)zsp_pool_ptr(&sp->pool, ite->cond);
                if (cmp->op == BIN_EQ) {
                    int is_vc = _is_var(sp, cmp->lhs, &cmp_vid) &&
                                _is_const(sp, cmp->rhs, &cmp_cv);
                    int is_cv = !is_vc && _is_const(sp, cmp->lhs, &cmp_cv) &&
                                _is_var(sp, cmp->rhs, &cmp_vid);
                    if (is_vc || is_cv) {
                        cmp_vid = _resolve(ctx, cmp_vid);
                        uint8_t cv_w = ctx->vars[cmp_vid].width;
                        /* Guard boolean [0,1] — VAR_AUX: determined by propagation */
                        uint32_t gid = ctx->n_vars;
                        _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
                        ctx->n_vars = gid + 1;
                        if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
                        /* Const-var for comparison value — VAR_AUX singleton */
                        uint32_t cvc_id = ctx->n_vars;
                        _init_tier0(&ctx->vars[cvc_id], cv_w, VAR_AUX, cmp_cv, cmp_cv);
                        ctx->n_vars = cvc_id + 1;
                        if (ctx->watcher_heads) ctx->watcher_heads[cvc_id] = EXPR_NULL;
                        /* guard ↔ (cmp_vid == cmp_cv) */
                        prop_add_reification_eq_32(ctx, gid, cmp_vid, cvc_id, 0);
                        cond_id = gid;
                        cond_ok = 1;
                    } else {
                        /* var-var EQ condition: guard ↔ (lhs_var == rhs_var)
                         * — needed for `select(store(rf, w_addr, w_data), r_addr)`
                         * rewrites which produce an outer ITE with cond
                         * `(= w_addr r_addr)`. */
                        uint32_t lhs_vid, rhs_vid;
                        if (_is_var(sp, cmp->lhs, &lhs_vid) &&
                            _is_var(sp, cmp->rhs, &rhs_vid) &&
                            ctx->n_vars + 1 <= ctx->n_vars_capacity) {
                            lhs_vid = _resolve(ctx, lhs_vid);
                            rhs_vid = _resolve(ctx, rhs_vid);
                            uint32_t gid = ctx->n_vars;
                            _init_tier0(&ctx->vars[gid], 1, VAR_AUX, 0, 1);
                            ctx->n_vars = gid + 1;
                            if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
                            prop_add_reification_eq_32(ctx, gid, lhs_vid, rhs_vid, 0);
                            cond_id = gid;
                            cond_ok = 1;
                        }
                    }
                }
            }
        }
        if (!cond_ok) return EXPR_NULL;

        uint32_t then_id = _value_to_var(ctx, sp, ite->then_e, width);
        if (then_id == EXPR_NULL) return EXPR_NULL;
        uint32_t else_id = _value_to_var(ctx, sp, ite->else_e, width);
        if (else_id == EXPR_NULL) return EXPR_NULL;

        /* Create result var for this ITE — VAR_AUX: determined by propagation */
        if (ctx->n_vars >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t r_id = ctx->n_vars;
        uint8_t w = width ? width : 32;
        int64_t max_val = (w >= 64) ? (int64_t)UINT64_MAX
                        : (int64_t)((1ULL << w) - 1);
        _init_tier0(&ctx->vars[r_id], w, VAR_AUX, 0, max_val);
        ctx->n_vars = r_id + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[r_id] = EXPR_NULL;
        prop_add_ite_value_64(ctx, r_id, cond_id, then_id, else_id, 0);
        return r_id;
    }
    if (k == EXPR_EXTRACT) {
        ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ref);
        uint8_t out_w = (uint8_t)(ex->hi_bit - ex->lo_bit + 1);
        uint32_t src_id = _value_to_var(ctx, sp, ex->operand, 0);
        if (src_id == EXPR_NULL) return EXPR_NULL;
        if (ctx->n_vars >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t r_id = ctx->n_vars;
        int64_t max_val = (out_w >= 64) ? (int64_t)UINT64_MAX
                                        : (int64_t)((1ULL << out_w) - 1);
        _init_tier0(&ctx->vars[r_id], out_w, VAR_AUX, 0, max_val);
        ctx->n_vars = r_id + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[r_id] = EXPR_NULL;
        if (_var_needs_wide(ctx, r_id) || _var_needs_wide(ctx, src_id)) {
            prop_add_bit_slice_64(ctx, r_id, src_id, ex->hi_bit, ex->lo_bit, 0);
        } else {
            prop_add_bit_slice_32(ctx, r_id, src_id, ex->hi_bit, ex->lo_bit, 0);
        }
        return r_id;
    }
    if (k == EXPR_EXTEND) {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        uint32_t src_id = _value_to_var(ctx, sp, ee->operand, ee->from_bits);
        if (src_id == EXPR_NULL) return EXPR_NULL;
        if (ctx->n_vars >= ctx->n_vars_capacity) return EXPR_NULL;
        uint32_t r_id = ctx->n_vars;
        int64_t max_val = (ee->to_bits >= 64) ? (int64_t)UINT64_MAX
                                              : (int64_t)((1ULL << ee->to_bits) - 1);
        int64_t min_val = ee->sign_extend ? -((int64_t)1 << (ee->from_bits - 1)) : 0;
        if (ee->sign_extend) {
            max_val = ((int64_t)1 << (ee->from_bits - 1)) - 1;
        }
        _init_tier0(&ctx->vars[r_id], ee->to_bits, VAR_AUX, min_val, max_val);
        ctx->n_vars = r_id + 1;
        if (ctx->watcher_heads) ctx->watcher_heads[r_id] = EXPR_NULL;
        /* Zero-extend: r's value == src's value (since src ∈ [0, 2^from-1]).
         * Sign-extend: r's value == src's signed value (same int64 repr). */
        int wide = _var_needs_wide(ctx, r_id) || _var_needs_wide(ctx, src_id);
        if (wide) prop_add_bounds_eq_64(ctx, r_id, src_id, 0);
        else      prop_add_bounds_eq_32(ctx, r_id, src_id, 0);
        return r_id;
    }
    return EXPR_NULL;
}

/* Compile "the negation of root" as a constraint.  Used by the top-level
 * (not (or ...)) De Morgan rewrite and by recursive negation pushdown.
 * Returns 1 if compiled, 0 if could not be handled, -1 if UNSAT. */
static int _compile_neg_constraint(SolveCtx *ctx, SolveProblem *sp, ExprRef root) {
    if (root == EXPR_NULL) return -1;   /* (not nothing) = false = unsat */

    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, root);
    if (k == EXPR_CONST) {
        ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, root);
        return (ec->value == 0) ? 1 : -1;
    }
    if (k == EXPR_VAR) {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, root);
        uint32_t vid = _resolve(ctx, ev->var_id);
        if (ctx_tighten_ub64(ctx, vid, 0) == PROP_CONFLICT) return -1;
        return 1;
    }
    if (k == EXPR_UNARY) {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, root);
        if (eu->op == UN_NOT) {
            /* (not (not X)) -> X */
            return _compile_constraint(ctx, sp, eu->operand);
        }
        return 0;
    }
    if (k == EXPR_BINARY) {
        ExprBinary *e = (ExprBinary *)zsp_pool_ptr(&sp->pool, root);
        /* not-or: De Morgan to AND of negs. */
        if (e->op == BIN_OR) {
            int r1 = _compile_neg_constraint(ctx, sp, e->lhs);
            if (r1 < 0) return r1;
            int r2 = _compile_neg_constraint(ctx, sp, e->rhs);
            if (r2 < 0) return r2;
            return (r1 == 0 || r2 == 0) ? 0 : 1;
        }
        /* not-and: would need DisjClause of negated leaves; punt for now. */
        if (e->op == BIN_AND) return 0;
        /* not-cmp: flip operator and use the standard var-const path. */
        uint32_t neg_op;
        int is_cmp = 1;
        switch (e->op) {
        case BIN_EQ:  neg_op = BIN_NEQ; break;
        case BIN_NEQ: neg_op = BIN_EQ;  break;
        case BIN_LT:  neg_op = BIN_GTE; break;
        case BIN_LTE: neg_op = BIN_GT;  break;
        case BIN_GT:  neg_op = BIN_LTE; break;
        case BIN_GTE: neg_op = BIN_LT;  break;
        default: is_cmp = 0; neg_op = e->op; break;
        }
        if (is_cmp) {
            uint32_t vid; int64_t cv;
            if (_is_var(sp, e->lhs, &vid) && _is_const(sp, e->rhs, &cv)) {
                vid = _resolve(ctx, vid);
                return _compile_var_const_cmp(ctx, neg_op, vid, cv, 0);
            }
            if (_is_const(sp, e->lhs, &cv) && _is_var(sp, e->rhs, &vid)) {
                vid = _resolve(ctx, vid);
                return _compile_var_const_cmp(ctx, neg_op, vid, cv, 1);
            }
        }
    }
    return 0;
}

/* Returns 1 if the constraint was compiled, 0 if it could not be handled. */
static int _compile_constraint(SolveCtx *ctx, SolveProblem *sp, ExprRef root) {
    if (root == EXPR_NULL) return 1; /* vacuously handled */

    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, root);

    /* (assert CONST): trivially decided at compile time. */
    if (k == EXPR_CONST) {
        ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, root);
        return (ec->value != 0) ? 1 : -1;
    }

    /* (assert (not X)): push the negation down so we can compile the
     * resulting positive constraint (which is the only shape our
     * downstream propagator wiring understands). De Morgan handles
     * not-or / not-and; comparison ops flip; double negation strips. */
    if (k == EXPR_UNARY) {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, root);
        if (eu->op == UN_NOT) {
            ExprRef inner = eu->operand;
            int64_t cv;
            if (_is_const(sp, inner, &cv)) {
                return (cv == 0) ? 1 : -1;
            }
            ExprKind ik = *(ExprKind *)zsp_pool_ptr(&sp->pool, inner);
            if (ik == EXPR_VAR) {
                /* (not var): var must be 0. */
                ExprVar *iv = (ExprVar *)zsp_pool_ptr(&sp->pool, inner);
                uint32_t vid = _resolve(ctx, iv->var_id);
                if (ctx_tighten_ub64(ctx, vid, 0) == PROP_CONFLICT) return -1;
                return 1;
            }
            if (ik == EXPR_UNARY) {
                /* (not (not X)) -> X */
                ExprUnary *iu = (ExprUnary *)zsp_pool_ptr(&sp->pool, inner);
                if (iu->op == UN_NOT) {
                    return _compile_constraint(ctx, sp, iu->operand);
                }
            }
            if (ik == EXPR_BINARY) {
                ExprBinary *ib = (ExprBinary *)zsp_pool_ptr(&sp->pool, inner);
                /* not-or: De Morgan -> AND of negated leaves; recurse via
                 * synthetic (not lhs) and (not rhs) constraints. We have
                 * no convenient way to build new ExprRefs inside compile,
                 * so do it by re-using _compile_constraint with a forged
                 * "negated" sub-tree: easier to inline the recursion.
                 *
                 * (not (or A B))  ==  (and (not A) (not B))
                 *   compile by treating both leaves as negated assertions. */
                if (ib->op == BIN_OR) {
                    int r1 = _compile_neg_constraint(ctx, sp, ib->lhs);
                    if (r1 < 0) return r1;
                    int r2 = _compile_neg_constraint(ctx, sp, ib->rhs);
                    if (r2 < 0) return r2;
                    return (r1 == 0 || r2 == 0) ? 0 : 1;
                }
                /* not-cmp: flip the operator and reuse the existing cmp
                 * compile path. */
                {
                    uint32_t neg_op;
                    int is_cmp = 1;
                    switch (ib->op) {
                    case BIN_EQ:  neg_op = BIN_NEQ; break;
                    case BIN_NEQ: neg_op = BIN_EQ;  break;
                    case BIN_LT:  neg_op = BIN_GTE; break;
                    case BIN_LTE: neg_op = BIN_GT;  break;
                    case BIN_GT:  neg_op = BIN_LTE; break;
                    case BIN_GTE: neg_op = BIN_LT;  break;
                    default: is_cmp = 0; neg_op = ib->op; break;
                    }
                    if (is_cmp) {
                        /* Reuse var/const cmp handling. */
                        uint32_t vid; int64_t cv2;
                        if (_is_var(sp, ib->lhs, &vid) && _is_const(sp, ib->rhs, &cv2)) {
                            vid = _resolve(ctx, vid);
                            int r = _compile_var_const_cmp(ctx, neg_op, vid, cv2, 0);
                            if (r != 0) return r;
                        } else if (_is_const(sp, ib->lhs, &cv2) && _is_var(sp, ib->rhs, &vid)) {
                            vid = _resolve(ctx, vid);
                            int r = _compile_var_const_cmp(ctx, neg_op, vid, cv2, 1);
                            if (r != 0) return r;
                        }
                        /* Fall through if neither shape matched. */
                    }
                }
            }
            /* Unrecognised negation shape: leave it for the constraint to
             * appear as uncompiled (and let the model-validation pass
             * catch any violation post-solve). */
        }
    }

    /* Boolean/BV assertion: (assert var) means var must be non-zero (true). */
    if (k == EXPR_VAR) {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, root);
        uint32_t v_id = _resolve(ctx, ev->var_id);
        if (ctx_tighten_lb64(ctx, v_id, 1) == PROP_CONFLICT) return -1;
        return 1;
    }

    if (k == EXPR_BINARY) {
        ExprBinary *e = (ExprBinary *)zsp_pool_ptr(&sp->pool, root);
        uint32_t lid, rid2;

        /* Constant folding: CONST op CONST → evaluate at compile time */
        {
            int64_t lc, rc;
            if (_is_const(sp, e->lhs, &lc) && _is_const(sp, e->rhs, &rc)) {
                int truth;
                switch (e->op) {
                case BIN_EQ:  truth = (lc == rc); break;
                case BIN_NEQ: truth = (lc != rc); break;
                case BIN_LT:  truth = (lc <  rc); break;
                case BIN_LTE: truth = (lc <= rc); break;
                case BIN_GT:  truth = (lc >  rc); break;
                case BIN_GTE: truth = (lc >= rc); break;
                case BIN_AND: truth = (lc != 0 && rc != 0); break;
                case BIN_OR:  truth = (lc != 0 || rc != 0); break;
                default: truth = 1; break;
                }
                return truth ? 1 : -1;
            }
        }

        /* AND constraint: both sides must hold — compile each separately */
        if (e->op == BIN_AND) {
            int r1 = _compile_constraint(ctx, sp, e->lhs);
            if (r1 < 0) return r1;
            int r2 = _compile_constraint(ctx, sp, e->rhs);
            if (r2 < 0) return r2;
            return (r1 == 0 || r2 == 0) ? 0 : 1;
        }

        /* OR tree of comparisons → DisjClause propagator (with
         * compile-time folding of always-true/always-false leaves and
         * stripping of `not`/`zero_extend` wrappers). */
        if (e->op == BIN_OR) {
            OrClause clauses[MAX_OR_CLAUSES];
            int any_true = 0;
            int n = _flatten_or(sp, root, clauses, MAX_OR_CLAUSES, &any_true);
            if (any_true) {
                /* OR is entailed — no propagator needed. */
                return 1;
            }
            if (n == 0) {
                /* Every leaf folded to false: OR is unsatisfiable. */
                return -1;
            }
            if (n == 1) {
                /* Single surviving clause: enforce it directly. */
                OrClause *c = &clauses[0];
                uint32_t vid = _resolve(ctx, c->var_id);
                if (c->rhs_var_id == UINT32_MAX) {
                    int rc = _compile_var_const_cmp(ctx, c->op, vid,
                                                     c->constant, 0);
                    if (rc != 0) return rc;
                }
                /* fall through to disj_clause for var-var single-clause */
            }
            if (n >= 1 && n <= (int)MAX_OR_CLAUSES) {
                uint32_t vids[MAX_OR_CLAUSES];
                uint32_t ops[MAX_OR_CLAUSES];
                int64_t  cvs[MAX_OR_CLAUSES];
                uint32_t rvids[MAX_OR_CLAUSES];
                for (int i = 0; i < n; i++) {
                    vids[i] = _resolve(ctx, clauses[i].var_id);
                    ops[i]  = clauses[i].op;
                    cvs[i]  = clauses[i].constant;
                    rvids[i] = (clauses[i].rhs_var_id == UINT32_MAX)
                               ? UINT32_MAX
                               : _resolve(ctx, clauses[i].rhs_var_id);
                }
                uint32_t ref = prop_add_disj_clause(ctx, (uint32_t)n,
                                                     vids, ops, cvs, 0,
                                                     rvids);
                return (ref != EXPR_NULL) ? 1 : 0;
            }
            /* Fallback: convert the whole OR tree to a Boolean guard via
             * _bool_to_var and tighten the guard to 1. This handles cases
             * the OrClause flattener can't (e.g. leaves are themselves OR
             * trees wrapped in `not`, or AND-of-comparisons leaves). */
            {
                uint32_t g = _bool_to_var(ctx, sp, root);
                if (g != EXPR_NULL) {
                    if (ctx_tighten_lb64(ctx, g, 1) == PROP_CONFLICT) return -1;
                    return 1;
                }
            }
            return 0;
        }

        /* Binary comparison: var op var */
        if (_is_var(sp, e->lhs, &lid) && _is_var(sp, e->rhs, &rid2)) {
            /* Resolve through alias table */
            lid = _resolve(ctx, lid);
            rid2 = _resolve(ctx, rid2);
            /* If aliased to same root: EQ is trivially true, NEQ is UNSAT */
            if (lid == rid2 && e->op == BIN_EQ)  return 1;
            if (lid == rid2 && e->op == BIN_NEQ) return -1;
            /* Use 64-bit propagators if either variable is promoted to tier-1+ */
            int wide = _var_needs_wide(ctx, lid) || _var_needs_wide(ctx, rid2);
            uint16_t w = wide ? 64 : (ctx->vars[lid].width > ctx->vars[rid2].width
                         ? ctx->vars[lid].width : ctx->vars[rid2].width);
            if (w <= 32) {
                switch (e->op) {
                case BIN_LTE: prop_add_bounds_le_32(ctx, lid, rid2, 0); return 1;
                case BIN_LT:  prop_add_bounds_lt_32(ctx, lid, rid2, 0); return 1;
                case BIN_EQ:  prop_add_bounds_eq_32(ctx, lid, rid2, 0); return 1;
                case BIN_NEQ: prop_add_bounds_ne_32(ctx, lid, rid2, 0); return 1;
                case BIN_GT:  prop_add_bounds_lt_32(ctx, rid2, lid, 0); return 1;
                case BIN_GTE: prop_add_bounds_le_32(ctx, rid2, lid, 0); return 1;
                default: break;
                }
            } else {
                switch (e->op) {
                case BIN_LTE: prop_add_bounds_le_64(ctx, lid, rid2, 0); return 1;
                case BIN_LT:  prop_add_bounds_lt_64(ctx, lid, rid2, 0); return 1;
                case BIN_EQ:  prop_add_bounds_eq_64(ctx, lid, rid2, 0); return 1;
                case BIN_NEQ: prop_add_bounds_ne_64(ctx, lid, rid2, 0); return 1;
                case BIN_GT:  prop_add_bounds_lt_64(ctx, rid2, lid, 0); return 1;
                case BIN_GTE: prop_add_bounds_le_64(ctx, rid2, lid, 0); return 1;
                default: break;
                }
            }
        }

        /* Binary comparison: var op const  or  const op var */
        {
            uint32_t vid2; int64_t cv2;
            if (_is_var(sp, e->lhs, &vid2) && _is_const(sp, e->rhs, &cv2)) {
                vid2 = _resolve(ctx, vid2);
                int r = _compile_var_const_cmp(ctx, e->op, vid2, cv2, 0);
                if (r != 0) return r;  /* 1 = compiled, -1 = UNSAT */
            } else if (_is_const(sp, e->lhs, &cv2) && _is_var(sp, e->rhs, &vid2)) {
                vid2 = _resolve(ctx, vid2);
                int r = _compile_var_const_cmp(ctx, e->op, vid2, cv2, 1);
                if (r != 0) return r;
            }
        }

        /* var != const: create const-var and NE propagator */
        {
            uint32_t vid3; int64_t cv3;
            if (e->op == BIN_NEQ) {
                int is_vc3 = _is_var(sp, e->lhs, &vid3) && _is_const(sp, e->rhs, &cv3);
                int is_cv3 = !is_vc3 && _is_const(sp, e->lhs, &cv3) && _is_var(sp, e->rhs, &vid3);
                if (is_vc3 || is_cv3) {
                    vid3 = _resolve(ctx, vid3);
                    if (ctx->n_vars < ctx->n_vars_capacity) {
                        uint32_t cv_id = ctx->n_vars;
                        Variable *cvv = &ctx->vars[cv_id];
                        _init_tier0(cvv, 32, 0, cv3, cv3);
                        ctx->n_vars = cv_id + 1;
                        if (ctx->watcher_heads) ctx->watcher_heads[cv_id] = EXPR_NULL;
                        prop_add_bounds_ne_32(ctx, vid3, cv_id, 0);
                        return 1;
                    }
                }
            }
        }

        /* r = a op b  (EQ with RHS binary expression) */
        /* r = a op b  (EQ with one side a var, other side a binary expr)
         * Handles both  var == BinOp(a, b)  and  BinOp(a, b) == var/const
         * Also handles const-var operand order: var == const * var */
        if (e->op == BIN_EQ) {
            ExprRef var_side = EXPR_NULL, expr_side = EXPR_NULL;
            /* Determine which side is the "result" var and which is the expr */
            if (e->lhs != EXPR_NULL && e->rhs != EXPR_NULL) {
                ExprKind lk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->lhs);
                ExprKind rk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->rhs);
                if (lk == EXPR_VAR && rk == EXPR_BINARY) {
                    var_side = e->lhs; expr_side = e->rhs;
                } else if (lk == EXPR_BINARY && rk == EXPR_VAR) {
                    var_side = e->rhs; expr_side = e->lhs;
                } else if (lk == EXPR_BINARY && rk == EXPR_CONST) {
                    /* BinOp(...) == const: create a temp variable pinned to
                     * the constant, then directly compile BinOp with that
                     * temp as the result variable. No ExprRef allocation in
                     * sp needed (pool may be full after finalization). */
                    ExprConst *rhs_c = (ExprConst *)zsp_pool_ptr(&sp->pool, e->rhs);
                    int64_t pin_val = rhs_c->value;
                    if (ctx->n_vars < ctx->n_vars_capacity) {
                        uint32_t r_id = ctx->n_vars;
                        Variable *rv = &ctx->vars[r_id];
                        _init_tier0(rv, 32, 0, pin_val, pin_val);
                        ctx->n_vars = r_id + 1;
                        if (ctx->watcher_heads)
                            ctx->watcher_heads[r_id] = EXPR_NULL;
                        /* Directly compile: r_id == BinExpr(lhs) */
                        return _compile_binexpr_eq_var(ctx, sp, e->lhs, r_id);
                    }
                } else if (lk == EXPR_CONST && rk == EXPR_BINARY) {
                    /* const == BinOp(...): symmetric */
                    ExprConst *lhs_c = (ExprConst *)zsp_pool_ptr(&sp->pool, e->lhs);
                    int64_t pin_val = lhs_c->value;
                    if (ctx->n_vars < ctx->n_vars_capacity) {
                        uint32_t r_id = ctx->n_vars;
                        Variable *rv = &ctx->vars[r_id];
                        _init_tier0(rv, 32, 0, pin_val, pin_val);
                        ctx->n_vars = r_id + 1;
                        if (ctx->watcher_heads)
                            ctx->watcher_heads[r_id] = EXPR_NULL;
                        return _compile_binexpr_eq_var(ctx, sp, e->rhs, r_id);
                    }
                }
            }
            /* Also handle: var == EXPR_UNARY(op, var) */
            if (var_side == EXPR_NULL && e->lhs != EXPR_NULL && e->rhs != EXPR_NULL) {
                ExprKind lk2 = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->lhs);
                ExprKind rk2 = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->rhs);
                if (lk2 == EXPR_VAR && rk2 == EXPR_UNARY) {
                    ExprVar *ev2 = (ExprVar *)zsp_pool_ptr(&sp->pool, e->lhs);
                    ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, e->rhs);
                    uint32_t r2 = _resolve(ctx, ev2->var_id);
                    uint32_t a2;
                    if (_is_var(sp, eu->operand, &a2)) {
                        a2 = _resolve(ctx, a2);
                        switch (eu->op) {
                        case UN_NEG:
                            prop_add_unary_neg_32(ctx, r2, a2, 0); return 1;
                        case UN_INVERT:
                            prop_add_bounds_bnot_64(ctx, r2, a2, 0); return 1;
                        default: break;
                        }
                    }
                } else if (lk2 == EXPR_UNARY && rk2 == EXPR_VAR) {
                    ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, e->lhs);
                    ExprVar *ev2 = (ExprVar *)zsp_pool_ptr(&sp->pool, e->rhs);
                    uint32_t r2 = _resolve(ctx, ev2->var_id);
                    uint32_t a2;
                    if (_is_var(sp, eu->operand, &a2)) {
                        a2 = _resolve(ctx, a2);
                        switch (eu->op) {
                        case UN_NEG:
                            prop_add_unary_neg_32(ctx, r2, a2, 0); return 1;
                        case UN_INVERT:
                            prop_add_bounds_bnot_64(ctx, r2, a2, 0); return 1;
                        default: break;
                        }
                    }
                }
            }
            if (var_side != EXPR_NULL && expr_side != EXPR_NULL) {
                ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, var_side);
                uint32_t r_id = _resolve(ctx, ev->var_id);
                /* Delegate to _compile_binexpr_eq_var first — handles reified
                 * comparisons (r ↔ a op b) before falling through to arithmetic. */
                {
                    int rc_d = _compile_binexpr_eq_var(ctx, sp, expr_side, r_id);
                    if (rc_d != 0) return rc_d;
                }
                ExprBinary *binop = (ExprBinary *)zsp_pool_ptr(&sp->pool, expr_side);
                uint32_t a_id, b_id;
                int64_t cv;
                int has_var_var = _is_var(sp, binop->lhs, &a_id) && _is_var(sp, binop->rhs, &b_id);
                int has_const_var = _is_const(sp, binop->lhs, &cv) && _is_var(sp, binop->rhs, &b_id);
                int has_var_const = _is_var(sp, binop->lhs, &a_id) && _is_const(sp, binop->rhs, &cv);
                if (has_var_var || has_const_var || has_var_const) {
                    /* For var-const or const-var: create a compile-time
                     * const-variable (domain = [cv, cv]) so the propagator
                     * sees two var IDs. */
                    if (has_var_const && !has_var_var) {
                        /* a_id is set, cv is the constant on the right.
                         * Create a const-var for cv. */
                        if (ctx->n_vars < ctx->n_vars_capacity) {
                            b_id = ctx->n_vars;
                            Variable *cv_var = &ctx->vars[b_id];
                            _init_tier0(cv_var, 32, 0, cv, cv);
                            ctx->n_vars = b_id + 1;
                            if (ctx->watcher_heads)
                                ctx->watcher_heads[b_id] = EXPR_NULL;
                            has_var_var = 1;
                        }
                    }
                    if (has_const_var && !has_var_var) {
                        /* cv is the constant on the left, b_id is the var on the right. */
                        if (ctx->n_vars < ctx->n_vars_capacity) {
                            a_id = ctx->n_vars;
                            Variable *cv_var = &ctx->vars[a_id];
                            _init_tier0(cv_var, 32, 0, cv, cv);
                            ctx->n_vars = a_id + 1;
                            if (ctx->watcher_heads)
                                ctx->watcher_heads[a_id] = EXPR_NULL;
                            has_var_var = 1;
                        }
                    }
                    if (has_var_var) {
                        /* Use 64-bit propagators if any operand is promoted */
                        int wide = _var_needs_wide(ctx, r_id) ||
                                   _var_needs_wide(ctx, a_id) ||
                                   _var_needs_wide(ctx, b_id);
                        uint16_t w = wide ? 64 : ctx->vars[r_id].width;
                        if (w <= 32) {
                            switch (binop->op) {
                            case BIN_ADD: prop_add_bounds_add_32(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_SUB: prop_add_bounds_add_32(ctx, a_id, r_id, b_id, 0); return 1;
                            case BIN_MUL: prop_add_bounds_mul_32(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_DIV: prop_add_bounds_div_32(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_MOD: prop_add_bounds_mod_32(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_BAND: prop_add_bounds_band_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_BOR:  prop_add_bounds_bor_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_BXOR: prop_add_bounds_bxor_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_LSHIFT: prop_add_bounds_shl_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_RSHIFT: prop_add_bounds_lshr_64(ctx, r_id, a_id, b_id, 0); return 1;
                            default: break;
                            }
                        } else {
                            switch (binop->op) {
                            case BIN_ADD: prop_add_bounds_add_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_SUB: prop_add_bounds_add_64(ctx, a_id, r_id, b_id, 0); return 1;
                            case BIN_MUL: prop_add_bounds_mul_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_DIV: prop_add_bounds_div_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_MOD: prop_add_bounds_mod_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_BAND: prop_add_bounds_band_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_BOR:  prop_add_bounds_bor_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_BXOR: prop_add_bounds_bxor_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_LSHIFT: prop_add_bounds_shl_64(ctx, r_id, a_id, b_id, 0); return 1;
                            case BIN_RSHIFT: prop_add_bounds_lshr_64(ctx, r_id, a_id, b_id, 0); return 1;
                            default: break;
                            }
                        }
                    }
                }
            }
        }

        /* BinOp(var, var) op const  — handle the pattern where an arithmetic
         * expression is compared to a constant (e.g. s012345 + p6 == 200) */
        {
            int64_t cv;
            ExprRef expr_side = EXPR_NULL;
            BinOp cmp_op = e->op;
            int flipped = 0;
            if (e->lhs != EXPR_NULL && _is_const(sp, e->rhs, &cv)) {
                ExprKind lk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->lhs);
                if (lk == EXPR_BINARY) expr_side = e->lhs;
            } else if (e->rhs != EXPR_NULL && _is_const(sp, e->lhs, &cv)) {
                ExprKind rk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->rhs);
                if (rk == EXPR_BINARY) { expr_side = e->rhs; flipped = 1; }
            }
            if (expr_side != EXPR_NULL) {
                ExprBinary *binop = (ExprBinary *)zsp_pool_ptr(&sp->pool, expr_side);
                uint32_t a_id, b_id;
                if (_is_var(sp, binop->lhs, &a_id) && _is_var(sp, binop->rhs, &b_id)) {
                    if (binop->op == BIN_ADD && (cmp_op == BIN_EQ ||
                        cmp_op == BIN_LT || cmp_op == BIN_LTE ||
                        cmp_op == BIN_GT || cmp_op == BIN_GTE)) {
                        /* (a + b) cmp cv: tighten bounds on a and b */
                        /* EQ: a+b == cv => a in [cv-b_hi, cv-b_lo], b in [cv-a_hi, cv-a_lo] */
                        BinOp eff = cmp_op;
                        if (flipped) {
                            switch (cmp_op) {
                            case BIN_LT:  eff = BIN_GT;  break;
                            case BIN_LTE: eff = BIN_GTE; break;
                            case BIN_GT:  eff = BIN_LT;  break;
                            case BIN_GTE: eff = BIN_LTE; break;
                            default: break;
                            }
                        }
                        /* Create a temporary variable for the sum, set its bounds
                         * from the comparison, and add an ADD propagator. */
                        /* For now, use the IR translator to handle this by
                         * introducing a temp var. See below. */
                    }
                }
            }
        }
    }
    /* ---- EXPR_ITE at constraint root ---- */
    if (k == EXPR_ITE) {
        ExprITE *ite = (ExprITE *)zsp_pool_ptr(&sp->pool, root);

        /* Check if this is an ITE-as-value inside an EQ: handled above.
         * Here we handle ITE as a constraint: if(cond) then else else.
         * Both branches are constraint expressions. */

        /* Determine if cond is a variable or expression */
        uint32_t cond_var_id;
        int cond_is_var = _is_var(sp, ite->cond, &cond_var_id);

        if (!cond_is_var) {
            /* Cond is a constant or expression -- check if it's a constant */
            int64_t cond_val;
            if (_is_const(sp, ite->cond, &cond_val)) {
                /* Static condition: compile only the active branch */
                if (cond_val != 0)
                    return _compile_constraint(ctx, sp, ite->then_e);
                else
                    return _compile_constraint(ctx, sp, ite->else_e);
            }
            /* Cond is a comparison expression: ITE(var op const, then, else).
             * Handle pattern: if (var == const) { then_constraint }.
             * Create a boolean guard linked to the comparison via
             * Reification, and gate the then-branch with this guard. */
            ExprKind ck = *(ExprKind *)zsp_pool_ptr(&sp->pool, ite->cond);
            if (ck == EXPR_BINARY) {
                ExprBinary *cmp = (ExprBinary *)zsp_pool_ptr(&sp->pool, ite->cond);
                uint32_t cmp_vid; int64_t cmp_cv;
                int is_vc = _is_var(sp, cmp->lhs, &cmp_vid) && _is_const(sp, cmp->rhs, &cmp_cv);
                int is_cv = !is_vc && _is_const(sp, cmp->lhs, &cmp_cv) && _is_var(sp, cmp->rhs, &cmp_vid);

                if ((is_vc || is_cv) && cmp->op == BIN_EQ &&
                    ctx->n_vars < ctx->n_vars_capacity) {
                    /* Create guard boolean [0,1] and const-var for cmp_cv */
                    uint32_t gid = ctx->n_vars;
                    Variable *gv = &ctx->vars[gid];
                    gv->lo = 0; gv->hi = 1;
                    gv->width = 1; gv->flags = 0;
                    gv->holes_offset = 0; gv->_pad = 0;
                    ctx->n_vars = gid + 1;
                    if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
                    if (gid < 64) ctx->unassigned_mask |= (1ULL << gid);

                    /* Bidirectional: guard ↔ (cmp_vid == cmp_cv)
                     * Uses ReificationEq with a const-var pinned to cmp_cv. */
                    if (ctx->n_vars < ctx->n_vars_capacity) {
                        uint32_t cv_id = ctx->n_vars;
                        Variable *cvv = &ctx->vars[cv_id];
                        _init_tier0(cvv, 32, 0, cmp_cv, cmp_cv);
                        ctx->n_vars = cv_id + 1;
                        if (ctx->watcher_heads) ctx->watcher_heads[cv_id] = EXPR_NULL;
                        prop_add_reification_eq_32(ctx, gid, cmp_vid, cv_id, 0);
                    }

                    /* Compile then-branch with guard (uses gated helper
                     * to avoid irreversible compile-time tightening) */
                    int then_rc = _compile_gated_constraint(ctx, sp,
                                                            ite->then_e, gid);
                    if (then_rc < 0) return then_rc;

                    /* Else-branch: compile with not_guard */
                    int64_t else_cv2;
                    if (ite->else_e == EXPR_NULL ||
                        (_is_const(sp, ite->else_e, &else_cv2) && else_cv2 != 0)) {
                        /* No else or trivially true else -- done */
                    } else if (ctx->n_vars + 2 <= ctx->n_vars_capacity) {
                        /* Create not_guard: not_guard + guard == 1 */
                        uint32_t ng_id = ctx->n_vars;
                        Variable *ngv = &ctx->vars[ng_id];
                        ngv->lo = 0; ngv->hi = 1;
                        ngv->width = 1; ngv->flags = 0;
                        ngv->holes_offset = 0; ngv->_pad = 0;
                        ctx->n_vars = ng_id + 1;
                        if (ctx->watcher_heads) ctx->watcher_heads[ng_id] = EXPR_NULL;
                        if (ng_id < 64) ctx->unassigned_mask |= (1ULL << ng_id);

                        uint32_t one_id2 = ctx->n_vars;
                        Variable *ov2 = &ctx->vars[one_id2];
                        _init_tier0(ov2, 32, 0, 1, 1);
                        ctx->n_vars = one_id2 + 1;
                        if (ctx->watcher_heads) ctx->watcher_heads[one_id2] = EXPR_NULL;
                        prop_add_bounds_add_32(ctx, one_id2, gid, ng_id, 0);

                        _compile_gated_constraint(ctx, sp, ite->else_e, ng_id);
                    }

                    return (then_rc > 0) ? 1 : 0;
                }

                /* var == var condition: guard <-> (lhs_var == rhs_var) */
                uint32_t lhs_vid, rhs_vid;
                int is_vv = _is_var(sp, cmp->lhs, &lhs_vid) &&
                            _is_var(sp, cmp->rhs, &rhs_vid);
                if (is_vv && cmp->op == BIN_EQ &&
                    ctx->n_vars < ctx->n_vars_capacity) {
                    uint32_t gid = ctx->n_vars;
                    Variable *gv = &ctx->vars[gid];
                    gv->lo = 0; gv->hi = 1;
                    gv->width = 1; gv->flags = 0;
                    gv->holes_offset = 0; gv->_pad = 0;
                    ctx->n_vars = gid + 1;
                    if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
                    if (gid < 64) ctx->unassigned_mask |= (1ULL << gid);

                    /* guard <-> (lhs == rhs) */
                    prop_add_reification_eq_32(ctx, gid, lhs_vid, rhs_vid, 0);

                    /* Compile then-branch with guard (gated helper) */
                    int then_rc = _compile_gated_constraint(ctx, sp,
                                                            ite->then_e, gid);
                    if (then_rc < 0) return then_rc;

                    /* Handle else-branch */
                    int64_t else_cv3;
                    if (ite->else_e != EXPR_NULL &&
                        !(_is_const(sp, ite->else_e, &else_cv3) && else_cv3 != 0)) {
                        /* Non-trivial else: compile with not_guard */
                        if (ctx->n_vars < ctx->n_vars_capacity) {
                            uint32_t ng_id = ctx->n_vars;
                            Variable *ngv = &ctx->vars[ng_id];
                            ngv->lo = 0; ngv->hi = 1;
                            ngv->width = 1; ngv->flags = 0;
                            ngv->holes_offset = 0; ngv->_pad = 0;
                            ctx->n_vars = ng_id + 1;
                            if (ctx->watcher_heads) ctx->watcher_heads[ng_id] = EXPR_NULL;
                            if (ng_id < 64) ctx->unassigned_mask |= (1ULL << ng_id);

                            /* not_guard == 1 - guard: use a const-1 var and
                             * the ADD propagator: guard + not_guard == 1 */
                            if (ctx->n_vars < ctx->n_vars_capacity) {
                                uint32_t one_id = ctx->n_vars;
                                Variable *ov = &ctx->vars[one_id];
                                _init_tier0(ov, 32, 0, 1, 1);
                                ctx->n_vars = one_id + 1;
                                if (ctx->watcher_heads) ctx->watcher_heads[one_id] = EXPR_NULL;
                                prop_add_bounds_add_32(ctx, one_id, gid, ng_id, 0);
                            }

                            int else_rc = _compile_gated_constraint(ctx, sp,
                                                                ite->else_e, ng_id);
                            (void)else_rc;
                        }
                    }

                    return (then_rc > 0) ? 1 : 0;
                }
            }
            return 0;
        }

        /* Cond is a variable: compile both branches with guard gating.
         * Then-branch fires when cond_var == 1.
         * Else-branch fires when cond_var == 0, which we track with a
         * helper not_cond variable: not_cond = 1 - cond. */

        /* Compile then-branch constraints */
        int then_rc = _compile_constraint(ctx, sp, ite->then_e);
        if (then_rc < 0) return then_rc;

        if (then_rc > 0) {
            /* Then-branch was compiled successfully.
             * The most recently added propagator is for the then-branch.
             * Set its guard to cond_var. */
            if (ctx->n_props > 0) {
                uint32_t last_prop_id = ctx->n_props - 1;
                if (ctx->prop_guard_vars && last_prop_id < ctx->n_prop_refs_capacity)
                    ctx->prop_guard_vars[last_prop_id] = cond_var_id;
            }
        }

        /* Compile else-branch if present */
        if (ite->else_e != EXPR_NULL) {
            int else_rc = _compile_constraint(ctx, sp, ite->else_e);
            if (else_rc < 0) return else_rc;

            if (else_rc > 0 && ctx->n_props > 0) {
                /* Else-branch: create a NOT-cond variable and use as guard.
                 * We need not_cond_var where not_cond = 1 - cond.
                 * For a boolean cond in [0,1], use a NE propagator approach:
                 * Add a temp variable for not_cond, constrain not_cond + cond == 1. */

                /* For simplicity, use a DisjClause-based approach instead:
                 * The else propagator should fire when cond == 0.
                 * We can achieve this by negating: create a variable that is
                 * 1 when cond is 0 and 0 when cond is 1.
                 * Use the Implication approach: set guard to cond_var but
                 * invert the semantics in the guard check.
                 * 
                 * Actually, simpler approach for boolean guard:
                 * Mark the else-propagator's guard with a special encoding.
                 * Use (cond_var_id | 0x80000000) to indicate negated guard.
                 * But that's hacky. Instead, just allocate a not_cond var
                 * and add an equality: not_cond + cond == 1 */

                /* Allocate not_cond as a new variable if we have capacity */
                uint32_t not_cond_id = ctx->n_vars;
                if (not_cond_id < ctx->n_vars_capacity) {
                    Variable *nv = &ctx->vars[not_cond_id];
                    /* Boolean: signed, [0,1] so it stays tier-0 */
                    nv->lo = 0; nv->hi = 1;
                    nv->width = 1; nv->flags = VAR_SIGNED;
                    nv->holes_offset = 0; nv->_pad = 0;
                    ctx->n_vars = not_cond_id + 1;

                    /* Ensure watcher head is initialized */
                    if (ctx->watcher_heads)
                        ctx->watcher_heads[not_cond_id] = EXPR_NULL;

                    /* Set unassigned bit */
                    if (not_cond_id < 64)
                        ctx->unassigned_mask |= (1ULL << not_cond_id);

                    /* Add constraint: not_cond + cond == 1 via add propagator.
                     * We need a temp "one" variable. Simpler: use NE propagator
                     * between cond and not_cond, plus bounds.
                     * Actually simplest: just use the add propagator.
                     * Create a const-1 variable. */
                    uint32_t one_id = ctx->n_vars;
                    if (one_id < ctx->n_vars_capacity) {
                        Variable *ov = &ctx->vars[one_id];
                        ov->lo = 1; ov->hi = 1;
                        ov->width = 1; ov->flags = VAR_SIGNED;
                        ov->holes_offset = 0; ov->_pad = 0;
                        ctx->n_vars = one_id + 1;
                        if (ctx->watcher_heads)
                            ctx->watcher_heads[one_id] = EXPR_NULL;
                        /* one_id is singleton, don't set unassigned bit */

                        /* one == cond + not_cond */
                        prop_add_bounds_add_32(ctx, one_id, cond_var_id,
                                               not_cond_id, 0);
                    }

                    /* Set guard on else-propagator */
                    uint32_t last_prop_id = ctx->n_props - 2;
                    /* Actually we just added the add propagator, so the else
                     * propagator is further back. Track it properly. */
                    /* The else branch compiled a propagator, then we added
                     * the add propagator. The else propagator is at
                     * n_props - 2 (before the add prop we just created). */
                    if (ctx->prop_guard_vars && last_prop_id < ctx->n_prop_refs_capacity)
                        ctx->prop_guard_vars[last_prop_id] = not_cond_id;
                }
            }
        }

        return (then_rc > 0) ? 1 : 0;
    }

    /* ---- r == extend(a): zero/sign extend compilation ---- */
    if (k == EXPR_BINARY) {
        ExprBinary *e_ext = (ExprBinary *)zsp_pool_ptr(&sp->pool, root);
        if (e_ext->op == BIN_EQ && e_ext->lhs != EXPR_NULL && e_ext->rhs != EXPR_NULL) {
            ExprKind ext_lk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e_ext->lhs);
            ExprKind ext_rk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e_ext->rhs);
            ExprRef ext_var_side = EXPR_NULL, ext_ext_side = EXPR_NULL;
            if (ext_lk == EXPR_VAR && ext_rk == EXPR_EXTEND) {
                ext_var_side = e_ext->lhs; ext_ext_side = e_ext->rhs;
            } else if (ext_lk == EXPR_EXTEND && ext_rk == EXPR_VAR) {
                ext_var_side = e_ext->rhs; ext_ext_side = e_ext->lhs;
            }
            if (ext_var_side != EXPR_NULL && ext_ext_side != EXPR_NULL) {
                ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ext_var_side);
                uint32_t r_id = ev->var_id;
                ExprExtend *ext = (ExprExtend *)zsp_pool_ptr(&sp->pool, ext_ext_side);
                uint32_t operand_id;
                if (_is_var(sp, ext->operand, &operand_id)) {
                    if (!ext->sign_extend) {
                        /* Zero-extend: r in [0, (1<<from_bits)-1] */
                        int64_t max_val = (ext->from_bits < 64)
                            ? ((int64_t)1 << ext->from_bits) - 1
                            : INT64_MAX;
                        if (ctx_tighten_lb64(ctx, r_id, 0) == PROP_CONFLICT)
                            return -1;
                        if (ctx_tighten_ub64(ctx, r_id, max_val) == PROP_CONFLICT)
                            return -1;
                        if (ctx_tighten_lb64(ctx, operand_id, 0) == PROP_CONFLICT)
                            return -1;
                        if (ctx_tighten_ub64(ctx, operand_id, max_val) == PROP_CONFLICT)
                            return -1;
                    } else {
                        /* Sign-extend: r in [-2^(from-1), 2^(from-1)-1] */
                        int64_t min_val = -((int64_t)1 << (ext->from_bits - 1));
                        int64_t max_val = ((int64_t)1 << (ext->from_bits - 1)) - 1;
                        if (ctx_tighten_lb64(ctx, r_id, min_val) == PROP_CONFLICT)
                            return -1;
                        if (ctx_tighten_ub64(ctx, r_id, max_val) == PROP_CONFLICT)
                            return -1;
                        if (ctx_tighten_lb64(ctx, operand_id, min_val) == PROP_CONFLICT)
                            return -1;
                        if (ctx_tighten_ub64(ctx, operand_id, max_val) == PROP_CONFLICT)
                            return -1;
                    }
                    /* Link r and operand via EQ propagator */
                    prop_add_bounds_eq_64(ctx, r_id, operand_id, 0);
                    return 1;
                }
            }
        }
    }

    /* ---- r == extract(a, hi, lo): bit-slice compilation ---- */
    if (k == EXPR_BINARY) {
        ExprBinary *e_ex = (ExprBinary *)zsp_pool_ptr(&sp->pool, root);
        if (e_ex->op == BIN_EQ && e_ex->lhs != EXPR_NULL && e_ex->rhs != EXPR_NULL) {
            ExprKind ex_lk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e_ex->lhs);
            ExprKind ex_rk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e_ex->rhs);
            ExprRef var_side = EXPR_NULL, ex_side = EXPR_NULL;
            if (ex_lk == EXPR_VAR && ex_rk == EXPR_EXTRACT) {
                var_side = e_ex->lhs; ex_side = e_ex->rhs;
            } else if (ex_lk == EXPR_EXTRACT && ex_rk == EXPR_VAR) {
                var_side = e_ex->rhs; ex_side = e_ex->lhs;
            }
            if (var_side != EXPR_NULL && ex_side != EXPR_NULL) {
                ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, var_side);
                uint32_t r_id = _resolve(ctx, ev->var_id);
                ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ex_side);
                uint32_t operand_id;
                if (_is_var(sp, ex->operand, &operand_id)) {
                    operand_id = _resolve(ctx, operand_id);
                    if (_var_needs_wide(ctx, r_id) || _var_needs_wide(ctx, operand_id)) {
                        prop_add_bit_slice_64(ctx, r_id, operand_id,
                                              ex->hi_bit, ex->lo_bit, 0);
                    } else {
                        prop_add_bit_slice_32(ctx, r_id, operand_id,
                                              ex->hi_bit, ex->lo_bit, 0);
                    }
                    return 1;
                }
            }
        }
    }


    /* ---- r == concat(hi, lo): bit concatenation ---- */
    if (k == EXPR_BINARY) {
        ExprBinary *e_cat = (ExprBinary *)zsp_pool_ptr(&sp->pool, root);
        if (e_cat->op == BIN_EQ && e_cat->lhs != EXPR_NULL && e_cat->rhs != EXPR_NULL) {
            ExprKind cat_lk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e_cat->lhs);
            ExprKind cat_rk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e_cat->rhs);
            ExprRef cat_var_side = EXPR_NULL, cat_cat_side = EXPR_NULL;
            if (cat_lk == EXPR_VAR && cat_rk == EXPR_CONCAT) {
                cat_var_side = e_cat->lhs; cat_cat_side = e_cat->rhs;
            } else if (cat_lk == EXPR_CONCAT && cat_rk == EXPR_VAR) {
                cat_var_side = e_cat->rhs; cat_cat_side = e_cat->lhs;
            }
            if (cat_var_side != EXPR_NULL && cat_cat_side != EXPR_NULL) {
                ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, cat_var_side);
                uint32_t r_id = ev->var_id;
                ExprConcat *cat = (ExprConcat *)zsp_pool_ptr(&sp->pool, cat_cat_side);
                uint32_t hi_id, lo_id;
                if (_is_var(sp, cat->hi, &hi_id) && _is_var(sp, cat->lo, &lo_id)) {
                    prop_add_bounds_concat_64(ctx, r_id, hi_id, lo_id,
                                              cat->lo_width, 0);
                    return 1;
                }
            }
        }
    }

    /* ---- EXPR_ITE as value inside EQ: r == (cond ? a : b) ---- */
    if (k == EXPR_BINARY) {
        ExprBinary *e = (ExprBinary *)zsp_pool_ptr(&sp->pool, root);
        if (e->op == BIN_EQ) {
            /* Check for var == ITE pattern */
            ExprRef var_side = EXPR_NULL, ite_side = EXPR_NULL;
            if (e->lhs != EXPR_NULL && e->rhs != EXPR_NULL) {
                ExprKind lk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->lhs);
                ExprKind rk = *(ExprKind *)zsp_pool_ptr(&sp->pool, e->rhs);
                if (lk == EXPR_VAR && rk == EXPR_ITE) {
                    var_side = e->lhs; ite_side = e->rhs;
                } else if (lk == EXPR_ITE && rk == EXPR_VAR) {
                    var_side = e->rhs; ite_side = e->lhs;
                }
            }
            if (var_side != EXPR_NULL && ite_side != EXPR_NULL) {
                ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, var_side);
                uint32_t r_id = _resolve(ctx, ev->var_id);
                ExprITE *ite = (ExprITE *)zsp_pool_ptr(&sp->pool, ite_side);

                uint32_t cond_id, a_id, b_id;
                int64_t then_cv, else_cv;
                int cond_ok = _is_var(sp, ite->cond, &cond_id);
                int then_ok = _is_var(sp, ite->then_e, &a_id);
                int else_ok = _is_var(sp, ite->else_e, &b_id);
                if (cond_ok) cond_id = _resolve(ctx, cond_id);
                if (then_ok) a_id    = _resolve(ctx, a_id);
                if (else_ok) b_id    = _resolve(ctx, b_id);

                /* Promote const then/else branches to const-vars */
                if (cond_ok && !then_ok && _is_const(sp, ite->then_e, &then_cv) &&
                    ctx->n_vars < ctx->n_vars_capacity) {
                    a_id = ctx->n_vars;
                    _init_tier0(&ctx->vars[a_id], 32, 0, then_cv, then_cv);
                    ctx->n_vars = a_id + 1;
                    if (ctx->watcher_heads) ctx->watcher_heads[a_id] = EXPR_NULL;
                    then_ok = 1;
                }
                if (cond_ok && !else_ok && _is_const(sp, ite->else_e, &else_cv) &&
                    ctx->n_vars < ctx->n_vars_capacity) {
                    b_id = ctx->n_vars;
                    _init_tier0(&ctx->vars[b_id], 32, 0, else_cv, else_cv);
                    ctx->n_vars = b_id + 1;
                    if (ctx->watcher_heads) ctx->watcher_heads[b_id] = EXPR_NULL;
                    else_ok = 1;
                }

                if (cond_ok && then_ok && else_ok) {
                    prop_add_ite_value_64(ctx, r_id, cond_id, a_id, b_id, 0);
                    return 1;
                }

                /* Fallback: handle ITE with comparison cond or nested ITE
                 * branches by recursively materialising the ITE chain. */
                {
                    uint8_t val_w = (uint8_t)(r_id < ctx->n_vars_capacity
                                              ? ctx->vars[r_id].width : 32);
                    uint32_t ite_var = _value_to_var(ctx, sp, ite_side, val_w);
                    if (ite_var != EXPR_NULL) {
                        int wide = _var_needs_wide(ctx, r_id) ||
                                   _var_needs_wide(ctx, ite_var);
                        if (wide) prop_add_bounds_eq_64(ctx, r_id, ite_var, 0);
                        else      prop_add_bounds_eq_32(ctx, r_id, ite_var, 0);
                        return 1;
                    }
                }
            }
        }
    }

    /* ---- EXPR_ARRAY_SELECT: result = base[index] ---- */
    if (k == EXPR_ARRAY_SELECT) {
        ExprArraySelect *as = (ExprArraySelect *)zsp_pool_ptr(&sp->pool, root);
        uint32_t r_id, idx_id;
        if (!_is_var(sp, as->result, &r_id)) return 0;
        if (!_is_var(sp, as->index, &idx_id)) return 0;
        r_id   = _resolve(ctx, r_id);
        idx_id = _resolve(ctx, idx_id);
        uint32_t base = as->base_var_id;
        uint32_t n = as->n_elems;

        if (n == 0) return 1;  /* empty array: vacuously true */
        if (n > 64) return 0;  /* too large for ITE chain */

        /* Tighten index to [0, n-1] */
        if (ctx_tighten_lb64(ctx, idx_id, 0) == PROP_CONFLICT) return -1;
        if (ctx_tighten_ub64(ctx, idx_id, (int64_t)(n - 1)) == PROP_CONFLICT) return -1;

        /* For each element i, create: guard_i ↔ (idx == i), then
         * guard_i → (result == base[i]).
         * Uses ReificationEq for the guard link and bounds_eq for the
         * conditional equality. */
        for (uint32_t i = 0; i < n; i++) {
            uint32_t elem_id = base + i;
            if (elem_id >= ctx->n_vars_capacity) return 0;

            /* Create guard variable [0,1] */
            if (ctx->n_vars >= ctx->n_vars_capacity) return 0;
            uint32_t gid = ctx->n_vars;
            Variable *gv = &ctx->vars[gid];
            _init_tier0(gv, 1, 0, 0, 1);
            ctx->n_vars = gid + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[gid] = EXPR_NULL;
            if (gid < 64) ctx->unassigned_mask |= (1ULL << gid);

            /* Create const-var for index value i */
            if (ctx->n_vars >= ctx->n_vars_capacity) return 0;
            uint32_t cv_id = ctx->n_vars;
            Variable *cvv = &ctx->vars[cv_id];
            _init_tier0(cvv, 32, 0, (int64_t)i, (int64_t)i);
            ctx->n_vars = cv_id + 1;
            if (ctx->watcher_heads) ctx->watcher_heads[cv_id] = EXPR_NULL;

            /* guard ↔ (idx == i) */
            prop_add_reification_eq_32(ctx, gid, idx_id, cv_id, 0);

            /* guard → (result == base[i]): guard-gated EQ propagator */
            uint32_t props_before = ctx->n_props;
            prop_add_bounds_eq_32(ctx, r_id, elem_id, 0);
            /* Gate the EQ propagator with the guard */
            for (uint32_t pi = props_before; pi < ctx->n_props; pi++) {
                if (ctx->prop_guard_vars && pi < ctx->n_prop_refs_capacity)
                    ctx->prop_guard_vars[pi] = gid;
            }
        }
        return 1;
    }

    /* ---- EXPR_SUM: result == sum of var_ids[] ---- */
    if (k == EXPR_SUM) {
        ExprSum *es = (ExprSum *)zsp_pool_ptr(&sp->pool, root);
        uint32_t r_id;
        if (!_is_var(sp, es->result, &r_id)) return 0;
        r_id = _resolve(ctx, r_id);

        ExprRef *sum_refs = (ExprRef *)(es + 1);
        uint32_t summand_ids[MAX_SUM_VARS];
        uint32_t ns = es->n_vars;
        if (ns > MAX_SUM_VARS) return 0;

        for (uint32_t i = 0; i < ns; i++) {
            if (!_is_var(sp, sum_refs[i], &summand_ids[i])) return 0;
            summand_ids[i] = _resolve(ctx, summand_ids[i]);
        }

        uint32_t ref = prop_add_sum_eq_32(ctx, r_id, ns, summand_ids, 0);
        return (ref != EXPR_NULL) ? 1 : 0;
    }

    /* ---- EXPR_COUNTONES: result == popcount(operand) ---- */
    if (k == EXPR_COUNTONES) {
        ExprCountones *ec = (ExprCountones *)zsp_pool_ptr(&sp->pool, root);
        uint32_t r_id, x_id;
        if (!_is_var(sp, ec->result, &r_id)) return 0;
        if (!_is_var(sp, ec->operand, &x_id)) return 0;
        r_id = _resolve(ctx, r_id);
        x_id = _resolve(ctx, x_id);
        uint32_t ref = prop_add_countones_32(ctx, r_id, x_id, 0);
        return (ref != EXPR_NULL) ? 1 : 0;
    }

    /* ---- EXPR_CLOG2: result == ceil(log2(operand)) ---- */
    if (k == EXPR_CLOG2) {
        ExprClog2 *ec = (ExprClog2 *)zsp_pool_ptr(&sp->pool, root);
        uint32_t r_id, x_id;
        if (!_is_var(sp, ec->result, &r_id)) return 0;
        if (!_is_var(sp, ec->operand, &x_id)) return 0;
        r_id = _resolve(ctx, r_id);
        x_id = _resolve(ctx, x_id);
        uint32_t ref = prop_add_clog2_32(ctx, r_id, x_id, 0);
        return (ref != EXPR_NULL) ? 1 : 0;
    }

    /* ---- EXPR_IN_SET: value in {elem0, elem1, ...} ---- */
    if (k == EXPR_IN_SET) {
        ExprInSet *eis = (ExprInSet *)zsp_pool_ptr(&sp->pool, root);
        uint32_t vid;
        if (!_is_var(sp, eis->value, &vid)) return 0;
        vid = _resolve(ctx, vid);

        ExprRef *elem_refs = (ExprRef *)(eis + 1);
        uint32_t ne = eis->n_elems;
        if (ne == 0) return -1;  /* empty set -> UNSAT */

        /* Extract constant values from the element ExprRefs */
        int32_t *vals = (int32_t *)__builtin_alloca(ne * sizeof(int32_t));
        for (uint32_t i = 0; i < ne; i++) {
            int64_t cv;
            if (!_is_const(sp, elem_refs[i], &cv)) return 0;
            vals[i] = (int32_t)cv;
        }

        uint32_t ref = prop_add_in_set_32(ctx, vid, ne, vals, 0);
        return (ref != EXPR_NULL) ? 1 : 0;
    }

    /* ---- EXPR_IN_RANGE: value in [lo, hi] ---- */
    if (k == EXPR_IN_RANGE) {
        ExprInRange *eir = (ExprInRange *)zsp_pool_ptr(&sp->pool, root);
        uint32_t vid;
        int64_t lo_val, hi_val;
        if (!_is_var(sp, eir->value, &vid)) return 0;
        vid = _resolve(ctx, vid);
        if (!_is_const(sp, eir->lo, &lo_val)) return 0;
        if (!_is_const(sp, eir->hi, &hi_val)) return 0;

        /* Compile-time bound tightening */
        if (ctx_tighten_lb64(ctx, vid, lo_val) == PROP_CONFLICT) return -1;
        if (ctx_tighten_ub64(ctx, vid, hi_val) == PROP_CONFLICT) return -1;
        return 1;
    }

    /* ---- EXPR_UNARY at constraint root ---- */
    if (k == EXPR_UNARY) {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, root);

        /* Pattern: UNARY as a boolean constraint (e.g. !expr).
         * UN_NOT(expr): compile expr, then negate by adding != 0.
         * For now, handle UN_NOT of a comparison: !cmp -> negate the cmp. */
        if (eu->op == UN_NOT) {
            /* Check if operand is a binary comparison */
            if (eu->operand != EXPR_NULL) {
                ExprKind ok2 = *(ExprKind *)zsp_pool_ptr(&sp->pool, eu->operand);
                if (ok2 == EXPR_BINARY) {
                    ExprBinary *inner = (ExprBinary *)zsp_pool_ptr(&sp->pool, eu->operand);
                    /* Negate the comparison operator */
                    BinOp negated;
                    switch (inner->op) {
                    case BIN_EQ:  negated = BIN_NEQ; break;
                    case BIN_NEQ: negated = BIN_EQ;  break;
                    case BIN_LT:  negated = BIN_GTE; break;
                    case BIN_LTE: negated = BIN_GT;  break;
                    case BIN_GT:  negated = BIN_LTE; break;
                    case BIN_GTE: negated = BIN_LT;  break;
                    default:      return 0;  /* can't negate non-comparison */
                    }
                    /* Build a synthetic ExprBinary with the negated op.
                     * We can reuse the lhs/rhs from inner since they
                     * point into the sp pool which is read-only here.
                     * Create a temporary ExprBinary on the stack. */
                    /* We can't call _compile_constraint with a stack-local
                     * ExprRef, so handle var-const, var-var, and const-const
                     * directly with the negated operator. */
                    uint32_t lid, rid2;
                    int64_t cv, lc2, rc2;

                    /* Constant folding: CONST negated_op CONST */
                    if (_is_const(sp, inner->lhs, &lc2) && _is_const(sp, inner->rhs, &rc2)) {
                        int truth;
                        switch (negated) {
                        case BIN_EQ:  truth = (lc2 == rc2); break;
                        case BIN_NEQ: truth = (lc2 != rc2); break;
                        case BIN_LT:  truth = (lc2 <  rc2); break;
                        case BIN_LTE: truth = (lc2 <= rc2); break;
                        case BIN_GT:  truth = (lc2 >  rc2); break;
                        case BIN_GTE: truth = (lc2 >= rc2); break;
                        default:      truth = 1;             break;
                        }
                        return truth ? 1 : -1;
                    }
                    if (_is_var(sp, inner->lhs, &lid) && _is_var(sp, inner->rhs, &rid2)) {
                        lid = _resolve(ctx, lid);
                        rid2 = _resolve(ctx, rid2);
                        /* Same-var special cases */
                        if (lid == rid2) {
                            if (negated == BIN_EQ)  return 1;   /* v == v: tautology */
                            if (negated == BIN_NEQ) return -1;  /* v != v: UNSAT */
                        }
                        switch (negated) {
                        case BIN_LTE: prop_add_bounds_le_32(ctx, lid, rid2, 0); return 1;
                        case BIN_LT:  prop_add_bounds_lt_32(ctx, lid, rid2, 0); return 1;
                        case BIN_EQ:  prop_add_bounds_eq_32(ctx, lid, rid2, 0); return 1;
                        case BIN_NEQ: prop_add_bounds_ne_32(ctx, lid, rid2, 0); return 1;
                        case BIN_GT:  prop_add_bounds_lt_32(ctx, rid2, lid, 0); return 1;
                        case BIN_GTE: prop_add_bounds_le_32(ctx, rid2, lid, 0); return 1;
                        default: break;
                        }
                    }
                    /* var NEQ const: create const-var + NE propagator */
                    if (negated == BIN_NEQ) {
                        int is_vc = _is_var(sp, inner->lhs, &lid) && _is_const(sp, inner->rhs, &cv);
                        int is_cv = !is_vc && _is_const(sp, inner->lhs, &cv) && _is_var(sp, inner->rhs, &lid);
                        if ((is_vc || is_cv) && ctx->n_vars < ctx->n_vars_capacity) {
                            lid = _resolve(ctx, lid);
                            uint32_t cv_id = ctx->n_vars;
                            _init_tier0(&ctx->vars[cv_id], 32, 0, cv, cv);
                            ctx->n_vars = cv_id + 1;
                            if (ctx->watcher_heads) ctx->watcher_heads[cv_id] = EXPR_NULL;
                            prop_add_bounds_ne_32(ctx, lid, cv_id, 0);
                            return 1;
                        }
                    }
                    /* var EQ/cmp const: delegate to _compile_var_const_cmp */
                    if (_is_var(sp, inner->lhs, &lid) && _is_const(sp, inner->rhs, &cv)) {
                        lid = _resolve(ctx, lid);
                        int r = _compile_var_const_cmp(ctx, negated, lid, cv, 0);
                        if (r != 0) return r;
                    }
                    if (_is_const(sp, inner->lhs, &cv) && _is_var(sp, inner->rhs, &lid)) {
                        lid = _resolve(ctx, lid);
                        int r = _compile_var_const_cmp(ctx, negated, lid, cv, 1);
                        if (r != 0) return r;
                    }
                    /* NOT(EQ(ITE_chain, var)) or NOT(EQ(var, ITE_chain)):
                     * materialize the ITE into an aux var and add a NE constraint. */
                    if (negated == BIN_NEQ) {
                        ExprKind lk3 = *(ExprKind *)zsp_pool_ptr(&sp->pool, inner->lhs);
                        ExprKind rk3 = *(ExprKind *)zsp_pool_ptr(&sp->pool, inner->rhs);
                        ExprRef ite_s = EXPR_NULL; uint32_t v_id;
                        if (lk3 == EXPR_ITE && _is_var(sp, inner->rhs, &v_id))
                            ite_s = inner->lhs;
                        else if (rk3 == EXPR_ITE && _is_var(sp, inner->lhs, &v_id))
                            ite_s = inner->rhs;
                        if (ite_s != EXPR_NULL) {
                            v_id = _resolve(ctx, v_id);
                            uint8_t val_w = (uint8_t)(v_id < ctx->n_vars_capacity
                                                      ? ctx->vars[v_id].width : 32);
                            uint32_t ite_var = _value_to_var(ctx, sp, ite_s, val_w);
                            if (ite_var != EXPR_NULL) {
                                prop_add_bounds_ne_32(ctx, v_id, ite_var, 0);
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Unhandled expression type */
    return 0;
}

/* Recursively collect BIN_EQ(var,var) pairs from a constraint tree
 * (including those nested inside BIN_AND) into the alias table.
 * This lets the compile-time UNSAT check detect contradictions like
 * eq(x,y) + ne(x,y) via alias resolution before search. */
static void _collect_eq_aliases(uint32_t *alias, SolveProblem *sp, ExprRef ref) {
    if (ref == EXPR_NULL) return;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    if (k != EXPR_BINARY) return;
    ExprBinary *e = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
    if (e->op == BIN_AND) {
        _collect_eq_aliases(alias, sp, e->lhs);
        _collect_eq_aliases(alias, sp, e->rhs);
    } else if (e->op == BIN_EQ) {
        uint32_t lid, rid;
        if (_is_var(sp, e->lhs, &lid) && _is_var(sp, e->rhs, &rid))
            _alias_union(alias, lid, rid);
    }
}

/* ------------------------------------------------------------------ */
/* solver_compile                                                      */
/* ------------------------------------------------------------------ */

int solver_compile(SolveCtx *ctx, SolveProblem *sp) {
    uint32_t n = sp->n_vars;
    if (n == 0) {
        ctx->vars   = NULL;
        ctx->n_vars = 0;
        return 0;
    }

    /* Allocate a contiguous Variable[n + slack] in the static pool.
       Slack is needed for compile-time aux vars (ITE chain materialisation,
       reification guards, const-vars) plus incremental variable addition.
       Each select-over-N-array element creates ~3N aux during compile,
       and nested stores compound that — scale slack with `n` so larger
       problems don't silently silently drop the bounds_eq link. */
    #define VAR_SLACK_MIN  64u
    #define VAR_SLACK_FACTOR 32u
    uint32_t slack = n * VAR_SLACK_FACTOR;
    if (slack < VAR_SLACK_MIN) slack = VAR_SLACK_MIN;
    uint32_t capacity = n + slack;
    uint32_t vars_ref = zsp_pool_alloc(&ctx->pool,
                                        capacity * (uint32_t)sizeof(Variable),
                                        (uint32_t)_Alignof(Variable));
    if (vars_ref == EXPR_NULL) return -1;

    ctx->vars           = (Variable *)zsp_pool_ptr(&ctx->pool, vars_ref);
    ctx->n_vars         = n;
    ctx->n_vars_capacity = capacity;

    /* Zero-initialise the whole array (including slack) */
    memset(ctx->vars, 0, capacity * sizeof(Variable));

    /* Initialise unassigned_mask -- set after variable init below */
    ctx->unassigned_mask = 0;

    /* Walk the VarSpec linked list (stored in sp's pool) */
    ExprRef ref = sp->vars_head;

    while (ref != EXPR_NULL) {
        VarSpec *vs = (VarSpec *)zsp_pool_ptr(&sp->pool, ref);
        uint32_t id = vs->var_id;

        if (id >= n) {
            /* var_id out of range — pool corruption or misuse */
            return -1;
        }

        Variable *v = &ctx->vars[id];
        uint8_t  flags = 0;
        if (vs->is_signed) flags |= VAR_SIGNED;
        if (vs->is_aux)    flags |= VAR_AUX;

        int rc;
        if (vs->width < 32) {
            _init_tier0(v, vs->width, flags, vs->lo, vs->hi);
            rc = 0;
        } else if (vs->width == 32 && (flags & VAR_SIGNED)) {
            _init_tier0(v, vs->width, flags, vs->lo, vs->hi);
            rc = 0;
        } else if (vs->width == 32 && !(flags & VAR_SIGNED)) {
            rc = _init_tier1(ctx, v, vs->width, flags, vs->lo, vs->hi);
        } else if (vs->width <= 64) {
            rc = _init_tier1(ctx, v, vs->width, flags, vs->lo, vs->hi);
        } else {
            rc = _init_tier2(ctx, v, vs->width, flags, vs->lo, vs->hi);
        }

        if (rc != 0) return -1;
        ref = vs->next;
    }

    /* Build unassigned_mask: set bits for non-singleton, non-aliased variables */
    if (n <= 64) {
        uint64_t mask = 0;
        for (uint32_t i = 0; i < n; i++) {
            /* Skip aliased (non-root) variables */
            if (ctx->var_alias && ctx->var_alias[i] != i) continue;
            Variable *v = &ctx->vars[i];
            int64_t lo = var_lo64(ctx, v);
            int64_t hi = var_hi64(ctx, v);
            if (lo != hi) mask |= (1ULL << i);
        }
        ctx->unassigned_mask = mask;
    }

    /* ---- Allocate per-variable watcher head array (with slack) ---- */
    uint32_t wh_ref = zsp_pool_alloc(&ctx->pool,
                                      capacity * (uint32_t)sizeof(uint32_t),
                                      (uint32_t)_Alignof(uint32_t));
    if (wh_ref == EXPR_NULL) return -1;
    ctx->watcher_heads = (uint32_t *)zsp_pool_ptr(&ctx->pool, wh_ref);
    for (uint32_t i = 0; i < capacity; i++) ctx->watcher_heads[i] = EXPR_NULL;

    /* ---- Allocate prop_refs array for checkpoint/restore ----
     * Capacity must cover all propagators, including aux ones created
     * during compile-time materialisation of ITE chains, boolean trees,
     * and array select/store. A fixed slack of 128 was hit by larger
     * QF_AUFBV BMC fixtures, so we use a multiplicative slack mirroring
     * the variable-slack policy. */
    #define PROP_SLACK_FACTOR 32u
    #define PROP_SLACK_MIN    128u
    uint32_t pr_base = sp->n_constraints + sp->n_alldiffs;
    uint32_t pr_cap  = pr_base * PROP_SLACK_FACTOR;
    if (pr_cap < pr_base + PROP_SLACK_MIN) pr_cap = pr_base + PROP_SLACK_MIN;
    uint32_t pr_ref = zsp_pool_alloc(&ctx->pool,
                                      pr_cap * (uint32_t)sizeof(uint32_t),
                                      (uint32_t)_Alignof(uint32_t));
    if (pr_ref == EXPR_NULL) return -1;
    ctx->prop_refs = (uint32_t *)zsp_pool_ptr(&ctx->pool, pr_ref);
    ctx->n_prop_refs_capacity = pr_cap;
    for (uint32_t i = 0; i < pr_cap; i++) ctx->prop_refs[i] = EXPR_NULL;

    /* ---- Allocate guard-variable side table ---- */
    uint32_t gv_ref = zsp_pool_alloc(&ctx->pool,
                                      pr_cap * (uint32_t)sizeof(uint32_t),
                                      (uint32_t)_Alignof(uint32_t));
    if (gv_ref == EXPR_NULL) return -1;
    ctx->prop_guard_vars = (uint32_t *)zsp_pool_ptr(&ctx->pool, gv_ref);
    for (uint32_t i = 0; i < pr_cap; i++) ctx->prop_guard_vars[i] = EXPR_NULL;

    /* ---- Allocate prop_constraint_id side table ---- */
    uint32_t pc_ref = zsp_pool_alloc(&ctx->pool,
                                      pr_cap * (uint32_t)sizeof(uint32_t),
                                      (uint32_t)_Alignof(uint32_t));
    if (pc_ref == EXPR_NULL) return -1;
    ctx->prop_constraint_id = (uint32_t *)zsp_pool_ptr(&ctx->pool, pc_ref);
    for (uint32_t i = 0; i < pr_cap; i++) ctx->prop_constraint_id[i] = 0;

    /* ---- Build var alias table (union-find for BIN_EQ(var,var)) ---- */
    {
        uint32_t al_ref = zsp_pool_alloc(&ctx->pool,
                                          capacity * (uint32_t)sizeof(uint32_t),
                                          (uint32_t)_Alignof(uint32_t));
        if (al_ref != EXPR_NULL) {
            ctx->var_alias = (uint32_t *)zsp_pool_ptr(&ctx->pool, al_ref);
            for (uint32_t i = 0; i < capacity; i++) ctx->var_alias[i] = i;

            /* Pre-scan: collect unconditional BIN_EQ(var, var) constraints,
             * including those nested inside BIN_AND trees. */
            ExprRef scan = sp->constraints_head;
            while (scan != EXPR_NULL) {
                ConstraintSpec *cs = (ConstraintSpec *)zsp_pool_ptr(&sp->pool, scan);
                _collect_eq_aliases(ctx->var_alias, sp, cs->root);
                scan = cs->next;
            }

            /* Merge domains: for each non-root, intersect with root */
            for (uint32_t i = 0; i < n; i++) {
                uint32_t root_id = _alias_find(ctx->var_alias, i);
                if (root_id != i) {
                    Variable *rv = &ctx->vars[root_id];
                    Variable *iv = &ctx->vars[i];
                    /* Intersect domains */
                    int64_t new_lo, new_hi;
                    if (VAR_IS_TIER0(rv->flags) && VAR_IS_TIER0(iv->flags)) {
                        new_lo = rv->lo > iv->lo ? rv->lo : iv->lo;
                        new_hi = rv->hi < iv->hi ? rv->hi : iv->hi;
                        if (new_lo > new_hi) return -2; /* UNSAT */
                        rv->lo = (int32_t)new_lo;
                        rv->hi = (int32_t)new_hi;
                    }
                    /* Mark aliased var as singleton pointing to root value.
                     * It won't be in the unassigned set. */
                }
            }
        }
    }

    /* ---- Walk ConstraintSpec list → create propagators ---- */
    int n_uncompiled = 0;
    ExprRef cref = sp->constraints_head;
    while (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)zsp_pool_ptr(&sp->pool, cref);
        uint32_t props_before_c = ctx->n_props;
        int r = _compile_constraint(ctx, sp, cs->root);
        if (r < 0) return -2;  /* -2 = UNSAT detected at compile time */
        if (r == 0) n_uncompiled++;
        /* Tag all newly created propagators with this constraint's ID */
        if (cs->constraint_id && ctx->prop_constraint_id) {
            for (uint32_t pi = props_before_c;
                 pi < ctx->n_props && pi < ctx->n_prop_refs_capacity; pi++) {
                ctx->prop_constraint_id[pi] = cs->constraint_id;
            }
        }
        cref = cs->next;
    }


    /* ---- Walk AllDiffSpec list -> create AllDifferent propagators ---- */
    ExprRef adref = sp->allDiff_head;
    while (adref != EXPR_NULL) {
        AllDiffSpec *ad = (AllDiffSpec *)zsp_pool_ptr(&sp->pool, adref);
        uint32_t *vids = (uint32_t *)(ad + 1);
        uint32_t pref = prop_add_all_different(ctx, ad->n_vars, vids, 1);
        if (pref == EXPR_NULL) return -1;
        adref = ad->next;
    }

    /* ---- Walk SoftSpec list -> create assumption-gated propagators ---- */
    ctx->n_assumptions = 0;
    ctx->assumption_var_ids = NULL;
    ctx->assumption_priorities = NULL;
    ctx->assumption_active_mask = 0;

    if (sp->n_softs > 0) {
        uint32_t ns = sp->n_softs;
        /* Allocate assumption tracking arrays */
        uint32_t av_ref = zsp_pool_alloc(&ctx->pool,
                                          ns * (uint32_t)sizeof(uint32_t),
                                          (uint32_t)_Alignof(uint32_t));
        uint32_t ap_ref = zsp_pool_alloc(&ctx->pool,
                                          ns * (uint32_t)sizeof(uint32_t),
                                          (uint32_t)_Alignof(uint32_t));
        if (av_ref != EXPR_NULL && ap_ref != EXPR_NULL) {
            ctx->assumption_var_ids = (uint32_t *)zsp_pool_ptr(&ctx->pool, av_ref);
            ctx->assumption_priorities = (uint32_t *)zsp_pool_ptr(&ctx->pool, ap_ref);

            uint32_t aidx = 0;
            ExprRef sref = sp->softs_head;
            while (sref != EXPR_NULL) {
                SoftSpec *ss = (SoftSpec *)zsp_pool_ptr(&sp->pool, sref);

                /* Create an assumption boolean variable [0,1], pinned to 1 */
                uint32_t avar_id = ctx->n_vars;
                if (avar_id < ctx->n_vars_capacity) {
                    Variable *av = &ctx->vars[avar_id];
                    av->lo = 1; av->hi = 1;   /* pinned to 1 = active */
                    av->width = 1; av->flags = 0;
                    av->holes_offset = 0; av->_pad = 0;
                    ctx->n_vars = avar_id + 1;

                    if (ctx->watcher_heads)
                        ctx->watcher_heads[avar_id] = EXPR_NULL;
                    /* Assumption var is singleton 1, no unassigned bit */

                    /* Record assumption metadata */
                    ctx->assumption_var_ids[aidx] = avar_id;
                    ctx->assumption_priorities[aidx] = ss->priority;

                    /* Compile the soft constraint body.
                     * For var-const comparisons, use Implication propagators
                     * (which have built-in guard semantics via avar_id)
                     * instead of _compile_constraint which does compile-time
                     * tightening that can't be undone by guard relaxation. */
                    int soft_compiled = 0;
                    if (ss->root != EXPR_NULL) {
                        ExprKind sk = *(ExprKind *)zsp_pool_ptr(&sp->pool, ss->root);
                        if (sk == EXPR_BINARY) {
                            ExprBinary *se = (ExprBinary *)zsp_pool_ptr(&sp->pool, ss->root);
                            uint32_t svid; int64_t scv;
                            int is_vc = _is_var(sp, se->lhs, &svid) && _is_const(sp, se->rhs, &scv);
                            int is_cv = !is_vc && _is_const(sp, se->lhs, &scv) && _is_var(sp, se->rhs, &svid);
                            if (is_vc || is_cv) {
                                /* Flip operator for const-var ordering */
                                BinOp sop = se->op;
                                if (is_cv) {
                                    switch (sop) {
                                    case BIN_LT:  sop = BIN_GT;  break;
                                    case BIN_LTE: sop = BIN_GTE; break;
                                    case BIN_GT:  sop = BIN_LT;  break;
                                    case BIN_GTE: sop = BIN_LTE; break;
                                    default: break;
                                    }
                                }
                                /* Create implication propagators gated by avar_id */
                                switch (sop) {
                                case BIN_EQ:
                                    prop_add_implication_32(ctx, avar_id, svid, (int32_t)scv, 1, 0);
                                    prop_add_implication_32(ctx, avar_id, svid, (int32_t)scv, 0, 0);
                                    soft_compiled = 1;
                                    break;
                                case BIN_LTE:
                                    prop_add_implication_32(ctx, avar_id, svid, (int32_t)scv, 1, 0);
                                    soft_compiled = 1;
                                    break;
                                case BIN_LT:
                                    prop_add_implication_32(ctx, avar_id, svid, (int32_t)(scv - 1), 1, 0);
                                    soft_compiled = 1;
                                    break;
                                case BIN_GTE:
                                    prop_add_implication_32(ctx, avar_id, svid, (int32_t)scv, 0, 0);
                                    soft_compiled = 1;
                                    break;
                                case BIN_GT:
                                    prop_add_implication_32(ctx, avar_id, svid, (int32_t)(scv + 1), 0, 0);
                                    soft_compiled = 1;
                                    break;
                                default: break;
                                }
                            }
                        }
                    }
                    /* Fallback: use _compile_constraint + guard-gating
                     * for patterns that create propagators */
                    if (!soft_compiled) {
                        uint32_t props_before = ctx->n_props;
                        int r = _compile_constraint(ctx, sp, ss->root);
                        if (r > 0) {
                            for (uint32_t pi = props_before; pi < ctx->n_props; pi++) {
                                if (ctx->prop_guard_vars &&
                                    pi < ctx->n_prop_refs_capacity)
                                    ctx->prop_guard_vars[pi] = avar_id;
                            }
                        }
                    }

                    aidx++;
                }
                sref = ss->next;
            }
            ctx->n_assumptions = aidx;
            /* All assumptions start active */
            ctx->assumption_active_mask = (aidx < 64)
                ? ((1ULL << aidx) - 1) : ~0ULL;
        }
    }

    /* ---- Walk DistSpec list -> store dist metadata per variable ---- */
    ctx->dist_offsets = NULL;
    if (sp->n_dists > 0) {
        /* Allocate per-var dist_offsets array */
        uint32_t do_ref = zsp_pool_alloc(&ctx->pool,
                                          ctx->n_vars_capacity * (uint32_t)sizeof(uint32_t),
                                          (uint32_t)_Alignof(uint32_t));
        if (do_ref != EXPR_NULL) {
            ctx->dist_offsets = (uint32_t *)zsp_pool_ptr(&ctx->pool, do_ref);
            for (uint32_t i = 0; i < ctx->n_vars_capacity; i++)
                ctx->dist_offsets[i] = 0;

            ExprRef dref = sp->dists_head;
            while (dref != EXPR_NULL) {
                DistSpec *ds = (DistSpec *)zsp_pool_ptr(&sp->pool, dref);
                uint32_t vid = ds->var_id;
                uint32_t ne = ds->n_entries;
                DistEntry *src_entries = (DistEntry *)(ds + 1);

                /* Allocate DistMeta + trailing DistMetaEntry array in pool */
                uint32_t dm_total = (uint32_t)sizeof(DistMeta) +
                                    ne * (uint32_t)sizeof(DistMetaEntry);
                uint32_t dm_ref = zsp_pool_alloc(&ctx->pool, dm_total,
                                                  (uint32_t)_Alignof(DistMeta));
                if (dm_ref != EXPR_NULL && vid < ctx->n_vars_capacity) {
                    DistMeta *dm = (DistMeta *)zsp_pool_ptr(&ctx->pool, dm_ref);
                    dm->n_entries = ne;
                    dm->_pad = 0;
                    DistMetaEntry *dme = (DistMetaEntry *)(dm + 1);

                    uint64_t cum = 0;
                    for (uint32_t i = 0; i < ne; i++) {
                        dme[i].lo = src_entries[i].lo;
                        dme[i].hi = src_entries[i].hi;
                        dme[i].weight = src_entries[i].weight;
                        dme[i].is_per_value = src_entries[i].is_per_value;
                        dme[i]._dmpad[0] = dme[i]._dmpad[1] = dme[i]._dmpad[2] = 0;

                        /* Compute effective weight for this entry */
                        uint64_t ew;
                        if (src_entries[i].weight == 0) {
                            ew = 0;
                        } else if (src_entries[i].is_per_value) {
                            /* := weight: each value gets 'weight', so total is weight * count */
                            uint64_t count = (uint64_t)(src_entries[i].hi - src_entries[i].lo) + 1u;
                            ew = (uint64_t)src_entries[i].weight * count;
                        } else {
                            /* :/ weight: the entire range shares 'weight' */
                            ew = (uint64_t)src_entries[i].weight;
                        }
                        cum += ew;
                        dme[i].cum_weight = cum;
                    }
                    ctx->dist_offsets[vid] = dm_ref;
                }
                dref = ds->next;
            }
        }
    }

    /* ---- Allocate per-variable hole list heads (randc support) ---- */
    {
        uint32_t vh_ref = zsp_pool_alloc(&ctx->pool,
                                          ctx->n_vars_capacity * (uint32_t)sizeof(uint32_t),
                                          (uint32_t)_Alignof(uint32_t));
        if (vh_ref != EXPR_NULL) {
            ctx->var_holes_head = (uint32_t *)zsp_pool_ptr(&ctx->pool, vh_ref);
            for (uint32_t i = 0; i < ctx->n_vars_capacity; i++)
                ctx->var_holes_head[i] = 0;
        } else {
            ctx->var_holes_head = NULL;
        }
    }

    /* ---- Save initial variable state for solver_reset() ---- */
    /* Use ctx->n_vars (not n) to include assumption vars from soft constraints */
    {
        uint32_t save_n = ctx->n_vars;
        uint32_t iv_ref = zsp_pool_alloc(&ctx->pool,
                                          save_n * (uint32_t)sizeof(Variable),
                                          (uint32_t)_Alignof(Variable));
        if (iv_ref != EXPR_NULL) {
            ctx->initial_vars = (Variable *)zsp_pool_ptr(&ctx->pool, iv_ref);
            memcpy(ctx->initial_vars, ctx->vars, save_n * sizeof(Variable));
            ctx->initial_n_vars = save_n;

            /* For tier-1 vars, also save the WideBounds64 contents.
             * The initial_vars[] have correct holes_offset values, so we
             * can restore from there. The WideBounds64 data at those offsets
             * will be overwritten during solving. Save a copy. */
            for (uint32_t i = 0; i < save_n; i++) {
                Variable *v = &ctx->vars[i];
                if (VAR_IS_TIER1(v->flags) && v->holes_offset != 0) {
                    /* The initial_vars[i].holes_offset points to the same
                     * WideBounds64 in the pool. We need a separate copy. */
                    uint32_t wb_ref = zsp_pool_alloc(&ctx->pool,
                                                      (uint32_t)sizeof(WideBounds64),
                                                      (uint32_t)_Alignof(WideBounds64));
                    if (wb_ref != EXPR_NULL) {
                        WideBounds64 *src = (WideBounds64 *)zsp_pool_ptr(&ctx->pool, v->holes_offset);
                        WideBounds64 *dst = (WideBounds64 *)zsp_pool_ptr(&ctx->pool, wb_ref);
                        *dst = *src;
                        /* Point initial_vars[i].holes_offset to the saved copy */
                        ctx->initial_vars[i].holes_offset = wb_ref;
                    }
                }
            }
        } else {
            ctx->initial_vars   = NULL;
            ctx->initial_n_vars = 0;
        }
    }

    /* Return count of uncompiled constraints (0 = all compiled, >0 = partial,
       negative values reserved for hard errors above). */
    return n_uncompiled;
}

/* ------------------------------------------------------------------ */
/* solver_add_constraint — incremental constraint addition            */
/* ------------------------------------------------------------------ */

int solver_add_constraint(SolveCtx *ctx, SolveProblem *aux_sp) {
    int n_uncompiled = 0;

    /* ---- Add new variables ---- */
    ExprRef vref = aux_sp->vars_head;
    while (vref != EXPR_NULL) {
        VarSpec *vs = (VarSpec *)zsp_pool_ptr(&aux_sp->pool, vref);
        uint32_t id = vs->var_id;

        if (id >= ctx->n_vars_capacity) return -1;  /* no room */

        if (id >= ctx->n_vars) {
            /* New variable: initialise it */
            Variable *v = &ctx->vars[id];
            uint8_t flags = 0;
            if (vs->is_signed) flags |= VAR_SIGNED;
            if (vs->is_aux)    flags |= VAR_AUX;

            int rc;
            if (vs->width < 32) {
                _init_tier0(v, vs->width, flags, vs->lo, vs->hi);
                rc = 0;
            } else if (vs->width == 32 && (flags & VAR_SIGNED)) {
                _init_tier0(v, vs->width, flags, vs->lo, vs->hi);
                rc = 0;
            } else if (vs->width == 32 && !(flags & VAR_SIGNED)) {
                rc = _init_tier1(ctx, v, vs->width, flags, vs->lo, vs->hi);
            } else if (vs->width <= 64) {
                rc = _init_tier1(ctx, v, vs->width, flags, vs->lo, vs->hi);
            } else {
                rc = _init_tier2(ctx, v, vs->width, flags, vs->lo, vs->hi);
            }
            if (rc != 0) return -1;

            /* Update n_vars to include this and any gaps */
            if (id + 1 > ctx->n_vars)
                ctx->n_vars = id + 1;
        }
        vref = vs->next;
    }

    /* ---- Compile new constraints ---- */
    ExprRef cref = aux_sp->constraints_head;
    while (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)zsp_pool_ptr(&aux_sp->pool, cref);
        int r = _compile_constraint(ctx, aux_sp, cs->root);
        if (r < 0) return -2;  /* UNSAT at compile time */
        if (r == 0) n_uncompiled++;
        cref = cs->next;
    }

    /* ---- Compile new AllDifferent constraints ---- */
    ExprRef adref = aux_sp->allDiff_head;
    while (adref != EXPR_NULL) {
        AllDiffSpec *ad = (AllDiffSpec *)zsp_pool_ptr(&aux_sp->pool, adref);
        uint32_t *vids = (uint32_t *)(ad + 1);
        uint32_t pref = prop_add_all_different(ctx, ad->n_vars, vids, 1);
        if (pref == EXPR_NULL) return -1;
        adref = ad->next;
    }

    /* ---- Run propagation to fixpoint ---- */
    PropResult pr = solver_propagate(ctx);
    if (pr == PROP_CONFLICT) return -2;

    return n_uncompiled;
}

/* ------------------------------------------------------------------ */
/* solver_add_array_vars — bulk-create element variables               */
/* ------------------------------------------------------------------ */

int solver_add_array_vars(SolveCtx *ctx,
                          uint32_t elem_var_base,
                          uint32_t n_elems,
                          uint8_t  width,
                          uint8_t  is_signed,
                          int64_t  lo,
                          int64_t  hi) {
    uint32_t end = elem_var_base + n_elems;
    if (end > ctx->n_vars_capacity) return -1;

    uint8_t flags = is_signed ? VAR_SIGNED : 0;
    for (uint32_t i = elem_var_base; i < end; i++) {
        Variable *v = &ctx->vars[i];
        int rc;
        if (width < 32) {
            _init_tier0(v, width, flags, lo, hi);
            rc = 0;
        } else if (width == 32 && is_signed) {
            _init_tier0(v, width, flags, lo, hi);
            rc = 0;
        } else if (width == 32 && !is_signed) {
            rc = _init_tier1(ctx, v, width, flags, lo, hi);
        } else if (width <= 64) {
            rc = _init_tier1(ctx, v, width, flags, lo, hi);
        } else {
            rc = _init_tier2(ctx, v, width, flags, lo, hi);
        }
        if (rc != 0) return -1;
        if (ctx->watcher_heads) ctx->watcher_heads[i] = EXPR_NULL;
    }
    if (end > ctx->n_vars) ctx->n_vars = end;
    return 0;
}
