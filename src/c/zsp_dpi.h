#ifndef ZSP_DPI_H
#define ZSP_DPI_H

/*
 * DPI-C interface to zuspec-solver for SystemVerilog testbenches.
 *
 * Chandle-based API (cross-simulator, works with Verilator):
 *   zsp_dpi_compile_b64  -- compile from base64-encoded problem buffer
 *   zsp_dpi_solve_h      -- solve using compiled handle
 *   zsp_dpi_get_value_h  -- retrieve one variable's value after solve
 *   zsp_dpi_release_h    -- release compiled handle
 *
 * All DPI arguments are scalars, strings, or chandles -- no open arrays.
 * Return codes: 0=OK, 1=UNSAT, 2=TIMEOUT, -1=ERROR
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
 * @param ctx   Handle from zsp_dpi_compile_b64.
 * @param seed  RNG seed.
 * @return  0=OK, 1=UNSAT, 2=TIMEOUT, -1=ERROR
 */
int zsp_dpi_solve_h(void *ctx, long long seed);

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
