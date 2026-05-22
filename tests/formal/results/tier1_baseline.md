# Tier 1 Baseline Results

| Benchmark | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 1.0 | sat | 36096 | 3.4 | sat | 42240 | 5.0 | sat | 36096 |
| arrayordering | 2.2 | unsat | 36096 | 4.3 | sat | 42240 | 8.2 | unsat | 2048 |
| arraysum8 | 4.7 | sat | 36096 | 3.4 | sat | 42240 | 7.5 | sat | 38144 |
| axi4burst | 1.9 | sat | 36096 | 3.9 | sat | 42240 | 6.9 | sat | 38144 |
| bustransaction | 1.3 | sat | 36096 | 2.6 | sat | 42240 | 6.3 | sat | 38144 |
| condinside | 0.8 | sat | 36096 | 2.8 | sat | 42240 | 6.0 | sat | 38144 |
| ddr5cmdbasic | 1.0 | sat | 36096 | 3.1 | sat | 42240 | 6.5 | sat | 38144 |
| ddr5moderegister | 0.7 | sat | 36096 | 3.2 | sat | 42240 | 6.4 | sat | 38144 |
| ddr5timing | 1.6 | sat | 36096 | 3.0 | sat | 42240 | 5.9 | sat | 38144 |
| distweighted | 0.8 | sat | 36096 | 2.9 | sat | 42240 | 4.5 | sat | 38144 |
| enumcond | 6.2 | sat | 36096 | 3.9 | sat | 42240 | 7.1 | sat | 38144 |
| fifoctrl | 0.7 | sat | 36096 | 2.7 | sat | 42240 | 5.1 | sat | 38144 |
| implicationchain8 | 1.1 | sat | 36096 | 3.5 | sat | 42240 | 5.6 | sat | 38144 |
| inequalityweb | 3.0 | sat | 36096 | 2.9 | sat | 42240 | 6.2 | sat | 38144 |
| memmaptight32 | 116.8 | sat | 36096 | 6.8 | sat | 42240 | 39.6 | sat | 4096 |
| mempartitionknapsack | 9.0 | sat | 36096 | 3.9 | sat | 42240 | 9.3 | sat | 42240 |
| memtransaction | 1.2 | sat | 36096 | 3.7 | sat | 42240 | 7.5 | sat | 42240 |
| muldivscenario | 11.5 | sat | 36096 | 3.3 | sat | 42240 | 7.9 | sat | 42240 |
| nqueens8 | 10.3 | sat | 36096 | 3.9 | sat | 42240 | 8.6 | sat | 42240 |
| onehot8 | 0.6 | sat | 36096 | 2.8 | sat | 42240 | 5.2 | sat | 42240 |
| packethdr | 1.2 | sat | 36096 | 2.8 | sat | 42240 | 7.3 | sat | 42240 |
| pcietlp | 0.9 | sat | 36096 | 2.8 | sat | 42240 | 6.4 | sat | 42240 |
| shiftaligned | 0.6 | sat | 36096 | 2.7 | sat | 42240 | 4.6 | sat | 42240 |
| socaddrmap32 | 50.6 | sat | 36096 | 3.4 | sat | 42240 | 12.5 | sat | 42240 |
| socaddrmap40 | 56.8 | sat | 36096 | 3.2 | sat | 42240 | 14.2 | sat | 42240 |
| socmemmap | 11.6 | sat | 36096 | 3.0 | sat | 42240 | 7.7 | sat | 42240 |
| softrelaxbaseline | 0.7 | sat | 36096 | 2.6 | sat | 42240 | 7.1 | sat | 42240 |
| softrelaxwithconflict | 0.6 | sat | 36096 | 2.5 | sat | 42240 | 6.6 | sat | 42240 |
| sumpartition | 8.8 | sat | 36096 | 3.9 | sat | 42240 | 8.7 | sat | 42240 |
| threeunique | 1.5 | sat | 36096 | 2.6 | sat | 42240 | 5.9 | sat | 42240 |
| unique16 | 21.0 | sat | 36096 | 3.7 | sat | 42240 | 8.8 | sat | 42240 |
| unique32 | 1.0 | sat | 36096 | 3.1 | sat | 42240 | 4.8 | sat | 42240 |
| unsignedops | 1.0 | sat | 36096 | 3.6 | sat | 42240 | 7.2 | sat | 42240 |
| verilatorops | 2.5 | sat | 36096 | 2.6 | sat | 42240 | 6.6 | sat | 42240 |
