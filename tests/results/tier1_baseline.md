# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.3 | unknown | 36864 | 2.5 | unsat | 37376 | 4.8 | unsat | 37376 |
| cache_direct_1way_bmc_d2 | 2.2 | unknown | 512 | 2.6 | unsat | 37376 | 4.9 | unsat | 37376 |
| cache_direct_1way_bmc_d4 | 2.9 | unknown | 37376 | 3.7 | unsat | 37376 | 5.4 | unsat | 37376 |
| cache_direct_1way_bmc_d8 | 2.5 | unknown | 37376 | 5.4 | unsat | 37376 | 5.9 | unsat | 37376 |
| dma_engine_small_bmc_d1 | 2.4 | unsat | 37376 | 0.6 | unsat | 37376 | 4.5 | unsat | 37376 |
| dma_engine_small_bmc_d2 | 1.7 | sat | 37376 | 0.9 | sat | 37376 | 4.4 | sat | 37376 |
| dma_engine_small_bmc_d4 | 2.2 | unsat | 37376 | 1.4 | unsat | 37376 | 4.6 | unsat | 37376 |
| dma_engine_small_bmc_d8 | 2.4 | unsat | 37376 | 1.5 | unsat | 37376 | 5.5 | unsat | 37376 |
| fifo_8x16_bmc_d1 | 2.1 | unsat | 37376 | 0.9 | unsat | 37376 | 5.0 | unsat | 37376 |
| fifo_8x16_bmc_d2 | 2.5 | unsat | 37376 | 1.1 | unsat | 37376 | 5.0 | unsat | 37376 |
| fifo_8x16_bmc_d4 | 3.1 | unsat | 37376 | 2.4 | unsat | 37376 | 5.7 | unsat | 37376 |
| fifo_8x16_bmc_d8 | 4.7 | unsat | 37376 | 2.5 | unsat | 37376 | 12.7 | unsat | 37376 |
| regfile_addr_alias_bmc_d1 | 1.8 | unsat | 37376 | 1.6 | unsat | 37376 | 4.6 | unsat | 37376 |
| regfile_addr_alias_bmc_d2 | 2.0 | unsat | 37376 | 1.6 | unsat | 37376 | 4.2 | unsat | 37376 |
| regfile_addr_alias_bmc_d4 | 2.0 | unsat | 37376 | 1.9 | unsat | 37376 | 5.4 | unsat | 37376 |
| regfile_addr_alias_bmc_d8 | 2.5 | unsat | 37376 | 2.9 | unsat | 37376 | 4.9 | unsat | 37376 |
| regfile_simple_bmc_d1 | 1.8 | unsat | 37376 | 1.1 | unsat | 37376 | 4.8 | unsat | 37376 |
| regfile_simple_bmc_d2 | 1.7 | unsat | 37376 | 1.4 | unsat | 37376 | 5.4 | unsat | 37376 |
| regfile_simple_bmc_d4 | 1.8 | unsat | 37376 | 1.7 | unsat | 37376 | 5.3 | unsat | 37376 |
| regfile_simple_bmc_d8 | 2.1 | unsat | 37376 | 2.8 | unsat | 37376 | 6.1 | unsat | 37376 |
| wide_datapath_128_bmc_d1 | 1.7 | unsat | 37376 | 0.5 | unsat | 37376 | 4.2 | unsat | 37376 |
| wide_datapath_128_bmc_d2 | 2.7 | unsat | 37376 | 0.6 | unsat | 37376 | 3.8 | unsat | 37376 |
| wide_datapath_128_bmc_d4 | 2.6 | unsat | 37376 | 0.6 | unsat | 37376 | 4.3 | unsat | 37376 |
| wide_datapath_128_bmc_d8 | 3.5 | unsat | 37376 | 0.6 | unsat | 37376 | 4.2 | unsat | 37376 |
