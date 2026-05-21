#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "smt2/smt2_frontend.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define CTX_BUF_SIZE  (4u * 1024u * 1024u)
#define BA_BLOCK_SIZE (64u * 1024u)

static const TypedExpr TYPED_NULL = { EXPR_NULL, 0 };

/* ------------------------------------------------------------------ */
/* Helpers: symbol table                                               */
/* ------------------------------------------------------------------ */

static int _add_var(Smt2Frontend *fe, const char *name, uint32_t len,
                    uint32_t var_id, uint8_t width) {
    if (fe->n_vars == fe->vars_cap) {
        uint32_t newcap = fe->vars_cap ? fe->vars_cap * 2 : 16;
        Smt2Var *tmp = (Smt2Var *)realloc(fe->vars, newcap * sizeof(Smt2Var));
        if (!tmp) return -1;
        fe->vars     = tmp;
        fe->vars_cap = newcap;
    }
    Smt2Var *v = &fe->vars[fe->n_vars++];
    uint32_t copy_len = len < SMT2_MAX_NAME - 1 ? len : SMT2_MAX_NAME - 1;
    memcpy(v->name, name, copy_len);
    v->name[copy_len] = '\0';
    v->var_id   = var_id;
    v->width    = width;
    v->is_signed = 0;
    v->_pad[0] = v->_pad[1] = 0;
    return 0;
}

static Smt2Var *_find_var(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < fe->n_vars; i++) {
        if (strlen(fe->vars[i].name) == len &&
            memcmp(fe->vars[i].name, name, len) == 0)
            return &fe->vars[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Auxiliary variable creation                                         */
/* ------------------------------------------------------------------ */

static uint32_t _next_var_id(Smt2Frontend *fe) {
    /* var IDs grow from n_vars; we track the next available one */
    return fe->n_vars;
}

/**
 * Create a fresh auxiliary variable with the given width and full
 * unsigned domain, returning its var_id.  The variable is added to
 * both the builder and the symbol table (with an internal name).
 */
static uint32_t _fresh_aux(Smt2Frontend *fe, uint16_t width) {
    uint32_t var_id = _next_var_id(fe);
    uint64_t max_val = (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);
    builder_add_var(fe->builder, var_id, (uint8_t)width, 0, 0, (int64_t)max_val);

    /* Add to symbol table with a synthetic name */
    char name[SMT2_MAX_NAME];
    snprintf(name, sizeof(name), "__aux%u", var_id);
    _add_var(fe, name, (uint32_t)strlen(name), var_id, (uint8_t)width);

    return var_id;
}

/**
 * If te is already a variable reference, return it unchanged.
 * Otherwise, create a fresh aux variable, emit (= aux expr), and
 * return a TypedExpr pointing to the aux variable.
 */
static TypedExpr _ensure_var(Smt2Frontend *fe, TypedExpr te) {
    if (te.ref == EXPR_NULL) return TYPED_NULL;

    /* Check if it's already a const -- we can use it directly in
     * var-const constraint patterns */
    /* We always flatten to a variable for nested expressions.
     * The builder expression could be a var, const, or complex.
     * We check by peeking at the ExprKind -- but we can't peek into
     * the builder's block-list easily. Instead, we track this via
     * a simple heuristic: if the TypedExpr was built from a variable
     * symbol or a constant, we flag it. For simplicity, always flatten
     * non-leaf nodes by creating aux vars. */

    /* Actually, we can't introspect builder refs. The simplest correct
     * approach: every call site explicitly marks whether the result
     * needs flattening. See _translate_expr below. */
    return te;
}

/* ------------------------------------------------------------------ */
/* Sort parsing                                                        */
/* ------------------------------------------------------------------ */

static uint8_t _parse_bitvec_sort(Smt2Frontend *fe, const Sexpr *sort) {
    (void)fe;
    if (!sort || sort->kind != SEXPR_LIST || sort->list.count != 3)
        return 0;
    if (!sexpr_is_symbol(sort->list.items[0], "_")) return 0;
    if (!sexpr_is_symbol(sort->list.items[1], "BitVec")) return 0;
    if (sort->list.items[2]->kind != SEXPR_NUMERAL) return 0;
    uint64_t w = sort->list.items[2]->numval;
    if (w == 0 || w > 64) return 0;
    return (uint8_t)w;
}

/* ------------------------------------------------------------------ */
/* Parse (_ bvN W) symbol                                              */
/* ------------------------------------------------------------------ */

static int _parse_bv_sym(const Sexpr *sym, uint64_t *val_out) {
    if (sym->kind != SEXPR_SYMBOL) return 0;
    if (sym->sym.len < 3) return 0;
    if (sym->sym.str[0] != 'b' || sym->sym.str[1] != 'v') return 0;
    uint64_t v = 0;
    for (uint32_t i = 2; i < sym->sym.len; i++) {
        char c = sym->sym.str[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (uint64_t)(c - '0');
    }
    *val_out = v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Expression translation with three-address flattening                */
/*                                                                     */
/* Every BV sub-expression is flattened to a variable reference.       */
/* The translator recursively processes sub-expressions, and for any   */
/* non-leaf (non-var, non-const) result, it creates an auxiliary       */
/* variable and emits a constraint tying them together.                */
/*                                                                     */
/* "is_leaf" on a TypedExpr means it's either a builder_expr_var or    */
/* builder_expr_const result -- safe to use as an operand in a binary  */
/* constraint that the solver can compile.                             */
/* ------------------------------------------------------------------ */

static TypedExpr _translate_expr(Smt2Frontend *fe, const Sexpr *s);

/**
 * Flatten a TypedExpr to a variable if it is a complex expression.
 * leaf_kind: 0 = unknown/complex, 1 = var, 2 = const
 */
typedef struct {
    TypedExpr te;
    int       leaf_kind; /* 0=complex, 1=var, 2=const */
} TaggedExpr;

static const TaggedExpr TAGGED_NULL = { { EXPR_NULL, 0 }, 0 };

static TaggedExpr _translate_tagged(Smt2Frontend *fe, const Sexpr *s);

/**
 * Ensure a tagged expression is a variable by introducing an aux var
 * if it's complex.  Constants are OK as-is since the compiler handles
 * var-const patterns.
 */
static TaggedExpr _flatten_to_var(Smt2Frontend *fe, TaggedExpr tg) {
    if (tg.te.ref == EXPR_NULL) return TAGGED_NULL;
    if (tg.leaf_kind == 1 || tg.leaf_kind == 2) return tg;

    /* Complex expression -- create aux var and constrain it */
    uint32_t aux_id = _fresh_aux(fe, tg.te.width);
    ExprRef aux_ref = builder_expr_var(fe->builder, aux_id);
    ExprRef eq = builder_expr_binary(fe->builder, BIN_EQ, aux_ref, tg.te.ref);
    builder_add_constraint(fe->builder, eq);

    TaggedExpr result;
    result.te.ref   = aux_ref;
    result.te.width = tg.te.width;
    result.leaf_kind = 1;
    return result;
}

/* ---- Symbol translation ---- */

static TaggedExpr _translate_symbol_tagged(Smt2Frontend *fe, const Sexpr *s) {
    if (sexpr_is_symbol(s, "true")) {
        ExprRef r = builder_expr_const(fe->builder, 1, 0);
        return (TaggedExpr){ { r, 1 }, 2 };
    }
    if (sexpr_is_symbol(s, "false")) {
        ExprRef r = builder_expr_const(fe->builder, 0, 0);
        return (TaggedExpr){ { r, 1 }, 2 };
    }
    Smt2Var *v = _find_var(fe, s->sym.str, s->sym.len);
    if (!v) {
        fprintf(fe->err, "error: unknown variable '%.*s'\n",
                (int)s->sym.len, s->sym.str);
        return TAGGED_NULL;
    }
    ExprRef r = builder_expr_var(fe->builder, v->var_id);
    return (TaggedExpr){ { r, v->width }, 1 };
}

/* ---- List expression translation ---- */

static TaggedExpr _translate_list_tagged(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count == 0) return TAGGED_NULL;

    Sexpr *head = s->list.items[0];

    /* (_ bvN W) */
    if (sexpr_is_symbol(head, "_")) {
        if (s->list.count < 3) return TAGGED_NULL;
        Sexpr *op = s->list.items[1];
        uint64_t bv_val;
        if (_parse_bv_sym(op, &bv_val)) {
            if (s->list.items[2]->kind != SEXPR_NUMERAL) return TAGGED_NULL;
            uint16_t w = (uint16_t)s->list.items[2]->numval;
            ExprRef r = builder_expr_const(fe->builder, (int64_t)bv_val, 0);
            return (TaggedExpr){ { r, w }, 2 };
        }
        fprintf(fe->err, "error: unexpected indexed identifier\n");
        return TAGGED_NULL;
    }

    /* ((_ zero_extend N) expr) etc */
    if (head->kind == SEXPR_LIST && head->list.count >= 3 &&
        sexpr_is_symbol(head->list.items[0], "_")) {

        Sexpr *op_sym = head->list.items[1];

        if (sexpr_is_symbol(op_sym, "zero_extend") ||
            sexpr_is_symbol(op_sym, "sign_extend")) {
            if (s->list.count != 2) return TAGGED_NULL;
            uint8_t sign = sexpr_is_symbol(op_sym, "sign_extend") ? 1 : 0;
            uint64_t ext_n = head->list.items[2]->numval;

            TaggedExpr inner = _translate_tagged(fe, s->list.items[1]);
            inner = _flatten_to_var(fe, inner);
            if (inner.te.ref == EXPR_NULL) return TAGGED_NULL;

            uint16_t new_width = inner.te.width + (uint16_t)ext_n;
            ExprRef r = builder_expr_extend(fe->builder, inner.te.ref,
                                            (uint8_t)inner.te.width,
                                            (uint8_t)new_width, sign);
            return (TaggedExpr){ { r, new_width }, 0 }; /* complex */
        }

        if (sexpr_is_symbol(op_sym, "extract")) {
            if (s->list.count != 2 || head->list.count != 4) return TAGGED_NULL;
            uint8_t hi = (uint8_t)head->list.items[2]->numval;
            uint8_t lo = (uint8_t)head->list.items[3]->numval;

            TaggedExpr inner = _translate_tagged(fe, s->list.items[1]);
            inner = _flatten_to_var(fe, inner);
            if (inner.te.ref == EXPR_NULL) return TAGGED_NULL;

            ExprRef r = builder_expr_extract(fe->builder, inner.te.ref, hi, lo);
            return (TaggedExpr){ { r, (uint16_t)(hi - lo + 1) }, 0 };
        }

        fprintf(fe->err, "error: unsupported indexed operator\n");
        return TAGGED_NULL;
    }

    if (head->kind != SEXPR_SYMBOL) {
        fprintf(fe->err, "error: expected symbol at head of expression\n");
        return TAGGED_NULL;
    }

    const char *op = head->sym.str;
    uint32_t   oplen = head->sym.len;

    /* ---- Binary BV arithmetic (result has same width as operands) ---- */
#define BINOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count != 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2])); \
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
        ExprRef r = builder_expr_binary(fe->builder, binop, a.te.ref, b.te.ref); \
        return (TaggedExpr){ { r, a.te.width }, 0 }; \
    }

    /* ---- BV comparison (result is width 1) ---- */
#define CMPOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count != 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2])); \
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
        ExprRef r = builder_expr_binary(fe->builder, binop, a.te.ref, b.te.ref); \
        return (TaggedExpr){ { r, 1 }, 0 }; \
    }

    BINOP_CASE("bvadd", BIN_ADD)
    BINOP_CASE("bvsub", BIN_SUB)
    BINOP_CASE("bvmul", BIN_MUL)
    BINOP_CASE("bvudiv", BIN_DIV)
    BINOP_CASE("bvurem", BIN_MOD)
    BINOP_CASE("bvand", BIN_BAND)
    BINOP_CASE("bvor", BIN_BOR)
    BINOP_CASE("bvxor", BIN_BXOR)
    BINOP_CASE("bvshl", BIN_LSHIFT)
    BINOP_CASE("bvlshr", BIN_RSHIFT)

    CMPOP_CASE("bvult", BIN_LT)
    CMPOP_CASE("bvule", BIN_LTE)
    CMPOP_CASE("bvugt", BIN_GT)
    CMPOP_CASE("bvuge", BIN_GTE)

#undef BINOP_CASE
#undef CMPOP_CASE

    /* = (equality) */
    if (oplen == 1 && op[0] == '=') {
        if (s->list.count != 3) return TAGGED_NULL;
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_binary(fe->builder, BIN_EQ, a.te.ref, b.te.ref);
        return (TaggedExpr){ { r, 1 }, 0 };
    }

    /* distinct */
    if (oplen == 8 && memcmp(op, "distinct", 8) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        uint32_t n = s->list.count - 1;
        TaggedExpr *args = (TaggedExpr *)malloc(n * sizeof(TaggedExpr));
        if (!args) return TAGGED_NULL;
        for (uint32_t i = 0; i < n; i++) {
            args[i] = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[i + 1]));
            if (args[i].te.ref == EXPR_NULL) { free(args); return TAGGED_NULL; }
        }
        ExprRef result = EXPR_NULL;
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                ExprRef neq = builder_expr_binary(fe->builder, BIN_NEQ,
                                                  args[i].te.ref, args[j].te.ref);
                if (result == EXPR_NULL) result = neq;
                else result = builder_expr_binary(fe->builder, BIN_AND, result, neq);
            }
        }
        free(args);
        return (TaggedExpr){ { result, 1 }, 0 };
    }

    /* ---- Unary BV ops ---- */
    if (oplen == 5 && memcmp(op, "bvnot", 5) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_unary(fe->builder, UN_INVERT, a.te.ref);
        return (TaggedExpr){ { r, a.te.width }, 0 };
    }
    if (oplen == 5 && memcmp(op, "bvneg", 5) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_unary(fe->builder, UN_NEG, a.te.ref);
        return (TaggedExpr){ { r, a.te.width }, 0 };
    }

    /* ---- Boolean connectives ---- */
    if (oplen == 3 && memcmp(op, "not", 3) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_unary(fe->builder, UN_NOT, a.te.ref);
        return (TaggedExpr){ { r, 1 }, 0 };
    }

    /* N-ary and */
    if (oplen == 3 && memcmp(op, "and", 3) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        TaggedExpr acc = _translate_tagged(fe, s->list.items[1]);
        if (acc.te.ref == EXPR_NULL) return TAGGED_NULL;
        for (uint32_t i = 2; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            acc.te.ref = builder_expr_binary(fe->builder, BIN_AND,
                                             acc.te.ref, b.te.ref);
            acc.leaf_kind = 0;
        }
        acc.te.width = 1;
        return acc;
    }

    /* N-ary or */
    if (oplen == 2 && memcmp(op, "or", 2) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        TaggedExpr acc = _translate_tagged(fe, s->list.items[1]);
        if (acc.te.ref == EXPR_NULL) return TAGGED_NULL;
        for (uint32_t i = 2; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            acc.te.ref = builder_expr_binary(fe->builder, BIN_OR,
                                             acc.te.ref, b.te.ref);
            acc.leaf_kind = 0;
        }
        acc.te.width = 1;
        return acc;
    }

    /* => */
    if (oplen == 2 && memcmp(op, "=>", 2) == 0) {
        if (s->list.count != 3) return TAGGED_NULL;
        TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        TaggedExpr b = _translate_tagged(fe, s->list.items[2]);
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef not_a = builder_expr_unary(fe->builder, UN_NOT, a.te.ref);
        ExprRef r = builder_expr_binary(fe->builder, BIN_OR, not_a, b.te.ref);
        return (TaggedExpr){ { r, 1 }, 0 };
    }

    /* ite */
    if (oplen == 3 && memcmp(op, "ite", 3) == 0) {
        if (s->list.count != 4) return TAGGED_NULL;
        TaggedExpr c = _translate_tagged(fe, s->list.items[1]);
        if (c.te.ref == EXPR_NULL) return TAGGED_NULL;
        TaggedExpr t = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
        if (t.te.ref == EXPR_NULL) return TAGGED_NULL;
        TaggedExpr e = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[3]));
        if (e.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_ite(fe->builder, c.te.ref, t.te.ref, e.te.ref);
        return (TaggedExpr){ { r, t.te.width }, 0 };
    }

    /* concat */
    if (oplen == 6 && memcmp(op, "concat", 6) == 0) {
        if (s->list.count != 3) return TAGGED_NULL;
        TaggedExpr hi = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (hi.te.ref == EXPR_NULL) return TAGGED_NULL;
        TaggedExpr lo = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
        if (lo.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_concat(fe->builder, hi.te.ref, lo.te.ref,
                                        (uint8_t)lo.te.width);
        return (TaggedExpr){ { r, (uint16_t)(hi.te.width + lo.te.width) }, 0 };
    }

    /* ---- Signed ops: deferred ---- */
    if ((oplen == 5 && memcmp(op, "bvslt", 5) == 0) ||
        (oplen == 5 && memcmp(op, "bvsle", 5) == 0) ||
        (oplen == 5 && memcmp(op, "bvsgt", 5) == 0) ||
        (oplen == 5 && memcmp(op, "bvsge", 5) == 0) ||
        (oplen == 6 && memcmp(op, "bvsdiv", 6) == 0) ||
        (oplen == 6 && memcmp(op, "bvsrem", 6) == 0) ||
        (oplen == 6 && memcmp(op, "bvashr", 6) == 0)) {
        fprintf(fe->err, "error: signed operation '%.*s' not yet supported\n",
                (int)oplen, op);
        return TAGGED_NULL;
    }

    fprintf(fe->err, "error: unsupported operation '%.*s'\n",
            (int)oplen, op);
    return TAGGED_NULL;
}

/* Entry points */
static TaggedExpr _translate_tagged(Smt2Frontend *fe, const Sexpr *s) {
    switch (s->kind) {
    case SEXPR_SYMBOL:
        return _translate_symbol_tagged(fe, s);
    case SEXPR_NUMERAL: {
        ExprRef r = builder_expr_const(fe->builder, (int64_t)s->numval, 0);
        return (TaggedExpr){ { r, 64 }, 2 };
    }
    case SEXPR_BITVEC: {
        ExprRef r = builder_expr_const(fe->builder, (int64_t)s->bv.value, 0);
        return (TaggedExpr){ { r, (uint16_t)s->bv.width }, 2 };
    }
    case SEXPR_LIST:
        return _translate_list_tagged(fe, s);
    default:
        fprintf(fe->err, "error: unexpected S-expression kind in expression\n");
        return TAGGED_NULL;
    }
}

static TypedExpr _translate_expr(Smt2Frontend *fe, const Sexpr *s) {
    return _translate_tagged(fe, s).te;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static int _cmd_set_logic(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || !sexpr_is_symbol(cmd->list.items[1], "QF_BV")) {
        fprintf(fe->err, "error: only QF_BV logic is supported\n");
        return -1;
    }
    return 0;
}

static int _cmd_set_option(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count < 3) return 0;
    Sexpr *key = cmd->list.items[1];
    Sexpr *val = cmd->list.items[2];

    if (sexpr_is_keyword(key, ":produce-models")) {
        if (sexpr_is_symbol(val, "true"))
            fe->produce_models = 1;
        return 0;
    }
    if (sexpr_is_keyword(key, ":seed")) {
        if (val->kind == SEXPR_NUMERAL)
            fe->seed = val->numval;
        return 0;
    }
    return 0;
}

static int _cmd_declare_const(Smt2Frontend *fe, const Sexpr *cmd) {
    Sexpr *name_s;
    Sexpr *sort_s;

    if (sexpr_is_command(cmd, "declare-const")) {
        if (cmd->list.count != 3) {
            fprintf(fe->err, "error: declare-const requires name and sort\n");
            return -1;
        }
        name_s = cmd->list.items[1];
        sort_s = cmd->list.items[2];
    } else {
        if (cmd->list.count != 4) {
            fprintf(fe->err, "error: declare-fun requires name, params, sort\n");
            return -1;
        }
        name_s = cmd->list.items[1];
        sort_s = cmd->list.items[3];
        Sexpr *params = cmd->list.items[2];
        if (params->kind != SEXPR_LIST || params->list.count != 0) {
            fprintf(fe->err, "error: only 0-arity declare-fun supported\n");
            return -1;
        }
    }

    if (name_s->kind != SEXPR_SYMBOL) {
        fprintf(fe->err, "error: expected symbol for variable name\n");
        return -1;
    }

    uint8_t width = _parse_bitvec_sort(fe, sort_s);
    if (width == 0) {
        fprintf(fe->err, "error: unsupported sort (only BitVec 1-64)\n");
        return -1;
    }

    uint32_t var_id = _next_var_id(fe);
    uint64_t max_val = (width == 64) ? UINT64_MAX : ((1ULL << width) - 1);

    builder_add_var(fe->builder, var_id, width, 0, 0, (int64_t)max_val);
    if (_add_var(fe, name_s->sym.str, name_s->sym.len, var_id, width) < 0) {
        fprintf(fe->err, "error: out of memory adding variable\n");
        return -1;
    }

    return 0;
}

static int _cmd_assert(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2) {
        fprintf(fe->err, "error: assert requires exactly one expression\n");
        return -1;
    }
    TypedExpr te = _translate_expr(fe, cmd->list.items[1]);
    if (te.ref == EXPR_NULL) {
        fprintf(fe->err, "error: failed to translate assert expression\n");
        return -1;
    }
    builder_add_constraint(fe->builder, te.ref);
    return 0;
}

static int _cmd_check_sat(Smt2Frontend *fe, const Sexpr *cmd) {
    (void)cmd;

    fe->problem = builder_finalize(fe->builder, &fe->problem_size);
    if (!fe->problem) {
        fprintf(fe->out, "unknown\n");
        fprintf(fe->err, "error: builder_finalize failed\n");
        return -1;
    }

    fe->ctx_buf_size = CTX_BUF_SIZE;
    fe->ctx_buf = malloc(fe->ctx_buf_size);
    if (!fe->ctx_buf) {
        fprintf(fe->out, "unknown\n");
        return -1;
    }

    fe->block_alloc = zsp_block_alloc_create(NULL, BA_BLOCK_SIZE);
    if (!fe->block_alloc) {
        fprintf(fe->out, "unknown\n");
        return -1;
    }

    fe->ctx = solver_create(fe->ctx_buf, fe->ctx_buf_size, fe->block_alloc);
    if (!fe->ctx) {
        fprintf(fe->out, "unknown\n");
        return -1;
    }

    int crc = solver_compile(fe->ctx, fe->problem);
    if (crc != 0) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
    }

    SolveOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.seed = fe->seed;

    fe->last_result = solver_solve(fe->ctx, &opts);
    fe->has_result = 1;

    switch (fe->last_result) {
    case SOLVE_OK:      fprintf(fe->out, "sat\n"); break;
    case SOLVE_UNSAT:   fprintf(fe->out, "unsat\n"); break;
    case SOLVE_TIMEOUT: fprintf(fe->out, "unknown\n"); break;
    }

    return 0;
}

static int _cmd_get_value(Smt2Frontend *fe, const Sexpr *cmd) {
    if (!fe->has_result || fe->last_result != SOLVE_OK) {
        fprintf(fe->err, "error: get-value requires a prior sat result\n");
        return 0;
    }

    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_LIST) {
        fprintf(fe->err, "error: get-value requires a list of variables\n");
        return 0;
    }

    Sexpr *vars_list = cmd->list.items[1];

    fprintf(fe->out, "(");
    for (uint32_t i = 0; i < vars_list->list.count; i++) {
        Sexpr *name_s = vars_list->list.items[i];
        if (name_s->kind != SEXPR_SYMBOL) continue;

        Smt2Var *v = _find_var(fe, name_s->sym.str, name_s->sym.len);
        if (!v) {
            fprintf(fe->err, "error: unknown variable in get-value: '%.*s'\n",
                    (int)name_s->sym.len, name_s->sym.str);
            continue;
        }

        int64_t val = solver_get_value(fe->ctx, v->var_id);
        if (i > 0) fprintf(fe->out, "\n ");
        fprintf(fe->out, "(%.*s (_ bv%" PRIu64 " %u))",
                (int)name_s->sym.len, name_s->sym.str,
                (uint64_t)val, (unsigned)v->width);
    }
    fprintf(fe->out, ")\n");

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void smt2_frontend_init(Smt2Frontend *fe, FILE *out, FILE *err) {
    memset(fe, 0, sizeof(*fe));
    fe->out = out;
    fe->err = err;
    fe->builder = builder_create(0, NULL);
}

void smt2_frontend_destroy(Smt2Frontend *fe) {
    if (fe->problem)     free(fe->problem);
    if (fe->builder)     builder_destroy(fe->builder);
    if (fe->ctx)         solver_destroy(fe->ctx);
    if (fe->block_alloc) zsp_block_alloc_destroy(fe->block_alloc);
    free(fe->ctx_buf);
    free(fe->vars);
    memset(fe, 0, sizeof(*fe));
}

int smt2_frontend_dispatch(Smt2Frontend *fe, const Sexpr *cmd) {
    if (!cmd || cmd->kind != SEXPR_LIST || cmd->list.count == 0)
        return 0;

    Sexpr *head = cmd->list.items[0];
    if (head->kind != SEXPR_SYMBOL) {
        fprintf(fe->err, "error: expected command symbol\n");
        return 0;
    }

    if (sexpr_is_symbol(head, "set-logic"))
        return _cmd_set_logic(fe, cmd);
    if (sexpr_is_symbol(head, "set-option"))
        return _cmd_set_option(fe, cmd);
    if (sexpr_is_symbol(head, "declare-const"))
        return _cmd_declare_const(fe, cmd);
    if (sexpr_is_symbol(head, "declare-fun"))
        return _cmd_declare_const(fe, cmd);
    if (sexpr_is_symbol(head, "assert"))
        return _cmd_assert(fe, cmd);
    if (sexpr_is_symbol(head, "check-sat"))
        return _cmd_check_sat(fe, cmd);
    if (sexpr_is_symbol(head, "get-value"))
        return _cmd_get_value(fe, cmd);
    if (sexpr_is_symbol(head, "exit"))
        return 1;
    if (sexpr_is_symbol(head, "set-info"))
        return 0;

    fprintf(fe->err, "(error \"unsupported: %.*s\")\n",
            (int)head->sym.len, head->sym.str);
    return 0;
}
