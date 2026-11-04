# simbench pre-flight: the 10-28 reference measured a stuck sampler (2026-11-04)

The first runs of the simbench CI driver (`docs/simbench_nightly_plan.md`, S1)
failed the README §5 pre-flight. dvs-standalone `memmap.soc_mem_map` ran at 503
µs/call against a reference of 39, and `scenario.bus_transaction` at 63 against
7. Calibration was normal, and the other four smoke benches were at or under
their reference. This note records why, and what changed as a result.

## Setup

- Host: AMD Ryzen 9 9950X, idle (loadavg < 0.6), GCC 13.3, Release builds.
- Verilator 5.054 (`edapack/verilator-bin`, pinned in
  `tests/perf/sim_tools.lock.json`). z3 4.8.12 for the `vlt-z3-default` dumps.
  xezim-bin 0.11.3.
- Sessions: the smoke-tier captures (`dvs-standalone`, N=200, seed 1: 10
  warm-up calls plus the start-up check gives 211 check-sats). Each build
  replays the same session file, so the simulator is held constant:
  `bench_vlt_replay <session> --reps 1 --passes api-cache --calls 210
  --warmup 10 --mode default --quality --dump-values <file>`, the harness's
  own arguments (`standalone.py`).

## Not Verilator, not the measurement

- The 10-28 reference sessions (captured with Verilator 5.053 devel at
  simbench 1434232; kept in another checkout's
  `docs/notes/simbench/trial2/data/sessions/`) are byte-identical to the 5.054
  captures for both benches. HEAD replays either copy at 502 / 63 µs/call.
- The replay arguments are unchanged (`--mode default`). There are no
  escalations: 2 distinct problems and 209 reuses per bench. The time is
  solve plus readback on a reused bitblast context; build and compile take
  about 4 µs.

## Time by build (same sessions)

| commit | soc_mem_map µs/call | bus_transaction µs/call |
|---|---|---|
| b259e6a, 48ceaf1, b65f94d | 35 | 6 |
| ce37488 "Sampling: resample a reused bitblast context; seed the affine model" | 490 | 30 |
| ab0816c "bbsolver: div/mul rejection sampler (`_divsamp`)" | — | 51 |
| f6184d1, 2c70357, 33eb67b (`_divsamp` follow-ups) | — | 61–65 |
| 17fb20a (main, 0.5.0) | 503 | 63 |
| d69b110 (`wpk/solve-stats-routing-gap`, R1–R3 sampler visit caps) | 512 | 37.5 |

At 17fb20a, `--engine cdcl` gives 83 and 21 µs/call.

## Spread by build (the answers each build timed)

`bench_vlt_replay --quality`: every model was checked against the
constraints, and none was invalid in any build. Per field: distinct values
over 211 models / entropy normalized by log(211).

**memmap.soc_mem_map** (12 fields, 17 bits each)

| | stuck | distinct per field | min entropy |
|---|---|---|---|
| b65f94d (before ce37488) | **12 of 12** | 1 | 0.00 |
| ce37488 | 0 | 16–80 | 0.238 |
| 17fb20a | 0 | 20–85 | 0.258 |
| d69b110 | 0 | 20–85 | 0.258 |
| Verilator + z3, default sampler (200 calls) | 0 | 143–181 | 0.91 |
| xezim 0.11.3 (200 calls) | 0 | 187–200 | 0.98 |

**scenario.bus_transaction**

| | beat_size | burst_len | end_addr | word_addr | min entropy |
|---|---|---|---|---|---|
| b65f94d | 4 / 0.26 | **1 / 0.00** | **1 / 0.00** | **1 / 0.00** | 0.00 |
| ce37488 | 4 / 0.26 | 117 / 0.85 | 193 / 0.98 | 196 / 0.98 | 0.259 |
| 17fb20a | 4 / 0.26 | 150 / 0.92 | 210 / 1.00 | 210 / 1.00 | 0.259 |
| d69b110 | 4 / 0.26 | 150 / 0.92 | 210 / 1.00 | 210 / 1.00 | 0.259 |
| Verilator + z3 | 4 / 0.26 | 109 / 0.85 | 199 / 1.00 | 197 / 1.00 | 0.260 |
| xezim 0.11.3 | 4 / 0.26 | 138 / 0.91 | 199 / 1.00 | 200 / 1.00 | 0.258 |

(beat_size has 4 legal values, so 0.26 is its ceiling for everyone.)

## Conclusions

1. **The 10-28 reference was the cost of not sampling.** Before ce37488 a
   reused bitblast context returned its previous answer, so every reused call
   gave the same values. ce37488 fixed that bug. The 35 and 6 µs/call it made
   possible were not a baseline to regress against. The README §5 table was
   re-measured at 17fb20a.
2. **bus_transaction is fine.** Its spread matches or beats z3's and xezim's.
   The routing-gap branch keeps the same spread at 37.5 µs/call (from 63), so
   it is a clean win.
3. **soc_mem_map is an open problem** (B130 in `docs/solver_bug_backlog.md`).
   It is slow and still clumped: 20–85 distinct values per 17-bit field over
   211 calls, where z3 gets 143–181 and xezim 187–200. xezim's whole
   simulation costs 134 µs/call; dv-solve's replay alone costs about 500.
4. **The pre-flight now checks spread as well as speed** (`ci.py`
   `check_preflight`, README §5). It fails on a stuck field or an invalid
   model, and on a min entropy more than 0.05 below the reference.

## Spread is compared with the reference, not with z3

Verilator's default sampler spreads each field. dv-solve is uniform over the
solutions, as the LRM asks. These give different per-field entropy wherever
the solutions are uneven across a field's values. In scenario.axi4_burst,
`burst` has 3 legal values, but 16 + 256 + 4 solutions (burst = 0 / 1 / 2).
Uniform over solutions gives burst = 1 about 93% of the time, and normalized
entropy 0.056. dv-solve measures 0.059, xezim 0.078, and z3 0.163. A
higher figure is not better there. soc_mem_map is different: its fields have
large ranges and many solutions each, so a uniform sampler should give nearly
all-distinct values, as z3 and xezim do.

## Pre-flight at 17fb20a (three runs; spread identical in all three)

| config / bench | µs/call | min entropy |
|---|---|---|
| dvs-standalone construct.range | 7.2–7.3 | 0.809 |
| dvs-standalone dist.weighted | 26.7–28.0 | 0.179 |
| dvs-standalone memmap.soc_mem_map | 502.3–502.9 | 0.258 |
| dvs-standalone scenario.axi4_burst | 8.1–8.2 | 0.059 |
| dvs-standalone scenario.bus_transaction | 62.7–63.3 | 0.259 |
| dvs-standalone scenario.reset_reuse | 4.1–4.4 | 0.669 |
| vlt-dvs-solver construct.range | 68.5–69.4 | 0.817 |
| vlt-dvs-solver dist.weighted | 49.5–53.8 | 0.201 |
| vlt-dvs-solver memmap.soc_mem_map | 7250–7312 | 0.271 |
| vlt-dvs-solver scenario.axi4_burst | 46.0–48.9 | 0.059 |
| vlt-dvs-solver scenario.bus_transaction | 1246–1307 | 0.261 |
| vlt-dvs-solver scenario.reset_reuse | 26.7–29.6 | 0.670 |

`--calib`: cpu_ns_per_iter 1.05, dvs_solve_us 0.72, dvs_compile_us 3.8.
(One of the three runs read cpu_ns_per_iter 1.37 on its first calib, which
was cold; its bench timings matched the others.)
