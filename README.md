# dv-solve

Domain-agnostic CLP(FD) constraint solver. No Zuspec runtime dependency.

## Install

```
pip install dv-solve
```

For Zuspec-powered benchmarks and tests:

```
pip install dv-solve[zuspec]
```

## Quick start

```python
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK

b = SolveProblemBuilder()
x = b.add_variable("x", 0, 255, signed=False, bits=8)
b.add_constraint(b.gt(b.var(x), b.constant(100, 8)))
prob_bytes, _ = b.finalize()

ctx = SolveCtx(prob_bytes)
ctx.compile()
assert ctx.solve() == SOLVE_OK
print("x =", ctx.get_value(x))
```

## Module map

| Module | Purpose |
|---|---|
| `dv_solve.lib` | ctypes loader for `libdv_solve.so` |
| `dv_solve.problem` | `SolveProblem` buffer wrapper and expression constants |
| `dv_solve.builder` | `SolveProblemBuilder` — build constraint problems |
| `dv_solve.ctx` | `SolveCtx` — compile and solve problems |
| `dv_solve.icl` | ICL (Instance Constraint Library) tables |
| `dv_solve.partitioner` | Decompose a problem into independent subproblems |
| `dv_solve.structural_solver` | State-chain inference engine |
| `dv_solve.stream_solver` | Stream-linked constraint merging |
| `dv_solve.flow_constraint_store` | Flow-based constraint accumulation |
| `dv_solve.buffer_inference` | Buffer supply inference with ICL lookup and backtracking |
| `dv_solve.state_graph` | State transition graphs |
| `dv_solve.scheduling_graph` | Scheduling constraint graphs |

## Zuspec integration

Install `zuspec-solver` to wire this solver into `zuspec-dataclasses` randomization
via the `zuspec.solver.backend` entry point.

## Building the C library

```bash
cmake -S . -B build
cmake --build build
```

The shared library (`libdv_solve.so`) is installed alongside the Python package.
The C source prefix (`zsp_`) will be renamed to `dvs_` in a future release.
