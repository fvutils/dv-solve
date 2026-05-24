#ifndef SMT2_FRONTEND_H
#define SMT2_FRONTEND_H

#include <stdio.h>
#include <stdint.h>
#include "zsp_builder.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_block_alloc.h"
#include "smt2/smt2_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SMT2_MAX_NAME       128
#define SMT2_MAX_FUNS      8192   /* yosys-smtbmc emits one per BMC unroll step */
#define SMT2_MAX_FUN_PARAMS   8
#define SMT2_MAX_SORTS       16
#define SMT2_MAX_SORT_FUNS  256
#define SMT2_MAX_SORT_CONSTS 256
#define SMT2_MAX_SUBST       64

/* Array expansion limits */
#define SMT2_MAX_ARRAY_ADDR_BITS  10   /* max M: 2^10 = 1024 elements */
#define SMT2_MAX_ARRAY_VARS      256   /* max declared + mangled array vars */
#define SMT2_CMD_ALLOC_MAX       512   /* per-command transient allocations */

/* ------------------------------------------------------------------ */
/* Variable table entry                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char     name[SMT2_MAX_NAME];
    uint32_t var_id;
    uint8_t  width;
    uint8_t  is_signed;
    uint8_t  _pad[2];
} Smt2Var;

/* ------------------------------------------------------------------ */
/* Array sort and value types                                          */
/* ------------------------------------------------------------------ */

/* Description of an (Array (_ BitVec M) (_ BitVec N)) sort. */
typedef struct {
    uint8_t  addr_width;   /* M */
    uint8_t  data_width;   /* N */
    uint8_t  _pad[2];
} Smt2ArraySort;

/* Frontend-side array value handle: vector of n_elems ExprRefs of width N.
 * Leaf array vars (from declare-const/declare-fun) are persistent (malloc).
 * Intermediate values (store, as-const, ite) live in the per-command pool.
 *
 * R1 metadata: when this value was produced by a symbolic-index store,
 * store_idx_varid holds the solver var_id of the index variable and
 * store_val holds the ExprRef of the written value, so that
 * select(store(a,i,v),i) can be rewritten to v without an ITE chain.
 * store_idx_varid == UINT32_MAX means no R1 metadata is available. */
typedef struct {
    Smt2ArraySort sort;
    uint32_t      n_elems;        /* always 1 << sort.addr_width */
    ExprRef      *elems;          /* n_elems entries; width = sort.data_width */
    uint32_t      store_idx_varid;/* R1: var_id of symbolic store index, or UINT32_MAX */
    ExprRef       store_val;      /* R1: ExprRef of store value, or EXPR_NULL */
} Smt2ArrayValue;

/* Symbol-table entry for an array-typed declared variable. */
typedef struct {
    char            name[SMT2_MAX_NAME];
    Smt2ArraySort   sort;
    Smt2ArrayValue *value;   /* never NULL after declare */
} Smt2ArrayVar;

/* ------------------------------------------------------------------ */
/* Sort-typed function (declare-fun with arity >= 1)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char          name[SMT2_MAX_NAME];
    uint8_t       n_params;
    uint8_t       return_width;    /* 0 if array return, else BV/Bool width */
    uint8_t       is_bool_return;
    uint8_t       is_array_return; /* 1 if return sort is (Array ...) */
    Smt2ArraySort array_sort;      /* valid iff is_array_return */
} Smt2SortFun;

/* Sort-typed concrete constant (declare-const x |SomeSort|) */
typedef struct {
    char     name[SMT2_MAX_NAME];
    char     sort_name[SMT2_MAX_NAME];
} Smt2SortConst;

/* Parameterized macro (define-fun) */
typedef struct {
    char           name[SMT2_MAX_NAME];
    uint32_t       n_params;
    char           param_names[SMT2_MAX_FUN_PARAMS][SMT2_MAX_NAME];
    /* param_widths[i]: 0 if param is sort-typed (opaque/array), else BV/Bool width */
    uint8_t        param_widths[SMT2_MAX_FUN_PARAMS];
    uint8_t        param_is_sort[SMT2_MAX_FUN_PARAMS];
    const struct Sexpr *body;   /* lives in persistent arena */
    uint8_t        return_width; /* 1 for Bool, BV width otherwise; 0 for array return */
    uint8_t        is_bool_return;
    uint8_t        is_array_return;
    Smt2ArraySort  array_return_sort;  /* valid iff is_array_return */
} Smt2FunDef;

/* One substitution-stack entry: name -> Sexpr* */
typedef struct {
    const char         *name;
    uint32_t            len;
    const struct Sexpr *value;
    /* Memoized translation for let-bindings (has_cache == 1).
     * Stored as the three components of TaggedExpr to avoid a circular
     * dependency with the .c-local TaggedExpr typedef. */
    ExprRef             cached_ref;      /* EXPR_NULL until translated */
    uint16_t            cached_width;
    Smt2ArrayValue     *cached_array;
    int                 cached_leaf_kind;
    uint8_t             has_cache;       /* 0 for define-fun params, 1 for let bindings */
} Smt2Subst;

/* ------------------------------------------------------------------ */
/* TypedExpr -- expression reference with width tracking               */
/* ------------------------------------------------------------------ */

typedef struct {
    ExprRef  ref;
    uint16_t width;  /* bit width (up to 65535) */
} TypedExpr;

/* ------------------------------------------------------------------ */
/* Frontend state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    SolveProblemBuilder *builder;
    SolveCtx            *ctx;          /* NULL until check-sat */
    SolveProblem        *problem;      /* finalized; NULL until check-sat */
    size_t               problem_size;

    /* Retained aux SolveProblems from _flush_aux. The model-validation
     * pass walks these in addition to fe->problem so that user-asserts
     * added incrementally (after the first check-sat) can also be
     * checked. Each entry is freed in smt2_frontend_destroy. */
    SolveProblem       **aux_problems;
    uint32_t             n_aux_problems;
    uint32_t             aux_problems_cap;
    zsp_block_alloc_t   *block_alloc;
    void                *ctx_buf;      /* raw buffer for SolveCtx */
    size_t               ctx_buf_size;

    /* Symbol table */
    Smt2Var             *vars;
    uint32_t             n_vars;
    uint32_t             vars_cap;

    /* Configuration */
    int                  produce_models;
    uint64_t             seed;

    /* Result of last check-sat */
    SolveResult          last_result;
    int                  has_result;   /* 1 after check-sat */

    /* Output */
    FILE                *out;
    FILE                *err;

    /* Statistics */
    int                  print_stats;

    /* Persistent arena for define-fun bodies (deep-copied Sexpr trees) */
    SexprArena           persistent_arena;

    /* Opaque sorts */
    char                 sort_names[SMT2_MAX_SORTS][SMT2_MAX_NAME];
    uint32_t             n_sort_names;

    /* Sort-typed declare-fun entries (e.g., accessor functions) */
    Smt2SortFun          sort_funs[SMT2_MAX_SORT_FUNS];
    uint32_t             n_sort_funs;

    /* Sort-typed declare-const entries (state instances) */
    Smt2SortConst        sort_consts[SMT2_MAX_SORT_CONSTS];
    uint32_t             n_sort_consts;

    /* Parameterized macros (define-fun) */
    Smt2FunDef           funs[SMT2_MAX_FUNS];
    uint32_t             n_funs;

    /* Substitution stack used during define-fun body translation */
    Smt2Subst            subst_stack[SMT2_MAX_SUBST];
    uint32_t             subst_depth;

    /* Push/pop stack: maps to solver_checkpoint indices */
    uint32_t             push_stack[32];
    uint32_t             push_n_vars[32];
    uint32_t             push_n_array_vars[32];
    uint32_t             push_n_aux_problems[32];
    uint32_t             push_depth;

    /* Incremental state: 1 once the initial problem has been compiled. */
    int                  compiled;
    int                  has_aux;

    /* Array variable table (persistent: element vars survive commands) */
    Smt2ArrayVar         array_vars[SMT2_MAX_ARRAY_VARS];
    uint32_t             n_array_vars;

    /* Per-command transient allocation pool.
     * Freed at the start of each top-level command dispatch.
     * Used for intermediate Smt2ArrayValue objects (store, as-const, ite). */
    void                *cmd_allocs[SMT2_CMD_ALLOC_MAX];
    uint32_t             n_cmd_allocs;
} Smt2Frontend;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * Initialise a frontend.  Caller owns *fe and must call smt2_frontend_destroy().
 */
void smt2_frontend_init(Smt2Frontend *fe, FILE *out, FILE *err);

/**
 * Destroy frontend, freeing all internal allocations.
 */
void smt2_frontend_destroy(Smt2Frontend *fe);

/**
 * Dispatch one parsed S-expression (a top-level SMT-LIB2 command).
 *
 * @return  0 to continue, 1 on (exit), -1 on fatal error.
 */
int smt2_frontend_dispatch(Smt2Frontend *fe, const Sexpr *cmd);

#ifdef __cplusplus
}
#endif

#endif /* SMT2_FRONTEND_H */
