# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 1.9 | sat | 12213748 | 0.6 | sat | 12213748 | 1.7 | sat | 12213748 | 5.3 | sat | 12213748 |
| arrayordering | 2.9 | unsat | 12213748 | 1.7 | unsat | 12213748 | 1.3 | unsat | 12213748 | 7.7 | unsat | 12213748 |
| arraysum8 | 2.7 | sat | 12213748 | 4.5 | sat | 12213748 | 1.2 | sat | 12213748 | 6.8 | sat | 12213748 |
| axi4burst | 1.9 | sat | 12213748 | 1.5 | sat | 12213748 | 0.9 | sat | 12213748 | 7.2 | sat | 12213748 |
| bustransaction | 1.8 | sat | 12213748 | 0.9 | sat | 12213748 | 1.0 | sat | 12213748 | 8.0 | sat | 12213748 |
| condinside | 1.8 | sat | 12213748 | 0.8 | sat | 12213748 | 0.8 | sat | 12213748 | 7.1 | sat | 12213748 |
| ddr5cmdbasic | 1.9 | sat | 12213748 | 0.9 | sat | 12213748 | 0.8 | sat | 12213748 | 6.5 | sat | 12213748 |
| ddr5moderegister | 1.8 | sat | 12213748 | 0.7 | sat | 12213748 | 0.8 | sat | 12213748 | 6.3 | sat | 12213748 |
| ddr5timing | 2.5 | sat | 12213748 | 1.5 | sat | 12213748 | 0.8 | sat | 12213748 | 6.6 | sat | 12213748 |
| distweighted | 1.5 | sat | 12213748 | 0.5 | sat | 12213748 | 1.0 | sat | 12213748 | 5.1 | sat | 12213748 |
| enumcond | 2.1 | sat | 12213748 | 5.9 | sat | 12213748 | 0.8 | sat | 12213748 | 7.0 | sat | 12213748 |
| fifoctrl | 1.6 | sat | 12213748 | 0.6 | sat | 12213748 | 0.8 | sat | 12213748 | 4.2 | sat | 12213748 |
| implicationchain8 | 1.6 | sat | 12213748 | 1.3 | sat | 12213748 | 0.8 | sat | 12213748 | 6.7 | sat | 12213748 |
| inequalityweb | 2.0 | sat | 12213748 | 3.0 | sat | 12213748 | 0.8 | sat | 12213748 | 7.5 | sat | 12213748 |
| memmaptight32 | 10.4 | sat | 12213748 | 110.2 | sat | 12213748 | 7.2 | sat | 12213748 | 36.9 | sat | 12213748 |
| mempartitionknapsack | 3.8 | sat | 12213748 | 9.6 | sat | 12213748 | 3.7 | sat | 12213748 | 10.4 | sat | 12213748 |
| memtransaction | 1.8 | sat | 12213748 | 1.2 | sat | 12213748 | 0.8 | sat | 12213748 | 7.1 | sat | 12213748 |
| muldivscenario | 2.3 | sat | 12213748 | 11.3 | sat | 12213748 | 0.9 | sat | 12213748 | 7.5 | sat | 12213748 |
| nqueens8 | 4.4 | sat | 12213748 | 10.8 | sat | 12213748 | 1.5 | sat | 12213748 | 8.3 | sat | 12213748 |
| onehot8 | 1.6 | sat | 12213748 | 0.6 | sat | 12213748 | 1.4 | sat | 12213748 | 4.8 | sat | 12213748 |
| packethdr | 1.4 | sat | 12213748 | 1.6 | sat | 12213748 | 1.0 | sat | 12213748 | 6.1 | sat | 12213748 |
| pcietlp | 2.0 | sat | 12213748 | 0.9 | sat | 12213748 | 0.8 | sat | 12213748 | 6.4 | sat | 12213748 |
| shiftaligned | 1.7 | sat | 12213748 | 0.6 | sat | 12213748 | 1.2 | sat | 12213748 | 4.5 | sat | 12213748 |
| socaddrmap32 | 4.5 | sat | 12213748 | 50.8 | sat | 12213748 | 1.1 | sat | 12213748 | 12.2 | sat | 12213748 |
| socaddrmap40 | 4.7 | sat | 12213748 | 60.7 | sat | 12213748 | 1.1 | sat | 12213748 | 15.7 | sat | 12213748 |
| socmemmap | 2.5 | sat | 12213748 | 10.9 | sat | 12213748 | 1.0 | sat | 12213748 | 7.7 | sat | 12213748 |
| softrelaxbaseline | 2.0 | sat | 12213748 | 0.9 | sat | 12213748 | 0.8 | sat | 12213748 | 6.8 | sat | 12213748 |
| softrelaxwithconflict | 1.7 | sat | 12213748 | 0.6 | sat | 12213748 | 1.1 | sat | 12213748 | 5.4 | sat | 12213748 |
| sumpartition | 2.7 | sat | 12213748 | 9.5 | sat | 12213748 | 0.9 | sat | 12213748 | 8.7 | sat | 12213748 |
| threeunique | 2.0 | sat | 12213748 | 1.5 | sat | 12213748 | 0.8 | sat | 12213748 | 5.0 | sat | 12213748 |
| unique16 | 4.0 | sat | 12213748 | 20.2 | sat | 12213748 | 1.0 | sat | 12213748 | 8.7 | sat | 12213748 |
| unique32 | 1.7 | sat | 12213748 | 0.6 | sat | 12213748 | 0.9 | sat | 12213748 | 3.9 | sat | 12213748 |
| unsignedops | 2.0 | sat | 12213748 | 1.1 | sat | 12213748 | 0.8 | sat | 12213748 | 8.1 | sat | 12213748 |
| verilatorops | 2.5 | sat | 12213748 | 2.3 | sat | 12213748 | 0.8 | sat | 12213748 | 6.6 | sat | 12213748 |
