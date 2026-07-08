# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 9.9 | sat | 41472 | 1.3 | sat | 41472 | 3.8 | sat | 41728 | 5.8 | sat | 41472 |
| arrayordering | 2.8 | unsat | 41472 | 1.8 | unsat | 41472 | 3.9 | unsat | 41728 | 9.3 | unsat | 41472 |
| arraysum8 | 2.2 | sat | 41472 | 4.6 | sat | 41472 | 4.1 | sat | 41728 | 8.8 | sat | 41472 |
| axi4burst | 3.0 | sat | 41472 | 1.4 | sat | 41472 | 3.7 | sat | 41728 | 7.0 | sat | 41472 |
| bustransaction | 1.7 | sat | 41472 | 0.7 | sat | 41472 | 3.5 | sat | 41728 | 8.2 | sat | 41472 |
| condinside | 1.4 | sat | 41472 | 0.8 | sat | 41472 | 3.3 | sat | 41728 | 8.0 | sat | 41472 |
| ddr5cmdbasic | 1.7 | sat | 41472 | 1.3 | sat | 41472 | 3.7 | sat | 41728 | 7.7 | sat | 41472 |
| ddr5moderegister | 1.6 | sat | 41472 | 0.6 | sat | 41472 | 3.3 | sat | 41728 | 9.6 | sat | 41472 |
| ddr5timing | 1.8 | sat | 41472 | 1.5 | sat | 41472 | 3.3 | sat | 41728 | 9.3 | sat | 41472 |
| distweighted | 1.3 | sat | 41472 | 0.4 | sat | 41472 | 3.6 | sat | 41728 | 5.4 | sat | 41472 |
| enumcond | 1.8 | sat | 41472 | 5.8 | sat | 41472 | 3.8 | sat | 41728 | 10.2 | sat | 41472 |
| fifoctrl | 1.4 | sat | 41472 | 1.2 | sat | 41472 | 3.3 | sat | 41728 | 6.0 | sat | 41472 |
| implicationchain8 | 2.0 | sat | 41472 | 1.2 | sat | 41472 | 3.2 | sat | 41728 | 8.8 | sat | 41472 |
| inequalityweb | 1.7 | sat | 41472 | 3.2 | sat | 41472 | 3.3 | sat | 41728 | 8.2 | sat | 41472 |
| memmaptight32 | 11.2 | sat | 41472 | 122.2 | sat | 41472 | 4.7 | sat | 41728 | 55.3 | sat | 256 |
| mempartitionknapsack | 4.2 | sat | 41472 | 10.3 | sat | 41472 | 5.3 | sat | 41728 | 11.8 | sat | 41728 |
| memtransaction | 1.9 | sat | 41472 | 1.8 | sat | 41472 | 3.6 | sat | 41728 | 8.4 | sat | 41728 |
| muldivscenario | 1.9 | sat | 41472 | 11.5 | sat | 41472 | 3.3 | sat | 41728 | 9.0 | sat | 41728 |
| nqueens8 | 4.6 | sat | 41472 | 11.8 | sat | 41472 | 3.7 | sat | 41728 | 9.2 | sat | 41728 |
| onehot8 | 1.3 | sat | 41472 | 0.7 | sat | 41472 | 3.4 | sat | 41728 | 7.7 | sat | 41728 |
| packethdr | 1.5 | sat | 41472 | 1.8 | sat | 41472 | 3.5 | sat | 41728 | 9.2 | sat | 41728 |
| pcietlp | 1.5 | sat | 41472 | 1.2 | sat | 41472 | 3.5 | sat | 41728 | 7.7 | sat | 41728 |
| shiftaligned | 2.1 | sat | 41472 | 0.7 | sat | 41472 | 3.5 | sat | 41728 | 4.5 | sat | 41728 |
| socaddrmap32 | 4.3 | sat | 41472 | 52.9 | sat | 41472 | 3.3 | sat | 41728 | 16.3 | sat | 41728 |
| socaddrmap40 | 5.0 | sat | 41472 | 60.9 | sat | 41472 | 3.3 | sat | 41728 | 17.4 | sat | 41728 |
| socmemmap | 2.2 | sat | 41472 | 11.6 | sat | 41472 | 3.5 | sat | 41728 | 10.0 | sat | 41728 |
| softrelaxbaseline | 1.3 | sat | 41472 | 1.0 | sat | 41472 | 3.8 | sat | 41728 | 7.4 | sat | 41728 |
| softrelaxwithconflict | 1.3 | sat | 41472 | 0.7 | sat | 41472 | 4.4 | sat | 41728 | 8.8 | sat | 41728 |
| sumpartition | 2.5 | sat | 41472 | 8.9 | sat | 41472 | 3.7 | sat | 41728 | 9.4 | sat | 41728 |
| threeunique | 1.5 | sat | 41472 | 1.9 | sat | 41472 | 3.2 | sat | 41728 | 6.8 | sat | 41728 |
| unique16 | 6.0 | sat | 41472 | 20.8 | sat | 41472 | 3.6 | sat | 41728 | 8.8 | sat | 41728 |
| unique32 | 1.9 | sat | 41472 | 1.0 | sat | 41472 | 4.3 | sat | 41728 | 6.4 | sat | 41728 |
| unsignedops | 1.7 | sat | 41472 | 1.5 | sat | 41472 | 3.2 | sat | 41728 | 7.7 | sat | 41728 |
| verilatorops | 2.0 | sat | 41472 | 2.6 | sat | 41472 | 4.1 | sat | 41728 | 9.3 | sat | 41728 |
