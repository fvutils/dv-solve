# dv-solve

dv-solve is a constraint solver for design verification. It finds values for
bit-vector variables that satisfy a set of constraints, and when asked for many
solutions it spreads them across the solution space. That suits it both to
constrained-random stimulus generation and to answering yes/no questions from
formal tools. Answers are sound: `sat` and `unsat` are definitive, and when
dv-solve can't decide a problem it says `unknown` rather than guessing.

**Documentation: <https://dvkit.org/fvutils/dv-solve/>**

## Ways to use it

| If you are... | Use | Start here |
|---|---|---|
| Building constraint problems from Python | the `dv_solve` package | [Python quick start](https://dvkit.org/fvutils/dv-solve/getting-started/quickstart-python) |
| Running SMT-LIB2 files, or plugging a solver into a tool that speaks SMT-LIB2 | the `dv-solve-smt2` executable | [SMT-LIB2 quick start](https://dvkit.org/fvutils/dv-solve/getting-started/quickstart-smt2) |
| Randomizing SystemVerilog classes in Verilator | `dv-solve-smt2` as Verilator's constraint solver | [Verilator quick start](https://dvkit.org/fvutils/dv-solve/getting-started/quickstart-verilator) |
| Randomizing from SystemVerilog through DPI, on any simulator | `dvs_dpi_pkg` and `libdv_solve_dpi` | [DPI guide](https://dvkit.org/fvutils/dv-solve/guides/systemverilog-dpi) |
| Embedding the solver in a C or C++ program | `libdv_solve` and `dv_solve.h` | [C API](https://dvkit.org/fvutils/dv-solve/reference/c-api) |

## Install

```
pip install dv-solve
```

The wheel holds the Python package, the native libraries, the C header, the
SystemVerilog packages and, on Linux and macOS, the `dv-solve-smt2`
executable, which pip puts on `PATH`:

```bash
dv-solve-smt2 --version
```

To build everything from source instead:

```bash
cmake -S . -B build -DDVS_WITH_CADICAL=OFF
cmake --build build
```

See [Installation](https://dvkit.org/fvutils/dv-solve/getting-started/install)
for the details, including the optional CaDiCaL back end.

To develop in a checkout, set up the workspace with IVPM and build in place:

```bash
ivpm update -d dev
packages/python/bin/python setup.py build_ext --inplace
```

That fetches CaDiCaL and the Python environment into `packages/`, then builds
the libraries into `build/lib` and `dv-solve-smt2` into `build/bin`. The
`dv_solve` package in `src/` loads the library from `build/lib`.

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

## Public API

| Module | Purpose |
|---|---|
| `dv_solve.builder` | `SolveProblemBuilder`: declare variables and constraints |
| `dv_solve.ctx` | `SolveCtx`: compile and solve; status codes and exceptions |
| `dv_solve.problem` | operator constants (`BIN_*`, `UN_*`) |
| `dv_solve` | `get_libs()`, `get_libdirs()`, `get_incdirs()`, `get_svdirs()`, `get_dpi_lib()` for build systems |

Other modules are internal and may change. The C API is the one header
`dv_solve.h`; other installed headers are internal. See the
[reference](https://dvkit.org/fvutils/dv-solve/reference/python).

## License

Apache-2.0.
