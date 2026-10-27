#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include "smt2/smt2_frontend.h"
#include "smt2/smt2_eval.h"
#include "dvs_lcg.h"
#include "dvs_bbsolver.h"
#include "dvs_cube.h"
#include "dvs_shape.h"


/* ------------------------------------------------------------------ */
/* Input errors (plan D1/D2 in docs/fw_hdl_implementation_plan.md)     */
/* ------------------------------------------------------------------ */

/* The body of an SMT-LIB string literal: `"` doubles, control chars become ' '. */
static void _fprint_smt2_string_body(FILE *out, const char *s) {
    for (; *s; s++) {
        if (*s == '"') fputs("\"\"", out);
        else if ((unsigned char)*s < 0x20) fputc(' ', out);
        else fputc(*s, out);
    }
}

static void _report_error(Smt2Frontend *fe, uint32_t line, uint32_t col,
                          int poison, const char *fmt, va_list ap) {
    char msg[sizeof fe->input_err_reason - 32];
    vsnprintf(msg, sizeof msg, fmt, ap);
    if (poison && !fe->input_poisoned) {
        if (line)
            snprintf(fe->input_err_reason, sizeof fe->input_err_reason,
                     "line %u column %u: %s", line, col, msg);
        else
            snprintf(fe->input_err_reason, sizeof fe->input_err_reason, "%s", msg);
    }
    if (poison) fe->input_poisoned = 1;
    uint32_t n = ++fe->n_input_errors;
    if (n > SMT2_MAX_REPORTED_ERRORS + 1) return;
    fputs("(error \"", fe->out);
    if (n > SMT2_MAX_REPORTED_ERRORS) {
        fputs("too many errors; further errors are not reported", fe->out);
    } else {
        if (line) fprintf(fe->out, "line %u column %u: ", line, col);
        _fprint_smt2_string_body(fe->out, msg);
    }
    fputs("\")\n", fe->out);
    fflush(fe->out);
}

void smt2_frontend_input_error(Smt2Frontend *fe, uint32_t line, uint32_t col,
                               const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _report_error(fe, line, col, 1, fmt, ap);
    va_end(ap);
}

/* A query that cannot be answered (get-value with no model, a term we cannot
 * evaluate): an `(error ...)` response and the error exit status, but the
 * assertions are intact, so the session is not poisoned. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void _query_error(Smt2Frontend *fe, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _report_error(fe, fe->cmd_line, fe->cmd_col, 0, fmt, ap);
    va_end(ap);
}

/* An in-memory output stream, to build a response that may still turn into an
 * error (open_memstream where there is one, else a temporary file). */
typedef struct { FILE *f; char *buf; size_t len; } Smt2MemOut;

static int _memout_open(Smt2MemOut *m) {
    m->buf = NULL;
    m->len = 0;
#ifdef _WIN32
    m->f = tmpfile();
#else
    m->f = open_memstream(&m->buf, &m->len);
#endif
    return m->f != NULL;
}

/* Close the stream; copy what was written to `dest` unless it is NULL. */
static void _memout_finish(Smt2MemOut *m, FILE *dest) {
#ifdef _WIN32
    long n = ftell(m->f);
    if (dest && n > 0) {
        char tmp[4096];
        rewind(m->f);
        size_t r;
        while ((r = fread(tmp, 1, sizeof tmp, m->f)) > 0) fwrite(tmp, 1, r, dest);
    }
    fclose(m->f);
#else
    fclose(m->f);
    if (dest && m->len) fwrite(m->buf, 1, m->len, dest);
    free(m->buf);
#endif
    m->f = NULL;
}

/* An input error at the command being dispatched. */
#define _input_error(fe, ...) \
    smt2_frontend_input_error((fe), (fe)->cmd_line, (fe)->cmd_col, __VA_ARGS__)

/* An ill-formed term (see Smt2Frontend.term_err): keep the first message, and
 * log it as a diagnostic. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void _term_error(Smt2Frontend *fe, const char *fmt, ...) {
    char msg[sizeof fe->term_err];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!fe->term_err[0]) memcpy(fe->term_err, msg, sizeof msg);
    fprintf(fe->err, "error: %s\n", msg);
}

/* A well-formed term dv-solve does not implement: keep the first message for
 * (get-info :reason-unknown), and log it as a diagnostic. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void _unsupported(Smt2Frontend *fe, const char *fmt, ...) {
    char msg[sizeof fe->unsup_msg];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!fe->unsup_msg[0]) memcpy(fe->unsup_msg, msg, sizeof msg);
    fprintf(fe->err, "error: %s\n", msg);
}
#include "dvs_i128.h"
#include "dvs_stackinfo.h"

/* DVS_VERSION comes from src/dv_solve/__version__.py, through CMake. */
#ifndef DVS_VERSION
#define DVS_VERSION "unknown"
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */


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
                    uint32_t var_id, uint16_t width) {
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
    v->is_bool  = 0;
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

static uint32_t _var_hash(const char *name, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) h = (h ^ (uint8_t)name[i]) * 16777619u;
    return h;
}

static int _var_index_put(Smt2Frontend *fe, uint32_t slot) {
    if (2 * (fe->var_idx_used + 1) > fe->var_idx_cap) {
        uint32_t cap = fe->var_idx_cap ? 2 * fe->var_idx_cap : 1024;
        uint32_t *t = (uint32_t *)calloc(cap, sizeof(uint32_t));
        if (!t) return -1;
        for (uint32_t i = 0; i < fe->var_idx_cap; i++) {
            uint32_t e = fe->var_idx[i];
            if (!e) continue;
            const char *nm = fe->vars[e - 1].name;
            uint32_t j = _var_hash(nm, (uint32_t)strlen(nm)) & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = e;
        }
        free(fe->var_idx);
        fe->var_idx = t;
        fe->var_idx_cap = cap;
    }
    const char *nm = fe->vars[slot].name;
    uint32_t j = _var_hash(nm, (uint32_t)strlen(nm)) & (fe->var_idx_cap - 1);
    while (fe->var_idx[j]) j = (j + 1) & (fe->var_idx_cap - 1);
    fe->var_idx[j] = slot + 1;
    fe->var_idx_used++;
    return 0;
}

/* The first (lowest) slot named `name`, as a scan of vars[] would find. */
static Smt2Var *_find_var(Smt2Frontend *fe, const char *name, uint32_t len) {
    uint32_t n = fe->n_vars < fe->vars_cap ? fe->n_vars : fe->vars_cap;
    if (len >= SMT2_MAX_NAME) return NULL;     /* stored names are shorter */
    if (fe->var_idx_n > n) {                   /* popped: rebuild */
        if (fe->var_idx) memset(fe->var_idx, 0, fe->var_idx_cap * sizeof(uint32_t));
        fe->var_idx_used = 0;
        fe->var_idx_n = 0;
    }
    for (; fe->var_idx_n < n; fe->var_idx_n++) {
        if (!fe->vars[fe->var_idx_n].name[0]) continue;
        if (_var_index_put(fe, fe->var_idx_n)) {
            /* Out of memory: scan. */
            for (uint32_t i = 0; i < n; i++) {
                if (strlen(fe->vars[i].name) == len &&
                    memcmp(fe->vars[i].name, name, len) == 0)
                    return &fe->vars[i];
            }
            return NULL;
        }
    }
    if (!fe->var_idx_cap) return NULL;
    Smt2Var *best = NULL;
    for (uint32_t j = _var_hash(name, len) & (fe->var_idx_cap - 1); fe->var_idx[j];
         j = (j + 1) & (fe->var_idx_cap - 1)) {
        Smt2Var *v = &fe->vars[fe->var_idx[j] - 1];
        if ((!best || v < best) && v->name[len] == '\0' && memcmp(v->name, name, len) == 0)
            best = v;
    }
    return best;
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

static int _fun_index_put(Smt2Frontend *fe, uint32_t slot) {
    if (2 * (fe->fun_idx_used + 1) > fe->fun_idx_cap) {
        uint32_t cap = fe->fun_idx_cap ? 2 * fe->fun_idx_cap : 256;
        uint32_t *t = (uint32_t *)calloc(cap, sizeof(uint32_t));
        if (!t) return -1;
        for (uint32_t i = 0; i < fe->fun_idx_cap; i++) {
            uint32_t e = fe->fun_idx[i];
            if (!e) continue;
            const char *nm = fe->funs[e - 1].name;
            uint32_t j = _var_hash(nm, (uint32_t)strlen(nm)) & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = e;
        }
        free(fe->fun_idx);
        fe->fun_idx = t;
        fe->fun_idx_cap = cap;
    }
    const char *nm = fe->funs[slot].name;
    uint32_t j = _var_hash(nm, (uint32_t)strlen(nm)) & (fe->fun_idx_cap - 1);
    while (fe->fun_idx[j]) j = (j + 1) & (fe->fun_idx_cap - 1);
    fe->fun_idx[j] = slot + 1;
    fe->fun_idx_used++;
    return 0;
}

/* The first (lowest) slot named `name`, as a scan of funs[] would find. The
 * index is caught up lazily and rebuilt after a pop, as for _find_var. */
static Smt2FunDef *_find_fun(Smt2Frontend *fe, const char *name, uint32_t len) {
    uint32_t n = fe->n_funs;
    if (len >= SMT2_MAX_NAME) return NULL;     /* stored names are shorter */
    if (fe->fun_idx_n > n) {
        if (fe->fun_idx) memset(fe->fun_idx, 0, fe->fun_idx_cap * sizeof(uint32_t));
        fe->fun_idx_used = 0;
        fe->fun_idx_n = 0;
    }
    for (; fe->fun_idx_n < n; fe->fun_idx_n++) {
        if (_fun_index_put(fe, fe->fun_idx_n)) {
            for (uint32_t i = 0; i < n; i++)    /* out of memory: scan */
                if (_name_eq(name, len, fe->funs[i].name)) return &fe->funs[i];
            return NULL;
        }
    }
    if (!fe->fun_idx_cap) return NULL;
    Smt2FunDef *best = NULL;
    for (uint32_t j = _var_hash(name, len) & (fe->fun_idx_cap - 1); fe->fun_idx[j];
         j = (j + 1) & (fe->fun_idx_cap - 1)) {
        Smt2FunDef *f = &fe->funs[fe->fun_idx[j] - 1];
        if ((!best || f < best) && f->name[len] == '\0' && memcmp(f->name, name, len) == 0)
            best = f;
    }
    return best;
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

/* Push one binding onto the stack, linked into the current environment
 * (the caller has reserved room). The caller makes it visible by setting
 * fe->subst_env to the returned link. */
static uint32_t _subst_push(Smt2Frontend *fe, const char *name, uint32_t len,
                            uint32_t up) {
    Smt2Subst *e = &fe->subst_stack[fe->subst_depth++];
    memset(e, 0, sizeof(*e));
    e->name = name;
    e->len  = len;
    e->up   = up;
    return fe->subst_depth;            /* this entry's link: index + 1 */
}

/* Resolve a symbol in environment `env`, innermost binding first.
 * Uses stored lengths rather than strlen to support non-null-terminated
 * names (e.g. let-binding names that point into the parser's arena). */
static Smt2Subst *_subst_find(Smt2Frontend *fe, uint32_t env, const char *name,
                              uint32_t len) {
    for (uint32_t k = env; k; k = fe->subst_stack[k - 1].up) {
        Smt2Subst *e = &fe->subst_stack[k - 1];
        if (e->len == len && memcmp(name, e->name, len) == 0) return e;
    }
    return NULL;
}

static Smt2Subst *_subst_lookup_entry(Smt2Frontend *fe, const char *name, uint32_t len) {
    return _subst_find(fe, fe->subst_env, name, len);
}

/* Resolve a symbol-typed Sexpr through define-fun parameters to the argument
 * it stands for. Each argument is looked up in its caller's environment
 * (arg_env), as _translate_symbol_tagged translates it: looked up in the
 * callee's, `(define-fun f ((state S)) .. (g state))` resolved `state` to
 * itself and looped forever (B84). A let name is returned as is. */
static const Sexpr *_resolve_sym(Smt2Frontend *fe, const Sexpr *s) {
    uint32_t env = fe->subst_env;
    while (s && s->kind == SEXPR_SYMBOL) {
        Smt2Subst *e = _subst_find(fe, env, s->sym.str, s->sym.len);
        if (!e || !e->value) break;
        s   = e->value;
        env = e->arg_env;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Sort parsing                                                        */
/* ------------------------------------------------------------------ */

/* A sort with a (define-sort) alias resolved; anything else unchanged. Alias
 * definitions are stored expanded, so one lookup is enough; aliases nested
 * in a compound sort are resolved by the parsers below as they recurse. */
static const Sexpr *_expand_sort(Smt2Frontend *fe, const Sexpr *sort) {
    if (!fe->n_sort_aliases || !sort || sort->kind != SEXPR_SYMBOL) return sort;
    for (uint32_t i = 0; i < fe->n_sort_aliases; i++)
        if (_name_eq(sort->sym.str, sort->sym.len, fe->sort_aliases[i].name))
            return fe->sort_aliases[i].def;
    return sort;
}

static int _is_bool_sort(Smt2Frontend *fe, const Sexpr *sort) {
    return sexpr_is_symbol(_expand_sort(fe, sort), "Bool");
}

static uint16_t _parse_bitvec_sort(Smt2Frontend *fe, const Sexpr *sort) {
    sort = _expand_sort(fe, sort);
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
    return (uint16_t)w;
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
    s = _expand_sort(fe, s);
    if (!s || s->kind != SEXPR_LIST) return 0;
    if (s->list.count != 3) return 0;
    if (!sexpr_is_symbol(s->list.items[0], "Array")) return 0;

    uint16_t addr_w = _parse_bitvec_sort(fe, s->list.items[1]);
    if (addr_w == 0 || addr_w > SMT2_MAX_ARRAY_BV_BITS) return -1;
    out->inner_addr = 0;
    out->_pad[0] = 0;

    /* (Array (_ BitVec I) (Array (_ BitVec J) (_ BitVec N))): one flat array
     * keyed by I ++ J (the key holds 128 bits). */
    Smt2ArraySort inner;
    if (nested_ok && _parse_array_sort_x(fe, s->list.items[2], &inner, 0) == 1) {
        if ((uint32_t)addr_w + inner.addr_width > SMT2_MAX_ARRAY_BV_BITS) return -1;
        out->addr_width = (uint8_t)(addr_w + inner.addr_width);
        out->data_width = inner.data_width;
        out->inner_addr = inner.addr_width;
        return 1;
    }

    uint16_t data_w = _parse_bitvec_sort(fe, s->list.items[2]);
    if (data_w == 0 || data_w > SMT2_MAX_ARRAY_BV_BITS) return -1;

    out->addr_width = (uint8_t)addr_w;
    out->data_width = (uint8_t)data_w;
    return 1;
}

static int _parse_array_sort(Smt2Frontend *fe, const Sexpr *s, Smt2ArraySort *out) {
    return _parse_array_sort_x(fe, s, out, 0);
}

/* Returns 1 if sort is an opaque (declare-sort) sort name. */
static int _is_opaque_sort(Smt2Frontend *fe, const Sexpr *sort) {
    sort = _expand_sort(fe, sort);
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

/* For a function with a SMT2_BV_MAX_LIMBS buffer (8 KB) that the recursive
 * translator calls: kept out of line, the buffer exists only while that call
 * runs. Inlined, it would sit in every nesting level's frame (9.8 KB a level
 * instead of under 1 KB). */
#if defined(_MSC_VER)
#define SMT2_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define SMT2_NOINLINE __attribute__((noinline))
#else
#define SMT2_NOINLINE
#endif

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
    dvs_expr_t vref = dvs_builder_add_var(fe->builder, var_id, (uint16_t)width, 0, 0, max_val);
    /* Mark aux vars as VAR_AUX so search never decides them: they are
     * fully determined by their defining constraint. With the ITE_value
     * cond back-propagation in place, propagation can pin them on its
     * own through whichever branch is consistent with r's domain. */
    dvs_builder_mark_var_aux(fe->builder, vref);
    char name[SMT2_MAX_NAME];
    /* A backslash cannot occur in any SMT-LIB symbol, so no user name
     * collides with an internal one. */
    snprintf(name, sizeof(name), "\\aux%u", var_id);
    _add_var(fe, name, (uint32_t)strlen(name), var_id, (uint16_t)width);
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
    /* No store: left at calloc's 0, the R1 rewrite in _array_select took a
     * select indexed by var 0 (the first declared) for a store at that index
     * and returned ref 0, not a node (B85). */
    av->value->store_idx_varid = UINT32_MAX;
    av->value->store_val       = EXPR_NULL;
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
    dvs_builder_add_var(fe->builder, var_id, (uint16_t)width, 0, 0, max_val);
    char name[SMT2_MAX_NAME];
    snprintf(name, sizeof(name), "\\rd%u", var_id);
    _add_var(fe, name, (uint32_t)strlen(name), var_id, (uint16_t)width);
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
 * BV-valued constant this front end emits is therefore SIZED. */
static dvs_expr_t _bv_const(Smt2Frontend *fe, int64_t value, uint16_t width) {
    if (width == 0)
        return dvs_builder_expr_const(fe->builder, value, 0);
    return dvs_builder_expr_const_sized(fe->builder, value, 0, width);
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

/* Every translator level returns one of these, so its size sets the stack
 * cost of nesting: at 16 bytes the SysV ABI returns it in two registers, while
 * at 24 bytes each call site got its own stack slot and a level's frame grew by
 * about 1.2 KB. The fields are TypedExpr's, flattened so leaf_kind fills what
 * would otherwise be TypedExpr's padding. Build one with _tagged(). */
typedef struct {
    dvs_expr_t       ref;
    uint16_t         width;
    uint8_t          leaf_kind;  /* 0=complex, 1=var, 2=const */
    Smt2ArrayValue  *array;      /* non-NULL for array-typed results */
} TaggedExpr;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(void *) != 8 || sizeof(TaggedExpr) == 16,
               "TaggedExpr must stay 16 bytes: see the comment above");
#endif

static const TaggedExpr TAGGED_NULL = { EXPR_NULL, 0, 0, NULL };

static inline TaggedExpr _tagged(dvs_expr_t ref, uint32_t width, int leaf_kind,
                                 Smt2ArrayValue *array) {
    TaggedExpr t;
    t.ref       = ref;
    t.width     = (uint16_t)width;
    t.leaf_kind = (uint8_t)leaf_kind;
    t.array     = array;
    return t;
}

static inline TypedExpr _typed(TaggedExpr t) {
    TypedExpr te;
    te.ref   = t.ref;
    te.width = t.width;
    return te;
}

static TaggedExpr _translate_tagged(Smt2Frontend *fe, const Sexpr *s);

static SMT2_NOINLINE int _const_index(Smt2Frontend *fe, const Sexpr *s, uint32_t width,
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
    return _tagged(dvs_builder_expr_var(fe->builder, rv), w, 1, NULL);
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
    if (fe->push_depth > 0 && ai < fe->push[fe->push_depth - 1].n_array_vars) return -1;
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

/* B12: maximum _translate_tagged recursion depth where the stack cannot be
 * measured (where it can, the stack guard is the limit). Without a measured
 * stack the core's passes have no guard either, so the budget covers them
 * too: a conservative 16 KB/level against 70% of the stack, comfortably below
 * the true overflow point however the stack was provisioned. Clamp to a sane range so a
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
 * of recursing into a stack overflow. The limit is the stack the calling
 * thread has left (dvs_stack_low) where the platform can report it, and the
 * depth estimate of _translate_depth_limit elsewhere. The core's passes guard
 * their own recursion, so nesting the translator accepts but they cannot
 * hold comes back "unknown" from them. The
 * counter is balanced (increment paired with decrement on every non-bail path)
 * so it returns to 0 after each top-level translate. */
static TaggedExpr _translate_tagged(Smt2Frontend *fe, const Sexpr *s) {
    if (fe->translate_depth == 0)
        fe->translate_max_depth = _translate_depth_limit();
    if (dvs_stack_deep(&fe->stack_guard, fe->translate_depth,
                       (long)fe->translate_max_depth - 1)) {
        SMT2_TAINT(fe, "expression nesting exceeded the translator's stack");
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
    if (tg.ref == EXPR_NULL) return TAGGED_NULL;
    if (tg.leaf_kind == 1 || tg.leaf_kind == 2) return tg;

    uint32_t aux_id = _fresh_aux(fe, tg.width);
    dvs_expr_t aux_ref = dvs_builder_expr_var(fe->builder, aux_id);
    dvs_expr_t eq = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, aux_ref, tg.ref);
    dvs_builder_add_constraint(fe->builder, eq);

    TaggedExpr result;
    result.ref   = aux_ref;
    result.width = tg.width;
    result.leaf_kind = 1;
    result.array    = NULL;
    return result;
}

/* ------------------------------------------------------------------ */
/* Sort-fun application: build/lookup mangled flat variable           */
/* ------------------------------------------------------------------ */

static SMT2_NOINLINE TaggedExpr _apply_sort_fun(Smt2Frontend *fe, Smt2SortFun *sf,
                                  const Sexpr *call) {
    if (call->list.count != (uint32_t)sf->n_params + 1) {
        _term_error(fe, "arity mismatch for '%s' (expected %u, got %u)",
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
        return _tagged(EXPR_NULL, 0, 0, av->value);
    }

    /* BV/Bool return: look up or create a scalar var. */
    Smt2Var *v = _find_var(fe, mangled, (uint32_t)mlen);
    uint16_t width = sf->return_width ? sf->return_width : 1;
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
    return _tagged(r, v->width, 1, NULL);
}

/* ------------------------------------------------------------------ */
/* define-fun application memo                                         */
/*                                                                     */
/* A body is translated afresh at each call, its arguments by name.    */
/* Yosys output nests calls: a cell's function reads the previous      */
/* cell's function once per bit, so a CRC pipeline expands            */
/* exponentially. A call whose arguments are all names of global      */
/* variables or translated let bindings has the same value wherever    */
/* it is made in one command (a body sees only its parameters and the  */
/* globals, B78), so its translation is kept and reused.               */
/* ------------------------------------------------------------------ */

#define SMT2_MEMO_MAX_ARGS 4

typedef struct Smt2FunMemo {
    const Smt2FunDef *fd;
    uint64_t          keys[SMT2_MEMO_MAX_ARGS];
    uint32_t          gen;
    dvs_expr_t        ref;
    uint16_t          width;
    int               leaf_kind;
} Smt2FunMemo;

/* Forget every memoized application: at each command, and wherever the
 * builder is rewound or reset (the memoized refs would dangle). */
static void _fun_memo_clear(Smt2Frontend *fe) {
    fe->fun_memo_n = 0;
    if (++fe->fun_memo_gen == 0) {       /* wrapped: no stale entry may match */
        if (fe->fun_memo) memset(fe->fun_memo, 0, fe->fun_memo_cap * sizeof *fe->fun_memo);
        fe->fun_memo_gen = 1;
    }
}

/* The identity of argument `a`, translated in environment `env`: a global
 * variable (its id) or a translated let binding (its expression), followed
 * through define-fun parameters. 0 when it is anything else. */
static uint64_t _fun_arg_key(Smt2Frontend *fe, const Sexpr *a, uint32_t env) {
    for (int hops = 0; hops < 256; hops++) {
        if (!a || a->kind != SEXPR_SYMBOL) return 0;
        Smt2Subst *e = _subst_find(fe, env, a->sym.str, a->sym.len);
        if (!e) {
            /* resolved as _translate_symbol_tagged does; true, false and
             * 0-ary functions are not keyed */
            if (_find_fun(fe, a->sym.str, a->sym.len)) return 0;
            Smt2Var *v = _find_var(fe, a->sym.str, a->sym.len);
            return v ? ((uint64_t)v->var_id << 2) | 1u : 0;
        }
        if (e->has_cache)
            return e->cached_ref != EXPR_NULL && !e->cached_array
                 ? ((uint64_t)e->cached_ref << 24) | ((uint64_t)e->cached_width << 8)
                   | ((uint64_t)(e->cached_leaf_kind & 3) << 2) | 2u
                 : 0;
        a = e->value;
        env = e->arg_env;
    }
    return 0;
}

static uint32_t _fun_memo_hash(const Smt2FunDef *fd, const uint64_t *keys, uint32_t n) {
    uint64_t h = (uint64_t)(uintptr_t)fd * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ keys[i]) * 0x100000001B3ull;
    return (uint32_t)(h ^ (h >> 29));
}

static Smt2FunMemo *_fun_memo_slot(Smt2Frontend *fe, const Smt2FunDef *fd,
                                   const uint64_t *keys, uint32_t n) {
    if (!fe->fun_memo_cap) return NULL;
    uint32_t m = fe->fun_memo_cap - 1;
    for (uint32_t i = _fun_memo_hash(fd, keys, n) & m;; i = (i + 1) & m) {
        Smt2FunMemo *e = &fe->fun_memo[i];
        if (e->gen != fe->fun_memo_gen) return e;
        if (e->fd == fd && memcmp(e->keys, keys, n * sizeof keys[0]) == 0) return e;
    }
}

static void _fun_memo_put(Smt2Frontend *fe, const Smt2FunDef *fd,
                          const uint64_t *keys, uint32_t n, TaggedExpr r) {
    if ((fe->fun_memo_n + 1) * 2 > fe->fun_memo_cap) {
        uint32_t cap = fe->fun_memo_cap ? fe->fun_memo_cap * 2 : 1024;
        Smt2FunMemo *old = fe->fun_memo;
        uint32_t ocap = fe->fun_memo_cap, gen = fe->fun_memo_gen;
        Smt2FunMemo *t = (Smt2FunMemo *)calloc(cap, sizeof *t);
        if (!t) return;
        fe->fun_memo = t;
        fe->fun_memo_cap = cap;
        fe->fun_memo_n = 0;
        fe->fun_memo_gen = 1;
        for (uint32_t i = 0; i < ocap; i++) {
            if (old[i].gen != gen) continue;
            Smt2FunMemo *d = _fun_memo_slot(fe, old[i].fd, old[i].keys, SMT2_MEMO_MAX_ARGS);
            *d = old[i];
            d->gen = 1;
            fe->fun_memo_n++;
        }
        free(old);
    }
    Smt2FunMemo *e = _fun_memo_slot(fe, fd, keys, n);
    if (e->gen != fe->fun_memo_gen) fe->fun_memo_n++;
    e->fd = fd;
    memcpy(e->keys, keys, sizeof e->keys);
    e->gen = fe->fun_memo_gen;
    e->ref = r.ref;
    e->width = r.width;
    e->leaf_kind = r.leaf_kind;
}

/* ------------------------------------------------------------------ */
/* define-fun inline expansion                                         */
/* ------------------------------------------------------------------ */

static SMT2_NOINLINE TaggedExpr _apply_fun_def(Smt2Frontend *fe, Smt2FunDef *fd,
                                 const Sexpr *call) {
    if (call->list.count != fd->n_params + 1) {
        _term_error(fe, "arity mismatch for define-fun '%s' (expected %u, got %u)",
                fd->name, fd->n_params, call->list.count - 1);
        return TAGGED_NULL;
    }
    if (!_subst_reserve(fe, fd->n_params)) {
        fprintf(fe->err, "error: out of memory expanding '%s'\n", fd->name);
        return TAGGED_NULL;
    }

    uint64_t keys[SMT2_MEMO_MAX_ARGS] = { 0 };
    static int memo_off = -1;               /* DV_FUN_MEMO=0 turns it off */
    if (memo_off < 0) {
        const char *ev = getenv("DV_FUN_MEMO");
        memo_off = ev && ev[0] == '0';
    }
    int memo = !memo_off && !fd->is_array_return && fd->n_params <= SMT2_MEMO_MAX_ARGS;
    for (uint32_t i = 0; memo && i < fd->n_params; i++)
        memo = (keys[i] = _fun_arg_key(fe, call->list.items[i + 1], fe->subst_env)) != 0;
    if (memo) {
        Smt2FunMemo *e = _fun_memo_slot(fe, fd, keys, SMT2_MEMO_MAX_ARGS);
        if (e && e->gen == fe->fun_memo_gen)
            return _tagged(e->ref, e->width, e->leaf_kind, NULL);
    }

    /* Push bindings. Arguments are translated lazily (call-by-name), which is
     * required so that sort/record-typed arguments -- e.g. a yosys state
     * instance passed to `(define-fun pred ((s S)) ...)` and used inside the
     * body only as `(accessor s)` -- are substituted textually rather than
     * translated bare (a bare record has no expression form).
     *
     * Scoping is lexical (B78): the body sees only its parameters and the
     * globals, never the caller's let names, and each argument is translated
     * in the caller's environment (arg_env), never under the body's lets. */
    uint32_t saved_depth = fe->subst_depth, saved_env = fe->subst_env;
    uint32_t env = 0;
    for (uint32_t i = 0; i < fd->n_params; i++) {
        env = _subst_push(fe, fd->param_names[i], (uint32_t)strlen(fd->param_names[i]), env);
        Smt2Subst *s = &fe->subst_stack[env - 1];
        s->value   = call->list.items[i + 1];
        s->arg_env = saved_env;
    }

    /* Translate body */
    fe->subst_env = env;
    TaggedExpr res = _translate_tagged(fe, fd->body);

    /* Pop bindings */
    fe->subst_depth = saved_depth;
    fe->subst_env   = saved_env;

    if (memo && res.ref != EXPR_NULL && !res.array)
        _fun_memo_put(fe, fd, keys, SMT2_MEMO_MAX_ARGS, res);
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
            return _tagged(arr->store_val, arr->sort.data_width, 0, NULL);
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
    return _tagged(result, arr->sort.data_width, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Symbol translation                                                  */
/* ------------------------------------------------------------------ */

static SMT2_NOINLINE TaggedExpr _translate_symbol_tagged(Smt2Frontend *fe, const Sexpr *s) {
    /* Substitution stack first */
    Smt2Subst *sub_entry = _subst_lookup_entry(fe, s->sym.str, s->sym.len);
    if (sub_entry) {
        if (sub_entry->has_cache && sub_entry->cached_ref != EXPR_NULL) {
            return _tagged(sub_entry->cached_ref, sub_entry->cached_width, sub_entry->cached_leaf_kind, sub_entry->cached_array);
        }
        if (sub_entry->has_cache) {
            /* let-binding: value was pre-translated eagerly; return it directly */
            return _tagged(sub_entry->cached_ref, sub_entry->cached_width, sub_entry->cached_leaf_kind, sub_entry->cached_array);
        }
        /* define-fun parameter: lazily translate the argument expression,
         * in the caller's environment at the call (B78). */
        uint32_t saved_env = fe->subst_env;
        fe->subst_env = sub_entry->arg_env;
        TaggedExpr r = _translate_tagged(fe, sub_entry->value);
        fe->subst_env = saved_env;
        return r;
    }

    if (sexpr_is_symbol(s, "true")) {
        dvs_expr_t r = _bv_const(fe, 1, 1);
        return _tagged(r, 1, 2, NULL);
    }
    if (sexpr_is_symbol(s, "false")) {
        dvs_expr_t r = _bv_const(fe, 0, 1);
        return _tagged(r, 1, 2, NULL);
    }

    /* Zero-arg define-fun? */
    Smt2FunDef *fd = _find_fun(fe, s->sym.str, s->sym.len);
    if (fd && fd->n_params == 0) {
        /* the body sees only globals, not the caller's let names (B78) */
        uint32_t saved_env = fe->subst_env;
        fe->subst_env = 0;
        TaggedExpr r = _translate_tagged(fe, fd->body);
        fe->subst_env = saved_env;
        return r;
    }

    /* BV/Bool variable */
    Smt2Var *v = _find_var(fe, s->sym.str, s->sym.len);
    if (v) {
        dvs_expr_t r = dvs_builder_expr_var(fe->builder, v->var_id);
        return _tagged(r, v->width, 1, NULL);
    }

    /* Array variable */
    Smt2ArrayVar *av = _find_array_var(fe, s->sym.str, s->sym.len);
    if (av) {
        return _tagged(EXPR_NULL, 0, 0, av->value);
    }

    _term_error(fe, "unknown variable '%.*s'",
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
    return _flatten_to_var(fe, _tagged(e, w, 0, NULL)).ref;
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
 * step is its own aux var so the CDCL compile links each one, and CDCL
 * answers all three exactly. (It once left the quotient uncompiled: the
 * compile had no arm for the zero-divisor value `~0`; see _udivrem_expr.)
 * These problems still route to bitblast because model validation evaluates
 * this same lowering, so it is no independent check of it (B69; plan
 * docs/cdcl_construct_plan.md phase 6).
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
    if (t.leaf_kind != 2 || t.array != NULL || t.width == 0 || t.width > 64) return 0;
    const ExprConst *ec = (const ExprConst *)dvs_builder_ref_ptr(fe->builder, t.ref);
    if (!ec || ec->kind != EXPR_CONST) return 0;
    uint64_t m = t.width == 64 ? ~0ull : ((1ull << t.width) - 1);
    *v = (uint64_t)ec->value & m;
    return 1;
}

static TaggedExpr _bool_lit(Smt2Frontend *fe, int v) {
    return _tagged(_bv_const(fe, v ? 1 : 0, 1), 1, 2, NULL);
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
    if (sa.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, smod->list.items[2]));
    if (tb.ref == EXPR_NULL) return TAGGED_NULL;
    uint16_t w = sa.width ? sa.width : tb.width;
    if (w == 0) return TAGGED_NULL;
    dvs_builder_t *b = fe->builder;
    dvs_expr_t mag[2];
    dvs_expr_t ops[2] = { sa.ref, tb.ref };
    for (int i = 0; i < 2; i++) {
        dvs_expr_t msb = _fv(fe, dvs_builder_expr_extract(b, ops[i], w - 1, w - 1), 1);
        mag[i] = _fv(fe, dvs_builder_expr_ite(b, msb, _fv(fe, _neg_expr(fe, ops[i], w), w),
                                              ops[i]), w);
    }
    dvs_expr_t rem = _fv(fe, _udivrem_expr(fe, mag[0], mag[1], w, /*is_rem=*/1), w);
    dvs_expr_t r = dvs_builder_expr_binary(b, DVS_BIN_EQ, rem, _bv_const(fe, 0, w));
    return _tagged(r, 1, 0, NULL);
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
/* Operands of `op` that SMT-LIB requires to have one sort: an ill-sorted term
 * is an input error (z3: "Sorts (_ BitVec 4) and (_ BitVec 8) are
 * incompatible"), never a silent truncation. A bare numeral (width 64 here)
 * and an internal width-0 term are left alone. */
static int _same_width(Smt2Frontend *fe, const char *op, const Sexpr *sa, TaggedExpr a,
                       const Sexpr *sb, TaggedExpr b) {
    if (a.ref == EXPR_NULL || b.ref == EXPR_NULL || a.array || b.array) return 1;
    if (!a.width || !b.width || a.width == b.width) return 1;
    if ((sa && sa->kind == SEXPR_NUMERAL) || (sb && sb->kind == SEXPR_NUMERAL)) return 1;
    _term_error(fe, "%s: sorts (_ BitVec %u) and (_ BitVec %u) are incompatible",
                op, (unsigned)a.width, (unsigned)b.width);
    return 0;
}

/* A term wider than any sort we accept, built by an extension, concat or
 * repeat: the engines' widths are narrower than that, and wrapped (crash on
 * sign_extend to 256 bits), so it is an honest unknown. */
static int _too_wide(Smt2Frontend *fe, uint64_t w) {
    if (w <= SMT2_MAX_BV_BITS) return 0;
    fprintf(fe->err, "error: a %llu-bit term is wider than the %u-bit maximum\n",
            (unsigned long long)w, (unsigned)SMT2_MAX_BV_BITS);
    SMT2_TAINT(fe, "term wider than the maximum bit-vector width");
    return 1;
}

static int _rewrite_text(const char *op, size_t oplen, uint32_t w, uint64_t k,
                         char *buf, size_t n);
static const Sexpr *_rewrite_body(Smt2Frontend *fe, const char *op, size_t oplen,
                                  uint32_t w, uint64_t k);
static TaggedExpr _apply_rewrite(Smt2Frontend *fe, const Sexpr *s, const Sexpr *body);

static TaggedExpr _translate_signed_cmp(Smt2Frontend *fe, const Sexpr *s,
                                        dvs_binop_t binop) {
    if (s->list.count != 3) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
    if (b.ref == EXPR_NULL) return TAGGED_NULL;
    if (!_same_width(fe, "signed compare", s->list.items[1], a, s->list.items[2], b))
        return TAGGED_NULL;
    if (_same_var(fe, a.ref, b.ref))
        return _bool_lit(fe, binop == DVS_BIN_LTE || binop == DVS_BIN_GTE);
    uint16_t sw = a.width ? a.width : b.width;
    if (sw > 64) {                 /* the same identity, as an SMT-LIB rewrite */
        const Sexpr *h = s->list.items[0];
        return _apply_rewrite(fe, s, _rewrite_body(fe, h->sym.str, h->sym.len, sw, 0));
    }
    if (sw == 0) {
        _unsupported(fe, "signed compare of width %u",
                (unsigned)sw);
        return TAGGED_NULL;
    }
    int64_t  bias_val = (int64_t)(1ULL << (sw - 1));
    uint64_t mask     = (sw < 64) ? (((uint64_t)1 << sw) - 1) : ~0ULL;
    dvs_expr_t  bias     = _bv_const(fe, bias_val, sw);

    /* variable side: flip then flatten to a var (materialises the bxor) */
    dvs_expr_t   af  = dvs_builder_expr_binary(fe->builder, DVS_BIN_BXOR, a.ref, bias);
    TaggedExpr aff = _flatten_to_var(fe, _tagged(af, sw, 0, NULL));
    if (aff.ref == EXPR_NULL) return TAGGED_NULL;

    /* rhs: fold the flip when constant, else materialise (var-var -> unknown) */
    dvs_expr_t bref;
    const void *bp = dvs_builder_ref_ptr(fe->builder, b.ref);
    if (bp && *(const ExprKind *)bp == EXPR_CONST) {
        int64_t bval = ((const ExprConst *)bp)->value;
        bref = _bv_const(fe,
                   (int64_t)(((uint64_t)bval ^ (uint64_t)bias_val) & mask), sw);
    } else {
        dvs_expr_t bf = dvs_builder_expr_binary(fe->builder, DVS_BIN_BXOR, b.ref, bias);
        TaggedExpr bff = _flatten_to_var(fe, _tagged(bf, sw, 0, NULL));
        if (bff.ref == EXPR_NULL) return TAGGED_NULL;
        bref = bff.ref;
    }
    dvs_expr_t r = dvs_builder_expr_binary(fe->builder, binop, aff.ref, bref);
    return _tagged(r, 1, 0, NULL);
}

/* A bit-vector constant wider than 64 bits (W2), given as little-endian limbs.
 *
 * EXPR_CONST carries an int64 value, so a wide constant is built from sized
 * <=64-bit chunks through the ordinary expression builders: a zero_extend of
 * the low limb when every higher limb is zero, else a concat of the chunks.
 * The result is tagged complex (leaf_kind 0), never const: the const-fold
 * sites read ExprConst.value as the whole value, which for a wide constant it
 * is not. Like every >64-bit term it is bitblast-only. */
static SMT2_NOINLINE TaggedExpr _wide_const(Smt2Frontend *fe, const uint64_t *limbs,
                                            uint32_t width) {
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
        r = dvs_builder_expr_extend(fe->builder, lo, 64, (uint16_t)width, 0);
    } else {
        r = _bv_const(fe, (int64_t)v[nl - 1], (uint16_t)top_w);
        for (uint32_t i = nl - 1; i-- > 0;)
            r = dvs_builder_expr_concat(fe->builder, r, _bv_const(fe, (int64_t)v[i], 64), 64);
    }
    if (r == EXPR_NULL) return TAGGED_NULL;
    return _tagged(r, (uint16_t)width, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Operators lowered by rewriting                                      */
/* ------------------------------------------------------------------ */

/* The SMT-LIB definition of an operator the translator has no lowering of
 * its own for, written with operators it has, for operand width w (and index
 * k of rotate_left/rotate_right): a term over ?0 and ?1, the operands. Returns
 * the arity, or 0 when `op` is not one of these. */
static int _rewrite_text(const char *op, size_t oplen, uint32_t w, uint64_t k,
                         char *buf, size_t n) {
#define IS(name) (oplen == sizeof(name) - 1 && memcmp(op, name, oplen) == 0)
    char min[64];                         /* the most negative value, 1 then w-1 zeros */
    if (w <= 1) snprintf(min, sizeof min, "#b1");
    else        snprintf(min, sizeof min, "(concat #b1 (_ bv0 %u))", w - 1);
    if (IS("bvnand"))  { snprintf(buf, n, "(bvnot (bvand ?0 ?1))"); return 2; }
    if (IS("bvnor"))   { snprintf(buf, n, "(bvnot (bvor ?0 ?1))"); return 2; }
    if (IS("bvxnor"))  { snprintf(buf, n, "(bvnot (bvxor ?0 ?1))"); return 2; }
    if (IS("bvcomp"))  { snprintf(buf, n, "(ite (= ?0 ?1) #b1 #b0)"); return 2; }
    if (IS("bvredor")) { snprintf(buf, n, "(ite (= ?0 (_ bv0 %u)) #b0 #b1)", w); return 1; }
    if (IS("bvredand")) {
        snprintf(buf, n, "(ite (= (bvnot ?0) (_ bv0 %u)) #b1 #b0)", w); return 1;
    }
    if (IS("bvnego"))  { snprintf(buf, n, "(= ?0 %s)", min); return 1; }
    if (IS("bvusubo")) { snprintf(buf, n, "(bvult ?0 ?1)"); return 2; }
    if (IS("bvuaddo")) {
        snprintf(buf, n, "(= ((_ extract %u %u) (bvadd ((_ zero_extend 1) ?0) "
                 "((_ zero_extend 1) ?1))) #b1)", w, w);
        return 2;
    }
    if (IS("bvsaddo") || IS("bvssubo")) {
        snprintf(buf, n, "(let ((s (%s ((_ sign_extend 1) ?0) ((_ sign_extend 1) ?1)))) "
                 "(distinct ((_ extract %u %u) s) ((_ extract %u %u) s)))",
                 IS("bvsaddo") ? "bvadd" : "bvsub", w, w, w - 1, w - 1);
        return 2;
    }
    if (IS("bvumulo")) {
        snprintf(buf, n, "(distinct ((_ extract %u %u) (bvmul ((_ zero_extend %u) ?0) "
                 "((_ zero_extend %u) ?1))) (_ bv0 %u))", 2 * w - 1, w, w, w, w);
        return 2;
    }
    if (IS("bvsmulo")) {
        snprintf(buf, n, "(let ((p (bvmul ((_ sign_extend %u) ?0) ((_ sign_extend %u) ?1)))) "
                 "(distinct p ((_ sign_extend %u) ((_ extract %u 0) p))))", w, w, w, w - 1);
        return 2;
    }
    if (IS("bvsdivo")) {
        snprintf(buf, n, "(and (= ?0 %s) (= ?1 (bvnot (_ bv0 %u))))", min, w);
        return 2;
    }
    if (IS("rotate_left") || IS("rotate_right")) {
        uint64_t r = w ? k % w : 0;
        if (IS("rotate_right") && r) r = w - r;           /* as a left rotation */
        if (!r) snprintf(buf, n, "?0");
        else    snprintf(buf, n, "(let ((a ?0)) (concat ((_ extract %u 0) a) "
                         "((_ extract %u %u) a)))", (unsigned)(w - r - 1), w - 1,
                         (unsigned)(w - r));
        return 1;
    }
    /* signed compares wider than 64 bits: the MSB-flip identity */
    if (IS("bvslt") || IS("bvsle") || IS("bvsgt") || IS("bvsge")) {
        snprintf(buf, n, "(bvu%.2s (bvxor ?0 %s) (bvxor ?1 %s))", op + 3, min, min);
        return 2;
    }
#undef IS
    return 0;
}

/* Translate `s`, an application of an operator _rewrite_text defines, as
 * that definition: like a define-fun call, the operands are bound to ?0 and
 * ?1 and translated in the caller's environment. */
/* The parsed definition of operator `op` at operand width `w` (index `k`),
 * cached per text; NULL on failure (tainted). Out of line, with the text
 * buffer, so the recursion in _apply_rewrite does not carry it. */
static SMT2_NOINLINE const Sexpr *_rewrite_body(Smt2Frontend *fe, const char *op,
                                                size_t oplen, uint32_t w, uint64_t k) {
    char text[512];
    _rewrite_text(op, oplen, w, k, text, sizeof text);
    const Sexpr *body = NULL;
    for (uint32_t i = 0; i < fe->n_rewrites && !body; i++)
        if (strcmp(fe->rewrites[i].text, text) == 0) body = fe->rewrites[i].body;
    if (!body) {
        /* A cached template outlives a soft reset, so it lives in
         * rewrite_arena; once the cache is full, a template is parsed per use
         * into persistent_arena. */
        SexprArena *a = fe->n_rewrites < SMT2_MAX_REWRITES ? &fe->rewrite_arena
                                                           : &fe->persistent_arena;
        size_t len = strlen(text);
        char *t = (char *)sexpr_arena_alloc(a, len + 1, 1);
        if (!t) return NULL;
        memcpy(t, text, len + 1);
        Smt2Lexer lex;
        smt2_lexer_init(&lex, t, len);
        body = sexpr_parse(&lex, a);
        if (!body) {
            fprintf(fe->err, "internal error: rewrite '%s' does not parse\n", t);
            SMT2_TAINT(fe, "internal error in an operator rewrite");
            return NULL;
        }
        if (fe->n_rewrites < SMT2_MAX_REWRITES) {
            fe->rewrites[fe->n_rewrites].text = t;
            fe->rewrites[fe->n_rewrites++].body = body;
        }
    }
    return body;
}

/* Translate `s` through a rewrite definition, its ?0 / ?1 bound to the
 * operands. */
static TaggedExpr _apply_rewrite(Smt2Frontend *fe, const Sexpr *s, const Sexpr *body) {
    static const char *const params[2] = { "?0", "?1" };
    if (!body) return TAGGED_NULL;
    uint32_t n = s->list.count - 1;
    if (n > 2 || !_subst_reserve(fe, n)) return TAGGED_NULL;
    uint32_t saved_depth = fe->subst_depth, saved_env = fe->subst_env, env = 0;
    for (uint32_t i = 0; i < n; i++) {
        env = _subst_push(fe, params[i], 2, env);
        fe->subst_stack[env - 1].value   = s->list.items[i + 1];
        fe->subst_stack[env - 1].arg_env = saved_env;
    }
    fe->subst_env = env;
    TaggedExpr r = _translate_tagged(fe, body);
    fe->subst_depth = saved_depth;
    fe->subst_env   = saved_env;
    return r;
}

/* An operator _rewrite_text defines (indexed: k is its index): check the
 * arity, find the operand width, translate the definition. NULL `handled`
 * result: not one of them. */
/* The arity of the operator _rewrite_text defines as `op`, or 0. */
static SMT2_NOINLINE int _rewrite_arity(const char *op, size_t oplen, uint64_t k) {
    char text[512];
    return _rewrite_text(op, oplen, 1, k, text, sizeof text);
}

static TaggedExpr _try_rewrite(Smt2Frontend *fe, const Sexpr *s, const char *op,
                               size_t oplen, uint64_t k, int *handled) {
    int arity = _rewrite_arity(op, oplen, k);
    *handled = arity > 0;
    if (!arity) return TAGGED_NULL;
    if (s->list.count != (uint32_t)arity + 1) {
        _term_error(fe, "arity mismatch for '%.*s' (expected %d, got %u)",
                    (int)oplen, op, arity, s->list.count - 1);
        return TAGGED_NULL;
    }
    TaggedExpr a = _translate_tagged(fe, s->list.items[1]);   /* for its width */
    if (a.ref == EXPR_NULL || a.array) return TAGGED_NULL;
    return _apply_rewrite(fe, s, _rewrite_body(fe, op, oplen, a.width, k));
}

/* Re-read the `bvN` of `(_ bvN W)` exactly, for W > 64 or N >= 2^64 (mod
 * 2^128 is exact mod 2^W for every accepted W). Returns 1 with the wide
 * constant in *wide when W > 64, else 0 with the low 64 bits in *val. */
static SMT2_NOINLINE int _reread_bv_sym(Smt2Frontend *fe, const Sexpr *op, uint64_t wn,
                                        uint64_t *val, TaggedExpr *wide) {
    uint64_t limbs[SMT2_BV_MAX_LIMBS];
    _parse_bv_sym_limbs(op, limbs, SMT2_BV_MAX_LIMBS);
    if (wn > 64) { *wide = _wide_const(fe, limbs, (uint32_t)wn); return 1; }
    *val = limbs[0];
    return 0;
}

/* (_ bvN W) */
static SMT2_NOINLINE TaggedExpr _tr_bv_lit(Smt2Frontend *fe, const Sexpr *s) {
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
            TaggedExpr wide;
            if (_reread_bv_sym(fe, op, wn, &bv_val, &wide)) return wide;
        }
        /* The value is N mod 2^W (as z3 reads it). An unreduced N >= 2^W
         * made the CDCL engine answer a wrong `unsat` on e.g.
         * `(= x8 (_ bv300 8))`; reduce it here, for every width. */
        if (wn > 0 && wn < 64) bv_val &= ((uint64_t)1 << wn) - 1;
        uint16_t w = (uint16_t)wn;
        dvs_expr_t r = _bv_const(fe, (int64_t)bv_val, w);
        return _tagged(r, w, 2, NULL);
    }
    _term_error(fe, "unexpected indexed identifier");
    return TAGGED_NULL;
}

/* ((_ zero_extend N) expr) / ((_ sign_extend N) expr) / ((_ extract hi lo) expr) */
static SMT2_NOINLINE TaggedExpr _tr_indexed_app(Smt2Frontend *fe, const Sexpr *s) {
    Sexpr *head = s->list.items[0];
    Sexpr *op_sym = head->list.items[1];

    if (sexpr_is_symbol(op_sym, "zero_extend") ||
        sexpr_is_symbol(op_sym, "sign_extend")) {
        if (s->list.count != 2) return TAGGED_NULL;
        uint8_t sign = sexpr_is_symbol(op_sym, "sign_extend") ? 1 : 0;
        uint64_t ext_n = head->list.items[2]->numval;

        TaggedExpr inner = _translate_tagged(fe, s->list.items[1]);
        if (inner.ref == EXPR_NULL) return TAGGED_NULL;
        if (_too_wide(fe, (uint64_t)inner.width + ext_n)) return TAGGED_NULL;
        uint16_t new_width = inner.width + (uint16_t)ext_n;

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
            ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, inner.ref);
            uint64_t v = (uint64_t)ec->value & (((uint64_t)1 << new_width) - 1);
            dvs_expr_t cr = _bv_const(fe, (int64_t)v, new_width);
            return _tagged(cr, new_width, 2, NULL);
        }

        inner = _flatten_to_var(fe, inner);
        if (inner.ref == EXPR_NULL) return TAGGED_NULL;
        dvs_expr_t r = dvs_builder_expr_extend(fe->builder, inner.ref,
                                        (uint16_t)inner.width,
                                        new_width, sign);
        return _tagged(r, new_width, 0, NULL);
    }

    if (sexpr_is_symbol(op_sym, "extract")) {
        if (s->list.count != 2 || head->list.count != 4) return TAGGED_NULL;
        uint64_t hi = head->list.items[2]->numval;
        uint64_t lo = head->list.items[3]->numval;

        TaggedExpr inner = _translate_tagged(fe, s->list.items[1]);
        inner = _flatten_to_var(fe, inner);
        if (inner.ref == EXPR_NULL) return TAGGED_NULL;
        if (lo > hi || (inner.width && hi >= inner.width)) {
            _term_error(fe, "extract %llu %llu of a %u-bit term",
                        (unsigned long long)hi, (unsigned long long)lo,
                        (unsigned)inner.width);
            return TAGGED_NULL;
        }

        dvs_expr_t r = dvs_builder_expr_extract(fe->builder, inner.ref,
                                                (uint16_t)hi, (uint16_t)lo);
        return _tagged(r, (uint16_t)(hi - lo + 1), 0, NULL);
    }

    /* ((_ repeat N) expr) = expr concatenated with itself N times. */
    if (sexpr_is_symbol(op_sym, "repeat")) {
        if (s->list.count != 2) return TAGGED_NULL;
        uint64_t rep_n = head->list.items[2]->numval;
        if (rep_n == 0) return TAGGED_NULL;
        TaggedExpr inner = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
        if (inner.ref == EXPR_NULL) return TAGGED_NULL;
        uint16_t w = inner.width;
        if (_too_wide(fe, (uint64_t)w * rep_n)) return TAGGED_NULL;
        dvs_expr_t acc = inner.ref;              /* N==1 is the identity */
        for (uint64_t i = 1; i < rep_n; i++)
            acc = dvs_builder_expr_concat(fe->builder, acc, inner.ref, w);
        return _tagged(acc, (uint16_t)(w * rep_n), 0, NULL);
    }

    if (op_sym->kind == SEXPR_SYMBOL && head->list.count == 3 &&
        head->list.items[2]->kind == SEXPR_NUMERAL) {
        int handled;
        TaggedExpr r = _try_rewrite(fe, s, op_sym->sym.str, op_sym->sym.len,
                                    head->list.items[2]->numval, &handled);
        if (handled) return r;
    }
    if (op_sym->kind == SEXPR_SYMBOL)
        _unsupported(fe, "indexed operator '%.*s'", (int)op_sym->sym.len, op_sym->sym.str);
    else
        _unsupported(fe, "indexed operator");
    return TAGGED_NULL;
}

/* ((as const (Array M N)) value) */
static SMT2_NOINLINE TaggedExpr _tr_as_const(Smt2Frontend *fe, const Sexpr *s) {
    Sexpr *head = s->list.items[0];
    Smt2ArraySort sort;
    int r = _parse_array_sort(fe, head->list.items[2], &sort);
    if (r <= 0) {
        _term_error(fe, "(as const ...) requires valid Array sort");
        return TAGGED_NULL;
    }
    if (s->list.count != 2) return TAGGED_NULL;

    TaggedExpr val_te = _translate_tagged(fe, s->list.items[1]);
    if (val_te.ref == EXPR_NULL || val_te.array != NULL) {
        if (val_te.array) _term_error(fe, "(as const ...) value must be a BV");   /* else: already reported */
        return TAGGED_NULL;
    }

    if (_abstract_sort(fe, sort)) {
        val_te = _flatten_to_var(fe, val_te);
        Smt2ArrayValue *arr = _anode_new(fe, SMT2_ANODE_CONST, sort);
        if (!arr) { SMT2_TAINT(fe, "abstract const-array node allocation failed"); return TAGGED_NULL; }
        arr->store_val = val_te.ref;   /* every read == default */
        return _tagged(EXPR_NULL, 0, 0, arr);
    }

    Smt2ArrayValue *arr = _make_array_value(fe, sort);
    if (!arr) return TAGGED_NULL;

    for (uint32_t i = 0; i < arr->n_elems; i++)
        arr->elems[i] = val_te.ref;

    return _tagged(EXPR_NULL, 0, 0, arr);
}

/* (let ((x1 e1) (x2 e2) ...) body) — parallel binding.
 * SMT-LIB2 parallel semantics: all ei are evaluated in the CURRENT scope
 * before any xi binding takes effect.  We pre-translate eagerly. */
/* Phase 1 of a let: evaluate ALL value expressions in the current (outer)
 * scope before any binding takes effect -- true parallel SMT-LIB2 semantics --
 * then push the bindings (fe->subst_env). Small lets use an inline buffer;
 * larger ones heap-allocate. Keeping this off a fixed SMT2_MAX_SUBST-sized
 * stack array, and out of _tr_let's frame, is what lets deeply nested `let`
 * recurse (phase 2) cheaply. 0 on failure, reported. */
static SMT2_NOINLINE int _let_bind(Smt2Frontend *fe, const Sexpr *binds) {
    uint32_t n = binds->list.count;
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
    if (!_subst_reserve(fe, n)) {
        if (let_heap) { free(let_vals); free(let_names); }
        goto let_oom;
    }
    uint32_t env = fe->subst_env;
    for (uint32_t i = 0; i < n; i++) {
        env = _subst_push(fe, let_names[i]->sym.str, let_names[i]->sym.len, env);
        Smt2Subst *e        = &fe->subst_stack[env - 1];
        e->cached_ref       = let_vals[i].ref;
        e->cached_width     = let_vals[i].width;
        e->cached_leaf_kind = let_vals[i].leaf_kind;
        e->cached_array     = let_vals[i].array;
        e->has_cache        = 1;
    }
    fe->subst_env = env;
    /* Values are copied into the subst stack; release the temp buffers
     * BEFORE the (possibly very deep) phase-2 recursion. */
    if (let_heap) { free(let_vals); free(let_names); }
    return 1;
let_bad:
    if (let_heap) { free(let_vals); free(let_names); }
    _term_error(fe, "malformed let binding");
    return 0;
let_oom:
    fprintf(fe->err, "error: out of memory in let (%u bindings)\n", n);
    return 0;
}

static SMT2_NOINLINE TaggedExpr _tr_let(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 3) {
        _term_error(fe, "malformed let (expected 3 elements)");
        return TAGGED_NULL;
    }
    const Sexpr *binds = s->list.items[1];
    const Sexpr *body  = s->list.items[2];
    if (binds->kind != SEXPR_LIST) {
        _term_error(fe, "let: binding list must be a list");
        return TAGGED_NULL;
    }
    uint32_t saved = fe->subst_depth, saved_env = fe->subst_env;
    TaggedExpr res = TAGGED_NULL;
    /* Phase 2: translate body with all bindings now in scope. */
    if (_let_bind(fe, binds)) res = _translate_tagged(fe, body);
    fe->subst_depth = saved;
    fe->subst_env   = saved_env;
    return res;
}

/* (select A i) */
static SMT2_NOINLINE TaggedExpr _tr_select(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 3) return TAGGED_NULL;

    TaggedExpr arr_te = _translate_tagged(fe, s->list.items[1]);
    if (arr_te.array == NULL) {
        if (arr_te.ref != EXPR_NULL) _term_error(fe, "select: first argument is not an array");   /* else: already reported */
        return TAGGED_NULL;
    }
    Smt2ArrayValue *arr = arr_te.array;

    TaggedExpr idx_te = _translate_tagged(fe, s->list.items[2]);
    if (idx_te.ref == EXPR_NULL || idx_te.array != NULL) {
        if (idx_te.array) _term_error(fe, "select: index must be a BV");   /* else: already reported */
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
            return _tagged(e, arr->sort.data_width, 1, NULL);
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
        return _tagged(EXPR_NULL, 0, 0, sl);
    }

    /* Word-level abstract path (DV_ARRAY): const or symbolic index alike
     * become a fresh read var; read-over-write / congruence deferred. */
    if (arr->is_abstract) {
        idx_te = _flatten_to_var(fe, idx_te);
        if (idx_te.ref == EXPR_NULL) { SMT2_TAINT(fe, "abstract-array select index not flattenable to a var"); return TAGGED_NULL; }
        return _abs_select(fe, arr, idx_te.ref);
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
            return _tagged(e, arr->sort.data_width, 1, NULL);
        }
        if (k_hi == 0 && k < arr->n_elems)
            return _tagged(arr->elems[k], arr->sort.data_width, 1, NULL);
    }

    /* Symbolic index. A sparse (large-address) array cannot resolve one
     * without enumerating 2^M entries: promote it to the word-level engine,
     * or, where that is not possible, an honest unknown rather than a wrong
     * answer. Dense arrays lower to an ITE tree over their elements. */
    if (arr->is_sparse && _promote_sparse(fe, arr) == 0) {
        idx_te = _flatten_to_var(fe, idx_te);
        if (idx_te.ref == EXPR_NULL) { SMT2_TAINT(fe, "abstract-array select index not flattenable to a var"); return TAGGED_NULL; }
        return _abs_select(fe, arr, idx_te.ref);
    }
    if (arr->is_sparse) {
        fprintf(fe->err, "error: symbolic index into a large (sparse) array "
                         "is unsupported -> result will be unknown\n");
        SMT2_TAINT(fe, "symbolic index into a large (sparse) array");
        return TAGGED_NULL;
    }
    idx_te = _flatten_to_var(fe, idx_te);
    return _array_select(fe, arr, idx_te.ref);
}

/* (store A i v) */
static SMT2_NOINLINE TaggedExpr _tr_store(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 4) return TAGGED_NULL;

    TaggedExpr arr_te = _translate_tagged(fe, s->list.items[1]);
    if (arr_te.array == NULL) {
        if (arr_te.ref != EXPR_NULL) _term_error(fe, "store: first argument is not an array");   /* else: already reported */
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
        if (idx_te.ref == EXPR_NULL || idx_te.array != NULL) {
            if (idx_te.array) _term_error(fe, "store: index must be a BV");   /* else: already reported */
            return TAGGED_NULL;
        }
        TaggedExpr val_te = _translate_tagged(fe, s->list.items[3]);
        if (val_te.ref == EXPR_NULL || val_te.array != NULL) {
            if (val_te.array) _term_error(fe, "store: value must be a BV");   /* else: already reported */
            return TAGGED_NULL;
        }
        idx_te = _flatten_to_var(fe, idx_te);
        val_te = _flatten_to_var(fe, val_te);
        Smt2ArrayValue *node = _anode_store(fe, arr, idx_te.ref,
                                            val_te.ref);
        if (!node) { SMT2_TAINT(fe, "abstract-array store node allocation failed"); return TAGGED_NULL; }
        return _tagged(EXPR_NULL, 0, 0, node);
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
    if (idx_te.ref == EXPR_NULL || idx_te.array != NULL) {
        if (idx_te.array) _term_error(fe, "store: index must be a BV");   /* else: already reported */
        return TAGGED_NULL;
    }

    TaggedExpr val_te = _translate_tagged(fe, s->list.items[3]);
    if (val_te.ref == EXPR_NULL || val_te.array != NULL) {
        if (val_te.array) _term_error(fe, "store: value must be a BV");   /* else: already reported */
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
                new_arr->elems[k] = val_te.ref;
            return _tagged(EXPR_NULL, 0, 0, new_arr);
        }
    }

    /* Symbolic index: elementwise ITE.  Record R1 metadata (store index
     * var_id) so a later select at the same index can short-circuit. */
    idx_te = _flatten_to_var(fe, idx_te);
    val_te = _flatten_to_var(fe, val_te);
    {
        ExprVar *ev = (ExprVar *)dvs_builder_ref_ptr(fe->builder, idx_te.ref);
        if (ev && ev->kind == EXPR_VAR) {
            new_arr->store_idx_varid = ev->var_id;
            new_arr->store_val       = val_te.ref;
        }
    }
    for (uint32_t j = 0; j < arr->n_elems; j++) {
        dvs_expr_t j_const = _bv_const(fe, (int64_t)j, arr->sort.addr_width);
        dvs_expr_t cond = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ,
                                           idx_te.ref, j_const);
        new_arr->elems[j] = dvs_builder_expr_ite(fe->builder, cond,
                                              val_te.ref, arr->elems[j]);
    }
    return _tagged(EXPR_NULL, 0, 0, new_arr);
}

/* Binary BV arithmetic (result has same width as operands). These ops are
 * :left-assoc in SMT-LIB, and Verilator emits them variadically (e.g. (bvor a
 * b c)). Left-fold over all operands so both the binary and n-ary forms
 * translate: (op a b c) == (op (op a b) c).
 *
 * bvand / bvor with literal operands: drop identities, short-circuit on the
 * absorbing value, then fold the rest as _tr_binop does. */
static SMT2_NOINLINE TaggedExpr _tr_band_bor(Smt2Frontend *fe, const Sexpr *s, int is_band) {
    TaggedExpr acc = TAGGED_NULL;
    uint16_t w = 0;
    for (uint32_t i = 1; i < s->list.count; i++) {
        TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
        if (b.ref == EXPR_NULL || b.array != NULL) return TAGGED_NULL;
        if (w && b.width && b.width != w
                && s->list.items[i]->kind != SEXPR_NUMERAL) {
            _term_error(fe, "%s: sorts (_ BitVec %u) and (_ BitVec %u) are incompatible",
                        is_band ? "bvand" : "bvor", (unsigned)w, (unsigned)b.width);
            return TAGGED_NULL;
        }
        if (!w) w = b.width;
        uint64_t v, ones = w >= 64 ? ~0ull : ((1ull << w) - 1);
        if (b.width == w && _lit_value(fe, b, &v)) {
            if (v == (is_band ? 0 : ones))
                return _tagged(_bv_const(fe, (int64_t)v, w), w, 2, NULL);
            if (v == (is_band ? ones : 0)) continue;
        }
        b = _flatten_to_var(fe, b);
        if (b.ref == EXPR_NULL) return TAGGED_NULL;
        if (acc.ref == EXPR_NULL) { acc = b; continue; }
        acc = _flatten_to_var(fe, acc);
        if (acc.ref == EXPR_NULL) return TAGGED_NULL;
        acc.ref = dvs_builder_expr_binary(fe->builder,
                                             is_band ? DVS_BIN_BAND : DVS_BIN_BOR,
                                             acc.ref, b.ref);
        acc.leaf_kind = 0;
    }
    if (acc.ref == EXPR_NULL) {     /* every operand was the identity */
        uint64_t id = is_band ? (w >= 64 ? ~0ull : ((1ull << w) - 1)) : 0;
        return _tagged(_bv_const(fe, (int64_t)id, w), w, 2, NULL);
    }
    _flag_wide_arith(fe, w);
    return _tagged(acc.ref, w, acc.leaf_kind, NULL);
}

/* (op a b ...) for a left-assoc BV operator `name`, folded left. */
static SMT2_NOINLINE TaggedExpr _tr_binop(Smt2Frontend *fe, const Sexpr *s, const char *name,
                                           dvs_binop_t binop) {
    if (s->list.count < 3) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    for (uint32_t _i = 2; _i < s->list.count; _i++) {
        TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[_i]));
        if (b.ref == EXPR_NULL) return TAGGED_NULL;
        if (!_same_width(fe, name, s->list.items[1], a, s->list.items[_i], b))
            return TAGGED_NULL;
        /* Re-flatten the accumulator each round. Without this, from the 2nd
         * operand on the left side is a raw composed node, and the CDCL
         * compile only links var-var / const-var operands -- so an n-ary op
         * with arity >= 3 was left UNCOMPILED, the model then violated it,
         * and validation downgraded a perfectly good `sat` to `unknown`.
         * That silently disabled `x inside {a,b,c}` (a 3-way bvor) on CDCL.
         * Same lesson as B4: never reuse a raw composed sub-expr. */
        a = _flatten_to_var(fe, a);
        if (a.ref == EXPR_NULL) return TAGGED_NULL;
        a.ref = dvs_builder_expr_binary(fe->builder, binop, a.ref, b.ref);
        a.leaf_kind = 0;   /* composed: must be re-flattened next round */
    }
    _flag_wide_arith(fe, a.width);
    return _tagged(a.ref, a.width, 0, NULL);
}

/* (op a b) for an unsigned comparison `name`. */
static SMT2_NOINLINE TaggedExpr _tr_cmpop(Smt2Frontend *fe, const Sexpr *s, const char *name,
                                           dvs_binop_t binop) {
    if (s->list.count != 3) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
    if (b.ref == EXPR_NULL) return TAGGED_NULL;
    if (!_same_width(fe, name, s->list.items[1], a, s->list.items[2], b))
        return TAGGED_NULL;
    if (_same_var(fe, a.ref, b.ref))
        return _bool_lit(fe, binop == DVS_BIN_LTE || binop == DVS_BIN_GTE);
    dvs_expr_t r = dvs_builder_expr_binary(fe->builder, binop, a.ref, b.ref);
    return _tagged(r, 1, 0, NULL);
}

/* Unsigned division and remainder. SMT-LIB defines a zero divisor:
 * (bvudiv a 0) = all ones, (bvurem a 0) = a. The engines' own DIV/MOD
 * leave the result unconstrained at a zero divisor (SystemVerilog's x),
 * so unless the divisor is a nonzero constant, guard it here; without the
 * guard a model could pick any value and `unsat` problems came back `sat`
 * (B52). */
static SMT2_NOINLINE TaggedExpr _tr_udivrem(Smt2Frontend *fe, const Sexpr *s, int is_urem) {
    if (s->list.count != 3) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr b = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
    if (b.ref == EXPR_NULL) return TAGGED_NULL;
    if (!_same_width(fe, is_urem ? "bvurem" : "bvudiv", s->list.items[1], a,
                     s->list.items[2], b))
        return TAGGED_NULL;
    uint16_t w = a.width ? a.width : b.width;
    _flag_wide_arith(fe, w);
    dvs_expr_t q = _udivrem_expr(fe, a.ref, b.ref, w, is_urem);
    return _tagged(q, w, 0, NULL);
}

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
static SMT2_NOINLINE TaggedExpr _tr_sdivrem(Smt2Frontend *fe, const Sexpr *s, int is_srem) {
    /* The ITE-of-unsigned-divisions this lowers to is solved exactly by
     * bitblast but left unpinned (wrong model) by the CDCL bounds
     * engine, so force bitblast for this problem. */
    { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "sdiv-srem"; }
    if (s->list.count != 3) return TAGGED_NULL;
    TaggedExpr sa = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (sa.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
    if (tb.ref == EXPR_NULL) return TAGGED_NULL;
    uint16_t w = sa.width ? sa.width : tb.width;
    if (w == 0) return TAGGED_NULL;
    dvs_expr_t r = _signed_divrem_expr(fe, sa.ref, tb.ref,
                                    w, is_srem);
    return _tagged(r, w, 0, NULL);
}

/* Signed modulo (SMT-LIB `bvsmod`): result sign follows the DIVISOR t.
 * Built on the (validated) signed remainder: let r = bvsrem(s,t);
 *   r == 0                 -> 0
 *   sign(r) == sign(t)     -> r
 *   else                   -> r + t   (flip the sign toward the divisor)
 */
static SMT2_NOINLINE TaggedExpr _tr_smod(Smt2Frontend *fe, const Sexpr *s) {
    { fe->needs_bitblast = 1; if (!fe->nb_why) fe->nb_why = "smod"; }
    if (s->list.count != 3) return TAGGED_NULL;
    TaggedExpr sa = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (sa.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr tb = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
    if (tb.ref == EXPR_NULL) return TAGGED_NULL;
    uint16_t w = sa.width ? sa.width : tb.width;
    if (w == 0) return TAGGED_NULL;
    dvs_expr_t T_ = tb.ref;
    dvs_expr_t r_expr = _signed_divrem_expr(fe, sa.ref, T_, w, /*is_rem=*/1);
    /* Materialise the remainder into its own var: bvsmod references it
     * three times (sign bit, +t, ==0), and re-embedding the raw ITE tree
     * at each use makes the bitblaster produce inconsistent values for
     * the shared sub-DAG. One aux var = one bit-blasted value, shared. */
    TaggedExpr rt = _flatten_to_var(fe, _tagged(r_expr, w, 0, NULL));
    if (rt.ref == EXPR_NULL) return TAGGED_NULL;
    dvs_expr_t r = rt.ref;
    dvs_expr_t msb_r = dvs_builder_expr_extract(fe->builder, r, w - 1, w - 1);
    dvs_expr_t msb_t = dvs_builder_expr_extract(fe->builder, T_, w - 1, w - 1);
    dvs_expr_t same_sign = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, msb_r, msb_t);
    dvs_expr_t r_plus_t = dvs_builder_expr_binary(fe->builder, DVS_BIN_ADD, r, T_);
    dvs_expr_t adjusted = dvs_builder_expr_ite(fe->builder, same_sign, r, r_plus_t);
    dvs_expr_t zero = _bv_const(fe, 0, w);
    dvs_expr_t r_is_zero = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, r, zero);
    dvs_expr_t result = dvs_builder_expr_ite(fe->builder, r_is_zero, r, adjusted);
    return _tagged(result, w, 0, NULL);
}

/* = (equality) -- handles both BV and array operands */
static SMT2_NOINLINE TaggedExpr _tr_eq(Smt2Frontend *fe, const Sexpr *s) {
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
            return _tagged(p, 1, 1, NULL);
        }
        /* Array equality: AND of element-wise equalities */
        if (a.array->n_elems != b.array->n_elems) {
            _term_error(fe, "array equality on arrays with different sizes");
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
        return _tagged(result, 1, 0, NULL);
    }
    if (a.array != NULL || b.array != NULL) {
        /* a side that failed to translate has already been reported */
        if (a.ref != EXPR_NULL || b.ref != EXPR_NULL)
            _term_error(fe, "type mismatch in equality (array vs scalar)");
        return TAGGED_NULL;
    }

    /* BV/Bool equality */
    if (!_same_width(fe, "=", s->list.items[1], a, s->list.items[2], b)) return TAGGED_NULL;
    uint64_t av, bv;
    if (_lit_value(fe, a, &av) && _lit_value(fe, b, &bv) && a.width == b.width)
        return _bool_lit(fe, av == bv);
    a = _flatten_to_var(fe, a);
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    b = _flatten_to_var(fe, b);
    if (b.ref == EXPR_NULL) return TAGGED_NULL;
    dvs_expr_t r = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ, a.ref, b.ref);
    return _tagged(r, 1, 0, NULL);
}

/* distinct */
static SMT2_NOINLINE TaggedExpr _tr_distinct(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count < 3) return TAGGED_NULL;
    uint32_t n = s->list.count - 1;
    TaggedExpr *args = (TaggedExpr *)malloc(n * sizeof(TaggedExpr));
    if (!args) return TAGGED_NULL;
    for (uint32_t i = 0; i < n; i++) {
        args[i] = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[i + 1]));
        if (args[i].ref == EXPR_NULL) { free(args); return TAGGED_NULL; }
        if (i && !_same_width(fe, "distinct", s->list.items[1], args[0],
                              s->list.items[i + 1], args[i])) {
            free(args);
            return TAGGED_NULL;
        }
    }
    dvs_expr_t result = EXPR_NULL;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            dvs_expr_t neq = dvs_builder_expr_binary(fe->builder, DVS_BIN_NEQ,
                                              args[i].ref, args[j].ref);
            if (result == EXPR_NULL) result = neq;
            else result = dvs_builder_expr_binary(fe->builder, DVS_BIN_AND, result, neq);
        }
    }
    free(args);
    return _tagged(result, 1, 0, NULL);
}

/* bvnot */
static SMT2_NOINLINE TaggedExpr _tr_bvnot(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 2) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    dvs_expr_t r = dvs_builder_expr_unary(fe->builder, DVS_UN_INVERT, a.ref);
    _flag_wide_arith(fe, a.width);
    return _tagged(r, a.width, 0, NULL);
}

/* bvneg */
static SMT2_NOINLINE TaggedExpr _tr_bvneg(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 2) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    /* bvneg x is (bvsub 0 x), modulo 2^w. It must NOT lower to DVS_UN_NEG:
     * the CDCL engine (and the model validator) treat DVS_UN_NEG as integer
     * negation with no wrap, so for an unsigned x in [0, 2^w) the result
     * lands in (-2^w, 0] and `(= (bvneg x) K)` was a spurious `unsat` for
     * every constant K. Built exactly as BINOP_CASE builds bvsub. */
    TaggedExpr z = _flatten_to_var(fe, _tagged(_bv_const(fe, 0, a.width), a.width, 2, NULL));
    if (z.ref == EXPR_NULL) return TAGGED_NULL;
    dvs_expr_t r = dvs_builder_expr_binary(fe->builder, DVS_BIN_SUB, z.ref, a.ref);
    _flag_wide_arith(fe, a.width);
    return _tagged(r, a.width, 0, NULL);
}

/* Boolean connectives, with compile-time const folding: not */
static SMT2_NOINLINE TaggedExpr _tr_not(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 2) return TAGGED_NULL;
    TaggedExpr a = _translate_tagged(fe, s->list.items[1]);
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    if (a.leaf_kind == 2) {
        ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, a.ref);
        int64_t neg = (ec->value != 0) ? 0 : 1;
        dvs_expr_t cr = _bv_const(fe, neg, 1);
        return _tagged(cr, 1, 2, NULL);
    }
    dvs_expr_t r = dvs_builder_expr_unary(fe->builder, DVS_UN_NOT, a.ref);
    return _tagged(r, 1, 0, NULL);
}

/* and */
static SMT2_NOINLINE TaggedExpr _tr_and(Smt2Frontend *fe, const Sexpr *s) {
    /* SMT-LIB permits arity >= 1 here; `(and x)` / `(or x)` / `(xor x)` are
     * the identity and z3 accepts them (nullary `(and)` it rejects, so we do
     * too). Rejecting the unary form used to taint the assert -> `unknown`,
     * which silently killed any single-variable model-enumeration loop --
     * a blocking clause over one variable is exactly `(not (and (= x V)))`.
     * See docs/solver_bug_backlog.md B16. */
    if (s->list.count < 2) return TAGGED_NULL;
    /* Collect non-trivial operands; short-circuit on any literal false. */
    TaggedExpr acc = _tagged(EXPR_NULL, 1, 0, NULL);
    for (uint32_t i = 1; i < s->list.count; i++) {
        TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
        if (b.ref == EXPR_NULL) return TAGGED_NULL;
        if (b.leaf_kind == 2) {
            ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, b.ref);
            if (ec->value == 0) {
                /* (and ... false ...) -> false */
                dvs_expr_t cr = _bv_const(fe, 0, 1);
                return _tagged(cr, 1, 2, NULL);
            }
            /* (and ... true ...) -> drop this operand */
            continue;
        }
        if (acc.ref == EXPR_NULL) { acc = b; acc.width = 1; }
        else {
            acc.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_AND,
                                             acc.ref, b.ref);
            acc.leaf_kind = 0;
        }
    }
    if (acc.ref == EXPR_NULL) {
        /* All operands were literal true */
        dvs_expr_t cr = _bv_const(fe, 1, 1);
        return _tagged(cr, 1, 2, NULL);
    }
    acc.width = 1;
    return acc;
}

/* or */
static SMT2_NOINLINE TaggedExpr _tr_or(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count < 2) return TAGGED_NULL;   /* arity >= 1; see `and` */
    /* Collect non-trivial operands; short-circuit on any literal true. */
    TaggedExpr acc = _tagged(EXPR_NULL, 1, 0, NULL);
    for (uint32_t i = 1; i < s->list.count; i++) {
        TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
        if (b.ref == EXPR_NULL) return TAGGED_NULL;
        if (b.leaf_kind == 2) {
            ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, b.ref);
            if (ec->value != 0) {
                /* (or ... true ...) -> true */
                dvs_expr_t cr = _bv_const(fe, 1, 1);
                return _tagged(cr, 1, 2, NULL);
            }
            /* (or ... false ...) -> drop this operand */
            continue;
        }
        if (acc.ref == EXPR_NULL) { acc = b; acc.width = 1; }
        else {
            acc.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_OR,
                                             acc.ref, b.ref);
            acc.leaf_kind = 0;
        }
    }
    if (acc.ref == EXPR_NULL) {
        /* All operands were literal false */
        dvs_expr_t cr = _bv_const(fe, 0, 1);
        return _tagged(cr, 1, 2, NULL);
    }
    acc.width = 1;
    return acc;
}

/* xor */
static SMT2_NOINLINE TaggedExpr _tr_xor(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count < 2) return TAGGED_NULL;   /* arity >= 1; see `and` */
    TaggedExpr acc = _translate_tagged(fe, s->list.items[1]);
    if (acc.ref == EXPR_NULL) return TAGGED_NULL;
    for (uint32_t i = 2; i < s->list.count; i++) {
        TaggedExpr b = _translate_tagged(fe, s->list.items[i]);
        if (b.ref == EXPR_NULL) return TAGGED_NULL;
        acc.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_BXOR,
                                         acc.ref, b.ref);
        acc.leaf_kind = 0;
    }
    acc.width = 1;
    return acc;
}

/* => */
static SMT2_NOINLINE TaggedExpr _tr_implies(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count < 3) return TAGGED_NULL;
    /* SMT-LIB =>: right-associative. (=> a b c) == (=> a (=> b c)) */
    TaggedExpr last = _translate_tagged(fe, s->list.items[s->list.count - 1]);
    if (last.ref == EXPR_NULL) return TAGGED_NULL;
    for (int i = (int)s->list.count - 2; i >= 1; i--) {
        TaggedExpr a = _translate_tagged(fe, s->list.items[i]);
        if (a.ref == EXPR_NULL) return TAGGED_NULL;
        uint64_t av, lv;
        if (_lit_value(fe, a, &av) || (_lit_value(fe, last, &lv) && lv)) {
            if (_lit_value(fe, a, &av) && !av) last = _bool_lit(fe, 1);  /* false => _ */
            continue;                       /* true => b is b; a => true is true */
        }
        dvs_expr_t not_a = dvs_builder_expr_unary(fe->builder, DVS_UN_NOT, a.ref);
        last.ref = dvs_builder_expr_binary(fe->builder, DVS_BIN_OR, not_a, last.ref);
        last.leaf_kind = 0;
    }
    last.width = 1;
    return last;
}

/* ite -- handles both BV and array branches */
static SMT2_NOINLINE TaggedExpr _tr_ite(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 4) return TAGGED_NULL;
    TaggedExpr c = _translate_tagged(fe, s->list.items[1]);
    if (c.ref == EXPR_NULL) return TAGGED_NULL;
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
            Smt2ArrayValue *node = _anode_ite(fe, c.ref, t.array, e.array);
            if (!node) { SMT2_TAINT(fe, "abstract-array ite node allocation failed"); return TAGGED_NULL; }
            return _tagged(EXPR_NULL, 0, 0, node);
        }
        if (t.array->n_elems != e.array->n_elems) {
            _term_error(fe, "ite branches are arrays of different sizes");
            return TAGGED_NULL;
        }
        Smt2ArrayValue *arr = _make_array_value(fe, t.array->sort);
        if (!arr) return TAGGED_NULL;
        for (uint32_t i = 0; i < t.array->n_elems; i++) {
            arr->elems[i] = dvs_builder_expr_ite(fe->builder, c.ref,
                                              t.array->elems[i],
                                              e.array->elems[i]);
        }
        return _tagged(EXPR_NULL, 0, 0, arr);
    }

    /* BV ite */
    if (!_same_width(fe, "ite", s->list.items[2], t, s->list.items[3], e)) return TAGGED_NULL;
    t = _flatten_to_var(fe, t);
    if (t.ref == EXPR_NULL) return TAGGED_NULL;
    e = _flatten_to_var(fe, e);
    if (e.ref == EXPR_NULL) return TAGGED_NULL;
    dvs_expr_t r = dvs_builder_expr_ite(fe->builder, c.ref, t.ref, e.ref);
    _flag_wide_arith(fe, t.width);
    return _tagged(r, t.width, 0, NULL);
}

/* concat. SMT-LIB's is binary; Verilator's solver interface (and z3)
 * also write it n-ary, (concat a b c) == (concat (concat a b) c). Refusing
 * that left the whole assertion uncompiled, and the check-sat `unknown`. */
static SMT2_NOINLINE TaggedExpr _tr_concat(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count < 3) return TAGGED_NULL;
    TaggedExpr acc = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (acc.ref == EXPR_NULL) return TAGGED_NULL;
    for (uint32_t i = 2; i < s->list.count; i++) {
        TaggedExpr lo = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[i]));
        if (lo.ref == EXPR_NULL) return TAGGED_NULL;
        if (_too_wide(fe, (uint64_t)acc.width + lo.width)) return TAGGED_NULL;
        dvs_expr_t r = dvs_builder_expr_concat(fe->builder, acc.ref, lo.ref,
                                        lo.width);
        acc = _tagged(r, (uint16_t)(acc.width + lo.width), 0, NULL);
        if (i + 1 < s->list.count) {
            acc = _flatten_to_var(fe, acc);
            if (acc.ref == EXPR_NULL) return TAGGED_NULL;
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
static SMT2_NOINLINE TaggedExpr _tr_bvashr(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count != 3) return TAGGED_NULL;
    TaggedExpr a = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[1]));
    if (a.ref == EXPR_NULL) return TAGGED_NULL;
    TaggedExpr k = _flatten_to_var(fe, _translate_tagged(fe, s->list.items[2]));
    if (k.ref == EXPR_NULL) return TAGGED_NULL;
    uint16_t w = a.width;
    if (w == 0 || k.width != w) {
        _term_error(fe, "bvashr operands must share a width");
        return TAGGED_NULL;
    }
    dvs_expr_t as = dvs_builder_expr_sv_cast(fe->builder, a.ref,
                                      (uint16_t)w, (uint16_t)w, 0, 1);
    dvs_expr_t sh = dvs_builder_expr_binary(fe->builder, DVS_BIN_ASHR, as, k.ref);
    dvs_expr_t r  = dvs_builder_expr_sv_cast(fe->builder, sh,
                                      (uint16_t)w, (uint16_t)w, 1, 0);
    _flag_wide_arith(fe, w);
    return _tagged(r, w, 0, NULL);
}

/* One list term: dispatch on its head to the operator's handler. Each handler
 * is its own (noinline) function, so a level of nesting costs the stack of the
 * one handler it uses rather than the union of every operator's locals; the
 * calls here are in tail position. */
static SMT2_NOINLINE TaggedExpr _translate_list_tagged(Smt2Frontend *fe, const Sexpr *s) {
    if (s->list.count == 0) return TAGGED_NULL;

    Sexpr *head = s->list.items[0];

    if (sexpr_is_symbol(head, "_")) return _tr_bv_lit(fe, s);
    if (head->kind == SEXPR_LIST && head->list.count >= 3 &&
        sexpr_is_symbol(head->list.items[0], "_"))
        return _tr_indexed_app(fe, s);
    if (head->kind == SEXPR_LIST && head->list.count == 3 &&
        sexpr_is_symbol(head->list.items[0], "as") &&
        sexpr_is_symbol(head->list.items[1], "const"))
        return _tr_as_const(fe, s);

    /* (! <term> :attr val ...) — annotated term. The attributes (e.g. :named
     * for unsat cores, :pattern) are metadata; the value is just <term>. */
    if (head->kind == SEXPR_SYMBOL && sexpr_is_symbol(head, "!") &&
        s->list.count >= 2) {
        return _translate_tagged(fe, s->list.items[1]);
    }
    if (head->kind == SEXPR_SYMBOL && sexpr_is_symbol(head, "let"))
        return _tr_let(fe, s);

    if (head->kind != SEXPR_SYMBOL) {
        _term_error(fe, "expected symbol at head of expression");
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

#define OP_IS(name) (oplen == sizeof(name) - 1 && memcmp(op, name, oplen) == 0)
    if (OP_IS("select")) return _tr_select(fe, s);
    if (OP_IS("store"))  return _tr_store(fe, s);
    if ((OP_IS("bvand") || OP_IS("bvor")) && s->list.count >= 3)
        return _tr_band_bor(fe, s, OP_IS("bvand"));
    if (OP_IS("bvudiv")) return _tr_udivrem(fe, s, 0);
    if (OP_IS("bvurem")) return _tr_udivrem(fe, s, 1);
    if (OP_IS("bvadd"))  return _tr_binop(fe, s, "bvadd", DVS_BIN_ADD);
    if (OP_IS("bvsub"))  return _tr_binop(fe, s, "bvsub", DVS_BIN_SUB);
    if (OP_IS("bvmul"))  return _tr_binop(fe, s, "bvmul", DVS_BIN_MUL);
    if (OP_IS("bvand"))  return _tr_binop(fe, s, "bvand", DVS_BIN_BAND);
    if (OP_IS("bvor"))   return _tr_binop(fe, s, "bvor", DVS_BIN_BOR);
    if (OP_IS("bvxor"))  return _tr_binop(fe, s, "bvxor", DVS_BIN_BXOR);
    if (OP_IS("bvshl"))  return _tr_binop(fe, s, "bvshl", DVS_BIN_LSHIFT);
    if (OP_IS("bvlshr")) return _tr_binop(fe, s, "bvlshr", DVS_BIN_RSHIFT);
    if (OP_IS("bvult"))  return _tr_cmpop(fe, s, "bvult", DVS_BIN_LT);
    if (OP_IS("bvule"))  return _tr_cmpop(fe, s, "bvule", DVS_BIN_LTE);
    if (OP_IS("bvugt"))  return _tr_cmpop(fe, s, "bvugt", DVS_BIN_GT);
    if (OP_IS("bvuge"))  return _tr_cmpop(fe, s, "bvuge", DVS_BIN_GTE);
    /* Signed comparisons -- see _translate_signed_cmp (MSB-flip lowering that
     * materialises the variable side's xor and folds a constant operand). */
    if (OP_IS("bvslt"))  return _translate_signed_cmp(fe, s, DVS_BIN_LT);
    if (OP_IS("bvsle"))  return _translate_signed_cmp(fe, s, DVS_BIN_LTE);
    if (OP_IS("bvsgt"))  return _translate_signed_cmp(fe, s, DVS_BIN_GT);
    if (OP_IS("bvsge"))  return _translate_signed_cmp(fe, s, DVS_BIN_GTE);
    if (OP_IS("bvsdiv")) return _tr_sdivrem(fe, s, 0);
    if (OP_IS("bvsrem")) return _tr_sdivrem(fe, s, 1);
    if (OP_IS("bvsmod")) return _tr_smod(fe, s);
    if (OP_IS("="))      return _tr_eq(fe, s);
    if (OP_IS("distinct")) return _tr_distinct(fe, s);
    if (OP_IS("bvnot"))  return _tr_bvnot(fe, s);
    if (OP_IS("bvneg"))  return _tr_bvneg(fe, s);
    if (OP_IS("not"))    return _tr_not(fe, s);
    if (OP_IS("and"))    return _tr_and(fe, s);
    if (OP_IS("or"))     return _tr_or(fe, s);
    if (OP_IS("xor"))    return _tr_xor(fe, s);
    if (OP_IS("=>"))     return _tr_implies(fe, s);
    if (OP_IS("ite"))    return _tr_ite(fe, s);
    if (OP_IS("concat")) return _tr_concat(fe, s);
    if (OP_IS("bvashr")) return _tr_bvashr(fe, s);
#undef OP_IS

    {
        int handled;
        TaggedExpr r = _try_rewrite(fe, s, op, oplen, 0, &handled);
        if (handled) return r;
    }

    _unsupported(fe, "operation '%.*s'",
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
        return _tagged(r, 64, 2, NULL);
    }
    case SEXPR_BITVEC: {
        /* A `#x…`/`#b…` literal wider than 64 bits carries its full value in
         * limbs (Sexpr.bv.value is only the low 64 bits). */
        if (s->bv.width > 64)
            return _wide_const(fe, s->bv.limbs, s->bv.width);
        dvs_expr_t r = _bv_const(fe, (int64_t)s->bv.value, (uint16_t)s->bv.width);
        return _tagged(r, (uint16_t)s->bv.width, 2, NULL);
    }
    case SEXPR_LIST:
        return _translate_list_tagged(fe, s);
    default:
        _term_error(fe, "unexpected S-expression kind in expression");
        return TAGGED_NULL;
    }
}

static TypedExpr _translate_expr(Smt2Frontend *fe, const Sexpr *s) {
    return _typed(_translate_tagged(fe, s));
}

/* ------------------------------------------------------------------ */
/* Deep-copy Sexpr into persistent arena                              */
/* ------------------------------------------------------------------ */

/* Copy one node; a list gets a fresh item array, its items still to fill. */
static Sexpr *_sexpr_copy_node(SexprArena *arena, const Sexpr *src) {
    Sexpr *dst = (Sexpr *)sexpr_arena_alloc(arena, sizeof(Sexpr), _Alignof(Sexpr));
    if (!dst) return NULL;
    dst->kind = src->kind;
    switch (src->kind) {
    case SEXPR_SYMBOL:
    case SEXPR_KEYWORD:
    case SEXPR_DECIMAL:
    case SEXPR_STRING: {
        char *s = (char *)sexpr_arena_alloc(arena, src->sym.len, 1);
        if (!s) return NULL;
        memcpy(s, src->sym.str, src->sym.len);
        dst->quoted  = src->quoted;
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
    case SEXPR_LIST:
        dst->list.count = src->list.count;
        dst->list.items = NULL;
        if (src->list.count) {
            dst->list.items = (Sexpr **)sexpr_arena_alloc(
                arena, src->list.count * sizeof(Sexpr *), _Alignof(Sexpr *));
            if (!dst->list.items) return NULL;
        }
        break;
    }
    return dst;
}

/* Iterative, with a heap work list of (slot to fill, node to copy), so a deep
 * term costs memory rather than C stack. NULL when out of memory. */
static const Sexpr *_sexpr_deep_copy(SexprArena *arena, const Sexpr *src) {
    typedef struct { Sexpr **slot; const Sexpr *src; } Work;
    Sexpr *root = NULL;
    uint32_t n = 0, cap = 64;
    Work *w = (Work *)malloc(cap * sizeof *w);
    if (!w) return NULL;
    w[n++] = (Work){ &root, src };
    while (n) {
        Work it = w[--n];
        Sexpr *dst = _sexpr_copy_node(arena, it.src);
        if (!dst) { free(w); return NULL; }
        *it.slot = dst;
        if (it.src->kind != SEXPR_LIST) continue;
        for (uint32_t i = 0; i < it.src->list.count; i++) {
            if (n == cap) {
                Work *t = (Work *)realloc(w, (size_t)cap * 2 * sizeof *w);
                if (!t) { free(w); return NULL; }
                w = t;
                cap *= 2;
            }
            w[n++] = (Work){ &dst->list.items[i], it.src->list.items[i] };
        }
    }
    free(w);
    return root;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static int _cmd_set_logic(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_SYMBOL) {
        _input_error(fe, "set-logic requires a logic name");
        return -1;
    }
    const Sexpr *l = cmd->list.items[1];
    if (fe->logic != SMT2_LOGIC_UNSET) {
        _input_error(fe, "the logic has already been set");
        return -1;
    }
    if (sexpr_is_symbol(l, "QF_BV"))    { fe->logic = SMT2_LOGIC_QF_BV;    return 0; }
    if (sexpr_is_symbol(l, "QF_UFBV"))  { fe->logic = SMT2_LOGIC_QF_UFBV;  return 0; }
    if (sexpr_is_symbol(l, "QF_ABV"))   { fe->logic = SMT2_LOGIC_QF_ABV;   return 0; }
    if (sexpr_is_symbol(l, "QF_AUFBV")) { fe->logic = SMT2_LOGIC_QF_AUFBV; return 0; }
    if (sexpr_is_symbol(l, "ALL"))      { fe->logic = SMT2_LOGIC_ALL;      return 0; }
    _input_error(fe, "unsupported logic '%.*s' (supported: QF_BV, QF_UFBV, QF_ABV, QF_AUFBV, ALL)",
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
    if (sexpr_is_keyword(key, ":print-success")) {
        fe->print_success = sexpr_is_symbol(val, "true");
        return 0;
    }
    if (sexpr_is_keyword(key, ":produce-assertions")) {
        /* only from here on: assertions made before are not kept */
        fe->produce_assertions = sexpr_is_symbol(val, "true") && fe->n_checked == 0;
        return 0;
    }
    return 0;
}

/* (get-option :key): the options this front end keeps; `unsupported` for the
 * rest, as SMT-LIB prescribes. */
static int _cmd_get_option(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_KEYWORD) {
        _query_error(fe, "get-option expects one keyword");
        return 0;
    }
    const Sexpr *k = cmd->list.items[1];
    if (sexpr_is_keyword(k, ":produce-models"))
        fprintf(fe->out, "%s\n", fe->produce_models ? "true" : "false");
    else if (sexpr_is_keyword(k, ":produce-unsat-cores"))
        fprintf(fe->out, "%s\n", fe->produce_unsat_cores ? "true" : "false");
    else if (sexpr_is_keyword(k, ":print-success"))
        fprintf(fe->out, "%s\n", fe->print_success ? "true" : "false");
    else if (sexpr_is_keyword(k, ":produce-assertions"))
        fprintf(fe->out, "%s\n", fe->produce_assertions ? "true" : "false");
    else if (sexpr_is_keyword(k, ":random-seed") || sexpr_is_keyword(k, ":seed"))
        fprintf(fe->out, "%llu\n", (unsigned long long)fe->given_seed);
    else
        fprintf(fe->out, "unsupported\n");
    fflush(fe->out);
    return 0;
}

static void _fprint_sexpr(FILE *out, const Sexpr *s);
static int _check_models_on(Smt2Frontend *fe);

/* (get-assertions): the assertions in scope, as written. They are kept for the
 * model check, which is off in --mode=verilator; :produce-assertions keeps
 * them anyway. */
static int _cmd_get_assertions(Smt2Frontend *fe) {
    if (!(_check_models_on(fe) || fe->produce_assertions)) {
        _query_error(fe, "get-assertions: assertions are not kept; "
                         "use (set-option :produce-assertions true) first");
        return 0;
    }
    fprintf(fe->out, "(");
    for (uint32_t i = 0; i < fe->n_checked; i++) {
        fprintf(fe->out, "\n  ");
        _fprint_sexpr(fe->out, fe->checked[i]);
    }
    fprintf(fe->out, "%s)\n", fe->n_checked ? "\n" : "");
    fflush(fe->out);
    return 0;
}

static int _add_sort_fun(Smt2Frontend *fe, const Sexpr *name_s,
                         uint8_t n_params, uint16_t return_width, int is_bool);

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
        _input_error(fe, "declare-datatypes expects (pars defs)");
        return -1;
    }
    const Sexpr *pars = cmd->list.items[1];
    const Sexpr *defs = cmd->list.items[2];
    if (pars->kind != SEXPR_LIST || defs->kind != SEXPR_LIST) {
        _input_error(fe, "declare-datatypes: malformed pars/defs");
        return -1;
    }
    if (pars->list.count == 0 || pars->list.count != defs->list.count) {
        _input_error(fe, "declare-datatypes: pars/defs length mismatch");
        return -1;
    }

    for (uint32_t di = 0; di < pars->list.count; di++) {
        /* par = (NAME ARITY) */
        const Sexpr *par = pars->list.items[di];
        if (par->kind != SEXPR_LIST || par->list.count != 2 ||
            par->list.items[0]->kind != SEXPR_SYMBOL ||
            par->list.items[1]->kind != SEXPR_NUMERAL) {
            _input_error(fe, "declare-datatypes: malformed par entry");
            return -1;
        }
        const Sexpr *name_s = par->list.items[0];
        if (par->list.items[1]->numval != 0) {
            _input_error(fe, "unsupported: declare-datatypes type parameters "
                    "(got arity %lld for %.*s)",
                    (long long)par->list.items[1]->numval,
                    (int)name_s->sym.len, name_s->sym.str);
            return -1;
        }

        /* defs[di] = (CTOR ...) — list of constructors */
        const Sexpr *ctors = defs->list.items[di];
        if (ctors->kind != SEXPR_LIST || ctors->list.count != 1) {
            _input_error(fe, "unsupported: declare-datatypes with more than one "
                    "constructor (got %u for %.*s)",
                    (unsigned)(ctors->kind == SEXPR_LIST ? ctors->list.count : 0u),
                    (int)name_s->sym.len, name_s->sym.str);
            return -1;
        }

        /* Register type name as opaque sort */
        if (fe->n_sort_names >= SMT2_MAX_SORTS) {
            _input_error(fe, "unsupported: too many sorts declared");
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
            _input_error(fe, "declare-datatypes: malformed constructor");
            return -1;
        }

        /* Each remaining ctor element is a (FIELD TYPE) selector pair.
         * Field becomes a sort-fun: takes one instance of the record type,
         * returns the BV/Bool field. */
        for (uint32_t fi = 1; fi < ctor->list.count; fi++) {
            const Sexpr *field = ctor->list.items[fi];
            if (field->kind != SEXPR_LIST || field->list.count != 2 ||
                field->list.items[0]->kind != SEXPR_SYMBOL) {
                _input_error(fe, "declare-datatypes: malformed field");
                return -1;
            }
            const Sexpr *fname = field->list.items[0];
            const Sexpr *ftype = field->list.items[1];

            int is_bool = _is_bool_sort(fe, ftype);
            uint16_t bvw = is_bool ? 1 : _parse_bitvec_sort(fe, ftype);
            if (bvw == 0) {
                _input_error(fe, "unsupported: declare-datatypes field '%.*s' "
                        "type (only BitVec and Bool)",
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
        _input_error(fe, "declare-sort requires name and arity");
        return -1;
    }
    const Sexpr *name_s = cmd->list.items[1];
    const Sexpr *arity_s = cmd->list.items[2];
    if (name_s->kind != SEXPR_SYMBOL || arity_s->kind != SEXPR_NUMERAL) {
        _input_error(fe, "malformed declare-sort");
        return -1;
    }
    if (arity_s->numval != 0) {
        _input_error(fe, "unsupported: declare-sort with arity > 0");
        return -1;
    }
    if (fe->n_sort_names >= SMT2_MAX_SORTS) {
        _input_error(fe, "unsupported: too many sorts declared");
        return -1;
    }
    if (_is_known_sort(fe, name_s->sym.str, name_s->sym.len)) {
        /* SMT-LIB: redeclaring a sort is an error (z3 agrees). */
        _input_error(fe, "declare-sort: sort already declared");
        return -1;
    }

    uint32_t copy_len = name_s->sym.len < SMT2_MAX_NAME - 1 ?
                        name_s->sym.len : SMT2_MAX_NAME - 1;
    memcpy(fe->sort_names[fe->n_sort_names], name_s->sym.str, copy_len);
    fe->sort_names[fe->n_sort_names][copy_len] = '\0';
    fe->n_sort_names++;
    return 0;
}

/* (define-sort NAME () SORT). Only aliases without parameters: that is all
 * Yosys's write_smt2 -stbv emits (a module's state sort). */
static int _cmd_define_sort(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 4) {
        _input_error(fe, "malformed define-sort, expected (define-sort <symbol> (<symbol>*) <sort>)");
        return 0;
    }
    const Sexpr *name_s = cmd->list.items[1];
    const Sexpr *pars   = cmd->list.items[2];
    const Sexpr *def    = cmd->list.items[3];
    if (name_s->kind != SEXPR_SYMBOL) {
        _input_error(fe, "invalid sort definition, symbol expected");
        return 0;
    }
    if (pars->kind != SEXPR_LIST) {
        _input_error(fe, "invalid sort definition, parameter list expected");
        return 0;
    }
    if (pars->list.count != 0) {
        _input_error(fe, "unsupported: define-sort with parameters ('%.*s')",
                     (int)name_s->sym.len, name_s->sym.str);
        return 0;
    }
    if (name_s->sym.len >= SMT2_MAX_NAME) {
        _input_error(fe, "sort name too long ('%.40s...')", name_s->sym.str);
        return 0;
    }
    if (_expand_sort(fe, name_s) != name_s || _is_known_sort(fe, name_s->sym.str, name_s->sym.len)
            || sexpr_is_symbol(name_s, "Bool") || sexpr_is_symbol(name_s, "BitVec")
            || sexpr_is_symbol(name_s, "Array")) {
        _input_error(fe, "sort '%.*s' is already defined", (int)name_s->sym.len, name_s->sym.str);
        return 0;
    }
    /* Resolve aliases in the definition now, so a use needs one lookup. */
    def = _expand_sort(fe, def);
    Smt2ArraySort as;
    int ok = _parse_bitvec_sort(fe, def) != 0 || _is_opaque_sort(fe, def)
          || _parse_array_sort_x(fe, def, &as, /*nested_ok=*/1) == 1;
    if (!ok) {
        _input_error(fe, "define-sort '%.*s': unknown or unsupported sort",
                     (int)name_s->sym.len, name_s->sym.str);
        return 0;
    }
    if (fe->n_sort_aliases == fe->sort_alias_cap) {
        uint32_t cap = fe->sort_alias_cap ? fe->sort_alias_cap * 2 : 8;
        Smt2SortAlias *a = (Smt2SortAlias *)realloc(fe->sort_aliases, cap * sizeof(*a));
        if (!a) { SMT2_TAINT(fe, "out of memory in define-sort"); return 0; }
        fe->sort_aliases = a;
        fe->sort_alias_cap = cap;
    }
    const Sexpr *copy = _sexpr_deep_copy(&fe->persistent_arena, def);
    if (!copy) { SMT2_TAINT(fe, "out of memory in define-sort"); return 0; }
    Smt2SortAlias *a = &fe->sort_aliases[fe->n_sort_aliases++];
    memcpy(a->name, name_s->sym.str, name_s->sym.len);
    a->name[name_s->sym.len] = '\0';
    a->def = copy;
    return 0;
}

static int _add_sort_fun(Smt2Frontend *fe, const Sexpr *name_s,
                         uint8_t n_params, uint16_t return_width, int is_bool) {
    if (fe->n_sort_funs >= SMT2_MAX_SORT_FUNS) {
        _input_error(fe, "unsupported: too many sort-typed functions declared");
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
        _input_error(fe, "unsupported: too many sort-typed functions declared");
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
        _input_error(fe, "unsupported: too many sort-typed constants declared");
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

/* A name visible as a constant, array or function (scoped by push/pop),
 * including sort-typed constants and functions (B116). */
static int _declared(Smt2Frontend *fe, const Sexpr *name) {
    return _find_var(fe, name->sym.str, name->sym.len) != NULL
        || _find_array_var(fe, name->sym.str, name->sym.len) != NULL
        || _find_fun(fe, name->sym.str, name->sym.len) != NULL
        || _find_sort_fun(fe, name->sym.str, name->sym.len) != NULL
        || _find_sort_const(fe, name->sym.str, name->sym.len) != NULL;
}

static int _cmd_declare_const(Smt2Frontend *fe, const Sexpr *cmd) {
    Sexpr *name_s;
    Sexpr *sort_s;
    Sexpr *params = NULL;

    if (sexpr_is_command(cmd, "declare-const")) {
        if (cmd->list.count != 3) {
            _input_error(fe, "declare-const requires name and sort");
            return -1;
        }
        name_s = cmd->list.items[1];
        sort_s = cmd->list.items[2];
    } else {
        /* declare-fun */
        if (cmd->list.count != 4) {
            _input_error(fe, "declare-fun requires name, params, sort");
            return -1;
        }
        name_s = cmd->list.items[1];
        params = cmd->list.items[2];
        sort_s = cmd->list.items[3];
        if (params->kind != SEXPR_LIST) {
            _input_error(fe, "declare-fun params must be a list");
            return -1;
        }
    }

    if (name_s->kind != SEXPR_SYMBOL) {
        _input_error(fe, "expected symbol for variable name");
        return -1;
    }
    if (_declared(fe, name_s)) {
        _input_error(fe, "'%.*s' is already declared", (int)name_s->sym.len, name_s->sym.str);
        return -1;
    }
    fe->shape_fp = _fp_mix(fe->shape_fp ^ 0x51u, (const uint8_t *)name_s->sym.str,
                           name_s->sym.len);

    /* Case 1: declare-fun with arity > 0 -- sort-typed function */
    if (params && params->list.count > 0) {
        for (uint32_t i = 0; i < params->list.count; i++) {
            if (!_is_opaque_sort(fe, params->list.items[i])) {
                _input_error(fe, "unsupported: declare-fun with a parameter that is not a declare-sort sort");
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
        int is_bool = _is_bool_sort(fe, sort_s);
        uint16_t rw = _parse_bitvec_sort(fe, sort_s);
        if (rw == 0) {
            /* Opaque return sort: treat as width-1 accessor */
            if (_is_opaque_sort(fe, sort_s)) {
                rw = 1; is_bool = 0;
            } else {
                _input_error(fe, "unsupported: declare-fun return sort (Bool, BitVec or Array only)");
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
    uint16_t width = _parse_bitvec_sort(fe, sort_s);
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
    uint32_t slot = fe->n_vars;   /* _add_var fills this entry */
    if (_add_var(fe, name_s->sym.str, name_s->sym.len, var_id, width) < 0) {
        fprintf(fe->err, "error: out of memory adding variable\n");
        return -1;
    }
    fe->vars[slot].is_bool = (uint8_t)_is_bool_sort(fe, sort_s);
    _builder_touched(fe);
    return 0;
}

static int _cmd_define_fun(Smt2Frontend *fe, const Sexpr *cmd) {
    /* (define-fun NAME ((p1 S1) (p2 S2) ...) RET BODY) */
    if (cmd->list.count != 5) {
        _input_error(fe, "define-fun requires name, params, sort, body");
        return -1;
    }
    const Sexpr *name_s = cmd->list.items[1];
    const Sexpr *params_s = cmd->list.items[2];
    const Sexpr *sort_s = cmd->list.items[3];
    const Sexpr *body_s = cmd->list.items[4];

    if (name_s->kind != SEXPR_SYMBOL || params_s->kind != SEXPR_LIST) {
        _input_error(fe, "malformed define-fun");
        return -1;
    }
    if (_declared(fe, name_s)) {
        _input_error(fe, "'%.*s' is already declared", (int)name_s->sym.len, name_s->sym.str);
        return -1;
    }
    if (fe->n_funs >= SMT2_MAX_FUNS) {
        _input_error(fe, "unsupported: too many define-fun macros");
        return -1;
    }
    if (params_s->list.count > SMT2_MAX_FUN_PARAMS) {
        _input_error(fe, "unsupported: define-fun has too many parameters");
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
        fd->is_bool_return = _is_bool_sort(fe, sort_s) ? 1 : 0;
        fd->return_width = _parse_bitvec_sort(fe, sort_s);
        if (fd->return_width == 0) fd->return_width = 1;
    }

    for (uint32_t i = 0; i < params_s->list.count; i++) {
        const Sexpr *p = params_s->list.items[i];
        if (p->kind != SEXPR_LIST || p->list.count != 2 ||
            p->list.items[0]->kind != SEXPR_SYMBOL) {
            _input_error(fe, "malformed define-fun parameter");
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
                _input_error(fe, "unsupported: define-fun parameter sort");
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
    if (t.ref == EXPR_NULL || t.leaf_kind != 2 || t.width != 1) return 0;
    ExprConst *ec = (ExprConst *)dvs_builder_ref_ptr(fe->builder, t.ref);
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
    if (t.ref == EXPR_NULL || t.width != 1) return EXPR_NULL;
    return t.ref;
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
    if (v.ref == EXPR_NULL || v.leaf_kind != 1 || v.array
            || v.width == 0 || v.width > 32)
        return 0;
    TaggedExpr c = _translate_tagged(fe, q);
    if (c.ref == EXPR_NULL || c.leaf_kind != 2 || c.width != v.width) return 0;
    if (a->x == EXPR_NULL) { a->x = v.ref; a->width = v.width; }
    else if (!_same_operand(fe, a->x, v.ref)) return 0;
    if (a->n >= SMT2_INSET_MAX) return 0;
    a->elems[a->n++] = c.ref;
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

/* Whether a Verilator hash XOR carries UniGen2's `#b0` chain seed. */
static int _vlt_xor_seeded(const Sexpr *x) {
    for (uint32_t i = 1; i < x->list.count; i++)
        if (x->list.items[i]->kind == SEXPR_BITVEC) return 1;
    return 0;
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
        /* A lone XOR counts only with UniGen2's `#b0` chain seed. Without it
         * the assert is solveDiversityXor's parity round (Verilator 5.046 and
         * later, `(= #b0|#b1 (bvxor <bits>))`), after which Verilator keeps
         * the solver's own model: treating it as enumeration turned the
         * diversify pass off and skewed every randomize() (B95). */
        if (_is_vlt_xor(fe, x) && _vlt_xor_seeded(x)) return 1;
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

/* Whether the model check (2.5) is on: DV_CHECK_MODEL=0/1, else on except in
 * Verilator mode, whose per-randomize() loop is the latency-critical path.
 * Decided once per session. */
static int _check_models_on(Smt2Frontend *fe) {
    if (!fe->check_models) {
        const char *e = getenv("DV_CHECK_MODEL");
        int on = e ? atoi(e) != 0 : !fe->verilator_mode;
        fe->check_models = on ? 1 : -1;
    }
    return fe->check_models > 0;
}

/* Keep an assertion as written, for the model check. A copy that cannot be
 * made switches the check off for the session rather than check a subset. */
static void _record_checked(Smt2Frontend *fe, const Sexpr *t) {
    if (!_check_models_on(fe) && !fe->produce_assertions) return;
    if (fe->n_checked == fe->checked_cap) {
        uint32_t cap = fe->checked_cap ? fe->checked_cap * 2 : 64;
        const Sexpr **g = (const Sexpr **)realloc((void *)fe->checked, cap * sizeof(*g));
        if (!g) { fe->check_models = -1; fe->produce_assertions = 0; return; }
        fe->checked = g;
        fe->checked_cap = cap;
    }
    const Sexpr *copy = _sexpr_deep_copy(&fe->persistent_arena, t);
    if (!copy) { fe->check_models = -1; fe->produce_assertions = 0; return; }
    fe->checked[fe->n_checked++] = copy;
}

static int _cmd_assert(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2) {
        _input_error(fe, "assert requires exactly one expression");
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
    _record_checked(fe, cmd->list.items[1]);
    if (fe->verilator_mode && !fe->vlt_enum && _is_vlt_enum(fe, cmd->list.items[1])) fe->vlt_enum = 1;
    _record_named(fe, cmd->list.items[1]);
    if (_try_split_reified_and(fe, cmd->list.items[1])) return 0;

    if (_try_sparse_default(fe, cmd->list.items[1])) return 0;

    TypedExpr te;
    fe->term_err[0] = '\0';
    fe->unsup_msg[0] = '\0';
    const char *why0 = fe->incomplete ? fe->incomplete_why : NULL;
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
        /* An ill-formed term (unknown symbol, sort mismatch) is an input
         * error, unless the context is already tainted: then an unknown
         * symbol is most likely one whose unsupported declaration tainted it. */
        if (fe->term_err[0] && !fe->incomplete) {
            _input_error(fe, "%s", fe->term_err);
            return 0;
        }
        fprintf(fe->err, "error: failed to translate assert expression "
                         "(unsupported construct) -> result will be unknown\n");
        /* Keep a reason the translation recorded; else name the construct. */
        if (!fe->incomplete || fe->incomplete_why == why0) {
            if (fe->unsup_msg[0]) {
                snprintf(fe->taint_msg, sizeof fe->taint_msg, "%s", fe->unsup_msg);
                SMT2_TAINT(fe, fe->taint_msg);
            } else {
                SMT2_TAINT(fe, "assert used an untranslatable construct");
            }
        }
        return 0;
    }
    _add_user_constraint(fe, te.ref);
    return 0;
}

/* Finalize the builder into fe->problem WITHOUT building the CDCL context. The
 * bit-blast engine reads fe->problem directly and never touches fe->ctx, so in
 * Verilator mode (always bit-blast) we skip dvs_solver_create + dvs_solver_compile here
 * and, symmetrically, the dvs_solver_free the (reset) would pay --
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
    /* A context that is not compiled is one whose compile proved the
     * assertions unsat: say so every time, not just the first. Solving it as
     * if compiled answered a check-sat-assuming `sat`. */
    if (fe->ctx) return fe->compiled ? 0 : -2;
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

    /* The context grows as the compile needs (dvs_solver_new): no size to
     * estimate, no grow-and-recompile. A pool overflow now means memory ran
     * out, and compile reports it as DVS_COMPILE_NOMEM. */
    {
        fe->ctx = dvs_solver_new(NULL);
        if (!fe->ctx) return -1;
        int rc = dvs_solver_compile(fe->ctx, fe->problem);
        if (rc >= 0) {
            /* rc > 0 (some constraints left uncompiled; validation and
             * escalation cover them) is a context like any other. Leaving
             * `compiled` unset made _flush_aux drop every later assertion:
             * after `(assert (xor true ...))`, `(assert false)` answered sat
             * (B58). */
            fe->compiled = 1;
            fe->ctx_uncompiled = (uint32_t)rc;
            fe->builder_retained = 0;   /* fe->problem can't be rebuilt in place (B14) */
            fe->has_aux = 0;
            _start_cdcl_retention(fe);
            return rc;
        }
        if (rc == -2) fe->ctx_unsat_depth = fe->push_depth;
        if (rc != -2) {
            /* No usable context (e.g. a variable wider than 64 bits): release
             * it. Leaving it made the next push checkpoint a context that was
             * never compiled, and the pop then kept the popped assertions in
             * the builder the bitblast path solves -- a wrong unsat (B63). */
            dvs_solver_free(fe->ctx); fe->ctx = NULL;
        }
        return rc;
    }
}

/* After the CDCL compile, keep the builder (holding every assertion so far) so
 * a later CDCL `unknown` can escalate to bitblast even after incremental
 * asserts -- unless it is already too big to keep copying on every hand-off. */
#define SMT2_RETAIN_MAX_BYTES (16u << 20)

/* The retention cap; DV_RETAIN_MAX_BYTES overrides it (tests use a small
 * one to reach the end of retention without a 16 MB problem). */
static uint32_t _retain_max(void) {
    static uint32_t v;
    if (!v) {
        const char *e = getenv("DV_RETAIN_MAX_BYTES");
        v = (e && *e) ? (uint32_t)strtoul(e, NULL, 0) : SMT2_RETAIN_MAX_BYTES;
        if (!v) v = 1;
    }
    return v;
}
/* Retention ends (the builder is about to be reset): keep the retained
 * assertions for the validation pass in vbase, which pops trim. */
static void _retention_end(Smt2Frontend *fe) {
    if (!fe->vproblem) return;
    dvs_problem_t *vp = _explicit(dvs_builder_finalize_append(
                            fe->builder, fe->vproblem, &fe->vproblem_size));
    if (vp) fe->vproblem = vp;
    free(fe->vbase);           /* one retention per compile; never both */
    fe->vbase = fe->vproblem;
    fe->vproblem = NULL;
}

static void _start_cdcl_retention(Smt2Frontend *fe) {
    free(fe->vproblem);
    fe->vproblem = NULL;
    if (dvs_builder_virtual_used(fe->builder) <= _retain_max()) {
        fe->cdcl_retained = 1;
        fe->aux_mark = dvs_builder_mark(fe->builder);
        fe->retain_mark = fe->aux_mark;
    } else {
        dvs_builder_reset(fe->builder);
        _fun_memo_clear(fe);
        fe->cdcl_retained = 0;
        fe->bb_inc = 0;
        fe->bb_sess = 0;
        memset(&fe->aux_mark, 0, sizeof(fe->aux_mark));
    }
}

/* The builder's items since aux_mark have been handed to CDCL. `failed`: the
 * ctx refused them (it no longer stands for the assertion set, and later
 * checks bit-blast from the builder): keep the builder whole even past the
 * retention cap. Resetting it there lost those assertions, and a later check
 * answered without them (B119). */
static void _aux_handed_off(Smt2Frontend *fe, int failed) {
    if (fe->cdcl_retained &&
        (failed || dvs_builder_virtual_used(fe->builder) <= _retain_max())) {
        fe->aux_mark = dvs_builder_mark(fe->builder);
    } else {
        _retention_end(fe);
        dvs_builder_reset(fe->builder);
        _fun_memo_clear(fe);
        fe->cdcl_retained = 0;
        fe->bb_inc = 0;
        fe->bb_sess = 0;
        memset(&fe->aux_mark, 0, sizeof(fe->aux_mark));
    }
    fe->builder_retained = 0;
    fe->has_aux = 0;
}

static int _flush_aux(Smt2Frontend *fe) {
    if (!fe->compiled || !fe->has_aux) return 0;
    size_t aux_sz = 0;
    dvs_problem_t *aux = NULL;
    int rc;
    if (fe->cdcl_retained) {
        /* The builder holds every assertion (the ctx has those before
         * aux_mark). Bring its finalized copy up to date, copying only the new
         * bytes, and hand the ctx just the items since aux_mark, cut in
         * place. The ctx reads a problem only during the call. A full copy
         * per hand-off, each kept for validation, cost O(hand-offs x size)
         * memory and time (B117). */
        dvs_problem_t *vp = _explicit(dvs_builder_finalize_append(
                                fe->builder, fe->vproblem, &fe->vproblem_size));
        if (!vp) return -1;
        fe->vproblem = vp;
        dvs_problem_cut_t cut;
        if (dvs_problem_cut_since(vp, &fe->aux_mark, &cut) != 0) return -1;
        rc = dvs_solver_add_constraint(fe->ctx, vp);
        dvs_problem_uncut(vp, &cut);
    } else {
        /* Only the items added since the last hand-off: the builder was reset
         * after it, so this copy is small. */
        aux = _explicit(dvs_builder_finalize_since(fe->builder, &fe->aux_mark, &aux_sz));
        if (!aux) return -1;
        rc = dvs_solver_add_constraint(fe->ctx, aux);
    }
    /* An add that overflows the context pool compiles only part of `aux`, and
     * can read the failed allocation as a conflict: neither its `unsat` nor
     * the context can be trusted. Report it incomplete -- check-sat then
     * answers from bitblast -- and give later instances a bigger pool. */
    if (fe->ctx->pool.overflow) rc = 1;
    if (rc > 0) fe->ctx_uncompiled += (uint32_t)rc;
    /* Record the hand-off (scoped by push/pop; see aux_problems). A copy is
     * kept so the post-solve model-validation pass can re-evaluate the
     * constraints it contributed; retained ones are in vproblem. */
    if (fe->n_aux_problems == fe->aux_problems_cap) {
        uint32_t new_cap = fe->aux_problems_cap ? fe->aux_problems_cap * 2 : 8;
        dvs_problem_t **grow = (dvs_problem_t **)realloc(fe->aux_problems,
                                  new_cap * sizeof(dvs_problem_t *));
        if (!grow) {
            /* Out of memory: fall back to the old behaviour (drop the
             * aux). Validation will be incomplete for this run but the
             * solver result is unaffected. */
            free(aux);
            _aux_handed_off(fe, rc < 0 && rc != -2);
            if (rc == -2 && !fe->flush_unsat) fe->flush_unsat = fe->push_depth + 1;
            return rc;
        }
        fe->aux_problems     = grow;
        fe->aux_problems_cap = new_cap;
    }
    fe->aux_problems[fe->n_aux_problems++] = aux;
    _aux_handed_off(fe, rc < 0 && rc != -2);
    if (rc == -2 && !fe->flush_unsat) fe->flush_unsat = fe->push_depth + 1;
    return rc;
}

/* Validate the ctx's model against `sp`'s items since retain_mark: the
 * assertions made after the compile (fe->problem has those before). */
static int _validate_since(Smt2Frontend *fe, dvs_problem_t *sp) {
    if (!sp) return 0;
    dvs_problem_cut_t cut;
    if (dvs_problem_cut_since(sp, &fe->retain_mark, &cut) != 0) return 0;
    int viol = dvs_solver_validate_model(fe->ctx, sp, fe->err);
    dvs_problem_uncut(sp, &cut);
    return viol;
}

/* Check the ctx's model against every assertion in scope: the compiled
 * problem, then the incremental ones (hand-off copies, and the retained or
 * once-retained builder). Returns the number of violated constraints. */
static int _validate_all(Smt2Frontend *fe) {
    int viol = dvs_solver_validate_model(fe->ctx, fe->problem, fe->err);
    for (uint32_t i = 0; i < fe->n_aux_problems; i++)
        if (fe->aux_problems[i])
            viol += dvs_solver_validate_model(fe->ctx, fe->aux_problems[i], fe->err);
    viol += _validate_since(fe, fe->vbase);
    if (fe->cdcl_retained && fe->vproblem) {
        /* Up to date with the builder: a pop since the last hand-off dropped
         * items from its lists. */
        dvs_problem_t *vp = _explicit(dvs_builder_finalize_append(
                                fe->builder, fe->vproblem, &fe->vproblem_size));
        if (vp) fe->vproblem = vp;
        viol += _validate_since(fe, fe->vproblem);
    }
    return viol;
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
    return dvs_problem_fingerprint(p);
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
    if (problem_add_var(fe->problem, id, (uint16_t)w, 0, 0,
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
            SMT2_EMIT_UNKNOWN(fe, "incomplete: an abstract array wider than 64 bits"); fflush(fe->out);
            fe->last_result = DVS_SOLVE_TIMEOUT; fe->has_result = 1;
            return 0;
        }
    }

    /* Finalize the BV skeleton with pool headroom for in-place lemmas + reads.
     * The builder keeps every assertion, for a later check or a pop. */
    uint32_t vu = dvs_builder_virtual_used(fe->builder);
    uint32_t reserve = vu > (32u << 20) ? vu : (32u << 20);   /* >= 32 MB slack */
    fe->problem = _explicit(dvs_builder_finalize_reserve(fe->builder, &fe->problem_size, reserve));
    if (!fe->problem) { SMT2_EMIT_UNKNOWN(fe, "memout"); fflush(fe->out); return -1; }
    fe->problem_dirty = 0;
    fe->has_aux = 0;
    if (!fe->compiled) fe->builder_retained = 1;

    /* Incremental CaDiCaL backend is required for assert + resolve. */
    fe->bb_solver = dvs_bbsolver_new_backend(NULL, fe->problem, /*cadical=*/1);
    fe->bb_over_problem = 1;
    if (!fe->bb_solver || !dvs_bbsolver_is_incremental(fe->bb_solver)) {
        SMT2_EMIT_UNKNOWN(fe, "incomplete: no incremental SAT backend for the array engine"); fflush(fe->out);
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
        SMT2_EMIT_UNKNOWN(fe, dvs_bbsolver_unknown_reason(fe->bb_solver));
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
    fe->bb_sess = 0;
    fe->bb_sess_assume = 0;
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
        SMT2_EMIT_UNKNOWN(fe, "memout");
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
        SMT2_EMIT_UNKNOWN(fe, "memout");
        fflush(fe->out);
        fe->cache_valid = 0;
        return -1;
    }
    /* Simulation first (plan pieces 3.3, 3.4), except in Verilator mode, where
     * it was off because the model it found was the first in sweep order. A
     * seeded sweep now samples uniformly (piece 6.4); turning it on here waits
     * on a per-randomize cost measurement. DV_SIM=0 turns it off. */
    if (!fe->verilator_mode) {
        const char *sim = getenv("DV_SIM");
        dvs_bbsolver_set_presim(fe->bb_solver, !(sim && *sim == '0'));
    }
    int rc = dvs_bbsolver_check(fe->bb_solver, fe->seed);
    fe->bb_model_valid = (rc == DVS_BB_SAT);
    fe->bb_checks++;
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
        SMT2_EMIT_UNKNOWN(fe, dvs_bbsolver_unknown_reason(fe->bb_solver));
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
    /* (A context whose compile proved unsat is not compiled; _ensure_compiled
     * kept the builder for it.) */
    if (fe->compiled) return fe->cdcl_retained;
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

/* One bit-blast instance for a whole session (plan piece 6.1). A session that
 * keeps asserting -- a BMC unroll, a latency search -- re-blasted every
 * assertion at every check-sat: quadratic in its length. Instead keep one
 * CaDiCaL instance, add only the constraints asserted since the last
 * check-sat, and mirror the push stack with selector scopes (pop retracts).
 * check-sat-assuming adds one more scope for its assumptions.
 *
 * A lone check-sat at the top level stays one-shot: kissat and the
 * whole-problem passes. The session starts at the first check-sat inside a
 * push, the second check-sat overall, or a check-sat-assuming.
 * DV_BB_SESSION=0 turns it off. */
static int _session_ok(Smt2Frontend *fe) {
    dvs_builder_t *b = fe->builder;
    /* The builder must hold every assertion in scope: after a CDCL compile
     * that is retention (the parts handed to CDCL stay in it too). */
    if (fe->verilator_mode || !b || !_builder_is_full(fe)) return 0;
    if (b->n_sources || b->n_alldiffs || b->n_softs || b->n_dists) return 0;
    const char *env = getenv("DV_BB_SESSION");
    if (env && *env == '0') return 0;
    return dvs_sat_has_cadical();
}

/* Bring the session up to date with the builder: every constraint added, at
 * the depth of its scope. Returns 0, or -1 to leave the query to the regular
 * path (the session is dropped). */
static int _session_sync(Smt2Frontend *fe) {
    dvs_builder_t *b = fe->builder;
    size_t sz = 0;
    if (fe->bb_sess && (!fe->bb_solver || b->n_constraints < fe->bb_sess_ncons))
        _free_bb(fe);
    /* The session's problem grows in place: a full finalize copied the whole
     * pool at every check-sat. */
    dvs_problem_t *np = _explicit(dvs_builder_finalize_append(
                            b, fe->bb_sess ? fe->bb_problem : NULL, &sz));
    if (!np) { if (fe->bb_sess) _free_bb(fe); return -1; }
    if (fe->bb_sess) {
        fe->bb_problem = np;                  /* realloc'd (or not) from it */
        if (dvs_bbsolver_rebase(fe->bb_solver, np) != 0) { _free_bb(fe); return -1; }
    } else {
        _free_bb(fe);
        fe->bb_problem = np;
        fe->bb_solver = dvs_bbsolver_new_backend(NULL, np, /*prefer_cadical=*/1);
        if (!fe->bb_solver || !dvs_bbsolver_is_incremental(fe->bb_solver)
                || dvs_bbsolver_sess_begin(fe->bb_solver) != 0) {
            _free_bb(fe);
            return -1;
        }
        const char *sim = getenv("DV_SIM");
        dvs_bbsolver_set_presim(fe->bb_solver, !(sim && *sim == '0'));
        fe->bb_sess = 1;
        fe->bb_sess_ncons = 0;
    }

    /* The new constraints lead the newest-first list; take them oldest first. */
    uint32_t j0 = fe->bb_sess_ncons, k = b->n_constraints - j0;
    dvs_expr_t *roots = (dvs_expr_t *)malloc((k ? k : 1) * sizeof(dvs_expr_t));
    if (!roots) { _free_bb(fe); return -1; }
    dvs_expr_t cur = np->constraints_head;
    for (uint32_t i = 0; i < k && cur != EXPR_NULL; i++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(np, cur);
        roots[k - 1 - i] = cs->root;
        cur = cs->next;
    }
    /* Constraint j belongs to every scope whose push came before it: add
     * each run at its depth, opening scopes as their marks are passed. */
    uint32_t d = dvs_bbsolver_sess_depth(fe->bb_solver), start = 0;
    int arc = 0;
    for (uint32_t i = 0; arc == 0; i++) {
        int end = (i == k);
        uint32_t j = j0 + i;
        if (end || (d < fe->push_depth && fe->push[d].bmark.n_constraints <= j)) {
            if (i > start)
                arc = dvs_bbsolver_sess_add(fe->bb_solver, roots + start, i - start);
            start = i;
            while (arc == 0 && d < fe->push_depth
                   && (end || fe->push[d].bmark.n_constraints <= j)) {
                if (dvs_bbsolver_sess_push(fe->bb_solver, fe->push[d].n_vars) != 0)
                    arc = DVS_BB_ERROR;
                else
                    d++;
            }
        }
        if (end) break;
    }
    free(roots);
    if (arc != 0) { _free_bb(fe); return -1; }
    fe->bb_sess_ncons = b->n_constraints;
    return 0;
}

/* Solve the session and answer. Returns 1, or 0 to leave the query to the
 * regular path (the session is dropped). */
static int _session_answer(Smt2Frontend *fe, uint32_t added) {
    int rc = dvs_bbsolver_sess_solve(fe->bb_solver, fe->seed);
    if (getenv("DV_LOG"))
        fprintf(fe->err, "bb-session: check %u depth %u added %u constraints -> %s\n",
                fe->bb_checks + 1, dvs_bbsolver_sess_depth(fe->bb_solver), added,
                rc == DVS_BB_SAT ? "sat" : rc == DVS_BB_UNSAT ? "unsat" : "unknown");
    if (rc != DVS_BB_SAT && rc != DVS_BB_UNSAT) { _free_bb(fe); return 0; }
    fe->bb_checks++;
    fe->cache_valid = 0;
    fe->bb_model_valid = (rc == DVS_BB_SAT);
    fprintf(fe->out, rc == DVS_BB_SAT ? "sat\n" : "unsat\n");
    fe->last_result = rc == DVS_BB_SAT ? DVS_SOLVE_OK : DVS_SOLVE_UNSAT;
    fe->has_result = 1;
    fflush(fe->out);
    return 1;
}

/* check-sat on the session. Returns 1 when it answered, 0 to leave the query
 * to the regular path. */
static int _check_sat_session(Smt2Frontend *fe) {
    if (!_session_ok(fe)) return 0;
    if (!fe->bb_sess && fe->push_depth == 0 && fe->bb_checks == 0) return 0;
    uint32_t before = fe->bb_sess ? fe->bb_sess_ncons : 0;
    if (_session_sync(fe) != 0) return 0;
    return _session_answer(fe, fe->bb_sess_ncons - before);
}

/* check-sat-assuming on the session: the assumptions go in a scope of their
 * own, which stays open (so get-value can read the model) until the next
 * command that changes the assertion stack (_end_assumptions). The literals
 * are already validated and recorded. Returns 1 when it answered, 0 to leave
 * the query to the regular path. */
static int _check_sat_assuming_session(Smt2Frontend *fe, const dvs_expr_t *lits,
                                       uint32_t n) {
    if (!_session_ok(fe)) return 0;
    if (_session_sync(fe) != 0) return 0;
    if (dvs_bbsolver_sess_push(fe->bb_solver, fe->n_vars) != 0) { _free_bb(fe); return 0; }
    fe->bb_sess_assume = 1;
    if (dvs_bbsolver_sess_add(fe->bb_solver, lits, n) != 0) { _free_bb(fe); return 0; }
    return _session_answer(fe, n);
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
        SMT2_EMIT_UNKNOWN(fe, "memout");
        fflush(fe->out);
        return -1;
    }
    _free_bb(fe);
    /* Prefer CaDiCaL: cube-and-conquer needs retractable assumptions. Falls
     * back to kissat (→ single-shot solve) when CaDiCaL is not compiled in. */
    fe->bb_solver = dvs_bbsolver_new_backend(NULL, fe->problem, /*prefer_cadical=*/1);
    if (!fe->bb_solver) {
        SMT2_EMIT_UNKNOWN(fe, "memout");
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
        SMT2_EMIT_UNKNOWN(fe, "incomplete: cube-and-conquer gave no verdict");
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
    if (fe->qfbv_bb) return 1;    /* one-shot QF_BV routed by its operator mix */
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
    if (fe->bb_sess_assume) {
        /* The bit-blast session's assumption scope (_check_sat_assuming_session). */
        fe->bb_sess_assume = 0;
        fe->bb_model_valid = 0;
        if (fe->bb_sess && dvs_bbsolver_sess_pop(fe->bb_solver) != 0) _free_bb(fe);
    }
    if (!fe->assump_cp1) return;
    uint32_t cp = fe->assump_cp1 - 1;
    fe->assump_cp1 = 0;
    if (fe->ctx) dvs_solver_restore(fe->ctx, cp);
}

/* One-shot QF_BV routing (docs/fw_hdl_implementation_plan.md, piece 3.1).
 * CDCL is built for randomization: ranges, orderings and a few equalities over
 * a handful of variables, where it answers in milliseconds and samples near
 * uniformly. On circuits -- XOR/AND/shift networks, multipliers, miters
 * asserting two circuits differ -- it stalls until its budget runs out, while
 * bit-blast answers in milliseconds (fwhdl-core baseline, plan section 1.3).
 * Returns why this problem goes to bitblast, or NULL to keep CDCL. */
static const char *_qfbv_route(const dvs_shape_t *sh) {
    if (sh->n_mul)                                    return "multiplier";
    /* The front end binds every let and define-fun to a variable through an
     * equality, so equalities say nothing about the shape; orderings do.
     * Randomization is mostly orderings, circuits mostly bit operations. */
    if ((uint64_t)sh->n_bit + sh->n_arith >= 3ull * sh->n_cmp) return "bit-level";
    return NULL;
}

/* Pick the engine for a one-shot QF_BV check-sat (sets fe->qfbv_bb). */
static void _route_qfbv(Smt2Frontend *fe) {
    fe->qfbv_bb  = 0;
    fe->qfbv_why = NULL;
    if (fe->verilator_mode || fe->needs_bitblast || fe->compiled || fe->ctx
            || getenv("DV_ENGINE") || getenv("DV_NO_BITBLAST")
            || _engine_is_bitblast(fe))
        return;
    if (_ensure_problem(fe) != 0 || !fe->problem) return;
    dvs_shape_t sh;
    if (dvs_shape_compute(fe->problem, &sh) != 0) return;
    fe->qfbv_why = _qfbv_route(&sh);
    /* An explicit seed asks for a sample. The bit-blast engine's simulation
     * draws one uniformly (dvs_aigsim.h) and its flip-check keeps it uniform;
     * CDCL's decisions skew orderings and disjunctions (a < b: a == 0 at half
     * its share; plan piece 6.4, perf-out/p64/route_ab.log). */
    if (!fe->qfbv_why && fe->seed_given && fe->seed) fe->qfbv_why = "seeded sample";
    fe->qfbv_bb  = fe->qfbv_why != NULL;
    if (getenv("DV_LOG"))
        fprintf(fe->err, "qfbv-route: %s (%s) cons=%u nodes=%u vars=%u bit=%u arith=%u "
                         "mul=%u cmp=%u eq=%u bool=%u other=%u%s\n",
                fe->qfbv_bb ? "bitblast" : "cdcl", fe->qfbv_why ? fe->qfbv_why : "randomization",
                sh.n_constraints, sh.n_nodes, sh.n_vars, sh.n_bit, sh.n_arith, sh.n_mul,
                sh.n_cmp, sh.n_eq, sh.n_bool, sh.n_other,
                sh.truncated ? " truncated" : "");
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
/* Why CDCL answered unknown, for SMT2_EMIT_UNKNOWN. */
static const char *_cdcl_unknown_why(Smt2Frontend *fe) {
    if (fe->last_validate_viol > 0)
        return "incomplete: the CDCL model failed validation, and the problem "
               "could not be bit-blasted instead";
    snprintf(fe->unknown_why, sizeof fe->unknown_why, "incomplete: CDCL search stopped: %s",
             dvs_solver_bail_reason_str(fe->ctx));
    return NULL;
}

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

    /* Tainted context (an assert used an unsupported construct), or input that
     * was malformed: answer honestly with `unknown` rather than solve a problem
     * that is missing constraints. */
    if (fe->incomplete || fe->input_poisoned) {
        if (fe->print_stats || getenv("DV_LOG"))
            fprintf(fe->err, "cdcl-unknown: tainted at %s:%u -- %s\n",
                    "smt2_frontend.c", fe->incomplete_line,
                    fe->incomplete_why ? fe->incomplete_why : "reason not recorded");
        SMT2_EMIT_UNKNOWN(fe, NULL);
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
        SMT2_EMIT_UNKNOWN(fe, "incomplete: abstract arrays after a CDCL compile");
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_TIMEOUT;
        fe->has_result = 1;
        return 0;
    }

    /* DV_ARRAY=eager: emit the one-shot Ackermann axioms into the builder before
     * the first finalize (correctness oracle). */
    if (fe->array_eager && !fe->problem && !fe->compiled) {
        if (_emit_array_axioms(fe) < 0) {
            SMT2_EMIT_UNKNOWN(fe, "incomplete: the array axioms could not be built (out of memory?)");
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

    _route_qfbv(fe);

    /* Routing trace for the other paths: the engine this check-sat starts on
     * and every rule that pointed it there. nb = first needs_bitblast trigger,
     * qfbv = one-shot QF_BV operator-mix route, logic = the QF_UFBV/ABV/AUFBV
     * route. (A CDCL `unknown` may still escalate to bitblast further down.) */
    if (!fe->verilator_mode && getenv("DV_LOG")) {
        int logic_bb = fe->logic == SMT2_LOGIC_QF_UFBV || fe->logic == SMT2_LOGIC_QF_ABV
                    || fe->logic == SMT2_LOGIC_QF_AUFBV;
        const char *eng = _engine_is_cube(fe) ? "cube"
                        : _engine_is_bitblast(fe) ? "bitblast" : "cdcl";
        fprintf(fe->err, "route: %s nb=%s qfbv=%s logic=%d\n", eng,
                fe->needs_bitblast ? (fe->nb_why ? fe->nb_why : "?") : "-",
                fe->qfbv_why ? fe->qfbv_why : "-", logic_bb);
    }

    if (_engine_is_cube(fe)) {
        int prc = _ensure_problem(fe);
        if (prc < 0) {
            /* -2: stale problem we cannot safely rebuild -> honest `unknown`,
             * not an error, so the driver's protocol stream stays in sync. */
            if (prc == -2) SMT2_TAINT(fe, "stale problem could not be safely rebuilt");
            SMT2_EMIT_UNKNOWN(fe, prc == -2 ? NULL : "memout");
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
        if (_check_sat_session(fe)) return 0;
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
            SMT2_EMIT_UNKNOWN(fe, prc == -2 ? NULL : "memout");
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
        if (_check_sat_session(fe)) return 0;
        return _check_sat_bitblast(fe);
    }
    if (crc < 0) {
        if (fe->print_stats || getenv("DV_LOG"))
            fprintf(fe->err, "cdcl-unknown: dvs_solver_compile failed (rc=%d) -- the "
                             "CDCL compile could not build a context for this "
                             "problem\n", crc);
        SMT2_EMIT_UNKNOWN(fe, "incomplete: the CDCL compile could not build a context");
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
    /* flush_unsat: an earlier flush in a live scope met a root conflict. The
     * reset above forgot it, so a later flush and solve answered `sat`. */
    if (frc == -2 || fe->flush_unsat) {
        fprintf(fe->out, "unsat\n");
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
    }
    if (frc < 0) {
        /* The context missed part of the assertion set; it was handed off all
         * the same, so a later check-sat would solve without it. Bit-blast
         * from here on (B87). */
        fe->needs_bitblast = 1;
        if (!fe->nb_why) fe->nb_why = "flush-failed";
        SMT2_EMIT_UNKNOWN(fe, "incomplete: an assertion could not be added to the CDCL context; later checks bit-blast");
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
        SMT2_EMIT_UNKNOWN(fe, "incomplete: the CDCL context overflowed with incremental assertions");
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

    /* One-shot QF_BV (plan piece 3.2): a randomization-shaped problem CDCL
     * cannot finish quickly goes to bitblast, which is decision-complete, after
     * a short budget rather than CDCL's 10 s default. CDCL answers every such
     * problem it can in under 15 ms (tier1, fwhdl-core). Only where the
     * escalation below can take the whole problem; otherwise CDCL keeps its
     * full budget. DV_QFBV_CDCL_MS sets the budget, 0 the old behaviour. */
    if (!fe->verilator_mode && !route_cdcl && !getenv("DV_ENGINE") && !getenv("DV_NO_BITBLAST")
            && !getenv("DV_CDCL_TIME_LIMIT")
            && (fe->n_aux_problems == 0 || (fe->cdcl_retained && !fe->has_aux))) {
        const char *ev = getenv("DV_QFBV_CDCL_MS");
        uint32_t ms = ev && *ev ? (uint32_t)atoi(ev) : 20;
        if (ms) opts.time_limit_ms = ms;
    }

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
        /* Every bail counter by index (dvs_lcg.c), then the propagator kinds
         * behind bail [5] (a propagator conflict with no current-level bound). */
        extern const char *lcg_dbg_bail_kind[];
        extern uint64_t    lcg_dbg_bail_kind_n[];
        fprintf(stderr, "[lcg] bail[]:");
        for (int i = 0; i < 16; i++)
            if (lcg_dbg_bail[i]) fprintf(stderr, " %d=%llu", i, (unsigned long long)lcg_dbg_bail[i]);
        fprintf(stderr, "\n[lcg] bail[5] by propagator:");
        for (int i = 0; i < 48 && lcg_dbg_bail_kind[i]; i++)
            fprintf(stderr, " %s=%llu", lcg_dbg_bail_kind[i],
                    (unsigned long long)lcg_dbg_bail_kind_n[i]);
        fprintf(stderr, "\n");
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
            int viol = _validate_all(fe);
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
                "cdcl-unknown: %s; uncompiled-constraints=%u; vars=%u; conflicts=%llu\n",
                dvs_solver_bail_reason_str(fe->ctx), fe->ctx_uncompiled,
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
    case DVS_SOLVE_TIMEOUT: SMT2_EMIT_UNKNOWN(fe, _cdcl_unknown_why(fe)); break;
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
/* The model, as smt2_eval.c sees it                                   */
/*                                                                     */
/* get-value of anything but a declared name -- a define-fun (yosys-   */
/* smtbmc's (get-value (|UNROLL#N|))), an expression, a select at a    */
/* symbolic index -- is evaluated by smt2_eval.c over the model. It    */
/* reads the model only through these callbacks.                       */
/* ------------------------------------------------------------------ */

/* Element (f_lo, f_hi) of a declared array -- the flat key of a nested one --
 * into `nl` limbs. Returns 1, or 2 when no constraint reads the element: its
 * value is then a seeded random one (a DV solver randomizes free elements). */
static int _fe_array_elem(Smt2Frontend *fe, const Smt2ArrayVar *av,
                          uint64_t f_lo, uint64_t f_hi, uint64_t *limbs, uint32_t nl) {
    for (uint32_t i = 0; i < nl; i++) limbs[i] = 0;
    uint64_t seed_key = f_lo ^ (f_hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t name_len = (uint32_t)strlen(av->name);
    const Smt2ArrayValue *v = av->value;
    if (v->is_abstract) {
        int64_t val;
        if (_abs_base_value(fe, v, f_lo, &val)) { limbs[0] = (uint64_t)val; return 1; }
        limbs[0] = (uint64_t)_free_elem_value(fe, seed_key, name_len);
        return 2;
    }
    if (v->is_sparse) {
        uint32_t vid;
        if (_sparse_find((Smt2ArrayValue *)v, f_lo, f_hi, &vid)) {
            _fe_get_var_value_wide(fe, vid, limbs, nl);
            return 1;
        }
        if (v->has_default) { limbs[0] = v->default_val; return 1; }
        limbs[0] = (uint64_t)_free_elem_value(fe, seed_key, name_len);
        return 2;
    }
    if (f_hi == 0 && f_lo < v->n_elems) {
        /* Look the element var up by name ("arr[k]") rather than via
         * value->elems[k]: that dvs_expr_t points into the builder arena,
         * which is reset after compilation, so it is stale here. */
        char en[SMT2_MAX_NAME + 24];
        snprintf(en, sizeof(en), "%s[%llu]", av->name, (unsigned long long)f_lo);
        Smt2Var *ev = _find_var(fe, en, (uint32_t)strlen(en));
        if (ev) { _fe_get_var_value_wide(fe, ev->var_id, limbs, nl); return 1; }
    }
    return 2;
}

static int _evh_fun(void *ctx, const char *name, uint32_t len, Smt2EvalFun *f) {
    Smt2FunDef *fd = _find_fun((Smt2Frontend *)ctx, name, len);
    if (!fd || !fd->body || fd->n_params > SMT2_EVAL_MAX_PARAMS) return 0;
    f->n_params = fd->n_params;
    for (uint32_t i = 0; i < fd->n_params; i++) f->param_names[i] = fd->param_names[i];
    f->body = fd->body;
    return 1;
}

static int _evh_scalar(void *ctx, const char *name, uint32_t len, uint32_t *width,
                       int *is_bool, uint64_t *limbs, uint32_t max_limbs) {
    Smt2Frontend *fe = (Smt2Frontend *)ctx;
    Smt2Var *v = _find_var(fe, name, len);
    if (!v) return 0;
    uint64_t l[SMT2_BV_MAX_LIMBS];
    uint32_t w = v->width ? v->width : 1u;
    uint32_t nl = (w + 63u) / 64u;
    if (nl > SMT2_BV_MAX_LIMBS) return 0;
    _fe_get_var_value_wide(fe, v->var_id, l, nl);
    for (uint32_t i = 0; i < nl && i < max_limbs; i++) limbs[i] = l[i];
    *width = w;
    *is_bool = v->is_bool;
    return 1;
}

static const void *_evh_array(void *ctx, const char *name, uint32_t len, uint32_t *index_w,
                              uint32_t *inner_w, uint32_t *elem_w, int *total) {
    Smt2ArrayVar *av = _find_array_var((Smt2Frontend *)ctx, name, len);
    if (!av) return NULL;
    *index_w = av->sort.addr_width;
    *inner_w = av->sort.inner_addr;
    *elem_w = av->sort.data_width;
    *total = !av->value->is_sparse && !av->value->is_abstract;
    return av;
}

static int _evh_array_elem(void *ctx, const void *arr, const uint64_t *key,
                           uint64_t *limbs, uint32_t elem_w) {
    const Smt2ArrayVar *av = (const Smt2ArrayVar *)arr;
    uint32_t kw = av->sort.addr_width;
    if (kw > 128) return 0;   /* keys are at most 128 bits */
    uint32_t nl = (elem_w + 63u) / 64u;
    if (nl > SMT2_BV_MAX_LIMBS) return 0;
    uint64_t l[SMT2_BV_MAX_LIMBS];
    int rc = _fe_array_elem((Smt2Frontend *)ctx, av, key[0], kw > 64 ? key[1] : 0, l, nl);
    for (uint32_t i = 0; i < nl; i++) limbs[i] = l[i];
    return rc;
}

static void _emit_array_store_chain(Smt2Frontend *fe, Smt2ArrayVar *av);

static int _evh_print_array(void *ctx, FILE *out, const void *arr) {
    Smt2Frontend *fe = (Smt2Frontend *)ctx;
    FILE *saved = fe->out;
    fe->out = out;
    _emit_array_store_chain(fe, (Smt2ArrayVar *)arr);
    fe->out = saved;
    return 1;
}

static uint32_t _evh_uf_arity(void *ctx, const char *name, uint32_t len) {
    Smt2SortFun *sf = _find_sort_fun((Smt2Frontend *)ctx, name, len);
    return sf ? sf->n_params : 0;
}

static int _evh_opaque_const(void *ctx, const char *name, uint32_t len) {
    return _find_sort_const((Smt2Frontend *)ctx, name, len) != NULL;
}

static const Sexpr *_evh_sort_alias(void *ctx, const char *name, uint32_t len) {
    Smt2Frontend *fe = (Smt2Frontend *)ctx;
    for (uint32_t i = 0; i < fe->n_sort_aliases; i++)
        if (_name_eq(name, len, fe->sort_aliases[i].name)) return fe->sort_aliases[i].def;
    return NULL;
}

static Smt2EvalHost _eval_host(Smt2Frontend *fe) {
    Smt2EvalHost h;
    memset(&h, 0, sizeof h);
    h.ctx = fe;
    h.fun = _evh_fun;
    h.scalar = _evh_scalar;
    h.array = _evh_array;
    h.array_elem = _evh_array_elem;
    h.print_array = _evh_print_array;
    h.uf_arity = _evh_uf_arity;
    h.opaque_const = _evh_opaque_const;
    h.sort_alias = _evh_sort_alias;
    /* Where the thread's stack is known, evaluation stops at it, as the
     * translator does. Elsewhere: the translator budgets 16 KB of stack a
     * level, and an evaluation level takes well under a quarter of that. */
    uint64_t d = dvs_stack_size() ? 4000000u : (uint64_t)_translate_depth_limit() * 4u;
    h.max_depth = d > 4000000u ? 4000000u : (uint32_t)d;
    return h;
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

/* Print a non-list term. `hex`: a bit-vector literal whose width is a
 * multiple of 4 prints as #x, the way z3 echoes a get-value key. */
static void _fprint_atom(FILE *out, const Sexpr *s, int hex) {
    switch (s->kind) {
    case SEXPR_SYMBOL: case SEXPR_KEYWORD: case SEXPR_DECIMAL:
        if (s->kind == SEXPR_SYMBOL && s->quoted)
            fprintf(out, "|%.*s|", (int)s->sym.len, s->sym.str);
        else
            fprintf(out, "%.*s", (int)s->sym.len, s->sym.str);
        break;
    case SEXPR_STRING:
        fprintf(out, "\"%.*s\"", (int)s->sym.len, s->sym.str); break;
    case SEXPR_NUMERAL:
        fprintf(out, "%llu", (unsigned long long)s->numval); break;
    case SEXPR_BITVEC: {
        uint32_t w = s->bv.width ? s->bv.width : 1;
        if (hex && w % 4 == 0) {
            fputs("#x", out);
            for (uint32_t d = w / 4; d-- > 0;) {
                uint32_t bit = d * 4;
                uint64_t limb = s->bv.limbs ? s->bv.limbs[bit / 64]
                              : (bit < 64 ? s->bv.value : 0);
                fputc("0123456789abcdef"[(limb >> (bit % 64)) & 0xF], out);
            }
        } else if (s->bv.width > 64 && s->bv.limbs)
            _emit_bv_bin_literal_wide(out, s->bv.limbs, (s->bv.width + 63u) / 64u,
                                      (unsigned)s->bv.width);
        else
            _emit_bv_bin_literal(out, s->bv.value, (unsigned)s->bv.width);
        break;
    }
    default: break;
    }
}

/* Print a term back as SMT-LIB. Iterative: a term nests as deep as the
 * input, and this also prints the terms the evaluator refused as too deep. */
static void _fprint_any(FILE *out, const Sexpr *s, int hex) {
    typedef struct { const Sexpr *l; uint32_t i; } Fr;
    Fr inl[32], *st = inl;
    size_t n = 0, cap = 32;
    for (;;) {
        if (s && s->kind != SEXPR_LIST) {
            _fprint_atom(out, s, hex);
        } else if (s) {
            if (n == cap) {
                Fr *ns = (Fr *)malloc(2 * cap * sizeof *ns);
                if (!ns) { fputs("...", out); s = NULL; goto next; }
                memcpy(ns, st, n * sizeof *ns);
                if (st != inl) free(st);
                st = ns;
                cap *= 2;
            }
            st[n].l = s;
            st[n++].i = 0;
            fputc('(', out);
        }
        s = NULL;
    next:
        if (!n) break;
        Fr *f = &st[n - 1];
        if (f->i == f->l->list.count) { fputc(')', out); n--; continue; }
        if (f->i) fputc(' ', out);
        s = f->l->list.items[f->i++];
    }
    if (st != inl) free(st);
}

static void _fprint_sexpr(FILE *out, const Sexpr *s) { _fprint_any(out, s, 0); }

/* Print a term the way z3 echoes a get-value key: as written, except that a
 * bit-vector literal is `#x` when its width is a multiple of 4, else `#b`. */
static void _fprint_term(FILE *out, const Sexpr *s) { _fprint_any(out, s, 1); }

static int _get_value_body(Smt2Frontend *fe, const Sexpr *vars_list);

static int _cmd_get_value(Smt2Frontend *fe, const Sexpr *cmd) {
    if (!fe->has_result || fe->last_result != DVS_SOLVE_OK) {
        _query_error(fe, "get-value: no model available, the last check-sat did not answer sat");
        return 0;
    }
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_LIST
            || cmd->list.items[1]->list.count == 0) {
        _query_error(fe, "get-value expects a non-empty list of terms");
        return 0;
    }
    const Sexpr *vars_list = cmd->list.items[1];

    /* Plain declared names (every Verilator and smtbmc query) cannot fail:
     * answer straight to the output. Anything else is built in memory first,
     * so a term we cannot evaluate turns the whole response into an error
     * rather than a partial list. */
    int simple = 1;
    for (uint32_t i = 0; i < vars_list->list.count && simple; i++) {
        const Sexpr *t = vars_list->list.items[i];
        simple = t->kind == SEXPR_SYMBOL
              && (_find_var(fe, t->sym.str, t->sym.len)
                  || _find_array_var(fe, t->sym.str, t->sym.len));
    }
    if (simple) return _get_value_body(fe, vars_list) < 0 ? 0 : 0;

    Smt2MemOut m;
    if (!_memout_open(&m)) return _get_value_body(fe, vars_list) < 0 ? 0 : 0;
    FILE *real = fe->out;
    fe->out = m.f;
    int bad = _get_value_body(fe, vars_list);
    fe->out = real;
    _memout_finish(&m, bad < 0 ? real : NULL);
    if (bad >= 0) {
        /* _get_value_body left the reason in term_err. */
        Smt2MemOut t;
        if (_memout_open(&t)) {
            _fprint_term(t.f, vars_list->list.items[bad]);
            fputc('\0', t.f);
            fflush(t.f);
#ifdef _WIN32
            _memout_finish(&t, NULL);
            _query_error(fe, "get-value: cannot evaluate a term: %s", fe->term_err);
#else
            _query_error(fe, "get-value: cannot evaluate '%.80s': %s",
                         t.buf ? t.buf : "?", fe->term_err);
            _memout_finish(&t, NULL);
#endif
        } else {
            _query_error(fe, "get-value: cannot evaluate a term: %s", fe->term_err);
        }
        fe->term_err[0] = 0;
    }
    fflush(fe->out);
    return 0;
}

/* Print the get-value response for `vars_list`. Returns -1 when every term was
 * answered, else the index of the first term that could not be evaluated (the
 * output is then incomplete and the caller discards it). */
static int _get_value_body(Smt2Frontend *fe, const Sexpr *vars_list) {
    Smt2EvalHost host = _eval_host(fe);
    Smt2Eval *ev = NULL;            /* created on the first term that needs it */
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
                fputc('(', fe->out);
                _fprint_sexpr(fe->out, name_s);
                fputc(' ', fe->out);
                _emit_array_store_chain(fe, av);
                fprintf(fe->out, ")");
                continue;
            }

            Smt2Var *v = _find_var(fe, name_s->sym.str, name_s->sym.len);
            if (v) {
                fputc('(', fe->out);
                _fprint_sexpr(fe->out, name_s);
                fputc(' ', fe->out);
                if (v->is_bool) {
                    fputs(_fe_get_var_value(fe, v->var_id) & 1 ? "true" : "false", fe->out);
                } else if (v->width > 64) {
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
            if (av && !is_const && !outer_idx && iw <= 64) {
                if (!ev) ev = smt2_eval_new(&host);
                const Smt2Value *iv = ev ? smt2_eval(ev, name_s->list.items[2]) : NULL;
                if (iv && smt2_value_kind(iv) == SMT2_VAL_BV && smt2_value_width(iv) == iw) {
                    k = smt2_value_u64(iv);
                    is_const = 1;
                    idx_evald = 1;
                }
            }
            if (av && is_const) {
                uint64_t f_lo = k, f_hi = k_hi;          /* flat key */
                if (outer_idx) _key_concat(o_lo, o_hi, k, k_hi, iw, &f_lo, &f_hi);
                uint64_t limbs[SMT2_BV_MAX_LIMBS];
                _fe_array_elem(fe, av, f_lo, f_hi, limbs, (dw + 63u) / 64u);
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
                        fprintf(fe->out, "(select ");
                        _fprint_sexpr(fe->out, arr_s);
                        fputc(' ', fe->out);
                        _emit_hex_key(fe->out, o_lo, o_hi, ow);
                        fprintf(fe->out, ")");
                    } else {
                        _fprint_sexpr(fe->out, arr_s);
                    }
                    fprintf(fe->out, " ");
                    _emit_hex_key(fe->out, k, k_hi, iw);
                    fprintf(fe->out, ") ");
                }
                _emit_bv_bin_literal_wide(fe->out, limbs, (dw + 63u) / 64u, (unsigned)dw);
                fprintf(fe->out, ")");
                continue;
            }
        }

        /* Anything else -- a define-fun (yosys-smtbmc's cover loop asks
         * (get-value (|UNROLL#28|))), an expression -- is evaluated over the
         * model by smt2_eval.c. */
        if (!ev) ev = smt2_eval_new(&host);
        const Smt2Value *val = ev ? smt2_eval(ev, name_s) : NULL;
        /* The key echoes the term (SMT-LIB 2.6 §4.2.6: consumers pair values
         * with terms by it). */
        if (val) {
            fputc('(', fe->out);
            _fprint_term(fe->out, name_s);
            fputc(' ', fe->out);
        }
        if (!val || !smt2_value_print(fe->out, val, &host)) {
            /* Never a placeholder value (fw-hdl R1): the caller discards the
             * buffered response and reports the term and this reason. */
            snprintf(fe->term_err, sizeof fe->term_err, "%s",
                     !ev ? "out of memory" : !val ? smt2_eval_error(ev)
                         : "the value cannot be printed");
            smt2_eval_free(ev);
            return (int)i;
        }
        fputc(')', fe->out);
    }
    smt2_eval_free(ev);
    fprintf(fe->out, ")\n");
    fflush(fe->out);
    return -1;
}

static int _cmd_get_model(Smt2Frontend *fe, const Sexpr *cmd) {
    (void)cmd;
    if (!fe->has_result || fe->last_result != DVS_SOLVE_OK) {
        _query_error(fe, "get-model: no model available, the last check-sat did not answer sat");
        return 0;
    }
    fprintf(fe->out, "(\n");
    /* BV/Bool vars */
    for (uint32_t i = 0; i < fe->n_vars && i < fe->vars_cap; i++) {
        Smt2Var *v = &fe->vars[i];
        if (!v->name[0]) continue;                 /* an unnamed id (_bump_n_vars) */
        if (v->name[0] == '\\') continue;         /* internal (_fresh_aux) */
        /* Skip array element vars "name[i]" (they appear in the array's
         * model). A user symbol never ends in ']': a simple one cannot
         * contain it and a quoted one ends in '|'. */
        size_t nl = strlen(v->name);
        if (nl && v->name[nl - 1] == ']') continue;
        if (v->width > 64) {
            uint64_t limbs[(SMT2_MAX_BV_BITS + 63) / 64];
            uint32_t nl = (v->width + 63u) / 64u;
            _fe_get_var_value_wide(fe, v->var_id, limbs, nl);
            fprintf(fe->out, "  (define-fun %s () (_ BitVec %u) ", v->name, (unsigned)v->width);
            _emit_bv_bin_literal_wide(fe->out, limbs, nl, (unsigned)v->width);
            fprintf(fe->out, ")\n");
            continue;
        }
        int64_t val = _fe_get_var_value(fe, v->var_id);
        if (v->is_bool) {
            fprintf(fe->out, "  (define-fun %s () Bool %s)\n", v->name, val & 1 ? "true" : "false");
            continue;
        }
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
        _query_error(fe, "echo expects a string");
        return 0;
    }
    /* The token keeps its quotes: echo it as written (SMT-LIB 2.6 §4.2.1). */
    const Sexpr *s = cmd->list.items[1];
    fprintf(fe->out, "%.*s\n", (int)s->sym.len, s->sym.str);
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
    } else if (sexpr_is_keyword(key, ":reason-unknown")) {
        /* Why the last check-sat said `unknown`, most specific cause first. */
        if (fe->input_poisoned) {
            fprintf(fe->out, "(:reason-unknown \"input error: ");
            _fprint_smt2_string_body(fe->out, fe->input_err_reason);
            fprintf(fe->out, "\")\n");
        } else if (fe->model_check_why[0]) {
            fprintf(fe->out, "(:reason-unknown \"model check failed: ");
            _fprint_smt2_string_body(fe->out, fe->model_check_why);
            fprintf(fe->out, "\")\n");
        } else if (fe->incomplete) {
            fprintf(fe->out, "(:reason-unknown \"unsupported: ");
            _fprint_smt2_string_body(fe->out, fe->incomplete_why
                                     ? fe->incomplete_why : "construct");
            fprintf(fe->out, "\")\n");
        } else if (!strcmp(fe->unknown_why, "memout")) {
            fprintf(fe->out, "(:reason-unknown memout)\n");
        } else if (fe->unknown_why[0]) {
            fprintf(fe->out, "(:reason-unknown \"");
            _fprint_smt2_string_body(fe->out, fe->unknown_why);
            fprintf(fe->out, "\")\n");
        } else {
            fprintf(fe->out, "(:reason-unknown incomplete)\n");
        }
    } else {
        fprintf(fe->out, "(:unsupported)\n");
    }
    fflush(fe->out);
    return 0;
}

/* Push depth limit: about what a growable solver context can hold
 * (DVS_MAX_CHECKPOINTS). */
#define SMT2_MAX_PUSH 0xFE00u

/* Room for `n` push frames: 0, or -1 (an input error, or out of memory). */
static int _push_reserve(Smt2Frontend *fe, uint64_t n) {
    if (n > SMT2_MAX_PUSH) {
        _input_error(fe, "push: max push depth exceeded");
        return -1;
    }
    if (n <= fe->push_cap) return 0;
    uint32_t cap = fe->push_cap ? 2 * fe->push_cap : 16;
    if (cap < n) cap = (uint32_t)n;
    Smt2PushFrame *f = (Smt2PushFrame *)realloc(fe->push, (size_t)cap * sizeof(*f));
    if (!f) {
        fprintf(fe->err, "error: out of memory growing the push stack\n");
        return -1;
    }
    fe->push = f;
    fe->push_cap = cap;
    return 0;
}

static void _abs_push_frame(Smt2Frontend *fe, uint32_t d);

/* Open a push frame recording what a pop returns to; `cp` is its solver
 * checkpoint, or (uint32_t)-1. The caller made room (_push_reserve). */
static void _push_frame(Smt2Frontend *fe, uint32_t cp) {
    uint32_t d = fe->push_depth++;
    Smt2PushFrame *f = &fe->push[d];
    f->cp             = cp;
    f->n_vars         = fe->n_vars;
    f->n_array_vars   = fe->n_array_vars;
    f->n_aux_problems = fe->n_aux_problems;
    f->n_named        = fe->n_named;
    f->n_core_hist    = fe->n_core_hist;
    f->n_checked      = fe->n_checked;
    f->n_funs         = fe->n_funs;
    f->n_sort_aliases = fe->n_sort_aliases;
    f->n_sort_names   = fe->n_sort_names;
    f->n_sort_funs    = fe->n_sort_funs;
    f->n_sort_consts  = fe->n_sort_consts;
    f->incomplete     = (uint8_t)fe->incomplete;
    f->vbase_ncons    = fe->cdcl_retained ? fe->builder->n_constraints : UINT32_MAX;
    _abs_push_frame(fe, d);
    f->bmark          = dvs_builder_mark(fe->builder);
}

/* Record the abstract-array state a pop of frame `d` returns to: translation
 * counts only, a solve's own reads are rebuilt by the next solve. */
static void _abs_push_frame(Smt2Frontend *fe, uint32_t d) {
    fe->push[d].n_anodes = fe->n_anodes;
    fe->push[d].n_areads = fe->n_areads_user;
    fe->push[d].n_aeqs   = fe->n_aeqs;
}

/* Pop the abstract-array state back to frame `d`: nodes, reads and equalities
 * made inside the scope go (their vars went with the builder rewind). Nodes
 * made before it never point at later ones. */
static void _abs_pop_frame(Smt2Frontend *fe, uint32_t d) {
    _abs_drop_solve_state(fe);
    if (fe->push[d].n_areads < fe->n_areads) {
        fe->n_areads = fe->n_areads_user = fe->push[d].n_areads;
        if (fe->aread_hash) {
            memset(fe->aread_hash, 0, fe->aread_hash_cap * sizeof(uint32_t));
            for (uint32_t i = 0; i < fe->n_areads; i++) _aread_hash_put(fe, i);
        }
    }
    if (fe->push[d].n_aeqs < fe->n_aeqs) fe->n_aeqs = fe->push[d].n_aeqs;
    for (uint32_t i = fe->push[d].n_anodes; i < fe->n_anodes; i++) free(fe->anodes[i]);
    if (fe->push[d].n_anodes < fe->n_anodes) fe->n_anodes = fe->push[d].n_anodes;
}

static int _cmd_push(Smt2Frontend *fe, const Sexpr *cmd) {
    _end_assumptions(fe);
    uint32_t n = 1;
    if (cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_NUMERAL) {
        n = (uint32_t)cmd->list.items[1]->numval;
    }
    /* Abstract arrays solve from the builder (_check_sat_array), so their
     * scopes live there: no CDCL context, the frame only marks the builder.
     * Likewise a problem that must bit-blast: every check-sat goes there, and
     * compiling (or failing to, for >64-bit variables) at every push cost a
     * finalize plus a compile of the whole problem each time. */
    int crc = ((fe->n_anodes > 0 || fe->needs_bitblast) && !fe->compiled)
              ? -1 : _ensure_compiled(fe);
    /* Compile-time UNSAT, or the assertions since the last push conflicting as
     * they reach the context: the scope pushed from is unsat. Failing the push
     * on the latter made the next pop drop the scope holding the conflicting
     * assertion -- `x == 3; push; x == 4; push; pop` answered sat. Record dead
     * frames (no checkpoint) instead: everything asserted under them stays in
     * the context until the pop of the scope that is unsat, whose checkpoint
     * takes it all back. */
    int dead = (crc == -2);
    if (crc >= 0) {
        /* Flush into a context at its root, as check-sat does: one still
         * holding the last model met `x` against x's model value as a
         * conflict, and called the scope unsat. */
        if (fe->has_result) { dvs_solver_reset(fe->ctx); fe->has_result = 0; }
        int frc = _flush_aux(fe);
        if (frc == -2) dead = 1;
        else if (frc < 0) {
            /* The context cannot take the assertions (e.g. a variable wider
             * than 64 bits declared after the compile), so it no longer stands
             * for the assertion set: bit-blast from here on, and keep the
             * scope. Returning here dropped it, and the next pop removed the
             * enclosing scope with its assertions -- a wrong `sat` (B87). */
            fe->needs_bitblast = 1;
            if (!fe->nb_why) fe->nb_why = "flush-failed";
            crc = -1;
        }
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
        if (_push_reserve(fe, (uint64_t)fe->push_depth + n) != 0) return -1;
        for (uint32_t i = 0; i < n; i++) _push_frame(fe, (uint32_t)-1);
        return 0;
    }
    if (fe->has_result) dvs_solver_reset(fe->ctx);
    fe->has_result = 0;

    if (_push_reserve(fe, (uint64_t)fe->push_depth + n) != 0) return -1;
    for (uint32_t i = 0; i < n; i++) {
        int cp = dvs_solver_checkpoint(fe->ctx);
        if (cp < 0) {
            fprintf(fe->err, "error: push: too many checkpoints\n");
            return -1;
        }
        _push_frame(fe, (uint32_t)cp);
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
    if (fe->flush_unsat && n <= fe->push_depth && fe->push_depth - n < fe->flush_unsat - 1)
        fe->flush_unsat = 0;
    if (fe->ctx && !fe->compiled && n <= fe->push_depth
            && fe->push_depth - n < fe->ctx_unsat_depth) {
        /* The compile that proved unsat ran inside a scope being popped, so
         * its verdict goes with it. The builder still holds every assertion
         * (_ensure_compiled keeps it), so the next use compiles afresh. */
        dvs_solver_free(fe->ctx); fe->ctx = NULL;
        if (fe->problem) fe->problem_dirty = 1;
    }
    if (fe->bb_sess && n <= fe->push_depth) {
        /* The bit-blast session retracts the popped scopes itself. */
        uint32_t t = fe->push_depth - n;
        int had = dvs_bbsolver_sess_depth(fe->bb_solver) > t;
        while (fe->bb_sess && dvs_bbsolver_sess_depth(fe->bb_solver) > t)
            if (dvs_bbsolver_sess_pop(fe->bb_solver) != 0) _free_bb(fe);
        if (fe->bb_sess && fe->bb_sess_ncons > fe->push[t].bmark.n_constraints) {
            if (had) fe->bb_sess_ncons = fe->push[t].bmark.n_constraints;
            else     _free_bb(fe);       /* constraints past a mark with no scope */
        }
        fe->bb_model_valid = 0;
    }
    if (fe->compiled) {
        if (fe->cdcl_retained && n <= fe->push_depth &&
            dvs_builder_rewind(fe->builder, &fe->push[fe->push_depth - n].bmark) == 0) {
            /* Rewind to the outermost popped push instead: the builder then
             * holds exactly the assertions still in scope, so retention (and
             * the bitblast route, which finalizes from it) stays valid. A push
             * flushes first, so everything up to its mark reached the ctx.
             * Resetting the builder here left a bitblast-routed check after
             * the pop solving a stale problem -- a wrong unsat (B63). */
            fe->aux_mark = fe->push[fe->push_depth - n].bmark;
        } else {
            if (fe->cdcl_retained) _retention_end(fe);  /* the loop trims it */
            dvs_builder_reset(fe->builder);
            _fun_memo_clear(fe);
            fe->cdcl_retained = 0;
            fe->bb_sess = 0;
            memset(&fe->aux_mark, 0, sizeof(fe->aux_mark));
        }
        fe->has_aux = 0;
        if (fe->problem) fe->problem_dirty = 1;
    } else if (n <= fe->push_depth) {
        /* No compiled context: the scopes live only in the builder, which
         * the bitblast path finalizes from. Rewind it to the outermost
         * popped push, and make the next check-sat re-finalize (B63). */
        _fun_memo_clear(fe);
        if (dvs_builder_rewind(fe->builder, &fe->push[fe->push_depth - n].bmark) != 0) {
            SMT2_TAINT(fe, "pop could not rewind the assertion set");
            fe->bb_sess = 0;
        }
        if (fe->problem) fe->problem_dirty = 1;
        fe->has_result = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (fe->push_depth == 0) {
            _input_error(fe, "pop: stack underflow");
            return -1;
        }
        fe->push_depth--;
        /* Un-taint if the untranslatable assert lived in the popped scope. */
        fe->incomplete = fe->push[fe->push_depth].incomplete;
        if (fe->ctx && fe->push[fe->push_depth].cp != (uint32_t)-1) {
            /* dvs_solver_restore handles trail backtrack itself; calling
             * dvs_solver_reset first would zero ctx->trail_top while leaving
             * checkpoint marks pointing at stale TrailEntry addresses,
             * leading to a NULL-deref inside trail_backtrack. */
            fe->has_result = 0;
            dvs_solver_restore(fe->ctx, fe->push[fe->push_depth].cp);
        }
        fe->n_vars = fe->push[fe->push_depth].n_vars;
        fe->var_idx_n = UINT32_MAX;     /* the name index rebuilds */

        /* Free arrays declared inside the popped scope. The solver-side BV
         * vars are already unwound by dvs_solver_restore via the n_vars rewind;
         * here we drop the frontend-side tracking and malloc'd element list. */
        uint32_t target_n_arr = fe->push[fe->push_depth].n_array_vars;
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
        uint32_t target_n_aux = fe->push[fe->push_depth].n_aux_problems;
        for (uint32_t i = target_n_aux; i < fe->n_aux_problems; i++) {
            free(fe->aux_problems[i]);
            fe->aux_problems[i] = NULL;
        }
        fe->n_aux_problems = target_n_aux;
        /* The once-retained assertions made since this push go too. */
        uint32_t vkeep = fe->push[fe->push_depth].vbase_ncons;
        if (fe->vbase && vkeep != UINT32_MAX) {
            dvs_problem_t *vb = fe->vbase;
            while (vb->n_constraints > vkeep) {
                vb->constraints_head = ((ConstraintSpec *)POOL_PTR(vb, vb->constraints_head))->next;
                vb->n_constraints--;
            }
        }
        _truncate_named(fe, fe->push[fe->push_depth].n_named);
        if (fe->n_core_hist > fe->push[fe->push_depth].n_core_hist)
            fe->n_core_hist = fe->push[fe->push_depth].n_core_hist;
        if (fe->n_checked > fe->push[fe->push_depth].n_checked)
            fe->n_checked = fe->push[fe->push_depth].n_checked;
        /* Definitions made in the popped scope go with it (B82: a stale one
         * shadowed a later redefinition -> wrong unsat). */
        if (fe->n_funs > fe->push[fe->push_depth].n_funs) {
            fe->n_funs = fe->push[fe->push_depth].n_funs;
            fe->fun_idx_n = UINT32_MAX;     /* the name index rebuilds */
        }
        if (fe->n_sort_aliases > fe->push[fe->push_depth].n_sort_aliases)
            fe->n_sort_aliases = fe->push[fe->push_depth].n_sort_aliases;
        if (fe->n_sort_names > fe->push[fe->push_depth].n_sort_names)
            fe->n_sort_names = fe->push[fe->push_depth].n_sort_names;
        if (fe->n_sort_funs > fe->push[fe->push_depth].n_sort_funs)
            fe->n_sort_funs = fe->push[fe->push_depth].n_sort_funs;
        if (fe->n_sort_consts > fe->push[fe->push_depth].n_sort_consts)
            fe->n_sort_consts = fe->push[fe->push_depth].n_sort_consts;
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

/* check-sat-assuming on the bit-blast session: each literal `p` / `(not p)`
 * of a declared Bool becomes the constraint p == 1 / p == 0 in the
 * assumption scope. Returns 1 when it answered (literals recorded for
 * get-unsat-assumptions), 0 to leave the query to the CDCL path, which also
 * reports malformed literals. */
static int _check_sat_assuming_bb(Smt2Frontend *fe, const Sexpr *ls) {
    dvs_expr_t *roots = (dvs_expr_t *)malloc((ls->list.count + 1) * sizeof(dvs_expr_t));
    if (!roots) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ls->list.count; i++) {
        const Sexpr *lit = ls->list.items[i];
        int positive = lit->kind == SEXPR_SYMBOL;
        const Sexpr *sym = positive ? lit
                         : (lit->kind == SEXPR_LIST && lit->list.count == 2 &&
                            sexpr_is_symbol(lit->list.items[0], "not") &&
                            lit->list.items[1]->kind == SEXPR_SYMBOL) ? lit->list.items[1] : NULL;
        Smt2Var *v = sym ? _find_var(fe, sym->sym.str, sym->sym.len) : NULL;
        if (!v || !v->is_bool) { free(roots); return 0; }
        roots[n] = dvs_builder_expr_binary(fe->builder, DVS_BIN_EQ,
                       dvs_builder_expr_var(fe->builder, v->var_id),
                       _bv_const(fe, positive ? 1 : 0, 1));
        if (roots[n++] == EXPR_NULL) { free(roots); return 0; }
    }
    fe->n_last_assump = 0;
    for (uint32_t i = 0; i < ls->list.count && fe->n_last_assump < SMT2_MAX_ASSUMPS; i++) {
        const Sexpr *lit = ls->list.items[i];
        const Sexpr *sym = lit->kind == SEXPR_SYMBOL ? lit : lit->list.items[1];
        uint32_t k = fe->n_last_assump++;
        uint32_t cpy = sym->sym.len < SMT2_MAX_NAME - 1 ? sym->sym.len : SMT2_MAX_NAME - 1;
        memcpy(fe->last_assump_names[k], sym->sym.str, cpy);
        fe->last_assump_names[k][cpy] = '\0';
        fe->last_assump_positive[k] = (uint8_t)(lit->kind == SEXPR_SYMBOL);
    }
    int ok = _check_sat_assuming_session(fe, roots, n);
    free(roots);
    if (ok) fe->last_assump_unsat = (fe->last_result == DVS_SOLVE_UNSAT);
    return ok;
}

static int _cmd_check_sat_assuming(Smt2Frontend *fe, const Sexpr *cmd) {
    if (cmd->list.count != 2 || cmd->list.items[1]->kind != SEXPR_LIST) {
        _input_error(fe, "check-sat-assuming requires a literal list");
        return -1;
    }

    fe->core_replayable = 0;   /* the core would have to cover the assumptions */

    /* Tainted or poisoned context -> honest `unknown` (see _cmd_check_sat). */
    if (fe->incomplete || fe->input_poisoned) {
        SMT2_EMIT_UNKNOWN(fe, NULL);
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
            default:            SMT2_EMIT_UNKNOWN(fe, NULL); break;
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

    /* A problem that bit-blasts: the assumptions go in a scope of the
     * bit-blast session (plan piece 6.1), so successive properties share
     * the design's encoding. The CDCL path below cannot take variables
     * wider than 64 bits, so these used to answer `unknown`. */
    _end_assumptions(fe);
    _route_qfbv(fe);
    if (_engine_is_bitblast(fe) && _check_sat_assuming_bb(fe, cmd->list.items[1]))
        return 0;

    int crc = _ensure_compiled(fe);
    if (crc == -2) {
        /* The assertions alone are unsat; the empty set of assumptions is a
         * core. */
        fe->n_last_assump = 0;
        fe->last_assump_unsat = 1;
        fprintf(fe->out, "unsat\n");
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
    }
    if (crc < 0) {
        SMT2_EMIT_UNKNOWN(fe, "incomplete: the CDCL compile could not build a context");
        fflush(fe->out);
        return -1;
    }
    _end_assumptions(fe);
    if (fe->has_result) dvs_solver_reset(fe->ctx);
    fe->has_result = 0;
    int frc = _flush_aux(fe);
    if (frc < 0 && frc != -2) {
        fe->needs_bitblast = 1;            /* the context missed some (B87) */
        if (!fe->nb_why) fe->nb_why = "flush-failed";
        SMT2_EMIT_UNKNOWN(fe, "incomplete: an assertion could not be added to the CDCL context; later checks bit-blast");
        fflush(fe->out);
        return -1;
    }
    if (fe->flush_unsat) {
        /* The assertions alone are unsat (a flush met the conflict). The
         * checkpoint below loses the context's record of it: the next
         * check-sat-assuming answered `sat`. */
        fe->n_last_assump = 0;
        fe->last_assump_unsat = 1;
        fprintf(fe->out, "unsat\n");
        fflush(fe->out);
        fe->last_result = DVS_SOLVE_UNSAT;
        fe->has_result = 1;
        return 0;
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
            _input_error(fe, "malformed assumption literal");
            dvs_solver_restore(fe->ctx, (uint32_t)cp);
            return -1;
        }
        Smt2Var *v = _find_var(fe, name, nlen);
        if (!v) {
            _input_error(fe, "unknown assumption literal '%.*s'", (int)nlen, name);
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
        /* The validation net check-sat has: a constraint the compile could
         * not take is missing from the context, so its `sat` must be checked
         * against the whole problem. Without it, `p == (not (x < 3))` under
         * the assumption (not p) answered sat with x = 4. */
        const char *mode_env = getenv("DV_VALIDATE_MODEL");
        int mode = mode_env ? atoi(mode_env) : 2;
        if (fe->last_result == DVS_SOLVE_OK && fe->problem && mode > 0) {
            int viol = _validate_all(fe);
            if (viol > 0) {
                fprintf(fe->err, "model-validation: %d constraint(s) violated%s\n", viol,
                        mode >= 2 ? "; downgrading sat -> unknown" : "");
                if (mode >= 2) fe->last_result = DVS_SOLVE_TIMEOUT;
            }
        }
        if (fe->last_result == DVS_SOLVE_TIMEOUT && !getenv("DV_NO_BITBLAST")) {
            /* Escalate to the bit-blast session, as check-sat escalates. */
            dvs_solver_restore(fe->ctx, (uint32_t)cp);
            if (_check_sat_assuming_bb(fe, lits)) return 0;
            cp = dvs_solver_checkpoint(fe->ctx);
            if (cp < 0) {
                fprintf(fe->err, "error: check-sat-assuming: no checkpoint slot\n");
                return -1;
            }
        }
        switch (fe->last_result) {
        case DVS_SOLVE_OK:      fprintf(fe->out, "sat\n"); break;
        case DVS_SOLVE_UNSAT:   fprintf(fe->out, "unsat\n"); break;
        case DVS_SOLVE_TIMEOUT: SMT2_EMIT_UNKNOWN(fe, _cdcl_unknown_why(fe)); break;
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
    sexpr_arena_init(&fe->rewrite_arena, 4096);
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
    if (fe->ctx)         dvs_solver_free(fe->ctx);
    free(fe->vars);
    free(fe->var_idx);
    free(fe->fun_idx);
    free(fe->funs);
    free(fe->subst_stack);
    free(fe->fun_memo);
    free(fe->sort_aliases);
    free(fe->checked);
    free(fe->decl_hist);
    sexpr_arena_destroy(&fe->persistent_arena);
    sexpr_arena_destroy(&fe->rewrite_arena);
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
    free(fe->push);
    free(fe->vproblem);
    free(fe->vbase);
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
    /* The array engine's bb_solver was built over fe->problem, freed below;
     * a session's holds the assertions being reset. */
    if ((fe->bb_over_problem || fe->bb_sess) && fe->bb_solver) {
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
    void    *shape_tab = fe->shape_tab;
    uint32_t n_input_errors = fe->n_input_errors;   /* drives the exit status */
    int8_t   check_models = fe->check_models;
    /* operator rewrites: parsed into rewrite_arena, which is kept */
    uint32_t n_rewrites = fe->n_rewrites;
    SexprArena rarena = fe->rewrite_arena;
    struct Smt2Rewrite rewrites[SMT2_MAX_REWRITES];
    memcpy(rewrites, fe->rewrites, n_rewrites * sizeof rewrites[0]);
    /* Reused allocations (reset in place below rather than freed). */
    dvs_builder_t *builder = fe->builder;
    SexprArena           parena  = fe->persistent_arena;

    /* Free exactly what destroy() frees, EXCEPT builder + persistent_arena. */
    if (fe->problem) free(fe->problem);
    for (uint32_t i = 0; i < fe->n_aux_problems; i++) free(fe->aux_problems[i]);
    free(fe->aux_problems);
    if (fe->ctx)         dvs_solver_free(fe->ctx);
    free(fe->vars);
    free(fe->var_idx);
    free(fe->fun_idx);
    free(fe->funs);          /* re-zeroed by the memset below; realloc'd on next define-fun */
    free(fe->subst_stack);   /* re-zeroed by the memset below; realloc'd on next let */
    free(fe->fun_memo);
    free(fe->sort_aliases);
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
    free(fe->push);
    free(fe->vproblem);
    free(fe->vbase);
    _cmd_alloc_reset(fe);

    _truncate_named(fe, 0);
    free(fe->named);
    free(fe->core_hist);   /* the copies live in persistent_arena, reset below */
    free(fe->checked);     /* likewise */
    free(fe->decl_hist);   /* likewise */

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
    fe->shape_tab = shape_tab;
    fe->n_input_errors = n_input_errors;
    fe->check_models = check_models;
    fe->n_rewrites = n_rewrites;
    memcpy(fe->rewrites, rewrites, n_rewrites * sizeof rewrites[0]);
    fe->rewrite_arena = rarena;
    fe->builder = builder;
    fe->persistent_arena = parena;
}

static int _dispatch(Smt2Frontend *fe, const Sexpr *cmd);

/* Commands that shape the assertion set, and so are replayed to minimise an
 * unsat core. Options, info and queries are not: none can change a verdict. */
static int _core_shapes_assertions(const Sexpr *head) {
    static const char *const kinds[] = {
        "set-logic", "declare-sort", "define-sort", "declare-const", "declare-fun",
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

static void smt2_frontend_soft_reset(Smt2Frontend *fe);

/* Commands (reset-assertions) keeps: those that declare or define at level 0,
 * and options. */
static int _survives_reset_assertions(const Sexpr *head) {
    static const char *const kinds[] = {
        "set-logic", "set-option", "set-info", "declare-sort", "define-sort",
        "declare-const", "declare-fun", "define-fun", "declare-datatypes",
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (sexpr_is_symbol(head, kinds[i])) return 1;
    return 0;
}

static void _decl_record(Smt2Frontend *fe, const Sexpr *cmd) {
    if (fe->n_decl_hist == fe->decl_hist_cap) {
        uint32_t cap = fe->decl_hist_cap ? fe->decl_hist_cap * 2 : 64;
        const Sexpr **g = (const Sexpr **)realloc((void *)fe->decl_hist, cap * sizeof(*g));
        if (!g) return;
        fe->decl_hist = g;
        fe->decl_hist_cap = cap;
    }
    const Sexpr *copy = _sexpr_deep_copy(&fe->persistent_arena, cmd);
    if (copy) fe->decl_hist[fe->n_decl_hist++] = copy;
}

/* (reset-assertions): SMT-LIB 2.6 removes every assertion and pops every
 * level, keeping level-0 declarations, definitions and options. A compiled
 * constraint cannot be taken back, so this is a (reset) and a replay of those
 * commands. It used to be a no-op that kept every assertion in force: `x = 1;
 * reset-assertions; x = 2` answered unsat (B77). Like (reset), it clears an
 * input-error poison: the replay re-declares only what succeeded. */
static int _cmd_reset_assertions(Smt2Frontend *fe) {
    uint32_t n = fe->n_decl_hist;
    SexprArena tmp;
    sexpr_arena_init(&tmp, 16384);
    const Sexpr **cmds = (const Sexpr **)malloc((n ? n : 1) * sizeof(*cmds));
    if (!cmds) {
        sexpr_arena_destroy(&tmp);
        _input_error(fe, "reset-assertions: out of memory");
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) cmds[i] = _sexpr_deep_copy(&tmp, fe->decl_hist[i]);
    smt2_frontend_soft_reset(fe);
    Smt2MemOut m;
    int quiet = _memout_open(&m);
    FILE *real = fe->out;
    if (quiet) fe->out = m.f;
    for (uint32_t i = 0; i < n; i++)
        if (cmds[i]) smt2_frontend_dispatch(fe, cmds[i]);
    fe->out = real;
    if (quiet) _memout_finish(&m, NULL);
    free((void *)cmds);
    sexpr_arena_destroy(&tmp);
    return 0;
}

static int _silent_command(const Sexpr *head);

int smt2_frontend_dispatch(Smt2Frontend *fe, const Sexpr *cmd) {
    fe->model_reused = 0;
    fe->cmd_dropped = 0;
    fe->last_validate_viol = -1;
    int record = fe->produce_unsat_cores && cmd && cmd->kind == SEXPR_LIST
              && cmd->list.count > 0 && _core_shapes_assertions(cmd->list.items[0]);
    int keep = cmd && cmd->kind == SEXPR_LIST && cmd->list.count > 0
            && fe->push_depth == 0 && _survives_reset_assertions(cmd->list.items[0]);
    uint32_t n_named = fe->n_named;
    uint32_t n_errors = fe->n_input_errors;
    int rc = _dispatch(fe, cmd);
    if (fe->print_success && rc >= 0 && fe->n_input_errors == n_errors && cmd
            && cmd->kind == SEXPR_LIST && cmd->list.count > 0
            && _silent_command(cmd->list.items[0])) {
        fprintf(fe->out, "success\n");
        fflush(fe->out);
    }
    if (record && rc == 0)
        _core_record(fe, cmd, fe->n_named > n_named ? (int32_t)fe->n_named - 1 : -1);
    if (keep && rc == 0 && fe->n_input_errors == n_errors)
        _decl_record(fe, cmd);
    return rc;
}

/* The model check (plan 2.5). Evaluate every assertion in scope -- and a
 * check-sat-assuming's assumptions -- under the model just found, with
 * smt2_eval.c. Returns -1 when one is false (the model is wrong), else 0. A
 * term the evaluator cannot evaluate, or whose value rests on an array element
 * no constraint reads (arbitrary in the model), proves nothing either way and
 * is counted as inconclusive. */
static int _model_check(Smt2Frontend *fe, const Sexpr *cmd) {
    Smt2EvalHost host = _eval_host(fe);
    Smt2Eval *ev = smt2_eval_new(&host);
    if (!ev) return 0;
    const Sexpr *assumps = NULL;
    if (!fe->verilator_mode && sexpr_is_command(cmd, "check-sat-assuming")
            && cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_LIST)
        assumps = cmd->list.items[1];   /* Verilator mode ignores them, by design */
    uint32_t n = fe->n_checked + (assumps ? assumps->list.count : 0);
    uint32_t inconclusive = 0;
    int bad = 0;
    for (uint32_t i = 0; i < n && !bad; i++) {
        const Sexpr *t = i < fe->n_checked ? fe->checked[i]
                                           : assumps->list.items[i - fe->n_checked];
        const Smt2Value *v = smt2_eval(ev, t);
        if (!v || smt2_value_kind(v) != SMT2_VAL_BOOL) {
            inconclusive++;
            if (fe->print_stats || getenv("DV_LOG"))
                fprintf(fe->err, "model-check: %s %u not evaluated: %s\n",
                        i < fe->n_checked ? "assertion" : "assumption",
                        i < fe->n_checked ? i + 1 : i - fe->n_checked + 1,
                        v ? "not Bool" : smt2_eval_error(ev));
            continue;
        }
        if (smt2_value_u64(v)) continue;
        if (smt2_value_arbitrary(v)) { inconclusive++; continue; }
        bad = 1;
        if (i < fe->n_checked)
            snprintf(fe->model_check_why, sizeof fe->model_check_why,
                     "assertion %u is false under the model", i + 1);
        else
            snprintf(fe->model_check_why, sizeof fe->model_check_why,
                     "assumption %u is false under the model", i - fe->n_checked + 1);
    }
    smt2_eval_free(ev);
    if (bad)
        fprintf(fe->err, "model-check: %s; answering unknown\n", fe->model_check_why);
    if (inconclusive && (fe->print_stats || getenv("DV_LOG")))
        fprintf(fe->err, "model-check: %u of %u inconclusive\n", inconclusive, n);
    return bad ? -1 : 0;
}

/* check-sat / check-sat-assuming with the model check: the answer is held back
 * until the model has been checked, so a wrong `sat` never reaches the driver. */
/* Record the check-sat just run for the exit status (smt2_main.c). */
static void _note_answer(Smt2Frontend *fe, int rc) {
    fe->answered = 1;
    fe->answered_result = rc == 0 && fe->has_result ? fe->last_result : DVS_SOLVE_TIMEOUT;
}

static int _check_sat_checked(Smt2Frontend *fe, const Sexpr *cmd,
                              int (*run)(Smt2Frontend *, const Sexpr *)) {
    fe->model_check_why[0] = '\0';
    fe->unknown_why[0] = '\0';
    Smt2MemOut m;
    if (!_check_models_on(fe) || !_memout_open(&m)) {
        int rc = run(fe, cmd);
        _note_answer(fe, rc);
        return rc;
    }
    FILE *real = fe->out;
    fe->out = m.f;
    int rc = run(fe, cmd);
    fe->out = real;
    if (rc == 0 && fe->has_result && fe->last_result == DVS_SOLVE_OK && !fe->model_reused
            && _model_check(fe, cmd) < 0) {
        _memout_finish(&m, NULL);
        fe->last_result = DVS_SOLVE_TIMEOUT;
        fprintf(real, "unknown\n");
    } else {
        _memout_finish(&m, real);
    }
    fflush(real);
    _note_answer(fe, rc);
    return rc;
}

static int _dispatch(Smt2Frontend *fe, const Sexpr *cmd) {
    /* Reset per-command transient allocations from prior command */
    _cmd_alloc_reset(fe);
    _fun_memo_clear(fe);

    if (!cmd) return 0;
    if (cmd->kind != SEXPR_LIST) {
        _input_error(fe, "invalid command, '(' expected");
        return 0;
    }
    if (cmd->list.count == 0) {
        _input_error(fe, "empty command '()'");
        return 0;
    }

    Sexpr *head = cmd->list.items[0];
    if (head->kind != SEXPR_SYMBOL) {
        _input_error(fe, "invalid command, a command name is expected");
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
    if (sexpr_is_symbol(head, "define-sort"))
        return _cmd_define_sort(fe, cmd);
    if (sexpr_is_symbol(head, "declare-const"))
        return _cmd_declare_const(fe, cmd);
    if (sexpr_is_symbol(head, "declare-fun"))
        return _cmd_declare_const(fe, cmd);
    if (sexpr_is_symbol(head, "define-fun"))
        return _cmd_define_fun(fe, cmd);
    if (sexpr_is_symbol(head, "assert"))
        return _cmd_assert(fe, cmd);
    if (sexpr_is_symbol(head, "check-sat"))
        return _check_sat_checked(fe, cmd, _cmd_check_sat);
    if (sexpr_is_symbol(head, "check-sat-assuming"))
        return _check_sat_checked(fe, cmd, _cmd_check_sat_assuming);
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
        _input_error(fe, "unsupported command 'declare-datatype'");
        return 0;
    }
    if (sexpr_is_symbol(head, "reset-assertions"))
        return _cmd_reset_assertions(fe);
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
    if (sexpr_is_symbol(head, "get-option"))
        return _cmd_get_option(fe, cmd);
    if (sexpr_is_symbol(head, "get-assertions"))
        return _cmd_get_assertions(fe);

    /* A query changes no assertion, so not answering it poisons nothing. */
    if (head->sym.len > 4 && memcmp(head->sym.str, "get-", 4) == 0) {
        _query_error(fe, "unsupported command '%.*s'", (int)head->sym.len, head->sym.str);
        return 0;
    }
    _input_error(fe, "unsupported command '%.*s'", (int)head->sym.len, head->sym.str);
    return 0;
}

/* Commands whose only response is `success` under :print-success. */
static int _silent_command(const Sexpr *head) {
    static const char *const names[] = {
        "set-logic", "set-option", "set-info", "declare-sort", "define-sort",
        "declare-const", "declare-fun", "define-fun", "declare-datatypes", "assert",
        "push", "pop", "reset", "reset-assertions", "exit", NULL };
    for (int i = 0; names[i]; i++)
        if (sexpr_is_symbol(head, names[i])) return 1;
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
