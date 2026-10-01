# Quick start: Python

This example declares three 8-bit variables, constrains them, and draws a few
random solutions.

```{literalinclude} ../../examples/quickstart.py
:language: python
:lines: 2-
```

Running it prints something like:

```text
seed=1: x=188 lo=30 hi=221
seed=2: x=236 lo=5 hi=151
seed=3: x=226 lo=229 hi=238
```

## What each step does

Build the problem
: A {py:class}`~dv_solve.builder.SolveProblemBuilder` collects variables and
  constraints. Each variable has an id (0, 1, 2, ... in turn), a bit width,
  a signedness and a value range. Constraints are expression trees built from
  `expr_var`, `expr_const` and operators such as `expr_binary`.

Finalize
: `finalize()` packs the problem into a compact buffer. The buffer is what
  gets compiled, so one problem description can be compiled and solved many
  times.

Compile
: `SolveCtx(problem)` compiles the buffer. If the constraints can be shown to
  be contradictory at this stage, it raises `CompileUnsatError`.

Solve
: `solve()` returns `SOLVE_OK`, `SOLVE_UNSAT` or `SOLVE_TIMEOUT`. The seed
  selects which solution you get: the same seed gives the same solution, and
  different seeds give different ones. `fair_pick=True` spreads solutions
  evenly over the solution space, which is what you want for random stimulus.
  Call `reset()` before each new solve on the same context.

Read values
: `get_value(var_id)` returns a variable's value from the last successful
  solve.

## Next steps

- {doc}`../concepts/problem-model` lists everything you can express.
- {doc}`../reference/python` is the API reference.
