/*
 * dv_solve.h -- the public C API of dv-solve.
 *
 * This header declares everything a C program needs to build a constraint
 * problem, solve it and read the solution. It is installed as
 * <dv_solve/dv_solve.h>. The other headers installed beside it are internal:
 * they may change or disappear in any release.
 *
 * A problem is built with a dvs_builder_t, finalized into a dvs_problem_t,
 * and compiled into a dvs_ctx_t, which solves it:
 *
 *     dvs_builder_t *b = dvs_builder_create(0, NULL);
 *     dvs_builder_add_var(b, 0, 8, 0, 0, 255);
 *     dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT,
 *         dvs_builder_expr_var(b, 0), dvs_builder_expr_const(b, 10, 1)));
 *     size_t sz;
 *     dvs_problem_t *p = dvs_builder_finalize(b, &sz);
 *
 *     dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1 << 20);
 *     static uint8_t buf[1 << 20];
 *     dvs_ctx_t *ctx = dvs_solver_create(buf, sizeof(buf), ba);
 *     if (dvs_solver_compile(ctx, p) == 0 &&
 *         dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK)
 *         printf("%lld\n", (long long)dvs_solver_get_value(ctx, 0));
 *
 *     dvs_solver_destroy(ctx);
 *     dvs_block_alloc_destroy(ba);
 *     dvs_builder_free_problem(b, p, sz);
 *     dvs_builder_destroy(b);
 */
#ifndef DV_SOLVE_H
#define DV_SOLVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/** Builds a problem; see dvs_builder_create(). */
typedef struct dvs_builder_s dvs_builder_t;

/** A finalized problem, ready to compile; see dvs_builder_finalize(). */
typedef struct dvs_problem_s dvs_problem_t;

/** A solver context holding one compiled problem; see dvs_solver_create(). */
typedef struct dvs_ctx_s dvs_ctx_t;

/** A memory allocator. Every function that takes one accepts NULL, meaning
 *  malloc and free. */
typedef struct dvs_alloc_s dvs_alloc_t;

/** Supplies the solver context's working memory; see dvs_block_alloc_create(). */
typedef struct dvs_block_alloc_s dvs_block_alloc_t;

/** A reference to an expression or a constraint inside a builder. */
typedef uint32_t dvs_expr_t;

/** The dvs_expr_t a builder function returns when it cannot allocate memory. */
#define DVS_EXPR_NULL ((dvs_expr_t)0xFFFFFFFFu)

/** Binary operators, for dvs_builder_expr_binary(). */
typedef enum {
    DVS_BIN_ADD = 0,    /* +   */
    DVS_BIN_SUB,        /* -   */
    DVS_BIN_MUL,        /* *   */
    DVS_BIN_DIV,        /* /   */
    DVS_BIN_MOD,        /* %   */
    DVS_BIN_BAND,       /* &   */
    DVS_BIN_BOR,        /* |   */
    DVS_BIN_BXOR,       /* ^   */
    DVS_BIN_LSHIFT,     /* <<  */
    DVS_BIN_RSHIFT,     /* >>  (logical)                                  */
    DVS_BIN_EQ,         /* ==  */
    DVS_BIN_NEQ,        /* !=  */
    DVS_BIN_LT,         /* <   */
    DVS_BIN_LTE,        /* <=  */
    DVS_BIN_GT,         /* >   */
    DVS_BIN_GTE,        /* >=  */
    DVS_BIN_AND,        /* &&  */
    DVS_BIN_OR,         /* ||  */
    DVS_BIN_ASHR,       /* >>> (arithmetic in a signed expression, else >>) */
} dvs_binop_t;

/** Unary operators, for dvs_builder_expr_unary(). */
typedef enum {
    DVS_UN_NEG    = 0,  /* -  */
    DVS_UN_NOT    = 1,  /* !  */
    DVS_UN_INVERT = 2,  /* ~  */
} dvs_unop_t;

/** One entry of a weighted distribution; see dvs_builder_add_dist(). */
typedef struct {
    int64_t  lo;           /* lowest value of the range                     */
    int64_t  hi;           /* highest value of the range (== lo for a value) */
    uint32_t weight;
    uint8_t  is_per_value; /* 1: each value gets `weight` (SV :=);
                            * 0: the range shares `weight` (SV :/)          */
    uint8_t  _dpad[3];
} dvs_dist_entry_t;

/** The result of dvs_solver_solve(). */
typedef enum {
    DVS_SOLVE_OK      = 0,  /* a solution was found                        */
    DVS_SOLVE_UNSAT   = 1,  /* no solution exists                          */
    DVS_SOLVE_TIMEOUT = 2,  /* the search gave up; a solution may or may not
                             * exist                                        */
} dvs_result_t;

/** Options for dvs_solver_solve(). Zero-initialize, then set what you need;
 *  a zero field takes its default. */
typedef struct {
    uint64_t seed;            /* selects which solution is returned; the same
                               * seed gives the same solution. 0 continues
                               * from the context's current random state    */
    uint32_t max_conflicts;   /* conflicts per restart; 0 = no limit        */
    uint32_t max_restarts;    /* restarts in total; 0 = no limit            */
    uint8_t  use_phase_save;  /* search tuning; leave 0                     */
    uint8_t  use_lcg;         /* search tuning; leave 0                     */
    uint8_t  fair_pick;       /* 1: break decision ties at random, giving
                               * evenly spread solutions (use this for
                               * constrained-random stimulus); 0: fastest   */
    uint8_t  _pad[1];
    uint32_t max_shave_iters; /* search tuning; leave 0                     */
    uint32_t time_limit_ms;   /* wall-clock budget for this solve; 0 = the
                               * default                                    */
} dvs_solve_opts_t;

/* dvs_solver_compile() return codes. A positive value is the number of
 * constraints that could not be compiled. */
#define DVS_COMPILE_OK                  0
#define DVS_COMPILE_NOMEM             (-1)  /* the context buffer is too small */
#define DVS_COMPILE_UNSAT             (-2)  /* no solution exists              */
#define DVS_COMPILE_UNSUPPORTED_WIDTH (-3)  /* a variable is wider than 64 bits,
                                             * an expression wider than 255
                                             * bits or nested too deeply, or
                                             * an all-different over more
                                             * than 16 variables or one
                                             * wider than 32 bits            */
#define DVS_COMPILE_BAD_VAR           (-4)  /* variable ids are not 0..n-1, each
                                             * declared once, or an expression
                                             * names an undeclared variable  */

/* ------------------------------------------------------------------ */
/* Building a problem                                                  */
/* ------------------------------------------------------------------ */

/**
 * Create a builder.
 *
 * @param block_size  Size of each memory block the builder grows by; 0 for
 *                    the default (4096 bytes).
 * @param alloc       Allocator, or NULL for malloc.
 * @return  The builder, or NULL if memory could not be allocated.
 */
dvs_builder_t *dvs_builder_create(uint32_t block_size, dvs_alloc_t *alloc);

/** Free the builder. Problems it finalized are not freed. */
void dvs_builder_destroy(dvs_builder_t *b);

/** Empty the builder so it can build another problem, keeping its memory. */
void dvs_builder_reset(dvs_builder_t *b);

/**
 * Declare variable `var_id`.
 *
 * @param var_id     The variable's id, used in dvs_builder_expr_var() and
 *                   dvs_solver_get_value(). A problem with n variables uses
 *                   the ids 0 to n-1, each declared once.
 * @param width      Width in bits, 1 to 64.
 * @param is_signed  1 if the variable is signed.
 * @param lo, hi     The variable's value must lie in [lo, hi].
 */
dvs_expr_t dvs_builder_add_var(dvs_builder_t *b, uint32_t var_id,
                               uint8_t width, uint8_t is_signed,
                               int64_t lo, int64_t hi);

/** Require the Boolean expression `root` to hold. */
dvs_expr_t dvs_builder_add_constraint(dvs_builder_t *b, dvs_expr_t root);

/** Require the variables to take pairwise different values (SV `unique`). */
dvs_expr_t dvs_builder_add_all_different(dvs_builder_t *b, uint32_t n_vars,
                                         const uint32_t *var_ids);

/**
 * Ask for `root` to hold where possible. Priority 0 is the most important:
 * when soft constraints conflict, those with the highest priority number are
 * dropped first.
 */
dvs_expr_t dvs_builder_add_soft_constraint(dvs_builder_t *b, dvs_expr_t root,
                                           uint32_t priority);

/** Give variable `var_id` a weighted distribution (SV `dist`). */
dvs_expr_t dvs_builder_add_dist(dvs_builder_t *b, uint32_t var_id,
                                uint32_t n_entries,
                                const dvs_dist_entry_t *entries);

/** A constant. Like an unsized SystemVerilog literal it is 32 bits wide,
 *  unless the value doesn't fit in 32 bits. */
dvs_expr_t dvs_builder_expr_const(dvs_builder_t *b, int64_t value,
                                  uint8_t is_signed);

/** A reference to variable `var_id`. */
dvs_expr_t dvs_builder_expr_var(dvs_builder_t *b, uint32_t var_id);

/** `lhs op rhs`. */
dvs_expr_t dvs_builder_expr_binary(dvs_builder_t *b, dvs_binop_t op,
                                   dvs_expr_t lhs, dvs_expr_t rhs);

/** `op operand`. */
dvs_expr_t dvs_builder_expr_unary(dvs_builder_t *b, dvs_unop_t op,
                                  dvs_expr_t operand);

/** `cond ? then_e : else_e`. With Boolean arms, an if/else constraint. */
dvs_expr_t dvs_builder_expr_ite(dvs_builder_t *b, dvs_expr_t cond,
                                dvs_expr_t then_e, dvs_expr_t else_e);

/** `value inside {[lo:hi]}`. */
dvs_expr_t dvs_builder_expr_in_range(dvs_builder_t *b, dvs_expr_t value,
                                     dvs_expr_t lo, dvs_expr_t hi);

/** `value inside {elems[0], elems[1], ...}`. */
dvs_expr_t dvs_builder_expr_in_set(dvs_builder_t *b, dvs_expr_t value,
                                   uint32_t n_elems, const dvs_expr_t *elems);

/** `value inside {[los[0]:his[0]], [los[1]:his[1]], ...}`. */
dvs_expr_t dvs_builder_expr_in_ranges(dvs_builder_t *b, dvs_expr_t value,
                                      uint32_t n_ranges, const dvs_expr_t *los,
                                      const dvs_expr_t *his);

/** Zero-extend (sign_extend 0) or sign-extend `operand` from `from_bits`
 *  to `to_bits`. */
dvs_expr_t dvs_builder_expr_extend(dvs_builder_t *b, dvs_expr_t operand,
                                   uint8_t from_bits, uint8_t to_bits,
                                   uint8_t sign_extend);

/** A cast of `operand` to a `to_bits`-bit integer, signed when `to_signed`:
 *  SystemVerilog's `T'(operand)`. When `to_bits` is wider than the operand,
 *  the operand is evaluated at `to_bits` (an assignment-like context), so
 *  `cast(a * b, 64, 0)` of two 32-bit variables does not wrap at 32 bits.
 *  The value is truncated to `to_bits`, or extended by the operand's own
 *  signedness, and then read as signed or unsigned. */
dvs_expr_t dvs_builder_expr_cast(dvs_builder_t *b, dvs_expr_t operand,
                                 uint8_t to_bits, uint8_t to_signed);

/** `operand[hi_bit:lo_bit]`. */
dvs_expr_t dvs_builder_expr_extract(dvs_builder_t *b, dvs_expr_t operand,
                                    uint8_t hi_bit, uint8_t lo_bit);

/** `{hi, lo}`, where `lo` is `lo_width` bits wide. */
dvs_expr_t dvs_builder_expr_concat(dvs_builder_t *b, dvs_expr_t hi,
                                   dvs_expr_t lo, uint8_t lo_width);

/** The constraint `result == var_refs[0] + var_refs[1] + ...`. Pass it to
 *  dvs_builder_add_constraint(). */
dvs_expr_t dvs_builder_expr_sum(dvs_builder_t *b, dvs_expr_t result,
                                uint32_t n_vars, const dvs_expr_t *var_refs);

/** The constraint `result == $countones(operand)`. */
dvs_expr_t dvs_builder_expr_countones(dvs_builder_t *b, dvs_expr_t result,
                                      dvs_expr_t operand);

/** The constraint `result == $clog2(operand)`. */
dvs_expr_t dvs_builder_expr_clog2(dvs_builder_t *b, dvs_expr_t result,
                                  dvs_expr_t operand);

/**
 * Produce the finished problem. The builder is unchanged and can go on to be
 * reset or destroyed.
 *
 * @param size  If not NULL, receives the problem's size in bytes, which
 *              dvs_builder_free_problem() needs.
 * @return  The problem, or NULL if memory could not be allocated. Free it
 *          with dvs_builder_free_problem(), and not before every context
 *          compiled from it is destroyed.
 */
dvs_problem_t *dvs_builder_finalize(dvs_builder_t *b, size_t *size);

/** Free a problem returned by dvs_builder_finalize(). */
void dvs_builder_free_problem(dvs_builder_t *b, dvs_problem_t *p, size_t size);

/* ------------------------------------------------------------------ */
/* Solving                                                             */
/* ------------------------------------------------------------------ */

/**
 * Create the allocator that supplies a solver context's working memory.
 *
 * @param alloc       Allocator, or NULL for malloc.
 * @param block_size  Size of each block; 1 MiB is a good default.
 */
dvs_block_alloc_t *dvs_block_alloc_create(dvs_alloc_t *alloc,
                                          size_t block_size);

/** Free a block allocator. Destroy every context using it first. */
void dvs_block_alloc_destroy(dvs_block_alloc_t *ba);

/**
 * Create a solver context inside a buffer you supply.
 *
 * @param static_buf   Memory for the context, aligned as malloc() aligns. It
 *                     must stay valid until dvs_solver_destroy().
 * @param static_size  Its size. 1 MiB holds large problems; if it is too
 *                     small, dvs_solver_compile() returns DVS_COMPILE_NOMEM.
 * @param block_alloc  Supplies the rest of the context's memory.
 * @return  The context (at static_buf), or NULL if static_size is too small.
 */
dvs_ctx_t *dvs_solver_create(void *static_buf, size_t static_size,
                             dvs_block_alloc_t *block_alloc);

/** Release the context's memory. static_buf itself is yours to free. */
void dvs_solver_destroy(dvs_ctx_t *ctx);

/**
 * Compile problem `p` into the context. `p` must stay valid until the
 * context is destroyed.
 *
 * @return  DVS_COMPILE_OK; DVS_COMPILE_UNSAT if no solution exists;
 *          DVS_COMPILE_NOMEM, DVS_COMPILE_UNSUPPORTED_WIDTH or
 *          DVS_COMPILE_BAD_VAR on failure; or a positive count of
 *          constraints that could not be compiled. A solve after a positive
 *          return would ignore those constraints, so treat it as an error.
 */
int dvs_solver_compile(dvs_ctx_t *ctx, dvs_problem_t *p);

/**
 * Search for a solution.
 *
 * @param opts  Options, or NULL for the defaults.
 */
dvs_result_t dvs_solver_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts);

/** The value of variable `var_id` in the solution. Valid after
 *  dvs_solver_solve() returns DVS_SOLVE_OK. */
int64_t dvs_solver_get_value(const dvs_ctx_t *ctx, uint32_t var_id);

/** The values of `n` variables: out[i] receives the value of var_ids[i]. */
void dvs_solver_get_values(const dvs_ctx_t *ctx, uint32_t n,
                           const uint32_t *var_ids, int64_t *out);

/** Clear the previous solution so dvs_solver_solve() can run again. Call it
 *  before every solve after the first; it also removes pins. */
void dvs_solver_reset(dvs_ctx_t *ctx);

/**
 * Fix variable `var_id` to `value` for the following solves, until
 * dvs_solver_reset() (SV rand_mode(0), or a state variable).
 *
 * @return  0, or -1 if the value contradicts the constraints.
 */
int dvs_solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

/**
 * Remove `value` from the values variable `var_id` may take. Unlike a pin,
 * this lasts across dvs_solver_reset(); excluding each solution in turn
 * gives SV randc behaviour.
 *
 * @return  0, or -1 if no value would remain.
 */
int dvs_solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

/**
 * Add the constraints of problem `p` to an already compiled context. `p`
 * must stay valid until the context is destroyed.
 *
 * @return  The same codes as dvs_solver_compile(). `p` may name variables
 *          the context already has without declaring them.
 */
int dvs_solver_add_constraint(dvs_ctx_t *ctx, dvs_problem_t *p);

/**
 * Save the context's state.
 *
 * @return  A checkpoint index for dvs_solver_restore(), or -1 if too many
 *          checkpoints are open.
 */
int dvs_solver_checkpoint(dvs_ctx_t *ctx);

/** Return to the state saved by dvs_solver_checkpoint(), dropping pins and
 *  constraints added since. Checkpoint `cp` and every later one are
 *  discarded; take a new checkpoint to restore to the same state again. */
void dvs_solver_restore(dvs_ctx_t *ctx, uint32_t cp);

/**
 * Check the current solution against every constraint of `p`, the problem
 * the context was compiled from.
 *
 * @param err  Where to describe each violation, or NULL.
 * Constraints it cannot evaluate (arrays, sums, $countones, $clog2, inside)
 * are skipped.
 *
 * @return  0 if every checked constraint holds, otherwise the number
 *          violated.
 */
int dvs_solver_validate_model(dvs_ctx_t *ctx, dvs_problem_t *p, FILE *err);

/* ------------------------------------------------------------------ */
/* Oracle check                                                        */
/* ------------------------------------------------------------------ */
/*
 * A debug/test mode that checks every answer of a context against a
 * reference SMT solver (z3 by default), run as a child process: a solution
 * (DVS_SOLVE_OK) by asking the reference solver whether the problem holds with
 * every variable fixed to dv-solve's value; an UNSAT (from a solve, or from
 * compile) by letting it solve the problem. The problem is translated to
 * SMT-LIB2 independently of dv-solve's own elaboration (see
 * dvs_problem_write_smt2). Each answer is recorded in a run directory, the
 * same format as dv-solve-smt2's DV_ORACLE check; summarise one or many with
 * `python -m dv_solve.oracle report <dir>`. The oracle never changes what
 * dv-solve answers. POSIX only; not thread-safe. See docs/api_oracle_plan.md.
 */

/** A reference solver process and the run directory it records into. */
typedef struct dvs_oracle_s dvs_oracle_t;

/** Options for dvs_oracle_create(). Zero-initialize, then set what you need. */
typedef struct {
    const char *solver;    /* "z3" (NULL: the default) or "bitwuzla"         */
    const char *bin;       /* the solver's executable; NULL: found on PATH   */
    const char *command;   /* instead of solver/bin: a whole command line of a
                            * solver reading SMT-LIB2 on stdin              */
    const char *out_dir;   /* the run directory; %t expands to the UTC time,
                            * %p to the pid. NULL: "dvs-oracle/%t-%p"       */
    const char *tag;       /* free text recorded in run.json                 */
    double      timeout_s; /* per query; 0: 10 seconds                       */
    int         keep_all;  /* 1: also keep transcript.smt2, every script sent */
} dvs_oracle_opts_t;

/** Per-query results, as dvs_oracle_last_result() returns them. */
#define DVS_ORACLE_RES_NONE       (-1) /* nothing checked yet              */
#define DVS_ORACLE_RES_OK            0 /* the reference solver agrees      */
#define DVS_ORACLE_RES_BAD_MODEL     1 /* dv-solve's values violate the
                                        * constraints                      */
#define DVS_ORACLE_RES_BAD_UNSAT     2 /* dv-solve said UNSAT; it is not   */
#define DVS_ORACLE_RES_UNCHECKED     4 /* the reference solver could not
                                        * decide in time                   */
#define DVS_ORACLE_RES_ERROR         5 /* the reference solver failed, or
                                        * the problem could not be
                                        * translated                       */
#define DVS_ORACLE_RES_SKIPPED       6 /* not a sat/unsat answer (TIMEOUT) */

/** Totals over every query so far. */
typedef struct {
    uint64_t queries;
    uint64_t ok, bad_model, bad_unsat, unchecked, error, skipped;
    double   dvs_ms;       /* time dv-solve spent on the checked calls       */
    double   oracle_ms;    /* time the reference solver spent                */
} dvs_oracle_stats_t;

/**
 * Start a reference solver and claim a run directory.
 *
 * @param opts  Options, or NULL for the defaults.
 * @param err   Where to report setup problems and failed checks; NULL for
 *              stderr.
 * @return  The oracle, or NULL if the solver could not be started or the run
 *          directory created (the reason is reported on `err`).
 */
dvs_oracle_t *dvs_oracle_create(const dvs_oracle_opts_t *opts, FILE *err);

/** Finalise run.json, stop the reference solver and free the oracle. Detach
 *  (or destroy) every context using it first. */
void dvs_oracle_destroy(dvs_oracle_t *o);

/**
 * Check every answer of `ctx` with `o` from now on; NULL detaches. Attach
 * before dvs_solver_compile(): the oracle takes the problem from it. One
 * oracle may serve many contexts.
 *
 * @param label  Names the context in every record (e.g. the fields being
 *               randomized), or NULL.
 * @return  0, or -1 if `ctx` is NULL or memory ran out.
 */
int dvs_solver_set_oracle(dvs_ctx_t *ctx, dvs_oracle_t *o, const char *label);

/** The result of the most recent query (DVS_ORACLE_RES_*). */
int dvs_oracle_last_result(const dvs_oracle_t *o);

/** Totals so far. */
void dvs_oracle_get_stats(const dvs_oracle_t *o, dvs_oracle_stats_t *st);

/** The run directory. Valid until dvs_oracle_destroy(). */
const char *dvs_oracle_run_dir(const dvs_oracle_t *o);

/**
 * Write problem `p` as a stand-alone SMT-LIB2 (QF_BV) script: the variables
 * (as v<id>) with their domains, the hard constraints, and (check-sat). This
 * is the translation the oracle uses.
 *
 * @return  0; -1 if part of the problem could not be translated (the script
 *          says what) or the write failed.
 */
int dvs_problem_write_smt2(const dvs_problem_t *p, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* DV_SOLVE_H */
