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
   Verilator's own mechanism for varying the results; dv-solve simply solves
   what it is given.
3. Verilator reads the values back and assigns them.

Results follow Verilator's seed (`+verilator+seed+N`): the same seed repeats a
run exactly, and different seeds give different values.

## When the constraints can't be met

If the constraints contradict each other, `randomize()` returns 0, and
Verilator warns about the constraints involved:

```text
%Warning-UNSATCONSTR: bad.sv:3: Unsatisfied constraint: 'constraint c1 { x > 8'd10; }'
%Warning-UNSATCONSTR: bad.sv:4: Unsatisfied constraint: 'constraint c2 { x < 8'd5; }'
```

dv-solve currently lists every named constraint of the class, not only the
ones that conflict.

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
