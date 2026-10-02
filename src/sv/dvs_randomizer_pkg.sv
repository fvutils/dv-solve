// dvs_randomizer_pkg -- base class for DPI-based randomizers using dv-solve.
//
// A subclass supplies the problem (get_problem_b64) and copies a solution
// into an object (apply_solution); randomize_obj() does the rest. The problem
// is compiled once, on the first randomize_obj() call, and reused.

package dvs_randomizer_pkg;

  import dvs_dpi_pkg::*;

  virtual class dvs_randomizer #(type T = int);

    // -- Pure virtual methods (supplied by the subclass) -------

    // Return the problem data as a base64-encoded string.
    pure virtual function string get_problem_b64();

    // Copy the solution into obj's fields with dvs_dpi_get_value_h(ctx, id).
    pure virtual function void apply_solution(T obj, chandle ctx);

    // -- Internal state -----------------------------------------------

    local chandle m_ctx;
    local bit     m_initialized;

    // -- Constructor --------------------------------------------------

    // NOTE: get_problem_b64() is pure virtual and cannot be safely called
    // here (Verilator resolves virtual calls during construction using the
    // base-class vtable, returning an empty string).  Initialization is
    // deferred to the first randomize_obj() call instead.
    function new();
      m_ctx         = null;
      m_initialized = 0;
    endfunction

    // -- Lazy initialization ------------------------------------------

    local function void _ensure_initialized();
      if (!m_initialized) begin
        string b64;
        b64 = get_problem_b64();
        m_ctx = dvs_dpi_compile_b64(b64);
        if (m_ctx == null)
          $fatal(1, "dvs_randomizer: compile failed");
        m_initialized = 1;
      end
    endfunction

    // -- Randomize ----------------------------------------------------

    // Solve and, on success, apply the solution to obj. Returns the
    // dvs_dpi_solve_h code: 0 on success; obj is unchanged otherwise.
    virtual function int randomize_obj(T obj, longint seed = 0);
      int rc;
      _ensure_initialized();
      rc = dvs_dpi_solve_h(m_ctx, seed);
      if (rc == 0)
        apply_solution(obj, m_ctx);
      return rc;
    endfunction

    // -- Cleanup ------------------------------------------------------

    // Release the compiled problem. A later randomize_obj() recompiles it.
    function void cleanup();
      if (m_ctx != null) begin
        dvs_dpi_release_h(m_ctx);
        m_ctx = null;
      end
      m_initialized = 0;
    endfunction

  endclass

endpackage
