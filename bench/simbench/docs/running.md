# Running simbench and sharing results

All commands run from this directory (`bench/simbench`).

## Install

- Python 3.9+ (tested with 3.12) and the Python packages in `pyproject.toml`:
  `pip install pyyaml dv-flow-mgr dv-flow-libhdlsim pytest pytest-dfm`
  (or `pip install -e .`). Without installing, prefix commands with `PYTHONPATH=src`
  as below.
- At least one simulator:
  - **Stock Verilator** (5.040 or later) on PATH, plus `z3` on PATH: config `vlt540-z3`.
  - **Verilator with sampler control** and **dv-solve-smt2** (configs `vlt-z3-default`,
    `vlt-dvs-solver`, `vlt-dvs-default`, and the replay configs): build Verilator's
    sampler-control branch and dv-solve's `dv-solve-smt2`; point at them with the
    variables below if they are not at the default paths.
  - **VCS**, **Xcelium**, **Questa**: `vcs`, `xrun`, `vsim` on PATH (or named by a variable).

A config whose tool is missing is skipped, with the reason, not failed.

## Configs

A config is a simulator + solver + sampler. They are in
`src/dv_solve_simbench/share/configs.yaml`, whose header documents every field.

| id | what |
|---|---|
| `vlt-z3-default` | sampler-control Verilator, z3 over the SMT-LIB pipe, Verilator's sampler (reference "A") |
| `vlt-dvs-solver` | sampler-control Verilator, dv-solve-smt2, solver-side sampler ("B") |
| `vlt-dvs-default` | sampler-control Verilator, dv-solve-smt2, Verilator's sampler |
| `vlt540-z3` | stock Verilator, z3 |
| `dvs-standalone`, `z3-standalone`, `dvs-smt2-standalone` | replay B's captured solver session outside Verilator (see README) |
| `vcs`, `xcelium`, `questa` | commercial simulators, built-in solver and sampler |

Tool paths (environment variables; defaults in `configs.yaml`):

| variable | used by | default |
|---|---|---|
| `SIMBENCH_VERILATOR` | `vlt540-z3` | `verilator` on PATH |
| `SIMBENCH_VERILATOR_SC` | sampler-control Verilator configs | `~/projects/verilator/verilator-sampler-control/bin/verilator` |
| `SIMBENCH_DVS_SMT2` | dv-solve-smt2 configs | `~/projects/verilator/verilator-sampler-control/dv-solve/build/dv-solve-smt2` |
| `SIMBENCH_DVS_REPLAY` | `dvs-standalone` | dv-solve's `build/bench_vlt_replay` |
| `SIMBENCH_VCS` / `SIMBENCH_XRUN` / `SIMBENCH_VSIM` | `vcs` / `xcelium` / `questa` | `vcs` / `xrun` / `vsim` on PATH |

To add or change configs without editing the package, write a YAML file in the
same format and set `SIMBENCH_CONFIGS=/path/to/my-configs.yaml` (a config with
the same `id` replaces the built-in one). Use `version:` to pin the tool version
you meant to run; the test fails if the tool reports something else.

## Choosing simulators and benches

Tests are named `<config>-<bench>`, so `-k` selects both:

```
PYTHONPATH=src python3 -m pytest -m smoke -q                       # all configs, smoke benches
PYTHONPATH=src python3 -m pytest -m smoke -q -k vlt540-z3          # one config
PYTHONPATH=src python3 -m pytest -q -k "vcs and memmap"            # one config, one group, all tiers
```

Tiers: `-m smoke` (6 benches, ~1 min per config), `-m standard`, `-m stress`;
no `-m` runs all 100 benches.

## Before a full run: pre-flight check

The 10-27 dv-solve replay numbers could not be reproduced afterwards (later
replays of the same benches ran 10-60x faster), and the records could not say
why. Check the dv-solve build and the host before spending hours on a matrix:

1. Build dv-solve as Release (the default since 2026-10-28; a `Debug` build is
   ASAN at -O0). Then check what the binaries really load, **in the shell you
   will run from** (`LD_LIBRARY_PATH` overrides the replay's RUNPATH):

   ```
   $SIMBENCH_DVS_REPLAY --build-info     # library must be the build you expect,
   $SIMBENCH_DVS_SMT2 --build-info       # optimized=1 asan=0, the commit you expect
   ```
2. Time fixed work: `$SIMBENCH_DVS_REPLAY --calib`.
3. Run the smoke tier of the replay config and compare with the reference below:

   ```
   PYTHONPATH=src python3 -m pytest -q -m smoke -k dvs-standalone --simbench-out=preflight.jsonl
   ```

Reference (AMD Ryzen 9 9950X, Release, gcc 13.3, idle host, 2026-10-28):

| reading | value |
|---|---|
| `--calib` cpu_ns_per_iter | 1.18 |
| `--calib` dvs_solve_us | 0.74 |
| `--calib` dvs_compile_us | 4.3 |
| construct.range | 15 us/call |
| scenario.axi4_burst | 13 us/call |
| scenario.bus_transaction | 7 us/call |
| scenario.reset_reuse | 3 us/call |
| memmap.soc_mem_map | 39 us/call |
| dist.weighted | 52 us/call |

The calib numbers scale with host speed. If the smoke results are more than
~3x off the reference **after** scaling by `dvs_solve_us`, stop: the build or
the setup is wrong, not the host. Send the preflight JSONL; its records hold
what is needed to find out why (next section).

## Full matrix

One Verilator config over all 100 benches takes ~15 min at N=200 (build ~8 s per
bench). To keep each run short, run it in two halves into the same file:

```
CFG=vlt540-z3
PYTHONPATH=src python3 -m pytest -q --simbench-out=$CFG.jsonl \
    -k "$CFG and (arith or array or construct or control)"
PYTHONPATH=src python3 -m pytest -q --simbench-out=$CFG.jsonl \
    -k "$CFG and not (arith or array or construct or control)"
```

`--simbench-out` appends, so a rerun adds records; the report keeps the last
record for each (config, bench, host).

## Outputs and run settings

- `--simbench-out=x.jsonl`: one JSON record per test (dv-solve
  `tests/perf/schema.py` run record). It holds the config, bench, bench source
  sha, simbench git rev (and whether the tree was dirty), simulator version,
  host/CPU, N and seed, the result (ns per randomize call) or the failure reason,
  and randomization quality metrics. For troubleshooting a mismatch it also holds:
  - `tools.solver_build`, `tools.replay_build`: for dv-solve binaries, their
    `--build-info` (dv-solve commit and dirty state at build time, build type,
    optimized, asan, compiler), the library file each really loaded under the
    run's environment, and sha256 of binary and library.
  - `tools.env`: environment variables that change the simulator or solver
    (`DV_*`, `VERILATOR*`, `SIMBENCH_*`, `LD_LIBRARY_PATH`, `LD_PRELOAD`, `PATH`, ...).
  - `machine`: also CPU governor, mean and max clock, CPUs in the affinity
    mask, pytest-xdist workers, and the load average.
  - `calib`: fixed work timed just before the run: a Python loop (`py_loop_ms`)
    and, when a `bench_vlt_replay` is available, its `--calib` (`cpu_ns_per_iter`,
    `dvs_solve_us`, `dvs_compile_us`) and the library it loaded.
  - replay configs: `simbench.session` (size, sha256, check-sats of the captured
    solver session) and `simbench.replay` (arguments, mode, sessions and calls,
    warm-up, distinct/reused problems, escalations, failures, build/compile/solve
    split, reused vs new context time, and the replay's own build info).
- `SIMBENCH_N` (default 200): randomize() calls per run. `SIMBENCH_SEED` (default 1).
- `SIMBENCH_HOST=<label>`: a readable machine name for the records (default: the machine id).
- `SIMBENCH_DUMP_DIR=<dir>`: keep the randomized values in `<dir>/<config>/<bench>.txt`;
  chi-square is then computed against the `SIMBENCH_REF_CONFIG` (default
  `vlt-z3-default`) dump if one is there.
- `SIMBENCH_DIVERSITY=1`: 20 extra single-call runs per test to measure first-value diversity.
- `SIMBENCH_SESSION_DIR=<dir>`: keep each captured solver session as
  `<dir>/<config>/<bench>.smt2.gz` (replay configs), so a disputed number can be
  replayed on another machine with exactly the same problems.

## What to send back

The JSONL file(s). Please set `SIMBENCH_HOST` to something that names the
machine, and keep the default N and seed unless asked otherwise. For replay
configs, also run with `SIMBENCH_SESSION_DIR` set and send that directory
(compressed sessions, ~1-2 MB for all benches), and the preflight JSONL.
Run the matrix with the documented pytest commands; if you use a wrapper
script, send it too, so the way the numbers were produced is on record.
Commercial-simulator license terms may restrict publishing benchmark numbers;
check them before sharing results outside your organization.

## Comparing results

Merge any number of JSONL files (from one or many machines) into a markdown
table, one row per bench and one column per `config@host`, with us/call and the
ratio to a reference config, plus the geometric mean of the ratios:

```
PYTHONPATH=src python3 -m dv_solve_simbench.report a.jsonl b.jsonl --ref vlt540-z3 -o report.md
```
