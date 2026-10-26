# simbench

Plain-SystemVerilog randomization benchmarks that run on any simulator
(Verilator with z3 or dv-solve, VCS, Xcelium, Questa, ...).

Self-contained: no imports of dv-solve internals. Results are JSONL records
compatible with dv-solve `tests/perf/schema.py`.

Layout:
- `src/dv_solve_simbench/share/sv/` — harness (`simbench_pkg.sv`, `simbench_top.sv`, `sb_time.c`)
- `benchmarks/<category>/<name>/{bench.sv,bench.yaml}` — one benchmark each
- `tests/` — pytest collection over config x bench x sweep point

## Benchmarks

100 benchmarks in 8 groups. Tier is standard unless marked;
select a tier with `pytest -m smoke|standard|stress`.

- **arith** (14): add_ovf, alignment, bitslice, countones, mod_align, mul_div_64, pow2, redops, shift_align, shift_width, sysfunc, unsigned_ops, verilator_operators, wide_add_64
- **array** (16): array_index, array_ordering, array_reduction, array_sum, assoc_basic, cls_arr_member, dyn_size, foreach_basic, queue_basic, sorted_increasing, sum_partition, sum_with, unique_16, unique_32, unique_dyn, unpacked_2d
- **construct** (15): bool_logic, cmp_web, enum_basic, enum_cond, eq_chain, inside_ranges, inside_set, ne_set, packed_struct, range (smoke), signed_mix, struct_fields, three_unique, width_mix, xor_parity
- **control** (12): cond_inside, constraint_mode, implication_chain_8, implication_set, inheritance, inline_with, ite_cond, rand_mode, soft_override_with, soft_relaxation, solve_before, solve_before_array
- **dist** (8): dist_enum, dist_if, dist_mixed, dist_range, dist_weight_div, randc_basic, randc_enum, weighted (smoke)
- **hard** (7): global_nested, graph_color_8, knapsack_small, magic_square_3, nqueens_8 (stress), sudoku_4x4, unique_64 (stress)
- **memmap** (8): addr_map_64, mem_partition_knapsack, memmap_tight_32 (stress), nonoverlap_regions_8, page_table_entry, soc_addr_map_32, soc_addr_map_40, soc_mem_map (smoke)
- **scenario** (20): ahb_burst, apb_xfer, axi4_burst (smoke), bus_transaction (smoke), ddr5_cmd_basic, ddr5_mode_register, ddr5_timing, ddr5_timing_full (stress), dma_desc, ethernet_hdr, fifo_ctrl, i2c_xfer, mem_transaction, pcie_tlp, reset_reuse (smoke), riscv_instr_mem, riscv_instr_rtype, riscv_pmp_cfg, tl_ul_item, usb_pkt

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
