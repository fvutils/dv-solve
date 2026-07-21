# Task-A Baseline — dv-solve vs SMT-COMP 2025 parallel-track BV corpus

Run: `run_corpus.py --timeout 120 --solvers dv-solve-smt2,dv-solve-smt2-bb`
(93 benchmarks, single-thread, 120s/benchmark). Raw: `baseline_120s.csv`.

**Caveat:** competition ran 1200s multi-core; this is a fast single-thread pass.
Compare solved-count + correctness, not wall-time.

## Scorecard

| engine (120s, 1 thread)      | solved | correct | **wrong** | timeout | unknown | error |
|------------------------------|-------:|--------:|----------:|--------:|--------:|------:|
| dv-solve-smt2 (CDCL)         | 5      | 5       | **0**     | 9       | 77      | 2     |
| dv-solve-smt2-bb (bitblast)  | 6      | 6       | **0**     | 17      | 68      | 3     |
| union (either engine)        | 6      | 6       | **0**     | —       | —       | —     |
| — competition reference (1200s, multi-core) — |
| Bitwuzla                     | 32     | | | | | |
| STP-Parti-Bitwuzla (winner)  | 27     | | | | | |
| Yices2                       | 10     | | | | | |

All 6 dv-solve solves agree with ground truth and with the reference solvers.

## What the gap actually is

The gap to Bitwuzla is **not** solver speed — it's feature coverage + a crash:

- **Arrays / UF are a wholesale gap.** Every QF_ABV (24), QF_UFBV (19), and
  QF_AUFBV (4) benchmark returns fast honest `unknown` — 47 benchmarks, ~half the
  corpus, bailed in <2s because the array/UF path isn't supported on this route.
- **QF_BV is the reachable frontier.** All 6 solves + all 9–17 timeouts are QF_BV.
  Of 46 QF_BV: 6 solved, ~9 timeout (real hard-solve), rest fast-unknown/error.
- **Of the fast-unknowns, 33 were solved by a reference solver** → concrete,
  addressable feature gaps. The other 38 are genuinely open (nobody solved them
  in-competition either).
- **B10 crash (logged in backlog):** `asp/GraphColouring` (nodes=130/140) SIGSEGV
  on both engines; `sudoku.in3` hangs under bitblast. Plain QF_BV, valid input.

## Timeouts — the genuine parallelism candidates (QF_BV only)

These solve slowly (not unknown), so more time / partitioning could help. Marked
whether a reference solver cracked them at 1200s:

| benchmark | truth | ref solved it? |
|---|---|---|
| 68.smt2 | sat | Bitwuzla, STP-Parti |
| edge-matching-w10-h10-c13 | sat | Bitwuzla, STP-Parti |
| string1x16.3…paired | sat | Bitwuzla, STP-Parti |
| laby_18_18_04.lp | sat | STP-Parti |
| 140.smt2 (bb) | sat | Bitwuzla, STP-Parti |
| 176.smt2 (bb) | sat | Bitwuzla |
| 185, 59, 77, 95, mult_ub_16x16_1.sf, 119/126/135, string*, tcp_full | unknown | nobody |

The `ref solved it? = yes` rows are the sweet spot for the parallelization thesis:
sequential dv-solve times out at 120s, but the instance is provably solvable, and
the parallel-track winner used partitioning (cube-and-conquer) to crack it.

## Recommended next steps

1. **Fix B10 crash** (robustness; soundness-adjacent). Minimize + ASAN backtrace.
2. **1200s re-run on the QF_BV timeout subset** (~9 instances) to see how many
   dv-solve solves with competition-equal time before any parallelism — this
   isolates "needs more time" from "needs partitioning".
3. **Separability probe** on those instances (independent constraint components /
   cube structure) → prototype partition-and-conquer, re-score here.
4. (Separate track) array/UF support would unlock ~47 benchmarks but is a feature
   effort, not a parallelism one.
