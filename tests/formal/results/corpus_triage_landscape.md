# Corpus-wide triage landscape (2026-07-20)

Stepped back from banging on mcm/140,/176 to characterize where dv-solve is
fast / slow / bails across ALL 46 QF_BV instances of the SMT-COMP2025 parallel
corpus. Method: `dv-solve --engine=bitblast` vs `bitwuzla`, 60 s wall cap, 16 GB
vmem guard, sequential. Raw data: `corpus_triage_60s.csv`.

**Caveat on the cap:** 60 s is deliberately tight (categorization pass, not a
competition run). The published refs (1200 s) show bitwuzla solving mcm/68 @227 s,
/140 @149 s — none of which fits in 60 s. So "both fail" here means "neither fits
in 60 s," not "unsolvable."

## Six structural archetypes (static fingerprint)

| archetype | families | shape | width |
|---|---|---|---|
| shift-add arith | mcm | huge bvshl+bvadd, deep | 12 |
| wide div/rem | log-slicing, float | bvudiv/urem/smod, mul | 64–1024 |
| bitwise / CNF-like | vlsat3, brummayerbiere | pure bvand/or/xor + ITE, massive assert count | 11–256 |
| combinatorial CSP | asp (sudoku/graph/edge/labyrinth) | ~0 arith, huge = / ITE, tiny width | 4–10 |
| multiplier verify | Booth, Sage2 | huge concat + bvmul | 33–129 |
| automata | StringMatching, TCP, oisc | ITE-transition heavy (oisc: 891k ite) | 8–32 |

## Head-to-head at equal 60 s budget: dv-solve is AHEAD (6 wins, 2 forfeits)

**Our WINS (we solve; bitwuzla times out):**
- vlsat3_a57 / a80 / a67 — 1.8 / 4.3 / 10 s (bitwise-CNF; bitblast→kissat is world-class here)
- mcm/60, mcm/70 — sat 26 / 40 s (SAT-side arithmetic; kissat finds models fast)
- Sage2/bench_16172 — unsat 45 s

**Our FORFEITS (bitwuzla solves; we return unknown):**
- ecrw bw512_20 (unsat 0.0 s), bw512_11 (unsat 1.1 s) — both gated by div/rem at width 512

**Both fail at 60 s: 38 instances** — the real research frontier.

## Three failure modes (not two)

1. **slow-but-finish** — the diagnostic gold. mcm/60,/70 (sat), Sage2/16172, vlsat3.
2. **timeout** — large mcm (all SATISFIABLE per ref: 68/140/176 sat, bitwuzla solves; 59/77/95/119/126/135/185 unknown to EVERY competition solver), Booth mult-verify, asp combinatorial, automata.
3. **unknown-bail (coverage)** — ~16/46. Confirmed genuine at the product level
   (`--engine=auto` returns unknown too; CDCL fallback also bails). Root gates:
   - **signed ops** (largest, ~9): A-4 guard `zsp_bbsolver.c:682` bails on first
     signed leaf. Hits all float, uum32, Sage2/15955, oisc, bvsmod_17, BuchwaldFried.
   - **div/rem** (~5): ecrw×3, log-slicing, float.
   - **width > 128** (~7, overlaps div/rem): ecrw(512), log-slicing(1024), minandmaxor256, Booth64(129).
   - **2 mysteries**: fft/Sz1024 (w8), maxxormaxorand032 (w32) — no signed/div/wide/rotate, still bail.

## Strategic reframe

- **We are NOT generally behind.** At equal budget we win more head-to-heads than we
  lose. The "behind" narrative was specific to large-SAT mcm (140/176) at 1200 s.
- **Our genuine strength is bitwise/CNF-like + SAT-side arithmetic** (kissat one-shot).
  bitwuzla's word-level machinery is pure overhead there. Protect this.
- **Coverage bail (mode 3) is the biggest *count* of losses — but low ROI where the
  instance is also hard** (fixing signed float just turns unknown→timeout). The only
  true "forfeit a winnable game" cases are the ecrw div/rem unsats (2).
- **The mcm wall is large-SATISFIABLE instances (140/176), a model-finding-at-scale gap** (NOT unsat — corrected 2026-07-20 from reference_results.csv). Word-level preprocessing
  (bitwuzla's proven edge) is the lever, but it is high-effort and ties-not-beats.

## Candidate next levers, ranked by (value × tractability)

1. **Deeper-cap (300 s) profiling of the slow-but-finish band** (mcm/60,/70, Sage2/16172,
   mcm/68 which cube cracks @108 s) — measure WHERE our time goes (encode vs SAT solve;
   SAT stats) and how bitwuzla spends its differently. Cheap, directly answers "research
   the characteristics where we're slow." **Recommended first.**
2. **Understand WHY we win on vlsat/mcm-sat** — generalize the strength.
3. **div/rem coverage** — converts the 2 clear forfeits into wins; also unblocks log-slicing.
4. **signed-op coverage (A-4 guard)** — biggest count, but many targets hard anyway; unblocks the float archetype for study.
5. **large-SAT-mcm model finding (140/176)** — SAT instances bitwuzla solves and we don't; lever is preprocessing/propagation/partition for model-finding at scale, not refutation. High-effort, competition-grade, defer until 1–4 harvested.
6. **2 mystery bails (fft, brummayer032)** — cheap to root-cause; may be a detection bug.
