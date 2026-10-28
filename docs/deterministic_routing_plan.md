# Deterministic engine routing (no wall-clock decisions)

Decision (2026-10-28): which engine answers a check-sat must not depend on
wall-clock time. Both engines are exact, so verdicts never depended on it,
but the *model* did: on a loaded machine the same seed gave different
models, and CDCL's near-uniform sampling was swapped for bitblast's skewed
one. Found as a flaky `tests/unit/test_enum_reuse.py` on GitHub
linux-x86_64 (one 16 ms preemption locked the shape to bitblast; reproduced
6/6 with `taskset -c 0` plus busy loops).

## Sites that routed on time

| # | Where | What | Now |
|---|---|---|---|
| A | `smt2_frontend.c` `_shape_record` / `_shape_wants_bitblast` / `_cmd_check_sat` | Verilator shape learning: CDCL avg > 3 ms → 2 bitblast trials → keep bitblast if ≥2× cheaper, sticky across `(reset)` | estimated cost from work counts (`_check_sat_cost_ns`) |
| B | `smt2_frontend.c` CDCL probe (`opts.time_limit_ms = 50`) | Verilator CDCL-viability probe; running out sets `cdcl_route = 2` (sticky) | work budget `SMT2_PROBE_WORK` |
| C | `smt2_frontend.c` one-shot QF_BV (`DV_QFBV_CDCL_MS`, 20 ms) | CDCL gets a short budget, then bitblast | work budget `SMT2_QFBV_CDCL_WORK` |

## Mechanism

- **Core work budget.** `dvs_ctx_t.work_budget` (internal; no public API or
  ABI change): the caller sets it before one `dvs_solver_solve`, which
  consumes and clears it. Work is `dvs_ctx_work(ctx)` = propagator firings +
  `DVS_WORK_PER_CONFLICT` (1300) × conflicts. Checked where the wall-clock
  deadline is checked (`_prop_over_budget` every 1024 firings, the decision
  loop and the conflict loop); running out returns a timeout with bail
  reason `DVS_BAIL_WORK` ("work budget for this solve"). Like an explicit
  `time_limit_ms`, a work-budget solve is not thrash-capped and its timeout
  does not mark the problem thrashed. The fields sit at the end of
  `dvs_ctx_t`: placed next to the thrash fields they cost ~13% on a
  CDCL-bound bench through layout alone.
- **Bit-blast work counts.** `dvs_bbsolver_work()` reports clauses encoded,
  SAT work (`dvs_sat_work`: kissat `search_ticks` via a new fork accessor
  `kissat_search_ticks`; CaDiCaL has no statistics in its C API, so its
  backend always installs a terminate hook and counts the polls, which
  CaDiCaL makes at fixed intervals of its own search steps, forwarding to a
  caller's hook), and AIG node visits outside SAT (`aux_work`: the diversity
  pass, pre-solve simulation). The frontend keeps the counts of freed
  solvers (`bb_work_retired`) so a check-sat's delta survives `_free_bb`.
- **Shape cost (A).** Per check-sat, in estimated ns:
  `19·fires + 24730·conflicts + 14·psz` (if CDCL ran) +
  `15·clauses_k + 8·ticks_k + 160·clauses_c + 443·polls_c + 27·aig_visits
  + 3·psz` (if bit-blast ran), `psz` = problem pool bytes. The rule is
  unchanged otherwise: slow past 3 ms average, keep bitblast if 2× cheaper.
  Only the weights' ratios matter, so a faster or slower machine routes the
  same way.
- **Budgets (B, C).** `SMT2_WORK_PER_MS` = 52 600 units (1 ms at 19 ns per
  firing); `SMT2_PROBE_WORK` = 2 000 000 (≈ the old 50 ms: crc_filter, the
  bench CDCL declines, used 2.04 M units in its 53.7 ms probe);
  `SMT2_QFBV_CDCL_WORK` = 1 050 000 (≈ 20 ms). `DV_QFBV_CDCL_WORK` sets C in
  units; `DV_QFBV_CDCL_MS` still works, converted at `SMT2_WORK_PER_MS`.
- The 10 s wall-clock CDCL default stays, only as a hang guard.

Knobs: `DV_VLT_SHAPE=0` turns shape routing off (calibration, A/B);
`DV_VLT_STALL_US=N` stalls every Verilator-mode check-sat N µs, as a loaded
machine would (test hook). `DV_LOG`'s `vlt-time` line now carries the
counts (`conflicts=` per solve, `ticks=`, `bbclk= bbkw= bbclc= bbcw= bbax=
psz=`) and the estimate (`cost=` µs).

## Calibration (reference machine, 2026-10-28)

`tests/perf/route_calib.py` replays 20 Verilator-shaped workloads
(riscv_illegal_instr, riscv_loop_instr's divisibility, the 18
`tests/perf/rand_benches.py` benches through the Verilator 5.046 protocol),
20 randomize() calls each, with each engine forced (`DV_VLT_SHAPE=0`;
`DV_ENGINE=bitblast` on kissat and on CaDiCaL), takes the fastest of 3 runs
per check-sat, and fits µs per count by non-negative least squares on
absolute error (the decisions are all made in the milliseconds range).
Raw fit: CDCL 0.01908 µs/firing, 24.73 µs/conflict, 0.01362 µs/byte;
bit-blast 0.01478 µs/kissat clause, 0.008315 µs/kissat tick, 0.1602
µs/CaDiCaL clause, 0.4431 µs/CaDiCaL poll, 0.02675 µs/AIG visit, 0.003335
µs/byte. Medians (µs, measured / estimated):

| workload | CDCL | bitblast (kissat) | route |
|---|---|---|---|
| illegal_instr | 5494 / 4869 | 1560 / 1990 | bitblast after 2 + 2 (as before) |
| divisibility | 50 / 167 | 6199 / 5202 | CDCL (as before) |
| linear_sys | 1284 / 483 | 321 / 336 | CDCL (as before) |
| sum_unique | 534 / 351 | 430 / 462 | CDCL |
| other 16 benches | 6–113 / 6–360 | 10–906 | CDCL |

Instance router (B): crc_filter is still declined on its first probe;
illegal_instr's probes (110 k–965 k units) still pass. One-shot (C): over 502
fixtures (tier1, tier2, fw_hdl, usecase, gaps) the same 7 escalate to
bitblast as with the 20 ms budget, and no verdict changes.

## Verification

- Repro under load (`taskset -c 0` + 6 busy loops on core 0):
  `test_enum_reuse::test_hash_asserts_draw_from_kept_list` fails 6/6 on
  origin/main, passes 6/6 here.
- `tests/unit/test_vlt_route_determinism.py`: 5 workloads replayed fast and
  with every check-sat stalled 20 ms; routes and stdout identical. With the
  shape cost swapped back to measured time it fails 4 of 5.
- Full `pytest tests/unit tests/formal` (minus e2e/partitioner): same 8
  failures as origin/main (3 simbench, test_verilator_guide_unknown, 4 tier3
  bitwuzla-reference unknowns), +6 new passes. ctest 31/31.
- Perf (Verilator protocol, 18 benches × 300 randomize(), min of 3 CPU):
  total 1.008× origin/main; models identical on all 18. crc_filter, the
  only bench whose probe runs out, +1.5%. One-shot (476 tier1/tier2/fw_hdl
  fixtures, min of 3 CPU): 1.003×.

## Follow-ups (not in this change)

- **Bit-blast simulation race** (`dvs_bbsolver.c`, presim): exhaustive
  simulation runs in a thread raced against SAT, and the winner's model is
  returned, so which model comes back is timing-dependent there too.
- Other wall-clock budgets that change *which* answer comes back, only in
  pathological cases: the in-propagation deadline (B61), the 10 s CDCL
  default escalating to bitblast, `SMT2_CORE_BUDGET_MS` (unsat-core size),
  `dvs_contradiction.c` diagnostics budgets, `dvs_search.c` 5000 ms default.
- `test_stack_recursion` fails on origin/main before reaching most files
  (`dvs_build_info.c` needs a generated header), so it did not check this
  change.

## Status

- 2026-10-28: plan written.
- 2026-10-28: A, B, C implemented on `feat/deterministic-routing`,
  calibrated, verified (above).
- 2026-10-28: merged to main as 48ceaf1.
