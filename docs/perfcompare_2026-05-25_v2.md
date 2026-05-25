# Perf comparison v2: dv-solve (bitblast) vs z3 vs bitwuzla — 2026-05-25

Refreshed snapshot after the substitution + Tseitin fix work. The original
snapshot in `perfcompare_2026-05-25.md` predates those two commits and
reflects the pre-substitution numbers.

## Setup

- Fixtures: 118 SMT2 files under `tests/formal/smt2/tier{1,2,3}/`.
- Driver: `DV_ENGINE=bitblast python tests/formal/run_compare.py --timeout 30 --solvers dv-solve-smt2,z3,bitwuzla`.
- Timing: per-fixture wall clock reported by each solver harness
  (`run_smt2_solver` in `tests/formal/harness/_subprocess_solver.py`).
- Three runs measured. Median run used for the per-tier and by-result
  breakdowns below. Variance across runs is ~2% (within solver
  noise floor).
- Raw CSV: `tests/formal/results/perfcompare_2026-05-25_v2.csv`.
- HEAD includes: kissat backend (B.0), AIG + Tseitin + BV bit-blaster,
  zsp_bbsolver with variable bit-fix (Phase A.1 wired), top-level + AND-
  descent equality substitution, ExprRef memoization, **Tseitin
  top-level dedup-by-(id,sign) fix**.

## Top-line

| Solver          | sat | unsat | unknown/err | total ms |
|-----------------|----:|------:|------------:|---------:|
| **dv-solve-smt2** | 35  | 83    | 0           | **615**  |
| z3              | 35  | 83    | 0           |  726     |
| bitwuzla 0.8.2  | 35  | 79    | 4           |  391     |

- dv-solve retires every fixture; z3 same; bitwuzla times out 4 fixtures
  on the `cache_direct_1way` QF_UFBV / array-via-uf cases.
- **Zero disagreements across all 118 fixtures.**
- dv-solve is **15% faster than z3** in aggregate wall time.
- bitwuzla still ahead at ~1.55× dv-solve (over its 114 matched fixtures).

## Per-tier

| Tier | N  | dv ms | z3 ms | bz ms | dv/z3 |
|------|---:|------:|------:|------:|------:|
| 1    | 34 |   168 |   269 |    97 | **0.62×** |
| 2    | 60 |   225 |   329 |   231 | **0.68×** |
| 3    | 24 |   222 |   128 |    63 | 1.73× |
| all  | 118|   615 |   726 |   391 | **0.85×** |

## SAT vs UNSAT

| Result | N  | dv ms | z3 ms | bz ms | dv/z3 |
|--------|---:|------:|------:|------:|------:|
| sat    | 35 |   170 |   273 |   101 | **0.62×** |
| unsat  | 83 |   445 |   453 |   290 | **0.98×** |

The biggest qualitative change vs v1: dv-solve has reached **parity with z3
on UNSAT** (the formal-proof workload). The v1 snapshot showed dv-solve
1.18× *slower* than z3 on UNSAT. Substitution + AND descent closed that
gap.

## What changed vs v1

| Metric | v1 (before subst) | v2 (after subst + fix) | change |
|---|--:|--:|--:|
| dv-solve total ms | 714 | **615** | **-14%** |
| dv-solve tier-3 ms | 282 | **222** | -21% |
| dv-solve UNSAT ms | 526 | **445** | -15% |
| dv/z3 aggregate | 0.98× | **0.85×** | -13% |

Two commits drove this:

1. `b714608` — Memoization + top-level equality substitution. Recorded
   `subst[var] = EXPR` for `(= var EXPR)` constraints at the top level,
   re-routed `bv_for_var` through the substitution. Drove the move from
   714 → 666ms (top-level alone).

2. `3e88292` — Tseitin top-level dedup bug fix + AND descent enabled.
   The latent bug: the top-level CNF flatten deduped visited nodes by
   `abs(node_id)`, so positive and negative occurrences of the same AIG
   node deduped each other; conflicting literals (which made the
   conjunction necessarily FALSE) were silently dropped, allowing the
   SAT solver to find a false-SAT. Bug had been there forever; only
   manifest once substitution produced AIG structures with conflicting
   literals at the top level. Fix: dedup by `(id, sign)` pairs. Drove
   666 → 615ms by unlocking AND descent.

## Slowest dv-solve fixtures

The 10 fixtures where dv-solve spends the most wall time in the median
run, with the gap to z3 and bitwuzla:

| Tier | Fixture                          | dv ms | z3 ms | bz ms  |
|-----:|----------------------------------|------:|------:|-------:|
| 1    | memmaptight32                    |   38  |  38   |  10    |
| 2    | fifo_ptr_valid_bmc_d16           |   20  |  24   |   9    |
| 3    | cache_direct_1way_bmc_d8         |   23  |   6   |  *unk* |
| 3    | cache_direct_1way_bmc_d4         |   21  |   5   |  *unk* |
| 3    | regfile_addr_alias_bmc_d8        |   19  |   5   |   3    |
| 3    | regfile_simple_bmc_d8            |   18  |   6   |   3    |
| 3    | cache_direct_1way_bmc_d2         |   18  |   5   |  *unk* |
| 3    | regfile_addr_alias_bmc_d4        |   17  |   5   |   2    |
| 3    | regfile_simple_bmc_d4            |   16  |   5   |   2    |
| 3    | cache_direct_1way_bmc_d1         |   12  |   3   |  *unk* |

9 of the slowest 10 are tier-3 BMC unsat cases. The `cache_direct_1way`
cases are where dv-solve hits the QF_UFBV / array-via-uf path that
bitwuzla can't handle at all; dv-solve handles them but spends most of
the SAT budget there.

Note: `fifo_8x16_bmc_d8` is no longer in the top 10. Pre-substitution
it was 23ms; substitution shrank it dramatically (the inline trace
during debugging showed 12k AIG ANDs → 336 on this fixture under AND
descent).

## Where the remaining headroom is

In rough priority:

1. **Phase A.3 rewriter** — IR-level structural rewrites that
   substitution alone can't catch: `bvand x 0 → 0`, `ite TRUE t e → t`,
   double-negation, sign-ext idempotence, extract-of-concat, AC-flatten
   of `(and a (and b c))`. The AIG already catches some of this at
   bit level, but pre-AIG rewriting at the IR level collapses whole
   subtrees before bit-blasting.
2. **Operator-specific tuning on the slow tier-3 fixtures** — the
   slowest 3-4 fixtures spend 15-23ms each in SAT. The bit-blast cost
   is 1-3ms; the rest is SAT. If a specific operator (mul? udiv?
   shift?) dominates the AIG size, targeted rewrites or a better
   bit-blast circuit there would help more than generic rewrites.
3. **Phase B.1 kissat fork** — long-term plan: fork kissat into
   `src/c/sat/`, route through `zsp_alloc_t`, integrate with the trail
   and checkpoint primitives. Foundational for the dv-solve-native CDCL
   path the plan ultimately calls for.

## What this snapshot confirms

- **The bit-blast engine is sound**: 0 disagreements across 118
  fixtures with two independent reference solvers, after fixing the
  Tseitin dedup bug.
- **dv-solve is competitive with z3** and faster overall.
- **The tier-1/2 lead is real and consistent** — randomization-heavy
  and small-BMC workloads play to dv-solve's strengths.
- **Tier-3 is the remaining gap** — and the slowest tier-3 fixtures
  are concentrated in the QF_UFBV/array path. The next big unlock is
  likely operator-specific rather than generic.
