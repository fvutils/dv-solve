# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 2.7 | sat | 443532 | 0.6 | sat | 443532 | 1.1 | sat | 443532 | 4.8 | sat | 443532 |
| arrayordering | 3.3 | unsat | 443532 | 1.6 | unsat | 443532 | 1.5 | unsat | 443532 | 7.5 | unsat | 443532 |
| arraysum8 | 2.4 | sat | 443532 | 4.6 | sat | 443532 | 1.1 | sat | 443532 | 8.4 | sat | 443532 |
| axi4burst | 2.3 | sat | 443532 | 1.5 | sat | 443532 | 1.4 | sat | 443532 | 6.9 | sat | 443532 |
| bustransaction | 1.9 | sat | 443532 | 0.8 | sat | 443532 | 1.1 | sat | 443532 | 5.5 | sat | 443532 |
| condinside | 2.1 | sat | 443532 | 1.0 | sat | 443532 | 0.9 | sat | 443532 | 6.3 | sat | 443532 |
| ddr5cmdbasic | 1.6 | sat | 443532 | 0.8 | sat | 443532 | 1.1 | sat | 443532 | 7.2 | sat | 443532 |
| ddr5moderegister | 1.8 | sat | 443532 | 0.6 | sat | 443532 | 0.8 | sat | 443532 | 6.2 | sat | 443532 |
| ddr5timing | 1.9 | sat | 443532 | 1.8 | sat | 443532 | 0.8 | sat | 443532 | 7.2 | sat | 443532 |
| distweighted | 1.4 | sat | 443532 | 0.4 | sat | 443532 | 0.7 | sat | 443532 | 4.9 | sat | 443532 |
| enumcond | 2.0 | sat | 443532 | 7.2 | sat | 443532 | 0.8 | sat | 443532 | 8.3 | sat | 443532 |
| fifoctrl | 1.9 | sat | 443532 | 0.7 | sat | 443532 | 0.7 | sat | 443532 | 5.4 | sat | 443532 |
| implicationchain8 | 1.8 | sat | 443532 | 1.0 | sat | 443532 | 0.7 | sat | 443532 | 7.0 | sat | 443532 |
| inequalityweb | 1.6 | sat | 443532 | 3.2 | sat | 443532 | 1.1 | sat | 443532 | 7.2 | sat | 443532 |
| memmaptight32 | 10.8 | sat | 443532 | 114.6 | sat | 443532 | 1.5 | sat | 443532 | 38.7 | sat | 443532 |
| mempartitionknapsack | 3.7 | sat | 443532 | 9.3 | sat | 443532 | 3.7 | sat | 443532 | 10.9 | sat | 443532 |
| memtransaction | 1.5 | sat | 443532 | 1.1 | sat | 443532 | 0.7 | sat | 443532 | 7.7 | sat | 443532 |
| muldivscenario | 1.9 | sat | 443532 | 11.6 | sat | 443532 | 1.1 | sat | 443532 | 7.9 | sat | 443532 |
| nqueens8 | 5.7 | sat | 443532 | 11.2 | sat | 443532 | 1.2 | sat | 443532 | 9.5 | sat | 443532 |
| onehot8 | 1.3 | sat | 443532 | 0.6 | sat | 443532 | 0.8 | sat | 443532 | 5.8 | sat | 443532 |
| packethdr | 1.5 | sat | 443532 | 1.2 | sat | 443532 | 0.8 | sat | 443532 | 6.6 | sat | 443532 |
| pcietlp | 1.6 | sat | 443532 | 0.7 | sat | 443532 | 0.8 | sat | 443532 | 6.7 | sat | 443532 |
| shiftaligned | 1.4 | sat | 443532 | 0.7 | sat | 443532 | 0.7 | sat | 443532 | 6.3 | sat | 443532 |
| socaddrmap32 | 4.9 | sat | 443532 | 50.9 | sat | 443532 | 0.8 | sat | 443532 | 13.7 | sat | 443532 |
| socaddrmap40 | 5.5 | sat | 443532 | 60.4 | sat | 443532 | 0.8 | sat | 443532 | 18.3 | sat | 443532 |
| socmemmap | 2.3 | sat | 443532 | 11.1 | sat | 443532 | 0.8 | sat | 443532 | 9.3 | sat | 443532 |
| softrelaxbaseline | 1.3 | sat | 443532 | 0.9 | sat | 443532 | 0.7 | sat | 443532 | 7.0 | sat | 443532 |
| softrelaxwithconflict | 1.7 | sat | 443532 | 0.4 | sat | 443532 | 0.7 | sat | 443532 | 7.0 | sat | 443532 |
| sumpartition | 3.2 | sat | 443532 | 9.1 | sat | 443532 | 0.8 | sat | 443532 | 8.6 | sat | 443532 |
| threeunique | 1.9 | sat | 443532 | 2.1 | sat | 443532 | 1.3 | sat | 443532 | 6.4 | sat | 443532 |
| unique16 | 3.8 | sat | 443532 | 21.4 | sat | 443532 | 1.4 | sat | 443532 | 7.8 | sat | 443532 |
| unique32 | 1.5 | sat | 443532 | 0.5 | sat | 443532 | 0.7 | sat | 443532 | 4.5 | sat | 443532 |
| unsignedops | 1.9 | sat | 443532 | 0.9 | sat | 443532 | 0.8 | sat | 443532 | 8.0 | sat | 443532 |
| verilatorops | 1.6 | sat | 443532 | 2.7 | sat | 443532 | 0.7 | sat | 443532 | 6.6 | sat | 443532 |
