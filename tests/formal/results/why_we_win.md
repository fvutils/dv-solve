# Why dv-solve beats bitwuzla (where it does) — 2026-07-20

Follow-up to `corpus_triage_landscape.md`: dv-solve wins 6 head-to-heads at a 60 s
budget (vlsat3 ×3, mcm/60, mcm/70, Sage2/16172). Investigated the mechanism via
controlled backend swap (same encoding, kissat vs cadical) + bitwuzla factorial.
Single-seed, 4 instances — strong directional, not statistically hardened.

## Two distinct mechanisms

### Mechanism 1: our encoding is a moat (the surprising one)
`vlsat3_a57`: dv-solve+**cadical** = 9.5 s, but bitwuzla+**cadical** (its default) = timeout >40 s.
Same SAT solver, opposite outcome ⇒ the differentiator is the **CNF we hand it**, not the backend.

- Our pipeline: direct bitblast → Tseitin CNF with variable substitution. On vlsat3_a57:
  encode = **12.9 ms**, 46546 vars / 180618 clauses / **4044 substs**, then SAT = 1803 ms.
  Encoding is ~1% of wall; it's lean and does real simplification up front.
- bitwuzla `--preprocess=false` AND `-rwl 0` BOTH still time out → its disadvantage here
  is not "preprocessing overhead we could disable," it's the whole word-level→bitblast
  pipeline producing a CNF that cadical likes less than ours.
- **Consequence:** for the CNF-like / bitwise / unsat-heavy archetype, word-level machinery
  HURTS. This is the OPPOSITE of the "we need word-level preprocessing" thesis. That thesis
  is archetype-specific (large-SAT-mcm model finding at scale — see correction below), not universal.

### Mechanism 2: kissat wins SAT-side model search
`mcm/60` (satisfiable): kissat = 26 s, cadical = timeout >60 s (same encoding).
On satisfiable arithmetic, kissat's search/phase heuristics reach a model cadical doesn't.
Backend-specific — here the SAT solver itself is the lever.

Divergence summary (same encoding):
| instance      | kissat | cadical | winner |
|---------------|--------|---------|--------|
| vlsat3_a57    | 1.9 s  | 9.5 s   | kissat (both solve) |
| vlsat3_a80    | 4.3 s  | 3.9 s   | ~tie   |
| mcm/60 (sat)  | 26 s   | >60 s   | **kissat decisively** |
| Sage2/16172   | 45 s   | 46 s    | ~tie   |

## Ablation: what in the encoding is the moat? (DV_BB_NO_SUBST / DV_BB_NO_MEMO)

Variable substitution is archetype-dependent and DECISIVE on arithmetic:

| instance      | archetype  | baseline (subst)      | no-subst              | verdict |
|---------------|------------|-----------------------|-----------------------|---------|
| mcm/60        | shift-add  | sat 26 s / 599 K cl   | TIMEOUT / 4.99 M cl (8×) | subst = the moat |
| Sage2/16172   | mul-heavy  | unsat 45 s / 77 K cl  | unsat 60 s / 603 K cl (8×)| subst helps |
| vlsat3_a80    | CNF-like   | 4.0 s / 1.47 M cl     | 3.1 s / 4.4 M cl      | subst ~neutral (memory only) |
| vlsat3_a57    | CNF-like   | 1.8 s / 181 K cl      | 1.1 s / 536 K cl      | subst slightly HURTS solve |

Memoization (DV_BB_NO_MEMO): no effect on vlsat3_a57 (identical clause count).

**Correction of an earlier read:** on the first ablated instance (vlsat3_a57, CNF-like)
substitution shrank the CNF 3× yet the solve got slower — which looked like "subst isn't
the moat." The arithmetic instances overturn that: subst collapses shift-add/mul redundancy
**8×** and is the difference between solving mcm/60 and timing out. So:

- **Our variable substitution IS a lean word-level preprocessing**, and it is decisive on the
  arithmetic archetype. It is our real word-level lever, done lean enough to hand off well to SAT.
- On CNF-like, subst is ~neutral; the win there is the direct-to-kissat handoff itself.
- Reconciles the "word-level hurts" claim: the lever is HOW MUCH, gated to archetype. LEAN
  word-level (our subst) → big win on arithmetic. HEAVY word-level (bitwuzla full pipeline)
  → hurts CNF-like. Not a binary.

## Strategic consequences

1. **Protect and invest in encoding leanness** (structural hashing, variable substitution,
   Tseitin quality) — it's why we beat bitwuzla even with the same backend. Do NOT reflexively
   adopt bitwuzla-style word-level preprocessing; it's double-edged (helps large-SAT mcm, hurts CNF-like).
2. **kissat is the right default; cadical is a genuine diversity partner** (cadical ties/wins
   vlsat3_a80). Validates instance-level backend selection / a 2-backend portfolio — but the
   real, proven divergence is narrow (mcm-sat model search), so keep expectations sized to that.
3. **Word-level preprocessing is archetype-specific, not a universal lever.** Any future
   preprocessing work must be gated to the archetypes it helps (shift-add / large-SAT mcm) and kept
   off the CNF-like path where it regresses.

## CORRECTION (2026-07-20): the mcm wall is SAT, not unsat

reference_results.csv (authoritative ground truth) shows the mcm instances we lose on
are SATISFIABLE, and bitwuzla solves them AS sat:
- SAT, bitwuzla solves: 60 (259 s), 68 (227 s), 70 (38 s), **140 (149 s)**, **176 (370 s)**
- unknown to EVERY competition solver (bitwuzla + STP-Parti + Yices all timeout @1200 s):
  59, 77, 95, 119, 126, 135, 185

So there are TWO distinct mcm walls, not one:
- **Wall B — large SATISFIABLE (140, 176):** bitwuzla finds the MCM network; our bitblast+kissat
  times out @1200 s. This is a **model-finding-at-scale** gap, NOT refutation. Well-defined
  target (known achievable in 149 s). This is the tractable one to attack.
- **Wall A — small-but-unsolved (59, 77, 95):** smallest mcm files (117 KB) yet UNKNOWN to all
  solvers — the classic tight sat/unsat boundary, likely genuinely unsat/hardest. Open even
  for competition solvers; research-grade ROI.

Earlier turns in this investigation mislabeled the wall "mcm-UNSAT / structural reasoning" —
wrong. The lever for Wall B is whatever lets bitwuzla FIND the model at scale (preprocessing,
propagation-based search, or partition/cube), not refutation strength. Test in progress:
does bitwuzla `--preprocess=false` lose /140 (⇒ preprocessing is its model-finding edge)?

## Open / to harden
- Single-seed; mcm/60 SAT timing is heavy-tail — confirm the kissat>>cadical gap with 5 seeds.
- Confirm the encoding-quality claim directly by comparing CNF var/clause counts ours vs
  bitwuzla's (needs a CNF dump path from bitwuzla; not yet found).
- Why exactly is our CNF friendlier? (substitution count? AIG structural hashing? gate polarity?)
