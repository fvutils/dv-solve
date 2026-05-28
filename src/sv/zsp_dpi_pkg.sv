// DPI import declarations for zuspec-solver.
//
// All arguments are scalars, strings, or chandles -- no open arrays.
// Works on Verilator, VCS, Questa, Xcelium.

package zsp_dpi_pkg;

  // Compile a problem from a base64-encoded byte buffer.
  // Returns an opaque handle (chandle), or null on error.
  import "DPI-C" function chandle zsp_dpi_compile_b64(
    input string b64_data
  );

  // Solve using a compiled handle.
  // Returns 0=OK, 1=UNSAT, 2=TIMEOUT, -1=ERROR.
  import "DPI-C" function int zsp_dpi_solve_h(
    input chandle ctx,
    input longint seed
  );

  // Retrieve one variable's value after a successful solve.
  import "DPI-C" function longint zsp_dpi_get_value_h(
    input chandle ctx,
    input int     var_id
  );

  // Release a compiled handle and free all associated memory.
  import "DPI-C" function void zsp_dpi_release_h(
    input chandle ctx
  );

  // ----------------------------------------------------------------
  // Incremental / chain-solve functions
  // ----------------------------------------------------------------

  // Pin a variable to a specific value before solving.
  // Returns 0=OK, -1=ERROR (invalid ctx or var_id), -2=UNSAT (conflict).
  import "DPI-C" function int zsp_dpi_pin_var_h(
    input chandle  ctx,
    input int      var_id,
    input longint  value
  );

  // Save a checkpoint of the current solver state.
  // Returns checkpoint index (>= 0), or -1 on error.
  import "DPI-C" function int zsp_dpi_checkpoint_h(
    input chandle ctx
  );

  // Restore solver state to a previously saved checkpoint.
  // Undoes all pins/solves that occurred after the checkpoint.
  import "DPI-C" function void zsp_dpi_restore_h(
    input chandle ctx,
    input int     cp
  );

endpackage
