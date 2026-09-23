# Tier 1 Baseline Results

| Benchmark | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 2.9 | unsat | 302900 | 4.9 | unsat | 302900 |
| cache_direct_1way_bmc_d2 | 2.6 | unsat | 302900 | 5.3 | unsat | 302900 |
| cache_direct_1way_bmc_d4 | 3.4 | unsat | 302900 | 5.3 | unsat | 302900 |
| cache_direct_1way_bmc_d8 | 5.8 | unsat | 302900 | 5.7 | unsat | 302900 |
| dma_engine_small_bmc_d1 | 1.0 | unsat | 302900 | 4.7 | unsat | 302900 |
| dma_engine_small_bmc_d2 | 1.0 | sat | 302900 | 5.0 | sat | 302900 |
| dma_engine_small_bmc_d4 | 1.1 | unsat | 302900 | 4.9 | unsat | 302900 |
| dma_engine_small_bmc_d8 | 1.4 | unsat | 302900 | 6.2 | unsat | 302900 |
| fifo_8x16_bmc_d1 | 0.8 | unsat | 302900 | 5.6 | unsat | 302900 |
| fifo_8x16_bmc_d2 | 0.8 | unsat | 302900 | 5.7 | unsat | 302900 |
| fifo_8x16_bmc_d4 | 1.4 | unsat | 302900 | 5.4 | unsat | 302900 |
| fifo_8x16_bmc_d8 | 3.0 | unsat | 302900 | 11.2 | unsat | 302900 |
| regfile_addr_alias_bmc_d1 | 1.7 | unsat | 302900 | 5.9 | unsat | 302900 |
| regfile_addr_alias_bmc_d2 | 2.0 | unsat | 302900 | 5.3 | unsat | 302900 |
| regfile_addr_alias_bmc_d4 | 2.5 | unsat | 302900 | 5.4 | unsat | 302900 |
| regfile_addr_alias_bmc_d8 | 3.2 | unsat | 302900 | 6.6 | unsat | 302900 |
| regfile_simple_bmc_d1 | 1.3 | unsat | 302900 | 5.8 | unsat | 302900 |
| regfile_simple_bmc_d2 | 1.5 | unsat | 302900 | 5.6 | unsat | 302900 |
| regfile_simple_bmc_d4 | 2.0 | unsat | 302900 | 5.4 | unsat | 302900 |
| regfile_simple_bmc_d8 | 2.8 | unsat | 302900 | 6.0 | unsat | 302900 |
| wide_datapath_128_bmc_d1 | 0.7 | unsat | 302900 | 4.7 | unsat | 302900 |
| wide_datapath_128_bmc_d2 | 0.7 | unsat | 302900 | 3.8 | unsat | 302900 |
| wide_datapath_128_bmc_d4 | 0.7 | unsat | 302900 | 4.6 | unsat | 302900 |
| wide_datapath_128_bmc_d8 | 0.8 | unsat | 302900 | 4.7 | unsat | 302900 |
