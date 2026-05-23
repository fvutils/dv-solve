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
} EvalVal;

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

static EvalVal _eval(const SolveCtx *ctx, const SolveProblem *sp,
                      ExprRef ref, int *skip);

/* Try-call helper: returns 1 on success, 0 if skip was set. */
static int _eval_ok(const SolveCtx *ctx, const SolveProblem *sp,
                     ExprRef ref, EvalVal *out, int *skip) {
    *out = _eval(ctx, sp, ref, skip);
    return *skip == 0;
}

static EvalVal _eval(const SolveCtx *ctx, const SolveProblem *sp,
                      ExprRef ref, int *skip) {
    EvalVal r = { 0, 0 };
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
        return r;
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal a, b;
        if (!_eval_ok(ctx, sp, eb->lhs, &a, skip)) return r;
        if (!_eval_ok(ctx, sp, eb->rhs, &b, skip)) return r;
        uint16_t w = a.width > b.width ? a.width : b.width;
        uint64_t mask = _mask_for(w);
        uint64_t ua = (uint64_t)a.value & mask;
        uint64_t ub = (uint64_t)b.value & mask;
        switch (eb->op) {
        case BIN_ADD: r.value = (int64_t)((ua + ub) & mask); r.width = w; return r;
        case BIN_SUB: r.value = (int64_t)((ua - ub) & mask); r.width = w; return r;
        case BIN_MUL: r.value = (int64_t)((ua * ub) & mask); r.width = w; return r;
        case BIN_DIV:
            if (ub == 0) { *skip = 1; return r; }
            r.value = (int64_t)(ua / ub); r.width = w; return r;
        case BIN_MOD:
            if (ub == 0) { *skip = 1; return r; }
            r.value = (int64_t)(ua % ub); r.width = w; return r;
        case BIN_BAND: r.value = (int64_t)(ua & ub); r.width = w; return r;
        case BIN_BOR:  r.value = (int64_t)(ua | ub); r.width = w; return r;
        case BIN_BXOR: r.value = (int64_t)(ua ^ ub); r.width = w; return r;
        case BIN_LSHIFT:
            if (ub >= 64) r.value = 0;
            else r.value = (int64_t)((ua << ub) & mask);
            r.width = w; return r;
        case BIN_RSHIFT:
            if (ub >= 64) r.value = 0;
            else r.value = (int64_t)(ua >> ub);
            r.width = w; return r;
        case BIN_EQ:  r.value = (ua == ub); r.width = 1; return r;
        case BIN_NEQ: r.value = (ua != ub); r.width = 1; return r;
        case BIN_LT:  r.value = (ua <  ub); r.width = 1; return r;
        case BIN_LTE: r.value = (ua <= ub); r.width = 1; return r;
        case BIN_GT:  r.value = (ua >  ub); r.width = 1; return r;
        case BIN_GTE: r.value = (ua >= ub); r.width = 1; return r;
        case BIN_AND: r.value = ((a.value != 0) && (b.value != 0)); r.width = 1; return r;
        case BIN_OR:  r.value = ((a.value != 0) || (b.value != 0)); r.width = 1; return r;
        default: *skip = 1; return r;
        }
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal a;
        if (!_eval_ok(ctx, sp, eu->operand, &a, skip)) return r;
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
        if (!_eval_ok(ctx, sp, ei->cond, &c, skip)) return r;
        return _eval(ctx, sp, c.value != 0 ? ei->then_e : ei->else_e, skip);
    }
    case EXPR_EXTEND: {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        EvalVal a;
        if (!_eval_ok(ctx, sp, ee->operand, &a, skip)) return r;
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
        if (!_eval_ok(ctx, sp, ex->operand, &a, skip)) return r;
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
        if (!_eval_ok(ctx, sp, ec->hi, &hi, skip)) return r;
        if (!_eval_ok(ctx, sp, ec->lo, &lo, skip)) return r;
        uint64_t lo_mask = _mask_for(ec->lo_width);
        r.value = (int64_t)((((uint64_t)hi.value) << ec->lo_width) |
                            ((uint64_t)lo.value & lo_mask));
        r.width = (uint16_t)(hi.width + ec->lo_width);
        return r;
    }
    /* Constructs we don't (yet) evaluate: skip rather than misreport. */
    case EXPR_IN_RANGE:
    case EXPR_IN_SET:
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
        EvalVal v = _eval(ctx, sp, cs->root, &skip);
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
