#ifndef DVS_CONTRADICTION_H
#define DVS_CONTRADICTION_H

#ifdef DVS_CONTRADICTION_ANALYSIS

#include <stdint.h>
#include <stddef.h>
#include "dvs_ctx.h"
#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constraint source info (caller-provided)                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t    constraint_id;
    const char *name;          /* e.g. "x + y <= 10" */
    const char *source_file;   /* e.g. "input.pss"   */
    uint32_t    source_line;
} ContraConstraintInfo;

/* ------------------------------------------------------------------ */
/* Relaxation suggestion for a single MUS constraint                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t constraint_id;      /* which MUS constraint             */
    int64_t  original_constant;  /* original constant value          */
    int64_t  relaxed_constant;   /* minimum feasible value           */
    int64_t  delta;              /* relaxed - original               */
    uint8_t  is_relaxable;       /* 0 = cannot be relaxed (binary)   */
    uint8_t  relax_direction;    /* 0 = increase, 1 = decrease, 2 = both */
    uint8_t  _rpad[6];
} ContraRelaxSuggestion;

/* ------------------------------------------------------------------ */
/* Analysis result                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t *mus_constraint_ids;   /* array of constraint IDs in MUS */
    uint32_t  mus_size;             /* number of constraints in MUS   */
    char     *proof_text;           /* human-readable proof (malloc'd)*/
    char     *proof_json;           /* JSON proof (malloc'd, or NULL) */
    uint32_t  core_size;            /* size of initial UNSAT core     */
    uint32_t  n_solver_calls;       /* solver invocations used        */
    double    elapsed_sec;          /* wall-clock time for analysis   */
    ContraRelaxSuggestion *relaxations; /* per-MUS-constraint         */
    uint32_t  n_relaxations;        /* == mus_size when computed      */
    uint8_t   unconfirmed;          /* 1 if UNSAT not fully confirmed */
    uint8_t   _res_pad[7];
} ContraResult;

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t  max_solver_calls;     /* budget (0=unlimited)           */
    double    time_limit_sec;       /* wall-clock limit (0=unlimited) */
    uint8_t   skip_minimization;    /* 1 = return core without MUS    */
    uint8_t   emit_json;            /* 1 = produce JSON output        */
    uint8_t   emit_proof;           /* 1 = produce proof (default on) */
    uint8_t   compute_relaxations;  /* 1 = compute relax suggestions  */
    uint8_t   find_alternatives;    /* 1 = find alt soft relaxations  */
    uint8_t   _pad[3];
    ContraConstraintInfo *constraint_info;  /* optional name mapping  */
    uint32_t              n_constraint_info;
} ContraOpts;

/* ------------------------------------------------------------------ */
/* Soft constraint diagnostic entry                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t               soft_constraint_id;
    uint32_t               soft_priority;
    uint32_t              *conflict_hard_ids;   /* hard constraints in MUS  */
    uint32_t               n_conflict_hard;
    char                  *proof_text;          /* proof text (malloc'd)    */
    ContraRelaxSuggestion *hard_relax;          /* per-hard relaxation      */
    uint32_t               n_hard_relax;
    uint32_t              *alternative_soft_ids;/* substitute softs         */
    uint32_t               n_alternatives;
} ContraSoftDiagEntry;

typedef struct {
    ContraSoftDiagEntry *entries;
    uint32_t             n_entries;     /* one per relaxed soft      */
    double               elapsed_sec;
} ContraSoftDiagResult;

/* ------------------------------------------------------------------ */
/* API functions                                                       */
/* ------------------------------------------------------------------ */

/**
 * Analyze why a problem is unsatisfiable.
 *
 * Call after dvs_solver_solve() returns DVS_SOLVE_UNSAT or DVS_SOLVE_TIMEOUT.
 * The dvs_problem_t sp must still be available (not reset/freed).
 *
 * @param ctx    Solver context (post-solve state).
 * @param sp     The original dvs_problem_t.
 * @param opts   Analysis options (NULL for defaults).
 * @param result Output (caller frees via contra_result_free).
 * @return 0 on success, -1 on error.
 */
int contra_analyze_unsat(dvs_ctx_t *ctx, dvs_problem_t *sp,
                          const ContraOpts *opts, ContraResult *result);

/** Free result memory allocated by contra_analyze_unsat(). */
void contra_result_free(ContraResult *result);

/**
 * Quick UNSAT core (Phase 1 only, no minimization).
 *
 * Faster than full analysis but core may be larger than minimal.
 *
 * @param ctx     Solver context.
 * @param sp      The original dvs_problem_t.
 * @param out_ids Caller-allocated array to receive constraint IDs.
 * @param out_n   On input: capacity of out_ids. On output: number filled.
 * @return 0 on success, -1 on error.
 */
int contra_quick_core(dvs_ctx_t *ctx, dvs_problem_t *sp,
                       uint32_t *out_ids, uint32_t *out_n);

/**
 * Explain why soft constraints were relaxed.
 *
 * Call after dvs_solver_solve() returns DVS_SOLVE_OK with relaxed softs.
 *
 * @param ctx    Solver context (post-solve state).
 * @param sp     The original dvs_problem_t.
 * @param opts   Analysis options (NULL for defaults).
 * @param result Output (caller frees via contra_soft_diag_free).
 * @return 0 on success, -1 on error.
 */
int contra_explain_soft(dvs_ctx_t *ctx, dvs_problem_t *sp,
                         const ContraOpts *opts,
                         ContraSoftDiagResult *result);

/** Free soft diagnostic result memory. */
void contra_soft_diag_free(ContraSoftDiagResult *result);

/**
 * Compute relaxation suggestions for MUS constraints.
 *
 * @param ctx      Solver context.
 * @param sp       The original dvs_problem_t.
 * @param mus_ids  Constraint IDs in the MUS.
 * @param mus_size Number of MUS constraints.
 * @param opts     Analysis options (NULL for defaults).
 * @param out      Output array (caller-allocated, size >= mus_size).
 * @return 0 on success, -1 on error.
 */
int contra_compute_relaxations(dvs_ctx_t *ctx, dvs_problem_t *sp,
                                const uint32_t *mus_ids, uint32_t mus_size,
                                const ContraOpts *opts,
                                ContraRelaxSuggestion *out);

#ifdef __cplusplus
}
#endif

#endif /* DVS_CONTRADICTION_ANALYSIS */

#endif /* DVS_CONTRADICTION_H */
