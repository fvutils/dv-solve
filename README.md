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
from dv_solve.problem import BIN_GT

b = SolveProblemBuilder()
X = 0
b.add_var(X, width=8, is_signed=False, lo=0, hi=255)
b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(X), b.expr_const(100)))
problem, _ = b.finalize()

with SolveCtx(problem) as ctx:
    assert ctx.solve(seed=1, fair_pick=True) == SOLVE_OK
    print("x =", ctx.get_value(X))
```

Documentation: <https://dvkit.org/fvutils/dv-solve/>

## Public API

| Module | Purpose |
|---|---|
| `dv_solve.builder` | `SolveProblemBuilder` — declare variables and constraints |
| `dv_solve.ctx` | `SolveCtx` — compile and solve; status codes and exceptions |
| `dv_solve.problem` | operator constants (`BIN_*`, `UN_*`) |
| `dv_solve` | `get_libdirs()`, `get_incdirs()`, `get_svdirs()`, `get_dpi_lib()` for build systems |

Other modules are internal and may change. See the
[Python API reference](https://dvkit.org/fvutils/dv-solve/reference/python).

## Zuspec integration

Install `zuspec-solver` to wire this solver into `zuspec-dataclasses` randomization
via the `zuspec.solver.backend` entry point.

## Building the C library

```bash
cmake -S . -B build
cmake --build build
```

The shared library (`libdv_solve.so`) is installed alongside the Python package.
Internal C symbols are prefixed `dvs_`.
