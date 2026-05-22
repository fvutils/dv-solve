#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "smt2/smt2_frontend.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define CTX_BUF_SIZE  (16u * 1024u * 1024u)
#define BA_BLOCK_SIZE (64u * 1024u)

static const TypedExpr TYPED_NULL = { EXPR_NULL, 0 };

/* ------------------------------------------------------------------ */
/* Per-command transient allocation pool                               */
/* ------------------------------------------------------------------ */

static void *_cmd_alloc(Smt2Frontend *fe, size_t sz) {
    void *p = malloc(sz);
    if (!p) return NULL;
    if (fe->n_cmd_allocs < SMT2_CMD_ALLOC_MAX)
        fe->cmd_allocs[fe->n_cmd_allocs++] = p;
    /* If the pool is full we still return the allocation; it leaks until
     * smt2_frontend_destroy().  This should never happen in practice. */
    return p;
}

static void _cmd_alloc_reset(Smt2Frontend *fe) {
    for (uint32_t i = 0; i < fe->n_cmd_allocs; i++) {
        free(fe->cmd_allocs[i]);
    }
    fe->n_cmd_allocs = 0;
}

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

static int _name_eq(const char *a, uint32_t alen, const char *b) {
    uint32_t blen = (uint32_t)strlen(b);
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* ------------------------------------------------------------------ */
/* Sort / sort-fun / sort-const / define-fun lookup                   */
/* ------------------------------------------------------------------ */

static int _is_known_sort(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < fe->n_sort_names; i++) {
        if (_name_eq(name, len, fe->sort_names[i])) return 1;
    }
    return 0;
}

static Smt2SortFun *_find_sort_fun(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < fe->n_sort_funs; i++) {
        if (_name_eq(name, len, fe->sort_funs[i].name)) return &fe->sort_funs[i];
    }
    return NULL;
}

static Smt2SortConst *_find_sort_const(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < fe->n_sort_consts; i++) {
        if (_name_eq(name, len, fe->sort_consts[i].name)) return &fe->sort_consts[i];
    }
    return NULL;
}

static Smt2FunDef *_find_fun(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < fe->n_funs; i++) {
        if (_name_eq(name, len, fe->funs[i].name)) return &fe->funs[i];
    }
    return NULL;
}

static Smt2ArrayVar *_find_array_var(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < fe->n_array_vars; i++) {
        if (_name_eq(name, len, fe->array_vars[i].name))
            return &fe->array_vars[i];
    }
    return NULL;
}

/* Walk the substitution stack (most recent first) to resolve a symbol.
 * Uses stored lengths rather than strlen to support non-null-terminated
 * names (e.g. let-binding names that point into the parser's arena). */
static const Sexpr *_subst_lookup(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (int i = (int)fe->subst_depth - 1; i >= 0; i--) {
        if (fe->subst_stack[i].len == len &&
            memcmp(name, fe->subst_stack[i].name, len) == 0) {
            return fe->subst_stack[i].value;
        }
    }
    return NULL;
}

/* Like _subst_lookup but returns the entry pointer so callers can cache. */
static Smt2Subst *_subst_lookup_entry(Smt2Frontend *fe, const char *name, uint32_t len) {
    for (int i = (int)fe->subst_depth - 1; i >= 0; i--) {
        if (fe->subst_stack[i].len == len &&
            memcmp(name, fe->subst_stack[i].name, len) == 0) {
            return &fe->subst_stack[i];
        }
    }
    return NULL;
}

/* Recursively resolve a symbol-typed Sexpr through subst chains. */
static const Sexpr *_resolve_sym(Smt2Frontend *fe, const Sexpr *s) {
    while (s && s->kind == SEXPR_SYMBOL) {
        const Sexpr *next = _subst_lookup(fe, s->sym.str, s->sym.len);
        if (!next) break;
        s = next;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Sort parsing                                                        */
/* ------------------------------------------------------------------ */

static uint8_t _parse_bitvec_sort(Smt2Frontend *fe, const Sexpr *sort) {
    (void)fe;
    if (!sort) return 0;
    if (sort->kind == SEXPR_SYMBOL) {
        if (_name_eq(sort->sym.str, sort->sym.len, "Bool")) return 1;
        return 0;
    }
    if (sort->kind != SEXPR_LIST || sort->list.count != 3)
        return 0;
    if (!sexpr_is_symbol(sort->list.items[0], "_")) return 0;
    if (!sexpr_is_symbol(sort->list.items[1], "BitVec")) return 0;
    if (sort->list.items[2]->kind != SEXPR_NUMERAL) return 0;
    uint64_t w = sort->list.items[2]->numval;
    if (w == 0 || w > 64) return 0;
    return (uint8_t)w;
}

/* Returns 1 on success (populates *out), 0 if not an Array sort,
 * -1 on malformed Array sort. */
static int _parse_array_sort(Smt2Frontend *fe, const Sexpr *s, Smt2ArraySort *out) {
    /* (Array (_ BitVec M) (_ BitVec N)) */
    if (!s || s->kind != SEXPR_LIST) return 0;
    if (s->list.count != 3) return 0;
    if (!sexpr_is_symbol(s->list.items[0], "Array")) return 0;

    uint8_t addr_w = _parse_bitvec_sort(fe, s->list.items[1]);
    uint8_t data_w = _parse_bitvec_sort(fe, s->list.items[2]);

    if (addr_w == 0 || data_w == 0) return -1;

    out->addr_width = addr_w;
    out->data_width = data_w;
    out->_pad[0] = out->_pad[1] = 0;
    return 1;
}

/* Returns 1 if sort is an opaque (declare-sort) sort name. */
static int _is_opaque_sort(Smt2Frontend *fe, const Sexpr *sort) {
    if (!sort || sort->kind != SEXPR_SYMBOL) return 0;
    return _is_known_sort(fe, sort->sym.str, sort->sym.len);
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
/* Aux var creation                                                    */
/* ------------------------------------------------------------------ */

static uint32_t _next_var_id(Smt2Frontend *fe) {
    return fe->n_vars;
}

static uint32_t _fresh_aux(Smt2Frontend *fe, uint16_t width) {
    uint32_t var_id = _next_var_id(fe);
    uint64_t max_val = (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);
    builder_add_var(fe->builder, var_id, (uint8_t)width, 0, 0, (int64_t)max_val);
    char name[SMT2_MAX_NAME];
    snprintf(name, sizeof(name), "__aux%u", var_id);
    _add_var(fe, name, (uint32_t)strlen(name), var_id, (uint8_t)width);
    if (fe->compiled) fe->has_aux = 1;
    return var_id;
}

/* ------------------------------------------------------------------ */
/* Array value helpers                                                 */
/* ------------------------------------------------------------------ */

/* Allocate a transient Smt2ArrayValue (freed at end of command). */
static Smt2ArrayValue *_make_array_value(Smt2Frontend *fe, Smt2ArraySort sort) {
    uint32_t n = 1u << sort.addr_width;
    Smt2ArrayValue *av = (Smt2ArrayValue *)_cmd_alloc(fe, sizeof(Smt2ArrayValue));
    if (!av) return NULL;
    ExprRef *elems = (ExprRef *)_cmd_alloc(fe, n * sizeof(ExprRef));
    if (!elems) return NULL;
    av->sort             = sort;
    av->n_elems          = n;
    av->elems            = elems;
    av->store_idx_varid  = UINT32_MAX;
    av->store_val        = EXPR_NULL;
    return av;
}

/* Declare a persistent array variable (element vars are solver vars).
 * Uses plain malloc so it survives command boundaries. */
static Smt2ArrayVar *_declare_array_const(Smt2Frontend *fe,
                                           const char *name, uint32_t nlen,
                                           Smt2ArraySort sort) {
    if (fe->n_array_vars >= SMT2_MAX_ARRAY_VARS) {
        fprintf(fe->err, "(error \"too many array variables\")\n");
        return NULL;
    }

    uint32_t n_elems = 1u << sort.addr_width;
    Smt2ArrayVar *av = &fe->array_vars[fe->n_array_vars++];

    uint32_t copy_len = nlen < SMT2_MAX_NAME - 1 ? nlen : SMT2_MAX_NAME - 1;
    memcpy(av->name, name, copy_len);
    av->name[copy_len] = '\0';
    av->sort = sort;

    av->value = (Smt2ArrayValue *)malloc(sizeof(Smt2ArrayValue));
    if (!av->value) { fe->n_array_vars--; return NULL; }
    av->value->sort   = sort;
    av->value->n_elems = n_elems;
    av->value->elems  = (ExprRef *)malloc(n_elems * sizeof(ExprRef));
    if (!av->value->elems) {
        free(av->value); av->value = NULL;
        fe->n_array_vars--;
        return NULL;
    }

    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t var_id = _next_var_id(fe);
        uint64_t max_val = (sort.data_width >= 64) ? UINT64_MAX
                           : ((1ULL << sort.data_width) - 1);
        builder_add_var(fe->builder, var_id, sort.data_width, 0, 0,
                        (int64_t)max_val);
        char elem_name[SMT2_MAX_NAME];
        snprintf(elem_name, sizeof(elem_name), "%.*s[%u]", (int)copy_len, name, i);
        _add_var(fe, elem_name, (uint32_t)strlen(elem_name), var_id,
                 sort.data_width);
        av->value->elems[i] = builder_expr_var(fe->builder, var_id);
    }

    if (fe->compiled) fe->has_aux = 1;
    return av;
}

/* ------------------------------------------------------------------ */
/* TaggedExpr (leaf-kind tracking + optional array payload)           */
/* ------------------------------------------------------------------ */

typedef struct {
    TypedExpr        te;
    int              leaf_kind;  /* 0=complex, 1=var, 2=const */
    Smt2ArrayValue  *array;      /* non-NULL for array-typed results */
} TaggedExpr;

static const TaggedExpr TAGGED_NULL = { { EXPR_NULL, 0 }, 0, NULL };

static TaggedExpr _translate_tagged(Smt2Frontend *fe, const Sexpr *s);

static TaggedExpr _flatten_to_var(Smt2Frontend *fe, TaggedExpr tg) {
    /* Arrays pass through; they do not need a scalar variable. */
    if (tg.array != NULL) return tg;
    if (tg.te.ref == EXPR_NULL) return TAGGED_NULL;
    if (tg.leaf_kind == 1 || tg.leaf_kind == 2) return tg;

    uint32_t aux_id = _fresh_aux(fe, tg.te.width);
    ExprRef aux_ref = builder_expr_var(fe->builder, aux_id);
    ExprRef eq = builder_expr_binary(fe->builder, BIN_EQ, aux_ref, tg.te.ref);
    builder_add_constraint(fe->builder, eq);

    TaggedExpr result;
    result.te.ref   = aux_ref;
    result.te.width = tg.te.width;
    result.leaf_kind = 1;
    result.array    = NULL;
    return result;
}

/* ------------------------------------------------------------------ */
/* Sort-fun application: build/lookup mangled flat variable           */
/* ------------------------------------------------------------------ */

static TaggedExpr _apply_sort_fun(Smt2Frontend *fe, Smt2SortFun *sf,
                                  const Sexpr *call) {
    if (call->list.count != (uint32_t)sf->n_params + 1) {
        fprintf(fe->err, "error: arity mismatch for '%s' (expected %u, got %u)\n",
                sf->name, sf->n_params, call->list.count - 1);
        return TAGGED_NULL;
    }

    /* Build mangled name: F@arg0@arg1@... */
    char mangled[SMT2_MAX_NAME];
    size_t mlen = 0;
    size_t flen = strlen(sf->name);
    if (flen >= SMT2_MAX_NAME) flen = SMT2_MAX_NAME - 1;
    memcpy(mangled, sf->name, flen); mlen = flen;

    for (uint32_t i = 0; i < sf->n_params; i++) {
        const Sexpr *arg = _resolve_sym(fe, call->list.items[i + 1]);
        if (!arg || arg->kind != SEXPR_SYMBOL) {
            fprintf(fe->err, "error: sort-fun '%s' argument %u is not a symbol\n",
                    sf->name, i);
            return TAGGED_NULL;
        }
        if (mlen + 1 + arg->sym.len >= SMT2_MAX_NAME) {
            fprintf(fe->err, "error: mangled name overflow for '%s'\n", sf->name);
            return TAGGED_NULL;
        }
        mangled[mlen++] = '@';
        memcpy(mangled + mlen, arg->sym.str, arg->sym.len);
        mlen += arg->sym.len;
    }
    mangled[mlen] = '\0';

    /* Array-returning sort fun: look up or lazily create an array var. */
    if (sf->is_array_return) {
        Smt2ArrayVar *av = _find_array_var(fe, mangled, (uint32_t)mlen);
        if (!av) {
            av = _declare_array_const(fe, mangled, (uint32_t)mlen, sf->array_sort);
            if (!av) return TAGGED_NULL;
        }
        return (TaggedExpr){ { EXPR_NULL, 0 }, 0, av->value };
    }

    /* BV/Bool return: look up or create a scalar var. */
    Smt2Var *v = _find_var(fe, mangled, (uint32_t)mlen);
    uint8_t width = sf->return_width ? sf->return_width : 1;
    if (!v) {
        uint32_t var_id = _next_var_id(fe);
        uint64_t max_val = (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);
        builder_add_var(fe->builder, var_id, width, 0, 0, (int64_t)max_val);
        if (_add_var(fe, mangled, (uint32_t)mlen, var_id, width) < 0) {
            fprintf(fe->err, "error: out of memory creating mangled var\n");
            return TAGGED_NULL;
        }
        v = _find_var(fe, mangled, (uint32_t)mlen);
        if (fe->compiled) fe->has_aux = 1;
    }
    ExprRef r = builder_expr_var(fe->builder, v->var_id);
    return (TaggedExpr){ { r, v->width }, 1, NULL };
}

/* ------------------------------------------------------------------ */
/* define-fun inline expansion                                         */
/* ------------------------------------------------------------------ */

static TaggedExpr _apply_fun_def(Smt2Frontend *fe, Smt2FunDef *fd,
                                 const Sexpr *call) {
    if (call->list.count != fd->n_params + 1) {
        fprintf(fe->err, "error: arity mismatch for define-fun '%s' (expected %u, got %u)\n",
                fd->name, fd->n_params, call->list.count - 1);
        return TAGGED_NULL;
    }
    if (fe->subst_depth + fd->n_params > SMT2_MAX_SUBST) {
        fprintf(fe->err, "error: substitution stack overflow expanding '%s'\n", fd->name);
        return TAGGED_NULL;
    }

    /* Push bindings */
    uint32_t saved_depth = fe->subst_depth;
    for (uint32_t i = 0; i < fd->n_params; i++) {
        Smt2Subst *s = &fe->subst_stack[fe->subst_depth++];
        s->name      = fd->param_names[i];
        s->len       = (uint32_t)strlen(fd->param_names[i]);
        s->value     = call->list.items[i + 1];
        s->has_cache = 0;
    }

    /* Translate body */
    TaggedExpr res = _translate_tagged(fe, fd->body);

    /* Pop bindings */
    fe->subst_depth = saved_depth;

    return res;
}

/* ------------------------------------------------------------------ */
/* select: build symbolic ITE tree over array elements                */
/* ------------------------------------------------------------------ */

static TaggedExpr _array_select(Smt2Frontend *fe, Smt2ArrayValue *arr,
                                 ExprRef idx) {
    uint32_t n = arr->n_elems;

    /* R1 rewrite: select(store(a, i, v), i) = v
     * The store handler records the var_id of its symbolic index.  If the
     * select index is the same variable, return the stored value directly. */
    if (arr->store_idx_varid != UINT32_MAX) {
        ExprVar *ev = (ExprVar *)builder_ref_ptr(fe->builder, idx);
        if (ev && ev->kind == EXPR_VAR && ev->var_id == arr->store_idx_varid)
            return (TaggedExpr){ { arr->store_val, arr->sort.data_width }, 0, NULL };
    }

    if (fe->print_stats && n > 64) {
        fprintf(fe->err, "stats: select ITE tree over %u elements\n", n);
    }

    /* Linear chain from n-2 down to 0; last element is the fallthrough. */
    ExprRef result = arr->elems[n - 1];
    for (int32_t i = (int32_t)n - 2; i >= 0; i--) {
        ExprRef idx_const = builder_expr_const(fe->builder, (int64_t)i, 0);
        ExprRef cond = builder_expr_binary(fe->builder, BIN_EQ, idx, idx_const);
        result = builder_expr_ite(fe->builder, cond, arr->elems[i], result);
    }
    return (TaggedExpr){ { result, arr->sort.data_width }, 0, NULL };
}

/* ------------------------------------------------------------------ */
/* Symbol translation                                                  */
/* ------------------------------------------------------------------ */

static TaggedExpr _translate_symbol_tagged(Smt2Frontend *fe, const Sexpr *s) {
    /* Substitution stack first */
    Smt2Subst *sub_entry = _subst_lookup_entry(fe, s->sym.str, s->sym.len);
    if (sub_entry) {
        if (sub_entry->has_cache && sub_entry->cached_ref != EXPR_NULL) {
            return (TaggedExpr){ { sub_entry->cached_ref, sub_entry->cached_width },
                                 sub_entry->cached_leaf_kind, sub_entry->cached_array };
        }
        if (sub_entry->has_cache) {
            /* let-binding: value was pre-translated eagerly; return it directly */
            return (TaggedExpr){ { sub_entry->cached_ref, sub_entry->cached_width },
                                 sub_entry->cached_leaf_kind, sub_entry->cached_array };
        }
        /* define-fun parameter: lazily translate the argument expression */
        return _translate_tagged(fe, sub_entry->value);
    }

    if (sexpr_is_symbol(s, "true")) {
        ExprRef r = builder_expr_const(fe->builder, 1, 0);
        return (TaggedExpr){ { r, 1 }, 2, NULL };
    }
    if (sexpr_is_symbol(s, "false")) {
        ExprRef r = builder_expr_const(fe->builder, 0, 0);
        return (TaggedExpr){ { r, 1 }, 2, NULL };
    }

    /* Zero-arg define-fun? */
    Smt2FunDef *fd = _find_fun(fe, s->sym.str, s->sym.len);
    if (fd && fd->n_params == 0) {
        return _translate_tagged(fe, fd->body);
    }

    /* BV/Bool variable */
    Smt2Var *v = _find_var(fe, s->sym.str, s->sym.len);
    if (v) {
        ExprRef r = builder_expr_var(fe->builder, v->var_id);
        return (TaggedExpr){ { r, v->width }, 1, NULL };
    }

    /* Array variable */
    Smt2ArrayVar *av = _find_array_var(fe, s->sym.str, s->sym.len);
    if (av) {
        return (TaggedExpr){ { EXPR_NULL, 0 }, 0, av->value };
    }

    fprintf(fe->err, "error: unknown variable '%.*s'\n",
            (int)s->sym.len, s->sym.str);
    return TAGGED_NULL;
}

/* ------------------------------------------------------------------ */
/* List expression translation                                         */
/* ------------------------------------------------------------------ */

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
            return (TaggedExpr){ { r, w }, 2, NULL };
        }
        fprintf(fe->err, "error: unexpected indexed identifier\n");
        return TAGGED_NULL;
    }

    /* ((_ zero_extend N) expr) / ((_ sign_extend N) expr) / ((_ extract hi lo) expr) */
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
            return (TaggedExpr){ { r, new_width }, 0, NULL };
        }

        if (sexpr_is_symbol(op_sym, "extract")) {
            if (s->list.count != 2 || head->list.count != 4) return TAGGED_NULL;
            uint8_t hi = (uint8_t)head->list.items[2]->numval;
            uint8_t lo = (uint8_t)head->list.items[3]->numval;

            TaggedExpr inner = _translate_tagged(fe, s->list.items[1]);
            inner = _flatten_to_var(fe, inner);
            if (inner.te.ref == EXPR_NULL) return TAGGED_NULL;

            ExprRef r = builder_expr_extract(fe->builder, inner.te.ref, hi, lo);
            return (TaggedExpr){ { r, (uint16_t)(hi - lo + 1) }, 0, NULL };
        }

        fprintf(fe->err, "error: unsupported indexed operator\n");
        return TAGGED_NULL;
    }

    /* ((as const (Array M N)) value) */
    if (head->kind == SEXPR_LIST && head->list.count == 3 &&
        sexpr_is_symbol(head->list.items[0], "as") &&
        sexpr_is_symbol(head->list.items[1], "const")) {

        Smt2ArraySort sort;
        int r = _parse_array_sort(fe, head->list.items[2], &sort);
        if (r <= 0) {
            fprintf(fe->err, "error: (as const ...) requires valid Array sort\n");
            return TAGGED_NULL;
        }
        if (s->list.count != 2) return TAGGED_NULL;

        TaggedExpr val_te = _translate_tagged(fe, s->list.items[1]);
        if (val_te.te.ref == EXPR_NULL || val_te.array != NULL) {
            fprintf(fe->err, "error: (as const ...) value must be a BV\n");
            return TAGGED_NULL;
        }

        Smt2ArrayValue *arr = _make_array_value(fe, sort);
        if (!arr) return TAGGED_NULL;

        for (uint32_t i = 0; i < arr->n_elems; i++)
            arr->elems[i] = val_te.te.ref;

        return (TaggedExpr){ { EXPR_NULL, 0 }, 0, arr };
    }

    /* (let ((x1 e1) (x2 e2) ...) body) — parallel binding.
     * SMT-LIB2 parallel semantics: all ei are evaluated in the CURRENT scope
     * before any xi binding takes effect.  We pre-translate eagerly. */
    if (head->kind == SEXPR_SYMBOL && sexpr_is_symbol(head, "let")) {
        if (s->list.count != 3) {
            fprintf(fe->err, "error: malformed let (expected 3 elements)\n");
            return TAGGED_NULL;
        }
        const Sexpr *binds = s->list.items[1];
        const Sexpr *body  = s->list.items[2];
        if (binds->kind != SEXPR_LIST) {
            fprintf(fe->err, "error: let: binding list must be a list\n");
            return TAGGED_NULL;
        }
        uint32_t n = binds->list.count;
        if (fe->subst_depth + n > SMT2_MAX_SUBST) {
            fprintf(fe->err, "error: substitution stack overflow in let (%u bindings)\n", n);
            return TAGGED_NULL;
        }
        uint32_t saved = fe->subst_depth;

        /* Phase 1: evaluate ALL value expressions in the current (outer) scope
         * before any binding takes effect — true parallel SMT-LIB2 semantics.
         * Store translated values in a temporary buffer, then push bindings. */
        TaggedExpr let_vals[SMT2_MAX_SUBST];
        const Sexpr *let_names[SMT2_MAX_SUBST];
        for (uint32_t i = 0; i < n; i++) {
            const Sexpr *b = binds->list.items[i];
            if (b->kind != SEXPR_LIST || b->list.count != 2) goto let_bad;
            let_names[i] = b->list.items[0];
            if (let_names[i]->kind != SEXPR_SYMBOL) goto let_bad;
            let_vals[i] = _translate_tagged(fe, b->list.items[1]);
        }
        /* Push all bindings after all values are evaluated */
        for (uint32_t i = 0; i < n; i++) {
            Smt2Subst *e        = &fe->subst_stack[fe->subst_depth++];
            e->name             = let_names[i]->sym.str;
            e->len              = let_names[i]->sym.len;
            e->value            = NULL; /* not used; has_cache=1 takes precedence */
            e->cached_ref       = let_vals[i].te.ref;
            e->cached_width     = let_vals[i].te.width;
            e->cached_leaf_kind = let_vals[i].leaf_kind;
            e->cached_array     = let_vals[i].array;
            e->has_cache        = 1;
        }

        /* Phase 2: translate body with all bindings now in scope. */
        {
            TaggedExpr res = _translate_tagged(fe, body);
            fe->subst_depth = saved;
            return res;
        }
    let_bad:
        fprintf(fe->err, "error: malformed let binding\n");
        fe->subst_depth = saved;
        return TAGGED_NULL;
    }

    if (head->kind != SEXPR_SYMBOL) {
        fprintf(fe->err, "error: expected symbol at head of expression\n");
        return TAGGED_NULL;
    }

    const char *op = head->sym.str;
    uint32_t   oplen = head->sym.len;

    /* Check for define-fun application */
    Smt2FunDef *fd = _find_fun(fe, op, oplen);
    if (fd) {
        return _apply_fun_def(fe, fd, s);
    }

    /* Check for sort-fun application */
    Smt2SortFun *sf = _find_sort_fun(fe, op, oplen);
    if (sf) {
        return _apply_sort_fun(fe, sf, s);
    }

    /* ---- select / store ---- */
    if (oplen == 6 && memcmp(op, "select", 6) == 0) {
        if (s->list.count != 3) return TAGGED_NULL;

        TaggedExpr arr_te = _translate_tagged(fe, s->list.items[1]);
        if (arr_te.array == NULL) {
            fprintf(fe->err, "error: select: first argument is not an array\n");
            return TAGGED_NULL;
        }
        Smt2ArrayValue *arr = arr_te.array;

        TaggedExpr idx_te = _translate_tagged(fe, s->list.items[2]);
        if (idx_te.te.ref == EXPR_NULL || idx_te.array != NULL) {
            fprintf(fe->err, "error: select: index must be a BV\n");
            return TAGGED_NULL;
        }

        /* Constant index path (rewrite R2 included): check if the sexpr is
         * a bitvec literal after substitution resolution. */
        const Sexpr *idx_s = _resolve_sym(fe, s->list.items[2]);
        if (idx_s) {
            uint64_t k = 0;
            int is_const = 0;
            if (idx_s->kind == SEXPR_BITVEC) {
                k = idx_s->bv.value; is_const = 1;
            } else if (idx_s->kind == SEXPR_LIST && idx_s->list.count == 3 &&
                       sexpr_is_symbol(idx_s->list.items[0], "_")) {
                uint64_t bv_val;
                if (_parse_bv_sym(idx_s->list.items[1], &bv_val)) {
                    k = bv_val; is_const = 1;
                }
            }
            if (is_const && k < arr->n_elems) {
                return (TaggedExpr){ { arr->elems[k], arr->sort.data_width }, 2, NULL };
            }
        }

        /* Symbolic index: ITE tree */
        idx_te = _flatten_to_var(fe, idx_te);
        return _array_select(fe, arr, idx_te.te.ref);
    }

    if (oplen == 5 && memcmp(op, "store", 5) == 0) {
        if (s->list.count != 4) return TAGGED_NULL;

        TaggedExpr arr_te = _translate_tagged(fe, s->list.items[1]);
        if (arr_te.array == NULL) {
            fprintf(fe->err, "error: store: first argument is not an array\n");
            return TAGGED_NULL;
        }
        Smt2ArrayValue *arr = arr_te.array;

        TaggedExpr idx_te = _translate_tagged(fe, s->list.items[2]);
        if (idx_te.te.ref == EXPR_NULL || idx_te.array != NULL) {
            fprintf(fe->err, "error: store: index must be a BV\n");
            return TAGGED_NULL;
        }

        TaggedExpr val_te = _translate_tagged(fe, s->list.items[3]);
        if (val_te.te.ref == EXPR_NULL || val_te.array != NULL) {
            fprintf(fe->err, "error: store: value must be a BV\n");
            return TAGGED_NULL;
        }

        Smt2ArrayValue *new_arr = _make_array_value(fe, arr->sort);
        if (!new_arr) return TAGGED_NULL;

        /* Constant index path */
        const Sexpr *idx_s = _resolve_sym(fe, s->list.items[2]);
        if (idx_s) {
            uint64_t k = 0;
            int is_const = 0;
            if (idx_s->kind == SEXPR_BITVEC) {
                k = idx_s->bv.value; is_const = 1;
            } else if (idx_s->kind == SEXPR_LIST && idx_s->list.count == 3 &&
                       sexpr_is_symbol(idx_s->list.items[0], "_")) {
                uint64_t bv_val;
                if (_parse_bv_sym(idx_s->list.items[1], &bv_val)) {
                    k = bv_val; is_const = 1;
                }
            }
            if (is_const) {
                memcpy(new_arr->elems, arr->elems, arr->n_elems * sizeof(ExprRef));
                if (k < arr->n_elems)
                    new_arr->elems[k] = val_te.te.ref;
                return (TaggedExpr){ { EXPR_NULL, 0 }, 0, new_arr };
            }
        }

        /* Symbolic index: elementwise ITE.  Record R1 metadata (store index
         * var_id) so a later select at the same index can short-circuit. */
        idx_te = _flatten_to_var(fe, idx_te);
        val_te = _flatten_to_var(fe, val_te);
        {
            ExprVar *ev = (ExprVar *)builder_ref_ptr(fe->builder, idx_te.te.ref);
            if (ev && ev->kind == EXPR_VAR) {
                new_arr->store_idx_varid = ev->var_id;
                new_arr->store_val       = val_te.te.ref;
            }
        }
        for (uint32_t j = 0; j < arr->n_elems; j++) {
            ExprRef j_const = builder_expr_const(fe->builder, (int64_t)j, 0);
            ExprRef cond = builder_expr_binary(fe->builder, BIN_EQ,
                                               idx_te.te.ref, j_const);
            new_arr->elems[j] = builder_expr_ite(fe->builder, cond,
                                                  val_te.te.ref, arr->elems[j]);
        }
        return (TaggedExpr){ { EXPR_NULL, 0 }, 0, new_arr };
    }

    /* ---- Binary BV arithmetic (result has same width as operands) ---- */
#define BINOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count != 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2])); \
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
        ExprRef r = builder_expr_binary(fe->builder, binop, a.te.ref, b.te.ref); \
        return (TaggedExpr){ { r, a.te.width }, 0, NULL }; \
    }

#define CMPOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count != 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2])); \
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
        ExprRef r = builder_expr_binary(fe->builder, binop, a.te.ref, b.te.ref); \
        return (TaggedExpr){ { r, 1 }, 0, NULL }; \
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

    /* = (equality) -- handles both BV and array operands */
    if (oplen == 1 && op[0] == '=') {
        if (s->list.count != 3) return TAGGED_NULL;

        TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
        TaggedExpr b = _translate_tagged(fe, s->list.items[2]);

        if (a.array != NULL && b.array != NULL) {
            /* Array equality: AND of element-wise equalities */
            if (a.array->n_elems != b.array->n_elems) {
                fprintf(fe->err, "error: array equality on arrays with different sizes\n");
                return TAGGED_NULL;
            }
            ExprRef result = EXPR_NULL;
            for (uint32_t i = 0; i < a.array->n_elems; i++) {
                ExprRef eq_i = builder_expr_binary(fe->builder, BIN_EQ,
                                                   a.array->elems[i],
                                                   b.array->elems[i]);
                result = (result == EXPR_NULL) ? eq_i
                       : builder_expr_binary(fe->builder, BIN_AND, result, eq_i);
            }
            return (TaggedExpr){ { result, 1 }, 0, NULL };
        }
        if (a.array != NULL || b.array != NULL) {
            fprintf(fe->err, "error: type mismatch in equality (array vs scalar)\n");
            return TAGGED_NULL;
        }

        /* BV/Bool equality */
        a = _flatten_to_var(fe, a);
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        b = _flatten_to_var(fe, b);
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_binary(fe->builder, BIN_EQ, a.te.ref, b.te.ref);
        return (TaggedExpr){ { r, 1 }, 0, NULL };
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
        return (TaggedExpr){ { result, 1 }, 0, NULL };
    }

    /* ---- Unary BV ops ---- */
    if (oplen == 5 && memcmp(op, "bvnot", 5) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_unary(fe->builder, UN_INVERT, a.te.ref);
        return (TaggedExpr){ { r, a.te.width }, 0, NULL };
    }
    if (oplen == 5 && memcmp(op, "bvneg", 5) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_unary(fe->builder, UN_NEG, a.te.ref);
        return (TaggedExpr){ { r, a.te.width }, 0, NULL };
    }

    /* ---- Boolean connectives ---- */
    if (oplen == 3 && memcmp(op, "not", 3) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_unary(fe->builder, UN_NOT, a.te.ref);
        return (TaggedExpr){ { r, 1 }, 0, NULL };
    }

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

    /* xor */
    if (oplen == 3 && memcmp(op, "xor", 3) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        TaggedExpr acc = _translate_tagged(fe, s->list.items[1]);
        if (acc.te.ref == EXPR_NULL) return TAGGED_NULL;
        for (uint32_t i = 2; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            acc.te.ref = builder_expr_binary(fe->builder, BIN_BXOR,
                                             acc.te.ref, b.te.ref);
            acc.leaf_kind = 0;
        }
        acc.te.width = 1;
        return acc;
    }

    /* => */
    if (oplen == 2 && memcmp(op, "=>", 2) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        /* SMT-LIB =>: right-associative. (=> a b c) == (=> a (=> b c)) */
        TaggedExpr last = _translate_tagged(fe, s->list.items[s->list.count - 1]);
        if (last.te.ref == EXPR_NULL) return TAGGED_NULL;
        for (int i = (int)s->list.count - 2; i >= 1; i--) {
            TaggedExpr a = _translate_tagged(fe, s->list.items[i]);
            if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
            ExprRef not_a = builder_expr_unary(fe->builder, UN_NOT, a.te.ref);
            last.te.ref = builder_expr_binary(fe->builder, BIN_OR, not_a, last.te.ref);
            last.leaf_kind = 0;
        }
        last.te.width = 1;
        return last;
    }

    /* ite -- handles both BV and array branches */
    if (oplen == 3 && memcmp(op, "ite", 3) == 0) {
        if (s->list.count != 4) return TAGGED_NULL;
        TaggedExpr c = _translate_tagged(fe, s->list.items[1]);
        if (c.te.ref == EXPR_NULL) return TAGGED_NULL;
        TaggedExpr t = _translate_tagged(fe, s->list.items[2]);
        TaggedExpr e = _translate_tagged(fe, s->list.items[3]);

        if (t.array != NULL && e.array != NULL) {
            if (t.array->n_elems != e.array->n_elems) {
                fprintf(fe->err, "error: ite branches are arrays of different sizes\n");
                return TAGGED_NULL;
            }
            Smt2ArrayValue *arr = _make_array_value(fe, t.array->sort);
            if (!arr) return TAGGED_NULL;
            for (uint32_t i = 0; i < t.array->n_elems; i++) {
                arr->elems[i] = builder_expr_ite(fe->builder, c.te.ref,
                                                  t.array->elems[i],
                                                  e.array->elems[i]);
            }
            return (TaggedExpr){ { EXPR_NULL, 0 }, 0, arr };
        }

        /* BV ite */
        t = _flatten_to_var(fe, t);
        if (t.te.ref == EXPR_NULL) return TAGGED_NULL;
        e = _flatten_to_var(fe, e);
        if (e.te.ref == EXPR_NULL) return TAGGED_NULL;
        ExprRef r = builder_expr_ite(fe->builder, c.te.ref, t.te.ref, e.te.ref);
        return (TaggedExpr){ { r, t.te.width }, 0, NULL };
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
        return (TaggedExpr){ { r, (uint16_t)(hi.te.width + lo.te.width) }, 0, NULL };
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
        return (TaggedExpr){ { r, 64 }, 2, NULL };
    }
    case SEXPR_BITVEC: {
        ExprRef r = builder_expr_const(fe->builder, (int64_t)s->bv.value, 0);
        return (TaggedExpr){ { r, (uint16_t)s->bv.width }, 2, NULL };
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
/* Deep-copy Sexpr into persistent arena                              */
/* ------------------------------------------------------------------ */

static const Sexpr *_sexpr_deep_copy(SexprArena *arena, const Sexpr *src) {
    Sexpr *dst = (Sexpr *)sexpr_arena_alloc(arena, sizeof(Sexpr), _Alignof(Sexpr));
    if (!dst) return NULL;
    dst->kind = src->kind;
    switch (src->kind) {
    case SEXPR_SYMBOL:
    case SEXPR_KEYWORD:
    case SEXPR_STRING: {
        char *s = (char *)sexpr_arena_alloc(arena, src->sym.len, 1);
        if (!s) return NULL;
        memcpy(s, src->sym.str, src->sym.len);
        dst->sym.str = s;
        dst->sym.len = src->sym.len;
        break;
    }
    case SEXPR_NUMERAL:
        dst->numval = src->numval;
        break;
    case SEXPR_BITVEC:
        dst->bv = src->bv;
        break;
    case SEXPR_LIST: {
        dst->list.count = src->list.count;
        if (src->list.count == 0) {
            dst->list.items = NULL;
        } else {
            Sexpr **arr = (Sexpr **)sexpr_arena_alloc(
                arena, src->list.count * sizeof(Sexpr *), _Alignof(Sexpr *));
            if (!arr) return NULL;
            for (uint32_t i = 0; i < src->list.count; i++) {
                arr[i] = (Sexpr *)_sexpr_deep_copy(arena, src->list.items[i]);
                if (!arr[i]) return NULL;
            }
            dst->list.items = arr;
        }
        break;
    }
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static int _cmd_set_logic(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_SYMBOL) {
        fprintf(fe->err, "error: set-logic requires a logic name\n");
        return -1;
    }
    const Sexpr *l = cmd->list.items[1];
    if (sexpr_is_symbol(l, "QF_BV") ||
        sexpr_is_symbol(l, "QF_UFBV") ||
        sexpr_is_symbol(l, "QF_ABV") ||
        sexpr_is_symbol(l, "QF_AUFBV") ||
        sexpr_is_symbol(l, "ALL")) {
        return 0;
    }
    fprintf(fe->err, "error: unsupported logic '%.*s' (supported: QF_BV, QF_UFBV, QF_ABV, QF_AUFBV, ALL)\n",
            (int)l->sym.len, l->sym.str);
    return -1;
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

static int _cmd_declare_sort(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 3) {
        fprintf(fe->err, "error: declare-sort requires name and arity\n");
        return -1;
    }
    const Sexpr *name_s = cmd->list.items[1];
    const Sexpr *arity_s = cmd->list.items[2];
    if (name_s->kind != SEXPR_SYMBOL || arity_s->kind != SEXPR_NUMERAL) {
        fprintf(fe->err, "error: malformed declare-sort\n");
        return -1;
    }
    if (arity_s->numval != 0) {
        fprintf(fe->err, "error: only arity-0 sorts supported\n");
        return -1;
    }
    if (fe->n_sort_names >= SMT2_MAX_SORTS) {
        fprintf(fe->err, "error: too many sorts declared\n");
        return -1;
    }
    if (_is_known_sort(fe, name_s->sym.str, name_s->sym.len)) return 0;

    uint32_t copy_len = name_s->sym.len < SMT2_MAX_NAME - 1 ?
                        name_s->sym.len : SMT2_MAX_NAME - 1;
    memcpy(fe->sort_names[fe->n_sort_names], name_s->sym.str, copy_len);
    fe->sort_names[fe->n_sort_names][copy_len] = '\0';
    fe->n_sort_names++;
    return 0;
}

static int _add_sort_fun(Smt2Frontend *fe, const Sexpr *name_s,
                         uint8_t n_params, uint8_t return_width, int is_bool) {
    if (fe->n_sort_funs >= SMT2_MAX_SORT_FUNS) {
        fprintf(fe->err, "error: too many sort-typed functions declared\n");
        return -1;
    }
    Smt2SortFun *sf = &fe->sort_funs[fe->n_sort_funs++];
    uint32_t copy_len = name_s->sym.len < SMT2_MAX_NAME - 1 ?
                        name_s->sym.len : SMT2_MAX_NAME - 1;
    memcpy(sf->name, name_s->sym.str, copy_len);
    sf->name[copy_len] = '\0';
    sf->n_params = n_params;
    sf->return_width = return_width;
    sf->is_bool_return = is_bool ? 1 : 0;
    sf->is_array_return = 0;
    memset(&sf->array_sort, 0, sizeof(sf->array_sort));
    return 0;
}

static int _add_sort_fun_array(Smt2Frontend *fe, const Sexpr *name_s,
                               uint8_t n_params, Smt2ArraySort array_sort) {
    if (fe->n_sort_funs >= SMT2_MAX_SORT_FUNS) {
        fprintf(fe->err, "error: too many sort-typed functions declared\n");
        return -1;
    }
    Smt2SortFun *sf = &fe->sort_funs[fe->n_sort_funs++];
    uint32_t copy_len = name_s->sym.len < SMT2_MAX_NAME - 1 ?
                        name_s->sym.len : SMT2_MAX_NAME - 1;
    memcpy(sf->name, name_s->sym.str, copy_len);
    sf->name[copy_len] = '\0';
    sf->n_params = n_params;
    sf->return_width = 0;
    sf->is_bool_return = 0;
    sf->is_array_return = 1;
    sf->array_sort = array_sort;
    return 0;
}

static int _add_sort_const(Smt2Frontend *fe, const Sexpr *name_s,
                           const Sexpr *sort_s) {
    if (fe->n_sort_consts >= SMT2_MAX_SORT_CONSTS) {
        fprintf(fe->err, "error: too many sort-typed constants declared\n");
        return -1;
    }
    Smt2SortConst *sc = &fe->sort_consts[fe->n_sort_consts++];
    uint32_t nlen = name_s->sym.len < SMT2_MAX_NAME - 1 ?
                    name_s->sym.len : SMT2_MAX_NAME - 1;
    memcpy(sc->name, name_s->sym.str, nlen);
    sc->name[nlen] = '\0';
    uint32_t slen = sort_s->sym.len < SMT2_MAX_NAME - 1 ?
                    sort_s->sym.len : SMT2_MAX_NAME - 1;
    memcpy(sc->sort_name, sort_s->sym.str, slen);
    sc->sort_name[slen] = '\0';
    return 0;
}

static int _cmd_declare_const(Smt2Frontend *fe, const Sexpr *cmd) {
    Sexpr *name_s;
    Sexpr *sort_s;
    Sexpr *params = NULL;

    if (sexpr_is_command(cmd, "declare-const")) {
        if (cmd->list.count != 3) {
            fprintf(fe->err, "error: declare-const requires name and sort\n");
            return -1;
        }
        name_s = cmd->list.items[1];
        sort_s = cmd->list.items[2];
    } else {
        /* declare-fun */
        if (cmd->list.count != 4) {
            fprintf(fe->err, "error: declare-fun requires name, params, sort\n");
            return -1;
        }
        name_s = cmd->list.items[1];
        params = cmd->list.items[2];
        sort_s = cmd->list.items[3];
        if (params->kind != SEXPR_LIST) {
            fprintf(fe->err, "error: declare-fun params must be a list\n");
            return -1;
        }
    }

    if (name_s->kind != SEXPR_SYMBOL) {
        fprintf(fe->err, "error: expected symbol for variable name\n");
        return -1;
    }

    /* Case 1: declare-fun with arity > 0 -- sort-typed function */
    if (params && params->list.count > 0) {
        for (uint32_t i = 0; i < params->list.count; i++) {
            if (!_is_opaque_sort(fe, params->list.items[i])) {
                fprintf(fe->err, "error: declare-fun param sort must be a declared sort\n");
                return -1;
            }
        }
        /* Check if return sort is an Array */
        Smt2ArraySort array_sort;
        int is_arr = _parse_array_sort(fe, sort_s, &array_sort);
        if (is_arr == 1) {
            return _add_sort_fun_array(fe, name_s,
                                       (uint8_t)params->list.count, array_sort);
        }
        int is_bool = sexpr_is_symbol(sort_s, "Bool");
        uint8_t rw = _parse_bitvec_sort(fe, sort_s);
        if (rw == 0) {
            /* Opaque return sort: treat as width-1 accessor */
            if (_is_opaque_sort(fe, sort_s)) {
                rw = 1; is_bool = 0;
            } else {
                fprintf(fe->err, "error: declare-fun return sort must be Bool, BitVec, or Array\n");
                return -1;
            }
        }
        return _add_sort_fun(fe, name_s, (uint8_t)params->list.count, rw, is_bool);
    }

    /* Case 2: declare-const / 0-arity declare-fun with Array sort */
    Smt2ArraySort array_sort;
    int is_arr = _parse_array_sort(fe, sort_s, &array_sort);
    if (is_arr == 1) {
        /* Guard against push/pop: array vars at push_depth > 0 not supported */
        if (fe->push_depth > 0) {
            fprintf(fe->err,
                    "(error \"declare-const Array inside push scope not supported\")\n");
            return 0;
        }
        /* Reject oversized address space (continue session, don't create var) */
        if (array_sort.addr_width > SMT2_MAX_ARRAY_ADDR_BITS) {
            fprintf(fe->err,
                    "(error \"array address width %u exceeds limit %u\")\n",
                    (unsigned)array_sort.addr_width, SMT2_MAX_ARRAY_ADDR_BITS);
            return 0;
        }
        Smt2ArrayVar *av = _declare_array_const(fe, name_s->sym.str,
                                                 name_s->sym.len, array_sort);
        return av ? 0 : -1;
    }

    /* Case 3: declare-const / 0-arity declare-fun with opaque sort */
    if (sort_s->kind == SEXPR_SYMBOL && _is_opaque_sort(fe, sort_s)) {
        return _add_sort_const(fe, name_s, sort_s);
    }

    /* Case 4: standard BV (or Bool) declaration */
    uint8_t width = _parse_bitvec_sort(fe, sort_s);
    if (width == 0) {
        fprintf(fe->err, "error: unsupported sort (only BitVec 1-64, Bool, or Array)\n");
        return -1;
    }

    uint32_t var_id = _next_var_id(fe);
    uint64_t max_val = (width == 64) ? UINT64_MAX : ((1ULL << width) - 1);

    builder_add_var(fe->builder, var_id, width, 0, 0, (int64_t)max_val);
    if (_add_var(fe, name_s->sym.str, name_s->sym.len, var_id, width) < 0) {
        fprintf(fe->err, "error: out of memory adding variable\n");
        return -1;
    }
    if (fe->compiled) fe->has_aux = 1;
    return 0;
}

static int _cmd_define_fun(Smt2Frontend *fe, const Sexpr *cmd) {
    /* (define-fun NAME ((p1 S1) (p2 S2) ...) RET BODY) */
    if (cmd->list.count != 5) {
        fprintf(fe->err, "error: define-fun requires name, params, sort, body\n");
        return -1;
    }
    const Sexpr *name_s = cmd->list.items[1];
    const Sexpr *params_s = cmd->list.items[2];
    const Sexpr *sort_s = cmd->list.items[3];
    const Sexpr *body_s = cmd->list.items[4];

    if (name_s->kind != SEXPR_SYMBOL || params_s->kind != SEXPR_LIST) {
        fprintf(fe->err, "error: malformed define-fun\n");
        return -1;
    }
    if (fe->n_funs >= SMT2_MAX_FUNS) {
        fprintf(fe->err, "error: too many define-fun macros\n");
        return -1;
    }
    if (params_s->list.count > SMT2_MAX_FUN_PARAMS) {
        fprintf(fe->err, "error: define-fun has too many parameters\n");
        return -1;
    }

    Smt2FunDef *fd = &fe->funs[fe->n_funs];
    uint32_t nlen = name_s->sym.len < SMT2_MAX_NAME - 1 ?
                    name_s->sym.len : SMT2_MAX_NAME - 1;
    memcpy(fd->name, name_s->sym.str, nlen);
    fd->name[nlen] = '\0';
    fd->n_params = params_s->list.count;

    /* Return sort */
    Smt2ArraySort ret_arr_sort;
    int ret_is_arr = _parse_array_sort(fe, sort_s, &ret_arr_sort);
    if (ret_is_arr == 1) {
        fd->is_array_return = 1;
        fd->array_return_sort = ret_arr_sort;
        fd->is_bool_return = 0;
        fd->return_width = 0;
    } else {
        fd->is_array_return = 0;
        fd->is_bool_return = sexpr_is_symbol(sort_s, "Bool") ? 1 : 0;
        fd->return_width = _parse_bitvec_sort(fe, sort_s);
        if (fd->return_width == 0) fd->return_width = 1;
    }

    for (uint32_t i = 0; i < params_s->list.count; i++) {
        const Sexpr *p = params_s->list.items[i];
        if (p->kind != SEXPR_LIST || p->list.count != 2 ||
            p->list.items[0]->kind != SEXPR_SYMBOL) {
            fprintf(fe->err, "error: malformed define-fun parameter\n");
            return -1;
        }
        const Sexpr *pname = p->list.items[0];
        const Sexpr *psort = p->list.items[1];
        uint32_t plen = pname->sym.len < SMT2_MAX_NAME - 1 ?
                        pname->sym.len : SMT2_MAX_NAME - 1;
        memcpy(fd->param_names[i], pname->sym.str, plen);
        fd->param_names[i][plen] = '\0';

        /* Accept opaque sorts AND array sorts as opaque-typed params */
        Smt2ArraySort param_arr_sort;
        int param_is_arr = _parse_array_sort(fe, psort, &param_arr_sort);
        if (param_is_arr == 1 || _is_opaque_sort(fe, psort)) {
            fd->param_is_sort[i] = 1;
            fd->param_widths[i] = 0;
        } else {
            fd->param_is_sort[i] = 0;
            fd->param_widths[i] = _parse_bitvec_sort(fe, psort);
            if (fd->param_widths[i] == 0) {
                fprintf(fe->err, "error: define-fun parameter sort unsupported\n");
                return -1;
            }
        }
    }

    fd->body = _sexpr_deep_copy(&fe->persistent_arena, body_s);
    if (!fd->body) {
        fprintf(fe->err, "error: out of memory deep-copying define-fun body\n");
        return -1;
    }

    fe->n_funs++;
    return 0;
}

static int _ensure_compiled(Smt2Frontend *fe);

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
    if (fe->compiled) fe->has_aux = 1;
    return 0;
}

static int _ensure_compiled(Smt2Frontend *fe) {
    if (fe->ctx) return 0;
    fe->problem = builder_finalize(fe->builder, &fe->problem_size);
    if (!fe->problem) return -1;
    fe->ctx_buf_size = CTX_BUF_SIZE;
    fe->ctx_buf = malloc(fe->ctx_buf_size);
    if (!fe->ctx_buf) return -1;
    fe->block_alloc = zsp_block_alloc_create(NULL, BA_BLOCK_SIZE);
    if (!fe->block_alloc) return -1;
    fe->ctx = solver_create(fe->ctx_buf, fe->ctx_buf_size, fe->block_alloc);
    if (!fe->ctx) return -1;
    int rc = solver_compile(fe->ctx, fe->problem);
    if (rc == 0) {
        fe->compiled = 1;
        builder_reset(fe->builder);
        fe->has_aux = 0;
    }
    return rc;
}

static int _flush_aux(Smt2Frontend *fe) {
    if (!fe->compiled || !fe->has_aux) return 0;
    size_t aux_sz = 0;
    SolveProblem *aux = builder_finalize(fe->builder, &aux_sz);
    if (!aux) return -1;
    int rc = solver_add_constraint(fe->ctx, aux);
    free(aux);
    builder_reset(fe->builder);
    fe->has_aux = 0;
    return rc;
}

static int _cmd_check_sat(Smt2Frontend *fe, const Sexpr *cmd) {
    (void)cmd;

    int crc = _ensure_compiled(fe);
    if (crc == -2) {
        /* UNSAT detected at compile time (propagation contradiction). */
        fprintf(fe->out, "unsat\n");
        fflush(fe->out);
        fe->last_result = SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
    }
    if (crc < 0) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        return -1;
    }

    if (fe->has_result) solver_reset(fe->ctx);

    int frc = _flush_aux(fe);
    if (frc == -2) {
        fprintf(fe->out, "unsat\n");
        fflush(fe->out);
        fe->last_result = SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
    }
    if (frc < 0) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        return -1;
    }

    SolveOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.seed = fe->seed;
    opts.max_conflicts = 10000;
    opts.max_restarts = 1000;

    fe->last_result = solver_solve(fe->ctx, &opts);
    fe->has_result = 1;

    switch (fe->last_result) {
    case SOLVE_OK:      fprintf(fe->out, "sat\n"); break;
    case SOLVE_UNSAT:   fprintf(fe->out, "unsat\n"); break;
    case SOLVE_TIMEOUT: fprintf(fe->out, "unknown\n"); break;
    }
    fflush(fe->out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Array model output helper                                           */
/* ------------------------------------------------------------------ */

static void _emit_array_store_chain(Smt2Frontend *fe, Smt2ArrayVar *av) {
    uint32_t n = av->value->n_elems;
    uint8_t aw = av->sort.addr_width;
    uint8_t dw = av->sort.data_width;

    /* Use a larger buffer to avoid truncation: name (127) + "[1023]" (6) + nul */
    char elem_name[SMT2_MAX_NAME + 16];

    /* Pre-gather element values by looking up "name[i]" in the var table */
    int64_t *vals = (int64_t *)alloca(n * sizeof(int64_t));
    for (uint32_t i = 0; i < n; i++) {
        snprintf(elem_name, sizeof(elem_name), "%s[%u]", av->name, i);
        Smt2Var *vvar = _find_var(fe, elem_name, (uint32_t)strlen(elem_name));
        vals[i] = vvar ? solver_get_value(fe->ctx, vvar->var_id) : 0;
    }

    /* Emit (store (store ... (as const ...) ...) ...) chain.
     * Open n-1 store wrappers, then the base as-const, then close each. */
    for (uint32_t i = 1; i < n; i++) fprintf(fe->out, "(store ");

    fprintf(fe->out,
            "((as const (Array (_ BitVec %u) (_ BitVec %u))) (_ bv%" PRId64 " %u))",
            (unsigned)aw, (unsigned)dw, vals[0], (unsigned)dw);

    for (uint32_t i = 1; i < n; i++) {
        fprintf(fe->out,
                " (_ bv%u %u) (_ bv%" PRId64 " %u))",
                i, (unsigned)aw, vals[i], (unsigned)dw);
    }
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
    int first = 1;
    for (uint32_t i = 0; i < vars_list->list.count; i++) {
        Sexpr *name_s = vars_list->list.items[i];
        if (name_s->kind != SEXPR_SYMBOL) continue;

        if (!first) fprintf(fe->out, "\n ");
        first = 0;

        /* Array variable? */
        Smt2ArrayVar *av = _find_array_var(fe, name_s->sym.str, name_s->sym.len);
        if (av) {
            fprintf(fe->out, "(%.*s ", (int)name_s->sym.len, name_s->sym.str);
            _emit_array_store_chain(fe, av);
            fprintf(fe->out, ")");
            continue;
        }

        Smt2Var *v = _find_var(fe, name_s->sym.str, name_s->sym.len);
        if (!v) {
            fprintf(fe->err, "error: unknown variable in get-value: '%.*s'\n",
                    (int)name_s->sym.len, name_s->sym.str);
            continue;
        }
        int64_t val = solver_get_value(fe->ctx, v->var_id);
        fprintf(fe->out, "(%.*s (_ bv%" PRIu64 " %u))",
                (int)name_s->sym.len, name_s->sym.str,
                (uint64_t)val, (unsigned)v->width);
    }
    fprintf(fe->out, ")\n");
    fflush(fe->out);
    return 0;
}

static int _cmd_get_model(Smt2Frontend *fe, const Sexpr *cmd) {
    (void)cmd;
    if (!fe->has_result || fe->last_result != SOLVE_OK) {
        fprintf(fe->err, "error: get-model requires a prior sat result\n");
        return 0;
    }
    fprintf(fe->out, "(\n");
    /* BV/Bool vars */
    for (uint32_t i = 0; i < fe->n_vars; i++) {
        Smt2Var *v = &fe->vars[i];
        if (strncmp(v->name, "__aux", 5) == 0) continue;
        /* Skip array element vars (they appear as part of array model) */
        if (strchr(v->name, '[') != NULL) continue;
        int64_t val = solver_get_value(fe->ctx, v->var_id);
        fprintf(fe->out, "  (define-fun %s () (_ BitVec %u) (_ bv%" PRIu64 " %u))\n",
                v->name, (unsigned)v->width,
                (uint64_t)val, (unsigned)v->width);
    }
    /* Array vars */
    for (uint32_t i = 0; i < fe->n_array_vars; i++) {
        Smt2ArrayVar *av = &fe->array_vars[i];
        fprintf(fe->out, "  (define-fun %s () (Array (_ BitVec %u) (_ BitVec %u)) ",
                av->name,
                (unsigned)av->sort.addr_width,
                (unsigned)av->sort.data_width);
        _emit_array_store_chain(fe, av);
        fprintf(fe->out, ")\n");
    }
    fprintf(fe->out, ")\n");
    fflush(fe->out);
    return 0;
}

static int _cmd_echo(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_STRING) {
        fprintf(fe->err, "error: echo requires a string\n");
        return 0;
    }
    const Sexpr *s = cmd->list.items[1];
    fprintf(fe->out, "\"%.*s\"\n", (int)s->sym.len, s->sym.str);
    fflush(fe->out);
    return 0;
}

static int _cmd_get_info(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2) {
        fprintf(fe->out, "()\n");
        fflush(fe->out);
        return 0;
    }
    Sexpr *key = cmd->list.items[1];
    if (sexpr_is_keyword(key, ":name")) {
        fprintf(fe->out, "(:name \"dv-solve-smt2\")\n");
    } else if (sexpr_is_keyword(key, ":version")) {
        fprintf(fe->out, "(:version \"0.1.0\")\n");
    } else if (sexpr_is_keyword(key, ":authors")) {
        fprintf(fe->out, "(:authors \"dv-solve contributors\")\n");
    } else {
        fprintf(fe->out, "(:unsupported)\n");
    }
    fflush(fe->out);
    return 0;
}

typedef struct {
    uint32_t cp;
    uint32_t n_vars;
} PushFrame;

#define SMT2_MAX_PUSH 32

static int _cmd_push(Smt2Frontend *fe, const Sexpr *cmd) {
    uint32_t n = 1;
    if (cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_NUMERAL) {
        n = (uint32_t)cmd->list.items[1]->numval;
    }
    int crc = _ensure_compiled(fe);
    if (crc == -2) {
        /* Compile-time UNSAT: simulate a dead push so pop can restore state. */
        fe->last_result = SOLVE_UNSAT;
        fe->has_result = 1;
        if (fe->push_depth < SMT2_MAX_PUSH) {
            fe->push_stack[fe->push_depth] = (uint32_t)-1;
            fe->push_n_vars[fe->push_depth] = fe->n_vars;
            fe->push_depth++;
        }
        return 0;
    }
    if (crc < 0) {
        fprintf(fe->err, "error: push: compile failed\n");
        return -1;
    }
    if (_flush_aux(fe) < 0) return -1;
    if (fe->has_result) solver_reset(fe->ctx);
    fe->has_result = 0;

    for (uint32_t i = 0; i < n; i++) {
        int cp = solver_checkpoint(fe->ctx);
        if (cp < 0) {
            fprintf(fe->err, "error: push: too many checkpoints\n");
            return -1;
        }
        if (fe->push_depth >= SMT2_MAX_PUSH) {
            fprintf(fe->err, "error: push: max push depth exceeded\n");
            return -1;
        }
        fe->push_stack[fe->push_depth++] = (uint32_t)cp;
        fe->push_n_vars[fe->push_depth - 1] = fe->n_vars;
    }
    return 0;
}

static int _cmd_pop(Smt2Frontend *fe, const Sexpr *cmd) {
    uint32_t n = 1;
    if (cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_NUMERAL) {
        n = (uint32_t)cmd->list.items[1]->numval;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (fe->push_depth == 0) {
            fprintf(fe->err, "error: pop: stack underflow\n");
            return -1;
        }
        fe->push_depth--;
        if (fe->ctx && fe->push_stack[fe->push_depth] != (uint32_t)-1) {
            if (fe->has_result) solver_reset(fe->ctx);
            fe->has_result = 0;
            solver_restore(fe->ctx, fe->push_stack[fe->push_depth]);
        }
        fe->n_vars = fe->push_n_vars[fe->push_depth];
    }
    return 0;
}

static int _cmd_check_sat_assuming(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_LIST) {
        fprintf(fe->err, "error: check-sat-assuming requires a literal list\n");
        return -1;
    }
    if (_ensure_compiled(fe) < 0) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        return -1;
    }
    if (fe->has_result) solver_reset(fe->ctx);
    fe->has_result = 0;
    if (_flush_aux(fe) < 0) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        return -1;
    }
    int cp = solver_checkpoint(fe->ctx);
    if (cp < 0) {
        fprintf(fe->err, "error: check-sat-assuming: no checkpoint slot\n");
        return -1;
    }
    Sexpr *lits = cmd->list.items[1];
    int forced_unsat = 0;
    for (uint32_t i = 0; i < lits->list.count; i++) {
        Sexpr *lit = lits->list.items[i];
        const char *name = NULL;
        uint32_t nlen = 0;
        int positive = 1;
        if (lit->kind == SEXPR_SYMBOL) {
            name = lit->sym.str; nlen = lit->sym.len;
        } else if (lit->kind == SEXPR_LIST && lit->list.count == 2 &&
                   sexpr_is_symbol(lit->list.items[0], "not") &&
                   lit->list.items[1]->kind == SEXPR_SYMBOL) {
            name = lit->list.items[1]->sym.str;
            nlen = lit->list.items[1]->sym.len;
            positive = 0;
        } else {
            fprintf(fe->err, "error: malformed assumption literal\n");
            solver_restore(fe->ctx, (uint32_t)cp);
            return -1;
        }
        Smt2Var *v = _find_var(fe, name, nlen);
        if (!v) {
            fprintf(fe->err, "error: unknown assumption var '%.*s'\n", (int)nlen, name);
            solver_restore(fe->ctx, (uint32_t)cp);
            return -1;
        }
        int rc = solver_pin_var(fe->ctx, v->var_id, positive ? 1 : 0);
        if (rc != 0) { forced_unsat = 1; break; }
    }

    if (forced_unsat) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = SOLVE_UNSAT;
    } else {
        SolveOpts opts;
        memset(&opts, 0, sizeof(opts));
        opts.seed = fe->seed;
        fe->last_result = solver_solve(fe->ctx, &opts);
        switch (fe->last_result) {
        case SOLVE_OK:      fprintf(fe->out, "sat\n"); break;
        case SOLVE_UNSAT:   fprintf(fe->out, "unsat\n"); break;
        case SOLVE_TIMEOUT: fprintf(fe->out, "unknown\n"); break;
        }
    }
    fe->has_result = 1;
    fflush(fe->out);
    (void)cp;
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
    sexpr_arena_init(&fe->persistent_arena, 16384);
}

void smt2_frontend_destroy(Smt2Frontend *fe) {
    if (fe->problem)     free(fe->problem);
    if (fe->builder)     builder_destroy(fe->builder);
    if (fe->ctx)         solver_destroy(fe->ctx);
    if (fe->block_alloc) zsp_block_alloc_destroy(fe->block_alloc);
    free(fe->ctx_buf);
    free(fe->vars);
    sexpr_arena_destroy(&fe->persistent_arena);
    /* Free persistent array var allocations */
    for (uint32_t i = 0; i < fe->n_array_vars; i++) {
        if (fe->array_vars[i].value) {
            free(fe->array_vars[i].value->elems);
            free(fe->array_vars[i].value);
        }
    }
    /* Free any remaining per-command transient allocations */
    _cmd_alloc_reset(fe);
    memset(fe, 0, sizeof(*fe));
}

int smt2_frontend_dispatch(Smt2Frontend *fe, const Sexpr *cmd) {
    /* Reset per-command transient allocations from prior command */
    _cmd_alloc_reset(fe);

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
    if (sexpr_is_symbol(head, "set-info"))
        return 0;
    if (sexpr_is_symbol(head, "declare-sort"))
        return _cmd_declare_sort(fe, cmd);
    if (sexpr_is_symbol(head, "declare-const"))
        return _cmd_declare_const(fe, cmd);
    if (sexpr_is_symbol(head, "declare-fun"))
        return _cmd_declare_const(fe, cmd);
    if (sexpr_is_symbol(head, "define-fun"))
        return _cmd_define_fun(fe, cmd);
    if (sexpr_is_symbol(head, "assert"))
        return _cmd_assert(fe, cmd);
    if (sexpr_is_symbol(head, "check-sat"))
        return _cmd_check_sat(fe, cmd);
    if (sexpr_is_symbol(head, "check-sat-assuming"))
        return _cmd_check_sat_assuming(fe, cmd);
    if (sexpr_is_symbol(head, "get-value"))
        return _cmd_get_value(fe, cmd);
    if (sexpr_is_symbol(head, "get-model"))
        return _cmd_get_model(fe, cmd);
    if (sexpr_is_symbol(head, "get-info"))
        return _cmd_get_info(fe, cmd);
    if (sexpr_is_symbol(head, "push"))
        return _cmd_push(fe, cmd);
    if (sexpr_is_symbol(head, "pop"))
        return _cmd_pop(fe, cmd);
    if (sexpr_is_symbol(head, "echo"))
        return _cmd_echo(fe, cmd);
    if (sexpr_is_symbol(head, "declare-datatype") ||
        sexpr_is_symbol(head, "declare-datatypes")) {
        fprintf(fe->err, "warning: %.*s not supported, ignored\n",
                (int)head->sym.len, head->sym.str);
        return 0;
    }
    if (sexpr_is_symbol(head, "reset") ||
        sexpr_is_symbol(head, "reset-assertions")) {
        fprintf(fe->err, "warning: %.*s not fully implemented\n",
                (int)head->sym.len, head->sym.str);
        return 0;
    }
    if (sexpr_is_symbol(head, "exit"))
        return 1;

    fprintf(fe->err, "(error \"unsupported: %.*s\")\n",
            (int)head->sym.len, head->sym.str);
    return 0;
}
