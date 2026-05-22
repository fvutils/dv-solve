# Tier 1 Baseline Results

| Benchmark | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|
| cache_direct_1way_bmc_d1 | 3.9 | sat | 35584 | 5.8 | unsat | 35584 |
| cache_direct_1way_bmc_d2 | 4.9 | sat | 512 | 6.0 | unsat | 35584 |
| cache_direct_1way_bmc_d4 | 3.9 | sat | 36096 | 5.1 | unsat | 35584 |
| cache_direct_1way_bmc_d8 | 54.4 | error | 1684 | 6.7 | unsat | 35584 |
| dma_engine_small_bmc_d1 | 4.0 | sat | 37780 | 5.8 | unsat | 35584 |
| dma_engine_small_bmc_d2 | 3.2 | sat | 37780 | 5.9 | sat | 35584 |
| dma_engine_small_bmc_d4 | 3.3 | sat | 37780 | 5.1 | unsat | 35584 |
| dma_engine_small_bmc_d8 | 3.4 | sat | 37780 | 7.4 | unsat | 35584 |
| fifo_8x16_bmc_d1 | 3.0 | unsat | 37780 | 6.1 | unsat | 35584 |
| fifo_8x16_bmc_d2 | 3.5 | unsat | 37780 | 7.0 | unsat | 35584 |
| fifo_8x16_bmc_d4 | 3.5 | unsat | 37780 | 6.3 | unsat | 35584 |
| fifo_8x16_bmc_d8 | 4.5 | unsat | 37780 | 11.3 | unsat | 35584 |
| regfile_addr_alias_bmc_d1 | 2.8 | sat | 37780 | 5.4 | unsat | 35584 |
| regfile_addr_alias_bmc_d2 | 3.1 | sat | 37780 | 4.9 | unsat | 35584 |
| regfile_addr_alias_bmc_d4 | 3.0 | sat | 37780 | 5.4 | unsat | 35584 |
| regfile_addr_alias_bmc_d8 | 3.3 | sat | 37780 | 5.5 | unsat | 35584 |
| regfile_simple_bmc_d1 | 3.4 | sat | 37780 | 4.9 | unsat | 35584 |
| regfile_simple_bmc_d2 | 3.1 | sat | 37780 | 4.4 | unsat | 35584 |
| regfile_simple_bmc_d4 | 3.0 | sat | 37780 | 5.3 | unsat | 35584 |
| regfile_simple_bmc_d8 | 3.4 | sat | 37780 | 5.4 | unsat | 35584 |
| wide_datapath_128_bmc_d1 | 2.9 | unsat | 37780 | 4.5 | unsat | 35584 |
| wide_datapath_128_bmc_d2 | 2.9 | unsat | 37780 | 4.0 | unsat | 35584 |
| wide_datapath_128_bmc_d4 | 2.7 | unsat | 37780 | 4.3 | unsat | 35584 |
| wide_datapath_128_bmc_d8 | 2.9 | unsat | 37780 | 4.1 | unsat | 35584 |
