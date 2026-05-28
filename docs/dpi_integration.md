# DPI Integration Guide

The zuspec-solver DPI integration lets SystemVerilog testbenches call the
native constraint solver via DPI-C.  A Python code generator produces SV
classes with a base64-encoded `SolveProblem` buffer; at simulation time
the solver decodes, compiles, and solves in C with sub-microsecond solve
times.

The DPI interface uses only scalar, string, and chandle arguments -- no
open arrays.  This works on all simulators including Verilator.

## Prerequisites

- `libdv_solve_dpi.so` (built from `packages/zuspec-solver` via CMake)
- A SystemVerilog simulator with DPI support (Verilator, Questa, VCS, etc.)

## Quick Start

1. Define a dataclass with constraints:

```python
from dataclasses import dataclass
from zuspec.dataclasses import rand, constraint

@dataclass
class MemTransaction:
    addr: int = rand(domain=(0, 255))
    size: int = rand(domain=(1, 8))

    @constraint
    def addr_align(self):
        assert self.addr % 4 == 0
```

2. Generate the SV harness:

```python
from zuspec.solver.sv_randomizer_gen import SVRandomizerGenerator
sv_text = SVRandomizerGenerator().emit(MemTransaction)
with open("harness.sv", "w") as f:
    f.write(sv_text)
```

3. Compile and simulate:

```bash
# Questa
vlog -sv src/sv/zsp_dpi_pkg.sv src/sv/zsp_randomizer_pkg.sv harness.sv src/c/*.c
vopt -o simv_opt MemTransaction_harness
vsim -batch -do "run -a; quit -f" simv_opt +n_solutions=100

# Verilator (link pre-built .so)
verilator --cc --exe --main -o simv -Wno-fatal --timing \
    src/sv/zsp_dpi_pkg.sv src/sv/zsp_randomizer_pkg.sv harness.sv \
    --top-module MemTransaction_harness
make -C obj_dir -f VMemTransaction_harness.mk \
    VM_USER_LDLIBS="-Lbuild -Wl,-rpath,build -ldv_solve_dpi"
obj_dir/simv +n_solutions=100
```

## API Reference

### SV DPI Package (`zsp_dpi_pkg`)

```systemverilog
// Compile from base64-encoded problem buffer. Returns chandle.
function chandle zsp_dpi_compile_b64(input string b64_data);

// Solve using compiled handle. Returns 0=OK, 1=UNSAT, -1=ERROR.
function int zsp_dpi_solve_h(input chandle ctx, input longint seed);

// Retrieve one variable's value after solve.
function longint zsp_dpi_get_value_h(input chandle ctx, input int var_id);

// Release compiled handle.
function void zsp_dpi_release_h(input chandle ctx);
```

### SV Randomizer Base Class (`zsp_randomizer_pkg`)

The `zsp_randomizer #(type T)` virtual class handles the DPI plumbing.
Generated subclasses override:

- `get_problem_b64()` -- returns the base64 problem string
- `get_n_vars()` -- returns the number of rand fields
- `apply_solution(T obj, chandle ctx)` -- reads values via `get_value_h`

Usage:

```systemverilog
MyClass obj = new();
MyClass_randomizer rnd = new();  // decodes + compiles once

repeat (100) begin
    int rc = rnd.randomize_obj(obj, $urandom());
    if (rc != 0) $fatal(1, "randomize failed");
end

rnd.cleanup();  // releases the compiled context
```

## Design Notes

The chandle-based API avoids open-array DPI arguments, which are not
supported by all simulators (notably Verilator).  Problem data is
base64-encoded as a string constant in the generated SV -- decoded once
at construction time.  Per-solve cost is one `solve_h` call plus N
`get_value_h` calls (one per variable), all scalar DPI calls.

## Limitations

- No inline constraints (`randomize_with` is not supported)
- No dynamic arrays or queues as rand fields
- No soft constraints
- Base64 encoding inflates problem data ~33% (decoded once at init)

## Troubleshooting

**Verilator C++ compilation errors for solver sources**
The solver C code uses C11 features not compatible with C++.  Link
against the pre-built `libdv_solve_dpi.so` instead of compiling
sources directly.

**`compile failed` at simulation start**
The base64 problem string is invalid or the buffer doesn't match the
platform's `SolveProblem` layout.  Regenerate the SV from the same
build of the solver library.

## Incremental Solving: Pin, Checkpoint, Restore

The three functions `zsp_dpi_pin_var_h`, `zsp_dpi_checkpoint_h`, and
`zsp_dpi_restore_h` enable **incremental solving** over a single compiled
handle.  This is used when the same constraint problem must be solved
multiple times with different input values pinned -- for example, when
re-solving with different seeds from a common pinned state, or chaining
solves where each step pins inputs to a previous step's output.

### Key design property

The compiled handle maintains a **persistent `SolveCtx`**.  All three
operations act directly on this context:

- `pin_var_h(h, var_id, value)` tightens the variable's domain to
  `[value, value]` and runs constraint propagation.  Returns 0 on success,
  -2 if the pin makes the problem infeasible.
- `checkpoint_h(h)` saves the current domain/trail state and returns a
  checkpoint index (>= 0).
- `restore_h(h, cp)` unwinds the context back to the saved checkpoint,
  undoing all domain changes and search assignments that occurred after it.

### Example: Pin → Solve → Restore → Re-solve

```systemverilog
import zsp_dpi_pkg::*;

module example;
  initial begin
    chandle h;
    int cp, rc;
    longint x, y;

    // Compile: x in [0,10], y in [0,10], x+y==10
    h = zsp_dpi_compile_b64(MY_PROBLEM_B64);

    // Take a clean checkpoint before any pins
    cp = zsp_dpi_checkpoint_h(h);

    // First solve: pin x=3, solve, read y (must be 7)
    rc = zsp_dpi_pin_var_h(h, 0, 3);    // pin x=3
    rc = zsp_dpi_solve_h(h, $urandom());
    y  = zsp_dpi_get_value_h(h, 1);    // y==7

    // Restore to clean checkpoint, then pin x=5
    zsp_dpi_restore_h(h, cp);
    rc = zsp_dpi_pin_var_h(h, 0, 5);   // pin x=5
    rc = zsp_dpi_solve_h(h, $urandom());
    y  = zsp_dpi_get_value_h(h, 1);    // y==5

    zsp_dpi_release_h(h);
    $finish;
  end
endmodule
```

### Return codes

| Function | Return | Meaning |
|---|---|---|
| `pin_var_h` | 0 | Success |
| `pin_var_h` | -1 | Invalid handle or var_id out of range |
| `pin_var_h` | -2 | UNSAT: pin makes the problem infeasible |
| `checkpoint_h` | >= 0 | Checkpoint index (pass to restore_h) |
| `checkpoint_h` | -1 | Error (invalid handle or checkpoint limit exceeded) |
| `restore_h` | void | Always succeeds if cp is a valid index |
