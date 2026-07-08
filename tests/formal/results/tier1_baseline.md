# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 2.2 | sat | 42240 | 0.6 | sat | 42240 | 2.6 | sat | 42496 | 5.1 | sat | 42240 |
| arrayordering | 3.0 | unsat | 42240 | 1.5 | unsat | 42240 | 2.9 | unsat | 42496 | 8.1 | unsat | 42240 |
| arraysum8 | 2.2 | sat | 42240 | 4.8 | sat | 42240 | 2.9 | sat | 42496 | 7.5 | sat | 42240 |
| axi4burst | 2.1 | sat | 42240 | 1.9 | sat | 42240 | 2.4 | sat | 42496 | 6.3 | sat | 42240 |
| bustransaction | 1.8 | sat | 42240 | 0.9 | sat | 42240 | 2.5 | sat | 42496 | 6.4 | sat | 42240 |
| condinside | 2.0 | sat | 42240 | 1.0 | sat | 42240 | 2.7 | sat | 42496 | 6.1 | sat | 42240 |
| ddr5cmdbasic | 1.8 | sat | 42240 | 0.8 | sat | 42240 | 2.9 | sat | 42496 | 6.1 | sat | 42240 |
| ddr5moderegister | 1.9 | sat | 42240 | 0.6 | sat | 42240 | 2.5 | sat | 42496 | 6.3 | sat | 42240 |
| ddr5timing | 2.0 | sat | 42240 | 1.4 | sat | 42240 | 2.5 | sat | 42496 | 6.8 | sat | 42240 |
| distweighted | 1.6 | sat | 42240 | 0.4 | sat | 42240 | 2.5 | sat | 42496 | 4.6 | sat | 42240 |
| enumcond | 2.1 | sat | 42240 | 6.1 | sat | 42240 | 2.8 | sat | 42496 | 6.6 | sat | 42240 |
| fifoctrl | 1.7 | sat | 42240 | 0.6 | sat | 42240 | 2.4 | sat | 42496 | 4.9 | sat | 42240 |
| implicationchain8 | 2.1 | sat | 42240 | 1.0 | sat | 42240 | 2.6 | sat | 42496 | 6.3 | sat | 42240 |
| inequalityweb | 2.0 | sat | 42240 | 2.9 | sat | 42240 | 2.7 | sat | 42496 | 5.9 | sat | 42240 |
| memmaptight32 | 10.6 | sat | 42240 | 112.4 | sat | 42240 | 3.2 | sat | 42496 | 37.8 | sat | 256 |
| mempartitionknapsack | 4.2 | sat | 42240 | 9.5 | sat | 42240 | 5.1 | sat | 42496 | 10.3 | sat | 42496 |
| memtransaction | 1.8 | sat | 42240 | 1.4 | sat | 42240 | 2.7 | sat | 42496 | 5.6 | sat | 42496 |
| muldivscenario | 2.2 | sat | 42240 | 11.3 | sat | 42240 | 2.6 | sat | 42496 | 6.8 | sat | 42496 |
| nqueens8 | 4.8 | sat | 42240 | 10.4 | sat | 42240 | 2.9 | sat | 42496 | 6.9 | sat | 42496 |
| onehot8 | 1.6 | sat | 42240 | 0.9 | sat | 42240 | 2.4 | sat | 42496 | 4.8 | sat | 42496 |
| packethdr | 1.7 | sat | 42240 | 1.2 | sat | 42240 | 3.3 | sat | 42496 | 5.5 | sat | 42496 |
| pcietlp | 1.9 | sat | 42240 | 0.9 | sat | 42240 | 2.5 | sat | 42496 | 6.1 | sat | 42496 |
| shiftaligned | 1.7 | sat | 42240 | 0.6 | sat | 42240 | 2.5 | sat | 42496 | 4.8 | sat | 42496 |
| socaddrmap32 | 4.6 | sat | 42240 | 51.7 | sat | 42240 | 2.5 | sat | 42496 | 11.7 | sat | 42496 |
| socaddrmap40 | 4.8 | sat | 42240 | 58.4 | sat | 42240 | 2.4 | sat | 42496 | 14.9 | sat | 42496 |
| socmemmap | 2.6 | sat | 42240 | 11.1 | sat | 42240 | 3.0 | sat | 42496 | 7.9 | sat | 42496 |
| softrelaxbaseline | 1.5 | sat | 42240 | 0.9 | sat | 42240 | 2.6 | sat | 42496 | 6.5 | sat | 42496 |
| softrelaxwithconflict | 1.6 | sat | 42240 | 1.3 | sat | 42240 | 2.5 | sat | 42496 | 6.0 | sat | 42496 |
| sumpartition | 3.0 | sat | 42240 | 9.4 | sat | 42240 | 2.5 | sat | 42496 | 8.2 | sat | 42496 |
| threeunique | 1.8 | sat | 42240 | 1.4 | sat | 42240 | 2.4 | sat | 42496 | 6.4 | sat | 42496 |
| unique16 | 4.0 | sat | 42240 | 19.5 | sat | 42240 | 2.5 | sat | 42496 | 8.2 | sat | 42496 |
| unique32 | 1.7 | sat | 42240 | 1.0 | sat | 42240 | 2.5 | sat | 42496 | 5.0 | sat | 42496 |
| unsignedops | 2.0 | sat | 42240 | 1.0 | sat | 42240 | 2.4 | sat | 42496 | 6.6 | sat | 42496 |
| verilatorops | 1.7 | sat | 42240 | 2.4 | sat | 42240 | 2.6 | sat | 42496 | 6.7 | sat | 42496 |
