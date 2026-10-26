# simbench

Plain-SystemVerilog randomization benchmarks that run on any simulator
(Verilator with z3 or dv-solve, VCS, Xcelium, Questa, ...).

Self-contained: no imports of dv-solve internals. Results are JSONL records
compatible with dv-solve `tests/perf/schema.py`.

Layout:
- `src/dv_solve_simbench/share/sv/` — harness (`simbench_pkg.sv`, `simbench_top.sv`, `sb_time.c`)
- `benchmarks/<category>/<name>/{bench.sv,bench.yaml}` — one benchmark each
- `tests/` — pytest collection over config x bench x sweep point

## Results

`pytest -m smoke --simbench-out=results.jsonl` appends one JSONL record per
bench run. Each is a dv-solve `tests/perf/schema.py` run record (schema 1,
kind `manual`); the config, bench, parameters and `SIMBENCH_RESULT` metrics
are under the `simbench` key.
