#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include "smt2/smt2_frontend.h"
#include "zsp_lcg.h"
#include "zsp_bbsolver.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define CTX_BUF_SIZE  (64u * 1024u * 1024u)
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
    /* Grow to fit. n_vars may have been bumped past vars_cap by
     * _next_var_id syncing with backend-allocated ctx->n_vars
     * (see _fresh_aux). Loop until the slot at n_vars is valid. */
    while (fe->n_vars >= fe->vars_cap) {
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
        if (fe->subst_stack[i].expanding) continue;   /* skip in-progress binding */
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
        if (fe->subst_stack[i].expanding) continue;   /* skip in-progress binding */
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
    /* The frontend allocates IDs starting from its own n_vars, but the
     * backend may have allocated extra internal aux vars (constant-aux
     * for reification, ITE result aux, etc.) inside solver_compile that
     * the frontend never saw. Skip past those so a fresh frontend aux
     * doesn't collide with a backend slot already in use. */
    uint32_t id = fe->n_vars;
    if (fe->ctx && fe->ctx->n_vars > id) id = fe->ctx->n_vars;
    return id;
}

static uint32_t _fresh_aux(Smt2Frontend *fe, uint16_t width) {
    uint32_t var_id = _next_var_id(fe);
    uint64_t max_val = (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);
    ExprRef vref = builder_add_var(fe->builder, var_id, (uint8_t)width, 0, 0, (int64_t)max_val);
    /* Mark aux vars as VAR_AUX so search never decides them: they are
     * fully determined by their defining constraint. With the ITE_value
     * cond back-propagation in place, propagation can pin them on its
     * own through whichever branch is consistent with r's domain. */
    builder_mark_var_aux(fe->builder, vref);
    char name[SMT2_MAX_NAME];
    snprintf(name, sizeof(name), "__aux%u", var_id);
    _add_var(fe, name, (uint32_t)strlen(name), var_id, (uint8_t)width);
    /* If _next_var_id skipped past fe->n_vars to dodge backend-internal
     * aux slots, sync fe->n_vars up so the next allocation doesn't
     * collide with the var we just claimed. */
    if (var_id + 1 > fe->n_vars) fe->n_vars = var_id + 1;
    if (fe->compiled) fe->has_aux = 1;
    return var_id;
}

/* ------------------------------------------------------------------ */
/* Array value helpers                                                 */
/* ------------------------------------------------------------------ */

/* Allocate a transient dense Smt2ArrayValue (freed at end of command). Returns
 * NULL for a too-large address space -- intermediate array values (store/ite/
 * as-const results) over a sparse sort aren't materialized densely; the caller
 * turns NULL into an `unknown` result. */
static Smt2ArrayValue *_make_array_value(Smt2Frontend *fe, Smt2ArraySort sort) {
    if (sort.addr_width > SMT2_MAX_ARRAY_ADDR_BITS) return NULL;
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
    av->is_sparse        = 0;
    av->n_sparse         = 0;
    av->sparse_cap       = 0;
    av->sparse_idx       = NULL;
    av->sparse_varid     = NULL;
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

    /* Sparse array: address space too large to expand densely. Element vars are
     * created lazily on first select/store of each concrete index. */
    if (sort.addr_width > SMT2_MAX_ARRAY_ADDR_BITS) {
        Smt2ArrayVar *av = &fe->array_vars[fe->n_array_vars++];
        uint32_t copy_len = nlen < SMT2_MAX_NAME - 1 ? nlen : SMT2_MAX_NAME - 1;
        memcpy(av->name, name, copy_len);
        av->name[copy_len] = '\0';
        av->sort = sort;
        av->value = (Smt2ArrayValue *)calloc(1, sizeof(Smt2ArrayValue));
        if (!av->value) { fe->n_array_vars--; return NULL; }
        av->value->sort            = sort;
        av->value->store_idx_varid = UINT32_MAX;
        av->value->store_val       = EXPR_NULL;
        av->value->is_sparse       = 1;
        return av;
    }

    uint32_t n_elems = 1u << sort.addr_width;
    Smt2ArrayVar *av = &fe->array_vars[fe->n_array_vars++];

    uint32_t copy_len = nlen < SMT2_MAX_NAME - 1 ? nlen : SMT2_MAX_NAME - 1;
    memcpy(av->name, name, copy_len);
    av->name[copy_len] = '\0';
    av->sort = sort;

    av->value = (Smt2ArrayValue *)calloc(1, sizeof(Smt2ArrayValue));
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

/* Look up (or lazily materialize) the element ExprRef for concrete index `k`
 * in a sparse array. Each index maps to a real solver var, so all accesses to
 * that index -- across constraints and get-value -- observe the same variable.
 * Returns EXPR_NULL if the sparse table is full (which the caller turns into an
 * `unknown` result rather than a wrong answer). */
/* Returns 1 and sets *out_varid if concrete index `k` already has an element
 * var; else returns 0. */
static int _sparse_find(Smt2ArrayValue *arr, uint64_t k, uint32_t *out_varid) {
    for (uint32_t i = 0; i < arr->n_sparse; i++)
        if (arr->sparse_idx[i] == k) { *out_varid = arr->sparse_varid[i]; return 1; }
    return 0;
}

static ExprRef _sparse_elem(Smt2Frontend *fe, Smt2ArrayValue *arr,
                            uint64_t k, int create) {
    uint32_t existing;
    if (_sparse_find(arr, k, &existing))
        return builder_expr_var(fe->builder, existing);
    if (!create) return EXPR_NULL;

    if (arr->n_sparse >= SMT2_MAX_SPARSE_ELEMS) {
        fprintf(fe->err, "error: sparse array exceeded %u distinct indices\n",
                (unsigned)SMT2_MAX_SPARSE_ELEMS);
        return EXPR_NULL;
    }
    if (arr->n_sparse == arr->sparse_cap) {
        uint32_t nc = arr->sparse_cap ? arr->sparse_cap * 2 : 8;
        uint64_t *ni = (uint64_t *)realloc(arr->sparse_idx, nc * sizeof(uint64_t));
        if (!ni) return EXPR_NULL;
        arr->sparse_idx = ni;
        uint32_t *nv = (uint32_t *)realloc(arr->sparse_varid, nc * sizeof(uint32_t));
        if (!nv) return EXPR_NULL;
        arr->sparse_varid = nv;
        arr->sparse_cap = nc;
    }

    uint32_t var_id = _next_var_id(fe);
    uint64_t max_val = (arr->sort.data_width >= 64) ? UINT64_MAX
                       : ((1ULL << arr->sort.data_width) - 1);
    builder_add_var(fe->builder, var_id, arr->sort.data_width, 0, 0, (int64_t)max_val);
    char nm[SMT2_MAX_NAME];
    snprintf(nm, sizeof(nm), "__arr%u_%llu", var_id, (unsigned long long)k);
    _add_var(fe, nm, (uint32_t)strlen(nm), var_id, arr->sort.data_width);
    if (var_id + 1 > fe->n_vars) fe->n_vars = var_id + 1;
    arr->sparse_idx[arr->n_sparse] = k;
    arr->sparse_varid[arr->n_sparse] = var_id;
    arr->n_sparse++;
    if (fe->compiled) fe->has_aux = 1;
    return builder_expr_var(fe->builder, var_id);
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

    /* Push bindings. Arguments are translated lazily (call-by-name), which is
     * required so that sort/record-typed arguments -- e.g. a yosys state
     * instance passed to `(define-fun pred ((s S)) ...)` and used inside the
     * body only as `(accessor s)` -- are substituted textually rather than
     * translated bare (a bare record has no expression form). Hygiene against
     * self-referential arguments is handled by the `expanding` guard set in
     * _translate_symbol_tagged (see Smt2Subst.expanding). */
    uint32_t saved_depth = fe->subst_depth;
    for (uint32_t i = 0; i < fd->n_params; i++) {
        Smt2Subst *s = &fe->subst_stack[fe->subst_depth++];
        s->name      = fd->param_names[i];
        s->len       = (uint32_t)strlen(fd->param_names[i]);
        s->value     = call->list.items[i + 1];
        s->has_cache = 0;
        s->expanding = 0;
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
        /* define-fun parameter: lazily translate the argument expression.
         * Mark this entry as expanding so a self-referential argument (an
         * inner symbol with the same name as this parameter) resolves past it
         * to the outer/global binding instead of recursing infinitely. */
        sub_entry->expanding = 1;
        TaggedExpr r = _translate_tagged(fe, sub_entry->value);
        sub_entry->expanding = 0;
        return r;
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

/* Build the SMT-LIB signed division (is_rem=0) or remainder (is_rem=1) of two
 * width-`w` operands, lowered to unsigned bvudiv/bvurem + sign muxing (round
 * toward zero). Reused by bvsdiv/bvsrem and by bvsmod. Returns an ExprRef of
 * width `w`. The caller must set fe->needs_bitblast (the composed ITE is unsound
 * on the CDCL bounds engine). */
static ExprRef _signed_divrem_expr(SolveProblemBuilder *b, ExprRef S_, ExprRef T_,
                                   uint16_t w, int is_rem) {
    uint32_t uop = is_rem ? BIN_MOD : BIN_DIV;
    ExprRef msb_s = builder_expr_extract(b, S_, w - 1, w - 1);
    ExprRef msb_t = builder_expr_extract(b, T_, w - 1, w - 1);
    ExprRef nS = builder_expr_unary(b, UN_NEG, S_);
    ExprRef nT = builder_expr_unary(b, UN_NEG, T_);
    ExprRef q00 = builder_expr_binary(b, uop, S_, T_);
    ExprRef q10 = builder_expr_unary(b, UN_NEG, builder_expr_binary(b, uop, nS, T_));
    ExprRef q01, q11;
    if (is_rem) {   /* remainder: sign follows the dividend s */
        q01 = builder_expr_binary(b, uop, S_, nT);
        q11 = builder_expr_unary(b, UN_NEG, builder_expr_binary(b, uop, nS, nT));
    } else {        /* quotient: sign = sign(s) xor sign(t) */
        q01 = builder_expr_unary(b, UN_NEG, builder_expr_binary(b, uop, S_, nT));
        q11 = builder_expr_binary(b, uop, nS, nT);
    }
    ExprRef hi = builder_expr_ite(b, msb_t, q11, q10);   /* s < 0 */
    ExprRef lo = builder_expr_ite(b, msb_t, q01, q00);   /* s >= 0 */
    return builder_expr_ite(b, msb_s, hi, lo);
}

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
            if (inner.te.ref == EXPR_NULL) return TAGGED_NULL;
            uint16_t new_width = inner.te.width + (uint16_t)ext_n;

            /* Fold zero-extend of a constant: the value is unchanged, but
             * making it a real EXPR_CONST lets downstream passes (e.g. the
             * modular bvadd-with-constant routing) recognise it as a
             * literal. Sign-extend is skipped here -- downstream type
             * interpretation of signed constants is fragile. */
            if (!sign && inner.leaf_kind == 2 && new_width < 64) {
                ExprConst *ec = (ExprConst *)builder_ref_ptr(fe->builder, inner.te.ref);
                uint64_t v = (uint64_t)ec->value & (((uint64_t)1 << new_width) - 1);
                ExprRef cr = builder_expr_const(fe->builder, (int64_t)v, 0);
                return (TaggedExpr){ { cr, new_width }, 2, NULL };
            }

            inner = _flatten_to_var(fe, inner);
            if (inner.te.ref == EXPR_NULL) return TAGGED_NULL;
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

        /* ((_ repeat N) expr) = expr concatenated with itself N times. */
        if (sexpr_is_symbol(op_sym, "repeat")) {
            if (s->list.count != 2) return TAGGED_NULL;
            uint64_t rep_n = head->list.items[2]->numval;
            if (rep_n == 0) return TAGGED_NULL;
            TaggedExpr inner = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
            if (inner.te.ref == EXPR_NULL) return TAGGED_NULL;
            uint16_t w = inner.te.width;
            ExprRef acc = inner.te.ref;              /* N==1 is the identity */
            for (uint64_t i = 1; i < rep_n; i++)
                acc = builder_expr_concat(fe->builder, acc, inner.te.ref, w);
            return (TaggedExpr){ { acc, (uint16_t)(w * rep_n) }, 0, NULL };
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

    /* (! <term> :attr val ...) — annotated term. The attributes (e.g. :named
     * for unsat cores, :pattern) are metadata; the value is just <term>. */
    if (head->kind == SEXPR_SYMBOL && sexpr_is_symbol(head, "!") &&
        s->list.count >= 2) {
        return _translate_tagged(fe, s->list.items[1]);
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
            if (is_const) {
                /* The array element is a VARIABLE (builder_expr_var), so tag it
                 * leaf_kind == 1 (var), NOT 2 (const). Tagging it const made the
                 * const-fold sites (e.g. zero_extend, boolean connectives) read
                 * the var's ExprRef as an ExprConst and fold it to a garbage
                 * literal (0) -> wrong `unsat` for e.g.
                 * `(= ((_ zero_extend N) (select a i)) k)`. */
                if (arr->is_sparse) {
                    ExprRef e = _sparse_elem(fe, arr, k, /*create=*/1);
                    if (e == EXPR_NULL) { fe->incomplete = 1; return TAGGED_NULL; }
                    return (TaggedExpr){ { e, arr->sort.data_width }, 1, NULL };
                }
                if (k < arr->n_elems)
                    return (TaggedExpr){ { arr->elems[k], arr->sort.data_width }, 1, NULL };
            }
        }

        /* Symbolic index. A sparse (large-address) array cannot resolve one
         * without enumerating 2^M entries -> honest unknown rather than a wrong
         * answer. Dense arrays lower to an ITE tree over their elements. */
        if (arr->is_sparse) {
            fprintf(fe->err, "error: symbolic index into a large (sparse) array "
                             "is unsupported -> result will be unknown\n");
            fe->incomplete = 1;
            return TAGGED_NULL;
        }
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

        /* store on a sparse (large-address) array needs a lazy read-over-write
         * copy; not yet implemented, so report unknown rather than guess. */
        if (arr->is_sparse) {
            fprintf(fe->err, "error: store on a large (sparse) array is "
                             "unsupported -> result will be unknown\n");
            fe->incomplete = 1;
            return TAGGED_NULL;
        }

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
    /* These ops are :left-assoc in SMT-LIB, and Verilator emits them variadically
     * (e.g. (bvor a b c)). Left-fold over all operands so both the binary and
     * n-ary forms translate: (op a b c) == (op (op a b) c). */
#define BINOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count < 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        for (uint32_t _i = 2; _i < s->list.count; _i++) { \
            TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[_i])); \
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
            a.te.ref = builder_expr_binary(fe->builder, binop, a.te.ref, b.te.ref); \
        } \
        return (TaggedExpr){ { a.te.ref, a.te.width }, 0, NULL }; \
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

    /* Signed comparisons via the MSB-flip identity:
     *   a <s b  <=>  (a ^ 2^(w-1)) <u (b ^ 2^(w-1))
     * Flipping the sign bit maps signed order onto unsigned (offset-binary)
     * order, so this lowers to already-supported unsigned compare + xor and
     * needs no engine changes. Verilator emits these for signed rand vars. */
#define SCMPOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count != 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2])); \
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
        uint16_t sw = a.te.width ? a.te.width : b.te.width; \
        if (sw == 0 || sw > 64) { \
            fprintf(fe->err, "error: signed compare of width %u unsupported\n", (unsigned)sw); \
            return TAGGED_NULL; \
        } \
        ExprRef bias = builder_expr_const(fe->builder, (int64_t)(1ULL << (sw - 1)), 0); \
        ExprRef af = builder_expr_binary(fe->builder, BIN_BXOR, a.te.ref, bias); \
        ExprRef bf = builder_expr_binary(fe->builder, BIN_BXOR, b.te.ref, bias); \
        ExprRef r = builder_expr_binary(fe->builder, binop, af, bf); \
        return (TaggedExpr){ { r, 1 }, 0, NULL }; \
    }

    SCMPOP_CASE("bvslt", BIN_LT)
    SCMPOP_CASE("bvsle", BIN_LTE)
    SCMPOP_CASE("bvsgt", BIN_GT)
    SCMPOP_CASE("bvsge", BIN_GTE)

#undef BINOP_CASE
#undef CMPOP_CASE
#undef SCMPOP_CASE

    /* Signed division / remainder (SMT-LIB `bvsdiv` / `bvsrem`, round toward
     * zero), lowered to the already-supported unsigned bvudiv/bvurem plus sign
     * muxing. No engine changes: on bitblast the ite/neg/udiv are primitives;
     * on CDCL each flattened sub-op has a propagator. Verilator emits these for
     * signed `/` and `%`.
     *
     *   sdiv(s,t): quadrant by (sign s, sign t) —
     *     (+,+) udiv(s,t)      (-,+) -udiv(-s,t)
     *     (+,-) -udiv(s,-t)    (-,-) udiv(-s,-t)
     *   srem(s,t): sign follows the dividend s —
     *     (+,+) urem(s,t)      (-,+) -urem(-s,t)
     *     (+,-) urem(s,-t)     (-,-) -urem(-s,-t)
     */
    {
        int is_sdiv = (oplen == 6 && memcmp(op, "bvsdiv", 6) == 0);
        int is_srem = (oplen == 6 && memcmp(op, "bvsrem", 6) == 0);
        if (is_sdiv || is_srem) {
            /* The ITE-of-unsigned-divisions this lowers to is solved exactly by
             * bitblast but left unpinned (wrong model) by the CDCL bounds
             * engine, so force bitblast for this problem. */
            fe->needs_bitblast = 1;
            if (s->list.count != 3) return TAGGED_NULL;
            TaggedExpr sa = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
            if (sa.te.ref == EXPR_NULL) return TAGGED_NULL;
            TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
            if (tb.te.ref == EXPR_NULL) return TAGGED_NULL;
            uint16_t w = sa.te.width ? sa.te.width : tb.te.width;
            if (w == 0) return TAGGED_NULL;
            ExprRef r = _signed_divrem_expr(fe->builder, sa.te.ref, tb.te.ref,
                                            w, is_srem);
            return (TaggedExpr){ { r, w }, 0, NULL };
        }
    }

    /* Signed modulo (SMT-LIB `bvsmod`): result sign follows the DIVISOR t.
     * Built on the (validated) signed remainder: let r = bvsrem(s,t);
     *   r == 0                 -> 0
     *   sign(r) == sign(t)     -> r
     *   else                   -> r + t   (flip the sign toward the divisor)
     */
    {
        if (oplen == 6 && memcmp(op, "bvsmod", 6) == 0) {
            fe->needs_bitblast = 1;
            if (s->list.count != 3) return TAGGED_NULL;
            TaggedExpr sa = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
            if (sa.te.ref == EXPR_NULL) return TAGGED_NULL;
            TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
            if (tb.te.ref == EXPR_NULL) return TAGGED_NULL;
            uint16_t w = sa.te.width ? sa.te.width : tb.te.width;
            if (w == 0) return TAGGED_NULL;
            ExprRef T_ = tb.te.ref;
            ExprRef r_expr = _signed_divrem_expr(fe->builder, sa.te.ref, T_, w, /*is_rem=*/1);
            /* Materialise the remainder into its own var: bvsmod references it
             * three times (sign bit, +t, ==0), and re-embedding the raw ITE tree
             * at each use makes the bitblaster produce inconsistent values for
             * the shared sub-DAG. One aux var = one bit-blasted value, shared. */
            TaggedExpr rt = _flatten_to_var(fe, (TaggedExpr){ { r_expr, w }, 0, NULL });
            if (rt.te.ref == EXPR_NULL) return TAGGED_NULL;
            ExprRef r = rt.te.ref;
            ExprRef msb_r = builder_expr_extract(fe->builder, r, w - 1, w - 1);
            ExprRef msb_t = builder_expr_extract(fe->builder, T_, w - 1, w - 1);
            ExprRef same_sign = builder_expr_binary(fe->builder, BIN_EQ, msb_r, msb_t);
            ExprRef r_plus_t = builder_expr_binary(fe->builder, BIN_ADD, r, T_);
            ExprRef adjusted = builder_expr_ite(fe->builder, same_sign, r, r_plus_t);
            ExprRef zero = builder_expr_const(fe->builder, 0, 0);
            ExprRef r_is_zero = builder_expr_binary(fe->builder, BIN_EQ, r, zero);
            ExprRef result = builder_expr_ite(fe->builder, r_is_zero, r, adjusted);
            return (TaggedExpr){ { result, w }, 0, NULL };
        }
    }

    /* bvsmod (signed modulo, sign follows the divisor) is intentionally NOT
     * lowered here: an earlier attempt was only 219/256 correct on bitblast, so
     * it is left to the honest `unknown` path rather than risk a wrong answer.
     * SV `%` lowers to bvsrem (handled above), so bvsmod is comparatively rare. */

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

    /* ---- Boolean connectives (with compile-time const folding) ---- */
    if (oplen == 3 && memcmp(op, "not", 3) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        if (a.leaf_kind == 2) {
            ExprConst *ec = (ExprConst *)builder_ref_ptr(fe->builder, a.te.ref);
            int64_t neg = (ec->value != 0) ? 0 : 1;
            ExprRef cr = builder_expr_const(fe->builder, neg, 0);
            return (TaggedExpr){ { cr, 1 }, 2, NULL };
        }
        ExprRef r = builder_expr_unary(fe->builder, UN_NOT, a.te.ref);
        return (TaggedExpr){ { r, 1 }, 0, NULL };
    }

    if (oplen == 3 && memcmp(op, "and", 3) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        /* Collect non-trivial operands; short-circuit on any literal false. */
        TaggedExpr acc = { { EXPR_NULL, 1 }, 0, NULL };
        for (uint32_t i = 1; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            if (b.leaf_kind == 2) {
                ExprConst *ec = (ExprConst *)builder_ref_ptr(fe->builder, b.te.ref);
                if (ec->value == 0) {
                    /* (and ... false ...) -> false */
                    ExprRef cr = builder_expr_const(fe->builder, 0, 0);
                    return (TaggedExpr){ { cr, 1 }, 2, NULL };
                }
                /* (and ... true ...) -> drop this operand */
                continue;
            }
            if (acc.te.ref == EXPR_NULL) { acc = b; acc.te.width = 1; }
            else {
                acc.te.ref = builder_expr_binary(fe->builder, BIN_AND,
                                                 acc.te.ref, b.te.ref);
                acc.leaf_kind = 0;
            }
        }
        if (acc.te.ref == EXPR_NULL) {
            /* All operands were literal true */
            ExprRef cr = builder_expr_const(fe->builder, 1, 0);
            return (TaggedExpr){ { cr, 1 }, 2, NULL };
        }
        acc.te.width = 1;
        return acc;
    }

    if (oplen == 2 && memcmp(op, "or", 2) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        /* Collect non-trivial operands; short-circuit on any literal true. */
        TaggedExpr acc = { { EXPR_NULL, 1 }, 0, NULL };
        for (uint32_t i = 1; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            if (b.leaf_kind == 2) {
                ExprConst *ec = (ExprConst *)builder_ref_ptr(fe->builder, b.te.ref);
                if (ec->value != 0) {
                    /* (or ... true ...) -> true */
                    ExprRef cr = builder_expr_const(fe->builder, 1, 0);
                    return (TaggedExpr){ { cr, 1 }, 2, NULL };
                }
                /* (or ... false ...) -> drop this operand */
                continue;
            }
            if (acc.te.ref == EXPR_NULL) { acc = b; acc.te.width = 1; }
            else {
                acc.te.ref = builder_expr_binary(fe->builder, BIN_OR,
                                                 acc.te.ref, b.te.ref);
                acc.leaf_kind = 0;
            }
        }
        if (acc.te.ref == EXPR_NULL) {
            /* All operands were literal false */
            ExprRef cr = builder_expr_const(fe->builder, 0, 0);
            return (TaggedExpr){ { cr, 1 }, 2, NULL };
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

    /* ---- Signed arithmetic ops: still deferred (comparisons handled above) ---- */
    if ((oplen == 6 && memcmp(op, "bvsdiv", 6) == 0) ||
        (oplen == 6 && memcmp(op, "bvsrem", 6) == 0) ||
        (oplen == 6 && memcmp(op, "bvsmod", 6) == 0) ||
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
    if (sexpr_is_symbol(l, "QF_BV"))    { fe->logic = SMT2_LOGIC_QF_BV;    return 0; }
    if (sexpr_is_symbol(l, "QF_UFBV"))  { fe->logic = SMT2_LOGIC_QF_UFBV;  return 0; }
    if (sexpr_is_symbol(l, "QF_ABV"))   { fe->logic = SMT2_LOGIC_QF_ABV;   return 0; }
    if (sexpr_is_symbol(l, "QF_AUFBV")) { fe->logic = SMT2_LOGIC_QF_AUFBV; return 0; }
    if (sexpr_is_symbol(l, "ALL"))      { fe->logic = SMT2_LOGIC_ALL;      return 0; }
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

static int _add_sort_fun(Smt2Frontend *fe, const Sexpr *name_s,
                         uint8_t n_params, uint8_t return_width, int is_bool);

/* Translate a yosys/SMT-LIB 2.6 single-constructor record datatype into
 * our existing opaque-sort + per-field sort-fun representation.
 *
 *   (declare-datatypes ((NAME 0)) (((CTOR (FIELD1 TYPE1) (FIELD2 TYPE2) ...))))
 *
 * Becomes equivalent to:
 *   (declare-sort NAME 0)
 *   (declare-fun FIELD1 (NAME) TYPE1)
 *   (declare-fun FIELD2 (NAME) TYPE2)
 *   ...
 *
 * Scope: single-constructor records whose fields are all BitVec or Bool
 * (matches the shape yosys/smtbmc emits for module state). Sum types,
 * recursive types, type parameters, and Array-valued fields are rejected.
 */
static int _cmd_declare_datatypes(Smt2Frontend *fe, const Sexpr *cmd) {
    /* (declare-datatypes <par-list> <defn-list>) */
    if (cmd->list.count != 3) {
        fprintf(fe->err, "error: declare-datatypes expects (pars defs)\n");
        return -1;
    }
    const Sexpr *pars = cmd->list.items[1];
    const Sexpr *defs = cmd->list.items[2];
    if (pars->kind != SEXPR_LIST || defs->kind != SEXPR_LIST) {
        fprintf(fe->err, "error: declare-datatypes: malformed pars/defs\n");
        return -1;
    }
    if (pars->list.count == 0 || pars->list.count != defs->list.count) {
        fprintf(fe->err, "error: declare-datatypes: pars/defs length mismatch\n");
        return -1;
    }

    for (uint32_t di = 0; di < pars->list.count; di++) {
        /* par = (NAME ARITY) */
        const Sexpr *par = pars->list.items[di];
        if (par->kind != SEXPR_LIST || par->list.count != 2 ||
            par->list.items[0]->kind != SEXPR_SYMBOL ||
            par->list.items[1]->kind != SEXPR_NUMERAL) {
            fprintf(fe->err, "error: declare-datatypes: malformed par entry\n");
            return -1;
        }
        const Sexpr *name_s = par->list.items[0];
        if (par->list.items[1]->numval != 0) {
            fprintf(fe->err, "error: declare-datatypes: type parameters "
                    "not supported (got arity %lld for %.*s)\n",
                    (long long)par->list.items[1]->numval,
                    (int)name_s->sym.len, name_s->sym.str);
            return -1;
        }

        /* defs[di] = (CTOR ...) — list of constructors */
        const Sexpr *ctors = defs->list.items[di];
        if (ctors->kind != SEXPR_LIST || ctors->list.count != 1) {
            fprintf(fe->err, "error: declare-datatypes: only single-constructor "
                    "record types are supported (got %u constructors for %.*s)\n",
                    (unsigned)(ctors->kind == SEXPR_LIST ? ctors->list.count : 0u),
                    (int)name_s->sym.len, name_s->sym.str);
            return -1;
        }

        /* Register type name as opaque sort */
        if (fe->n_sort_names >= SMT2_MAX_SORTS) {
            fprintf(fe->err, "error: too many sorts declared\n");
            return -1;
        }
        if (!_is_known_sort(fe, name_s->sym.str, name_s->sym.len)) {
            uint32_t cl = name_s->sym.len < SMT2_MAX_NAME - 1 ?
                          name_s->sym.len : SMT2_MAX_NAME - 1;
            memcpy(fe->sort_names[fe->n_sort_names], name_s->sym.str, cl);
            fe->sort_names[fe->n_sort_names][cl] = '\0';
            fe->n_sort_names++;
        }

        /* ctor = (CTOR_NAME (FIELD TYPE) ...) */
        const Sexpr *ctor = ctors->list.items[0];
        if (ctor->kind != SEXPR_LIST || ctor->list.count < 1 ||
            ctor->list.items[0]->kind != SEXPR_SYMBOL) {
            fprintf(fe->err, "error: declare-datatypes: malformed constructor\n");
            return -1;
        }

        /* Each remaining ctor element is a (FIELD TYPE) selector pair.
         * Field becomes a sort-fun: takes one instance of the record type,
         * returns the BV/Bool field. */
        for (uint32_t fi = 1; fi < ctor->list.count; fi++) {
            const Sexpr *field = ctor->list.items[fi];
            if (field->kind != SEXPR_LIST || field->list.count != 2 ||
                field->list.items[0]->kind != SEXPR_SYMBOL) {
                fprintf(fe->err, "error: declare-datatypes: malformed field\n");
                return -1;
            }
            const Sexpr *fname = field->list.items[0];
            const Sexpr *ftype = field->list.items[1];

            int is_bool = sexpr_is_symbol(ftype, "Bool");
            uint8_t bvw = is_bool ? 1 : _parse_bitvec_sort(fe, ftype);
            if (bvw == 0) {
                fprintf(fe->err, "error: declare-datatypes: field '%.*s' "
                        "has unsupported type (only BitVec and Bool allowed)\n",
                        (int)fname->sym.len, fname->sym.str);
                return -1;
            }
            if (_add_sort_fun(fe, fname, 1, bvw, is_bool) < 0) return -1;
        }
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
        /* Large address spaces are declared sparse inside _declare_array_const
         * (elements materialized lazily at the concrete indices used), so no
         * hard address-width cap here. */
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
        /* Loud but in-sync: an unsupported sort taints the context so the next
         * (check-sat) is `unknown`, rather than exiting and hanging the driver.
         * The var is not created; asserts referencing it also taint. */
        fprintf(fe->err, "error: unsupported sort (only BitVec 1-64, Bool, or "
                         "Array) -> result will be unknown\n");
        fe->incomplete = 1;
        return 0;
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
        /* Loud but in-sync: the assert used a construct we can't translate
         * (unsupported operator/sort). Taint the context so the next
         * (check-sat) answers `unknown` rather than an unsound result, and keep
         * the REPL running so the driver's protocol stream stays synchronized
         * (exiting here would close the pipe and hang a driver blocked on read). */
        fprintf(fe->err, "error: failed to translate assert expression "
                         "(unsupported construct) -> result will be unknown\n");
        fe->incomplete = 1;
        return 0;
    }
    /* Tag user-level asserts with constraint_id=1 so the model-validation
     * pass can distinguish them from internal aux constraints (which are
     * left at id=0). Internal aux constraints can be temporarily out of
     * sync with their associated aux var without affecting the user-
     * visible result; flagging them in the validator produces noise. */
    ExprRef cref = builder_add_constraint(fe->builder, te.ref);
    if (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)builder_ref_ptr(fe->builder, cref);
        if (cs) cs->constraint_id = 1;
    }
    if (fe->compiled) fe->has_aux = 1;
    return 0;
}

/* Initial static-pool size estimate for a problem. The pool holds per-variable
 * (Variable + watcher head + prop ref) and per-constraint (propagator) state,
 * all linear in the problem, atop the 8192-var incremental_capacity_hint floor.
 * The estimate only needs to be right for the common case: grow-and-retry in
 * _ensure_compiled reallocates a larger buffer if this under-provisions. */
static size_t _ctx_buf_size_for(const SolveProblem *p) {
    size_t nv = p ? p->n_vars : 0;
    size_t nc = p ? ((size_t)p->n_constraints + p->n_softs + p->n_dists
                     + p->n_alldiffs) : 0;
    size_t est = (size_t)2 * 1024 * 1024   /* base: 8192-var hint + headroom */
               + nv * 256                  /* per variable */
               + nc * 512;                 /* per constraint (propagator) */
    if (est > (size_t)CTX_BUF_SIZE) est = (size_t)CTX_BUF_SIZE;
    return est;
}

/* Finalize the builder into fe->problem WITHOUT building the CDCL context. The
 * bit-blast engine reads fe->problem directly and never touches fe->ctx, so in
 * Verilator mode (always bit-blast) we skip solver_create + solver_compile here
 * and, symmetrically, the solver_destroy/ctx_buf-free the (reset) would pay --
 * both were pure waste on every randomize(). */
static int _ensure_problem(Smt2Frontend *fe) {
    if (fe->problem) return 0;
    fe->problem = builder_finalize(fe->builder, &fe->problem_size);
    if (!fe->problem) return -1;
    builder_reset(fe->builder);
    fe->has_aux = 0;
    return 0;
}

static int _ensure_compiled(Smt2Frontend *fe) {
    if (fe->ctx) return 0;
    fe->problem = builder_finalize(fe->builder, &fe->problem_size);
    if (!fe->problem) return -1;

    /* The static ctx pool is a fixed-capacity, relocatable bump allocator by
     * design (32-bit offsets, snapshot wholesale for push/pop checkpoints), so
     * it cannot grow in place. Rather than always allocate the 64 MB maximum --
     * a large per-solve cost the frontend pays on every Verilator randomize() --
     * start from a problem-sized estimate and, if solver_compile overflows the
     * pool, reallocate a larger contiguous buffer and recompile. This gives
     * dynamic sizing with no risk of under-provisioning (capped at the old max). */
    size_t sz = _ctx_buf_size_for(fe->problem);
    for (;;) {
        fe->ctx_buf = malloc(sz);
        if (!fe->ctx_buf) return -1;
        fe->ctx_buf_size = sz;
        fe->block_alloc = zsp_block_alloc_create(NULL, BA_BLOCK_SIZE);
        if (!fe->block_alloc) { free(fe->ctx_buf); fe->ctx_buf = NULL; return -1; }
        fe->ctx = solver_create(fe->ctx_buf, sz, fe->block_alloc);
        if (!fe->ctx) {
            zsp_block_alloc_destroy(fe->block_alloc); fe->block_alloc = NULL;
            free(fe->ctx_buf); fe->ctx_buf = NULL;
            return -1;
        }
        /* Vars[] capacity headroom for yosys-smtbmc-style incremental aux vars. */
        fe->ctx->incremental_capacity_hint = 8192;
        int rc = solver_compile(fe->ctx, fe->problem);
        if (rc == 0) {
            fe->compiled = 1;
            builder_reset(fe->builder);
            fe->has_aux = 0;
            return 0;
        }
        /* Grow-and-retry only on a genuine pool overflow (not a compile-time
         * UNSAT, rc == -2, or other error), and only until the cap. */
        if (fe->ctx->pool.overflow && sz < (size_t)CTX_BUF_SIZE) {
            solver_destroy(fe->ctx); fe->ctx = NULL;
            zsp_block_alloc_destroy(fe->block_alloc); fe->block_alloc = NULL;
            free(fe->ctx_buf); fe->ctx_buf = NULL;
            sz = (sz * 4 < (size_t)CTX_BUF_SIZE) ? sz * 4 : (size_t)CTX_BUF_SIZE;
            continue;
        }
        return rc;
    }
}

static int _flush_aux(Smt2Frontend *fe) {
    if (!fe->compiled || !fe->has_aux) return 0;
    size_t aux_sz = 0;
    SolveProblem *aux = builder_finalize(fe->builder, &aux_sz);
    if (!aux) return -1;
    int rc = solver_add_constraint(fe->ctx, aux);
    /* Retain the aux SolveProblem (instead of freeing) so the post-solve
     * model-validation pass can re-evaluate the constraints it contributed.
     * Storage is reclaimed in smt2_frontend_destroy. */
    if (fe->n_aux_problems == fe->aux_problems_cap) {
        uint32_t new_cap = fe->aux_problems_cap ? fe->aux_problems_cap * 2 : 8;
        SolveProblem **grow = (SolveProblem **)realloc(fe->aux_problems,
                                  new_cap * sizeof(SolveProblem *));
        if (!grow) {
            /* Out of memory: fall back to the old behaviour (drop the
             * aux). Validation will be incomplete for this run but the
             * solver result is unaffected. */
            free(aux);
            builder_reset(fe->builder);
            fe->has_aux = 0;
            return rc;
        }
        fe->aux_problems     = grow;
        fe->aux_problems_cap = new_cap;
    }
    fe->aux_problems[fe->n_aux_problems++] = aux;
    builder_reset(fe->builder);
    fe->has_aux = 0;
    return rc;
}

/* FNV-1a fingerprint of a finalized SolveProblem. builder_finalize() zeroes the
 * whole buffer then fills it deterministically from the builder's blocks, and
 * every ExprRef is a pool-relative offset, so identical problems produce
 * byte-identical content and thus the same fingerprint. We hash the leading
 * scalar/head fields plus the pool DATA region, skipping the embedded zsp_pool_t
 * header (which may hold non-deterministic bookkeeping). Used only as a cache
 * key in Verilator mode; a collision would at worst reuse a wrong-but-still-
 * sound model, and the space (64-bit) makes that astronomically unlikely. */
static uint64_t _problem_fingerprint(const SolveProblem *p) {
    uint64_t h = 1469598103934665603ULL;              /* FNV-1a offset basis */
    const uint8_t *lead = (const uint8_t *)p;
    size_t lead_len = (size_t)((const uint8_t *)&p->pool - (const uint8_t *)p);
    for (size_t i = 0; i < lead_len; i++) { h ^= lead[i]; h *= 1099511628211ULL; }
    const uint8_t *data = (const uint8_t *)&p->pool + sizeof(zsp_pool_t);
    size_t used = p->pool.used;
    for (size_t i = 0; i < used; i++) { h ^= data[i]; h *= 1099511628211ULL; }
    return h ? h : 1;                                  /* never return 0 */
}

/* Phase B.0: bit-blast + kissat engine. Bypasses solver_solve() and runs
 * directly on fe->problem. Triggered by DV_ENGINE=bitblast (set by the
 * --engine=bitblast CLI flag). The bbsolver is kept alive on fe->bb_solver
 * so that subsequent (get-value) calls can read back model values. */
static int _check_sat_bitblast(Smt2Frontend *fe) {
    if (!fe->problem) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        return -1;
    }

    /* Verilator-mode identity cache: Verilator re-sends a byte-identical problem
     * after every (reset), differing only in the desired per-bit assignment
     * (which this mode drives via the seeded flip-check, not the SAT search). If
     * the fingerprint matches the cached solved instance we skip the bit-blast +
     * SAT re-solve entirely and just re-diversify the cached model with the new
     * seed -- the dominant per-randomize cost is kissat, so this is the big win.
     * A periodic full re-solve (reseed_period) refreshes the base model so
     * tightly-coupled fields aren't anchored to a single solution forever. */
    uint64_t fp = fe->verilator_mode ? _problem_fingerprint(fe->problem) : 0;
    if (fe->verilator_mode && fe->cache_valid && fp == fe->cached_fp) {
        int force_resolve = fe->reseed_period &&
                            (fe->div_counter % fe->reseed_period == 0);
        if (!force_resolve) {
            if (fe->cached_result == 0) {              /* cached UNSAT */
                fprintf(fe->out, "unsat\n");
                fflush(fe->out);
                fe->last_result = SOLVE_UNSAT;
                fe->has_result = 1;
                return 0;
            }
            if (fe->bb_solver &&
                zsp_bbsolver_rediversify(fe->bb_solver, fe->seed) == 0) {
                fprintf(fe->out, "sat\n");
                fflush(fe->out);
                fe->last_result = SOLVE_OK;
                fe->has_result = 1;
                return 0;
            }
        }
    }

    /* Cache miss (or forced re-solve): drop any prior bbsolver and solve fresh. */
    if (fe->bb_solver) {
        zsp_bbsolver_free(fe->bb_solver);
        fe->bb_solver = NULL;
    }
    fe->bb_solver = zsp_bbsolver_new(NULL, fe->problem);
    if (!fe->bb_solver) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        fe->cache_valid = 0;
        return -1;
    }
    int rc = zsp_bbsolver_check(fe->bb_solver, fe->seed);
    if (fe->verilator_mode && (rc == ZSP_BB_SAT || rc == ZSP_BB_UNSAT)) {
        fe->cached_fp = fp;
        fe->cache_valid = 1;
        fe->cached_result = (rc == ZSP_BB_SAT) ? 1 : 0;
    } else {
        fe->cache_valid = 0;                           /* unknown/error: never reuse */
    }
    if (rc == ZSP_BB_SAT) {
        fprintf(fe->out, "sat\n");
        fe->last_result = SOLVE_OK;
        fe->has_result = 1;
    } else if (rc == ZSP_BB_UNSAT) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = SOLVE_UNSAT;
        fe->has_result = 1;
    } else {
        fprintf(fe->out, "unknown\n");
    }
    fflush(fe->out);
    /* The CDCL path returns 0 for both SAT and UNSAT (only protocol errors
     * are negative). Mirror that — UNSAT is a valid result, not an error. */
    return rc == ZSP_BB_ERROR ? -1 : 0;
}

/* Pick the solve engine for the current problem.
 *
 *   DV_ENGINE=bitblast / bb  → force bitblast
 *   DV_ENGINE=cdcl           → force CDCL
 *   unset                    → auto: bitblast for QF_UFBV / QF_ABV /
 *                              QF_AUFBV (yosys-smtbmc dialect, where the
 *                              CDCL theory loop is dramatically slower
 *                              than bit-blast → AIG → kissat); CDCL for
 *                              QF_BV / ALL / unset logic (the dv-solve
 *                              native workload where CDCL was tuned).
 *
 * Rationale: a 2026-05-26 measurement found that every QF_UFBV BMC
 * fixture in tier2 / tier3 that times out under CDCL solves in <20ms
 * under bitblast. The auto-route lifts those out of the timeout bucket
 * without users having to set DV_ENGINE manually. */
static int _engine_is_bitblast(Smt2Frontend *fe) {
    /* A problem that lowered a CDCL-unsound construct (e.g. signed div/rem)
     * MUST bitblast — even under an explicit DV_ENGINE=cdcl, since CDCL would
     * return a wrong model here. This overrides the env, deliberately. */
    if (fe->needs_bitblast) return 1;
    const char *e = getenv("DV_ENGINE");
    if (e) {
        if (strcmp(e, "bitblast") == 0 || strcmp(e, "bb") == 0) return 1;
        if (strcmp(e, "cdcl") == 0) return 0;
    }
    return fe->logic == SMT2_LOGIC_QF_UFBV
        || fe->logic == SMT2_LOGIC_QF_ABV
        || fe->logic == SMT2_LOGIC_QF_AUFBV;
}

/* Route variable value lookup through the bbsolver when it's the active
 * engine; otherwise fall back to the CDCL solver_get_value path. */
static int64_t _fe_get_var_value(Smt2Frontend *fe, uint32_t var_id) {
    if (fe->bb_solver) {
        int64_t v = 0;
        if (zsp_bbsolver_value(fe->bb_solver, var_id, &v) == 0) return v;
        /* fall through to CDCL on bbsolver miss */
    }
    return solver_get_value(fe->ctx, var_id);
}

static int _cmd_check_sat(Smt2Frontend *fe, const Sexpr *cmd) {
    (void)cmd;

    /* Tainted context (an assert used an unsupported construct): answer honestly
     * with `unknown` rather than solve a problem that is missing constraints. */
    if (fe->incomplete) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        fe->last_result = SOLVE_TIMEOUT;
        fe->has_result = 1;
        return 0;
    }

    /* In Verilator mode every solve draws a fresh seed so the returned model
     * varies run-to-run (that is where randomization diversity comes from --
     * the following check-sat-assuming reuses this model rather than re-solving,
     * so each randomize() costs a single solve). */
    if (fe->verilator_mode)
        fe->seed = ++fe->div_counter;

    /* Verilator fast path: bit-blast only, so finalize the problem but skip the
     * unused CDCL context build (and the reset that would free it). */
    if (fe->verilator_mode && _engine_is_bitblast(fe)) {
        if (_ensure_problem(fe) < 0) {
            fprintf(fe->out, "unknown\n");
            fflush(fe->out);
            return -1;
        }
        return _check_sat_bitblast(fe);
    }

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

    if (_engine_is_bitblast(fe)) {
        return _check_sat_bitblast(fe);
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
    /* Restart cadence: base unit × Luby(i) conflicts per restart.
     * Phase 6.1d A/B on tier1 + tier2-unsat: results were invariant
     * across (mc,mr) in {(10000,1000), (1000,1000), (100,200), (50,5000)}
     * because converging cases finish well below the first restart and
     * stuck cases are stuck on missing propagation, not unlucky restarts.
     * Keep the larger base so currently-solvable kind_k* cases that take
     * many conflicts don't get cut off mid-search. */
    opts.max_conflicts = 10000;
    {
        const char *mc = getenv("DV_MAX_CONFLICTS");
        if (mc && *mc) opts.max_conflicts = (uint32_t)atoi(mc);
    }
    opts.max_restarts  = 1000;
    {
        const char *mr = getenv("DV_MAX_RESTARTS");
        if (mr && *mr) opts.max_restarts = (uint32_t)atoi(mr);
    }

    /* CDCL (lazy clause generation) is on by default; set DV_USE_LCG=0 to
     * disable.  Conflict analysis falls back defensively to bisection
     * when an antecedent propagator lacks an explain callback, so this
     * is safe to leave on for any input. */
    opts.use_lcg = 1;
    {
        const char *ev = getenv("DV_USE_LCG");
        if (ev && *ev == '0') opts.use_lcg = 0;
    }

    /* Phase saving: default on. Net +1 fixture on cross-check after
     * Phase 2 changed clause shapes (98/20 → 99/19): mempartitionknapsack
     * (tier1, sat) and fsm_onehot_d4/d16 (tier2, unsat) recover;
     * regfile_addr_alias d1/d4 (tier3) regress. The tier1 + tier2
     * recovery is the more valuable side of the trade. Disable
     * with DV_USE_PHASE_SAVE=0 if a specific run regresses. */
    opts.use_phase_save = 1;
    {
        const char *ev = getenv("DV_USE_PHASE_SAVE");
        if (ev && *ev == '0') opts.use_phase_save = 0;
    }

    fe->last_result = solver_solve(fe->ctx, &opts);
    fe->has_result = 1;

    if (opts.use_lcg && getenv("DV_LCG_STATS") && fe->ctx->lcg) {
        const LCGCtx *L = (const LCGCtx *)fe->ctx->lcg;
        extern uint64_t lcg_dbg_bail[16];
        fprintf(stderr, "[lcg] conflicts=%llu analyses=%llu learnt=%llu clauses=%u "
                "bail: oNoEx=%llu oExFail=%llu neither=%llu propConf=%llu noSrc=%llu "
                "cNoEx=%llu cExFail=%llu rNoEx=%llu rExFail=%llu\n",
                (unsigned long long)fe->ctx->conflict_count,
                (unsigned long long)lcg_n_analyses(L),
                (unsigned long long)lcg_n_learnt(L),
                lcg_n_clauses(L),
                (unsigned long long)lcg_dbg_bail[0], (unsigned long long)lcg_dbg_bail[1],
                (unsigned long long)lcg_dbg_bail[2], (unsigned long long)lcg_dbg_bail[3],
                (unsigned long long)lcg_dbg_bail[4], (unsigned long long)lcg_dbg_bail[5],
                (unsigned long long)lcg_dbg_bail[6], (unsigned long long)lcg_dbg_bail[7],
                (unsigned long long)lcg_dbg_bail[8]);
    }

    /* Sanity check: if we got SOLVE_OK, re-evaluate every top-level
     * constraint under the returned assignment. A failure here means a
     * constraint was silently dropped (or wrongly compiled) — we can't
     * trust the "sat" answer, so downgrade to unknown.
     *
     * Behaviour controlled by DV_VALIDATE_MODEL env var:
     *   unset / 2 — validate, log, and downgrade sat->unknown on
     *               violation. Default: prefer honest "unknown" over a
     *               possibly-wrong "sat".
     *   1         — validate and log violations but keep the "sat" answer
     *   0         — skip validation entirely (escape hatch) */
    if (fe->last_result == SOLVE_OK && fe->problem) {
        const char *mode_env = getenv("DV_VALIDATE_MODEL");
        int mode = mode_env ? atoi(mode_env) : 2;
        if (mode > 0) {
            int viol = solver_validate_model(fe->ctx, fe->problem, fe->err);
            for (uint32_t i = 0; i < fe->n_aux_problems; i++) {
                viol += solver_validate_model(fe->ctx, fe->aux_problems[i], fe->err);
            }
            if (viol > 0) {
                fprintf(fe->err,
                    "model-validation: %d top-level constraint(s) violated%s\n",
                    viol, mode >= 2 ? "; downgrading sat -> unknown" : "");
                if (mode >= 2) fe->last_result = SOLVE_TIMEOUT;
            }
        }
    }

    /* Escalate to the complete bit-blast engine when CDCL couldn't return a
     * definitive answer -- its interval propagation is imprecise on some
     * bitwise/signed shapes (the model-validation net downgrades those to
     * unknown) and it can also hit its conflict budget. Bitblast is
     * decision-complete for bit-vectors, so this recovers a sound sat/unsat.
     * Guarded on no incremental aux constraints: _check_sat_bitblast solves
     * fe->problem only, so with pending aux it would miss constraints -- there
     * we keep the honest unknown. */
    if (fe->last_result == SOLVE_TIMEOUT && fe->n_aux_problems == 0) {
        return _check_sat_bitblast(fe);
    }

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
        vals[i] = vvar ? _fe_get_var_value(fe, vvar->var_id) : 0;
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

/* ------------------------------------------------------------------ */
/* Sexpr → value evaluator for get-value on define-fun macros          */
/*                                                                     */
/* yosys-smtbmc's cover loop calls (get-value (|UNROLL#N|)) where      */
/* UNROLL#N is a define-fun body, not a declared variable. We evaluate */
/* the body directly against the current model. Returns (value, width) */
/* on success; sets *ok=0 on unsupported/unknown shapes.               */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t value; uint16_t width; } EvalRet;

static EvalRet _eval_sexpr(Smt2Frontend *fe, const Sexpr *s, int *ok);

static uint64_t _trunc(uint64_t v, uint16_t w) {
    if (w == 0 || w >= 64) return v;
    return v & (((uint64_t)1 << w) - 1);
}

static EvalRet _eval_sexpr(Smt2Frontend *fe, const Sexpr *s, int *ok) {
    EvalRet r = { 0, 0 };
    if (!*ok || !s) { *ok = 0; return r; }

    if (s->kind == SEXPR_BITVEC) {
        r.value = s->bv.value;
        r.width = (uint16_t)s->bv.width;
        return r;
    }
    if (s->kind == SEXPR_SYMBOL) {
        if (sexpr_is_symbol(s, "true"))  { r.value = 1; r.width = 1; return r; }
        if (sexpr_is_symbol(s, "false")) { r.value = 0; r.width = 1; return r; }
        Smt2FunDef *fd = _find_fun(fe, s->sym.str, s->sym.len);
        if (fd && fd->n_params == 0) return _eval_sexpr(fe, fd->body, ok);
        Smt2Var *v = _find_var(fe, s->sym.str, s->sym.len);
        if (v) {
            int64_t val = _fe_get_var_value(fe, v->var_id);
            r.value = _trunc((uint64_t)val, v->width);
            r.width = v->width;
            return r;
        }
        *ok = 0;
        return r;
    }
    if (s->kind == SEXPR_NUMERAL) {
        r.value = s->numval;
        r.width = 0; /* width-less; caller widens if used in BV context */
        return r;
    }
    if (s->kind != SEXPR_LIST || s->list.count == 0) { *ok = 0; return r; }

    const Sexpr *head = s->list.items[0];

    /* (_ bvN W) literal */
    if (sexpr_is_symbol(head, "_") && s->list.count == 3 &&
        s->list.items[1]->kind == SEXPR_SYMBOL &&
        s->list.items[2]->kind == SEXPR_NUMERAL) {
        const Sexpr *bv = s->list.items[1];
        if (bv->sym.len > 2 && bv->sym.str[0] == 'b' && bv->sym.str[1] == 'v') {
            uint64_t val = 0;
            for (uint32_t i = 2; i < bv->sym.len; i++) {
                if (bv->sym.str[i] < '0' || bv->sym.str[i] > '9') { *ok = 0; return r; }
                val = val * 10 + (uint64_t)(bv->sym.str[i] - '0');
            }
            r.width = (uint16_t)s->list.items[2]->numval;
            r.value = _trunc(val, r.width);
            return r;
        }
    }

    /* ((_ extract hi lo) x) */
    if (head->kind == SEXPR_LIST && head->list.count == 4 &&
        sexpr_is_symbol(head->list.items[0], "_") &&
        sexpr_is_symbol(head->list.items[1], "extract") &&
        head->list.items[2]->kind == SEXPR_NUMERAL &&
        head->list.items[3]->kind == SEXPR_NUMERAL &&
        s->list.count == 2) {
        uint32_t hi = (uint32_t)head->list.items[2]->numval;
        uint32_t lo = (uint32_t)head->list.items[3]->numval;
        EvalRet inner = _eval_sexpr(fe, s->list.items[1], ok);
        if (!*ok) return r;
        r.width = (uint16_t)(hi - lo + 1);
        r.value = _trunc(inner.value >> lo, r.width);
        return r;
    }

    if (head->kind != SEXPR_SYMBOL) { *ok = 0; return r; }

    /* Variadic logical: and, or */
    if (sexpr_is_symbol(head, "and")) {
        r.value = 1; r.width = 1;
        for (uint32_t i = 1; i < s->list.count; i++) {
            EvalRet a = _eval_sexpr(fe, s->list.items[i], ok);
            if (!*ok) return r;
            if (a.value == 0) { r.value = 0; return r; }
        }
        return r;
    }
    if (sexpr_is_symbol(head, "or")) {
        r.value = 0; r.width = 1;
        for (uint32_t i = 1; i < s->list.count; i++) {
            EvalRet a = _eval_sexpr(fe, s->list.items[i], ok);
            if (!*ok) return r;
            if (a.value != 0) { r.value = 1; return r; }
        }
        return r;
    }
    if (sexpr_is_symbol(head, "not") && s->list.count == 2) {
        EvalRet a = _eval_sexpr(fe, s->list.items[1], ok);
        if (!*ok) return r;
        r.value = a.value ? 0 : 1; r.width = 1;
        return r;
    }
    if (sexpr_is_symbol(head, "=") && s->list.count == 3) {
        EvalRet a = _eval_sexpr(fe, s->list.items[1], ok);
        EvalRet b = _eval_sexpr(fe, s->list.items[2], ok);
        if (!*ok) return r;
        r.value = (a.value == b.value) ? 1 : 0; r.width = 1;
        return r;
    }
    if (sexpr_is_symbol(head, "distinct") && s->list.count == 3) {
        EvalRet a = _eval_sexpr(fe, s->list.items[1], ok);
        EvalRet b = _eval_sexpr(fe, s->list.items[2], ok);
        if (!*ok) return r;
        r.value = (a.value != b.value) ? 1 : 0; r.width = 1;
        return r;
    }
    if (sexpr_is_symbol(head, "ite") && s->list.count == 4) {
        EvalRet c = _eval_sexpr(fe, s->list.items[1], ok);
        if (!*ok) return r;
        return _eval_sexpr(fe, s->list.items[c.value ? 2 : 3], ok);
    }
    if (sexpr_is_symbol(head, "bvnot") && s->list.count == 2) {
        EvalRet a = _eval_sexpr(fe, s->list.items[1], ok);
        if (!*ok) return r;
        r.width = a.width;
        r.value = _trunc(~a.value, r.width);
        return r;
    }
    if (s->list.count == 3) {
        EvalRet a = _eval_sexpr(fe, s->list.items[1], ok);
        EvalRet b = _eval_sexpr(fe, s->list.items[2], ok);
        if (!*ok) return r;
        uint16_t w = a.width > b.width ? a.width : b.width;
        r.width = w;
        if (sexpr_is_symbol(head, "bvand")) { r.value = _trunc(a.value & b.value, w); return r; }
        if (sexpr_is_symbol(head, "bvor"))  { r.value = _trunc(a.value | b.value, w); return r; }
        if (sexpr_is_symbol(head, "bvxor")) { r.value = _trunc(a.value ^ b.value, w); return r; }
        if (sexpr_is_symbol(head, "bvadd")) { r.value = _trunc(a.value + b.value, w); return r; }
        if (sexpr_is_symbol(head, "bvsub")) { r.value = _trunc(a.value - b.value, w); return r; }
        if (sexpr_is_symbol(head, "bvmul")) { r.value = _trunc(a.value * b.value, w); return r; }
        if (sexpr_is_symbol(head, "bvult")) { r.value = (a.value < b.value) ? 1 : 0; r.width = 1; return r; }
        if (sexpr_is_symbol(head, "bvule")) { r.value = (a.value <= b.value) ? 1 : 0; r.width = 1; return r; }
        if (sexpr_is_symbol(head, "bvugt")) { r.value = (a.value > b.value) ? 1 : 0; r.width = 1; return r; }
        if (sexpr_is_symbol(head, "bvuge")) { r.value = (a.value >= b.value) ? 1 : 0; r.width = 1; return r; }
        if (sexpr_is_symbol(head, "concat")) {
            r.width = a.width + b.width;
            r.value = _trunc((a.value << b.width) | b.value, r.width);
            return r;
        }
    }

    *ok = 0;
    return r;
}

/* Emit a bit-vector value as a flat SMT-LIB binary literal `#b<bits>`.
 *
 * We deliberately use `#b...` rather than the indexed `(_ bvN W)` form: the
 * binary literal is a single flat token with no interior parentheses, which
 * every SMT get-value consumer accepts (z3, yosys smtio) AND which Verilator's
 * paren-counting response parser requires -- it does getline(value, ')') and
 * would mis-balance on the nested `)` of `(_ bvN W)`, truncating the model and
 * desyncing the interactive stream. `#b` also sidesteps SMT-LIB's rule that
 * `#x` is only legal when the width is a multiple of 4. Values are limited to
 * 64 bits (the width of the model value); wider bits are emitted as 0. */
static void _emit_bv_bin_literal(FILE *out, uint64_t val, unsigned width) {
    if (width == 0) width = 1;
    fputs("#b", out);
    for (int i = (int)width - 1; i >= 0; i--) {
        unsigned bit = (i < 64) ? (unsigned)((val >> i) & 1u) : 0u;
        fputc(bit ? '1' : '0', out);
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

        if (!first) fprintf(fe->out, "\n ");
        first = 0;

        /* Symbol lookup: declared var, array, or zero-arg define-fun */
        if (name_s->kind == SEXPR_SYMBOL) {
            Smt2ArrayVar *av = _find_array_var(fe, name_s->sym.str, name_s->sym.len);
            if (av) {
                fprintf(fe->out, "(%.*s ", (int)name_s->sym.len, name_s->sym.str);
                _emit_array_store_chain(fe, av);
                fprintf(fe->out, ")");
                continue;
            }

            Smt2Var *v = _find_var(fe, name_s->sym.str, name_s->sym.len);
            if (v) {
                int64_t val = _fe_get_var_value(fe, v->var_id);
                fprintf(fe->out, "(%.*s ", (int)name_s->sym.len, name_s->sym.str);
                _emit_bv_bin_literal(fe->out, (uint64_t)val, (unsigned)v->width);
                fprintf(fe->out, ")");
                continue;
            }
        }

        /* (select <arrayvar> <const-index>): a driver reads array elements
         * this way (Verilator get-values each rand array element). Echo the
         * select as the key and emit the element's model value. */
        if (name_s->kind == SEXPR_LIST && name_s->list.count == 3 &&
            sexpr_is_symbol(name_s->list.items[0], "select") &&
            name_s->list.items[1]->kind == SEXPR_SYMBOL) {
            const Sexpr *arr_s = name_s->list.items[1];
            Smt2ArrayVar *av = _find_array_var(fe, arr_s->sym.str, arr_s->sym.len);
            const Sexpr *idx_s = _resolve_sym(fe, name_s->list.items[2]);
            uint64_t k = 0; int is_const = 0;
            if (idx_s && idx_s->kind == SEXPR_BITVEC) { k = idx_s->bv.value; is_const = 1; }
            else if (idx_s && idx_s->kind == SEXPR_LIST && idx_s->list.count == 3 &&
                     sexpr_is_symbol(idx_s->list.items[0], "_")) {
                uint64_t bv; if (_parse_bv_sym(idx_s->list.items[1], &bv)) { k = bv; is_const = 1; }
            }
            if (av && is_const) {
                uint8_t dw = av->sort.data_width, aw = av->sort.addr_width;
                int64_t val = 0;
                if (av->value->is_sparse) {
                    uint32_t vid;
                    if (_sparse_find(av->value, k, &vid)) {
                        val = _fe_get_var_value(fe, vid);
                    } else if (fe->seed) {
                        /* Element never referenced by a constraint: fully free,
                         * so give it a seeded-random value (a DV solver should
                         * randomize unconstrained elements, not return 0). */
                        uint64_t x = fe->seed
                            + 0x9E3779B97F4A7C15ULL * (k + 1)
                            + 0xD1B54A32D192ED03ULL * (arr_s->sym.len + 1);
                        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
                        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
                        val = (int64_t)(x ^ (x >> 31));
                    }
                } else if (k < av->value->n_elems) {
                    /* Look the element var up by name ("arr[k]") rather than via
                     * value->elems[k]: that ExprRef points into the builder
                     * arena, which is reset after compilation, so it is stale
                     * here (dereferencing it segfaults). */
                    char en[SMT2_MAX_NAME + 16];
                    snprintf(en, sizeof(en), "%.*s[%llu]",
                             (int)arr_s->sym.len, arr_s->sym.str,
                             (unsigned long long)k);
                    Smt2Var *ev = _find_var(fe, en, (uint32_t)strlen(en));
                    if (ev) val = _fe_get_var_value(fe, ev->var_id);
                }
                /* Emit ((select name #x<idx>) #b<value>): the key echoes the
                 * select (hex index padded to the address width -- the form
                 * drivers parse), the value is the element's model. */
                fprintf(fe->out, "((select %.*s #x%0*llx) ",
                        (int)arr_s->sym.len, arr_s->sym.str,
                        (int)((aw + 3) / 4), (unsigned long long)k);
                _emit_bv_bin_literal(fe->out, (uint64_t)val, (unsigned)dw);
                fprintf(fe->out, ")");
                continue;
            }
        }

        /* Fallback: evaluate as a sub-expression (define-fun macro,
         * extract, etc.). yosys-smtbmc's cover loop calls e.g.
         * (get-value (|UNROLL#28|)) where UNROLL#28 is a define-fun. */
        int ok = 1;
        EvalRet ev = _eval_sexpr(fe, name_s, &ok);
        if (ok) {
            uint16_t w = ev.width ? ev.width : 1;
            /* Emit echoing the original expression as the key. For a
             * plain symbol we emit it bare; for a list we print its
             * canonical form (limited to what we evaluated). */
            if (name_s->kind == SEXPR_SYMBOL) {
                fprintf(fe->out, "(%.*s ", (int)name_s->sym.len, name_s->sym.str);
                _emit_bv_bin_literal(fe->out, (uint64_t)ev.value, (unsigned)w);
                fprintf(fe->out, ")");
            } else {
                /* For a list expression, smtbmc typically only queries
                 * symbols, so this branch is rarely hit. Print a
                 * minimal valid response. */
                fprintf(fe->out, "(? ");
                _emit_bv_bin_literal(fe->out, (uint64_t)ev.value, (unsigned)w);
                fprintf(fe->out, ")");
            }
            continue;
        }

        if (name_s->kind == SEXPR_SYMBOL) {
            fprintf(fe->err, "error: unknown variable in get-value: '%.*s'\n",
                    (int)name_s->sym.len, name_s->sym.str);
        } else {
            fprintf(fe->err, "error: unsupported expression in get-value\n");
        }
        /* Emit a placeholder so smtio's parser sees a well-formed pair
         * instead of an empty list (which crashes the parser). */
        if (name_s->kind == SEXPR_SYMBOL) {
            fprintf(fe->out, "(%.*s #b0)", (int)name_s->sym.len, name_s->sym.str);
        } else {
            fprintf(fe->out, "(? #b0)");
        }
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
        int64_t val = _fe_get_var_value(fe, v->var_id);
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
            fe->push_n_array_vars[fe->push_depth] = fe->n_array_vars;
            fe->push_n_aux_problems[fe->push_depth] = fe->n_aux_problems;
            fe->push_incomplete[fe->push_depth] = (uint8_t)fe->incomplete;
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
        fe->push_n_array_vars[fe->push_depth - 1] = fe->n_array_vars;
        fe->push_n_aux_problems[fe->push_depth - 1] = fe->n_aux_problems;
        fe->push_incomplete[fe->push_depth - 1] = (uint8_t)fe->incomplete;
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
        /* Un-taint if the untranslatable assert lived in the popped scope. */
        fe->incomplete = fe->push_incomplete[fe->push_depth];
        if (fe->ctx && fe->push_stack[fe->push_depth] != (uint32_t)-1) {
            /* solver_restore handles trail backtrack itself; calling
             * solver_reset first would zero ctx->trail_top while leaving
             * checkpoint marks pointing at stale TrailEntry addresses,
             * leading to a NULL-deref inside trail_backtrack. */
            fe->has_result = 0;
            solver_restore(fe->ctx, fe->push_stack[fe->push_depth]);
        }
        fe->n_vars = fe->push_n_vars[fe->push_depth];

        /* Free arrays declared inside the popped scope. The solver-side BV
         * vars are already unwound by solver_restore via the n_vars rewind;
         * here we drop the frontend-side tracking and malloc'd element list. */
        uint32_t target_n_arr = fe->push_n_array_vars[fe->push_depth];
        for (uint32_t i = target_n_arr; i < fe->n_array_vars; i++) {
            Smt2ArrayVar *av = &fe->array_vars[i];
            if (av->value) {
                free(av->value->elems);
                free(av->value->sparse_idx);
                free(av->value->sparse_varid);
                free(av->value);
                av->value = NULL;
            }
            av->name[0] = '\0';
        }
        fe->n_array_vars = target_n_arr;

        /* Drop aux problems added between push and pop. The solver-side
         * propagators those aux problems compiled were already marked
         * ENTAILED by solver_restore, but their SolveProblem buffers
         * remained in fe->aux_problems[], which the model-validation
         * pass would still re-evaluate -- producing spurious violations
         * (and downgrading sat -> unknown) for assertions the user
         * popped off the stack. */
        uint32_t target_n_aux = fe->push_n_aux_problems[fe->push_depth];
        for (uint32_t i = target_n_aux; i < fe->n_aux_problems; i++) {
            free(fe->aux_problems[i]);
            fe->aux_problems[i] = NULL;
        }
        fe->n_aux_problems = target_n_aux;
    }
    return 0;
}

static int _cmd_check_sat_assuming(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_LIST) {
        fprintf(fe->err, "error: check-sat-assuming requires a literal list\n");
        return -1;
    }

    /* Tainted context -> honest `unknown` (see _cmd_check_sat). */
    if (fe->incomplete) {
        fprintf(fe->out, "unknown\n");
        fflush(fe->out);
        fe->last_result = SOLVE_TIMEOUT;
        fe->has_result = 1;
        return 0;
    }

    /* Verilator drop-in mode: a check-sat-assuming here is Verilator's
     * randomization-diversity mechanism -- each assumption literal ties one
     * free bit to a random target purely to coax variety out of a deterministic
     * solver. We don't need that: we ignore the assumption pins and return a
     * natively well-distributed model of the base problem, drawn with a fresh
     * seed. This collapses Verilator's up-to-(rand-width) relaxation round-trips
     * to a single solve AND sidesteps the bounds-vs-bits unsoundness of the CDCL
     * pin path (spurious UNSAT at >=7 bit-pins on one variable). Delegating to
     * _cmd_check_sat reuses the sound, auto-routed engine (bitblast for QF_ABV),
     * whose freshly-seeded model is what the following (get-value) reads back.
     * NOTE: only enabled by --mode=verilator; the default path below preserves
     * standard check-sat-assuming semantics for the yosys/sby BMC flow. */
    if (fe->verilator_mode) {
        fe->n_last_assump = 0;
        fe->last_assump_unsat = 0;      /* so a stray get-unsat-assumptions -> () */
        /* Verilator always issues (check-sat) immediately before its diversity
         * (check-sat-assuming), and both solve the same base problem. That prior
         * solve already produced a freshly-seeded model still held in the engine
         * (bb_solver / CDCL ctx), which the following (get-value) will read. So
         * reuse it: just re-emit the cached result -- one solve per randomize().
         * Only re-solve if there is no valid prior result (a check-sat-assuming
         * arriving without a preceding check-sat). */
        if (fe->has_result) {
            switch (fe->last_result) {
            case SOLVE_OK:      fprintf(fe->out, "sat\n");     break;
            case SOLVE_UNSAT:   fprintf(fe->out, "unsat\n");   break;
            default:            fprintf(fe->out, "unknown\n"); break;
            }
            fflush(fe->out);
            return 0;
        }
        return _cmd_check_sat(fe, cmd);
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
    fe->n_last_assump = 0;
    fe->last_assump_unsat = 0;
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
        /* Record the literal so (get-unsat-assumptions) can answer. */
        if (fe->n_last_assump < SMT2_MAX_ASSUMPS) {
            uint32_t k = fe->n_last_assump++;
            uint32_t cpy = nlen < SMT2_MAX_NAME - 1 ? nlen : SMT2_MAX_NAME - 1;
            memcpy(fe->last_assump_names[k], name, cpy);
            fe->last_assump_names[k][cpy] = '\0';
            fe->last_assump_positive[k] = (uint8_t)positive;
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
    fe->last_assump_unsat = (fe->last_result == SOLVE_UNSAT);
    fe->has_result = 1;
    fflush(fe->out);
    (void)cp;
    return 0;
}

/* (get-unsat-assumptions): return a subset of the last check-sat-assuming
 * assumptions that is unsatisfiable together with the assertions. We return the
 * full recorded set -- a valid (non-minimal) answer. Verilator uses this to
 * relax its per-bit randomization targets one at a time until SAT, so a
 * non-minimal core still yields a legal (if less diverse) solution. */
static int _cmd_get_unsat_assumptions(Smt2Frontend *fe, const Sexpr *cmd) {
    (void)cmd;
    fprintf(fe->out, "(");
    if (fe->last_assump_unsat) {
        for (uint32_t i = 0; i < fe->n_last_assump; i++) {
            if (i) fprintf(fe->out, " ");
            if (fe->last_assump_positive[i])
                fprintf(fe->out, "%s", fe->last_assump_names[i]);
            else
                fprintf(fe->out, "(not %s)", fe->last_assump_names[i]);
        }
    }
    fprintf(fe->out, ")\n");
    fflush(fe->out);
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
    /* Verilator-mode cache reseed cadence: force a full re-solve every Nth
     * randomize so coupled fields periodically get a fresh base model.
     * DV_VERILATOR_RESEED=0 (default) = pure cache (max throughput). */
    fe->reseed_period = 0;
    const char *rs = getenv("DV_VERILATOR_RESEED");
    if (rs) { long v = strtol(rs, NULL, 10); if (v > 0) fe->reseed_period = (uint32_t)v; }
}

void smt2_frontend_destroy(Smt2Frontend *fe) {
    if (fe->bb_solver)   { zsp_bbsolver_free(fe->bb_solver); fe->bb_solver = NULL; }
    if (fe->problem)     free(fe->problem);
    for (uint32_t i = 0; i < fe->n_aux_problems; i++) {
        free(fe->aux_problems[i]);
    }
    free(fe->aux_problems);
    if (fe->builder)     builder_destroy(fe->builder);
    if (fe->ctx)         solver_destroy(fe->ctx);
    if (fe->block_alloc) zsp_block_alloc_destroy(fe->block_alloc);
    free(fe->ctx_buf);
    free(fe->vars);
    sexpr_arena_destroy(&fe->persistent_arena);
    /* Free persistent array var allocations (dense elems[] or sparse map) */
    for (uint32_t i = 0; i < fe->n_array_vars; i++) {
        if (fe->array_vars[i].value) {
            free(fe->array_vars[i].value->elems);
            free(fe->array_vars[i].value->sparse_idx);
            free(fe->array_vars[i].value->sparse_varid);
            free(fe->array_vars[i].value);
        }
    }
    /* Free any remaining per-command transient allocations */
    _cmd_alloc_reset(fe);
    memset(fe, 0, sizeof(*fe));
}

/* Fast (reset): produce the same clean, post-startup state as
 * smt2_frontend_destroy() + smt2_frontend_init(), but REUSE the two largest
 * allocations -- the builder and the persistent s-expr arena -- via their
 * in-place reset primitives instead of free + re-malloc. Verilator issues
 * (reset) after every randomize(), where that allocation churn dominated
 * per-randomize time. Everything else is freed and the struct re-zeroed exactly
 * as destroy()+init() would (so the resulting session state is identical), and
 * the Verilator model cache + session config are carried across. */
static void smt2_frontend_soft_reset(Smt2Frontend *fe) {
    /* Session config + Verilator model cache to carry across the reset. */
    FILE    *out = fe->out, *err = fe->err;
    int      print_stats = fe->print_stats, verilator_mode = fe->verilator_mode;
    uint64_t div_counter = fe->div_counter;
    uint32_t reseed_period = fe->reseed_period;
    zsp_bbsolver_t *bb = fe->bb_solver;
    uint64_t cached_fp = fe->cached_fp;
    int      cache_valid = fe->cache_valid, cached_result = fe->cached_result;
    /* Reused allocations (reset in place below rather than freed). */
    SolveProblemBuilder *builder = fe->builder;
    SexprArena           parena  = fe->persistent_arena;

    /* Free exactly what destroy() frees, EXCEPT builder + persistent_arena. */
    if (fe->problem) free(fe->problem);
    for (uint32_t i = 0; i < fe->n_aux_problems; i++) free(fe->aux_problems[i]);
    free(fe->aux_problems);
    if (fe->ctx)         solver_destroy(fe->ctx);
    if (fe->block_alloc) zsp_block_alloc_destroy(fe->block_alloc);
    free(fe->ctx_buf);
    free(fe->vars);
    for (uint32_t i = 0; i < fe->n_array_vars; i++) {
        if (fe->array_vars[i].value) {
            free(fe->array_vars[i].value->elems);
            free(fe->array_vars[i].value->sparse_idx);
            free(fe->array_vars[i].value->sparse_varid);
            free(fe->array_vars[i].value);
        }
    }
    _cmd_alloc_reset(fe);

    /* Reset the reused allocations to empty (equivalent to a fresh create). */
    builder_reset(builder);
    sexpr_arena_reset(&parena);

    /* Re-zero all state (mirrors init's memset), then restore carried fields. */
    memset(fe, 0, sizeof(*fe));
    fe->out = out;
    fe->err = err;
    fe->print_stats = print_stats;
    fe->verilator_mode = verilator_mode;
    fe->div_counter = div_counter;
    fe->reseed_period = reseed_period;
    fe->bb_solver = bb;
    fe->cached_fp = cached_fp;
    fe->cache_valid = cache_valid;
    fe->cached_result = cached_result;
    fe->builder = builder;
    fe->persistent_arena = parena;
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
    if (sexpr_is_symbol(head, "get-unsat-assumptions"))
        return _cmd_get_unsat_assumptions(fe, cmd);
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
    if (sexpr_is_symbol(head, "declare-datatypes")) {
        return _cmd_declare_datatypes(fe, cmd);
    }
    if (sexpr_is_symbol(head, "declare-datatype")) {
        fprintf(fe->err, "warning: declare-datatype not supported, ignored\n");
        return 0;
    }
    if (sexpr_is_symbol(head, "reset-assertions")) {
        /* No-op: dropping individual asserted constraints from a compiled
         * solver isn't currently supported (would require constraint-level
         * tracking that wasn't part of the Phase-5 IR). For sby/yosys flows
         * this is typically issued at session boundaries where the next
         * (check-sat) re-asserts everything anyway, so a silent no-op is
         * sound for those use cases. If a (check-sat) follows and the
         * cached result is stale, _cmd_check_sat resets via solver_reset. */
        if (fe->has_result) {
            solver_reset(fe->ctx);
            fe->has_result = 0;
        }
        fe->incomplete = 0;
        return 0;
    }
    if (sexpr_is_symbol(head, "reset")) {
        /* SMT-LIB2 (reset): restore the solver to its immediately-post-startup
         * state -- clear all declarations, asserts, define-funs, the logic, and
         * option settings. Verilator issues (reset) after every randomize(), so
         * this must be both sound and fast: smt2_frontend_soft_reset() produces
         * the exact destroy()+init() state but reuses the builder and s-expr
         * arena allocations, and carries the Verilator model cache + session
         * config across. */
        smt2_frontend_soft_reset(fe);
        return 0;
    }
    if (sexpr_is_symbol(head, "exit"))
        return 1;

    fprintf(fe->err, "(error \"unsupported: %.*s\")\n",
            (int)head->sym.len, head->sym.str);
    return 0;
}
