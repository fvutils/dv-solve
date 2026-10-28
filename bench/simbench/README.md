# simbench

Plain-SystemVerilog randomization benchmarks that run on any simulator:
Verilator with z3 or dv-solve, VCS, Xcelium or Questa. Each bench calls
`randomize()` N times in a loop, times the loop, checks every result against
the constraints, and records the time per call and the quality of the random
values. Results are JSONL records compatible with dv-solve's
`tests/perf/schema.py`.

This README is the complete procedure for running the benchmark. It is written
so that a person or an agent can follow it without other documents. Follow it
as written: **use the commands here, not a wrapper script.** The 10-27 run on the
shared bench host used home-made wrapper scripts and an unoptimised dv-solve
build, and its dv-solve numbers came out 6–14× too slow. Nobody could tell why
until the scripts and the build were inspected by hand.

All commands below run from this directory (`bench/simbench`) unless they say otherwise.

## 1. What you will produce

1. `preflight.jsonl`: the pre-flight run (section 5).
2. `results.jsonl`: one record per (config, bench) for the full matrix (section 6).
3. `sessions/`: the captured solver sessions (compressed, ~1–2 MB in total).
4. `run-notes.md`, written by you:
   - the host, and what else was running on it;
   - the exact commands you ran;
   - anything you changed or worked around, and why;
   - the output of the build and pre-flight checks (sections 3–5).

Then make the report (section 7).

## 2. Rules

- **Run serially.** Run one pytest process at a time and do not use
  `pytest -n`/xdist. Benches are timed, and parallel runs disturb each other.
  Prefer an idle host, and note the load (records carry the load average as well).
- **Do not edit the harness, benches or configs.** If something has to change
  (compiler, tool paths), do it through the environment variables below and say
  so in `run-notes.md`. If you cannot get a step to pass, stop and report: do not
  work around it.
- **Keep N=200 and seed 1** (the defaults) unless asked otherwise.
- **Stop at any failed check** in sections 3–5. A failed check means the numbers
  would be wrong.

## 3. Build and check the tools

### 3.1 dv-solve (this checkout)

From the dv-solve root (`../..`):

```
ivpm update -d dev                       # fetches CaDiCaL (and a Python env) into packages/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j > build.log 2>&1; tail -3 build.log
```

Always use `Release`. A `Debug` build is ASAN at `-O0`. Since 2026-10-28 a missing
build type also means Release, but say it anyway: an old `build/` directory keeps
its cached build type. This builds `build/dv-solve-smt2` and `build/bench_vlt_replay`.
simbench uses them by default when it runs from this checkout. Set
`SIMBENCH_DVS_SMT2` / `SIMBENCH_DVS_REPLAY` only to benchmark a different dv-solve.

Check what you built, **in the shell you will run the benchmark from**:

```
../../build/bench_vlt_replay --build-info
../../build/dv-solve-smt2 --build-info
```

Both must print `optimized=1 asan=0` and the commit you expect.
`bench_vlt_replay` must also print `library <dv-solve root>/build/lib/libdv_solve.so`.
If the library is anywhere else, `LD_LIBRARY_PATH` is redirecting it to another
build; it overrides the binary's own search path. Fix the environment, for
example with `env -u LD_LIBRARY_PATH`, and check again. A trailing `:` or an empty
entry in `LD_LIBRARY_PATH` means "the current directory", which has picked up
stale libraries before.

### 3.2 Verilator with sampler control

The Verilator configs that set a sampler (`vlt-z3-default`, `vlt-dvs-solver`,
`vlt-dvs-default`, `dvs-standalone` and the other replay configs) need a
Verilator with **sampler control**: the `+verilator+rand+sampler+<name>` runtime
option. It is in upstream Verilator master since PR #8899; releases up to 5.052
do not have it. Build upstream master:

```
git clone https://github.com/verilator/verilator && cd verilator
autoconf && ./configure && make -j > build.log 2>&1; tail -3 build.log
```

Check it. This must print the option:

```
bin/verilator --help | grep -F '+verilator+rand+sampler+'
```

Point simbench at it with `SIMBENCH_VERILATOR_SC=<path>/verilator/bin/verilator`
(the default is `verilator` on PATH). The harness repeats this check before every
sampler config's test, and fails the test if the option is missing. After each
run, it also checks that the image really used the sampler asked for (the
`; sampler:` line of the solver log).

**C++ compiler:** `verilator --binary` with randomization needs a C++20 compiler
(coroutines), e.g. GCC 10 or later. If the default `g++` is older, put a newer
one first on `PATH`, and pass extra Verilator arguments to every Verilator build
through the environment, e.g.:

```
export SIMBENCH_VERILATOR_ARGS="-MAKEFLAGS CXX=/path/to/gcc-12/bin/g++ -CFLAGS -std=gnu++20"
```

Record this in `run-notes.md`. If that compiler's runtime needs `LD_LIBRARY_PATH`,
set it, then repeat the `--build-info` check of 3.1 in that environment.

### 3.3 z3, and the commercial simulators

- `z3` on PATH (any 4.8+) for the `*-z3*` configs.
- VCS, Xcelium, Questa: `vcs`, `xrun`, `vsim` on PATH, or named by
  `SIMBENCH_VCS`, `SIMBENCH_XRUN`, `SIMBENCH_VSIM`. Licences must be available.
  Their license terms may restrict publishing benchmark numbers.

A config whose tool is missing is **skipped** (shown as `s`), not failed. Check
the skip reasons (`pytest -rs`) to make sure nothing you meant to run was
skipped.

## 4. Environment for every run

```
cd bench/simbench
pip install -e .                       # or prefix every command with PYTHONPATH=src
export SIMBENCH_HOST=<short machine name>
export SIMBENCH_VERILATOR_SC=<path>/verilator/bin/verilator
export SIMBENCH_BUILD_DIR=$PWD/out/build          # keeps built images across pytest runs
export SIMBENCH_SESSION_DIR=$PWD/out/sessions     # keeps the captured solver sessions
```

| variable | meaning | default |
|---|---|---|
| `SIMBENCH_HOST` | machine label in the records | the machine id |
| `SIMBENCH_VERILATOR_SC` | Verilator with sampler control | `verilator` on PATH |
| `SIMBENCH_VERILATOR` | stock Verilator ≥ 5.040 for `vlt540-z3` | `verilator` on PATH |
| `SIMBENCH_VERILATOR_ARGS` | extra arguments for every Verilator build (compiler, C++ standard) | none |
| `SIMBENCH_DVS_SMT2`, `SIMBENCH_DVS_REPLAY` | dv-solve tools | this checkout's `build/` |
| `SIMBENCH_VCS`, `SIMBENCH_XRUN`, `SIMBENCH_VSIM` | commercial simulators | on PATH |
| `SIMBENCH_BUILD_DIR` | kept build images; keyed by tool version, bench and build args | a fresh pytest temp dir |
| `SIMBENCH_SESSION_DIR` | keep captured sessions as `<dir>/<config>/<bench>.smt2.gz` | not kept |
| `SIMBENCH_N`, `SIMBENCH_SEED` | calls per run, seed | 200, 1 |
| `SIMBENCH_DUMP_DIR` | keep the randomized values per (config, bench) | not kept |
| `SIMBENCH_CONFIGS` | extra config YAML (same `id` replaces a built-in one) | none |

## 5. Pre-flight check (about 2 minutes)

```
../../build/bench_vlt_replay --calib
python3 -m pytest -q -rs -m smoke -k "dvs-standalone or vlt-dvs-solver" --simbench-out=out/preflight.jsonl
```

`--calib` times fixed work: a plain integer loop (the host's speed) and a fixed
dv-solve problem (the host's speed times the library build). Compare everything
with this reference (AMD Ryzen 9 9950X, Release, GCC 13.3, idle host, 2026-10-28):

| reading | reference |
|---|---|
| `--calib` `cpu_ns_per_iter` | 1.18 |
| `--calib` `dvs_solve_us` | 0.74 |
| `--calib` `dvs_compile_us` | 4.3 |
| dvs-standalone construct.range | 15 µs/call |
| dvs-standalone scenario.axi4_burst | 13 µs/call |
| dvs-standalone scenario.bus_transaction | 7 µs/call |
| dvs-standalone scenario.reset_reuse | 3 µs/call |
| dvs-standalone memmap.soc_mem_map | 39 µs/call |
| dvs-standalone dist.weighted | 52 µs/call |
| vlt-dvs-solver construct.range | 127 µs/call |
| vlt-dvs-solver scenario.axi4_burst | 85 µs/call |

To read the µs/call values from the file:

```
python3 -c "import json;[print(r['simbench']['config'],r['simbench']['bench'],r['valid'],(r['simbench']['result'] or {}).get('ns_per_call',0)/1e3) for r in map(json.loads,open('out/preflight.jsonl'))]"
```

Pass criteria:
- all 12 tests pass;
- `dvs_solve_us / cpu_ns_per_iter` is within 1.5× of the reference ratio (0.63). A
  bigger ratio means the dv-solve build is slow, not the host;
- after scaling by `dvs_solve_us / 0.74`, every µs/call is within 3× of the reference.

If a criterion fails, stop. Send `out/preflight.jsonl` and the outputs of
`--build-info` and `--calib`.

## 6. Full matrix

Run each config separately, all into one results file. One Verilator config over
all 100 benches takes about 15 minutes at N=200.

```
for CFG in vlt-dvs-solver dvs-standalone vlt-z3-default vcs xcelium questa; do
  python3 -m pytest -q -rs -k "$CFG-" --simbench-out=out/results.jsonl > out/$CFG.log 2>&1
  tail -1 out/$CFG.log
done
```

- `-k "$CFG-"` selects one config. Tests are named `<config>-<bench>`, and the
  trailing `-` keeps `vcs` from matching other ids.
- Leave out configs whose tools you do not have; they would only be skipped.
- **Resuming:** `--simbench-out` appends, and the report keeps the last record for
  each (config, bench, host). To re-run only the failures of the last run of a
  config, add `--lf` to the same command.
- **Failures:** the reason is in the record (`reason`) and in `out/$CFG.log`.
  Report them, and do not edit benches to make them pass.

Tiers: `-m smoke` (6 benches), `-m standard`, `-m stress`; with no `-m`, all 100 run.

## 7. Report and hand-off

```
python3 -m dv_solve_simbench.report out/results.jsonl --ref vcs -o out/report.md
```

(Use `--ref vlt-z3-default` when no commercial simulator ran.) Hand over:
- `out/results.jsonl`, `out/preflight.jsonl`, `out/report.md`, `out/sessions/`;
- `out/*.log`;
- `run-notes.md`.

## 8. What each config measures

| id | simulator, solver, sampler | measures |
|---|---|---|
| `vlt-dvs-solver` | Verilator (sampler control), dv-solve-smt2 over the SMT-LIB pipe, sampler `solver` | end-to-end randomize() with dv-solve as Verilator's solver today |
| `dvs-standalone` | the same run, then its captured session replayed in-process by `bench_vlt_replay` | dv-solve's per-call cost alone, without Verilator or the pipe (below) |
| `vlt-z3-default` | Verilator (sampler control), z3, Verilator's sampler | Verilator's default flow; the open-source reference |
| `vlt-dvs-default` | Verilator (sampler control), dv-solve-smt2, Verilator's sampler | dv-solve as a plain SMT solver |
| `z3-standalone`, `dvs-smt2-standalone` | one solver process reads the captured session text | text replay, process start and parse included |
| `vlt540-z3` | stock Verilator ≥ 5.040, z3 | released Verilator |
| `vcs`, `xcelium`, `questa` | commercial simulator, built-in solver | the commercial reference |

**`dvs-standalone` in detail:**
1. The Verilator run captures its solver session (`+verilator+solver+file+`). Its
   own time is not used: logging the session slows it, so `vlt-dvs-solver` is
   timed by its own run, with no logging.
2. `bench_vlt_replay` parses the session once, untimed, in the capture's mode
   (from the config's solver command), and captures each check-sat's problem.
3. The timed pass (`api-cache`) rebuilds each problem through the public
   `dvs_builder` API, compiles it (reusing a compiled context when the problem
   repeats byte-for-byte), solves, and reads every variable back.
4. Time is reported per randomize() call (a call starts at `(reset)`), without the
   start-up check and the 10 warm-up calls. When the sessions per call vary (randc
   retries), the warm-up stays in the average, and the record says so
   (`simbench.replay.warmup_in_average`).

It is the solver-side cost of a direct-API integration. It leaves out Verilator's
own per-call cost: collecting variables and constraints, and writing results back.

## 9. What a record holds

Each line of `results.jsonl` is one (config, bench) run. For troubleshooting a
number that looks wrong, it holds:

- `simbench.result`: ns per call, first-call time, failures, check errors, and
  quality (`randomization`: distinct values and entropy per field).
- `tools.solver_build`, `tools.replay_build`: for dv-solve binaries, the dv-solve
  commit, dirty state, build type, `optimized`, `asan` and compiler; the library
  file actually loaded under the run's environment; sha256 of binary and library.
- `tools.simulator_version`, `tools.sampler_active`: the tool, and the sampler the
  image actually used.
- `tools.env`: the environment variables that change simulator or solver behaviour.
- `machine`: CPU, cores, governor, clock, CPUs in the affinity mask, load average.
- `calib`: fixed work timed just before this run (`py_loop_ms`, and `--calib`'s
  `cpu_ns_per_iter`, `dvs_solve_us`, `dvs_compile_us`).
- replay configs: `simbench.session` (size, sha256 and check-sats of the captured
  session; its file when kept) and `simbench.replay` (arguments, mode, sessions
  and calls, distinct/reused problems, escalations, failures, build/compile/solve
  split, and reused vs new-context time).

## 10. Troubleshooting

| symptom | check |
|---|---|
| dv-solve configs much slower than the reference | `--build-info` (`optimized=1`, `asan=0`, library path); `calib.dvs_solve_us` vs `cpu_ns_per_iter` |
| "has no sampler control" | `SIMBENCH_VERILATOR_SC` points at a Verilator without `+verilator+rand+sampler+` (section 3.2) |
| "image ran sampler X; config wants Y" | the image ignored the sampler plusarg; rebuild with a fresh `SIMBENCH_BUILD_DIR` |
| Verilator build fails on coroutines or C++20 | newer compiler and `SIMBENCH_VERILATOR_ARGS` (section 3.2) |
| "replay timed N calls, the run made M" | the session does not match the run; send the kept session and the record |
| a config is skipped | `pytest -rs` shows why (tool not found) |
| results differ between hosts | compare `calib`, `machine`, `tools.*_build` and `simbench.session.sha256` of the two records |

## Layout

- `src/dv_solve_simbench/share/sv/`: the harness (`simbench_pkg.sv`, `simbench_top.sv`, `sb_time.c`)
- `src/dv_solve_simbench/share/configs.yaml`: the configs (its header documents every field)
- `benchmarks/<category>/<name>/{bench.sv,bench.yaml}`: one benchmark each
- `tests/`: the pytest collection over config × bench

## Benchmarks

100 benchmarks in 8 groups. The tier is standard unless marked.

- **arith** (14): add_ovf, alignment, bitslice, countones, mod_align, mul_div_64, pow2, redops, shift_align, shift_width, sysfunc, unsigned_ops, verilator_operators, wide_add_64
- **array** (16): array_index, array_ordering, array_reduction, array_sum, assoc_basic, cls_arr_member, dyn_size, foreach_basic, queue_basic, sorted_increasing, sum_partition, sum_with, unique_16, unique_32, unique_dyn, unpacked_2d
- **construct** (15): bool_logic, cmp_web, enum_basic, enum_cond, eq_chain, inside_ranges, inside_set, ne_set, packed_struct, range (smoke), signed_mix, struct_fields, three_unique, width_mix, xor_parity
- **control** (12): cond_inside, constraint_mode, implication_chain_8, implication_set, inheritance, inline_with, ite_cond, rand_mode, soft_override_with, soft_relaxation, solve_before, solve_before_array
- **dist** (8): dist_enum, dist_if, dist_mixed, dist_range, dist_weight_div, randc_basic, randc_enum, weighted (smoke)
- **hard** (7): global_nested, graph_color_8, knapsack_small, magic_square_3, nqueens_8 (stress), sudoku_4x4, unique_64 (stress)
- **memmap** (8): addr_map_64, mem_partition_knapsack, memmap_tight_32 (stress), nonoverlap_regions_8, page_table_entry, soc_addr_map_32, soc_addr_map_40, soc_mem_map (smoke)
- **scenario** (20): ahb_burst, apb_xfer, axi4_burst (smoke), bus_transaction (smoke), ddr5_cmd_basic, ddr5_mode_register, ddr5_timing, ddr5_timing_full (stress), dma_desc, ethernet_hdr, fifo_ctrl, i2c_xfer, mem_transaction, pcie_tlp, reset_reuse (smoke), riscv_instr_mem, riscv_instr_rtype, riscv_pmp_cfg, tl_ul_item, usb_pkt
