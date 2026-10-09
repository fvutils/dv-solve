# fw-hdl latency scaling (plan piece 5.3)

Measured 2026-10-09 on the development machine (32 cores, one solver per core),
60 s per run. Plan: `docs/fw_hdl_implementation_plan.md` §4 Phase 5 and §6.

## Question

fw-hdl checks that a pipelined Verilog implementation equals its IR at the
right latency. The miter unrolls Yosys' transition relation L steps from an
arbitrary state (`codegen-equiv`, `tests/formal/gen_codegen_equiv.py`). Two
questions:

1. How does solve time grow with the latency L?
2. When fw-hdl searches for the latency, should it send one file per depth
   (monolithic), or one session that adds a step at a time and asks each depth
   under `push`/`pop` (incremental)?

## How it was measured

- `tests/perf/latency_scaling.py` runs the five `codegen-equiv` designs at
  L = 1, 2, 4, 8, 12, 16, 24 and 32. crc32x16 (16 bytes) replaces crc32x4/x8, so
  every depth up to 16 has logic in each stage. Each design runs in two
  variants:
  - `ok` compares at the right step and is unsat.
  - `early` compares one step too soon and is sat.
- `tests/perf/latency_incr.py` asks "does the output at step k equal the
  specification?" for k = 1..16, for each design at its fwhdl-core latency.
  `mono` sums 16 separate runs; `incr` is one session.
- Competitors are the pinned ones (`tests/perf/tools.lock.json`). yices, cvc5
  and z3 ran to L = 16 only.

## Results

### Depth

crc32x16, `ok` (unsat), seconds:

| L | dv-solve | boolector | bitwuzla | yices | cvc5 | z3 |
|---|---|---|---|---|---|---|
| 1 | 0.02 | 0.02 | 0.11 | 35.2 | 41.1 | >60 |
| 2 | 0.02 | 0.04 | 1.83 | 13.7 | 8.2 | >60 |
| 4 | 0.03 | 0.06 | 6.53 | 19.9 | >60 | >60 |
| 8 | 0.07 | 0.15 | 7.31 | 16.5 | >60 | >60 |
| 16 | 0.14 | 0.33 | 8.01 | 24.7 | 35.3 | >60 |
| 24 | 0.23 | 0.52 | 9.36 | | | |
| 32 | 0.30 | 0.69 | 20.2 | | | |

- **dv-solve's time grows linearly with depth.** The fitted exponent over
  L = 4..32 is 1.09; boolector's is 1.16. The problem itself grows linearly
  (one more transition per step).
- **dv-solve is fastest at every depth** (tied with boolector at L = 1). From
  L = 4 it is about 2× ahead of boolector and 25× or more ahead of bitwuzla. z3
  cannot prove any depth within 60 s.

`early` (sat): dv-solve takes 0.02 s at L = 1, 0.15 s at L = 16 and 0.32 s at
L = 32. Here dv-solve is not fastest at depth: yices finds the counterexample
in 0.05 s at L = 16.

The other four designs are small. Slowest run over all depths, `ok`:

| design | dv-solve | boolector | bitwuzla | yices | cvc5 | z3 |
|---|---|---|---|---|---|---|
| addtree8x16 | 0.01 | 1.16 | 1.22 | 2.87 | 0.96 | 6.06 |
| rotl32 | 0.03 | 0.15 | 0.19 | 0.09 | 0.25 | 0.05 |
| mac16 | 0.02 | 0.29 | 0.88 | 0.02 | 0.01 | 2.25 |
| fir4 | 0.01 | 0.42 | 0.45 | 0.03 | 0.22 | 3.76 |

No solver gave a wrong answer.

### Incremental against monolithic

16 depth queries in total, seconds:

| design (L) | dv-solve mono | dv-solve incr | bitwuzla mono | bitwuzla incr | z3 mono | z3 incr |
|---|---|---|---|---|---|---|
| crc32x16 (8) | 1.13 | **0.79** | 10.3 | 4.77 | 677 (3 timeouts) | >120 |
| addtree8x16 (3) | 0.06 | 0.04 | 0.95 | 1.23 | | |
| rotl32 (5) | 0.05 | 0.02 | 0.18 | 0.04 | 0.21 | 0.06 |
| mac16 (3) | 0.08 | 0.06 | 0.28 | 0.07 | 1.50 | >120 |
| fir4 (3) | 0.05 | 0.02 | 0.43 | 0.19 | 1.64 | 18.1 |

Before the fixes below, dv-solve was **slower** incrementally: 3.10 s on
crc32x16 against 2.15 s for the separate files. Each query cost more than the
one before, beyond the growth of the problem.

## What was fixed

The incremental runs exposed three costs that grew with the square of the
problem size. All three were in code that runs on every problem, so single
files sped up too: crc32x16 at L = 16 went from 0.30 s to 0.14 s.

| Where | Cost | Fix |
|---|---|---|
| `dvs_builder_ref_ptr` (`src/c/dvs_builder.c`) | Every expression lookup walked the builder's linked list of 4 KB blocks. Half of a single run's time. | A sorted array of block pointers, searched by binary search. |
| `_find_var` (`src/c/smt2/smt2_frontend.c`) | A linear scan with `strlen` over every variable. The front end binds each `let` through a variable, so crc32x16 at depth 16 has over 100k of them. This took 60% of the incremental session. | A hash index over the names. It is built lazily, rebuilt after a `pop`, and returns the same (lowest) slot as the scan. |
| `try_record_subst`, `try_record_slice` (`src/c/dvs_bbsolver.c`) | Each substitution cleared a per-variable byte array before its cycle check: substitutions × variables bytes. | Epoch stamps (`reach_mark`), so nothing is cleared per query. |

The runs also found **B86**: a session ending `(check-sat) (pop 1)` printed
`sat` but exited 2 (`unknown`). `push`, `pop` and `assert` clear the model,
and the exit status was read from it. It is now read from the last
`check-sat`'s answer. See `docs/solver_bug_backlog.md`.

## Default mode

There is no mode to change in dv-solve; the choice is how fw-hdl drives it.

- **One property at a known latency:** send one file (monolithic). It costs
  the same as the last query of a session.
- **A latency search, or many depths:** use one incremental session (assert
  one transition per step, ask each depth under `push`/`pop`). It is now faster
  than separate files: 0.79 s against 1.13 s on crc32x16.

The rest of the incremental cost is that each `check-sat` bit-blasts the
whole assertion set again; SAT itself takes under 1 ms per query. Reusing the
encoding across queries is piece 6.1, and would bring a 16-depth search close
to the cost of its deepest query (0.14 s here).
