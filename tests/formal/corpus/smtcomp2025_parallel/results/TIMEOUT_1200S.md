# 1200s single-thread re-run — "needs more time" vs "needs partitioning"

Ran the 6 ref-solvable QF_BV timeout instances at competition-equal time (1200s,
single thread, both engines). Raw: `timeout_1200s.csv`. Cross-referenced with
`SEPARABILITY.md`. All 6 are SAT; all 6 were solved by a reference solver at
1200s multi-core.

| instance | CDCL @1200s | bitblast @1200s | verdict |
|---|---|---|---|
| StringMatching/string1x16.3 | **sat 992s** | **sat 991s** | ✅ needs more time only |
| mcm/68 | timeout | **sat 1198s** | ✅ needs more time (bitblast, at the wire) |
| asp/Labyrinth/laby_18_18_04 | timeout | timeout | ⛔ needs partitioning |
| mcm/140 | unknown 0.5s | timeout | ⛔ partitioning + CDCL feature gap |
| mcm/176 | unknown 30s | timeout | ⛔ partitioning + CDCL feature gap |
| asp/EdgeMatching/edge-matching-10x10 | unknown 562s | **error 571s** | ⚠ robustness (see below) |

## Read-out

**2 of 6 were "just needs more time".** `string1x16.3` (both engines, ~990s) and
`mcm/68` (bitblast, 1198s — right at the deadline) solve sequentially given
competition-equal time. At 120s they looked like failures; they aren't. This is
the good news: dv-solve's core engine is competitive on these when not
time-starved. Parallelism here buys *speedup* (990s is uncomfortably close to
the limit), not new solves.

**3 of 6 are genuine parallelism targets.** `laby`, `mcm/140`, `mcm/176` are not
solved at 1200s single-thread but are provably SAT and were cracked by the
parallel-track solvers. Per `SEPARABILITY.md` these are single-component search
problems with rich disjunctive structure (140/176 have 33/69 top-level `or`
forms) — i.e. **cube-and-conquer targets**, exactly what STP-Parti exploits.
Note `laby` is the hardest: even Bitwuzla timed out at 1200s; only the
partitioning solver STP-Parti solved it — the single strongest evidence that
partitioning (not raw sequential speed) is the lever.

**1 of 6 is a robustness issue, not a parallelism story.** `edge-matching`
(246k vars) — CDCL worked 562s then honestly bailed `unknown`; bitblast
**errored at 571s** (being classified: likely OOM bitblasting a 246k-var AIG).
Tracked separately.

## CDCL feature gap, sharpened

On `mcm/140` and `mcm/176`, CDCL returns `unknown` fast (0.5s / 30s) — it bails
on an unsupported construct rather than searching. The `(x3 x30 x31)`-style
applications in the mcm family (a function-ish head applied to bit-vectors)
aren't handled on the CDCL path. Bitblast is the only engine that engages these
at all. So for the mcm targets, a cube-and-conquer prototype should sit on top
of the **bitblast** engine.

## Combined conclusion for the parallelization thesis

- Constraint-component decomposition: **not applicable** here (all one blob).
- Cube-and-conquer partitioning: **the** lever. Concrete first targets, in order:
  1. `laby_18_18_04` — hardest; only the partitioning solver got it. Highest-value proof point.
  2. `mcm/140`, `mcm/176` — small, rich `or`-structure, clean cube seeds (on bitblast).
  3. Speedup (not new-solve) targets: `string1x16.3`, `mcm/68` — already solvable at ~1000s; partitioning should cut that materially.
- Parallel side quests, separately tracked: B10 crash (graph-colouring),
  edge-matching bitblast error (OOM?), and the array/UF feature gap (47 benchmarks).
