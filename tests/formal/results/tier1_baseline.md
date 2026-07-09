# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 2.3 | sat | 42480 | 0.6 | sat | 42480 | 2.9 | sat | 42480 | 5.1 | sat | 42480 |
| arrayordering | 2.7 | unsat | 42480 | 1.6 | unsat | 42480 | 2.9 | unsat | 42480 | 7.1 | unsat | 42480 |
| arraysum8 | 2.4 | sat | 42480 | 5.2 | sat | 42480 | 2.7 | sat | 42480 | 7.7 | sat | 42480 |
| axi4burst | 1.8 | sat | 42480 | 1.9 | sat | 42480 | 3.0 | sat | 42480 | 7.0 | sat | 42480 |
| bustransaction | 2.1 | sat | 42480 | 0.7 | sat | 42480 | 2.8 | sat | 42480 | 7.7 | sat | 42480 |
| condinside | 1.9 | sat | 42480 | 0.7 | sat | 42480 | 2.6 | sat | 42480 | 6.1 | sat | 42480 |
| ddr5cmdbasic | 2.2 | sat | 42480 | 0.8 | sat | 42480 | 2.8 | sat | 42480 | 6.6 | sat | 42480 |
| ddr5moderegister | 1.5 | sat | 42480 | 1.0 | sat | 42480 | 2.7 | sat | 42480 | 6.2 | sat | 42480 |
| ddr5timing | 2.8 | sat | 42480 | 1.5 | sat | 42480 | 2.6 | sat | 42480 | 6.3 | sat | 42480 |
| distweighted | 1.6 | sat | 42480 | 0.8 | sat | 42480 | 3.1 | sat | 42480 | 4.6 | sat | 42480 |
| enumcond | 2.0 | sat | 42480 | 5.9 | sat | 42480 | 3.0 | sat | 42480 | 6.7 | sat | 42480 |
| fifoctrl | 2.0 | sat | 42480 | 0.5 | sat | 42480 | 2.8 | sat | 42480 | 4.5 | sat | 42480 |
| implicationchain8 | 2.3 | sat | 42480 | 0.9 | sat | 42480 | 2.8 | sat | 42480 | 5.9 | sat | 42480 |
| inequalityweb | 2.2 | sat | 42480 | 4.1 | sat | 42480 | 2.4 | sat | 42480 | 6.5 | sat | 42480 |
| memmaptight32 | 12.5 | sat | 42480 | 114.0 | sat | 42480 | 3.4 | sat | 42480 | 42.2 | sat | 42480 |
| mempartitionknapsack | 3.9 | sat | 42480 | 9.3 | sat | 42480 | 3.9 | sat | 42480 | 10.1 | sat | 42480 |
| memtransaction | 2.4 | sat | 42480 | 1.4 | sat | 42480 | 2.5 | sat | 42480 | 6.4 | sat | 42480 |
| muldivscenario | 2.2 | sat | 42480 | 11.5 | sat | 42480 | 2.4 | sat | 42480 | 8.2 | sat | 42480 |
| nqueens8 | 4.6 | sat | 42480 | 10.9 | sat | 42480 | 2.8 | sat | 42480 | 8.1 | sat | 42480 |
| onehot8 | 1.9 | sat | 42480 | 0.5 | sat | 42480 | 2.8 | sat | 42480 | 4.8 | sat | 42480 |
| packethdr | 1.8 | sat | 42480 | 1.6 | sat | 42480 | 2.8 | sat | 42480 | 7.3 | sat | 42480 |
| pcietlp | 2.1 | sat | 42480 | 1.2 | sat | 42480 | 2.6 | sat | 42480 | 5.8 | sat | 42480 |
| shiftaligned | 1.4 | sat | 42480 | 0.7 | sat | 42480 | 2.5 | sat | 42480 | 5.2 | sat | 42480 |
| socaddrmap32 | 4.8 | sat | 42480 | 50.3 | sat | 42480 | 2.7 | sat | 42480 | 14.3 | sat | 42480 |
| socaddrmap40 | 4.8 | sat | 42480 | 59.4 | sat | 42480 | 2.6 | sat | 42480 | 14.1 | sat | 42480 |
| socmemmap | 2.2 | sat | 42480 | 10.9 | sat | 42480 | 2.5 | sat | 42480 | 8.9 | sat | 42480 |
| softrelaxbaseline | 1.3 | sat | 42480 | 0.8 | sat | 42480 | 2.4 | sat | 42480 | 6.9 | sat | 42480 |
| softrelaxwithconflict | 1.6 | sat | 42480 | 0.8 | sat | 42480 | 2.4 | sat | 42480 | 6.3 | sat | 42480 |
| sumpartition | 2.4 | sat | 42480 | 9.1 | sat | 42480 | 2.4 | sat | 42480 | 7.7 | sat | 42480 |
| threeunique | 1.5 | sat | 42480 | 1.5 | sat | 42480 | 2.4 | sat | 42480 | 5.2 | sat | 42480 |
| unique16 | 4.6 | sat | 42480 | 20.2 | sat | 42480 | 2.5 | sat | 42480 | 6.8 | sat | 42480 |
| unique32 | 1.7 | sat | 42480 | 1.0 | sat | 42480 | 2.4 | sat | 42480 | 4.2 | sat | 42480 |
| unsignedops | 1.6 | sat | 42480 | 1.1 | sat | 42480 | 2.4 | sat | 42480 | 6.4 | sat | 42480 |
| verilatorops | 2.0 | sat | 42480 | 2.7 | sat | 42480 | 2.5 | sat | 42480 | 6.8 | sat | 42480 |
