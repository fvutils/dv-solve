# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 2.2 | sat | 17237248 | 0.9 | sat | 17237248 | 3.2 | sat | 17237248 | 4.9 | sat | 17237248 |
| arrayordering | 3.2 | unsat | 17237248 | 1.7 | unsat | 17237248 | 3.0 | unsat | 17237248 | 7.0 | unsat | 17237248 |
| arraysum8 | 2.1 | sat | 17237248 | 4.6 | sat | 17237248 | 2.4 | sat | 17237248 | 6.9 | sat | 17237248 |
| axi4burst | 2.5 | sat | 17237248 | 1.4 | sat | 17237248 | 3.2 | sat | 17237248 | 7.8 | sat | 17237248 |
| bustransaction | 1.8 | sat | 17237248 | 0.8 | sat | 17237248 | 2.6 | sat | 17237248 | 7.4 | sat | 17237248 |
| condinside | 1.6 | sat | 17237248 | 1.0 | sat | 17237248 | 3.4 | sat | 17237248 | 6.2 | sat | 17237248 |
| ddr5cmdbasic | 2.1 | sat | 17237248 | 0.9 | sat | 17237248 | 2.5 | sat | 17237248 | 6.6 | sat | 17237248 |
| ddr5moderegister | 2.2 | sat | 17237248 | 0.6 | sat | 17237248 | 2.7 | sat | 17237248 | 5.7 | sat | 17237248 |
| ddr5timing | 2.4 | sat | 17237248 | 1.4 | sat | 17237248 | 2.9 | sat | 17237248 | 6.6 | sat | 17237248 |
| distweighted | 1.5 | sat | 17237248 | 0.4 | sat | 17237248 | 2.3 | sat | 17237248 | 4.9 | sat | 17237248 |
| enumcond | 1.9 | sat | 17237248 | 5.8 | sat | 17237248 | 2.4 | sat | 17237248 | 5.9 | sat | 17237248 |
| fifoctrl | 1.8 | sat | 17237248 | 0.6 | sat | 17237248 | 2.2 | sat | 17237248 | 4.9 | sat | 17237248 |
| implicationchain8 | 1.6 | sat | 17237248 | 0.9 | sat | 17237248 | 2.3 | sat | 17237248 | 6.0 | sat | 17237248 |
| inequalityweb | 1.9 | sat | 17237248 | 3.0 | sat | 17237248 | 2.3 | sat | 17237248 | 6.3 | sat | 17237248 |
| memmaptight32 | 10.8 | sat | 17237248 | 110.4 | sat | 17237248 | 2.7 | sat | 17237248 | 42.3 | sat | 17237248 |
| mempartitionknapsack | 4.5 | sat | 17237248 | 9.2 | sat | 17237248 | 4.4 | sat | 17237248 | 9.9 | sat | 17237248 |
| memtransaction | 1.7 | sat | 17237248 | 1.4 | sat | 17237248 | 2.4 | sat | 17237248 | 7.4 | sat | 17237248 |
| muldivscenario | 2.0 | sat | 17237248 | 11.4 | sat | 17237248 | 2.3 | sat | 17237248 | 8.6 | sat | 17237248 |
| nqueens8 | 4.6 | sat | 17237248 | 11.1 | sat | 17237248 | 2.5 | sat | 17237248 | 7.7 | sat | 17237248 |
| onehot8 | 1.6 | sat | 17237248 | 0.8 | sat | 17237248 | 2.6 | sat | 17237248 | 4.8 | sat | 17237248 |
| packethdr | 2.2 | sat | 17237248 | 1.4 | sat | 17237248 | 2.4 | sat | 17237248 | 7.5 | sat | 17237248 |
| pcietlp | 2.5 | sat | 17237248 | 0.7 | sat | 17237248 | 2.3 | sat | 17237248 | 7.4 | sat | 17237248 |
| shiftaligned | 1.7 | sat | 17237248 | 0.5 | sat | 17237248 | 2.3 | sat | 17237248 | 4.5 | sat | 17237248 |
| socaddrmap32 | 4.5 | sat | 17237248 | 51.2 | sat | 17237248 | 2.3 | sat | 17237248 | 11.5 | sat | 17237248 |
| socaddrmap40 | 4.9 | sat | 17237248 | 60.6 | sat | 17237248 | 2.3 | sat | 17237248 | 15.8 | sat | 17237248 |
| socmemmap | 2.3 | sat | 17237248 | 10.7 | sat | 17237248 | 2.6 | sat | 17237248 | 8.5 | sat | 17237248 |
| softrelaxbaseline | 1.7 | sat | 17237248 | 0.7 | sat | 17237248 | 2.4 | sat | 17237248 | 6.2 | sat | 17237248 |
| softrelaxwithconflict | 1.3 | sat | 17237248 | 0.6 | sat | 17237248 | 2.3 | sat | 17237248 | 5.9 | sat | 17237248 |
| sumpartition | 2.5 | sat | 17237248 | 8.8 | sat | 17237248 | 2.7 | sat | 17237248 | 8.7 | sat | 17237248 |
| threeunique | 1.5 | sat | 17237248 | 2.3 | sat | 17237248 | 2.6 | sat | 17237248 | 6.6 | sat | 17237248 |
| unique16 | 3.7 | sat | 17237248 | 19.9 | sat | 17237248 | 2.8 | sat | 17237248 | 7.5 | sat | 17237248 |
| unique32 | 1.4 | sat | 17237248 | 0.9 | sat | 17237248 | 2.3 | sat | 17237248 | 4.2 | sat | 17237248 |
| unsignedops | 2.0 | sat | 17237248 | 1.4 | sat | 17237248 | 2.6 | sat | 17237248 | 6.2 | sat | 17237248 |
| verilatorops | 2.0 | sat | 17237248 | 2.2 | sat | 17237248 | 2.5 | sat | 17237248 | 6.3 | sat | 17237248 |
