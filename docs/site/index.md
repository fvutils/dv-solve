# dv-solve

dv-solve is a constraint solver for design verification. It finds values for
bit-vector variables that satisfy a set of constraints, and when asked for
many solutions it spreads them across the solution space rather than
returning the same answer each time. That makes it suited both to
constrained-random stimulus generation and to answering yes/no questions from
formal tools.

Answers are sound: `sat` and `unsat` are definitive. When dv-solve can't
decide a problem, or meets a construct it doesn't support, it says `unknown`
rather than guessing.

## Ways to use it

| If you are... | Use | Start here |
|---|---|---|
| Building constraint problems from Python | the Python API (`dv_solve`) | {doc}`getting-started/quickstart-python` |
| Running SMT-LIB2 files, or plugging a solver into a tool that speaks SMT-LIB2 | the `dv-solve-smt2` executable | {doc}`getting-started/quickstart-smt2` |
| Randomizing SystemVerilog classes in Verilator | `dv-solve-smt2` as Verilator's constraint solver | {doc}`getting-started/quickstart-verilator` |
| Randomizing from SystemVerilog through DPI | the `zsp_dpi_pkg` SystemVerilog package and `libdv_solve_dpi` | {doc}`getting-started/install` |
| Embedding the solver in a C or C++ program | the C library, `libdv_solve` | {doc}`getting-started/install` |

```{toctree}
:maxdepth: 2
:caption: Getting started

getting-started/install
getting-started/quickstart-python
getting-started/quickstart-smt2
getting-started/quickstart-verilator
```

```{toctree}
:maxdepth: 2
:caption: Concepts

concepts/soundness
concepts/engines
concepts/randomization-seeds
```

```{toctree}
:maxdepth: 2
:caption: Guides

guides/smt2-solver
guides/verilator
```

```{toctree}
:maxdepth: 2
:caption: Reference

reference/cli
reference/environment
```
