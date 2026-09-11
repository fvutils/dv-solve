# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.7 | unknown | 299300 | 3.2 | unsat | 299300 | 5.1 | unsat | 299300 |
| cache_direct_1way_bmc_d2 | 2.3 | unknown | 299300 | 2.7 | unsat | 299300 | 5.1 | unsat | 299300 |
| cache_direct_1way_bmc_d4 | 2.1 | unknown | 299300 | 3.2 | unsat | 299300 | 6.2 | unsat | 299300 |
| cache_direct_1way_bmc_d8 | 2.6 | unknown | 299300 | 5.4 | unsat | 299300 | 5.9 | unsat | 299300 |
| dma_engine_small_bmc_d1 | 1.9 | unsat | 299300 | 0.8 | unsat | 299300 | 4.8 | unsat | 299300 |
| dma_engine_small_bmc_d2 | 2.0 | sat | 299300 | 1.5 | sat | 299300 | 5.8 | sat | 299300 |
| dma_engine_small_bmc_d4 | 2.1 | unsat | 299300 | 1.2 | unsat | 299300 | 5.0 | unsat | 299300 |
| dma_engine_small_bmc_d8 | 3.0 | unsat | 299300 | 1.4 | unsat | 299300 | 7.0 | unsat | 299300 |
| fifo_8x16_bmc_d1 | 2.1 | unsat | 299300 | 0.9 | unsat | 299300 | 5.0 | unsat | 299300 |
| fifo_8x16_bmc_d2 | 2.4 | unsat | 299300 | 1.0 | unsat | 299300 | 5.3 | unsat | 299300 |
| fifo_8x16_bmc_d4 | 3.3 | unsat | 299300 | 1.5 | unsat | 299300 | 6.4 | unsat | 299300 |
| fifo_8x16_bmc_d8 | 4.6 | unsat | 299300 | 3.0 | unsat | 299300 | 10.3 | unsat | 299300 |
| regfile_addr_alias_bmc_d1 | 1.9 | unsat | 299300 | 1.7 | unsat | 299300 | 5.2 | unsat | 299300 |
| regfile_addr_alias_bmc_d2 | 1.9 | unsat | 299300 | 2.0 | unsat | 299300 | 5.1 | unsat | 299300 |
| regfile_addr_alias_bmc_d4 | 2.1 | unsat | 299300 | 2.5 | unsat | 299300 | 5.5 | unsat | 299300 |
| regfile_addr_alias_bmc_d8 | 2.4 | unsat | 299300 | 3.1 | unsat | 299300 | 5.4 | unsat | 299300 |
| regfile_simple_bmc_d1 | 1.8 | unsat | 299300 | 1.3 | unsat | 299300 | 4.9 | unsat | 299300 |
| regfile_simple_bmc_d2 | 1.6 | unsat | 299300 | 1.6 | unsat | 299300 | 4.7 | unsat | 299300 |
| regfile_simple_bmc_d4 | 2.1 | unsat | 299300 | 2.0 | unsat | 299300 | 4.8 | unsat | 299300 |
| regfile_simple_bmc_d8 | 2.4 | unsat | 299300 | 2.7 | unsat | 299300 | 5.4 | unsat | 299300 |
| wide_datapath_128_bmc_d1 | 2.1 | unsat | 299300 | 0.7 | unsat | 299300 | 5.3 | unsat | 299300 |
| wide_datapath_128_bmc_d2 | 2.2 | unsat | 299300 | 0.7 | unsat | 299300 | 4.3 | unsat | 299300 |
| wide_datapath_128_bmc_d4 | 2.2 | unsat | 299300 | 0.9 | unsat | 299300 | 4.4 | unsat | 299300 |
| wide_datapath_128_bmc_d8 | 3.4 | unsat | 299300 | 1.1 | unsat | 299300 | 3.8 | unsat | 299300 |
