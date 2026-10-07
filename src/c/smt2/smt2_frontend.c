#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include "smt2/smt2_frontend.h"
#include "dvs_lcg.h"
#include "dvs_bbsolver.h"
#include "dvs_cube.h"
#include "dvs_i128.h"
#include "dvs_stackinfo.h"

/* DVS_VERSION comes from src/dv_solve/__version__.py, through CMake. */
#ifndef DVS_VERSION
#define DVS_VERSION "unknown"
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define CTX_BUF_SIZE  (64u * 1024u * 1024u)
#define BA_BLOCK_SIZE (64u * 1024u)

static const TypedExpr TYPED_NULL = { EXPR_NULL, 0 };

static void _builder_touched(Smt2Frontend *fe);

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

/* Raise n_vars, the next free var id, to `n`. vars[] doubles as the name table
 * and every slot below n_vars is scanned, so the skipped slots get empty names.
 * Raising it bare left them uninitialized -- or past vars_cap, for the vars the
 * array engine mints in the problem -- and the next lookup read off the end. */
static void _bump_n_vars(Smt2Frontend *fe, uint32_t n) {
    if (n <= fe->n_vars) return;
    if (n > fe->vars_cap) {
        uint32_t nc = fe->vars_cap ? fe->vars_cap : 16;
        while (nc < n) nc *= 2;
        Smt2Var *g = (Smt2Var *)realloc(fe->vars, nc * sizeof(Smt2Var));
        if (g) { fe->vars = g; fe->vars_cap = nc; }
    }
    uint32_t top = n < fe->vars_cap ? n : fe->vars_cap;
    for (uint32_t i = fe->n_vars; i < top; i++) {
        memset(&fe->vars[i], 0, sizeof(Smt2Var));
        fe->vars[i].var_id = UINT32_MAX;
    }
    fe->n_vars = n;   /* ids stay unique even if the table could not grow */
}

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
    /* Any >64-bit variable is bitblast-only: the CDCL bounds engine models a
     * variable's domain with an int64 [lo,hi], which cannot represent a wide
     * value. Routing here is central — it covers scalar, aux, array-element,
     * and function-return vars alike. */
    if (width > 64) { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "wide-var"; }
    /* n_vars is also the next free id (_next_var_id). An id taken past it --
     * _next_var_id skips the ids a compile used internally -- must move it on,
     * or the next declaration got the SAME id: after a compile with internal
     * vars, `declare z; declare w; z != w` was unsat, and every element of an
     * array declared then was one var. */
    if (var_id + 1 > fe->n_vars) _bump_n_vars(fe, var_id + 1);
    return 0;
}

static Smt2Var *_find_var(Smt2Frontend *fe, const char *name, uint32_t len) {
    uint32_t n = fe->n_vars < fe->vars_cap ? fe->n_vars : fe->vars_cap;
    for (uint32_t i = 0; i < n; i++) {
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

/* Ensure the substitution stack can hold `need` more entries beyond the current
 * depth, growing the heap buffer geometrically. Returns 0 only on OOM (caller
 * then falls back to `unknown`). Never caches a Smt2Subst* across a call that
 * may reserve — lookups re-index fe->subst_stack, so realloc during recursion
 * is safe. */
static int _subst_reserve(Smt2Frontend *fe, uint32_t need) {
    if (fe->subst_depth + need <= fe->subst_cap) return 1;
    uint32_t nc = fe->subst_cap ? fe->subst_cap : SMT2_MAX_SUBST;
    while (nc < fe->subst_depth + need) nc *= 2;
    Smt2Subst *ns = (Smt2Subst *)realloc(fe->subst_stack, (size_t)nc * sizeof(Smt2Subst));
    if (!ns) return 0;
    fe->subst_stack = ns;
    fe->subst_cap   = nc;
    return 1;
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
    if (w == 0 || w > SMT2_MAX_BV_BITS) return 0;
    return (uint8_t)w;
}

/* Upper bound (as an int64) for an *unsigned* bit-vector of `width` bits.
 * A width >= 64 cannot represent its full unsigned range in an int64. Wide
 * (>64-bit) vars are bitblast-only — their true domain is the bit width, not
 * this bound — so INT64_MAX signals "full range" without the -1 aliasing that
 * (int64_t)UINT64_MAX would produce. The 64-bit case keeps its legacy
 * UINT64_MAX(-1) encoding so existing CDCL 64-bit behavior is unchanged. */
static int64_t _bv_unsigned_hi(uint32_t width) {
    if (width > 64)  return INT64_MAX;
    if (width == 64) return (int64_t)UINT64_MAX;
    return (int64_t)((1ULL << width) - 1);
}

/* A width >= 64 value produced by ARITHMETIC / bitwise / shift / ite is a CDCL
 * soundness hazard: the engine models a variable's domain with an int64
 * [lo,hi], so any result landing in [2^63, 2^64) reads as *negative* and breaks
 * unsigned propagation — e.g. `(bvult (bvadd x y) x)` (unsigned-overflow check)
 * returns a wrong `unsat`. Route such a problem to the bit-exact bit-blast
 * engine. Plain comparisons / equality over 64-bit values are sound (they yield
 * a 1-bit result and never materialize a >=2^63-bounded aux var), so they stay
 * on CDCL — this keeps the routing targeted (width-64 arithmetic is rare). */
static void _flag_wide_arith(Smt2Frontend *fe, uint32_t width) {
    if (width >= 64) { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "wide-arith"; }
}

/* Returns 1 on success (populates *out), 0 if not an Array sort,
 * -1 on malformed Array sort. A nested sort is accepted only where the caller
 * handles one (`nested_ok`, the array declarations): elsewhere it is -1 as it
 * always was. */
static int _parse_array_sort_x(Smt2Frontend *fe, const Sexpr *s, Smt2ArraySort *out,
                               int nested_ok) {
    /* (Array (_ BitVec M) (_ BitVec N)) */
    if (!s || s->kind != SEXPR_LIST) return 0;
    if (s->list.count != 3) return 0;
    if (!sexpr_is_symbol(s->list.items[0], "Array")) return 0;

    uint8_t addr_w = _parse_bitvec_sort(fe, s->list.items[1]);
    if (addr_w == 0) return -1;
    out->inner_addr = 0;
    out->_pad[0] = 0;

    /* (Array (_ BitVec I) (Array (_ BitVec J) (_ BitVec N))): one flat array
     * keyed by I ++ J (the key holds 128 bits). */
    Smt2ArraySort inner;
    if (nested_ok && _parse_array_sort_x(fe, s->list.items[2], &inner, 0) == 1) {
        if ((uint32_t)addr_w + inner.addr_width > SMT2_MAX_BV_BITS) return -1;
        out->addr_width = (uint8_t)(addr_w + inner.addr_width);
        out->data_width = inner.data_width;
        out->inner_addr = inner.addr_width;
        return 1;
    }

    uint8_t data_w = _parse_bitvec_sort(fe, s->list.items[2]);
    if (data_w == 0) return -1;

    out->addr_width = addr_w;
    out->data_width = data_w;
    return 1;
}

static int _parse_array_sort(Smt2Frontend *fe, const Sexpr *s, Smt2ArraySort *out) {
    return _parse_array_sort_x(fe, s, out, 0);
}

/* Returns 1 if sort is an opaque (declare-sort) sort name. */
static int _is_opaque_sort(Smt2Frontend *fe, const Sexpr *sort) {
    if (!sort || sort->kind != SEXPR_SYMBOL) return 0;
    return _is_known_sort(fe, sort->sym.str, sort->sym.len);
}

/* ------------------------------------------------------------------ */
/* Parse (_ bvN W) symbol                                              */
/* ------------------------------------------------------------------ */

static int _parse_bv_sym(const Sexpr *sym, uint64_t *val_out, int *overflow_out) {
    if (overflow_out) *overflow_out = 0;
    if (sym->kind != SEXPR_SYMBOL) return 0;
    if (sym->sym.len < 3) return 0;
    if (sym->sym.str[0] != 'b' || sym->sym.str[1] != 'v') return 0;
    uint64_t v = 0;
    int ovf = 0;
    for (uint32_t i = 2; i < sym->sym.len; i++) {
        char c = sym->sym.str[i];
        if (c < '0' || c > '9') return 0;
        uint64_t d = (uint64_t)(c - '0');
        /* Detect a value that no longer fits in 64 bits. W1 represents constant
         * values as 64-bit, so a wider decimal literal must be flagged and the
         * caller must fall back to `unknown` rather than use a truncated value. */
        if (v > (UINT64_MAX - d) / 10u) ovf = 1;
        v = v * 10u + d;
    }
    /* A caller that does not ask about overflow (the array constant-index
     * shortcuts, which key elements by a uint64) must not see a truncated
     * value: report "not a constant" so it takes its general path instead. */
    if (overflow_out) *overflow_out = ovf;
    else if (ovf) return 0;
    *val_out = v;
    return 1;
}

/* Limbs needed to hold the widest accepted bit-vector value. */
#define SMT2_BV_MAX_LIMBS ((SMT2_MAX_BV_BITS + 63) / 64)

/* Parse the `bvN` symbol of `(_ bvN W)` into little-endian 64-bit limbs, as
 * N modulo 2^(64*n_limbs). SMT-LIB's value is N mod 2^W and the caller sizes
 * n_limbs so that W <= 64*n_limbs, so the low W bits are exact even when N
 * itself needs more bits than the limbs hold. Returns 1 on success, 0 if `sym`
 * is not a `bvN` symbol. */
static int _parse_bv_sym_limbs(const Sexpr *sym, uint64_t *limbs, uint32_t n_limbs) {
    if (sym->kind != SEXPR_SYMBOL) return 0;
    if (sym->sym.len < 3) return 0;
    if (sym->sym.str[0] != 'b' || sym->sym.str[1] != 'v') return 0;
    for (uint32_t k = 0; k < n_limbs; k++) limbs[k] = 0;
    for (uint32_t i = 2; i < sym->sym.len; i++) {
        char c = sym->sym.str[i];
        if (c < '0' || c > '9') return 0;
        /* limbs = limbs * 10 + d, carried through 32-bit halves (portable:
         * no 128-bit product needed). */
        uint64_t carry = (uint64_t)(c - '0');
        for (uint32_t k = 0; k < n_limbs; k++) {
            uint64_t lo = (limbs[k] & 0xFFFFFFFFu) * 10u + carry;
            uint64_t hi = (limbs[k] >> 32) * 10u + (lo >> 32);
            limbs[k] = (hi << 32) | (lo & 0xFFFFFFFFu);
            carry = hi >> 32;
        }
    }
    return 1;
}

/* A constant array index, as the 128-bit key (lo, hi) reduced to `width` bits:
 * a #x/#b literal of any accepted width or (_ bvN W). Returns 0 for anything
 * else (a symbolic index). */
static int _const_index(Smt2Frontend *fe, const Sexpr *s, uint32_t width,
                        uint64_t *lo, uint64_t *hi);

/* (k << J) | j over 128-bit keys: the flat key of element j of slice k. */
static void _key_concat(uint64_t k_lo, uint64_t k_hi, uint64_t j_lo, uint64_t j_hi,
                        uint32_t J, uint64_t *lo, uint64_t *hi) {
    uint64_t sl, sh;
    if (J == 0)       { sl = k_lo; sh = k_hi; }
    else if (J >= 64) { sl = 0; sh = J >= 128 ? 0 : k_lo << (J - 64); }
    else              { sl = k_lo << J; sh = (k_hi << J) | (k_lo >> (64 - J)); }
    *lo = sl | j_lo;
    *hi = sh | j_hi;
}

/* ------------------------------------------------------------------ */
/* Aux var creation                                                    */
/* ------------------------------------------------------------------ */

static uint32_t _next_var_id(Smt2Frontend *fe) {
    /* The frontend allocates IDs starting from its own n_vars, but the
     * backend may have allocated extra internal aux vars (constant-aux
     * for reification, ITE result aux, etc.) inside dvs_solver_compile that
     * the frontend never saw. Skip past those so a fresh frontend aux
     * doesn't collide with a backend slot already in use. */
    uint32_t id = fe->n_vars;
    if (fe->ctx && fe->ctx->n_vars > id) id = fe->ctx->n_vars;
    return id;
}

static uint32_t _fresh_aux(Smt2Frontend *fe, uint16_t width) {
    uint32_t var_id = _next_var_id(fe);
    int64_t max_val = _bv_unsigned_hi(width);
    dvs_expr_t vref = dvs_builder_add_var(fe->builder, var_id, (uint8_t)width, 0, 0, max_val);
    /* Mark aux vars as VAR_AUX so search never decides them: they are
     * fully determined by their defining constraint. With the ITE_value
     * cond back-propagation in place, propagation can pin them on its
     * own through whichever branch is consistent with r's domain. */
    dvs_builder_mark_var_aux(fe->builder, vref);
    char name[SMT2_MAX_NAME];
    snprintf(name, sizeof(name), "__aux%u", var_id);
    _add_var(fe, name, (uint32_t)strlen(name), var_id, (uint8_t)width);
    /* If _next_var_id skipped past fe->n_vars to dodge backend-internal
     * aux slots, sync fe->n_vars up so the next allocation doesn't
     * collide with the var we just claimed. */
    _bump_n_vars(fe, var_id + 1);
    _builder_touched(fe);
    return var_id;
}

/* Forward decl: abstract-array node allocator (defined below, used by the
 * abstract branch of _declare_array_const). */
static Smt2ArrayValue *_anode_new(Smt2Frontend *fe, uint8_t akind,
                                  Smt2ArraySort sort);

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
    /* is_abstract is read on every array operand, not only under DV_ARRAY. */
    memset(av, 0, sizeof(*av));
    dvs_expr_t *elems = (dvs_expr_t *)_cmd_alloc(fe, n * sizeof(dvs_expr_t));
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

/* An array of this sort is word-level abstract: always under DV_ARRAY, and by
 * default when its address space is too large to expand densely and something
 * needs more than the sparse path's constant indices. Wider than 64 bits stays
 * sparse, which answers `unknown` for what it cannot do: the refinement
 * compares model values as int64. */
static int _abstract_sort(const Smt2Frontend *fe, Smt2ArraySort sort) {
    if (sort.inner_addr) return 0;     /* nested: constant indices only, sparse */
    if (sort.addr_width > 64 || sort.data_width > 64) return 0;
    if (fe->array_lazy) return 1;
    return fe->array_auto && sort.addr_width > SMT2_MAX_ARRAY_ADDR_BITS;
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

    /* Word-level abstract array (DV_ARRAY): a BASE leaf node with no dense
     * elems[] and no per-element vars, for any address width. Reads become
     * fresh vars; congruence is enforced at solve time. Registered in anodes[]
     * for uniform ownership/lookup with STORE/CONST/ITE nodes. A large array
     * otherwise starts sparse and is promoted on first need (_promote_sparse):
     * constant-index-only arrays, Verilator's unpacked arrays, stay fast. A
     * nested array is always sparse (constant indices only). */
    if (fe->array_lazy && _abstract_sort(fe, sort)) {
        Smt2ArrayValue *base = _anode_new(fe, SMT2_ANODE_BASE, sort);
        if (!base) return NULL;
        Smt2ArrayVar *av = &fe->array_vars[fe->n_array_vars++];
        uint32_t copy_len = nlen < SMT2_MAX_NAME - 1 ? nlen : SMT2_MAX_NAME - 1;
        memcpy(av->name, name, copy_len);
        av->name[copy_len] = '\0';
        av->sort = sort;
        av->value = base;
        return av;
    }

    /* Sparse array: address space too large to expand densely, or nested.
     * Element vars are created lazily on first select/store of each concrete
     * index. */
    if (sort.addr_width > SMT2_MAX_ARRAY_ADDR_BITS || sort.inner_addr) {
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
    av->value->elems  = (dvs_expr_t *)malloc(n_elems * sizeof(dvs_expr_t));
    if (!av->value->elems) {
        free(av->value); av->value = NULL;
        fe->n_array_vars--;
        return NULL;
    }

    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t var_id = _next_var_id(fe);
        int64_t max_val = _bv_unsigned_hi(sort.data_width);
        dvs_builder_add_var(fe->builder, var_id, sort.data_width, 0, 0,
                        max_val);
        char elem_name[SMT2_MAX_NAME];
        snprintf(elem_name, sizeof(elem_name), "%.*s[%u]", (int)copy_len, name, i);
        _add_var(fe, elem_name, (uint32_t)strlen(elem_name), var_id,
                 sort.data_width);
        av->value->elems[i] = dvs_builder_expr_var(fe->builder, var_id);
    }

    _builder_touched(fe);
    return av;
}

/* Look up (or lazily materialize) the element dvs_expr_t for concrete index `k`
 * in a sparse array. Each index maps to a real solver var, so all accesses to
 * that index -- across constraints and get-value -- observe the same variable.
 * Returns EXPR_NULL if the sparse table is full (which the caller turns into an
 * `unknown` result rather than a wrong answer). */
/* Returns 1 and sets *out_varid if concrete index `k` already has an element
 * var; else returns 0. */
static int _sparse_find(Smt2ArrayValue *arr, uint64_t k, uint64_t k_hi,
                        uint32_t *out_varid) {
    for (uint32_t i = 0; i < arr->n_sparse; i++)
        if (arr->sparse_idx[i] == k && arr->sparse_idx_hi[i] == k_hi) {
            *out_varid = arr->sparse_varid[i];
            return 1;
        }
    return 0;
}

static dvs_expr_t _sparse_elem(Smt2Frontend *fe, Smt2ArrayValue *arr,
                            uint64_t k, uint64_t k_hi, int create) {
    uint32_t existing;
    if (_sparse_find(arr, k, k_hi, &existing))
        return dvs_builder_expr_var(fe->builder, existing);
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
        uint64_t *nh = (uint64_t *)realloc(arr->sparse_idx_hi, nc * sizeof(uint64_t));
        if (!nh) return EXPR_NULL;
        arr->sparse_idx_hi = nh;
        uint32_t *nv = (uint32_t *)realloc(arr->sparse_varid, nc * sizeof(uint32_t));
        if (!nv) return EXPR_NULL;
        arr->sparse_varid = nv;
        arr->sparse_cap = nc;
    }

    uint32_t var_id = _next_var_id(fe);
    int64_t max_val = _bv_unsigned_hi(arr->sort.data_width);
    if (arr->has_default)
        dvs_builder_add_var(fe->builder, var_id, arr->sort.data_width, 0,
                            (int64_t)arr->default_val, (int64_t)arr->default_val);
    else
        dvs_builder_add_var(fe->builder, var_id, arr->sort.data_width, 0, 0, max_val);
    char nm[SMT2_MAX_NAME];
    snprintf(nm, sizeof(nm), "__arr%u_%llu", var_id, (unsigned long long)k);
    _add_var(fe, nm, (uint32_t)strlen(nm), var_id, arr->sort.data_width);
    _bump_n_vars(fe, var_id + 1);
    arr->sparse_idx[arr->n_sparse] = k;
    arr->sparse_idx_hi[arr->n_sparse] = k_hi;
    arr->sparse_varid[arr->n_sparse] = var_id;
    arr->n_sparse++;
    _builder_touched(fe);
    return dvs_builder_expr_var(fe->builder, var_id);
}

/* ------------------------------------------------------------------ */
/* Word-level abstract arrays (DV_ARRAY)                               */
/* ------------------------------------------------------------------ */

/* Mint a fresh solver variable that search MAY decide (unlike _fresh_aux, which
 * marks VAR_AUX). Abstract-array read vars are genuinely free on a BASE leaf --
 * only pinned once enough read-over-write / congruence axioms are added -- so
 * marking them aux could wrongly yield `unknown` when search was needed. */
static uint32_t _fresh_read_var(Smt2Frontend *fe, uint16_t width) {
    uint32_t var_id = _next_var_id(fe);
    int64_t max_val = _bv_unsigned_hi(width);
    dvs_builder_add_var(fe->builder, var_id, (uint8_t)width, 0, 0, max_val);
    char name[SMT2_MAX_NAME];
    snprintf(name, sizeof(name), "__rd%u", var_id);
    _add_var(fe, name, (uint32_t)strlen(name), var_id, (uint8_t)width);
    _bump_n_vars(fe, var_id + 1);
    _builder_touched(fe);
    return var_id;
}

/* Allocate + register a persistent abstract-array DAG node (STORE/CONST/ITE).
 * BASE nodes are created directly in _declare_array_const. Returns NULL on OOM. */
static Smt2ArrayValue *_anode_new(Smt2Frontend *fe, uint8_t akind,
                                  Smt2ArraySort sort) {
    if (fe->n_anodes == fe->anodes_cap) {
        uint32_t nc = fe->anodes_cap ? fe->anodes_cap * 2 : 16;
        Smt2ArrayValue **grow = (Smt2ArrayValue **)realloc(
            fe->anodes, nc * sizeof(Smt2ArrayValue *));
        if (!grow) return NULL;
        fe->anodes = grow;
        fe->anodes_cap = nc;
    }
    Smt2ArrayValue *n = (Smt2ArrayValue *)calloc(1, sizeof(Smt2ArrayValue));
    if (!n) return NULL;
    n->sort            = sort;
    n->store_idx_varid = UINT32_MAX;
    n->store_val       = EXPR_NULL;
    n->is_abstract     = 1;
    n->akind           = akind;
    n->store_idx_ref   = EXPR_NULL;
    n->cond_ref        = EXPR_NULL;
    fe->anodes[fe->n_anodes++] = n;
    return n;
}

/* Two operand ExprRefs denote the same value (same var, or same constant, or the
 * same node). Used to hash-cons abstract array nodes so structurally-identical
 * terms -- e.g. (store A i v) written twice, or a define-fun array expanded at
 * two use sites -- map to ONE node, whose reads are then correctly shared. */
/* An SMT-LIB bit-vector literal of `width` bits.
 *
 * The builder API types an UNSIZED constant as a SystemVerilog integer
 * literal (32-bit signed), which would widen the context of every operator it
 * meets: `(bvadd x #x03)` over an 8-bit x must wrap at 8 bits. Every
 * BV-valued constant this front end emits is therefore SIZED. Widths are
 * bounded by SMT2_MAX_BV_BITS (128), which fits the node's uint8_t. */
static dvs_expr_t _bv_const(Smt2Frontend *fe, int64_t value, uint16_t width) {
    if (width == 0 || width > 255)
        return dvs_builder_expr_const(fe->builder, value, 0);
    return dvs_builder_expr_const_sized(fe->builder, value, 0, (uint8_t)width);
}

/* Mark a problem this front end finalized as already explicit (dvs_sv.h):
 * SMT-LIB semantics are exact bit-vector semantics, every constant is sized,
 * so SystemVerilog elaboration must leave it alone. */
static dvs_problem_t *_explicit(dvs_problem_t *p) {
    if (p) p->flags |= DVS_PROBLEM_F_EXPLICIT;
    return p;
}

static int _same_operand(Smt2Frontend *fe, dvs_expr_t a, dvs_expr_t b) {
    if (a == b) return 1;
    if (a == EXPR_NULL || b == EXPR_NULL) return 0;
    ExprKind *ka = (ExprKind *)dvs_builder_ref_ptr(fe->builder, a);
    ExprKind *kb = (ExprKind *)dvs_builder_ref_ptr(fe->builder, b);
    if (!ka || !kb || *ka != *kb) return 0;
    if (*ka == EXPR_VAR)
        return ((ExprVar *)ka)->var_id == ((ExprVar *)kb)->var_id;
    if (*ka == EXPR_CONST)
        return ((ExprConst *)ka)->value == ((ExprConst *)kb)->value &&
               ((ExprConst *)ka)->width == ((ExprConst *)kb)->width;
    return 0;
}

/* Find or create a hash-consed abstract STORE node store(parent, idx, val). */
static Smt2ArrayValue *_anode_store(Smt2Frontend *fe, Smt2ArrayValue *parent,
                                    dvs_expr_t idx_ref, dvs_expr_t val_ref) {
    for (uint32_t i = 0; i < fe->n_anodes; i++) {
        Smt2ArrayValue *n = fe->anodes[i];
        if (n->akind == SMT2_ANODE_STORE && n->parent == parent &&
            _same_operand(fe, n->store_idx_ref, idx_ref) &&
            _same_operand(fe, n->store_val, val_ref))
            return n;
    }
    Smt2ArrayValue *n = _anode_new(fe, SMT2_ANODE_STORE, parent->sort);
    if (!n) return NULL;
    n->parent        = parent;
    n->store_idx_ref = idx_ref;
    n->store_val     = val_ref;
    return n;
}

/* Find or create a hash-consed abstract ITE node ite(cond, then, else). */
static Smt2ArrayValue *_anode_ite(Smt2Frontend *fe, dvs_expr_t cond_ref,
                                  Smt2ArrayValue *then_n, Smt2ArrayValue *else_n) {
    for (uint32_t i = 0; i < fe->n_anodes; i++) {
        Smt2ArrayValue *n = fe->anodes[i];
        if (n->akind == SMT2_ANODE_ITE && n->parent == then_n &&
            n->else_node == else_n && _same_operand(fe, n->cond_ref, cond_ref))
            return n;
    }
    Smt2ArrayValue *n = _anode_new(fe, SMT2_ANODE_ITE, then_n->sort);
    if (!n) return NULL;
    n->cond_ref  = cond_ref;
    n->parent    = then_n;
    n->else_node = else_n;
    return n;
}

/* var_id of an index dvs_expr_t if it is a plain EXPR_VAR, else UINT32_MAX. */
static uint32_t _idx_varid(Smt2Frontend *fe, dvs_expr_t idx_ref) {
    ExprVar *ev = (ExprVar *)dvs_builder_ref_ptr(fe->builder, idx_ref);
    if (ev && ev->kind == EXPR_VAR) return ev->var_id;
    return UINT32_MAX;
}

/* --- areads[] hash index (Lever A) -----------------------------------------
 * A read's identity is (node, idx_varid) when the index is a plain variable
 * (so distinct ExprRefs for the same var dedup), else (node, idx_ref). idx_varid
 * is a deterministic function of idx_ref (_idx_varid), so the key is well
 * defined from (node, idx_ref) alone and lookups/inserts stay consistent. */
static uint32_t _aread_key_hash(const Smt2ArrayValue *node,
                                uint32_t idx_varid, dvs_expr_t idx_ref) {
    uint64_t h = (uint64_t)(uintptr_t)node * 0x9E3779B97F4A7C15ull;
    uint64_t k = (idx_varid != UINT32_MAX) ? ((uint64_t)idx_varid << 1) | 1u
                                           : ((uint64_t)idx_ref  << 1);
    h ^= k + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(h ^ (h >> 29));
}

/* Insert areads[slot] into the hash. Requires a free bucket (caller reserves). */
static void _aread_hash_put(Smt2Frontend *fe, uint32_t slot) {
    Smt2ArrayRead *r = &fe->areads[slot];
    uint32_t mask = fe->aread_hash_cap - 1;
    uint32_t h = _aread_key_hash(r->node, r->idx_varid, r->idx_ref) & mask;
    while (fe->aread_hash[h]) h = (h + 1) & mask;
    fe->aread_hash[h] = slot + 1;
}

/* Ensure the hash can hold `need` reads at <=0.75 load, rebuilding existing
 * slots [0, n_areads) on grow. Returns -1 on OOM (old hash left intact). */
static int _aread_hash_reserve(Smt2Frontend *fe, uint32_t need) {
    if (fe->aread_hash && need * 4 <= fe->aread_hash_cap * 3) return 0;
    uint32_t nc = fe->aread_hash_cap ? fe->aread_hash_cap : 64;
    while (need * 4 > nc * 3) nc *= 2;
    uint32_t *g = (uint32_t *)calloc(nc, sizeof(uint32_t));
    if (!g) return -1;
    free(fe->aread_hash);
    fe->aread_hash = g;
    fe->aread_hash_cap = nc;
    for (uint32_t i = 0; i < fe->n_areads; i++) _aread_hash_put(fe, i);
    return 0;
}

/* Look up the areads slot for (node, idx), or UINT32_MAX. Uses the hash when
 * present, else a linear scan (identical match test). */
static uint32_t _aread_lookup(Smt2Frontend *fe, const Smt2ArrayValue *node,
                              uint32_t idx_varid, dvs_expr_t idx_ref) {
    if (fe->aread_hash) {
        uint32_t mask = fe->aread_hash_cap - 1;
        uint32_t h = _aread_key_hash(node, idx_varid, idx_ref) & mask;
        while (fe->aread_hash[h]) {
            uint32_t slot = fe->aread_hash[h] - 1;
            Smt2ArrayRead *r = &fe->areads[slot];
            if (r->node == node &&
                (idx_varid != UINT32_MAX ? (r->idx_varid == idx_varid)
                                         : (r->idx_ref == idx_ref)))
                return slot;
            h = (h + 1) & mask;
        }
        return UINT32_MAX;
    }
    for (uint32_t i = 0; i < fe->n_areads; i++) {
        Smt2ArrayRead *r = &fe->areads[i];
        if (r->node != node) continue;
        if (idx_varid != UINT32_MAX ? (r->idx_varid == idx_varid)
                                    : (r->idx_ref == idx_ref))
            return i;
    }
    return UINT32_MAX;
}

/* Reserve hash room for one more read BEFORE it is appended (so reserve's
 * rebuild covers only the existing [0, n_areads) and never the new slot). On OOM
 * the hash is dropped (NULL) => linear-scan fallback. Pair with _aread_put_last
 * after the append. */
static void _aread_reserve_one(Smt2Frontend *fe) {
    if (_aread_hash_reserve(fe, fe->n_areads + 1) < 0) {
        free(fe->aread_hash);
        fe->aread_hash = NULL;
        fe->aread_hash_cap = 0;
    }
}
/* Insert the just-appended read (slot n_areads-1). No-op if hash was dropped. */
static void _aread_put_last(Smt2Frontend *fe) {
    if (fe->aread_hash) _aread_hash_put(fe, fe->n_areads - 1);
}

/* Find (or create) the read variable for select(node, idx). Dedups by var_id
 * when the index is a plain variable, else by dvs_expr_t identity. Records the
 * read in areads[]. Returns the read var_id (UINT32_MAX on OOM). */
/* Drop the reads a solve appended past areads[n_areads_user) -- they live in
 * fe->problem's pool, not the builder -- and the per-solve marks on the rest.
 * Called before a translation adds a read and before a solve starts. */
static void _abs_drop_solve_state(Smt2Frontend *fe) {
    if (fe->n_areads > fe->n_areads_user) {
        fe->n_areads = fe->n_areads_user;
        if (fe->aread_hash) {
            memset(fe->aread_hash, 0, fe->aread_hash_cap * sizeof(uint32_t));
            for (uint32_t i = 0; i < fe->n_areads; i++) _aread_hash_put(fe, i);
        }
    }
    for (uint32_t i = 0; i < fe->n_areads; i++) fe->areads[i].emitted = 0;
    for (uint32_t e = 0; e < fe->n_aeqs; e++) fe->aeqs[e].wit_idx_ref = EXPR_NULL;
}

static uint32_t _abs_find_or_create_read(Smt2Frontend *fe, Smt2ArrayValue *node,
                                         dvs_expr_t idx_ref, uint32_t idx_varid,
                                         uint16_t width) {
    if (fe->n_areads > fe->n_areads_user) _abs_drop_solve_state(fe);
    uint32_t slot = _aread_lookup(fe, node, idx_varid, idx_ref);
    if (slot != UINT32_MAX) return fe->areads[slot].read_varid;
    if (fe->n_areads == fe->areads_cap) {
        uint32_t nc = fe->areads_cap ? fe->areads_cap * 2 : 32;
        Smt2ArrayRead *grow = (Smt2ArrayRead *)realloc(
            fe->areads, nc * sizeof(Smt2ArrayRead));
        if (!grow) return UINT32_MAX;
        fe->areads = grow;
        fe->areads_cap = nc;
    }
    _aread_reserve_one(fe);
    uint32_t rv = _fresh_read_var(fe, width);
    Smt2ArrayRead *r = &fe->areads[fe->n_areads++];
    r->node       = node;
    r->idx_ref    = idx_ref;
    r->idx_varid  = idx_varid;
    r->read_varid = rv;
    r->width      = width;
    r->emitted    = 0;
    _aread_put_last(fe);
    fe->n_areads_user = fe->n_areads;
    return rv;
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

static int _const_index(Smt2Frontend *fe, const Sexpr *s, uint32_t width,
                        uint64_t *lo, uint64_t *hi) {
    const Sexpr *r = _resolve_sym(fe, s);
    uint64_t l[SMT2_BV_MAX_LIMBS];
    for (uint32_t i = 0; i < SMT2_BV_MAX_LIMBS; i++) l[i] = 0;
    if (!r || width == 0 || width > SMT2_MAX_BV_BITS) return 0;
    if (r->kind == SEXPR_BITVEC) {
        if (r->bv.width > SMT2_MAX_BV_BITS) return 0;
        if (r->bv.width > 64) {
            if (!r->bv.limbs) return 0;
            for (uint32_t i = 0; i < (r->bv.width + 63u) / 64u; i++) l[i] = r->bv.limbs[i];
        } else {
            l[0] = r->bv.value;
        }
    } else if (r->kind == SEXPR_LIST && r->list.count == 3 &&
               sexpr_is_symbol(r->list.items[0], "_")) {
        if (!_parse_bv_sym_limbs(r->list.items[1], l, SMT2_BV_MAX_LIMBS)) return 0;
    } else {
        return 0;
    }
    if (width < 64)       { l[0] &= ((uint64_t)1 << width) - 1; l[1] = 0; }
    else if (width == 64) { l[1] = 0; }
    else if (width < 128) { l[1] &= ((uint64_t)1 << (width - 64)) - 1; }
    *lo = l[0];
    *hi = l[1];
    return 1;
}

/* Translation-time select on an abstract array: return the read var's dvs_expr_t.
 * The read-over-write / congruence axioms relating it to the store chain are
 * emitted later by _emit_array_axioms (Phase A) or lazily on a model (Phase B). */
static TaggedExpr _abs_select(Smt2Frontend *fe, Smt2ArrayValue *node,
                              dvs_expr_t idx_ref) {
    uint16_t w = node->sort.data_width;
    uint32_t idxv = _idx_varid(fe, idx_ref);
    uint32_t rv = _abs_find_or_create_read(fe, node, idx_ref, idxv, w);
    if (rv == UINT32_MAX) { SMT2_TAINT(fe, "abstract-array read var could not be created"); return TAGGED_NULL; }
    return (TaggedExpr){ { dvs_builder_expr_var(fe->builder, rv), w }, 1, NULL };
}

/* Turn a sparse array into an abstract BASE node in place, when something
 * needs more than constant indices (a symbolic index, store, array = or ite).
 * Each element var already made for a constant index becomes a read at that
 * index, so constraints already built on it keep their meaning. Returns 0, or
 * -1 when the array cannot be promoted (the caller taints). */
static dvs_expr_t _abs_array_eq(Smt2Frontend *fe, Smt2ArrayValue *a,
                                Smt2ArrayValue *b);

static int _promote_sparse(Smt2Frontend *fe, Smt2ArrayValue *arr) {
    if (arr->is_abstract) return 0;
    if (!arr->is_sparse || !_abstract_sort(fe, arr->sort)) return -1;
    /* A pop frees the nodes made in its scope; the declaration it belongs to
     * must go with it, or the array would point at a freed node. */
    uint32_t ai = UINT32_MAX;
    for (uint32_t i = 0; i < fe->n_array_vars; i++)
        if (fe->array_vars[i].value == arr) { ai = i; break; }
    if (ai == UINT32_MAX) return -1;
    if (fe->push_depth > 0 && ai < fe->push_n_array_vars[fe->push_depth - 1]) return -1;
    if (fe->n_anodes == fe->anodes_cap) {
        uint32_t nc = fe->anodes_cap ? fe->anodes_cap * 2 : 16;
        Smt2ArrayValue **grow = (Smt2ArrayValue **)realloc(
            fe->anodes, nc * sizeof(Smt2ArrayValue *));
        if (!grow) return -1;
        fe->anodes = grow;
        fe->anodes_cap = nc;
    }
    if (fe->n_areads > fe->n_areads_user) _abs_drop_solve_state(fe);
    if (fe->n_areads + arr->n_sparse > fe->areads_cap) {
        uint32_t nc = fe->areads_cap ? fe->areads_cap : 32;
        while (nc < fe->n_areads + arr->n_sparse) nc *= 2;
        Smt2ArrayRead *grow = (Smt2ArrayRead *)realloc(fe->areads, nc * sizeof(Smt2ArrayRead));
        if (!grow) return -1;
        fe->areads = grow;
        fe->areads_cap = nc;
    }
    fe->anodes[fe->n_anodes++] = arr;
    arr->is_abstract     = 1;
    arr->akind           = SMT2_ANODE_BASE;
    arr->store_idx_ref   = EXPR_NULL;
    arr->cond_ref        = EXPR_NULL;
    for (uint32_t i = 0; i < arr->n_sparse; i++) {
        _aread_reserve_one(fe);
        Smt2ArrayRead *r = &fe->areads[fe->n_areads++];
        r->node       = arr;
        r->idx_ref    = _bv_const(fe, (int64_t)arr->sparse_idx[i], arr->sort.addr_width);
        r->idx_varid  = UINT32_MAX;
        r->read_varid = arr->sparse_varid[i];
        r->width      = arr->sort.data_width;
        r->emitted    = 0;
        _aread_put_last(fe);
    }
    fe->n_areads_user = fe->n_areads;
    free(arr->sparse_idx);   arr->sparse_idx = NULL;
    free(arr->sparse_idx_hi); arr->sparse_idx_hi = NULL;
    free(arr->sparse_varid); arr->sparse_varid = NULL;
    arr->n_sparse = arr->sparse_cap = 0;
    arr->is_sparse = 0;
    if (arr->has_default) {
        /* The default no longer reaches reads the word-level engine makes:
         * restate it as the array equality it came from. */
        arr->has_default = 0;
        Smt2ArrayValue *c = _anode_new(fe, SMT2_ANODE_CONST, arr->sort);
        if (!c) return -1;
        c->store_val = _bv_const(fe, (int64_t)arr->default_val, arr->sort.data_width);
        dvs_expr_t p = _abs_array_eq(fe, arr, c);
        if (p == EXPR_NULL) return -1;
        dvs_builder_add_constraint(fe->builder,
            dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, p, _bv_const(fe, 1, 1)));
    }
    return 0;
}

/* Promote whichever of two array operands is sparse, so both are abstract.
 * Returns 1 when both are abstract afterwards. */
static int _promote_pair(Smt2Frontend *fe, Smt2ArrayValue *a, Smt2ArrayValue *b) {
    if (!a->is_abstract && _promote_sparse(fe, a) < 0) return 0;
    if (!b->is_abstract && _promote_sparse(fe, b) < 0) return 0;
    return 1;
}

/* Reify an abstract array equality (a == b) onto a fresh boolean var and record
 * it; the consistency + extensionality axioms are emitted by _emit_array_axioms.
 * Returns the boolean var's dvs_expr_t, or EXPR_NULL on OOM. */
static dvs_expr_t _abs_array_eq(Smt2Frontend *fe, Smt2ArrayValue *a,
                             Smt2ArrayValue *b) {
    if (fe->n_aeqs == fe->aeqs_cap) {
        uint32_t nc = fe->aeqs_cap ? fe->aeqs_cap * 2 : 16;
        Smt2ArrayEq *grow = (Smt2ArrayEq *)realloc(fe->aeqs,
                                                   nc * sizeof(Smt2ArrayEq));
        if (!grow) return EXPR_NULL;
        fe->aeqs = grow;
        fe->aeqs_cap = nc;
    }
    uint32_t p = _fresh_read_var(fe, 1);
    Smt2ArrayEq *e = &fe->aeqs[fe->n_aeqs++];
    e->a = a; e->b = b; e->p_varid = p; e->wit_idx_ref = EXPR_NULL;
    return dvs_builder_expr_var(fe->builder, p);
}
static TaggedExpr _translate_tagged_impl(Smt2Frontend *fe, const Sexpr *s);

/* Set by smt2_main to the worker thread's stack size (bytes) so the B12 depth
 * guard sizes itself to the REAL stack the translator runs on. Necessary
 * because RLIMIT_STACK governs only the main thread — the CLI runs the solve on
 * an explicit large-stack pthread, and runtime setrlimit raises are unreliable
 * (Linux may refuse to grow the main stack past its initial reservation). 0 =
 * unset (library / main-thread use) -> fall back to RLIMIT_STACK. */
size_t g_smt2_translate_stack_bytes = 0;

/* B12: maximum _translate_tagged recursion depth before we bail to `unknown`
 * rather than overflow the C stack (SIGSEGV). The translator uses ~8.6 KB of
 * stack per nesting level; budget a conservative 16 KB/level and use 70% of the
 * available stack, so the bound is always comfortably below the true overflow
 * point regardless of how the stack was provisioned. Clamp to a sane range so a
 * missing/unlimited limit can't produce a degenerate bound. Computed once and
 * cached (single-threaded frontend). */
static uint32_t _translate_depth_limit(void) {
    static uint32_t cached = 0;
    if (cached) return cached;
    uint64_t stack_bytes = 0;
    if (g_smt2_translate_stack_bytes) {
        stack_bytes = (uint64_t)g_smt2_translate_stack_bytes;
    } else {
#if defined(_WIN32)
        stack_bytes = (uint64_t)dvs_stack_size();   /* no RLIMIT_STACK */
#else
        struct rlimit rl;
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > 0)
            stack_bytes = (uint64_t)rl.rlim_cur;
#endif
    }
    uint32_t bound;
    if (stack_bytes == 0) {
        /* Unknown / unlimited stack: guard only pathological input (~1.6 GB of
         * stack at 16 KB/level would be needed to reach this depth anyway). */
        bound = 100000;
    } else {
        uint64_t frames = (stack_bytes / (16u * 1024u)) * 7 / 10;
        if (frames < 256)     frames = 256;
        if (frames > 4000000) frames = 4000000;
        bound = (uint32_t)frames;
    }
    cached = bound;
    return cached;
}

/* Depth-guarded entry to the expression translator. All recursion funnels
 * through here (both _translate_expr and the mutual _translate_tagged <->
 * _translate_list_tagged loop), so one guard makes the whole walk crash-proof:
 * past the limit we set `incomplete` (the check-sat prints `unknown`) instead
 * of recursing into a stack overflow. The counter is balanced (increment paired
 * with decrement on every non-bail path) so it returns to 0 after each
 * top-level translate. */
static TaggedExpr _translate_tagged(Smt2Frontend *fe, const Sexpr *s) {
    if (fe->translate_depth >= _translate_depth_limit()) {
        SMT2_TAINT(fe, "expression nesting exceeded the stack-derived depth limit");
        return TAGGED_NULL;
    }
    fe->translate_depth++;
    TaggedExpr r = _translate_tagged_impl(fe, s);
    fe->translate_depth--;
    return r;
}

static TaggedExpr _flatten_to_var(Smt2Frontend *fe, TaggedExpr tg) {
    /* Arrays pass through; they do not need a scalar variable. */
    if (tg.array != NULL) return tg;
    if (tg.te.ref == EXPR_NULL) return TAGGED_NULL;
    if (tg.leaf_kind == 1 || tg.leaf_kind == 2) return tg;

    uint32_t aux_id = _fresh_aux(fe, tg.te.width);
    dvs_expr_t aux_ref = dvs_builder_expr_var(fe->builder, aux_id);
    dvs_expr_t eq = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, aux_ref, tg.te.ref);
    dvs_builder_add_constraint(fe->builder, eq);

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
        int64_t max_val = _bv_unsigned_hi(width);
        dvs_builder_add_var(fe->builder, var_id, width, 0, 0, max_val);
        if (_add_var(fe, mangled, (uint32_t)mlen, var_id, width) < 0) {
            fprintf(fe->err, "error: out of memory creating mangled var\n");
            return TAGGED_NULL;
        }
        v = _find_var(fe, mangled, (uint32_t)mlen);
        _builder_touched(fe);
    }
    dvs_expr_t r = dvs_builder_expr_var(fe->builder, v->var_id);
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
    if (!_subst_reserve(fe, fd->n_params)) {
        fprintf(fe->err, "error: out of memory expanding '%s'\n", fd->name);
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
                                 dvs_expr_t idx) {
    uint32_t n = arr->n_elems;

    /* R1 rewrite: select(store(a, i, v), i) = v
     * The store handler records the var_id of its symbolic index.  If the
     * select index is the same variable, return the stored value directly. */
    if (arr->store_idx_varid != UINT32_MAX) {
        ExprVar *ev = (ExprVar *)dvs_builder_ref_ptr(fe->builder, idx);
        if (ev && ev->kind == EXPR_VAR && ev->var_id == arr->store_idx_varid)
            return (TaggedExpr){ { arr->store_val, arr->sort.data_width }, 0, NULL };
    }

    if (fe->print_stats && n > 64) {
        fprintf(fe->err, "stats: select ITE tree over %u elements\n", n);
    }

    /* Linear chain from n-2 down to 0; last element is the fallthrough. */
    dvs_expr_t result = arr->elems[n - 1];
    for (int32_t i = (int32_t)n - 2; i >= 0; i--) {
        dvs_expr_t idx_const = _bv_const(fe, (int64_t)i, arr->sort.addr_width);
        dvs_expr_t cond = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, idx, idx_const);
        result = dvs_builder_expr_ite(fe->builder, cond, arr->elems[i], result);
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
        dvs_expr_t r = _bv_const(fe, 1, 1);
        return (TaggedExpr){ { r, 1 }, 2, NULL };
    }
    if (sexpr_is_symbol(s, "false")) {
        dvs_expr_t r = _bv_const(fe, 0, 1);
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
        dvs_expr_t r = dvs_builder_expr_var(fe->builder, v->var_id);
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

/* SMT-LIB unsigned division (is_rem=0) or remainder (is_rem=1) of two
 * width-`w` operands, with the zero-divisor case made explicit: a/0 is all
 * ones and a%0 is a. A nonzero constant divisor needs no guard. */
static dvs_expr_t _udivrem_expr(Smt2Frontend *fe, dvs_expr_t A, dvs_expr_t B,
                                uint16_t w, int is_rem) {
    dvs_builder_t *b = fe->builder;
    dvs_expr_t q = dvs_builder_expr_binary(b, is_rem ? DVS_BIN_MOD : DVS_BIN_DIV, A, B);
    const void *bp = dvs_builder_ref_ptr(b, B);
    if (bp && *(const ExprKind *)bp == EXPR_CONST &&
        ((const ExprConst *)bp)->value != 0)
        return q;
    dvs_expr_t zero = _bv_const(fe, 0, w);
    dvs_expr_t b_is_zero = dvs_builder_expr_binary(b, DVS_BIN_EQ, B, zero);
    dvs_expr_t at_zero = is_rem ? A : dvs_builder_expr_unary(b, DVS_UN_INVERT, zero);
    return dvs_builder_expr_ite(b, b_is_zero, at_zero, q);
}

/* Flatten `e` (width `w`) to an aux var; see _flatten_to_var. */
static dvs_expr_t _fv(Smt2Frontend *fe, dvs_expr_t e, uint16_t w) {
    return _flatten_to_var(fe, (TaggedExpr){ { e, w }, 0, NULL }).te.ref;
}

/* -x as 0 - x: the CDCL engine's bvsub propagator wraps mod 2^w; its unary
 * negate does not. */
static dvs_expr_t _neg_expr(Smt2Frontend *fe, dvs_expr_t x, uint16_t w) {
    return dvs_builder_expr_binary(fe->builder, DVS_BIN_SUB, _bv_const(fe, 0, w), x);
}

/* Build the SMT-LIB signed division (is_rem=0) or remainder (is_rem=1) of two
 * width-`w` operands: one unsigned bvudiv/bvurem of the magnitudes plus a sign
 * fix (round toward zero). The quotient is negated when the signs differ, the
 * remainder takes the dividend's sign. MIN's magnitude is 2^(w-1) unsigned,
 * and a zero divisor gives SMT-LIB's results (s/0 = s<0 ? 1 : -1, s%0 = s).
 *
 * One divider, not one per sign quadrant: on bitblast each is a w*w-cell
 * array, and four of them made riscv_loop_instr's `bvsmod` ~3x slower. Every
 * step is its own aux var so the CDCL compile links each one; in that form
 * CDCL answers bvsrem/bvsmod exactly, but its division propagator does not
 * finish bvudiv, so these problems still route to bitblast.
 *
 * Reused by bvsdiv/bvsrem and by bvsmod. Returns an dvs_expr_t of width `w`.
 * The caller must set fe->needs_bitblast. */
static dvs_expr_t _signed_divrem_expr(Smt2Frontend *fe, dvs_expr_t S_, dvs_expr_t T_,
                                   uint16_t w, int is_rem) {
    dvs_builder_t *b = fe->builder;
    dvs_expr_t msb_s = _fv(fe, dvs_builder_expr_extract(b, S_, w - 1, w - 1), 1);
    dvs_expr_t msb_t = _fv(fe, dvs_builder_expr_extract(b, T_, w - 1, w - 1), 1);
    dvs_expr_t mag_s = _fv(fe, dvs_builder_expr_ite(b, msb_s,
                           _fv(fe, _neg_expr(fe, S_, w), w), S_), w);
    dvs_expr_t mag_t = _fv(fe, dvs_builder_expr_ite(b, msb_t,
                           _fv(fe, _neg_expr(fe, T_, w), w), T_), w);
    dvs_expr_t m = _fv(fe, _udivrem_expr(fe, mag_s, mag_t, w, is_rem), w);
    dvs_expr_t neg = is_rem ? msb_s
                   : _fv(fe, dvs_builder_expr_binary(b, DVS_BIN_BXOR, msb_s, msb_t), 1);
    return dvs_builder_expr_ite(b, neg, _fv(fe, _neg_expr(fe, m, w), w), m);
}

/* Both operands are the same variable (after flattening). A comparison of a
 * variable with itself is decided -- and left to the engines it is not:
 * `x <s x` lowers each side to its own `x ^ 2^(w-1)` auxiliary, and the
 * search spends its whole CDCL budget on it at 63 bits (B62). */
static int _same_var(Smt2Frontend *fe, dvs_expr_t a, dvs_expr_t b) {
    const void *pa = dvs_builder_ref_ptr(fe->builder, a);
    const void *pb = dvs_builder_ref_ptr(fe->builder, b);
    if (!pa || !pb || *(const ExprKind *)pa != EXPR_VAR || *(const ExprKind *)pb != EXPR_VAR)
        return 0;
    return ((const ExprVar *)pa)->var_id == ((const ExprVar *)pb)->var_id;
}

/* A literal (leaf_kind 2) of at most 64 bits: 1 and its value, else 0. The
 * connectives below fold these, so Verilator's constant guards --
 * `(=> (__Vbool #b1) B)`, `(bvand (__Vbv ..) #b1)`, an ite on a folded test --
 * never reach the engines as reified constants. A guard left as `1 == 1`
 * is a free boolean to the CDCL search until every operand is decided: a
 * one-line riscv-dv constraint under `(=> (__Vbool #b1) ..)` took 667
 * conflicts and fell to bitblast. */
static int _lit_value(Smt2Frontend *fe, TaggedExpr t, uint64_t *v) {
    if (t.leaf_kind != 2 || t.array != NULL || t.te.width == 0 || t.te.width > 64) return 0;
    const ExprConst *ec = (const ExprConst *)dvs_builder_ref_ptr(fe->builder, t.te.ref);
    if (!ec || ec->kind != EXPR_CONST) return 0;
    uint64_t m = t.te.width == 64 ? ~0ull : ((1ull << t.te.width) - 1);
    *v = (uint64_t)ec->value & m;
    return 1;
}

static TaggedExpr _bool_lit(Smt2Frontend *fe, int v) {
    return (TaggedExpr){ { _bv_const(fe, v ? 1 : 0, 1), 1 }, 2, NULL };
}

static int _is_app(const Sexpr *e, const char *name) {
    size_t n = strlen(name);
    return e->kind == SEXPR_LIST && e->list.count > 0
        && e->list.items[0]->kind == SEXPR_SYMBOL
        && e->list.items[0]->sym.len == n && memcmp(e->list.items[0]->sym.str, name, n) == 0;
}

static TaggedExpr _translate_tagged(Smt2Frontend *fe, const Sexpr *s);
static TaggedExpr _flatten_to_var(Smt2Frontend *fe, TaggedExpr te);

/* `(bvsmod x t) == 0`: t divides x. Equivalent, for every t, to
 * `(bvurem |x| |t|) == 0` -- the signs only decide the remainder's sign, not
 * whether it is zero, and at t == 0 both sides reduce to x == 0 (SMT-LIB's
 * smod/urem by zero return the dividend). riscv_loop_instr's
 * `(limit - init) % step == 0` is this shape. The unsigned remainder stays on
 * the CDCL engine, where a general bvsmod forces the whole problem to
 * bitblast (~4 ms a solve instead of ~0.1 ms). |x| of the most negative value
 * wraps to itself, which read unsigned is the right magnitude. */
static TaggedExpr _smod_is_zero(Smt2Frontend *fe, const Sexpr *smod) {
    if (smod->list.count != 3) return TAGGED_NULL;
    TaggedExpr sa = _flatten_to_var(fe, _translate_tagged(fe, smod->list.items[1]));
    if (sa.te.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, smod->list.items[2]));
    if (tb.te.ref == EXPR_NULL) return TAGGED_NULL;
    uint16_t w = sa.te.width ? sa.te.width : tb.te.width;
    if (w == 0) return TAGGED_NULL;
    dvs_builder_t *b = fe->builder;
    dvs_expr_t mag[2];
    dvs_expr_t ops[2] = { sa.te.ref, tb.te.ref };
    for (int i = 0; i < 2; i++) {
        dvs_expr_t msb = _fv(fe, dvs_builder_expr_extract(b, ops[i], w - 1, w - 1), 1);
        mag[i] = _fv(fe, dvs_builder_expr_ite(b, msb, _fv(fe, _neg_expr(fe, ops[i], w), w),
                                              ops[i]), w);
    }
    dvs_expr_t rem = _fv(fe, _udivrem_expr(fe, mag[0], mag[1], w, /*is_rem=*/1), w);
    dvs_expr_t r = dvs_builder_expr_binary(b, DVS_BIN_EQ, rem, _bv_const(fe, 0, w));
    return (TaggedExpr){ { r, 1 }, 0, NULL };
}

/* Signed comparison `a <op>s b`, lowered via the MSB-flip identity
 *   a <s b  <=>  (a ^ 2^(w-1)) <u (b ^ 2^(w-1))
 * which maps signed order onto unsigned (offset-binary) order.
 *
 * The flipped *variable* side is flattened to a materialised var so its bxor
 * binding is actually compiled and propagates (without this the compare's
 * operand is a raw xor-expr; the reifier can't materialise it, and the invert
 * branch — bvsgt/bvsge — dropped to `unknown`). A *constant* operand has its
 * flip folded into a plain const (the builder does not fold), so _bool_to_var
 * sees a clean var-vs-const inequality (it reifies var-vs-const, not var-vs-var).
 * A signed compare of two vars stays var-vs-var -> uncompiled -> sound `unknown`. */
static TaggedExpr _translate_signed_cmp(Smt2Frontend *fe, const Sexpr *s,
                                        dvs_binop_t binop) {
    if (s->list.count != 3) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
    if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
    if (_same_var(fe, a.te.ref, b.te.ref))
        return _bool_lit(fe, binop == DVS_BIN_LTE || binop == DVS_BIN_GTE);
    uint16_t sw = a.te.width ? a.te.width : b.te.width;
    if (sw == 0 || sw > 64) {
        fprintf(fe->err, "error: signed compare of width %u unsupported\n",
                (unsigned)sw);
        return TAGGED_NULL;
    }
    int64_t  bias_val = (int64_t)(1ULL << (sw - 1));
    uint64_t mask     = (sw < 64) ? (((uint64_t)1 << sw) - 1) : ~0ULL;
    dvs_expr_t  bias     = _bv_const(fe, bias_val, sw);

    /* variable side: flip then flatten to a var (materialises the bxor) */
    dvs_expr_t   af  = dvs_builder_expr_binary(fe->builder, DVS_BIN_BXOR, a.te.ref, bias);
    TaggedExpr aff = _flatten_to_var(fe, (TaggedExpr){ { af, sw }, 0, NULL });
    if (aff.te.ref == EXPR_NULL) return TAGGED_NULL;

    /* rhs: fold the flip when constant, else materialise (var-var -> unknown) */
    dvs_expr_t bref;
    const void *bp = dvs_builder_ref_ptr(fe->builder, b.te.ref);
    if (bp && *(const ExprKind *)bp == EXPR_CONST) {
        int64_t bval = ((const ExprConst *)bp)->value;
        bref = _bv_const(fe,
                   (int64_t)(((uint64_t)bval ^ (uint64_t)bias_val) & mask), sw);
    } else {
        dvs_expr_t bf = dvs_builder_expr_binary(fe->builder, DVS_BIN_BXOR, b.te.ref, bias);
        TaggedExpr bff = _flatten_to_var(fe, (TaggedExpr){ { bf, sw }, 0, NULL });
        if (bff.te.ref == EXPR_NULL) return TAGGED_NULL;
        bref = bff.te.ref;
    }
    dvs_expr_t r = dvs_builder_expr_binary(fe->builder, binop, aff.te.ref, bref);
    return (TaggedExpr){ { r, 1 }, 0, NULL };
}

/* A bit-vector constant wider than 64 bits (W2), given as little-endian limbs.
 *
 * EXPR_CONST carries an int64 value, so a wide constant is built from sized
 * <=64-bit chunks through the ordinary expression builders: a zero_extend of
 * the low limb when every higher limb is zero, else a concat of the chunks.
 * The result is tagged complex (leaf_kind 0), never const: the const-fold
 * sites read ExprConst.value as the whole value, which for a wide constant it
 * is not. Like every >64-bit term it is bitblast-only. */
static TaggedExpr _wide_const(Smt2Frontend *fe, const uint64_t *limbs, uint32_t width) {
    if (width > SMT2_MAX_BV_BITS || !limbs) {
        SMT2_TAINT(fe, "bitvector constant wider than the widest supported sort");
        return TAGGED_NULL;
    }
    { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "wide-const"; }
    uint32_t nl = (width + 63u) / 64u;
    uint64_t v[SMT2_BV_MAX_LIMBS];
    for (uint32_t i = 0; i < nl; i++) v[i] = limbs[i];
    uint32_t top_w = width - 64u * (nl - 1u);
    if (top_w < 64) v[nl - 1] &= ((uint64_t)1 << top_w) - 1;

    int hi_zero = 1;
    for (uint32_t i = 1; i < nl; i++)
        if (v[i]) hi_zero = 0;

    dvs_expr_t r;
    if (hi_zero) {
        dvs_expr_t lo = _bv_const(fe, (int64_t)v[0], 64);
        r = dvs_builder_expr_extend(fe->builder, lo, 64, (uint8_t)width, 0);
    } else {
        r = _bv_const(fe, (int64_t)v[nl - 1], (uint16_t)top_w);
        for (uint32_t i = nl - 1; i-- > 0;)
            r = dvs_builder_expr_concat(fe->builder, r, _bv_const(fe, (int64_t)v[i], 64), 64);
    }
    if (r == EXPR_NULL) return TAGGED_NULL;
    return (TaggedExpr){ { r, (uint16_t)width }, 0, NULL };
}

static TaggedExpr _translate_list_tagged(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count == 0) return TAGGED_NULL;

    Sexpr *head = s->list.items[0];

    /* (_ bvN W) */
    if (sexpr_is_symbol(head, "_")) {
        if (s->list.count < 3) return TAGGED_NULL;
        Sexpr *op = s->list.items[1];
        uint64_t bv_val;
        int bv_ovf = 0;
        if (_parse_bv_sym(op, &bv_val, &bv_ovf)) {
            if (s->list.items[2]->kind != SEXPR_NUMERAL) return TAGGED_NULL;
            uint64_t wn = s->list.items[2]->numval;
            if (wn > 64 || bv_ovf) {
                /* A wide sort, or N >= 2^64: re-read N exactly (mod 2^128,
                 * which is exact mod 2^W for every accepted W). */
                if (wn > SMT2_MAX_BV_BITS) {
                    SMT2_TAINT(fe, "bitvector constant wider than the widest supported sort");
                    return TAGGED_NULL;
                }
                uint64_t limbs[SMT2_BV_MAX_LIMBS];
                _parse_bv_sym_limbs(op, limbs, SMT2_BV_MAX_LIMBS);
                if (wn > 64) return _wide_const(fe, limbs, (uint32_t)wn);
                bv_val = limbs[0];
            }
            /* The value is N mod 2^W (as z3 reads it). An unreduced N >= 2^W
             * made the CDCL engine answer a wrong `unsat` on e.g.
             * `(= x8 (_ bv300 8))`; reduce it here, for every width. */
            if (wn > 0 && wn < 64) bv_val &= ((uint64_t)1 << wn) - 1;
            uint16_t w = (uint16_t)wn;
            dvs_expr_t r = _bv_const(fe, (int64_t)bv_val, w);
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

            /* The native CDCL compile of `r == sign_extend(a)` bounds a wide
             * (>=64-bit) unsigned result var with a negative lower bound, which
             * as an unsigned domain becomes a high sliver -> spurious compile-
             * time UNSAT (verified: sext(x32)->64 == 5 is wrongly unsat). The
             * bit-blast engine lowers sign-extend exactly, so force it. */
            if (sign && new_width >= 64)
                { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "sext64"; }

            /* Fold zero-extend of a constant: the value is unchanged, but
             * making it a real EXPR_CONST lets downstream passes (e.g. the
             * modular bvadd-with-constant routing) recognise it as a
             * literal. Sign-extend is skipped here -- downstream type
             * interpretation of signed constants is fragile. */
            if (!sign && inner.leaf_kind == 2 && new_width < 64) {
                ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, inner.te.ref);
                uint64_t v = (uint64_t)ec->value & (((uint64_t)1 << new_width) - 1);
                dvs_expr_t cr = _bv_const(fe, (int64_t)v, new_width);
                return (TaggedExpr){ { cr, new_width }, 2, NULL };
            }

            inner = _flatten_to_var(fe, inner);
            if (inner.te.ref == EXPR_NULL) return TAGGED_NULL;
            dvs_expr_t r = dvs_builder_expr_extend(fe->builder, inner.te.ref,
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

            dvs_expr_t r = dvs_builder_expr_extract(fe->builder, inner.te.ref, hi, lo);
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
            dvs_expr_t acc = inner.te.ref;              /* N==1 is the identity */
            for (uint64_t i = 1; i < rep_n; i++)
                acc = dvs_builder_expr_concat(fe->builder, acc, inner.te.ref, w);
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

        if (_abstract_sort(fe, sort)) {
            val_te = _flatten_to_var(fe, val_te);
            Smt2ArrayValue *arr = _anode_new(fe, SMT2_ANODE_CONST, sort);
            if (!arr) { SMT2_TAINT(fe, "abstract const-array node allocation failed"); return TAGGED_NULL; }
            arr->store_val = val_te.te.ref;   /* every read == default */
            return (TaggedExpr){ { EXPR_NULL, 0 }, 0, arr };
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
        uint32_t saved = fe->subst_depth;

        /* Phase 1: evaluate ALL value expressions in the current (outer) scope
         * before any binding takes effect — true parallel SMT-LIB2 semantics.
         * Store translated values in a temporary buffer, then push bindings.
         * Small lets use an inline buffer; larger ones heap-allocate. Keeping
         * this off a fixed SMT2_MAX_SUBST-sized stack array is what lets deeply
         * nested `let` recurse (phase 2) without blowing the C stack. */
        TaggedExpr    sv[8];
        const Sexpr  *sn[8];
        TaggedExpr   *let_vals  = sv;
        const Sexpr **let_names = sn;
        int           let_heap  = 0;
        if (n > 8) {
            let_vals  = (TaggedExpr *)malloc((size_t)n * sizeof(TaggedExpr));
            let_names = (const Sexpr **)malloc((size_t)n * sizeof(const Sexpr *));
            if (!let_vals || !let_names) { free(let_vals); free(let_names); goto let_oom; }
            let_heap = 1;
        }
        for (uint32_t i = 0; i < n; i++) {
            const Sexpr *b = binds->list.items[i];
            if (b->kind != SEXPR_LIST || b->list.count != 2) goto let_bad;
            let_names[i] = b->list.items[0];
            if (let_names[i]->kind != SEXPR_SYMBOL) goto let_bad;
            let_vals[i] = _translate_tagged(fe, b->list.items[1]);
        }
        /* Reserve after phase 1 (nested translations may have grown the stack),
         * then push all bindings now that every value is evaluated. */
        if (!_subst_reserve(fe, n)) goto let_oom;
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
            e->expanding        = 0;   /* subst_stack is now heap (realloc'd, not
                                        * zero-init) — must clear explicitly or a
                                        * garbage `expanding` byte hides this binding
                                        * from _subst_lookup ("unknown variable"). */
        }
        /* Values are copied into the subst stack; release the temp buffers
         * BEFORE the (possibly very deep) phase-2 recursion. */
        if (let_heap) { free(let_vals); free(let_names); }

        /* Phase 2: translate body with all bindings now in scope. */
        {
            TaggedExpr res = _translate_tagged(fe, body);
            fe->subst_depth = saved;
            return res;
        }
    let_bad:
        if (let_heap) { free(let_vals); free(let_names); }
        fprintf(fe->err, "error: malformed let binding\n");
        fe->subst_depth = saved;
        return TAGGED_NULL;
    let_oom:
        fprintf(fe->err, "error: out of memory in let (%u bindings)\n", n);
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

        /* A nested array, or a slice of one: constant indices only. (select A
         * k) is a slice view; (select slice j) is A's flat element k ++ j. */
        if (arr->slice_of || arr->sort.inner_addr) {
            uint64_t lo, hi;
            uint32_t w = arr->slice_of ? arr->sort.addr_width
                       : (uint32_t)(arr->sort.addr_width - arr->sort.inner_addr);
            if (!_const_index(fe, s->list.items[2], w, &lo, &hi)) {
                SMT2_TAINT(fe, "symbolic index into a nested array");
                return TAGGED_NULL;
            }
            if (arr->slice_of) {
                uint64_t klo, khi;
                _key_concat(arr->slice_lo, arr->slice_hi, lo, hi, arr->sort.addr_width,
                            &klo, &khi);
                dvs_expr_t e = _sparse_elem(fe, arr->slice_of, klo, khi, /*create=*/1);
                if (e == EXPR_NULL) { SMT2_TAINT(fe, "sparse-array element could not be materialized"); return TAGGED_NULL; }
                return (TaggedExpr){ { e, arr->sort.data_width }, 1, NULL };
            }
            Smt2ArrayValue *sl = (Smt2ArrayValue *)_cmd_alloc(fe, sizeof(Smt2ArrayValue));
            if (!sl) { SMT2_TAINT(fe, "nested-array slice allocation failed"); return TAGGED_NULL; }
            memset(sl, 0, sizeof(*sl));
            sl->sort.addr_width = arr->sort.inner_addr;
            sl->sort.data_width = arr->sort.data_width;
            sl->store_idx_varid = UINT32_MAX;
            sl->store_val       = EXPR_NULL;
            sl->slice_of = arr;
            sl->slice_lo = lo;
            sl->slice_hi = hi;
            return (TaggedExpr){ { EXPR_NULL, 0 }, 0, sl };
        }

        /* Word-level abstract path (DV_ARRAY): const or symbolic index alike
         * become a fresh read var; read-over-write / congruence deferred. */
        if (arr->is_abstract) {
            idx_te = _flatten_to_var(fe, idx_te);
            if (idx_te.te.ref == EXPR_NULL) { SMT2_TAINT(fe, "abstract-array select index not flattenable to a var"); return TAGGED_NULL; }
            return _abs_select(fe, arr, idx_te.te.ref);
        }

        /* Constant index path (rewrite R2 included): check if the sexpr is
         * a bitvec literal after substitution resolution. */
        uint64_t k = 0, k_hi = 0;
        if (_const_index(fe, s->list.items[2], arr->sort.addr_width, &k, &k_hi)) {
            /* The array element is a VARIABLE (dvs_builder_expr_var), so tag it
             * leaf_kind == 1 (var), NOT 2 (const). Tagging it const made the
             * const-fold sites (e.g. zero_extend, boolean connectives) read
             * the var's dvs_expr_t as an ExprConst and fold it to a garbage
             * literal (0) -> wrong `unsat` for e.g.
             * `(= ((_ zero_extend N) (select a i)) k)`. */
            if (arr->is_sparse) {
                dvs_expr_t e = _sparse_elem(fe, arr, k, k_hi, /*create=*/1);
                if (e == EXPR_NULL) { SMT2_TAINT(fe, "sparse-array element could not be materialized"); return TAGGED_NULL; }
                return (TaggedExpr){ { e, arr->sort.data_width }, 1, NULL };
            }
            if (k_hi == 0 && k < arr->n_elems)
                return (TaggedExpr){ { arr->elems[k], arr->sort.data_width }, 1, NULL };
        }

        /* Symbolic index. A sparse (large-address) array cannot resolve one
         * without enumerating 2^M entries: promote it to the word-level engine,
         * or, where that is not possible, an honest unknown rather than a wrong
         * answer. Dense arrays lower to an ITE tree over their elements. */
        if (arr->is_sparse && _promote_sparse(fe, arr) == 0) {
            idx_te = _flatten_to_var(fe, idx_te);
            if (idx_te.te.ref == EXPR_NULL) { SMT2_TAINT(fe, "abstract-array select index not flattenable to a var"); return TAGGED_NULL; }
            return _abs_select(fe, arr, idx_te.te.ref);
        }
        if (arr->is_sparse) {
            fprintf(fe->err, "error: symbolic index into a large (sparse) array "
                             "is unsupported -> result will be unknown\n");
            SMT2_TAINT(fe, "symbolic index into a large (sparse) array");
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
        if (arr->slice_of || arr->sort.inner_addr) {
            SMT2_TAINT(fe, "store on a nested array");
            return TAGGED_NULL;
        }
        if (arr->is_sparse) _promote_sparse(fe, arr);

        /* Word-level abstract path (DV_ARRAY): build a STORE DAG node instead of
         * an elementwise ITE array; select resolves it via read-over-write. This
         * also handles large (sparse) address spaces the dense path bails on. */
        if (arr->is_abstract) {
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
            idx_te = _flatten_to_var(fe, idx_te);
            val_te = _flatten_to_var(fe, val_te);
            Smt2ArrayValue *node = _anode_store(fe, arr, idx_te.te.ref,
                                                val_te.te.ref);
            if (!node) { SMT2_TAINT(fe, "abstract-array store node allocation failed"); return TAGGED_NULL; }
            return (TaggedExpr){ { EXPR_NULL, 0 }, 0, node };
        }

        /* store on a sparse (large-address) array needs a lazy read-over-write
         * copy; not yet implemented, so report unknown rather than guess. */
        if (arr->is_sparse) {
            fprintf(fe->err, "error: store on a large (sparse) array is "
                             "unsupported -> result will be unknown\n");
            SMT2_TAINT(fe, "store on a large (sparse) array");
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
            if (idx_s->kind == SEXPR_BITVEC && idx_s->bv.width <= 64) {  /* wide: general path */
                k = idx_s->bv.value; is_const = 1;
            } else if (idx_s->kind == SEXPR_LIST && idx_s->list.count == 3 &&
                       sexpr_is_symbol(idx_s->list.items[0], "_")) {
                uint64_t bv_val;
                if (_parse_bv_sym(idx_s->list.items[1], &bv_val, NULL)) {
                    k = bv_val; is_const = 1;
                }
            }
            if (is_const) {
                memcpy(new_arr->elems, arr->elems, arr->n_elems * sizeof(dvs_expr_t));
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
            ExprVar *ev = (ExprVar *)dvs_builder_ref_ptr(fe->builder, idx_te.te.ref);
            if (ev && ev->kind == EXPR_VAR) {
                new_arr->store_idx_varid = ev->var_id;
                new_arr->store_val       = val_te.te.ref;
            }
        }
        for (uint32_t j = 0; j < arr->n_elems; j++) {
            dvs_expr_t j_const = _bv_const(fe, (int64_t)j, arr->sort.addr_width);
            dvs_expr_t cond = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ,
                                               idx_te.te.ref, j_const);
            new_arr->elems[j] = dvs_builder_expr_ite(fe->builder, cond,
                                                  val_te.te.ref, arr->elems[j]);
        }
        return (TaggedExpr){ { EXPR_NULL, 0 }, 0, new_arr };
    }

    /* ---- Binary BV arithmetic (result has same width as operands) ---- */
    /* These ops are :left-assoc in SMT-LIB, and Verilator emits them variadically
     * (e.g. (bvor a b c)). Left-fold over all operands so both the binary and
     * n-ary forms translate: (op a b c) == (op (op a b) c). */
    /* bvand / bvor with literal operands: drop identities, short-circuit on the
     * absorbing value, then fold the rest as BINOP_CASE does. */
    {
        int is_band = (oplen == 5 && memcmp(op, "bvand", 5) == 0);
        int is_bor  = (oplen == 4 && memcmp(op, "bvor", 4) == 0);
        if ((is_band || is_bor) && s->list.count >= 3) {
            TaggedExpr acc = TAGGED_NULL;
            uint16_t w = 0;
            for (uint32_t i = 1; i < s->list.count; i++) {
                TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
                if (b.te.ref == EXPR_NULL || b.array != NULL) return TAGGED_NULL;
                if (!w) w = b.te.width;
                uint64_t v, ones = w >= 64 ? ~0ull : ((1ull << w) - 1);
                if (b.te.width == w && _lit_value(fe, b, &v)) {
                    if (v == (is_band ? 0 : ones))
                        return (TaggedExpr){ { _bv_const(fe, (int64_t)v, w), w }, 2, NULL };
                    if (v == (is_band ? ones : 0)) continue;
                }
                b = _flatten_to_var(fe, b);
                if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
                if (acc.te.ref == EXPR_NULL) { acc = b; continue; }
                acc = _flatten_to_var(fe, acc);
                if (acc.te.ref == EXPR_NULL) return TAGGED_NULL;
                acc.te.ref = dvs_builder_expr_binary(fe->builder,
                                                     is_band ? DVS_BIN_BAND : DVS_BIN_BOR,
                                                     acc.te.ref, b.te.ref);
                acc.leaf_kind = 0;
            }
            if (acc.te.ref == EXPR_NULL) {     /* every operand was the identity */
                uint64_t id = is_band ? (w >= 64 ? ~0ull : ((1ull << w) - 1)) : 0;
                return (TaggedExpr){ { _bv_const(fe, (int64_t)id, w), w }, 2, NULL };
            }
            _flag_wide_arith(fe, w);
            return (TaggedExpr){ { acc.te.ref, w }, acc.leaf_kind, NULL };
        }
    }

#define BINOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count < 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        for (uint32_t _i = 2; _i < s->list.count; _i++) { \
            TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[_i])); \
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
            /* Re-flatten the accumulator each round. Without this, from the 2nd \
             * operand on the left side is a raw composed node, and the CDCL \
             * compile only links var-var / const-var operands -- so an n-ary op \
             * with arity >= 3 was left UNCOMPILED, the model then violated it, \
             * and validation downgraded a perfectly good `sat` to `unknown`. \
             * That silently disabled `x inside {a,b,c}` (a 3-way bvor) on CDCL. \
             * Same lesson as B4: never reuse a raw composed sub-expr. */ \
            a = _flatten_to_var(fe, a); \
            if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
            a.te.ref = dvs_builder_expr_binary(fe->builder, binop, a.te.ref, b.te.ref); \
            a.leaf_kind = 0;   /* composed: must be re-flattened next round */ \
        } \
        _flag_wide_arith(fe, a.te.width); \
        return (TaggedExpr){ { a.te.ref, a.te.width }, 0, NULL }; \
    }

#define CMPOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) { \
        if (s->list.count != 3) return TAGGED_NULL; \
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1])); \
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL; \
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2])); \
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL; \
        if (_same_var(fe, a.te.ref, b.te.ref)) \
            return _bool_lit(fe, binop == DVS_BIN_LTE || binop == DVS_BIN_GTE); \
        dvs_expr_t r = dvs_builder_expr_binary(fe->builder, binop, a.te.ref, b.te.ref); \
        return (TaggedExpr){ { r, 1 }, 0, NULL }; \
    }

    /* Unsigned division and remainder. SMT-LIB defines a zero divisor:
     * (bvudiv a 0) = all ones, (bvurem a 0) = a. The engines' own DIV/MOD
     * leave the result unconstrained at a zero divisor (SystemVerilog's x),
     * so unless the divisor is a nonzero constant, guard it here; without the
     * guard a model could pick any value and `unsat` problems came back `sat`
     * (B52). */
    {
        int is_udiv = (oplen == 6 && memcmp(op, "bvudiv", 6) == 0);
        int is_urem = (oplen == 6 && memcmp(op, "bvurem", 6) == 0);
        if (is_udiv || is_urem) {
            if (s->list.count != 3) return TAGGED_NULL;
            TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
            if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
            TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            uint16_t w = a.te.width ? a.te.width : b.te.width;
            _flag_wide_arith(fe, w);
            dvs_expr_t q = _udivrem_expr(fe, a.te.ref, b.te.ref, w, is_urem);
            return (TaggedExpr){ { q, w }, 0, NULL };
        }
    }

    BINOP_CASE("bvadd", DVS_BIN_ADD)
    BINOP_CASE("bvsub", DVS_BIN_SUB)
    BINOP_CASE("bvmul", DVS_BIN_MUL)
    BINOP_CASE("bvand", DVS_BIN_BAND)
    BINOP_CASE("bvor", DVS_BIN_BOR)
    BINOP_CASE("bvxor", DVS_BIN_BXOR)
    BINOP_CASE("bvshl", DVS_BIN_LSHIFT)
    BINOP_CASE("bvlshr", DVS_BIN_RSHIFT)

    CMPOP_CASE("bvult", DVS_BIN_LT)
    CMPOP_CASE("bvule", DVS_BIN_LTE)
    CMPOP_CASE("bvugt", DVS_BIN_GT)
    CMPOP_CASE("bvuge", DVS_BIN_GTE)

    /* Signed comparisons — see _translate_signed_cmp (MSB-flip lowering that
     * materialises the variable side's xor and folds a constant operand). */
#define SCMPOP_CASE(name, binop) \
    if (oplen == sizeof(name)-1 && memcmp(op, name, oplen) == 0) \
        return _translate_signed_cmp(fe, s, binop);

    SCMPOP_CASE("bvslt", DVS_BIN_LT)
    SCMPOP_CASE("bvsle", DVS_BIN_LTE)
    SCMPOP_CASE("bvsgt", DVS_BIN_GT)
    SCMPOP_CASE("bvsge", DVS_BIN_GTE)

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
            { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "sdiv-srem"; }
            if (s->list.count != 3) return TAGGED_NULL;
            TaggedExpr sa = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
            if (sa.te.ref == EXPR_NULL) return TAGGED_NULL;
            TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
            if (tb.te.ref == EXPR_NULL) return TAGGED_NULL;
            uint16_t w = sa.te.width ? sa.te.width : tb.te.width;
            if (w == 0) return TAGGED_NULL;
            dvs_expr_t r = _signed_divrem_expr(fe, sa.te.ref, tb.te.ref,
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
            { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "smod"; }
            if (s->list.count != 3) return TAGGED_NULL;
            TaggedExpr sa = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
            if (sa.te.ref == EXPR_NULL) return TAGGED_NULL;
            TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
            if (tb.te.ref == EXPR_NULL) return TAGGED_NULL;
            uint16_t w = sa.te.width ? sa.te.width : tb.te.width;
            if (w == 0) return TAGGED_NULL;
            dvs_expr_t T_ = tb.te.ref;
            dvs_expr_t r_expr = _signed_divrem_expr(fe, sa.te.ref, T_, w, /*is_rem=*/1);
            /* Materialise the remainder into its own var: bvsmod references it
             * three times (sign bit, +t, ==0), and re-embedding the raw ITE tree
             * at each use makes the bitblaster produce inconsistent values for
             * the shared sub-DAG. One aux var = one bit-blasted value, shared. */
            TaggedExpr rt = _flatten_to_var(fe, (TaggedExpr){ { r_expr, w }, 0, NULL });
            if (rt.te.ref == EXPR_NULL) return TAGGED_NULL;
            dvs_expr_t r = rt.te.ref;
            dvs_expr_t msb_r = dvs_builder_expr_extract(fe->builder, r, w - 1, w - 1);
            dvs_expr_t msb_t = dvs_builder_expr_extract(fe->builder, T_, w - 1, w - 1);
            dvs_expr_t same_sign = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, msb_r, msb_t);
            dvs_expr_t r_plus_t = dvs_builder_expr_binary(fe->builder, DVS_BIN_ADD, r, T_);
            dvs_expr_t adjusted = dvs_builder_expr_ite(fe->builder, same_sign, r, r_plus_t);
            dvs_expr_t zero = _bv_const(fe, 0, w);
            dvs_expr_t r_is_zero = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, r, zero);
            dvs_expr_t result = dvs_builder_expr_ite(fe->builder, r_is_zero, r, adjusted);
            return (TaggedExpr){ { result, w }, 0, NULL };
        }
    }

    /* = (equality) -- handles both BV and array operands */
    if (oplen == 1 && op[0] == '=') {
        if (s->list.count != 3) return TAGGED_NULL;
        for (int i = 1; i <= 2; i++) {
            const Sexpr *other = s->list.items[3 - i];
            if (!_is_app(s->list.items[i], "bvsmod") || (other->kind == SEXPR_LIST && !_is_app(other, "_")))
                continue;
            uint64_t zv;
            if (_lit_value(fe, _translate_tagged(fe, other), &zv) && zv == 0)
                return _smod_is_zero(fe, s->list.items[i]);
        }

        TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
        TaggedExpr b = _translate_tagged(fe, s->list.items[2]);

        if (a.array != NULL && b.array != NULL) {
            if (a.array->slice_of || a.array->sort.inner_addr ||
                b.array->slice_of || b.array->sort.inner_addr) {
                SMT2_TAINT(fe, "equality of nested arrays");
                return TAGGED_NULL;
            }
            /* Word-level abstract path: reify onto a boolean var; consistency +
             * extensionality axioms are emitted at solve time. */
            if ((a.array->is_abstract || b.array->is_abstract || a.array->is_sparse)
                && _promote_pair(fe, a.array, b.array)) {
                dvs_expr_t p = _abs_array_eq(fe, a.array, b.array);
                if (p == EXPR_NULL) { SMT2_TAINT(fe, "abstract-array equality could not be reified"); return TAGGED_NULL; }
                return (TaggedExpr){ { p, 1 }, 1, NULL };
            }
            /* Array equality: AND of element-wise equalities */
            if (a.array->n_elems != b.array->n_elems) {
                fprintf(fe->err, "error: array equality on arrays with different sizes\n");
                return TAGGED_NULL;
            }
            dvs_expr_t result = EXPR_NULL;
            for (uint32_t i = 0; i < a.array->n_elems; i++) {
                dvs_expr_t eq_i = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ,
                                                   a.array->elems[i],
                                                   b.array->elems[i]);
                result = (result == EXPR_NULL) ? eq_i
                       : dvs_builder_expr_binary(fe->builder, DVS_BIN_AND, result, eq_i);
            }
            return (TaggedExpr){ { result, 1 }, 0, NULL };
        }
        if (a.array != NULL || b.array != NULL) {
            fprintf(fe->err, "error: type mismatch in equality (array vs scalar)\n");
            return TAGGED_NULL;
        }

        /* BV/Bool equality */
        uint64_t av, bv;
        if (_lit_value(fe, a, &av) && _lit_value(fe, b, &bv) && a.te.width == b.te.width)
            return _bool_lit(fe, av == bv);
        a = _flatten_to_var(fe, a);
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        b = _flatten_to_var(fe, b);
        if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
        dvs_expr_t r = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, a.te.ref, b.te.ref);
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
        dvs_expr_t result = EXPR_NULL;
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                dvs_expr_t neq = dvs_builder_expr_binary(fe->builder, DVS_BIN_NEQ,
                                                  args[i].te.ref, args[j].te.ref);
                if (result == EXPR_NULL) result = neq;
                else result = dvs_builder_expr_binary(fe->builder, DVS_BIN_AND, result, neq);
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
        dvs_expr_t r = dvs_builder_expr_unary(fe->builder, DVS_UN_INVERT, a.te.ref);
        _flag_wide_arith(fe, a.te.width);
        return (TaggedExpr){ { r, a.te.width }, 0, NULL };
    }
    if (oplen == 5 && memcmp(op, "bvneg", 5) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        /* bvneg x is (bvsub 0 x), modulo 2^w. It must NOT lower to DVS_UN_NEG:
         * the CDCL engine (and the model validator) treat DVS_UN_NEG as integer
         * negation with no wrap, so for an unsigned x in [0, 2^w) the result
         * lands in (-2^w, 0] and `(= (bvneg x) K)` was a spurious `unsat` for
         * every constant K. Built exactly as BINOP_CASE builds bvsub. */
        TaggedExpr z = _flatten_to_var(fe, (TaggedExpr){
            { _bv_const(fe, 0, a.te.width), a.te.width }, 2, NULL });
        if (z.te.ref == EXPR_NULL) return TAGGED_NULL;
        dvs_expr_t r = dvs_builder_expr_binary(fe->builder, DVS_BIN_SUB, z.te.ref, a.te.ref);
        _flag_wide_arith(fe, a.te.width);
        return (TaggedExpr){ { r, a.te.width }, 0, NULL };
    }

    /* ---- Boolean connectives (with compile-time const folding) ---- */
    if (oplen == 3 && memcmp(op, "not", 3) == 0) {
        if (s->list.count != 2) return TAGGED_NULL;
        TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        if (a.leaf_kind == 2) {
            ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, a.te.ref);
            int64_t neg = (ec->value != 0) ? 0 : 1;
            dvs_expr_t cr = _bv_const(fe, neg, 1);
            return (TaggedExpr){ { cr, 1 }, 2, NULL };
        }
        dvs_expr_t r = dvs_builder_expr_unary(fe->builder, DVS_UN_NOT, a.te.ref);
        return (TaggedExpr){ { r, 1 }, 0, NULL };
    }

    if (oplen == 3 && memcmp(op, "and", 3) == 0) {
        /* SMT-LIB permits arity >= 1 here; `(and x)` / `(or x)` / `(xor x)` are
         * the identity and z3 accepts them (nullary `(and)` it rejects, so we do
         * too). Rejecting the unary form used to taint the assert -> `unknown`,
         * which silently killed any single-variable model-enumeration loop --
         * a blocking clause over one variable is exactly `(not (and (= x V)))`.
         * See docs/solver_bug_backlog.md B16. */
        if (s->list.count < 2) return TAGGED_NULL;
        /* Collect non-trivial operands; short-circuit on any literal false. */
        TaggedExpr acc = { { EXPR_NULL, 1 }, 0, NULL };
        for (uint32_t i = 1; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            if (b.leaf_kind == 2) {
                ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, b.te.ref);
                if (ec->value == 0) {
                    /* (and ... false ...) -> false */
                    dvs_expr_t cr = _bv_const(fe, 0, 1);
                    return (TaggedExpr){ { cr, 1 }, 2, NULL };
                }
                /* (and ... true ...) -> drop this operand */
                continue;
            }
            if (acc.te.ref == EXPR_NULL) { acc = b; acc.te.width = 1; }
            else {
                acc.te.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_AND,
                                                 acc.te.ref, b.te.ref);
                acc.leaf_kind = 0;
            }
        }
        if (acc.te.ref == EXPR_NULL) {
            /* All operands were literal true */
            dvs_expr_t cr = _bv_const(fe, 1, 1);
            return (TaggedExpr){ { cr, 1 }, 2, NULL };
        }
        acc.te.width = 1;
        return acc;
    }

    if (oplen == 2 && memcmp(op, "or", 2) == 0) {
        if (s->list.count < 2) return TAGGED_NULL;   /* arity >= 1; see `and` */
        /* Collect non-trivial operands; short-circuit on any literal true. */
        TaggedExpr acc = { { EXPR_NULL, 1 }, 0, NULL };
        for (uint32_t i = 1; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            if (b.leaf_kind == 2) {
                ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, b.te.ref);
                if (ec->value != 0) {
                    /* (or ... true ...) -> true */
                    dvs_expr_t cr = _bv_const(fe, 1, 1);
                    return (TaggedExpr){ { cr, 1 }, 2, NULL };
                }
                /* (or ... false ...) -> drop this operand */
                continue;
            }
            if (acc.te.ref == EXPR_NULL) { acc = b; acc.te.width = 1; }
            else {
                acc.te.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_OR,
                                                 acc.te.ref, b.te.ref);
                acc.leaf_kind = 0;
            }
        }
        if (acc.te.ref == EXPR_NULL) {
            /* All operands were literal false */
            dvs_expr_t cr = _bv_const(fe, 0, 1);
            return (TaggedExpr){ { cr, 1 }, 2, NULL };
        }
        acc.te.width = 1;
        return acc;
    }

    /* xor */
    if (oplen == 3 && memcmp(op, "xor", 3) == 0) {
        if (s->list.count < 2) return TAGGED_NULL;   /* arity >= 1; see `and` */
        TaggedExpr acc = _translate_tagged(fe, s->list.items[1]);
        if (acc.te.ref == EXPR_NULL) return TAGGED_NULL;
        for (uint32_t i = 2; i < s->list.count; i++) {
            TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
            if (b.te.ref == EXPR_NULL) return TAGGED_NULL;
            acc.te.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_BXOR,
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
            uint64_t av, lv;
            if (_lit_value(fe, a, &av) || (_lit_value(fe, last, &lv) && lv)) {
                if (_lit_value(fe, a, &av) && !av) last = _bool_lit(fe, 1);  /* false => _ */
                continue;                       /* true => b is b; a => true is true */
            }
            dvs_expr_t not_a = dvs_builder_expr_unary(fe->builder, DVS_UN_NOT, a.te.ref);
            last.te.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_OR, not_a, last.te.ref);
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
        uint64_t cv;
        if (_lit_value(fe, c, &cv)) return cv ? t : e;

        if (t.array != NULL && e.array != NULL) {
            if (t.array->slice_of || t.array->sort.inner_addr ||
                e.array->slice_of || e.array->sort.inner_addr) {
                SMT2_TAINT(fe, "ite over nested arrays");
                return TAGGED_NULL;
            }
            /* Word-level abstract path: an ITE DAG node; select resolves it to
             * ite(cond, read(then,i), read(else,i)). */
            if ((t.array->is_abstract || e.array->is_abstract || t.array->is_sparse)
                && _promote_pair(fe, t.array, e.array)) {
                Smt2ArrayValue *node = _anode_ite(fe, c.te.ref, t.array, e.array);
                if (!node) { SMT2_TAINT(fe, "abstract-array ite node allocation failed"); return TAGGED_NULL; }
                return (TaggedExpr){ { EXPR_NULL, 0 }, 0, node };
            }
            if (t.array->n_elems != e.array->n_elems) {
                fprintf(fe->err, "error: ite branches are arrays of different sizes\n");
                return TAGGED_NULL;
            }
            Smt2ArrayValue *arr = _make_array_value(fe, t.array->sort);
            if (!arr) return TAGGED_NULL;
            for (uint32_t i = 0; i < t.array->n_elems; i++) {
                arr->elems[i] = dvs_builder_expr_ite(fe->builder, c.te.ref,
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
        dvs_expr_t r = dvs_builder_expr_ite(fe->builder, c.te.ref, t.te.ref, e.te.ref);
        _flag_wide_arith(fe, t.te.width);
        return (TaggedExpr){ { r, t.te.width }, 0, NULL };
    }

    /* concat. SMT-LIB's is binary; Verilator's solver interface (and z3)
     * also write it n-ary, (concat a b c) == (concat (concat a b) c). Refusing
     * that left the whole assertion uncompiled, and the check-sat `unknown`. */
    if (oplen == 6 && memcmp(op, "concat", 6) == 0) {
        if (s->list.count < 3) return TAGGED_NULL;
        TaggedExpr acc = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (acc.te.ref == EXPR_NULL) return TAGGED_NULL;
        for (uint32_t i = 2; i < s->list.count; i++) {
            TaggedExpr lo = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[i]));
            if (lo.te.ref == EXPR_NULL) return TAGGED_NULL;
            dvs_expr_t r = dvs_builder_expr_concat(fe->builder, acc.te.ref, lo.te.ref,
                                            (uint8_t)lo.te.width);
            acc = (TaggedExpr){ { r, (uint16_t)(acc.te.width + lo.te.width) }, 0, NULL };
            if (i + 1 < s->list.count) {
                acc = _flatten_to_var(fe, acc);
                if (acc.te.ref == EXPR_NULL) return TAGGED_NULL;
            }
        }
        return acc;
    }

    /* ---- bvashr: arithmetic shift right ----
     * SMT-LIB values are unsigned bit patterns, while DVS_BIN_ASHR is arithmetic
     * only for a SIGNED left operand (SystemVerilog `>>>`). So read the
     * pattern as signed at its own width (an explicit cast -- the problem is
     * flagged explicit and never SV-elaborated), shift, and read the result
     * back as an unsigned pattern. The amount stays an unsigned w-bit value;
     * any amount >= w leaves only sign bits, exactly as SMT-LIB defines. */
    if (oplen == 6 && memcmp(op, "bvashr", 6) == 0) {
        if (s->list.count != 3) return TAGGED_NULL;
        TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (a.te.ref == EXPR_NULL) return TAGGED_NULL;
        TaggedExpr k = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
        if (k.te.ref == EXPR_NULL) return TAGGED_NULL;
        uint16_t w = a.te.width;
        if (w == 0 || w > 255 || k.te.width != w) {
            fprintf(fe->err, "error: bvashr operands must share a width\n");
            return TAGGED_NULL;
        }
        dvs_expr_t as = dvs_builder_expr_sv_cast(fe->builder, a.te.ref,
                                          (uint8_t)w, (uint8_t)w, 0, 1);
        dvs_expr_t sh = dvs_builder_expr_binary(fe->builder, DVS_BIN_ASHR, as, k.te.ref);
        dvs_expr_t r  = dvs_builder_expr_sv_cast(fe->builder, sh,
                                          (uint8_t)w, (uint8_t)w, 1, 0);
        _flag_wide_arith(fe, w);
        return (TaggedExpr){ { r, w }, 0, NULL };
    }

    /* ---- Signed arithmetic ops: still deferred (comparisons handled above) ---- */
    if ((oplen == 6 && memcmp(op, "bvsdiv", 6) == 0) ||
        (oplen == 6 && memcmp(op, "bvsrem", 6) == 0) ||
        (oplen == 6 && memcmp(op, "bvsmod", 6) == 0)) {
        fprintf(fe->err, "error: signed operation '%.*s' not yet supported\n",
                (int)oplen, op);
        return TAGGED_NULL;
    }

    fprintf(fe->err, "error: unsupported operation '%.*s'\n",
            (int)oplen, op);
    return TAGGED_NULL;
}

/* Entry points */
static TaggedExpr _translate_tagged_impl(Smt2Frontend *fe, const Sexpr *s) {
    switch (s->kind) {
    case SEXPR_SYMBOL:
        return _translate_symbol_tagged(fe, s);
    case SEXPR_NUMERAL: {
        dvs_expr_t r = dvs_builder_expr_const(fe->builder, (int64_t)s->numval, 0);
        return (TaggedExpr){ { r, 64 }, 2, NULL };
    }
    case SEXPR_BITVEC: {
        /* A `#x…`/`#b…` literal wider than 64 bits carries its full value in
         * limbs (Sexpr.bv.value is only the low 64 bits). */
        if (s->bv.width > 64)
            return _wide_const(fe, s->bv.limbs, s->bv.width);
        dvs_expr_t r = _bv_const(fe, (int64_t)s->bv.value, (uint16_t)s->bv.width);
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
        if (src->bv.limbs) {
            /* A wide literal's limbs live in the source arena: copy them too. */
            uint32_t nl = (src->bv.width + 63u) / 64u;
            uint64_t *l = (uint64_t *)sexpr_arena_alloc(
                arena, nl * sizeof(uint64_t), _Alignof(uint64_t));
            if (!l) return NULL;
            memcpy(l, src->bv.limbs, nl * sizeof(uint64_t));
            dst->bv.limbs = l;
        }
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
    if (sexpr_is_keyword(key, ":seed") || sexpr_is_keyword(key, ":random-seed")) {
        if (val->kind == SEXPR_NUMERAL) {
            fe->seed = val->numval;
            fe->seed_given = 1;
            fe->given_seed = val->numval;
        }
        return 0;
    }
    if (sexpr_is_keyword(key, ":produce-unsat-cores")) {
        fe->produce_unsat_cores = sexpr_is_symbol(val, "true");
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

static uint64_t _fp_mix(uint64_t h, const uint8_t *d, size_t n);

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
    fe->shape_fp = _fp_mix(fe->shape_fp ^ 0x51u, (const uint8_t *)name_s->sym.str,
                           name_s->sym.len);

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
    int is_arr = _parse_array_sort_x(fe, sort_s, &array_sort, /*nested_ok=*/1);
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
        fprintf(fe->err, "error: unsupported sort (only BitVec 1-%d, Bool, or "
                         "Array) -> result will be unknown\n", SMT2_MAX_BV_BITS);
        SMT2_TAINT(fe, "unsupported sort (BitVec wider than SMT2_MAX_BV_BITS, or not BitVec/Bool/Array)");
        return 0;
    }

    uint32_t var_id = _next_var_id(fe);
    int64_t max_val = _bv_unsigned_hi(width);

    dvs_builder_add_var(fe->builder, var_id, width, 0, 0, max_val);
    if (_add_var(fe, name_s->sym.str, name_s->sym.len, var_id, width) < 0) {
        fprintf(fe->err, "error: out of memory adding variable\n");
        return -1;
    }
    _builder_touched(fe);
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

    /* Grow the heap-backed funs table (geometric, clamped to the hard cap). */
    if (fe->n_funs >= fe->funs_cap) {
        uint32_t newcap = fe->funs_cap ? fe->funs_cap * 2 : 16;
        if (newcap > SMT2_MAX_FUNS) newcap = SMT2_MAX_FUNS;
        Smt2FunDef *tmp = (Smt2FunDef *)realloc(
            fe->funs, (size_t)newcap * sizeof(Smt2FunDef));
        if (!tmp) {
            fprintf(fe->err, "error: out of memory growing define-fun table\n");
            return -1;
        }
        fe->funs = tmp;
        fe->funs_cap = newcap;
    }
    Smt2FunDef *fd = &fe->funs[fe->n_funs];
    /* realloc does not zero; the old inline array was zero-initialized by the
     * blanket memset, so clear this slot to preserve identical semantics. */
    memset(fd, 0, sizeof(*fd));
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

/* Record that the builder was mutated (a var or constraint was added).
 *
 * Two consumers, one for each engine:
 *   - CDCL   : has_aux drives _flush_aux, which folds the addition into the
 *              already-compiled live ctx.
 *   - bitblast: it has no such fold, so mark fe->problem stale and let
 *              _ensure_problem re-finalize before the next solve. Without this
 *              an (assert) after a (check-sat) was silently dropped (B14).
 * Only meaningful once the problem/ctx exists; before that the builder IS the
 * pending state and the first finalize picks everything up. */
static void _builder_touched(Smt2Frontend *fe) {
    if (fe->compiled) fe->has_aux = 1;
    if (fe->problem)  fe->problem_dirty = 1;
}

/* Is `s` the 1-bit literal #b1? Translating it is the cheapest reliable test:
 * it folds (_ bv1 1) and #b1 alike, and reports the width we need to check. */
static int _is_bit1_literal(Smt2Frontend *fe, const Sexpr *s) {
    /* Only an atom or `(_ bvN 1)` can be the literal. Translating any other
     * term just to ask has side effects: a `(bvsmod ..)` operand flagged the
     * whole problem for bitblast, and flattened operands leave their aux
     * definitions in the builder. */
    if (s->kind == SEXPR_LIST && !(s->list.count > 0 && sexpr_is_symbol(s->list.items[0], "_")))
        return 0;
    TaggedExpr t = _translate_tagged(fe, s);
    if (t.te.ref == EXPR_NULL || t.leaf_kind != 2 || t.te.width != 1) return 0;
    ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, t.te.ref);
    return ec && ec->value == 1;
}

/**
 * Translate `(= #b1 (bvor P1 ... Pn))` as a LOGICAL disjunction.
 *
 * Verilator emits `x inside {a, b, c}` as
 *   (= #b1 (bvand #b1 (bvor (__Vbv (= x a)) (__Vbv (= x b)) ...)))
 * where __Vbv lifts a Bool to (_ BitVec 1). `bvor` maps to DVS_BIN_BOR (bitwise),
 * whose operands then get flattened to aux vars -- so the disjunctive
 * structure was gone by the time the CDCL compiler ran. It saw a chain of ITE
 * guard propagators carrying no bound information, and `t_constraint_dist`
 * left a 32-bit variable at its full domain for the search to enumerate.
 *
 * Emitting DVS_BIN_OR instead keeps the tree intact, so dvs_compile.c's OR-tree
 * flattener can reach it, classify the reified `ite(P,1,0)` leaves, and build
 * a DisjClause propagator (which then hulls the domain).
 *
 * Sound because every operand is checked to be width 1: for 1-bit values
 * bitwise-or and logical-or coincide, and `bvand #b1` is the identity.
 * Returns EXPR_NULL when the pattern does not apply, so anything else keeps
 * its existing translation.
 */
/* DISJUNCTIONS ONLY, deliberately.
 *
 * An early version also rewrote `bvand` trees into DVS_BIN_AND. That regressed
 * `t_constraint_sysfunc` from sat to unknown: `(= #b1 (bvand (__Vbv A)
 * (__Vbv B)))` already compiled correctly via the existing equality path,
 * whereas the rewritten AND-of-ITE tree did not, so the constraint was left
 * uncompiled and the validation net downgraded the result. The disjunctive
 * case is the one that had no working path, so that is the only case we take
 * over. A `bvand` that is not a plain `#b1` mask makes us decline outright.
 */
static dvs_expr_t _reified_bool(Smt2Frontend *fe, const Sexpr *s, int depth) {
    if (!s || depth > 32) return EXPR_NULL;

    if (s->kind == SEXPR_LIST && s->list.count >= 3 &&
        sexpr_is_symbol(s->list.items[0], "bvor")) {
        dvs_expr_t acc = EXPR_NULL;
        for (uint32_t i = 1; i < s->list.count; i++) {
            dvs_expr_t r = _reified_bool(fe, s->list.items[i], depth + 1);
            if (r == EXPR_NULL) return EXPR_NULL;
            acc = (acc == EXPR_NULL)
                ? r : dvs_builder_expr_binary(fe->builder, DVS_BIN_OR, acc, r);
            if (acc == EXPR_NULL) return EXPR_NULL;
        }
        return acc;
    }

    /* Leaf: any 1-bit term (typically the reified `(ite P #b1 #b0)`). Anything
     * wider means this was genuine bitwise arithmetic, not a Boolean tree. */
    TaggedExpr t = _translate_tagged(fe, s);
    if (t.te.ref == EXPR_NULL || t.te.width != 1) return EXPR_NULL;
    return t.te.ref;
}

static int _is_bv_literal(const Sexpr *s, int value, uint16_t width);

/* `(__Vbool #b1)`: Verilator's always-true guard. */
static int _is_vlt_true_guard(const Sexpr *g) {
    return g && g->kind == SEXPR_LIST && g->list.count == 2 &&
           sexpr_is_symbol(g->list.items[0], "__Vbool") && _is_bv_literal(g->list.items[1], 1, 1);
}

/* Verilator's Bool round trip around a 1-bit Y, `(__Vbv (__Vbool Y))`, possibly
 * under always-true guards, `(__Vbv (=> (__Vbool #b1) .. (__Vbool Y)))`: Y, else
 * NULL. Constrained-random code emits a conjunction this way for a constraint
 * guarded by a static condition; left wrapped it was not split into separate
 * asserts, and the CDCL search took hundreds of conflicts on it as one reified
 * conjunction (~50 ms, then bitblast). */
static const Sexpr *_vlt_unwrap_bv1(Smt2Frontend *fe, const Sexpr *b) {
    if (!fe->verilator_mode || !b || b->kind != SEXPR_LIST || b->list.count != 2 ||
        !sexpr_is_symbol(b->list.items[0], "__Vbv"))
        return NULL;
    const Sexpr *x = b->list.items[1];
    while (x && x->kind == SEXPR_LIST && x->list.count == 3 &&
           sexpr_is_symbol(x->list.items[0], "=>") && _is_vlt_true_guard(x->list.items[1]))
        x = x->list.items[2];
    if (x && x->kind == SEXPR_LIST && x->list.count == 2 && sexpr_is_symbol(x->list.items[0], "__Vbool"))
        return x->list.items[1];
    return NULL;
}

/* Strip `(bvand #b1 Y)` / `(bvand Y #b1)` masks: identity on a 1-bit Y. In
 * Verilator mode also the Bool round trip of _vlt_unwrap_bv1. */
static const Sexpr *_strip_bit1_mask(Smt2Frontend *fe, const Sexpr *body) {
    for (;;) {
        const Sexpr *u = _vlt_unwrap_bv1(fe, body);
        if (u) { body = u; continue; }
        if (!body || body->kind != SEXPR_LIST || body->list.count != 3) return body;
        if (!sexpr_is_symbol(body->list.items[0], "bvand")) return body;
        if (_is_bit1_literal(fe, body->list.items[1]))
            body = body->list.items[2];
        else if (_is_bit1_literal(fe, body->list.items[2]))
            body = body->list.items[1];
        else
            return body;
    }
}

static dvs_expr_t _try_translate_reified_or(Smt2Frontend *fe, const Sexpr *s) {
    if (!s || s->kind != SEXPR_LIST || s->list.count != 3) return EXPR_NULL;
    if (!sexpr_is_symbol(s->list.items[0], "=")) return EXPR_NULL;

    const Sexpr *body;
    if (_is_bit1_literal(fe, s->list.items[1]))      body = s->list.items[2];
    else if (_is_bit1_literal(fe, s->list.items[2])) body = s->list.items[1];
    else return EXPR_NULL;

    body = _strip_bit1_mask(fe, body);

    /* Only rewrite an actual bvor tree. A bare reified leaf, or a conjunction,
     * keeps its existing translation -- both already compile, and disturbing
     * them regressed sysfunc once already. */
    if (!body || body->kind != SEXPR_LIST || body->list.count < 3)
        return EXPR_NULL;
    if (!sexpr_is_symbol(body->list.items[0], "bvor"))
        return EXPR_NULL;

    return _reified_bool(fe, body, 0);
}

/* ------------------------------------------------------------------ */
/* Set membership: a large `x inside {c0, c1, ...}` as one EXPR_IN_SET */
/*                                                                     */
/* Verilator emits `inside` as a bvor of reified equalities, or as a   */
/* Bool `or` of them, sometimes under `(=> (__Vbool #b1) ...)`. As an  */
/* OR tree a set past DisjClause's 16 disjuncts compiles to one guard  */
/* per value, and the search finds a member by trial: 22 ms a solve    */
/* for one 32-bit riscv-dv field under three such sets. As an IN_SET   */
/* it is one propagator whose gaps the compile punches out as holes,   */
/* so the first value picked is a member. Smaller sets keep the        */
/* DisjClause path, which already does the same.                       */
/*                                                                     */
/* Only a plain variable symbol against constants is taken, so a       */
/* declined match leaves nothing behind but unused builder nodes.      */
/* ------------------------------------------------------------------ */
#define SMT2_INSET_MIN 17u
#define SMT2_INSET_MAX 4096u

typedef struct {
    dvs_expr_t  x;         /* the variable, once the first leaf names it */
    uint16_t    width;
    uint32_t    n;
    dvs_expr_t  elems[SMT2_INSET_MAX];
} Smt2InSetAcc;

static int _is_bv_literal(const Sexpr *s, int value, uint16_t width) {
    return s && s->kind == SEXPR_BITVEC && s->bv.width == width && s->bv.width <= 64
        && s->bv.value == (uint64_t)value;
}

/* `(= x c)` / `(= c x)`: x a variable of at most 32 bits, c a constant. */
static int _inset_leaf(Smt2Frontend *fe, const Sexpr *s, Smt2InSetAcc *a) {
    const Sexpr *p = s->list.items[1], *q = s->list.items[2];
    if (p->kind != SEXPR_SYMBOL) { const Sexpr *t = p; p = q; q = t; }
    if (p->kind != SEXPR_SYMBOL || q->kind == SEXPR_SYMBOL) return 0;
    TaggedExpr v = _translate_tagged(fe, p);
    if (v.te.ref == EXPR_NULL || v.leaf_kind != 1 || v.array
            || v.te.width == 0 || v.te.width > 32)
        return 0;
    TaggedExpr c = _translate_tagged(fe, q);
    if (c.te.ref == EXPR_NULL || c.leaf_kind != 2 || c.te.width != v.te.width) return 0;
    if (a->x == EXPR_NULL) { a->x = v.te.ref; a->width = v.te.width; }
    else if (!_same_operand(fe, a->x, v.te.ref)) return 0;
    if (a->n >= SMT2_INSET_MAX) return 0;
    a->elems[a->n++] = c.te.ref;
    return 1;
}

/* Collect the disjuncts of a formula that holds iff x is one of them. */
static int _inset_collect(Smt2Frontend *fe, const Sexpr *s, Smt2InSetAcc *a, int depth) {
    if (!s || depth > 32) return 0;
    if (_is_bv_literal(s, 0, 1)) return 1;                 /* #b0: bvor identity */
    if (s->kind != SEXPR_LIST || s->list.count < 2) return 0;
    const Sexpr *op = s->list.items[0];
    if (sexpr_is_symbol(op, "bvor") || sexpr_is_symbol(op, "or")) {
        for (uint32_t i = 1; i < s->list.count; i++)
            if (!_inset_collect(fe, s->list.items[i], a, depth + 1)) return 0;
        return 1;
    }
    if (s->list.count == 3 && sexpr_is_symbol(op, "=")) return _inset_leaf(fe, s, a);
    if (!fe->verilator_mode) return 0;
    /* Verilator's Bool <-> (_ BitVec 1) lifts, and `(=> (__Vbool #b1) B)`. */
    if (s->list.count == 2 && (sexpr_is_symbol(op, "__Vbv") || sexpr_is_symbol(op, "__Vbool")))
        return _inset_collect(fe, s->list.items[1], a, depth + 1);
    if (s->list.count == 3 && sexpr_is_symbol(op, "=>")) {
        const Sexpr *g = s->list.items[1];
        if (!(g->kind == SEXPR_LIST && g->list.count == 2 && sexpr_is_symbol(g->list.items[0], "__Vbool")
              && _is_bv_literal(g->list.items[1], 1, 1)))
            return 0;
        return _inset_collect(fe, s->list.items[2], a, depth + 1);
    }
    return 0;
}

/* `(= #b1 BODY)` where BODY holds iff x inside {...}: the IN_SET, else EXPR_NULL. */
static dvs_expr_t _try_translate_in_set(Smt2Frontend *fe, const Sexpr *s) {
    if (!s || s->kind != SEXPR_LIST || s->list.count != 3) return EXPR_NULL;
    if (!sexpr_is_symbol(s->list.items[0], "=")) return EXPR_NULL;
    const Sexpr *body;
    if (_is_bv_literal(s->list.items[1], 1, 1))      body = s->list.items[2];
    else if (_is_bv_literal(s->list.items[2], 1, 1)) body = s->list.items[1];
    else return EXPR_NULL;
    Smt2InSetAcc *a = (Smt2InSetAcc *)malloc(sizeof *a);
    if (!a) return EXPR_NULL;
    a->x = EXPR_NULL; a->width = 0; a->n = 0;
    dvs_expr_t r = EXPR_NULL;
    if (_inset_collect(fe, _strip_bit1_mask(fe, body), a, 0) && a->n >= SMT2_INSET_MIN)
        r = dvs_builder_expr_in_set(fe->builder, a->x, a->n, a->elems);
    free(a);
    return r;
}

/* ------------------------------------------------------------------ */
/* Reified conjunction: split `(= #b1 (bvand A B ...))` into one       */
/* top-level assert per conjunct.                                      */
/*                                                                     */
/* For 1-bit A and B, `(= #b1 (bvand A B))` is exactly                 */
/* `(= #b1 A) AND (= #b1 B)`, so the split is semantics-preserving --  */
/* but it is the difference between a solvable and an unsolvable       */
/* instance. Kept whole, the conjunction reifies into a Boolean guard  */
/* and the operands' comparisons never restrict any variable's domain, */
/* so a range like `x inside [5:6]` leaves x free over all 2^32 and    */
/* the search enumerates blind. Split, each conjunct compiles to a     */
/* bounds propagator. Measured on t_constraint_dist: 10 s timeout ->   */
/* 2 ms.                                                               */
/*                                                                     */
/* This is a top-level ASSERT split, not a general bvand -> logical-AND*/
/* rewrite. Rewriting bvand as an expression regressed                 */
/* t_constraint_sysfunc from sat to unknown (see                       */
/* _try_translate_reified_or); splitting the assert leaves each        */
/* conjunct's own translation exactly as it was.                       */
/* ------------------------------------------------------------------ */
#define SMT2_MAX_CONJUNCTS 64u

/** Flatten a nested `bvand` tree into its 1-bit leaves, dropping `#b1` masks.
 *  Returns 0 if the tree is deeper/wider than we are willing to handle. */
static int _collect_bit1_conjuncts(Smt2Frontend *fe, const Sexpr *s,
                                    const Sexpr **out, uint32_t *n, int depth) {
    if (!s || depth > 32) return 0;
    s = _strip_bit1_mask(fe, s);
    if (s && s->kind == SEXPR_LIST && s->list.count >= 3 &&
        sexpr_is_symbol(s->list.items[0], "bvand")) {
        for (uint32_t i = 1; i < s->list.count; i++) {
            if (_is_bit1_literal(fe, s->list.items[i])) continue;  /* mask */
            if (!_collect_bit1_conjuncts(fe, s->list.items[i], out, n, depth + 1))
                return 0;
        }
        return 1;
    }
    if (*n >= SMT2_MAX_CONJUNCTS) return 0;
    out[(*n)++] = s;
    return 1;
}

/** Add `ref` as a user-level (constraint_id=1) top-level constraint. */
static void _add_user_constraint(Smt2Frontend *fe, dvs_expr_t ref) {
    /* Tag user-level asserts with constraint_id=1 so the model-validation
     * pass can distinguish them from internal aux constraints (which are
     * left at id=0). Internal aux constraints can be temporarily out of
     * sync with their associated aux var without affecting the user-
     * visible result; flagging them in the validator produces noise. */
    dvs_expr_t cref = dvs_builder_add_constraint(fe->builder, ref);
    if (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)dvs_builder_ref_ptr(fe->builder, cref);
        if (cs) cs->constraint_id = 1;
    }
    _builder_touched(fe);
}

/** Translate one conjunct as if Verilator had emitted `(= #b1 <leaf>)` as its
 *  own assert. Returns EXPR_NULL if the conjunct does not translate.
 *
 *  The synthetic `(= #b1 leaf)` node matters: translating `leaf` alone and
 *  wrapping the result in a hand-built DVS_BIN_EQ is NOT equivalent. The `=`
 *  translation has folding and reification paths of its own -- notably
 *  `(= #b1 (ite b #b1 #b0))` collapsing to `b`, and the B11 signed-compare
 *  flattening -- and bypassing them leaves the constraint uncompiled (measured:
 *  dist and sysfunc both went to uncompiled=2). Reusing the assert's own `=`
 *  and `#b1` nodes reproduces the emitted form exactly.
 */
static dvs_expr_t _translate_bit1_conjunct(Smt2Frontend *fe, const Sexpr *eq_sym,
                                        const Sexpr *one_lit, const Sexpr *leaf) {
    const Sexpr *body = _strip_bit1_mask(fe, leaf);
    if (!body) return EXPR_NULL;

    /* A conjunct may itself be a disjunction; route it through the same
     * normalization a whole-assert bvor gets. */
    if (body->kind == SEXPR_LIST && body->list.count >= 3 &&
        sexpr_is_symbol(body->list.items[0], "bvor")) {
        dvs_expr_t r = _reified_bool(fe, body, 0);
        if (r != EXPR_NULL) return r;
    }

    Sexpr *items[3];
    items[0] = (Sexpr *)eq_sym;
    items[1] = (Sexpr *)one_lit;
    items[2] = (Sexpr *)body;
    Sexpr eq;
    eq.kind = SEXPR_LIST;
    eq.list.items = items;
    eq.list.count = 3;

    TypedExpr t = _translate_expr(fe, &eq);
    return (t.width == 1) ? t.ref : EXPR_NULL;
}

/** If `s` is `(= #b1 (bvand ...))`, assert each conjunct separately.
 *  Returns 1 if it handled the assert, 0 if `s` is some other shape. */
static int _try_split_reified_and(Smt2Frontend *fe, const Sexpr *s) {
    if (!s || s->kind != SEXPR_LIST || s->list.count != 3) return 0;
    if (!sexpr_is_symbol(s->list.items[0], "=")) return 0;

    const Sexpr *body, *one_lit;
    if (_is_bit1_literal(fe, s->list.items[1])) {
        one_lit = s->list.items[1]; body = s->list.items[2];
    } else if (_is_bit1_literal(fe, s->list.items[2])) {
        one_lit = s->list.items[2]; body = s->list.items[1];
    } else return 0;

    body = _strip_bit1_mask(fe, body);
    if (!body || body->kind != SEXPR_LIST || body->list.count < 3) return 0;
    if (!sexpr_is_symbol(body->list.items[0], "bvand")) return 0;

    const Sexpr *conj[SMT2_MAX_CONJUNCTS];
    uint32_t n = 0;
    if (!_collect_bit1_conjuncts(fe, body, conj, &n, 0)) return 0;
    if (n < 2) return 0;

    /* All-or-nothing: translate every conjunct before adding any constraint,
     * so a leaf we cannot translate leaves the assert to the ordinary path
     * (which taints properly) rather than half-asserted. */
    dvs_expr_t refs[SMT2_MAX_CONJUNCTS];
    for (uint32_t i = 0; i < n; i++) {
        refs[i] = _translate_bit1_conjunct(fe, s->list.items[0], one_lit, conj[i]);
        if (refs[i] == EXPR_NULL) return 0;
    }
    for (uint32_t i = 0; i < n; i++) _add_user_constraint(fe, refs[i]);
    return 1;
}

/* Record the :named label of an `(! t ... :named N ...)` assertion, for
 * get-unsat-core. Best effort: a name we cannot store is simply not reported,
 * which only makes the reported core larger-than-needed, never wrong. */
static void _record_named(Smt2Frontend *fe, const Sexpr *term) {
    if (!term || term->kind != SEXPR_LIST || term->list.count < 4) return;
    if (!sexpr_is_symbol(term->list.items[0], "!")) return;
    for (uint32_t i = 2; i + 1 < term->list.count; i++) {
        const Sexpr *k = term->list.items[i], *v = term->list.items[i + 1];
        if (!sexpr_is_keyword(k, ":named") || v->kind != SEXPR_SYMBOL) continue;
        if (fe->n_named == fe->named_cap) {
            uint32_t cap = fe->named_cap ? fe->named_cap * 2 : 16;
            char **g = (char **)realloc(fe->named, cap * sizeof(char *));
            if (!g) return;
            fe->named = g;
            fe->named_cap = cap;
        }
        char *nm = (char *)malloc((size_t)v->sym.len + 1);
        if (!nm) return;
        memcpy(nm, v->sym.str, v->sym.len);
        nm[v->sym.len] = '\0';
        fe->named[fe->n_named++] = nm;
        return;
    }
}

static void _truncate_named(Smt2Frontend *fe, uint32_t n) {
    for (uint32_t i = n; i < fe->n_named; i++) free(fe->named[i]);
    if (n < fe->n_named) fe->n_named = n;
}

/* ((_ extract i i) <declared var>): one bit of a declared variable. */
static int _is_vlt_hash_bit(Smt2Frontend *fe, const Sexpr *s) {
    if (s->kind != SEXPR_LIST || s->list.count != 2) return 0;
    const Sexpr *ix = s->list.items[0], *v = s->list.items[1];
    if (ix->kind != SEXPR_LIST || ix->list.count != 4 ||
        !sexpr_is_symbol(ix->list.items[0], "_") ||
        !sexpr_is_symbol(ix->list.items[1], "extract") ||
        ix->list.items[2]->kind != SEXPR_NUMERAL ||
        ix->list.items[3]->kind != SEXPR_NUMERAL ||
        ix->list.items[2]->numval != ix->list.items[3]->numval)
        return 0;
    if (v->kind != SEXPR_SYMBOL) return 0;
    Smt2Var *var = _find_var(fe, v->sym.str, v->sym.len);
    return var && ix->list.items[2]->numval < var->width;
}

/* Verilator's randomConstraint() with _VL_SOLVER_HASH_LEN 1:
 *   (= #b0|#b1 (bvxor <bit> <bit> ...))   or   (= #b0|#b1 <bit>)
 * where each <bit> is one bit of a declared variable. */
static int _is_vlt_hash(Smt2Frontend *fe, const Sexpr *e) {
    if (e->kind != SEXPR_LIST || e->list.count != 3 || !sexpr_is_symbol(e->list.items[0], "="))
        return 0;
    const Sexpr *lit = e->list.items[1], *x = e->list.items[2];
    if (lit->kind != SEXPR_BITVEC || lit->bv.width != 1) return 0;
    if (_is_vlt_hash_bit(fe, x)) return 1;
    if (x->kind != SEXPR_LIST || x->list.count < 3 || !sexpr_is_symbol(x->list.items[0], "bvxor"))
        return 0;
    for (uint32_t i = 1; i < x->list.count; i++)
        if (!_is_vlt_hash_bit(fe, x->list.items[i])) return 0;
    return 1;
}

/* Verilator (5.052+) UniGen2 enumeration shapes:
 *   blocking clause  (not (and [true] (= <term> <bv-literal>) ...))
 *   hash constraint  (= #b... (bvxor ...))  |  (= #b... (concat (bvxor ...) ...))
 * A session carrying these is an enumeration loop whose witness Verilator's own
 * sampler picks, so CDCL's sampling buys nothing there; and XOR plus nogood
 * enumeration is the bit-blaster's strength and interval CDCL's worst case
 * (per-query time grew to the 10 s cap). Such sessions route to bitblast. */
/* A term Verilator hashes a bit of: a declared variable, or an element of an
 * array variable, `(select <base> <bv-literal>)` with <base> itself such a term
 * (nested arrays). */
static int _is_vlt_hashed_term(const Sexpr *t) {
    while (t->kind == SEXPR_LIST) {
        if (t->list.count != 3 || !sexpr_is_symbol(t->list.items[0], "select")
            || t->list.items[2]->kind != SEXPR_BITVEC)
            return 0;
        t = t->list.items[1];
    }
    return t->kind == SEXPR_SYMBOL;
}

/* One operand of a Verilator hash XOR: `#b0` (UniGen2 seeds every chain with
 * it) or one bit, `((_ extract i i) <term>)`, of a variable or array element.
 * Nothing else, so a user constraint that happens to be an XOR (`a ^ b`) is
 * not mistaken for a hash. */
static int _is_vlt_xor_arg(Smt2Frontend *fe, const Sexpr *s) {
    if (s->kind == SEXPR_BITVEC) return s->bv.width == 1;
    if (_is_vlt_hash_bit(fe, s)) return 1;
    if (s->kind != SEXPR_LIST || s->list.count != 2) return 0;
    const Sexpr *ix = s->list.items[0];
    return ix->kind == SEXPR_LIST && ix->list.count == 4
        && sexpr_is_symbol(ix->list.items[0], "_")
        && sexpr_is_symbol(ix->list.items[1], "extract")
        && ix->list.items[2]->kind == SEXPR_NUMERAL
        && ix->list.items[3]->kind == SEXPR_NUMERAL
        && ix->list.items[2]->numval == ix->list.items[3]->numval
        && _is_vlt_hashed_term(s->list.items[1]);
}

static int _is_vlt_xor(Smt2Frontend *fe, const Sexpr *x) {
    if (x->kind != SEXPR_LIST || x->list.count < 2 || !sexpr_is_symbol(x->list.items[0], "bvxor"))
        return 0;
    for (uint32_t i = 1; i < x->list.count; i++)
        if (!_is_vlt_xor_arg(fe, x->list.items[i])) return 0;
    return 1;
}

static int _is_vlt_enum(Smt2Frontend *fe, const Sexpr *e) {
    if (e->kind != SEXPR_LIST) return 0;
    if (e->list.count == 2 && sexpr_is_symbol(e->list.items[0], "not")) {
        const Sexpr *a = e->list.items[1];
        if (a->kind != SEXPR_LIST || a->list.count < 2 || !sexpr_is_symbol(a->list.items[0], "and"))
            return 0;
        for (uint32_t i = 1; i < a->list.count; i++) {
            const Sexpr *x = a->list.items[i];
            if (sexpr_is_symbol(x, "true")) continue;
            if (x->kind != SEXPR_LIST || x->list.count != 3 || !sexpr_is_symbol(x->list.items[0], "=")
                || x->list.items[2]->kind != SEXPR_BITVEC)
                return 0;
        }
        return 1;
    }
    if (e->list.count == 3 && sexpr_is_symbol(e->list.items[0], "=")
        && e->list.items[1]->kind == SEXPR_BITVEC) {
        const Sexpr *x = e->list.items[2];
        if (_is_vlt_xor(fe, x)) return 1;
        if (x->kind == SEXPR_LIST && x->list.count >= 3 && sexpr_is_symbol(x->list.items[0], "concat")) {
            for (uint32_t i = 1; i < x->list.count; i++)
                if (!_is_vlt_xor(fe, x->list.items[i])) return 0;
            return 1;
        }
    }
    return 0;
}

/* `(assert (= a ((as const (Array ..)) v)))` on a declared sparse array `a`
 * with a literal `v`: Verilator emits this to clear a dynamic array before
 * sizing it, often for arrays the problem never reads. Record `v` as the
 * array's default (see Smt2ArrayValue.has_default) instead of building a
 * word-level equality, which routed the whole problem to the array engine at
 * ~5 ms a solve. Top level only (a pop could not undo it), and a second default
 * on the same array falls through to the general path. Returns 1 if taken. */
static int _try_sparse_default(Smt2Frontend *fe, const Sexpr *e) {
    if (fe->push_depth > 0 || e->kind != SEXPR_LIST || e->list.count != 3 ||
        !sexpr_is_symbol(e->list.items[0], "="))
        return 0;
    for (int side = 0; side < 2; side++) {
        const Sexpr *a = e->list.items[1 + side], *c = e->list.items[2 - side];
        if (a->kind != SEXPR_SYMBOL || c->kind != SEXPR_LIST || c->list.count != 2)
            continue;
        const Sexpr *h = c->list.items[0];
        if (h->kind != SEXPR_LIST || h->list.count != 3 ||
            !sexpr_is_symbol(h->list.items[0], "as") ||
            !sexpr_is_symbol(h->list.items[1], "const"))
            continue;
        Smt2ArrayVar *av = _find_array_var(fe, a->sym.str, a->sym.len);
        if (!av || !av->value) return 0;
        Smt2ArrayValue *v = av->value;
        Smt2ArraySort sort;
        if (_parse_array_sort(fe, h->list.items[2], &sort) <= 0 ||
            sort.addr_width != v->sort.addr_width || sort.data_width != v->sort.data_width)
            return 0;
        uint64_t lo, hi;
        if (!v->is_sparse || v->is_abstract || v->has_default || v->sort.inner_addr ||
            v->sort.data_width > 63 ||
            !_const_index(fe, c->list.items[1], v->sort.data_width, &lo, &hi))
            return 0;
        v->has_default = 1;
        v->default_val = lo;
        for (uint32_t i = 0; i < v->n_sparse; i++)
            _add_user_constraint(fe, dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ,
                dvs_builder_expr_var(fe->builder, v->sparse_varid[i]),
                _bv_const(fe, (int64_t)lo, v->sort.data_width)));
        return 1;
    }
    return 0;
}

static int _cmd_assert(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2) {
        fprintf(fe->err, "error: assert requires exactly one expression\n");
        return -1;
    }
    /* --verilator-hash=ignore: Verilator's parity asserts follow a `sat` and
     * nothing else does in its protocol; skip exactly that shape there. */
    if (fe->verilator_mode && fe->vlt_hash_ignore && fe->has_result &&
        fe->last_result == DVS_SOLVE_OK && _is_vlt_hash(fe, cmd->list.items[1])) {
        fe->vlt_hash_pending = 1;
        fe->cmd_dropped = 1;
        return 0;
    }
    fe->vlt_hash_pending = 0;      /* any other assert: solve for real */
    if (fe->verilator_mode && !fe->vlt_enum && _is_vlt_enum(fe, cmd->list.items[1])) fe->vlt_enum = 1;
    _record_named(fe, cmd->list.items[1]);
    if (_try_split_reified_and(fe, cmd->list.items[1])) return 0;

    if (_try_sparse_default(fe, cmd->list.items[1])) return 0;

    TypedExpr te;
    dvs_expr_t reified = _try_translate_in_set(fe, cmd->list.items[1]);
    if (reified == EXPR_NULL) reified = _try_translate_reified_or(fe, cmd->list.items[1]);
    if (reified != EXPR_NULL) {
        te.ref = reified;
        te.width = 1;
    } else {
        te = _translate_expr(fe, cmd->list.items[1]);
    }
    if (te.ref == EXPR_NULL) {
        /* Loud but in-sync: the assert used a construct we can't translate
         * (unsupported operator/sort). Taint the context so the next
         * (check-sat) answers `unknown` rather than an unsound result, and keep
         * the REPL running so the driver's protocol stream stays synchronized
         * (exiting here would close the pipe and hang a driver blocked on read). */
        fprintf(fe->err, "error: failed to translate assert expression "
                         "(unsupported construct) -> result will be unknown\n");
        SMT2_TAINT(fe, "assert used an untranslatable construct");
        return 0;
    }
    _add_user_constraint(fe, te.ref);
    return 0;
}

/* Initial static-pool size estimate for a problem. The pool holds per-variable
 * (Variable + watcher head + prop ref) and per-constraint (propagator) state,
 * all linear in the problem, atop the 8192-var incremental_capacity_hint floor.
 * The estimate only needs to be right for the common case: grow-and-retry in
 * _ensure_compiled reallocates a larger buffer if this under-provisions. */
static size_t _ctx_buf_size_for(const dvs_problem_t *p) {
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
 * Verilator mode (always bit-blast) we skip dvs_solver_create + dvs_solver_compile here
 * and, symmetrically, the dvs_solver_destroy/ctx_buf-free the (reset) would pay --
 * both were pure waste on every randomize(). */
/* Returns 0 on success, -1 on a hard error, -2 when the problem is stale but
 * cannot be safely rebuilt (caller must answer `unknown`, never a verdict).
 *
 * The builder is deliberately NOT reset after finalize here: keeping it lets a
 * later (assert) be folded in by re-finalizing the FULL set (B14). Any path
 * that does reset the builder must clear builder_retained. */
static int _ensure_problem(Smt2Frontend *fe) {
    if (fe->problem && !fe->problem_dirty) return 0;
    if (fe->problem) {
        /* Stale. Rebuilding is only sound while the builder still holds every
         * constraint; if something reset it in between, the accumulated set is
         * gone and a rebuild would silently DROP the earlier asserts -- exactly
         * the wrong-`sat` this fix exists to prevent. Bail to `unknown`. */
        if (!fe->builder_retained) return -2;
        free(fe->problem);
        fe->problem = NULL;
    }
    fe->problem = _explicit(dvs_builder_finalize(fe->builder, &fe->problem_size));
    if (!fe->problem) return -1;
    fe->builder_retained = 1;
    fe->problem_dirty    = 0;
    fe->has_aux          = 0;
    return 0;
}

static void _start_cdcl_retention(Smt2Frontend *fe);

static int _ensure_compiled(Smt2Frontend *fe) {
    if (fe->ctx) return 0;
    /* A prior bit-blast-routed solve may have finalized one already (possible
     * when needs_bitblast flips mid-session); free it before overwriting. */
    if (fe->problem) { free(fe->problem); fe->problem = NULL; }
    fe->problem = _explicit(dvs_builder_finalize(fe->builder, &fe->problem_size));
    if (!fe->problem) return -1;
    /* The builder still holds every assertion; if no context comes of this
     * (e.g. variables wider than 64 bits), the bitblast path re-finalizes
     * from it after later asserts or a pop. A successful compile clears it. */
    fe->builder_retained = 1;
    fe->problem_dirty    = 0;

    /* The static ctx pool is a fixed-capacity, relocatable bump allocator by
     * design (32-bit offsets, snapshot wholesale for push/pop checkpoints), so
     * it cannot grow in place. Rather than always allocate the 64 MB maximum --
     * a large per-solve cost the frontend pays on every Verilator randomize() --
     * start from a problem-sized estimate and, if dvs_solver_compile overflows the
     * pool, reallocate a larger contiguous buffer and recompile. This gives
     * dynamic sizing with no risk of under-provisioning (capped at the old max). */
    if (!fe->ctx_buf_scale) fe->ctx_buf_scale = 1;
    size_t sz = _ctx_buf_size_for(fe->problem) * fe->ctx_buf_scale;
    if (sz > (size_t)CTX_BUF_SIZE) sz = (size_t)CTX_BUF_SIZE;
    for (;;) {
        fe->ctx_buf = malloc(sz);
        if (!fe->ctx_buf) return -1;
        fe->ctx_buf_size = sz;
        fe->block_alloc = dvs_block_alloc_create(NULL, BA_BLOCK_SIZE);
        if (!fe->block_alloc) { free(fe->ctx_buf); fe->ctx_buf = NULL; return -1; }
        fe->ctx = dvs_solver_create(fe->ctx_buf, sz, fe->block_alloc);
        if (!fe->ctx) {
            dvs_block_alloc_destroy(fe->block_alloc); fe->block_alloc = NULL;
            free(fe->ctx_buf); fe->ctx_buf = NULL;
            return -1;
        }
        /* Vars[] capacity headroom for yosys-smtbmc-style incremental aux vars. */
        fe->ctx->incremental_capacity_hint = 8192;
        int rc = dvs_solver_compile(fe->ctx, fe->problem);
        /* The pool cannot grow once compiled, and an incremental assert
         * (e.g. after a push) that overflows it leaves its constraints
         * uncompiled -- the first such add also re-copies the reset snapshot
         * of every variable. Keep a quarter of the compile's use free;
         * failing that, recompile into a bigger pool. */
        if (rc >= 0 && sz < (size_t)CTX_BUF_SIZE) {
            size_t used = fe->ctx->pool.used, cap = fe->ctx->pool.capacity;
            size_t want = used / 4 > ((size_t)512 << 10) ? used / 4 : ((size_t)512 << 10);
            if (cap - used < want) {
                dvs_solver_destroy(fe->ctx); fe->ctx = NULL;
                dvs_block_alloc_destroy(fe->block_alloc); fe->block_alloc = NULL;
                free(fe->ctx_buf); fe->ctx_buf = NULL;
                size_t need = used + want;
                while (sz < need && sz < (size_t)CTX_BUF_SIZE) { sz *= 2; if (fe->ctx_buf_scale < 64) fe->ctx_buf_scale *= 2; }
                if (sz > (size_t)CTX_BUF_SIZE) sz = (size_t)CTX_BUF_SIZE;
                continue;
            }
        }
        if (rc >= 0) {
            /* rc > 0 (some constraints left uncompiled; validation and
             * escalation cover them) is a context like any other. Leaving
             * `compiled` unset made _flush_aux drop every later assertion:
             * after `(assert (xor true ...))`, `(assert false)` answered sat
             * (B58). */
            fe->compiled = 1;
            fe->builder_retained = 0;   /* fe->problem can't be rebuilt in place (B14) */
            fe->has_aux = 0;
            _start_cdcl_retention(fe);
            return rc;
        }
        /* Grow-and-retry only on a genuine pool overflow (not a compile-time
         * UNSAT, rc == -2, or other error), and only until the cap. */
        if (fe->ctx->pool.overflow && sz < (size_t)CTX_BUF_SIZE) {
            dvs_solver_destroy(fe->ctx); fe->ctx = NULL;
            dvs_block_alloc_destroy(fe->block_alloc); fe->block_alloc = NULL;
            free(fe->ctx_buf); fe->ctx_buf = NULL;
            sz = (sz * 4 < (size_t)CTX_BUF_SIZE) ? sz * 4 : (size_t)CTX_BUF_SIZE;
            if (fe->ctx_buf_scale < 64) fe->ctx_buf_scale *= 4;
            continue;
        }
        if (rc != -2) {
            /* No usable context (e.g. a variable wider than 64 bits): release
             * it. Leaving it made the next push checkpoint a context that was
             * never compiled, and the pop then kept the popped assertions in
             * the builder the bitblast path solves -- a wrong unsat (B63). */
            dvs_solver_destroy(fe->ctx); fe->ctx = NULL;
            dvs_block_alloc_destroy(fe->block_alloc); fe->block_alloc = NULL;
            free(fe->ctx_buf); fe->ctx_buf = NULL;
        }
        return rc;
    }
}

/* After the CDCL compile, keep the builder (holding every assertion so far) so
 * a later CDCL `unknown` can escalate to bitblast even after incremental
 * asserts -- unless it is already too big to keep copying on every hand-off. */
#define SMT2_RETAIN_MAX_BYTES (16u << 20)
static void _start_cdcl_retention(Smt2Frontend *fe) {
    if (dvs_builder_virtual_used(fe->builder) <= SMT2_RETAIN_MAX_BYTES) {
        fe->cdcl_retained = 1;
        fe->aux_mark = dvs_builder_mark(fe->builder);
    } else {
        dvs_builder_reset(fe->builder);
        fe->cdcl_retained = 0;
        fe->bb_inc = 0;
        memset(&fe->aux_mark, 0, sizeof(fe->aux_mark));
    }
}

/* The builder's items since aux_mark have been handed to CDCL. */
static void _aux_handed_off(Smt2Frontend *fe) {
    if (fe->cdcl_retained &&
        dvs_builder_virtual_used(fe->builder) <= SMT2_RETAIN_MAX_BYTES) {
        fe->aux_mark = dvs_builder_mark(fe->builder);
    } else {
        dvs_builder_reset(fe->builder);
        fe->cdcl_retained = 0;
        fe->bb_inc = 0;
        memset(&fe->aux_mark, 0, sizeof(fe->aux_mark));
    }
    fe->builder_retained = 0;
    fe->has_aux = 0;
}

static int _flush_aux(Smt2Frontend *fe) {
    if (!fe->compiled || !fe->has_aux) return 0;
    size_t aux_sz = 0;
    /* Only the items added since the last hand-off: the builder may also hold
     * everything before it (cdcl_retained), which the ctx already has. */
    dvs_problem_t *aux = _explicit(dvs_builder_finalize_since(fe->builder, &fe->aux_mark, &aux_sz));
    if (!aux) return -1;
    int rc = dvs_solver_add_constraint(fe->ctx, aux);
    /* An add that overflows the context pool compiles only part of `aux`, and
     * can read the failed allocation as a conflict: neither its `unsat` nor
     * the context can be trusted. Report it incomplete -- check-sat then
     * answers from bitblast -- and give later instances a bigger pool. */
    if (fe->ctx->pool.overflow) {
        if (fe->ctx_buf_scale && fe->ctx_buf_scale < 64) fe->ctx_buf_scale *= 2;
        rc = 1;
    }
    /* Retain the aux dvs_problem_t (instead of freeing) so the post-solve
     * model-validation pass can re-evaluate the constraints it contributed.
     * Storage is reclaimed in smt2_frontend_destroy. */
    if (fe->n_aux_problems == fe->aux_problems_cap) {
        uint32_t new_cap = fe->aux_problems_cap ? fe->aux_problems_cap * 2 : 8;
        dvs_problem_t **grow = (dvs_problem_t **)realloc(fe->aux_problems,
                                  new_cap * sizeof(dvs_problem_t *));
        if (!grow) {
            /* Out of memory: fall back to the old behaviour (drop the
             * aux). Validation will be incomplete for this run but the
             * solver result is unaffected. */
            free(aux);
            _aux_handed_off(fe);
            return rc;
        }
        fe->aux_problems     = grow;
        fe->aux_problems_cap = new_cap;
    }
    fe->aux_problems[fe->n_aux_problems++] = aux;
    _aux_handed_off(fe);
    return rc;
}

/* Fingerprint of a finalized dvs_problem_t. dvs_builder_finalize() zeroes the
 * whole buffer then fills it deterministically from the builder's blocks, and
 * every dvs_expr_t is a pool-relative offset, so identical problems produce
 * byte-identical content and thus the same fingerprint. We hash the leading
 * scalar/head fields plus the pool DATA region, skipping the embedded dvs_pool_t
 * header (which may hold non-deterministic bookkeeping). Used only as a cache
 * key in Verilator mode; a collision would at worst reuse a wrong-but-still-
 * sound model, and the space (64-bit) makes that astronomically unlikely. */
static uint64_t _fp_mix(uint64_t h, const uint8_t *d, size_t n) {
    /* Eight bytes per step: the byte-wise loop was ~17% of an enumeration-heavy
     * run, since every check-sat hashes the whole pool. */
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, d + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 32;
    }
    for (; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t _problem_fingerprint(const dvs_problem_t *p) {
    uint64_t h = 1469598103934665603ULL;
    size_t lead_len = (size_t)((const uint8_t *)&p->pool - (const uint8_t *)p);
    h = _fp_mix(h, (const uint8_t *)p, lead_len);
    h = _fp_mix(h, (const uint8_t *)&p->pool + sizeof(dvs_pool_t), p->pool.used);
    h ^= (uint64_t)p->pool.used;
    return h ? h : 1;                                  /* never return 0 */
}

/* Emit the array theory axioms for all abstract-array reads + equalities
 * (DV_ARRAY Phase A: eager Ackermann). Runs on the builder before finalize.
 *
 * Complete for QF_ABV: (1) read-over-write pushes each read down its store/ite
 * chain to BASE/CONST leaves; (2) congruence ties reads on a shared BASE leaf by
 * index equality; (3) array equality a==b is reified onto a boolean p with
 * consistency (p -> equal reads at every shared index) and a Skolem
 * extensionality witness (¬p -> read(a,w)!=read(b,w)). A fixpoint first grows the
 * read set so every needed (node,index) read exists: ROW/ITE create parent
 * reads, and an equality mirrors reads across its two sides. The set is finite
 * ((node,index) pairs are bounded), so it terminates. Emitted constraints are
 * ordinary BV predicates -- any engine solves them and a returned model already
 * satisfies the array theory. Returns 0, or -1 on OOM. */
static int _emit_array_axioms(Smt2Frontend *fe) {
    if (!fe->array_lazy || (fe->n_areads == 0 && fe->n_aeqs == 0)) return 0;
    dvs_builder_t *b = fe->builder;

    /* Seed one extensionality witness read per equality (fresh index, read on
     * both sides). */
    for (uint32_t e = 0; e < fe->n_aeqs; e++) {
        Smt2ArrayValue *A = fe->aeqs[e].a, *B = fe->aeqs[e].b;
        uint16_t aw = A->sort.addr_width, dw = A->sort.data_width;
        uint32_t k = _fresh_read_var(fe, aw ? aw : 1);
        dvs_expr_t kref = dvs_builder_expr_var(b, k);
        fe->aeqs[e].wit_idx_ref = kref;
        if (_abs_find_or_create_read(fe, A, kref, k, dw) == UINT32_MAX) return -1;
        if (_abs_find_or_create_read(fe, B, kref, k, dw) == UINT32_MAX) return -1;
    }

    /* Fixpoint: grow the read set until stable. */
    for (;;) {
        uint32_t before = fe->n_areads;
        for (uint32_t i = 0; i < before; i++) {
            Smt2ArrayValue *node = fe->areads[i].node;
            dvs_expr_t  idx  = fe->areads[i].idx_ref;
            uint32_t idxv = fe->areads[i].idx_varid;
            uint16_t w    = fe->areads[i].width;
            if (node->akind == SMT2_ANODE_STORE) {
                if (_abs_find_or_create_read(fe, node->parent, idx, idxv, w) == UINT32_MAX) return -1;
            } else if (node->akind == SMT2_ANODE_ITE) {
                if (_abs_find_or_create_read(fe, node->parent, idx, idxv, w) == UINT32_MAX) return -1;
                if (_abs_find_or_create_read(fe, node->else_node, idx, idxv, w) == UINT32_MAX) return -1;
            }
        }
        for (uint32_t e = 0; e < fe->n_aeqs; e++) {
            Smt2ArrayValue *A = fe->aeqs[e].a, *B = fe->aeqs[e].b;
            for (uint32_t i = 0, n = fe->n_areads; i < n; i++) {
                Smt2ArrayValue *node = fe->areads[i].node;
                if (node != A && node != B) continue;
                Smt2ArrayValue *other = (node == A) ? B : A;
                dvs_expr_t  idx  = fe->areads[i].idx_ref;
                uint32_t idxv = fe->areads[i].idx_varid;
                uint16_t w    = fe->areads[i].width;
                if (_abs_find_or_create_read(fe, other, idx, idxv, w) == UINT32_MAX) return -1;
            }
        }
        if (fe->n_areads == before) break;
    }

    /* (1) Read-over-write / ite / const defining constraints. All parent reads
     * exist now, so these find (not create) and cannot realloc mid-emit. */
    for (uint32_t i = 0; i < fe->n_areads; i++) {
        Smt2ArrayValue *node = fe->areads[i].node;
        dvs_expr_t  idx  = fe->areads[i].idx_ref;
        uint32_t idxv = fe->areads[i].idx_varid;
        uint16_t w    = fe->areads[i].width;
        dvs_expr_t  rref = dvs_builder_expr_var(b, fe->areads[i].read_varid);
        switch (node->akind) {
        case SMT2_ANODE_STORE: {
            uint32_t rp = _abs_find_or_create_read(fe, node->parent, idx, idxv, w);
            dvs_expr_t cond = dvs_builder_expr_binary(b, DVS_BIN_EQ, idx, node->store_idx_ref);
            dvs_expr_t sel  = dvs_builder_expr_ite(b, cond, node->store_val,
                                            dvs_builder_expr_var(b, rp));
            dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ, rref, sel));
            break;
        }
        case SMT2_ANODE_ITE: {
            uint32_t rt = _abs_find_or_create_read(fe, node->parent, idx, idxv, w);
            uint32_t re = _abs_find_or_create_read(fe, node->else_node, idx, idxv, w);
            dvs_expr_t sel = dvs_builder_expr_ite(b, node->cond_ref,
                                           dvs_builder_expr_var(b, rt),
                                           dvs_builder_expr_var(b, re));
            dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ, rref, sel));
            break;
        }
        case SMT2_ANODE_CONST:
            dvs_builder_add_constraint(b,
                dvs_builder_expr_binary(b, DVS_BIN_EQ, rref, node->store_val));
            break;
        default: /* SMT2_ANODE_BASE: congruence below */
            break;
        }
    }

    /* (2) Congruence on shared BASE leaves: (idx_a==idx_b) -> (r_a==r_b). Reads
     * with the same index already dedup to one read var, so every pair here has
     * distinct indices. */
    for (uint32_t i = 0; i < fe->n_areads; i++) {
        if (fe->areads[i].node->akind != SMT2_ANODE_BASE) continue;
        for (uint32_t j = i + 1; j < fe->n_areads; j++) {
            if (fe->areads[j].node != fe->areads[i].node) continue;
            dvs_expr_t cond = dvs_builder_expr_binary(b, DVS_BIN_EQ,
                              fe->areads[i].idx_ref, fe->areads[j].idx_ref);
            dvs_expr_t eqv  = dvs_builder_expr_binary(b, DVS_BIN_EQ,
                              dvs_builder_expr_var(b, fe->areads[i].read_varid),
                              dvs_builder_expr_var(b, fe->areads[j].read_varid));
            dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_OR,
                              dvs_builder_expr_unary(b, DVS_UN_NOT, cond), eqv));
        }
    }

    /* (3) Array equality reification. */
    for (uint32_t e = 0; e < fe->n_aeqs; e++) {
        Smt2ArrayValue *A = fe->aeqs[e].a, *B = fe->aeqs[e].b;
        uint16_t dw = A->sort.data_width;
        dvs_expr_t pref  = dvs_builder_expr_var(b, fe->aeqs[e].p_varid);
        dvs_expr_t npref = dvs_builder_expr_unary(b, DVS_UN_NOT, pref);
        /* Consistency: p -> read(A,i)==read(B,i) for every index read on A
         * (== indices read on B after the coupling fixpoint). */
        for (uint32_t i = 0; i < fe->n_areads; i++) {
            if (fe->areads[i].node != A) continue;
            uint32_t rB = _abs_find_or_create_read(fe, B, fe->areads[i].idx_ref,
                                                   fe->areads[i].idx_varid, dw);
            dvs_expr_t eqv = dvs_builder_expr_binary(b, DVS_BIN_EQ,
                              dvs_builder_expr_var(b, fe->areads[i].read_varid),
                              dvs_builder_expr_var(b, rB));
            dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_OR, npref, eqv));
        }
        /* Extensionality: ¬p -> read(A,w)!=read(B,w). */
        uint32_t idxv = _idx_varid(fe, fe->aeqs[e].wit_idx_ref);
        uint32_t rAk = _abs_find_or_create_read(fe, A, fe->aeqs[e].wit_idx_ref, idxv, dw);
        uint32_t rBk = _abs_find_or_create_read(fe, B, fe->aeqs[e].wit_idx_ref, idxv, dw);
        dvs_expr_t neq = dvs_builder_expr_binary(b, DVS_BIN_NEQ,
                          dvs_builder_expr_var(b, rAk), dvs_builder_expr_var(b, rBk));
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_OR, pref, neq));
    }
    return 0;
}

/* ================= DV_ARRAY lazy (lemmas-on-demand) ==================== *
 * The lazy engine finalizes the BV skeleton (constraints reference abstract
 * read vars; NO array axioms) with pool headroom, solves on the incremental
 * CaDiCaL backend, then refines: each SAT model is checked against the array
 * theory and only violated read-over-write / congruence / equality lemmas are
 * asserted (with any newly needed read var minted in-place), then re-solved.
 * Terminates when a model satisfies every axiom (real SAT) or the solver
 * reports UNSAT. Every lemma is a valid QF_ABV consequence, so UNSAT is real
 * and a returned SAT model is array-consistent by construction. */

/* Model value of a plain-var / const dvs_expr_t under the current bb_solver model. */
static int64_t _abs_mval_ref(Smt2Frontend *fe, dvs_expr_t ref) {
    if (ref == EXPR_NULL) return 0;
    ExprKind *k = (ExprKind *)POOL_PTR(fe->problem, ref);
    if (!k) return 0;
    if (*k == EXPR_VAR) {
        int64_t v = 0;
        dvs_bbsolver_value(fe->bb_solver, ((ExprVar *)k)->var_id, &v);
        return v;
    }
    if (*k == EXPR_CONST) return ((ExprConst *)k)->value;
    return 0;
}
static int64_t _abs_mval_var(Smt2Frontend *fe, uint32_t varid) {
    int64_t v = 0;
    dvs_bbsolver_value(fe->bb_solver, varid, &v);
    return v;
}

/* Pure find (no create) of the read var for (node, idx). UINT32_MAX if absent. */
static uint32_t _abs_find_read(Smt2Frontend *fe, Smt2ArrayValue *node,
                               dvs_expr_t idx_ref, uint32_t idx_varid) {
    uint32_t slot = _aread_lookup(fe, node, idx_varid, idx_ref);
    return slot == UINT32_MAX ? UINT32_MAX : fe->areads[slot].read_varid;
}

/* Find or create read(node, idx), minting the var IN-PLACE on fe->problem (the
 * finalized-with-headroom pool). Returns the read var_id, or UINT32_MAX on OOM
 * / slack exhaustion. */
static uint32_t _abs_read_inplace(Smt2Frontend *fe, Smt2ArrayValue *node,
                                  dvs_expr_t idx_ref, uint32_t idx_varid, uint16_t w) {
    uint32_t ex = _abs_find_read(fe, node, idx_ref, idx_varid);
    if (ex != UINT32_MAX) return ex;
    if (fe->n_areads == fe->areads_cap) {
        uint32_t nc = fe->areads_cap ? fe->areads_cap * 2 : 32;
        Smt2ArrayRead *g = (Smt2ArrayRead *)realloc(fe->areads,
                                                    nc * sizeof(Smt2ArrayRead));
        if (!g) return UINT32_MAX;
        fe->areads = g; fe->areads_cap = nc;
    }
    uint32_t id = fe->problem->n_vars;
    if (fe->n_vars > id) id = fe->n_vars;
    if (problem_add_var(fe->problem, id, (uint8_t)w, 0, 0,
                        _bv_unsigned_hi(w)) == EXPR_NULL)
        return UINT32_MAX;   /* pool slack exhausted -> caller bails to unknown */
    _bump_n_vars(fe, id + 1);
    _aread_reserve_one(fe);
    Smt2ArrayRead *r = &fe->areads[fe->n_areads++];
    r->node = node; r->idx_ref = idx_ref; r->idx_varid = idx_varid;
    r->read_varid = id; r->width = w; r->emitted = 0;
    _aread_put_last(fe);
    return id;
}

/* Assert an in-place-built predicate into the live incremental solver. */
static int _abs_assert(Smt2Frontend *fe, dvs_expr_t pred) {
    if (pred == EXPR_NULL) return -1;
    return dvs_bbsolver_assert(fe->bb_solver, pred) == 0 ? 0 : -1;
}

/* Build read i's one-step defining constraint (STORE/ITE/CONST), minting parent
 * reads in-place as needed. Marks emitted. Returns the lemma dvs_expr_t, or
 * EXPR_NULL on OOM / slack exhaustion. Does NOT assert (the caller batches
 * asserts after model reads, since asserting invalidates the SAT model). */
static dvs_expr_t _abs_build_read_def(Smt2Frontend *fe, uint32_t i) {
    dvs_problem_t *P = fe->problem;
    Smt2ArrayValue *node = fe->areads[i].node;
    dvs_expr_t idx  = fe->areads[i].idx_ref;
    uint32_t idxv = fe->areads[i].idx_varid;
    uint32_t r   = fe->areads[i].read_varid;
    uint16_t w   = fe->areads[i].width;
    dvs_expr_t rref = expr_var(P, r);
    dvs_expr_t lem = EXPR_NULL;
    if (node->akind == SMT2_ANODE_STORE) {
        uint32_t rp = _abs_read_inplace(fe, node->parent, idx, idxv, w);
        if (rp == UINT32_MAX) return EXPR_NULL;
        dvs_expr_t cond = expr_binary(P, DVS_BIN_EQ, idx, node->store_idx_ref);
        dvs_expr_t sel  = expr_ite(P, cond, node->store_val, expr_var(P, rp));
        lem = expr_binary(P, DVS_BIN_EQ, rref, sel);
    } else if (node->akind == SMT2_ANODE_ITE) {
        uint32_t rt = _abs_read_inplace(fe, node->parent, idx, idxv, w);
        uint32_t re = _abs_read_inplace(fe, node->else_node, idx, idxv, w);
        if (rt == UINT32_MAX || re == UINT32_MAX) return EXPR_NULL;
        dvs_expr_t sel = expr_ite(P, node->cond_ref, expr_var(P, rt), expr_var(P, re));
        lem = expr_binary(P, DVS_BIN_EQ, rref, sel);
    } else if (node->akind == SMT2_ANODE_CONST) {
        lem = expr_binary(P, DVS_BIN_EQ, rref, node->store_val);
    } else {
        return EXPR_NULL;   /* BASE: no defining constraint */
    }
    fe->areads[i].emitted = 1;
    return lem;
}

/* Lever D: in the NON-extensional case (no array equality), emit only the
 * model-relevant HALF of a STORE read's read-over-write axiom, as an implication
 * rather than the full ite-mux equality:
 *   hit  (idx==sidx): (idx!=sidx) | (r == store_val)     -- no parent read
 *   miss (idx!=sidx): (idx==sidx) | (r == read(parent))  -- pushes down one level
 * Each half is a valid QF_ABV consequence, and for the non-extensional theory
 * emitting the model-active half on demand is complete (see
 * docs/large_memory_array_design.md 4.1). Much lighter for the SAT backend --
 * it drops the 32-bit mux + stored value from every miss level, which is where
 * the deep-chain solve cost lives. `emitted` is used as a 2-bit mask (HIT|MISS)
 * so the other half is added if a later model flips the branch. Returns the
 * lemma dvs_expr_t (EXPR_NULL on OOM). */
#define _AR_HIT  1u
#define _AR_MISS 2u
static dvs_expr_t _abs_build_store_half(Smt2Frontend *fe, uint32_t i, uint8_t want) {
    dvs_problem_t *P = fe->problem;
    Smt2ArrayValue *node = fe->areads[i].node;
    dvs_expr_t idx  = fe->areads[i].idx_ref;
    dvs_expr_t rref = expr_var(P, fe->areads[i].read_varid);
    dvs_expr_t cond = expr_binary(P, DVS_BIN_EQ, idx, node->store_idx_ref);
    dvs_expr_t lem;
    if (want == _AR_HIT) {
        dvs_expr_t eqv = expr_binary(P, DVS_BIN_EQ, rref, node->store_val);
        lem = expr_binary(P, DVS_BIN_OR, expr_unary(P, DVS_UN_NOT, cond), eqv);
    } else {
        uint32_t rp = _abs_read_inplace(fe, node->parent, idx,
                                        fe->areads[i].idx_varid, fe->areads[i].width);
        if (rp == UINT32_MAX) return EXPR_NULL;
        dvs_expr_t eqv = expr_binary(P, DVS_BIN_EQ, rref, expr_var(P, rp));
        lem = expr_binary(P, DVS_BIN_OR, cond, eqv);
    }
    if (lem != EXPR_NULL) fe->areads[i].emitted |= want;
    return lem;
}

/* Seed reads at every STORE index in an abstract array node's chain, on the
 * given `side` node. Required for array-equality completeness: two arrays being
 * equal must agree at every index where either has a store, even if that index
 * is never explicitly selected. Walks parent/ite structure; bounded by chain
 * length. Returns 0 / -1 on OOM. */
static int _seed_store_index_reads(Smt2Frontend *fe, Smt2ArrayValue *chain,
                                   Smt2ArrayValue *side, uint16_t dw) {
    /* Iterative DFS over the (finite, acyclic) node DAG of `chain`. */
    for (Smt2ArrayValue *n = chain; n; ) {
        if (n->akind == SMT2_ANODE_STORE) {
            uint32_t iv = _idx_varid(fe, n->store_idx_ref);
            if (_abs_read_inplace(fe, side, n->store_idx_ref, iv, dw) == UINT32_MAX)
                return -1;
            n = n->parent;
        } else if (n->akind == SMT2_ANODE_ITE) {
            if (_seed_store_index_reads(fe, n->parent, side, dw) < 0) return -1;
            n = n->else_node;
        } else {
            break;   /* BASE / CONST: no more store indices */
        }
    }
    return 0;
}

/* Per-equality setup: (1) create reads on both sides at every store index of
 * either chain (equality completeness), and (2) add the extensionality witness
 * (p) | (read(A,w) != read(B,w)) with a fresh witness index w. */
static int _array_emit_extensionality(Smt2Frontend *fe) {
    dvs_problem_t *P = fe->problem;
    for (uint32_t e = 0; e < fe->n_aeqs; e++) {
        Smt2ArrayValue *A = fe->aeqs[e].a, *B = fe->aeqs[e].b;
        uint16_t dw = A->sort.data_width;
        uint16_t aw = A->sort.addr_width ? A->sort.addr_width : 1;
        /* Store-index reads on BOTH sides for BOTH chains (so consistency, which
         * ties reads at every shared index, covers all store points). */
        if (_seed_store_index_reads(fe, A, A, dw) < 0) return -1;
        if (_seed_store_index_reads(fe, A, B, dw) < 0) return -1;
        if (_seed_store_index_reads(fe, B, A, dw) < 0) return -1;
        if (_seed_store_index_reads(fe, B, B, dw) < 0) return -1;
        /* Extensionality witness. */
        uint32_t kid = fe->problem->n_vars;
        if (fe->n_vars > kid) kid = fe->n_vars;
        if (problem_add_var(P, kid, (uint8_t)aw, 0, 0,
                            _bv_unsigned_hi(aw)) == EXPR_NULL) return -1;
        _bump_n_vars(fe, kid + 1);
        dvs_expr_t kref = expr_var(P, kid);
        fe->aeqs[e].wit_idx_ref = kref;
        uint32_t rA = _abs_read_inplace(fe, A, kref, kid, dw);
        uint32_t rB = _abs_read_inplace(fe, B, kref, kid, dw);
        if (rA == UINT32_MAX || rB == UINT32_MAX) return -1;
        dvs_expr_t neq = expr_binary(P, DVS_BIN_NEQ, expr_var(P, rA), expr_var(P, rB));
        dvs_expr_t lem = expr_binary(P, DVS_BIN_OR, expr_var(P, fe->aeqs[e].p_varid), neq);
        if (_abs_assert(fe, lem) < 0) return -1;
    }
    return 0;
}

/* One refinement pass over the current model. IMPORTANT: reading a CaDiCaL model
 * value and asserting a clause cannot interleave -- the first assert leaves the
 * "satisfied" state, so a later val() aborts. So this runs in two phases: phase A
 * reads the model and BUILDS every violated lemma (minting read vars is fine --
 * only clause asserts invalidate the model); phase B asserts them all. Returns
 * the number of lemmas asserted, or -1 on OOM / slack exhaustion. */
/* Congruence bucketing entry (Lever B): reads sorted by (node, model index
 * value) so only genuinely aliasing reads are compared. */
typedef struct {
    const Smt2ArrayValue *node;
    int64_t  idxv;   /* model value of the index */
    int64_t  rval;   /* model value of the read */
    uint32_t slot;   /* areads[] slot */
} Smt2CongEnt;

static int _cong_cmp(const void *pa, const void *pb) {
    const Smt2CongEnt *a = (const Smt2CongEnt *)pa, *b = (const Smt2CongEnt *)pb;
    if (a->node != b->node)
        return (uintptr_t)a->node < (uintptr_t)b->node ? -1 : 1;
    if (a->idxv != b->idxv) return a->idxv < b->idxv ? -1 : 1;
    return 0;
}

static int _array_refine(Smt2Frontend *fe) {
    dvs_problem_t *P = fe->problem;
    dvs_expr_t *lems = NULL;
    uint32_t nlem = 0, cap = 0;
    #define _LEM_PUSH(L) do {                                                  \
        dvs_expr_t _l = (L);                                                      \
        if (_l == EXPR_NULL) { free(lems); return -1; }                        \
        if (nlem == cap) {                                                     \
            cap = cap ? cap * 2 : 64;                                          \
            dvs_expr_t *_g = (dvs_expr_t *)realloc(lems, cap * sizeof(dvs_expr_t));     \
            if (!_g) { free(lems); return -1; }                               \
            lems = _g;                                                         \
        }                                                                      \
        lems[nlem++] = _l;                                                     \
    } while (0)

    /* --- Phase A: read model, build violated lemmas (no asserts) --- */

    /* (1) Read-over-write / ite / const: materialize a read's one-step
     *     definition when the model violates it (or a needed child read is
     *     missing, forcing the chain to expand one level along the model path).
     *     nonext (no array equality) uses Lever D: lighter model-relevant HALF
     *     lemmas for STORE reads (see _abs_build_store_half). */
    int nonext = (fe->n_aeqs == 0);
    for (uint32_t i = 0; i < fe->n_areads; i++) {
        Smt2ArrayValue *node = fe->areads[i].node;
        if (node->akind == SMT2_ANODE_BASE) continue;
        dvs_expr_t  idx  = fe->areads[i].idx_ref;
        uint32_t idxv = fe->areads[i].idx_varid;

        if (nonext && node->akind == SMT2_ANODE_STORE) {
            uint64_t k    = (uint64_t)_abs_mval_ref(fe, idx);
            uint64_t sidx = (uint64_t)_abs_mval_ref(fe, node->store_idx_ref);
            uint8_t want = (k == sidx) ? _AR_HIT : _AR_MISS;
            if (fe->areads[i].emitted & want) continue;  /* active half present */
            _LEM_PUSH(_abs_build_store_half(fe, i, want));
            continue;
        }

        if (fe->areads[i].emitted) continue;
        int64_t  rv   = _abs_mval_var(fe, fe->areads[i].read_varid);
        int emit = 0;
        if (node->akind == SMT2_ANODE_CONST) {
            if (rv != _abs_mval_ref(fe, node->store_val)) emit = 1;
        } else if (node->akind == SMT2_ANODE_STORE) {
            uint64_t k    = (uint64_t)_abs_mval_ref(fe, idx);
            uint64_t sidx = (uint64_t)_abs_mval_ref(fe, node->store_idx_ref);
            if (k == sidx) {
                if (rv != _abs_mval_ref(fe, node->store_val)) emit = 1;
            } else {
                uint32_t rp = _abs_find_read(fe, node->parent, idx, idxv);
                if (rp == UINT32_MAX || _abs_mval_var(fe, rp) != rv) emit = 1;
            }
        } else if (node->akind == SMT2_ANODE_ITE) {
            int64_t c = _abs_mval_ref(fe, node->cond_ref);
            Smt2ArrayValue *child = c ? node->parent : node->else_node;
            uint32_t rc_ = _abs_find_read(fe, child, idx, idxv);
            if (rc_ == UINT32_MAX || _abs_mval_var(fe, rc_) != rv) emit = 1;
        }
        if (emit) _LEM_PUSH(_abs_build_read_def(fe, i));
    }

    /* (2) Congruence: two reads on the SAME array node at model-equal indices
     *     but differing values. Applies to every node kind (not just BASE): a
     *     store/ite node is still a function of its index, and enforcing this
     *     directly is sound and avoids relying on ROW being fully materialized.
     *     Bucketed by (node, model index value) via a sort so only genuinely
     *     aliasing reads are compared -- the lemma set is identical to the naive
     *     all-pairs scan, but the cost is O(n log n) + within-bucket pairs
     *     instead of O(n^2) over every read pair. */
    if (fe->n_areads > 1) {
        Smt2CongEnt *ce = (Smt2CongEnt *)malloc(fe->n_areads * sizeof(*ce));
        if (!ce) { free(lems); return -1; }
        for (uint32_t i = 0; i < fe->n_areads; i++) {
            ce[i].node = fe->areads[i].node;
            ce[i].idxv = _abs_mval_ref(fe, fe->areads[i].idx_ref);
            ce[i].rval = _abs_mval_var(fe, fe->areads[i].read_varid);
            ce[i].slot = i;
        }
        qsort(ce, fe->n_areads, sizeof(*ce), _cong_cmp);
        for (uint32_t a = 0; a < fe->n_areads; ) {
            uint32_t b = a + 1;
            while (b < fe->n_areads &&
                   ce[b].node == ce[a].node && ce[b].idxv == ce[a].idxv) b++;
            /* run [a,b): same node, same model index value */
            for (uint32_t x = a; x < b; x++)
                for (uint32_t y = x + 1; y < b; y++) {
                    if (ce[x].rval == ce[y].rval) continue;
                    uint32_t si = ce[x].slot, sj = ce[y].slot;
                    dvs_expr_t cond = expr_binary(P, DVS_BIN_EQ,
                                      fe->areads[si].idx_ref, fe->areads[sj].idx_ref);
                    dvs_expr_t eqv  = expr_binary(P, DVS_BIN_EQ,
                                      expr_var(P, fe->areads[si].read_varid),
                                      expr_var(P, fe->areads[sj].read_varid));
                    dvs_expr_t lem = (cond == EXPR_NULL || eqv == EXPR_NULL)
                                  ? EXPR_NULL
                                  : expr_binary(P, DVS_BIN_OR,
                                        expr_unary(P, DVS_UN_NOT, cond), eqv);
                    if (lem == EXPR_NULL) { free(ce); free(lems); return -1; }
                    if (nlem == cap) {
                        cap = cap ? cap * 2 : 64;
                        dvs_expr_t *g = (dvs_expr_t *)realloc(lems, cap * sizeof(dvs_expr_t));
                        if (!g) { free(ce); free(lems); return -1; }
                        lems = g;
                    }
                    lems[nlem++] = lem;
                }
            a = b;
        }
        free(ce);
    }

    /* (3) Array-equality consistency: for a true equality, every index read on
     *     EITHER side must read equal on both sides (p -> read(A,i)==read(B,i)).
     *     Symmetric: iterate reads on A and on B, minting the matching read on
     *     the other side. */
    for (uint32_t e = 0; e < fe->n_aeqs; e++) {
        if (!(int)_abs_mval_var(fe, fe->aeqs[e].p_varid)) continue;
        Smt2ArrayValue *A = fe->aeqs[e].a, *B = fe->aeqs[e].b;
        uint16_t dw = A->sort.data_width;
        uint32_t n_now = fe->n_areads;   /* don't chase reads added this pass */
        for (uint32_t i = 0; i < n_now; i++) {
            Smt2ArrayValue *side = fe->areads[i].node;
            if (side != A && side != B) continue;
            Smt2ArrayValue *other = (side == A) ? B : A;
            dvs_expr_t  bidx = fe->areads[i].idx_ref;
            uint32_t bidxv = fe->areads[i].idx_varid;
            int64_t va = _abs_mval_var(fe, fe->areads[i].read_varid);
            /* Read `other` at the same index. Only read its model if it already
             * exists (a freshly minted read has no model value yet). */
            uint32_t rO = _abs_find_read(fe, other, bidx, bidxv);
            if (rO != UINT32_MAX && _abs_mval_var(fe, rO) == va) continue;
            if (rO == UINT32_MAX) {
                rO = _abs_read_inplace(fe, other, bidx, bidxv, dw);
                if (rO == UINT32_MAX) { free(lems); return -1; }
            }
            dvs_expr_t eqv = expr_binary(P, DVS_BIN_EQ,
                              expr_var(P, fe->areads[i].read_varid),
                              expr_var(P, rO));
            _LEM_PUSH(expr_binary(P, DVS_BIN_OR,
                          expr_unary(P, DVS_UN_NOT, expr_var(P, fe->aeqs[e].p_varid)),
                          eqv));
        }
    }

    /* --- Phase B: assert everything (model no longer read past here) --- */
    for (uint32_t i = 0; i < nlem; i++) {
        if (_abs_assert(fe, lems[i]) < 0) { free(lems); return -1; }
    }
    int added = (int)nlem;
    free(lems);
    #undef _LEM_PUSH
    return added;
}

static void _free_bb(Smt2Frontend *fe);

/* Run the refinement loop on the solver's raw model to a fixpoint: it ends
 * only on a model the array theory accepts. */
static int _array_refine_loop(Smt2Frontend *fe, int rc) {
    while (rc == DVS_BB_SAT) {
        int added = _array_refine(fe);
        if (added < 0) return DVS_BB_UNKNOWN;
        if (added == 0) break;                     /* model satisfies the theory */
        rc = dvs_bbsolver_resolve_raw(fe->bb_solver);
    }
    return rc;
}

/* A seeded solve: draw a random model of the refined instance. Re-solve under
 * a random cube -- random polarities on a random subset of the variables' bit
 * literals -- halving the cube while it is unsat or over budget, and refine
 * each cube's model to the array theory as usual. Assumptions retract between
 * solves and lemmas are theory-valid, so whatever cube is drawn the answer is
 * sound; with every cube refused the converged raw model stands. */
#define SMT2_ARRAY_CUBE_MAX      32
#define SMT2_ARRAY_CUBE_CONFLICTS 2000
static int _array_diversify(Smt2Frontend *fe, uint64_t seed) {
    uint32_t cap = 4096;
    int32_t *pool = (int32_t *)malloc(cap * sizeof(int32_t));
    if (!pool) return DVS_BB_SAT;
    uint32_t n = dvs_bbsolver_split_lits(fe->bb_solver, pool, cap);
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
    #define _RND() (x ^= x << 13, x ^= x >> 7, x ^= x << 17, x)
    int32_t cube[SMT2_ARRAY_CUBE_MAX];
    uint32_t k = n < SMT2_ARRAY_CUBE_MAX ? n : SMT2_ARRAY_CUBE_MAX;
    int rc = DVS_BB_SAT;
    while (k > 0) {
        /* k distinct literals (partial Fisher-Yates), random polarities. */
        for (uint32_t i = 0; i < k; i++) {
            uint32_t j = i + (uint32_t)(_RND() % (n - i));
            int32_t t = pool[i]; pool[i] = pool[j]; pool[j] = t;
            cube[i] = (_RND() & 1) ? pool[i] : -pool[i];
        }
        int crc = dvs_bbsolver_solve_assuming(fe->bb_solver, cube, k,
                                              SMT2_ARRAY_CUBE_CONFLICTS);
        while (crc == DVS_BB_SAT) {
            int added = _array_refine(fe);
            if (added < 0) { free(pool); return DVS_BB_UNKNOWN; }
            if (added == 0) {
                /* Accepted. Also re-randomize its don't-care bits (a var only
                 * array reads constrain, like an index, is otherwise left at
                 * the solver's phase); keep that only if the theory accepts. */
                if (dvs_bbsolver_rediversify(fe->bb_solver, seed) == 0) {
                    added = _array_refine(fe);
                    if (added < 0) { free(pool); return DVS_BB_UNKNOWN; }
                    if (added > 0) {
                        crc = dvs_bbsolver_solve_assuming(fe->bb_solver, cube, k,
                                                          SMT2_ARRAY_CUBE_CONFLICTS);
                        continue;
                    }
                }
                free(pool);
                return DVS_BB_SAT;
            }
            crc = dvs_bbsolver_solve_assuming(fe->bb_solver, cube, k,
                                              SMT2_ARRAY_CUBE_CONFLICTS);
        }
        if (crc == DVS_BB_ERROR) break;
        k /= 2;
    }
    #undef _RND
    free(pool);
    /* The solver's model is a refused cube's: converge the raw model again. */
    rc = _array_refine_loop(fe, dvs_bbsolver_resolve_raw(fe->bb_solver));
    return rc;
}

/* Lazy array engine driver. Mirrors _check_sat_bitblast's result reporting.
 *
 * Every check-sat solves the full assertion set from the builder, which the
 * caller guarantees holds it: lemmas from an earlier check are theory-valid
 * but live in the old problem, so the refinement rediscovers what it needs. */
static int _check_sat_array(Smt2Frontend *fe) {
    _free_bb(fe);
    if (fe->problem)   { free(fe->problem); fe->problem = NULL; }
    _abs_drop_solve_state(fe);
    fe->cache_valid = 0;

    /* The refinement compares index and data model values as int64. */
    for (uint32_t i = 0; i < fe->n_anodes; i++) {
        if (fe->anodes[i]->sort.addr_width > 64 || fe->anodes[i]->sort.data_width > 64) {
            if (fe->print_stats || getenv("DV_LOG"))
                fprintf(fe->err, "cdcl-unknown: abstract array wider than 64 bits\n");
            SMT2_EMIT_UNKNOWN(fe); fflush(fe->out);
            fe->last_result = DVS_SOLVE_TIMEOUT; fe->has_result = 1;
            return 0;
        }
    }

    /* Finalize the BV skeleton with pool headroom for in-place lemmas + reads.
     * The builder keeps every assertion, for a later check or a pop. */
    uint32_t vu = dvs_builder_virtual_used(fe->builder);
    uint32_t reserve = vu > (32u << 20) ? vu : (32u << 20);   /* >= 32 MB slack */
    fe->problem = _explicit(dvs_builder_finalize_reserve(fe->builder, &fe->problem_size, reserve));
    if (!fe->problem) { SMT2_EMIT_UNKNOWN(fe); fflush(fe->out); return -1; }
    fe->problem_dirty = 0;
    fe->has_aux = 0;
    if (!fe->compiled) fe->builder_retained = 1;

    /* Incremental CaDiCaL backend is required for assert + resolve. */
    fe->bb_solver = dvs_bbsolver_new_backend(NULL, fe->problem, /*cadical=*/1);
    fe->bb_over_problem = 1;
    if (!fe->bb_solver || !dvs_bbsolver_is_incremental(fe->bb_solver)) {
        SMT2_EMIT_UNKNOWN(fe); fflush(fe->out);
        return -1;
    }

    int rc = dvs_bbsolver_prepare(fe->bb_solver, fe->seed);
    if (rc == DVS_BB_ENCODE_READY) {
        /* Refine on the raw model first: diversification randomizes don't-care
         * bits, which include the lazily-unconstrained read vars, so refining
         * diversified models costs many more rounds. Then, for a seeded solve,
         * draw a diversified model and refine that until the theory accepts
         * it, so repeated randomizes differ. */
        if (_array_emit_extensionality(fe) < 0) rc = DVS_BB_UNKNOWN;
        else rc = _array_refine_loop(fe, dvs_bbsolver_resolve_raw(fe->bb_solver));
        if (rc == DVS_BB_SAT && fe->seed) rc = _array_diversify(fe, fe->seed);
    }

    fe->bb_model_valid = (rc == DVS_BB_SAT);
    if (rc == DVS_BB_SAT) {
        fprintf(fe->out, "sat\n");
        fe->last_result = DVS_SOLVE_OK; fe->has_result = 1;
    } else if (rc == DVS_BB_UNSAT) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = DVS_SOLVE_UNSAT; fe->has_result = 1;
    } else {
        SMT2_EMIT_UNKNOWN(fe);
        fe->last_result = DVS_SOLVE_TIMEOUT; fe->has_result = 1;
    }
    fflush(fe->out);
    return rc == DVS_BB_ERROR ? -1 : 0;
}

/* Phase B.0: bit-blast + kissat engine. Bypasses dvs_solver_solve() and runs
 * directly on fe->problem. Triggered by DV_ENGINE=bitblast (set by the
 * --engine=bitblast CLI flag). The bbsolver is kept alive on fe->bb_solver
 * so that subsequent (get-value) calls can read back model values. */
/* Free the bit-blast solver and the problem it was built from, if owned. */
static void _free_bb(Smt2Frontend *fe) {
    if (fe->bb_solver)  { dvs_bbsolver_free(fe->bb_solver); fe->bb_solver = NULL; }
    if (fe->bb_problem) { free(fe->bb_problem); fe->bb_problem = NULL; }
    fe->bb_over_problem = 0;
    fe->bb_inc = 0;
}

/* Solve `p` with the bit-blast engine. `owned`, if non-NULL, is `p` handed
 * over by the caller: it becomes fe->bb_problem when a new bb_solver is built
 * from it, and is freed otherwise. */
static int _check_sat_bitblast_on(Smt2Frontend *fe, dvs_problem_t *p,
                                  dvs_problem_t *owned);
static int _check_sat_bitblast_body(Smt2Frontend *fe, dvs_problem_t *p,
                                    dvs_problem_t **owned_io);

static int _check_sat_bitblast(Smt2Frontend *fe) {
    return _check_sat_bitblast_on(fe, fe->problem, NULL);
}

static int _check_sat_bitblast_on(Smt2Frontend *fe, dvs_problem_t *p,
                                  dvs_problem_t *owned) {
    int ret = _check_sat_bitblast_body(fe, p, &owned);
    free(owned);                            /* NULL once adopted as bb_problem */
    return ret;
}

static int _check_sat_bitblast_body(Smt2Frontend *fe, dvs_problem_t *p,
                                    dvs_problem_t **owned_io) {
    dvs_problem_t *owned = *owned_io;
    if (!p) {
        SMT2_EMIT_UNKNOWN(fe);
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
    uint64_t fp = fe->verilator_mode ? _problem_fingerprint(p) : 0;
    if (fe->verilator_mode && fe->cache_valid && fp == fe->cached_fp) {
        int force_resolve = fe->reseed_period &&
                            (fe->div_counter % fe->reseed_period == 0);
        if (!force_resolve) {
            if (fe->cached_result == 0) {              /* cached UNSAT */
                fprintf(fe->out, "unsat\n");
                fflush(fe->out);
                fe->last_result = DVS_SOLVE_UNSAT;
                fe->has_result = 1;
                return 0;
            }
            if (fe->bb_solver &&
                dvs_bbsolver_rediversify(fe->bb_solver, fe->seed) == 0) {
                fe->bb_model_valid = 1;
                fprintf(fe->out, "sat\n");
                fflush(fe->out);
                fe->last_result = DVS_SOLVE_OK;
                fe->has_result = 1;
                return 0;
            }
        }
    }

    /* Cache miss (or forced re-solve): drop any prior bbsolver and solve fresh. */
    _free_bb(fe);
    fe->bb_solver  = dvs_bbsolver_new(NULL, p);
    fe->bb_problem = owned;                 /* lives exactly as long as bb_solver */
    *owned_io = NULL;
    if (!fe->bb_solver) {
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        fe->cache_valid = 0;
        return -1;
    }
    int rc = dvs_bbsolver_check(fe->bb_solver, fe->seed);
    fe->bb_model_valid = (rc == DVS_BB_SAT);
    if (fe->verilator_mode && (rc == DVS_BB_SAT || rc == DVS_BB_UNSAT)) {
        fe->cached_fp = fp;
        fe->cache_valid = 1;
        fe->cached_result = (rc == DVS_BB_SAT) ? 1 : 0;
    } else {
        fe->cache_valid = 0;                           /* unknown/error: never reuse */
    }
    if (rc == DVS_BB_SAT) {
        fprintf(fe->out, "sat\n");
        fe->last_result = DVS_SOLVE_OK;
        fe->has_result = 1;
    } else if (rc == DVS_BB_UNSAT) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
    } else {
        SMT2_EMIT_UNKNOWN(fe);
    }
    fflush(fe->out);
    /* The CDCL path returns 0 for both SAT and UNSAT (only protocol errors
     * are negative). Mirror that — UNSAT is a valid result, not an error. */
    return rc == DVS_BB_ERROR ? -1 : 0;
}

/* Does the builder hold every assertion in scope? After a CDCL compile it does
 * only while retention lasts (_start_cdcl_retention); otherwise unless it was
 * reset beside an unrebuildable fe->problem (_ensure_problem's -2). */
static int _builder_is_full(const Smt2Frontend *fe) {
    if (fe->ctx || fe->compiled) return fe->cdcl_retained;
    return !fe->problem || fe->builder_retained;
}

/* Verilator UniGen2 enumeration (vlt_enum): inside a push scope Verilator
 * re-solves after every blocking clause `(assert (not (and (= v c) ...)))`,
 * up to 61 times per cell. Solving each from scratch re-blasted the whole
 * accumulated set into a fresh SAT instance -- quadratic in the clauses, and
 * 10x Bitwuzla on riscv-dv. Instead keep one CaDiCaL instance per scope and
 * add only the constraints asserted since the last check-sat; it keeps its
 * learned clauses too. Pop, reset or any builder rewind drops the instance
 * (it cannot retract), and the next check-sat rebuilds it.
 *
 * No diversify pass: Verilator draws uniformly from the cell itself, so which
 * witness comes back first does not matter, and the flip-check was the other
 * big per-query cost.
 *
 * Returns 1 when it answered, 0 to leave the query to the regular path. */
static int _check_sat_enum(Smt2Frontend *fe) {
    dvs_builder_t *b = fe->builder;
    if (!b || fe->n_aux_problems || !_builder_is_full(fe)) return 0;

    size_t sz = 0;
    dvs_problem_t *np = _explicit(dvs_builder_finalize(b, &sz));
    if (!np) return 0;

    int rc = DVS_BB_UNKNOWN, done = 0;
    if (fe->bb_inc && fe->bb_solver && b->n_constraints > fe->bb_inc_ncons
            && b->n_softs == fe->bb_inc_nsofts && b->n_dists == fe->bb_inc_ndists
            && b->n_alldiffs == fe->bb_inc_nalldiffs) {
        /* Constraints are prepended, so the new ones lead the list, ending at
         * the head the instance already holds. */
        uint32_t k = b->n_constraints - fe->bb_inc_ncons;
        dvs_expr_t *roots = (dvs_expr_t *)malloc(k * sizeof(dvs_expr_t));
        dvs_expr_t cur = np->constraints_head;
        uint32_t i = 0;
        while (roots && i < k && cur != EXPR_NULL) {
            ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(np, cur);
            roots[i++] = cs->root;
            cur = cs->next;
        }
        if (roots && i == k && cur == fe->bb_inc_head
                && dvs_bbsolver_rebase(fe->bb_solver, np) == 0) {
            free(fe->bb_problem);
            fe->bb_problem = np;
            np = NULL;
            int arc = 0;
            for (uint32_t j = k; j-- > 0 && arc == 0; )   /* oldest first */
                arc = dvs_bbsolver_assert(fe->bb_solver, roots[j]);
            if (arc == 0) {
                rc = dvs_bbsolver_resolve(fe->bb_solver, fe->seed);
                done = 1;
            }
        }
        free(roots);
        if (!done) {
            /* Half-applied: the instance is unusable. Rebuild from scratch. */
            _free_bb(fe);
            if (!np) {
                np = _explicit(dvs_builder_finalize(b, &sz));
                if (!np) return 0;
            }
        }
    }
    if (!done) {
        _free_bb(fe);
        fe->bb_solver = dvs_bbsolver_new_backend(NULL, np, /*prefer_cadical=*/1);
        if (!fe->bb_solver) { free(np); return 0; }
        fe->bb_problem = np;
        dvs_bbsolver_set_diversify(fe->bb_solver, 0);
        rc = dvs_bbsolver_check(fe->bb_solver, fe->seed);
    }

    if ((rc == DVS_BB_SAT || rc == DVS_BB_UNSAT)
            && dvs_bbsolver_is_incremental(fe->bb_solver)) {
        fe->bb_inc           = 1;
        fe->bb_inc_head      = b->constraints_head;
        fe->bb_inc_ncons     = b->n_constraints;
        fe->bb_inc_nsofts    = b->n_softs;
        fe->bb_inc_ndists    = b->n_dists;
        fe->bb_inc_nalldiffs = b->n_alldiffs;
    } else {
        fe->bb_inc = 0;
    }
    fe->cache_valid = 0;            /* not a whole-problem identity-cache entry */
    fe->bb_model_valid = (rc == DVS_BB_SAT);
    if (rc == DVS_BB_SAT) {
        fprintf(fe->out, "sat\n");
        fe->last_result = DVS_SOLVE_OK;
        fe->has_result = 1;
    } else if (rc == DVS_BB_UNSAT) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
    } else {
        /* unknown/error: let the regular path have a go from scratch */
        _free_bb(fe);
        return 0;
    }
    fflush(fe->out);
    return 1;
}

/* True when the cube-and-conquer engine is explicitly selected
 * (DV_ENGINE=cube). Opt-in only: cube-and-conquer targets single hard QF_BV
 * instances, so it never auto-engages — the caller asks for it. */
static int _engine_is_cube(Smt2Frontend *fe) {
    (void)fe;
    const char *e = getenv("DV_ENGINE");
    return e && strcmp(e, "cube") == 0;
}

/* Cube-and-conquer engine (docs/cube_and_conquer_design.md). Like the bitblast
 * path it runs directly on fe->problem and keeps fe->bb_solver alive for
 * subsequent (get-value). Bit-blasts once, then partitions the search space
 * into cubes solved over the shared instance. Soundness contract is enforced
 * inside dvs_cube_check (SAT on any cube; UNSAT only on an exhaustive all-UNSAT
 * partition; otherwise unknown). No Verilator identity cache — cube is for
 * one-shot hard instances, not the re-randomize loop. */
static int _check_sat_cube(Smt2Frontend *fe) {
    if (!fe->problem) {
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        return -1;
    }
    _free_bb(fe);
    /* Prefer CaDiCaL: cube-and-conquer needs retractable assumptions. Falls
     * back to kissat (→ single-shot solve) when CaDiCaL is not compiled in. */
    fe->bb_solver = dvs_bbsolver_new_backend(NULL, fe->problem, /*prefer_cadical=*/1);
    if (!fe->bb_solver) {
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        return -1;
    }
    int rc = dvs_cube_check(fe->bb_solver, fe->seed);
    fe->bb_model_valid = (rc == DVS_BB_SAT);
    if (rc == DVS_BB_SAT) {
        fprintf(fe->out, "sat\n");
        fe->last_result = DVS_SOLVE_OK;
        fe->has_result = 1;
    } else if (rc == DVS_BB_UNSAT) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
    } else {
        SMT2_EMIT_UNKNOWN(fe);
    }
    fflush(fe->out);
    return rc == DVS_BB_ERROR ? -1 : 0;
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
    /* Audit knob: force CDCL for everything, ignoring needs_bitblast and the
     * logic-based auto-route. Used to verify CDCL is correct on its own (bitblast
     * as a pure performance optimizer, not a correctness crutch). Under this
     * knob a CantCompile construct should yield `unknown` or a correct answer —
     * never a wrong sat/unsat. */
    if (getenv("DV_NO_BITBLAST")) return 0;
    /* A problem that lowered a CDCL-unsound construct (e.g. signed div/rem)
     * MUST bitblast — even under an explicit DV_ENGINE=cdcl, since CDCL would
     * return a wrong model here. This overrides the env, deliberately. */
    if (fe->needs_bitblast) return 1;
    if (fe->shape_bb) return 1;   /* this check-sat's shape belongs to bitblast */
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
 * engine; otherwise fall back to the CDCL dvs_solver_get_value path. */
static int64_t _fe_get_var_value(Smt2Frontend *fe, uint32_t var_id) {
    if (fe->bb_solver && fe->bb_model_valid) {
        int64_t v = 0;
        if (dvs_bbsolver_value(fe->bb_solver, var_id, &v) == 0) return v;
        /* fall through to CDCL on bbsolver miss */
    }
    return dvs_solver_get_value(fe->ctx, var_id);
}

/* Read back the full (possibly >64-bit) model value of `var_id` into
 * little-endian 64-bit limbs. Only meaningful on the bitblast engine, which is
 * the only path that solves wide (>64-bit) variables; returns 0 on success. */
static int _fe_get_var_value_wide(Smt2Frontend *fe, uint32_t var_id,
                                  uint64_t *limbs, uint32_t n_limbs) {
    for (uint32_t i = 0; i < n_limbs; i++) limbs[i] = 0;
    if (fe->bb_solver && fe->bb_model_valid &&
        dvs_bbsolver_value_wide(fe->bb_solver, var_id, limbs, n_limbs) == 0)
        return 0;
    /* Fallback: the low 64 bits from the scalar reader (wide vars never reach
     * the CDCL engine, so this is only hit if the bbsolver lookup missed). */
    limbs[0] = (uint64_t)_fe_get_var_value(fe, var_id);
    return 0;
}

/* Emit a `#b…` binary literal for a wide value carried in little-endian limbs
 * (limbs[0] = bits [0,63]). Mirrors _emit_bv_bin_literal but spans all bits. */
static void _emit_bv_bin_literal_wide(FILE *out, const uint64_t *limbs,
                                      uint32_t n_limbs, unsigned width) {
    if (width == 0) width = 1;
    fputs("#b", out);
    for (int i = (int)width - 1; i >= 0; i--) {
        unsigned limb = (unsigned)(i / 64);
        unsigned bit = (limb < n_limbs)
            ? (unsigned)((limbs[limb] >> (i % 64)) & 1u) : 0u;
        fputc(bit ? '1' : '0', out);
    }
}

/* Close the checkpoint the last check-sat-assuming left open (assump_cp1).
 * Its pins must not outlive the answer: a reset inside a checkpoint scope
 * re-establishes the scope's pins, so a later (check-sat-assuming ((not p)))
 * found p still pinned by an earlier (check-sat-assuming (p)) -- a wrong
 * `unsat` -- and a later push numbered its checkpoint past the leaked one. */
static void _end_assumptions(Smt2Frontend *fe) {
    if (!fe->assump_cp1) return;
    uint32_t cp = fe->assump_cp1 - 1;
    fe->assump_cp1 = 0;
    if (fe->ctx) dvs_solver_restore(fe->ctx, cp);
}

static int _cmd_check_sat_body(Smt2Frontend *fe, const Sexpr *cmd);

/* Per-shape engine choice (Verilator mode). The exact-instance probe in
 * check-sat keys on the problem fingerprint, but many randomize() call sites
 * inline the object's non-rand state as constants, so every call is a new
 * instance and the probe never settles anything. The shape -- the declared
 * variable names -- is the same on every call, so learn the engine there.
 *
 * Neither engine is uniformly cheaper. riscv_illegal_instr (~6000 search
 * variables from bit-sliced instr_bin) costs CDCL 10-50 ms where bitblast
 * needs ~1 ms; riscv_loop_instr's divisibility test costs CDCL ~2.5 ms where
 * bitblast, building a 32-bit divider, needs ~6 ms. So measure: once CDCL
 * averages over SHAPE_SLOW_US on a shape, send the next SHAPE_TRIALS
 * instances to bitblast, and keep bitblast only if it was at least twice as
 * fast; otherwise CDCL is confirmed and the shape is not measured again.
 * Wall time, like the probe's budget, only picks which engine answers; both
 * are exact, so no verdict depends on it. */
#define SHAPE_TAB_N    256u
#define SHAPE_SLOW_US  3000u
#define SHAPE_TRIALS   2u
enum { SHAPE_LEARN = 0, SHAPE_CDCL = 1, SHAPE_BITBLAST = 2 };
typedef struct {
    uint64_t key;
    uint64_t cdcl_us, bb_us;
    uint16_t cdcl_n, bb_n;
    uint8_t  state;
} Smt2ShapeCost;

static Smt2ShapeCost *_shape_slot(Smt2Frontend *fe) {
    if (!fe->shape_fp) return NULL;
    if (!fe->shape_tab) {
        fe->shape_tab = calloc(SHAPE_TAB_N, sizeof(Smt2ShapeCost));
        if (!fe->shape_tab) return NULL;
    }
    Smt2ShapeCost *e = &((Smt2ShapeCost *)fe->shape_tab)[fe->shape_fp % SHAPE_TAB_N];
    if (e->key != fe->shape_fp) { memset(e, 0, sizeof *e); e->key = fe->shape_fp; }
    return e;
}

/* Route this check-sat to bitblast for its shape: decided, or on trial. */
static int _shape_wants_bitblast(Smt2Frontend *fe) {
    Smt2ShapeCost *e = _shape_slot(fe);
    if (!e) return 0;
    if (e->state == SHAPE_BITBLAST) return 1;
    return e->state == SHAPE_LEARN && e->cdcl_n >= 2
        && e->cdcl_us > (uint64_t)SHAPE_SLOW_US * e->cdcl_n && e->bb_n < SHAPE_TRIALS;
}

/* After a check-sat: account its time to the engine the shape routing chose
 * (fe->shape_route: 1 CDCL, 2 bitblast) and settle the shape when it can. */
static void _shape_record(Smt2Frontend *fe, long us) {
    Smt2ShapeCost *e = fe->shape_route ? _shape_slot(fe) : NULL;
    if (!e || e->state != SHAPE_LEARN || us < 0) return;
    if (fe->shape_route == 1 && e->cdcl_n < 0xFFFF) { e->cdcl_n++; e->cdcl_us += (uint64_t)us; }
    if (fe->shape_route == 2) { e->bb_n++; e->bb_us += (uint64_t)us; }
    if (e->bb_n >= SHAPE_TRIALS)
        e->state = e->bb_us * 2 * e->cdcl_n < e->cdcl_us * e->bb_n ? SHAPE_BITBLAST : SHAPE_CDCL;
}

/* DV_LOG in Verilator mode: one `vlt-time <us> <engine>/<why>` line per
 * check-sat, so solve time can be attributed to the routing decision. */
static int _cmd_check_sat(Smt2Frontend *fe, const Sexpr *cmd) {
    if (!fe->verilator_mode) return _cmd_check_sat_body(fe, cmd);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    fe->vlt_route = "other";
    fe->cdcl_ticks = 0;
    fe->shape_route = 0;
    fe->shape_bb = 0;
    int rc = _cmd_check_sat_body(fe, cmd);
    fe->shape_bb = 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (long)((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000);
    _shape_record(fe, us);
    if (!getenv("DV_LOG")) return rc;
    fprintf(fe->err, "vlt-time %ld %s vars=%u props=%u conflicts=%llu ticks=%u\n",
            us, fe->vlt_route, fe->ctx ? fe->ctx->n_vars : 0u, fe->ctx ? fe->ctx->n_props : 0u,
            fe->ctx ? (unsigned long long)fe->ctx->conflict_count : 0ull, fe->cdcl_ticks);
    return rc;
}

static int _cmd_check_sat_body(Smt2Frontend *fe, const Sexpr *cmd) {
    /* Only skipped parity asserts since the last `sat`: the model stands. */
    if (fe->vlt_hash_pending) {
        fe->vlt_hash_pending = 0;
        fe->vlt_route = "hash-skip";
        if (fe->has_result && fe->last_result == DVS_SOLVE_OK) {
            fe->model_reused = 1;
            fprintf(fe->out, "sat\n");
            fflush(fe->out);
            return 0;
        }
    }
    _end_assumptions(fe);
    (void)cmd;
    fe->core_hist_at_check = fe->n_core_hist;
    fe->core_replayable    = 1;
    /* A previous answer's bit-blast model is not this answer's (B36). */
    fe->bb_model_valid = 0;

    /* Tainted context (an assert used an unsupported construct): answer honestly
     * with `unknown` rather than solve a problem that is missing constraints. */
    if (fe->incomplete) {
        if (fe->print_stats || getenv("DV_LOG"))
            fprintf(fe->err, "cdcl-unknown: tainted at %s:%u -- %s\n",
                    "smt2_frontend.c", fe->incomplete_line,
                    fe->incomplete_why ? fe->incomplete_why : "reason not recorded");
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_TIMEOUT;
        fe->has_result = 1;
        return 0;
    }

    /* In Verilator mode every solve draws a fresh seed so the returned model
     * varies run-to-run (that is where randomization diversity comes from --
     * the following check-sat-assuming reuses this model rather than re-solving,
     * so each randomize() costs a single solve). A seed the driver set for this
     * transaction wins, offset by one because seed 0 means "no diversity". */
    if (fe->verilator_mode)
        fe->seed = fe->seed_given ? fe->given_seed + 1 : ++fe->div_counter;

    /* Abstract arrays: the lazy engine's own finalize + lemmas-on-demand
     * refinement, from the builder, on every check-sat. Any other path would
     * solve the skeleton with the reads as free vars and no array lemmas -- a
     * spurious `sat` (B70). A CDCL context compiled before the first array
     * owns fe->problem; unless the builder was retained beside it, the full
     * assertion set is gone, so answer `unknown`. */
    if (fe->n_anodes > 0 && (fe->n_areads > 0 || fe->n_aeqs > 0)
        && (!fe->array_eager || fe->problem || fe->compiled)) {
        /* (DV_ARRAY=eager emits its axioms once, before the first finalize.) */
        if (!fe->array_eager && !fe->compiled && (!fe->problem || fe->builder_retained)) {
            fe->vlt_route = "bitblast/array";
            return _check_sat_array(fe);
        }
        if (fe->print_stats || getenv("DV_LOG"))
            fprintf(fe->err, "cdcl-unknown: abstract arrays after a CDCL compile\n");
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_TIMEOUT;
        fe->has_result = 1;
        return 0;
    }

    /* DV_ARRAY=eager: emit the one-shot Ackermann axioms into the builder before
     * the first finalize (correctness oracle). */
    if (fe->array_eager && !fe->problem && !fe->compiled) {
        if (_emit_array_axioms(fe) < 0) {
            SMT2_EMIT_UNKNOWN(fe);
            fflush(fe->out);
            fe->last_result = DVS_SOLVE_TIMEOUT;
            fe->has_result = 1;
            return 0;
        }
    }

    /* Bit-blast path: finalize the problem but skip the unused CDCL context
     * build. The bit-blast engine reads fe->problem directly and never touches
     * fe->ctx, so compiling first is pure waste — and worse, it can report a
     * spurious compile-time UNSAT for constructs the native CDCL compile lowers
     * unsoundly (e.g. `r == sign_extend(a)` into a wide unsigned var, which the
     * compile bounds with a negative lb → empty domain). Skipping the compile
     * lets bit-blast answer these correctly. (Previously gated on
     * verilator_mode; now applies to any bit-blast-routed solve.) */
    /* ---- Verilator randomization routing (see Smt2Frontend.cdcl_route) ----
     * Prefer CDCL: it samples near-uniformly (statistically indistinguishable
     * from a true uniform RNG on every shaped benchmark measured), whereas the
     * bitblast path re-diversifies ONE cached model by flipping bits, which
     * concentrates mass on boundary values (~50% of draws on a single solution
     * for an OpenTitan-style range constraint). Soundness is unaffected either
     * way: CDCL is correct-or-`unknown` and an `unknown` escalates to bitblast
     * further down, so routing can only change WHICH engine answers, never the
     * verdict.
     *
     * Probe once per constraint set and remember the outcome. The randomize()
     * loop re-solves an identical instance thousands of times, so an instance
     * CDCL cannot handle must cost one bounded probe, not a probe per call. */
    int cdcl_probing = 0;
    int shape_slow = fe->verilator_mode && !getenv("DV_ENGINE") && _shape_wants_bitblast(fe);
    if (fe->verilator_mode && fe->verilator_cdcl && !fe->needs_bitblast && !fe->vlt_enum
            && !shape_slow
            && !fe->array_lazy && !fe->array_eager && !_engine_is_cube(fe)
            && !getenv("DV_ENGINE")) {
        /* After a compile plus later asserts (e.g. under push) the full set
         * cannot be re-finalized; key on the compiled base problem then,
         * rather than reuse a route decided for some other instance. */
        if ((_ensure_problem(fe) == 0 || fe->compiled) && fe->problem) {
            uint64_t rfp = _problem_fingerprint(fe->problem);
            if (fe->cdcl_route != 0 && fe->cdcl_probe_fp == rfp) {
                /* Decision already made for this exact instance. */
                cdcl_probing = 0;
            } else {
                /* New (or changed) instance: probe CDCL under a bounded budget. */
                fe->cdcl_probe_fp = rfp;
                fe->cdcl_route    = 0;
                cdcl_probing      = 1;
            }
        }
    }
    /* cdcl_route survives (reset) and may have been decided for a different
     * instance (e.g. Verilator's start-up handshake), so it never overrides a
     * problem that must bitblast: that forced a >64-bit problem onto CDCL, whose
     * compile declined it -> `unknown` for every riscv-dv cfg.randomize(). */
    int route_cdcl = cdcl_probing || (fe->verilator_mode && fe->verilator_cdcl
                                      && !fe->needs_bitblast && !fe->vlt_enum
                                      && !shape_slow && fe->cdcl_route == 1);
    fe->shape_bb    = shape_slow;
    fe->shape_route = route_cdcl ? 1 : shape_slow ? 2 : 0;

    /* Verilator routing trace: which engine this check-sat starts on, and why.
     * (A CDCL `unknown` may still escalate to bitblast further down.) */
    if (fe->verilator_mode && getenv("DV_LOG")) {
        const char *eng = route_cdcl ? "cdcl" : _engine_is_bitblast(fe) ? "bitblast" : "cdcl";
        const char *why = cdcl_probing                ? "probe"
                        : route_cdcl                  ? "sticky"
                        : fe->needs_bitblast          ? (fe->nb_why ? fe->nb_why : "needs-bitblast")
                        : fe->vlt_enum                ? "enumeration"
                        : !fe->verilator_cdcl         ? "opt-out"
                        : shape_slow                  ? "cdcl-slow"
                        : fe->cdcl_route == 2         ? "cdcl-declined"
                        :                               "logic";
        fprintf(fe->err, "vlt-route: %s (%s)\n", eng, why);
        static char rbuf[64];
        snprintf(rbuf, sizeof rbuf, "%s/%s", eng, why);
        fe->vlt_route = rbuf;
    }

    if (_engine_is_cube(fe)) {
        int prc = _ensure_problem(fe);
        if (prc < 0) {
            /* -2: stale problem we cannot safely rebuild -> honest `unknown`,
             * not an error, so the driver's protocol stream stays in sync. */
            if (prc == -2) SMT2_TAINT(fe, "stale problem could not be safely rebuilt");
            SMT2_EMIT_UNKNOWN(fe);
            fflush(fe->out);
            fe->last_result = DVS_SOLVE_TIMEOUT;
            fe->has_result  = 1;
            return prc == -2 ? 0 : -1;
        }
        return _check_sat_cube(fe);
    }

    if (_engine_is_bitblast(fe) && !route_cdcl) {
        if (fe->verilator_mode && fe->vlt_enum && !getenv("DV_VLT_ENUM_SCRATCH")
                && _check_sat_enum(fe))
            return 0;
        int prc = _ensure_problem(fe);
        if (prc == -2 && fe->cdcl_retained) {
            /* fe->problem predates asserts made after a CDCL compile, and
             * belongs to the live ctx so it can't be rebuilt in place. The
             * retained builder holds every assertion (including any not yet
             * handed to CDCL): solve a full copy of it instead. */
            size_t full_sz = 0;
            dvs_problem_t *full = _explicit(dvs_builder_finalize(fe->builder, &full_sz));
            if (full) return _check_sat_bitblast_on(fe, full, full);
        }
        if (prc < 0) {
            /* -2: stale problem we cannot safely rebuild -> honest `unknown`,
             * not an error, so the driver's protocol stream stays in sync. */
            if (prc == -2) SMT2_TAINT(fe, "stale problem could not be safely rebuilt");
            SMT2_EMIT_UNKNOWN(fe);
            fflush(fe->out);
            fe->last_result = DVS_SOLVE_TIMEOUT;
            fe->has_result  = 1;
            return prc == -2 ? 0 : -1;
        }
        return _check_sat_bitblast(fe);
    }

    int crc = _ensure_compiled(fe);
    if (crc == -2) {
        /* UNSAT detected at compile time (propagation contradiction). */
        fprintf(fe->out, "unsat\n");
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
    }
    if (crc < 0 && fe->problem && !getenv("DV_NO_BITBLAST")) {
        /* CDCL could not build a context; the bit-blaster still can. */
        if (fe->print_stats || getenv("DV_LOG"))
            fprintf(fe->err, "cdcl-compile-failed (rc=%d): escalating to bitblast\n", crc);
        fe->vlt_route = "bitblast/compile-failed";
        if (cdcl_probing) fe->cdcl_route = 2;
        return _check_sat_bitblast(fe);
    }
    if (crc < 0) {
        if (fe->print_stats || getenv("DV_LOG"))
            fprintf(fe->err, "cdcl-unknown: dvs_solver_compile failed (rc=%d) -- the "
                             "CDCL compile could not build a context for this "
                             "problem\n", crc);
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        return -1;
    }
    /* crc > 0 (some constraints uncompiled) is SOUND without special handling:
     * a CDCL `unsat` is monotone-correct (dropping constraints only loosens the
     * problem), and a CDCL `sat` is re-checked by the model-validation net below
     * against the FULL fe->problem (incl. the dropped constraints), downgrading
     * to unknown on any violation. So the only remaining CDCL soundness risk is
     * a wrong `unsat`, which validation cannot catch — those are fixed at the
     * source (e.g. the sign_extend compile no longer conflicts; see dvs_compile). */

    if (_engine_is_bitblast(fe) && !route_cdcl) {
        return _check_sat_bitblast(fe);
    }

    if (fe->has_result) dvs_solver_reset(fe->ctx);

    int frc = _flush_aux(fe);
    if (frc == -2) {
        fprintf(fe->out, "unsat\n");
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
    }
    if (frc < 0) {
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        return -1;
    }
    if (fe->ctx->pool.overflow && !getenv("DV_NO_BITBLAST")) {
        /* Part of the asserted set never reached the context (see _flush_aux):
         * solve the full retained set on bitblast instead. */
        if (fe->print_stats || getenv("DV_LOG"))
            fprintf(fe->err, "cdcl-pool-overflow: escalating to bitblast\n");
        fe->vlt_route = "bitblast/pool-overflow";
        if (fe->cdcl_retained && !fe->has_aux) {
            size_t full_sz = 0;
            dvs_problem_t *full = _explicit(dvs_builder_finalize(fe->builder, &full_sz));
            if (full) return _check_sat_bitblast_on(fe, full, full);
        }
        if (fe->n_aux_problems == 0) return _check_sat_bitblast(fe);
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_TIMEOUT;
        fe->has_result = 1;
        return 0;
    }

    dvs_solve_opts_t opts;
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

    /* Decision-variable tie-break. Default (0) keeps the fast lowest-index
     * tie-break; DV_FAIR_PICK=1 reservoir-samples uniformly among the
     * smallest-domain vars, which stops the same variable from always being
     * decided first and skewing every other variable's marginal (see
     * _select_unassigned in dvs_search.c). Costs a little speed, so it is
     * opt-in -- but it is a real sampling-quality lever, so it needs to be
     * reachable to be evaluated (tests/formal/sample_quality.py). */
    {
        const char *ev = getenv("DV_FAIR_PICK");
        if (ev && *ev == '1') opts.fair_pick = 1;
    }

    /* Verilator randomization: use the uniform tie-break. Without it the same
     * variable is decided first on every solve, so its marginal is uniform but
     * every other variable is sampled over a conditional (skewed) domain -- e.g.
     * under `a < b`, always deciding `a` first starves `b`'s low values. This is
     * the mode dvs_solve_opts_t.fair_pick documents as "the right mode for
     * constrained-random stimulus". */
    if (route_cdcl) opts.fair_pick = 1;

    /* Bound the CDCL-viability probe. Without this the first randomize() of a
     * constraint set CDCL cannot handle would stall for the full 10 s default
     * before escalating. Paid once per constraint set, then the sticky route
     * sends later calls straight to bitblast. 50 ms is ~25x the time CDCL needs
     * on every instance it can actually solve (measured 0.8-2 ms across the 37
     * captured Verilator transcripts), so the bound costs no coverage. */
    if (cdcl_probing) opts.time_limit_ms = 50;

    uint32_t ticks0 = fe->ctx->prop_ticks;
    fe->last_result = dvs_solver_solve(fe->ctx, &opts);
    fe->cdcl_ticks = fe->ctx->prop_ticks - ticks0;
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

    /* Sanity check: if we got DVS_SOLVE_OK, re-evaluate every top-level
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
    if (fe->last_result == DVS_SOLVE_OK && fe->problem) {
        const char *mode_env = getenv("DV_VALIDATE_MODEL");
        int mode = mode_env ? atoi(mode_env) : 2;
        if (mode > 0) {
            int viol = dvs_solver_validate_model(fe->ctx, fe->problem, fe->err);
            for (uint32_t i = 0; i < fe->n_aux_problems; i++) {
                viol += dvs_solver_validate_model(fe->ctx, fe->aux_problems[i], fe->err);
            }
            fe->last_validate_viol = viol;
            if (viol > 0) {
                fprintf(fe->err,
                    "model-validation: %d top-level constraint(s) violated%s\n",
                    viol, mode >= 2 ? "; downgrading sat -> unknown" : "");
                if (mode >= 2) fe->last_result = DVS_SOLVE_TIMEOUT;
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
    /* Diagnostic: a CDCL `unknown` used to be completely silent, so every gap
     * cost a manual delta-debug to learn whether the search ran out of time, ran
     * out of decision depth, or the constraint was never compiled at all. Report
     * it under DV_LOG / --stats. `crc` is the uncompiled-constraint count from
     * dvs_solver_compile (>0 means CDCL dropped constraints and is relying on the
     * validation net + escalation). */
    if (fe->last_result == DVS_SOLVE_TIMEOUT
            && (fe->print_stats || getenv("DV_LOG"))) {
        fprintf(fe->err,
                "cdcl-unknown: %s; uncompiled-constraints=%d; vars=%u; conflicts=%llu\n",
                dvs_solver_bail_reason_str(fe->ctx), crc,
                fe->ctx ? fe->ctx->n_vars : 0u,
                fe->ctx ? (unsigned long long)fe->ctx->conflict_count : 0ull);
    }

    /* Record the probe verdict for this instance (see cdcl_route). A definitive
     * CDCL answer means keep sampling with CDCL; anything else means this
     * constraint set belongs to bitblast and we should not probe again. */
    if (cdcl_probing) {
        fe->cdcl_route = (fe->last_result == DVS_SOLVE_OK
                          || fe->last_result == DVS_SOLVE_UNSAT) ? 1 : 2;
    }

    if (fe->last_result == DVS_SOLVE_TIMEOUT
        && !getenv("DV_NO_BITBLAST")) {   /* audit mode: keep pure-CDCL unknown */
        if (fe->vlt_route) {
            static char ebuf[80];
            snprintf(ebuf, sizeof ebuf, "%s->bitblast", fe->vlt_route);
            fe->vlt_route = ebuf;
        }
        if (fe->n_aux_problems == 0)
            return _check_sat_bitblast(fe);
        /* Constraints were asserted after the compile, so fe->problem is not
         * the whole set -- but the retained builder is (see cdcl_retained).
         * This is Verilator's random XOR-hash asserts after the first
         * check-sat: without it every such randomize() answered `unknown`. */
        if (fe->cdcl_retained && !fe->has_aux) {
            size_t full_sz = 0;
            dvs_problem_t *full = _explicit(dvs_builder_finalize(fe->builder, &full_sz));
            if (full) return _check_sat_bitblast_on(fe, full, full);
        }
    }

    switch (fe->last_result) {
    case DVS_SOLVE_OK:      fprintf(fe->out, "sat\n"); break;
    case DVS_SOLVE_UNSAT:   fprintf(fe->out, "unsat\n"); break;
    case DVS_SOLVE_TIMEOUT: SMT2_EMIT_UNKNOWN(fe); break;
    }
    fflush(fe->out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Array model output helper                                           */
/* ------------------------------------------------------------------ */

static void _emit_bv_bin_literal(FILE *out, uint64_t val, unsigned width);

/* Model value of element `k` of an abstract array's BASE node: the value of any
 * read on it whose index takes the value k (congruence makes them agree).
 * Returns 0 if no read lands on k -- the element is unconstrained. */
static int _abs_base_value(Smt2Frontend *fe, const Smt2ArrayValue *base,
                           uint64_t k, int64_t *out) {
    if (!fe->bb_solver || !fe->bb_model_valid || !fe->problem) return 0;
    uint16_t aw = base->sort.addr_width;
    uint64_t mask = aw >= 64 ? ~0ull : ((1ull << aw) - 1u);
    for (uint32_t i = 0; i < fe->n_areads; i++) {
        if (fe->areads[i].node != base) continue;
        if (((uint64_t)_abs_mval_ref(fe, fe->areads[i].idx_ref) & mask) != (k & mask))
            continue;
        *out = _abs_mval_var(fe, fe->areads[i].read_varid);
        return 1;
    }
    return 0;
}

/* A seeded-random value for an array element no constraint references: a DV
 * solver should randomize unconstrained elements, not return 0. */
static int64_t _free_elem_value(const Smt2Frontend *fe, uint64_t k, uint32_t name_len) {
    if (!fe->seed) return 0;
    uint64_t x = fe->seed
        + 0x9E3779B97F4A7C15ULL * (k + 1)
        + 0xD1B54A32D192ED03ULL * (name_len + 1);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return (int64_t)(x ^ (x >> 31));
}

/* (get-value (a)) on an abstract array: the default 0 with a store for every
 * index some read landed on. */
static void _emit_abs_array(Smt2Frontend *fe, Smt2ArrayVar *av) {
    const Smt2ArrayValue *base = av->value;
    uint8_t aw = av->sort.addr_width, dw = av->sort.data_width;
    uint64_t mask = aw >= 64 ? ~0ull : ((1ull << aw) - 1u);
    uint32_t n = 0;
    uint64_t *idx = (uint64_t *)malloc((fe->n_areads + 1) * sizeof(uint64_t));
    int64_t  *val = (int64_t *)malloc((fe->n_areads + 1) * sizeof(int64_t));
    if (idx && val && fe->bb_solver && fe->bb_model_valid && fe->problem) {
        for (uint32_t i = 0; i < fe->n_areads; i++) {
            if (fe->areads[i].node != base) continue;
            uint64_t k = (uint64_t)_abs_mval_ref(fe, fe->areads[i].idx_ref) & mask;
            uint32_t j = 0;
            while (j < n && idx[j] != k) j++;
            if (j < n) continue;
            idx[n] = k;
            val[n++] = _abs_mval_var(fe, fe->areads[i].read_varid);
        }
    }
    for (uint32_t i = 0; i < n; i++) fprintf(fe->out, "(store ");
    fprintf(fe->out, "((as const (Array (_ BitVec %u) (_ BitVec %u))) ",
            (unsigned)aw, (unsigned)dw);
    _emit_bv_bin_literal(fe->out, 0, (unsigned)dw);
    fprintf(fe->out, ")");
    for (uint32_t i = 0; i < n; i++) {
        fprintf(fe->out, " ");
        _emit_bv_bin_literal(fe->out, idx[i], (unsigned)aw);
        fprintf(fe->out, " ");
        _emit_bv_bin_literal(fe->out, (uint64_t)val[i], (unsigned)dw);
        fprintf(fe->out, ")");
    }
    free(idx);
    free(val);
}

static void _emit_hex_key(FILE *out, uint64_t lo, uint64_t hi, uint32_t width);

/* An element value of a sparse array, at its full data width. */
static void _emit_sparse_elem(Smt2Frontend *fe, uint32_t vid, unsigned dw) {
    if (dw > 64) {
        uint64_t limbs[SMT2_BV_MAX_LIMBS];
        uint32_t nl = (dw + 63u) / 64u;
        _fe_get_var_value_wide(fe, vid, limbs, nl);
        _emit_bv_bin_literal_wide(fe->out, limbs, nl, dw);
    } else {
        _emit_bv_bin_literal(fe->out, (uint64_t)_fe_get_var_value(fe, vid), dw);
    }
}

/* (get-value (a)) on a sparse array: the default 0 with a store for every
 * element some constraint made. A nested array is an outer store chain of
 * inner arrays, one per outer key used. */
static void _emit_sparse_array(Smt2Frontend *fe, Smt2ArrayVar *av) {
    const Smt2ArrayValue *v = av->value;
    unsigned dw = av->sort.data_width;
    uint32_t J = av->sort.inner_addr;
    uint32_t I = av->sort.addr_width - J;
    if (!J) {
        for (uint32_t i = 0; i < v->n_sparse; i++) fprintf(fe->out, "(store ");
        fprintf(fe->out, "((as const (Array (_ BitVec %u) (_ BitVec %u))) ",
                (unsigned)av->sort.addr_width, dw);
        _emit_bv_bin_literal(fe->out, v->has_default ? v->default_val : 0, dw);
        fprintf(fe->out, ")");
        for (uint32_t i = 0; i < v->n_sparse; i++) {
            fprintf(fe->out, " ");
            _emit_hex_key(fe->out, v->sparse_idx[i], v->sparse_idx_hi[i], av->sort.addr_width);
            fprintf(fe->out, " ");
            _emit_sparse_elem(fe, v->sparse_varid[i], dw);
            fprintf(fe->out, ")");
        }
        return;
    }
    /* Split each flat key into (outer, inner) and group by outer. */
    uint32_t n = v->n_sparse;
    uint64_t *olo = (uint64_t *)calloc(n + 1, sizeof(uint64_t));
    uint64_t *ohi = (uint64_t *)calloc(n + 1, sizeof(uint64_t));
    uint32_t n_outer = 0;
    uint64_t imask_lo = J >= 64 ? ~0ull : ((1ull << J) - 1u);
    uint64_t imask_hi = J > 64 ? (J >= 128 ? ~0ull : ((1ull << (J - 64)) - 1u)) : 0;
    #define _OUTER(i, lo, hi) do {                                              \
        uint64_t _l = v->sparse_idx[i], _h = v->sparse_idx_hi[i];              \
        if (J >= 64) { lo = J >= 128 ? 0 : _h >> (J - 64); hi = 0; }           \
        else { lo = (_l >> J) | (_h << (64 - J)); hi = _h >> J; }              \
    } while (0)
    for (uint32_t i = 0; olo && ohi && i < n; i++) {
        uint64_t lo, hi;
        _OUTER(i, lo, hi);
        uint32_t j = 0;
        while (j < n_outer && !(olo[j] == lo && ohi[j] == hi)) j++;
        if (j == n_outer) { olo[n_outer] = lo; ohi[n_outer++] = hi; }
    }
    for (uint32_t o = 0; o < n_outer; o++) fprintf(fe->out, "(store ");
    fprintf(fe->out, "((as const (Array (_ BitVec %u) (Array (_ BitVec %u) (_ BitVec %u)))) "
            "((as const (Array (_ BitVec %u) (_ BitVec %u))) ", I, J, dw, J, dw);
    _emit_bv_bin_literal(fe->out, 0, dw);
    fprintf(fe->out, "))");
    for (uint32_t o = 0; o < n_outer; o++) {
        fprintf(fe->out, " ");
        _emit_hex_key(fe->out, olo[o], ohi[o], I);
        fprintf(fe->out, " ");
        uint32_t cnt = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t lo, hi;
            _OUTER(i, lo, hi);
            if (lo == olo[o] && hi == ohi[o]) { fprintf(fe->out, "(store "); cnt++; }
        }
        fprintf(fe->out, "((as const (Array (_ BitVec %u) (_ BitVec %u))) ", J, dw);
        _emit_bv_bin_literal(fe->out, 0, dw);
        fprintf(fe->out, ")");
        for (uint32_t i = 0; i < n && cnt; i++) {
            uint64_t lo, hi;
            _OUTER(i, lo, hi);
            if (!(lo == olo[o] && hi == ohi[o])) continue;
            fprintf(fe->out, " ");
            _emit_hex_key(fe->out, v->sparse_idx[i] & imask_lo,
                          v->sparse_idx_hi[i] & imask_hi, J);
            fprintf(fe->out, " ");
            _emit_sparse_elem(fe, v->sparse_varid[i], dw);
            fprintf(fe->out, ")");
        }
        fprintf(fe->out, ")");
    }
    #undef _OUTER
    free(olo);
    free(ohi);
}

static void _emit_array_store_chain(Smt2Frontend *fe, Smt2ArrayVar *av) {
    if (av->value->is_abstract) { _emit_abs_array(fe, av); return; }
    if (av->value->is_sparse)   { _emit_sparse_array(fe, av); return; }
    uint32_t n = av->value->n_elems;
    uint8_t aw = av->sort.addr_width;
    uint8_t dw = av->sort.data_width;

    /* Use a larger buffer to avoid truncation: name (127) + "[1023]" (6) + nul */
    char elem_name[SMT2_MAX_NAME + 16];

    /* Pre-gather element values by looking up "name[i]" in the var table */
    int64_t *vals = (int64_t *)alloca(n * sizeof(int64_t));
    uint32_t *vids = (uint32_t *)alloca(n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        snprintf(elem_name, sizeof(elem_name), "%s[%u]", av->name, i);
        Smt2Var *vvar = _find_var(fe, elem_name, (uint32_t)strlen(elem_name));
        vals[i] = vvar ? _fe_get_var_value(fe, vvar->var_id) : 0;
        vids[i] = vvar ? vvar->var_id : UINT32_MAX;
    }

    if (dw > 64) {
        /* A wide element value does not fit `(_ bvN W)`'s int64 print: emit
         * each element as a full-width `#b…` literal instead. */
        uint64_t limbs[SMT2_BV_MAX_LIMBS];
        uint32_t nl = (dw + 63u) / 64u;
        for (uint32_t i = 1; i < n; i++) fprintf(fe->out, "(store ");
        fprintf(fe->out, "((as const (Array (_ BitVec %u) (_ BitVec %u))) ",
                (unsigned)aw, (unsigned)dw);
        for (uint32_t j = 0; j < nl; j++) limbs[j] = 0;
        if (vids[0] != UINT32_MAX) _fe_get_var_value_wide(fe, vids[0], limbs, nl);
        _emit_bv_bin_literal_wide(fe->out, limbs, nl, (unsigned)dw);
        fprintf(fe->out, ")");
        for (uint32_t i = 1; i < n; i++) {
            fprintf(fe->out, " (_ bv%u %u) ", i, (unsigned)aw);
            for (uint32_t j = 0; j < nl; j++) limbs[j] = 0;
            if (vids[i] != UINT32_MAX) _fe_get_var_value_wide(fe, vids[i], limbs, nl);
            _emit_bv_bin_literal_wide(fe->out, limbs, nl, (unsigned)dw);
            fprintf(fe->out, ")");
        }
        return;
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

/* The evaluator's value type: 128 bits where the compiler has them, so every
 * accepted width (SMT2_MAX_BV_BITS) folds exactly; otherwise 64 bits. A value
 * wider than EVAL_MAX_W is refused (*ok = 0), never truncated: get-value then
 * emits an honest error placeholder rather than a wrong value. */
#if defined(DVS_HAVE_INT128)
typedef unsigned __int128 EvalVal;
#define EVAL_MAX_W 128
#else
typedef uint64_t EvalVal;
#define EVAL_MAX_W 64
#endif

typedef struct { EvalVal value; uint16_t width; } EvalRet;

static EvalRet _eval_sexpr(Smt2Frontend *fe, const Sexpr *s, int *ok);

static EvalVal _trunc(EvalVal v, uint16_t w) {
    if (w == 0 || w >= EVAL_MAX_W) return v;
    return v & (((EvalVal)1 << w) - 1);
}

/* Little-endian limbs -> EvalVal (the caller has checked width <= EVAL_MAX_W). */
static EvalVal _eval_from_limbs(const uint64_t *limbs, uint32_t n_limbs) {
    EvalVal v = 0;
    for (uint32_t i = n_limbs; i-- > 0;) {
#if EVAL_MAX_W > 64
        v = (v << 64) | (EvalVal)limbs[i];
#else
        v = (EvalVal)limbs[i];
#endif
    }
    return v;
}

static EvalRet _eval_sexpr(Smt2Frontend *fe, const Sexpr *s, int *ok) {
    EvalRet r = { 0, 0 };
    if (!*ok || !s) { *ok = 0; return r; }

    if (s->kind == SEXPR_BITVEC) {
        if (s->bv.width > EVAL_MAX_W) { *ok = 0; return r; }
        if (s->bv.width > 64) {
            if (!s->bv.limbs) { *ok = 0; return r; }
            r.value = _eval_from_limbs(s->bv.limbs, (s->bv.width + 63u) / 64u);
        } else {
            r.value = s->bv.value;
        }
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
            if (v->width > EVAL_MAX_W) { *ok = 0; return r; }
            if (v->width > 64) {
                uint64_t limbs[SMT2_BV_MAX_LIMBS];
                uint32_t nl = (v->width + 63u) / 64u;
                _fe_get_var_value_wide(fe, v->var_id, limbs, nl);
                r.value = _trunc(_eval_from_limbs(limbs, nl), v->width);
            } else {
                int64_t val = _fe_get_var_value(fe, v->var_id);
                r.value = _trunc((EvalVal)(uint64_t)val, v->width);
            }
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
        uint64_t limbs[SMT2_BV_MAX_LIMBS];
        if (_parse_bv_sym_limbs(bv, limbs, SMT2_BV_MAX_LIMBS)) {
            uint64_t wn = s->list.items[2]->numval;
            if (wn > EVAL_MAX_W) { *ok = 0; return r; }  /* never fold truncated */
            r.width = (uint16_t)wn;
            r.value = _trunc(_eval_from_limbs(limbs, (EVAL_MAX_W + 63) / 64), r.width);
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
        uint64_t hi = head->list.items[2]->numval;
        uint64_t lo = head->list.items[3]->numval;
        EvalRet inner = _eval_sexpr(fe, s->list.items[1], ok);
        if (!*ok) return r;
        if (hi < lo || hi >= EVAL_MAX_W) { *ok = 0; return r; }
        r.width = (uint16_t)(hi - lo + 1);
        r.value = _trunc(inner.value >> lo, r.width);
        return r;
    }

    /* ((_ zero_extend n) x), ((_ sign_extend n) x): Verilator indexes an
     * array this way, and get-values the select. */
    if (head->kind == SEXPR_LIST && head->list.count == 3 &&
        sexpr_is_symbol(head->list.items[0], "_") &&
        (sexpr_is_symbol(head->list.items[1], "zero_extend") ||
         sexpr_is_symbol(head->list.items[1], "sign_extend")) &&
        head->list.items[2]->kind == SEXPR_NUMERAL && s->list.count == 2) {
        uint64_t n = head->list.items[2]->numval;
        EvalRet inner = _eval_sexpr(fe, s->list.items[1], ok);
        if (!*ok) return r;
        if (!inner.width || inner.width + n > EVAL_MAX_W) { *ok = 0; return r; }
        r.width = (uint16_t)(inner.width + n);
        r.value = inner.value;
        if (sexpr_is_symbol(head->list.items[1], "sign_extend") && n &&
            ((inner.value >> (inner.width - 1)) & 1))
            r.value |= _trunc(~(EvalVal)0, r.width) & ~_trunc(~(EvalVal)0, inner.width);
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
    if (sexpr_is_symbol(head, "concat") && s->list.count > 3) {
        /* n-ary concat, left-associative (see the translator's). */
        EvalRet r = _eval_sexpr(fe, s->list.items[1], ok);
        for (uint32_t i = 2; i < s->list.count && *ok; i++) {
            EvalRet b = _eval_sexpr(fe, s->list.items[i], ok);
            /* A result past EVAL_MAX_W cannot be held: refuse, never truncate. */
            if (!r.width || !b.width || r.width + b.width > EVAL_MAX_W) { *ok = 0; return r; }
            r.width = r.width + b.width;
            r.value = _trunc((r.value << b.width) | b.value, r.width);
        }
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
            if (!a.width || !b.width || a.width + b.width > EVAL_MAX_W) { *ok = 0; return r; }
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

/* A 128-bit array key as `#x...`, zero-padded to `width` bits' digits. The
 * hex form is what Verilator parses back (a width that is not a multiple of 4
 * pads up to the next digit, as it always did). */
static void _emit_hex_key(FILE *out, uint64_t lo, uint64_t hi, uint32_t width) {
    uint32_t nd = (width + 3) / 4;
    if (nd == 0) nd = 1;
    fputs("#x", out);
    for (uint32_t d = nd; d-- > 0;) {
        uint32_t bit = d * 4;
        uint64_t nib = bit >= 64 ? (bit >= 128 ? 0 : hi >> (bit - 64)) : lo >> bit;
        fputc("0123456789abcdef"[nib & 0xF], out);
    }
}

/* Print a term back as SMT-LIB, for echoing a get-value key. */
static void _fprint_sexpr(FILE *out, const Sexpr *s) {
    switch (s->kind) {
    case SEXPR_SYMBOL: case SEXPR_KEYWORD:
        fprintf(out, "%.*s", (int)s->sym.len, s->sym.str); break;
    case SEXPR_STRING:
        fprintf(out, "\"%.*s\"", (int)s->sym.len, s->sym.str); break;
    case SEXPR_NUMERAL:
        fprintf(out, "%llu", (unsigned long long)s->numval); break;
    case SEXPR_BITVEC:
        if (s->bv.width > 64 && s->bv.limbs)
            _emit_bv_bin_literal_wide(out, s->bv.limbs, (s->bv.width + 63u) / 64u,
                                      (unsigned)s->bv.width);
        else
            _emit_bv_bin_literal(out, s->bv.value, (unsigned)s->bv.width);
        break;
    case SEXPR_LIST:
        fprintf(out, "(");
        for (uint32_t i = 0; i < s->list.count; i++) {
            if (i) fprintf(out, " ");
            _fprint_sexpr(out, s->list.items[i]);
        }
        fprintf(out, ")");
        break;
    default: break;
    }
}

/* Emit an evaluator value of `width` bits; wider than 64 bits spans limbs. */
static void _emit_eval_value(FILE *out, EvalVal v, unsigned width) {
    uint64_t limbs[(EVAL_MAX_W + 63) / 64];
    for (uint32_t i = 0; i < (EVAL_MAX_W + 63) / 64; i++) {
        limbs[i] = (uint64_t)v;
#if EVAL_MAX_W > 64
        v >>= 64;
#endif
    }
    _emit_bv_bin_literal_wide(out, limbs, (EVAL_MAX_W + 63) / 64, width);
}

static int _cmd_get_value(Smt2Frontend *fe, const Sexpr *cmd) {
    if (!fe->has_result || fe->last_result != DVS_SOLVE_OK) {
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
                fprintf(fe->out, "(%.*s ", (int)name_s->sym.len, name_s->sym.str);
                if (v->width > 64) {
                    uint64_t limbs[(SMT2_MAX_BV_BITS + 63) / 64];
                    uint32_t nl = (v->width + 63u) / 64u;
                    _fe_get_var_value_wide(fe, v->var_id, limbs, nl);
                    _emit_bv_bin_literal_wide(fe->out, limbs, nl, (unsigned)v->width);
                } else {
                    int64_t val = _fe_get_var_value(fe, v->var_id);
                    _emit_bv_bin_literal(fe->out, (uint64_t)val, (unsigned)v->width);
                }
                fprintf(fe->out, ")");
                continue;
            }
        }

        /* (select <arrayvar> <const-index>): a driver reads array elements
         * this way (Verilator get-values each rand array element), and an
         * element of a nested array as (select (select <arrayvar> k) j). Echo
         * the select as the key and emit the element's model value. */
        if (name_s->kind == SEXPR_LIST && name_s->list.count == 3 &&
            sexpr_is_symbol(name_s->list.items[0], "select")) {
            const Sexpr *arr_s = name_s->list.items[1];
            const Sexpr *outer_idx = NULL;
            if (arr_s->kind == SEXPR_LIST && arr_s->list.count == 3 &&
                sexpr_is_symbol(arr_s->list.items[0], "select")) {
                outer_idx = arr_s->list.items[2];
                arr_s = arr_s->list.items[1];
            }
            Smt2ArrayVar *av = arr_s->kind == SEXPR_SYMBOL
                ? _find_array_var(fe, arr_s->sym.str, arr_s->sym.len) : NULL;
            /* The select's shape must match the sort: nested takes two. */
            if (av && (outer_idx != NULL) != (av->sort.inner_addr != 0)) av = NULL;
            uint8_t dw = av ? av->sort.data_width : 0;
            uint32_t ow = av ? (uint32_t)(av->sort.addr_width - av->sort.inner_addr) : 0;
            uint32_t iw = av ? (av->sort.inner_addr ? av->sort.inner_addr
                                                    : av->sort.addr_width) : 0;
            uint64_t o_lo = 0, o_hi = 0, k = 0, k_hi = 0;
            int is_const = 0, idx_evald = 0;
            if (av && outer_idx && !_const_index(fe, outer_idx, ow, &o_lo, &o_hi)) av = NULL;
            if (av) is_const = _const_index(fe, name_s->list.items[2], iw, &k, &k_hi);
            /* A non-constant index: the element its model value selects. */
            if (av && !is_const && !outer_idx) {
                int iok = 1;
                EvalRet ie = _eval_sexpr(fe, name_s->list.items[2], &iok);
                if (iok && ie.width <= 64) { k = (uint64_t)ie.value; is_const = 1; idx_evald = 1; }
            }
            if (av && is_const) {
                uint64_t f_lo = k, f_hi = k_hi;          /* flat key */
                if (outer_idx) _key_concat(o_lo, o_hi, k, k_hi, iw, &f_lo, &f_hi);
                int64_t val = 0;
                uint32_t elem_vid = UINT32_MAX;   /* element var, for a wide read */
                uint64_t seed_key = f_lo ^ (f_hi * 0xC2B2AE3D27D4EB4Full);
                if (av->value->is_abstract) {
                    if (!_abs_base_value(fe, av->value, f_lo, &val))
                        val = _free_elem_value(fe, seed_key, arr_s->sym.len);
                } else if (av->value->is_sparse) {
                    uint32_t vid;
                    if (_sparse_find(av->value, f_lo, f_hi, &vid)) {
                        val = _fe_get_var_value(fe, vid);
                        elem_vid = vid;
                    } else if (av->value->has_default) {
                        val = (int64_t)av->value->default_val;
                    } else {
                        val = _free_elem_value(fe, seed_key, arr_s->sym.len);
                    }
                } else if (f_hi == 0 && f_lo < av->value->n_elems) {
                    /* Look the element var up by name ("arr[k]") rather than via
                     * value->elems[k]: that dvs_expr_t points into the builder
                     * arena, which is reset after compilation, so it is stale
                     * here (dereferencing it segfaults). */
                    char en[SMT2_MAX_NAME + 16];
                    snprintf(en, sizeof(en), "%.*s[%llu]",
                             (int)arr_s->sym.len, arr_s->sym.str,
                             (unsigned long long)f_lo);
                    Smt2Var *ev = _find_var(fe, en, (uint32_t)strlen(en));
                    if (ev) { val = _fe_get_var_value(fe, ev->var_id); elem_vid = ev->var_id; }
                }
                /* Emit ((select name #x<idx>) #b<value>): the key echoes the
                 * select (hex indices padded to their widths -- the form
                 * drivers parse), the value is the element's model. */
                if (idx_evald) {
                    fprintf(fe->out, "(");
                    _fprint_sexpr(fe->out, name_s);
                    fprintf(fe->out, " ");
                } else {
                    fprintf(fe->out, "((select ");
                    if (outer_idx) {
                        fprintf(fe->out, "(select %.*s ", (int)arr_s->sym.len, arr_s->sym.str);
                        _emit_hex_key(fe->out, o_lo, o_hi, ow);
                        fprintf(fe->out, ")");
                    } else {
                        fprintf(fe->out, "%.*s", (int)arr_s->sym.len, arr_s->sym.str);
                    }
                    fprintf(fe->out, " ");
                    _emit_hex_key(fe->out, k, k_hi, iw);
                    fprintf(fe->out, ") ");
                }
                if (dw > 64 && elem_vid != UINT32_MAX) {
                    uint64_t limbs[SMT2_BV_MAX_LIMBS];
                    uint32_t nl = (dw + 63u) / 64u;
                    _fe_get_var_value_wide(fe, elem_vid, limbs, nl);
                    _emit_bv_bin_literal_wide(fe->out, limbs, nl, (unsigned)dw);
                } else {
                    _emit_bv_bin_literal(fe->out, (uint64_t)val, (unsigned)dw);
                }
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
                _emit_eval_value(fe->out, ev.value, (unsigned)w);
                fprintf(fe->out, ")");
            } else {
                /* For a list expression, smtbmc typically only queries
                 * symbols, so this branch is rarely hit. Print a
                 * minimal valid response. */
                fprintf(fe->out, "(? ");
                _emit_eval_value(fe->out, ev.value, (unsigned)w);
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
    if (!fe->has_result || fe->last_result != DVS_SOLVE_OK) {
        fprintf(fe->err, "error: get-model requires a prior sat result\n");
        return 0;
    }
    fprintf(fe->out, "(\n");
    /* BV/Bool vars */
    for (uint32_t i = 0; i < fe->n_vars && i < fe->vars_cap; i++) {
        Smt2Var *v = &fe->vars[i];
        if (!v->name[0]) continue;                 /* an unnamed id (_bump_n_vars) */
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
        if (av->sort.inner_addr)
            fprintf(fe->out, "  (define-fun %s () (Array (_ BitVec %u) (Array (_ BitVec %u) "
                    "(_ BitVec %u))) ", av->name,
                    (unsigned)(av->sort.addr_width - av->sort.inner_addr),
                    (unsigned)av->sort.inner_addr, (unsigned)av->sort.data_width);
        else
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
        fprintf(fe->out, "(:version \"%s\")\n", DVS_VERSION);
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

/* Record the abstract-array state a pop of frame `d` returns to: translation
 * counts only, a solve's own reads are rebuilt by the next solve. */
static void _abs_push_frame(Smt2Frontend *fe, uint32_t d) {
    fe->push_n_anodes[d] = fe->n_anodes;
    fe->push_n_areads[d] = fe->n_areads_user;
    fe->push_n_aeqs[d]   = fe->n_aeqs;
}

/* Pop the abstract-array state back to frame `d`: nodes, reads and equalities
 * made inside the scope go (their vars went with the builder rewind). Nodes
 * made before it never point at later ones. */
static void _abs_pop_frame(Smt2Frontend *fe, uint32_t d) {
    _abs_drop_solve_state(fe);
    if (fe->push_n_areads[d] < fe->n_areads) {
        fe->n_areads = fe->n_areads_user = fe->push_n_areads[d];
        if (fe->aread_hash) {
            memset(fe->aread_hash, 0, fe->aread_hash_cap * sizeof(uint32_t));
            for (uint32_t i = 0; i < fe->n_areads; i++) _aread_hash_put(fe, i);
        }
    }
    if (fe->push_n_aeqs[d] < fe->n_aeqs) fe->n_aeqs = fe->push_n_aeqs[d];
    for (uint32_t i = fe->push_n_anodes[d]; i < fe->n_anodes; i++) free(fe->anodes[i]);
    if (fe->push_n_anodes[d] < fe->n_anodes) fe->n_anodes = fe->push_n_anodes[d];
}

static int _cmd_push(Smt2Frontend *fe, const Sexpr *cmd) {
    _end_assumptions(fe);
    uint32_t n = 1;
    if (cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_NUMERAL) {
        n = (uint32_t)cmd->list.items[1]->numval;
    }
    /* Abstract arrays solve from the builder (_check_sat_array), so their
     * scopes live there: no CDCL context, the frame only marks the builder. */
    int crc = (fe->n_anodes > 0 && !fe->compiled) ? -1 : _ensure_compiled(fe);
    /* Compile-time UNSAT, or the assertions since the last push conflicting as
     * they reach the context: the scope pushed from is unsat. Failing the push
     * on the latter made the next pop drop the scope holding the conflicting
     * assertion -- `x == 3; push; x == 4; push; pop` answered sat. Record dead
     * frames (no checkpoint) instead: everything asserted under them stays in
     * the context until the pop of the scope that is unsat, whose checkpoint
     * takes it all back. */
    int dead = (crc == -2);
    if (crc >= 0) {
        int frc = _flush_aux(fe);
        if (frc == -2) dead = 1;
        else if (frc < 0) return -1;
    }
    if (dead || crc < 0) {
        /* No CDCL context (e.g. variables wider than 64 bits: bitblast only).
         * The scope still has to exist: record a frame whose pop rewinds the
         * builder. Failing the push instead left everything asserted after it
         * in force forever -- a popped `false` answered unsat (B63). */
        if (dead) {
            fe->last_result = DVS_SOLVE_UNSAT;
            fe->has_result = 1;
        }
        if (fe->push_depth + n > SMT2_MAX_PUSH) {
            fprintf(fe->err, "error: push: max push depth exceeded\n");
            return -1;
        }
        for (uint32_t i = 0; i < n; i++) {
            fe->push_stack[fe->push_depth] = (uint32_t)-1;
            fe->push_n_vars[fe->push_depth] = fe->n_vars;
            fe->push_n_array_vars[fe->push_depth] = fe->n_array_vars;
            fe->push_n_aux_problems[fe->push_depth] = fe->n_aux_problems;
            fe->push_n_named[fe->push_depth] = fe->n_named;
            fe->push_n_core_hist[fe->push_depth] = fe->n_core_hist;
            fe->push_incomplete[fe->push_depth] = (uint8_t)fe->incomplete;
            _abs_push_frame(fe, fe->push_depth);
            fe->push_bmark[fe->push_depth] = dvs_builder_mark(fe->builder);
            fe->push_depth++;
        }
        return 0;
    }
    if (fe->has_result) dvs_solver_reset(fe->ctx);
    fe->has_result = 0;

    for (uint32_t i = 0; i < n; i++) {
        int cp = dvs_solver_checkpoint(fe->ctx);
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
        fe->push_n_named[fe->push_depth - 1] = fe->n_named;
        fe->push_n_core_hist[fe->push_depth - 1] = fe->n_core_hist;
        fe->push_incomplete[fe->push_depth - 1] = (uint8_t)fe->incomplete;
        _abs_push_frame(fe, fe->push_depth - 1);
        fe->push_bmark[fe->push_depth - 1] = dvs_builder_mark(fe->builder);
    }
    return 0;
}

static int _cmd_pop(Smt2Frontend *fe, const Sexpr *cmd) {
    /* The incremental enumeration solver holds the popped scope's asserts and
     * cannot retract them. (The builder rewinds below, so a later finalize
     * could even put a different constraint at the same offset.) */
    fe->bb_inc = 0;
    _end_assumptions(fe);
    /* push always compiles and flushes, so anything still pending in the
     * builder was asserted inside the scope being popped: drop it, or it would
     * reach the ctx at the next flush -- a retracted assertion still enforced,
     * i.e. a wrong `unsat` (B37). This also ends CDCL retention: the builder
     * can no longer stand in for the full, current assertion set. */
    uint32_t n = 1;
    if (cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_NUMERAL) {
        n = (uint32_t)cmd->list.items[1]->numval;
    }
    if (fe->compiled) {
        if (fe->cdcl_retained && n <= fe->push_depth &&
            dvs_builder_rewind(fe->builder, &fe->push_bmark[fe->push_depth - n]) == 0) {
            /* Rewind to the outermost popped push instead: the builder then
             * holds exactly the assertions still in scope, so retention (and
             * the bitblast route, which finalizes from it) stays valid. A push
             * flushes first, so everything up to its mark reached the ctx.
             * Resetting the builder here left a bitblast-routed check after
             * the pop solving a stale problem -- a wrong unsat (B63). */
            fe->aux_mark = fe->push_bmark[fe->push_depth - n];
        } else {
            dvs_builder_reset(fe->builder);
            fe->cdcl_retained = 0;
            memset(&fe->aux_mark, 0, sizeof(fe->aux_mark));
        }
        fe->has_aux = 0;
        if (fe->problem) fe->problem_dirty = 1;
    } else if (n <= fe->push_depth) {
        /* No compiled context: the scopes live only in the builder, which
         * the bitblast path finalizes from. Rewind it to the outermost
         * popped push, and make the next check-sat re-finalize (B63). */
        if (dvs_builder_rewind(fe->builder, &fe->push_bmark[fe->push_depth - n]) != 0)
            SMT2_TAINT(fe, "pop could not rewind the assertion set");
        if (fe->problem) fe->problem_dirty = 1;
        fe->has_result = 0;
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
            /* dvs_solver_restore handles trail backtrack itself; calling
             * dvs_solver_reset first would zero ctx->trail_top while leaving
             * checkpoint marks pointing at stale TrailEntry addresses,
             * leading to a NULL-deref inside trail_backtrack. */
            fe->has_result = 0;
            dvs_solver_restore(fe->ctx, fe->push_stack[fe->push_depth]);
        }
        fe->n_vars = fe->push_n_vars[fe->push_depth];

        /* Free arrays declared inside the popped scope. The solver-side BV
         * vars are already unwound by dvs_solver_restore via the n_vars rewind;
         * here we drop the frontend-side tracking and malloc'd element list. */
        uint32_t target_n_arr = fe->push_n_array_vars[fe->push_depth];
        for (uint32_t i = target_n_arr; i < fe->n_array_vars; i++) {
            Smt2ArrayVar *av = &fe->array_vars[i];
            /* Abstract (DV_ARRAY) values are owned by anodes[]; don't free here
             * (freed at reset/destroy) -- just drop the name-table reference. */
            if (av->value && !av->value->is_abstract) {
                free(av->value->elems);
                free(av->value->sparse_idx);
                free(av->value->sparse_idx_hi);
                free(av->value->sparse_varid);
                free(av->value);
            }
            av->value = NULL;
            av->name[0] = '\0';
        }
        fe->n_array_vars = target_n_arr;
        /* A sparse array declared before the push keeps element vars made
         * inside it in its table; those vars are gone with the n_vars rewind,
         * and a later select of that index read a dangling var (a crash). Vars
         * are numbered upward, so they are exactly the entries past n_vars. */
        for (uint32_t i = 0; i < fe->n_array_vars; i++) {
            Smt2ArrayValue *v = fe->array_vars[i].value;
            if (!v || !v->is_sparse) continue;
            uint32_t keep = 0;
            for (uint32_t j = 0; j < v->n_sparse; j++) {
                if (v->sparse_varid[j] >= fe->n_vars) continue;
                v->sparse_idx[keep] = v->sparse_idx[j];
                v->sparse_idx_hi[keep] = v->sparse_idx_hi[j];
                v->sparse_varid[keep++] = v->sparse_varid[j];
            }
            v->n_sparse = keep;
        }
        _abs_pop_frame(fe, fe->push_depth);

        /* Drop aux problems added between push and pop. The solver-side
         * propagators those aux problems compiled were already marked
         * ENTAILED by dvs_solver_restore, but their dvs_problem_t buffers
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
        _truncate_named(fe, fe->push_n_named[fe->push_depth]);
        if (fe->n_core_hist > fe->push_n_core_hist[fe->push_depth])
            fe->n_core_hist = fe->push_n_core_hist[fe->push_depth];
    }
    return 0;
}

/* Wall-clock budget for minimising one unsat core. A core cut short is still
 * a valid core, only a larger one. */
#define SMT2_CORE_BUDGET_MS 10000

/* Replay the recorded commands up to the last check-sat in a scratch frontend,
 * leaving out the named assertions marked in `drop`, and solve. Returns 1 only
 * for a definite `unsat`; sat, unknown and any error return 0, which keeps the
 * assertion in the core. */
static int _core_replay_unsat(const Smt2Frontend *fe, const uint8_t *drop,
                              FILE *sink) {
    Smt2Frontend *sub = (Smt2Frontend *)malloc(sizeof(*sub));
    if (!sub) return 0;
    smt2_frontend_init(sub, sink, sink);
    int ok = 1;
    for (uint32_t i = 0; i < fe->core_hist_at_check && ok; i++) {
        const Smt2CoreCmd *c = &fe->core_hist[i];
        if (c->named >= 0 && drop[c->named]) continue;
        ok = (smt2_frontend_dispatch(sub, c->cmd) == 0);
    }
    if (ok) _cmd_check_sat(sub, NULL);
    int unsat = ok && !sub->incomplete && sub->has_result
             && sub->last_result == DVS_SOLVE_UNSAT;
    smt2_frontend_destroy(sub);
    free(sub);
    return unsat;
}

static uint64_t _core_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Mark in `drop` the named assertions the contradiction does not need
 * (deletion-based: try each one out, keep it out while the rest stay unsat).
 * Every step keeps the invariant that the replayed set without the dropped
 * names is unsat, so whatever is left -- even when the budget runs out -- is
 * a valid core. */
static void _core_minimise(const Smt2Frontend *fe, uint8_t *drop) {
    if (!fe->produce_unsat_cores || !fe->core_replayable) return;
    uint8_t *recorded = (uint8_t *)calloc(fe->n_named ? fe->n_named : 1, 1);
    FILE *sink = fopen("/dev/null", "w");
    if (!recorded || !sink) goto done;
    uint32_t n_recorded = 0;
    for (uint32_t i = 0; i < fe->core_hist_at_check; i++) {
        int32_t k = fe->core_hist[i].named;
        if (k >= 0 && (uint32_t)k < fe->n_named && !recorded[k]) {
            recorded[k] = 1;
            n_recorded++;
        }
    }
    if (n_recorded == 0) goto done;
    uint64_t t0 = _core_now_ms();
    /* The replay must reproduce the unsat before it can be trusted to shrink
     * it (a command recorded before :produce-unsat-cores was set is missing). */
    if (!_core_replay_unsat(fe, drop, sink)) goto done;
    for (uint32_t k = 0; k < fe->n_named; k++) {
        if (!recorded[k]) continue;
        if (_core_now_ms() - t0 > SMT2_CORE_BUDGET_MS) break;
        drop[k] = 1;
        if (!_core_replay_unsat(fe, drop, sink)) drop[k] = 0;
    }
done:
    if (sink) fclose(sink);
    free(recorded);
}

/* (get-unsat-core): after `unsat`, report :named assertions that are
 * unsatisfiable together with the unnamed ones. With :produce-unsat-cores set
 * the set is minimised: dropping any one reported name leaves a satisfiable
 * set (unless the time budget ran out). Otherwise every name in scope is
 * reported -- the whole assertion set is unsatisfiable, so that is a valid, if
 * not minimal, core. There is ALWAYS a reply on stdout: drivers (Verilator's
 * randomize() on an unsat constraint set) block reading one, and a missing
 * reply hangs them. */
static int _cmd_get_unsat_core(Smt2Frontend *fe, const Sexpr *cmd) {
    (void)cmd;
    if (!fe->has_result || fe->last_result != DVS_SOLVE_UNSAT) {
        fprintf(fe->out, "(error \"get-unsat-core requires a prior unsat result\")\n");
        fflush(fe->out);
        return 0;
    }
    uint8_t *drop = (uint8_t *)calloc(fe->n_named ? fe->n_named : 1, 1);
    if (drop) _core_minimise(fe, drop);
    fputc('(', fe->out);
    int first = 1;
    for (uint32_t i = 0; i < fe->n_named; i++) {
        if (drop && drop[i]) continue;
        fprintf(fe->out, first ? "%s" : " %s", fe->named[i]);
        first = 0;
    }
    fputs(")\n", fe->out);
    fflush(fe->out);
    free(drop);
    return 0;
}

static int _cmd_check_sat_assuming(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_LIST) {
        fprintf(fe->err, "error: check-sat-assuming requires a literal list\n");
        return -1;
    }

    fe->core_replayable = 0;   /* the core would have to cover the assumptions */

    /* Tainted context -> honest `unknown` (see _cmd_check_sat). */
    if (fe->incomplete) {
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_TIMEOUT;
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
            fe->model_reused = 1;
            switch (fe->last_result) {
            case DVS_SOLVE_OK:      fprintf(fe->out, "sat\n");     break;
            case DVS_SOLVE_UNSAT:   fprintf(fe->out, "unsat\n");   break;
            default:            SMT2_EMIT_UNKNOWN(fe); break;
            }
            fflush(fe->out);
            return 0;
        }
        return _cmd_check_sat(fe, cmd);
    }
    /* Standard path: this solves afresh, so any earlier bit-blast model is not
     * this answer's (B36). (The Verilator branch above deliberately re-uses the
     * previous check-sat's model, so it must NOT clear this.) */
    fe->bb_model_valid = 0;

    if (_ensure_compiled(fe) < 0) {
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        return -1;
    }
    _end_assumptions(fe);
    if (fe->has_result) dvs_solver_reset(fe->ctx);
    fe->has_result = 0;
    if (_flush_aux(fe) < 0) {
        SMT2_EMIT_UNKNOWN(fe);
        fflush(fe->out);
        return -1;
    }
    int cp = dvs_solver_checkpoint(fe->ctx);
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
            dvs_solver_restore(fe->ctx, (uint32_t)cp);
            return -1;
        }
        Smt2Var *v = _find_var(fe, name, nlen);
        if (!v) {
            fprintf(fe->err, "error: unknown assumption var '%.*s'\n", (int)nlen, name);
            dvs_solver_restore(fe->ctx, (uint32_t)cp);
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
        int rc = dvs_solver_pin_var(fe->ctx, v->var_id, positive ? 1 : 0);
        if (rc != 0) { forced_unsat = 1; break; }
    }

    if (forced_unsat) {
        fprintf(fe->out, "unsat\n");
        fe->last_result = DVS_SOLVE_UNSAT;
    } else {
        dvs_solve_opts_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.seed = fe->seed;
        fe->last_result = dvs_solver_solve(fe->ctx, &opts);
        switch (fe->last_result) {
        case DVS_SOLVE_OK:      fprintf(fe->out, "sat\n"); break;
        case DVS_SOLVE_UNSAT:   fprintf(fe->out, "unsat\n"); break;
        case DVS_SOLVE_TIMEOUT: SMT2_EMIT_UNKNOWN(fe); break;
        }
    }
    fe->last_assump_unsat = (fe->last_result == DVS_SOLVE_UNSAT);
    fe->has_result = 1;
    fe->assump_cp1 = (uint32_t)cp + 1;
    fflush(fe->out);
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
    fe->builder = dvs_builder_create(0, NULL);
    sexpr_arena_init(&fe->persistent_arena, 16384);
    /* Verilator-mode cache reseed cadence: force a full re-solve every Nth
     * randomize so coupled fields periodically get a fresh base model.
     * DV_VERILATOR_RESEED=0 (default) = pure cache (max throughput). */
    fe->reseed_period = 0;
    const char *rs = getenv("DV_VERILATOR_RESEED");
    if (rs) { long v = strtol(rs, NULL, 10); if (v > 0) fe->reseed_period = (uint32_t)v; }
    /* Verilator randomization prefers the CDCL engine (near-uniform sampler)
     * over bitblast's _diversify (bit-flip repair, measurably skewed).
     * DV_VERILATOR_CDCL=0 restores the old bitblast-always routing. */
    fe->verilator_cdcl = 1;
    { const char *vc = getenv("DV_VERILATOR_CDCL");
      if (vc && *vc == '0') fe->verilator_cdcl = 0; }
    fe->cdcl_probe_fp = 0;
    fe->cdcl_route    = 0;
    /* DV_ARRAY: word-level abstract array reasoning (opt-in). DV_ARRAY=eager
     * selects the one-shot Ackermann encoding (correctness oracle); any other
     * truthy value selects the lazy lemmas-on-demand engine. */
    const char *da = getenv("DV_ARRAY");
    fe->array_lazy = (da && da[0] && strcmp(da, "0") != 0) ? 1 : 0;
    fe->array_eager = (da && strcmp(da, "eager") == 0) ? 1 : 0;
    /* Unset: only arrays too large to expand densely are abstract. DV_ARRAY=0:
     * none are (a symbolic index into one is `unknown`). */
    fe->array_auto = !(da && strcmp(da, "0") == 0);
}

void smt2_frontend_destroy(Smt2Frontend *fe) {
    free(fe->shape_tab);
    _truncate_named(fe, 0);
    free(fe->named);
    free(fe->core_hist);   /* the copies themselves live in persistent_arena */
    _free_bb(fe);
    if (fe->problem)     free(fe->problem);
    for (uint32_t i = 0; i < fe->n_aux_problems; i++) {
        free(fe->aux_problems[i]);
    }
    free(fe->aux_problems);
    if (fe->builder)     dvs_builder_destroy(fe->builder);
    if (fe->ctx)         dvs_solver_destroy(fe->ctx);
    if (fe->block_alloc) dvs_block_alloc_destroy(fe->block_alloc);
    free(fe->ctx_buf);
    free(fe->vars);
    free(fe->funs);
    free(fe->subst_stack);
    sexpr_arena_destroy(&fe->persistent_arena);
    /* Free persistent array var allocations (dense elems[] or sparse map).
     * Abstract (DV_ARRAY) values are owned by anodes[] -- freed below -- so skip
     * them here to avoid a double free. */
    for (uint32_t i = 0; i < fe->n_array_vars; i++) {
        if (fe->array_vars[i].value && !fe->array_vars[i].value->is_abstract) {
            free(fe->array_vars[i].value->elems);
            free(fe->array_vars[i].value->sparse_idx);
            free(fe->array_vars[i].value->sparse_idx_hi);
            free(fe->array_vars[i].value->sparse_varid);
            free(fe->array_vars[i].value);
        }
    }
    /* Free the abstract-array DAG (BASE/STORE/CONST/ITE nodes) + read table. */
    for (uint32_t i = 0; i < fe->n_anodes; i++) free(fe->anodes[i]);
    free(fe->anodes);
    free(fe->areads);
    free(fe->aread_hash);
    free(fe->aeqs);
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
    int      array_lazy = fe->array_lazy, array_eager = fe->array_eager;
    int      array_auto = fe->array_auto;
    uint64_t div_counter = fe->div_counter;
    uint32_t reseed_period = fe->reseed_period;
    /* The array engine's bb_solver was built over fe->problem, freed below. */
    if (fe->bb_over_problem && fe->bb_solver) {
        dvs_bbsolver_free(fe->bb_solver);
        fe->bb_solver = NULL;
        fe->cache_valid = 0;
    }
    dvs_bbsolver_t *bb = fe->bb_solver;
    dvs_problem_t   *bb_problem = fe->bb_problem;   /* backs bb; carried with it */
    uint64_t cached_fp = fe->cached_fp;
    int      cache_valid = fe->cache_valid, cached_result = fe->cached_result;
    int      verilator_cdcl = fe->verilator_cdcl;
    int      vlt_hash_ignore = fe->vlt_hash_ignore;
    uint64_t cdcl_probe_fp = fe->cdcl_probe_fp;
    uint8_t  cdcl_route = fe->cdcl_route;
    uint32_t ctx_buf_scale = fe->ctx_buf_scale;
    void    *shape_tab = fe->shape_tab;
    /* Reused allocations (reset in place below rather than freed). */
    dvs_builder_t *builder = fe->builder;
    SexprArena           parena  = fe->persistent_arena;

    /* Free exactly what destroy() frees, EXCEPT builder + persistent_arena. */
    if (fe->problem) free(fe->problem);
    for (uint32_t i = 0; i < fe->n_aux_problems; i++) free(fe->aux_problems[i]);
    free(fe->aux_problems);
    if (fe->ctx)         dvs_solver_destroy(fe->ctx);
    if (fe->block_alloc) dvs_block_alloc_destroy(fe->block_alloc);
    free(fe->ctx_buf);
    free(fe->vars);
    free(fe->funs);          /* re-zeroed by the memset below; realloc'd on next define-fun */
    free(fe->subst_stack);   /* re-zeroed by the memset below; realloc'd on next let */
    for (uint32_t i = 0; i < fe->n_array_vars; i++) {
        if (fe->array_vars[i].value && !fe->array_vars[i].value->is_abstract) {
            free(fe->array_vars[i].value->elems);
            free(fe->array_vars[i].value->sparse_idx);
            free(fe->array_vars[i].value->sparse_idx_hi);
            free(fe->array_vars[i].value->sparse_varid);
            free(fe->array_vars[i].value);
        }
    }
    for (uint32_t i = 0; i < fe->n_anodes; i++) free(fe->anodes[i]);
    free(fe->anodes);
    free(fe->areads);
    free(fe->aread_hash);
    free(fe->aeqs);
    _cmd_alloc_reset(fe);

    _truncate_named(fe, 0);
    free(fe->named);
    free(fe->core_hist);   /* the copies live in persistent_arena, reset below */

    /* Reset the reused allocations to empty (equivalent to a fresh create). */
    dvs_builder_reset(builder);
    sexpr_arena_reset(&parena);

    /* Re-zero all state (mirrors init's memset), then restore carried fields. */
    memset(fe, 0, sizeof(*fe));
    fe->out = out;
    fe->err = err;
    fe->print_stats = print_stats;
    fe->verilator_mode = verilator_mode;
    fe->array_lazy = array_lazy;
    fe->array_eager = array_eager;
    fe->array_auto = array_auto;
    fe->div_counter = div_counter;
    fe->reseed_period = reseed_period;
    fe->bb_solver = bb;
    fe->bb_problem = bb_problem;
    fe->cached_fp = cached_fp;
    fe->cache_valid = cache_valid;
    fe->cached_result = cached_result;
    fe->verilator_cdcl = verilator_cdcl;
    fe->vlt_hash_ignore = vlt_hash_ignore;
    fe->cdcl_probe_fp = cdcl_probe_fp;
    fe->cdcl_route = cdcl_route;
    fe->ctx_buf_scale = ctx_buf_scale;
    fe->shape_tab = shape_tab;
    fe->builder = builder;
    fe->persistent_arena = parena;
}

static int _dispatch(Smt2Frontend *fe, const Sexpr *cmd);

/* Commands that shape the assertion set, and so are replayed to minimise an
 * unsat core. Options, info and queries are not: none can change a verdict. */
static int _core_shapes_assertions(const Sexpr *head) {
    static const char *const kinds[] = {
        "set-logic", "declare-sort", "declare-const", "declare-fun",
        "define-fun", "declare-datatypes", "assert",
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (sexpr_is_symbol(head, kinds[i])) return 1;
    return 0;
}

/* Record a command for get-unsat-core replay. Best effort: a command that
 * cannot be recorded stops recording for this assertion set, so the replay
 * fails to reproduce the unsat and the core is reported unminimised. */
static void _core_record(Smt2Frontend *fe, const Sexpr *cmd, int32_t named) {
    if (fe->n_core_hist == fe->core_hist_cap) {
        uint32_t cap = fe->core_hist_cap ? fe->core_hist_cap * 2 : 64;
        Smt2CoreCmd *g = (Smt2CoreCmd *)realloc(fe->core_hist, cap * sizeof(*g));
        if (!g) { fe->produce_unsat_cores = 0; return; }
        fe->core_hist = g;
        fe->core_hist_cap = cap;
    }
    const Sexpr *copy = _sexpr_deep_copy(&fe->persistent_arena, cmd);
    if (!copy) { fe->produce_unsat_cores = 0; return; }
    fe->core_hist[fe->n_core_hist].cmd   = copy;
    fe->core_hist[fe->n_core_hist].named = named;
    fe->n_core_hist++;
}

int smt2_frontend_dispatch(Smt2Frontend *fe, const Sexpr *cmd) {
    fe->model_reused = 0;
    fe->cmd_dropped = 0;
    fe->last_validate_viol = -1;
    int record = fe->produce_unsat_cores && cmd && cmd->kind == SEXPR_LIST
              && cmd->list.count > 0 && _core_shapes_assertions(cmd->list.items[0]);
    uint32_t n_named = fe->n_named;
    int rc = _dispatch(fe, cmd);
    if (record && rc == 0)
        _core_record(fe, cmd, fe->n_named > n_named ? (int32_t)fe->n_named - 1 : -1);
    return rc;
}

static int _dispatch(Smt2Frontend *fe, const Sexpr *cmd) {
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
    if (sexpr_is_symbol(head, "get-unsat-core"))
        return _cmd_get_unsat_core(fe, cmd);
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
         * cached result is stale, _cmd_check_sat resets via dvs_solver_reset. */
        _end_assumptions(fe);
        if (fe->has_result) {
            dvs_solver_reset(fe->ctx);
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

char *smt2_frontend_get_value_text(Smt2Frontend *fe, const char *cmd_text,
                                   size_t cmd_len, size_t *out_len) {
    Smt2Lexer lex;
    smt2_lexer_init(&lex, cmd_text, cmd_len);
    SexprArena arena;
    sexpr_arena_init(&arena, 4096);
    Sexpr *cmd = sexpr_parse(&lex, &arena);
    char *buf = NULL;
    size_t len = 0;
    if (cmd && cmd->kind == SEXPR_LIST) {
        FILE *mem = NULL;
#ifdef _WIN32
        mem = tmpfile();
#else
        mem = open_memstream(&buf, &len);
#endif
        if (mem) {
            FILE *saved = fe->out;
            fe->out = mem;
            _cmd_get_value(fe, cmd);
            fe->out = saved;
#ifdef _WIN32
            long n = ftell(mem);
            buf = (char *)malloc((size_t)(n > 0 ? n : 0) + 1);
            if (buf) {
                rewind(mem);
                len = fread(buf, 1, (size_t)(n > 0 ? n : 0), mem);
                buf[len] = '\0';
            }
#endif
            fclose(mem);
        }
    }
    sexpr_arena_destroy(&arena);
    if (out_len) *out_len = buf ? len : 0;
    return buf;
}

const char *smt2_frontend_engine(const Smt2Frontend *fe) {
    if (fe->verilator_mode && fe->vlt_route) return fe->vlt_route;
    return fe->bb_model_valid ? "bitblast" : "cdcl";
}
