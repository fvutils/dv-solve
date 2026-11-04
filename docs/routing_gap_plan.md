# Plan: close the routing gap the best-case comparison found

**Origin:** the expected-cost work (`docs/solve_stats_capture_plan.md`, S5a)
runs every corpus item under every arm. On the first run (2026-11-01, commit
3fca456), the corpus took 21.8 s on dv-solve's default route, **6.7 s if each
item had used our own best engine**, and 2.9 s with the best arm of any
solver. In 47 items one of our own forced engines beat the default route by
4x or more. The largest checked case: `fletcher16_fold_bytes2` is sent to
bitblast by the `multiplier` rule (0.61 s) where CDCL takes 0.01 s.

This plan closes the part of the gap that is ours alone: choosing among our
own engines.

## Ground rules

- **Soundness first.** Both engines are exact, so a route change cannot change
  a verdict, but each change still runs the QF_BV fuzzer and the whole
  `env -u LD_LIBRARY_PATH pytest -q`.
- **Deterministic routing** (`docs/deterministic_routing_plan.md`): rules use
  the problem's shape and deterministic work counts, never wall time.
- **API/SMT2 parity:** routing belongs in the core (`dvs_shape_route`); a rule
  changed for SMT2 applies to every path that uses it.
- **Seeded sampling is not only speed.** The `seeded sample` rule sends seeded
  problems to bitblast for distribution quality (fw-hdl piece 6.4). A change
  there must keep the uniformity harness passing, not just get faster.
- **Measure with the collector** (`tests/perf/perfmodel_corpus.py`): the same
  items, before and after; a fix counts only if the gap closes on the items
  it targets and no other item gets slower. Training runs are kept in
  `perf-out/perfmodel/` (local, not in git): the session scratch directory
  is cleared between sessions.
- One commit per step, when asked.

## Steps

| # | Step | Status |
|---|---|---|
| R0 | Attribute the gap: per routing rule, the items it misroutes and the time lost | done |
| R1 | Cap the bit-level engine's pre-SAT samplers by evaluator node visits | done: corpus 21.3 s -> 10.0 s on the default route |
| R2 | The remaining misroutes: sampler budgets in visits, sweep before the table on small cones | done: corpus 10.0 s -> 6.9 s on the default route |
| R3 | Re-fit the expected-cost model; record the corpus totals before and after | open |

## Log

- R0 (2026-11-02): corpus re-collected into `perf-out/perfmodel/` after the
  first run was lost with the scratch directory. Tool: `tests/perf/route_gaps.py`
  (per routing reason: items, misrouted = an own engine >= 2x faster, time
  lost). 613 SMT2 items: 21.3 s default, 6.5 s on our own best engine; 14.6 s
  lost to misroutes: `bit-level` 36 items 7.9 s, `multiplier` 19 items 5.8 s,
  `logic` 39 items 0.8 s. The faster engine was mostly **cube**, and cube won
  on CPU time too (crc8 0.6 s CPU vs 4.55 s), so it was less work, not more
  cores.
  - **The cause is not the routing rules but the bit-level engine's pre-SAT
    samplers.** By answer path, the misrouted items lost 5.7 s via
    `bb-affine`, 4.9 s via `bb-divmul`, 1.8 s via `bb-sim-exhaustive`.
    Profiles (crc8, secded, fletcher16): 95-99% in `bb_eval`, the expression
    evaluator, called from the enumeration-table build (`_en_build`) and the
    divmul sampler (`_divsamp`). Their budgets count **evaluations**
    (`DVS_EN_MAX_NODES` 2^19, `DVS_SS_WORK` 2^20) whatever a leaf's size, and
    a leaf that is a circuit visits thousands of nodes per evaluation: crc8's
    failed table build visited 1.19e9 nodes (4.5 s) before declining; SAT
    then answered in milliseconds. Failed table builds alone were 8.2 s of the
    13.7 s the misrouted items lost.
  - The `multiplier` rule also counts division and remainder: fletcher16 has
    no multiply, only `bvurem x 255`. Not changed: the rule is not where the
    time went.
  - Side finding: cube records zero work in the stats (its SAT runs on worker
    threads; an open S0 item), and the samplers' work was undercounted the
    same way (0.22 s recorded as 22k units).
- R1 (2026-11-02): `S->eval_steps` counts the nodes `bb_eval` visits.
  - `_en_build` stops past 2^24 visits, or 2^22 with no model found (the one
    table the SMT2 corpus builds takes 8.5e6), and records visits as its work.
  - Every pre-SAT sampler (pop, unique, sorted, divmul, enum) runs under
    `_capped`: past 2^25 visits `bb_eval` declines, which every sampler
    already reads as "leave it to SAT". Calibrated on 2,987 sampler calls
    (SMT2 corpus and the sampling tests): successes visited at most 1.25e7.
  - Soundness: the samplers only ever return SAT or unknown (checked: no
    sampler returns UNSAT), and a capped evaluation is a failure every
    sampler handles as a decline, so the cap cannot change a verdict.
  - Times: crc8 4.6 s -> 0.02 s, secded_data4 0.22 -> 0.02 s,
    fletcher16_collision_bytes8 0.67 -> 0.07 s, fletcher16 folds 0.62 ->
    0.19 s, clz_w16 0.15 -> 0.09 s; verdicts unchanged.
  - Tests: ctest 34/34; uniformity harness, bvsat sampling/fuzz, enum tier,
    Verilator-mode sampling and the QF_BV fuzzer: 486 passed. Full pytest:
    35 failures; rerun on the same build without R1, 25 fail too
    (pre-existing) and the other 10 pass in isolation with R1 as well (the
    order-dependent e2e/partitioner tests and the whole-run coverage checks).
  - **Re-measured** (`perf-out/perfmodel/train-r1.json`): the 613 SMT2 items
    take **10.0 s** on the default route (21.3 s before), 6.4 s on our own
    best engine; the loss to misroutes fell from 14.6 s to **3.3 s**:
    `multiplier` 14 items 1.6 s (fletcher16 folds 200 ms vs cube 8 ms, CDCL
    10 ms), `bit-level` 34 items 1.0 s (cube faster on 28).
- R2 (2026-11-02): the remaining misroutes were not the routing rules either.
  Traced per sampler (visits per pre-SAT sampler, `DV_LOG`, `DV_BB_STATS`):
  - **fletcher16 folds** (`multiplier`, 200 ms): the divmul sampler's domain
    enumeration of one 16-bit free var. Its budget, `DVS_SS_WORK`, counts
    values x leaves (131k, under 2^20), but each value visits 13k nodes, so it
    ran into the 2^25 sampler cap. Now the enumeration extrapolates its visits
    from the share of the hull passed and declines past `DVS_DS_DOM_STEPS`
    (2^24) after 64 values. The table build extrapolates the same way past
    2^20 visits (`DVS_EN_PROBE_STEPS`), declining a build bound to pass
    2x `DVS_EN_MAX_STEPS`.
  - **clz_w16, gray_early_stop_w16** (`bit-level`, 96-128 ms): the
    enumeration table build over a 16-bit cone found no model by 2^22 visits;
    the exhaustive sweep then decided it in 0.15 ms. The sweep already
    reservoir-samples uniformly over every satisfying assignment when seeded,
    so on the one-shot path the evaluator-based table is skipped when the
    sweep fits `DV_SIM_MAX_WORK` (`_sweep_small`). The reused-context draw
    keeps its table (built once, it serves every later draw).
  - **Empty tables** (`logic`, e.g. array_symbolic_index 37 ms): traced over
    the corpus, 255 of 259 table builds found no model and spent 1.4e8
    visits; where a model was found, the first came by 255k visits (2,004 in
    the C tests). `DVS_EN_EMPTY_STEPS` 2^22 -> 2^20.
  - All three only decline sooner (a sampler decline hands the problem to
    the sweep/SAT path), so verdicts cannot change. fletcher16 fold, clz_w16,
    gray_early_stop_w16: 0.13-0.19 s -> under 10 ms each.
  - Tests: ctest 34/34, the 486 sampling/uniformity/fuzz tests pass; full
    pytest: the same 35 failures as R1 (25 pre-existing, 10 order-dependent),
    none new.
  - **Re-measured** (`perf-out/perfmodel/train-r2.json`,
    `route_gaps-r2.txt`): the 613 SMT2 items take **6.9 s** on the default
    route (10.0 s after R1, 21.3 s before), 6.0 s on our own best engine; the
    loss to misroutes fell from 3.3 s to **0.7 s** (`bit-level` 13 items
    0.38 s, `logic` 29 items 0.16 s; `multiplier` none). Items 4x or more over
    the best arm of any solver: 110 -> 47. No item got slower (one flagged at
    14 ms re-times under 10 ms).
  - Left (0.7 s, not worth a rule change yet): clz_nz_w16 ~42 ms vs CDCL
    2 ms, parity_w16 ~35-47 ms vs cube; array_symbolic_index ~19 ms vs
    CDCL 2 ms.
