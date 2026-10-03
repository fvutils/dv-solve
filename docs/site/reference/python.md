# Python API

The public Python API is the problem builder, the solve context, their
status codes and exceptions, and the operator constants. Anything not listed
on this page is internal and may change.

For how problems are expressed, see {doc}`../concepts/problem-model`.

## Building a problem

```{autoclass} dv_solve.builder.SolveProblemBuilder
:members: add_var, add_constraint, add_soft_constraint, add_all_different, add_dist,
  expr_var, expr_const, expr_binary, expr_unary, expr_ite, expr_in_set, expr_in_range,
  expr_in_ranges, expr_extract, expr_concat, expr_extend, expr_cast, expr_sum, expr_countones,
  expr_clog2, finalize, finalize_bytes, reset, destroy
:member-order: bysource
```

## Solving

```{autoclass} dv_solve.ctx.SolveCtx
:members: solve, reset, get_value, destroy
:member-order: bysource
```

### Results

`SolveCtx.solve()` returns one of these constants from `dv_solve.ctx`:

| Constant | Meaning |
|---|---|
| `SOLVE_OK` | A solution was found; read it with `get_value`. |
| `SOLVE_UNSAT` | The constraints cannot be met. |
| `SOLVE_TIMEOUT` | The search gave up before deciding. |

### Exceptions

```{autoexception} dv_solve.ctx.CompileUnsatError
```

```{autoexception} dv_solve.ctx.CompileIncompleteError
```

```{autoexception} dv_solve.ctx.CompileUnsupportedError
```

## Operator constants

`dv_solve.problem` defines the operator codes for `expr_binary` and
`expr_unary`: `BIN_ADD`, `BIN_SUB`, `BIN_MUL`, `BIN_DIV`, `BIN_MOD`,
`BIN_BAND`, `BIN_BOR`, `BIN_BXOR`, `BIN_LSHIFT`, `BIN_RSHIFT`, `BIN_ASHR`, `BIN_EQ`,
`BIN_NEQ`, `BIN_LT`, `BIN_LTE`, `BIN_GT`, `BIN_GTE`, `BIN_AND`, `BIN_OR`,
`UN_NEG`, `UN_NOT` and `UN_INVERT`. Their meanings are listed in
{doc}`../concepts/problem-model`.

## Locating the native files

The functions for finding the shared libraries, C headers and SystemVerilog
packages are described in {doc}`../guides/packaging`.
