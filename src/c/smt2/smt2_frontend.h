#ifndef SMT2_FRONTEND_H
#define SMT2_FRONTEND_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "dvs_builder.h"
#include "dvs_ctx.h"
#include "dvs_search.h"
#include "dvs_block_alloc.h"
#include "smt2/smt2_parser.h"

/* Forward declaration: full definition in dvs_bbsolver.h. Used when
 * DV_ENGINE=bitblast to keep the bit-blast solver alive across check-sat
 * and get-value commands. */
typedef struct dvs_bbsolver_s dvs_bbsolver_t;

#ifdef __cplusplus
extern "C" {
#endif

/* B12 depth guard: set this to the stack size (bytes) of the thread the SMT2
 * translator runs on, so the guard can size itself to the real stack rather
 * than RLIMIT_STACK (which governs only the main thread). 0 = use RLIMIT_STACK.
 * The CLI (smt2_main) runs the solve on an explicit large-stack pthread and
 * sets this to that stack size. See _translate_depth_limit in the .c. */
extern size_t g_smt2_translate_stack_bytes;

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SMT2_MAX_NAME       128
#define SMT2_MAX_BV_BITS    128   /* widest BitVec sort accepted (Phase W1: >64-bit is bitblast-routed) */
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

/* Taint the context to `unknown`, recording why + where. Every `unknown` that
 * comes from an unsupported construct should go through this, so the reason is
 * always recoverable (DV_LOG=1). Never affects soundness -- it only ever turns a
 * result into `unknown`. */
/* Emit `unknown` on the result stream, recording WHERE it came from. Phase 0 of
 * docs/cdcl_verilator_coverage_plan.md: an `unknown` with no explanation costs a
 * manual delta-debug every time, so every emission site is traceable under
 * DV_LOG=1. Purely diagnostic. */
#define SMT2_EMIT_UNKNOWN(fe) do {                                        \
        if ((fe)->print_stats || getenv("DV_LOG"))                        \
            fprintf((fe)->err, "unknown-from: smt2_frontend.c:%d%s%s\n",   \
                    __LINE__,                                             \
                    (fe)->incomplete_why ? " -- " : "",                   \
                    (fe)->incomplete_why ? (fe)->incomplete_why : "");    \
        fprintf((fe)->out, "unknown\n");                                  \
    } while (0)

#define SMT2_TAINT(fe, why_) do {              \
        (fe)->incomplete = 1;                  \
        (fe)->incomplete_why  = (why_);        \
        (fe)->incomplete_line = __LINE__;      \
    } while (0)

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

/* One recorded command for get-unsat-core replay: a deep copy of the command
 * and, for a :named assertion, its index in named[] (-1 otherwise). */
typedef struct {
    const Sexpr *cmd;
    int32_t      named;
} Smt2CoreCmd;

/* ------------------------------------------------------------------ */
/* Array sort and value types                                          */
/* ------------------------------------------------------------------ */

/* Description of an (Array (_ BitVec M) (_ BitVec N)) sort.
 *
 * A nested (Array (_ BitVec I) (Array (_ BitVec J) (_ BitVec N))) -- Verilator's
 * queues of queues and arrays in structs -- is one flat array keyed by the
 * concatenation of both indices: addr_width = I + J, inner_addr = J. */
typedef struct {
    uint8_t  addr_width;   /* M (I + J when nested) */
    uint8_t  data_width;   /* N */
    uint8_t  inner_addr;   /* J for a nested sort, else 0 */
    uint8_t  _pad[1];
} Smt2ArraySort;

/* Frontend-side array value handle: vector of n_elems ExprRefs of width N.
 * Leaf array vars (from declare-const/declare-fun) are persistent (malloc).
 * Intermediate values (store, as-const, ite) live in the per-command pool.
 *
 * R1 metadata: when this value was produced by a symbolic-index store,
 * store_idx_varid holds the solver var_id of the index variable and
 * store_val holds the dvs_expr_t of the written value, so that
 * select(store(a,i,v),i) can be rewritten to v without an ITE chain.
 * store_idx_varid == UINT32_MAX means no R1 metadata is available. */
/* Abstract-array node kinds (DV_ARRAY word-level path). See is_abstract. */
#define SMT2_ANODE_BASE   0   /* free array variable (leaf)              */
#define SMT2_ANODE_STORE  1   /* store(parent, store_idx_ref, store_val) */
#define SMT2_ANODE_CONST  2   /* constant array: every read == store_val */
#define SMT2_ANODE_ITE    3   /* ite(cond_ref, parent, else_node)        */

typedef struct Smt2ArrayValue {
    Smt2ArraySort sort;
    uint32_t      n_elems;        /* always 1 << sort.addr_width (dense only) */
    dvs_expr_t      *elems;          /* n_elems entries; width = sort.data_width */
    uint32_t      store_idx_varid;/* R1: var_id of symbolic store index, or UINT32_MAX */
    dvs_expr_t       store_val;      /* R1: dvs_expr_t of store value, or EXPR_NULL */

    /* Word-level abstract array (DV_ARRAY). When is_abstract=1 this value is a
     * node in a persistent select/store DAG rather than a dense elems[] vector.
     * BASE nodes are owned by array_vars[]; STORE/CONST/ITE nodes are created
     * during translation and owned by the frontend's anodes[] list. Reads are
     * abstract (fresh vars); read-over-write + congruence axioms are emitted at
     * check-sat (Phase A eager) or lazily on a model (Phase B). */
    uint8_t       is_abstract;
    uint8_t       akind;            /* SMT2_ANODE_* */
    struct Smt2ArrayValue *parent;  /* STORE parent / ITE then-branch */
    struct Smt2ArrayValue *else_node; /* ITE else-branch */
    dvs_expr_t       store_idx_ref;    /* STORE index dvs_expr_t (store_val = value) */
    dvs_expr_t       cond_ref;         /* ITE condition dvs_expr_t */

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
    uint64_t     *sparse_idx_hi;  /* their bits 64..127 (128-bit keys: Verilator's
                                   * string-keyed associative arrays) */
    uint32_t     *sparse_varid;   /* solver var_id of each index's element var.
                                   * Stored as var_id (not dvs_expr_t) so get-value
                                   * survives the dvs_builder_reset that follows
                                   * compilation -- like the dense path's
                                   * name->var lookup. */

    /* Slice view: (select A k) of a nested array A at a constant k, an array
     * of the inner sort whose element j is A's flat element (k ++ j). A
     * per-command transient; only select reads it. */
    struct Smt2ArrayValue *slice_of;
    uint64_t      slice_lo, slice_hi;  /* k */
} Smt2ArrayValue;

/* Symbol-table entry for an array-typed declared variable. */
typedef struct {
    char            name[SMT2_MAX_NAME];
    Smt2ArraySort   sort;
    Smt2ArrayValue *value;   /* never NULL after declare */
} Smt2ArrayVar;

/* One abstract-array read: read_var == select(node, idx). Recorded per symbolic
 * select on an abstract array (DV_ARRAY), plus the intermediate reads that
 * read-over-write pushes down the store chain. idx_varid caches the index when
 * it is a plain variable so distinct dvs_expr_t nodes for the same variable dedup
 * to one read var. */
typedef struct Smt2ArrayRead {
    Smt2ArrayValue *node;
    dvs_expr_t         idx_ref;
    uint32_t        idx_varid;   /* var_id of idx if a plain var, else UINT32_MAX */
    uint32_t        read_varid;
    uint16_t        width;
    uint8_t         emitted;     /* lazy loop: one-step defining constraint added */
} Smt2ArrayRead;

/* One abstract array equality (a == b), reified onto boolean var p_varid.
 * Consistency (p -> reads equal at every shared index) + a Skolem extensionality
 * witness (¬p -> read(a,wit) != read(b,wit)) reify p soundly in both polarities.
 * wit_idx_ref is the witness index dvs_expr_t (a fresh addr-width var). */
typedef struct Smt2ArrayEq {
    Smt2ArrayValue *a;
    Smt2ArrayValue *b;
    uint32_t        p_varid;
    dvs_expr_t         wit_idx_ref;
} Smt2ArrayEq;

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
    dvs_expr_t             cached_ref;      /* EXPR_NULL until translated */
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
    dvs_expr_t  ref;
    uint16_t width;  /* bit width (up to 65535) */
} TypedExpr;

/* ------------------------------------------------------------------ */
/* Frontend state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    dvs_builder_t *builder;
    dvs_ctx_t            *ctx;          /* NULL until check-sat */
    dvs_problem_t        *problem;      /* finalized; NULL until check-sat */
    size_t               problem_size;

    /* Retained aux SolveProblems from _flush_aux. The model-validation
     * pass walks these in addition to fe->problem so that user-asserts
     * added incrementally (after the first check-sat) can also be
     * checked. Each entry is freed in smt2_frontend_destroy. */
    dvs_problem_t       **aux_problems;
    uint32_t             n_aux_problems;
    uint32_t             aux_problems_cap;
    /* Names from `(assert (! t :named N))`, in assertion order, for
     * get-unsat-core. Scoped by push/pop (push_n_named); freed on reset. */
    /* 1 iff fe->bb_solver holds the model for the CURRENT check-sat result.
     * bb_solver outlives its answer (the Verilator identity cache carries it
     * across (reset)), so model readback must not trust it just because it
     * exists: a later CDCL answer would otherwise report the stale bit-blast
     * model (B36). Cleared at every check-sat; set by the bit-blast paths. */
    uint8_t              bb_model_valid;
    /* Constraints asserted after the CDCL compile reach the ctx as aux
     * problems. So that a CDCL `unknown` can still escalate to bitblast with
     * EVERY assertion, the builder is kept (not reset) after compile while
     * cdcl_retained is set; aux_mark is where the last hand-off to CDCL ended.
     * Cleared by pop (the builder would still hold retracted assertions) and
     * when the builder outgrows SMT2_RETAIN_MAX_BYTES. */
    uint8_t              cdcl_retained;
    dvs_builder_mark_t          aux_mark;
    /* Problem that bb_solver was built from when it is NOT fe->problem (the
     * escalation's full rebuild). Owned; freed together with bb_solver. */
    dvs_problem_t        *bb_problem;
    char               **named;
    uint32_t             n_named;
    uint32_t             named_cap;
    /* get-unsat-core minimisation. While :produce-unsat-cores is true, every
     * command that shapes the assertion set (set-logic, declarations,
     * definitions, assertions) is deep-copied here in order, scoped by push/pop
     * like named[]. get-unsat-core replays it in a scratch frontend to drop
     * named assertions the contradiction does not need. Off by default, so the
     * Verilator randomize() loop never pays for the copies. */
    int                  produce_unsat_cores;
    Smt2CoreCmd         *core_hist;
    uint32_t             n_core_hist;
    uint32_t             core_hist_cap;
    uint32_t             core_hist_at_check; /* n_core_hist at the last check-sat */
    uint8_t              core_replayable;    /* last unsat came from a plain check-sat */
    dvs_block_alloc_t   *block_alloc;
    void                *ctx_buf;      /* raw buffer for dvs_ctx_t */
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

    /* --verilator-hash=ignore (verilator mode only). Verilator 5.x follows
     * every satisfiable randomize() query with up to four random parity
     * ("hash") asserts, purely to coax variety out of a deterministic
     * solver. With this set, an assert of exactly that shape arriving after a
     * `sat` is recorded but not added, and the check-sat after it answers
     * `sat` with the model already found: the user's constraints still hold,
     * Verilator's variety request is not honoured. Off by default; the
     * results pages measure both settings. */
    int                  vlt_hash_ignore;
    int                  vlt_hash_pending;  /* a hash was skipped since the last check-sat */

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

    /* B12 guard: current depth of the _translate_tagged recursion. A single
     * expression nested deeper than SMT2_MAX_TRANSLATE_DEPTH would overflow the
     * C stack (~1490 with the default 8-12 MB), so we bail to `unknown` (set
     * `incomplete`) instead of crashing. Transient per translate call; a stray
     * non-zero from an earlier bail is harmless (only ever increments/compares)
     * but it is reset to 0 at the start of every top-level expression. */
    uint32_t             translate_depth;

    /* Set when a construct is lowered into a composed expression the CDCL
     * engine cannot solve soundly (e.g. signed bvsdiv/bvsrem lower to an ITE of
     * unsigned divisions whose result CDCL leaves unpinned -> wrong model). The
     * bitblast engine handles these exactly, so the solve is forced onto it. */
    int                  needs_bitblast;

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
    dvs_result_t          last_result;
    int                  has_result;   /* 1 after check-sat */
    /* Checkpoint a successful default-mode check-sat-assuming left open, plus
     * one (0: none). It holds the assumption pins while get-value and
     * get-unsat-assumptions read the answer; _end_assumptions restores it
     * before the next command that changes the solver state. */
    uint32_t             assump_cp1;

    /* Bit-blast solver kept alive across check-sat and get-value when
     * DV_ENGINE=bitblast. NULL when the CDCL engine is in use. Freed in
     * smt2_frontend_destroy and replaced on each (check-sat). */
    dvs_bbsolver_t      *bb_solver;

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

    /* Parameterized macros (define-fun). Heap-allocated, lazily grown (was an
     * inline funs[SMT2_MAX_FUNS] ~9.8 MB array — kept off the struct so the
     * blanket memset(fe) in init/destroy/soft_reset stays cheap; that zeroing
     * dominated one-shot startup and every Verilator (reset). SMT2_MAX_FUNS
     * remains the hard cap. See docs/perf_sweep_easy_band_2026-07-21.md). */
    Smt2FunDef          *funs;
    uint32_t             funs_cap;
    uint32_t             n_funs;

    /* Substitution stack used during define-fun body + let translation.
     * Heap-backed and grown on demand (see _subst_reserve) so deeply-nested
     * `let` (thousands deep, common in tool-emitted SMT-LIB) is not silently
     * truncated to `unknown`. SMT2_MAX_SUBST is only the initial capacity. */
    Smt2Subst           *subst_stack;
    uint32_t             subst_cap;
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

    /* Push/pop stack: maps to dvs_solver_checkpoint indices */
    uint32_t             push_stack[32];
    uint32_t             push_n_vars[32];
    uint32_t             push_n_array_vars[32];
    uint32_t             push_n_aux_problems[32];
    uint32_t             push_n_named[32];
    uint32_t             push_n_core_hist[32];
    dvs_builder_mark_t   push_bmark[32];     /* builder position at each push */
    uint8_t              push_incomplete[32];  /* `incomplete` at each push */
    uint32_t             push_n_anodes[32];    /* abstract-array state at each push */
    uint32_t             push_n_areads[32];
    uint32_t             push_n_aeqs[32];
    uint32_t             push_depth;

    /* Incremental state: 1 once the initial problem has been compiled. */
    int                  compiled;
    int                  has_aux;

    /* Bit-blast incremental state (B14). The bit-blast engine reads fe->problem
     * directly and, unlike the CDCL path, has no _flush_aux equivalent -- so an
     * (assert) issued AFTER a (check-sat) used to sit in the builder forever and
     * never reach the solver, silently producing a wrong `sat`.
     *
     *   problem_dirty    -- constraints were added to the builder after
     *                       fe->problem was finalized; it must be rebuilt.
     *   builder_retained -- fe->problem was finalized WITHOUT a following
     *                       dvs_builder_reset, so the builder still holds the FULL
     *                       constraint set and re-finalizing reproduces it.
     *                       Cleared by any path that resets the builder
     *                       (_ensure_compiled, _check_sat_array), after which a
     *                       rebuild would silently lose the earlier constraints
     *                       -- so we answer `unknown` instead. */
    int                  problem_dirty;
    int                  builder_retained;

    /* Verilator randomization routing. CDCL is a near-uniform sampler; the
     * bitblast path's _diversify (bit-flip repair over one cached model) is
     * measurably skewed -- so in verilator mode we prefer CDCL and fall back to
     * bitblast only where CDCL cannot answer. CDCL is correct-or-unknown and
     * `unknown` escalates to bitblast, so the routing cannot change a verdict.
     *
     * The decision is STICKY per constraint set: a randomize() loop re-solves
     * the same instance thousands of times, so a failed CDCL probe must be paid
     * once, not once per call.
     *   verilator_cdcl  -- master enable (DV_VERILATOR_CDCL=0 opts out)
     *   cdcl_probe_fp   -- problem fingerprint the decision below applies to
     *   cdcl_route      -- 0 = not yet probed, 1 = use CDCL, 2 = use bitblast */
    /* Why the context was tainted to `unknown` (diagnostic only). Most taint
     * sites used to be silent, so a fixture that answered `unknown` gave no
     * clue which construct was responsible. Reported by (check-sat) under
     * DV_LOG / --stats. */
    const char          *incomplete_why;
    uint32_t             incomplete_line;

    int                  verilator_cdcl;
    uint64_t             cdcl_probe_fp;
    uint8_t              cdcl_route;

    /* Array variable table (persistent: element vars survive commands) */
    Smt2ArrayVar         array_vars[SMT2_MAX_ARRAY_VARS];
    uint32_t             n_array_vars;

    /* Word-level abstract arrays (DV_ARRAY). array_lazy is set from the env at
     * init. anodes[] owns the STORE/CONST/ITE DAG nodes created during
     * translation (BASE nodes are owned by array_vars[]); areads[] is the read
     * table. Both persist translation->check-sat and are freed at reset/destroy. */
    int                  array_lazy;   /* DV_ARRAY set: every array abstract */
    int                  array_eager;  /* DV_ARRAY=eager: one-shot Ackermann oracle */
    int                  array_auto;   /* abstract only the sorts too large to expand
                                        * densely (default; DV_ARRAY=0 turns it off) */
    int                  bb_over_problem; /* bb_solver was built over fe->problem
                                           * (the array engine), not bb_problem */
    Smt2ArrayValue     **anodes;
    uint32_t             n_anodes, anodes_cap;
    Smt2ArrayRead       *areads;
    uint32_t             n_areads, areads_cap;
    /* Reads made by translation are areads[0, n_areads_user); a solve appends
     * its own after them, in fe->problem's pool. Those are dropped before the
     * next translation or solve, which rebuild from the builder. */
    uint32_t             n_areads_user;
    Smt2ArrayEq         *aeqs;
    uint32_t             n_aeqs, aeqs_cap;
    /* Open-addressing index over areads[] keyed by (node, idx): maps a read key
     * to its areads slot+1 (0 = empty), so find-or-create is O(1) instead of a
     * linear scan. Rebuilt on grow; NULL => fall back to linear scan (OOM). */
    uint32_t            *aread_hash;
    uint32_t             aread_hash_cap;   /* power of two, or 0 */

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
