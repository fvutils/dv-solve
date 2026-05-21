# Benchmark Suite

Throughput benchmarks for the zuspec-solver constraint engine.
Each test measures solves/sec across multiple solver backends.

## Running

```bash
# Run all benchmarks (default: 5s per scenario per backend)
pytest tests/bench/ -m bench

# Quick regression (2s target per benchmark)
pytest tests/bench/ -m bench --bench-target-secs=2

# Specific backend only
ZSP_SOLVERS=native pytest tests/bench/ -m bench
```

## Scenarios

**Core arithmetic**
- `test_sum_partition` -- sum constraint with 3 variables
- `test_mul_div_64` -- 64-bit multiply/divide
- `test_unsigned_ops` -- unsigned 32-bit operations in the high range
- `test_verilator_operators` -- mixed arithmetic + bitwise + comparison

**Constraint patterns**
- `test_mem_transaction` -- aligned memory transaction
- `test_bus_transaction` -- bus protocol constraints
- `test_inequality_web` -- chain of inequalities
- `test_ite_cond` -- ITE / conditional constraint throughput
- `test_cond_inside` -- conditional inside constraint
- `test_enum_cond` -- enum-based conditional

**Hardware models**
- `test_ethernet_hdr` -- Ethernet frame header
- `test_ddr5_cmd_basic` -- DDR5 command encoding
- `test_ddr5_mode_register` -- DDR5 mode register
- `test_ddr5_timing` -- DDR5 timing constraints
- `test_soc_addr_map_32` / `test_soc_addr_map_40` -- SoC address maps
- `test_soc_mem_map` -- SoC memory map
- `test_fifo_ctrl` -- FIFO control

**Solver lifecycle**
- `test_reset_reuse` -- solver_reset + re-solve vs full recompile

**New features (Sprints 6-8)**
- `test_soft_relaxation` -- soft constraint relaxation overhead
- `test_dist_weighted` -- distribution-weighted value selection
- `test_shift_align` -- shift-based alignment constraints
- `test_alignment` -- bitwise AND alignment

**Structural**
- `test_three_unique` -- AllDifferent on 3 variables
- `test_array_ordering` -- ordered array elements
- `test_memmap_tight_32` -- tight 32-bit memory map

## Interpreting Results

Each benchmark reports a JSON file in `results/` with:
- `n_solutions` -- number of solutions produced
- `bench_ns` -- total benchmark time in nanoseconds
- Derived: `solves_per_sec = n_solutions / (bench_ns / 1e9)`

Key metrics to watch:
- **Solves/sec** -- primary throughput metric
- **Relative performance** -- native vs python backend ratio (target: 10x+)
- **Regression** -- compare against stored `results/*.json` baselines
