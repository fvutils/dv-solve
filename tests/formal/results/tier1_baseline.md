# Tier 1 Baseline Results

| Benchmark | bitwuzla (ms) | bitwuzla result | bitwuzla mem (KB) | boolector (ms) | boolector result | boolector mem (KB) | dv-solve-smt2 (ms) | dv-solve-smt2 result | dv-solve-smt2 mem (KB) | z3 (ms) | z3 result | z3 mem (KB) |
|-----------|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|----------:|:----------:|------------:|
| alignedaddr | 7.5 | sat | 40804 | 2.0 | sat | 40536 | 2.1 | sat | 42188 | 14.2 | sat | 41068 |
| arrayordering | 77.9 | unsat | 40804 | 4.7 | unsat | 40536 | 2.3 | sat | 42188 | 81.1 | unsat | 41068 |
| arraysum8 | 7.6 | sat | 264 | 11.3 | sat | 268 | 2.0 | sat | 42188 | 85.5 | sat | 41068 |
| axi4burst | 7.5 | sat | 41068 | 75.7 | sat | 40804 | 2.2 | sat | 42188 | 12.9 | sat | 41068 |
| bustransaction | 7.6 | sat | 41068 | 7.3 | sat | 40804 | 1.9 | sat | 42188 | 83.3 | sat | 41068 |
| condinside | 8.5 | sat | 41068 | 6.7 | sat | 40804 | 1.7 | sat | 42188 | 11.0 | sat | 41068 |
| ddr5cmdbasic | 72.5 | sat | 41068 | 2.7 | sat | 40804 | 1.7 | sat | 42188 | 76.6 | sat | 41068 |
| ddr5moderegister | 6.8 | sat | 41068 | 2.1 | sat | 40804 | 68.0 | sat | 42188 | 11.0 | sat | 41068 |
| ddr5timing | 7.5 | sat | 41068 | 3.9 | sat | 40804 | 2.5 | sat | 42188 | 11.8 | sat | 41068 |
| distweighted | 74.6 | sat | 41068 | 71.0 | sat | 40804 | 2.0 | sat | 42188 | 8.0 | sat | 41068 |
| enumcond | 7.5 | sat | 41068 | 14.4 | sat | 40804 | 2.2 | sat | 42188 | 13.0 | sat | 41068 |
| fifoctrl | 6.4 | sat | 41068 | 2.0 | sat | 40804 | 1.8 | sat | 42188 | 9.4 | sat | 41068 |
| implicationchain8 | 76.9 | sat | 41068 | 3.0 | sat | 40804 | 2.1 | sat | 42188 | 11.7 | sat | 41068 |
| inequalityweb | 7.5 | sat | 41068 | 71.5 | sat | 40804 | 70.6 | sat | 42188 | 87.0 | sat | 41068 |
| memmaptight32 | 99.9 | sat | 41068 | 807.8 | sat | 40804 | 3.9 | sat | 42188 | 301.3 | sat | 1120 |
| mempartitionknapsack | 78.4 | sat | 41068 | 84.5 | sat | 40804 | 3.2 | sat | 42188 | 92.6 | sat | 42188 |
| memtransaction | 6.3 | sat | 41068 | 3.0 | sat | 40804 | 1.7 | sat | 42188 | 11.8 | sat | 42188 |
| muldivscenario | 87.2 | sat | 41068 | 95.9 | sat | 40804 | 1.9 | sat | 42188 | 80.8 | sat | 42188 |
| nqueens8 | 16.1 | sat | 41068 | 24.3 | sat | 40804 | 2.0 | sat | 42188 | 15.0 | sat | 42188 |
| onehot8 | 6.0 | sat | 41068 | 64.2 | sat | 40804 | 69.4 | sat | 42188 | 75.9 | sat | 42188 |
| packethdr | 74.9 | sat | 41068 | 3.1 | sat | 40804 | 2.0 | sat | 42188 | 11.7 | sat | 42188 |
| pcietlp | 6.8 | sat | 41068 | 2.5 | sat | 40804 | 2.9 | sat | 42188 | 76.0 | sat | 42188 |
| shiftaligned | 5.9 | sat | 41068 | 1.8 | sat | 40804 | 2.3 | sat | 42188 | 9.3 | sat | 42188 |
| socaddrmap32 | 83.0 | sat | 41068 | 311.3 | sat | 40804 | 2.0 | sat | 42188 | 95.8 | sat | 42188 |
| socaddrmap40 | 15.7 | sat | 41068 | 408.2 | sat | 40804 | 2.2 | sat | 42188 | 95.8 | sat | 42188 |
| socmemmap | 75.2 | sat | 41068 | 98.5 | sat | 40804 | 70.5 | sat | 42188 | 15.2 | sat | 42188 |
| softrelaxbaseline | 5.5 | sat | 41068 | 1.9 | sat | 40804 | 2.2 | sat | 42188 | 11.9 | sat | 42188 |
| softrelaxwithconflict | 6.8 | sat | 41068 | 2.0 | sat | 40804 | 1.9 | sat | 42188 | 12.6 | sat | 42188 |
| sumpartition | 76.6 | sat | 41068 | 88.8 | sat | 40804 | 2.2 | sat | 42188 | 15.6 | sat | 42188 |
| threeunique | 6.5 | sat | 41068 | 4.0 | sat | 40804 | 1.9 | sat | 42188 | 11.4 | sat | 42188 |
| unique16 | 13.4 | sat | 41068 | 182.3 | sat | 40804 | 2.0 | sat | 42188 | 17.2 | sat | 42188 |
| unique32 | 6.5 | sat | 41068 | 2.1 | sat | 40804 | 1.7 | sat | 42188 | 79.5 | sat | 42188 |
| unsignedops | 8.5 | sat | 41068 | 2.9 | sat | 40804 | 67.5 | sat | 42188 | 12.0 | sat | 42188 |
| verilatorops | 7.1 | sat | 41068 | 8.4 | sat | 40804 | 2.0 | sat | 42188 | 12.1 | sat | 42188 |
