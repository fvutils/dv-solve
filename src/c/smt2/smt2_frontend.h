#ifndef SMT2_FRONTEND_H
#define SMT2_FRONTEND_H

#include <stdio.h>
#include <stdint.h>
#include "zsp_builder.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_block_alloc.h"
#include "smt2/smt2_parser.h"

/* Forward declaration: full definition in zsp_bbsolver.h. Used when
 * DV_ENGINE=bitblast to keep the bit-blast solver alive across check-sat
 * and get-value commands. */
typedef struct zsp_bbsolver_s zsp_bbsolver_t;

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
#define SMT2_MAX_ASSUMPS    256   /* assumption literals recorded per check-sat-assuming */

/* Array expansion limits */
#define SMT2_MAX_ARRAY_ADDR_BITS  10   /* max M for dense expansion: 2^10 = 1024 */
#define SMT2_MAX_ARRAY_VARS      256   /* max declared + mangled array vars */
#define SMT2_MAX_SPARSE_ELEMS   4096   /* max distinct indices of a sparse array */
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
    uint32_t      n_elems;        /* always 1 << sort.addr_width (dense only) */
    ExprRef      *elems;          /* n_elems entries; width = sort.data_width */
    uint32_t      store_idx_varid;/* R1: var_id of symbolic store index, or UINT32_MAX */
    ExprRef       store_val;      /* R1: ExprRef of store value, or EXPR_NULL */

    /* Sparse mode (is_sparse=1): used when addr_width is too large to expand
     * densely (2^M elements). The array is then materialized lazily as a map
     * from the concrete indices actually accessed to element ExprRefs -- a
     * standard lazy read-over-write instantiation valid whenever every access
     * uses a concrete index (a structural property of the constraints, not of
     * any particular front end). A select/store at a *symbolic* index on a
     * sparse array cannot be resolved without enumerating 2^M entries and is
     * reported as unknown (see fe->incomplete). elems/n_elems are unused. */
    uint8_t       is_sparse;
    uint32_t      n_sparse;
    uint32_t      sparse_cap;
    uint64_t     *sparse_idx;     /* concrete indices, n_sparse entries */
    uint32_t     *sparse_varid;   /* solver var_id of each index's element var.
                                   * Stored as var_id (not ExprRef) so get-value
                                   * survives the builder_reset that follows
                                   * compilation -- like the dense path's
                                   * name->var lookup. */
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
    /* Set while this define-fun parameter's argument is being lazily
     * translated. Lookups skip an entry that is currently expanding so a
     * self-referential argument (e.g. param `b` bound to `(bvult a b)` where
     * `b` is also a global) resolves the inner `b` to the outer/global binding
     * instead of re-entering this parameter and recursing until the C stack
     * overflows (SIGSEGV). */
    uint8_t             expanding;
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

    /* Verilator drop-in mode (--mode=verilator). When set, check-sat-assuming
     * is treated as a randomization-diversity request rather than a literal
     * assumption solve: Verilator emits per-bit "assumption" literals only to
     * coax variety out of a deterministic solver, and its relaxation loop then
     * needs up to (rand-bit-width) round-trips to converge -- which also hits
     * the bounds-vs-bits unsoundness of the CDCL pin path. In this mode we skip
     * the whole dance: solve the base problem once with a fresh seed and return
     * a natively well-distributed model. Off by default so the yosys/sby BMC
     * flow (which uses check-sat-assuming for real assumption-based solving)
     * keeps standard semantics. */
    int                  verilator_mode;
    uint64_t             div_counter;   /* seeds successive diversity solves */

    /* Verilator-mode identity cache. Verilator re-sends an identical problem
     * after every (reset), so a matching fingerprint lets us skip the bit-blast
     * + SAT re-solve and just re-diversify the cached bb_solver with the fresh
     * seed (see _check_sat_bitblast). bb_solver + these fields survive (reset).
     * reseed_period > 0 forces a full re-solve every Nth solve (env
     * DV_VERILATOR_RESEED) so coupled fields aren't pinned to one base model;
     * 0 = pure cache. */
    uint64_t             cached_fp;      /* fingerprint of the cached problem   */
    int                  cache_valid;    /* 1 = cached_fp/result are meaningful */
    int                  cached_result;  /* 1 = sat, 0 = unsat (iff cache_valid) */
    uint32_t             reseed_period;  /* 0 = pure cache; N = re-solve every Nth */

    /* "Incomplete" taint: set when a command in the current assertion context
     * could not be fully translated (an unsupported operator/sort/construct).
     * We keep the REPL alive and in-sync rather than exiting -- the next
     * (check-sat) reports `unknown` instead of an unsound sat/unsat, which a
     * driver (Verilator/yosys) reads as a clean solver error rather than
     * blocking on a closed pipe. Cleared by (reset)/(reset-assertions); saved
     * and restored across (push)/(pop) like the other incremental watermarks. */
    int                  incomplete;

    /* Logic set via (set-logic ...). Used by engine auto-routing in
     * _cmd_check_sat: QF_UFBV / QF_ABV / QF_AUFBV default to the
     * bitblast engine because the CDCL theory loop is dramatically
     * slower than bit-blast → AIG → kissat on those shapes (every
     * yosys-smtbmc-style BMC fixture solves <20ms under bitblast vs
     * 10s+ CDCL timeout). DV_ENGINE env still overrides ("bitblast"
     * or "bb" forces bitblast, "cdcl" forces CDCL). */
    enum {
        SMT2_LOGIC_UNSET = 0,
        SMT2_LOGIC_QF_BV,
        SMT2_LOGIC_QF_UFBV,
        SMT2_LOGIC_QF_ABV,
        SMT2_LOGIC_QF_AUFBV,
        SMT2_LOGIC_ALL,
    } logic;

    /* Result of last check-sat */
    SolveResult          last_result;
    int                  has_result;   /* 1 after check-sat */

    /* Bit-blast solver kept alive across check-sat and get-value when
     * DV_ENGINE=bitblast. NULL when the CDCL engine is in use. Freed in
     * smt2_frontend_destroy and replaced on each (check-sat). */
    zsp_bbsolver_t      *bb_solver;

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

    /* Assumption literals from the most recent check-sat-assuming, recorded so
     * (get-unsat-assumptions) can answer. When the last check-sat-assuming was
     * UNSAT we return the full recorded set: it is a valid (if non-minimal)
     * unsat-assumptions answer, and it lets Verilator's diversity-relaxation
     * loop (drop one conflicting literal, recheck) converge. */
    char                 last_assump_names[SMT2_MAX_ASSUMPS][SMT2_MAX_NAME];
    uint8_t              last_assump_positive[SMT2_MAX_ASSUMPS];
    uint32_t             n_last_assump;
    int                  last_assump_unsat;   /* 1 if last check-sat-assuming was unsat */

    /* Push/pop stack: maps to solver_checkpoint indices */
    uint32_t             push_stack[32];
    uint32_t             push_n_vars[32];
    uint32_t             push_n_array_vars[32];
    uint32_t             push_n_aux_problems[32];
    uint8_t              push_incomplete[32];  /* `incomplete` at each push */
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
