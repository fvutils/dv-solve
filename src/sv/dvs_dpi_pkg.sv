// dvs_dpi_pkg -- DPI import declarations for dv-solve.
//
// The functions are implemented in the dv_solve_dpi shared library; load it
// into the simulator (for example with -sv_lib). All arguments are scalars,
// strings or chandles -- no open arrays -- so the package works on Verilator,
// VCS, Questa and Xcelium.

package dvs_dpi_pkg;

  // Compile a problem from a base64-encoded problem buffer, as produced by
  // dv_solve's SolveProblemBuilder.finalize_bytes() with the same dv-solve
  // version. Returns a handle, or null if the string is malformed or the
  // problem cannot be compiled (including when it provably has no solution).
  import "DPI-C" function chandle dvs_dpi_compile_b64(
    input string b64_data
  );

  // Number of constraints compile could not take; 0 normally. When it is
  // positive, dvs_dpi_solve_h checks each solution against the full problem
  // and returns 3 for one that violates a constraint. -1 for a null handle.
  import "DPI-C" function int dvs_dpi_n_uncompiled_h(
    input chandle ctx
  );

  // Search for a solution. Each call starts from the constraints and any
  // active pins, never from the previous solution. The same seed from the
  // same state gives the same solution; seed 0 continues from the handle's
  // random state.
  // Returns 0=OK, 1=UNSAT, 2=TIMEOUT, 3=MODEL INVALID (see
  // dvs_dpi_n_uncompiled_h), -1=ERROR.
  import "DPI-C" function int dvs_dpi_solve_h(
    input chandle ctx,
    input longint seed
  );

  // One variable's value after a solve that returned 0; otherwise 0.
  import "DPI-C" function longint dvs_dpi_get_value_h(
    input chandle ctx,
    input int     var_id
  );

  // Release a handle and all its memory.
  import "DPI-C" function void dvs_dpi_release_h(
    input chandle ctx
  );

  // ----------------------------------------------------------------
  // Pins and checkpoints
  // ----------------------------------------------------------------

  // Fix a variable to a value for every following solve, until a restore to
  // a checkpoint taken before the pin.
  // Returns 0=OK, -1=ERROR (null handle or var_id out of range),
  // -2=the value contradicts the constraints or the other pins.
  import "DPI-C" function int dvs_dpi_pin_var_h(
    input chandle  ctx,
    input int      var_id,
    input longint  value
  );

  // Save the current pins. Returns a checkpoint index (>= 0), or -1 on
  // error (null handle, or 31 checkpoints already open).
  import "DPI-C" function int dvs_dpi_checkpoint_h(
    input chandle ctx
  );

  // Undo every pin applied since checkpoint cp. Later checkpoints are
  // discarded; cp itself is kept and can be restored again.
  import "DPI-C" function void dvs_dpi_restore_h(
    input chandle ctx,
    input int     cp
  );

endpackage
