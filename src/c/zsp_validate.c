/*
 * solver_validate_model — post-solve sanity check.
 *
 * Walks every top-level ConstraintSpec in the original SolveProblem and
 * evaluates its root expression under the current variable assignment.
 * A 0 value means the constraint is violated; this catches silent
 * constraint-drop bugs that the compile path would otherwise hide
 * behind a SOLVE_OK result.
 *
 * Expressions are evaluated under the builder API's SystemVerilog sizing
 * and signedness rules (zsp_sv.h), independently of the engines (which run on
 * the SV-elaborated problem). Expressions involving arrays / sums /
 * countones / clog2, a division by zero, or a type wider than 64 bits are
 * skipped (skip flag) rather than misreported.
 */
#include <stdint.h>
#include <stdio.h>
#include "zsp_ctx.h"
#include "zsp_problem.h"
#include "zsp_sv.h"

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


/* ------------------------------------------------------------------ */
/* SystemVerilog evaluation (IEEE 1800 sizing and signedness)          */
/*                                                                     */
/* An INDEPENDENT implementation of the rules in zsp_sv.h: the engines */
/* run on the elaborated problem, the validator evaluates the problem  */
/* the caller built. Every value below is a 2's-complement number of a */
/* known (width, signedness) type, held already wrapped to that type.  */
/* A constraint that needs a wider type than the evaluator holds,      */
/* divides by zero, or uses a construct it does not model is SKIPPED,  */
/* never reported.                                                     */
/* ------------------------------------------------------------------ */

/* Values are held in the widest integer the compiler offers: 128 bits where
 * available (so a 65..128-bit SMT-LIB constant or context still evaluates),
 * else 64. A type wider than that is SKIPPED. */
#if defined(__SIZEOF_INT128__)
typedef __int128          VI;
typedef unsigned __int128 VU;
#define V_MAXW 128
#else
typedef int64_t           VI;
typedef uint64_t          VU;
#define V_MAXW 64
#endif

typedef struct { uint16_t w; uint8_t s; } VTy;

static VU _mask_for(uint16_t w) {
    if (w == 0 || w >= V_MAXW) return ~(VU)0;
    return ((VU)1 << w) - 1;
}

/* The value of pattern `u` read as a `w`-bit number of signedness `s`. */
static VI _wrap(VU u, uint16_t w, uint8_t s) {
    if (w == 0 || w >= V_MAXW) return (VI)u;
    VU m = ((VU)1 << w) - 1;
    u &= m;
    if (s && ((u >> (w - 1)) & 1u)) u |= ~m;
    return (VI)u;
}

static const Variable *_var_of(const SolveCtx *ctx, uint32_t id) {
    uint32_t r = id;
    if (ctx->var_alias) {
        while (ctx->var_alias[r] != r) r = ctx->var_alias[r];
    }
    return &ctx->vars[r];
}

static int _vis_arith(BinOp op) {
    switch (op) {
    case BIN_ADD: case BIN_SUB: case BIN_MUL: case BIN_DIV: case BIN_MOD:
    case BIN_BAND: case BIN_BOR: case BIN_BXOR:
        return 1;
    default:
        return 0;
    }
}

/* Self-determined type of a node. */
static VTy _vtype(const SolveCtx *ctx, const SolveProblem *sp, ExprRef ref,
                  int *skip) {
    VTy t = { 1, 0 };
    if (ref == EXPR_NULL) { *skip = 1; return t; }
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    switch (k) {
    case EXPR_VAR: {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        if (ev->var_id >= ctx->n_vars) { *skip = 1; return t; }
        t.w = ctx->vars[ev->var_id].width;
        t.s = (ctx->vars[ev->var_id].flags & VAR_SIGNED) ? 1 : 0;
        break;
    }
    case EXPR_CONST: {
        uint16_t w; uint8_t s;
        zsp_sv_const_type((ExprConst *)zsp_pool_ptr(&sp->pool, ref), &w, &s);
        t.w = w; t.s = s;
        break;
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        if (_vis_arith(eb->op)) {
            VTy a = _vtype(ctx, sp, eb->lhs, skip);
            VTy b = _vtype(ctx, sp, eb->rhs, skip);
            t.w = a.w > b.w ? a.w : b.w;
            t.s = (uint8_t)(a.s && b.s);
        } else if (eb->op == BIN_LSHIFT || eb->op == BIN_RSHIFT) {
            t = _vtype(ctx, sp, eb->lhs, skip);
        }
        break;
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        if (eu->op != UN_NOT) t = _vtype(ctx, sp, eu->operand, skip);
        break;
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)zsp_pool_ptr(&sp->pool, ref);
        VTy a = _vtype(ctx, sp, ei->then_e, skip);
        VTy b = _vtype(ctx, sp, ei->else_e, skip);
        t.w = a.w > b.w ? a.w : b.w;
        t.s = (uint8_t)(a.s && b.s);
        break;
    }
    case EXPR_EXTEND: {
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        t.w = ee->to_bits;
        t.s = _vtype(ctx, sp, ee->operand, skip).s;
        break;
    }
    case EXPR_SV_CAST: {
        ExprSvCast *ec = (ExprSvCast *)zsp_pool_ptr(&sp->pool, ref);
        t.w = ec->to_bits;
        t.s = ec->dst_signed ? 1 : 0;
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ref);
        t.w = (uint16_t)(ex->hi_bit - ex->lo_bit + 1);
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat *ec = (ExprConcat *)zsp_pool_ptr(&sp->pool, ref);
        t.w = (uint16_t)(_vtype(ctx, sp, ec->hi, skip).w + ec->lo_width);
        break;
    }
    default:
        break;      /* comparisons, connectives, membership: 1-bit unsigned */
    }
    if (t.w == 0) t.w = 1;
    return t;
}

static VI _ev(const SolveCtx *ctx, const SolveProblem *sp, ExprRef ref,
              uint16_t W, uint8_t S, int *skip);

/* Truth value of a node evaluated in its own type. */
static int _truth(const SolveCtx *ctx, const SolveProblem *sp, ExprRef ref,
                  int *skip) {
    VTy t = _vtype(ctx, sp, ref, skip);
    if (*skip) return 0;
    if (t.w > V_MAXW) { *skip = 1; return 0; }
    return _ev(ctx, sp, ref, t.w, t.s, skip) != 0;
}

/* `l op r` for a comparison op, in the comparison's own context. */
static int _cmp(const SolveCtx *ctx, const SolveProblem *sp, BinOp op,
                ExprRef l, ExprRef r, int *skip) {
    VTy a = _vtype(ctx, sp, l, skip);
    VTy b = _vtype(ctx, sp, r, skip);
    if (*skip) return 0;
    uint16_t w = a.w > b.w ? a.w : b.w;
    uint8_t  s = (uint8_t)(a.s && b.s);
    if (w > V_MAXW) { *skip = 1; return 0; }
    VI x = _ev(ctx, sp, l, w, s, skip);
    VI y = _ev(ctx, sp, r, w, s, skip);
    if (*skip) return 0;
    VU ux = (VU)x & _mask_for(w), uy = (VU)y & _mask_for(w);
    switch (op) {
    case BIN_EQ:  return ux == uy;
    case BIN_NEQ: return ux != uy;
    case BIN_LT:  return s ? x <  y : ux <  uy;
    case BIN_LTE: return s ? x <= y : ux <= uy;
    case BIN_GT:  return s ? x >  y : ux >  uy;
    case BIN_GTE: return s ? x >= y : ux >= uy;
    default: *skip = 1; return 0;
    }
}

static VI _ev(const SolveCtx *ctx, const SolveProblem *sp, ExprRef ref,
              uint16_t W, uint8_t S, int *skip) {
    if (*skip) return 0;
    if (ref == EXPR_NULL || W > V_MAXW) { *skip = 1; return 0; }
    VU M = _mask_for(W);
    ExprKind k = *(ExprKind *)zsp_pool_ptr(&sp->pool, ref);
    switch (k) {
    case EXPR_CONST: {
        ExprConst *ec = (ExprConst *)zsp_pool_ptr(&sp->pool, ref);
        uint16_t cw; uint8_t cs;
        zsp_sv_const_type(ec, &cw, &cs);
        if (cw > V_MAXW) { *skip = 1; return 0; }
        /* The int64 `value` spells the constant's low 64 bits; a wider one is
         * sign-extended when signed, else zero-extended (the bit-blaster's
         * reading). Extend by the constant's own signedness, read per the
         * context. */
        VI own = cs ? (VI)ec->value : (VI)(VU)(uint64_t)ec->value;
        own = _wrap((VU)own, cw, cs);
        return _wrap((VU)own, W, S);
    }
    case EXPR_VAR: {
        ExprVar *ev = (ExprVar *)zsp_pool_ptr(&sp->pool, ref);
        if (ev->var_id >= ctx->n_vars) { *skip = 1; return 0; }
        const Variable *dv = &ctx->vars[ev->var_id];
        if (dv->width > 64) { *skip = 1; return 0; }
        uint8_t vs = (dv->flags & VAR_SIGNED) ? 1 : 0;
        int64_t v = var_lo64(ctx, _var_of(ctx, ev->var_id));
        /* An unsigned 64-bit var's bound is its bit pattern. */
        VI own = vs ? (VI)v : (VI)(VU)(uint64_t)v;
        own = _wrap((VU)own, dv->width, vs);
        return _wrap((VU)own, W, S);
    }
    case EXPR_BINARY: {
        ExprBinary *eb = (ExprBinary *)zsp_pool_ptr(&sp->pool, ref);
        BinOp op = eb->op;
        if (_vis_arith(op)) {
            VI a = _ev(ctx, sp, eb->lhs, W, S, skip);
            VI b = _ev(ctx, sp, eb->rhs, W, S, skip);
            if (*skip) return 0;
            VU ua = (VU)a & M, ub = (VU)b & M;
            switch (op) {
            case BIN_ADD:  return _wrap(ua + ub, W, S);
            case BIN_SUB:  return _wrap(ua - ub, W, S);
            case BIN_MUL:  return _wrap(ua * ub, W, S);
            case BIN_BAND: return _wrap(ua & ub, W, S);
            case BIN_BOR:  return _wrap(ua | ub, W, S);
            case BIN_BXOR: return _wrap(ua ^ ub, W, S);
            case BIN_DIV:
                if (ub == 0) { *skip = 1; return 0; }
                if (S) {
                    /* truncating; MIN / -1 traps in C and wraps here */
                    if (b == -1) return _wrap((VU)0 - (VU)a, W, S);
                    return _wrap((VU)(a / b), W, S);
                }
                return _wrap(ua / ub, W, S);
            case BIN_MOD:
                if (ub == 0) { *skip = 1; return 0; }
                if (S) {
                    if (b == -1) return 0;
                    return _wrap((VU)(a % b), W, S);   /* sign of dividend */
                }
                return _wrap(ua % ub, W, S);
            default: break;
            }
            *skip = 1; return 0;
        }
        if (op == BIN_LSHIFT || op == BIN_RSHIFT) {
            VI a = _ev(ctx, sp, eb->lhs, W, S, skip);
            VTy rt = _vtype(ctx, sp, eb->rhs, skip);
            if (*skip) return 0;
            if (rt.w > V_MAXW) { *skip = 1; return 0; }
            /* The amount is self-determined and read unsigned. */
            VU sh = (VU)_ev(ctx, sp, eb->rhs, rt.w, rt.s, skip) & _mask_for(rt.w);
            if (*skip) return 0;
            if (sh >= W) return 0;
            VU ua = (VU)a & M;
            /* `>>` is LOGICAL on the context-width pattern (SV >>, not >>>). */
            return _wrap(op == BIN_LSHIFT ? (ua << (int)sh) : (ua >> (int)sh), W, S);
        }
        switch (op) {
        case BIN_EQ: case BIN_NEQ: case BIN_LT:
        case BIN_LTE: case BIN_GT: case BIN_GTE:
            return _cmp(ctx, sp, op, eb->lhs, eb->rhs, skip);
        case BIN_AND: {
            int l = _truth(ctx, sp, eb->lhs, skip);
            int r = _truth(ctx, sp, eb->rhs, skip);
            return l && r;
        }
        case BIN_OR: {
            int l = _truth(ctx, sp, eb->lhs, skip);
            int r = _truth(ctx, sp, eb->rhs, skip);
            return l || r;
        }
        default:
            *skip = 1; return 0;
        }
    }
    case EXPR_UNARY: {
        ExprUnary *eu = (ExprUnary *)zsp_pool_ptr(&sp->pool, ref);
        if (eu->op == UN_NOT) return !_truth(ctx, sp, eu->operand, skip);
        VI a = _ev(ctx, sp, eu->operand, W, S, skip);
        if (*skip) return 0;
        if (eu->op == UN_NEG)    return _wrap((VU)0 - (VU)a, W, S);
        if (eu->op == UN_INVERT) return _wrap(~(VU)a, W, S);
        *skip = 1; return 0;
    }
    case EXPR_ITE: {
        ExprITE *ei = (ExprITE *)zsp_pool_ptr(&sp->pool, ref);
        int c = _truth(ctx, sp, ei->cond, skip);
        if (*skip) return 0;
        return _ev(ctx, sp, c ? ei->then_e : ei->else_e, W, S, skip);
    }
    case EXPR_EXTEND:
    case EXPR_SV_CAST: {
        /* Same layout: the operand's low from_bits bits, extended to to_bits,
         * typed (to_bits, operand signedness) for an extend and (to_bits,
         * dst_signed) for a cast. */
        ExprExtend *ee = (ExprExtend *)zsp_pool_ptr(&sp->pool, ref);
        VTy ot = _vtype(ctx, sp, ee->operand, skip);
        if (*skip) return 0;
        if (ot.w > V_MAXW || ee->to_bits > V_MAXW || ee->from_bits == 0) {
            *skip = 1; return 0;
        }
        uint8_t own_s = (k == EXPR_EXTEND) ? ot.s
                      : (((ExprSvCast *)(void *)ee)->dst_signed ? 1 : 0);
        VI o = _ev(ctx, sp, ee->operand, ot.w, ot.s, skip);
        if (*skip) return 0;
        VU bits = (VU)_wrap((VU)o, ee->from_bits, ee->sign_extend ? 1 : 0);
        if (!ee->sign_extend) bits &= _mask_for(ee->from_bits);
        return _wrap((VU)_wrap(bits, ee->to_bits, own_s), W, S);
    }
    case EXPR_EXTRACT: {
        ExprExtract *ex = (ExprExtract *)zsp_pool_ptr(&sp->pool, ref);
        VTy ot = _vtype(ctx, sp, ex->operand, skip);
        if (*skip) return 0;
        if (ot.w > V_MAXW) { *skip = 1; return 0; }
        VI o = _ev(ctx, sp, ex->operand, ot.w, ot.s, skip);
        if (*skip) return 0;
        uint16_t w = (uint16_t)(ex->hi_bit - ex->lo_bit + 1);
        VI sh = ex->lo_bit < V_MAXW ? (o >> ex->lo_bit) : (o < 0 ? -1 : 0);
        return _wrap((VU)_wrap((VU)sh, w, 0), W, S);
    }
    case EXPR_CONCAT: {
        ExprConcat *ec = (ExprConcat *)zsp_pool_ptr(&sp->pool, ref);
        VTy ht = _vtype(ctx, sp, ec->hi, skip);
        VTy lt = _vtype(ctx, sp, ec->lo, skip);
        if (*skip) return 0;
        if (ht.w + ec->lo_width > V_MAXW || lt.w > V_MAXW) { *skip = 1; return 0; }
        VI h = _ev(ctx, sp, ec->hi, ht.w, ht.s, skip);
        VI l = _ev(ctx, sp, ec->lo, lt.w, lt.s, skip);
        if (*skip) return 0;
        VU own = (((VU)h & _mask_for(ht.w)) << ec->lo_width)
               | ((VU)l & _mask_for(ec->lo_width));
        return _wrap((VU)_wrap(own, (uint16_t)(ht.w + ec->lo_width), 0), W, S);
    }
    case EXPR_IN_RANGE: {
        /* Each comparison is its own SV context. */
        ExprInRange *ir = (ExprInRange *)zsp_pool_ptr(&sp->pool, ref);
        int a = _cmp(ctx, sp, BIN_GTE, ir->value, ir->lo, skip);
        int b = _cmp(ctx, sp, BIN_LTE, ir->value, ir->hi, skip);
        return a && b;
    }
    case EXPR_IN_SET: {
        ExprInSet *is = (ExprInSet *)zsp_pool_ptr(&sp->pool, ref);
        const ExprRef *el = (const ExprRef *)(is + 1);
        for (uint32_t i = 0; i < is->n_elems; i++) {
            if (_cmp(ctx, sp, BIN_EQ, is->value, el[i], skip)) return 1;
            if (*skip) return 0;
        }
        return 0;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *ir = (ExprInRanges *)zsp_pool_ptr(&sp->pool, ref);
        const ExprRef *los = (const ExprRef *)(ir + 1);
        const ExprRef *his = los + ir->n_ranges;
        for (uint32_t i = 0; i < ir->n_ranges; i++) {
            if (_cmp(ctx, sp, BIN_GTE, ir->value, los[i], skip) &&
                _cmp(ctx, sp, BIN_LTE, ir->value, his[i], skip)) return 1;
            if (*skip) return 0;
        }
        return 0;
    }
    /* Constructs we don't evaluate: skip rather than misreport. */
    case EXPR_SUM:
    case EXPR_COUNTONES:
    case EXPR_CLOG2:
    case EXPR_ARRAY_SELECT:
    default:
        *skip = 1;
        return 0;
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
        int holds = _truth(ctx, sp, cs->root, &skip);
        if (!skip && !holds) {
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
