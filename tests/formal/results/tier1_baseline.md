# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 2.8 | sat | 41984 | 0.9 | sat | 41984 | 3.5 | sat | 41984 | 5.7 | sat | 41984 |
| arrayordering | 2.9 | unsat | 41984 | 2.0 | unsat | 41984 | 3.9 | unsat | 41984 | 9.4 | unsat | 41984 |
| arraysum8 | 2.5 | sat | 41984 | 5.3 | sat | 41984 | 4.3 | sat | 41984 | 8.4 | sat | 41984 |
| axi4burst | 1.7 | sat | 41984 | 1.4 | sat | 41984 | 4.1 | sat | 41984 | 7.4 | sat | 41984 |
| bustransaction | 2.2 | sat | 41984 | 1.0 | sat | 41984 | 4.0 | sat | 41984 | 8.0 | sat | 41984 |
| condinside | 1.4 | sat | 41984 | 0.7 | sat | 41984 | 3.6 | sat | 41984 | 6.7 | sat | 41984 |
| ddr5cmdbasic | 2.0 | sat | 41984 | 1.1 | sat | 41984 | 4.3 | sat | 41984 | 7.1 | sat | 41984 |
| ddr5moderegister | 1.8 | sat | 41984 | 0.6 | sat | 41984 | 3.4 | sat | 41984 | 7.3 | sat | 41984 |
| ddr5timing | 3.9 | sat | 41984 | 1.4 | sat | 41984 | 3.1 | sat | 41984 | 8.3 | sat | 41984 |
| distweighted | 2.0 | sat | 41984 | 0.8 | sat | 41984 | 3.2 | sat | 41984 | 5.5 | sat | 41984 |
| enumcond | 1.8 | sat | 41984 | 6.9 | sat | 41984 | 3.6 | sat | 41984 | 8.4 | sat | 41984 |
| fifoctrl | 1.4 | sat | 41984 | 0.5 | sat | 41984 | 3.4 | sat | 41984 | 5.8 | sat | 41984 |
| implicationchain8 | 1.6 | sat | 41984 | 1.1 | sat | 41984 | 3.8 | sat | 41984 | 8.5 | sat | 41984 |
| inequalityweb | 1.6 | sat | 41984 | 3.0 | sat | 41984 | 3.8 | sat | 41984 | 8.7 | sat | 41984 |
| memmaptight32 | 11.1 | sat | 41984 | 124.7 | sat | 41984 | 4.1 | sat | 41984 | 51.4 | sat | 41984 |
| mempartitionknapsack | 5.3 | sat | 41984 | 9.3 | sat | 41984 | 5.3 | sat | 41984 | 12.3 | sat | 41984 |
| memtransaction | 3.0 | sat | 41984 | 1.5 | sat | 41984 | 3.5 | sat | 41984 | 8.1 | sat | 41984 |
| muldivscenario | 2.3 | sat | 41984 | 13.0 | sat | 41984 | 2.9 | sat | 41984 | 11.2 | sat | 41984 |
| nqueens8 | 7.3 | sat | 41984 | 11.0 | sat | 41984 | 3.5 | sat | 41984 | 9.3 | sat | 41984 |
| onehot8 | 1.7 | sat | 41984 | 0.5 | sat | 41984 | 3.7 | sat | 41984 | 6.3 | sat | 41984 |
| packethdr | 2.0 | sat | 41984 | 1.1 | sat | 41984 | 3.5 | sat | 41984 | 7.1 | sat | 41984 |
| pcietlp | 1.5 | sat | 41984 | 1.0 | sat | 41984 | 3.4 | sat | 41984 | 10.6 | sat | 41984 |
| shiftaligned | 1.9 | sat | 41984 | 0.9 | sat | 41984 | 4.2 | sat | 41984 | 5.9 | sat | 41984 |
| socaddrmap32 | 4.1 | sat | 41984 | 51.0 | sat | 41984 | 3.9 | sat | 41984 | 13.3 | sat | 41984 |
| socaddrmap40 | 5.0 | sat | 41984 | 63.1 | sat | 41984 | 3.3 | sat | 41984 | 17.2 | sat | 41984 |
| socmemmap | 2.5 | sat | 41984 | 11.1 | sat | 41984 | 3.6 | sat | 41984 | 10.1 | sat | 41984 |
| softrelaxbaseline | 1.4 | sat | 41984 | 0.8 | sat | 41984 | 3.4 | sat | 41984 | 11.0 | sat | 41984 |
| softrelaxwithconflict | 3.4 | sat | 41984 | 0.7 | sat | 41984 | 3.9 | sat | 41984 | 7.5 | sat | 41984 |
| sumpartition | 2.8 | sat | 41984 | 9.5 | sat | 41984 | 3.4 | sat | 41984 | 10.1 | sat | 41984 |
| threeunique | 1.6 | sat | 41984 | 1.6 | sat | 41984 | 4.2 | sat | 41984 | 10.0 | sat | 41984 |
| unique16 | 3.9 | sat | 41984 | 20.7 | sat | 41984 | 4.2 | sat | 41984 | 8.9 | sat | 41984 |
| unique32 | 2.0 | sat | 41984 | 0.9 | sat | 41984 | 3.9 | sat | 41984 | 5.1 | sat | 41984 |
| unsignedops | 1.7 | sat | 41984 | 1.8 | sat | 41984 | 3.8 | sat | 41984 | 8.3 | sat | 41984 |
| verilatorops | 1.6 | sat | 41984 | 2.5 | sat | 41984 | 3.4 | sat | 41984 | 10.8 | sat | 41984 |
