# Performance status (2026-11-06)

Working tree: 06635aa plus the uncommitted structural-reasoning work (step 3
parts 1-4 and the low-slice fix; docs/structural_reasoning_design.md). Release
build. Host loaded (~14 of 32 cores busy with other jobs); `--calib` read
about 15% slow against the reference. xezim was re-run under the same load,
so the ratios hold.

## Summary

- **Default path vs. xezim: no change since 11-05.** The 12 benches that lost
  still lose by about the same ratios (2.9x-22x). The CDCL work hasn't reached
  them, because the replay's default mode routes them to bit-blast.
- **With `--engine cdcl` the memmap benches and `mul_div_64` now win or come
  close, with full entropy.** `mul_div_64` takes 13 µs (xezim 16), and
  `addr_map_64` takes 316 µs (xezim 302); on 11-05 it fell back to bit-blast.
  The next step is routing (gap plan step 3b), not more solver work.
- **Solver level (SMT-LIB, vs. z3):** tier 1 is 6.5x faster (geomean), tier 2
  is 1.6x faster, and tier 3 is 0.6x (slower; BMC register-file and cache
  shapes, 20-110 ms against z3's 5-8 ms). All answers agree with z3.

## Simbench: dv-solve standalone vs. xezim

`bench_vlt_replay <session> --reps 5 --passes api-cache --mode default
--calls 210 --warmup 10 --quality` (the CI's arguments). Median of 3 rounds.

| bench | dvs µs now | dvs µs 11-05 | xezim µs now | dvs / xezim | entropy dvs | `--engine cdcl` µs / entropy |
|---|---:|---:|---:|---:|---:|---|
| arith.mul_div_64 | 336 | 301 | 16.2 | 21x | 1.00 | **13.1 / 1.00** |
| memmap.memmap_tight_32 | 16,266 | 14,590 | 891 | 18x | 0.21 | 6,445 / 0.999 |
| memmap.addr_map_64 | 2,897 | 2,696 | 302 | 9.6x | 0.13 | **316 / 1.00** |
| memmap.soc_addr_map_40 | 2,502 | 2,312 | 337 | 7.4x | 0.13 | **334 / 1.00** |
| scenario.bus_transaction | 69.3 | 84 | 12.6 | 5.5x | 0.259 | |
| memmap.soc_addr_map_32 | 1,802 | 1,686 | 301 | 6.0x | 0.149 | 327 / 1.00 |
| dist.dist_range | 47.9 | 37 | 8.8 | 5.4x | 0.786 | |
| memmap.soc_mem_map | 559 | 520 | 146 | 3.8x | 0.258 | **88.5 / 0.99** |
| dist.dist_mixed | 66.8 | 28-51 | 17.3 | 3.9x | 0.19 | |
| dist.dist_weight_div | 37.3 | 28-51 | 8.2 | 4.5x | 0.42 | |
| dist.dist_weighted | 36.9 | 28-51 | 8.6 | 4.3x | 0.18 | |
| dist.dist_if | 35.5 | 28 | 13.2 | 2.7x | 0.129 | |

Controls (dvs / xezim µs): ethernet_hdr 19.8 / 12.8, dma_desc 159 / 114,
randc_basic 13.6 / 8.4, sum_with 35.4 / 28.4, wide_add_64 15.0 / 13.4,
array_sum 192 / 186 (all 1.0-1.6x slower, as on 11-05). dv-solve is ahead on
construct.range (7.9 / 15.4), axi4_burst (9.1 / 118) and reset_reuse
(4.7 / 12.0). There were no invalid models and no stuck fields.

Most rounds varied by less than 3%. The dist benches and addr_map_64 ran
10-25% high in the first round only.

## API rand suite (`tests/perf/run_rand.py`, N=500)

Each cell is ms per call / JSD.

| bench | arm | now | 11-05 |
|---|---|---|---|
| memmap3 | dv-api | 1.61 / 0.085 | unchanged |
| memmap3 | dv-capi | 1.74 / 0.119 | unchanged |
| memmap3 | dv-swizzle | 5.1 / 0.004 | unchanged |
| memmap3 | z3-swizzle | 11.0 / 0.061 | |
| mul_div_64 | dv-api | 0.004 / 0.134 | failed (no compile) |
| mul_div_64 | dv-capi | 0.001 / 0.119 | failed |
| mul_div_64 | dv-swizzle | 21.5 / 0.132 | 90 / 0.81 |
| mul_div_64 | z3-swizzle | 370 / 0.39 | 384 / 0.39 |

The uniform floor is 0.007 for memmap3 and 0.121 for mul_div_64. The
`mul_div_64` arms are now at the floor. `memmap3` is still skewed on the API
arms (0.085-0.119 against 0.007).

## Solver level: SMT-LIB tiers vs. z3 (10 s timeout)

`run_compare.py --tier 1 --tier 2 --tier 3`, wall ms per problem including
process start.

| tier | n | default engine: geomean z3/dv | total dv / z3 ms | pure CDCL (`DV_NO_BITBLAST=1`) |
|---|---:|---:|---:|---|
| 1 | 34 | 6.5x | 45 / 323 | 6.9x |
| 2 | 60 | 1.6x | 654 / 596 | 5.1x; fifo_ptr_valid_bmc_d16 233 ms |
| 3 | 24 | 0.6x | 661 / 158 | 0.2x; wide_datapath_128 x4 unsolved |

- **Tier 2, default engine:** 12 BMC/k-induction problems take 18-37 ms
  against z3's 5-7 ms. Pure CDCL is faster on that tier overall, so the
  router picks bit-blast where CDCL would win.
- **Tier 3:** cache_direct_1way (41-49 ms), regfile_addr_alias (40-108 ms),
  regfile_simple and dma_engine_small (23-35 ms), all against z3's 5-8 ms.

## Where the work stands

1. **Routing (gap plan step 3b).** The replay and SMT front-end still pick
   bit-blast for memmap, `mul_div_64` and tier-2 BMC shapes, where CDCL is now
   faster and better spread. Before switching, check `memmap_tight_32`
   (6.4 ms on CDCL against xezim's 0.9 ms; 2 bit-blast fall-backs) and
   dist_range (worse on CDCL on 11-05).
2. **dist benches (3-5x) and bus_transaction (5.5x).** These come from
   `_enumsamp` and a context created per call (gap plan step 4, core caching).
   Not touched yet.
3. **Tier 3 BMC shapes vs. z3.** This is open, and is where pure CDCL is
   weakest.

## Repeating the measurement

From `bench/simbench`, with Verilator 5.054 (sampler option) from
`perf-builds/sim-tools/`:

```
export SIMBENCH_HOST=localnow PYTHONPATH=src \
  SIMBENCH_VERILATOR_SC=$REPO/perf-builds/sim-tools/verilator-5.054/verilator/bin/verilator \
  SIMBENCH_BUILD_DIR=$S/simb/build SIMBENCH_SESSION_DIR=$S/simb/sessions
env -u LD_LIBRARY_PATH python3 -m pytest -q -rs -k "dvs-standalone- and (mul_div_64 or memmap_tight_32 or ...)" --simbench-out=$S/simb/results.jsonl
zcat $S/simb/sessions/dvs-standalone/<b>.smt2.gz > $S/raw/<b>.smt2   # already in commands form: no extract_cmds
build/bench_vlt_replay $S/raw/<b>.smt2 --reps 5 --passes api-cache --mode default --calls 210 --warmup 10 --quality
```

To run xezim, set
`SIMBENCH_XEZIM=$REPO/perf-builds/sim-tools/xezim-0.11.3/xezim/bin/xezim` and
use the same `-k` with `xezim-` in place of `dvs-standalone-`. The rand suite
is `run_rand.run_suite({"n": 500, ...}, arms, workers=2)`.
