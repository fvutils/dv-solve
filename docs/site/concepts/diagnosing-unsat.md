# Diagnosing `unsat`

An `unsat` answer means no assignment meets every constraint at once (see
{doc}`soundness`). To fix the constraints you need to know which ones
conflict. The useful answer is a *minimal* conflicting set: a set of
constraints that can't all hold, where dropping any one of them leaves the
rest satisfiable. A problem can contain more than one such set; fixing one
may reveal another.

## In Verilator

Verilator asks dv-solve for a minimal conflicting set whenever `randomize()`
fails, and prints one `UNSATCONSTR` warning per constraint in it. The other
constraints of the class are not listed. See
{doc}`../guides/verilator` for an example.

## In SMT-LIB2

Name the assertions you want reported with `:named`, set
`:produce-unsat-cores`, and ask for `get-unsat-core` after `unsat`:

```{literalinclude} ../../examples/smt2/unsat_core.smt2
:language: lisp
```

The reply names a minimal set; `len_max` and `hdr_fits` are left out because
they play no part in the conflict:

```{literalinclude} ../../examples/smt2/unsat_core.expected
:language: text
```

Unnamed assertions are always kept in the problem but never reported. Without
`:produce-unsat-cores`, `get-unsat-core` lists every named assertion in scope,
which is a valid but usually larger set. The limits are described in
{doc}`../guides/smt2-solver`.

## In Python and C

The builder APIs have no unsat-core call. Find a minimal set yourself by
re-solving without each constraint in turn: if the rest is still
unsatisfiable, leave that constraint out for good. What remains when every
constraint has been tried is minimal.

```{literalinclude} ../../examples/unsat_core.py
:language: python
:lines: 2-
```

This takes one solve per constraint. For a large problem, drop constraints in
halves first and refine within the half that still conflicts.

The C API works the same way: build each candidate problem with the subset of
constraints; it is unsatisfiable when `dvs_solver_compile()` returns
`DVS_COMPILE_UNSAT` or `dvs_solver_solve()` returns `DVS_SOLVE_UNSAT`.

## If you believe the answer is wrong

`unsat` is meant to be definitive. If a set of constraints you know to be
satisfiable comes back `unsat`, that is a bug; please report it with the
input. Checking the same problem with another SMT solver, such as z3 or
bitwuzla on the SMT-LIB2 form, is a quick way to confirm it.
