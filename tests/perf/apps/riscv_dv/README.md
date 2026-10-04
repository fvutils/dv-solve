# riscv-dv application suite

riscv-dv instruction generation under Verilator, measured end to end for
performance and random diversity. Design: `docs/riscv_dv_results_design.md`.

**Status: collection only.** `collect` writes the suite into the run record
(`app`, `app_summary`), but nothing trends, consolidates or renders it yet.
That waits for Verilator to release `+verilator+rand+sampler+`, which the
headline arm (`dv@solver`) needs. Until then the app Verilator is a local
build of the sampler-control tree.

## Running the nightly set

```
export PATH=<z3 5.1.0>/bin:$PATH                  # the pinned z3 (tests/perf/solvers.lock)
export DVS_APP_VERILATOR=~/projects/verilator/verilator-sampler-control   # built tree
export DVS_APP_UVM=<verilator bundle>/share/uvm   # holds src/uvm_pkg.sv
python3 -m tests.perf.collect --kind manual --suites calib,app-riscv-dv-core \
    [--riscv-dv-repo <local riscv-dv clone>] --out perf-out
python3 -m tests.perf.apps.riscv_dv.report perf-out/perf-manual-*.json.gz
```

- **Time:** about 20 minutes on an idle host, plus about 80 s per target the
  first time a generator is built.
- **Idle host:** the cells run one at a time, and timing under load is
  meaningless (load inflated the T2 sweep 2.5x).
- **Cell subset:** `DVS_APP_CELLS=rv32imc/riscv_rand_instr_test,...` runs
  only those cells. The manifest then says `subset`.
- **Run on an idle machine:** the run waits up to 2 minutes for the load
  average to drop below 1.0 after building. It records the load and marks
  the run `noisy` above 4.

## What runs

`tests/perf/suites/app-riscv-dv-core.json` lists the 8 cells and the arms.

| Arm | Runs per cell | Measures |
|---|---|---|
| `dv@solver`: dv-solve, `+verilator+rand+sampler+solver` | 3 timed (`solver_rusage`, not in the data path) and 1 recorded (`solver_tap`) | speed from the fastest timed run; values from the recorded one |
| `z3@verilator`: z3 5.1.0, Verilator's default sampler | 1 recorded | the tap costs a reference well under 1%, so one run gives both speed and values |

| File | Role |
|---|---|
| `build.py` | riscv-dv at the pinned commit plus `patches/P01-*`; a copy of the app Verilator's runtime plus `patches/V-P01-*`; verilate with UVM's DPI layer; V-P02 (`fix_nary.py`). Cached under `perf-builds/riscv-dv/` by input hash. |
| `run.py` | one generation: solver CPU accounting (it is a child subreaper, so a solver orphaned by a SIGKILLed wrapper is still counted), outcome, options-took-effect check, program statistics |
| `quality.py` / `dist.py` | program-level and value-level diversity |
| `suite.py` | every cell under every arm; summary, ratios, validity |
| `report.py` | Markdown from a run record |

**The run is invalid when:**
- a cell's options did not take effect (the T1 failure mode); or
- `dv@solver` fails a cell that `z3@verilator` passes.

**The summary is `degraded`** when `dv@solver`'s value spread falls below
z3's.
