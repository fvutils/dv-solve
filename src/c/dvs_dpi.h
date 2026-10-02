#ifndef DVS_DPI_H
#define DVS_DPI_H

/*
 * DPI-C interface to dv-solve for SystemVerilog testbenches; the SV side
 * is dvs_dpi_pkg (src/sv/dvs_dpi_pkg.sv).
 *
 * Chandle-based API (cross-simulator, works with Verilator).
 * All arguments are scalars, strings, or chandles -- no open arrays.
 *
 * Core functions:
 *   dvs_dpi_compile_b64    -- compile from base64-encoded problem buffer
 *   dvs_dpi_n_uncompiled_h -- constraints compile could not take
 *   dvs_dpi_solve_h        -- solve using compiled handle
 *   dvs_dpi_get_value_h    -- retrieve one variable's value after solve
 *   dvs_dpi_release_h      -- release compiled handle and free memory
 *
 * Incremental / chain-solve functions:
 *   dvs_dpi_pin_var_h    -- pin a variable to a value before solving
 *   dvs_dpi_checkpoint_h -- save solver state (returns checkpoint index)
 *   dvs_dpi_restore_h    -- restore to a saved checkpoint
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Chandle-based API (cross-simulator)                                 */
/* ------------------------------------------------------------------ */

/**
 * Compile a problem from a base64-encoded byte buffer.
 *
 * @param b64_data  Null-terminated base64 string (standard alphabet, padded)
 *                  of a problem buffer from dvs_builder_finalize() -- in
 *                  Python, SolveProblemBuilder.finalize_bytes() -- made by
 *                  the same dv-solve version.
 * @return  Opaque handle (chandle) on success; NULL if the string is not
 *          valid base64, memory runs out, or compile fails or proves the
 *          problem has no solution.
 */
void *dvs_dpi_compile_b64(const char *b64_data);

/**
 * Solve using a compiled handle.
 *
 * Each call searches from the constraints plus any active pins, never from
 * the previous solution, so successive calls give different solutions.
 * Ties are broken at random (fair_pick), for evenly spread stimulus.
 *
 * When compile could not take every constraint (see
 * dvs_dpi_n_uncompiled_h), a successful search is re-checked against the
 * ORIGINAL problem before being reported as OK, and a model that violates a
 * dropped constraint is reported as 3 rather than success. Without that check
 * the caller receives under-constrained stimulus indistinguishable from a
 * correct solve.
 *
 * @param ctx   Handle from dvs_dpi_compile_b64.
 * @param seed  Selects the solution; the same seed from the same state gives
 *              the same solution. 0 continues from the handle's random state.
 * @return  0=OK, 1=UNSAT, 2=TIMEOUT, 3=MODEL INVALID (a constraint was
 *          dropped at compile and the assignment violates it), -1=ERROR
 *          (NULL handle, or every checkpoint slot is in use)
 */
int dvs_dpi_solve_h(void *ctx, long long seed);

/**
 * Number of constraints dvs_solver_compile could not compile natively.
 *
 * 0 means the compiled context covers the whole problem. A positive value
 * means the search is running against a SUBSET of the constraints, and
 * dvs_dpi_solve_h validates each model against the full problem to compensate.
 *
 * @param ctx  Handle from dvs_dpi_compile_b64.
 * @return  Count, or -1 if ctx is NULL.
 */
int dvs_dpi_n_uncompiled_h(void *ctx);

/**
 * Pin a variable to a specific value before solving.
 *
 * Tightens the variable's domain to [value, value] and runs propagation.
 * The pin applies to every following solve until the handle is restored to
 * a checkpoint taken before the pin.
 *
 * @param ctx     Handle from dvs_dpi_compile_b64.
 * @param var_id  0-based variable index.
 * @param value   Value to pin to.
 * @return  0=OK, -1=ERROR (invalid ctx or var_id out of range),
 *          -2=UNSAT (pinning makes the problem infeasible).
 */
int dvs_dpi_pin_var_h(void *ctx, int var_id, long long value);

/**
 * Save a checkpoint of the current solver state.
 *
 * Captures the pins applied so far. Pins applied later can be undone by
 * calling dvs_dpi_restore_h with the returned index.
 *
 * @param ctx  Handle from dvs_dpi_compile_b64.
 * @return  Checkpoint index (>= 0), or -1 on error (NULL handle, or too many
 *          checkpoints: at most 31 can be open).
 */
int dvs_dpi_checkpoint_h(void *ctx);

/**
 * Restore solver state to a previously saved checkpoint.
 *
 * Undoes every pin applied after the checkpoint was taken. Checkpoints taken
 * after `cp` are discarded; `cp` itself is kept and can be restored again.
 *
 * @param ctx  Handle from dvs_dpi_compile_b64.
 * @param cp   Checkpoint index returned by dvs_dpi_checkpoint_h.
 */
void dvs_dpi_restore_h(void *ctx, int cp);

/**
 * Retrieve one variable's value after a successful solve.
 *
 * @param ctx     Handle from dvs_dpi_compile_b64.
 * @param var_id  0-based variable index.
 * @return  The solved value; 0 if ctx is NULL, var_id is out of range, or
 *          the last dvs_dpi_solve_h did not return 0.
 */
long long dvs_dpi_get_value_h(void *ctx, int var_id);

/**
 * Release a compiled handle and free all associated memory.
 */
void dvs_dpi_release_h(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DVS_DPI_H */
