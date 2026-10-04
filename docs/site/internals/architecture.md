# Architecture

This page describes how dv-solve is put together, for readers who want to
know why it behaves as it does. None of it is an interface: any of it may
change in any release.

Every front end turns its input into the same form, a *problem*. Compiling a
problem prepares it for one of two engines, and the engine searches for a
solution.

```text
 Python / C builder ─┐
 SMT-LIB2 ───────────┼─▶ problem ─▶ elaborate ─▶ compile ─▶ CDCL engine ──────────┐
 DPI (base64) ───────┘                       └─────────────▶ bit-blaster ─▶ SAT ──┴─▶ answer
```

## The problem

A problem is one contiguous buffer holding the variable declarations, the
constraint expressions (as trees), and the all-different groups, soft
constraints and distributions. Inside the buffer, items refer to each other
by offset, so the buffer can be copied, saved, or passed to another process
as it is. That is what the DPI string is: the problem buffer in base64.

The builder (`dvs_builder_*` in C, `SolveProblemBuilder` in Python) grows the
buffer as you add to it, and `finalize` packs it to its exact size. A
finalized problem can be compiled any number of times.

## Front ends

**Python and C** build the problem directly, compile it once into a context,
and solve it as often as the caller asks. They use the CDCL engine only.

**SMT-LIB2** (`dv-solve-smt2`) translates each set of assertions into a
problem through the same builder. On `check-sat` it chooses an engine (see
{doc}`../concepts/engines`), solves, and checks every `sat` answer against
the original assertions before reporting it; a model that fails the check is
reported as `unknown`. When the CDCL engine gives up, it retries with the
bit-blaster. The solve runs on a thread with a large stack, so deeply nested
input doesn't exhaust the stack.

**DPI** decodes the base64 string, compiles the problem once when the handle
is created, and solves on each call. Each solve runs inside a private
checkpoint, so it starts from the constraints and the pins, never from the
previous solution.

## Elaboration

Before compiling, the problem's expressions are sized and signed by
SystemVerilog's rules (see {doc}`../concepts/problem-model`): constants get
their width, and an explicit conversion is inserted wherever extending or
truncating changes a value. SMT-LIB2 problems are already explicit and skip
this step.

## Compiling for the CDCL engine

Each variable gets a **domain**: an interval `[lo, hi]` of the values it may
still take. Values can also be ruled out individually (the gaps in an
`inside` set, or `dvs_solver_exclude_value`); these *holes* steer value
choice, while propagation reasons about the interval.

Compiling then:

- **Merges equal variables.** A constraint `x == y` between two variables
  makes them one variable, before any search.
- **Lowers each constraint to propagators.** A propagator enforces one small
  relation, such as `x <= y` or `r == a + b`, by narrowing the domains of its
  variables. Each subexpression of a constraint gets an auxiliary variable,
  and each comparison under `&&`, `||` or `?:` gets a 0/1 *guard* variable
  that is 1 exactly when the comparison holds. {doc}`propagators` lists them.
- **Gates soft constraints.** Each soft constraint's propagators only act
  while an assumption variable for it is 1; dropping a soft constraint sets
  its assumption to 0.
- **Records distributions.** `dist` weights are used when the search picks
  values; they are not constraints.

A constraint whose shape compiling can't lower is counted as *uncompiled*.
Python raises `CompileIncompleteError` for such a problem, and the C API
returns the count, to be treated as an error. SMT-LIB2 and DPI solve without
the constraint and then check the model against the full problem.

## The CDCL engine

**Propagation.** Every propagator watches its variables. When a variable's
domain narrows, its watchers are queued, and the queue runs (cheap
propagators first) until nothing changes or some domain becomes empty, which
is a conflict.

**Search.** The engine picks a variable with the smallest domain, giving
auxiliary variables last, and picks a value for it: at random, weighted by
any `dist`, and avoiding holes. With `fair_pick`, ties between equally good
variables are broken at random as well, which spreads solutions evenly over
the solution space. After a conflict it backtracks and tries the rest of the
domain. The seed drives every random choice, so the same seed repeats a
solve exactly.

**Learning.** For SMT-LIB2 input the engine also learns from conflicts (lazy
clause generation): each propagator can explain a narrowing in terms of
other variables' bounds, and those explanations combine into a learned
clause that stops the search repeating the same mistake. Once it has
learned, it prefers variables involved in recent conflicts. It restarts
periodically and discards learned clauses of little value.

**Limits.** A search gives up after 10 seconds by default
(`time_limit_ms` changes this), or when it would need more than 256 nested
decisions; either way the result is `SOLVE_TIMEOUT` (`unknown`), never a
guess.

## The bit-blaster

The bit-blaster translates the problem into logic over individual bits:
first an and-inverter graph, simplified as it is built, then a CNF formula
for a SAT solver. It handles bit-vectors up to 192 bits and the array and
uninterpreted-function logics of SMT-LIB2.

A SAT solver returns one solution, usually much like the last. To vary them,
the bit-blaster then flips bits the constraints leave free toward random
targets drawn from the seed, keeping each flip only if every assertion still
holds.

## SAT solvers

The bit-blaster uses kissat, which is always built in. CaDiCaL, an optional
second SAT solver (see {doc}`../getting-started/install`), can take new
clauses after it has solved a problem; the few bit-blasting modes that need
that use it. The CDCL engine does not use a SAT solver.

## Memory

A CDCL context lives in a buffer supplied when it is created: the variables,
propagators and other per-problem data are allocated from it, and compiling
fails with `DVS_COMPILE_NOMEM` if it is too small. The Python API uses a
1 MiB buffer, DPI 2 MiB; `dv-solve-smt2` starts small and grows the buffer
when a problem needs more. The search's trail of changes is kept in blocks
from a separate block allocator, so its depth is not limited by the buffer.
