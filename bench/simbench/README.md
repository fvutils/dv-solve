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

## What config C (`dvs-standalone`) measures

C captures config B's solver session (`+verilator+solver+file+`) and replays
it in-process with dv-solve's `bench_vlt_replay`. The SMT-LIB text is parsed
once, untimed, only to capture each check-sat's problem. The timed `api`
pass rebuilds each problem through the public `dvs_builder` API, compiles,
solves and reads every variable back. `api-cache` also reuses a compiled
context when a problem repeats (found by hash). C reports the faster pass.

So C is the *solver-side floor* of a direct-API integration (a lower bound
on per-call time), not an end-to-end number. It leaves out:
1. Verilator's own per-randomize cost (collecting variables and constraints,
   writing results back, the code that would call the builder);
2. any difference between the problems the SMT-LIB front end builds and what
   a native Verilator-to-builder lowering would emit;
3. per-call-site caching (a native integration could know a problem repeats
   without hashing it).

`api-cache` on `construct.range` gets slower as a context is reused (a
dv-solve issue), so C's number on that bench comes from the `api` pass.
