# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.6 | unknown | 42480 | 14.0 | unsat | 42480 | 4.9 | unsat | 42480 |
| cache_direct_1way_bmc_d2 | 2.6 | unknown | 42480 | 18.1 | unsat | 42480 | 4.6 | unsat | 42480 |
| cache_direct_1way_bmc_d4 | 2.2 | unknown | 42480 | 21.5 | unsat | 42480 | 4.5 | unsat | 42480 |
| cache_direct_1way_bmc_d8 | 2.2 | unknown | 42480 | 26.8 | unsat | 42480 | 5.4 | unsat | 42480 |
| dma_engine_small_bmc_d1 | 1.6 | unsat | 42480 | 3.3 | unsat | 42480 | 4.4 | unsat | 42480 |
| dma_engine_small_bmc_d2 | 1.7 | sat | 42480 | 3.0 | sat | 42480 | 5.9 | sat | 42480 |
| dma_engine_small_bmc_d4 | 2.1 | unsat | 42480 | 4.3 | unsat | 42480 | 4.5 | unsat | 42480 |
| dma_engine_small_bmc_d8 | 2.3 | unsat | 42480 | 3.9 | unsat | 42480 | 5.4 | unsat | 42480 |
| fifo_8x16_bmc_d1 | 1.8 | unsat | 42480 | 3.0 | unsat | 42480 | 4.7 | unsat | 42480 |
| fifo_8x16_bmc_d2 | 2.7 | unsat | 42480 | 2.8 | unsat | 42480 | 5.0 | unsat | 42480 |
| fifo_8x16_bmc_d4 | 2.8 | unsat | 42480 | 6.4 | unsat | 42480 | 5.3 | unsat | 42480 |
| fifo_8x16_bmc_d8 | 4.9 | unsat | 42480 | 18.2 | unsat | 42480 | 11.5 | unsat | 42480 |
| regfile_addr_alias_bmc_d1 | 1.9 | unsat | 42480 | 16.5 | unsat | 42480 | 5.9 | unsat | 42480 |
| regfile_addr_alias_bmc_d2 | 1.6 | unsat | 42480 | 15.2 | unsat | 42480 | 4.7 | unsat | 42480 |
| regfile_addr_alias_bmc_d4 | 1.7 | unsat | 42480 | 14.8 | unsat | 42480 | 4.0 | unsat | 42480 |
| regfile_addr_alias_bmc_d8 | 2.6 | unsat | 42480 | 16.8 | unsat | 42480 | 6.8 | unsat | 42480 |
| regfile_simple_bmc_d1 | 2.1 | unsat | 42480 | 12.5 | unsat | 42480 | 4.8 | unsat | 42480 |
| regfile_simple_bmc_d2 | 1.7 | unsat | 42480 | 14.4 | unsat | 42480 | 4.6 | unsat | 42480 |
| regfile_simple_bmc_d4 | 1.9 | unsat | 42480 | 15.9 | unsat | 42480 | 4.4 | unsat | 42480 |
| regfile_simple_bmc_d8 | 2.1 | unsat | 42480 | 16.8 | unsat | 42480 | 4.8 | unsat | 42480 |
| wide_datapath_128_bmc_d1 | 2.3 | unsat | 42480 | 3.2 | unsat | 42480 | 3.6 | unsat | 42480 |
| wide_datapath_128_bmc_d2 | 1.9 | unsat | 42480 | 2.4 | unsat | 42480 | 4.8 | unsat | 42480 |
| wide_datapath_128_bmc_d4 | 2.9 | unsat | 42480 | 2.8 | unsat | 42480 | 4.6 | unsat | 42480 |
| wide_datapath_128_bmc_d8 | 3.5 | unsat | 42480 | 2.9 | unsat | 42480 | 3.4 | unsat | 42480 |
