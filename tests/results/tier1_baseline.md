# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.1 | unknown | 443532 | 3.5 | unsat | 443532 | 5.4 | unsat | 443532 |
| cache_direct_1way_bmc_d2 | 1.8 | unknown | 443532 | 2.7 | unsat | 443532 | 5.2 | unsat | 443532 |
| cache_direct_1way_bmc_d4 | 1.9 | unknown | 443532 | 3.5 | unsat | 443532 | 5.5 | unsat | 443532 |
| cache_direct_1way_bmc_d8 | 2.3 | unknown | 443532 | 6.1 | unsat | 443532 | 6.0 | unsat | 443532 |
| dma_engine_small_bmc_d1 | 1.9 | unsat | 443532 | 0.7 | unsat | 443532 | 6.2 | unsat | 443532 |
| dma_engine_small_bmc_d2 | 1.8 | sat | 443532 | 1.3 | sat | 443532 | 5.7 | sat | 443532 |
| dma_engine_small_bmc_d4 | 1.8 | unsat | 443532 | 1.6 | unsat | 443532 | 7.0 | unsat | 443532 |
| dma_engine_small_bmc_d8 | 2.1 | unsat | 443532 | 1.3 | unsat | 443532 | 6.0 | unsat | 443532 |
| fifo_8x16_bmc_d1 | 1.8 | unsat | 443532 | 0.9 | unsat | 443532 | 5.3 | unsat | 443532 |
| fifo_8x16_bmc_d2 | 2.1 | unsat | 443532 | 0.7 | unsat | 443532 | 5.7 | unsat | 443532 |
| fifo_8x16_bmc_d4 | 3.5 | unsat | 443532 | 1.7 | unsat | 443532 | 6.9 | unsat | 443532 |
| fifo_8x16_bmc_d8 | 4.4 | unsat | 443532 | 2.9 | unsat | 443532 | 13.6 | unsat | 443532 |
| regfile_addr_alias_bmc_d1 | 1.7 | unsat | 443532 | 2.2 | unsat | 443532 | 4.9 | unsat | 443532 |
| regfile_addr_alias_bmc_d2 | 2.0 | unsat | 443532 | 2.3 | unsat | 443532 | 5.1 | unsat | 443532 |
| regfile_addr_alias_bmc_d4 | 1.8 | unsat | 443532 | 2.5 | unsat | 443532 | 4.8 | unsat | 443532 |
| regfile_addr_alias_bmc_d8 | 2.6 | unsat | 443532 | 3.1 | unsat | 443532 | 5.4 | unsat | 443532 |
| regfile_simple_bmc_d1 | 2.0 | unsat | 443532 | 1.3 | unsat | 443532 | 4.5 | unsat | 443532 |
| regfile_simple_bmc_d2 | 1.7 | unsat | 443532 | 1.5 | unsat | 443532 | 4.4 | unsat | 443532 |
| regfile_simple_bmc_d4 | 1.8 | unsat | 443532 | 2.0 | unsat | 443532 | 6.5 | unsat | 443532 |
| regfile_simple_bmc_d8 | 2.7 | unsat | 443532 | 2.6 | unsat | 443532 | 6.2 | unsat | 443532 |
| wide_datapath_128_bmc_d1 | 1.8 | unsat | 443532 | 0.7 | unsat | 443532 | 4.4 | unsat | 443532 |
| wide_datapath_128_bmc_d2 | 2.0 | unsat | 443532 | 0.6 | unsat | 443532 | 4.9 | unsat | 443532 |
| wide_datapath_128_bmc_d4 | 2.3 | unsat | 443532 | 0.6 | unsat | 443532 | 4.5 | unsat | 443532 |
| wide_datapath_128_bmc_d8 | 4.0 | unsat | 443532 | 0.6 | unsat | 443532 | 4.7 | unsat | 443532 |
