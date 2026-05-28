#ifndef ZSP_DPI_H
#define ZSP_DPI_H

/*
 * DPI-C interface to zuspec-solver for SystemVerilog testbenches.
 *
 * Chandle-based API (cross-simulator, works with Verilator).
 * All arguments are scalars, strings, or chandles -- no open arrays.
 * Return codes: 0=OK, 1=UNSAT, 2=TIMEOUT, -1=ERROR, -2=UNSAT(pin).
 *
 * Core functions:
 *   zsp_dpi_compile_b64  -- compile from base64-encoded problem buffer
 *   zsp_dpi_solve_h      -- solve using compiled handle
 *   zsp_dpi_get_value_h  -- retrieve one variable's value after solve
 *   zsp_dpi_release_h    -- release compiled handle and free memory
 *
 * Incremental / chain-solve functions:
 *   zsp_dpi_pin_var_h    -- pin a variable to a value before solving
 *   zsp_dpi_checkpoint_h -- save solver state (returns checkpoint index)
 *   zsp_dpi_restore_h    -- restore to a saved checkpoint
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
 * @param b64_data  Null-terminated base64 string of the SolveProblem buffer.
 * @return  Opaque handle (chandle) on success, NULL on error.
 */
void *zsp_dpi_compile_b64(const char *b64_data);

/**
 * Solve using a compiled handle.
 *
 * Runs the solver from the current context state (including any active
 * pins set by zsp_dpi_pin_var_h).  Call zsp_dpi_restore_h before
 * re-solving if you want to try a different seed from the same pinned
 * state.
 *
 * @param ctx   Handle from zsp_dpi_compile_b64.
 * @param seed  RNG seed.
 * @return  0=OK, 1=UNSAT, 2=TIMEOUT, -1=ERROR
 */
int zsp_dpi_solve_h(void *ctx, long long seed);

/**
 * Pin a variable to a specific value before solving.
 *
 * Tightens the variable's domain to [value, value] and runs propagation.
 * The pin persists until the context is restored to a checkpoint that
 * predates the pin.  Pins applied after a checkpoint are undone by
 * restoring to that checkpoint.
 *
 * @param ctx     Handle from zsp_dpi_compile_b64.
 * @param var_id  0-based variable index.
 * @param value   Value to pin to.
 * @return  0=OK, -1=ERROR (invalid ctx or var_id out of range),
 *          -2=UNSAT (pinning makes the problem infeasible).
 */
int zsp_dpi_pin_var_h(void *ctx, int var_id, long long value);

/**
 * Save a checkpoint of the current solver state.
 *
 * Captures domain bounds, trail position, and propagator state.
 * Subsequent pin/solve operations can be undone by calling
 * zsp_dpi_restore_h with the returned index.
 *
 * Typical use: call after pinning chain-entry variables, before solving,
 * so you can restore and re-solve with a different seed if desired.
 *
 * @param ctx  Handle from zsp_dpi_compile_b64.
 * @return  Checkpoint index (>= 0), or -1 on error.
 */
int zsp_dpi_checkpoint_h(void *ctx);

/**
 * Restore solver state to a previously saved checkpoint.
 *
 * Undoes all domain changes, pins, and search assignments that occurred
 * after the checkpoint was taken.  The checkpoint itself is NOT consumed
 * and can be restored to multiple times.
 *
 * @param ctx  Handle from zsp_dpi_compile_b64.
 * @param cp   Checkpoint index returned by zsp_dpi_checkpoint_h.
 */
void zsp_dpi_restore_h(void *ctx, int cp);

/**
 * Retrieve one variable's value after a successful solve.
 *
 * @param ctx     Handle from zsp_dpi_compile_b64.
 * @param var_id  0-based variable index.
 * @return  The solved value, or 0 if ctx is invalid.
 */
long long zsp_dpi_get_value_h(void *ctx, int var_id);

/**
 * Release a compiled handle and free all associated memory.
 */
void zsp_dpi_release_h(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_DPI_H */
