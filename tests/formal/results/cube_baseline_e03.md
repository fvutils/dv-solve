# E0.3 — Cube-engine baseline refresh (2026-07-20)

**Engine:** `DV_ENGINE=cube` (P2.5 cartesian-or partition, default-on), current
B10-fixed binary `build/dv-solve-smt2`.
**Protocol:** single seed, 320 s wall cap, runs **sequential** (one at a time — the
clone-per-worker executor is memory-bandwidth-bound; concurrent runs inflate each
other's wall time). `W` = `DV_PARALLEL` worker count; `W=1` = sequential P0/P1
fallback. mcm/176 run under a 50 GB `ulimit -v` guard so an OOM can't destabilize
the box (manifests as SIGABRT/sig6 instead of a system OOM-kill).

| instance | W=1 | W=4 | W=8 | W=16 | W=28 |
|---|---|---|---|---|---|
| **mcm/68** (sat) | sat 108 s / 0.35 GB | **sat 76 s / 1.1 GB** | sat 136 s / 1.9 GB | sat 122 s / 3.5 GB | sat 141 s / 5.9 GB |
| **mcm/70** (sat) | **sat 237 s / 0.35 GB** | timeout / 1.1 GB | timeout / 1.9 GB | timeout / 3.6 GB | timeout / 6.0 GB |
| **mcm/140** (sat) | timeout / 1.1 GB | timeout / 3.3 GB | timeout / 6.2 GB | timeout / 11.3 GB | timeout / 20.3 GB |
| **mcm/176** (sat) | timeout / 7.9 GB | timeout / 23.6 GB | timeout / 41.8 GB | OOM(sig6) / 47 GB | OOM(sig6) / 49 GB |

## Starting line: confirmed, with three findings that update the documented story

**1. mcm/68 now solves at EVERY W (76–141 s), including W=1 sequential (108 s).**
The documented headline was "sat ~171 s @16w". The current binary beats that at
*every* worker count, and the sweet spot is **low W (W=4, 76 s)**, not 16. mcm/68
was NOT solvable by plain `--engine=bitblast` within 300 s (E0.2 baseline), so the
cube partition is the enabler — but **the parallelism is not**. Wall time is flat
(76–141 s, non-monotonic) while RSS grows *linearly* with W (0.35 → 5.9 GB, 17×).
The non-monotonic curve is heavy-tail variance, not a real W-preference: which cube
the schedule hits first dominates. **We pay 17× the memory for zero speedup above
W=4.** (The old "needs 16 workers" story predates the P2.5 cartesian seed default;
the win was always the partition, not the worker count.)

**2. mcm/70: cube parallelism is actively HARMFUL.** W=1 sequential solves in 237 s;
W≥4 **all time out**. Root cause = memory-bandwidth contention: N CaDiCaL clones
thrash the bus, per-worker throughput drops with W, and on a heavy-tail instance
that pushes a solvable case past the cap. (Separately, plain bitblast solves mcm/70
in 39 s — cube is a 6× regression here even sequentially. mcm/70's structure does
not benefit from the cartesian partition.)

**3. /140 timeout and /176 OOM confirmed as the starting line.** No W cracks /140.
/176 survives (timeout) only at W≤8; at W≥16 it aborts against the 50 GB guard —
the clone-per-worker RSS blowup (documented ceiling). RSS-vs-W is strictly linear
across all four instances, confirming the executor clones the full encoded instance
per worker.

## Cross-cutting signal (decision-relevant)

**The clone-per-worker parallel executor does not scale: W>4 never helps, sometimes
hurts (mcm/70), and RSS grows linearly with W.** Workers are informationally
isolated (first-SAT-wins, no clause exchange), so extra workers only add
bandwidth-contending redundant search. Implication for the checklist:

- **E1 (share-the-formula) presumes scaling W is desirable — but E0.3 shows it isn't
  yet.** Sharing the CNF would cut RSS (letting /176 run at high W), but there is no
  point running /176 at high W until workers actually *cooperate*.
- **E2 (clause sharing) should gate E1**, not the reverse: make workers cooperate
  first (turn isolated redundancy into a real portfolio), *then* pay for the memory
  model that lets that cooperation scale. Without E2, every W>4 worker is waste.
- The solvable class (mcm/68) is already handled by **low-W + the cartesian
  partition**; the hard class (/140, /176) needs per-solve strength (E5 word-level
  preprocessing) and/or SAT-directed cube ordering (E3) — not more workers.

## Follow-up (now urgent, was E0.2 TODO)

The single-seed non-monotonic mcm/68 curve makes **5-seed medians** necessary to
state the real W-scaling shape (and to confirm W=1's 108 s solve isn't luck). The
mcm/70 W≥4 timeouts likewise need multi-seed confirmation that parallelism *reliably*
regresses it rather than a single unlucky schedule.
