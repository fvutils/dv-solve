# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 8.9 | sat | 12799760 | 14.3 | sat | 12799760 | 4.7 | sat | 12799760 | 4.7 | sat | 12799760 |
| arrayordering | 3.0 | unsat | 12799760 | 2.5 | unsat | 12799760 | 1.9 | unsat | 12799760 | 8.0 | unsat | 12799760 |
| arraysum8 | 2.1 | sat | 12799760 | 5.0 | sat | 12799760 | 1.0 | sat | 12799760 | 7.4 | sat | 12799760 |
| axi4burst | 1.7 | sat | 12799760 | 1.6 | sat | 12799760 | 0.6 | sat | 12799760 | 6.2 | sat | 12799760 |
| bustransaction | 1.9 | sat | 12799760 | 1.1 | sat | 12799760 | 0.6 | sat | 12799760 | 6.8 | sat | 12799760 |
| condinside | 1.7 | sat | 12799760 | 1.0 | sat | 12799760 | 0.6 | sat | 12799760 | 6.3 | sat | 12799760 |
| ddr5cmdbasic | 1.9 | sat | 12799760 | 1.1 | sat | 12799760 | 0.7 | sat | 12799760 | 7.2 | sat | 12799760 |
| ddr5moderegister | 1.8 | sat | 12799760 | 0.8 | sat | 12799760 | 0.4 | sat | 12799760 | 7.6 | sat | 12799760 |
| ddr5timing | 2.1 | sat | 12799760 | 1.6 | sat | 12799760 | 0.5 | sat | 12799760 | 7.1 | sat | 12799760 |
| distweighted | 1.8 | sat | 12799760 | 0.6 | sat | 12799760 | 0.5 | sat | 12799760 | 5.8 | sat | 12799760 |
| enumcond | 2.2 | sat | 12799760 | 6.3 | sat | 12799760 | 0.6 | sat | 12799760 | 7.4 | sat | 12799760 |
| fifoctrl | 1.9 | sat | 12799760 | 0.7 | sat | 12799760 | 0.7 | sat | 12799760 | 4.9 | sat | 12799760 |
| implicationchain8 | 2.3 | sat | 12799760 | 1.0 | sat | 12799760 | 0.7 | sat | 12799760 | 6.8 | sat | 12799760 |
| inequalityweb | 1.8 | sat | 12799760 | 3.0 | sat | 12799760 | 0.6 | sat | 12799760 | 7.5 | sat | 12799760 |
| memmaptight32 | 11.1 | sat | 12799760 | 116.0 | sat | 12799760 | 3.9 | sat | 12799760 | 44.8 | sat | 12799760 |
| mempartitionknapsack | 3.7 | sat | 12799760 | 9.9 | sat | 12799760 | 2.5 | sat | 12799760 | 9.9 | sat | 12799760 |
| memtransaction | 1.9 | sat | 12799760 | 1.4 | sat | 12799760 | 0.4 | sat | 12799760 | 7.0 | sat | 12799760 |
| muldivscenario | 2.1 | sat | 12799760 | 11.1 | sat | 12799760 | 0.7 | sat | 12799760 | 7.5 | sat | 12799760 |
| nqueens8 | 4.8 | sat | 12799760 | 10.8 | sat | 12799760 | 0.6 | sat | 12799760 | 9.3 | sat | 12799760 |
| onehot8 | 1.8 | sat | 12799760 | 0.9 | sat | 12799760 | 0.4 | sat | 12799760 | 4.4 | sat | 12799760 |
| packethdr | 1.7 | sat | 12799760 | 1.3 | sat | 12799760 | 0.5 | sat | 12799760 | 7.0 | sat | 12799760 |
| pcietlp | 1.8 | sat | 12799760 | 0.8 | sat | 12799760 | 2.1 | sat | 12799760 | 7.1 | sat | 12799760 |
| shiftaligned | 2.0 | sat | 12799760 | 0.9 | sat | 12799760 | 0.5 | sat | 12799760 | 4.5 | sat | 12799760 |
| socaddrmap32 | 4.6 | sat | 12799760 | 51.5 | sat | 12799760 | 0.7 | sat | 12799760 | 13.2 | sat | 12799760 |
| socaddrmap40 | 4.9 | sat | 12799760 | 60.7 | sat | 12799760 | 0.9 | sat | 12799760 | 14.6 | sat | 12799760 |
| socmemmap | 2.5 | sat | 12799760 | 12.8 | sat | 12799760 | 0.6 | sat | 12799760 | 8.0 | sat | 12799760 |
| softrelaxbaseline | 1.6 | sat | 12799760 | 0.7 | sat | 12799760 | 0.4 | sat | 12799760 | 4.9 | sat | 12799760 |
| softrelaxwithconflict | 1.8 | sat | 12799760 | 0.6 | sat | 12799760 | 0.5 | sat | 12799760 | 4.7 | sat | 12799760 |
| sumpartition | 2.8 | sat | 12799760 | 9.0 | sat | 12799760 | 0.5 | sat | 12799760 | 9.5 | sat | 12799760 |
| threeunique | 1.8 | sat | 12799760 | 1.6 | sat | 12799760 | 0.4 | sat | 12799760 | 8.1 | sat | 12799760 |
| unique16 | 4.1 | sat | 12799760 | 19.7 | sat | 12799760 | 0.6 | sat | 12799760 | 6.5 | sat | 12799760 |
| unique32 | 1.6 | sat | 12799760 | 0.8 | sat | 12799760 | 0.5 | sat | 12799760 | 4.2 | sat | 12799760 |
| unsignedops | 2.0 | sat | 12799760 | 1.0 | sat | 12799760 | 0.5 | sat | 12799760 | 6.8 | sat | 12799760 |
| verilatorops | 1.9 | sat | 12799760 | 2.4 | sat | 12799760 | 0.5 | sat | 12799760 | 6.8 | sat | 12799760 |
