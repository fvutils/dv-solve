# simbench per-randomize stage breakdown (2026-10-28)

Goal: find where per-randomize time goes in dv-solve for the simbench
workloads, as input to a ~10x speedup plan. Context: the 2026-10-27 replay
report kept only totals
(dvs-standalone median 168 us/call).

## Setup

- dv-solve: main @ 4e55a4b, `build/` rebuilt (Release, -O3, no -g) before the run.
- Verilator: `~/projects/verilator/verilator-sampler-control/bin/verilator`,
  branch sampler-control, git 880ec8843 ("5.053 devel rev v5.052-610-g880ec8843").
  (`packages/verilator` is 5.052 without sampler control, so the
  `vlt-dvs-solver`/`dvs-standalone` configs cannot use it.)
- Capture: simbench config `dvs-standalone` (= `vlt-dvs-solver` capture with
  `+verilator+solver+file+`, then `build/bench_vlt_replay --passes api,api-cache --reps 3`),
  `SIMBENCH_DVS_SMT2=build/dv-solve-smt2`, `SIMBENCH_DVS_REPLAY=build/bench_vlt_replay`,
  N=200, seed=1 (10 warm-up calls + 1 start-up check => 211 check-sats).
- Host: AMD Ryzen 9 9950X, 32 threads, Linux 6.8, loadavg ~4.
- Profiles: `perf record --call-graph dwarf` on `build/bench_vlt_replay` (symbols
  only, no debug info), one pass type per run, reps chosen so the timed pass dominates.

## Summary table (us per check-sat; 1.0 check-sat per randomize except dist_enum = 2.0)

Engine = what the front end (verilator_mode=1, as in the replay text pass) answered on.
Split = build / compile / solve+read. For bitblast, "compile" is only
`dvs_bbsolver_new`; encoding + SAT + diversify are counted in solve.
No bench had escalations, bitblast failures, unsolved calls or unsat calls.

| bench | engine | api | api split | api-cache | api-cache split | distinct / reused | reused ctx vs new ctx (new = only 2 samples) |
|---|---|---:|---|---:|---|---|---|
| construct.inside_set | bitblast 208/211 | 77.7 | 1.5 / 34.4 / 41.8 | 27.7 | 1.0 / 17.0 / 9.8 | 2 / 209 | 9.1 vs 76.7 |
| construct.range | CDCL | 57.9 | 1.3 / 5.4 / 51.2 | 16.5 | 1.2 / 0.2 / 15.1 | 2 / 209 | 15.0 vs 26.2 |
| control.ite_cond | CDCL | 71.3 | 1.0 / 3.7 / 66.5 | 6.2 | 0.7 / 0.2 / 5.3 | 2 / 209 | 5.0 vs 32.2 |
| scenario.pcie_tlp | CDCL | 57.3 | 1.9 / 8.4 / 47.0 | 17.5 | 1.7 / 0.3 / 15.5 | 2 / 209 | 15.4 vs 31.3 |
| construct.packed_struct | CDCL | 36.8 | 1.0 / 5.1 / 30.7 | **144.3** | 1.0 / 0.1 / 143.2 | 2 / 209 | **144.4 vs 9.2** |
| dist.dist_if | CDCL | 704.9 | 3.3 / 9.5 / 692.1 | 718.7 | 3.2 / 11.1 / 704.4 | **211 / 0** | n/a (no hits) |
| arith.countones | bitblast 210/211 | 2976.8 | 17.4 / 5.1 / 2954.2 | 491.0 | 12.4 / 0.1 / 478.5 | 2 / 209 | 468 vs 1568 |
| arith.mul_div_64 | bitblast 210/211 | 15308.7 | 3.9 / 2.6 / 15302 | 10257.1 | 2.3 / 0.1 / 10254.7 | 2 / 209 | 10248 vs 10972 |
| hard.unique_64 | CDCL (2 bitblast) | 11331.7 | 40.3 / 239.5 / 11051.9 | **23711.4** | 38.8 / 13.7 / 23658.8 | 2 / 209 | **23867 vs 1919** |
| array.unique_32 | bitblast 204/211 | 7229.4 | 21.7 / 7.3 / 7200.3 | 329.7 | 15.8 / 1.0 / 312.9 | 2 / 209 | 310 vs 651 |
| dist.dist_enum | - | crash | | crash | | | |

Text pass (in-process SMT-LIB front end, verilator_mode=1), us/check-sat:

| bench | total | lex+parse | assert | check-sat | other |
|---|---:|---:|---:|---:|---:|
| inside_set | 49.7 | 3.2 | 2.8 | 40.7 | 2.7 |
| range | 73.9 | 3.4 | 4.0 | 54.6 | 11.4 |
| ite_cond | 77.2 | 2.1 | 1.6 | 67.9 | 5.5 |
| pcie_tlp | 68.7 | 3.9 | 4.7 | 53.2 | 6.5 |
| packed_struct | 45.6 | 2.7 | 2.8 | 33.9 | 6.0 |
| dist_if | 703.5 | 3.3 | 4.7 | 685.6 | 6.0 |
| countones | 722.5 | 9.4 | 19.8 | 689.9 | 3.0 |
| mul_div_64 | 10154.5 | 4.4 | 4.0 | 10138.0 | 5.9 |
| unique_64 | 11026.4 | 7.9 | 24.1 | 10981.1 | 13.2 |
| unique_32 | 616.9 | 10.4 | 21.7 | 576.4 | 8.3 |

## Session shape (construct.inside_set)

session.cmds.smt2: 2943 commands, 211 check-sats. One start-up
`(set-logic QF_ABV)(check-sat)`, then per randomize exactly 14 commands:
`(reset)`, 2x `set-option` (produce-models, random-seed <new>), `set-logic QF_ABV`,
2x `define-fun` (__Vbv/__Vbool), 3x `declare-fun`, 3x `assert` (identical every call),
1x `check-sat`, 1x `get-value (a b c)`. No push/pop, no XOR-hash asserts
(solver-side sampler: the seed is the only thing that changes). So every
randomize re-sends and re-builds an identical problem.

Variations seen in other benches:
- dist.dist_if: same shape, but the dist bucket choice is folded into the
  assert as random constants (`#x...0b` vs `#x...28`), so every problem is
  byte-distinct => 211 distinct, 0 cache hits.
- dist.dist_enum: base asserts, then `(push 1)`, one bucket-selection assert,
  then TWO consecutive `(check-sat)` (no pop) and `get-value` => 2 check-sats
  per randomize (421 total), no pop before the next `(reset)`.

## Profiles (api-cache pass, self time)

construct.inside_set (27.7 us/call; 2 distinct problems):
dvs_solver_exclude_value 60.9%, _prop_cone 12.2%, main (inlined FNV hash of the
built problem) 7.7%, _diversify_sweeps 5.2%, _check_clause 3.1%, restrict 1.2%,
dvs_builder_alloc 1.0%, _box_try 0.7%, dvs_bbsolver_value 0.5%, kissat_value 0.4%,
memset 0.4%, _pick_avoiding_holes 0.4%, _int_malloc 0.4%, dvs_aig_is_and 0.4%,
_fire_disj_clause 0.3%.
- The 61% is ONE-TIME compile cost: `_solver_compile_body` -> the "disj holes"
  punch-out (dvs_compile.c ~4713-4750) calls `dvs_solver_exclude_value` once per
  gap value. For `b inside {0x100,0x200,0x400,0x800}` that is ~1790 calls, each
  doing `_is_hole` + `_count_holes_in_range` over the hole list (quadratic).
  ~1.7 ms per CDCL compile; with 2-3 CDCL compiles it is 16 us/call amortized
  over 211 calls, and it is what makes the "new context" cost 77 us. It also
  appears in the front end (`_ensure_compiled`).
- Steady state on a reused bitblast context is 9 us: `dvs_bbsolver_rediversify`
  -> `_diversify_sweeps` (rebuilds the L/R/fanout CSR from the AIG every call,
  then `_prop_cone` per candidate bit) ~19% inclusive, build ~1 us, hash ~2 us.

scenario.pcie_tlp (17.5 us/call; CDCL; reused ctx 15.4 us):
main/hash 17.7%, dvs_solver_propagate 9.5%, _fire_diffcycle 9.0%, _wake_var 7.9%,
prop_enqueue 6.5%, memset 3.9%, dvs_builder_alloc 2.6%, _box_try 2.6%,
_rel_valid 2.4%, _drain 2.3%, restrict 2.3%, ctx_tighten_lb64 2.3%,
trail_backtrack 2.2%, _ev 2.2%, trail_record_lb 1.5%.
- Inclusive: `_solver_solve` 64%, of which `_box_sample` 51% (propagation
  inside box sampling), `dvs_solver_validate_model` 5%, `rebuild` (builder) 10%.
  The CDCL solve on a trivial problem is propagation-bound; the per-call
  fixed costs are hashing the problem (~18%), rebuilding it (~10%), memset (~4%).

arith.mul_div_64 (10.3 ms/call): `_prop_cone` 95.1%, `_diversify_sweeps` 3.2%,
kissat 0.4% total. Inclusive `dvs_bbsolver_rediversify` 98%. The SAT solve is
negligible; the cost is the diversity flip-check re-propagating each flipped
input bit through the bit-blasted 64-bit mul/div cone, plus rebuilding the
fanout CSR every call. A reused context does not help (10.2 vs 11.0 ms), because
rediversify is the whole cost. Same in the front end (`_check_sat_bitblast_body`).

hard.unique_64 (CDCL): api-cache self: _check_clause 34.5%, _solver_solve_core
16.6%, clause_propagate 11.6%, _wake_var 7.8%, _analyze 5.3%,
dvs_solver_propagate 4.6%, prop_enqueue 3.1%, _fire_reification_eq_32 2.8%,
clause_notify_lb 2.6%, _drain 2.1%. api (fresh ctx) self: _solver_solve_core
27.7%, _wake_var 15.8%, _check_clause 10.4%, _analyze 7.5%, propagate 7.2%.
- Reused contexts are ~2x SLOWER than fresh ones (23.9 ms/call reused vs 11.1 ms
  in the all-fresh api pass; the "new ctx 1.9 ms" column averages only the
  trivial start-up check and the first call, so it is not representative):
  the clause machinery (_check_clause, clause_propagate, clause_notify_*)
  goes from ~15% to ~65%. Consistent with learnt clauses accumulating across
  `dvs_solver_reset` on a cached context (not verified in source).
- Fresh-context cost is search (LCG conflict analysis + propagation), ~11 ms.

construct.packed_struct: same reuse regression (144 us reused vs 31 us solve on
a fresh context in the api pass);
api-cache self = _check_clause 75.1%, clause_propagate 8.3%.

dist.dist_if (api, 705 us): inclusive `_enum_sample` 72.6%, `_box_sample` 23%;
self _fire_diffcycle 32.4%, dvs_solver_propagate 11.5%, prop_enqueue 8.2%,
_wake_var 7.5%. Every call is a new problem (random dist constants), and
sampling enumerates the solution space by propagation.

## Mode mismatch: replay != what Verilator runs

`bench_vlt_replay`'s text pass (which picks the engine for api/api-cache) sets
`fe.verilator_mode = 1`, and its pipe pass runs `--mode=verilator`. But config
`vlt-dvs-solver` (and the capture for `dvs-standalone`) runs `dv-solve-smt2`
with no mode flag. Whole-session wall time of `dv-solve-smt2 <session.cmds.smt2>`
(best of 3, includes process start), us/check-sat:

| bench | default (what Verilator uses) | --mode=verilator (what replay uses) |
|---|---:|---:|
| dist.dist_if | 58.9 | 692.1 |
| hard.unique_64 | 40838.5 | 11164.6 |
| array.unique_32 | 7238.1 | 623.0 |
| construct.inside_set | 65.8 | 67.2 |
| scenario.pcie_tlp | 51.5 | 77.2 |

So the replay can be 10x off in either direction from the in-Verilator
solver path; the two modes route/sample differently (distribution quality
not checked here).

## dist.dist_enum crash (reproduces locally, not fixed)

`bench_vlt_replay ... --passes api` -> SIGSEGV during the untimed text pass,
before any API timing. With a RelWithDebInfo copy built in the scratchpad:

```
#0 capture_pre (fe=...) at tests/c/bench_vlt_replay.c:252
     g_pre[g_pre_n] = malloc(sizeof(dvs_problem_t) + a->pool.capacity);   a = 0x0, k = 0
#1 pass_text (do_capture=1, ...) at tests/c/bench_vlt_replay.c:286
#2 main at tests/c/bench_vlt_replay.c:760
```

`fe->aux_problems[0]` is NULL while `fe->n_aux_problems > 0`; triggered by the
dist push / assert / double check-sat sequence. The Verilator capture itself
passes (dv-solve-smt2 handles the session).

## Published vs local numbers

Local api-cache numbers are far below the 2026-10-27 published dvs-standalone
figures (inside_set 27.7 vs 146.5, range 16.5 vs 946.5, pcie_tlp 17.5 vs 292.7,
mul_div_64 10257 vs 59233, unique_64 11332 (api) vs 14000), but dist_if is
higher (705 vs 337). Different host (shared, flagged noisy) and dv-solve tree,
so treat the published totals as host-specific.

## Raw replay output

### arith.countones
```
  2733 commands, 211 check-sats, 0.9 MB of SMT-LIB
  captured 211 problems (210 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   0.628 s   2976.8 us/check-sat   (CPU 0.628 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.004 s     17.4 us
             compile/bitblast   0.001 s      5.1 us
             solve+read         0.623 s   2954.2 us
api-cache  wall   0.104 s    491.0 us/check-sat   (CPU 0.104 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.003 s     12.4 us
             compile/bitblast   0.000 s      0.1 us
             solve+read         0.101 s    478.5 us
               on a reused context: 468.0 us per call; on a new one: 1567.5 us
```
### arith.mul_div_64
```
  3153 commands, 211 check-sats, 0.1 MB of SMT-LIB
  captured 211 problems (210 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   3.230 s  15308.7 us/check-sat   (CPU 3.230 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.001 s      3.9 us
             compile/bitblast   0.001 s      2.6 us
             solve+read         3.229 s  15302.0 us
api-cache  wall   2.164 s  10257.1 us/check-sat   (CPU 2.164 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      2.3 us
             compile/bitblast   0.000 s      0.1 us
             solve+read         2.164 s  10254.7 us
               on a reused context: 10247.8 us per call; on a new one: 10971.9 us
```
### array.unique_32
```
  8823 commands, 211 check-sats, 0.8 MB of SMT-LIB
  captured 211 problems (204 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   1.525 s   7229.4 us/check-sat   (CPU 1.525 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.005 s     21.7 us
             compile/bitblast   0.002 s      7.3 us
             solve+read         1.519 s   7200.3 us
api-cache  wall   0.070 s    329.7 us/check-sat   (CPU 0.070 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.003 s     15.8 us
             compile/bitblast   0.000 s      1.0 us
             solve+read         0.066 s    312.9 us
               on a reused context: 309.7 us per call; on a new one: 650.9 us
```
### construct.inside_set
```
  2943 commands, 211 check-sats, 0.2 MB of SMT-LIB
  captured 211 problems (208 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   0.016 s     77.7 us/check-sat   (CPU 0.016 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.5 us
             compile/bitblast   0.007 s     34.4 us
             solve+read         0.009 s     41.8 us
api-cache  wall   0.006 s     27.7 us/check-sat   (CPU 0.006 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.0 us
             compile/bitblast   0.004 s     17.0 us
             solve+read         0.002 s      9.8 us
               on a reused context: 9.1 us per call; on a new one: 76.7 us
```
### construct.range
```
  4413 commands, 211 check-sats, 0.2 MB of SMT-LIB
  captured 211 problems (0 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   0.012 s     57.9 us/check-sat   (CPU 0.012 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.3 us
             compile/bitblast   0.001 s      5.4 us
             solve+read         0.011 s     51.2 us
api-cache  wall   0.003 s     16.5 us/check-sat   (CPU 0.003 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.2 us
             compile/bitblast   0.000 s      0.2 us
             solve+read         0.003 s     15.1 us
               on a reused context: 15.0 us per call; on a new one: 26.2 us
```
### construct.packed_struct
```
  2733 commands, 211 check-sats, 0.2 MB of SMT-LIB
  captured 211 problems (0 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   0.008 s     36.8 us/check-sat   (CPU 0.008 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.0 us
             compile/bitblast   0.001 s      5.1 us
             solve+read         0.006 s     30.7 us
api-cache  wall   0.030 s    144.3 us/check-sat   (CPU 0.030 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.0 us
             compile/bitblast   0.000 s      0.1 us
             solve+read         0.030 s    143.2 us
               on a reused context: 144.4 us per call; on a new one: 9.2 us
```
### control.ite_cond
```
  2313 commands, 211 check-sats, 0.1 MB of SMT-LIB
  captured 211 problems (0 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   0.015 s     71.3 us/check-sat   (CPU 0.015 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.0 us
             compile/bitblast   0.001 s      3.7 us
             solve+read         0.014 s     66.5 us
api-cache  wall   0.001 s      6.2 us/check-sat   (CPU 0.001 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      0.7 us
             compile/bitblast   0.000 s      0.2 us
             solve+read         0.001 s      5.3 us
               on a reused context: 5.0 us per call; on a new one: 32.2 us
```
### dist.dist_if
```
  2313 commands, 211 check-sats, 0.2 MB of SMT-LIB
  captured 211 problems (0 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   0.149 s    704.9 us/check-sat   (CPU 0.149 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.001 s      3.3 us
             compile/bitblast   0.002 s      9.5 us
             solve+read         0.146 s    692.1 us
api-cache  wall   0.152 s    718.7 us/check-sat   (CPU 0.152 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.001 s      3.2 us
             compile/bitblast   0.002 s     11.1 us
             solve+read         0.149 s    704.4 us
```
### dist.dist_enum
```
(replay crashed: SIGSEGV in capture_pre, see crash section)
```
### hard.unique_64
```
  2103 commands, 211 check-sats, 0.6 MB of SMT-LIB
  captured 211 problems (2 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   2.391 s  11331.7 us/check-sat   (CPU 2.391 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.008 s     40.3 us
             compile/bitblast   0.051 s    239.5 us
             solve+read         2.332 s  11051.9 us
api-cache  wall   5.003 s  23711.4 us/check-sat   (CPU 5.003 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.008 s     38.8 us
             compile/bitblast   0.003 s     13.7 us
             solve+read         4.992 s  23658.8 us
               on a reused context: 23866.8 us per call; on a new one: 1918.8 us
```
### scenario.pcie_tlp
```
  4413 commands, 211 check-sats, 0.3 MB of SMT-LIB
  captured 211 problems (0 answered on bitblast, 0 with constraints added under push, 0 unsat), 0 check-sats not captured
api        wall   0.012 s     57.3 us/check-sat   (CPU 0.012 s; 211 distinct, 0 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.9 us
             compile/bitblast   0.002 s      8.4 us
             solve+read         0.010 s     47.0 us
api-cache  wall   0.004 s     17.5 us/check-sat   (CPU 0.004 s; 2 distinct, 209 reused; 0 CDCL answers escalated to bitblast, 0 bitblast failures, 0 unsolved)
             build (builder)    0.000 s      1.7 us
             compile/bitblast   0.000 s      0.3 us
             solve+read         0.003 s     15.5 us
               on a reused context: 15.4 us per call; on a new one: 31.3 us
```

## Raw perf self-time (top 15)

Files: inside_set_dw (api-cache x200), pcie_dw (api-cache x100), muldiv_dw (api-cache x1), u64_api-cache, u64_api, ps = packed_struct api-cache x20, di = dist_if api x3. Each includes one untimed text pass.

### inside_set_dw
```
    60.49%  [.] dvs_solver_exclude_value
    11.69%  [.] _prop_cone.constprop.0
     8.02%  [.] main
     5.10%  [.] _diversify_sweeps
     2.55%  [.] _check_clause
     1.25%  [.] bool restrict
     1.09%  [.] dvs_builder_alloc
     0.79%  [.] dvs_bbsolver_value
     0.68%  [.] _box_try
     0.59%  [.] __memset_avx512_unaligned_erms
     0.42%  [.] _pick_avoiding_holes
     0.38%  [.] _fire_disj_clause
     0.38%  [.] dvs_aig_is_and
     0.33%  [.] _wake_var
     0.32%  [.] dvs_solver_propagate
```
### pcie_dw
```
    17.71%  [.] main
     9.52%  [.] dvs_solver_propagate
     9.01%  [.] _fire_diffcycle
     7.87%  [.] _wake_var
     6.47%  [.] prop_enqueue
     3.85%  [.] __memset_avx512_unaligned_erms
     2.60%  [.] dvs_builder_alloc
     2.59%  [.] _box_try
     2.37%  [.] _rel_valid.constprop.0
     2.31%  [.] _drain
     2.31%  [.] bool restrict
     2.29%  [.] ctx_tighten_lb64
     2.21%  [.] trail_backtrack
     2.19%  [.] _ev
     1.54%  [.] trail_record_lb
```
### muldiv_dw
```
    95.07%  [.] _prop_cone.constprop.0
     3.24%  [.] _diversify_sweeps
     0.32%  [.] kissat_search_propagate
     0.19%  [.] dvs_aig_is_and
     0.14%  [.] kissat_add
     0.11%  [.] dvs_aig_is_and@plt
     0.08%  [.] dvs_aig_mk_and
     0.06%  [.] dvs_aig_get_children
     0.06%  [.] __memmove_avx512_unaligned_erms
     0.05%  [.] new_binary_clause
     0.04%  [.] _int_malloc
     0.04%  [.] malloc
     0.04%  [.] kissat_enlarge_vector
     0.04%  [.] kissat_activate_literals
     0.04%  [.] __memset_avx512_unaligned_erms
```
### u64_api-cache
```
    34.48%  [.] _check_clause
    16.63%  [.] _solver_solve_core
    11.59%  [.] clause_propagate
     7.84%  [.] _wake_var
     5.33%  [.] _analyze.constprop.0
     4.56%  [.] dvs_solver_propagate
     3.09%  [.] prop_enqueue
     2.81%  [.] _fire_reification_eq_32
     2.59%  [.] clause_notify_lb
     2.11%  [.] _drain
     1.48%  [.] clause_notify_ub
     1.07%  [.] __memset_avx512_unaligned_erms
     0.71%  [.] trail_backtrack
     0.58%  [.] prop_enqueue@plt
     0.51%  [.] _pick_value_weighted
```
### u64_api
```
    27.65%  [.] _solver_solve_core
    15.84%  [.] _wake_var
    10.36%  [.] _check_clause
     7.45%  [.] _analyze.constprop.0
     7.17%  [.] dvs_solver_propagate
     4.99%  [.] prop_enqueue
     4.31%  [.] _fire_reification_eq_32
     3.69%  [.] clause_propagate
     2.48%  [.] trail_backtrack
     2.07%  [.] __memset_avx512_unaligned_erms
     1.94%  [.] kissat_search_propagate
     1.88%  [.] _drain
     1.20%  [.] prop_enqueue@plt
     0.57%  [.] _memo_put.part.0
     0.57%  [.] clause_notify_lb
```
### ps
```
    75.07%  [.] _check_clause
     8.28%  [.] clause_propagate
     1.85%  [.] _drain
     1.37%  [.] main
     1.34%  [.] _analyze.constprop.0
     1.28%  [.] _solver_solve_core
     1.06%  [.] _fire_diffcycle
     1.05%  [.] _wake_var
     0.97%  [.] clause_notify_lb
     0.65%  [.] dvs_solver_propagate
     0.64%  [.] clause_notify_ub
     0.50%  [.] prop_enqueue
     0.48%  [.] _fire_bounds_concat_64
     0.48%  [.] _ev
     0.33%  [.] ctx_tighten_lb64
```
### di
```
    32.39%  [.] _fire_diffcycle
    11.53%  [.] dvs_solver_propagate
     8.19%  [.] prop_enqueue
     7.50%  [.] _wake_var
     5.21%  [.] _rel_valid.constprop.0
     3.68%  [.] ctx_tighten_ub64
     2.88%  [.] ctx_tighten_lb64
     2.45%  [.] _drain
     2.29%  [.] _fire_bounds_eq_64
     2.13%  [.] _fire_reification_64
     2.12%  [.] dvs_stack_alloc
     1.96%  [.] trail_record_ub
     1.87%  [.] trail_backtrack
     1.36%  [.] __memset_avx512_unaligned_erms
     1.19%  [.] _fire_reification_eq_64
```
