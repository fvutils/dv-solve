# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 2.3 | sat | 299300 | 0.7 | sat | 299300 | 1.2 | sat | 299300 | 4.6 | sat | 299300 |
| arrayordering | 2.7 | unsat | 299300 | 1.7 | unsat | 299300 | 1.0 | unsat | 299300 | 8.4 | unsat | 299300 |
| arraysum8 | 1.9 | sat | 299300 | 4.7 | sat | 299300 | 0.9 | sat | 299300 | 7.5 | sat | 299300 |
| axi4burst | 2.6 | sat | 299300 | 2.0 | sat | 299300 | 1.1 | sat | 299300 | 7.1 | sat | 299300 |
| bustransaction | 1.9 | sat | 299300 | 0.9 | sat | 299300 | 0.8 | sat | 299300 | 6.0 | sat | 299300 |
| condinside | 1.8 | sat | 299300 | 0.8 | sat | 299300 | 0.8 | sat | 299300 | 6.1 | sat | 299300 |
| ddr5cmdbasic | 1.5 | sat | 299300 | 1.0 | sat | 299300 | 0.8 | sat | 299300 | 6.8 | sat | 299300 |
| ddr5moderegister | 2.6 | sat | 299300 | 0.7 | sat | 299300 | 0.8 | sat | 299300 | 6.6 | sat | 299300 |
| ddr5timing | 2.0 | sat | 299300 | 1.6 | sat | 299300 | 0.8 | sat | 299300 | 6.8 | sat | 299300 |
| distweighted | 1.6 | sat | 299300 | 0.6 | sat | 299300 | 0.8 | sat | 299300 | 4.4 | sat | 299300 |
| enumcond | 2.1 | sat | 299300 | 5.9 | sat | 299300 | 0.8 | sat | 299300 | 6.8 | sat | 299300 |
| fifoctrl | 1.9 | sat | 299300 | 0.6 | sat | 299300 | 0.8 | sat | 299300 | 4.0 | sat | 299300 |
| implicationchain8 | 1.8 | sat | 299300 | 1.0 | sat | 299300 | 0.8 | sat | 299300 | 5.7 | sat | 299300 |
| inequalityweb | 2.5 | sat | 299300 | 3.0 | sat | 299300 | 0.8 | sat | 299300 | 6.9 | sat | 299300 |
| memmaptight32 | 10.5 | sat | 299300 | 113.3 | sat | 299300 | 1.7 | sat | 299300 | 38.1 | sat | 299300 |
| mempartitionknapsack | 4.0 | sat | 299300 | 9.5 | sat | 299300 | 3.6 | sat | 299300 | 9.9 | sat | 299300 |
| memtransaction | 1.8 | sat | 299300 | 1.2 | sat | 299300 | 1.0 | sat | 299300 | 6.7 | sat | 299300 |
| muldivscenario | 2.2 | sat | 299300 | 11.9 | sat | 299300 | 1.3 | sat | 299300 | 7.1 | sat | 299300 |
| nqueens8 | 5.4 | sat | 299300 | 10.9 | sat | 299300 | 1.8 | sat | 299300 | 8.9 | sat | 299300 |
| onehot8 | 1.6 | sat | 299300 | 0.9 | sat | 299300 | 0.8 | sat | 299300 | 5.0 | sat | 299300 |
| packethdr | 1.6 | sat | 299300 | 1.8 | sat | 299300 | 1.4 | sat | 299300 | 7.0 | sat | 299300 |
| pcietlp | 1.7 | sat | 299300 | 0.7 | sat | 299300 | 0.8 | sat | 299300 | 6.2 | sat | 299300 |
| shiftaligned | 1.7 | sat | 299300 | 0.9 | sat | 299300 | 0.8 | sat | 299300 | 4.6 | sat | 299300 |
| socaddrmap32 | 4.5 | sat | 299300 | 50.7 | sat | 299300 | 0.9 | sat | 299300 | 11.6 | sat | 299300 |
| socaddrmap40 | 5.0 | sat | 299300 | 60.0 | sat | 299300 | 0.9 | sat | 299300 | 14.3 | sat | 299300 |
| socmemmap | 2.4 | sat | 299300 | 10.6 | sat | 299300 | 0.9 | sat | 299300 | 8.6 | sat | 299300 |
| softrelaxbaseline | 1.6 | sat | 299300 | 0.8 | sat | 299300 | 0.8 | sat | 299300 | 5.7 | sat | 299300 |
| softrelaxwithconflict | 1.6 | sat | 299300 | 0.6 | sat | 299300 | 0.8 | sat | 299300 | 6.2 | sat | 299300 |
| sumpartition | 2.8 | sat | 299300 | 8.6 | sat | 299300 | 0.8 | sat | 299300 | 6.7 | sat | 299300 |
| threeunique | 1.7 | sat | 299300 | 1.7 | sat | 299300 | 0.8 | sat | 299300 | 6.7 | sat | 299300 |
| unique16 | 4.0 | sat | 299300 | 19.9 | sat | 299300 | 0.9 | sat | 299300 | 7.9 | sat | 299300 |
| unique32 | 1.6 | sat | 299300 | 0.8 | sat | 299300 | 0.8 | sat | 299300 | 4.1 | sat | 299300 |
| unsignedops | 1.9 | sat | 299300 | 1.0 | sat | 299300 | 0.8 | sat | 299300 | 7.6 | sat | 299300 |
| verilatorops | 2.1 | sat | 299300 | 2.4 | sat | 299300 | 0.8 | sat | 299300 | 5.9 | sat | 299300 |
