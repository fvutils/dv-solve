# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.6 | unknown | 41984 | 17.2 | unsat | 41984 | 6.0 | unsat | 41984 |
| cache_direct_1way_bmc_d2 | 2.3 | unknown | 41984 | 18.3 | unsat | 41984 | 5.6 | unsat | 41984 |
| cache_direct_1way_bmc_d4 | 1.9 | unknown | 41984 | 23.4 | unsat | 41984 | 7.9 | unsat | 41984 |
| cache_direct_1way_bmc_d8 | 2.3 | unknown | 41984 | 31.9 | unsat | 41984 | 7.6 | unsat | 41984 |
| dma_engine_small_bmc_d1 | 1.7 | unsat | 41984 | 3.8 | unsat | 41984 | 6.0 | unsat | 41984 |
| dma_engine_small_bmc_d2 | 1.8 | sat | 41984 | 3.7 | sat | 41984 | 5.4 | sat | 41984 |
| dma_engine_small_bmc_d4 | 1.9 | unsat | 41984 | 5.9 | unsat | 41984 | 5.6 | unsat | 41984 |
| dma_engine_small_bmc_d8 | 3.1 | unsat | 41984 | 5.3 | unsat | 41984 | 6.5 | unsat | 41984 |
| fifo_8x16_bmc_d1 | 2.3 | unsat | 41984 | 4.3 | unsat | 41984 | 5.4 | unsat | 41984 |
| fifo_8x16_bmc_d2 | 2.4 | unsat | 41984 | 3.6 | unsat | 41984 | 6.2 | unsat | 41984 |
| fifo_8x16_bmc_d4 | 3.7 | unsat | 41984 | 4.7 | unsat | 41984 | 6.9 | unsat | 41984 |
| fifo_8x16_bmc_d8 | 4.5 | unsat | 41984 | 21.6 | unsat | 41984 | 11.0 | unsat | 41984 |
| regfile_addr_alias_bmc_d1 | 1.6 | unsat | 41984 | 15.9 | unsat | 41984 | 6.2 | unsat | 41984 |
| regfile_addr_alias_bmc_d2 | 1.7 | unsat | 41984 | 16.0 | unsat | 41984 | 4.7 | unsat | 41984 |
| regfile_addr_alias_bmc_d4 | 1.8 | unsat | 41984 | 16.1 | unsat | 41984 | 6.1 | unsat | 41984 |
| regfile_addr_alias_bmc_d8 | 2.1 | unsat | 41984 | 19.1 | unsat | 41984 | 7.0 | unsat | 41984 |
| regfile_simple_bmc_d1 | 1.7 | unsat | 41984 | 14.5 | unsat | 41984 | 5.3 | unsat | 41984 |
| regfile_simple_bmc_d2 | 1.8 | unsat | 41984 | 16.8 | unsat | 41984 | 5.9 | unsat | 41984 |
| regfile_simple_bmc_d4 | 3.4 | unsat | 41984 | 18.7 | unsat | 41984 | 5.8 | unsat | 41984 |
| regfile_simple_bmc_d8 | 2.5 | unsat | 41984 | 21.2 | unsat | 41984 | 6.5 | unsat | 41984 |
| wide_datapath_128_bmc_d1 | 1.8 | unsat | 41984 | 4.4 | unsat | 41984 | 5.6 | unsat | 41984 |
| wide_datapath_128_bmc_d2 | 2.0 | unsat | 41984 | 3.7 | unsat | 41984 | 5.1 | unsat | 41984 |
| wide_datapath_128_bmc_d4 | 2.4 | unsat | 41984 | 5.6 | unsat | 41984 | 5.5 | unsat | 41984 |
| wide_datapath_128_bmc_d8 | 3.1 | unsat | 41984 | 3.8 | unsat | 41984 | 4.9 | unsat | 41984 |
