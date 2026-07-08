# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.2 | unknown | 42496 | 14.2 | unsat | 42496 | 4.7 | unsat | 42496 |
| cache_direct_1way_bmc_d2 | 2.4 | unknown | 42496 | 16.6 | unsat | 42496 | 5.4 | unsat | 42496 |
| cache_direct_1way_bmc_d4 | 2.2 | unknown | 42496 | 20.8 | unsat | 42496 | 5.3 | unsat | 42496 |
| cache_direct_1way_bmc_d8 | 2.6 | unknown | 42496 | 26.4 | unsat | 42496 | 6.0 | unsat | 42496 |
| dma_engine_small_bmc_d1 | 1.9 | unsat | 42496 | 2.5 | unsat | 42496 | 4.6 | unsat | 42496 |
| dma_engine_small_bmc_d2 | 2.0 | sat | 42496 | 2.5 | sat | 42496 | 5.0 | sat | 42496 |
| dma_engine_small_bmc_d4 | 2.1 | unsat | 42496 | 2.9 | unsat | 42496 | 4.9 | unsat | 42496 |
| dma_engine_small_bmc_d8 | 2.5 | unsat | 42496 | 3.5 | unsat | 42496 | 5.4 | unsat | 42496 |
| fifo_8x16_bmc_d1 | 2.0 | unsat | 42496 | 3.2 | unsat | 42496 | 4.3 | unsat | 42496 |
| fifo_8x16_bmc_d2 | 2.2 | unsat | 42496 | 3.3 | unsat | 42496 | 5.4 | unsat | 42496 |
| fifo_8x16_bmc_d4 | 3.7 | unsat | 42496 | 4.3 | unsat | 42496 | 6.3 | unsat | 42496 |
| fifo_8x16_bmc_d8 | 4.5 | unsat | 42496 | 17.4 | unsat | 42496 | 10.2 | unsat | 42496 |
| regfile_addr_alias_bmc_d1 | 1.9 | unsat | 42496 | 14.6 | unsat | 42496 | 5.2 | unsat | 42496 |
| regfile_addr_alias_bmc_d2 | 1.9 | unsat | 42496 | 14.8 | unsat | 42496 | 5.1 | unsat | 42496 |
| regfile_addr_alias_bmc_d4 | 2.0 | unsat | 42496 | 15.5 | unsat | 42496 | 4.4 | unsat | 42496 |
| regfile_addr_alias_bmc_d8 | 2.5 | unsat | 42496 | 16.6 | unsat | 42496 | 5.2 | unsat | 42496 |
| regfile_simple_bmc_d1 | 1.8 | unsat | 42496 | 11.3 | unsat | 42496 | 4.0 | unsat | 42496 |
| regfile_simple_bmc_d2 | 1.9 | unsat | 42496 | 13.1 | unsat | 42496 | 6.0 | unsat | 42496 |
| regfile_simple_bmc_d4 | 2.0 | unsat | 42496 | 14.0 | unsat | 42496 | 5.9 | unsat | 42496 |
| regfile_simple_bmc_d8 | 2.0 | unsat | 42496 | 17.0 | unsat | 42496 | 5.8 | unsat | 42496 |
| wide_datapath_128_bmc_d1 | 2.1 | unsat | 42496 | 2.8 | unsat | 42496 | 3.7 | unsat | 42496 |
| wide_datapath_128_bmc_d2 | 2.2 | unsat | 42496 | 3.0 | unsat | 42496 | 4.4 | unsat | 42496 |
| wide_datapath_128_bmc_d4 | 2.9 | unsat | 42496 | 2.8 | unsat | 42496 | 4.6 | unsat | 42496 |
| wide_datapath_128_bmc_d8 | 3.3 | unsat | 42496 | 3.2 | unsat | 42496 | 4.4 | unsat | 42496 |
