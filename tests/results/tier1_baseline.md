# Tier 1 Baseline Results

| Benchmark | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 42.1 | unsat | 12729556 | 5.0 | unsat | 12729556 |
| cache_direct_1way_bmc_d2 | 46.9 | unsat | 12729556 | 5.3 | unsat | 12729556 |
| cache_direct_1way_bmc_d4 | 40.6 | unsat | 12729556 | 5.6 | unsat | 12729556 |
| cache_direct_1way_bmc_d8 | 41.5 | unsat | 12729556 | 6.0 | unsat | 12729556 |
| dma_engine_small_bmc_d1 | 22.5 | unsat | 12729556 | 4.5 | unsat | 12729556 |
| dma_engine_small_bmc_d2 | 1.1 | sat | 12729556 | 5.0 | sat | 12729556 |
| dma_engine_small_bmc_d4 | 22.9 | unsat | 12729556 | 4.9 | unsat | 12729556 |
| dma_engine_small_bmc_d8 | 23.0 | unsat | 12729556 | 6.2 | unsat | 12729556 |
| fifo_8x16_bmc_d1 | 0.8 | unsat | 12729556 | 5.0 | unsat | 12729556 |
| fifo_8x16_bmc_d2 | 1.0 | unsat | 12729556 | 4.5 | unsat | 12729556 |
| fifo_8x16_bmc_d4 | 7.2 | unsat | 12729556 | 5.5 | unsat | 12729556 |
| fifo_8x16_bmc_d8 | 3.8 | unsat | 12729556 | 13.0 | unsat | 12729556 |
| regfile_addr_alias_bmc_d1 | 36.1 | unsat | 12729556 | 4.3 | unsat | 12729556 |
| regfile_addr_alias_bmc_d2 | 51.3 | unsat | 12729556 | 4.7 | unsat | 12729556 |
| regfile_addr_alias_bmc_d4 | 68.3 | unsat | 12729556 | 4.7 | unsat | 12729556 |
| regfile_addr_alias_bmc_d8 | 101.0 | unsat | 12729556 | 4.9 | unsat | 12729556 |
| regfile_simple_bmc_d1 | 27.3 | unsat | 12729556 | 4.8 | unsat | 12729556 |
| regfile_simple_bmc_d2 | 26.2 | unsat | 12729556 | 5.0 | unsat | 12729556 |
| regfile_simple_bmc_d4 | 27.5 | unsat | 12729556 | 4.1 | unsat | 12729556 |
| regfile_simple_bmc_d8 | 27.5 | unsat | 12729556 | 5.6 | unsat | 12729556 |
| wide_datapath_128_bmc_d1 | 0.7 | unsat | 12729556 | 4.2 | unsat | 12729556 |
| wide_datapath_128_bmc_d2 | 0.9 | unsat | 12729556 | 3.9 | unsat | 12729556 |
| wide_datapath_128_bmc_d4 | 0.5 | unsat | 12729556 | 4.0 | unsat | 12729556 |
| wide_datapath_128_bmc_d8 | 0.8 | unsat | 12729556 | 5.0 | unsat | 12729556 |
