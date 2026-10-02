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

## Randomization

Each benchmark of the `rand-core` suite is a small constrained-random
problem, written once and given to every solver in the same form. Each solver
randomizes it 2000 times, and the solutions are scored against the exact
answer: the benchmarks with at most 16 random bits are enumerated in full,
and the two with 32-bit fields are scored on bins of solutions whose sizes
are known exactly.

| Solver | How it is driven |
|---|---|
| Verilator + dv-solve, z3, bitwuzla | one solver process for all 2000 calls, sent exactly the commands Verilator 5.046 sends for each randomize() |
| Verilator + dv-solve, parity ignored | the same, with `--verilator-hash=ignore`: dv-solve skips Verilator's parity constraints and keeps the solution it found. Measured beside the default so that a benchmark where dv-solve's own spread is worse than the parity constraints' shows |
| dv-solve API | dv-solve's library called directly, one compiled problem solved with a new seed each call, as SystemVerilog DPI and zuspec do |
| true random | a random choice among all solutions: what a perfect sampler scores at the same sample size |

The scores:

- **Coverage:** the share of solutions (or bins) returned at least once.
- **Excess JSD:** the Jensen-Shannon divergence, in bits, between the
  solutions returned and a uniform choice among all solutions, minus the same
  divergence for the true random sample. A finite sample is never exactly
  uniform, so the true random row is the floor; 0 is as good as true random.
- **Thin:** for benchmarks with a rare branch (`thold == 0` in a timer, the
  small band of two ranges), how often it comes up relative to its share of
  the solutions; 1 is ideal.
- **CPU per call:** for the Verilator rows, the solver process's CPU time
  divided by the number of calls; for the API row, the time inside the solve
  call. Verilator's own side is not counted for any solver.

Randomization benchmarks run one at a time: next to parallel runs z3's CPU
time roughly doubles while dv-solve's hardly changes, so running them side by
side would favour dv-solve.

Verilator does not ask its solver for a random solution. For each
randomize() it sends the constraints and asks for any solution; then, up to
four times, it adds a random parity constraint over about half of all the
random bits and asks again, keeping the last solution found. The command
sequence used here is checked against a real Verilator 5.046 simulation
before every run, command by command and in the distribution of the values
it returns (`tests/unit/test_vlt_protocol.py`); a run whose check fails
publishes nothing.

Every solution any solver returns is checked against the constraints. A
solution from the version being measured that violates them fails the job.

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
DVS_PERF_TOOLS=perf-tools python3 -m tests.perf.collect --suites sat-core,rand-core --out perf-out
python3 -m tests.perf.render --records perf-out
```
