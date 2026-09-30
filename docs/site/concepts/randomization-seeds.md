# Seeds and randomization

dv-solve is built for constrained-random verification, where you want many
different solutions to the same constraints, not just one.

**Seeds make solutions reproducible.** Every solve takes a seed. The same
problem with the same seed produces the same solution, on every run. Different
seeds produce different solutions. To reproduce a failing random test, rerun
it with the seed it used.

**The default seed is fixed.** If you don't set a seed, dv-solve uses the same
one every time, so repeated runs return the same answer. That answer is often
the smallest solution (all-zero where the constraints allow it), which is
fine when you only need to know whether the constraints can be met.

**How to set it:**

| Interface | Seed |
|---|---|
| SMT-LIB2 | `(set-option :seed N)` before `check-sat` |
| Python | `SolveCtx.solve(seed=N)` |

**Spread across the solution space.** For random stimulus, you want each
solution to be about as likely as any other, not a cluster around a few
values. In Python, pass `fair_pick=True` to `solve()`. It changes how the
solver breaks ties between equally good choices, so that the values of every
variable, not just the first one decided, are spread evenly.
