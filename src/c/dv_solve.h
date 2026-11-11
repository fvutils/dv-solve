/*
 * dv_solve.h -- the public C API of dv-solve.
 *
 * This header declares everything a C program needs to build a constraint
 * problem, solve it and read the solution. It is installed as
 * <dv_solve/dv_solve.h>. The problem-building half is in dvs_writer.h, which
 * this header includes and which can be used alone. The other headers
 * installed beside them are internal: they may change or disappear in any
 * release.
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
 *     dvs_ctx_t *ctx = dvs_solver_new(NULL);
 *     if (dvs_solver_compile(ctx, p) == 0 &&
 *         dvs_solver_solve(ctx, NULL) == DVS_SOLVE_OK)
 *         printf("%lld\n", (long long)dvs_solver_get_value(ctx, 0));
 *
 *     dvs_solver_free(ctx);
 *     dvs_builder_free_problem(b, p, sz);
 *     dvs_builder_destroy(b);
 *
 * The context takes its memory from an allocator and grows as the problem
 * needs. dvs_solver_create() instead places it in a buffer you supply, for
 * platforms that must bound or place memory themselves.
 */
#ifndef DV_SOLVE_H
#define DV_SOLVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dvs_api.h"
#include "dvs_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/** A solver context holding one compiled problem; see dvs_solver_create(). */
typedef struct dvs_ctx_s dvs_ctx_t;

/** Supplies the solver context's working memory; see dvs_block_alloc_create(). */
typedef struct dvs_block_alloc_s dvs_block_alloc_t;

/** DVS_MAX_WIDTH of the loaded library, for bindings that can't read the
 *  header. */
DVS_API uint32_t dvs_max_width(void);

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
    uint8_t  seq_pick;        /* with fair_pick: 1 draws variables one at a
                               * time (the search order), 0 draws them
                               * uniformly over solutions where it can.
                               * Set it for a solve-order (solve x before y) */
    uint32_t max_shave_iters; /* search tuning; leave 0                     */
    uint32_t time_limit_ms;   /* wall-clock budget for this solve; 0 = the
                               * default                                    */
    uint8_t  canonical;       /* 1: the values depend only on the
                               * constraints, the pinned values and the
                               * seed, never on what earlier solves learnt
                               * (a fixed variable order, per-variable
                               * candidate draws; clauses only reject)      */
} dvs_solve_opts_t;

/* dvs_solver_compile() return codes. A positive value is the number of
 * constraints that could not be compiled. */
#define DVS_COMPILE_OK                  0
#define DVS_COMPILE_NOMEM             (-1)  /* the context buffer is too small */
#define DVS_COMPILE_UNSAT             (-2)  /* no solution exists              */
#define DVS_COMPILE_UNSUPPORTED_WIDTH (-3)  /* a variable is wider than 64 bits,
                                             * an expression wider than
                                             * DVS_MAX_WIDTH bits or nested
                                             * too deeply, or
                                             * an all-different over a
                                             * variable wider than 32 bits   */
#define DVS_COMPILE_BAD_VAR           (-4)  /* variable ids are not 0..n-1, each
                                             * declared once, or an expression
                                             * names an undeclared variable  */
#define DVS_COMPILE_ARRAYS            (-5)  /* the problem has arrays: only a
                                             * handle solves it
                                             * (dvs_randomize_arrays)        */

/* ------------------------------------------------------------------ */
/* Solving                                                             */
/* ------------------------------------------------------------------ */

/**
 * Create the allocator that supplies a solver context's working memory.
 *
 * @param alloc       Allocator, or NULL for malloc.
 * @param block_size  Size of each block; 1 MiB is a good default.
 */
DVS_API dvs_block_alloc_t *dvs_block_alloc_create(dvs_alloc_t *alloc,
                                                  size_t block_size);

/** Free a block allocator. Destroy every context using it first. */
DVS_API void dvs_block_alloc_destroy(dvs_block_alloc_t *ba);

/**
 * Create a solver context. Its memory comes from `alloc` and grows with
 * the problem; there is no size to choose.
 *
 * @param alloc  Allocator, or NULL for malloc. It must outlive the context.
 * @return  The context, or NULL when memory runs out. Release it with
 *          dvs_solver_free().
 */
DVS_API dvs_ctx_t *dvs_solver_new(dvs_alloc_t *alloc);

/** Release a context made by dvs_solver_new(), and all its memory. */
DVS_API void dvs_solver_free(dvs_ctx_t *ctx);

/**
 * Create a solver context inside a buffer you supply: the embedded option,
 * for when memory must be bounded or placed. Prefer dvs_solver_new().
 *
 * @param static_buf   Memory for the context, aligned as malloc() aligns. It
 *                     must stay valid until dvs_solver_destroy().
 * @param static_size  Its size, at least 64 KiB.
 * @param block_alloc  With a block allocator, the context grows past
 *                     static_buf through the allocator behind it. NULL keeps
 *                     it inside static_buf (the context still mallocs a
 *                     little for its search trail): a problem that does not
 *                     fit makes dvs_solver_compile() return DVS_COMPILE_NOMEM.
 * @return  The context (at static_buf), or NULL if static_size is too small.
 */
DVS_API dvs_ctx_t *dvs_solver_create(void *static_buf, size_t static_size,
                                     dvs_block_alloc_t *block_alloc);

/** Release the memory of a context made by dvs_solver_create(). static_buf
 *  itself is yours to free. */
DVS_API void dvs_solver_destroy(dvs_ctx_t *ctx);

/** The engines a context solves on (dvs_solver_set_engine). */
enum {
    DVS_ENGINE_AUTO     = 0,   /* the context chooses (dvs_solver_compile) */
    DVS_ENGINE_CDCL     = 1,
    DVS_ENGINE_BITBLAST = 2
};

/**
 * Fix the engine before dvs_solver_compile(); for tests and benchmarks; a
 * caller need not choose. A context from dvs_solver_new() starts at AUTO,
 * one from dvs_solver_create() at CDCL (bit-blast's memory is not bounded).
 *
 * Under AUTO the context solves on CDCL, and moves to bit-blast when CDCL
 * cannot compile the problem whole (wide multiply/divide, for instance) or
 * when a CDCL search gives up (not one with its own time_limit_ms: bit-blast
 * has no budget to keep it to). A problem with STATE or RANDC variables, solve order, soft
 * constraints or a dist stays on CDCL: bit-blast has no pins, value
 * exclusion, order or weights. Pins, exclusion, checkpoints, incremental
 * adds, optimization and cost guidance are CDCL's: calling one moves the
 * context back to CDCL for good, or fails (-1) when CDCL could not compile
 * the problem whole.
 *
 * @return  0; -1 if the context has compiled already or `engine` is unknown.
 */
int dvs_solver_set_engine(dvs_ctx_t *ctx, int engine);

/** The engine the context solves on now: CDCL or BITBLAST once compiled,
 *  else its preference. */
int dvs_solver_engine(const dvs_ctx_t *ctx);

/**
 * Compile problem `p` into the context. The context reads `p` only during
 * the call: afterwards `p` may be freed, reused or moved.
 *
 * @return  DVS_COMPILE_OK; DVS_COMPILE_UNSAT if no solution exists;
 *          DVS_COMPILE_NOMEM, DVS_COMPILE_UNSUPPORTED_WIDTH or
 *          DVS_COMPILE_BAD_VAR on failure; or a positive count of
 *          constraints that could not be compiled. A solve after a positive
 *          return would ignore those constraints, so treat it as an error.
 *          Under AUTO a problem CDCL cannot compile whole goes to bit-blast
 *          and returns DVS_COMPILE_OK, unless it needs CDCL
 *          (dvs_solver_set_engine).
 */
DVS_API int dvs_solver_compile(dvs_ctx_t *ctx, dvs_problem_t *p);

/**
 * Search for a solution.
 *
 * @param opts  Options, or NULL for the defaults.
 */
DVS_API dvs_result_t dvs_solver_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts);

/** The value of variable `var_id` in the solution. Valid after
 *  dvs_solver_solve() returns DVS_SOLVE_OK. */
DVS_API int64_t dvs_solver_get_value(const dvs_ctx_t *ctx, uint32_t var_id);

/** The values of `n` variables: out[i] receives the value of var_ids[i]. */
DVS_API void dvs_solver_get_values(const dvs_ctx_t *ctx, uint32_t n,
                                   const uint32_t *var_ids, int64_t *out);

/** Clear the previous solution so dvs_solver_solve() can run again. Call it
 *  before every solve after the first; it also removes pins. */
DVS_API void dvs_solver_reset(dvs_ctx_t *ctx);

/**
 * Fix variable `var_id` to `value` for the following solves, until
 * dvs_solver_reset() (SV rand_mode(0), or a state variable). The pin is
 * hard: soft constraints give way to it.
 *
 * @return  0, or -1 if the value contradicts the constraints. With soft
 *          constraints a contradiction is left to the solve, which relaxes
 *          them and is DVS_SOLVE_UNSAT if that is not enough.
 */
DVS_API int dvs_solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

/**
 * Remove `value` from the values variable `var_id` may take. Unlike a pin,
 * this lasts across dvs_solver_reset(); excluding each solution in turn
 * gives SV randc behaviour.
 *
 * @return  0, or -1 if no value would remain.
 */
DVS_API int dvs_solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value);

/**
 * Add the constraints of problem `p` to an already compiled context. As with
 * dvs_solver_compile(), `p` is read only during the call.
 *
 * Variables `p` declares are new, numbered from dvs_solver_n_vars(): the
 * compile may have made variables of its own after the ones its problem
 * declared. `p` may also name, or re-declare at the same width, variables
 * the problem declared before; re-declaring any other id is
 * DVS_COMPILE_BAD_VAR.
 *
 * @return  The same codes as dvs_solver_compile(). `p` may name variables
 *          the context already has without declaring them.
 */
DVS_API int dvs_solver_add_constraint(dvs_ctx_t *ctx, dvs_problem_t *p);

/** The number of variables in the context: those its problems declared and
 *  those the compile made for itself. A problem passed to
 *  dvs_solver_add_constraint() numbers its new variables from here. */
DVS_API uint32_t dvs_solver_n_vars(const dvs_ctx_t *ctx);

/**
 * Save the context's state.
 *
 * @return  A checkpoint index for dvs_solver_restore(), or -1 if no more
 *          can be opened: a context from dvs_solver_create() holds at most
 *          32 open checkpoints; one from dvs_solver_new() grows its stack
 *          (to about 65,000), and fails only when out of memory.
 */
DVS_API int dvs_solver_checkpoint(dvs_ctx_t *ctx);

/** Return to the state saved by dvs_solver_checkpoint(), dropping pins and
 *  constraints added since. Checkpoint `cp` and every later one are
 *  discarded; take a new checkpoint to restore to the same state again. */
DVS_API void dvs_solver_restore(dvs_ctx_t *ctx, uint32_t cp);

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
DVS_API int dvs_solver_validate_model(dvs_ctx_t *ctx, dvs_problem_t *p, FILE *err);

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
DVS_API dvs_oracle_t *dvs_oracle_create(const dvs_oracle_opts_t *opts, FILE *err);

/** Finalise run.json, stop the reference solver and free the oracle. Detach
 *  (or destroy) every context using it first. */
DVS_API void dvs_oracle_destroy(dvs_oracle_t *o);

/**
 * Check every answer of `ctx` with `o` from now on; NULL detaches. Attach
 * before dvs_solver_compile(): the oracle takes the problem from it. One
 * oracle may serve many contexts.
 *
 * @param label  Names the context in every record (e.g. the fields being
 *               randomized), or NULL.
 * @return  0, or -1 if `ctx` is NULL or memory ran out.
 */
DVS_API int dvs_solver_set_oracle(dvs_ctx_t *ctx, dvs_oracle_t *o, const char *label);

/** The result of the most recent query (DVS_ORACLE_RES_*). */
DVS_API int dvs_oracle_last_result(const dvs_oracle_t *o);

/** Totals so far. */
DVS_API void dvs_oracle_get_stats(const dvs_oracle_t *o, dvs_oracle_stats_t *st);

/** The run directory. Valid until dvs_oracle_destroy(). */
DVS_API const char *dvs_oracle_run_dir(const dvs_oracle_t *o);

/**
 * Write problem `p` as a stand-alone SMT-LIB2 (QF_BV) script: the variables
 * (as v<id>) with their domains, the hard constraints, and (check-sat). This
 * is the translation the oracle uses.
 *
 * @return  0; -1 if part of the problem could not be translated (the script
 *          says what) or the write failed.
 */
DVS_API int dvs_problem_write_smt2(const dvs_problem_t *p, FILE *out);

/* ------------------------------------------------------------------ */
/* Problem handles: one randomize() call site (docs/verilator_problem_buffer.md) */
/* ------------------------------------------------------------------ */

/** A problem buffer compiled once and solved on every dvs_randomize(). */
typedef struct dvs_handle_s dvs_handle_t;

/** dvs_ref_t.layout values. Only DVS_REF_SCALAR is supported so far. */
enum {
    DVS_REF_SCALAR = 0   /* one little-endian word of 1, 2, 4 or 8 bytes (the
                          * smallest that holds `width`; Verilator C/S/I/Q) */
};

/** Where the value of one variable lives. refs[i] is variable i. */
typedef struct {
    void     *addr;       /* the variable's storage, or NULL: not read or written */
    uint32_t  width;      /* bits, 1 to 64 */
    uint8_t   is_signed;  /* a STATE value is sign-extended when set */
    uint8_t   layout;     /* DVS_REF_SCALAR */
    uint16_t  _pad;
} dvs_ref_t;

/**
 * Make a handle for the problem image `buf` of `size` bytes (as
 * dvs_builder_finalize() returns it). The image is copied: `buf` may be freed
 * or be read-only, and need not be aligned. It is compiled on the first
 * dvs_randomize().
 *
 * @return  The handle, or NULL if the image is not a well-formed problem of
 *          DVS_PROBLEM_FORMAT_VERSION (checked before anything in it is
 *          used: the header, then every offset is in range and aligned), or
 *          memory runs out.
 */
DVS_API dvs_handle_t *dvs_handle_new(const void *buf, size_t size);

/** Release a handle and its compiled context. NULL is ignored. */
DVS_API void dvs_handle_free(dvs_handle_t *h);

/** The number of entries dvs_randomize() reads from `refs`: one past the
 *  highest variable id the problem declares. A problem with arrays is solved
 *  by dvs_randomize_arrays() alone. */
DVS_API uint32_t dvs_handle_n_vars(const dvs_handle_t *h);

/**
 * Choose the engine before the first dvs_randomize(): the handle's context
 * gets it (dvs_solver_set_engine, which describes AUTO, the default). A
 * problem with STATE or RANDC variables, soft constraints, dist or
 * solve-order stays on CDCL: the bit-blast engine has no pins or value
 * exclusion yet, and its resample ignores the others.
 *
 * @return  0; -1 if `h` is NULL, has already solved, `engine` is unknown, or
 *          BITBLAST is asked of a problem that stays on CDCL.
 */
DVS_API int dvs_handle_set_engine(dvs_handle_t *h, int engine);

/** The engine the handle solves on: CDCL or BITBLAST once it has solved,
 *  else what dvs_handle_set_engine() set. */
DVS_API int dvs_handle_engine(const dvs_handle_t *h);

/** dvs_randomize() return codes. */
enum {
    DVS_RANDOMIZE_OK    = 0,   /* RAND and RANDC values were written      */
    DVS_RANDOMIZE_UNSAT = 1,   /* no solution with these STATE values     */
    DVS_RANDOMIZE_ERROR = -1,  /* unsupported ref or modes, a compile
                                * error, or the search gave up           */
    DVS_RANDOMIZE_TOO_LARGE = -2, /* arrays too large: a random size over
                                   * 65535 (dvs_randomize_sizes), or, to
                                   * solve together, over 65536 elements
                                   * (dvs_randomize_arrays); or over
                                   * 4096 objects or 65536 slot-array
                                   * elements (dvs_randomize_parts)     */
    DVS_RANDOMIZE_NULL = -3       /* a constraint reads a variable through
                                   * a null handle, and no guard of STATE
                                   * values rules the read out
                                   * (dvs_randomize_parts)              */
};

/**
 * Solve the handle's problem once (SV randomize()). Each STATE variable with
 * a non-NULL ref is pinned to the value read through it; then the values of
 * the RAND and RANDC variables with a non-NULL ref are written through it,
 * masked to the ref's width. On any return but DVS_RANDOMIZE_OK no ref is
 * written. A RANDC variable takes each value it can once per cycle; a cycle
 * ends when no value is left, and an UNSAT call ends every cycle (it is
 * solved once more before it returns DVS_RANDOMIZE_UNSAT). Calls on one
 * handle must not overlap.
 *
 * @param refs   dvs_handle_n_vars(h) entries.
 * @param modes  constraint_mode / rand_mode bits. The buffer format has none
 *               yet: pass NULL (non-NULL is DVS_RANDOMIZE_ERROR).
 * @param seed   The sampling seed; the same seed and STATE values give the
 *               same result.
 */
DVS_API int dvs_randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes,
                          uint64_t seed);

/** One array of a problem with arrays: `n_elems` elements, packed, each in
 *  the smallest of 1, 2, 4 or 8 bytes that holds the element width (as a
 *  DVS_REF_SCALAR ref), little-endian. */
typedef struct {
    void     *data;      /* NULL when n_elems is 0 */
    uint32_t  n_elems;
    uint32_t  _pad;
} dvs_array_ref_t;

/** The number of entries dvs_randomize_arrays() reads from `arrays`: the
 *  arrays the problem declares (dvs_builder_add_array). */
DVS_API uint32_t dvs_handle_n_arrays(const dvs_handle_t *h);

/**
 * Solve the random sizes of a problem with arrays
 * (dvs_builder_set_array_size_random), the first part of a randomize() call
 * whose constraints read sizes: with the constraints that read no element,
 * before any element is solved (IEEE 1800-2023 18.5.8.1). Nothing is written
 * through `refs`. The caller then gives each array its size and calls
 * dvs_randomize_arrays().
 *
 * @param sizes  dvs_handle_n_arrays(h) entries: the current size of each
 *               array on the call; on DVS_RANDOMIZE_OK, each random size is
 *               replaced by the size solved.
 * @return  as dvs_randomize(), or DVS_RANDOMIZE_TOO_LARGE if a size solved is
 *          over 65535 (sizes unchanged). With no random size, DVS_RANDOMIZE_OK.
 */
DVS_API int dvs_randomize_sizes(dvs_handle_t *h, const dvs_ref_t *refs, uint32_t *sizes,
                                uint64_t seed);

/**
 * Solve a problem with arrays once. The size of each array is its
 * `n_elems`, fixed for the call (IEEE 1800-2023 18.5.8.1: sizes are solved
 * before the elements' constraints). The elements of a STATE array are read;
 * those of a RAND array are written, on DVS_RANDOMIZE_OK only. `refs`,
 * `modes` and `seed` are as for dvs_randomize().
 *
 * An array whose elements are constrained only one at a time (in foreachs
 * over it that read nothing random but its element) is solved element by
 * element, for any size. The rest are instantiated for the sizes of the call
 * and compiled; the handle keeps the last few such instantiations.
 *
 * @param arrays  dvs_handle_n_arrays(h) entries.
 * @return  as dvs_randomize(), or DVS_RANDOMIZE_TOO_LARGE.
 */
DVS_API int dvs_randomize_arrays(dvs_handle_t *h, const dvs_ref_t *refs,
                                 const dvs_array_ref_t *arrays, const uint32_t *modes,
                                 uint64_t seed);

/* ------------------------------------------------------------------ */
/* Randomizing several objects together (IEEE 1800-2023 18.5.8)       */
/* ------------------------------------------------------------------ */

/** The number of slots of a part's problem (dvs_builder_set_n_slots). */
DVS_API uint32_t dvs_handle_n_slots(const dvs_handle_t *h);

/** dvs_slot_t.state values. */
enum {
    DVS_SLOT_NULL     = 0,  /* the handle is null                          */
    DVS_SLOT_INACTIVE = 1,  /* rand_mode(0): the object is not randomized;
                             * the exports imported through it are read   */
    DVS_SLOT_ACTIVE   = 2   /* the object is randomized with this one      */
};

typedef struct dvs_part_s dvs_part_t;

/** One slot of an object: its handle's state, and the object's part. The
 *  slot of a slot array (dvs_builder_add_slot_array) instead gives the
 *  handles the array holds: `n_elems` slots at `elems`. */
typedef struct dvs_slot_s {
    uint32_t    state;      /* DVS_SLOT_*; not read for a slot array */
    uint32_t    n_elems;    /* a slot array: its handles */
    dvs_part_t *child;      /* the object; ignored when state is DVS_SLOT_NULL */
    const struct dvs_slot_s *elems;  /* a slot array: n_elems slots */
} dvs_slot_t;

/** The slot of array `array_id` if it is a slot array, else DVS_NO_SLOT. */
DVS_API uint32_t dvs_handle_array_slot(const dvs_handle_t *h, uint32_t array_id);

/** One object of a dvs_randomize_parts() call. The part's address is the
 *  object's identity: an object reached through several slots is given by
 *  the same dvs_part_t, and is solved once. The array ref of a slot array
 *  gives its size, its `data` is not read. */
struct dvs_part_s {
    dvs_handle_t     *handle;  /* the problem of the object's class        */
    const dvs_ref_t  *refs;    /* dvs_handle_n_vars(handle) entries; those of
                                * imports are not read                      */
    dvs_array_ref_t  *arrays;  /* dvs_handle_n_arrays(handle) entries       */
    const dvs_slot_t *slots;   /* dvs_handle_n_slots(handle) entries        */
};

/**
 * Solve the object `root` and the objects reached from it through active
 * slots together (SV randomize() on `root`): the constraints of every such
 * object, its imports bound to the exports they name.
 *
 *  - An import through an inactive slot is a STATE value: the export's
 *    current value.
 *  - An import through a null slot is a STATE value of 0. A constraint that
 *    reads it must be guarded by STATE values that rule the read out
 *    (`if (h != null) ... h.x ...`, the guard a STATE bit combined with !,
 *    &&, || and conditions); else the call fails with DVS_RANDOMIZE_NULL.
 *  - A slot array of `n` elements (its array ref's `n_elems`, the size of
 *    the call) holds the handles of its slot's `elems`, then null handles
 *    if it has fewer (a dynamic array resized larger). A foreach over it is
 *    instantiated per element, each with the imports of that element.
 *  - Soft constraints (IEEE 1800-2023 18.5.13): those of an object rank
 *    above those of the objects it reaches, those reached through a later
 *    slot (or a later element of a slot array) above those reached through
 *    an earlier one, and an object reached twice takes the higher rank;
 *    within an object, its own priorities.
 *
 * The problem assembled for a shape (the classes, the state of each slot,
 * the size of each slot array) is kept on `root`'s handle, which keeps the
 * last few shapes. A root
 * with no slots is solved as dvs_randomize_arrays() solves it. Arrays are
 * solved as by dvs_randomize_arrays(), each at its `n_elems`.
 *
 * @return  as dvs_randomize_arrays(); DVS_RANDOMIZE_NULL; or
 *          DVS_RANDOMIZE_TOO_LARGE past 4096 objects or 65536 slot-array
 *          elements.
 */
DVS_API int dvs_randomize_parts(dvs_part_t *root, uint64_t seed);

/**
 * The random sizes of a dvs_randomize_parts() call, solved first, as
 * dvs_randomize_sizes() solves them: on DVS_RANDOMIZE_OK the `n_elems` of
 * each array of a random size is replaced by the size solved; no data is
 * read or written. The caller then gives each array its size and calls
 * dvs_randomize_parts(). A foreach over a slot array is not instantiated
 * here: it has no constraint on a size.
 */
DVS_API int dvs_randomize_parts_sizes(dvs_part_t *root, uint64_t seed);

/**
 * How this copy of the library was built, as one line of key=value pairs:
 * commit, dirty (tracked files modified at build time), build_type,
 * optimized (compiled with optimisation), asan, ndebug, cc. Benchmark records
 * carry it, so a result can be traced to the build that produced it.
 */
DVS_API const char *dvs_build_info(void);

/**
 * The file this library's code was loaded from: the shared library actually
 * resolved at run time (LD_LIBRARY_PATH takes precedence over a binary's
 * RUNPATH), or the executable when linked statically. "unknown" where the
 * platform cannot tell.
 */
DVS_API const char *dvs_build_library_path(void);

#ifdef __cplusplus
}
#endif

#endif /* DV_SOLVE_H */
