# dv-solve corpus baseline — SMT-COMP 2025 parallel track

_Generated 2026-07-20 · engine `--engine=bitblast` (default kissat backend) · seed 0 · 300 s cap._

**Measurement protocol note:** hard/bit-blasting instances (26) were timed SEQUENTIALLY (memory-bandwidth contention under parallelism inflates fast solves past the cap — e.g. mcm/70 39 s→timeout at 6-way). Instant-`unknown` instances (coverage gaps, ~0 s) are from the parallel screen (contention-immune). Single seed, 300 s cap: this is a diffable baseline, NOT competition-comparable (that needs 1100 s + 5-seed medians per the checklist).

## Headline

- **Solved (sat+unsat): 7 / 93**
- Crashes: 0
- Verdict distribution: sat=2, timeout=19, unknown=67, unsat=5

Reference (SMT-COMP solvers, 1200 s): 15 sat + 41 unsat solvable, 37 unknown-to-SOTA.

## By logic

| logic | solved / total |
|-------|----------------|
| QF_ABV | 0 / 24 |
| QF_AUFBV | 0 / 4 |
| QF_BV | 7 / 46 |
| QF_UFBV | 0 / 19 |

## Solved instances

| verdict | wall_s | cnf_clauses | rss_mb | instance |
|---------|--------|-------------|--------|----------|
| unsat | 1.9 | 180618 | 37.1 | QF_BV/20210312-Bouvier/vlsat3_a57.smt2 |
| unsat | 4.3 | 1474125 | 202.1 | QF_BV/20210312-Bouvier/vlsat3_a80.smt2 |
| unsat | 10.5 | 11594921 | 1507.6 | QF_BV/20210312-Bouvier/vlsat3_a67.smt2 |
| sat | 25.8 | 599258 | 251.7 | QF_BV/mcm/60.smt2 |
| sat | 38.9 | 588093 | 252.5 | QF_BV/mcm/70.smt2 |
| unsat | 45.1 | 77253 | 41.0 | QF_BV/Sage2/bench_16172.smt2 |
| unsat | 287.3 | 226414 | 129.4 | QF_BV/asp/GraphColouring/graph-colouring-nodes=140-density=0.1-instance=2.smt2 |

## Not solved (timeout/unknown), bit-blasting ones with CNF size

| verdict | wall_s | cnf_clauses | rss_mb | instance |
|---------|--------|-------------|--------|----------|
| timeout | 300.1 | 14623068 | 3366.6 | QF_BV/asp/Sudoku/sudoku.in3.smt2 |
| timeout | 300.1 | 12941365 | 4892.6 | QF_BV/mcm/176.smt2 |
| timeout | 300.1 | 7587425 | 1785.9 | QF_BV/20221214-p4dfa-XiaoqiChen/StringMatching/string4x16.6._bit8_na6_nr4_pairedtwobranch.smt2 |
| timeout | 300.0 | 2622462 | 1003.4 | QF_BV/mcm/135.smt2 |
| timeout | 300.0 | 2297527 | 862.2 | QF_BV/mcm/126.smt2 |
| timeout | 300.0 | 2297171 | 824.9 | QF_BV/mcm/119.smt2 |
| timeout | 300.0 | 2057626 | 849.9 | QF_BV/mcm/140.smt2 |
| timeout | 300.0 | 1920611 | 523.4 | QF_BV/20221214-p4dfa-XiaoqiChen/TCP/tcp_full_bit8_na6_nr3_paired.smt2 |
| timeout | 300.0 | 1585458 | 440.4 | QF_BV/20221214-p4dfa-XiaoqiChen/StringMatching/string2x8.8._bit8_na6_nr3_paired.smt2 |
| timeout | 300.0 | 1076045 | 319.0 | QF_BV/20221214-p4dfa-XiaoqiChen/StringMatching/string1x16.3._bit8_na6_nr3_paired.smt2 |
| timeout | 300.0 | 587811 | 255.4 | QF_BV/mcm/68.smt2 |
| timeout | 300.0 | 480595 | 209.7 | QF_BV/mcm/77.smt2 |
| timeout | 300.0 | 480496 | 211.9 | QF_BV/mcm/59.smt2 |
| timeout | 300.0 | 480430 | 211.7 | QF_BV/mcm/95.smt2 |
| timeout | 300.0 | 206114 | 111.2 | QF_BV/asp/GraphColouring/graph-colouring-nodes=130-density=0.1-instance=3.smt2 |
| timeout | 300.0 | 87288 | 80.6 | QF_BV/wienand-cav2008/Booth/mult_ub_16x16_1.sf.smt2 |

_67 instances returned `unknown` instantly (no bit-blast = encoding-coverage gaps: arrays/UF/wide ops)._
