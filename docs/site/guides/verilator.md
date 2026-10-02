# Using dv-solve with Verilator

Verilator 5.x hands every `randomize()` call to an external SMT-LIB2 solver.
This page explains how dv-solve serves those calls. For the setup, see
{doc}`../getting-started/quickstart-verilator`.

## Setting it up

```bash
export VERILATOR_SOLVER="/path/to/dv-solve-smt2 --interactive --mode=verilator"
```

Verilator reads the variable when the simulation starts, so the same compiled
model can run with dv-solve or with z3.

`--interactive`
: Verilator talks to the solver over a pipe and waits for each answer, so
  commands must be answered as they arrive.

`--mode=verilator`
: The problems Verilator sends are randomization requests, not proofs. In
  this mode dv-solve solves them with the engine that spreads solutions most
  evenly, and keeps recently used problems ready, since a testbench usually
  calls `randomize()` on the same class many times.

## What happens in a `randomize()` call

1. Verilator sends the class's random variables and its enabled constraints,
   and asks for a solution.
2. It then adds a few random parity constraints over the variables' bits and
   asks again, keeping the last solution that satisfied them. This is
   Verilator's own mechanism for varying the results; by default dv-solve
   solves what it is given (see below).
3. Verilator reads the values back and assigns them.

Results follow Verilator's seed (`+verilator+seed+N`): the same seed repeats a
run exactly, and different seeds give different values.

## Verilator's parity constraints

Verilator adds up to four parity constraints to every `randomize()` call
because most solvers return the same solution to the same question. dv-solve
already picks a random solution each time, so for dv-solve the parity
constraints mostly add cost: they take most of the time of a call.

`--verilator-hash=ignore` skips them: dv-solve answers each of those queries
with the solution it already found, which satisfies every constraint of the
class. Only Verilator's request for variety is not honoured.

```bash
export VERILATOR_SOLVER="/path/to/dv-solve-smt2 --interactive --mode=verilator --verilator-hash=ignore"
```

Calls get much cheaper. Whether the values spread as well depends on the
constraints: the parity constraints also even out what dv-solve's own choice
leaves uneven. The {doc}`../results/randomization` page measures both settings
on every benchmark, so you can see where each one stands.

## When the constraints can't be met

If the constraints contradict each other, `randomize()` returns 0, and
Verilator warns about the constraints involved. For this class:

```{literalinclude} ../../examples/verilator/bad.sv
:language: systemverilog
:lines: 1-6
```

the simulation prints:

```text
%Warning-UNSATCONSTR: bad.sv:3: Unsatisfied constraint: 'constraint c1 { x > 10; }'
%Warning-UNSATCONSTR: bad.sv:5: Unsatisfied constraint: 'constraint c3 { x < 5; }'
randomize() failed
```

The warnings name a minimal conflicting set: `c1` and `c3` can't both hold,
and removing either one would make the rest satisfiable. `c2` is not listed
because it plays no part in the conflict.

## When dv-solve can't decide

If a constraint uses something dv-solve doesn't support, dv-solve answers
`unknown` (see {doc}`../concepts/soundness`). Verilator reports that as

```text
%Warning: .../verilated_random.cpp:624: Internal: Solver error: unknown
```

and `randomize()` returns 0. The SMT-LIB2 operators dv-solve does not support
yet are listed in {doc}`smt2-solver`.

To see why dv-solve answered `unknown`, set `DV_LOG` to a file name before
running the simulation. dv-solve writes the reason for each `unknown` there.
