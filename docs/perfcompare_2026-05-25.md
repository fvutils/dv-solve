# Perf comparison: dv-solve (bitblast) vs z3 vs bitwuzla — 2026-05-25

Snapshot of cross-solver performance after Phase B.0 + A.1/A.2 + bit-fix.

## Setup

- Fixtures: 118 SMT2 files under `tests/formal/smt2/tier{1,2,3}/`.
- Driver: `DV_ENGINE=bitblast python tests/formal/run_compare.py --timeout 30 --solvers dv-solve-smt2,z3,bitwuzla`.
- Timing: per-fixture wall clock reported by each solver harness (`run_smt2_solver` in `tests/formal/harness/_subprocess_solver.py`).
- Raw CSV: `tests/formal/results/perfcompare_2026-05-25.csv`.
- Solvers:
  - `dv-solve-smt2 --engine=bitblast` — current HEAD, built with kissat
    as the SAT backend, bit-fix from variable bounds, bit-blast through
    `zsp_bbsolver`.
  - `z3` — system binary.
  - `bitwuzla` — bundled 0.8.2 from `packages/verilator-bin/bin/bitwuzla`.

## Top-line

| Solver         | sat | unsat | unknown/err | total ms |
|----------------|----:|------:|------------:|---------:|
| dv-solve-smt2  | 35  | 83    | 0           |    **714** |
| z3             | 35  | 83    | 0           |    728 |
| bitwuzla       | 35  | 79    | 4           |    398 |

- **dv-solve produces a correct verdict on every one of the 118 fixtures**.
  Bitwuzla returns `unknown` on the four `cache_direct_1way_bmc_d{1,2,4,8}`
  cases (QF_UFBV / array-via-uf dialect).
- 0 disagreements between dv-solve and z3 across all 118 fixtures.
- Aggregate wall time: dv-solve ≈ z3 (within 2%); both ~1.8× slower
  than bitwuzla.

## Per-tier

| Tier | N  | dv ms | z3 ms | bz ms | dv vs z3 | dv vs bitwuzla (matched fixtures) |
|------|---:|------:|------:|------:|---------:|----------------------------------:|
| 1    | 34 |   187 |   281 |   107 |  **0.67×** | 1.75× (N=34 matched) |
| 2    | 60 |   245 |   327 |   229 |  **0.75×** | 1.07× (N=60 matched) |
| 3    | 24 |   282 |   120 |    62 |    2.35× | 2.02× (N=20 matched) |
| all  | 118|   714 |   728 |   398 |    0.98× | 1.79× (N=114 matched) |

- **Tier 1** (mostly satisfiable randomization): dv-solve **beats z3 by
  50% and is within 1.75× of bitwuzla**.
- **Tier 2** (small formal BMC): dv-solve beats z3 by 33% and roughly
  matches bitwuzla.
- **Tier 3** (larger formal BMC, the cases CDCL was timing out on):
  dv-solve is now correct and bounded, but ~2.35× slower than z3 and
  ~2× slower than bitwuzla. The bit-blast path retired every previous
  timeout, but the per-fixture wall time on these BMC cases is where the
  remaining headroom is.

## SAT vs UNSAT

| Result | N  | dv ms | z3 ms | bz ms | dv/z3 | dv/bz |
|--------|---:|------:|------:|------:|------:|------:|
| sat    | 35 |   188 |   283 |   109 | **0.66×** | 1.72× |
| unsat  | 83 |   526 |   445 |   289 | 1.18× | 1.82× |

dv-solve is **strongest on SAT** (model-finding), which lines up with the
toolbox-of-techniques framing in the adoption plan: the bit-blast engine
plus the bit-fix from variable bounds is well-suited to randomization
problems, where the heuristic structure of the search matters more than
proof-search engineering.

dv-solve **loses ground on UNSAT** (formal proof). z3 and bitwuzla both
do substantial preprocessing (normalize, contradicting_ands, embedded
constraints, ...) before bit-blasting. dv-solve's Phase A preprocessing
layer is the obvious next investment.

## Slowest dv-solve fixtures

The 10 fixtures where dv-solve spends the most wall time, with the gap
to z3 and bitwuzla:

| Tier | Fixture                          | dv ms | z3 ms | bz ms  |
|-----:|----------------------------------|------:|------:|-------:|
| 1    | memmaptight32                    |   40  |  39   |  11    |
| 3    | cache_direct_1way_bmc_d8         |   27  |   6   |  *unk* |
| 3    | fifo_8x16_bmc_d8                 |   23  |  10   |   5    |
| 3    | cache_direct_1way_bmc_d4         |   22  |   5   |  *unk* |
| 2    | fifo_ptr_valid_bmc_d16           |   20  |  24   |  10    |
| 3    | regfile_simple_bmc_d8            |   19  |   6   |   3    |
| 3    | regfile_addr_alias_bmc_d8        |   19  |   5   |   3    |
| 3    | fifo_8x16_bmc_d4                 |   18  |   5   |   3    |
| 3    | cache_direct_1way_bmc_d2         |   18  |   5   |  *unk* |
| 3    | regfile_simple_bmc_d4            |   16  |   5   |   2    |

9 of the slowest 10 are tier-3 BMC unsat cases. These are exactly the
fixtures where preprocessing matters most — they have large redundant
sub-structures that normalize / variable_substitution would collapse
before bit-blasting. The current dv-solve path bit-blasts the whole
thing as written and asks kissat to figure it out.

## Where the headroom is

In rough priority:

1. **Phase A.3 rewriter** (no-op simplifications: ITE-of-constants,
   bvand/bvor with 0/ones, double-negation, ...). Cheapest wins. The
   AIG rewriter already catches some of this *during* bit-blasting,
   but pre-AIG rewriting at the IR level can collapse whole subtrees.
2. **Phase A.4 preprocessing passes**: `variable_substitution` is the
   largest win on BMC fixtures (1391 lines in bitwuzla but the BV-affine
   subset is much smaller). `embedded_constraints` and
   `contradicting_ands` are smaller but high-leverage.
3. **Eager domain propagation through expressions**: extend the bit-fix
   pass to track domains through the IR walk, not just at variable
   creation. Knowing `(bvand x 0xff)` has top 24 bits fixed-0 at the
   *result* gives downstream operations more constants to fold.
4. **Phase B.1 kissat fork** + dv-solve-native CDCL — the eventual goal
   per the plan. Will retire the remaining gap to bitwuzla once
   preprocessing is in place.

## What this confirms

- The bit-blast path is **sound** (0 disagreements across 118 fixtures
  vs z3) and **complete** in the practical sense (every fixture
  retired, no timeouts).
- dv-solve's randomization profile (tier 1) is genuinely competitive
  with z3 and approaching bitwuzla.
- The "tier-3 timeouts" regression that motivated Phase B.0 is fully
  resolved: 9 of the 24 tier-3 fixtures were hitting the 10s CDCL
  ceiling before; now every tier-3 fixture finishes in under 30ms.
