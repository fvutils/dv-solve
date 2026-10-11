# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.0 | unknown | 12213748 | 2.6 | unsat | 12213748 | 5.6 | unsat | 12213748 |
| cache_direct_1way_bmc_d2 | 2.3 | unknown | 12213748 | 3.2 | unsat | 12213748 | 5.2 | unsat | 12213748 |
| cache_direct_1way_bmc_d4 | 1.9 | unknown | 12213748 | 3.4 | unsat | 12213748 | 5.5 | unsat | 12213748 |
| cache_direct_1way_bmc_d8 | 2.5 | unknown | 12213748 | 7.1 | unsat | 12213748 | 6.2 | unsat | 12213748 |
| dma_engine_small_bmc_d1 | 2.0 | unsat | 12213748 | 0.8 | unsat | 12213748 | 4.4 | unsat | 12213748 |
| dma_engine_small_bmc_d2 | 2.1 | sat | 12213748 | 0.9 | sat | 12213748 | 5.6 | sat | 12213748 |
| dma_engine_small_bmc_d4 | 2.1 | unsat | 12213748 | 1.1 | unsat | 12213748 | 4.5 | unsat | 12213748 |
| dma_engine_small_bmc_d8 | 2.4 | unsat | 12213748 | 1.4 | unsat | 12213748 | 5.0 | unsat | 12213748 |
| fifo_8x16_bmc_d1 | 2.4 | unsat | 12213748 | 0.8 | unsat | 12213748 | 4.4 | unsat | 12213748 |
| fifo_8x16_bmc_d2 | 2.5 | unsat | 12213748 | 0.8 | unsat | 12213748 | 4.8 | unsat | 12213748 |
| fifo_8x16_bmc_d4 | 3.6 | unsat | 12213748 | 1.2 | unsat | 12213748 | 6.0 | unsat | 12213748 |
| fifo_8x16_bmc_d8 | 4.7 | unsat | 12213748 | 2.0 | unsat | 12213748 | 13.7 | unsat | 12213748 |
| regfile_addr_alias_bmc_d1 | 1.8 | unsat | 12213748 | 1.7 | unsat | 12213748 | 5.0 | unsat | 12213748 |
| regfile_addr_alias_bmc_d2 | 2.0 | unsat | 12213748 | 2.2 | unsat | 12213748 | 5.3 | unsat | 12213748 |
| regfile_addr_alias_bmc_d4 | 2.1 | unsat | 12213748 | 3.1 | unsat | 12213748 | 5.0 | unsat | 12213748 |
| regfile_addr_alias_bmc_d8 | 2.0 | unsat | 12213748 | 3.7 | unsat | 12213748 | 6.1 | unsat | 12213748 |
| regfile_simple_bmc_d1 | 1.9 | unsat | 12213748 | 1.7 | unsat | 12213748 | 4.4 | unsat | 12213748 |
| regfile_simple_bmc_d2 | 1.9 | unsat | 12213748 | 1.6 | unsat | 12213748 | 4.4 | unsat | 12213748 |
| regfile_simple_bmc_d4 | 2.2 | unsat | 12213748 | 1.9 | unsat | 12213748 | 4.6 | unsat | 12213748 |
| regfile_simple_bmc_d8 | 2.7 | unsat | 12213748 | 2.8 | unsat | 12213748 | 6.0 | unsat | 12213748 |
| wide_datapath_128_bmc_d1 | 2.0 | unsat | 12213748 | 0.7 | unsat | 12213748 | 3.9 | unsat | 12213748 |
| wide_datapath_128_bmc_d2 | 2.2 | unsat | 12213748 | 0.7 | unsat | 12213748 | 4.8 | unsat | 12213748 |
| wide_datapath_128_bmc_d4 | 2.2 | unsat | 12213748 | 0.7 | unsat | 12213748 | 4.5 | unsat | 12213748 |
| wide_datapath_128_bmc_d8 | 3.4 | unsat | 12213748 | 0.7 | unsat | 12213748 | 4.4 | unsat | 12213748 |
