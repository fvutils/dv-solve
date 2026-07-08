# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 3.3 | unknown | 41728 | 15.7 | unsat | 41728 | 5.8 | unsat | 41728 |
| cache_direct_1way_bmc_d2 | 1.7 | unknown | 41728 | 19.7 | unsat | 41728 | 5.6 | unsat | 41728 |
| cache_direct_1way_bmc_d4 | 2.5 | unknown | 41728 | 21.4 | unsat | 41728 | 8.3 | unsat | 41728 |
| cache_direct_1way_bmc_d8 | 2.2 | unknown | 41728 | 30.8 | unsat | 41728 | 6.9 | unsat | 41728 |
| dma_engine_small_bmc_d1 | 2.1 | unsat | 41728 | 4.3 | unsat | 41728 | 5.7 | unsat | 41728 |
| dma_engine_small_bmc_d2 | 1.8 | sat | 41728 | 3.8 | sat | 41728 | 6.1 | sat | 41728 |
| dma_engine_small_bmc_d4 | 2.4 | unsat | 41728 | 3.8 | unsat | 41728 | 8.1 | unsat | 41728 |
| dma_engine_small_bmc_d8 | 2.3 | unsat | 41728 | 4.7 | unsat | 41728 | 6.7 | unsat | 41728 |
| fifo_8x16_bmc_d1 | 2.0 | unsat | 41728 | 3.5 | unsat | 41728 | 5.6 | unsat | 41728 |
| fifo_8x16_bmc_d2 | 2.2 | unsat | 41728 | 3.5 | unsat | 41728 | 5.8 | unsat | 41728 |
| fifo_8x16_bmc_d4 | 3.4 | unsat | 41728 | 4.6 | unsat | 41728 | 5.3 | unsat | 41728 |
| fifo_8x16_bmc_d8 | 5.0 | unsat | 41728 | 19.3 | unsat | 41728 | 14.9 | unsat | 41728 |
| regfile_addr_alias_bmc_d1 | 2.2 | unsat | 41728 | 16.9 | unsat | 41728 | 4.8 | unsat | 41728 |
| regfile_addr_alias_bmc_d2 | 2.2 | unsat | 41728 | 14.8 | unsat | 41728 | 6.1 | unsat | 41728 |
| regfile_addr_alias_bmc_d4 | 1.8 | unsat | 41728 | 18.2 | unsat | 41728 | 7.7 | unsat | 41728 |
| regfile_addr_alias_bmc_d8 | 2.8 | unsat | 41728 | 19.8 | unsat | 41728 | 6.7 | unsat | 41728 |
| regfile_simple_bmc_d1 | 1.6 | unsat | 41728 | 13.2 | unsat | 41728 | 6.3 | unsat | 41728 |
| regfile_simple_bmc_d2 | 2.2 | unsat | 41728 | 16.8 | unsat | 41728 | 5.8 | unsat | 41728 |
| regfile_simple_bmc_d4 | 3.2 | unsat | 41728 | 16.8 | unsat | 41728 | 5.0 | unsat | 41728 |
| regfile_simple_bmc_d8 | 2.4 | unsat | 41728 | 20.2 | unsat | 41728 | 6.1 | unsat | 41728 |
| wide_datapath_128_bmc_d1 | 2.3 | unsat | 41728 | 4.5 | unsat | 41728 | 5.3 | unsat | 41728 |
| wide_datapath_128_bmc_d2 | 2.1 | unsat | 41728 | 4.1 | unsat | 41728 | 7.0 | unsat | 41728 |
| wide_datapath_128_bmc_d4 | 2.4 | unsat | 41728 | 4.2 | unsat | 41728 | 6.0 | unsat | 41728 |
| wide_datapath_128_bmc_d8 | 3.1 | unsat | 41728 | 3.4 | unsat | 41728 | 5.7 | unsat | 41728 |
