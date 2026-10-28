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
  and randomization quality metrics.
- `SIMBENCH_N` (default 200): randomize() calls per run. `SIMBENCH_SEED` (default 1).
- `SIMBENCH_HOST=<label>`: a readable machine name for the records (default: the machine id).
- `SIMBENCH_DUMP_DIR=<dir>`: keep the randomized values in `<dir>/<config>/<bench>.txt`;
  chi-square is then computed against the `SIMBENCH_REF_CONFIG` (default
  `vlt-z3-default`) dump if one is there.
- `SIMBENCH_DIVERSITY=1`: 20 extra single-call runs per test to measure first-value diversity.

## What to send back

The JSONL file(s), nothing else. Please set `SIMBENCH_HOST` to something that
names the machine, and keep the default N and seed unless asked otherwise.
Commercial-simulator license terms may restrict publishing benchmark numbers;
check them before sharing results outside your organization.

## Comparing results

Merge any number of JSONL files (from one or many machines) into a markdown
table, one row per bench and one column per `config@host`, with us/call and the
ratio to a reference config, plus the geometric mean of the ratios:

```
PYTHONPATH=src python3 -m dv_solve_simbench.report a.jsonl b.jsonl --ref vlt540-z3 -o report.md
```
