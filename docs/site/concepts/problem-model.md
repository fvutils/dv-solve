# The problem model

This page describes how a constraint problem is expressed through the Python
and C APIs. (SMT-LIB2 input has its own rules; see {doc}`../guides/smt2-solver`.)

## Variables

A variable has:

- an **id**, a number you choose and use to refer to it;
- a **width** in bits, from 1 to 64;
- a **signedness**;
- a **range**, `lo` to `hi` inclusive, that its value must stay within.

```python
b.add_var(0, width=8, is_signed=False, lo=0, hi=255)
```

The range is itself a constraint. A narrower range than the width allows is a
cheap way to restrict a variable.

## Expressions

Expressions are built bottom-up. Each builder method returns a reference (an
integer) that you pass to other methods:

```python
x   = b.expr_var(0)
ten = b.expr_const(10)
b.add_constraint(b.expr_binary(BIN_GT, x, ten))    # x > 10
```

**Arithmetic is on integers.** Results are not wrapped to the width of the
variables involved: for 8-bit `a` and `c`, `a + c == 300` can be satisfied
(for example by 100 and 200). This matches SystemVerilog, where such an
expression is evaluated at 32 bits or wider. Division truncates toward zero.

## Operators

Binary operators, used with `expr_binary(op, lhs, rhs)`:

| Constant | Meaning |
|---|---|
| `BIN_ADD`, `BIN_SUB`, `BIN_MUL` | `+`, `-`, `*` |
| `BIN_DIV`, `BIN_MOD` | `/`, `%` |
| `BIN_BAND`, `BIN_BOR`, `BIN_BXOR` | bitwise `&`, `\|`, `^` |
| `BIN_LSHIFT`, `BIN_RSHIFT` | `<<`, `>>` |
| `BIN_EQ`, `BIN_NEQ` | `==`, `!=` |
| `BIN_LT`, `BIN_LTE`, `BIN_GT`, `BIN_GTE` | `<`, `<=`, `>`, `>=` |
| `BIN_AND`, `BIN_OR` | logical `&&`, `\|\|` |

Unary operators, used with `expr_unary(op, operand)`:

| Constant | Meaning |
|---|---|
| `UN_NOT` | logical `!` |
| `UN_INVERT` | bitwise `~` |
| `UN_NEG` | arithmetic `-` |

All of these are in `dv_solve.problem`.

Other expression forms:

| Method | Meaning |
|---|---|
| `expr_ite(cond, a, b)` | `cond ? a : b`; with Boolean `a` and `b`, an if/else constraint |
| `expr_in_set(v, [e1, e2, ...])` | `v inside {e1, e2, ...}` |
| `expr_in_range(v, lo, hi)` | `v inside {[lo:hi]}` |
| `expr_in_ranges(v, [(lo, hi), ...])` | `v inside {[lo1:hi1], [lo2:hi2], ...}` |
| `expr_extract(v, hi, lo)` | `v[hi:lo]` |
| `expr_concat(hi, lo, lo_width)` | `{hi, lo}` |
| `expr_extend(v, from_bits, to_bits, sign_extend)` | zero- or sign-extension |

Some relations are constraints in their own right. Pass what they return to
`add_constraint`:

| Method | Constraint |
|---|---|
| `expr_sum(total, [e1, e2, ...])` | `total == e1 + e2 + ...` |
| `expr_countones(n, v)` | `n == $countones(v)` |
| `expr_clog2(n, v)` | `n == $clog2(v)` |

## Constraints

- `add_constraint(e)` requires `e` to hold.
- `add_all_different([id1, id2, ...])` requires the variables to take
  pairwise different values (SystemVerilog `unique`).
- `add_soft_constraint(e, priority)` asks for `e` to hold where possible.
  See {doc}`soft-constraints`.
- `add_dist(id, entries)` gives a variable a weighted distribution
  (SystemVerilog `dist`). See {doc}`randomization-seeds`.

## Example

Each part of this example builds and solves one small problem:

```{literalinclude} ../../examples/features.py
:language: python
:lines: 2-
```

## Limits

- Variables are at most 64 bits wide. A wider variable raises
  `CompileUnsupportedError`; wider bit-vectors are supported through the
  SMT-LIB2 front end.
- Some constraint shapes can't be compiled yet and raise
  `CompileIncompleteError` instead of being ignored. The most common is
  `UN_NEG` or `UN_INVERT` applied to a variable and then compared with a
  constant; write `-x` as `BIN_SUB` of `0` and `x` instead.
