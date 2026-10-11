# svcorpus — real SystemVerilog constraints as native solver tests

Constraint classes from open-source verification code (OpenTitan DV first,
riscv-dv next), each written as **direct builder API calls** so the solver
can be timed and its spread scored on its own: no simulator, no SMT-LIB
text in the measured path.

```
cases/NAME.py            the test: the class's constraints as SolveProblemBuilder calls
captures/NAME/harness.sv the class standalone (UVM stripped, constants inlined)
captures/NAME/base.smt2  the first query Verilator 5.052 sent for the harness
```

## How a case is made

1. **Harness.** Copy the class's rand variables and constraint blocks into a
   standalone `harness.sv`, as written. Non-random state the constraints read
   (cfg knobs, other objects) becomes a constant from the source's defaults.
   Smallest possible changes where Verilator rejects a construct; every change
   is listed in the case's docstring.
2. **Capture.** `tests/perf/svcorpus/capture.sh NAME` compiles the harness with
   Verilator, runs it with z3 as the solver, and keeps the first query as
   `base.smt2`. Verilator is the capture tool only.
3. **Case.** `cases/NAME.py` mirrors the SV source term for term with builder
   calls, in SystemVerilog sizing (the builder follows IEEE 1800). `dist`
   weights and `solve ... before` are recorded (`DIST`, `SOLVE_BEFORE`);
   `dist` membership is a hard constraint, as in SV. Format: `case.py`.
4. **Check.** `python -m tests.perf.svcorpus.equiv NAME` — z3 proves the case
   and the capture have the same solutions. Verilator's own protocol (phased
   `solve ... before`, the value it draws itself for a `dist`, parity hashes)
   comes after the first query and is not part of the case.

`test_svcorpus.py` (in the normal `pytest` run) repeats the equivalence check
and checks 50 seeded draws per case against the capture.

## Running the bench

```
env -u LD_LIBRARY_PATH PYTHONPATH=src:. python -m tests.perf.svcorpus.bench \
    --n 2000 --arm head=<HEAD build>/lib --arm now=build/lib [CASE...]
```

One process per arm (`DVS_SOLVER_PATH`): compile once, then N seeded
`reset(); solve(seed)` calls, as a native front end (zuspec, SV DPI) drives
the solver. Reports solve time (median/mean/p90), compile time, bad models
(checked against the capture, independent of the builder, and with
`validate_model()`), failed solves, and spread: per variable (16 buckets for a
wide one) the excess Jensen–Shannon divergence from a rejection-sampled
reference of the SV meaning (`solve ... before` first, `dist` by weight), and,
for cases with `solve ... before`, from uniform over the solutions. Results go
to `perf-out/svcorpus/<stamp>.json`.
