# How the results are measured

The results pages are generated from data, not written by hand: a nightly
performance job measures, its records are kept, and the pages are rendered
from the newest one when the documentation is built. Everything needed to
reproduce a number is in the repository under `tests/perf/`.

## What is compared

dv-solve is run as an SMT-LIB2 solver, `dv-solve-smt2 <file>`, on the same
fixtures as the reference solvers, in the same job on the same machine:

| Solver | What it shows |
|---|---|
| dv-solve | the commit being measured, with its automatic engine choice |
| dv-solve, bit-blasting forced | the same build with `DV_ENGINE=bitblast` |
| dv-solve v0.1.0 | the first release, built from its tag: the fixed baseline for comparing dv-solve with itself over time |
| bitwuzla, z3 | the reference SMT solvers, at versions pinned in the repository |
| boolector | a third reference, on QF_BV fixtures only: it cannot read the dialect the others use |

The pinned versions are in `tests/perf/solvers.lock` (pip) and
`tests/perf/tools.lock.json` (release tarballs, checked by sha256). A
different version is refused rather than measured.

The fixtures are the `sat-core` suite, `tests/perf/suites/sat-core.json`:
the SMT-LIB2 regression fixtures of the repository, grouped by category. They
are mostly small problems from verification tools, Verilator's randomization
among them.

## How time is measured

- **CPU time, not wall time.** Each solver runs as a separate process and its
  user plus system CPU time is read from the operating system when it exits.
  Time spent waiting for a processor while the machine is busy is not
  counted.
- **Minimum of several runs.** Each solver runs each fixture 5 times (3 when
  a run takes over a second), alternating between solvers, and the minimum is
  kept.
- **Start-up is reported separately.** Every solver also answers a
  one-variable problem; that time is its start-up cost. A speed-up compares
  the time spent beyond start-up, so a faster-starting process does not
  count as a faster solver.
- **Tiny fixtures are left out of speed-ups.** When both solvers finish within
  a millisecond of their start-up cost, the comparison would measure noise,
  so the fixture counts as answered but not in the speed-up.
- **Time limit.** 10 seconds per run; a solver that runs out answers nothing.

## How speed-ups are summarised

A speed-up is a geometric mean of per-fixture ratios, first within each
category and then across categories with the weights listed on the
{doc}`sat` page. Verilator fixtures weigh double, because constraint solving
for simulation is what dv-solve is built for, and no category may carry more
than 40% of the total. The weights are a choice about presentation: they are
applied when the pages are rendered, so changing them never needs a new
measurement.

## Correctness

Every `sat` or `unsat` that dv-solve gives is checked against the reference
solvers in the same run. A contradiction from the version being measured
fails the job, and its numbers are not published. Contradictions from the old
release used as a baseline are listed, not hidden: they are bugs fixed since.
When dv-solve cannot decide a problem it answers `unknown`, which is counted
as not answered.

## Comparing over time

The machine, its load and the reference solvers can all change between
nights, so the numbers that are tracked over time are ratios measured within
one run: against the reference solvers, and against the v0.1.0 baseline,
which is measured in every run. Each run also times a fixed calibration
workload, which shows when the machine itself has changed. A run taken while
the machine was busy is marked as such on the pages.

## Where the data is kept

Each run produces one record, kept as a build artifact for 90 days. A compact
summary of every run, and the full record of every release, are committed to
the repository under `tests/perf/history/`. The design is described in
`docs/results_publishing_design.md` in the repository.

## Reproducing a number

With a build of dv-solve in `build/` and the pinned solvers available:

```sh
python3 -m tests.perf.tools fetch --dest perf-tools
DVS_PERF_TOOLS=perf-tools python3 -m tests.perf.collect --suites sat-core --out perf-out
python3 -m tests.perf.render --records perf-out
```
