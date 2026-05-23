# Formal benchmark comparison — 2026-05-23

Two tracks, since boolector can only consume pure QF_BV (the
yosys-smtbmc output dialect uses uninterpreted state sorts that
boolector rejects).

| Track | Corpus | Dialect | Solvers compared |
|-------|--------|---------|-------------------|
| A | tier1 (34) | pure QF_BV (random emitter) | dv-solve, z3, boolector |
| B | tier2 + tier3 (84) | yosys-smtbmc QF_UFBV | dv-solve, z3 |

Driver: `python tests/formal/run_compare.py --tier {1,2,3} --timeout S`.
Raw rows in `compare_tier1.csv` and `compare.csv`.

---

## Track A — tier1, dv-solve vs z3 vs boolector (timeout 30s)

| Solver         | sat | unsat | timeout | unknown/err | Total ms |
|----------------|-----|-------|---------|-------------|----------|
| dv-solve-smt2  |  27 |     5 |       0 |           2 |      182 |
| z3             |  33 |     1 |       0 |           0 |      280 |
| boolector      |  33 |     1 |       0 |           0 |      340 |

**Five disagreements — all dv-solve false-UNSAT.** z3 and boolector
both return `sat` on these; dv-solve says `unsat`. Model validation
(`DV_VALIDATE_MODEL=2`) cannot catch false-unsat (no model to
re-evaluate), so this is undetected by the existing cross-check.

| Fixture        | dv-solve | z3   | boolector |
|----------------|----------|------|-----------|
| memmaptight32  | unsat ✗  | sat  | sat       |
| shiftaligned   | unsat ✗  | sat  | sat       |
| sumpartition   | unsat ✗  | sat  | sat       |
| unique16       | unsat ✗  | sat  | sat       |
| verilatorops   | unsat ✗  | sat  | sat       |

Honest non-answers (excluded from disagreement count):
- `arrayordering` — dv-solve `unknown`, both others `unsat` (known
  emitter overflow per README).
- `muldivscenario` — dv-solve `unknown`, others `sat`.

### Root cause: CDCL explain, not propagation

`DV_USE_LCG=0 build/dv-solve-smt2 <minimal-repro>` returns `sat`
(correct); the default `DV_USE_LCG=1` returns `unsat`. The bvand
*propagator* is sound — the *explain* callback that backs it
(`_explain_binary_bitwise` in `src/c/zsp_explain.c:464`) is the
suspect. It records the *current* `var_lo64/var_hi64` of every
watched var and int32-casts the bounds; a wider-than-int32 value or a
post-propagation bound snapshot can yield an unsound learnt clause.
This callback backs `bvand`, `bvor`, `bvxor` explain.

Disabling CDCL fixes tier1 but is not a real fix — the previous
session needed CDCL to land tier2/tier3 (68/16 unsolved without it).

### Minimal repro for the bvand pattern

Four of the five (everything except `unique16`) reduce to the same
shape: integer-range bound + `(zero_extend N) X = bv-op …` + tight
upper bound on `X`. Reduced from `verilatorops` to 7 lines:

```smt2
(set-logic QF_BV)
(declare-const a (_ BitVec 8))
(declare-const masked (_ BitVec 4))
(assert (bvuge a (_ bv10 8)))
(assert (bvule a (_ bv200 8)))
(assert (= ((_ zero_extend 4) masked) (bvand a (_ bv15 8))))
(assert (bvule masked (_ bv5 4)))
(check-sat)  ; dv-solve: unsat  — z3: sat (a=16, masked=0 satisfies)
```

Bisection: dropping any one of {bvuge bound on `a`, bvule bound on
`a`, bvule bound on `masked`} flips dv-solve back to `sat`. The bug
sits at the intersection of bvand bit-level reasoning and
integer-domain range tightening on a wider operand.

`unique16` is a separate failure: no `zero_extend`, just `(distinct
vi vj)` pairwise over 16 vars in a range — likely an unsoundness in
alldifferent / pairwise-distinct propagation under a range setup.

### Performance on tier1

Boolector wins absolute time on small fixtures (1–3 ms per call, vs.
4–14 ms for the others). dv-solve is faster than both on
mid-size fixtures where the constraints exercise it well
(`socaddrmap40` 6 vs 14 vs 56 ms, `nqueens8` 6 vs 8 vs 11 ms,
`unique32` 5 vs 4 vs 1 ms). Process startup still dominates at
this scale.

---

## Track B — tier2 + tier3, dv-solve vs z3 (timeout 15s)

84 yosys-smtbmc-dialect fixtures, BMC + k-induction at various depths.

| Solver         | sat | unsat | timeout | unknown/err | Total ms |
|----------------|-----|-------|---------|-------------|----------|
| dv-solve-smt2  |   2 |    82 |       0 |           0 |      498 |
| z3             |   2 |    82 |       0 |           0 |      471 |

**Zero disagreements.** Per-fixture times sit between 4 and 25 ms;
subprocess startup dominates real search time. Real solver work is
confirmed via `DV_LCG_STATS=1` — e.g., 46 CDCL conflicts on
`cache_direct_1way_bmc_d8`.

| Fixture                       | dv-solve | z3   | winner    |
|-------------------------------|---------:|-----:|-----------|
| fifo_ptr_valid_bmc_d16        |        7 |   25 | dv-solve  |
| arbiter_fairness_bmc_d16      |       12 |   20 | dv-solve  |
| arbiter_fairness_bmc_d8       |        7 |   12 | dv-solve  |
| fifo_8x16_bmc_d8              |        5 |   10 | dv-solve  |
| cache_direct_1way_bmc_d8      |       21 |    6 | z3 (3.5×) |

`cache_direct_1way_bmc_d8` is the only fixture where dv-solve runs
clearly slower than z3, and the prior session needed
`PROP_SLACK_FACTOR=32` just to land it under the 10s budget — it's
still the CDCL path's hardest workload in the suite.

---

## Anomaly / sanity checks performed

1. **dv-solve isn't short-circuiting.** `DV_LCG_STATS=1` confirms
   non-trivial conflict / learnt-clause counts on the harder fixtures.
2. **SAT answers are model-checked.** Default `DV_VALIDATE_MODEL=2`
   re-evaluates every top-level constraint under the returned
   assignment and downgrades to `unknown` on violation. This is why
   false-UNSAT is the failure mode showing up here, not false-SAT —
   the validator silently catches and downgrades false-SAT, but
   cannot guard the unsat path.
3. **Cross-solver oracle.** Pure-QF_BV fixtures get *two* independent
   oracles (z3 + boolector); they agree on all 5 disputed cases, so
   the bug is in dv-solve, not in z3.

## Gaps

- **No end-to-end SBY integration.** dv-solve has not been wired in
  as a yosys-smtbmc backend; the existing
  `tests/formal/sby/counter_assert.sby` still uses the `boolector`
  engine. Plumbing dv-solve into sby would be the path to running it
  on real verification flows.
- **bitwuzla / cvc5 / yices not installed locally.** Only z3 +
  boolector available; ideally we'd have at least one more oracle on
  Track A.
- **No stress fixtures.** Both corpora top out around 25 ms per
  fixture; meaningful throughput comparison would need much harder
  inputs.

## Recommended next step

Fix the false-UNSAT bug first — Track B's clean 84/84 agreement is
misleading until Track A's 5 disagreements are resolved. The
7-line bvand minimal repro should be checked in as a regression test
alongside `test_cross_check.py`, and the cross-check itself extended
to cover tier1 so this kind of bug can't ship undetected again.

Artifacts:
- `tests/formal/results/compare.csv` — tier2+tier3 raw rows
- `tests/formal/results/compare_tier1.csv` — tier1 raw rows
- This file.
