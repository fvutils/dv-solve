# Is XOR reasoning the lever for `affine-proof`? (piece 4.0)

2026-10-08. `docs/fw_hdl_implementation_plan.md` §1.2 guessed that boolector wins the
CRC proofs because its SAT solver (Lingeling) does Gaussian elimination over XOR
constraints, and that CDCL without it is exponential on these parity-shaped miters.
This note checks that guess before Phase 4 builds anything.

**Verdict: confirmed.** On our own CNF, CryptoMiniSat with Gauss–Jordan elimination
proves all 14 instances we lose in at most 2 s, most in 0.1 s. The same solver with
Gauss off proves 3 of them. kissat and CaDiCaL, our two backends, prove 5 and 1.

## Method

- The CNF is dv-solve's own bit-blast encoding, written by `DV_DUMP_CNF=<path>`
  (`src/c/dvs_sat.c`) with simulation off (`DV_SIM=0 DV_ENGINE=bitblast`). So the
  comparison is between SAT solvers on the same clauses, not between encodings.
- The instances are the 20 `affine-proof` instances in `docs/fwhdl_scorecard_phase3.md`
  that some competitor proves and dv-solve does not, plus `crc32_affine3`. Excluded:
  - `parity_w256`, which dv-solve does not encode (a 256-bit sort, above the 192-bit
    limit).
  - Four `crc_loop` variants that are the same shape as ones kept.
- The solvers are built from upstream heads under `perf-builds/sat/`:
  - Lingeling: with `--gauss=0` and without.
  - CryptoMiniSat 5: with `--maxnummatrices 0` (no Gauss matrices) and without.
  - kissat and CaDiCaL.
- Budget 60 s wall per run, 12 runs in parallel, with `tests/perf/sat_arms.sh`. The
  solver checkouts are under `perf-builds/sat/` (not in git; each is a plain
  `./configure && make` or CMake build).

## Results

Times in seconds; `—` is a timeout at 60 s. All answers are `unsat`.

| Instance | vars / clauses | CMS | CMS no Gauss | Lingeling | Lingeling no Gauss | kissat | CaDiCaL |
|---|---|---|---|---|---|---|---|
| `crc32_affine3` (20 s budget) | 1064 / 2209 | 0.07 | 9.05 | 0.07 | 0.49 | 0.60 | 4.20 |
| `crc_loop_w32_bytes2_k3` | 2504 / 5207 | 0.08 | 30.2 | 0.71 | 1.64 | 55.3 | — |
| `crc_loop_w32_bytes2_k4` | 3174 / 6624 | 0.11 | — | 0.57 | 11.2 | — | — |
| `crc_loop_w32_bytes4_k3` | 5336 / 10999 | 0.15 | 33.7 | 0.39 | 8.91 | 54.5 | — |
| `crc_loop_w64_bytes1_k4` | 2445 / 5343 | 0.09 | — | 0.10 | 4.26 | 55.7 | — |
| `crc_loop_w64_bytes2_k2` | 3831 / 7902 | 0.16 | — | 0.23 | 3.12 | — | — |
| `crc_loop_w8_bytes8_k4` | 3696 / 7428 | 0.07 | — | 0.30 | 23.3 | — | — |
| `gray_w64_decodeloop` | 6553 / 14378 | 0.26 | — | 0.50 | 12.0 | 46.0 | — |
| `gray_w128_decodeloop` | 25401 / 55018 | 2.04 | — | — | — | — | — |
| `lfsr_w16_rounds64` | 1920 / 4332 | 0.05 | — | — | — | — | — |
| `parity_w64` | 1220 / 3220 | 0.02 | — | 0.07 | — | — | — |
| `parity_w128` | 2820 / 7545 | 0.03 | — | 0.10 | — | — | — |
| `xorshift_injective_w32_rounds8` | 3247 / 5653 | 0.03 | 0.03 | 0.99 | 2.65 | 0.15 | 0.41 |
| `xorshift_linear_w32_rounds8` | 4776 / 8125 | 0.10 | — | 12.9 | — | — | — |
| `xorshift_linear_w64_rounds8` | 11928 / 23242 | 0.59 | — | — | — | — | — |

What the table says:

- **Gauss is the difference.** CryptoMiniSat goes from 3 proved to 14 when its Gauss
  matrices are allowed, and where both finish its time drops by up to 400×. Lingeling
  also leans on Gauss: 11 proved with it, 8 without.
- **kissat and CaDiCaL fail on parity, not on our encoding.** They are among the
  strongest plain CDCL solvers, and on the same CNF they prove 5 and 1, four of
  kissat's five at 46–56 s.
- **CryptoMiniSat beats boolector here.** Boolector times out on
  `lfsr_w16_rounds64`, `xorshift_linear_w64_rounds8` and `gray_w128_decodeloop`
  (`docs/fwhdl_scorecard_phase3.md`). Lingeling with Gauss also fails on these three,
  and Lingeling is the SAT solver this boolector build uses. CryptoMiniSat proves
  all three in ≤ 2 s.
- `xorshift_injective_w32_rounds8` needs no Gauss: kissat proves it in 0.15 s
  standalone, yet dv-solve timed out on it in the scorecard run. That gap is not
  XOR reasoning; it is unexplained and worth a look (in-tree kissat runs with
  different options).

## What this means for Phase 4

Every lost instance is a **pure affine miter**: the circuit is XOR, shifts,
constant masks and conditional XOR by a constant, and the proof obligation is
"these two affine maps differ somewhere". Piece 4.1 decides that shape exactly by
Gaussian elimination on the AIG, without any SAT call, so it should beat even
CryptoMiniSat. In-search Gauss–Jordan (4.3) is needed only when XOR is
entangled with nonlinear logic. No instance in `affine-proof` has that shape, so
4.3 waits for evidence from `mixed-proof` (decision D5's kill criterion).

That evidence is negative so far. The three `mixed-proof` instances we lose
(`fnv1a_w32_bytes4`, `fnv1a_w32_bytes8`, `fnv1a_w64_bytes4`; 5088–11851 vars) time
out at 60 s on every arm above, CryptoMiniSat with Gauss included. They are
multiplier-bound (FNV multiplies by a prime each byte), not XOR-bound. By D5's
kill criterion, 4.3 is dropped for now in favour of Phase 5.

**Result of 4.1** (`src/c/dvs_affine.c`): 19 of the 20 lost instances are decided in
under 7 ms wall, process start-up included. The 20th is `parity_w256`, blocked by
the sort width. Table-driven CRCs (`crc_table_*`, a constant lookup indexed by a
nibble) needed one more step: small gates whose truth table over a few affine
leaves is affine are recognized by simulation. They now take ≤ 0.4 ms, against up to
25 s on SAT.
