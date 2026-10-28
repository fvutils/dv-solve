# Uniformity harness (P0.6)

Measures how close dv-solve's randomize() answers are to LRM-uniform (IEEE 1800:
uniform over the solutions of the constraints), against an exact reference. It is
the quality gate for every new sampling layer on the way to 1.0.0, and the tool
for the 0.x vs 1.0 distribution comparison.

## How it works

1. **Problems.** Each problem is the SMT-LIB of one randomize() call:
   - `families.py`: reduced-width versions of the simbench constraint shapes
     (countones, unique, sorted, sum, mul/div, mod, ordered regions, implication,
     n-queens), written as plain QF_BV;
   - `corpus/<bench>.smt2`: the real problems simbench benches send, in Verilator's
     encoding, up to 4 distinct problems per bench (the most frequent ones).
2. **Ground truth.** z3 enumerates every solution over the get-value variables.
   Problems with more than `--cap` (4096) solutions, or that z3 cannot enumerate
   within 20 s per model, are `NOENUM`: they are sampled and reported, but not tested.
3. **Arms.** Each arm draws N models, one randomize() per seed. Seeds are 31-bit and
   spread out like Verilator's (`seeds()`), and the session shape is the
   simulator's (`(reset)`, seed, problem, `check-sat`, `get-value`).

   | arm | path | simbench config |
   |---|---|---|
   | `default` | `dv-solve-smt2`, default mode (a seeded check-sat goes to bitblast) | `vlt-dvs-solver` |
   | `cdcl` | `dv-solve-smt2 --engine=cdcl` | engine oracle (P0.2) |
   | `verilator` | `dv-solve-smt2 --mode=verilator` (CDCL sampling, fingerprint cache) | — |
   | `api-cache` | `bench_vlt_replay --passes api-cache --dump-values` (compiled-context reuse) | `dvs-standalone` |
   | `uniform` | a true RNG over the enumerated set | noise floor |

4. **Tests.** N = max(`--n`, 5 × solutions), up to `--n-max`. Each test is at
   `--alpha` (1e-3), Bonferroni-corrected over the tests of a problem:
   - `joint`: chi-square over all solutions (only when N ≥ 5 per solution);
   - one per variable: chi-square of its values against its exact marginal,
     with adjacent values merged until every bin expects ≥ 5.
   - `distinct`: how many distinct solutions were hit, against the exact law of
     that count under uniform draws. It is one-sided: too few is how a sampler
     that stays near a few base models fails. This is the test that still works
     when N is far below the number of solutions.

   It also reports coverage, total-variation distance (`tv`), and the max/min
   observed:expected ratio. Compare `tv` with the `uniform` arm's: that is what
   pure sampling noise gives at the same N.

   `--budget` (300 s) caps each arm on each problem. A slower arm gets fewer draws
   (at least 200), and its row records `reduced_from`.

Verdicts: `PASS`, `FAIL` (a test rejected uniform), `UNSOUND` (a model outside the
solution set), `UNANSWERED` (a call returned unsat or unknown), `NOENUM`, `ERROR`.

The constraint is LRM-uniform over *solutions*, not uniform per variable. For
example, under `mode==0 -> x<2` the variable `mode` is 0 in only 2 of 26 solutions,
so a sampler that picks `mode` uniformly first fails. The marginal tests compare
against the exact marginals for that reason.

## Running

```
python3 tests/uniformity/uniformity.py                   # families + corpus, 4 arms
python3 tests/uniformity/uniformity.py --source families --arms default,uniform
python3 tests/uniformity/uniformity.py --only countones,unique --n 2000
python3 tests/uniformity/uniformity.py --jobs 24 --json out.jsonl --md out.md
```

To compare versions, point it at another build. It only drives the two tools, so
it runs unchanged against any build that has them:

```
python3 tests/uniformity/uniformity.py --build ../dv-solve-v0.3.0/build --json v030.jsonl
```

Each build must be built from its own tag in its own directory. `dv-solve-smt2`
loads `libdv_solve.so` from its build directory, so a copied binary runs the new
library. The JSON header records `--build-info` and the library's sha256, so check
those.

`pytest tests/uniformity` checks the harness itself: the statistics accept uniform
and reject biased samples, the `uniform` arm passes every family, and every
dv-solve arm returns only models that satisfy the constraints. Uniformity verdicts
do not gate there: the 0.x samplers are known not to be uniform, and the harness
exists to measure by how much.

## Corpus

`corpus.py extract <session.cmds.smt2> <bench>` takes a captured simbench session
(`SIMBENCH_SESSION_DIR`, or the `session.cmds.smt2` in a dvs-standalone run dir).
It writes the bench's most frequent distinct problems. A call's problem is its
assert stack (push/pop replayed) at the first check-sat that z3 finds sat. That
resolves Verilator's soft-constraint retries, which come before it. It leaves out
what comes after it: up to 4 random parity asserts, which are Verilator's own
diversity protocol and not part of the user's constraints.

The current corpus has 72 of the 100 benches, from a 2026-10-26 capture. The
missing ones (most of memmap and scenario) need a capture with a sampler-control
Verilator (see `bench/simbench/README.md` §3.2). The dist benches resolve the
bucket in Verilator, so each of their problems is a single-bucket problem, often
with one solution.

## Requirements

z3 (Python module) and scipy, both in the dev environment.
