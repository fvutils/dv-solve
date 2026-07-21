# SMT-COMP 2025 Parallel-Track Bit-Vector Corpus (Task A)

A benchmark corpus + reference-scoring harness for evaluating dv-solve's
**parallelization of separable SAT/SMT problems** against a real competition
baseline.

## Why this corpus

The SMT-COMP *parallel* track is exactly the regime where separable-parallelism
should pay off: the organizers hand-select instances that are **unsolved or very
hard for sequential solvers**, then give solvers one hard instance at a time (a
1200 s wall-clock limit on a large multi-core machine). That is precisely the
"is there value in parallelizing a single hard instance?" question we want to
answer.

Better still, much of the bit-vector selection is **hardware BMC** produced by
yosys/SymbiYosys -- e.g. the `2019-Wolf-fmbench` VexRiscv-unrolled family
(`QF_ABV`) and `brummayerbiere` (Boolector's own HW benchmarks). So it doubles
as a domain-relevant corpus, not just a generic SMT set.

We take the whole **bit-vector family** the parallel track ran:

| logic     | benchmarks |
|-----------|-----------:|
| QF_BV     | 46 (the official "QF_Bitvec" division) |
| QF_ABV    | 24 |
| QF_UFBV   | 19 |
| QF_AUFBV  | 4  |
| **total** | **93** |

Ground-truth status (from the SMT-LIB index): **41 unsat, 15 sat, 37 unknown**
(the 37 unknowns are the genuinely open-hard instances nobody proved in-comp).

## Reference numbers (already captured, no download needed)

`meta/manifest.json` and `meta/reference_results.csv` carry the competition's
**published per-benchmark results** for every solver that ran the BV family:

| solver (parallel track, 1200 s, HW) | solved (of 93) | QF_BV only (of 46) |
|-------------------------------------|---------------:|-------------------:|
| Bitwuzla                            | 32 | 21 |
| STP-Parti-Bitwuzla (winner)         | 27 | 27 |
| Yices2                              | 10 | -- |

(These reproduce the official standings exactly, so we can score dv-solve on the
same solved/unsolved axis without re-running the reference solvers.)

Source of truth: SMT-COMP 2025 processed data
(`results-parallel-2025.json.gz`, `benchmarks-2025.json.gz`, kept verbatim in
`meta/`) and Zenodo record 16887742 (`parallel.tar.gz`).

## Layout

```
meta/
  rp2025.json.gz        raw competition parallel results (verbatim)
  bm2025.json.gz        SMT-LIB benchmark index (verbatim, for ground truth)
  manifest.json         the 93 benchmarks: logic, family, name, truth, per-solver ref
  reference_results.csv flat reference table (one row per benchmark)
smt2/<logic>/<family>/<name>.smt2   the formulas (gitignored; fetch_archive.sh)
build_manifest.py       regenerates meta/manifest.json + reference_results.csv
extract_from_archive.py de-scrambles parallel.tar.gz into smt2/
fetch_archive.sh        download + extract in one step
run_corpus.py           run solvers, score vs ground truth + competition reference
```

## Setup

```bash
# 1. materialize the formulas (downloads 279 MB, extracts 234 MB into smt2/)
bash tests/formal/corpus/smtcomp2025_parallel/fetch_archive.sh

# 2. sanity run (small timeout, few benchmarks)
direnv exec . python tests/formal/corpus/smtcomp2025_parallel/run_corpus.py \
    --logic QF_BV --timeout 30 --limit 5 --solvers dv-solve-smt2,bitwuzla
```

## Scoring notes / caveats

- The competition ran **1200 s, multi-core**. A local single-thread run with a
  small timeout is **not wall-time comparable** -- the honest comparison is
  **solved-count and correctness** (dv-solve must never contradict ground
  truth). Reproduce the headline with `--timeout 1200`.
- dv-solve returning `unknown` fast = honest unsupported-construct bailout
  (sound), *not* a wrong answer. Track those separately as feature gaps.
- 37/93 have no known ground truth; on those we can only compare solved-vs-open
  and cross-check answers against the reference solvers for agreement.

## Roadmap (Task A)

1. **Baseline** — run dv-solve (CDCL + bitblast engines) over all 93 at a large
   timeout; record solved/correct/wrong/unknown vs the reference table. This
   establishes where sequential dv-solve already stands.
2. **Separability analysis** — for the instances dv-solve can *almost* solve,
   measure constraint-component structure (independent sub-formulas, cube/xor
   partitions) to find where divide-and-conquer parallelism is available. Ties
   into `docs/parallelism_analysis.md`.
3. **Parallel prototype** — apply partition-and-conquer (per the parallel-track
   winners' cube-style approach) to the separable instances; re-score against
   this same reference to quantify the parallel speedup / newly-solved count.
