# Formal Verification Benchmarks

Infrastructure for running SMT-LIB2 files through external solvers and
collecting performance baselines.  Part of Phase 1 of the formal
benchmark plan (`docs/formal_benchmark_plan.md`).

## Quick Start

```bash
# Generate Tier 1 SMT2 files from @zdc.dataclass benchmarks
direnv exec . python tests/formal/generate_tier1_smt2.py

# Run baselines (boolector, bitwuzla, z3 if available)
direnv exec . pytest tests/formal/test_tier1_baseline.py -v

# Run sby smoke test (BMC + cover with yosys + boolector)
direnv exec . pytest tests/formal/test_sby_smoke.py -v

# Run everything
direnv exec . pytest tests/formal/ -v
```

## Directory Layout

```
tests/formal/
  __init__.py
  conftest.py                  # pytest fixtures, solver discovery
  generate_tier1_smt2.py       # Script to emit .smt2 from bench tests
  test_tier1_baseline.py       # Tier 1 parametrized baseline tests
  test_sby_smoke.py            # sby + yosys toolchain smoke tests
  harness/
    __init__.py
    protocol.py                # FormalSolver protocol + FormalResult
    _subprocess_solver.py      # Shared subprocess runner
    boolector_solver.py        # BoolectorSolver wrapper
    bitwuzla_solver.py         # BitwuzlaSolver wrapper
    z3_solver.py               # Z3Solver wrapper (optional)
    cvc5_solver.py             # CVC5Solver wrapper (optional)
    results_collector.py       # CSV / JSON / Markdown output
  smt2/tier1/                  # Generated .smt2 files (34 benchmarks)
  sby/counter_assert.sby       # sby configuration for smoke test
  sv/counter_assert.sv         # SystemVerilog counter with assertions
  results/
    tier1_baseline.csv         # Baseline timing + memory data
    tier1_baseline.md          # Human-readable Markdown table
    *.json                     # Per-(benchmark, solver) detail files
```

## Adding a New Solver

1. Create `tests/formal/harness/<name>_solver.py`.
2. Implement a class with the `FormalSolver` protocol:
   - `name` property returning the solver's short ID
   - `is_available()` returning whether the binary is found
   - `solve(smt2_path, *, timeout_s=30.0)` returning a `FormalResult`
3. Use `_subprocess_solver.run_smt2_solver()` for the heavy lifting.
4. Import and add the solver to `ALL_SOLVERS` in `conftest.py`.

## Interpreting Results

- **solve_time_ms**: Wall-clock time for the solver subprocess.
- **peak_memory_kb**: Peak RSS delta from `resource.getrusage(RUSAGE_CHILDREN)`.
- **result**: `sat` (satisfiable), `unsat`, `unknown`, `error`, or `timeout`.
- All Tier 1 benchmarks are randomization queries and should return `sat`.
  Benchmarks listed in `_KNOWN_EMITTER_ISSUES` in the test file have
  known `RandSMT2Emitter` width-inference bugs and are marked `xfail`.

## Known Limitations

- 4 of 38 benchmarks could not be emitted to SMT2:
  - `AddrMap64`, `Ddr5TimingFull`, `WideAdd64`: unsupported `'name'` IR node
  - `ConditionalPacket` (`test_ite_cond`): IR indexing issue
  - `ResetReuse` (`test_reset_reuse`): no `@zdc.dataclass`, uses ctypes
- `arrayordering` emits but is UNSAT due to bitvector width overflow in
  the multiplication constraint (6-bit multiply wraps for `5 * q` when `q >= 13`).
