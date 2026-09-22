/*
 * solver_validate_model — post-solve sanity check.
 *
 * Walks every top-level ConstraintSpec in the original SolveProblem and
 * evaluates its root expression under the current variable assignment.
 * A 0 value means the constraint is violated; this catches silent
 * constraint-drop bugs that the compile path would otherwise hide
 * behind a SOLVE_OK result.
 *
 * Expressions involving arrays / sums / countones / clog2 / in_set /
 * in_range are skipped (skip flag) — they require context the evaluator
 * does not have, and they're not the bug class this guard is targeting.
 */
#include <stdint.h>
#include <stdio.h>
#include "zsp_ctx.h"
#include "zsp_problem.h"

/* ------------------------------------------------------------------ */
/* Typed value: (value, width).  Width 0 means "untyped scalar" (a    */
/* constant or the result of an op where width does not matter).      */
/* width >= 1 means "BV of this width" — value bits above width are   */
/* the caller's responsibility (we mask after each arithmetic op).    */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t  value;
    uint16_t width;
    uint8_t  is_signed;   /* operand should be interpreted as signed */
} EvalVal;

/* Interpret the low `w` bits of `u` as a 2's-complement signed value. */
static int64_t _sext(uint64_t u, uint16_t w) {
    if (w == 0 || w >= 64) return (int64_t)u;
    uint64_t m = ((uint64_t)1 << w) - 1;
    u &= m;
    if (u & ((uint64_t)1 << (w - 1)))
        u |= ~m;
    return (int64_t)u;
}

/* Walk an expression tree and check whether it references any aux var
 * whose current domain is wider than a singleton. Such a constraint may
 * be unfinished by propagation rather than truly violated; treating it
 * as a violation produces false positives. */
static int _has_loose_aux(const SolveCtx *ctx, const SolveProblem *sp,
                           ExprRef ref) {
    if (ref == EXPR_NULL) return 0;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    switch (k) {
    case EXPR_VAR: {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        if (ev->var_id >= ctx->n_vars) return 0;
        uint32_t resolved = ev->var_id;
        if (ctx->var_alias) {
            while (ctx->var_alias[resolved] != resolved)
                resolved = ctx->var_alias[resolved];
        }
        const Variable *v = &ctx->vars[resolved];
        if (!(v->flags & VAR_AUX)) return 0;
        int64_t lo = var_lo64(ctx, v);
        int64_t hi = var_hi64(ctx, v);
        return lo != hi;
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        return _has_loose_aux(ctx, sp, eb->lhs)
            || _has_loose_aux(ctx, sp, eb->rhs);
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        return _has_loose_aux(ctx, sp, eu->operand);
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)zsp_pool_ptr(&sp->pool, ref);
        return _has_loose_aux(ctx, sp, ei->cond)
            || _has_loose_aux(ctx, sp, ei->then_e)
            || _has_loose_aux(ctx, sp, ei->else_e);
    }
    case EXPR_EXTEND: {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        return _has_loose_aux(ctx, sp, ee->operand);
    }
    case EXPR_EXTRACT: {
        ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ref);
        return _has_loose_aux(ctx, sp, ex->operand);
    }
    case EXPR_CONCAT: {
        ExprConcat *ec = (ExprConcat *)zsp_pool_ptr(&sp->pool, ref);
        return _has_loose_aux(ctx, sp, ec->hi)
            || _has_loose_aux(ctx, sp, ec->lo);
    }
    default: return 0;
    }
}

static uint64_t _mask_for(uint16_t w) {
    if (w == 0 || w >= 64) return ~(uint64_t)0;
    return ((uint64_t)1 << w) - 1;
}

/**
 * Widest declared operand width anywhere in a subtree -- the SystemVerilog
 * *context-determined* width, which is the rule the engine actually evaluates
 * under and therefore the rule this validator has to reproduce.
 *
 * Bit-width in SV is not a bottom-up property of each node. Every operand of an
 * expression is evaluated at the width of the WIDEST operand in the whole
 * expression, including the far side of the comparison the expression sits in.
 * Evaluating bottom-up with `max(lhs_width, rhs_width)` per node instead gets
 * both directions wrong, and each error surfaced as a false *violation* on a
 * perfectly good model:
 *
 *   a == k * 2          (u8 a, u4 k)  ->  8*2 masked to k's 4 bits == 0
 *   addr % (1 << size)  (37-bit addr) ->  1<<12 masked to size's 4 bits == 0
 *
 * while a genuinely narrow context must still wrap, which is what makes
 * `r == a + b` over three u8s equal 44 rather than 300.
 *
 * A shift's right operand is *self-determined* in SV -- it is a shift amount,
 * not a value in the result's space -- so it does not contribute here.
 * Constants are unsized and contribute nothing.
 */
static uint16_t _max_width(const SolveCtx *ctx, const SolveProblem *sp,
                           ExprRef ref) {
    if (ref == EXPR_NULL) return 0;
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    switch (k) {
    case EXPR_VAR: {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        if (ev->var_id >= ctx->n_vars) return 0;
        return ctx->vars[ev->var_id].width;
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        uint16_t l = _max_width(ctx, sp, eb->lhs);
        if (eb->op == BIN_LSHIFT || eb->op == BIN_RSHIFT) return l;
        uint16_t rr = _max_width(ctx, sp, eb->rhs);
        return l > rr ? l : rr;
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        return _max_width(ctx, sp, eu->operand);
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)zsp_pool_ptr(&sp->pool, ref);
        uint16_t t = _max_width(ctx, sp, ei->then_e);
        uint16_t e = _max_width(ctx, sp, ei->else_e);
        return t > e ? t : e;
    }
    case EXPR_EXTEND: {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        return ee->to_bits;
    }
    case EXPR_EXTRACT: {
        ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ref);
        return (uint16_t)(ex->hi_bit - ex->lo_bit + 1);
    }
    default:
        return 0;
    }
}

/* `ctx_w` is the context-determined width imposed from above (0 = none). */
static EvalVal _eval(const SolveCtx *ctx, const SolveProblem *sp,
                      ExprRef ref, int *skip, uint16_t ctx_w);

/* Try-call helper: returns 1 on success, 0 if skip was set. */
static int _eval_ok(const SolveCtx *ctx, const SolveProblem *sp,
                     ExprRef ref, EvalVal *out, int *skip, uint16_t ctx_w) {
    *out = _eval(ctx, sp, ref, skip, ctx_w);
    return *skip == 0;
}

static EvalVal _eval(const SolveCtx *ctx, const SolveProblem *sp,
                      ExprRef ref, int *skip, uint16_t ctx_w) {
    EvalVal r = { 0, 0, 0 };
    if (*skip) return r;
    if (ref == EXPR_NULL) { *skip = 1; return r; }

    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    switch (k) {
    case EXPR_CONST: {
        ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, ref);
        r.value = ec->value;
        r.width = 0;
        return r;
    }
    case EXPR_VAR: {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        if (ev->var_id >= ctx->n_vars) { *skip = 1; return r; }
        /* Resolve through alias table so we read the representative's
         * bounds (aliased vars are not kept in sync with their root). */
        uint32_t resolved = ev->var_id;
        if (ctx->var_alias) {
            while (ctx->var_alias[resolved] != resolved)
                resolved = ctx->var_alias[resolved];
        }
        const Variable *v = &ctx->vars[resolved];
        r.value = var_lo64(ctx, v);
        r.width = ctx->vars[ev->var_id].width;  /* width from declared var */
        r.is_signed = (v->flags & VAR_SIGNED) != 0;
        return r;
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal a, b;
        int is_cmp = 0, is_shift = 0, is_logic = 0;
        switch (eb->op) {
        case BIN_EQ: case BIN_NEQ:
        case BIN_LT: case BIN_LTE: case BIN_GT: case BIN_GTE:
            is_cmp = 1; break;
        case BIN_LSHIFT: case BIN_RSHIFT:
            is_shift = 1; break;
        case BIN_AND: case BIN_OR:
            is_logic = 1; break;
        default: break;
        }

        /* A comparison is where a new width context begins: both sides are
         * evaluated at the width of the widest operand across BOTH of them.
         * A logical AND/OR joins two self-contained operands, so neither
         * inherits a context. Everything else passes its own context down. */
        uint16_t child_w = ctx_w;
        if (is_cmp) {
            uint16_t lw = _max_width(ctx, sp, eb->lhs);
            uint16_t rw = _max_width(ctx, sp, eb->rhs);
            child_w = lw > rw ? lw : rw;
        } else if (is_logic) {
            child_w = 0;
        }

        if (!_eval_ok(ctx, sp, eb->lhs, &a, skip, child_w)) return r;
        /* A shift amount is self-determined -- it is not a value in the
         * result's space, so it takes no context width. */
        if (!_eval_ok(ctx, sp, eb->rhs, &b, skip, is_shift ? 0 : child_w))
            return r;

        /* Operate at the context width, never below the operands' own. */
        uint16_t w = child_w;
        if (a.width > w) w = a.width;
        if (!is_shift && b.width > w) w = b.width;
        uint64_t mask = _mask_for(w);
        uint64_t ua = (uint64_t)a.value & mask;
        uint64_t ub = (uint64_t)b.value & (is_shift ? ~(uint64_t)0 : mask);
        /* SystemVerilog signedness: an expression is signed only if EVERY
         * width-bearing operand is signed -- one unsigned operand makes the
         * whole comparison unsigned. `a.is_signed || b.is_signed` inverts that,
         * and `offset[i] <= max_offset - 1` (signed int32 vs an unsigned u32
         * whose value has the top bit set) was then evaluated as
         * `755852753 <= -576831748` and reported violated on a good model.
         *
         * A constant is unsized, so it does not get a vote unless it is the only
         * thing there -- otherwise `z < 0` over a signed z would be dragged
         * unsigned by the literal and read -2 as a huge positive. */
        int sgn;
        if (a.width && b.width)  sgn = a.is_signed && b.is_signed;
        else if (a.width)        sgn = a.is_signed;
        else if (b.width)        sgn = b.is_signed;
        else                     sgn = a.is_signed || b.is_signed;
        switch (eb->op) {
        case BIN_ADD: r.value = (int64_t)((ua + ub) & mask); r.width = w; r.is_signed = sgn; return r;
        case BIN_SUB: r.value = (int64_t)((ua - ub) & mask); r.width = w; r.is_signed = sgn; return r;
        case BIN_MUL: r.value = (int64_t)((ua * ub) & mask); r.width = w; r.is_signed = sgn; return r;
        case BIN_DIV:
            /* Signed: SV-truncated division (toward zero). C99 '/' truncates
             * toward zero, so operate on the sign-extended values directly. */
            if (sgn) {
                int64_t sb = _sext(ub, w);
                if (sb == 0) { *skip = 1; return r; }
                int64_t q = _sext(ua, w) / sb;
                r.value = (int64_t)((uint64_t)q & mask); r.width = w; r.is_signed = 1; return r;
            }
            if (ub == 0) { *skip = 1; return r; }
            r.value = (int64_t)(ua / ub); r.width = w; return r;
        case BIN_MOD:
            /* Signed: SV remainder, sign of dividend. C99 '%' gives the
             * sign of the dividend, matching SV semantics. */
            if (sgn) {
                int64_t sb = _sext(ub, w);
                if (sb == 0) { *skip = 1; return r; }
                int64_t rem = _sext(ua, w) % sb;
                r.value = (int64_t)((uint64_t)rem & mask); r.width = w; r.is_signed = 1; return r;
            }
            if (ub == 0) { *skip = 1; return r; }
            r.value = (int64_t)(ua % ub); r.width = w; return r;
        case BIN_BAND: r.value = (int64_t)(ua & ub); r.width = w; return r;
        case BIN_BOR:  r.value = (int64_t)(ua | ub); r.width = w; return r;
        case BIN_BXOR: r.value = (int64_t)(ua ^ ub); r.width = w; return r;
        case BIN_LSHIFT:
            r.value = (ub >= 64) ? 0 : (int64_t)((ua << ub) & mask);
            r.width = w; r.is_signed = a.is_signed; return r;
        case BIN_RSHIFT:
            r.value = (ub >= 64) ? 0 : (int64_t)(ua >> ub);
            r.width = w; r.is_signed = a.is_signed; return r;
        /* EQ/NEQ are sign-agnostic: at a common width, equal bit patterns are
         * equal values under either interpretation. The ORDER-sensitive
         * comparisons are not -- comparing raw patterns reads a signed var's
         * negative value as a huge positive one, which is how `z == -2` came to
         * be reported as violating `z < 0`. */
        case BIN_EQ:  r.value = (ua == ub); r.width = 1; return r;
        case BIN_NEQ: r.value = (ua != ub); r.width = 1; return r;
        case BIN_LT:  r.value = sgn ? (_sext(ua, w) <  _sext(ub, w)) : (ua <  ub); r.width = 1; return r;
        case BIN_LTE: r.value = sgn ? (_sext(ua, w) <= _sext(ub, w)) : (ua <= ub); r.width = 1; return r;
        case BIN_GT:  r.value = sgn ? (_sext(ua, w) >  _sext(ub, w)) : (ua >  ub); r.width = 1; return r;
        case BIN_GTE: r.value = sgn ? (_sext(ua, w) >= _sext(ub, w)) : (ua >= ub); r.width = 1; return r;
        case BIN_AND: r.value = ((a.value != 0) && (b.value != 0)); r.width = 1; return r;
        case BIN_OR:  r.value = ((a.value != 0) || (b.value != 0)); r.width = 1; return r;
        default: *skip = 1; return r;
        }
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal a;
        if (!_eval_ok(ctx, sp, eu->operand, &a, skip, ctx_w)) return r;
        switch (eu->op) {
        case UN_NEG:    r.value = -a.value; r.width = a.width; return r;
        case UN_NOT:    r.value = (a.value == 0); r.width = 1; return r;
        case UN_INVERT: {
            uint64_t mask = _mask_for(a.width);
            r.value = (int64_t)((~(uint64_t)a.value) & mask);
            r.width = a.width;
            return r;
        }
        default: *skip = 1; return r;
        }
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal c;
        if (!_eval_ok(ctx, sp, ei->cond, &c, skip, 0)) return r;
        return _eval(ctx, sp, c.value != 0 ? ei->then_e : ei->else_e, skip, ctx_w);
    }
    case EXPR_EXTEND: {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal a;
        if (!_eval_ok(ctx, sp, ee->operand, &a, skip, 0)) return r;
        uint64_t ua = (uint64_t)a.value & _mask_for(ee->from_bits);
        if (ee->sign_extend && ee->from_bits > 0 && ee->from_bits < 64) {
            uint64_t sign_bit = (uint64_t)1 << (ee->from_bits - 1);
            if (ua & sign_bit) {
                uint64_t to_mask = _mask_for(ee->to_bits);
                ua |= (~_mask_for(ee->from_bits)) & to_mask;
            }
        }
        r.value = (int64_t)ua;
        r.width = ee->to_bits;
        return r;
    }
    case EXPR_EXTRACT: {
        ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal a;
        if (!_eval_ok(ctx, sp, ex->operand, &a, skip, 0)) return r;
        uint64_t ua = (uint64_t)a.value;
        ua >>= ex->lo_bit;
        uint16_t w = (uint16_t)(ex->hi_bit - ex->lo_bit + 1);
        r.value = (int64_t)(ua & _mask_for(w));
        r.width = w;
        return r;
    }
    case EXPR_CONCAT: {
        ExprConcat *ec = (ExprConcat *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal hi, lo;
        if (!_eval_ok(ctx, sp, ec->hi, &hi, skip, 0)) return r;
        if (!_eval_ok(ctx, sp, ec->lo, &lo, skip, 0)) return r;
        uint64_t lo_mask = _mask_for(ec->lo_width);
        r.value = (int64_t)((((uint64_t)hi.value) << ec->lo_width) |
                            ((uint64_t)lo.value & lo_mask));
        r.width = (uint16_t)(hi.width + ec->lo_width);
        return r;
    }
    /* Constructs we don't (yet) evaluate: skip rather than misreport. */
    case EXPR_IN_RANGE:
    case EXPR_IN_SET:
    case EXPR_IN_RANGES:
    case EXPR_SUM:
    case EXPR_COUNTONES:
    case EXPR_CLOG2:
    case EXPR_ARRAY_SELECT:
    default:
        *skip = 1;
        return r;
    }
}

/* Compact expression-tree dumper for diagnostics. Prints a single-line
 * s-expression with var values inlined; truncated at depth 8. */
static void _dump_expr(const SolveCtx *ctx, const SolveProblem *sp,
                        ExprRef ref, FILE *err, int depth) {
    if (depth > 16) { fprintf(err, "..."); return; }
    if (ref == EXPR_NULL) { fprintf(err, "<null>"); return; }
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    switch (k) {
    case EXPR_CONST: {
        ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, ref);
        fprintf(err, "%lld", (long long)ec->value);
        return;
    }
    case EXPR_VAR: {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        if (ev->var_id < ctx->n_vars) {
            uint32_t resolved = ev->var_id;
            if (ctx->var_alias) {
                while (ctx->var_alias[resolved] != resolved)
                    resolved = ctx->var_alias[resolved];
            }
            const Variable *v = &ctx->vars[resolved];
            fprintf(err, "v%u%s[w%u]=%lld", ev->var_id,
                    resolved != ev->var_id ? "*" : "",
                    ctx->vars[ev->var_id].width,
                    (long long)var_lo64(ctx, v));
        } else {
            fprintf(err, "v%u<oob>", ev->var_id);
        }
        return;
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        static const char *names[] = {
            "+","-","*","/","%","&","|","^","<<",">>",
            "==","!=","<","<=",">",">=","and","or",
        };
        const char *n = (eb->op < (int)(sizeof(names)/sizeof(names[0])))
                        ? names[eb->op] : "?";
        fprintf(err, "(%s ", n);
        _dump_expr(ctx, sp, eb->lhs, err, depth+1);
        fprintf(err, " ");
        _dump_expr(ctx, sp, eb->rhs, err, depth+1);
        fprintf(err, ")");
        return;
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        static const char *names[] = { "neg", "not", "~" };
        const char *n = (eu->op < 3) ? names[eu->op] : "?";
        fprintf(err, "(%s ", n);
        _dump_expr(ctx, sp, eu->operand, err, depth+1);
        fprintf(err, ")");
        return;
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)zsp_pool_ptr(&sp->pool, ref);
        fprintf(err, "(ite ");
        _dump_expr(ctx, sp, ei->cond, err, depth+1);
        fprintf(err, " ");
        _dump_expr(ctx, sp, ei->then_e, err, depth+1);
        fprintf(err, " ");
        _dump_expr(ctx, sp, ei->else_e, err, depth+1);
        fprintf(err, ")");
        return;
    }
    case EXPR_EXTEND: {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        fprintf(err, "(%sext[%u->%u] ", ee->sign_extend ? "s" : "z",
                ee->from_bits, ee->to_bits);
        _dump_expr(ctx, sp, ee->operand, err, depth+1);
        fprintf(err, ")");
        return;
    }
    case EXPR_EXTRACT: {
        ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ref);
        fprintf(err, "(extract[%u:%u] ", ex->hi_bit, ex->lo_bit);
        _dump_expr(ctx, sp, ex->operand, err, depth+1);
        fprintf(err, ")");
        return;
    }
    case EXPR_CONCAT: {
        ExprConcat *ec = (ExprConcat *)zsp_pool_ptr(&sp->pool, ref);
        fprintf(err, "(concat[lo%u] ", ec->lo_width);
        _dump_expr(ctx, sp, ec->hi, err, depth+1);
        fprintf(err, " ");
        _dump_expr(ctx, sp, ec->lo, err, depth+1);
        fprintf(err, ")");
        return;
    }
    default:
        fprintf(err, "<kind=%d>", k);
        return;
    }
}

int solver_validate_model(SolveCtx *ctx, SolveProblem *sp, FILE *err) {
    if (!ctx || !sp) return 0;
    int violations = 0;
    ExprRef cref = sp->constraints_head;
    uint32_t idx = 0;
    while (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)zsp_pool_ptr(&sp->pool, cref);
        int skip = 0;
        EvalVal v = _eval(ctx, sp, cs->root, &skip, 0);
        if (!skip && v.value == 0) {
            violations++;
            if (err) {
                ExprKind rk = *(ExprKind *)zsp_pool_ptr(&sp->pool, cs->root);
                int op = -1;
                if (rk == EXPR_BINARY) op = ((ExprBinary *)zsp_pool_ptr(&sp->pool, cs->root))->op;
                else if (rk == EXPR_UNARY) op = ((ExprUnary *)zsp_pool_ptr(&sp->pool, cs->root))->op;
                fprintf(err,
                    "model-validation: constraint #%u (id=%u, kind=%d op=%d) violated: ",
                    idx, cs->constraint_id, rk, op);
                _dump_expr(ctx, sp, cs->root, err, 0);
                fprintf(err, "\n");
            }
        }
        cref = cs->next;
        idx++;
    }
    return violations;
}
